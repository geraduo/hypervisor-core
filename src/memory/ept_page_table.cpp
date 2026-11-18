#include "hv/memory/ept_page_table.hpp"

#include <algorithm>
#include <limits>

namespace hv::memory {
namespace {

constexpr std::uint64_t kIndexMask = 0x1FFULL;

std::uint64_t PageBase(std::uint64_t address, std::uint64_t page_size) noexcept {
    return address & ~(page_size - 1ULL);
}

} // namespace

std::string_view EptEntryLevelName(EptEntryLevel level) noexcept {
    switch (level) {
    case EptEntryLevel::Pml4: return "PML4";
    case EptEntryLevel::Pdpt: return "PDPT";
    case EptEntryLevel::Pd: return "PD";
    case EptEntryLevel::Pt: return "PT";
    }
    return "Unknown";
}

EptPageTable::EptPageTable() = default;

EptPageTable::EptPageTable(Config config) : config_(config) {
    if (config_.address_limit > kMaxPhysicalAddress) {
        config_.address_limit = kMaxPhysicalAddress;
    }
}

EptPageTable::~EptPageTable() = default;

std::uint16_t EptPageTable::Index(std::uint64_t address, int level) noexcept {
    int shift = 39;
    if (level == 1) shift = 30;
    else if (level == 2) shift = 21;
    else if (level == 3) shift = 12;
    return static_cast<std::uint16_t>((address >> shift) & kIndexMask);
}

int EptPageTable::LeafLevelForPageSize(std::uint64_t page_size) noexcept {
    if (page_size == kHugePageSize) return 1;
    if (page_size == kLargePageSize) return 2;
    if (page_size == kSmallPageSize) return 3;
    return -1;
}

std::uint64_t EptPageTable::PageSizeForLevel(int level) noexcept {
    if (level == 1) return kHugePageSize;
    if (level == 2) return kLargePageSize;
    if (level == 3) return kSmallPageSize;
    return 0;
}

bool EptPageTable::SubtreeHasMappings(const Node& node) noexcept {
    if (!node.leaves.empty()) return true;
    for (const auto& [index, child] : node.children) {
        (void)index;
        if (child != nullptr && SubtreeHasMappings(*child)) return true;
    }
    return false;
}

const EptPageTable::Leaf* EptPageTable::FindLeafLocked(
    std::uint64_t address, std::vector<std::uint16_t>* indices) const noexcept {
    const Node* node = &root_;
    for (int level = 0; level <= 3; ++level) {
        const std::uint16_t index = Index(address, level);
        if (indices != nullptr) indices->push_back(index);

        const auto leaf_it = node->leaves.find(index);
        if (leaf_it != node->leaves.end()) {
            return &leaf_it->second;
        }
        if (level == 3) break;

        const auto child_it = node->children.find(index);
        if (child_it == node->children.end() || child_it->second == nullptr) {
            return nullptr;
        }
        node = child_it->second.get();
    }
    return nullptr;
}

EptPageTable::Leaf* EptPageTable::FindLeafLocked(std::uint64_t address) noexcept {
    Node* node = &root_;
    for (int level = 0; level <= 3; ++level) {
        const std::uint16_t index = Index(address, level);
        const auto leaf_it = node->leaves.find(index);
        if (leaf_it != node->leaves.end()) return &leaf_it->second;
        if (level == 3) break;

        const auto child_it = node->children.find(index);
        if (child_it == node->children.end() || child_it->second == nullptr) return nullptr;
        node = child_it->second.get();
    }
    return nullptr;
}

MemoryResult EptPageTable::Map(GuestPhysicalAddress guest_page, HostPhysicalAddress host_page,
                               std::uint64_t page_size, Permissions permissions) {
    std::lock_guard lock(mutex_);
    return MapLocked(guest_page, host_page, page_size, permissions);
}

MemoryResult EptPageTable::MapLocked(GuestPhysicalAddress guest_page,
                                     HostPhysicalAddress host_page,
                                     std::uint64_t page_size,
                                     Permissions permissions) {
    const int leaf_level = LeafLevelForPageSize(page_size);
    if (leaf_level < 0) {
        return MemoryResult::Error(MemoryStatus::InvalidMapping,
                                   "EPT page size must be 4 KiB, 2 MiB or 1 GiB");
    }
    if (!config_.allow_large_pages && page_size != kSmallPageSize) {
        return MemoryResult::Error(MemoryStatus::Unsupported,
                                   "Large-page mappings are disabled by this page table");
    }
    if (!guest_page.IsAlignedTo(page_size)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Guest-physical mapping is not aligned to its page size");
    }
    if (!host_page.IsAlignedTo(page_size)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Host-physical mapping is not aligned to its page size");
    }
    if (!RangeFits(guest_page.Value(), page_size, config_.address_limit)) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Guest-physical mapping lies outside the configured limit");
    }
    if (!RangeFits(host_page.Value(), page_size, kMaxPhysicalAddress)) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Host-physical mapping lies outside the 48-bit model");
    }

    Node* node = &root_;
    for (int level = 0; level < leaf_level; ++level) {
        const std::uint16_t index = Index(guest_page.Value(), level);
        if (node->leaves.contains(index)) {
            return MemoryResult::Error(MemoryStatus::AlreadyMapped,
                                       "A larger EPT page already covers this guest page");
        }
        auto child_it = node->children.find(index);
        if (child_it == node->children.end()) {
            auto child = std::make_unique<Node>();
            Node* child_raw = child.get();
            node->children.emplace(index, std::move(child));
            node = child_raw;
        } else {
            if (child_it->second == nullptr) {
                return MemoryResult::Error(MemoryStatus::InternalError,
                                           "EPT hierarchy contains an empty child pointer");
            }
            node = child_it->second.get();
        }
    }

    const std::uint16_t index = Index(guest_page.Value(), leaf_level);
    if (node->leaves.contains(index)) {
        return MemoryResult::Error(MemoryStatus::AlreadyMapped,
                                   "Guest-physical page is already mapped");
    }
    auto child_it = node->children.find(index);
    if (child_it != node->children.end()) {
        if (child_it->second != nullptr && SubtreeHasMappings(*child_it->second)) {
            return MemoryResult::Error(MemoryStatus::AlreadyMapped,
                                       "Smaller EPT pages are already mapped inside this page");
        }
        node->children.erase(child_it);
    }

    node->leaves.emplace(index, Leaf{host_page, page_size, permissions,
                                     static_cast<EptEntryLevel>(leaf_level)});
    ++mapping_count_;
    return MemoryResult::Ok();
}

