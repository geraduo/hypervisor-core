#include "hv/memory/memory_snapshot_manager.hpp"

#include "hv/memory/address_space_manager.hpp"
#include "hv/memory/ept_manager.hpp"
#include "hv/memory/guest_memory_manager.hpp"
#include "hv/memory/memory_diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <utility>

namespace hv::memory {
namespace {

class Fnv1a64 final {
public:
    void Byte(std::uint8_t value) noexcept {
        hash_ ^= value;
        hash_ *= 1099511628211ULL;
    }

    void Bytes(const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) Byte(bytes[i]);
    }

    void U64(std::uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i) {
            Byte(static_cast<std::uint8_t>(value & 0xFFULL));
            value >>= 8U;
        }
    }

    void String(const std::string& text) noexcept {
        U64(static_cast<std::uint64_t>(text.size()));
        Bytes(text.data(), text.size());
    }

    void PermissionsBits(Permissions permissions) noexcept {
        std::uint64_t bits = 0;
        if (permissions.read) bits |= 1ULL;
        if (permissions.write) bits |= 2ULL;
        if (permissions.execute) bits |= 4ULL;
        U64(bits);
    }

    [[nodiscard]] std::uint64_t Value() const noexcept { return hash_; }

private:
    std::uint64_t hash_{1469598103934665603ULL};
};

std::uint64_t NowUnixMilliseconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

MemorySnapshotManager::MemorySnapshotManager(GuestMemoryManager& memory,
                                             GuestAddressSpaceManager& spaces,
                                             EptManager& ept,
                                             MemoryDiagnostics* diagnostics)
    : memory_(memory), spaces_(spaces), ept_(ept), diagnostics_(diagnostics) {}

std::uint64_t MemorySnapshotManager::CalculateChecksumLocked(const Snapshot& snapshot) const {
    Fnv1a64 hash;
    hash.U64(snapshot.info.id);
    hash.String(snapshot.info.name);
    hash.U64(static_cast<std::uint64_t>(snapshot.info.total_bytes));
    hash.U64(snapshot.info.created_unix_ms);
    hash.U64(static_cast<std::uint64_t>(snapshot.regions.size()));
    hash.U64(static_cast<std::uint64_t>(snapshot.guest_mappings.size()));
    hash.U64(static_cast<std::uint64_t>(snapshot.ept_mappings.size()));

    for (const RegionSnapshot& region : snapshot.regions) {
        hash.String(region.name);
        hash.U64(region.guest_base.Value());
        hash.U64(static_cast<std::uint64_t>(region.size));
        hash.U64(region.host_base.Value());
        hash.PermissionsBits(region.permissions);
        hash.Bytes(region.bytes.data(), region.bytes.size());
    }
    for (const GuestMappingSnapshot& mapping : snapshot.guest_mappings) {
        hash.U64(mapping.address_space.Value());
        hash.U64(mapping.virtual_page.Value());
        hash.U64(mapping.guest_physical_page.Value());
        hash.PermissionsBits(mapping.permissions);
    }
    for (const EptMappingSnapshot& mapping : snapshot.ept_mappings) {
        hash.U64(mapping.guest_page.Value());
        hash.U64(mapping.host_page.Value());
        hash.U64(mapping.page_size);
        hash.PermissionsBits(mapping.permissions);
    }
    return hash.Value();
}

MemorySnapshotInfo MemorySnapshotManager::InfoLocked(const Snapshot& snapshot) const {
    MemorySnapshotInfo info = snapshot.info;
    info.region_count = snapshot.regions.size();
    info.guest_mapping_count = snapshot.guest_mappings.size();
    info.ept_mapping_count = snapshot.ept_mappings.size();
    info.valid = CalculateChecksumLocked(snapshot) == snapshot.info.checksum;
    return info;
}

bool MemorySnapshotManager::ChecksumMatchesLocked(const Snapshot& snapshot) const {
    return CalculateChecksumLocked(snapshot) == snapshot.info.checksum;
}

