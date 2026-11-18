#pragma once

// In-memory snapshots of the software guest-memory model.
//
// A snapshot captures guest-region bytes, guest address-space mappings and EPT
// mappings.  It is a test/diagnostic artifact, not a host filesystem image or
// a VM snapshot.  Restore verifies size, structure and an FNV-1a checksum
// before touching live state.

#include "hv/memory/address_types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hv::memory {

class GuestMemoryManager;
class GuestAddressSpaceManager;
class EptManager;
class MemoryDiagnostics;

struct MemorySnapshotInfo {
    std::uint64_t id{0};
    std::string name;
    std::size_t total_bytes{0};
    std::uint64_t checksum{0};
    std::uint64_t created_unix_ms{0};
    std::size_t region_count{0};
    std::size_t guest_mapping_count{0};
    std::size_t ept_mapping_count{0};
    bool valid{false};
};

struct MemorySnapshotResult {
    MemoryStatus status{MemoryStatus::InternalError};
    std::uint64_t id{0};
    MemorySnapshotInfo info{};
    std::string message{};

    [[nodiscard]] bool Succeeded() const noexcept { return hv::memory::Succeeded(status); }
};

class MemorySnapshotManager final {
public:
    MemorySnapshotManager(GuestMemoryManager& memory, GuestAddressSpaceManager& spaces,
                          EptManager& ept, MemoryDiagnostics* diagnostics = nullptr);
    ~MemorySnapshotManager() = default;

    MemorySnapshotManager(const MemorySnapshotManager&) = delete;
    MemorySnapshotManager& operator=(const MemorySnapshotManager&) = delete;

    [[nodiscard]] MemorySnapshotResult CreateSnapshot(std::string name);
    [[nodiscard]] MemoryResult RestoreSnapshot(std::uint64_t snapshot_id);
    [[nodiscard]] MemoryResult DeleteSnapshot(std::uint64_t snapshot_id);
    [[nodiscard]] MemoryResult VerifySnapshot(std::uint64_t snapshot_id) const;
    [[nodiscard]] MemoryResult GetSnapshotInfo(std::uint64_t snapshot_id,
                                               MemorySnapshotInfo& out) const;
    [[nodiscard]] std::vector<MemorySnapshotInfo> GetSnapshotSummaries() const;
    [[nodiscard]] std::size_t SnapshotCount() const;
    void SetMaxSnapshotBytes(std::size_t max_bytes);
    [[nodiscard]] std::size_t GetMaxSnapshotBytes() const;
    void Clear();

    // Test hook: flips one payload byte so checksum verification can be
    // exercised without exposing mutable snapshot storage.
    [[nodiscard]] MemoryResult CorruptSnapshotForTesting(std::uint64_t snapshot_id,
                                                         std::size_t offset);

private:
    struct RegionSnapshot {
        std::string name;
        GuestPhysicalAddress guest_base{};
        std::size_t size{0};
        HostPhysicalAddress host_base{};
        Permissions permissions{};
        std::vector<std::byte> bytes;
    };

    struct GuestMappingSnapshot {
        AddressSpaceId address_space{};
        GuestVirtualAddress virtual_page{};
        GuestPhysicalAddress guest_physical_page{};
        Permissions permissions{};
    };

    struct EptMappingSnapshot {
        GuestPhysicalAddress guest_page{};
        HostPhysicalAddress host_page{};
        std::uint64_t page_size{kSmallPageSize};
        Permissions permissions{};
    };

    struct Snapshot {
        MemorySnapshotInfo info{};
        std::vector<RegionSnapshot> regions;
        std::vector<GuestMappingSnapshot> guest_mappings;
        std::vector<EptMappingSnapshot> ept_mappings;
    };

    [[nodiscard]] std::uint64_t CalculateChecksumLocked(const Snapshot& snapshot) const;
    [[nodiscard]] MemoryResult VerifyLocked(const Snapshot& snapshot) const;
    [[nodiscard]] bool ChecksumMatchesLocked(const Snapshot& snapshot) const;
    [[nodiscard]] MemorySnapshotInfo InfoLocked(const Snapshot& snapshot) const;

    mutable std::mutex mutex_;
    GuestMemoryManager& memory_;
    GuestAddressSpaceManager& spaces_;
    EptManager& ept_;
    MemoryDiagnostics* diagnostics_{nullptr};
    std::map<std::uint64_t, Snapshot> snapshots_;
    std::uint64_t next_snapshot_id_{1};
    std::size_t max_snapshot_bytes_{16ULL * 1024ULL * 1024ULL};
};

} // namespace hv::memory
