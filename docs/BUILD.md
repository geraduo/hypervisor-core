# Build Guide

## Requirements

* CMake 3.20+
* C++20 compiler (Visual Studio 2022 on Windows; GCC 12+ / Clang 15+ for the portable build)
* Windows Driver Kit only for the separate `driver/GeraduoDriver.vcxproj`

## User-mode components and tests (Windows, Visual Studio 2022)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Warnings stay enabled and are errors: `/W4 /WX /permissive-` on MSVC, `-Wall -Wextra -Wpedantic -Werror` elsewhere.

## Portable build (Linux / macOS)

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The native Windows providers return `Unsupported` there. Every test uses fakes, so no physical VT-x, driver or Windows is needed.

## Targets added in v0.5.0

| Target | Source | Notes |
|---|---|---|
| `hv_vmx_diagnostics` | `examples/vmx_diagnostics.cpp` | `HV_BUILD_VMX_DIAGNOSTICS` (ON) |
| `hv_vmx_capability_tests` | `tests/test_vmx_capabilities.cpp` | CTest |
| `hv_vmx_readiness_tests` | `tests/test_vmx_readiness.cpp` | CTest |
| `hv_vmx_lifecycle_tests` | `tests/test_vmx_lifecycle.cpp` | CTest, includes concurrency tests |

New library sources (in `hv_core`): `src/virtualization/intel/*.cpp`. `hv_core` now links `Threads::Threads`.
Test fakes: `tests/fakes/fake_vmx.hpp` (reuses `tests/fakes/fake_windows_platform.hpp`).

## Options

`HV_BUILD_TESTS`, `HV_BUILD_DEMO`, `HV_BUILD_LAUNCHER`, `HV_BUILD_PREFLIGHT`, `HV_BUILD_PLATFORM_DIAGNOSTICS`,
`HV_BUILD_VMX_DIAGNOSTICS`, `HV_BUILD_WDK_DRIVER`.

## Running the diagnostics tool

```powershell
.\build\Release\hv_vmx_diagnostics.exe
.\build\Release\hv_vmx_diagnostics.exe --json
```

Exit code `0`: report produced (verdict other than `Failed`); `1`: `Failed` or no report; `2`: bad arguments.
The tool never starts a hypervisor. Without the diagnostic driver installed it reports `Blocked` (driver missing).

## Driver

Not part of the portable build. See `docs/DRIVER_ARCHITECTURE.md` and `docs/DRIVER_INTEGRATION.md`.

## Verification status of this change

The v0.5.0 sources were written against the v0.4.0 headers of the `dev` branch but **were not compiled or executed** in the
environment that produced them (no compiler or CMake available). Run the three commands above on Windows and on a portable
toolchain before relying on the results; compile fixes, if any, should be small.
