#include "hv/memory/memory_mapping_manager.hpp"

#include "hv/memory/memory_diagnostics.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace hv::memory {
namespace {

bool SpanFitsModel(std::uint64_t address, std::size_t size) noexcept {
    if (size == 0) return true;
    if (address > kMaxPhysicalAddress) return false;
    return static_cast<std::uint64_t>(size - 1) <= kMaxPhysicalAddress - address;
}

Permissions Intersect(Permissions lhs, Permissions rhs) noexcept {
    return Permissions{lhs.read && rhs.read, lhs.write && rhs.write, lhs.execute && rhs.execute};
}

} // namespace

MemoryMappingManager::MemoryMappingManager(GuestAddressSpaceManager& spaces, EptManager& ept,
                                           GuestMemoryManager& memory,
                                           MemoryDiagnostics* diagnostics)
    : spaces_(spaces), ept_(ept), memory_(memory), diagnostics_(diagnostics) {}

void MemoryMappingManager::RecordTranslation(const TranslationResult& result) const {
    if (diagnostics_ == nullptr) return;
    diagnostics_->RecordTranslation(result.status, result.status == MemoryStatus::AccessDenied);
}

void MemoryMappingManager::UpdateGaugesLocked() const {
    if (diagnostics_ == nullptr) return;
    diagnostics_->SetMappedPages(static_cast<std::uint64_t>(ept_.MappingCount() +
                                                           spaces_.TotalMappingCount()));
    diagnostics_->SetAddressSpaces(static_cast<std::uint64_t>(spaces_.AddressSpaceCount()));
    diagnostics_->SetGuestRegions(static_cast<std::uint64_t>(memory_.RegionCount()));
    diagnostics_->SetAllocatedBackingBytes(
        static_cast<std::uint64_t>(memory_.AllocatedBytes()));
}

MemoryResult MemoryMappingManager::Map(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                       GuestPhysicalAddress guest_physical_page,
                                       Permissions permissions) {
    std::lock_guard lock(mutex_);
    const MemoryResult result =
        spaces_.Map(id, virtual_page, guest_physical_page, permissions);
    if (diagnostics_ != nullptr) diagnostics_->RecordMapping(result.status);
    if (result.Succeeded()) UpdateGaugesLocked();
    return result;
}

MemoryResult MemoryMappingManager::Unmap(AddressSpaceId id, GuestVirtualAddress virtual_page) {
    std::lock_guard lock(mutex_);
    const MemoryResult result = spaces_.Unmap(id, virtual_page);
    if (diagnostics_ != nullptr) diagnostics_->RecordUnmapping(result.status);
    if (result.Succeeded()) UpdateGaugesLocked();
    return result;
}

MemoryResult MemoryMappingManager::Protect(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                           Permissions permissions) {
    std::lock_guard lock(mutex_);
    const MemoryResult result = spaces_.Protect(id, virtual_page, permissions);
    if (diagnostics_ != nullptr) diagnostics_->RecordProtection(result.status);
    return result;
}

MemoryResult MemoryMappingManager::Query(AddressSpaceId id, GuestVirtualAddress address,
                                         MappingInfo& out) const {
    std::lock_guard lock(mutex_);
    GuestMappingInfo guest{};
    const MemoryResult guest_result = spaces_.Query(id, address, guest);
    if (!guest_result.Succeeded()) return guest_result;

    out = MappingInfo{};
    out.address_space = id;
    out.virtual_address = guest.virtual_page;
    out.guest_physical_address = guest.guest_physical_page;
    out.page_size = guest.page_size;
    out.permissions = guest.permissions;
    out.guest_mapped = true;

    GuestPageInfo page{};
    if (memory_.QueryPage(guest.guest_physical_page, page).Succeeded()) {
        out.backed = true;
        out.region_name = page.region_name;
        out.host_physical_address = page.host_physical;
    }

    EptMappingInfo nested{};
    if (ept_.Query(guest.guest_physical_page, nested).Succeeded()) {
        out.nested_mapped = true;
        out.host_physical_address = nested.host_physical_page;
        out.page_size = std::min(guest.page_size, nested.page_size);
        out.permissions = Intersect(out.permissions, nested.permissions);
    }
    return MemoryResult::Ok();
}

