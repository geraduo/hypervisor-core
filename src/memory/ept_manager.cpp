#include "hv/memory/ept_manager.hpp"

#include <limits>

namespace hv::memory {

EptManager::EptManager(EptPageTable::Config config) : table_(config) {}

MemoryResult EptManager::Map(GuestPhysicalAddress guest_page, HostPhysicalAddress host_page,
                             Permissions permissions, std::uint64_t page_size) {
    std::lock_guard lock(mutex_);
    return table_.Map(guest_page, host_page, page_size, permissions);
}

MemoryResult EptManager::MapRange(GuestPhysicalAddress guest_base,
                                  HostPhysicalAddress host_base,
                                  std::size_t page_count,
                                  Permissions permissions,
                                  std::uint64_t page_size) {
    if (page_count == 0) {
        return MemoryResult::Error(MemoryStatus::InvalidMapping,
                                   "EPT range mapping requires at least one page");
    }
    if (!IsSupportedPageSize(page_size)) {
        return MemoryResult::Error(MemoryStatus::InvalidMapping,
                                   "EPT range page size must be 4 KiB, 2 MiB or 1 GiB");
    }
    if (!guest_base.IsAlignedTo(page_size) || !host_base.IsAlignedTo(page_size)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "EPT range bases must be aligned to the page size");
    }
    const std::uint64_t count = static_cast<std::uint64_t>(page_count);
    if (count > std::numeric_limits<std::uint64_t>::max() / page_size) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "EPT range size overflows the 64-bit address model");
    }
    const std::uint64_t total = count * page_size;

    std::lock_guard lock(mutex_);
    if (!RangeFits(guest_base.Value(), total, table_.GetConfig().address_limit)) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "EPT range exceeds the guest-physical address limit");
    }
    if (!RangeFits(host_base.Value(), total, kMaxPhysicalAddress)) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "EPT range exceeds the host-physical address model");
    }

    std::size_t mapped = 0;
    for (std::size_t i = 0; i < page_count; ++i) {
        const std::uint64_t offset = static_cast<std::uint64_t>(i) * page_size;
        const MemoryResult result = table_.Map(
            GuestPhysicalAddress{guest_base.Value() + offset},
            HostPhysicalAddress{host_base.Value() + offset}, page_size, permissions);
        if (!result.Succeeded()) {
            for (std::size_t rollback = 0; rollback < mapped; ++rollback) {
                const std::uint64_t rollback_offset =
                    static_cast<std::uint64_t>(rollback) * page_size;
                (void)table_.Unmap(GuestPhysicalAddress{guest_base.Value() + rollback_offset});
            }
            return result;
        }
        ++mapped;
    }
    return MemoryResult::Ok();
}

MemoryResult EptManager::Unmap(GuestPhysicalAddress guest_page) {
    std::lock_guard lock(mutex_);
    return table_.Unmap(guest_page);
}

MemoryResult EptManager::Protect(GuestPhysicalAddress guest_page, Permissions permissions) {
    std::lock_guard lock(mutex_);
    return table_.Protect(guest_page, permissions);
}

MemoryResult EptManager::Query(GuestPhysicalAddress address, EptMappingInfo& out) const {
    EptEntryInfo entry{};
    const MemoryResult result = table_.Query(address, entry);
    if (!result.Succeeded()) return result;
    out.guest_physical_page = entry.guest_physical_page;
    out.host_physical_page = entry.host_physical_page;
    out.page_size = entry.page_size;
    out.permissions = entry.permissions;
    out.level = entry.level;
    return MemoryResult::Ok();
}

EptTranslationResult EptManager::Translate(GuestPhysicalAddress address, Access access) const {
    return table_.Translate(address, access);
}

MemoryResult EptManager::Validate() const {
    return table_.Validate();
}

std::size_t EptManager::MappingCount() const {
    return table_.MappingCount();
}

std::vector<EptMappingInfo> EptManager::Enumerate() const {
    const std::vector<EptEntryInfo> entries = table_.Enumerate();
    std::vector<EptMappingInfo> result;
    result.reserve(entries.size());
    for (const EptEntryInfo& entry : entries) {
        result.push_back(EptMappingInfo{entry.guest_physical_page, entry.host_physical_page,
                                        entry.page_size, entry.permissions, entry.level});
    }
    return result;
}

std::vector<EptRangeInfo> EptManager::GetMappingRanges() const {
    const std::vector<EptMappingInfo> entries = Enumerate();
    std::vector<EptRangeInfo> ranges;
    for (const EptMappingInfo& entry : entries) {
        if (!ranges.empty()) {
            EptRangeInfo& last = ranges.back();
            const bool contiguous_guest =
                last.guest_base.Value() + last.size == entry.guest_physical_page.Value();
            const bool contiguous_host =
                last.host_base.Value() + last.size == entry.host_physical_page.Value();
            const bool same_permissions =
                last.permissions.read == entry.permissions.read &&
                last.permissions.write == entry.permissions.write &&
                last.permissions.execute == entry.permissions.execute;
            if (contiguous_guest && contiguous_host && same_permissions &&
                last.page_size == entry.page_size) {
                last.size += entry.page_size;
                continue;
            }
        }
        ranges.push_back(EptRangeInfo{entry.guest_physical_page, entry.host_physical_page,
                                      entry.page_size, entry.page_size, entry.permissions});
    }
    return ranges;
}

void EptManager::Clear() {
    std::lock_guard lock(mutex_);
    table_.Clear();
}

EptPageTable::Config EptManager::GetConfig() const {
    return table_.GetConfig();
}

} // namespace hv::memory
