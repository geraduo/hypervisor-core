# Architecture

## Overview

Geraduo v0.6.0 is split into five layers:

1. **Portable core (`hv_core`)** — lifecycle, orchestration, synthetic memory,
   the v0.6.0 software memory model, page-table abstractions, program execution
   and CPUID preflight.
2. **Software memory model (inside `hv_core`, `namespace hv::memory`)** —
   strongly typed GVA/GPA/HPA addresses, address spaces, guest-region backing,
   a four-level software EPT model, two-stage mapping, snapshots and memory
   diagnostics. It is a model, not a hardware EPT implementation.
3. **Windows platform layer (`hv_platform`)** — Win32 probes, CPU capability
   analysis, compatibility policy, driver client and diagnostics journal.
4. **Intel VT-x foundation (`hv_virtualization`)** — capability, readiness,
   lifecycle and diagnostics for a future hardware backend. It links
   `hv_platform`; `hv_core` stays platform-neutral.
5. **Optional WDK driver (`GeraduoDriver`)** — one read-only diagnostic IOCTL.

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

The v0.6.0 software memory model is layered inside `hv_core`:

```text
hv_memory_demo.exe / memory tests / applications
        |
        v
MemoryMappingManager  (two-stage translation, permission checks, validation)
        |
        +-- GuestAddressSpaceManager --- SyntheticPageTable   GVA -> GPA
        |         (address-space contexts, 4 KiB guest pages)
        |
        +-- GuestMemoryManager --- IMemoryBackingProvider     GPA -> HPA
        |         (guest-region registry, real backing)       FakeMemoryBackingProvider
        |
        +-- EptManager --- EptPageTable                       GPA -> HPA
        |         (4-level software model, 4 KiB/2 MiB/1 GiB)
        |
        +-- MemorySnapshotManager  (in-memory state capture/restore)
        +-- MemoryDiagnostics      (counters, ranges, snapshots, JSON)
```

The memory model is independent of the VT-x foundation. `MemoryMappingManager`
uses only software page tables and a synthetic backing provider; no VMX
instruction, VMCS field or EPTP is touched.

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
- **Typed memory addresses.** The v0.6.0 memory layer keeps GVA, GPA and HPA in
  distinct C++ types. Legacy `uint64_t` aliases remain available through
  explicit conversion helpers, but new code cannot mix the three domains by
  accident.
- **Real backing, synthetic addresses.** Every guest region owns a genuine
  aligned allocation. Its synthetic HPA identifies that allocation; it is never
  dereferenced and never fabricated for missing storage.
- **One model, one agreement check.** The guest-region registry and the software
  EPT must translate a GPA to the same HPA. `MemoryMappingManager` rejects a
  disagreement with `InvalidMapping`.
- **Verify before mutate.** Snapshot restore checks metadata, counts, checksum,
  region layout, alignment and live backing before changing live mappings or
  bytes.
- **No false hardware claims.** The EPT model has no EPTP, VMCS, VMX
  instruction, page-walk cache or hardware invalidation semantics. Diagnostics
  reports `"model": "software"` and `"hardware_ept": false`.

## Existing core components

| Component | Responsibility |
| --- | --- |
| `Lifecycle` | Validates state transitions (stopped, initializing, running, shutting down) |
| `Hypervisor` | Coordinates host and guest backends with rollback on failure |
| `IHostBackend` / `IGuestBackend` | Host and synthetic-guest interfaces |
| `MemoryManager` | Two-stage guest-virtual → guest-physical → host-physical translation; supports the legacy nested table and the typed software EPT manager |
| `SyntheticPageTable` | In-memory guest page table used by tests and the synthetic guest |
| `NestedPageTable` | Software flat nested page table abstraction |
| `EptManager` / `NptManager` | Legacy Intel/AMD-flavored aliases over nested page-table operations |
| `ProgramRunner` | Launches a host process and captures its exit code |
| `RunPlatformPreflight` | Unprivileged CPUID report with kernel-only checks left unchecked |
| `Diagnostics` | Existing generic diagnostics primitives |

The platform and virtualization layers consume `ICpuidProvider` and `Result`
from the core; they do not alter the existing translation or lifecycle behavior.

### v0.6.0 memory components

| Component | Responsibility |
| --- | --- |
| `hv::memory::GuestVirtualAddress` | Strongly typed guest-virtual address |
| `hv::memory::GuestPhysicalAddress` | Strongly typed guest-physical address |
| `hv::memory::HostPhysicalAddress` | Strongly typed synthetic host-physical address |
| `hv::memory::AddressSpaceId` | Nonzero address-space context handle |
| `IMemoryBackingProvider` | Contract for real allocations and HPA-addressable access |
| `FakeMemoryBackingProvider` | Aligned allocator, tracking, limits, OOM injection, shutdown |
| `GuestAddressSpaceManager` | Address-space contexts and GVA -> GPA mappings |
| `GuestMemoryManager` | Guest-region registry, backing and GPA -> HPA translation |
| `EptPageTable` | Four-level software EPT hierarchy with 4 KiB/2 MiB/1 GiB pages |
| `EptManager` (typed) | EPT mapping ranges, protection, queries, validation |
| `MemoryMappingManager` | Combined GVA -> GPA -> HPA translation and access |
| `MemorySnapshotManager` | Verified in-memory snapshots and restore |
| `MemoryDiagnostics` | Counters, ranges, snapshot summaries and JSON |

## Software memory model

The memory layer is built from cooperating managers rather than one monolithic
allocator:

1. `GuestAddressSpaceManager` owns contexts and first-stage page tables.
2. `GuestMemoryManager` owns explicit guest regions and their real backing.
3. `EptPageTable` / `EptManager` model the second-stage page table.
4. `MemoryMappingManager` composes both stages and enforces permissions.
5. `MemorySnapshotManager` captures and restores the model.
6. `MemoryDiagnostics` records counters, ranges, snapshots and JSON.

### Address spaces

Each address space owns a `SyntheticPageTable`. The same GVA in two contexts is
independent. Page size is 4 KiB. Mapping, unmapping, protection, querying and
translation return typed results with precise statuses.

### Guest memory

Guest regions are explicit models of guest RAM, not a guest OS allocator. Every
region owns a real aligned allocation through `IMemoryBackingProvider`. Region
names are unique; sizes and alignments are validated; overlaps are rejected.
`FakeMemoryBackingProvider` assigns synthetic HPAs and never substitutes an
arbitrary address for missing storage.

### EPT model

`EptPageTable` models PML4/PDPT/PD/PT indexing with 4 KiB, 2 MiB and 1 GiB
pages. It detects duplicate and overlapping mappings, supports protection and
validation, and rejects rather than silently splitting large pages. It is
separate from the legacy flat `hv::NestedPageTable`. See
[EPT_MODEL.md](EPT_MODEL.md).

### Two-stage translation

`MemoryMappingManager::Translate` performs:

```text
GVA --[guest page table]--> GPA --[region backing]--> HPA
                                 \--[software EPT]--> HPA
```

The two second-stage results must agree. Permissions are checked at the guest
mapping, the guest region and the EPT leaf. A missing mapping returns `Unmapped`;
an existing mapping with insufficient permissions returns `AccessDenied`.

### Snapshots

Snapshots are in-memory copies of region bytes, region metadata, guest mappings
and EPT mappings. Creation enforces a size limit. Restore verifies structure,
counts, checksum and live region layout before mutating state. They are not VM
snapshots or host physical-memory captures. See
[MEMORY_SNAPSHOTS.md](MEMORY_SNAPSHOTS.md).

### Concurrency and compatibility

Each manager serializes its state with a mutex. `MemoryMappingManager` holds its
mutex while calling component managers; components never call back into it.
Tests exercise concurrent EPT mapping, concurrent two-stage mapping and
concurrent guest access.

The legacy `hv::MemoryManager` keeps its `NestedPageTable&` constructor and adds
a `hv::memory::EptManager&` constructor. `hv::SyntheticPageTable` keeps its
original operations; `Protect`, `Query`, `Enumerate` and `Clear` are additive.
`hv::Lifecycle`, `hv::Hypervisor`, host/guest backends and `hv::ProgramRunner`
are unchanged.

See [MEMORY_MANAGER.md](MEMORY_MANAGER.md) and
[ADDRESS_TRANSLATION.md](ADDRESS_TRANSLATION.md) for the full contract.

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

The concrete `IVirtualizationBackend` since v0.5.0.

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
| `hv_ept_manager_tests` | EPT 4 KiB/2 MiB/1 GiB mapping, translation, conflicts, ranges, rollback, validation, concurrency, legacy `MemoryManager` integration |
| `hv_guest_memory_tests` | Region allocation, read/write, page-crossing access, permissions, validation, removal, `ReleaseAll`, OOM, provider lifecycle, enumeration |
| `hv_address_space_tests` | Context lifecycle, map/unmap/protect, offsets, isolation, two-stage access, permission layers, `ProtectAll`/`UnmapAll`, concurrent mapping |
| `hv_memory_snapshot_tests` | Creation, metadata, checksum, restore, permission/byte restoration, corruption, size limits, lifecycle, layout changes |
| `hv_memory_integration_tests` | `MemoryManager` + typed EPT, diagnostics/JSON, permission intersection, concurrent guest access, snapshot round-trip |
| `hv_windows_platform_tests` | Protocol layout, missing driver, reconnect, malformed/version-mismatched replies, platform state machine, CPU manager, fakes |
| `hv_platform_compatibility_tests` | Hyper-V/third-party/unrecognized environments, VBS/HVCI, WHPX, CPU precedence, error paths, JSON escaping |
| `hv_vmx_capability_tests` | Supported/unsupported vendor, missing VMX, partial coverage, hypervisor review, missing probe, probe failure, malformed observations, single-CPU failure, invalid CPU reports, formatting |
| `hv_vmx_readiness_tests` | ReadyForReview, missing/denied/failed driver, protocol/layout/architecture/version mismatch, processor-count warning, unsupported CPU/platform, disabled probes, internal failures, formatting |
| `hv_vmx_lifecycle_tests` | Happy path, double initialize, terminal stop, rejected early calls, refresh commit/rollback, failed initialize recovery, concurrency, journal bounds/thread safety, JSON escaping |

All twelve test executables use fakes or software models; no test requires Intel
hardware, a loaded driver or administrator rights.

## What v0.6.0 deliberately does not contain

- `VMXON`/`VMXOFF`, VMCS creation/loading/clearing, VM entry/exit.
- Hardware EPT/NPT, EPTP programming, VPID, MSR bitmaps, interrupt injection,
  guest execution.
- Hardware page walks, EPT invalidation, accessed/dirty bits or memory types.
- AMD-V/SVM or Windows Hypervisor Platform backends.
- Physical-memory or arbitrary process-memory access.
- MSR or control-register access from user mode.
- Driver signing, installation, loading or runtime IOCTL validation.

The software memory model is validated only against its own tests. It is not
evidence of hardware EPT correctness. The driver is compiled but not loaded or
IOCTL-tested in this environment; see
[docs/DRIVER_INTEGRATION.md](DRIVER_INTEGRATION.md) and
[docs/UNSUPPORTED.md](UNSUPPORTED.md).
