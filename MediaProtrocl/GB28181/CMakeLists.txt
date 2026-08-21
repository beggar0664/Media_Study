cmake_minimum_required(VERSION 3.20)

if (POLICY CMP0148)
    cmake_policy(SET CMP0148 NEW)
endif()
if (NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

project(gb28181_minimal_module LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (WIN32)
    if (CMAKE_GENERATOR MATCHES "Visual Studio")
        set(CMAKE_GENERATOR_PLATFORM "Win32" CACHE STRING "Platform" FORCE)
    endif()
    if (CMAKE_GENERATOR MATCHES "Ninja")
        set(CMAKE_VS_PLATFORM_NAME "Win32" CACHE STRING "Platform" FORCE)
    endif()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MACHINE:X86")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MACHINE:X86")
endif()

set(GB28181_OUTPUT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/out)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${GB28181_OUTPUT_ROOT}/static_lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${GB28181_OUTPUT_ROOT}/dll_lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${GB28181_OUTPUT_ROOT}/buildBin)

option(GB28181_BUILD_LOCAL_THIRD_PARTY "Build bundled jthread and jrtplib sources" ON)

if (GB28181_BUILD_LOCAL_THIRD_PARTY)
    set(JRTPLIB_COMPILE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(JRTPLIB_COMPILE_TESTS OFF CACHE BOOL "" FORCE)
    set(JTHREAD_ENABLED OFF CACHE BOOL "" FORCE)
    add_subdirectory(jrtplib-3.11.2 EXCLUDE_FROM_ALL)
    set(GB28181_JRTPLIB_TARGET jrtplib-static)
else()
    find_package(JRTPLIB REQUIRED)
    set(GB28181_JRTPLIB_TARGET ${JRTPLIB_LIBRARIES})
endif()

add_library(gb28181_module STATIC
    gb28181_module.cpp
)

target_include_directories(gb28181_module PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if (GB28181_BUILD_LOCAL_THIRD_PARTY)
    target_include_directories(gb28181_module PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/jrtplib-3.11.2/src
        ${CMAKE_CURRENT_BINARY_DIR}/jrtplib-3.11.2/src
    )
endif()

if (MSVC)
    target_compile_options(gb28181_module PRIVATE /utf-8)
endif()

target_link_libraries(gb28181_module PRIVATE ${GB28181_JRTPLIB_TARGET})

add_executable(gb28181_minimal_example
    gb28181_minimal_example.cpp
)

if (MSVC)
    target_compile_options(gb28181_minimal_example PRIVATE /utf-8)
endif()

target_link_libraries(gb28181_minimal_example PRIVATE gb28181_module)

add_executable(gb28181_sip_register_client
    gb28181_sip_register_client.cpp
)

if (MSVC)
    target_compile_options(gb28181_sip_register_client PRIVATE /utf-8)
endif()

target_link_libraries(gb28181_sip_register_client PRIVATE gb28181_module ws2_32)

add_executable(gb28181_sip_mock_server
    gb28181_sip_mock_server.cpp
)

if (MSVC)
    target_compile_options(gb28181_sip_mock_server PRIVATE /utf-8)
endif()

target_link_libraries(gb28181_sip_mock_server PRIVATE gb28181_module ws2_32)

add_executable(gb28181_device_stateful
    gb28181_device_stateful.cpp
)

if (MSVC)
    target_compile_options(gb28181_device_stateful PRIVATE /utf-8)
endif()

target_link_libraries(gb28181_device_stateful PRIVATE gb28181_module ws2_32)