MemoryResult EptPageTable::Unmap(GuestPhysicalAddress guest_page) {
    std::lock_guard lock(mutex_);
    return UnmapLocked(guest_page);
}

MemoryResult EptPageTable::UnmapLocked(GuestPhysicalAddress guest_page) {
    Node* node = &root_;
    struct PathEntry {
        Node* node;
        std::uint16_t index;
    };
    std::vector<PathEntry> path;
    path.reserve(4);

    for (int level = 0; level <= 3; ++level) {
        const std::uint16_t index = Index(guest_page.Value(), level);
        path.push_back(PathEntry{node, index});
        const auto leaf_it = node->leaves.find(index);
        if (leaf_it != node->leaves.end()) {
            const std::uint64_t page_size = leaf_it->second.page_size;
            if (!guest_page.IsAlignedTo(page_size)) {
                return MemoryResult::Error(MemoryStatus::AlignmentError,
                                           "Address is inside a larger EPT page; unmap the page base");
            }
            node->leaves.erase(leaf_it);
            --mapping_count_;

            for (std::size_t i = path.size(); i > 1; --i) {
                Node* parent = path[i - 2].node;
                Node* child = path[i - 1].node;
                const std::uint16_t child_index = path[i - 2].index;
                if (!child->children.empty() || !child->leaves.empty()) break;
                parent->children.erase(child_index);
            }
            return MemoryResult::Ok();
        }
        if (level == 3) break;
        const auto child_it = node->children.find(index);
        if (child_it == node->children.end() || child_it->second == nullptr) {
            return MemoryResult::Error(MemoryStatus::Unmapped,
                                       "Guest-physical page is not mapped");
        }
        node = child_it->second.get();
    }
    return MemoryResult::Error(MemoryStatus::Unmapped, "Guest-physical page is not mapped");
}

