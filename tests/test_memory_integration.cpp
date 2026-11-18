#include "hv/memory/address_space_manager.hpp"
#include "hv/memory/ept_manager.hpp"
#include "hv/memory/guest_memory_manager.hpp"
#include "hv/memory/memory_diagnostics.hpp"
#include "hv/memory/memory_mapping_manager.hpp"
#include "hv/memory/memory_snapshot_manager.hpp"
#include "hv/memory_manager.hpp"
#include "hv/nested_paging.hpp"
#include "hv/page_table.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using test::Check;
using namespace hv::memory;

namespace {

constexpr std::uint64_t kGvaBase = 0x0000'0000'0003'0000ULL;

struct Stack {
    FakeMemoryBackingProvider backing;
    GuestMemoryManager memory;
    GuestAddressSpaceManager spaces;
    EptManager ept;
    MemoryDiagnostics diagnostics;
    MemoryMappingManager mappings;
    MemorySnapshotManager snapshots;
    AddressSpaceId space{};
    GuestPhysicalAddress gpa{};
    std::size_t pages{0};

    explicit Stack(std::size_t page_count)
        : memory(backing), mappings(spaces, ept, memory, &diagnostics),
          snapshots(memory, spaces, ept, &diagnostics), pages(page_count) {
        (void)backing.Initialize();
        GuestRegionRequest request;
        request.name = "integration-ram";
        request.size = page_count * static_cast<std::size_t>(kSmallPageSize);
        request.permissions = Permissions{true, true, false};
        request.alignment = static_cast<std::size_t>(kSmallPageSize);
        const GuestRegionResult region = memory.AddRegion(request);
        if (!region.Succeeded()) return;
        gpa = region.region.guest_base;
        const AddressSpaceCreationResult created =
            spaces.CreateAddressSpace("integration-space");
        if (!created.Succeeded()) return;
        space = created.id;
    }

    [[nodiscard]] bool Ready() const noexcept { return space.IsValid() && !gpa.IsZero(); }

