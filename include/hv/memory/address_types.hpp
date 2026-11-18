#pragma once

// Strongly typed guest-memory addresses and shared memory result types.
//
// The pre-0.6.0 public API (namespace hv) represents GVA/GPA/HPA as plain
// uint64_t aliases.  The v0.6.0 memory layer deliberately lives in
// namespace hv::memory and uses distinct class types so that an address of one
// level can never be passed accidentally where another level is expected.
//
// These types do not describe CPU page tables or real host-physical memory.
// They are values used by the software memory model implemented in this
// project.  See docs/ADDRESS_TRANSLATION.md for the full contract.

#include "hv/types.hpp" // hv::Access / hv::Permissions are reused, not duplicated.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hv::memory {

inline constexpr std::uint64_t kSmallPageSize = 4096ULL;
inline constexpr std::uint64_t kLargePageSize = 2ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kHugePageSize = 1024ULL * 1024ULL * 1024ULL;

// The software model limits physical addresses to the architectural 48-bit
// window.  Larger values are rejected with MemoryStatus::InvalidAddress.
inline constexpr std::uint64_t kMaxPhysicalAddress = (1ULL << 48U) - 1ULL;

// The first synthetic host-physical address handed out by
// FakeMemoryBackingProvider.  It is a model address, not an OS virtual address.
inline constexpr std::uint64_t kFirstSyntheticHostAddress = 0x0000'0100'0000'0000ULL;

namespace detail {

template <typename Tag>
class AddressValue final {
public:
    constexpr AddressValue() noexcept = default;
    explicit constexpr AddressValue(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t Value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool IsZero() const noexcept { return value_ == 0; }

    [[nodiscard]] constexpr bool IsAlignedTo(std::uint64_t alignment) const noexcept {
        return alignment != 0 && (value_ & (alignment - 1ULL)) == 0;
    }

    [[nodiscard]] constexpr std::uint64_t OffsetIn(std::uint64_t page_size) const noexcept {
        return page_size == 0 ? 0 : value_ & (page_size - 1ULL);
    }

    [[nodiscard]] constexpr std::uint64_t AlignDown(std::uint64_t alignment) const noexcept {
        return alignment == 0 ? value_ : value_ & ~(alignment - 1ULL);
    }

    [[nodiscard]] static constexpr AddressValue FromValue(std::uint64_t value) noexcept {
        return AddressValue(value);
    }

    friend constexpr auto operator<=>(const AddressValue&, const AddressValue&) = default;

private:
    std::uint64_t value_{};
};

struct GuestVirtualTag final {};
struct GuestPhysicalTag final {};
struct HostPhysicalTag final {};

} // namespace detail

using GuestVirtualAddress = detail::AddressValue<detail::GuestVirtualTag>;
using GuestPhysicalAddress = detail::AddressValue<detail::GuestPhysicalTag>;
using HostPhysicalAddress = detail::AddressValue<detail::HostPhysicalTag>;

// Address-space handles are not addresses.  Zero means "no context".
class AddressSpaceId final {
public:
    constexpr AddressSpaceId() noexcept = default;
    explicit constexpr AddressSpaceId(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t Value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return value_ != 0; }
    [[nodiscard]] static constexpr AddressSpaceId FromValue(std::uint64_t value) noexcept {
        return AddressSpaceId(value);
    }

    friend constexpr auto operator<=>(const AddressSpaceId&, const AddressSpaceId&) = default;

private:
    std::uint64_t value_{0};
};

// Reuse the existing permission and access enums.  Keeping a single definition
// prevents the legacy translation API and the v0.6.0 memory layer from drifting
// apart.
using Permissions = ::hv::Permissions;
using Access = ::hv::Access;

[[nodiscard]] constexpr bool Allows(Permissions permissions, Access access) noexcept {
    return permissions.Allows(access);
}

[[nodiscard]] constexpr Permissions MakePermissions(bool read, bool write, bool execute) noexcept {
    Permissions permissions{};
    permissions.read = read;
    permissions.write = write;
    permissions.execute = execute;
    return permissions;
}

[[nodiscard]] std::string PermissionsToString(Permissions permissions);
[[nodiscard]] std::string_view AccessName(Access access) noexcept;

enum class MemoryStatus : std::uint8_t {
    Success = 0,
    InvalidAddress,
    InvalidContext,
    Unmapped,
    AccessDenied,
    AlignmentError,
    InvalidMapping,
    InternalError,

