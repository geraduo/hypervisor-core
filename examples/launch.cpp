#include "hv/program_runner.hpp"
#include <chrono>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"Usage: hv_launch.exe <program-path> [arguments...]\n";
        return 2;
    }
    auto to_utf8 = [](const wchar_t* value) -> std::string {
        const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) return {};
        std::string output(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(), size, nullptr, nullptr);
        output.pop_back();
        return output;
    };
    const bool detach = std::wstring(argv[1]) == L"--detach";
    const int program_index = detach ? 2 : 1;
    if (argc <= program_index) {
        std::wcerr << L"Usage: hv_launch.exe [--detach] <program-path> [arguments...]\n";
        return 2;
    }
    hv::ProgramSpec spec;
    spec.executable = to_utf8(argv[program_index]);
    for (int i = program_index + 1; i < argc; ++i) spec.arguments.push_back(to_utf8(argv[i]));
#else
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: hv_launch [--detach] <program-path> [arguments...]\n";
        return 2;
    }
    const bool detach = std::string(argv[1]) == "--detach";
    const int program_index = detach ? 2 : 1;
    if (argc <= program_index) {
        std::cerr << "Usage: hv_launch [--detach] <program-path> [arguments...]\n";
        return 2;
    }
    hv::ProgramSpec spec;
    spec.executable = argv[program_index];
    for (int i = program_index + 1; i < argc; ++i) spec.arguments.emplace_back(argv[i]);
#endif
    hv::ProgramRunner runner;
    hv::ProcessInfo process;
    auto result = runner.Launch(spec, process);
    if (!result.Succeeded()) {
        std::cerr << "Launch failed: " << result.message << '\n';
        return 1;
    }
    std::cout << "Started host process. PID=" << process.native_process_id
              << ", handle=" << process.handle.value << '\n';
    if (detach) {
        std::cout << "Detached: the process continues after this launcher exits.\n";
        return 0;
    }
    result = runner.Wait(process.handle, std::chrono::hours(24), process);
    if (!result.Succeeded()) {
        std::cerr << "Wait failed: " << result.message << '\n';
        (void)runner.Terminate(process.handle);
        return 1;
    }
    std::cout << "Process exited with code " << process.exit_code << '\n';
    (void)runner.Forget(process.handle);
    return static_cast<int>(process.exit_code);
}
