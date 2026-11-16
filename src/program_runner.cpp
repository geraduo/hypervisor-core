#include "hv/program_runner.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hv {
namespace {
#ifdef _WIN32
bool Utf8ToWide(const std::string& input, std::wstring& output) {
    if (input.empty()) { output.clear(); return true; }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                               static_cast<int>(input.size()), output.data(), count) == count;
}

std::wstring QuoteWindowsArgument(const std::wstring& arg) {
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') { ++backslashes; continue; }
        if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}
#endif
} // namespace

struct ProgramRunner::Impl {
    struct Record {
        ProcessInfo info;
#ifdef _WIN32
        HANDLE process{nullptr};
#else
        pid_t pid{-1};
#endif
    };
    std::mutex mutex;
    std::map<std::uint64_t, Record> records;
    std::uint64_t next_id{1};

    ~Impl() {
#ifdef _WIN32
        for (auto& [id, record] : records) {
            (void)id;
            if (record.process != nullptr) CloseHandle(record.process);
        }
#endif
    }
};

ProgramRunner::ProgramRunner() : impl_(std::make_unique<Impl>()) {}
ProgramRunner::~ProgramRunner() = default;
ProgramRunner::ProgramRunner(ProgramRunner&&) noexcept = default;
ProgramRunner& ProgramRunner::operator=(ProgramRunner&&) noexcept = default;

Result ProgramRunner::Launch(const ProgramSpec& spec, ProcessInfo& out) {
    if (!impl_) return Result::Error(Status::InvalidState, "Runner has been moved from");
    if (spec.executable.empty()) return Result::Error(Status::InvalidArgument, "Executable path is empty");

    Impl::Record record;
#ifdef _WIN32
    std::wstring executable;
    std::wstring working_directory;
    if (!Utf8ToWide(spec.executable, executable))
        return Result::Error(Status::InvalidArgument, "Executable path is not valid UTF-8");
    if (!spec.working_directory.empty() && !Utf8ToWide(spec.working_directory, working_directory))
        return Result::Error(Status::InvalidArgument, "Working directory is not valid UTF-8");

    std::wstring command = QuoteWindowsArgument(executable);
    for (const auto& arg : spec.arguments) {
        std::wstring wide_arg;
        if (!Utf8ToWide(arg, wide_arg))
            return Result::Error(Status::InvalidArgument, "An argument is not valid UTF-8");
        command.push_back(L' ');
        command += QuoteWindowsArgument(wide_arg);
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};
    const wchar_t* cwd = spec.working_directory.empty() ? nullptr : working_directory.c_str();
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, cwd, &startup, &process_info)) {
        const auto error = GetLastError();
        return Result::Error(Status::InitializationFailed,
                             "CreateProcessW failed with Win32 error " + std::to_string(error));
    }
    CloseHandle(process_info.hThread);
    record.process = process_info.hProcess;
    record.info.native_process_id = process_info.dwProcessId;
#else
    std::vector<std::string> args;
    args.reserve(spec.arguments.size() + 1);
    args.push_back(spec.executable);
    args.insert(args.end(), spec.arguments.begin(), spec.arguments.end());
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) return Result::Error(Status::InitializationFailed, "fork failed: " + std::to_string(errno));
    if (pid == 0) {
        if (!spec.working_directory.empty() && chdir(spec.working_directory.c_str()) != 0) _exit(126);
        execvp(spec.executable.c_str(), argv.data());
        _exit(127);
    }
    record.pid = pid;
    record.info.native_process_id = static_cast<std::uint64_t>(pid);
#endif

    {
        std::lock_guard lock(impl_->mutex);
        record.info.handle.value = impl_->next_id++;
        record.info.running = true;
        out = record.info;
        impl_->records.emplace(record.info.handle.value, std::move(record));
    }
    return Result::Ok();
}

Result ProgramRunner::Query(ProcessHandle handle, ProcessInfo& out) {
    if (!impl_) return Result::Error(Status::InvalidState, "Runner has been moved from");
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->records.find(handle.value);
    if (it == impl_->records.end()) return Result::Error(Status::NotFound, "Process handle not found");
    auto& record = it->second;
    if (record.info.running) {
#ifdef _WIN32
        const DWORD wait_result = WaitForSingleObject(record.process, 0);
        if (wait_result == WAIT_OBJECT_0) {
            DWORD code = 0;
            if (!GetExitCodeProcess(record.process, &code))
                return Result::Error(Status::InternalError, "GetExitCodeProcess failed");
            record.info.running = false;
            record.info.has_exit_code = true;
            record.info.exit_code = static_cast<std::int64_t>(code);
        } else if (wait_result == WAIT_FAILED) {
            return Result::Error(Status::InternalError, "WaitForSingleObject failed");
        }
#else
        int status = 0;
        const pid_t result = waitpid(record.pid, &status, WNOHANG);
        if (result == record.pid) {
            record.info.running = false;
            record.info.has_exit_code = true;
            if (WIFEXITED(status)) record.info.exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) record.info.exit_code = 128 + WTERMSIG(status);
            else record.info.exit_code = -1;
        } else if (result < 0 && errno != EINTR) {
            return Result::Error(Status::InternalError, "waitpid failed: " + std::to_string(errno));
        }
#endif
    }
    out = record.info;
    return Result::Ok();
}

Result ProgramRunner::Wait(ProcessHandle handle, std::chrono::milliseconds timeout, ProcessInfo& out) {
    if (timeout.count() < 0) return Result::Error(Status::InvalidArgument, "Timeout cannot be negative");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        const auto query = Query(handle, out);
        if (!query.Succeeded()) return query;
        if (!out.running) return Result::Ok();
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (true);
    return Result::Error(Status::Timeout, "Timed out while waiting for child process");
}

Result ProgramRunner::Terminate(ProcessHandle handle, std::uint32_t exit_code) {
    if (!impl_) return Result::Error(Status::InvalidState, "Runner has been moved from");
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->records.find(handle.value);
    if (it == impl_->records.end()) return Result::Error(Status::NotFound, "Process handle not found");
    auto& record = it->second;
    if (!record.info.running) return Result::Ok();
#ifdef _WIN32
    if (!TerminateProcess(record.process, static_cast<UINT>(exit_code)))
        return Result::Error(Status::ShutdownFailed, "TerminateProcess failed with Win32 error " + std::to_string(GetLastError()));
#else
    (void)exit_code;
    if (kill(record.pid, SIGTERM) != 0 && errno != ESRCH)
        return Result::Error(Status::ShutdownFailed, "kill failed: " + std::to_string(errno));
#endif
    return Result::Ok();
}

Result ProgramRunner::Forget(ProcessHandle handle) {
    if (!impl_) return Result::Error(Status::InvalidState, "Runner has been moved from");
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->records.find(handle.value);
    if (it == impl_->records.end()) return Result::Error(Status::NotFound, "Process handle not found");
    if (it->second.info.running) return Result::Error(Status::InvalidState, "Cannot forget a running process");
#ifdef _WIN32
    if (it->second.process != nullptr) CloseHandle(it->second.process);
#endif
    impl_->records.erase(it);
    return Result::Ok();
}

} // namespace hv