MemoryResult MemorySnapshotManager::VerifyLocked(const Snapshot& snapshot) const {
    if (snapshot.regions.size() != snapshot.info.region_count ||
        snapshot.guest_mappings.size() != snapshot.info.guest_mapping_count ||
        snapshot.ept_mappings.size() != snapshot.info.ept_mapping_count) {
        return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                   "Snapshot metadata counts do not match its payload");
    }

    std::uint64_t total = 0;
    for (const RegionSnapshot& region : snapshot.regions) {
        if (region.name.empty() || region.name.size() > 128) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot contains an invalid region name");
        }
        if (region.size == 0 ||
            region.size % static_cast<std::size_t>(kSmallPageSize) != 0) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot region size is not a non-zero page multiple");
        }
        if (region.bytes.size() != region.size) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot region payload size does not match its metadata");
        }
        if (!region.guest_base.IsAlignedTo(kSmallPageSize) ||
            !region.host_base.IsAlignedTo(kSmallPageSize)) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot region addresses are not page aligned");
        }
        const std::uint64_t size64 = static_cast<std::uint64_t>(region.size);
        if (!RangeFits(region.guest_base.Value(), size64, kMaxPhysicalAddress) ||
            !RangeFits(region.host_base.Value(), size64, kMaxPhysicalAddress)) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot region exceeds the 48-bit model");
        }
        if (total > std::numeric_limits<std::uint64_t>::max() - size64) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot total size overflows the model");
        }
        total += size64;

        MemoryBlock block{};
        if (!memory_.Backing().TryGetBlock(region.host_base, block) ||
            block.size < region.size) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot region is not backed by a live allocation");
        }
    }
    if (total != static_cast<std::uint64_t>(snapshot.info.total_bytes)) {
        return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                   "Snapshot total size metadata is inconsistent");
    }

    std::vector<const RegionSnapshot*> sorted_regions;
    sorted_regions.reserve(snapshot.regions.size());
    for (const RegionSnapshot& region : snapshot.regions) sorted_regions.push_back(&region);
    std::sort(sorted_regions.begin(), sorted_regions.end(),
              [](const RegionSnapshot* lhs, const RegionSnapshot* rhs) {
                  return lhs->guest_base < rhs->guest_base;
              });
    for (std::size_t i = 1; i < sorted_regions.size(); ++i) {
        const RegionSnapshot& previous = *sorted_regions[i - 1];
        const RegionSnapshot& current = *sorted_regions[i];
        if (previous.guest_base.Value() + static_cast<std::uint64_t>(previous.size) >
            current.guest_base.Value()) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot contains overlapping guest regions");
        }
    }

    std::set<std::pair<std::uint64_t, std::uint64_t>> guest_pages;
    for (const GuestMappingSnapshot& mapping : snapshot.guest_mappings) {
        if (!mapping.address_space.IsValid() || !spaces_.Contains(mapping.address_space)) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot references a missing address space");
        }
        if (!mapping.virtual_page.IsAlignedTo(kSmallPageSize) ||
            !mapping.guest_physical_page.IsAlignedTo(kSmallPageSize) ||
            !IsCanonicalPhysicalAddress(mapping.virtual_page.Value()) ||
            !IsCanonicalPhysicalAddress(mapping.guest_physical_page.Value())) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot guest mapping is not page aligned");
        }
        const auto key = std::make_pair(mapping.address_space.Value(),
                                        mapping.virtual_page.Value());
        if (!guest_pages.insert(key).second) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot contains a duplicate guest mapping");
        }
        GuestPageInfo page{};
        if (!memory_.QueryPage(mapping.guest_physical_page, page).Succeeded()) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot guest mapping points to unbacked guest memory");
        }
    }

    std::vector<EptMappingSnapshot> sorted_ept = snapshot.ept_mappings;
    std::sort(sorted_ept.begin(), sorted_ept.end(),
              [](const EptMappingSnapshot& lhs, const EptMappingSnapshot& rhs) {
                  return lhs.guest_page < rhs.guest_page;
              });
    for (std::size_t i = 0; i < sorted_ept.size(); ++i) {
        const EptMappingSnapshot& mapping = sorted_ept[i];
        if (!IsSupportedPageSize(mapping.page_size) ||
            !mapping.guest_page.IsAlignedTo(mapping.page_size) ||
            !mapping.host_page.IsAlignedTo(mapping.page_size)) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot EPT mapping has an invalid page size or alignment");
        }
        if (!RangeFits(mapping.guest_page.Value(), mapping.page_size, kMaxPhysicalAddress) ||
            !RangeFits(mapping.host_page.Value(), mapping.page_size, kMaxPhysicalAddress)) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot EPT mapping exceeds the 48-bit model");
        }
        MemoryBlock block{};
        std::size_t offset = 0;
        if (!memory_.Backing().TryResolve(mapping.host_page, block, offset) ||
            static_cast<std::uint64_t>(mapping.page_size) >
                static_cast<std::uint64_t>(block.size - offset)) {
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Snapshot EPT mapping is not backed by live memory");
        }
        if (i > 0) {
            const EptMappingSnapshot& previous = sorted_ept[i - 1];
            if (previous.guest_page.Value() + previous.page_size > mapping.guest_page.Value()) {
                return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                           "Snapshot contains overlapping EPT mappings");
            }
        }
    }

    if (!ChecksumMatchesLocked(snapshot)) {
        return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                   "Snapshot checksum does not match its metadata and payload");
    }
    return MemoryResult::Ok();
}