MemoryResult MemoryMappingManager::GetMappingInfo(AddressSpaceId id,
                                                  GuestVirtualAddress address,
                                                  MappingInfo& out) const {
    return Query(id, address, out);
}

MemoryResult MemoryMappingManager::MapNested(GuestPhysicalAddress guest_page,
                                             HostPhysicalAddress host_page,
                                             Permissions permissions,
                                             std::uint64_t page_size) {
    std::lock_guard lock(mutex_);
    const MemoryResult result = ept_.Map(guest_page, host_page, permissions, page_size);
    if (diagnostics_ != nullptr) diagnostics_->RecordMapping(result.status);
    if (result.Succeeded()) UpdateGaugesLocked();
    return result;
}

MemoryResult MemoryMappingManager::UnmapNested(GuestPhysicalAddress guest_page) {
    std::lock_guard lock(mutex_);
    const MemoryResult result = ept_.Unmap(guest_page);
    if (diagnostics_ != nullptr) diagnostics_->RecordUnmapping(result.status);
    if (result.Succeeded()) UpdateGaugesLocked();
    return result;
}

MemoryResult MemoryMappingManager::ProtectNested(GuestPhysicalAddress guest_page,
                                                 Permissions permissions) {
    std::lock_guard lock(mutex_);
    const MemoryResult result = ept_.Protect(guest_page, permissions);
    if (diagnostics_ != nullptr) diagnostics_->RecordProtection(result.status);
    return result;
}

MemoryResult MemoryMappingManager::QueryNested(GuestPhysicalAddress address,
                                               MappingInfo& out) const {
    std::lock_guard lock(mutex_);
    EptMappingInfo nested{};
    const MemoryResult nested_result = ept_.Query(address, nested);
    if (!nested_result.Succeeded()) return nested_result;

    out = MappingInfo{};
    out.guest_physical_address = nested.guest_physical_page;
    out.host_physical_address = nested.host_physical_page;
    out.page_size = nested.page_size;
    out.permissions = nested.permissions;
    out.nested_mapped = true;

    GuestPageInfo page{};
    if (memory_.QueryPage(nested.guest_physical_page, page).Succeeded()) {
        out.backed = true;
        out.region_name = page.region_name;
    }
    return MemoryResult::Ok();
}

MemoryResult MemoryMappingManager::MapAll(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                          GuestPhysicalAddress guest_physical_page,
                                          Permissions permissions) {
    if (!virtual_page.IsAlignedTo(kSmallPageSize) ||
        !guest_physical_page.IsAlignedTo(kSmallPageSize)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Combined mappings must be 4 KiB aligned");
    }

    std::lock_guard lock(mutex_);
    GuestPageInfo page{};
    const MemoryResult page_result = memory_.QueryPage(guest_physical_page, page);
    if (!page_result.Succeeded()) return page_result;

    GuestMappingInfo existing{};
    const MemoryResult existing_result = spaces_.Query(id, virtual_page, existing);
    if (existing_result.Succeeded()) {
        return MemoryResult::Error(MemoryStatus::AlreadyMapped,
                                   "Guest virtual page is already mapped");
    }
    if (existing_result.status != MemoryStatus::Unmapped) return existing_result;

    bool created_nested = false;
    EptMappingInfo nested{};
    const MemoryResult nested_result = ept_.Query(guest_physical_page, nested);
    if (nested_result.Succeeded()) {
        if (nested.host_physical_page.Value() != page.host_physical.Value()) {
            return MemoryResult::Error(
                MemoryStatus::InvalidMapping,
                "Existing EPT mapping does not point at the region's backing memory");
        }
    } else if (nested_result.status == MemoryStatus::Unmapped) {
        const MemoryResult mapped = ept_.Map(guest_physical_page, page.host_physical,
                                             permissions, kSmallPageSize);
        if (!mapped.Succeeded()) return mapped;
        created_nested = true;
    } else {
        return nested_result;
    }

    const MemoryResult guest_result =
        spaces_.Map(id, virtual_page, guest_physical_page, permissions);
    if (!guest_result.Succeeded()) {
        if (created_nested) (void)ept_.Unmap(guest_physical_page);
        if (diagnostics_ != nullptr) diagnostics_->RecordMapping(guest_result.status);
        return guest_result;
    }

    if (diagnostics_ != nullptr) diagnostics_->RecordMapping(MemoryStatus::Success);
    UpdateGaugesLocked();
    return MemoryResult::Ok();
}

