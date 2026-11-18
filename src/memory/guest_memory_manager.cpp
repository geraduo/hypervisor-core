#include "hv/memory/guest_memory_manager.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace hv::memory {
namespace {

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) noexcept {
    if (alignment == 0) return value;
    const std::uint64_t remainder = value & (alignment - 1ULL);
    if (remainder == 0) return value;
    const std::uint64_t increment = alignment - remainder;
    if (value > std::numeric_limits<std::uint64_t>::max() - increment) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return value + increment;
}

bool SpanFitsModel(std::uint64_t address, std::size_t size) noexcept {
    if (size == 0) return true;
    if (address > kMaxPhysicalAddress) return false;
    return static_cast<std::uint64_t>(size - 1) <= kMaxPhysicalAddress - address;
}

} // namespace

GuestMemoryManager::GuestMemoryManager(IMemoryBackingProvider& backing) : backing_(backing) {}

GuestMemoryManager::Region* GuestMemoryManager::FindRegionLocked(
    GuestPhysicalAddress address) noexcept {
    if (regions_by_address_.empty()) return nullptr;
    auto it = regions_by_address_.upper_bound(address);
    if (it == regions_by_address_.begin()) return nullptr;
    --it;
    const auto region_it = regions_by_id_.find(it->second);
    if (region_it == regions_by_id_.end()) return nullptr;
    Region& region = region_it->second;
    if (address.Value() < region.info.guest_base.Value()) return nullptr;
    const std::uint64_t offset = address.Value() - region.info.guest_base.Value();
    if (offset >= static_cast<std::uint64_t>(region.info.size)) return nullptr;
    return &region;
}

const GuestMemoryManager::Region* GuestMemoryManager::FindRegionLocked(
    GuestPhysicalAddress address) const noexcept {
    if (regions_by_address_.empty()) return nullptr;
    auto it = regions_by_address_.upper_bound(address);
    if (it == regions_by_address_.begin()) return nullptr;
    --it;
    const auto region_it = regions_by_id_.find(it->second);
    if (region_it == regions_by_id_.end()) return nullptr;
    const Region& region = region_it->second;
    if (address.Value() < region.info.guest_base.Value()) return nullptr;
    const std::uint64_t offset = address.Value() - region.info.guest_base.Value();
    if (offset >= static_cast<std::uint64_t>(region.info.size)) return nullptr;
    return &region;
}

const GuestMemoryManager::Region* GuestMemoryManager::FindRegionByNameLocked(
    std::string_view name) const noexcept {
    for (const auto& [id, region] : regions_by_id_) {
        (void)id;
        if (region.info.name == name) return &region;
    }
    return nullptr;
}

bool GuestMemoryManager::OverlapsLocked(GuestPhysicalAddress base, std::size_t size) const noexcept {
    const std::uint64_t begin = base.Value();
    const std::uint64_t end = begin + static_cast<std::uint64_t>(size);
    for (const auto& [id, region] : regions_by_id_) {
        (void)id;
        const std::uint64_t other_begin = region.info.guest_base.Value();
        const std::uint64_t other_end = other_begin + static_cast<std::uint64_t>(region.info.size);
        if (begin < other_end && other_begin < end) return true;
    }
    return false;
}

