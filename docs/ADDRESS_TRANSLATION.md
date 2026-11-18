# Address Translation

## Three address domains

The v0.6.0 memory layer uses three distinct address domains. They are separate
C++ types in `namespace hv::memory`, so a value from one domain cannot be passed
to a function that expects another without an explicit conversion.

| Domain | Type | Meaning |
| --- | --- | --- |
| GVA | `GuestVirtualAddress` | An address in a guest address space. It is translated by the first stage. |
| GPA | `GuestPhysicalAddress` | An address in the guest-physical model. It names a guest RAM region and is translated by the second stage. |
| HPA | `HostPhysicalAddress` | A synthetic host-physical address assigned by `IMemoryBackingProvider`. It refers to real backing storage owned by the provider. |
| Context | `AddressSpaceId` | A nonzero handle for an address-space context. It is not an address. |

All four values are 64-bit at the storage level, but the type system carries the
semantic level. `AddressSpaceId{}` (zero) means "no context"; every other address
type can legitimately contain zero.

The pre-0.6.0 public API in `namespace hv` still uses `std::uint64_t` aliases
(`hv::GuestVirtualAddress`, `hv::GuestPhysicalAddress`,
`hv::HostPhysicalAddress`, `hv::AddressSpaceId`). The typed layer provides
explicit conversion helpers:

```cpp
hv::memory::GuestVirtualAddress gva{0x1000};

// Typed -> legacy
hv::GuestVirtualAddress legacy_gva = hv::memory::ToLegacy(gva);

// Legacy -> typed (the three legacy types are aliases, so the target names are
// distinct functions rather than overloads)
hv::memory::GuestVirtualAddress typed =
    hv::memory::GuestVirtualFromLegacy(legacy_gva);
```

`GuestPhysicalFromLegacy` and `HostPhysicalFromLegacy` are the corresponding
helpers for the other two domains.

## Address model

- Physical addresses (GPA and HPA) are limited to the architectural 48-bit
  window: `0 .. (1 << 48) - 1`.
- Guest virtual addresses use the same 48-bit software-model limit. This is a
  simplification of canonical x86-64 virtual addresses, not a full canonicality
  check.
- A mapping or span that crosses the limit is rejected with
  `MemoryStatus::InvalidAddress`.
- Guest mappings are 4 KiB pages. The software EPT model also supports 2 MiB and
  1 GiB pages; 4 KiB is the default.
- The synthetic HPA range starts at `0x0000010000000000`
  (`kFirstSyntheticHostAddress`). Those values are model addresses; they are
  never dereferenced. Data access goes through
  `IMemoryBackingProvider::Read` / `Write` or `TryResolve`.

## First stage: GVA -> GPA

`GuestAddressSpaceManager` owns one `SyntheticPageTable` per address space. The
page table is the existing v0.3-v0.5 software implementation, extended
additively with `Protect`, `Query`, `Enumerate` and `Clear`.

Translation:

1. Validate that the context exists (`InvalidContext` if not).
2. Validate the 48-bit model limit.
3. Look up the 4 KiB page containing the GVA.
4. Check the page permissions for the requested `Access`.
5. Return `GPA = mapped_page_base + offset_in_page`.

The result contains `guest_translated = true`, the GPA, the page size and the
mapping permissions. The first stage does not know about host memory.

Each address space is independent. The same GVA in two contexts can resolve to
different GPAs.

## Second stage: GPA -> HPA

There are two cooperating models for the second stage:

1. **Guest region backing** — `GuestMemoryManager` registers explicit guest RAM
   regions. Each region has a GPA base, a size, permissions and a real backing
   allocation. `GuestMemoryManager::Translate` resolves a GPA to the corresponding
   HPA inside that allocation.
2. **Software EPT** — `EptManager` / `EptPageTable` model a four-level second-stage
   page table. The EPT leaf maps a GPA page to an HPA page. `Translate` checks
   permissions and returns the translated HPA with the page offset.

Both models must agree. `MemoryMappingManager::Translate` performs the complete
pipeline and rejects an inconsistency with `MemoryStatus::InvalidMapping`:

```text
GVA --[SyntheticPageTable]--> GPA --[GuestMemoryManager region]--> HPA
                                  \--[EptPageTable]-------------> HPA
```

This agreement check is deliberately strict. It prevents a test from succeeding
with an EPT mapping that points at a different allocation than the guest region
that owns the GPA. `MapAll` enforces the same rule when it creates the nested
mapping: the EPT target must equal the region's backing address.

## Permission evaluation

`Permissions` has three independent booleans: `read`, `write`, `execute`.
`Permissions::Allows(Access)` is `constexpr` and is used by every layer.

A complete translation checks permissions at all applicable layers:

1. The guest page-table mapping.
2. The guest region permissions (`GuestMemoryManager`).
3. The EPT leaf permissions.

The effective permissions are the intersection of the guest-stage and
nested-stage permissions. A denied access returns `MemoryStatus::AccessDenied`
from the layer that rejected it. `MemoryMappingManager::Execute` only checks the
execute permission; it never transfers control to guest code.

`MemoryStatus::AccessDenied` is distinct from `Unmapped`. A mapping may exist and
still deny a particular access.

## Status values

The core statuses required by the memory layer are:

| Status | Meaning |
| --- | --- |
| `Success` | The operation completed. |
| `InvalidAddress` | The address, size or span is outside the 48-bit model, zero where nonzero is required, or otherwise invalid. |
| `InvalidContext` | The address space, region, snapshot or object does not exist or is not usable. |
| `Unmapped` | The translation path has no mapping for the address. |
| `AccessDenied` | A mapping exists but its permissions deny the requested access. |
| `AlignmentError` | A page size, base address or allocation alignment is invalid. |
| `InvalidMapping` | The requested mapping conflicts with the model, overlaps, or is internally inconsistent. |
| `InternalError` | An invariant or resource-management failure occurred. |

The implementation also uses precise statuses so that callers do not have to
collapse distinct failures:

| Status | Meaning |
| --- | --- |
| `AlreadyMapped` | A duplicate mapping was requested. |
| `OutOfMemory` | A backing allocation or snapshot-size limit was exceeded. |
| `SnapshotNotFound` | The requested snapshot id does not exist. |
| `SnapshotCorrupt` | Snapshot metadata, payload, checksum or live layout is inconsistent. |
| `NotInitialized` | The backing provider is shut down. |
| `Unsupported` | The operation is disabled by configuration (for example large EPT pages). |

`MemoryStatusName` returns a stable diagnostic string. `MemoryResult` and
`TranslationResult` carry the status plus a human-readable message.

## Worked example

```cpp
using namespace hv::memory;

FakeMemoryBackingProvider backing;
GuestMemoryManager memory(backing);
GuestAddressSpaceManager spaces;
EptManager ept;
MemoryMappingManager mappings(spaces, ept, memory);

const AddressSpaceId space = spaces.CreateAddressSpace("demo").id;

GuestRegionRequest request;
request.name = "ram";
request.size = 2 * kSmallPageSize;
request.permissions = Permissions{true, true, false};
const GuestRegionResult region = memory.AddRegion(request);

mappings.MapAll(space, GuestVirtualAddress{0x10000},
                region.region.guest_base, Permissions{true, true, true});

const TranslationResult result =
    mappings.Translate(space, GuestVirtualAddress{0x10123}, Access::Read);
// result.guest_physical == region.guest_base + 0x123
// result.host_physical == region.region.host_base + 0x123
// result.page_size == 4096
```

## Software/hardware boundary

This translation pipeline is a software model. It does **not**:

- program an EPTP or any VMCS field;
- execute `VMXON`, `VMLAUNCH`, `VMRESUME` or `VMREAD`/`VMWRITE`;
- turn on hardware EPT/NPT;
- read or write real machine-physical memory;
- implement hardware page-walk caching, accessed/dirty bits, memory types,
  VPID, EPT invalidation or exit-qualification behavior.

The 48-bit and page-size rules mirror architectural concepts so that a future
hardware backend can reuse the same interface vocabulary. They are not evidence
that hardware translation has been validated. See
[EPT_MODEL.md](EPT_MODEL.md) and [UNSUPPORTED.md](UNSUPPORTED.md).