MemorySnapshotResult MemorySnapshotManager::CreateSnapshot(std::string name) {
    if (name.empty() || name.size() > 128) {
        if (diagnostics_ != nullptr) {
            diagnostics_->RecordSnapshotCreated(MemoryStatus::InvalidContext);
        }
        return MemorySnapshotResult{MemoryStatus::InvalidContext, {}, {},
                                    "Snapshot name must contain between 1 and 128 characters"};
    }

    std::lock_guard lock(mutex_);
    Snapshot snapshot{};
    snapshot.info.name = std::move(name);
    snapshot.info.created_unix_ms = NowUnixMilliseconds();

    std::uint64_t total = 0;
    const std::vector<GuestRegionInfo> regions = memory_.EnumerateRegions();
    for (const GuestRegionInfo& region : regions) {
        const std::uint64_t size64 = static_cast<std::uint64_t>(region.size);
        if (size64 > max_snapshot_bytes_ || total > max_snapshot_bytes_ - size64) {
            if (diagnostics_ != nullptr) {
                diagnostics_->RecordSnapshotCreated(MemoryStatus::OutOfMemory);
            }
            return MemorySnapshotResult{MemoryStatus::OutOfMemory, {}, {},
                                        "Snapshot exceeds the configured size limit"};
        }
        RegionSnapshot saved{};
        saved.name = region.name;
        saved.guest_base = region.guest_base;
        saved.size = region.size;
        saved.host_base = region.host_base;
        saved.permissions = region.permissions;
        saved.bytes.resize(region.size);
        const MemoryResult read = memory_.ReadDirect(region.guest_base, saved.bytes);
        if (!read.Succeeded()) {
            if (diagnostics_ != nullptr) {
                diagnostics_->RecordSnapshotCreated(read.status);
            }
            return MemorySnapshotResult{read.status, {}, {}, read.message};
        }
        total += size64;
        snapshot.regions.push_back(std::move(saved));
    }
    snapshot.info.total_bytes = static_cast<std::size_t>(total);

    for (const AddressSpaceInfo& space : spaces_.EnumerateAddressSpaces()) {
        for (const GuestMappingInfo& mapping : spaces_.EnumerateMappings(space.id)) {
            snapshot.guest_mappings.push_back(GuestMappingSnapshot{
                mapping.address_space, mapping.virtual_page, mapping.guest_physical_page,
                mapping.permissions});
        }
    }
    for (const EptMappingInfo& mapping : ept_.Enumerate()) {
        snapshot.ept_mappings.push_back(
            EptMappingSnapshot{mapping.guest_physical_page, mapping.host_physical_page,
                               mapping.page_size, mapping.permissions});
    }

    if (next_snapshot_id_ == 0) {
        if (diagnostics_ != nullptr) {
            diagnostics_->RecordSnapshotCreated(MemoryStatus::InternalError);
        }
        return MemorySnapshotResult{MemoryStatus::InternalError, {}, {},
                                    "Snapshot identifier space is exhausted"};
    }
    while (snapshots_.contains(next_snapshot_id_)) {
        if (next_snapshot_id_ == std::numeric_limits<std::uint64_t>::max()) {
            if (diagnostics_ != nullptr) {
                diagnostics_->RecordSnapshotCreated(MemoryStatus::InternalError);
            }
            return MemorySnapshotResult{MemoryStatus::InternalError, {}, {},
                                        "Snapshot identifier space is exhausted"};
        }
        ++next_snapshot_id_;
    }
    const std::uint64_t snapshot_id = next_snapshot_id_++;
    snapshot.info.id = snapshot_id;
    snapshot.info.region_count = snapshot.regions.size();
    snapshot.info.guest_mapping_count = snapshot.guest_mappings.size();
    snapshot.info.ept_mapping_count = snapshot.ept_mappings.size();
    snapshot.info.checksum = CalculateChecksumLocked(snapshot);
    snapshot.info.valid = true;

    const MemorySnapshotInfo info = snapshot.info;
    snapshots_.emplace(snapshot_id, std::move(snapshot));
    if (diagnostics_ != nullptr) {
        diagnostics_->RecordSnapshotCreated(MemoryStatus::Success);
    }
    return MemorySnapshotResult{MemoryStatus::Success, snapshot_id, info, {}};
}

