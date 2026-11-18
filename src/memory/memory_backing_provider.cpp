#include "hv/memory/memory_backing_provider.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace hv::memory {
namespace {

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) noexcept {
    if (alignment == 0) return value;
    const std::uint64_t remainder = value & (alignment - 1ULL);
    if (remainder == 0) return value;
    const std::uint64_t increment = alignment - remainder;
    if (value > std::numeric_limits<std::uint64_t>::max() - increment) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return value + increment;
}

} // namespace

FakeMemoryBackingProvider::FakeMemoryBackingProvider() = default;

FakeMemoryBackingProvider::FakeMemoryBackingProvider(std::size_t max_bytes)
    : max_bytes_(max_bytes) {}

FakeMemoryBackingProvider::~FakeMemoryBackingProvider() {
    std::lock_guard lock(mutex_);
    (void)ReleaseAllLocked();
}

MemoryResult FakeMemoryBackingProvider::Initialize() {
    std::lock_guard lock(mutex_);
    initialized_ = true;
    return MemoryResult::Ok();
}

MemoryResult FakeMemoryBackingProvider::Shutdown() {
    std::lock_guard lock(mutex_);
    const MemoryResult released = ReleaseAllLocked();
    initialized_ = false;
    next_address_ = kFirstSyntheticHostAddress;
    high_water_mark_ = 0;
    return released;
}

bool FakeMemoryBackingProvider::IsInitialized() const noexcept {
    std::lock_guard lock(mutex_);
    return initialized_;
}

MemoryResult FakeMemoryBackingProvider::ValidateAlignment(std::size_t alignment) {
    if (alignment == 0) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Backing alignment must not be zero");
    }
    if (!IsPowerOfTwo(static_cast<std::uint64_t>(alignment))) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Backing alignment must be a power of two");
    }
    if (alignment < alignof(std::max_align_t)) {
        return MemoryResult::Error(MemoryStatus::AlignmentError,
                                   "Backing alignment is smaller than the allocator baseline");
    }
    return MemoryResult::Ok();
}

MemoryBlockResult FakeMemoryBackingProvider::Allocate(std::size_t size, std::size_t alignment) {
    const MemoryResult alignment_check = ValidateAlignment(alignment);
    if (!alignment_check.Succeeded()) {
        return MemoryBlockResult{alignment_check.status, {}, alignment_check.message};
    }
    if (size == 0) {
        return MemoryBlockResult{MemoryStatus::InvalidAddress, {},
                                 "Backing allocation size must not be zero"};
    }

    std::lock_guard lock(mutex_);
    if (!initialized_) {
        return MemoryBlockResult{MemoryStatus::NotInitialized, {},
                                 "Backing provider is shut down"};
    }
    if (fail_all_ || fail_next_) {
        fail_next_ = false;
        return MemoryBlockResult{MemoryStatus::OutOfMemory, {},
                                 "Backing allocation failure was injected"};
    }
    if (allocated_bytes_ > max_bytes_ || size > max_bytes_ - allocated_bytes_) {
        return MemoryBlockResult{MemoryStatus::OutOfMemory, {},
                                 "Backing allocation would exceed the configured limit"};
    }

    const std::uint64_t effective_alignment =
        std::max<std::uint64_t>(static_cast<std::uint64_t>(alignment), kSmallPageSize);
    const std::uint64_t address = AlignUp(next_address_, effective_alignment);
    const std::uint64_t size64 = static_cast<std::uint64_t>(size);
    if (!RangeFits(address, size64, kMaxPhysicalAddress)) {
        return MemoryBlockResult{MemoryStatus::OutOfMemory, {},
                                 "Synthetic host-physical address space is exhausted"};
    }

    void* storage = nullptr;
    try {
        storage = ::operator new(size, std::align_val_t(alignment));
    } catch (const std::bad_alloc&) {
        return MemoryBlockResult{MemoryStatus::OutOfMemory, {},
                                 "The host allocator rejected the backing allocation"};
    } catch (...) {
        return MemoryBlockResult{MemoryStatus::InternalError, {},
                                 "Unexpected exception during backing allocation"};
    }
    if (storage == nullptr) {
        return MemoryBlockResult{MemoryStatus::OutOfMemory, {},
                                 "The host allocator returned no storage"};
    }

    const MemoryBlock block{HostPhysicalAddress{address}, size, static_cast<std::byte*>(storage)};
    const auto next = address + size64;
    next_address_ = next;
    allocated_bytes_ += size;
    high_water_mark_ = std::max(high_water_mark_, allocated_bytes_);
    allocations_.emplace(address, Allocation{block, alignment});
    return MemoryBlockResult{MemoryStatus::Success, block, {}};
}

MemoryResult FakeMemoryBackingProvider::Release(HostPhysicalAddress address) {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
        return MemoryResult::Error(MemoryStatus::NotInitialized,
                                   "Backing provider is shut down");
    }
    const auto it = allocations_.find(address.Value());
    if (it == allocations_.end()) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "No backing allocation starts at this host-physical address");
    }
    ::operator delete(it->second.block.data, std::align_val_t(it->second.alignment));
    allocated_bytes_ -= it->second.block.size;
    allocations_.erase(it);
    return MemoryResult::Ok();
}

