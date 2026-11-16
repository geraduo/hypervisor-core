#pragma once
#include "hv/status.hpp"
#include "hv/types.hpp"
#include <map>
#include <mutex>

namespace hv {

// Software model of the second-stage GPA -> HPA mapping used conceptually by EPT/NPT.
// It does not program CPU page tables or provide access to physical memory.
class NestedPageTable final {
public:
    [[nodiscard]] Result Map(GuestPhysicalAddress guest_page, HostPhysicalAddress host_page,
                             Permissions permissions = {});
    [[nodiscard]] Result Unmap(GuestPhysicalAddress guest_page);
    [[nodiscard]] TranslationStatus Translate(GuestPhysicalAddress address, Access access,
                                                HostPhysicalAddress& out) const;
    [[nodiscard]] std::size_t MappingCount() const;

private:
    struct Mapping { HostPhysicalAddress host_page{}; Permissions permissions{}; };
    mutable std::mutex mutex_;
    std::map<GuestPhysicalAddress, Mapping> mappings_;
};

using EptManager = NestedPageTable;
using NptManager = NestedPageTable;

} // namespace hv
