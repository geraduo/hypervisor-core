#pragma once

// Abstraction for the real memory that backs the synthetic guest-physical
// model.  HostPhysicalAddress values are model addresses assigned by the
// provider; every such address is backed by a real allocation owned by the
// provider.  The provider never fabricates an address for an allocation that
// does not exist.

#include "hv/memory/address_types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace hv::memory {

// Borrowed view of one provider allocation.  The provider owns the storage;
// the pointer remains valid until Release/ReleaseAll/Shutdown or destruction.
struct MemoryBlock {
    HostPhysicalAddress address{};
    std::size_t size{0};
    std::byte* data{nullptr};

    [[nodiscard]] bool IsValid() const noexcept { return data != nullptr && size != 0; }
};

struct MemoryBlockResult {
    MemoryStatus status{MemoryStatus::NotInitialized};
    MemoryBlock block{};
    std::string message{};

    [[nodiscard]] bool Succeeded() const noexcept { return hv::memory::Succeeded(status); }
};

class IMemoryBackingProvider {
public:
    virtual ~IMemoryBackingProvider() = default;

    IMemoryBackingProvider(const IMemoryBackingProvider&) = delete;
    IMemoryBackingProvider& operator=(const IMemoryBackingProvider&) = delete;

    [[nodiscard]] virtual MemoryResult Initialize() = 0;
    // Releases every outstanding allocation.  The provider remains usable and
    // can be initialized again.
    [[nodiscard]] virtual MemoryResult Shutdown() = 0;
    [[nodiscard]] virtual bool IsInitialized() const noexcept = 0;

    // size and alignment are validated.  alignment must be a power of two and
    // at least alignof(std::max_align_t).
    [[nodiscard]] virtual MemoryBlockResult Allocate(std::size_t size, std::size_t alignment) = 0;
    [[nodiscard]] virtual MemoryResult Release(HostPhysicalAddress address) = 0;
    [[nodiscard]] virtual MemoryResult ReleaseAll() = 0;

    [[nodiscard]] virtual bool Owns(HostPhysicalAddress address) const noexcept = 0;
    [[nodiscard]] virtual bool TryGetBlock(HostPhysicalAddress address,
                                            MemoryBlock& out) const noexcept = 0;
    // Resolves an address that may point inside a block (not only at its base).
    [[nodiscard]] virtual bool TryResolve(HostPhysicalAddress address, MemoryBlock& out,
                                          std::size_t& offset) const noexcept = 0;

    [[nodiscard]] virtual MemoryResult Read(HostPhysicalAddress address,
                                            std::span<std::byte> destination) const = 0;
    [[nodiscard]] virtual MemoryResult Write(HostPhysicalAddress address,
                                             std::span<const std::byte> source) = 0;

    [[nodiscard]] virtual std::size_t AllocatedBytes() const noexcept = 0;
    [[nodiscard]] virtual std::size_t AllocationCount() const noexcept = 0;

protected:
    IMemoryBackingProvider() = default;
};

// Test/backing provider that allocates genuine aligned storage with the C++
// allocator and assigns monotonically increasing synthetic host-physical
// addresses to the allocations.  The synthetic address is never dereferenced
// directly; use Read/Write or TryResolve.
class FakeMemoryBackingProvider final : public IMemoryBackingProvider {
public:
    static constexpr std::size_t kDefaultMaxBytes = 64ULL * 1024ULL * 1024ULL;

    FakeMemoryBackingProvider();
    explicit FakeMemoryBackingProvider(std::size_t max_bytes);
    ~FakeMemoryBackingProvider() override;

    FakeMemoryBackingProvider(const FakeMemoryBackingProvider&) = delete;
    FakeMemoryBackingProvider& operator=(const FakeMemoryBackingProvider&) = delete;

    [[nodiscard]] MemoryResult Initialize() override;
    [[nodiscard]] MemoryResult Shutdown() override;
    [[nodiscard]] bool IsInitialized() const noexcept override;

    [[nodiscard]] MemoryBlockResult Allocate(std::size_t size, std::size_t alignment) override;
    [[nodiscard]] MemoryResult Release(HostPhysicalAddress address) override;
    [[nodiscard]] MemoryResult ReleaseAll() override;

    [[nodiscard]] bool Owns(HostPhysicalAddress address) const noexcept override;
    [[nodiscard]] bool TryGetBlock(HostPhysicalAddress address,
                                    MemoryBlock& out) const noexcept override;
    [[nodiscard]] bool TryResolve(HostPhysicalAddress address, MemoryBlock& out,
                                  std::size_t& offset) const noexcept override;

    [[nodiscard]] MemoryResult Read(HostPhysicalAddress address,
                                    std::span<std::byte> destination) const override;
    [[nodiscard]] MemoryResult Write(HostPhysicalAddress address,
                                     std::span<const std::byte> source) override;

    [[nodiscard]] std::size_t AllocatedBytes() const noexcept override;
    [[nodiscard]] std::size_t AllocationCount() const noexcept override;

    void SetMaxBytes(std::size_t max_bytes);
    [[nodiscard]] std::size_t MaxBytes() const noexcept;
    void SetFailNextAllocation(bool fail) noexcept;
    void SetFailAllocations(bool fail) noexcept;
    [[nodiscard]] std::size_t HighWaterMark() const noexcept;
    [[nodiscard]] std::vector<MemoryBlock> SnapshotAllocations() const;

private:
    struct Allocation {
        MemoryBlock block{};
        std::size_t alignment{0};
    };

    [[nodiscard]] MemoryResult ReleaseAllLocked() noexcept;
    [[nodiscard]] bool TryResolveLocked(std::uint64_t address, MemoryBlock& out,
                                        std::size_t& offset) const noexcept;
    [[nodiscard]] static MemoryResult ValidateAlignment(std::size_t alignment);

    mutable std::mutex mutex_;
    bool initialized_{true};
    bool fail_next_{false};
    bool fail_all_{false};
    std::size_t max_bytes_{kDefaultMaxBytes};
    std::size_t allocated_bytes_{0};
    std::size_t high_water_mark_{0};
    std::uint64_t next_address_{kFirstSyntheticHostAddress};
    std::map<std::uint64_t, Allocation> allocations_;
};

} // namespace hv::memory
