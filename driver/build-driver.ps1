<#
.SYNOPSIS
    Builds the Geraduo diagnostic kernel driver with the WDK compiler and linker.

.DESCRIPTION
    The official GeraduoDriver.vcxproj requires the WDK Visual Studio
    integration (the WindowsKernelModeDriver platform toolset). Some Visual
    Studio installations, notably VS 2026 without the WDK VSIX, do not expose
    that toolset to MSBuild. This script performs the same kernel-mode
    compile and link steps directly with cl.exe and link.exe from the
    installed Visual Studio and WDK.

    It is a build helper only. It does not sign, install, load or test the
    driver. Loading an unsigned test driver requires test signing (or a
    signed driver) and an elevated process.

.PARAMETER Configuration
    Debug or Release. Defaults to Release.

.PARAMETER WdkVersion
    Optional WDK/SDK version such as 10.0.28000.0. Defaults to the newest
    installed version that contains km\ntddk.h.

.PARAMETER OutputDir
    Optional output directory. Defaults to build-driver\<Configuration> under
    the repository root. Relative paths are resolved against the repository.

.PARAMETER Clean
    Remove the per-configuration output directory before building.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File driver\build-driver.ps1 -Configuration Release

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File driver\build-driver.ps1 -Configuration Debug -Clean
#>

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$WdkVersion = '',

    [string]$OutputDir = '',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $paths = & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        foreach ($path in $paths) {
            if (-not [string]::IsNullOrWhiteSpace($path) -and (Test-Path $path)) { return $path.Trim() }
        }
    }
    if ($env:VSINSTALLDIR -and (Test-Path $env:VSINSTALLDIR)) {
        return $env:VSINSTALLDIR.TrimEnd('\')
    }
    throw 'Visual Studio with the C++ toolset was not found. Install the Desktop development with C++ workload.'
}

function Find-Toolset {
    param([string]$VsRoot, [string]$Tool)
    $toolsRoot = Join-Path $VsRoot 'VC\Tools\MSVC'
    if (-not (Test-Path $toolsRoot)) { throw "MSVC tools were not found under $toolsRoot." }
    $versions = Get-ChildItem $toolsRoot -Directory |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($version in $versions) {
        $candidate = Join-Path $version.FullName "bin\Hostx64\x64\$Tool"
        if (Test-Path $candidate) { return $candidate }
    }
    throw "$Tool was not found in any MSVC toolset under $toolsRoot."
}

function Find-Wdk {
    param([string]$RequestedVersion)
    $root = if ($env:WindowsSdkDir) { $env:WindowsSdkDir.TrimEnd('\') } else { 'C:\Program Files (x86)\Windows Kits\10' }
    $includeRoot = Join-Path $root 'Include'
    if (-not (Test-Path $includeRoot)) { throw "Windows Kits include directory not found: $includeRoot" }

    $candidates = Get-ChildItem $includeRoot -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'km\ntddk.h') } |
        Sort-Object { [version]$_.Name } -Descending

    if ($RequestedVersion) {
        $candidates = $candidates | Where-Object Name -eq $RequestedVersion
    }
    if (-not $candidates) {
        throw "No WDK headers (km\ntddk.h) found under $includeRoot. Install the Windows Driver Kit."
    }

    $version = @($candidates)[0].Name
    $include = Join-Path $includeRoot $version
    $lib = Join-Path $root "Lib\$version\km\x64"
    foreach ($required in @('ntoskrnl.lib', 'wdmsec.lib', 'BufferOverflowK.lib', 'hal.lib')) {
        if (-not (Test-Path (Join-Path $lib $required))) {
            throw "WDK x64 library missing: $(Join-Path $lib $required)"
        }
    }
    return [pscustomobject]@{ Version = $version; Include = $include; Lib = $lib }
}

