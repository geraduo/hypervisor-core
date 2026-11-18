#include "hv/memory/address_space_manager.hpp"
#include "hv/memory/ept_manager.hpp"
#include "hv/memory/guest_memory_manager.hpp"
#include "hv/memory/memory_diagnostics.hpp"
#include "hv/memory/memory_mapping_manager.hpp"
#include "hv/memory/memory_snapshot_manager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hv::memory;

constexpr std::uint64_t kDemoGvaBase = 0x0000'0000'1000'0000ULL;
constexpr std::size_t kDemoPages = 4;

std::vector<std::byte> ToBytes(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

std::string ToString(std::span<const std::byte> bytes) {
    std::string text(bytes.size(), '\0');
    if (!bytes.empty()) {
        std::memcpy(text.data(), bytes.data(), bytes.size());
    }
    return text;
}

int Fail(const std::string& message) {
    std::cerr << "ERROR: " << message << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    const std::string output_path =
        argc > 1 ? std::string(argv[1]) : std::string("hv_memory_demo_diagnostics.json");

    std::cout << "Geraduo Hypervisor Core v0.6.0 - software memory model demo\n";
    std::cout << "This demo uses the software EPT model only. No VMX instructions are\n";
    std::cout << "executed and no hardware EPT structures are programmed.\n\n";

    FakeMemoryBackingProvider backing;
    if (!backing.Initialize().Succeeded()) {
        return Fail("backing provider initialization failed");
    }
    GuestMemoryManager memory(backing);
    GuestAddressSpaceManager spaces;
    EptManager ept;
    MemoryDiagnostics diagnostics;
    MemoryMappingManager mappings(spaces, ept, memory, &diagnostics);
    MemorySnapshotManager snapshots(memory, spaces, ept, &diagnostics);

    std::cout << "[1] Address space\n";
    const AddressSpaceCreationResult space = spaces.CreateAddressSpace("demo-space");
    if (!space.Succeeded()) {
        return Fail("address-space creation failed: " + space.message);
    }
    std::cout << "    created address space #" << space.id.Value() << " \"demo-space\"\n\n";

    std::cout << "[2] Guest-physical memory and synthetic pages\n";
    GuestRegionRequest request;
    request.name = "demo-ram";
    request.size = kDemoPages * static_cast<std::size_t>(kSmallPageSize);
    request.permissions = Permissions{true, true, false};
    request.alignment = static_cast<std::size_t>(kSmallPageSize);
    const GuestRegionResult region = memory.AddRegion(request);
    if (!region.Succeeded()) {
        return Fail("guest-region creation failed: " + region.message);
    }
    const GuestPhysicalAddress gpa = region.region.guest_base;
    const HostPhysicalAddress hpa = region.region.host_base;
    std::cout << "    region \"" << region.region.name << "\" id=" << region.region.id
              << " GPA=" << FormatAddress(gpa.Value())
              << " HPA=" << FormatAddress(hpa.Value())
              << " size=" << FormatBytes(region.region.size) << '\n';

    const std::string original_text = "Geraduo v0.6.0 software memory model";
    const std::vector<std::byte> original_bytes = ToBytes(original_text);
    const MemoryResult seeded = memory.Write(gpa, original_bytes);
    if (!seeded.Succeeded()) {
        return Fail("seeding guest memory failed: " + seeded.message);
    }
    std::vector<GuestPageInfo> pages;
    if (!memory.EnumeratePages("demo-ram", pages).Succeeded() || pages.empty()) {
        return Fail("guest-page enumeration failed");
    }
    std::cout << "    synthetic pages: " << pages.size() << ", first HPA="
              << FormatAddress(pages.front().host_physical.Value()) << ", last HPA="
              << FormatAddress(pages.back().host_physical.Value()) << '\n';
    std::cout << "    seeded page 0 with: \"" << ToString(original_bytes) << "\"\n\n";

    std::cout << "[3] Two-stage translation (GVA -> GPA -> HPA)\n";
    for (std::size_t page = 0; page < kDemoPages; ++page) {
        const std::uint64_t offset = static_cast<std::uint64_t>(page) * kSmallPageSize;
        const MemoryResult mapped = mappings.MapAll(
            space.id, GuestVirtualAddress{kDemoGvaBase + offset},
            GuestPhysicalAddress{gpa.Value() + offset}, Permissions{true, true, false});
        if (!mapped.Succeeded()) {
            return Fail("two-stage mapping failed: " + mapped.message);
        }
    }
    const TranslationResult translation =
        mappings.Translate(space.id, GuestVirtualAddress{kDemoGvaBase + 0x20}, Access::Read);
    if (!translation.Succeeded()) {
        return Fail("two-stage translation failed: " + translation.message);
    }
    std::cout << "    GVA " << FormatAddress(translation.guest_virtual.Value()) << " -> GPA "
              << FormatAddress(translation.guest_physical.Value()) << " -> HPA "
              << FormatAddress(translation.host_physical.Value()) << '\n';
    std::cout << "    page_size=" << translation.page_size
              << " permissions=" << PermissionsToString(translation.permissions)
              << " guest_translated=" << (translation.guest_translated ? "true" : "false")
              << " nested_translated=" << (translation.nested_translated ? "true" : "false")
              << '\n';

    const GuestPhysicalTranslation physical = memory.Translate(gpa, Access::Read);
    if (!physical.Succeeded()) {
        return Fail("GPA -> HPA lookup failed: " + physical.message);
    }
    std::cout << "    direct GPA " << FormatAddress(physical.guest_physical.Value())
              << " -> HPA " << FormatAddress(physical.host_physical.Value())
              << " (region \"" << physical.region_name << "\")\n\n";

    std::cout << "[4] Snapshot and permissions\n";
    const MemorySnapshotResult snapshot = snapshots.CreateSnapshot("demo-snapshot");
    if (!snapshot.Succeeded()) {
        return Fail("snapshot creation failed: " + snapshot.message);
    }
    std::cout << "    created snapshot #" << snapshot.id << " size="
              << FormatBytes(snapshot.info.total_bytes)
              << " checksum=" << FormatAddress(snapshot.info.checksum) << '\n';

    const GuestVirtualAddress read_only_page{kDemoGvaBase + kSmallPageSize};
    const GuestPhysicalAddress read_only_gpa{gpa.Value() + kSmallPageSize};
    if (!mappings.Protect(space.id, read_only_page, Permissions{true, false, false})
             .Succeeded() ||
        !mappings.ProtectNested(read_only_gpa, Permissions{true, false, false}).Succeeded()) {
        return Fail("permission change failed");
    }
    const std::array<std::byte, 1> byte{std::byte{'X'}};
    const MemoryResult denied_write = mappings.Write(space.id, read_only_page, byte);
    std::cout << "    write to read-only page: "
              << MemoryStatusName(denied_write.status) << '\n';
    const MemoryResult denied_execute =
        mappings.Execute(space.id, GuestVirtualAddress{kDemoGvaBase});
    std::cout << "    execute without execute permission: "
              << MemoryStatusName(denied_execute.status) << '\n';

    const std::vector<std::byte> modified(original_bytes.size(), std::byte{'X'});
    if (!memory.WriteDirect(gpa, modified).Succeeded()) {
        return Fail("direct modification of guest memory failed");
    }
    std::vector<std::byte> changed(original_bytes.size());
    if (!memory.ReadDirect(gpa, changed).Succeeded()) {
        return Fail("direct read of modified guest memory failed");
    }
    std::cout << "    modified page 0 directly to: \"" << ToString(changed) << "\"\n";

    const MemoryResult restored = snapshots.RestoreSnapshot(snapshot.id);
    if (!restored.Succeeded()) {
        return Fail("snapshot restore failed: " + restored.message);
    }
    std::vector<std::byte> restored_bytes(original_bytes.size());
    if (!memory.ReadDirect(gpa, restored_bytes).Succeeded()) {
        return Fail("reading restored guest memory failed");
    }
    const bool bytes_match = restored_bytes == original_bytes;
    std::cout << "    restored page 0 to: \"" << ToString(restored_bytes)
              << "\" (matches original: " << (bytes_match ? "yes" : "no") << ")\n";
    if (!bytes_match) {
        return Fail("snapshot restore did not reproduce the original bytes");
    }
    const MemoryResult write_after_restore = mappings.Write(space.id, read_only_page, byte);
    std::cout << "    write to page 1 after restore: "
              << MemoryStatusName(write_after_restore.status)
              << " (snapshot restored its permissions)\n\n";

    diagnostics.Refresh(spaces, ept, memory, snapshots);
    const MemoryStatistics stats = diagnostics.GetStatistics();
    std::cout << "[5] Diagnostics\n";
    std::cout << "    mapped_pages=" << stats.mapped_pages
              << " ranges=" << stats.mapping_ranges
              << " address_spaces=" << stats.address_spaces
              << " guest_regions=" << stats.guest_regions
              << " snapshots=" << stats.snapshots
              << " backing_bytes=" << stats.allocated_backing_bytes << '\n';
    std::cout << "    translations: " << stats.successful_translations << " succeeded, "
              << stats.failed_translations << " failed, " << stats.permission_denials
              << " permission denials\n";

    const std::string json = diagnostics.ToJson();
    std::cout << "\nJSON diagnostics:\n" << json;

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Fail("cannot open diagnostics output file: " + output_path);
    }
    output << json;
    output.close();
    if (!output) {
        return Fail("cannot write diagnostics output file: " + output_path);
    }
    std::cout << "\nDiagnostics exported to " << output_path << '\n';
    return 0;
}