    [[nodiscard]] bool MapAllPages(Permissions permissions) {
        for (std::size_t page = 0; page < pages; ++page) {
            const std::uint64_t offset = static_cast<std::uint64_t>(page) * kSmallPageSize;
            if (!mappings
                     .MapAll(space, GuestVirtualAddress{kGvaBase + offset},
                             GuestPhysicalAddress{gpa.Value() + offset}, permissions)
                     .Succeeded()) {
                return false;
            }
        }
        return true;
    }
};

void TestMemoryManagerEptIntegration() {
    hv::SyntheticPageTable guest;
    EptManager ept;
    Check(guest.Map(0x7000, 0x8000, {true, true, false}).Succeeded(),
          "integration: legacy first stage maps");
    Check(ept.Map(GuestPhysicalAddress{0x8000}, HostPhysicalAddress{0x1'0000},
                  Permissions{true, true, true})
              .Succeeded(),
          "integration: typed EPT maps the same GPA");
    hv::MemoryManager manager(guest, ept);

    const hv::TranslationResult result =
        manager.Translate({7, true}, 0x7123, hv::Access::Read);
    Check(result.Succeeded(), "integration: MemoryManager composes both stages");
    Check(result.host_physical_address == 0x1'0123,
          "integration: composed translation preserves the offset");
    Check(manager.Translate({7, true}, 0x7123, hv::Access::Write).Succeeded(),
          "integration: composed translation honours write permission");
    Check(manager.Translate({7, true}, 0x9000, hv::Access::Read).status ==
              hv::TranslationStatus::UnmappedGuestVirtual,
          "integration: first-stage miss is reported as an unmapped GVA");

    hv::NestedPageTable legacy;
    Check(legacy.Map(0x8000, 0x2'0000, {true, true, false}).Succeeded(),
          "integration: legacy nested table still works");
    hv::MemoryManager legacy_manager(guest, legacy);
    const hv::TranslationResult legacy_result =
        legacy_manager.Translate({8, true}, 0x7000, hv::Access::Read);
    Check(legacy_result.Succeeded() && legacy_result.host_physical_address == 0x2'0000,
          "integration: legacy MemoryManager remains backward compatible");
}

void TestDiagnosticsAndJson() {
    Stack stack(4);
    Check(stack.Ready(), "diagnostics: stack initializes");
    if (!stack.Ready()) return;
    Check(stack.MapAllPages(Permissions{true, true, false}), "diagnostics: map four pages");

    const std::array<std::byte, 4> payload{
        std::byte{0xCA}, std::byte{0xFE}, std::byte{0xBA}, std::byte{0xBE}};
    Check(stack.mappings
              .Write(stack.space, GuestVirtualAddress{kGvaBase + 0x30}, payload)
              .Succeeded(),
          "diagnostics: write through the full stack");
    std::array<std::byte, 4> readback{};
    Check(stack.mappings
                  .Read(stack.space, GuestVirtualAddress{kGvaBase + 0x30}, readback)
                  .Succeeded() &&
              readback == payload,
          "diagnostics: read through the full stack");

    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase + 0x10},
                                   Access::Read)
              .Succeeded(),
          "diagnostics: record a successful translation");
    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase + 0x90'000},
                                   Access::Read)
              .status == MemoryStatus::Unmapped,
          "diagnostics: record an unmapped translation");
    Check(stack.mappings.Execute(stack.space, GuestVirtualAddress{kGvaBase}).status ==
              MemoryStatus::AccessDenied,
          "diagnostics: record a permission denial");

    const MemorySnapshotResult snapshot = stack.snapshots.CreateSnapshot("diagnostic-snapshot");
    Check(snapshot.Succeeded(), "diagnostics: create a snapshot for the report");
    stack.diagnostics.Refresh(stack.spaces, stack.ept, stack.memory, stack.snapshots);

    const MemoryStatistics stats = stack.diagnostics.GetStatistics();
    Check(stats.mapped_pages == 8, "diagnostics: mapped-page gauge counts both stages");
    Check(stats.mapping_ranges == 2, "diagnostics: guest and EPT ranges are coalesced");
    Check(stats.address_spaces == 1, "diagnostics: address-space gauge is correct");
    Check(stats.guest_regions == 1, "diagnostics: guest-region gauge is correct");
    Check(stats.snapshots == 1, "diagnostics: snapshot gauge is correct");
    Check(stats.snapshots_created == 1, "diagnostics: snapshot creation is counted");
    Check(stats.allocated_backing_bytes == 4 * kSmallPageSize,
          "diagnostics: backing-byte gauge is correct");
    Check(stats.successful_translations >= 2,
          "diagnostics: successful translations are counted");
    Check(stats.failed_translations >= 2, "diagnostics: failed translations are counted");
    Check(stats.permission_denials >= 1, "diagnostics: permission denials are counted");

    Check(stack.diagnostics.Ranges().size() == 2, "diagnostics: range list has two entries");
    Check(stack.diagnostics.Snapshots().size() == 1,
          "diagnostics: snapshot list has one entry");

    const std::string json = stack.diagnostics.ToJson();
    Check(!json.empty() && json.front() == '{' && json.back() == '\n',
          "diagnostics: JSON is a complete object");
    Check(json.find("\"version\": \"0.6.0\"") != std::string::npos,
          "diagnostics: JSON reports the version");
    Check(json.find("\"model\": \"software\"") != std::string::npos,
          "diagnostics: JSON states the software model");
    Check(json.find("\"hardware_ept\": false") != std::string::npos,
          "diagnostics: JSON explicitly denies hardware EPT");
    Check(json.find("\"mapped_pages\": 8") != std::string::npos,
          "diagnostics: JSON contains mapped-page statistics");
    Check(json.find("\"address_spaces\": 1") != std::string::npos,
          "diagnostics: JSON contains address-space statistics");
    Check(json.find("\"mapping_ranges\"") != std::string::npos,
          "diagnostics: JSON contains mapping ranges");
    Check(json.find("\"domain\": \"ept\"") != std::string::npos,
          "diagnostics: JSON contains an EPT mapping range");
    Check(json.find("\"snapshots\"") != std::string::npos &&
              json.find("diagnostic-snapshot") != std::string::npos,
          "diagnostics: JSON contains snapshot metadata");
    Check(json.find("\"permissions\"") != std::string::npos,
          "diagnostics: JSON contains permissions for ranges");
}

