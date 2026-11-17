# Geraduo Hypervisor Core v0.5.0

A C++20 educational hypervisor-core project with a Windows platform diagnostic
layer, an Intel VT-x Virtualization Foundation and an optional read-only WDK
diagnostic kernel driver.

**v0.5.0 is a foundation and diagnostics release. It does not execute VMXON, does
not create a VMCS, does not enter a guest and does not provide hardware EPT.**
It adds a verifiable capability/readiness/lifecycle/diagnostics structure around
Intel VT-x so that later kernel work starts from explicit evidence instead of
assumptions. The best user-mode verdict, `ReadyForReview`, means *ready for
kernel-stage review only* — never *ready for hardware VMXON*.

## What is preserved from v0.4.0 and earlier

The existing architecture and public APIs remain available:

- Lifecycle state machine (`Lifecycle`)
- Hypervisor orchestration (`Hypervisor`)
- `IHostBackend` / `IGuestBackend`
- `MemoryManager`
- `SyntheticPageTable`
- `NestedPageTable`, `EptManager`, `NptManager`
- `ProgramRunner`
- CPUID preflight (`RunPlatformPreflight`)
- Diagnostics primitives
- `WindowsPlatformBackend`, `CpuCapabilityManager`,
  `PlatformCompatibilityManager`
- `KernelDriverInterface`, `DriverDiagnostics`
- `hv_platform_diagnostics.exe`
- The shared protocol version 1: 24-byte request, 80-byte response,
  `IOCTL_GERADUO_QUERY_PLATFORM_INFO = 0x00226000`

`hv_core` remains platform-neutral. The Windows platform code lives in
`hv_platform`; the new VT-x foundation lives in `hv_virtualization`, which links
`hv_platform`.

## v0.5.0 additions

| Component | Purpose |
| --- | --- |
| `VmxCapabilityManager` | Intel vendor/VMX evaluation plus optional per-logical-processor CPUID coverage |
| `VmxReadinessManager` | Combines VT-x capability, Windows platform, compatibility policy and driver protocol compatibility |
| `VirtualizationBackend` | Platform-neutral backend contract and the exact diagnostic state vocabulary |
| `VmxLifecycleController` | Thread-safe, transactional state machine with rollback, stop protection and a diagnostic journal |
| `VmxDiagnostics` | Bounded, thread-safe event journal and composed text/JSON report serialization |
| `hv_vmx_diagnostics.exe` | VT-x diagnostics CLI with text and `--json` output |
| Driver compatibility | Driver 0.5.0 reports version 0.5.0; protocol 1 remains compatible with driver 0.4.x |
| New tests | `hv_vmx_capability_tests`, `hv_vmx_readiness_tests`, `hv_vmx_lifecycle_tests` |
| New fakes | `FakeLogicalProcessorProbe`, `FakeCompatibilityEnvironment`, driver response overrides |

## Architecture

```text
hv_vmx_diagnostics.exe / applications
        |
        v
VmxLifecycleController  (state machine, rollback, thread safety, journal)
        |
        +-- VmxCapabilityManager --- CpuCapabilityManager --- ICpuidProvider
        |         |                       NativeCpuidProvider (unprivileged CPUID)
        |         +-- ILogicalProcessorProbe
        |                 NativeLogicalProcessorProbe (affinity-pinned CPUID per CPU)
        |
        +-- VmxReadinessManager --- WindowsPlatformBackend --- IPlatformProvider
        |         |                       NativePlatformProvider
        |         |
        |         +-- PlatformCompatibilityManager --- ICompatibilityEnvironment
        |         |                       NativeCompatibilityEnvironment
        |         |
        |         +-- KernelDriverInterface --- IDriverTransport
        |                 NativeDriverTransport (SetupAPI + DeviceIoControl)
        |                         |
        |                         | IOCTL_GERADUO_QUERY_PLATFORM_INFO
        |                         v
        |                 GeraduoDriver.sys (WDM, read-only diagnostic query)
        |
        +-- VmxDiagnostics  (bounded event journal, text/JSON report)
```

## Quick start

Canonical Visual Studio 2022 commands:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Release\hv_vmx_diagnostics.exe
build\Release\hv_vmx_diagnostics.exe --json
build\Release\hv_vmx_diagnostics.exe --no-driver
```

`hv_vmx_diagnostics.exe` exit codes:

| Code | Meaning |
| ---: | --- |
| `0` | The lifecycle produced a verdict: `NotSupported`, `Blocked` or `ReadyForReview` |
| `1` | A diagnostic component failed internally |
| `2` | Invalid command line |

A missing driver is a normal diagnostic result. It blocks `ReadyForReview` with
`DriverNotConnected` but does not produce an internal error or a nonzero exit
code.

The optional driver build requires the WDK:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHV_BUILD_DRIVER=ON
cmake --build build --config Release --target GeraduoDriver
```

