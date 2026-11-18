#pragma once

// High-level manager for the software EPT mapping model.
//
// The manager adds conflict detection, range operations, permission changes,
// queries and range enumeration on top of EptPageTable.  It does not touch
// hardware EPT structures.

#include "hv/memory/address_types.hpp"
#include "hv/memory/ept_page_table.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hv::memory {

struct EptMappingInfo {
    GuestPhysicalAddress guest_physical_page{};
    HostPhysicalAddress host_physical_page{};
    std::uint64_t page_size{kSmallPageSize};
    Permissions permissions{};
    EptEntryLevel level{EptEntryLevel::Pt};
};

struct EptRangeInfo {
    GuestPhysicalAddress guest_base{};
    HostPhysicalAddress host_base{};
    std::uint64_t size{0};
    std::uint64_t page_size{kSmallPageSize};
    Permissions permissions{};
};

class EptManager final {
public:
    EptManager() = default;
    explicit EptManager(EptPageTable::Config config);

    EptManager(const EptManager&) = delete;
    EptManager& operator=(const EptManager&) = delete;

    [[nodiscard]] MemoryResult Map(GuestPhysicalAddress guest_page,
                                   HostPhysicalAddress host_page,
                                   Permissions permissions = {},
                                   std::uint64_t page_size = kSmallPageSize);
    // Maps page_count consecutive pages.  Partial failures are rolled back.
    [[nodiscard]] MemoryResult MapRange(GuestPhysicalAddress guest_base,
                                        HostPhysicalAddress host_base,
                                        std::size_t page_count,
                                        Permissions permissions = {},
                                        std::uint64_t page_size = kSmallPageSize);

    [[nodiscard]] MemoryResult Unmap(GuestPhysicalAddress guest_page);
    [[nodiscard]] MemoryResult Protect(GuestPhysicalAddress guest_page, Permissions permissions);
    [[nodiscard]] MemoryResult Query(GuestPhysicalAddress address, EptMappingInfo& out) const;
    [[nodiscard]] EptTranslationResult Translate(GuestPhysicalAddress address,
                                                 Access access) const;

    [[nodiscard]] MemoryResult Validate() const;
    [[nodiscard]] std::size_t MappingCount() const;
    [[nodiscard]] std::vector<EptMappingInfo> Enumerate() const;
    [[nodiscard]] std::vector<EptRangeInfo> GetMappingRanges() const;
    void Clear();
    [[nodiscard]] EptPageTable::Config GetConfig() const;

private:
    mutable std::mutex mutex_;
    EptPageTable table_;
};

} // namespace hv::memory