MemoryResult MemoryMappingManager::UnmapAll(AddressSpaceId id,
                                            GuestVirtualAddress virtual_page) {
    std::lock_guard lock(mutex_);
    GuestMappingInfo guest{};
    const MemoryResult found = spaces_.Query(id, virtual_page, guest);
    if (!found.Succeeded()) return found;

    const MemoryResult guest_unmapped = spaces_.Unmap(id, guest.virtual_page);
    if (diagnostics_ != nullptr) diagnostics_->RecordUnmapping(guest_unmapped.status);
    if (!guest_unmapped.Succeeded()) return guest_unmapped;

    const MemoryResult nested_unmapped = ept_.Unmap(guest.guest_physical_page);
    if (diagnostics_ != nullptr) diagnostics_->RecordUnmapping(nested_unmapped.status);
    UpdateGaugesLocked();
    if (!nested_unmapped.Succeeded() && nested_unmapped.status != MemoryStatus::Unmapped) {
        return nested_unmapped;
    }
    return MemoryResult::Ok();
}

MemoryResult MemoryMappingManager::ProtectAll(AddressSpaceId id,
                                              GuestVirtualAddress virtual_page,
                                              Permissions permissions) {
    std::lock_guard lock(mutex_);
    GuestMappingInfo guest{};
    const MemoryResult found = spaces_.Query(id, virtual_page, guest);
    if (!found.Succeeded()) return found;

    const MemoryResult guest_protected = spaces_.Protect(id, guest.virtual_page, permissions);
    if (diagnostics_ != nullptr) diagnostics_->RecordProtection(guest_protected.status);
    if (!guest_protected.Succeeded()) return guest_protected;

    const MemoryResult nested_protected =
        ept_.Protect(guest.guest_physical_page, permissions);
    if (diagnostics_ != nullptr) diagnostics_->RecordProtection(nested_protected.status);
    UpdateGaugesLocked();
    if (!nested_protected.Succeeded() &&
        nested_protected.status != MemoryStatus::Unmapped) {
        return nested_protected;
    }
    return MemoryResult::Ok();
}

TranslationResult MemoryMappingManager::TranslateLocked(AddressSpaceId id,
                                                        GuestVirtualAddress address,
                                                        Access access) const {
    TranslationResult result{};
    result.guest_virtual = address;

    TranslationResult first = spaces_.Translate(id, address, access);
    if (!first.Succeeded()) {
        result.status = first.status;
        result.message = std::move(first.message);
        RecordTranslation(result);
        return result;
    }
    result.guest_physical = first.guest_physical;
    result.guest_translated = true;
    result.permissions = first.permissions;

    GuestPhysicalTranslation second = memory_.Translate(first.guest_physical, access);
    if (!second.Succeeded()) {
        result.status = second.status;
        result.message = std::move(second.message);
        RecordTranslation(result);
        return result;
    }

    EptTranslationResult third = ept_.Translate(first.guest_physical, access);
    if (!third.Succeeded()) {
        result.status = third.status;
        result.message = std::move(third.message);
        RecordTranslation(result);
        return result;
    }

    if (third.host_physical.Value() != second.host_physical.Value()) {
        result.status = MemoryStatus::InvalidMapping;
        result.message = "EPT translation disagrees with the registered guest region backing";
        RecordTranslation(result);
        return result;
    }

    result.host_physical = third.host_physical;
    result.page_size = std::min(first.page_size, third.page_size);
    result.permissions = Intersect(result.permissions, third.permissions);
    result.nested_translated = true;
    result.status = MemoryStatus::Success;
    RecordTranslation(result);
    return result;
}

