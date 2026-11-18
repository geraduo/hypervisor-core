#pragma once

// Software model of a multi-level EPT page table.
//
// This class models hierarchy, indexing, entry validity, page sizes and
// conflict detection.  It does NOT program a VMCS/EPTP, does not execute VMX
// instructions and does not turn on hardware EPT.  See docs/EPT_MODEL.md.

#include "hv/memory/address_types.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace hv::memory {

enum class EptEntryLevel : std::uint8_t {
    Pml4 = 0,
    Pdpt = 1,
    Pd = 2,
    Pt = 3
};

[[nodiscard]] std::string_view EptEntryLevelName(EptEntryLevel level) noexcept;

struct EptEntryInfo {
    GuestPhysicalAddress guest_physical_page{};
    HostPhysicalAddress host_physical_page{};
    std::uint64_t page_size{kSmallPageSize};
    Permissions permissions{};
    EptEntryLevel level{EptEntryLevel::Pt};
    bool present{false};
};

struct EptTranslationResult {
    MemoryStatus status{MemoryStatus::Unmapped};
    GuestPhysicalAddress guest_physical{};
    HostPhysicalAddress host_physical{};
    std::uint64_t page_size{0};
    Permissions permissions{};
    EptEntryLevel level{EptEntryLevel::Pt};
    std::string message{};

    [[nodiscard]] bool Succeeded() const noexcept { return hv::memory::Succeeded(status); }
};

class EptPageTable final {
public:
    struct Config {
        bool allow_large_pages{true};
        // Highest guest-physical address accepted by this table.
        std::uint64_t address_limit{kMaxPhysicalAddress};
    };

    EptPageTable();
    explicit EptPageTable(Config config);
    ~EptPageTable();

    EptPageTable(const EptPageTable&) = delete;
    EptPageTable& operator=(const EptPageTable&) = delete;

    [[nodiscard]] MemoryResult Map(GuestPhysicalAddress guest_page,
                                   HostPhysicalAddress host_page,
                                   std::uint64_t page_size = kSmallPageSize,
                                   Permissions permissions = {});
    [[nodiscard]] MemoryResult Unmap(GuestPhysicalAddress guest_page);
    [[nodiscard]] MemoryResult Protect(GuestPhysicalAddress guest_page, Permissions permissions);
    [[nodiscard]] EptTranslationResult Translate(GuestPhysicalAddress address,
                                                 Access access) const;
    [[nodiscard]] MemoryResult Query(GuestPhysicalAddress address, EptEntryInfo& out) const;

    [[nodiscard]] std::size_t MappingCount() const;
    [[nodiscard]] std::vector<EptEntryInfo> Enumerate() const;
    [[nodiscard]] MemoryResult Validate() const;
    void Clear();
    [[nodiscard]] Config GetConfig() const;

private:
    struct Leaf {
        HostPhysicalAddress host_page{};
        std::uint64_t page_size{kSmallPageSize};
        Permissions permissions{};
        EptEntryLevel level{EptEntryLevel::Pt};
    };

    struct Node {
        std::map<std::uint16_t, std::unique_ptr<Node>> children;
        std::map<std::uint16_t, Leaf> leaves;
    };

    [[nodiscard]] static std::uint16_t Index(std::uint64_t address, int level) noexcept;
    [[nodiscard]] static int LeafLevelForPageSize(std::uint64_t page_size) noexcept;
    [[nodiscard]] static std::uint64_t PageSizeForLevel(int level) noexcept;
    [[nodiscard]] static bool SubtreeHasMappings(const Node& node) noexcept;

    [[nodiscard]] const Leaf* FindLeafLocked(std::uint64_t address,
                                             std::vector<std::uint16_t>* indices) const noexcept;
    [[nodiscard]] Leaf* FindLeafLocked(std::uint64_t address) noexcept;

    [[nodiscard]] MemoryResult MapLocked(GuestPhysicalAddress guest_page,
                                         HostPhysicalAddress host_page,
                                         std::uint64_t page_size,
                                         Permissions permissions);
    [[nodiscard]] MemoryResult UnmapLocked(GuestPhysicalAddress guest_page);
    [[nodiscard]] MemoryResult ProtectLocked(GuestPhysicalAddress guest_page,
                                             Permissions permissions);

    void CollectLocked(const Node& node, std::vector<std::uint16_t>& indices,
                       int depth, std::vector<EptEntryInfo>& out) const;
    [[nodiscard]] bool ValidateNodeLocked(const Node& node, std::vector<std::uint16_t>& indices,
                                          int depth, std::size_t& leaf_count,
                                          std::string& error) const;

    mutable std::mutex mutex_;
    Node root_;
    std::size_t mapping_count_{0};
    Config config_{};
};

} // namespace hv::memory
