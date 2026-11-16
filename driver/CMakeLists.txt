if(NOT WIN32)
    message(FATAL_ERROR "The Geraduo WDM driver can only be built on Windows with the WDK.")
endif()

if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    message(FATAL_ERROR "Use a Visual Studio generator when HV_BUILD_WDK_DRIVER is enabled.")
endif()

set(HV_MSBUILD_EXECUTABLE "${CMAKE_VS_MSBUILD_COMMAND}")
if(NOT HV_MSBUILD_EXECUTABLE)
    find_program(HV_MSBUILD_EXECUTABLE MSBuild.exe)
endif()
if(NOT HV_MSBUILD_EXECUTABLE)
    message(FATAL_ERROR "MSBuild.exe was not found. Install Visual Studio 2022 and the WDK.")
endif()

add_custom_target(geraduo_wdm_driver
    COMMAND "${HV_MSBUILD_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/GeraduoDriver.vcxproj"
            /m
            "/p:Configuration=$<CONFIG>"
            /p:Platform=x64
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building Geraduo WDM driver with Visual Studio and WDK"
    VERBATIM
)