MemoryResult EptPageTable::Protect(GuestPhysicalAddress guest_page, Permissions permissions) {
    std::lock_guard lock(mutex_);
    return ProtectLocked(guest_page, permissions);
}

MemoryResult EptPageTable::ProtectLocked(GuestPhysicalAddress guest_page,
                                         Permissions permissions) {
    Leaf* leaf = FindLeafLocked(guest_page.Value());
    if (leaf == nullptr) {
        return MemoryResult::Error(MemoryStatus::Unmapped,
                                   "Guest-physical page is not mapped");
    }
    if (!guest_page.IsAlignedTo(leaf->page_size)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Address is inside a larger EPT page; protect the page base");
    }
    leaf->permissions = permissions;
    return MemoryResult::Ok();
}

EptTranslationResult EptPageTable::Translate(GuestPhysicalAddress address, Access access) const {
    std::lock_guard lock(mutex_);
    EptTranslationResult result{};
    result.guest_physical = address;
    if (!IsCanonicalPhysicalAddress(address.Value())) {
        result.status = MemoryStatus::InvalidAddress;
        result.message = "Guest-physical address exceeds the 48-bit model";
        return result;
    }
    if (address.Value() > config_.address_limit) {
        result.status = MemoryStatus::InvalidAddress;
        result.message = "Guest-physical address exceeds the page-table limit";
        return result;
    }
    const Leaf* leaf = FindLeafLocked(address.Value(), nullptr);
    if (leaf == nullptr) {
        result.status = MemoryStatus::Unmapped;
        result.message = "Guest-physical page is not mapped";
        return result;
    }
    const std::uint64_t base = PageBase(address.Value(), leaf->page_size);
    const std::uint64_t offset = address.Value() - base;
    result.host_physical = HostPhysicalAddress{leaf->host_page.Value() + offset};
    result.page_size = leaf->page_size;
    result.permissions = leaf->permissions;
    result.level = leaf->level;
    if (!leaf->permissions.Allows(access)) {
        result.status = MemoryStatus::AccessDenied;
        result.message = "EPT permissions deny this access";
        return result;
    }
    result.status = MemoryStatus::Success;
    return result;
}

MemoryResult EptPageTable::Query(GuestPhysicalAddress address, EptEntryInfo& out) const {
    std::lock_guard lock(mutex_);
    if (!IsCanonicalPhysicalAddress(address.Value()) || address.Value() > config_.address_limit) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Guest-physical address is outside the model");
    }
    const Leaf* leaf = FindLeafLocked(address.Value(), nullptr);
    if (leaf == nullptr) {
        return MemoryResult::Error(MemoryStatus::Unmapped,
                                   "Guest-physical page is not mapped");
    }
    out.guest_physical_page = GuestPhysicalAddress{PageBase(address.Value(), leaf->page_size)};
    out.host_physical_page = leaf->host_page;
    out.page_size = leaf->page_size;
    out.permissions = leaf->permissions;
    out.level = leaf->level;
    out.present = true;
    return MemoryResult::Ok();
}

std::size_t EptPageTable::MappingCount() const {
    std::lock_guard lock(mutex_);
    return mapping_count_;
}

std::vector<EptEntryInfo> EptPageTable::Enumerate() const {
    std::lock_guard lock(mutex_);
    std::vector<EptEntryInfo> result;
    std::vector<std::uint16_t> indices;
    indices.reserve(4);
    CollectLocked(root_, indices, 0, result);
    std::sort(result.begin(), result.end(), [](const EptEntryInfo& lhs, const EptEntryInfo& rhs) {
        return lhs.guest_physical_page < rhs.guest_physical_page;
    });
    return result;
}

