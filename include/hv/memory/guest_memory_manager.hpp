#pragma once

// Guest-physical memory regions and their real backing allocations.
//
// GuestMemoryManager registers guest-physical ranges, allocates genuine
// backing storage through IMemoryBackingProvider, tracks region metadata and
// validates bounds/permissions.  It is not a guest OS allocator: regions are
// explicit test/demo models of guest RAM.

#include "hv/memory/address_types.hpp"
#include "hv/memory/memory_backing_provider.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hv::memory {

inline constexpr std::uint64_t kFirstGuestPhysicalAddress = 0x0000'0000'4000'0000ULL;

struct GuestRegionInfo {
    std::uint64_t id{0};
    std::string name;
    GuestPhysicalAddress guest_base{};
    std::size_t size{0};
    HostPhysicalAddress host_base{};
    Permissions permissions{};
};

struct GuestPageInfo {
    GuestPhysicalAddress guest_physical{};
    HostPhysicalAddress host_physical{};
    std::string region_name;
    std::size_t region_offset{0};
    Permissions permissions{};
    bool backed{false};
};

struct GuestRegionRequest {
    std::string name;
    // Zero asks the manager to choose a non-overlapping guest-physical base.
    GuestPhysicalAddress guest_base{};
    std::size_t size{0};
    Permissions permissions{};
    // Must be a power of two and at least 4 KiB.
    std::size_t alignment{static_cast<std::size_t>(kSmallPageSize)};
};

struct GuestRegionResult {
    MemoryStatus status{MemoryStatus::InvalidContext};
    std::uint64_t region_id{0};
    GuestRegionInfo region{};
    std::string message{};

    [[nodiscard]] bool Succeeded() const noexcept { return hv::memory::Succeeded(status); }
};

struct GuestPhysicalTranslation {
    MemoryStatus status{MemoryStatus::Unmapped};
    GuestPhysicalAddress guest_physical{};
    HostPhysicalAddress host_physical{};
    std::string region_name;
    std::size_t region_offset{0};
    Permissions permissions{};
    std::string message{};

    [[nodiscard]] bool Succeeded() const noexcept { return hv::memory::Succeeded(status); }
};

class GuestMemoryManager final {
public:
    explicit GuestMemoryManager(IMemoryBackingProvider& backing);
    ~GuestMemoryManager() = default;

    GuestMemoryManager(const GuestMemoryManager&) = delete;
    GuestMemoryManager& operator=(const GuestMemoryManager&) = delete;

    [[nodiscard]] GuestRegionResult AddRegion(const GuestRegionRequest& request);
    [[nodiscard]] MemoryResult RemoveRegion(std::string_view name);
    [[nodiscard]] MemoryResult RemoveRegion(std::uint64_t region_id);
    [[nodiscard]] MemoryResult ReleaseAll();
    [[nodiscard]] MemoryResult SetRegionPermissions(std::string_view name,
                                                    Permissions permissions);

    [[nodiscard]] MemoryResult GetRegion(std::string_view name, GuestRegionInfo& out) const;
    [[nodiscard]] MemoryResult GetRegionById(std::uint64_t region_id, GuestRegionInfo& out) const;
    [[nodiscard]] std::vector<GuestRegionInfo> EnumerateRegions() const;
    [[nodiscard]] MemoryResult QueryPage(GuestPhysicalAddress page, GuestPageInfo& out) const;
    [[nodiscard]] MemoryResult EnumeratePages(std::string_view region_name,
                                              std::vector<GuestPageInfo>& out) const;

    [[nodiscard]] GuestPhysicalTranslation Translate(GuestPhysicalAddress address,
                                                     Access access) const;
    [[nodiscard]] MemoryResult Read(GuestPhysicalAddress address, std::span<std::byte> destination) const;
    [[nodiscard]] MemoryResult Write(GuestPhysicalAddress address,
                                     std::span<const std::byte> source);

    // Snapshot/diagnostic access.  Permissions are intentionally bypassed, but
    // every byte must still resolve to a registered region and real backing.
    [[nodiscard]] MemoryResult ReadDirect(GuestPhysicalAddress address,
                                          std::span<std::byte> destination) const;
    [[nodiscard]] MemoryResult WriteDirect(GuestPhysicalAddress address,
                                           std::span<const std::byte> source);

    [[nodiscard]] std::size_t RegionCount() const;
    [[nodiscard]] std::size_t PageCount() const;
    [[nodiscard]] std::size_t AllocatedBytes() const;
    [[nodiscard]] IMemoryBackingProvider& Backing() noexcept { return backing_; }
    [[nodiscard]] const IMemoryBackingProvider& Backing() const noexcept { return backing_; }

private:
    struct Region {
        std::uint64_t id{0};
        GuestRegionInfo info{};
    };

    [[nodiscard]] Region* FindRegionLocked(GuestPhysicalAddress address) noexcept;
    [[nodiscard]] const Region* FindRegionLocked(GuestPhysicalAddress address) const noexcept;
    [[nodiscard]] const Region* FindRegionByNameLocked(std::string_view name) const noexcept;
    [[nodiscard]] bool OverlapsLocked(GuestPhysicalAddress base, std::size_t size) const noexcept;
    [[nodiscard]] GuestPhysicalTranslation TranslateLocked(GuestPhysicalAddress address,
                                                           bool check_access,
                                                           Access access) const;

    mutable std::mutex mutex_;
    IMemoryBackingProvider& backing_;
    std::map<std::uint64_t, Region> regions_by_id_;
    std::map<GuestPhysicalAddress, std::uint64_t> regions_by_address_;
    std::uint64_t next_region_id_{1};
    GuestPhysicalAddress next_guest_address_{kFirstGuestPhysicalAddress};
};

} // namespace hv::memory