    // Additional precise statuses used by the software model.  They keep OOM,
    // duplicate mappings and snapshot failures distinguishable instead of
    // collapsing every non-success path into InvalidMapping/InternalError.
    AlreadyMapped,
    OutOfMemory,
    SnapshotNotFound,
    SnapshotCorrupt,
    NotInitialized,
    Unsupported
};

[[nodiscard]] std::string_view MemoryStatusName(MemoryStatus status) noexcept;
[[nodiscard]] constexpr bool Succeeded(MemoryStatus status) noexcept {
    return status == MemoryStatus::Success;
}

// Structured result used by every non-translation operation in this layer.
struct MemoryResult {
    MemoryStatus status{MemoryStatus::Success};
    std::string message{};

    [[nodiscard]] bool Succeeded() const noexcept { return hv::memory::Succeeded(status); }

    static MemoryResult Ok() { return {}; }
    static MemoryResult Error(MemoryStatus code, std::string text) {
        return MemoryResult{code, std::move(text)};
    }
};

// Result of one or both translation stages.
//
// guest_translated is true when the GVA -> GPA stage produced a page.
// nested_translated is true when the GPA -> HPA stage produced a page.
// page_size is the smallest page size that is valid for the whole translation
// (the guest page size and the nested page size may differ).
struct TranslationResult {
    MemoryStatus status{MemoryStatus::InvalidContext};
    GuestVirtualAddress guest_virtual{};
    GuestPhysicalAddress guest_physical{};
    HostPhysicalAddress host_physical{};
    std::uint64_t page_size{0};
    Permissions permissions{};
    bool guest_translated{false};
    bool nested_translated{false};
    std::string message{};

    [[nodiscard]] bool Succeeded() const noexcept { return hv::memory::Succeeded(status); }

    static TranslationResult Error(MemoryStatus code, std::string text) {
        TranslationResult result{};
        result.status = code;
        result.message = std::move(text);
        return result;
    }
};

[[nodiscard]] constexpr bool IsPowerOfTwo(std::uint64_t value) noexcept {
    return value != 0 && (value & (value - 1ULL)) == 0;
}

[[nodiscard]] constexpr bool IsSupportedPageSize(std::uint64_t page_size) noexcept {
    return page_size == kSmallPageSize || page_size == kLargePageSize || page_size == kHugePageSize;
}

[[nodiscard]] constexpr bool IsCanonicalPhysicalAddress(std::uint64_t value) noexcept {
    return value <= kMaxPhysicalAddress;
}

[[nodiscard]] constexpr bool RangeFits(std::uint64_t base, std::uint64_t size,
                                       std::uint64_t limit) noexcept {
    if (size == 0) return base <= limit;
    if (base > limit) return false;
    return size - 1ULL <= limit - base;
}

[[nodiscard]] std::string FormatAddress(std::uint64_t value);
[[nodiscard]] std::string FormatBytes(std::uint64_t value);

// Conversion helpers for the legacy hv::MemoryManager / SyntheticPageTable /
// NestedPageTable APIs.  They are intentionally explicit at every call site.
// The three legacy address types are uint64_t aliases, so the "from" helpers
// cannot be overloaded and use distinct names instead.
[[nodiscard]] constexpr ::hv::GuestVirtualAddress ToLegacy(GuestVirtualAddress address) noexcept {
    return address.Value();
}
[[nodiscard]] constexpr ::hv::GuestPhysicalAddress ToLegacy(GuestPhysicalAddress address) noexcept {
    return address.Value();
}
[[nodiscard]] constexpr ::hv::HostPhysicalAddress ToLegacy(HostPhysicalAddress address) noexcept {
    return address.Value();
}
[[nodiscard]] constexpr GuestVirtualAddress GuestVirtualFromLegacy(
    ::hv::GuestVirtualAddress address) noexcept {
    return GuestVirtualAddress{address};
}
[[nodiscard]] constexpr GuestPhysicalAddress GuestPhysicalFromLegacy(
    ::hv::GuestPhysicalAddress address) noexcept {
    return GuestPhysicalAddress{address};
}
[[nodiscard]] constexpr HostPhysicalAddress HostPhysicalFromLegacy(
    ::hv::HostPhysicalAddress address) noexcept {
    return HostPhysicalAddress{address};
}

} // namespace hv::memory