GuestRegionResult GuestMemoryManager::AddRegion(const GuestRegionRequest& request) {
    if (request.name.empty() || request.name.size() > 128) {
        return GuestRegionResult{MemoryStatus::InvalidContext, {}, {},
                                 "Guest region name must contain between 1 and 128 characters"};
    }
    if (request.size == 0) {
        return GuestRegionResult{MemoryStatus::InvalidAddress, {}, {},
                                 "Guest region size must not be zero"};
    }
    if (request.size % static_cast<std::size_t>(kSmallPageSize) != 0) {
        return GuestRegionResult{MemoryStatus::AlignmentError, {}, {},
                                 "Guest region size must be a multiple of 4 KiB"};
    }
    if (request.alignment < static_cast<std::size_t>(kSmallPageSize) ||
        request.alignment > static_cast<std::size_t>(kHugePageSize) ||
        !IsPowerOfTwo(static_cast<std::uint64_t>(request.alignment))) {
        return GuestRegionResult{MemoryStatus::AlignmentError, {}, {},
                                 "Guest region alignment must be a supported power of two"};
    }

    const std::uint64_t size64 = static_cast<std::uint64_t>(request.size);
    if (size64 > kMaxPhysicalAddress) {
        return GuestRegionResult{MemoryStatus::InvalidAddress, {}, {},
                                 "Guest region is too large for the 48-bit model"};
    }

    std::lock_guard lock(mutex_);
    if (FindRegionByNameLocked(request.name) != nullptr) {
        return GuestRegionResult{MemoryStatus::InvalidMapping, {}, {},
                                 "A guest region with this name already exists"};
    }

    GuestPhysicalAddress base = request.guest_base;
    if (base.IsZero()) {
        const std::uint64_t aligned = AlignUp(next_guest_address_.Value(),
                                              static_cast<std::uint64_t>(request.alignment));
        base = GuestPhysicalAddress{aligned};
    } else if (!base.IsAlignedTo(static_cast<std::uint64_t>(request.alignment))) {
        return GuestRegionResult{MemoryStatus::AlignmentError, {}, {},
                                 "Explicit guest region base is not aligned to the requested alignment"};
    }

    if (!RangeFits(base.Value(), size64, kMaxPhysicalAddress)) {
        return GuestRegionResult{MemoryStatus::InvalidAddress, {}, {},
                                 "Guest region exceeds the 48-bit physical model"};
    }
    if (OverlapsLocked(base, request.size)) {
        return GuestRegionResult{MemoryStatus::InvalidMapping, {}, {},
                                 "Guest region overlaps an existing region"};
    }
    if (!base.IsAlignedTo(kSmallPageSize)) {
        return GuestRegionResult{MemoryStatus::AlignmentError, {}, {},
                                 "Guest region base must be 4 KiB aligned"};
    }

    const MemoryBlockResult allocation =
        backing_.Allocate(request.size, request.alignment);
    if (!allocation.Succeeded()) {
        return GuestRegionResult{allocation.status, {}, {}, allocation.message};
    }
    if (!allocation.block.address.IsAlignedTo(static_cast<std::uint64_t>(request.alignment))) {
        (void)backing_.Release(allocation.block.address);
        return GuestRegionResult{MemoryStatus::InternalError, {}, {},
                                 "Backing provider returned a misaligned host-physical address"};
    }

    if (next_region_id_ == 0) {
        (void)backing_.Release(allocation.block.address);
        return GuestRegionResult{MemoryStatus::InternalError, {}, {},
                                 "Guest region identifier space is exhausted"};
    }
    while (regions_by_id_.contains(next_region_id_)) {
        if (next_region_id_ == std::numeric_limits<std::uint64_t>::max()) {
            (void)backing_.Release(allocation.block.address);
            return GuestRegionResult{MemoryStatus::InternalError, {}, {},
                                     "Guest region identifier space is exhausted"};
        }
        ++next_region_id_;
    }
    const std::uint64_t region_id = next_region_id_++;

    Region region{};
    region.id = region_id;
    region.info.id = region_id;
    region.info.name = request.name;
    region.info.guest_base = base;
    region.info.size = request.size;
    region.info.host_base = allocation.block.address;
    region.info.permissions = request.permissions;

    const auto inserted = regions_by_id_.emplace(region_id, std::move(region));
    if (!inserted.second) {
        (void)backing_.Release(allocation.block.address);
        return GuestRegionResult{MemoryStatus::InternalError, {}, {},
                                 "Guest region table rejected a duplicate identifier"};
    }
    regions_by_address_.emplace(base, region_id);

    const std::uint64_t end = base.Value() + size64;
    if (end > next_guest_address_.Value()) {
        next_guest_address_ = GuestPhysicalAddress{end};
    }

    return GuestRegionResult{MemoryStatus::Success, region_id, inserted.first->second.info, {}};
}

