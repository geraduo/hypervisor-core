# Building

## Prerequisites

### User-mode build

- CMake 3.20 or newer.
- A C++20 compiler. On Windows, Visual Studio 2022 (v143) or newer with the
  "Desktop development with C++" workload.
- Windows SDK 10.
- No WDK is required for `hv_core`, `hv_platform`, `hv_virtualization`, the
  tests, `hv_platform_diagnostics.exe` or `hv_vmx_diagnostics.exe`.

### Optional kernel driver

- Windows Driver Kit (WDK) and a matching Windows SDK. This release was built
  against WDK/SDK 10.0.28000.0.
- Visual Studio 2022 with the WDK Visual Studio integration for the MSBuild /
  `.vcxproj` path (`GeraduoDriver` target).
- Any Visual Studio with the C++ toolset for the direct `cl.exe`/`link.exe`
  path (`driver/build-driver.ps1` or the `GeraduoDriverDirect` target). This is
  the path used on the authoring machine, where VS2026 does not register the
  WDK platform toolset.
- Administrator rights for signing, installation and loading.
- Test signing enabled for an unsigned development driver.

## Requested Visual Studio 2022 commands

From the repository root:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

These are the canonical commands for a machine with VS2022. The authoring
machine for this release did **not** have VS2022; it used the installed
Visual Studio 2026 generator:

```bat
cmake -S . -B build-v050 -G "Visual Studio 18 2026" -A x64
cmake --build build-v050 --config Release
ctest --test-dir build-v050 -C Release --output-on-failure
```

Substitute the generator that matches your installation. If you use a
Developer Command Prompt, `cmake -S . -B build -A x64` can select the default
Visual Studio generator.

## Build outputs

With `--config Release`, the user-mode outputs are placed in:

```text
build\Release\hv_core.lib
build\Release\hv_platform.lib
build\Release\hv_virtualization.lib
build\Release\hv_demo.exe
build\Release\hv_launch.exe
build\Release\hv_preflight.exe
build\Release\hv_platform_diagnostics.exe
build\Release\hv_vmx_diagnostics.exe
build\Release\hv_tests.exe
build\Release\hv_preflight_tests.exe
build\Release\hv_windows_platform_tests.exe
build\Release\hv_platform_compatibility_tests.exe
build\Release\hv_vmx_capability_tests.exe
build\Release\hv_vmx_readiness_tests.exe
build\Release\hv_vmx_lifecycle_tests.exe
```

`hv_virtualization` is built only when `HV_BUILD_VIRTUALIZATION` and
`HV_BUILD_WINDOWS_PLATFORM` are both `ON`, because it links `hv_platform`.

## CMake options

| Option | Default | Description |
| --- | --- | --- |
| `HV_BUILD_TESTS` | `ON` | Build and register the test executables (seven in this release) |
| `HV_BUILD_DEMO` | `ON` | Build the existing architecture demo |
| `HV_BUILD_LAUNCHER` | `ON` | Build the existing host process launcher |
| `HV_BUILD_PREFLIGHT` | `ON` | Build the existing `hv_preflight` CPUID tool |
| `HV_BUILD_WINDOWS_PLATFORM` | `ON` | Build the `hv_platform` library and platform tests |
| `HV_BUILD_PLATFORM_DIAGNOSTICS` | `ON` | Build `hv_platform_diagnostics.exe` |
| `HV_BUILD_VIRTUALIZATION` | `ON` | Build the `hv_virtualization` Intel VT-x foundation library |
| `HV_BUILD_VMX_DIAGNOSTICS` | `ON` | Build `hv_vmx_diagnostics.exe` |
| `HV_BUILD_DRIVER` | `OFF` | Add the WDK MSBuild driver target |

Example minimal user-mode build without the demo, launcher and preflight tool:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DHV_BUILD_DEMO=OFF ^
  -DHV_BUILD_LAUNCHER=OFF ^
  -DHV_BUILD_PREFLIGHT=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Warning policy

All user-mode targets compile with:

```text
/W4 /WX /permissive- /EHsc
```

on MSVC, or `-Wall -Wextra -Wpedantic -Werror` elsewhere. New code must not
suppress warnings to pass the build. `hv_vmx_diagnostics` and
`hv_virtualization` also define `WIN32_LEAN_AND_MEAN`, `NOMINMAX` and
`_WIN32_WINNT=0x0A00`. The test targets link `Threads::Threads` for the
concurrency tests.

The driver project independently uses `/W4` and treats warnings as errors.

## Tests

`ctest` registers these tests:

| Test | Purpose |
| --- | --- |
| `hv_tests` | Original lifecycle, translation and program-runner tests |
| `hv_preflight_tests` | Original CPUID preflight tests |
| `hv_windows_platform_tests` | Platform backend, CPU manager, protocol, missing driver, reconnect, malformed/version-mismatched responses, diagnostics journal, fakes |
| `hv_platform_compatibility_tests` | Hypervisor environments, VBS/HVCI, WHPX, CPU precedence, formatting |
| `hv_vmx_capability_tests` | Intel VMX capability, per-processor coverage, `NOT_CHECKED`, malformed reports, JSON/text formatting |
| `hv_vmx_readiness_tests` | Readiness precedence, driver protocol/version/architecture checks, unsupported/blocked/failed cases, disabled probes, formatting |
| `hv_vmx_lifecycle_tests` | Lifecycle transitions, double initialize, terminal stop, refresh commit/rollback, concurrency, journal bounds/thread safety, JSON escaping |

Run one test verbosely:

```bat
ctest --test-dir build -C Release -R hv_vmx_lifecycle_tests --output-on-failure
```

Run a test executable directly to see every individual check:

```bat
build\Release\hv_vmx_lifecycle_tests.exe
```

## Running the VT-x diagnostics

```bat
build\Release\hv_vmx_diagnostics.exe
build\Release\hv_vmx_diagnostics.exe --json
build\Release\hv_vmx_diagnostics.exe --no-driver
build\Release\hv_vmx_diagnostics.exe --json --no-driver
```

Exit codes:

| Code | Meaning |
| ---: | --- |
| `0` | The lifecycle produced a verdict (`NotSupported`, `Blocked` or `ReadyForReview`) |
| `1` | A diagnostic component failed internally |
| `2` | Invalid command line |

A missing driver is a normal `Blocked` verdict and does not produce a nonzero
exit code. The tool never executes a VMX instruction; see
[docs/VMX_READINESS.md](VMX_READINESS.md).

## Driver build

The driver is not part of the default build.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHV_BUILD_DRIVER=ON
cmake --build build --config Release --target GeraduoDriver
```

`driver/CMakeLists.txt`:

1. Verifies that `MSBuild.exe` is available.
2. Locates `km\ntddk.h` under `WDKContentRoot`, `WindowsSdkDir` or the default
   Windows Kits path.
3. Invokes `driver/GeraduoDriver.vcxproj` with `Platform=x64` and the selected
   configuration.
4. Writes `GeraduoDriver.sys` to `build\driver\Release\`.

You can also open `driver/GeraduoDriver.vcxproj` directly in Visual Studio.

### Direct compiler fallback

The WDK MSBuild project needs the WDK Visual Studio integration (the
`WindowsKernelModeDriver10.0` platform toolset and, for this project, the
`v143` toolset). Some installations, including the authoring VS2026 machine, do
not register that integration even though the WDK headers and libraries are
installed. In that case MSBuild fails with `MSB8020` (toolset not found) or
`C1083` (`ntddk.h` not found).

The fallback compiles and links the same source directly with the installed
WDK and MSVC tools:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File driver\build-driver.ps1 -Configuration Release
```

Output: `build-driver\Release\GeraduoDriver.sys`.

Or through CMake:

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DHV_BUILD_DRIVER=ON
cmake --build build --config Release --target GeraduoDriverDirect
```

Output: `build\driver\Release\GeraduoDriver.sys`.

The script supports `-Configuration Debug|Release`, `-WdkVersion`,
`-OutputDir` and `-Clean`. It does not sign, install or load the driver.

### Verified driver build

On the authoring machine the direct path produced:

- WDK/SDK 10.0.28000.0, MSVC 14.52.37025, driver version 0.5.0.
- Release x64: `GeraduoDriver.sys`, 27,136 bytes.
- Debug x64: `GeraduoDriver.sys`, 27,648 bytes.
- `/W4 /WX` and `/permissive-` clean.
- `dumpbin /headers`: x64, native subsystem, `GsDriverEntry`, NX-compatible,
  dynamic base.
- `Get-AuthenticodeSignature`: `NotSigned`.
- No loading or IOCTL execution was performed.

The driver source is deliberately separate from the CMake C++ targets so a
missing WDK never breaks the normal build. Configuring with
`-DHV_BUILD_DRIVER=ON` on a machine without the WDK intentionally fails with:

```text
The Windows Driver Kit (km\ntddk.h) was not found.
```

The MSBuild target still requires the VS2022 `v143` toolset and the WDK Visual
Studio integration, while `GeraduoDriverDirect` works with the installed tools.

## Verified user-mode build

On the authoring machine:

- Windows Server 2025, build 10.0.26200.9974, x64.
- Visual Studio 18 2026 generator, MSVC 19.52.37029.0.
- Release x64, `/W4 /WX /permissive- /EHsc`, no warnings.
- `ctest`: 7/7 passed.
- `hv_vmx_diagnostics.exe` text and JSON output; JSON parsed with
  `ConvertFrom-Json`.
- The host is an AMD Ryzen 5 5600 VM under Microsoft Hyper-V, so the correct
  verdict is `NotSupported`/`UnsupportedCpuVendor` with
  `hardware_vmx_started=false`.

## Clean rebuild

```bat
cmake --build build --config Release --target clean
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

If CMake retains stale options, remove the build directory or run with a fresh
`-B` directory.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `MSB8020` / toolset not found | The requested `PlatformToolset` (v143) is not installed, or the WDK Visual Studio integration is missing; use VS2022 with the WDK extension or `GeraduoDriverDirect` |
| `km\ntddk.h` not found | WDK is missing, does not match the SDK, or the WDK MSBuild toolset is not registered; use the direct build script |
| `C4083` / `C4005` in WDK headers with the direct script | MSVC and WDK CRT headers overlap; the script defines `_VCRUNTIME_DISABLED_WARNINGS` and disables C4005 for that reason |
| `MSBuild.exe was not found` | Configure with a Visual Studio generator or use a Developer Command Prompt |
| Driver build succeeds, load fails with 577 | Driver is unsigned or test signing is disabled |
| Driver load fails with 5 | Start the service from an elevated prompt |
| `hv_vmx_diagnostics` reports `DriverNotConnected` | The driver is not installed, not loaded, or the device failed to start; this is a normal verdict |
| `hv_vmx_diagnostics` reports `DriverAccessDenied` | The process is not elevated or the SDDL does not grant access |
| JSON tool rejects output | Ensure you are running a build from this release; the CLI emits one JSON document |
