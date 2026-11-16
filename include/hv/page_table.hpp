#pragma once
#include "hv/status.hpp"
#include "hv/types.hpp"
#include <map>
#include <mutex>

namespace hv {

struct GuestPageMapping {
    GuestPhysicalAddress guest_page{};
    Permissions permissions{};
};

class SyntheticPageTable final {
public:
    [[nodiscard]] Result Map(GuestVirtualAddress virtual_page,
                             GuestPhysicalAddress guest_page,
                             Permissions permissions = {});
    [[nodiscard]] Result Unmap(GuestVirtualAddress virtual_page);
    [[nodiscard]] TranslationStatus Translate(GuestVirtualAddress address, Access access,
                                               GuestPhysicalAddress& out) const;
    [[nodiscard]] std::size_t MappingCount() const;

private:
    mutable std::mutex mutex_;
    std::map<GuestVirtualAddress, GuestPageMapping> mappings_;
};

} // namespace hv
