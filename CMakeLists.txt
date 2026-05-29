if (WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static")
    set(VCPKG_HOST_TRIPLET "x64-windows-static")
endif ()

cmake_minimum_required(VERSION 3.27.0)
project(BetterVR_Layer VERSION 0.9.16 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only x64 architecture is supported")
endif ()

set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/Cemu")
set(DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/Cemu")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if (WIN32)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set(CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD 1)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD 1)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT BetterVR_Layer)

    add_compile_definitions(NOMINMAX)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:EditAndContinue>$<$<CONFIG:RelWithDebInfo>:ProgramDatabase>")
    add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
endif ()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_path(VULKAN_HEADERS_INCLUDE_DIRS "vk_video/vulkan_video_codec_h264std.h")
find_package(OpenXR CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)

add_subdirectory(src)
if (WIN32)
    add_subdirectory(launcher)
endif ()
