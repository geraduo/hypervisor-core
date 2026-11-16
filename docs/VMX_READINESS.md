# VMX Readiness (v0.5.0)

v0.5.0 adds the infrastructure that **evaluates and tracks** whether a Windows x64 host is a
candidate for the Intel VT-x backend. It does **not** start a hypervisor.

> Not performed in this version: `VMXON`, VMCS creation, VM entry/exit, hardware EPT.
> `IsHypervisorRunning()` is always `false`; `StartVirtualization()` always returns `Status::Unsupported`.
> There is intentionally no `Ready` or `Running` state. The best outcome is `ReadyForReview`.

## Components

| Component | Header | Responsibility |
|---|---|---|
| `VmxCapabilityManager` | `hv/virtualization/vmx_capability_manager.hpp` | CPUID-based VMX capability per logical processor. Reuses `CpuCapabilityManager` and `RunPlatformPreflight`. |
| `ILogicalProcessorCpuidProvider` / `NativeLogicalProcessorCpuidProvider` | same | CPUID addressed to one logical processor (Windows: temporary, verified thread pinning, processor-group aware). |
| `VmxReadinessManager` | `vmx_readiness_manager.hpp` | Combines platform snapshot, capability, compatibility report and driver status into one verdict. |
| `IVirtualizationBackend` / `IntelVirtualizationBackend` | `virtualization_backend.hpp` | Single management interface: `Initialize`, `RunDiagnostics`, `StartVirtualization`, `Shutdown`, `GetState`. |
| `VmxLifecycleController` | `vmx_lifecycle_controller.hpp` | Thread-safe state machine. |
| `VmxDiagnostics` | `vmx_diagnostics.hpp` | Bounded structured event log, text and JSON reports. |
| `vmx_status.hpp` | | Shared enums and structs (`ProcessorDiagnostic`, `VmxFailureReason`, ...). |

Existing components are reused, not duplicated: `WindowsPlatformBackend`, `CpuCapabilityManager`,
`PlatformCompatibilityManager`, `KernelDriverInterface`, `DriverDiagnostics`. `VirtualizationBackendState`
is an alias of `VmxLifecycleState`; there is one state machine.

## States and transitions

```
Uninitialized ──► CheckingCapabilities ──► NotSupported | Blocked | ReadyForReview | Failed
NotSupported / Blocked / ReadyForReview / Failed ──► CheckingCapabilities (re-check)
any state except Stopped ──► Stopped            Stopped ──► Uninitialized | CheckingCapabilities
```

Everything else returns `Status::InvalidState` and leaves the state and history unchanged
(an event `TransitionRejected` is logged).

* **Repeated requests**: `Stop()` on a stopped controller is an idempotent success. `Initialize()` on a
  finished state performs a fresh check (an already retained driver connection is reused).
* **Concurrent requests**: `BeginCheck()` admits exactly one caller; the others get `InvalidState`.
  `IntelVirtualizationBackend` serializes `Initialize()`/`Shutdown()`; `RunDiagnostics()`/`GetState()` run concurrently.
* **Stop during a check**: each check has a token; `Stop()` invalidates it, so a stale result can never overwrite `Stopped`.
* **Partial failures**: each stage records its own reasons; later stages that depend on a failed stage are `NOT_CHECKED`.
* **Cleanup**: the driver connection opened by the readiness manager is kept only for `ReadyForReview`;
  every other verdict closes it. `Shutdown()` closes a retained connection. A connection opened by someone else is never closed.

## Verdicts and failure reasons

| Verdict → state | Reasons (`VmxFailureReason`) |
|---|---|
| `NotSupported` | `UnsupportedPlatform`, `UnsupportedArchitecture`, `UnsupportedVendor`, `VmxNotReported`, `PlatformUnsupported` |
| `Blocked` | `ProcessorNotChecked`, `DriverNotFound`, `DriverAccessDenied`, `DriverConnectionFailed`, `DriverProtocolMismatch`, `DriverQueryFailed`, `DriverDataInconsistent` |
| `Failed` | `CpuProbeFailed`, `ProcessorEnumerationFailed`, `PlatformBackendFailed`, `PlatformCompatibilityFailed`, `DiagnosticsFailed`, `InternalError` |
| `ReadyForReview` | none. Warnings (CPUID hypervisor bit, VBS/HVCI configured) are reported but do not block. |

Precedence when several categories occur: `Failed` > `NotSupported` > `Blocked` > `ReadyForReview`.
`Evaluate()` returns an error only for `Failed`. `IntelVirtualizationBackend::Initialize()` returns `Ok` only for
`ReadyForReview` (`NotSupported` → `Status::Unsupported`, `Blocked` → `InitializationFailed`, `Failed` → the failing status).

`VmxReadinessOptions::require_driver = false` downgrades driver problems to `WARN`; kernel-only checks remain `NOT_CHECKED`.

## Multiprocessor model

`ProcessorDiagnostic` is produced for **every** logical processor:

| Field | Meaning |
|---|---|
| `cpu_index` | Index across all processor groups |
| `vendor` | CPUID vendor string of that processor |
| `vmx_capability` | `CpuidReported`, `NotReported`, `UnsupportedVendor`, `NotChecked` |
| `check_status` | `PASS`, `FAIL`, `NOT_CHECKED` |
| `error_code` | `hv::Status` of the probe |
| `diagnostic_result` | Human-readable result |

A result is never copied to another processor. A processor that cannot be probed stays `NOT_CHECKED` with its error
code, and the system verdict becomes inconclusive (`Blocked` / `ProcessorNotChecked`), never "supported".
One confirmed negative makes the system `NotSupported`.

The native provider verifies with `GetCurrentProcessorNumberEx` that the thread really runs on the requested processor
and restores the previous affinity; if either step fails the result is discarded.

## Explicitly unverified conditions

Always `NOT_CHECKED` in v0.5.0 (they need kernel mode and are not performed by the diagnostic driver):

* `vmx.kernel.feature_control` – `IA32_FEATURE_CONTROL` lock / VMXON-enable bits (firmware can lock VMX off)
* `vmx.kernel.fixed_bits` – VMX capability MSRs, CR0/CR4 fixed bits
* `vmx.kernel.vmxon` – VMXON permission

`VmxReadinessReport::Unverified()` returns every `NOT_CHECKED` condition.

## Diagnostics and JSON

`VmxDiagnostics` stores events (`sequence`, `timestamp_ms`, level, code, component, message, `error_code`, optional `cpu_index`).
`FormatVmxReportJson` emits compact JSON with: `version`, `state`, `hypervisor_running` (always `false`),
`hardware_virtualization_started` (always `false`), `readiness` (`cpu`, `vmx_capability`, `processors[]`, `platform`,
`driver`, `compatibility`, `reasons[]`, `conditions[]`, `unverified[]`), `transitions[]`, `events[]`, `driver_events[]`.
Strings are escaped per RFC 8259. Check tokens are `PASS`, `FAIL`, `WARN`, `INFO`, `NOT_CHECKED`.

## Tool

```powershell
hv_vmx_diagnostics.exe          # text
hv_vmx_diagnostics.exe --json   # JSON
```

Exit code: `0` report produced (verdict other than `Failed`), `1` `Failed` or no report, `2` invalid arguments.

## Limitations

* Intel VT-x only; AMD SVM is reported as `UnsupportedVendor`.
* CPUID reporting VMX is not proof that `VMXON` is allowed.
* VBS/HVCI come from registry configuration, not runtime state.
* The Windows code paths (processor pinning, driver) were not compiled or run in the authoring environment; validate on Windows 10/11 x64.
