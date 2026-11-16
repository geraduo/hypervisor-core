#pragma once
#include <cstdint>

namespace hv {

using GuestVirtualAddress = std::uint64_t;
using GuestPhysicalAddress = std::uint64_t;
using HostPhysicalAddress = std::uint64_t;
using AddressSpaceId = std::uint64_t;

inline constexpr std::uint64_t kPageSize = 4096;
inline constexpr std::uint64_t kPageMask = ~(kPageSize - 1);

enum class Access : std::uint8_t { Read = 1, Write = 2, Execute = 4 };

struct Permissions {
    bool read{true};
    bool write{true};
    bool execute{false};

    [[nodiscard]] bool Allows(Access access) const noexcept {
        switch (access) {
        case Access::Read: return read;
        case Access::Write: return write;
        case Access::Execute: return execute;
        }
        return false;
    }
};

enum class TranslationStatus {
    Success,
    InvalidContext,
    UnmappedGuestVirtual,
    UnmappedGuestPhysical,
    AccessDenied
};

struct TranslationResult {
    TranslationStatus status{TranslationStatus::InvalidContext};
    HostPhysicalAddress host_physical_address{};
    std::uint64_t page_size{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == TranslationStatus::Success;
    }
};

} // namespace hv
