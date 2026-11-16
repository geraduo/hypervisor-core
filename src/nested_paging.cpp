#include "hv/nested_paging.hpp"

namespace hv {

Result NestedPageTable::Map(GuestPhysicalAddress guest_page, HostPhysicalAddress host_page,
                            Permissions permissions) {
    if ((guest_page & (kPageSize - 1)) != 0 || (host_page & (kPageSize - 1)) != 0)
        return Result::Error(Status::InvalidArgument, "Nested mappings must be page aligned");
    std::lock_guard lock(mutex_);
    if (mappings_.contains(guest_page))
        return Result::Error(Status::AlreadyMapped, "Guest physical page is already mapped");
    mappings_.emplace(guest_page, Mapping{host_page, permissions});
    return Result::Ok();
}

Result NestedPageTable::Unmap(GuestPhysicalAddress guest_page) {
    if ((guest_page & (kPageSize - 1)) != 0)
        return Result::Error(Status::InvalidArgument, "Guest physical page must be aligned");
    std::lock_guard lock(mutex_);
    if (mappings_.erase(guest_page) == 0)
        return Result::Error(Status::NotMapped, "Guest physical page is not mapped");
    return Result::Ok();
}

TranslationStatus NestedPageTable::Translate(GuestPhysicalAddress address, Access access,
                                              HostPhysicalAddress& out) const {
    const auto page = address & kPageMask;
    const auto offset = address & (kPageSize - 1);
    std::lock_guard lock(mutex_);
    const auto it = mappings_.find(page);
    if (it == mappings_.end()) return TranslationStatus::UnmappedGuestPhysical;
    if (!it->second.permissions.Allows(access)) return TranslationStatus::AccessDenied;
    out = it->second.host_page + offset;
    return TranslationStatus::Success;
}

std::size_t NestedPageTable::MappingCount() const {
    std::lock_guard lock(mutex_);
    return mappings_.size();
}

} // namespace hv
