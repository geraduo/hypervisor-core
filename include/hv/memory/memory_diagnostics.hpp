#pragma once

// Aggregated diagnostics for the software memory model: translation counters,
// permission denials, mapping ranges, address-space/region gauges and snapshot
// summaries.  ToJson() produces a self-contained JSON report.

#include "hv/memory/address_types.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hv::memory {

class GuestAddressSpaceManager;
class EptManager;
class GuestMemoryManager;
class MemorySnapshotManager;

struct MemoryStatistics {
    std::uint64_t mapped_pages{0};
    std::uint64_t mapping_ranges{0};
    std::uint64_t address_spaces{0};
    std::uint64_t guest_regions{0};
    std::uint64_t successful_translations{0};
    std::uint64_t failed_translations{0};
    std::uint64_t permission_denials{0};
    std::uint64_t mapping_operations{0};
    std::uint64_t unmapping_operations{0};
    std::uint64_t protection_changes{0};
    std::uint64_t snapshots{0};
    std::uint64_t snapshots_created{0};
    std::uint64_t snapshots_restored{0};
    std::uint64_t snapshot_failures{0};
    std::uint64_t allocated_backing_bytes{0};
};

enum class MappingDomain : std::uint8_t {
    GuestPageTable,
    Ept
};

[[nodiscard]] const char* MappingDomainName(MappingDomain domain) noexcept;

struct MappingRangeInfo {
    MappingDomain domain{MappingDomain::GuestPageTable};
    AddressSpaceId address_space{};
    std::uint64_t base{0};
    std::uint64_t size{0};
    std::uint64_t page_size{kSmallPageSize};
    Permissions permissions{};
};

struct SnapshotSummary {
    std::uint64_t id{0};
    std::string name;
    std::uint64_t size{0};
    std::uint64_t checksum{0};
    std::uint64_t created_unix_ms{0};
    bool valid{false};
};

class MemoryDiagnostics final {
public:
    MemoryDiagnostics() = default;
    ~MemoryDiagnostics() = default;

    MemoryDiagnostics(const MemoryDiagnostics&) = delete;
    MemoryDiagnostics& operator=(const MemoryDiagnostics&) = delete;

    void RecordTranslation(MemoryStatus status, bool permission_denied);
    void RecordMapping(MemoryStatus status);
    void RecordUnmapping(MemoryStatus status);
    void RecordProtection(MemoryStatus status);
    void RecordSnapshotCreated(MemoryStatus status);
    void RecordSnapshotRestored(MemoryStatus status);
    void RecordSnapshotFailure();

    void SetMappedPages(std::uint64_t count);
    void SetAddressSpaces(std::uint64_t count);
    void SetGuestRegions(std::uint64_t count);
    void SetAllocatedBackingBytes(std::uint64_t bytes);
    void SetMappingRanges(std::vector<MappingRangeInfo> ranges);
    void SetSnapshotSummaries(std::vector<SnapshotSummary> summaries);

    void Refresh(const GuestAddressSpaceManager& spaces, const EptManager& ept,
                 const GuestMemoryManager& memory, const MemorySnapshotManager& snapshots);

    [[nodiscard]] MemoryStatistics GetStatistics() const;
    [[nodiscard]] std::vector<MappingRangeInfo> Ranges() const;
    [[nodiscard]] std::vector<SnapshotSummary> Snapshots() const;
    [[nodiscard]] std::string ToJson() const;
    void Reset();

private:
    mutable std::mutex mutex_;
    MemoryStatistics statistics_{};
    std::vector<MappingRangeInfo> ranges_;
    std::vector<SnapshotSummary> snapshots_;
};

} // namespace hv::memory
