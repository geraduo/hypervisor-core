# Optional WDK kernel driver build.
#
# This subdirectory is only processed when the top-level project is configured
# with -DHV_BUILD_DRIVER=ON. The driver is intentionally NOT part of the normal
# user-mode build: it needs the Windows Driver Kit, the WDM driver target and
# administrator rights to install/load.

if(NOT WIN32)
    message(FATAL_ERROR "HV_BUILD_DRIVER is only supported on Windows.")
endif()

set(GERADUO_DRIVER_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/GeraduoDriver.vcxproj")
if(NOT EXISTS "${GERADUO_DRIVER_PROJECT}")
    message(FATAL_ERROR "Missing driver project: ${GERADUO_DRIVER_PROJECT}")
endif()

# Locate MSBuild. With a Visual Studio generator CMAKE_MAKE_PROGRAM already
# points at MSBuild.exe; otherwise fall back to PATH.
set(GERADUO_MSBUILD "")
if(CMAKE_MAKE_PROGRAM AND CMAKE_MAKE_PROGRAM MATCHES "[Mm][Ss][Bb][Uu][Ii][Ll][Dd]\\.exe$")
    set(GERADUO_MSBUILD "${CMAKE_MAKE_PROGRAM}")
else()
    find_program(GERADUO_MSBUILD NAMES MSBuild.exe msbuild.exe)
endif()
if(NOT GERADUO_MSBUILD)
    message(FATAL_ERROR
        "MSBuild.exe was not found. Configure with a Visual Studio generator or run from a "
        "Developer Command Prompt.")
endif()

# Locate km\ntddk.h. The WDK installs headers under the Windows Kits include
# directory. WDKContentRoot is set by the WDK developer environment.
set(_geraduo_wdk_include_candidates)
if(DEFINED ENV{WDKContentRoot})
    list(APPEND _geraduo_wdk_include_candidates "$ENV{WDKContentRoot}/Include")
endif()
if(DEFINED ENV{WindowsSdkDir})
    list(APPEND _geraduo_wdk_include_candidates "$ENV{WindowsSdkDir}/Include")
endif()
if(DEFINED ENV{ProgramFiles_x86})
    list(APPEND _geraduo_wdk_include_candidates "$ENV{ProgramFiles_x86}/Windows Kits/10/Include")
endif()
list(APPEND _geraduo_wdk_include_candidates
    "C:/Program Files (x86)/Windows Kits/10/Include")

set(GERADUO_WDK_FOUND FALSE)
foreach(_root IN LISTS _geraduo_wdk_include_candidates)
    if(EXISTS "${_root}")
        file(GLOB _headers "${_root}/*/km/ntddk.h")
        if(_headers)
            set(GERADUO_WDK_FOUND TRUE)
            list(GET _headers 0 GERADUO_WDK_NTDDK)
            break()
        endif()
    endif()
endforeach()

if(NOT GERADUO_WDK_FOUND)
    message(FATAL_ERROR
        "The Windows Driver Kit (km\\ntddk.h) was not found. Install the WDK matching the "
        "Visual Studio version, then reconfigure. The user-mode build does not need the WDK.")
endif()

message(STATUS "Geraduo driver: WDK header ${GERADUO_WDK_NTDDK}")
message(STATUS "Geraduo driver: MSBuild ${GERADUO_MSBUILD}")

set(GERADUO_DRIVER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/driver")
add_custom_target(GeraduoDriver
    COMMAND "${GERADUO_MSBUILD}" "${GERADUO_DRIVER_PROJECT}"
            /m /nologo
            /p:Configuration=$<CONFIG>
            /p:Platform=x64
            /p:OutDir=${GERADUO_DRIVER_OUTPUT_DIR}/$<CONFIG>/
            /p:IntDir=${GERADUO_DRIVER_OUTPUT_DIR}/obj/$<CONFIG>/
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building GeraduoDriver.sys (WDK, $<CONFIG>)"
    VERBATIM)

# Direct cl.exe/link.exe fallback. Some Visual Studio installations (for
# example VS 2026) do not register the WDK's WindowsKernelModeDriver MSBuild
# platform toolset even when the WDK headers and libraries are installed. The
# PowerShell script performs the same kernel compile/link steps without MSBuild.
find_program(GERADUO_POWERSHELL NAMES powershell pwsh)
if(GERADUO_POWERSHELL)
    add_custom_target(GeraduoDriverDirect
        COMMAND "${GERADUO_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/build-driver.ps1"
                -Configuration $<CONFIG>
                -OutputDir "${CMAKE_BINARY_DIR}/driver/$<CONFIG>"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building GeraduoDriver.sys directly with cl.exe/link.exe (WDK, $<CONFIG>)"
        VERBATIM)
    message(STATUS "Geraduo driver targets: GeraduoDriver (WDK MSBuild), "
                   "GeraduoDriverDirect (cl/link fallback)")
else()
    message(STATUS "Geraduo driver target enabled. PowerShell was not found; "
                   "only the WDK MSBuild target GeraduoDriver is available.")
endif()

message(STATUS
    "Build with: cmake --build <build-dir> --config Release --target GeraduoDriver "
    "or --target GeraduoDriverDirect")
