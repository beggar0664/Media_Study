cmake_minimum_required(VERSION 3.15)

project(live555_minimal_rtsp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W4 /EHsc)
endif()

set(LIVE555_ROOT "E:/code/QT_PRJ/VMS_TOOL/3rdParty/Live555" CACHE PATH "Live555 root directory")
set(LIVE555_INCLUDE_DIR "${LIVE555_ROOT}/include")
set(OPENSSL_ROOT "E:/code/QT_PRJ/VMS_TOOL/3rdParty/OpenSSL/1.1.1l" CACHE PATH "OpenSSL root directory")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT}/include")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(LIVE555_ARCH "x64")
else()
    set(LIVE555_ARCH "x86")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(LIVE555_CONFIG "release")
else()
    set(LIVE555_CONFIG "debug")
endif()

set(LIVE555_LIB_DIR "${LIVE555_ROOT}/lib/${LIVE555_ARCH}/${LIVE555_CONFIG}")
set(LIVE555_LIBS
    "${LIVE555_LIB_DIR}/liveMedia.lib"
    "${LIVE555_LIB_DIR}/groupsock.lib"
    "${LIVE555_LIB_DIR}/BasicUsageEnvironment.lib"
    "${LIVE555_LIB_DIR}/UsageEnvironment.lib"
)

set(OPENSSL_LIB_DIR "${OPENSSL_ROOT}/lib/${LIVE555_ARCH}/${LIVE555_CONFIG}")
set(OPENSSL_LIBS
    "${OPENSSL_LIB_DIR}/libssl.lib"
    "${OPENSSL_LIB_DIR}/libcrypto.lib"
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/out/buildBin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/out/buildBin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/out/buildBin")

add_executable(rtsp_push_server
    rtsp_push_server/main.cpp
)

add_executable(rtsp_pull_client
    rtsp_pull_client/main.cpp
)

foreach(target rtsp_push_server rtsp_pull_client)
    target_include_directories(${target} PRIVATE "${LIVE555_INCLUDE_DIR}" "${OPENSSL_INCLUDE_DIR}")
    target_link_libraries(${target} PRIVATE ${LIVE555_LIBS} ${OPENSSL_LIBS} ws2_32 winmm crypt32)
endforeach()
