# Software EPT Model

## Scope

`hv::memory::EptPageTable` and `hv::memory::EptManager` are a **software model**
of a four-level second-stage page table. They model hierarchy, indexing, entry
validity, page sizes, permissions and conflict detection.

They do **not** program hardware EPT, do not create a VMCS, do not set an EPTP,
do not execute VMX instructions and do not perform hardware page walks. A
successful `Validate()` proves internal software consistency, not hardware
readiness.

The model is separate from the pre-existing flat `hv::NestedPageTable` (also
exported as `hv::EptManager` in the legacy namespace). Both remain available:

- `hv::NestedPageTable` / `hv::EptManager` — the original flat 4 KiB software
  nested table used by the old `hv::MemoryManager` constructor.
- `hv::memory::EptPageTable` / `hv::memory::EptManager` — the hierarchical model
  described here.

Keeping them separate avoids changing the behavior of the pre-0.6.0 API while
giving the new memory layer a structure that can later be compared with real EPT
semantics.

## Hierarchy

The table has four levels, each indexed by nine bits of the guest-physical
address:

| Level | Enum | Index shift | Leaf page size |
| --- | --- | ---: | ---: |
| PML4 | `EptEntryLevel::Pml4` | 39 | not a valid leaf |
| PDPT | `EptEntryLevel::Pdpt` | 30 | 1 GiB |
| PD | `EptEntryLevel::Pd` | 21 | 2 MiB |
| PT | `EptEntryLevel::Pt` | 12 | 4 KiB |

A leaf stores:

- the host-physical page base;
- the page size;
- the permissions;
- the level.

A branch is created lazily when a smaller page is mapped below it. Empty
branches are pruned after `Unmap`. The root is a PML4 node and cannot contain a
leaf.

## Supported page sizes

- 4 KiB (`kSmallPageSize`)
- 2 MiB (`kLargePageSize`)
- 1 GiB (`kHugePageSize`)

`EptPageTable::Config::allow_large_pages` can disable the two large sizes; a
large-page request then returns `MemoryStatus::Unsupported`. Large pages must be
aligned to their own size.

## Mapping rules

`Map`:

1. Validates the page size, configuration, alignment and the 48-bit model limits.
2. Walks or creates the hierarchy down to the leaf level.
3. Rejects a mapping that would overlap an existing larger leaf
   (`AlreadyMapped`).
4. Rejects a larger mapping if any smaller leaf already exists inside its range
   (`AlreadyMapped`).
5. Rejects a duplicate leaf at the same level (`AlreadyMapped`).
6. Stores the leaf and increments the mapping counter.

The model deliberately **rejects** rather than silently splits a large page into
smaller pages. A future implementation can add explicit splitting, but hiding it
behind an ordinary `Map` would make tests and diagnostics ambiguous.

`Unmap`:

- Removes the leaf that covers the address.
- Requires the caller to pass the base address of that leaf; an address inside a
  larger page returns `AlignmentError`.
- Prunes empty branches.
- Returns `Unmapped` when no leaf exists.

`Protect`:

- Changes the permissions of the leaf that covers the address.
- Requires the exact leaf base, like `Unmap`.
- Does not change the host-physical target.

`Translate`:

- Validates the 48-bit model limit and the configured guest-physical limit.
- Finds the covering leaf, computes `HPA = leaf.host_page + offset`.
- Checks the leaf permissions for the requested `Access`.
- Reports the page size and hierarchy level.
- Returns `Unmapped` or `AccessDenied` without producing an HPA for a failed
  permission check.

`Query` returns the page-aligned guest-physical base, the host-physical base,
the page size, permissions and level.

`Validate` walks the whole hierarchy and checks:

- level depth and leaf-level consistency;
- absence of simultaneous leaf and branch entries at the same index;
- absence of leaves at the PML4 root;
- alignment of guest and host page bases;
- 48-bit limits and the configured guest-physical limit;
- the mapping counter matches the number of leaves.

## Manager layer

`EptManager` wraps `EptPageTable` and adds:

- `MapRange` / `Map` convenience overloads;
- rollback of partially created range mappings;
- `GetMappingRanges` coalescing of contiguous mappings with the same page size,
  permissions and contiguous host addresses;
- `Enumerate`, `MappingCount`, `Validate`, `Clear`, `GetConfig`.

`MapRange` maps `page_count` consecutive pages. If any page fails, every page
created by that call is removed and the original error is returned. Pages that
existed before the call are not touched.

Mappings may share a host page in this model; the table does not enforce a
one-to-one HPA ownership relation. Higher layers such as `MemoryMappingManager`
add the region-backing agreement check.

## Concurrency

`EptPageTable` and `EptManager` protect their state with mutexes. A mapping
operation is atomic with respect to other operations on the same instance.
Independent instances do not share state. Tests cover concurrent mapping from
four threads.

## Example

```cpp
using namespace hv::memory;

EptManager ept;

// 4 KiB page
ept.Map(GuestPhysicalAddress{0x1000}, HostPhysicalAddress{0x9000},
        Permissions{true, true, false});

// 2 MiB page
ept.Map(GuestPhysicalAddress{0x20'0000}, HostPhysicalAddress{0x40'0000},
        Permissions{true, true, false}, kLargePageSize);

const EptTranslationResult result =
    ept.Translate(GuestPhysicalAddress{0x20'1234}, Access::Read);
// result.host_physical == 0x40'1234
// result.page_size == kLargePageSize
// result.level == EptEntryLevel::Pd
```

## Error mapping

| Condition | Status |
| --- | --- |
| Unsupported page size | `InvalidMapping` |
| Large pages disabled | `Unsupported` |
| Misaligned guest or host base | `AlignmentError` |
| Address outside the model or configured limit | `InvalidAddress` |
| Duplicate or overlapping mapping | `AlreadyMapped` |
| Missing leaf | `Unmapped` |
| Permission mismatch | `AccessDenied` |
| Internal hierarchy/counter inconsistency | `InvalidMapping` or `InternalError` |

## Limitations

- No hardware EPTP, VMCS, VMX instruction or page-walk execution.
- No accessed/dirty flags, memory types, SVE/SPT attributes, VPID or EPT
  invalidation semantics.
- No huge-page splitting or merging.
- No guest-physical address canonicality beyond the 48-bit model limit.
- No host-physical memory ownership by the EPT layer; it only records HPA values.
  `IMemoryBackingProvider` owns the storage.
- `Validate` checks the model, not hardware behavior.

See [ADDRESS_TRANSLATION.md](ADDRESS_TRANSLATION.md) and
[UNSUPPORTED.md](UNSUPPORTED.md).
