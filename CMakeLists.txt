cmake_minimum_required(VERSION 3.20)
project(GeraduoHypervisorCore VERSION 0.5.0 LANGUAGES CXX)

option(HV_BUILD_TESTS "Build unit tests" ON)
option(HV_BUILD_DEMO "Build the architecture demo" ON)
option(HV_BUILD_LAUNCHER "Build the host process launcher" ON)
option(HV_BUILD_PREFLIGHT "Build the hv_preflight CPUID report tool" ON)
option(HV_BUILD_WINDOWS_PLATFORM "Build the Windows Platform Backend library" ON)
option(HV_BUILD_PLATFORM_DIAGNOSTICS "Build hv_platform_diagnostics.exe" ON)
option(HV_BUILD_VIRTUALIZATION "Build the Intel VT-x virtualization foundation" ON)
option(HV_BUILD_VMX_DIAGNOSTICS "Build hv_vmx_diagnostics.exe" ON)
option(HV_BUILD_DRIVER "Build the WDK kernel driver through MSBuild (requires WDK)" OFF)

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
)
target_include_directories(hv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hv_core PUBLIC cxx_std_20)
hv_set_warnings(hv_core)

if(HV_BUILD_WINDOWS_PLATFORM)
    add_library(hv_platform
        src/platform/windows/windows_platform_backend.cpp
        src/platform/windows/cpu_capability_manager.cpp
        src/platform/windows/platform_compatibility_manager.cpp
        src/platform/windows/kernel_driver_interface.cpp
        src/platform/windows/driver_diagnostics.cpp
    )
    target_include_directories(hv_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(hv_platform PUBLIC cxx_std_20)
    target_link_libraries(hv_platform PUBLIC hv_core)
    if(WIN32)
        target_compile_definitions(hv_platform PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(hv_platform PRIVATE setupapi advapi32)
    endif()
    hv_set_warnings(hv_platform)
endif()

if(HV_BUILD_VIRTUALIZATION AND HV_BUILD_WINDOWS_PLATFORM)
    add_library(hv_virtualization
        src/virtualization/intel/vmx_capability_manager.cpp
        src/virtualization/intel/vmx_readiness_manager.cpp
        src/virtualization/intel/vmx_lifecycle_controller.cpp
        src/virtualization/intel/vmx_diagnostics.cpp
    )
    target_include_directories(hv_virtualization PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(hv_virtualization PUBLIC cxx_std_20)
    target_link_libraries(hv_virtualization PUBLIC hv_platform)
    if(WIN32)
        target_compile_definitions(hv_virtualization PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _WIN32_WINNT=0x0A00
        )
    endif()
    hv_set_warnings(hv_virtualization)
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

if(HV_BUILD_PLATFORM_DIAGNOSTICS AND HV_BUILD_WINDOWS_PLATFORM)
    add_executable(hv_platform_diagnostics examples/platform_diagnostics.cpp)
    target_link_libraries(hv_platform_diagnostics PRIVATE hv_platform)
    if(WIN32)
        target_compile_definitions(hv_platform_diagnostics PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _WIN32_WINNT=0x0A00
        )
    endif()
    hv_set_warnings(hv_platform_diagnostics)
endif()

if(HV_BUILD_VMX_DIAGNOSTICS AND HV_BUILD_VIRTUALIZATION AND HV_BUILD_WINDOWS_PLATFORM)
    add_executable(hv_vmx_diagnostics examples/vmx_diagnostics.cpp)
    target_link_libraries(hv_vmx_diagnostics PRIVATE hv_virtualization)
    if(WIN32)
        target_compile_definitions(hv_vmx_diagnostics PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _WIN32_WINNT=0x0A00
        )
    endif()
    hv_set_warnings(hv_vmx_diagnostics)
endif()

if(HV_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_executable(hv_tests tests/test_main.cpp)
    target_link_libraries(hv_tests PRIVATE hv_core)
    hv_set_warnings(hv_tests)
    add_test(NAME hv_tests COMMAND hv_tests)

    add_executable(hv_preflight_tests tests/test_preflight.cpp)
    target_link_libraries(hv_preflight_tests PRIVATE hv_core)
    hv_set_warnings(hv_preflight_tests)
    add_test(NAME hv_preflight_tests COMMAND hv_preflight_tests)

    if(HV_BUILD_WINDOWS_PLATFORM)
        add_executable(hv_windows_platform_tests tests/test_windows_platform.cpp)
        target_link_libraries(hv_windows_platform_tests PRIVATE hv_platform)
        hv_set_warnings(hv_windows_platform_tests)
        add_test(NAME hv_windows_platform_tests COMMAND hv_windows_platform_tests)

        add_executable(hv_platform_compatibility_tests tests/test_platform_compatibility.cpp)
        target_link_libraries(hv_platform_compatibility_tests PRIVATE hv_platform)
        hv_set_warnings(hv_platform_compatibility_tests)
        add_test(NAME hv_platform_compatibility_tests COMMAND hv_platform_compatibility_tests)

        if(HV_BUILD_VIRTUALIZATION)
            add_executable(hv_vmx_capability_tests tests/test_vmx_capabilities.cpp)
            target_link_libraries(hv_vmx_capability_tests PRIVATE hv_virtualization Threads::Threads)
            hv_set_warnings(hv_vmx_capability_tests)
            add_test(NAME hv_vmx_capability_tests COMMAND hv_vmx_capability_tests)

            add_executable(hv_vmx_readiness_tests tests/test_vmx_readiness.cpp)
            target_link_libraries(hv_vmx_readiness_tests PRIVATE hv_virtualization Threads::Threads)
            hv_set_warnings(hv_vmx_readiness_tests)
            add_test(NAME hv_vmx_readiness_tests COMMAND hv_vmx_readiness_tests)

            add_executable(hv_vmx_lifecycle_tests tests/test_vmx_lifecycle.cpp)
            target_link_libraries(hv_vmx_lifecycle_tests PRIVATE hv_virtualization Threads::Threads)
            hv_set_warnings(hv_vmx_lifecycle_tests)
            add_test(NAME hv_vmx_lifecycle_tests COMMAND hv_vmx_lifecycle_tests)
        endif()
    endif()
endif()

if(HV_BUILD_DRIVER)
    add_subdirectory(driver)
endif()
