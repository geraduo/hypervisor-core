#include "hv/memory/address_space_manager.hpp"

#include <limits>
#include <utility>

namespace hv::memory {
namespace {

MemoryStatus TranslateLegacyStatus(::hv::Status status) noexcept {
    switch (status) {
    case ::hv::Status::Success: return MemoryStatus::Success;
    case ::hv::Status::InvalidArgument: return MemoryStatus::AlignmentError;
    case ::hv::Status::AlreadyMapped: return MemoryStatus::AlreadyMapped;
    case ::hv::Status::NotMapped: return MemoryStatus::Unmapped;
    case ::hv::Status::NotFound: return MemoryStatus::Unmapped;
    case ::hv::Status::AccessDenied: return MemoryStatus::AccessDenied;
    case ::hv::Status::Unsupported: return MemoryStatus::Unsupported;
    case ::hv::Status::InvalidState: return MemoryStatus::InvalidContext;
    case ::hv::Status::Timeout:
    case ::hv::Status::InitializationFailed:
    case ::hv::Status::ShutdownFailed:
    case ::hv::Status::InternalError: return MemoryStatus::InternalError;
    }
    return MemoryStatus::InternalError;
}

MemoryResult FromLegacyResult(const ::hv::Result& result) {
    if (result.Succeeded()) return MemoryResult::Ok();
    return MemoryResult::Error(TranslateLegacyStatus(result.status), result.message);
}

} // namespace

GuestAddressSpaceManager::Entry* GuestAddressSpaceManager::FindLocked(AddressSpaceId id) noexcept {
    const auto it = spaces_.find(id);
    return it == spaces_.end() ? nullptr : &it->second;
}

const GuestAddressSpaceManager::Entry* GuestAddressSpaceManager::FindLocked(
    AddressSpaceId id) const noexcept {
    const auto it = spaces_.find(id);
    return it == spaces_.end() ? nullptr : &it->second;
}

AddressSpaceId GuestAddressSpaceManager::AllocateIdLocked() {
    if (next_id_ == 0) return AddressSpaceId{};
    AddressSpaceId candidate{next_id_};
    while (spaces_.contains(candidate)) {
        if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
            next_id_ = 0;
            return AddressSpaceId{};
        }
        ++next_id_;
        candidate = AddressSpaceId{next_id_};
    }
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        next_id_ = 0;
    } else {
        ++next_id_;
    }
    return candidate;
}

AddressSpaceCreationResult GuestAddressSpaceManager::CreateAddressSpace(std::string name) {
    if (name.empty() || name.size() > 128) {
        return AddressSpaceCreationResult{
            MemoryStatus::InvalidContext, {},
            "Address-space name must contain between 1 and 128 characters"};
    }

    std::lock_guard lock(mutex_);
    for (const auto& [id, entry] : spaces_) {
        (void)id;
        if (entry.name == name) {
            return AddressSpaceCreationResult{
                MemoryStatus::InvalidMapping, {},
                "An address space with this name already exists"};
        }
    }

    const AddressSpaceId id = AllocateIdLocked();
    if (!id.IsValid()) {
        return AddressSpaceCreationResult{MemoryStatus::InternalError, {},
                                          "Address-space identifier space is exhausted"};
    }

    auto table = std::make_unique<SyntheticPageTable>();
    Entry entry{id, std::move(name), std::move(table)};
    spaces_.emplace(id, std::move(entry));
    return AddressSpaceCreationResult{MemoryStatus::Success, id, {}};
}

MemoryResult GuestAddressSpaceManager::DestroyAddressSpace(AddressSpaceId id) {
    std::lock_guard lock(mutex_);
    const auto it = spaces_.find(id);
    if (it == spaces_.end()) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Address space does not exist");
    }
    spaces_.erase(it);
    return MemoryResult::Ok();
}

MemoryResult GuestAddressSpaceManager::GetAddressSpace(AddressSpaceId id,
                                                       AddressSpaceInfo& out) const {
    std::lock_guard lock(mutex_);
    const Entry* entry = FindLocked(id);
    if (entry == nullptr) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Address space does not exist");
    }
    out.id = entry->id;
    out.name = entry->name;
    out.valid = true;
    out.mapping_count = entry->table->MappingCount();
    return MemoryResult::Ok();
}

std::vector<AddressSpaceInfo> GuestAddressSpaceManager::EnumerateAddressSpaces() const {
    std::lock_guard lock(mutex_);
    std::vector<AddressSpaceInfo> result;
    result.reserve(spaces_.size());
    for (const auto& [id, entry] : spaces_) {
        result.push_back(AddressSpaceInfo{id, entry.name, true, entry.table->MappingCount()});
    }
    return result;
}

std::size_t GuestAddressSpaceManager::AddressSpaceCount() const {
    std::lock_guard lock(mutex_);
    return spaces_.size();
}

bool GuestAddressSpaceManager::Contains(AddressSpaceId id) const {
    std::lock_guard lock(mutex_);
    return spaces_.contains(id);
}

MemoryResult GuestAddressSpaceManager::Map(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                           GuestPhysicalAddress guest_physical_page,
                                           Permissions permissions) {
    if (!virtual_page.IsAlignedTo(kSmallPageSize) ||
        !guest_physical_page.IsAlignedTo(kSmallPageSize)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Guest virtual and physical mappings must be 4 KiB aligned");
    }
    if (!IsCanonicalPhysicalAddress(guest_physical_page.Value())) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Guest-physical address exceeds the 48-bit model");
    }
    std::lock_guard lock(mutex_);
    Entry* entry = FindLocked(id);
    if (entry == nullptr) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Address space does not exist");
    }
    return FromLegacyResult(entry->table->Map(ToLegacy(virtual_page),
                                              ToLegacy(guest_physical_page), permissions));
}