MemoryResult GuestMemoryManager::RemoveRegion(std::string_view name) {
    std::uint64_t region_id = 0;
    {
        std::lock_guard lock(mutex_);
        const Region* region = FindRegionByNameLocked(name);
        if (region == nullptr) {
            return MemoryResult::Error(MemoryStatus::InvalidContext,
                                       "Guest region does not exist");
        }
        region_id = region->id;
    }
    return RemoveRegion(region_id);
}

MemoryResult GuestMemoryManager::RemoveRegion(std::uint64_t region_id) {
    std::lock_guard lock(mutex_);
    const auto it = regions_by_id_.find(region_id);
    if (it == regions_by_id_.end()) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Guest region does not exist");
    }
    const MemoryResult released = backing_.Release(it->second.info.host_base);
    if (!released.Succeeded()) return released;
    regions_by_address_.erase(it->second.info.guest_base);
    regions_by_id_.erase(it);
    return MemoryResult::Ok();
}

MemoryResult GuestMemoryManager::ReleaseAll() {
    std::lock_guard lock(mutex_);
    MemoryResult first_error = MemoryResult::Ok();
    for (const auto& [id, region] : regions_by_id_) {
        (void)id;
        const MemoryResult released = backing_.Release(region.info.host_base);
        if (!released.Succeeded() && first_error.Succeeded()) first_error = released;
    }
    regions_by_id_.clear();
    regions_by_address_.clear();
    return first_error;
}

MemoryResult GuestMemoryManager::SetRegionPermissions(std::string_view name,
                                                       Permissions permissions) {
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(regions_by_id_.begin(), regions_by_id_.end(),
                                 [&name](const auto& entry) {
                                     return entry.second.info.name == name;
                                 });
    if (it == regions_by_id_.end()) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Guest region does not exist");
    }
    it->second.info.permissions = permissions;
    return MemoryResult::Ok();
}

MemoryResult GuestMemoryManager::GetRegion(std::string_view name, GuestRegionInfo& out) const {
    std::lock_guard lock(mutex_);
    const Region* region = FindRegionByNameLocked(name);
    if (region == nullptr) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Guest region does not exist");
    }
    out = region->info;
    return MemoryResult::Ok();
}

MemoryResult GuestMemoryManager::GetRegionById(std::uint64_t region_id,
                                               GuestRegionInfo& out) const {
    std::lock_guard lock(mutex_);
    const auto it = regions_by_id_.find(region_id);
    if (it == regions_by_id_.end()) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Guest region does not exist");
    }
    out = it->second.info;
    return MemoryResult::Ok();
}

std::vector<GuestRegionInfo> GuestMemoryManager::EnumerateRegions() const {
    std::lock_guard lock(mutex_);
    std::vector<GuestRegionInfo> result;
    result.reserve(regions_by_id_.size());
    for (const auto& [id, region] : regions_by_id_) {
        (void)id;
        result.push_back(region.info);
    }
    return result;
}

MemoryResult GuestMemoryManager::QueryPage(GuestPhysicalAddress page, GuestPageInfo& out) const {
    if (!page.IsAlignedTo(kSmallPageSize)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Guest-physical page query requires 4 KiB alignment");
    }
    std::lock_guard lock(mutex_);
    const Region* region = FindRegionLocked(page);
    if (region == nullptr) {
        return MemoryResult::Error(MemoryStatus::Unmapped,
                                   "Guest-physical page has no registered region");
    }
    const std::uint64_t offset = page.Value() - region->info.guest_base.Value();
    if (offset + kSmallPageSize > static_cast<std::uint64_t>(region->info.size)) {
        return MemoryResult::Error(MemoryStatus::Unmapped,
                                   "Guest-physical page is only partially backed");
    }
    out.guest_physical = page;
    out.host_physical = HostPhysicalAddress{region->info.host_base.Value() + offset};
    out.region_name = region->info.name;
    out.region_offset = static_cast<std::size_t>(offset);
    out.permissions = region->info.permissions;
    out.backed = true;
    return MemoryResult::Ok();
}

