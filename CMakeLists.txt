cmake_minimum_required(VERSION 3.20)
project(GeraduoHypervisorCore VERSION 0.4.0 LANGUAGES CXX)

option(HV_BUILD_TESTS "Build unit tests" ON)
option(HV_BUILD_DEMO "Build the architecture demo" ON)
option(HV_BUILD_LAUNCHER "Build the host process launcher" ON)
option(HV_BUILD_PREFLIGHT "Build the hv_preflight CPUID report tool" ON)
option(HV_BUILD_PLATFORM_DIAGNOSTICS "Build the Windows platform diagnostics tool" ON)
option(HV_BUILD_WDK_DRIVER "Expose the separate WDK driver build target" OFF)

function(hv_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

add_library(hv_core
    src/lifecycle.cpp
    src/page_table.cpp
    src/nested_paging.cpp
    src/memory_manager.cpp
    src/hypervisor.cpp
    src/diagnostics.cpp
    src/program_runner.cpp
    src/platform/cpuid_native.cpp
    src/platform/preflight.cpp
    src/platform/preflight_report.cpp
    src/platform/windows/windows_platform_provider.cpp
    src/platform/windows/windows_platform_backend.cpp
    src/platform/windows/cpu_capability_manager.cpp
    src/platform/windows/platform_compatibility_manager.cpp
    src/platform/windows/kernel_driver_interface.cpp
    src/platform/windows/driver_diagnostics.cpp
)
target_include_directories(hv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hv_core PUBLIC cxx_std_20)
hv_set_warnings(hv_core)

if(WIN32)
    target_link_libraries(hv_core PRIVATE Advapi32 Setupapi)
endif()

if(HV_BUILD_DEMO)
    add_executable(hv_demo examples/demo.cpp)
    target_link_libraries(hv_demo PRIVATE hv_core)
endif()

if(HV_BUILD_LAUNCHER)
    add_executable(hv_launch examples/launch.cpp)
    target_link_libraries(hv_launch PRIVATE hv_core)
endif()

if(HV_BUILD_PREFLIGHT)
    add_executable(hv_preflight examples/preflight.cpp)
    target_link_libraries(hv_preflight PRIVATE hv_core)
endif()

if(HV_BUILD_PLATFORM_DIAGNOSTICS)
    add_executable(hv_platform_diagnostics examples/platform_diagnostics.cpp)
    target_link_libraries(hv_platform_diagnostics PRIVATE hv_core)
    hv_set_warnings(hv_platform_diagnostics)
endif()

if(HV_BUILD_WDK_DRIVER)
    add_subdirectory(driver)
endif()

if(HV_BUILD_TESTS)
    enable_testing()

    add_executable(hv_tests tests/test_main.cpp)
    target_link_libraries(hv_tests PRIVATE hv_core)
    hv_set_warnings(hv_tests)
    add_test(NAME hv_tests COMMAND hv_tests)

    add_executable(hv_preflight_tests tests/test_preflight.cpp)
    target_link_libraries(hv_preflight_tests PRIVATE hv_core)
    hv_set_warnings(hv_preflight_tests)
    add_test(NAME hv_preflight_tests COMMAND hv_preflight_tests)

    add_executable(hv_windows_platform_tests tests/test_windows_platform.cpp)
    target_link_libraries(hv_windows_platform_tests PRIVATE hv_core)
    hv_set_warnings(hv_windows_platform_tests)
    add_test(NAME hv_windows_platform_tests COMMAND hv_windows_platform_tests)

    add_executable(hv_platform_compatibility_tests tests/test_platform_compatibility.cpp)
    target_link_libraries(hv_platform_compatibility_tests PRIVATE hv_core)
    hv_set_warnings(hv_platform_compatibility_tests)
    add_test(NAME hv_platform_compatibility_tests COMMAND hv_platform_compatibility_tests)
endif()
