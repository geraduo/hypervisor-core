# Intel VT-x Readiness Model

## Scope

v0.5.0 introduced and v0.6.0 retains a **user-mode readiness model** for an
Intel VT-x foundation. It answers one question:

> Can a later kernel-stage implementation be reviewed against the evidence
> collected here?

It does **not** answer "can VMXON run on this machine?", and the best possible
verdict, `ReadyForReview`, is deliberately named to prevent that reading.

No code in this release executes `VMXON`, reads or writes a VMCS, enters a guest,
configures hardware EPT, accesses an MSR, or touches physical memory.

## Layers

```text
CPUID (unprivileged) + per-CPU CPUID coverage
        |
        v
VmxCapabilityManager        -> VmxCapabilityReport
        |
        v
Windows platform + compatibility policy + driver protocol
        |
        v
VmxReadinessManager         -> VmxReadinessReport
        |
        v
VmxLifecycleController      -> VirtualizationBackendState + journal
        |
        v
text / JSON report
```

Every layer returns both a verdict and an error classification. A missing
component produces a defined verdict (`NotSupported`, `Blocked`, `NotChecked`),
not an exception and not a false success.

## Capability evaluation

`VmxCapabilityManager` reuses `CpuCapabilityManager` for CPU identity and
user-mode eligibility, then adds per-logical-processor observation.

### Per-processor semantics

- The native probe pins the calling thread to each active processor group,
  queries CPUID leaf 1, and restores the previous affinity.
- An observation that did not read CPUID is **not** treated as "VMX absent". It
  becomes `CheckStatus::NotChecked`, and the overall support level becomes
  `VmxSupportLevel::NotChecked` with reason `PerProcessorCheckIncomplete`.
- A malformed observation set (empty list, duplicate processor index) is an
  internal `InvalidArgument` with reason `CpuidReportInvalid`.
- When no `ILogicalProcessorProbe` is supplied, only the calling processor is
  observed and `per_processor_checked` stays false.

### Support levels

| Level | Meaning |
| --- | --- |
| `NotChecked` | The capability could not be established, for example a partial per-CPU probe |
| `NotSupported` | Vendor, VMX bit or platform rule out the implemented backend |
| `ReviewRequired` | VMX is reported but a hypervisor environment requires review |
| `Supported` | Every observed logical processor reports Intel VMX |

`Supported` is a statement about CPUID, not about IA32_FEATURE_CONTROL. CPUID
may advertise VMX while firmware has locked VMXON off. That is why the
capability layer always keeps `kernel_verification_required = true` and
`hardware_vmx_started = false`.

### Capability reasons

| Reason | Produced when |
| --- | --- |
| `UnsupportedCpuVendor` | Vendor is not `GenuineIntel` (AMD, Hygon, unknown, other) |
| `VmxNotReported` | Intel CPU does not advertise VMX, or at least one observed processor does not |
| `CpuidProbeFailed` | The CPUID probe itself failed |
| `CpuidReportInvalid` | The CPU report or observation set is internally contradictory |
| `PerProcessorCheckIncomplete` | No probe was supplied, a probe failed, or individual CPUID reads failed |
| `HypervisorPresentReviewRequired` | A hypervisor-present bit was observed alongside VMX |

## Readiness precedence

`VmxReadinessManager` evaluates five groups in order and resolves conflicts with
a fixed precedence:

1. **Internal failure (`Failed`)** — any component returned an unexpected
   error. This wins over every other outcome.
2. **Unsupported (`NotSupported`)** — CPU, platform or compatibility rules out
   the implemented backend.
3. **Blocked** — a required component is missing, disabled, incompatible or not
   verified.
4. **ReadyForReview** — all user-mode prerequisites passed.

The reason reported is the first one recorded in the winning class. A missing
driver is `Blocked`/`DriverNotConnected`, never an internal error. A protocol or
layout mismatch is `Blocked` with `VmxErrorClass::Protocol`.

### Readiness states

| State | Status | Meaning |
| --- | --- | --- |
| `NotEvaluated` | `NOT CHECKED` | No evaluation has run |
| `NotSupported` | `FAIL` | The implemented backend cannot run on this configuration |
| `Blocked` | `WARN` | A required component is missing or unverified |
| `ReadyForReview` | `PASS` | User-mode prerequisites passed; kernel-stage review may begin |
| `Failed` | `FAIL` | A diagnostic component failed internally |

### Readiness reasons

