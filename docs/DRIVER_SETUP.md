# Driver Build, Installation and Connection

This document describes how to build and try the **optional** diagnostic driver.
The driver has been compiled and linked against WDK/SDK 10.0.28000.0, but it has
not been signed, installed, loaded or IOCTL-tested. Follow these steps on a
disposable test machine with test signing enabled.

Do not install an unsigned or test-signed kernel driver on a production system.

## 1. Prerequisites

- Windows 10/11 x64 or Windows Server test VM.
- Visual Studio 2022 with "Desktop development with C++".
- Windows Driver Kit matching the Visual Studio/SDK version.
- Windows SDK with `signtool.exe`.
- Administrator command prompt.
- Test signing enabled (`bcdedit /set testsigning on`) and a reboot.
- Secure Boot disabled on the test machine if firmware blocks test signing.
- Memory integrity / HVCI may block test-signed drivers. Disable HVCI on the
  isolated test VM if load fails with error 1275, and understand that this
  weakens the test VM's security.

## 2. Build

From the repository root in a Visual Studio 2022 developer prompt:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHV_BUILD_DRIVER=ON
cmake --build build --config Release --target GeraduoDriver
```

Alternatively, open `driver/GeraduoDriver.vcxproj` in Visual Studio and build
`Release|x64`.

Expected output:

```text
build\driver\Release\GeraduoDriver.sys
```

If the CMake driver target reports `MSB8020` or a missing `km\ntddk.h`, the WDK
Visual Studio integration or the `v143` toolset is not installed. Use the
direct compiler fallback instead:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\build-driver.ps1 -Configuration Release
```

Output: `build-driver\Release\GeraduoDriver.sys`.

Or through CMake:

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DHV_BUILD_DRIVER=ON
cmake --build build --config Release --target GeraduoDriverDirect
```

Output: `build\driver\Release\GeraduoDriver.sys`. The script uses the same
source, WDK headers and libraries, with `/W4 /WX`; it does not sign, install or
load the driver.

## 3. Test-sign the driver

In an elevated PowerShell prompt:

```powershell
$cert = New-SelfSignedCertificate `
  -Type CodeSigningCert `
  -Subject "CN=Geraduo Test Driver" `
  -CertStoreLocation Cert:\LocalMachine\My

Export-Certificate -Cert $cert -FilePath .\GeraduoTest.cer
Import-Certificate -FilePath .\GeraduoTest.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath .\GeraduoTest.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher

signtool.exe sign /v /fd SHA256 /s My /n "Geraduo Test Driver" `
  .\build\driver\Release\GeraduoDriver.sys
```

Verify:

```powershell
signtool.exe verify /v /pa .\build\driver\Release\GeraduoDriver.sys
```

The certificate is for local testing only. Never distribute it or trust it on a
production machine.

## 4. Install the driver service

The driver is a non-PnP WDM driver with a control device. `sc.exe` can create a
kernel service for it:

```bat
sc.exe create GeraduoDriver type= kernel start= demand error= normal ^
  binPath= "C:\full\path\build\driver\Release\GeraduoDriver.sys" ^
  DisplayName= "Geraduo Diagnostic Driver"

sc.exe start GeraduoDriver
sc.exe query GeraduoDriver
```

The driver creates:

- device `\Device\GeraduoDriver`
- symbolic link `\DosDevices\GeraduoDriver`
- device interface `{f3a8c7e2-5d41-4a9b-8c6e-2b7d1e9f4a30}`

The user-mode client enumerates the registered interface with SetupAPI; it does
not open the symbolic link path directly.

## 5. Connect from user mode

Run an elevated prompt, because the device SDDL grants read access only to
SYSTEM and Builtin Administrators:

```bat
build\Release\hv_platform_diagnostics.exe
```

The VT-x foundation diagnostics use the same driver and show a separate
compatibility block:

```bat
build\Release\hv_vmx_diagnostics.exe
build\Release\hv_vmx_diagnostics.exe --json
```

When connected, the driver section shows:

```text
=== Geraduo diagnostic driver ===
Connected to the diagnostic device interface.
Geraduo kernel driver platform information
Protocol: 1 (response 80 bytes), status Success
Driver version: 0.5.0
Windows: 10.0.xxxxx.0
Architecture: x64
Active processors: ...
Page size: 4096
Physical memory: ... bytes
Capability flags: 0x...
```

JSON mode:

```bat
build\Release\hv_platform_diagnostics.exe --json
```

The `driver.info` object is `null` when the driver is absent, and
`driver.diagnostics` records the reason.

In `hv_vmx_diagnostics.exe`, the readiness report shows protocol version,
response size, driver version, architecture and processor-count comparisons.
A missing or stopped driver is reported as `Blocked`/`DriverNotConnected`; it
is not an internal error.

## 6. Stop and remove

Close the diagnostic tool first, then from an elevated prompt:

```bat
sc.exe stop GeraduoDriver
sc.exe delete GeraduoDriver
```

Optionally remove the test certificate and disable test signing:

```powershell
Get-ChildItem Cert:\LocalMachine\My |
  Where-Object { $_.Subject -eq "CN=Geraduo Test Driver" } |
  Remove-Item

bcdedit /set testsigning off
```

Reboot after changing test signing.

## Troubleshooting

| Error | Meaning | Action |
| --- | --- | --- |
| `sc start` error 577 | Signature not accepted | Enable test signing, sign the `.sys`, reboot, verify with `signtool verify /pa` |
| `sc start` error 1275 | Driver blocked by HVCI/policy | Use an isolated VM without memory integrity, or use a properly production-signed driver |
| `sc start` error 5 | Not elevated | Run as administrator |
| `sc start` error 2 | Driver file not found | Check `binPath` for the absolute path |
| `Connect()` returns `AccessDenied` | Not elevated, or SDDL denies access | Run elevated; do not weaken the SDDL casually |
| `Connect()` returns `NotFound` | Service stopped, device failed to start, or interface not registered | `sc query GeraduoDriver`; check Event Viewer / DebugView |
| Protocol version mismatch | User-mode and driver binaries come from different builds | Rebuild both from the same commit |
| `STATUS_BUFFER_TOO_SMALL` / `InvalidArgument` | Broken client or corrupt request | Use `KernelDriverInterface`; do not hand-craft IOCTLs |
| Driver unload logs open handles | Diagnostic client still connected | Close the client and stop again |

## Debug output

The driver logs with `DbgPrintEx` under `DPFLTR_IHVDRIVER_ID`. Use DebugView or a
kernel debugger to observe:

- device creation and interface activation;
- rejected request magic/version/header;
- unsupported IOCTLs;
- per-query completion ids;
- open-handle counts;
- unload with open handles.

Format strings include explicit casts so `/W4` does not hide varargs type
mismatches.

## Absolute cautions

- This driver is a diagnostic, not a hypervisor.
- It performs no VMX, VMCS, hardware EPT/NPT, physical-memory or guest
  operation.
- It is not production-signed.
- It was compiled and linked with WDK 10.0.28000.0 / MSVC 14.52, but it was
  **not loaded or IOCTL-tested**; the driver-path tests use a scripted
  transport.
- `HVCI`/`VBS` state in the diagnostics is read-only policy reporting; this
  project never disables either.