See [docs/BUILD.md](docs/BUILD.md),
[docs/DRIVER_INTEGRATION.md](docs/DRIVER_INTEGRATION.md) and
[docs/DRIVER_SETUP.md](docs/DRIVER_SETUP.md).

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [VT-x readiness model](docs/VMX_READINESS.md)
- [Driver integration and protocol compatibility](docs/DRIVER_INTEGRATION.md)
- [Windows platform layer](docs/WINDOWS_PLATFORM.md)
- [Driver architecture](docs/DRIVER_ARCHITECTURE.md)
- [IOCTL protocol](docs/IOCTL_PROTOCOL.md)
- [Build instructions](docs/BUILD.md)
- [Driver installation and connection](docs/DRIVER_SETUP.md)
- [Diagnostics and error interpretation](docs/DIAGNOSTICS.md)
- [Unsupported features and safety boundaries](docs/UNSUPPORTED.md)
- [Changelog](CHANGELOG.md)
- [Driver folder](driver/README.md)

## Validation status

Validated in this workspace (Windows Server 2025, build `10.0.26200.9974`,
x64, AMD Ryzen 5 5600 virtual machine under Microsoft Hyper-V):

- User-mode Release x64 build with Visual Studio 2026 / MSVC 19.52.37029.0
  (`/W4 /WX /permissive- /EHsc`).
- Seven test executables, all passing: the four pre-existing suites plus
  `hv_vmx_capability_tests`, `hv_vmx_readiness_tests` and
  `hv_vmx_lifecycle_tests`.
- `hv_vmx_diagnostics.exe` text output and `--json` output.
- JSON output parses successfully with `ConvertFrom-Json`.
- The diagnostics report `NotSupported` / `UnsupportedCpuVendor` on this AMD
  host, with `ReadyForReview` and hardware-start flags explicitly false. This is
  the correct result: no AMD-V/SVM backend exists in v0.5.0.
- The optional kernel driver was compiled and linked against WDK/SDK
  **10.0.28000.0** with MSVC **14.52.37025**, Release and Debug x64, using
  `/W4 /WX`. Release: 27,136 bytes; Debug: 27,648 bytes. The image is an x64
  native-subsystem driver with `GsDriverEntry`, NX compatibility and dynamic
  base.
- The driver build used `driver/build-driver.ps1` (or the CMake
  `GeraduoDriverDirect` target) because the WDK Visual Studio integration is not
  registered in the installed Visual Studio 2026.

**Not validated:**

- **No hardware VT-x execution was tested.** The authoring host is AMD. No
  Intel machine was available, and no VMXON, VMCS, VM-entry/exit or hardware EPT
  code exists to test.
- The driver was not signed, installed, loaded or IOCTL-tested. Driver loading
  requires an elevated process, test signing (or a signed binary), compatible
  HVCI policy and an isolated test VM.
- The WDK MSBuild path (`GeraduoDriver` target, `GeraduoDriver.vcxproj`) could
  not run because the WDK platform toolset is not registered. The direct
  compiler/linker fallback produced the binaries above.
- The requested Visual Studio 17 2022 generator was not available; the installed
  Visual Studio 18 2026 generator was used. The VS2022 commands above are the
  documented canonical path, not a locally executed validation.

## Safety boundaries

This release does **not** implement or claim:

- `VMXON` / `VMXOFF`
- VMCS creation, loading, clearing, launch, VM entry or VM exit
- hardware EPT/NPT structures or hardware second-level translation
- physical-memory access, arbitrary kernel memory access, or MMIO
- arbitrary process-memory access
- MSR or control-register access from user mode
- guest execution, interrupt injection, or device emulation
- an AMD-V/SVM backend
- hypervisor concealment, anti-cheat bypass, or anti-detection behavior
- a production-signed driver

`ReadyForReview` is not a hardware-readiness claim. IA32_FEATURE_CONTROL,
VMX capability MSRs, CR0/CR4 fixed bits, VMCS setup, EPT and every VMX
instruction remain unverified or unimplemented. See
[docs/VMX_READINESS.md](docs/VMX_READINESS.md) and
[docs/UNSUPPORTED.md](docs/UNSUPPORTED.md).
