#include "hv/page_table.hpp"

namespace hv {

Result SyntheticPageTable::Map(GuestVirtualAddress virtual_page,
                               GuestPhysicalAddress guest_page, Permissions permissions) {
    if ((virtual_page & (kPageSize - 1)) != 0 || (guest_page & (kPageSize - 1)) != 0)
        return Result::Error(Status::InvalidArgument, "Mappings must be page aligned");
    std::lock_guard lock(mutex_);
    if (mappings_.contains(virtual_page))
        return Result::Error(Status::AlreadyMapped, "Guest virtual page is already mapped");
    mappings_.emplace(virtual_page, GuestPageMapping{guest_page, permissions});
    return Result::Ok();
}

Result SyntheticPageTable::Unmap(GuestVirtualAddress virtual_page) {
    if ((virtual_page & (kPageSize - 1)) != 0)
        return Result::Error(Status::InvalidArgument, "Virtual page must be aligned");
    std::lock_guard lock(mutex_);
    if (mappings_.erase(virtual_page) == 0)
        return Result::Error(Status::NotMapped, "Guest virtual page is not mapped");
    return Result::Ok();
}

TranslationStatus SyntheticPageTable::Translate(GuestVirtualAddress address, Access access,
                                                 GuestPhysicalAddress& out) const {
    const auto page = address & kPageMask;
    const auto offset = address & (kPageSize - 1);
    std::lock_guard lock(mutex_);
    const auto it = mappings_.find(page);
    if (it == mappings_.end()) return TranslationStatus::UnmappedGuestVirtual;
    if (!it->second.permissions.Allows(access)) return TranslationStatus::AccessDenied;
    out = it->second.guest_page + offset;
    return TranslationStatus::Success;
}

std::size_t SyntheticPageTable::MappingCount() const {
    std::lock_guard lock(mutex_);
    return mappings_.size();
}

} // namespace hv
