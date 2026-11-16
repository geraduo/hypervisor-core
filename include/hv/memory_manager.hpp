#pragma once
#include "hv/nested_paging.hpp"
#include "hv/page_table.hpp"
#include "hv/types.hpp"

namespace hv {

struct AddressSpace { AddressSpaceId id{}; bool valid{false}; };

class MemoryManager final {
public:
    MemoryManager(SyntheticPageTable& guest_tables, NestedPageTable& nested_tables)
        : guest_tables_(guest_tables), nested_tables_(nested_tables) {}
    [[nodiscard]] TranslationResult Translate(const AddressSpace& context,
                                               GuestVirtualAddress address, Access access) const;
private:
    SyntheticPageTable& guest_tables_;
    NestedPageTable& nested_tables_;
};

} // namespace hv