| Reason | Class | Meaning |
| --- | --- | --- |
| `PlatformNotChecked` | Blocked | Platform probe disabled by the caller |
| `PlatformBackendFailed` | Failed | The platform backend failed internally |
| `PlatformUnsupported` | NotSupported | Windows version/architecture is unsupported |
| `CompatibilityNotChecked` | Blocked | Compatibility probe disabled |
| `CompatibilityEvaluationFailed` | Failed | Compatibility evaluation failed |
| `CompatibilityUnsupported` | NotSupported | Compatibility policy rejects the platform |
| `CompatibilityReviewRequired` | Blocked | VBS/HVCI/WHPX state requires review |
| `HypervisorConflict` | Blocked | A known conflicting hypervisor environment was detected |
| `DriverNotChecked` | Blocked | Driver probe disabled |
| `DriverNotConnected` | Blocked | Device interface missing or driver not loaded |
| `DriverAccessDenied` | Blocked | The process may not open the device |
| `DriverQueryFailed` | Failed | The IOCTL failed unexpectedly |
| `DriverProtocolMismatch` | Blocked | Protocol version, layout or status mismatch |
| `DriverResponseInvalid` | Blocked/Failed | Malformed or contradictory response |
| `DriverArchitectureMismatch` | Blocked | Non-x64 driver |
| `DriverVersionIncompatible` | Blocked | Driver older than the minimum compatible version |

### Error classes

| Class | Meaning |
| --- | --- |
| `None` | Normal diagnostic verdict, including `NotSupported` and `Blocked` |
| `CpuProbe` | CPUID or CPU capability processing failed |
| `PlatformProbe` | Windows platform or compatibility probe failed |
| `Driver` | Unexpected transport/IOCTL failure |
| `Protocol` | Incompatible or malformed driver payload |
| `StateMachine` | Rejected lifecycle transition |
| `Internal` | Defensive fallback |

## Driver compatibility checks

When driver probing is enabled, the readiness layer checks, in order:

1. `Connect()` to the registered device interface.
2. `QueryPlatformInfo()` with the fixed 24-byte request.
3. Response magic (`0x47524450`).
4. Protocol version `1`.
5. Response structure size (`80`).
6. Driver protocol status `Success`.
7. Known architecture; the readiness layer requires x64.
8. Driver version at least `0.4`.
9. Nonzero active processors and zero reserved field.
10. Processor count against the Windows platform count (warning only).

Each comparison has a `*_checked` flag. If a step did not run, the flag stays
false and the detail says `NOT_CHECKED`; the report never invents a value.

## Lifecycle model

`VmxLifecycleController` implements `IVirtualizationBackend`.

| State | Meaning |
| --- | --- |
| `Uninitialized` | Constructed, no diagnostic run |
| `CheckingCapabilities` | Evaluation in progress |
| `NotSupported` | CPU/platform incompatible |
| `Blocked` | A required component is missing or unverified |
| `ReadyForReview` | User-mode review passed |
| `Failed` | Internal diagnostic failure; `Refresh()` may retry |
| `Stopped` | Terminal; no reinitialization |

### Transition rules

- `Initialize()` is valid only in `Uninitialized`. A second call is rejected
  with `InvalidState`, journals `LifecycleRejected`, sets
  `last_transition_failed` and does not change counters.
- `Refresh()` is valid in any non-`Stopped`, initialized state. It snapshots
  the committed state and report, runs the evaluation, and commits on success.
  On an internal failure it restores the snapshot and journals
  `EvaluationFailed`.
- `Stop()` is valid once. It releases the driver connection, sets `Stopped`,
  clears the blocking reason and is terminal for `Initialize()`, `Refresh()`
  and further `Stop()` calls.
- Every transition increments `generation`; only successful evaluations
  increment `successful_evaluations`.
- `hardware_vmx_started` and `hardware_start_supported` are always false, and
  `HardwareVmxStarted()` returns false for the whole 0.x line.

All transitions and readers are serialized with a mutex. The controller owns no
global or static virtualization state, so multiple instances are independent.

## Report fields

Both text and JSON output include:

- backend state, reason, error class, detail, generation and counters;
- transition-failure flag and message;
- CPU vendor, brand, family/model/stepping, VMX bit and hypervisor bit;
- per-processor observations and observed/enabled counts;
- platform state/support, compatibility verdict;
- every driver comparison with checked/compatible flags;
- explicit `hardware_vmx_started=false` and `hardware_start_supported=false`;
- a boundary block listing that VMXON, VMCS, VM-entry/exit, hardware EPT and
  privileged instructions were not executed.

## Honest results on the authoring host

The authoring machine is an AMD Ryzen 5 5600 VM under Microsoft Hyper-V. The
tool correctly reports:

```text
State: NotSupported
Reason: UnsupportedCpuVendor
Error class: None
Readiness: NotSupported
Hardware VMX started: no
Hardware VMX start supported: no
```

This is the intended behavior for an AMD host without an SVM backend. No Intel
hardware was available, so `Supported` and `ReadyForReview` were exercised only
through deterministic fakes, not on real VT-x hardware.
