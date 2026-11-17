# Architecture

## Overview

Geraduo v0.5.0 is split into four layers:

1. **Portable core (`hv_core`)** — lifecycle, orchestration, synthetic memory,
   page-table abstractions, program execution and CPUID preflight.
2. **Windows platform layer (`hv_platform`)** — Win32 probes, CPU capability
   analysis, compatibility policy, driver client and diagnostics journal.
3. **Intel VT-x foundation (`hv_virtualization`)** — capability, readiness,
   lifecycle and diagnostics for a future hardware backend. It links
   `hv_platform`; `hv_core` stays platform-neutral.
4. **Optional WDK driver (`GeraduoDriver`)** — one read-only diagnostic IOCTL.

The user-mode layers are ordinary C++20. The kernel driver shares only the
fixed-layout protocol header and uses WDM kernel APIs; it links no C++ runtime,
STL containers or exceptions.

```text
hv_vmx_diagnostics.exe / applications
        |
        v
VmxLifecycleController  (state machine, rollback, mutex, journal)
        |
        +-- VmxCapabilityManager --- CpuCapabilityManager --- ICpuidProvider
        |         |                       NativeCpuidProvider (unprivileged CPUID)
        |         +-- ILogicalProcessorProbe
        |                 NativeLogicalProcessorProbe (affinity-pinned CPUID per CPU)
        |
        +-- VmxReadinessManager --- WindowsPlatformBackend --- IPlatformProvider
        |         |                       NativePlatformProvider (RtlGetVersion,
        |         |                       GetNativeSystemInfo, GlobalMemoryStatusEx,
        |         |                       GetProductInfo, WinHvPlatform probe)
        |         |
        |         +-- PlatformCompatibilityManager --- ICpuidProvider
        |         |       ICompatibilityEnvironment
        |         |       NativeCompatibilityEnvironment (registry + WHvGetCapability)
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

The platform diagnostics tool and the earlier core remain available:

```text
hv_platform_diagnostics.exe
        |
        +-- WindowsPlatformBackend
        +-- CpuCapabilityManager
        +-- PlatformCompatibilityManager
        +-- KernelDriverInterface