MemoryResult FakeMemoryBackingProvider::ReleaseAll() {
    std::lock_guard lock(mutex_);
    return ReleaseAllLocked();
}

MemoryResult FakeMemoryBackingProvider::ReleaseAllLocked() noexcept {
    for (auto& entry : allocations_) {
        ::operator delete(entry.second.block.data, std::align_val_t(entry.second.alignment));
    }
    allocations_.clear();
    allocated_bytes_ = 0;
    return MemoryResult::Ok();
}

bool FakeMemoryBackingProvider::Owns(HostPhysicalAddress address) const noexcept {
    std::lock_guard lock(mutex_);
    return allocations_.contains(address.Value());
}

bool FakeMemoryBackingProvider::TryGetBlock(HostPhysicalAddress address,
                                             MemoryBlock& out) const noexcept {
    std::lock_guard lock(mutex_);
    const auto it = allocations_.find(address.Value());
    if (it == allocations_.end()) return false;
    out = it->second.block;
    return true;
}

bool FakeMemoryBackingProvider::TryResolve(HostPhysicalAddress address, MemoryBlock& out,
                                           std::size_t& offset) const noexcept {
    std::lock_guard lock(mutex_);
    return TryResolveLocked(address.Value(), out, offset);
}

bool FakeMemoryBackingProvider::TryResolveLocked(std::uint64_t value, MemoryBlock& out,
                                                 std::size_t& offset) const noexcept {
    if (allocations_.empty()) return false;
    auto it = allocations_.upper_bound(value);
    if (it == allocations_.begin()) return false;
    --it;
    const MemoryBlock& block = it->second.block;
    const std::uint64_t base = block.address.Value();
    const std::uint64_t size = static_cast<std::uint64_t>(block.size);
    if (value < base || value - base >= size) return false;
    out = block;
    offset = static_cast<std::size_t>(value - base);
    return true;
}

MemoryResult FakeMemoryBackingProvider::Read(HostPhysicalAddress address,
                                             std::span<std::byte> destination) const {
    if (destination.empty()) return MemoryResult::Ok();
    std::lock_guard lock(mutex_);
    if (!initialized_) {
        return MemoryResult::Error(MemoryStatus::NotInitialized,
                                   "Backing provider is shut down");
    }
    MemoryBlock block{};
    std::size_t offset = 0;
    if (!TryResolveLocked(address.Value(), block, offset)) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Host-physical read is outside every backing allocation");
    }
    if (destination.size() > block.size - offset) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Host-physical read crosses the end of its backing allocation");
    }
    std::memcpy(destination.data(), block.data + offset, destination.size());
    return MemoryResult::Ok();
}

MemoryResult FakeMemoryBackingProvider::Write(HostPhysicalAddress address,
                                              std::span<const std::byte> source) {
    if (source.empty()) return MemoryResult::Ok();
    std::lock_guard lock(mutex_);
    if (!initialized_) {
        return MemoryResult::Error(MemoryStatus::NotInitialized,
                                   "Backing provider is shut down");
    }
    MemoryBlock block{};
    std::size_t offset = 0;
    if (!TryResolveLocked(address.Value(), block, offset)) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Host-physical write is outside every backing allocation");
    }
    if (source.size() > block.size - offset) {
        return MemoryResult::Error(MemoryStatus::InvalidAddress,
                                   "Host-physical write crosses the end of its backing allocation");
    }
    std::memcpy(block.data + offset, source.data(), source.size());
    return MemoryResult::Ok();
}

std::size_t FakeMemoryBackingProvider::AllocatedBytes() const noexcept {
    std::lock_guard lock(mutex_);
    return allocated_bytes_;
}

std::size_t FakeMemoryBackingProvider::AllocationCount() const noexcept {
    std::lock_guard lock(mutex_);
    return allocations_.size();
}

void FakeMemoryBackingProvider::SetMaxBytes(std::size_t max_bytes) {
    std::lock_guard lock(mutex_);
    max_bytes_ = max_bytes;
}

std::size_t FakeMemoryBackingProvider::MaxBytes() const noexcept {
    std::lock_guard lock(mutex_);
    return max_bytes_;
}

void FakeMemoryBackingProvider::SetFailNextAllocation(bool fail) noexcept {
    std::lock_guard lock(mutex_);
    fail_next_ = fail;
}

void FakeMemoryBackingProvider::SetFailAllocations(bool fail) noexcept {
    std::lock_guard lock(mutex_);
    fail_all_ = fail;
}

std::size_t FakeMemoryBackingProvider::HighWaterMark() const noexcept {
    std::lock_guard lock(mutex_);
    return high_water_mark_;
}

std::vector<MemoryBlock> FakeMemoryBackingProvider::SnapshotAllocations() const {
    std::lock_guard lock(mutex_);
    std::vector<MemoryBlock> blocks;
    blocks.reserve(allocations_.size());
    for (const auto& entry : allocations_) blocks.push_back(entry.second.block);
    return blocks;
}

} // namespace hv::memory