MemoryResult MemorySnapshotManager::RestoreSnapshot(std::uint64_t snapshot_id) {
    std::lock_guard lock(mutex_);
    const auto it = snapshots_.find(snapshot_id);
    if (it == snapshots_.end()) {
        if (diagnostics_ != nullptr) diagnostics_->RecordSnapshotRestored(
            MemoryStatus::SnapshotNotFound);
        return MemoryResult::Error(MemoryStatus::SnapshotNotFound,
                                   "Snapshot does not exist");
    }
    const Snapshot& snapshot = it->second;
    const MemoryResult verified = VerifyLocked(snapshot);
    if (!verified.Succeeded()) {
        if (diagnostics_ != nullptr) {
            diagnostics_->RecordSnapshotRestored(MemoryStatus::SnapshotCorrupt);
        }
        return verified;
    }

    const std::vector<GuestRegionInfo> current_regions = memory_.EnumerateRegions();
    if (current_regions.size() != snapshot.regions.size()) {
        if (diagnostics_ != nullptr) {
            diagnostics_->RecordSnapshotRestored(MemoryStatus::SnapshotCorrupt);
        }
        return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                   "Live guest-region layout does not match the snapshot");
    }
    for (const RegionSnapshot& saved : snapshot.regions) {
        GuestRegionInfo current{};
        const MemoryResult found = memory_.GetRegion(saved.name, current);
        if (!found.Succeeded() || current.guest_base != saved.guest_base ||
            current.host_base != saved.host_base || current.size != saved.size) {
            if (diagnostics_ != nullptr) {
                diagnostics_->RecordSnapshotRestored(MemoryStatus::SnapshotCorrupt);
            }
            return MemoryResult::Error(MemoryStatus::SnapshotCorrupt,
                                       "Live guest region does not match the snapshot metadata");
        }
    }

    for (const AddressSpaceInfo& space : spaces_.EnumerateAddressSpaces()) {
        const MemoryResult cleared = spaces_.ClearMappings(space.id);
        if (!cleared.Succeeded()) {
            if (diagnostics_ != nullptr) {
                diagnostics_->RecordSnapshotRestored(cleared.status);
            }
            return cleared;
        }
    }
    ept_.Clear();

    for (const EptMappingSnapshot& mapping : snapshot.ept_mappings) {
        const MemoryResult mapped = ept_.Map(mapping.guest_page, mapping.host_page,
                                             mapping.permissions, mapping.page_size);
        if (!mapped.Succeeded()) {
            if (diagnostics_ != nullptr) {
                diagnostics_->RecordSnapshotRestored(MemoryStatus::InternalError);
            }
            return MemoryResult::Error(MemoryStatus::InternalError,
                                       "Failed to restore an EPT mapping: " + mapped.message);
        }
    }
    for (const GuestMappingSnapshot& mapping : snapshot.guest_mappings) {
        const MemoryResult mapped = spaces_.Map(mapping.address_space, mapping.virtual_page,
                                                mapping.guest_physical_page,
                                                mapping.permissions);
        if (!mapped.Succeeded()) {
            if (diagnostics_ != nullptr) {
                diagnostics_->RecordSnapshotRestored(MemoryStatus::InternalError);
            }
            return MemoryResult::Error(MemoryStatus::InternalError,
                                       "Failed to restore a guest mapping: " + mapped.message);
        }
    }
    for (const RegionSnapshot& region : snapshot.regions) {
        const MemoryResult written = memory_.WriteDirect(region.guest_base, region.bytes);
        if (!written.Succeeded()) {
            if (diagnostics_ != nullptr) {
                diagnostics_->RecordSnapshotRestored(written.status);
            }
            return written;
        }
        const MemoryResult permissions =
            memory_.SetRegionPermissions(region.name, region.permissions);
        if (!permissions.Succeeded()) {
            if (diagnostics_ != nullptr) {
                diagnostics_->RecordSnapshotRestored(permissions.status);
            }
            return permissions;
        }
    }

    if (diagnostics_ != nullptr) {
        diagnostics_->RecordSnapshotRestored(MemoryStatus::Success);
    }
    return MemoryResult::Ok();
}

