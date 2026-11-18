#include "hv/memory_manager.hpp"

#include "hv/memory/ept_manager.hpp"

namespace hv {
namespace {

TranslationStatus TranslateEptStatus(memory::MemoryStatus status) noexcept {
    switch (status) {
    case memory::MemoryStatus::Success: return TranslationStatus::Success;
    case memory::MemoryStatus::InvalidContext: return TranslationStatus::InvalidContext;
    case memory::MemoryStatus::AccessDenied: return TranslationStatus::AccessDenied;
    case memory::MemoryStatus::Unmapped: return TranslationStatus::UnmappedGuestPhysical;
    case memory::MemoryStatus::InvalidAddress:
    case memory::MemoryStatus::AlignmentError:
    case memory::MemoryStatus::InvalidMapping:
    case memory::MemoryStatus::InternalError:
    case memory::MemoryStatus::AlreadyMapped:
    case memory::MemoryStatus::OutOfMemory:
    case memory::MemoryStatus::SnapshotNotFound:
    case memory::MemoryStatus::SnapshotCorrupt:
    case memory::MemoryStatus::NotInitialized:
    case memory::MemoryStatus::Unsupported:
        return TranslationStatus::UnmappedGuestPhysical;
    }
    return TranslationStatus::UnmappedGuestPhysical;
}

} // namespace

TranslationResult MemoryManager::Translate(const AddressSpace& context,
                                            GuestVirtualAddress address, Access access) const {
    if (!context.valid || context.id == 0) return {TranslationStatus::InvalidContext, 0, 0};
    GuestPhysicalAddress guest_physical{};
    const auto first = guest_tables_.Translate(address, access, guest_physical);
    if (first != TranslationStatus::Success) return {first, 0, 0};

    if (nested_tables_ != nullptr) {
        HostPhysicalAddress host_physical{};
        const auto second = nested_tables_->Translate(guest_physical, access, host_physical);
        if (second != TranslationStatus::Success) return {second, 0, 0};
        return {TranslationStatus::Success, host_physical, kPageSize};
    }

    if (ept_manager_ == nullptr) return {TranslationStatus::InvalidContext, 0, 0};
    const memory::EptTranslationResult second =
        ept_manager_->Translate(memory::GuestPhysicalAddress{guest_physical}, access);
    if (!second.Succeeded()) {
        return {TranslateEptStatus(second.status), 0, 0};
    }
    return {TranslationStatus::Success, second.host_physical.Value(), second.page_size};
}

} // namespace hv
