#pragma once
#include "hv/nested_paging.hpp"
#include "hv/page_table.hpp"
#include "hv/types.hpp"

namespace hv {

namespace memory {
class EptManager;
} // namespace memory

struct AddressSpace { AddressSpaceId id{}; bool valid{false}; };

class MemoryManager final {
public:
    MemoryManager(SyntheticPageTable& guest_tables, NestedPageTable& nested_tables)
        : guest_tables_(guest_tables), nested_tables_(&nested_tables) {}

    // v0.6.0 integration constructor.  The first stage still uses the existing
    // SyntheticPageTable; the second stage uses the typed software EPT model.
    MemoryManager(SyntheticPageTable& guest_tables, memory::EptManager& ept_manager)
        : guest_tables_(guest_tables), ept_manager_(&ept_manager) {}

    [[nodiscard]] TranslationResult Translate(const AddressSpace& context,
                                               GuestVirtualAddress address, Access access) const;

private:
    SyntheticPageTable& guest_tables_;
    NestedPageTable* nested_tables_{nullptr};
    memory::EptManager* ept_manager_{nullptr};
};

} // namespace hv