MemoryResult MemorySnapshotManager::DeleteSnapshot(std::uint64_t snapshot_id) {
    std::lock_guard lock(mutex_);
    if (snapshots_.erase(snapshot_id) == 0) {
        return MemoryResult::Error(MemoryStatus::SnapshotNotFound,
                                   "Snapshot does not exist");
    }
    return MemoryResult::Ok();
}

MemoryResult MemorySnapshotManager::VerifySnapshot(std::uint64_t snapshot_id) const {
    std::lock_guard lock(mutex_);
    const auto it = snapshots_.find(snapshot_id);
    if (it == snapshots_.end()) {
        return MemoryResult::Error(MemoryStatus::SnapshotNotFound,
                                   "Snapshot does not exist");
    }
    return VerifyLocked(it->second);
}

MemoryResult MemorySnapshotManager::GetSnapshotInfo(std::uint64_t snapshot_id,
                                                    MemorySnapshotInfo& out) const {
    std::lock_guard lock(mutex_);
    const auto it = snapshots_.find(snapshot_id);
    if (it == snapshots_.end()) {
        return MemoryResult::Error(MemoryStatus::SnapshotNotFound,
                                   "Snapshot does not exist");
    }
    out = InfoLocked(it->second);
    return MemoryResult::Ok();
}

std::vector<MemorySnapshotInfo> MemorySnapshotManager::GetSnapshotSummaries() const {
    std::lock_guard lock(mutex_);
    std::vector<MemorySnapshotInfo> result;
    result.reserve(snapshots_.size());
    for (const auto& [id, snapshot] : snapshots_) {
        (void)id;
        result.push_back(InfoLocked(snapshot));
    }
    return result;
}

std::size_t MemorySnapshotManager::SnapshotCount() const {
    std::lock_guard lock(mutex_);
    return snapshots_.size();
}

void MemorySnapshotManager::SetMaxSnapshotBytes(std::size_t max_bytes) {
    std::lock_guard lock(mutex_);
    max_snapshot_bytes_ = max_bytes;
}

std::size_t MemorySnapshotManager::GetMaxSnapshotBytes() const {
    std::lock_guard lock(mutex_);
    return max_snapshot_bytes_;
}

void MemorySnapshotManager::Clear() {
    std::lock_guard lock(mutex_);
    snapshots_.clear();
}

MemoryResult MemorySnapshotManager::CorruptSnapshotForTesting(std::uint64_t snapshot_id,
                                                              std::size_t offset) {
    std::lock_guard lock(mutex_);
    const auto it = snapshots_.find(snapshot_id);
    if (it == snapshots_.end()) {
        return MemoryResult::Error(MemoryStatus::SnapshotNotFound,
                                   "Snapshot does not exist");
    }
    Snapshot& snapshot = it->second;
    std::size_t remaining = offset;
    for (RegionSnapshot& region : snapshot.regions) {
        if (remaining < region.bytes.size()) {
            region.bytes[remaining] =
                static_cast<std::byte>(std::to_integer<unsigned int>(region.bytes[remaining]) ^ 0x5AU);
            snapshot.info.valid = false;
            return MemoryResult::Ok();
        }
        remaining -= region.bytes.size();
    }
    snapshot.info.checksum ^= 1ULL;
    snapshot.info.valid = false;
    return MemoryResult::Ok();
}

} // namespace hv::memory