TranslationResult MemoryMappingManager::Translate(AddressSpaceId id, GuestVirtualAddress address,
                                                  Access access) const {
    std::lock_guard lock(mutex_);
    return TranslateLocked(id, address, access);
}

TranslationResult MemoryMappingManager::TranslatePhysicalLocked(GuestPhysicalAddress address,
                                                                Access access) const {
    TranslationResult result{};
    result.guest_physical = address;

    GuestPhysicalTranslation second = memory_.Translate(address, access);
    if (!second.Succeeded()) {
        result.status = second.status;
        result.message = std::move(second.message);
        RecordTranslation(result);
        return result;
    }
    result.permissions = second.permissions;

    EptTranslationResult third = ept_.Translate(address, access);
    if (!third.Succeeded()) {
        result.status = third.status;
        result.message = std::move(third.message);
        RecordTranslation(result);
        return result;
    }
    if (third.host_physical.Value() != second.host_physical.Value()) {
        result.status = MemoryStatus::InvalidMapping;
        result.message = "EPT translation disagrees with the registered guest region backing";
        RecordTranslation(result);
        return result;
    }

    result.host_physical = third.host_physical;
    result.page_size = third.page_size;
    result.permissions = Intersect(result.permissions, third.permissions);
    result.nested_translated = true;
    result.status = MemoryStatus::Success;
    RecordTranslation(result);
    return result;
}

TranslationResult MemoryMappingManager::TranslatePhysical(GuestPhysicalAddress address,
                                                          Access access) const {
    std::lock_guard lock(mutex_);
    return TranslatePhysicalLocked(address, access);
}

MemoryResult MemoryMappingManager::Read(AddressSpaceId id, GuestVirtualAddress address,
                                        std::span<std::byte> destination) {
    if (destination.empty()) return MemoryResult::Ok();
    if (!SpanFitsModel(address.Value(), destination.size())) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Guest virtual read span exceeds the 48-bit model");
    }
    std::lock_guard lock(mutex_);
    std::size_t done = 0;
    while (done < destination.size()) {
        const GuestVirtualAddress current{address.Value() + static_cast<std::uint64_t>(done)};
        const TranslationResult translation = TranslateLocked(id, current, Access::Read);
        if (!translation.Succeeded()) {
            return MemoryResult::Error(translation.status, translation.message);
        }
        if (translation.page_size == 0) {
            return MemoryResult::Error(MemoryStatus::InternalError,
                                       "Successful translation returned a zero page size");
        }
        const std::uint64_t page_offset = current.Value() & (translation.page_size - 1ULL);
        const std::size_t remaining = destination.size() - done;
        const std::size_t chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(translation.page_size - page_offset));
        const MemoryResult read =
            memory_.Backing().Read(translation.host_physical, destination.subspan(done, chunk));
        if (!read.Succeeded()) return read;
        done += chunk;
    }
    return MemoryResult::Ok();
}

