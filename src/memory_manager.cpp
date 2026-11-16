#include "hv/memory_manager.hpp"

namespace hv {

TranslationResult MemoryManager::Translate(const AddressSpace& context,
                                            GuestVirtualAddress address, Access access) const {
    if (!context.valid || context.id == 0) return {TranslationStatus::InvalidContext, 0, 0};
    GuestPhysicalAddress guest_physical{};
    const auto first = guest_tables_.Translate(address, access, guest_physical);
    if (first != TranslationStatus::Success) return {first, 0, 0};
    HostPhysicalAddress host_physical{};
    const auto second = nested_tables_.Translate(guest_physical, access, host_physical);
    if (second != TranslationStatus::Success) return {second, 0, 0};
    return {TranslationStatus::Success, host_physical, kPageSize};
}

} // namespace hv
