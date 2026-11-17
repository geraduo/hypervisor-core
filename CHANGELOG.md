# Changelog

All notable changes to Geraduo Hypervisor Core are documented here. The project
does not claim production readiness, hardware virtualization execution or driver
runtime validation at any version below.

## v0.5.0 — Intel VT-x Virtualization Foundation

### Added

- `hv_virtualization` library, layered on top of `hv_platform`.
- `VmxCapabilityManager`: Intel vendor/VMX evaluation built on
  `CpuCapabilityManager`, an optional per-logical-processor CPUID probe and
  explicit `NotChecked` semantics for incomplete coverage.
- `VmxReadinessManager`: combines VT-x capability, `WindowsPlatformBackend`,
  `PlatformCompatibilityManager`, `KernelDriverInterface` and
  `DriverDiagnostics` into a readiness verdict for kernel-stage review.
- `VirtualizationBackend` (`IVirtualizationBackend`): platform-neutral backend
  contract with the fixed state vocabulary `Uninitialized`,
  `CheckingCapabilities`, `NotSupported`, `Blocked`, `ReadyForReview`, `Failed`,
  `Stopped`.
- `VmxLifecycleController`: thread-safe, transactional lifecycle with rollback
  of a failed `Refresh()`, rejection of repeated `Initialize()`/`Stop()`,
  terminal `Stop()` and driver-connection release.
- `VmxDiagnostics`: bounded, thread-safe event journal with composed text and
  JSON report serialization.
- `hv_vmx_diagnostics.exe`: text, `--json`, `--no-driver` and `--help` modes;
  exit codes 0 (verdict), 1 (internal failure), 2 (invalid command line).
- `NativeLogicalProcessorProbe`: affinity-pinned CPUID observation per active
  logical processor, restoring the previous group affinity.
- Tests: `hv_vmx_capability_tests`, `hv_vmx_readiness_tests`,
  `hv_vmx_lifecycle_tests`, including rollback, repeated-transition,
  concurrency, journal bounds/thread safety, JSON escaping and `NOT_CHECKED`
  coverage.
- Fakes: `FakeLogicalProcessorProbe`, `FakeCompatibilityEnvironment`, driver
  response overrides in `FakeKernelDriverInterface`.
- Documentation: `docs/VMX_READINESS.md`, `docs/DRIVER_INTEGRATION.md`,
  refreshed architecture, build, diagnostics, IOCTL and unsupported-feature
  pages, and an architecture diagram in the README.

### Changed

- CMake project version is `0.5.0`.
- `driver_protocol.hpp` reports driver version `0.5.0` and defines
  `kGeraduoMinimumCompatibleDriverMajor` / `...Minor` as `0.4`.
- Version strings in the platform layer now refer to v0.5.0.
- `docs/BUILDING.md` was renamed to `docs/BUILD.md`.
- The driver remains a read-only diagnostic query; only its reported version
  changed.

### Compatibility

- All public v0.4.0 APIs remain available and unchanged in behavior.
- Protocol version stays `1`, the request stays 24 bytes, the response stays
  80 bytes and `IOCTL_GERADUO_QUERY_PLATFORM_INFO` stays `0x00226000`.
- A 0.4.x diagnostic driver remains protocol compatible. The readiness layer
  checks the reported driver version against the minimum `0.4` policy.
- The driver is not required to build or run the user-mode targets.

### Explicitly not included

- `VMXON` / `VMXOFF`, VMCS creation or access, VM entry/exit, exit handling.
- Hardware EPT/NPT, VPID, MSR bitmaps, interrupt injection, guest execution.
- AMD-V/SVM, Windows Hypervisor Platform backend.
- Physical-memory access, arbitrary process memory access, MSR access from
  user mode, `METHOD_NEITHER` IOCTLs or pointer escapes.
- Hypervisor concealment, anti-cheat bypass or anti-detection behavior.
- A signed, installed or loaded driver.

### Validation

Validated in the authoring workspace (Windows Server 2025 build
`10.0.26200.9974`, x64, AMD Ryzen 5 5600 VM under Microsoft Hyper-V):

- Release x64 build with Visual Studio 2026 / MSVC 19.52.37029.0, `/W4 /WX`,
  no warnings.
- `ctest`: 7/7 test executables passed.
- `hv_vmx_diagnostics.exe` text and JSON output; JSON parsed with
  `ConvertFrom-Json`.
- Host verdict: `NotSupported` / `UnsupportedCpuVendor`, error class `None`,
  readiness `NotSupported`, `hardware_vmx_started=false`. This is correct for
  an AMD host without an SVM backend.
- Driver compiled against WDK/SDK 10.0.28000.0, Release 27,136 bytes and Debug
  27,648 bytes, x64 native, `GsDriverEntry`, NX-compatible, dynamic base,
  unsigned.

Not validated:

- No hardware VT-x execution was tested. No Intel host was available and no
  VMXON/VMCS/VM-entry/exit code exists.
- The driver was not signed, installed, loaded or IOCTL-tested. That requires an
  elevated isolated test VM with test signing and a compatible HVCI policy.
- The requested Visual Studio 2022 generator was unavailable; Visual Studio 18
  2026 was used. VS2022 commands are documented but were not executed locally.

## v0.4.0 — Windows Platform Diagnostics

### Added

- `hv_platform` library with `WindowsPlatformBackend`
  (`IPlatformProvider`/`NativePlatformProvider`), `CpuCapabilityManager`,
  `PlatformCompatibilityManager` and `DriverDiagnostics`.
- `KernelDriverInterface` with a typed `IDriverTransport`, SetupAPI device
  interface discovery, fixed-size IOCTL validation and NTSTATUS/Win32 mapping.
- `hv_platform_diagnostics.exe` with text, `--json`, `--no-driver` and `--help`.
- `hv_windows_platform_tests` and `hv_platform_compatibility_tests`.
- Optional WDK driver `GeraduoDriver` exposing one read-only diagnostic IOCTL
  (`IOCTL_GERADUO_QUERY_PLATFORM_INFO = 0x00226000`) with `METHOD_BUFFERED` I/O,
  SDDL-restricted access and a fixed 24-byte request / 80-byte response.
- `driver/build-driver.ps1` and the `GeraduoDriverDirect` CMake target as a
  fallback when the WDK MSBuild toolset is not registered.

### Validation

- User-mode Release x64 build and tests passed.
- Driver compiled and linked; not signed, loaded or IOCTL-tested.
- No VMXON, VMCS, EPT, physical-memory or process-memory operation was added.

## v0.3.1 and earlier

The synthetic-memory core (`Lifecycle`, `Hypervisor`, `MemoryManager`,
`SyntheticPageTable`, `NestedPageTable`, `EptManager`, `NptManager`,
`ProgramRunner`, CPUID preflight and diagnostics primitives) is preserved.
Those releases never implemented hardware virtualization.
