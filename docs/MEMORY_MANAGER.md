# Software Memory Manager

## Purpose

The v0.6.0 memory layer models guest physical memory, address spaces,
second-stage translation, permissions, snapshots and diagnostics without
touching hardware virtualization. It is implemented in `namespace hv::memory`
and lives in `hv_core`.

The existing pre-0.6.0 API remains available:

- `hv::MemoryManager`
- `hv::SyntheticPageTable`
- `hv::NestedPageTable`, `hv::EptManager`, `hv::NptManager`
- `hv::Lifecycle`, `hv::Hypervisor`, `IHostBackend`, `IGuestBackend`,
  `hv::ProgramRunner`

The new layer reuses `SyntheticPageTable` for the first stage and adds a
separate hierarchical EPT model for the second stage. It does not rename or
silently replace the legacy components.

## Layers

```text
MemoryMappingManager        GVA -> GPA -> HPA, permissions, validation
  |             |             |
  |             |             +-- GuestMemoryManager ---- IMemoryBackingProvider
  |             |                     guest regions       real allocations
  |             |
  |             +------------------- EptManager ---------------- EptPageTable
  |                                   software EPT model       4-level hierarchy
  |
  +------------------------------- GuestAddressSpaceManager --- SyntheticPageTable
                                  address-space contexts       GVA -> GPA
```

Supporting components:

- `MemorySnapshotManager` — in-memory snapshots and restore.
- `MemoryDiagnostics` — counters, mapping ranges, snapshot summaries, JSON.
- `address_types.hpp` — typed addresses, permissions, statuses, results.

## Address spaces

`GuestAddressSpaceManager` owns named address-space contexts. A context is
created with `CreateAddressSpace(name)`, receives an `AddressSpaceId`, and can
be destroyed independently. Names must be 1-128 characters and unique.

Each context owns a `SyntheticPageTable`. The manager adds:

- `Map`, `Unmap`, `Protect` for 4 KiB guest pages;
- `Query` for page-aligned mapping information;
- `Translate` for GVA -> GPA with permission checking;
- `EnumerateMappings`, `ClearMappings`;
- context lifecycle and enumeration.

The same GVA in two contexts resolves independently. There is no implicit
default context: an invalid or zero id returns `InvalidContext`.

Example:

```cpp
GuestAddressSpaceManager spaces;
const AddressSpaceId id = spaces.CreateAddressSpace("user").id;
spaces.Map(id, GuestVirtualAddress{0x1000}, GuestPhysicalAddress{0x2000},
           Permissions{true, true, false});

const TranslationResult t =
    spaces.Translate(id, GuestVirtualAddress{0x1234}, Access::Read);
// t.guest_physical == 0x2234
```

## Guest memory and backing storage

`GuestMemoryManager` models explicit guest RAM regions. Add a region with a
name, size, permissions, optional GPA base and alignment:

```cpp
FakeMemoryBackingProvider backing;
GuestMemoryManager memory(backing);

GuestRegionRequest request;
request.name = "ram";
request.size = 4 * kSmallPageSize;
request.permissions = Permissions{true, true, false};
const GuestRegionResult region = memory.AddRegion(request);
```

Rules:

- the name is required and unique;
- the size must be a nonzero multiple of 4 KiB;
- the alignment must be a supported power of two from 4 KiB to 1 GiB;
- an explicit GPA base must be aligned to the requested alignment and must not
  overlap another region;
- a zero GPA base asks the manager to choose a non-overlapping address;
- every region owns a real allocation;
- removing a region releases its allocation;
- `ReleaseAll` releases every region.

`IMemoryBackingProvider` defines the storage contract. The provided
`FakeMemoryBackingProvider`:

- validates size and alignment;
- uses real `operator new` with aligned allocation;
- assigns monotonically increasing synthetic HPAs;
- tracks allocated bytes, allocation count and high-water mark;
- enforces a configurable byte limit;
- supports injected allocation failures for OOM tests;
- supports `Shutdown`, `ReleaseAll` and re-initialization;
- provides `Read`, `Write`, `TryGetBlock` and `TryResolve`.

The synthetic HPA is not a dereferenceable pointer. It identifies an allocation
owned by the provider. All data access goes through the provider.

`GuestMemoryManager` operations:

| Operation | Description |
| --- | --- |
| `AddRegion` | Validate, allocate backing, register the region |
| `RemoveRegion` | Release backing and unregister |
| `ReleaseAll` | Release every region |
| `GetRegion` / `GetRegionById` | Read metadata |
| `EnumerateRegions` | List regions |
| `QueryPage` | Translate a 4 KiB GPA to its region HPA |
| `EnumeratePages` | List the pages of a region |
| `Translate` | GPA -> HPA with permission checking |
| `Read` / `Write` | Permission-checked access through the provider |
| `ReadDirect` / `WriteDirect` | Snapshot/diagnostic access that bypasses region permissions but still requires registered, backed memory |
| `SetRegionPermissions` | Change region permissions |

## Two-stage mapping manager

`MemoryMappingManager` composes the first and second stages and is the preferred
entry point for tests and the demo.

```cpp
FakeMemoryBackingProvider backing;
GuestMemoryManager memory(backing);
GuestAddressSpaceManager spaces;
EptManager ept;
MemoryDiagnostics diagnostics;
MemoryMappingManager mappings(spaces, ept, memory, &diagnostics);
```