MemoryResult GuestMemoryManager::EnumeratePages(std::string_view region_name,
                                                std::vector<GuestPageInfo>& out) const {
    std::lock_guard lock(mutex_);
    const Region* region = FindRegionByNameLocked(region_name);
    if (region == nullptr) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Guest region does not exist");
    }
    const std::size_t pages = region->info.size / static_cast<std::size_t>(kSmallPageSize);
    out.clear();
    out.reserve(pages);
    for (std::size_t index = 0; index < pages; ++index) {
        const std::uint64_t offset = static_cast<std::uint64_t>(index) * kSmallPageSize;
        out.push_back(GuestPageInfo{
            GuestPhysicalAddress{region->info.guest_base.Value() + offset},
            HostPhysicalAddress{region->info.host_base.Value() + offset},
            region->info.name, static_cast<std::size_t>(offset),
            region->info.permissions, true});
    }
    return MemoryResult::Ok();
}

GuestPhysicalTranslation GuestMemoryManager::Translate(GuestPhysicalAddress address,
                                                       Access access) const {
    std::lock_guard lock(mutex_);
    return TranslateLocked(address, true, access);
}

GuestPhysicalTranslation GuestMemoryManager::TranslateLocked(GuestPhysicalAddress address,
                                                             bool check_access,
                                                             Access access) const {
    GuestPhysicalTranslation result{};
    result.guest_physical = address;
    if (!IsCanonicalPhysicalAddress(address.Value())) {
        result.status = MemoryStatus::InvalidAddress;
        result.message = "Guest-physical address exceeds the 48-bit model";
        return result;
    }
    const Region* region = FindRegionLocked(address);
    if (region == nullptr) {
        result.status = MemoryStatus::Unmapped;
        result.message = "Guest-physical address has no registered region";
        return result;
    }
    const std::uint64_t offset = address.Value() - region->info.guest_base.Value();
    if (offset >= static_cast<std::uint64_t>(region->info.size)) {
        result.status = MemoryStatus::Unmapped;
        result.message = "Guest-physical address lies outside its registered region";
        return result;
    }
    result.permissions = region->info.permissions;
    if (check_access && !region->info.permissions.Allows(access)) {
        result.status = MemoryStatus::AccessDenied;
        result.message = "Guest region permissions deny this access";
        return result;
    }
    result.host_physical = HostPhysicalAddress{region->info.host_base.Value() + offset};
    result.region_name = region->info.name;
    result.region_offset = static_cast<std::size_t>(offset);
    result.status = MemoryStatus::Success;
    return result;
}

MemoryResult GuestMemoryManager::Read(GuestPhysicalAddress address,
                                      std::span<std::byte> destination) const {
    if (destination.empty()) return MemoryResult::Ok();
    if (!SpanFitsModel(address.Value(), destination.size())) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Guest-physical read span exceeds the 48-bit model");
    }
    std::lock_guard lock(mutex_);
    std::size_t done = 0;
    while (done < destination.size()) {
        const GuestPhysicalAddress current{address.Value() + static_cast<std::uint64_t>(done)};
        const GuestPhysicalTranslation translation = TranslateLocked(current, true, Access::Read);
        if (!translation.Succeeded()) {
            return MemoryResult::Error(translation.status, translation.message);
        }
        const std::uint64_t page_offset = current.Value() & (kSmallPageSize - 1ULL);
        const std::size_t remaining = destination.size() - done;
        const std::size_t chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(kSmallPageSize - page_offset));
        const MemoryResult read = backing_.Read(translation.host_physical,
                                                destination.subspan(done, chunk));
        if (!read.Succeeded()) return read;
        done += chunk;
    }
    return MemoryResult::Ok();
}

