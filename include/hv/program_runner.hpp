#pragma once
#include "hv/status.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hv {

struct ProgramSpec {
    // UTF-8 executable path and arguments. Arguments are passed directly, not through a shell.
    std::string executable;
    std::vector<std::string> arguments;
    std::string working_directory;
};

struct ProcessHandle { std::uint64_t value{}; };

struct ProcessInfo {
    ProcessHandle handle{};
    std::uint64_t native_process_id{};
    bool running{false};
    bool has_exit_code{false};
    std::int64_t exit_code{};
};

// Starts ordinary child processes in the host OS. This is not guest-OS execution.
class ProgramRunner final {
public:
    ProgramRunner();
    ~ProgramRunner();
    ProgramRunner(const ProgramRunner&) = delete;
    ProgramRunner& operator=(const ProgramRunner&) = delete;
    ProgramRunner(ProgramRunner&&) noexcept;
    ProgramRunner& operator=(ProgramRunner&&) noexcept;

    [[nodiscard]] Result Launch(const ProgramSpec& spec, ProcessInfo& out);
    [[nodiscard]] Result Query(ProcessHandle handle, ProcessInfo& out);
    [[nodiscard]] Result Wait(ProcessHandle handle, std::chrono::milliseconds timeout,
                              ProcessInfo& out);
    [[nodiscard]] Result Terminate(ProcessHandle handle, std::uint32_t exit_code = 1);
    // A process record can be forgotten only after the child has exited.
    [[nodiscard]] Result Forget(ProcessHandle handle);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hv