void TestTwoStagePermissionIntersection() {
    Stack stack(1);
    Check(stack.Ready(), "permissions: stack initializes");
    if (!stack.Ready()) return;
    Check(stack.mappings
              .MapAll(stack.space, GuestVirtualAddress{kGvaBase}, stack.gpa,
                      Permissions{true, true, true})
              .Succeeded(),
          "permissions: map with guest execute enabled");
    Check(stack.mappings
              .ProtectNested(stack.gpa, Permissions{true, false, false})
              .Succeeded(),
          "permissions: restrict the nested stage");

    MappingInfo info{};
    Check(stack.mappings.Query(stack.space, GuestVirtualAddress{kGvaBase}, info).Succeeded(),
          "permissions: query combined mapping");
    Check(info.guest_mapped && info.nested_mapped, "permissions: both stages are present");
    Check(!info.permissions.write && !info.permissions.execute,
          "permissions: combined permissions intersect both stages");
    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase}, Access::Write)
              .status == MemoryStatus::AccessDenied,
          "permissions: combined translation denies write");
    Check(stack.mappings.Translate(stack.space, GuestVirtualAddress{kGvaBase}, Access::Read)
              .Succeeded(),
          "permissions: combined translation allows read");
    Check(stack.mappings.ValidateAll().Succeeded(),
          "permissions: validation accepts a restrictive but consistent mapping");
}

void TestConcurrentGuestAccess() {
    constexpr std::size_t kPageCount = 8;
    Stack stack(kPageCount);
    Check(stack.Ready(), "concurrent: stack initializes");
    if (!stack.Ready()) return;
    Check(stack.MapAllPages(Permissions{true, true, false}), "concurrent: map eight pages");

    constexpr int kThreads = 4;
    std::atomic<bool> all_ok{true};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&stack, &all_ok, thread]() {
            const std::uint64_t page = static_cast<std::uint64_t>(thread) * 2;
            const GuestVirtualAddress address{kGvaBase + page * kSmallPageSize};
            const std::array<std::byte, 2> value{
                static_cast<std::byte>(thread), std::byte{0x5A}};
            if (!stack.mappings.Write(stack.space, address, value).Succeeded()) {
                all_ok = false;
                return;
            }
            std::array<std::byte, 2> readback{};
            if (!stack.mappings.Read(stack.space, address, readback).Succeeded() ||
                readback != value) {
                all_ok = false;
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    Check(all_ok.load(), "concurrent: parallel two-stage writes and reads succeed");
    Check(stack.spaces.TotalMappingCount() == kPageCount,
          "concurrent: guest mapping count stays exact");
    Check(stack.ept.MappingCount() == kPageCount,
          "concurrent: nested mapping count stays exact");
    Check(stack.mappings.ValidateAll().Succeeded(),
          "concurrent: full-stack validation after parallel access");
}

void TestSnapshotRoundTripAcrossStack() {
    Stack stack(3);
    Check(stack.Ready(), "round-trip: stack initializes");
    if (!stack.Ready()) return;
    Check(stack.MapAllPages(Permissions{true, true, false}), "round-trip: map three pages");

    std::array<std::byte, 3> original{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    Check(stack.mappings
              .Write(stack.space, GuestVirtualAddress{kGvaBase + 0x700}, original)
              .Succeeded(),
          "round-trip: seed original bytes");
    const MemorySnapshotResult created = stack.snapshots.CreateSnapshot("round-trip");
    Check(created.Succeeded(), "round-trip: create snapshot");

    std::array<std::byte, 3> changed{std::byte{0x99}, std::byte{0x88}, std::byte{0x77}};
    Check(stack.mappings
              .Write(stack.space, GuestVirtualAddress{kGvaBase + 0x700}, changed)
              .Succeeded(),
          "round-trip: modify live bytes");
    Check(stack.snapshots.RestoreSnapshot(created.id).Succeeded(),
          "round-trip: restore snapshot");

    std::array<std::byte, 3> readback{};
    Check(stack.mappings
                  .Read(stack.space, GuestVirtualAddress{kGvaBase + 0x700}, readback)
                  .Succeeded() &&
              readback == original,
          "round-trip: original bytes are visible after restore");

    const MemoryStatistics stats = stack.diagnostics.GetStatistics();
    Check(stats.snapshots_restored == 1, "round-trip: restore counter is updated");
}

} // namespace

int main() {
    TestMemoryManagerEptIntegration();
    TestDiagnosticsAndJson();
    TestTwoStagePermissionIntersection();
    TestConcurrentGuestAccess();
    TestSnapshotRoundTripAcrossStack();

    if (test::failures != 0) {
        std::cerr << test::failures << " memory integration test(s) failed\n";
        return 1;
    }
    std::cout << "All memory integration tests passed\n";
    return 0;
}