MemoryResult GuestAddressSpaceManager::Unmap(AddressSpaceId id, GuestVirtualAddress virtual_page) {
    if (!virtual_page.IsAlignedTo(kSmallPageSize)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Guest virtual page must be 4 KiB aligned");
    }
    std::lock_guard lock(mutex_);
    Entry* entry = FindLocked(id);
    if (entry == nullptr) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Address space does not exist");
    }
    return FromLegacyResult(entry->table->Unmap(ToLegacy(virtual_page)));
}

MemoryResult GuestAddressSpaceManager::Protect(AddressSpaceId id,
                                               GuestVirtualAddress virtual_page,
                                               Permissions permissions) {
    if (!virtual_page.IsAlignedTo(kSmallPageSize)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Guest virtual page must be 4 KiB aligned");
    }
    std::lock_guard lock(mutex_);
    Entry* entry = FindLocked(id);
    if (entry == nullptr) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Address space does not exist");
    }
    return FromLegacyResult(entry->table->Protect(ToLegacy(virtual_page), permissions));
}

MemoryResult GuestAddressSpaceManager::Query(AddressSpaceId id, GuestVirtualAddress address,
                                             GuestMappingInfo& out) const {
    if (!IsCanonicalPhysicalAddress(address.Value())) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Guest virtual address exceeds the 48-bit model");
    }
    std::lock_guard lock(mutex_);
    const Entry* entry = FindLocked(id);
    if (entry == nullptr) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Address space does not exist");
    }
    const GuestVirtualAddress page =
        GuestVirtualAddress{address.AlignDown(kSmallPageSize)};
    ::hv::GuestPageMapping mapping{};
    const MemoryResult result =
        FromLegacyResult(entry->table->Query(ToLegacy(page), mapping));
    if (!result.Succeeded()) return result;
    out.address_space = id;
    out.virtual_page = page;
    out.guest_physical_page = GuestPhysicalFromLegacy(mapping.guest_page);
    out.permissions = mapping.permissions;
    out.page_size = kSmallPageSize;
    return MemoryResult::Ok();
}

TranslationResult GuestAddressSpaceManager::Translate(AddressSpaceId id,
                                                      GuestVirtualAddress address,
                                                      Access access) const {
    TranslationResult result{};
    result.guest_virtual = address;
    if (!IsCanonicalPhysicalAddress(address.Value())) {
        result.status = MemoryStatus::InvalidAddress;
        result.message = "Guest virtual address exceeds the 48-bit model";
        return result;
    }
    std::lock_guard lock(mutex_);
    const Entry* entry = FindLocked(id);
    if (entry == nullptr) {
        result.status = MemoryStatus::InvalidContext;
        result.message = "Address space does not exist";
        return result;
    }
    ::hv::GuestPhysicalAddress physical{};
    const ::hv::TranslationStatus status =
        entry->table->Translate(ToLegacy(address), access, physical);
    switch (status) {
    case ::hv::TranslationStatus::Success:
        result.status = MemoryStatus::Success;
        result.guest_physical = GuestPhysicalFromLegacy(physical);
        result.page_size = kSmallPageSize;
        result.guest_translated = true;
        break;
    case ::hv::TranslationStatus::InvalidContext:
        result.status = MemoryStatus::InvalidContext;
        result.message = "Guest page table reported an invalid context";
        return result;
    case ::hv::TranslationStatus::UnmappedGuestVirtual:
        result.status = MemoryStatus::Unmapped;
        result.message = "Guest virtual page is not mapped";
        return result;
    case ::hv::TranslationStatus::UnmappedGuestPhysical:
        result.status = MemoryStatus::Unmapped;
        result.message = "Guest physical page is not mapped";
        return result;
    case ::hv::TranslationStatus::AccessDenied:
        result.status = MemoryStatus::AccessDenied;
        result.message = "Guest page-table permissions deny this access";
        return result;
    }

    const GuestVirtualAddress page =
        GuestVirtualAddress{address.AlignDown(kSmallPageSize)};
    ::hv::GuestPageMapping mapping{};
    if (entry->table->Query(ToLegacy(page), mapping).Succeeded()) {
        result.permissions = mapping.permissions;
    }
    return result;
}

std::vector<GuestMappingInfo> GuestAddressSpaceManager::EnumerateMappings(
    AddressSpaceId id) const {
    std::lock_guard lock(mutex_);
    const Entry* entry = FindLocked(id);
    if (entry == nullptr) return {};
    const std::vector<::hv::GuestPageMappingInfo> raw = entry->table->Enumerate();
    std::vector<GuestMappingInfo> result;
    result.reserve(raw.size());
    for (const ::hv::GuestPageMappingInfo& mapping : raw) {
        result.push_back(GuestMappingInfo{id, GuestVirtualFromLegacy(mapping.virtual_page),
                                          GuestPhysicalFromLegacy(mapping.guest_page),
                                          mapping.permissions,
                                          kSmallPageSize});
    }
    return result;
}

MemoryResult GuestAddressSpaceManager::ClearMappings(AddressSpaceId id) {
    std::lock_guard lock(mutex_);
    Entry* entry = FindLocked(id);
    if (entry == nullptr) {
        return MemoryResult::Error(MemoryStatus::InvalidContext,
                                   "Address space does not exist");
    }
    entry->table->Clear();
    return MemoryResult::Ok();
}

std::size_t GuestAddressSpaceManager::TotalMappingCount() const {
    std::lock_guard lock(mutex_);
    std::size_t total = 0;
    for (const auto& [id, entry] : spaces_) {
        (void)id;
        total += entry.table->MappingCount();
    }
    return total;
}

} // namespace hv::memory