```

## Design rules

- **No false readiness.** User-mode code can never return "hardware
  virtualization ready". The best VT-x capability level is `Supported`; the best
  readiness state is `ReadyForReview`, explicitly defined as ready for
  kernel-stage review only.
- **No hidden privilege escalation.** The platform and virtualization layers
  read only unprivileged data. Registry reads are read-only. The driver is
  opened through a registered device interface, never a caller-supplied raw
  path. No MSR, control register or VMX instruction is touched.
- **Explicit capability state.** Checks can be `Pass`, `Warn`, `Fail`, `Info` or
  `NotChecked`. Kernel-only facts stay `NotChecked` in user mode; an unobserved
  processor is `NotChecked`, never `Fail`.
- **Separate verdicts from errors.** `NotSupported` and `Blocked` are normal
  diagnostic verdicts with `VmxErrorClass::None`. Only unexpected internal
  failures produce a `Result` error and `VmxErrorClass` other than `None`.
- **Deterministic test seams.** Provider interfaces are injected, so tests use
  fake CPUID, platform, compatibility, per-processor probe and driver data
  without touching hardware or installing a driver.
- **Fixed ABI.** The driver protocol is a packed, versioned, 24-byte request and
  80-byte response. There are no pointers, variable-length arrays or
  caller-provided kernel addresses.
- **No global virtualization state.** The lifecycle controller, journal and
  managers are ordinary objects. Nothing is stored in a static or singleton,
  so independent instances do not interfere.

## Existing core components

| Component | Responsibility |
| --- | --- |
| `Lifecycle` | Validates state transitions (stopped, initializing, running, shutting down) |
| `Hypervisor` | Coordinates host and guest backends with rollback on failure |
| `IHostBackend` / `IGuestBackend` | Host and synthetic-guest interfaces |
| `MemoryManager` | Two-stage guest-virtual → guest-physical → host-physical translation |
| `SyntheticPageTable` | In-memory guest page table used by tests and the synthetic guest |
| `NestedPageTable` | Software nested page table abstraction |
| `EptManager` / `NptManager` | Intel/AMD-flavored wrappers over nested page-table operations |
| `ProgramRunner` | Launches a host process and captures its exit code |
| `RunPlatformPreflight` | Unprivileged CPUID report with kernel-only checks left unchecked |
| `Diagnostics` | Existing generic diagnostics primitives |

The platform and virtualization layers consume `ICpuidProvider` and `Result`
from the core; they do not alter the existing translation or lifecycle behavior.

## Windows platform layer

### `WindowsPlatformBackend`

Owns a small state machine (`Uninitialized`, `Initialized`, `Failed`) and caches
one provider snapshot. `Initialize()` probes once; `Refresh()` probes again;
`Shutdown()` releases cached data. All getters reject access unless initialized.

The backend validates the provider snapshot before accepting it:

- OS major version is nonzero.
- Page size is nonzero and a power of two.
- Logical processor count is nonzero.

### `CpuCapabilityManager`

Calls the existing `RunPlatformPreflight` first, then adds:

- vendor and brand strings;
- family/model/stepping and maximum CPUID leaves;
- VMX, SVM and hypervisor-present bits;
- `CpuEligibility` with no "ready" state;
- text and JSON reports.

The manager performs no privileged instruction access.

### `PlatformCompatibilityManager`

Combines preflight data with:

- Device Guard `EnableVirtualizationBasedSecurity` registry value;
- HVCI `Enabled` registry value;
- optional `WHvGetCapability(WHvCapabilityCodeHypervisorPresent)`.

It classifies the environment as no hypervisor, Microsoft Hyper-V, third-party,
unrecognized or unknown. CPU unsupported takes precedence over VBS/HVCI/conflict
warnings. Missing registry values are treated conservatively; a missing HVCI
value only matters when VBS is enabled.

### `KernelDriverInterface` and `DriverDiagnostics`

`KernelDriverInterface` is a typed client:

- `Connect()` opens the registered device interface.
- `Disconnect()` closes it.
- `IsConnected()` reports current state.
- `QueryPlatformInfo()` sends one request and validates every response field.
- `GetLastError()` returns the last detailed error.

A bounded, thread-safe journal records connection, IOCTL, version, malformed
response and protocol-status failures. It works even when the driver is absent.

## Intel VT-x foundation

### `VmxCapabilityManager`

Reuses `CpuCapabilityManager` for CPU identity, then classifies Intel VMX
support. With an `ILogicalProcessorProbe`, it observes every active logical
processor; affinity is restored after each probe. Without a probe, only the
calling processor is observed and `per_processor_checked` stays false.

Key rules:

- non-Intel vendor → `NotSupported`/`UnsupportedCpuVendor`, no per-CPU probe;
- missing VMX on any observed processor → `NotSupported`/`VmxNotReported`;
- failed CPUID read on one processor → that processor `NotChecked`, overall
  `NotChecked`/`PerProcessorCheckIncomplete`;
- malformed or contradictory CPU report → `InvalidArgument`/`CpuidReportInvalid`;
- hypervisor-present bit with VMX → `ReviewRequired`/`HypervisorPresentReviewRequired`.

`Supported` means CPUID advertised VMX on every observed processor. It does not
mean IA32_FEATURE_CONTROL permits VMXON.

### `VmxReadinessManager`

Combines the capability report with the Windows platform backend, compatibility
policy and driver protocol. It evaluates five groups:

1. VT-x capability (error, unsupported or blocked classification).
2. Windows platform backend (initialize or refresh, then read system/capability
   reports).
3. Compatibility policy (Hyper-V, VBS, HVCI, WHPX).
4. Driver connect/query and protocol compatibility.
5. Kernel-stage checks and the hardware boundary, which stay `NOT CHECKED` by
   design and do not block `ReadyForReview`.

Precedence is `Failed` > `NotSupported` > `Blocked` > `ReadyForReview`.
Expected conditions (missing device, access denied, incompatible protocol) are
verdicts, not internal errors. Details are in
[docs/VMX_READINESS.md](VMX_READINESS.md).

### `VmxLifecycleController`

The concrete `IVirtualizationBackend` of v0.5.0.

- Valid states: `Uninitialized`, `CheckingCapabilities`, `NotSupported`,
  `Blocked`, `ReadyForReview`, `Failed`, `Stopped`.
- `Initialize()` is valid only from `Uninitialized`; repeated calls are
  rejected, journaled and do not change counters.
- `Refresh()` is transactional: it snapshots the committed state and reports,
  runs the evaluation, and restores the snapshot on internal failure while
  recording `last_transition_failed`.
- `Stop()` releases the driver connection, clears the blocking reason and is
  terminal. It is the only terminal state.
- `generation` advances on every transition; `successful_evaluations` advances
  only on successful evaluations.
- All transitions and readers are serialized by a mutex. There is no global or
  static virtualization state.
- `hardware_vmx_started` and `hardware_start_supported` remain false in every
  code path, and `HardwareVmxStarted()` is a constant false.

### `VmxDiagnostics`

A bounded, thread-safe journal of lifecycle, capability, readiness, driver,
protocol and failure events. Text and JSON reports compose backend,
capability, readiness and event data. JSON strings are escaped by the shared
`hv/detail/report_json.hpp` helper. Timestamps are wall-clock UTC milliseconds.

## Test strategy

| Test | Coverage |
| --- | --- |
| `hv_tests` | Original lifecycle, translation, orchestration, program runner |
| `hv_preflight_tests` | Original CPUID validation and serialization |
| `hv_windows_platform_tests` | Protocol layout, missing driver, reconnect, malformed/version-mismatched replies, platform state machine, CPU manager, fakes |
| `hv_platform_compatibility_tests` | Hyper-V/third-party/unrecognized environments, VBS/HVCI, WHPX, CPU precedence, error paths, JSON escaping |
| `hv_vmx_capability_tests` | Supported/unsupported vendor, missing VMX, partial coverage, hypervisor review, missing probe, probe failure, malformed observations, single-CPU failure, invalid CPU reports, formatting |
| `hv_vmx_readiness_tests` | ReadyForReview, missing/denied/failed driver, protocol/layout/architecture/version mismatch, processor-count warning, unsupported CPU/platform, disabled probes, internal failures, formatting |
| `hv_vmx_lifecycle_tests` | Happy path, double initialize, terminal stop, rejected early calls, refresh commit/rollback, failed initialize recovery, concurrency, journal bounds/thread safety, JSON escaping |

All seven test executables use fakes or scripted transports; no test requires
Intel hardware, a loaded driver or administrator rights.

## What v0.5.0 deliberately does not contain

- `VMXON`/`VMXOFF`, VMCS creation/loading/clearing, VM entry/exit.
- Hardware EPT/NPT, VPID, MSR bitmaps, interrupt injection, guest execution.
- AMD-V/SVM or Windows Hypervisor Platform backends.
- Physical-memory or arbitrary process-memory access.
- MSR or control-register access from user mode.
- Driver signing, installation, loading or runtime IOCTL validation.

The driver is compiled but not loaded or IOCTL-tested in this environment; see
[docs/DRIVER_INTEGRATION.md](DRIVER_INTEGRATION.md) and
[docs/UNSUPPORTED.md](UNSUPPORTED.md).
