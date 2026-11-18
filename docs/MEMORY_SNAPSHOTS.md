# Memory Snapshots

## Scope

`hv::memory::MemorySnapshotManager` captures the state of the software memory
model inside the current process. A snapshot is:

- an in-memory artifact;
- owned by the process that created it;
- released when the manager is destroyed or `Clear()` is called;
- unrelated to host filesystem images, VM snapshots, hibernation or physical
  memory capture.

The manager does not serialize to disk and does not read process or kernel
memory outside its registered guest regions.

## Captured state

A snapshot contains:

| Data | Description |
| --- | --- |
| Region metadata | Name, GPA base, size, HPA base, permissions |
| Region bytes | A copy of every registered region's backing bytes |
| Guest mappings | Address space id, GVA page, GPA page, permissions |
| EPT mappings | GPA page, HPA page, page size, permissions |
| Metadata | Snapshot id, name, sizes, counts, checksum, creation time |

The snapshot assumes one `GuestMemoryManager`, one `GuestAddressSpaceManager`
and one `EptManager`. It captures all address spaces and all EPT leaves.

## Creation

```cpp
MemorySnapshotManager snapshots(memory, spaces, ept, &diagnostics);
const MemorySnapshotResult result = snapshots.CreateSnapshot("demo-snapshot");
if (!result.Succeeded()) {
    // result.status and result.message explain the failure
}
```

Creation:

1. Validates the name (1-128 characters).
2. Enumerates live guest regions in a stable order.
3. Checks the configured total snapshot byte limit before copying.
4. Copies every region's bytes with `ReadDirect`, which bypasses region
   permissions but still requires registered, backed memory.
5. Captures guest and EPT mappings.
6. Assigns a monotonically increasing snapshot id.
7. Computes the checksum and stores the snapshot.

`SetMaxSnapshotBytes` controls the limit. The default is 16 MiB. Exceeding it
returns `MemoryStatus::OutOfMemory` and does not store a partial snapshot.

### Metadata

`MemorySnapshotInfo` exposes:

- `id`, `name`;
- `total_bytes` (sum of region sizes);
- `checksum`;
- `created_unix_ms`;
- `region_count`, `guest_mapping_count`, `ept_mapping_count`;
- `valid`.

`GetSnapshotInfo` recomputes validity from the stored checksum.
`GetSnapshotSummaries` returns all snapshot metadata. `SnapshotCount` returns
the number of retained snapshots.

## Integrity

The checksum is FNV-1a 64 over:

- snapshot id, name, total size and creation time;
- region count, guest-mapping count and EPT-mapping count;
- each region's name, addresses, size, permissions and bytes;
- each guest mapping's address space, GVA, GPA and permissions;
- each EPT mapping's GPA, HPA, page size and permissions.

`VerifySnapshot` checks:

- metadata counts match the payload vector sizes;
- region names are valid;
- region sizes are nonzero page multiples;
- region payload sizes match their metadata;
- region and mapping addresses are aligned and within the 48-bit model;
- the total size metadata equals the sum of region sizes;
- guest regions do not overlap;
- guest mappings reference live address spaces and backed GPA pages;
- EPT mappings have supported page sizes, valid alignment/range, no overlap and
  live backing;
- the checksum matches.

Any failure returns `MemoryStatus::SnapshotCorrupt`.

`CorruptSnapshotForTesting` is a deliberate test seam. It flips one payload byte
by offset, or flips the checksum field if the offset is outside the payload. It
is not a production mutation API.

## Restore

```cpp
const MemoryResult restored = snapshots.RestoreSnapshot(result.id);
```

Restore follows a verify-before-mutate order:

1. Find the snapshot (`SnapshotNotFound` if absent).
2. Run `VerifySnapshot` (`SnapshotCorrupt` if invalid).
3. Require the live region count to equal the snapshot region count.
4. Require every saved region name to exist with the same GPA base, HPA base and
   size.
5. Clear guest mappings and EPT mappings.
6. Recreate EPT mappings.
7. Recreate guest mappings.
8. Write saved bytes with `WriteDirect`.
9. Restore region permissions.

Because verification and layout checks happen before mutation, a corrupt
snapshot or a changed region layout leaves live memory untouched. A failure
during the later mutation phase (for example an unexpected allocation or mapping
error) is reported to the caller, but the model does not implement a full
transactional rollback of already-restored mappings. Re-create the stack or
restore a known-good snapshot if that occurs.

Restore deliberately does **not** recreate or resize regions. The region layout
is part of the snapshot contract; changing a region's name, base, HPA or size
invalidates the snapshot.

## Lifecycle

| Operation | Behavior |
| --- | --- |
| `CreateSnapshot` | Captures state, returns id and metadata |
| `RestoreSnapshot` | Verifies and applies a snapshot |
| `DeleteSnapshot` | Releases the snapshot's copied bytes |
| `VerifySnapshot` | Recomputes and validates integrity |
| `GetSnapshotInfo` | Returns metadata with recomputed validity |
| `GetSnapshotSummaries` | Returns all metadata |
| `SnapshotCount` | Number of retained snapshots |
| `Clear` | Deletes every snapshot |
| `SetMaxSnapshotBytes` / `GetMaxSnapshotBytes` | Size policy |
| `CorruptSnapshotForTesting` | Test-only corruption hook |

Snapshot ids are unique per manager and are never reused after deletion within
the lifetime of that manager (the counter is monotonic).

## Diagnostics

If a `MemoryDiagnostics*` is supplied:

- successful creation increments `snapshots_created`;
- successful restore increments `snapshots_restored`;
- failed creation, restore or explicit failure records increments
  `snapshot_failures`;
- `Refresh` copies snapshot summaries into the diagnostics report.

The JSON report includes snapshot id, name, size, checksum, creation time and
validity. It also includes `"hardware_ept": false` because snapshots describe the
software model only.

## Example

```cpp
MemorySnapshotManager snapshots(memory, spaces, ept, &diagnostics);

// Seed and map memory first.
const MemorySnapshotResult snap = snapshots.CreateSnapshot("before-test");
if (!snap.Succeeded()) return;

// Modify live state.
// ...

if (!snapshots.RestoreSnapshot(snap.id).Succeeded()) {
    // Inspect the returned status/message.
}

// Delete when no longer needed.
snapshots.DeleteSnapshot(snap.id);
```

## Limitations

- In-memory only; no file format, disk image or serialization.
- Not a VM snapshot and not a host physical-memory snapshot.
- No incremental or copy-on-write snapshots.
- No compression or encryption.
- No automatic region creation or layout migration.
- No hardware EPT/VMCS state.
- No rollback guarantee if a failure occurs after the restore mutation phase
  begins.
- Snapshot bytes are copied in full, so the configured size limit is the main
  memory cost.

See [MEMORY_MANAGER.md](MEMORY_MANAGER.md) and
[ADDRESS_TRANSLATION.md](ADDRESS_TRANSLATION.md).
