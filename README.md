# Geraduo Hypervisor Core v0.6.0

A C++20 educational hypervisor-core project with a Windows platform diagnostic
layer, a software memory model (GVA → GPA → HPA), an Intel VT-x Virtualization
Foundation and an optional read-only WDK diagnostic kernel driver.

**v0.6.0 adds a software memory layer. It is a model, not hardware EPT.** The
new layer provides strongly typed guest-virtual, guest-physical and
host-physical addresses, address spaces, guest-region backing, a four-level
software EPT hierarchy, two-stage translation, snapshots, diagnostics and a
runnable demo. It does not execute VMXON, does not create a VMCS, does not enter
a guest and does not program hardware EPT/NPT structures.

The VT-x verdict vocabulary from v0.5.0 is unchanged. The best user-mode verdict,
`ReadyForReview`, still means *ready for kernel-stage review only* — never
*ready for hardware VMXON*.

## What is preserved from v0.5.0 and earlier

All existing components and public APIs remain available:

- Lifecycle state machine (`Lifecycle`)
- Hypervisor orchestration (`Hypervisor`)
- `IHostBackend` / `IGuestBackend`
- `MemoryManager` (both the legacy `NestedPageTable&` constructor and the new
  typed `hv::memory::EptManager&` constructor)
- `SyntheticPageTable` (existing operations unchanged; `Protect`, `Query`,
  `Enumerate`, `Clear` added)
- `NestedPageTable`, `EptManager`, `NptManager` (legacy namespace)
- `ProgramRunner`
- CPUID preflight (`RunPlatformPreflight`)
- Diagnostics primitives
- `WindowsPlatformBackend`, `CpuCapabilityManager`,
  `PlatformCompatibilityManager`
- `KernelDriverInterface`, `DriverDiagnostics`
- `VmxCapabilityManager`, `VmxReadinessManager`, `VmxLifecycleController`,
  `VmxDiagnostics`
- `hv_platform_diagnostics.exe`, `hv_vmx_diagnostics.exe`
- The shared protocol version 1: 24-byte request, 80-byte response,
  `IOCTL_GERADUO_QUERY_PLATFORM_INFO = 0x00226000`

`hv_core` remains platform-neutral. The Windows platform code lives in
`hv_platform`; the VT-x foundation lives in `hv_virtualization`, which links
`hv_platform`. The new memory model is part of `hv_core` in
`namespace hv::memory`.

## v0.6.0 additions

| Component | Purpose |
| --- | --- |
| `GuestVirtualAddress`, `GuestPhysicalAddress`, `HostPhysicalAddress`, `AddressSpaceId` | Strongly typed address domains and context handle |
| `MemoryStatus`, `MemoryResult`, `TranslationResult` | Precise statuses and structured results for every memory operation |
| `IMemoryBackingProvider` | Contract for real backing allocations |
| `FakeMemoryBackingProvider` | Aligned real allocations, tracking, limits, OOM injection, shutdown and re-initialization |
| `GuestAddressSpaceManager` | Named address-space contexts and GVA → GPA mappings |
| `GuestMemoryManager` | Guest RAM regions, permissions and GPA → HPA translation |
| `EptPageTable` | Four-level software EPT hierarchy (4 KiB / 2 MiB / 1 GiB) |
| `EptManager` (typed) | EPT mapping, ranges, protection, queries, validation and rollback |
| `MemoryMappingManager` | Combined GVA → GPA → HPA translation, access, protection and validation |
| `MemorySnapshotManager` | In-memory snapshots, integrity verification, restore and lifecycle |
| `MemoryDiagnostics` | Mapped pages, ranges, translations, denials, snapshots and JSON export |
| `hv_memory_demo.exe` | End-to-end software memory model demonstration |
| New tests | `hv_ept_manager_tests`, `hv_guest_memory_tests`, `hv_address_space_tests`, `hv_memory_snapshot_tests`, `hv_memory_integration_tests` |

## Architecture

The existing VT-x diagnostic stack is unchanged:

```text
hv_vmx_diagnostics.exe / applications
        |
        v
VmxLifecycleController  (state machine, rollback, thread safety, journal)
        |
        +-- VmxCapabilityManager --- CpuCapabilityManager --- ICpuidProvider
        +-- VmxReadinessManager --- WindowsPlatformBackend / KernelDriverInterface
        +-- VmxDiagnostics  (bounded event journal, text/JSON report)
```

The v0.6.0 memory model is layered inside `hv_core`:

