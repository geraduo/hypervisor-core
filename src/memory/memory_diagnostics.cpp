#include "hv/memory/memory_diagnostics.hpp"

#include "hv/detail/report_json.hpp"
#include "hv/memory/address_space_manager.hpp"
#include "hv/memory/ept_manager.hpp"
#include "hv/memory/guest_memory_manager.hpp"
#include "hv/memory/memory_snapshot_manager.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace hv::memory {
namespace {

constexpr const char* kPermissionsJsonKey = "\"permissions\"";

std::string PermissionsJson(Permissions permissions) {
    std::string result;
    result += "{\"read\":";
    result += permissions.read ? "true" : "false";
    result += ",\"write\":";
    result += permissions.write ? "true" : "false";
    result += ",\"execute\":";
    result += permissions.execute ? "true" : "false";
    result += '}';
    return result;
}

void CoalesceRange(std::vector<MappingRangeInfo>& ranges, const MappingRangeInfo& next) {
    if (!ranges.empty()) {
        MappingRangeInfo& last = ranges.back();
        const bool same_domain = last.domain == next.domain;
        const bool same_space = last.address_space == next.address_space;
        const bool same_permissions =
            last.permissions.read == next.permissions.read &&
            last.permissions.write == next.permissions.write &&
            last.permissions.execute == next.permissions.execute;
        if (same_domain && same_space && same_permissions &&
            last.page_size == next.page_size &&
            last.base + last.size == next.base) {
            last.size += next.size;
            return;
        }
    }
    ranges.push_back(next);
}

} // namespace

const char* MappingDomainName(MappingDomain domain) noexcept {
    switch (domain) {
    case MappingDomain::GuestPageTable: return "guest";
    case MappingDomain::Ept: return "ept";
    }
    return "unknown";
}

void MemoryDiagnostics::RecordTranslation(MemoryStatus status, bool permission_denied) {
    std::lock_guard lock(mutex_);
    if (Succeeded(status)) {
        ++statistics_.successful_translations;
    } else {
        ++statistics_.failed_translations;
        if (permission_denied) ++statistics_.permission_denials;
    }
}

void MemoryDiagnostics::RecordMapping(MemoryStatus status) {
    (void)status;
    std::lock_guard lock(mutex_);
    ++statistics_.mapping_operations;
}

void MemoryDiagnostics::RecordUnmapping(MemoryStatus status) {
    (void)status;
    std::lock_guard lock(mutex_);
    ++statistics_.unmapping_operations;
}

void MemoryDiagnostics::RecordProtection(MemoryStatus status) {
    (void)status;
    std::lock_guard lock(mutex_);
    ++statistics_.protection_changes;
}

void MemoryDiagnostics::RecordSnapshotCreated(MemoryStatus status) {
    std::lock_guard lock(mutex_);
    if (Succeeded(status)) {
        ++statistics_.snapshots_created;
    } else {
        ++statistics_.snapshot_failures;
    }
}

void MemoryDiagnostics::RecordSnapshotRestored(MemoryStatus status) {
    std::lock_guard lock(mutex_);
    if (Succeeded(status)) {
        ++statistics_.snapshots_restored;
    } else {
        ++statistics_.snapshot_failures;
    }
}

void MemoryDiagnostics::RecordSnapshotFailure() {
    std::lock_guard lock(mutex_);
    ++statistics_.snapshot_failures;
}

void MemoryDiagnostics::SetMappedPages(std::uint64_t count) {
    std::lock_guard lock(mutex_);
    statistics_.mapped_pages = count;
}

void MemoryDiagnostics::SetAddressSpaces(std::uint64_t count) {
    std::lock_guard lock(mutex_);
    statistics_.address_spaces = count;
}

void MemoryDiagnostics::SetGuestRegions(std::uint64_t count) {
    std::lock_guard lock(mutex_);
    statistics_.guest_regions = count;
}

void MemoryDiagnostics::SetAllocatedBackingBytes(std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    statistics_.allocated_backing_bytes = bytes;
}

void MemoryDiagnostics::SetMappingRanges(std::vector<MappingRangeInfo> ranges) {
    std::lock_guard lock(mutex_);
    statistics_.mapping_ranges = static_cast<std::uint64_t>(ranges.size());
    ranges_ = std::move(ranges);
}

void MemoryDiagnostics::SetSnapshotSummaries(std::vector<SnapshotSummary> summaries) {
    std::lock_guard lock(mutex_);
    statistics_.snapshots = static_cast<std::uint64_t>(summaries.size());
    snapshots_ = std::move(summaries);
}

