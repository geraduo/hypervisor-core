#include "hv/memory/address_space_manager.hpp"
#include "hv/memory/ept_manager.hpp"
#include "hv/memory/guest_memory_manager.hpp"
#include "hv/memory/memory_diagnostics.hpp"
#include "hv/memory/memory_mapping_manager.hpp"
#include "hv/memory/memory_snapshot_manager.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using test::Check;
using namespace hv::memory;

namespace {

constexpr std::uint64_t kGvaBase = 0x0000'0000'0002'0000ULL;

struct Fixture {
    FakeMemoryBackingProvider backing;
    GuestMemoryManager memory;
    GuestAddressSpaceManager spaces;
    EptManager ept;
    MemoryDiagnostics diagnostics;
    MemoryMappingManager mappings;
    MemorySnapshotManager snapshots;
    AddressSpaceId space{};
    GuestPhysicalAddress gpa{};
    std::size_t region_size{0};

    explicit Fixture(std::size_t pages)
        : memory(backing), mappings(spaces, ept, memory, &diagnostics),
          snapshots(memory, spaces, ept, &diagnostics) {
        (void)backing.Initialize();
        GuestRegionRequest request;
        request.name = "ram";
        request.size = pages * static_cast<std::size_t>(kSmallPageSize);
        request.permissions = Permissions{true, true, false};
        request.alignment = static_cast<std::size_t>(kSmallPageSize);
        const GuestRegionResult region = memory.AddRegion(request);
        if (!region.Succeeded()) return;
        gpa = region.region.guest_base;
        region_size = region.region.size;
        const AddressSpaceCreationResult created = spaces.CreateAddressSpace("snapshots");
        if (!created.Succeeded()) return;
        space = created.id;
    }

    [[nodiscard]] bool Ready() const noexcept { return space.IsValid() && !gpa.IsZero(); }