MemoryResult GuestMemoryManager::Write(GuestPhysicalAddress address,
                                       std::span<const std::byte> source) {
    if (source.empty()) return MemoryResult::Ok();
    if (!SpanFitsModel(address.Value(), source.size())) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Guest-physical write span exceeds the 48-bit model");
    }
    std::lock_guard lock(mutex_);
    std::size_t done = 0;
    while (done < source.size()) {
        const GuestPhysicalAddress current{address.Value() + static_cast<std::uint64_t>(done)};
        const GuestPhysicalTranslation translation = TranslateLocked(current, true, Access::Write);
        if (!translation.Succeeded()) {
            return MemoryResult::Error(translation.status, translation.message);
        }
        const std::uint64_t page_offset = current.Value() & (kSmallPageSize - 1ULL);
        const std::size_t remaining = source.size() - done;
        const std::size_t chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(kSmallPageSize - page_offset));
        const MemoryResult write = backing_.Write(translation.host_physical,
                                                  source.subspan(done, chunk));
        if (!write.Succeeded()) return write;
        done += chunk;
    }
    return MemoryResult::Ok();
}

MemoryResult GuestMemoryManager::ReadDirect(GuestPhysicalAddress address,
                                            std::span<std::byte> destination) const {
    if (destination.empty()) return MemoryResult::Ok();
    if (!SpanFitsModel(address.Value(), destination.size())) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Direct guest-physical read span exceeds the 48-bit model");
    }
    std::lock_guard lock(mutex_);
    std::size_t done = 0;
    while (done < destination.size()) {
        const GuestPhysicalAddress current{address.Value() + static_cast<std::uint64_t>(done)};
        const GuestPhysicalTranslation translation =
            TranslateLocked(current, false, Access::Read);
        if (!translation.Succeeded()) {
            return MemoryResult::Error(translation.status, translation.message);
        }
        const std::uint64_t page_offset = current.Value() & (kSmallPageSize - 1ULL);
        const std::size_t remaining = destination.size() - done;
        const std::size_t chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(kSmallPageSize - page_offset));
        const MemoryResult read = backing_.Read(translation.host_physical,
                                                destination.subspan(done, chunk));
        if (!read.Succeeded()) return read;
        done += chunk;
    }
    return MemoryResult::Ok();
}

MemoryResult GuestMemoryManager::WriteDirect(GuestPhysicalAddress address,
                                             std::span<const std::byte> source) {
    if (source.empty()) return MemoryResult::Ok();
    if (!SpanFitsModel(address.Value(), source.size())) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Direct guest-physical write span exceeds the 48-bit model");
    }
    std::lock_guard lock(mutex_);
    std::size_t done = 0;
    while (done < source.size()) {
        const GuestPhysicalAddress current{address.Value() + static_cast<std::uint64_t>(done)};
        const GuestPhysicalTranslation translation =
            TranslateLocked(current, false, Access::Write);
        if (!translation.Succeeded()) {
            return MemoryResult::Error(translation.status, translation.message);
        }
        const std::uint64_t page_offset = current.Value() & (kSmallPageSize - 1ULL);
        const std::size_t remaining = source.size() - done;
        const std::size_t chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(kSmallPageSize - page_offset));
        const MemoryResult write = backing_.Write(translation.host_physical,
                                                  source.subspan(done, chunk));
        if (!write.Succeeded()) return write;
        done += chunk;
    }
    return MemoryResult::Ok();
}

std::size_t GuestMemoryManager::RegionCount() const {
    std::lock_guard lock(mutex_);
    return regions_by_id_.size();
}

std::size_t GuestMemoryManager::PageCount() const {
    std::lock_guard lock(mutex_);
    std::size_t pages = 0;
    for (const auto& [id, region] : regions_by_id_) {
        (void)id;
        pages += region.info.size / static_cast<std::size_t>(kSmallPageSize);
    }
    return pages;
}

std::size_t GuestMemoryManager::AllocatedBytes() const {
    std::lock_guard lock(mutex_);
    std::size_t bytes = 0;
    for (const auto& [id, region] : regions_by_id_) {
        (void)id;
        bytes += region.info.size;
    }
    return bytes;
}

} // namespace hv::memory