Operations:

| Operation | Stage | Notes |
| --- | --- | --- |
| `Map` | GVA -> GPA | Direct guest mapping |
| `Unmap` | GVA -> GPA | Direct guest unmap |
| `Protect` | GVA -> GPA | Direct guest permission change |
| `Query` / `GetMappingInfo` | both | Combined mapping information |
| `MapNested` | GPA -> HPA | Direct EPT mapping |
| `UnmapNested` | GPA -> HPA | Direct EPT unmap |
| `ProtectNested` | GPA -> HPA | Direct EPT permission change |
| `MapAll` | both | Maps guest and nested stages; the nested target must match the region backing |
| `UnmapAll` | both | Removes the guest mapping and the nested mapping for the same GPA |
| `ProtectAll` | both | Applies permissions to both stages |
| `Translate` | both | Full GVA -> GPA -> HPA pipeline |
| `TranslatePhysical` | GPA -> HPA | Backing + EPT agreement check |
| `Read` / `Write` | both | Permission-checked access through both stages |
| `Execute` | both | Execute-permission check only; no code execution |
| `Validate` / `ValidateAll` | both | Checks mappings, backing and EPT consistency |

`MapAll` requires:

1. the GPA is covered by a registered guest region and a 4 KiB page;
2. any existing EPT mapping points at the same backing HPA;
3. the guest mapping does not already exist.

If the guest mapping fails after a new nested mapping was created, the nested
mapping is rolled back.

`Translate` checks both the guest-region backing and the EPT. If the two host
addresses disagree, it returns `InvalidMapping`. This agreement rule is specific
to this software model: it keeps the region registry and the EPT model from
drifting apart.

`UnmapAll` and `ProtectAll` affect the nested page for the same GPA. EPT
mappings are shared second-stage state, so removing or protecting one also
affects any other address space that maps the same GPA. Use the stage-specific
operations when that is not desired.

## Diagnostics

`MemoryDiagnostics` records:

- successful and failed translations;
- permission denials;
- mapping, unmapping and protection operations;
- snapshot creation, restoration and failures;
- mapped pages, mapping ranges, address spaces and guest regions;
- allocated backing bytes;
- snapshot summaries.

`Refresh(spaces, ept, memory, snapshots)` recomputes gauges and coalesces
mapping ranges. `ToJson()` emits a self-contained report with:

- `version`: `0.6.0`;
- `model`: `software`;
- `hardware_ept`: `false`;
- statistics;
- guest and EPT mapping ranges;
- snapshot metadata and checksums.

The JSON is diagnostic data, not a stable external API.

## Snapshots

`MemorySnapshotManager` captures guest-region bytes, region metadata,
address-space mappings, EPT mappings and permissions. Restore verifies the
snapshot before changing live state and requires the live region layout to match.
See [MEMORY_SNAPSHOTS.md](MEMORY_SNAPSHOTS.md).

## Thread safety

Each manager owns its state behind a mutex:

- `GuestAddressSpaceManager` serializes all address-space operations;
- `GuestMemoryManager` serializes region and access operations;
- `EptPageTable` / `EptManager` serialize hierarchy operations;
- `MemoryMappingManager` serializes combined operations;
- `MemorySnapshotManager` serializes snapshot operations;
- `MemoryDiagnostics` serializes counters and report data;
- `FakeMemoryBackingProvider` serializes allocations and data access.

`MemoryMappingManager` holds its own mutex while calling the component managers.
Those managers never call back into `MemoryMappingManager`, so there is no lock
cycle in the current design. Snapshot restore holds the snapshot mutex while
calling the component managers; callers must not call snapshot operations from a
provider callback.

## Legacy compatibility

- `hv::SyntheticPageTable` keeps its original `Map`, `Unmap`, `Translate` and
  `MappingCount` behavior. `Protect`, `Query`, `Enumerate` and `Clear` are
  additive.
- `hv::MemoryManager` has a new constructor taking
  `hv::memory::EptManager&` while retaining the original
  `hv::NestedPageTable&` constructor. Both compile and are covered by tests.
- `hv::NestedPageTable` and its aliases are unchanged.
- `hv::Lifecycle`, `hv::Hypervisor`, `IHostBackend`, `IGuestBackend` and
  `hv::ProgramRunner` are unchanged.
- The legacy and typed address types coexist through explicit conversion
  helpers.

## Limits

- The model is 48-bit and single-process.
- First-stage pages are 4 KiB; the software EPT model supports 4 KiB, 2 MiB and
  1 GiB.
- Guest regions are explicit test/demo RAM models, not a guest OS allocator.
- The backing provider is a synthetic in-process provider, not a kernel physical
  memory allocator.
- No hardware EPT, VMCS, VMX instruction, IOMMU, MMIO or physical-memory access
  is implemented.
- Snapshots are in-memory artifacts, not disk images or VM snapshots.

See [ADDRESS_TRANSLATION.md](ADDRESS_TRANSLATION.md),
[EPT_MODEL.md](EPT_MODEL.md), [MEMORY_SNAPSHOTS.md](MEMORY_SNAPSHOTS.md) and
[UNSUPPORTED.md](UNSUPPORTED.md).