void EptPageTable::CollectLocked(const Node& node, std::vector<std::uint16_t>& indices,
                                 int depth, std::vector<EptEntryInfo>& out) const {
    for (const auto& [index, leaf] : node.leaves) {
        std::uint64_t base = 0;
        for (int level = 0; level < depth; ++level) {
            const int shift = 39 - (9 * level);
            base |= static_cast<std::uint64_t>(indices[static_cast<std::size_t>(level)])
                    << shift;
        }
        if (depth <= 3) {
            const int leaf_shift = 39 - (9 * depth);
            base |= static_cast<std::uint64_t>(index) << leaf_shift;
        }
        out.push_back(EptEntryInfo{GuestPhysicalAddress{base}, leaf.host_page, leaf.page_size,
                                   leaf.permissions, leaf.level, true});
    }
    if (depth >= 3) return;
    for (const auto& [index, child] : node.children) {
        if (child == nullptr) continue;
        indices.push_back(index);
        CollectLocked(*child, indices, depth + 1, out);
        indices.pop_back();
    }
}

bool EptPageTable::ValidateNodeLocked(const Node& node, std::vector<std::uint16_t>& indices,
                                      int depth, std::size_t& leaf_count,
                                      std::string& error) const {
    if (depth > 3) {
        error = "EPT hierarchy is deeper than four levels";
        return false;
    }
    if (indices.size() != static_cast<std::size_t>(depth)) {
        error = "EPT hierarchy index depth is inconsistent";
        return false;
    }
    for (const auto& [index, leaf] : node.leaves) {
        if (depth == 0) {
            error = "A leaf entry is not allowed at the PML4 root";
            return false;
        }
        if (node.children.contains(index)) {
            error = "The same EPT index contains both a leaf and a branch";
            return false;
        }
        const int expected_level = LeafLevelForPageSize(leaf.page_size);
        if (expected_level < 0) {
            error = "EPT leaf has an unsupported page size";
            return false;
        }
        if (expected_level != depth) {
            error = "EPT leaf page size does not match its hierarchy level";
            return false;
        }
        if (leaf.level != static_cast<EptEntryLevel>(expected_level)) {
            error = "EPT leaf level metadata is inconsistent";
            return false;
        }
        std::uint64_t base = 0;
        for (int level = 0; level < depth; ++level) {
            const int shift = 39 - (9 * level);
            base |= static_cast<std::uint64_t>(indices[static_cast<std::size_t>(level)])
                    << shift;
        }
        base |= static_cast<std::uint64_t>(index) << (39 - (9 * depth));
        if (!RangeFits(base, leaf.page_size, config_.address_limit)) {
            error = "EPT leaf lies outside the configured guest-physical limit";
            return false;
        }
        if (!RangeFits(leaf.host_page.Value(), leaf.page_size, kMaxPhysicalAddress)) {
            error = "EPT leaf host address lies outside the 48-bit model";
            return false;
        }
        if ((base & (leaf.page_size - 1ULL)) != 0 ||
            (leaf.host_page.Value() & (leaf.page_size - 1ULL)) != 0) {
            error = "EPT leaf is not aligned to its page size";
            return false;
        }
        ++leaf_count;
    }

    if (depth == 3) {
        if (!node.children.empty()) {
            error = "EPT PT level must not have child branches";
            return false;
        }
        return true;
    }

    for (const auto& [index, child] : node.children) {
        if (child == nullptr) {
            error = "EPT hierarchy contains a null child";
            return false;
        }
        if (node.leaves.contains(index)) {
            error = "The same EPT index contains both a leaf and a branch";
            return false;
        }
        indices.push_back(index);
        const bool ok = ValidateNodeLocked(*child, indices, depth + 1, leaf_count, error);
        indices.pop_back();
        if (!ok) return false;
    }
    return true;
}

MemoryResult EptPageTable::Validate() const {
    std::lock_guard lock(mutex_);
    std::vector<std::uint16_t> indices;
    indices.reserve(4);
    std::size_t leaf_count = 0;
    std::string error;
    if (!ValidateNodeLocked(root_, indices, 0, leaf_count, error)) {
        return MemoryResult::Error(MemoryStatus::InvalidMapping, std::move(error));
    }
    if (leaf_count != mapping_count_) {
        return MemoryResult::Error(MemoryStatus::InternalError,
                                   "EPT mapping counter does not match the hierarchy");
    }
    return MemoryResult::Ok();
}

void EptPageTable::Clear() {
    std::lock_guard lock(mutex_);
    root_ = Node{};
    mapping_count_ = 0;
}

EptPageTable::Config EptPageTable::GetConfig() const {
    std::lock_guard lock(mutex_);
    return config_;
}

} // namespace hv::memory