    [[nodiscard]] bool MapAllPages(std::size_t pages) {
        for (std::size_t page = 0; page < pages; ++page) {
            const std::uint64_t offset = static_cast<std::uint64_t>(page) * kSmallPageSize;
            if (!mappings
                     .MapAll(space, GuestVirtualAddress{kGvaBase + offset},
                             GuestPhysicalAddress{gpa.Value() + offset},
                             Permissions{true, true, false})
                     .Succeeded()) {
                return false;
            }
        }
        return true;
    }
};

void TestSnapshotCreationAndMetadata() {
    Fixture fixture(2);
    Check(fixture.Ready(), "snapshot: fixture initializes");
    if (!fixture.Ready()) return;
    Check(fixture.MapAllPages(2), "snapshot: map two pages");

    const std::array<std::byte, 8> payload{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
        std::byte{0x50}, std::byte{0x60}, std::byte{0x70}, std::byte{0x80}};
    Check(fixture.mappings
              .Write(fixture.space, GuestVirtualAddress{kGvaBase + 0x80}, payload)
              .Succeeded(),
          "snapshot: seed guest bytes");

    const MemorySnapshotResult created = fixture.snapshots.CreateSnapshot("snapshot-one");
    Check(created.Succeeded(), "snapshot: create a snapshot");
    Check(created.info.total_bytes == 2 * static_cast<std::size_t>(kSmallPageSize),
          "snapshot: metadata records the exact byte size");
    Check(created.info.region_count == 1, "snapshot: metadata records one region");
    Check(created.info.guest_mapping_count == 2,
          "snapshot: metadata records two guest mappings");
    Check(created.info.ept_mapping_count == 2,
          "snapshot: metadata records two EPT mappings");
    Check(created.info.checksum != 0, "snapshot: checksum is populated");
    Check(created.info.valid, "snapshot: new snapshot is marked valid");
    Check(fixture.snapshots.SnapshotCount() == 1, "snapshot: count reflects the new snapshot");

    MemorySnapshotInfo info{};
    Check(fixture.snapshots.GetSnapshotInfo(created.id, info).Succeeded(),
          "snapshot: read snapshot metadata");
    Check(info.id == created.id && info.name == "snapshot-one" && info.valid,
          "snapshot: metadata round-trips id, name and validity");
    Check(fixture.snapshots.VerifySnapshot(created.id).Succeeded(),
          "snapshot: integrity verification succeeds");
    Check(fixture.snapshots.GetSnapshotSummaries().size() == 1,
          "snapshot: summary list contains the snapshot");
}

void TestSnapshotRestoreBytesAndPermissions() {
    Fixture fixture(2);
    Check(fixture.Ready(), "restore: fixture initializes");
    if (!fixture.Ready()) return;
    Check(fixture.MapAllPages(2), "restore: map two pages");

    const std::array<std::byte, 4> original{
        std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    Check(fixture.mappings
              .Write(fixture.space, GuestVirtualAddress{kGvaBase + 0x40}, original)
              .Succeeded(),
          "restore: write original bytes");
    const MemorySnapshotResult created = fixture.snapshots.CreateSnapshot("restore-me");
    Check(created.Succeeded(), "restore: create baseline snapshot");

    const std::array<std::byte, 4> modified{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    Check(fixture.memory
              .WriteDirect(GuestPhysicalAddress{fixture.gpa.Value() + 0x40}, modified)
              .Succeeded(),
          "restore: modify live memory after the snapshot");
    Check(fixture.mappings
              .Protect(fixture.space, GuestVirtualAddress{kGvaBase},
                       Permissions{true, false, false})
              .Succeeded(),
          "restore: make the guest page read-only");
    Check(fixture.mappings.ProtectNested(fixture.gpa, Permissions{true, false, false})
              .Succeeded(),
          "restore: make the nested page read-only");
    Check(fixture.memory.SetRegionPermissions("ram", Permissions{true, false, false})
              .Succeeded(),
          "restore: make the region read-only");
    Check(fixture.mappings
              .Unmap(fixture.space, GuestVirtualAddress{kGvaBase + kSmallPageSize})
              .Succeeded(),
          "restore: remove one guest mapping");
    Check(fixture.mappings.UnmapNested(GuestPhysicalAddress{fixture.gpa.Value() + kSmallPageSize})
              .Succeeded(),
          "restore: remove one nested mapping");

    Check(fixture.snapshots.RestoreSnapshot(created.id).Succeeded(),
          "restore: restore the snapshot");
    std::array<std::byte, 4> readback{};
    Check(fixture.memory
                  .ReadDirect(GuestPhysicalAddress{fixture.gpa.Value() + 0x40}, readback)
                  .Succeeded() &&
              readback == original,
          "restore: snapshot bytes overwrite later modifications");

    GuestMappingInfo guest{};
    Check(fixture.spaces.Query(fixture.space, GuestVirtualAddress{kGvaBase}, guest).Succeeded(),
          "restore: guest mapping exists after restore");
    Check(guest.permissions.write, "restore: guest permissions are restored");

    EptMappingInfo nested{};
    Check(fixture.ept.Query(fixture.gpa, nested).Succeeded(),
          "restore: nested mapping exists after restore");
    Check(nested.permissions.write, "restore: nested permissions are restored");

    GuestRegionInfo region{};
    Check(fixture.memory.GetRegion("ram", region).Succeeded() && region.permissions.write,
          "restore: region permissions are restored");
    Check(fixture.spaces.TotalMappingCount() == 2,
          "restore: both guest mappings are recreated");
    Check(fixture.ept.MappingCount() == 2, "restore: both nested mappings are recreated");

    const MemoryStatistics stats = fixture.diagnostics.GetStatistics();
    Check(stats.snapshots_created == 1, "restore: diagnostics count snapshot creation");
    Check(stats.snapshots_restored == 1, "restore: diagnostics count snapshot restoration");
}

void TestCorruptedSnapshotIsRejected() {
    Fixture fixture(1);
    Check(fixture.Ready(), "corrupt: fixture initializes");
    if (!fixture.Ready()) return;
    Check(fixture.MapAllPages(1), "corrupt: map one page");
    const MemorySnapshotResult created = fixture.snapshots.CreateSnapshot("corrupt-me");
    Check(created.Succeeded(), "corrupt: create snapshot");

    const std::array<std::byte, 2> changed{std::byte{0xAA}, std::byte{0xBB}};
    Check(fixture.memory.WriteDirect(fixture.gpa, changed).Succeeded(),
          "corrupt: change live memory before the failed restore");

    Check(fixture.snapshots.CorruptSnapshotForTesting(created.id, 1).Succeeded(),
          "corrupt: flip one payload byte through the test hook");
    MemorySnapshotInfo info{};
    Check(fixture.snapshots.GetSnapshotInfo(created.id, info).Succeeded() && !info.valid,
          "corrupt: metadata reports the snapshot as invalid");
    Check(fixture.snapshots.VerifySnapshot(created.id).status == MemoryStatus::SnapshotCorrupt,
          "corrupt: verification rejects the modified payload");
    Check(fixture.snapshots.RestoreSnapshot(created.id).status == MemoryStatus::SnapshotCorrupt,
          "corrupt: restore rejects the modified payload");

    std::array<std::byte, 2> readback{};
    Check(fixture.memory.ReadDirect(fixture.gpa, readback).Succeeded() &&
              readback == changed,
          "corrupt: failed restore leaves live memory untouched");

    Check(fixture.snapshots.CorruptSnapshotForTesting(created.id, 1'000'000).Succeeded(),
          "corrupt: flip checksum metadata through the test hook");
    Check(fixture.snapshots.VerifySnapshot(created.id).status == MemoryStatus::SnapshotCorrupt,
          "corrupt: checksum corruption is detected");
    Check(fixture.snapshots.CorruptSnapshotForTesting(9999, 0).status ==
              MemoryStatus::SnapshotNotFound,
          "corrupt: corrupting a missing snapshot is rejected");
}

void TestSnapshotSizeLimit() {
    Fixture fixture(2);
    Check(fixture.Ready(), "limit: fixture initializes");
    if (!fixture.Ready()) return;
    Check(fixture.MapAllPages(2), "limit: map two pages");

    fixture.snapshots.SetMaxSnapshotBytes(static_cast<std::size_t>(kSmallPageSize));
    Check(fixture.snapshots.CreateSnapshot("too-large").status == MemoryStatus::OutOfMemory,
          "limit: snapshot larger than the configured limit is rejected");
    Check(fixture.snapshots.SnapshotCount() == 0, "limit: failed snapshot is not retained");
    Check(fixture.diagnostics.GetStatistics().snapshot_failures == 1,
          "limit: diagnostics count the snapshot failure");

    fixture.snapshots.SetMaxSnapshotBytes(static_cast<std::size_t>(kSmallPageSize) * 16);
    Check(fixture.snapshots.CreateSnapshot("fits").Succeeded(),
          "limit: a snapshot within the raised limit succeeds");
}

void TestSnapshotLifecycle() {
    Fixture fixture(1);
    Check(fixture.Ready(), "lifecycle: fixture initializes");
    if (!fixture.Ready()) return;

    const MemorySnapshotResult first = fixture.snapshots.CreateSnapshot("first");
    const MemorySnapshotResult second = fixture.snapshots.CreateSnapshot("second");
    Check(first.Succeeded() && second.Succeeded(), "lifecycle: create two snapshots");
    Check(fixture.snapshots.SnapshotCount() == 2, "lifecycle: count is two");

    Check(fixture.snapshots.DeleteSnapshot(first.id).Succeeded(),
          "lifecycle: delete the first snapshot");
    Check(fixture.snapshots.SnapshotCount() == 1, "lifecycle: count drops to one");
    Check(fixture.snapshots.DeleteSnapshot(first.id).status == MemoryStatus::SnapshotNotFound,
          "lifecycle: deleting a missing snapshot is rejected");
    Check(fixture.snapshots.RestoreSnapshot(first.id).status ==
              MemoryStatus::SnapshotNotFound,
          "lifecycle: restoring a missing snapshot is rejected");
    Check(fixture.snapshots.VerifySnapshot(first.id).status ==
              MemoryStatus::SnapshotNotFound,
          "lifecycle: verifying a missing snapshot is rejected");
    MemorySnapshotInfo info{};
    Check(fixture.snapshots.GetSnapshotInfo(first.id, info).status ==
              MemoryStatus::SnapshotNotFound,
          "lifecycle: metadata of a missing snapshot is rejected");

    fixture.snapshots.Clear();
    Check(fixture.snapshots.SnapshotCount() == 0, "lifecycle: clear removes every snapshot");
}

void TestRestoreAfterRegionLayoutChange() {
    Fixture fixture(1);
    Check(fixture.Ready(), "layout: fixture initializes");
    if (!fixture.Ready()) return;
    Check(fixture.MapAllPages(1), "layout: map one page");
    const MemorySnapshotResult created = fixture.snapshots.CreateSnapshot("layout");
    Check(created.Succeeded(), "layout: create snapshot");

    Check(fixture.memory.RemoveRegion("ram").Succeeded(),
          "layout: remove the snapshotted region");
    GuestRegionRequest replacement;
    replacement.name = "replacement";
    replacement.size = static_cast<std::size_t>(kSmallPageSize);
    replacement.permissions = Permissions{true, true, false};
    replacement.alignment = static_cast<std::size_t>(kSmallPageSize);
    Check(fixture.memory.AddRegion(replacement).Succeeded(),
          "layout: add a replacement region");

    Check(fixture.snapshots.RestoreSnapshot(created.id).status ==
              MemoryStatus::SnapshotCorrupt,
          "layout: restore refuses a changed region layout");
}

} // namespace

int main() {
    TestSnapshotCreationAndMetadata();
    TestSnapshotRestoreBytesAndPermissions();
    TestCorruptedSnapshotIsRejected();
    TestSnapshotSizeLimit();
    TestSnapshotLifecycle();
    TestRestoreAfterRegionLayoutChange();

    if (test::failures != 0) {
        std::cerr << test::failures << " snapshot test(s) failed\n";
        return 1;
    }
    std::cout << "All snapshot tests passed\n";
    return 0;
}
