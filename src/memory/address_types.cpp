#include "hv/memory/address_types.hpp"

#include <cstdio>

namespace hv::memory {

std::string PermissionsToString(Permissions permissions) {
    std::string result;
    result.reserve(8);
    result.push_back(permissions.read ? 'r' : '-');
    result.push_back(permissions.write ? 'w' : '-');
    result.push_back(permissions.execute ? 'x' : '-');
    return result;
}

std::string_view AccessName(Access access) noexcept {
    switch (access) {
    case Access::Read: return "read";
    case Access::Write: return "write";
    case Access::Execute: return "execute";
    }
    return "unknown";
}

std::string_view MemoryStatusName(MemoryStatus status) noexcept {
    switch (status) {
    case MemoryStatus::Success: return "Success";
    case MemoryStatus::InvalidAddress: return "InvalidAddress";
    case MemoryStatus::InvalidContext: return "InvalidContext";
    case MemoryStatus::Unmapped: return "Unmapped";
    case MemoryStatus::AccessDenied: return "AccessDenied";
    case MemoryStatus::AlignmentError: return "AlignmentError";
    case MemoryStatus::InvalidMapping: return "InvalidMapping";
    case MemoryStatus::InternalError: return "InternalError";
    case MemoryStatus::AlreadyMapped: return "AlreadyMapped";
    case MemoryStatus::OutOfMemory: return "OutOfMemory";
    case MemoryStatus::SnapshotNotFound: return "SnapshotNotFound";
    case MemoryStatus::SnapshotCorrupt: return "SnapshotCorrupt";
    case MemoryStatus::NotInitialized: return "NotInitialized";
    case MemoryStatus::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

std::string FormatAddress(std::uint64_t value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%llX",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

std::string FormatBytes(std::uint64_t value) {
    if (value >= 1024ULL * 1024ULL * 1024ULL) {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "%.2f GiB",
                      static_cast<double>(value) / (1024.0 * 1024.0 * 1024.0));
        return std::string(buffer);
    }
    if (value >= 1024ULL * 1024ULL) {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "%.2f MiB",
                      static_cast<double>(value) / (1024.0 * 1024.0));
        return std::string(buffer);
    }
    if (value >= 1024ULL) {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "%.2f KiB",
                      static_cast<double>(value) / 1024.0);
        return std::string(buffer);
    }
    return std::to_string(value) + " B";
}

} // namespace hv::memory
