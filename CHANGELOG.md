# Changelog

## [0.5.0] - VMX readiness and virtualization state management

Diagnostic infrastructure only. No `VMXON`, VMCS, VM entry/exit or hardware EPT is implemented; no hypervisor is started.

### Added
- `VmxCapabilityManager`: per-logical-processor Intel VMX detection through CPUID, built on `CpuCapabilityManager` and `RunPlatformPreflight`.
- `ILogicalProcessorCpuidProvider` and `NativeLogicalProcessorCpuidProvider` (Windows processor-group aware pinning with verification and affinity restore).
- `VmxReadinessManager`: combines `WindowsPlatformBackend`, capability, `PlatformCompatibilityManager` and the diagnostic driver; explicit `NOT_CHECKED` conditions; driver cleanup rules.
- `IVirtualizationBackend` and `IntelVirtualizationBackend`.
- `VmxLifecycleController`: thread-safe state machine (`Uninitialized`, `CheckingCapabilities`, `NotSupported`, `Blocked`, `ReadyForReview`, `Failed`, `Stopped`) with transition validation, check tokens and history.
- `VmxDiagnostics`: structured events, text report, JSON serialization, failure reasons (`VmxFailureReason`).
- `ProcessorDiagnostic` per logical processor (index, vendor, VMX capability, result, check status, error code).
- `hv_vmx_diagnostics.exe` with `--json`.
- Tests: `hv_vmx_capability_tests`, `hv_vmx_readiness_tests`, `hv_vmx_lifecycle_tests`; fakes in `tests/fakes/fake_vmx.hpp`.
- Docs: `docs/VMX_READINESS.md`, `docs/DRIVER_INTEGRATION.md`, `docs/BUILD.md`.

### Changed
- `CMakeLists.txt`: version 0.5.0, new sources/targets/option `HV_BUILD_VMX_DIAGNOSTICS`, `hv_core` links `Threads::Threads`.
- `README.md`: v0.5.0 section.

### Unchanged
- All v0.4.0 and earlier public APIs, the diagnostic driver and the driver protocol (v1).

### Known limitations
- Kernel-only checks (`IA32_FEATURE_CONTROL`, VMX MSRs, CR0/CR4 fixed bits, VMXON) stay `NOT_CHECKED`.
- Intel only; AMD SVM is reported as unsupported.
- Sources were not compiled or run in the authoring environment; see `docs/BUILD.md`.

## [0.4.0] - Windows Platform Backend and diagnostic driver
`WindowsPlatformBackend`, `CpuCapabilityManager`, `PlatformCompatibilityManager`, `KernelDriverInterface`, `DriverDiagnostics`, WDM diagnostic driver (protocol v1), `hv_platform_diagnostics`.

## [0.3.1]
Source audit and platform preflight; see `docs/AUDIT_V0_3_1.md`.