void MemoryDiagnostics::Refresh(const GuestAddressSpaceManager& spaces, const EptManager& ept,
                                const GuestMemoryManager& memory,
                                const MemorySnapshotManager& snapshots) {
    std::vector<MappingRangeInfo> ranges;

    const std::vector<AddressSpaceInfo> address_spaces = spaces.EnumerateAddressSpaces();
    for (const AddressSpaceInfo& space : address_spaces) {
        const std::vector<GuestMappingInfo> mappings = spaces.EnumerateMappings(space.id);
        for (const GuestMappingInfo& mapping : mappings) {
            CoalesceRange(ranges, MappingRangeInfo{MappingDomain::GuestPageTable, space.id,
                                                   mapping.virtual_page.Value(), mapping.page_size,
                                                   mapping.page_size, mapping.permissions});
        }
    }

    const std::vector<EptRangeInfo> ept_ranges = ept.GetMappingRanges();
    for (const EptRangeInfo& range : ept_ranges) {
        CoalesceRange(ranges, MappingRangeInfo{MappingDomain::Ept, AddressSpaceId{},
                                               range.guest_base.Value(), range.size,
                                               range.page_size, range.permissions});
    }

    std::vector<SnapshotSummary> summaries;
    const std::vector<MemorySnapshotInfo> snapshot_infos = snapshots.GetSnapshotSummaries();
    summaries.reserve(snapshot_infos.size());
    for (const MemorySnapshotInfo& info : snapshot_infos) {
        summaries.push_back(SnapshotSummary{info.id, info.name,
                                            static_cast<std::uint64_t>(info.total_bytes),
                                            info.checksum, info.created_unix_ms,
                                            info.valid});
    }

    std::lock_guard lock(mutex_);
    statistics_.mapped_pages = static_cast<std::uint64_t>(ept.MappingCount() +
                                                          spaces.TotalMappingCount());
    statistics_.mapping_ranges = static_cast<std::uint64_t>(ranges.size());
    statistics_.address_spaces = static_cast<std::uint64_t>(spaces.AddressSpaceCount());
    statistics_.guest_regions = static_cast<std::uint64_t>(memory.RegionCount());
    statistics_.allocated_backing_bytes = static_cast<std::uint64_t>(memory.AllocatedBytes());
    statistics_.snapshots = static_cast<std::uint64_t>(summaries.size());
    ranges_ = std::move(ranges);
    snapshots_ = std::move(summaries);
}

MemoryStatistics MemoryDiagnostics::GetStatistics() const {
    std::lock_guard lock(mutex_);
    return statistics_;
}

std::vector<MappingRangeInfo> MemoryDiagnostics::Ranges() const {
    std::lock_guard lock(mutex_);
    return ranges_;
}

std::vector<SnapshotSummary> MemoryDiagnostics::Snapshots() const {
    std::lock_guard lock(mutex_);
    return snapshots_;
}

std::string MemoryDiagnostics::ToJson() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": \"0.6.0\",\n";
    out << "  \"model\": \"software\",\n";
    out << "  \"hardware_ept\": false,\n";
    out << "  \"statistics\": {\n";
    out << "    \"mapped_pages\": " << statistics_.mapped_pages << ",\n";
    out << "    \"mapping_ranges\": " << statistics_.mapping_ranges << ",\n";
    out << "    \"address_spaces\": " << statistics_.address_spaces << ",\n";
    out << "    \"guest_regions\": " << statistics_.guest_regions << ",\n";
    out << "    \"successful_translations\": " << statistics_.successful_translations << ",\n";
    out << "    \"failed_translations\": " << statistics_.failed_translations << ",\n";
    out << "    \"permission_denials\": " << statistics_.permission_denials << ",\n";
    out << "    \"mapping_operations\": " << statistics_.mapping_operations << ",\n";
    out << "    \"unmapping_operations\": " << statistics_.unmapping_operations << ",\n";
    out << "    \"protection_changes\": " << statistics_.protection_changes << ",\n";
    out << "    \"snapshots\": " << statistics_.snapshots << ",\n";
    out << "    \"snapshots_created\": " << statistics_.snapshots_created << ",\n";
    out << "    \"snapshots_restored\": " << statistics_.snapshots_restored << ",\n";
    out << "    \"snapshot_failures\": " << statistics_.snapshot_failures << ",\n";
    out << "    \"allocated_backing_bytes\": " << statistics_.allocated_backing_bytes << "\n";
    out << "  },\n";

    out << "  \"mapping_ranges\": [\n";
    for (std::size_t i = 0; i < ranges_.size(); ++i) {
        const MappingRangeInfo& range = ranges_[i];
        out << "    {\"domain\": \"" << MappingDomainName(range.domain) << "\", ";
        out << "\"address_space\": " << range.address_space.Value() << ", ";
        out << "\"base\": " << range.base << ", ";
        out << "\"base_hex\": \"" << FormatAddress(range.base) << "\", ";
        out << "\"size\": " << range.size << ", ";
        out << "\"page_size\": " << range.page_size << ", ";
        out << kPermissionsJsonKey << ": " << PermissionsJson(range.permissions) << "}";
        if (i + 1 != ranges_.size()) out << ',';
        out << '\n';
    }
    out << "  ],\n";

    out << "  \"snapshots\": [\n";
    for (std::size_t i = 0; i < snapshots_.size(); ++i) {
        const SnapshotSummary& snapshot = snapshots_[i];
        out << "    {\"id\": " << snapshot.id << ", ";
        out << "\"name\": \"" << ::hv::detail::JsonEscape(snapshot.name) << "\", ";
        out << "\"size\": " << snapshot.size << ", ";
        out << "\"checksum\": " << snapshot.checksum << ", ";
        out << "\"created_unix_ms\": " << snapshot.created_unix_ms << ", ";
        out << "\"valid\": " << (snapshot.valid ? "true" : "false") << "}";
        if (i + 1 != snapshots_.size()) out << ',';
        out << '\n';
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

void MemoryDiagnostics::Reset() {
    std::lock_guard lock(mutex_);
    statistics_ = MemoryStatistics{};
    ranges_.clear();
    snapshots_.clear();
}

} // namespace hv::memory
