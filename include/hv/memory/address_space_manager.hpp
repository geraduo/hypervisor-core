#pragma once

// Guest address-space contexts and their GVA -> GPA page tables.
//
// Every address space owns an independent SyntheticPageTable.  The legacy
// SyntheticPageTable API remains the single implementation of first-stage
// translation; this manager only adds identity, lifecycle and typed access.

#include "hv/memory/address_types.hpp"
#include "hv/page_table.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace hv::memory {

struct AddressSpaceInfo {
    AddressSpaceId id{};
    std::string name;
    bool valid{false};
    std::size_t mapping_count{0};
};

struct AddressSpaceCreationResult {
    MemoryStatus status{MemoryStatus::InvalidContext};
    AddressSpaceId id{};
    std::string message{};

    [[nodiscard]] bool Succeeded() const noexcept { return hv::memory::Succeeded(status); }
};

struct GuestMappingInfo {
    AddressSpaceId address_space{};
    GuestVirtualAddress virtual_page{};
    GuestPhysicalAddress guest_physical_page{};
    Permissions permissions{};
    std::uint64_t page_size{kSmallPageSize};
};

class GuestAddressSpaceManager final {
public:
    GuestAddressSpaceManager() = default;
    ~GuestAddressSpaceManager() = default;

    GuestAddressSpaceManager(const GuestAddressSpaceManager&) = delete;
    GuestAddressSpaceManager& operator=(const GuestAddressSpaceManager&) = delete;

    [[nodiscard]] AddressSpaceCreationResult CreateAddressSpace(std::string name);
    [[nodiscard]] MemoryResult DestroyAddressSpace(AddressSpaceId id);
    [[nodiscard]] MemoryResult GetAddressSpace(AddressSpaceId id, AddressSpaceInfo& out) const;
    [[nodiscard]] std::vector<AddressSpaceInfo> EnumerateAddressSpaces() const;
    [[nodiscard]] std::size_t AddressSpaceCount() const;
    [[nodiscard]] bool Contains(AddressSpaceId id) const;

    [[nodiscard]] MemoryResult Map(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                   GuestPhysicalAddress guest_physical_page,
                                   Permissions permissions = {});
    [[nodiscard]] MemoryResult Unmap(AddressSpaceId id, GuestVirtualAddress virtual_page);
    [[nodiscard]] MemoryResult Protect(AddressSpaceId id, GuestVirtualAddress virtual_page,
                                       Permissions permissions);
    [[nodiscard]] MemoryResult Query(AddressSpaceId id, GuestVirtualAddress address,
                                     GuestMappingInfo& out) const;
    [[nodiscard]] TranslationResult Translate(AddressSpaceId id, GuestVirtualAddress address,
                                              Access access) const;
    [[nodiscard]] std::vector<GuestMappingInfo> EnumerateMappings(AddressSpaceId id) const;
    [[nodiscard]] MemoryResult ClearMappings(AddressSpaceId id);
    [[nodiscard]] std::size_t TotalMappingCount() const;

private:
    struct Entry {
        AddressSpaceId id{};
        std::string name;
        std::unique_ptr<SyntheticPageTable> table;
    };

    [[nodiscard]] Entry* FindLocked(AddressSpaceId id) noexcept;
    [[nodiscard]] const Entry* FindLocked(AddressSpaceId id) const noexcept;
    [[nodiscard]] AddressSpaceId AllocateIdLocked();

    mutable std::mutex mutex_;
    std::map<AddressSpaceId, Entry> spaces_;
    std::uint64_t next_id_{1};
};

} // namespace hv::memory