$repoRoot = Split-Path $PSScriptRoot -Parent
$source = Join-Path $PSScriptRoot 'src\driver_entry.cpp'
$driverInclude = Join-Path $PSScriptRoot 'include'
$sharedInclude = Join-Path $repoRoot 'include'

$vsRoot = Find-VisualStudio
$cl = Find-Toolset -VsRoot $vsRoot -Tool 'cl.exe'
$link = Find-Toolset -VsRoot $vsRoot -Tool 'link.exe'
# Find-Toolset returns <toolset>\bin\Hostx64\x64\cl.exe; the include directory is <toolset>\include.
$msvcRoot = Split-Path (Split-Path (Split-Path (Split-Path $cl -Parent) -Parent) -Parent) -Parent
$msvcInclude = Join-Path $msvcRoot 'include'
$wdk = Find-Wdk -RequestedVersion $WdkVersion

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $outputDir = Join-Path $repoRoot "build-driver\$Configuration"
} elseif ([IO.Path]::IsPathRooted($OutputDir)) {
    $outputDir = $OutputDir
} else {
    $outputDir = Join-Path $repoRoot $OutputDir
}
$objDir = Join-Path $outputDir 'obj'
if ($Clean -and (Test-Path $outputDir)) {
    Remove-Item $outputDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$clArgs = @(
    '/c', '/nologo',
    '/GS', '/W4', '/WX', '/permissive-', '/EHsc-', '/GR-',
    '/std:c++17', '/Zc:wchar_t-',
    '/D_AMD64_', '/D_WIN64',
    '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_UNICODE', '/DUNICODE',
    '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000012',
    # WDK 10.0.28000 uses the MSVC <stdint.h>/vcruntime headers. The WDK CRT
    # headers are older than MSVC 14.51 and do not define this macro, which the
    # MSVC vcruntime pragma expects. C4005 covers the intentional CRT macro
    # overlap between the two header sets.
    '/D_VCRUNTIME_DISABLED_WARNINGS=4324', '/wd4005',
    "/I$($wdk.Include)\km",
    "/I$($wdk.Include)\km\crt",
    "/I$($wdk.Include)\shared",
    "/I$($wdk.Include)\ucrt",
    "/I$msvcInclude",
    "/I$driverInclude",
    "/I$sharedInclude"
)

$releaseFlags = @('/O2', '/Oi', '/Gy')
$debugFlags = @('/Od', '/Zi', '/DDBG=1')
if ($Configuration -eq 'Release') {
    $clArgs += $releaseFlags
} else {
    $clArgs += $debugFlags
}

$obj = Join-Path $objDir 'driver_entry.obj'
$pdb = Join-Path $objDir 'GeraduoDriver.pdb'
$sys = Join-Path $outputDir 'GeraduoDriver.sys'

$clArgs += @("/Fo$obj", "/Fd$pdb", $source)

Write-Host "Visual Studio : $vsRoot"
Write-Host "MSVC compiler : $cl"
Write-Host "WDK version   : $($wdk.Version)"
Write-Host "Configuration : $Configuration"
Write-Host "Output        : $sys"

& $cl @clArgs
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE." }

$linkArgs = @(
    '/NOLOGO', '/DRIVER', '/SUBSYSTEM:NATIVE', '/MACHINE:X64',
    '/ENTRY:GsDriverEntry', '/NODEFAULTLIB',
    '/DEBUG', '/OPT:REF', '/OPT:ICF', '/DYNAMICBASE', '/NXCOMPAT',
    "/LIBPATH:$($wdk.Lib)",
    "/OUT:$sys",
    $obj,
    'ntoskrnl.lib', 'wdmsec.lib', 'hal.lib', 'wmilib.lib', 'BufferOverflowK.lib'
)

& $link @linkArgs
if ($LASTEXITCODE -ne 0) { throw "link.exe failed with exit code $LASTEXITCODE." }

$file = Get-Item $sys
Write-Host ("Built {0} ({1} bytes)" -f $file.FullName, $file.Length)
