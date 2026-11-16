#include "hv/virtualization/vmx_capability_manager.hpp"

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hv {

#ifdef _WIN32

namespace {

struct ProcessorLocation {
    WORD group{};
    DWORD bit{};
};

Result Locate(std::uint32_t cpu_index, ProcessorLocation& out) {
    const WORD groups = GetActiveProcessorGroupCount();
    if (groups == 0) {
        return Result::Error(Status::InvalidResponse, "GetActiveProcessorGroupCount returned zero");
    }
    DWORD remaining = static_cast<DWORD>(cpu_index);
    for (WORD group = 0; group < groups; ++group) {
        const DWORD in_group = GetActiveProcessorCount(group);
        if (remaining < in_group) {
            out.group = group;
            out.bit = remaining;
            return Result::Ok();
        }
        remaining -= in_group;
    }
    return Result::Error(Status::InvalidArgument, "logical processor index is out of range");
}

} // namespace

Result NativeLogicalProcessorCpuidProvider::GetLogicalProcessorCount(std::uint32_t& out) {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count == 0) {
        return Result::Error(Status::InvalidResponse,
                             "GetActiveProcessorCount returned zero (error " +
                                 std::to_string(GetLastError()) + ")");
    }
    out = static_cast<std::uint32_t>(count);
    return Result::Ok();
}

Result NativeLogicalProcessorCpuidProvider::Query(std::uint32_t cpu_index, std::uint32_t leaf,
                                                  std::uint32_t subleaf, CpuidRegisters& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    ProcessorLocation location;
    const Result located = Locate(cpu_index, location);
    if (!located.Succeeded()) return located;

    GROUP_AFFINITY target{};
    target.Group = location.group;
    target.Mask = static_cast<KAFFINITY>(1) << location.bit;
    GROUP_AFFINITY previous{};
    if (SetThreadGroupAffinity(GetCurrentThread(), &target, &previous) == 0) {
        return Result::Error(Status::AccessDenied,
                             "SetThreadGroupAffinity failed (error " +
                                 std::to_string(GetLastError()) + ")");
    }

    // Verify that the thread really executes on the requested processor. Otherwise the CPUID
    // result would describe a different processor and must not be reported for this index.
    PROCESSOR_NUMBER current{};
    GetCurrentProcessorNumberEx(&current);
    Result query_result;
    if (current.Group != location.group || static_cast<DWORD>(current.Number) != location.bit) {
        query_result = Result::Error(Status::InternalError,
                                     "the thread did not run on the requested logical processor");
    } else {
        query_result = cpuid_.Query(leaf, subleaf, out);
    }

    if (SetThreadGroupAffinity(GetCurrentThread(), &previous, nullptr) == 0) {
        return Result::Error(Status::InternalError,
                             "could not restore the thread affinity (error " +
                                 std::to_string(GetLastError()) + "); the result was discarded");
    }
    return query_result;
}

#else

Result NativeLogicalProcessorCpuidProvider::GetLogicalProcessorCount(std::uint32_t&) {
    return Result::Error(Status::Unsupported,
                         "per-processor CPUID enumeration is implemented for Windows only");
}

Result NativeLogicalProcessorCpuidProvider::Query(std::uint32_t, std::uint32_t, std::uint32_t,
                                                  CpuidRegisters&) {
    return Result::Error(Status::Unsupported,
                         "per-processor CPUID is implemented for Windows only");
}

#endif

} // namespace hv
