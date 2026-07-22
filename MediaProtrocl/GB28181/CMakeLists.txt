cmake_minimum_required(VERSION 3.10)

project(gb28181_minimal_module LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

target_link_libraries(gb28181_module PRIVATE ${GB28181_JRTPLIB_TARGET})

add_executable(gb28181_minimal_example
    gb28181_minimal_example.cpp
)

target_link_libraries(gb28181_minimal_example PRIVATE gb28181_module)