MemoryResult MemoryMappingManager::Write(AddressSpaceId id, GuestVirtualAddress address,
                                         std::span<const std::byte> source) {
    if (source.empty()) return MemoryResult::Ok();
    if (!SpanFitsModel(address.Value(), source.size())) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Guest virtual write span exceeds the 48-bit model");
    }
    std::lock_guard lock(mutex_);
    std::size_t done = 0;
    while (done < source.size()) {
        const GuestVirtualAddress current{address.Value() + static_cast<std::uint64_t>(done)};
        const TranslationResult translation = TranslateLocked(id, current, Access::Write);
        if (!translation.Succeeded()) {
            return MemoryResult::Error(translation.status, translation.message);
        }
        if (translation.page_size == 0) {
            return MemoryResult::Error(MemoryStatus::InternalError,
                                       "Successful translation returned a zero page size");
        }
        const std::uint64_t page_offset = current.Value() & (translation.page_size - 1ULL);
        const std::size_t remaining = source.size() - done;
        const std::size_t chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(translation.page_size - page_offset));
        const MemoryResult write =
            memory_.Backing().Write(translation.host_physical, source.subspan(done, chunk));
        if (!write.Succeeded()) return write;
        done += chunk;
    }
    return MemoryResult::Ok();
}

MemoryResult MemoryMappingManager::Execute(AddressSpaceId id, GuestVirtualAddress address) {
    const TranslationResult translation = Translate(id, address, Access::Execute);
    if (!translation.Succeeded()) {
        return MemoryResult::Error(translation.status, translation.message);
    }
    return MemoryResult::Ok();
}

MemoryResult MemoryMappingManager::ValidateLocked(AddressSpaceId id) const {
    AddressSpaceInfo space{};
    const MemoryResult found = spaces_.GetAddressSpace(id, space);
    if (!found.Succeeded()) return found;

    std::vector<std::string> issues;
    for (const GuestMappingInfo& mapping : spaces_.EnumerateMappings(id)) {
        GuestPageInfo page{};
        const MemoryResult page_result =
            memory_.QueryPage(mapping.guest_physical_page, page);
        if (!page_result.Succeeded()) {
            issues.push_back("GVA " + FormatAddress(mapping.virtual_page.Value()) +
                             " maps to unbacked GPA " +
                             FormatAddress(mapping.guest_physical_page.Value()));
            continue;
        }
        EptMappingInfo nested{};
        const MemoryResult nested_result = ept_.Query(mapping.guest_physical_page, nested);
        if (!nested_result.Succeeded()) {
            issues.push_back("GPA " + FormatAddress(mapping.guest_physical_page.Value()) +
                             " has no EPT mapping");
            continue;
        }
        if (nested.host_physical_page.Value() != page.host_physical.Value()) {
            issues.push_back("GPA " + FormatAddress(mapping.guest_physical_page.Value()) +
                             " EPT target does not match its backing region");
        }
    }

    const MemoryResult ept_validation = ept_.Validate();
    if (!ept_validation.Succeeded()) {
        issues.push_back("EPT table: " + ept_validation.message);
    }

    if (!issues.empty()) {
        std::ostringstream message;
        message << "Memory mapping validation failed for address space #" << id.Value();
        for (const std::string& issue : issues) {
            message << "; " << issue;
        }
        return MemoryResult::Error(MemoryStatus::InvalidMapping, message.str());
    }
    return MemoryResult::Ok();
}

MemoryResult MemoryMappingManager::ValidateAllLocked() const {
    std::vector<std::string> issues;
    for (const AddressSpaceInfo& space : spaces_.EnumerateAddressSpaces()) {
        const MemoryResult validation = ValidateLocked(space.id);
        if (!validation.Succeeded()) issues.push_back(validation.message);
    }
    const MemoryResult ept_validation = ept_.Validate();
    if (!ept_validation.Succeeded()) issues.push_back("EPT table: " + ept_validation.message);
    if (!issues.empty()) {
        std::ostringstream message;
        message << "Memory mapping validation failed";
        for (const std::string& issue : issues) {
            message << "; " << issue;
        }
        return MemoryResult::Error(MemoryStatus::InvalidMapping, message.str());
    }
    return MemoryResult::Ok();
}

MemoryResult MemoryMappingManager::Validate(AddressSpaceId id) const {
    std::lock_guard lock(mutex_);
    return ValidateLocked(id);
}

MemoryResult MemoryMappingManager::ValidateAll() const {
    std::lock_guard lock(mutex_);
    return ValidateAllLocked();
}

} // namespace hv::memory