```text
hv_memory_demo.exe / memory tests / applications
        |
        v
MemoryMappingManager  (two-stage translation, permissions, validation)
        |
        +-- GuestAddressSpaceManager --- SyntheticPageTable   GVA -> GPA
        |         (address-space contexts, 4 KiB guest pages)
        |
        +-- GuestMemoryManager --- IMemoryBackingProvider     GPA -> HPA
        |         (guest-region registry, real backing)       FakeMemoryBackingProvider
        |
        +-- EptManager --- EptPageTable                       GPA -> HPA
        |         (four-level software model, 4 KiB/2 MiB/1 GiB)
        |
        +-- MemorySnapshotManager  (in-memory state capture/restore)
        +-- MemoryDiagnostics      (counters, ranges, snapshots, JSON)
```

The memory layer is independent of the VT-x foundation: no VMX instruction,
VMCS field or EPTP is touched. Guest-region backing and the software EPT must
agree on the same HPA; a disagreement is rejected with `InvalidMapping`.

## Quick start

Canonical Visual Studio 2022 commands:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run the software memory demo:

```bat
build\Release\hv_memory_demo.exe
build\Release\hv_memory_demo.exe my_diagnostics.json
```

The demo creates an address space, registers a backed guest region, maps four
synthetic pages through both stages, prints a GVA → GPA → HPA translation,
demonstrates permission denials, creates and restores a snapshot, and exports
JSON diagnostics. The default output file is
`hv_memory_demo_diagnostics.json` in the current working directory. The JSON
contains `"model": "software"` and `"hardware_ept": false`.

Run the existing diagnostics tools:

```bat
build\Release\hv_vmx_diagnostics.exe
build\Release\hv_vmx_diagnostics.exe --json
build\Release\hv_vmx_diagnostics.exe --no-driver
build\Release\hv_platform_diagnostics.exe
```

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
- [Software memory manager](docs/MEMORY_MANAGER.md)
- [Address translation: GVA, GPA, HPA](docs/ADDRESS_TRANSLATION.md)
- [Software EPT model](docs/EPT_MODEL.md)
- [Memory snapshots](docs/MEMORY_SNAPSHOTS.md)
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
  (`/W4 /WX /permissive- /EHsc`), no warnings.
- Twelve test executables, all passing: the seven v0.5.0 suites plus
  `hv_ept_manager_tests`, `hv_guest_memory_tests`, `hv_address_space_tests`,
  `hv_memory_snapshot_tests` and `hv_memory_integration_tests`.
- `hv_memory_demo.exe` completed every stage and exported valid JSON with
  `"model": "software"` and `"hardware_ept": false`.
- `hv_vmx_diagnostics.exe` text and `--json` output; JSON parses successfully.
- The VT-x diagnostics report `NotSupported` / `UnsupportedCpuVendor` on this
  AMD host, with hardware-start flags explicitly false. This is the correct
  result: no AMD-V/SVM backend exists.
- The optional kernel driver from v0.5.0 remains unchanged: it was compiled and
  linked against WDK/SDK **10.0.28000.0** with MSVC **14.52.37025**, Release and
  Debug x64, using `/W4 /WX`. The driver still reports version **0.5.0**;
  protocol version 1, the wire layout and the IOCTL code are unchanged.

**Not validated:**

- **No hardware VT-x execution was tested.** The authoring host is AMD. No
  Intel machine was available, and no VMXON, VMCS, VM-entry/exit or hardware EPT
  code exists to test.
- **No hardware EPT was tested.** The v0.6.0 EPT model is software only; it does
  not program an EPTP or perform hardware page walks.
- The driver was not signed, installed, loaded or IOCTL-tested. Driver loading
  requires an elevated process, test signing (or a signed binary), compatible
  HVCI policy and an isolated test VM.
- The requested Visual Studio 17 2022 generator was not available; the installed
  Visual Studio 18 2026 generator was used. The VS2022 commands above are the
  documented canonical path, not a locally executed validation.

## Safety boundaries

This release does **not** implement or claim:

- `VMXON` / `VMXOFF`
- VMCS creation, loading, clearing, launch, VM entry or VM exit
- hardware EPT/NPT structures, EPTP programming or hardware second-level
  translation
- physical-memory access, arbitrary kernel memory access, or MMIO
- arbitrary process-memory access
- MSR or control-register access from user mode
- guest execution, interrupt injection, or device emulation
- an AMD-V/SVM backend
- hypervisor concealment, anti-cheat bypass, or anti-detection behavior
- a production-signed driver

The v0.6.0 memory layer uses synthetic HPA values that identify real in-process
backing allocations. Those values are never dereferenced as pointers. Snapshots
are in-memory artifacts, not VM snapshots or physical-memory captures.

`ReadyForReview` is not a hardware-readiness claim. IA32_FEATURE_CONTROL,
VMX capability MSRs, CR0/CR4 fixed bits, VMCS setup, EPT and every VMX
instruction remain unverified or unimplemented. See
[docs/VMX_READINESS.md](docs/VMX_READINESS.md),
[docs/EPT_MODEL.md](docs/EPT_MODEL.md) and
[docs/UNSUPPORTED.md](docs/UNSUPPORTED.md).
