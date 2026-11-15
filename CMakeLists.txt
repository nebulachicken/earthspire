cmake_minimum_required(VERSION 3.21)
project(CapeCanaveralSentinel VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SVD_BUILD_DESKTOP "Build the desktop downloader (needs curl, GDAL, nlohmann-json)" ON)
option(SVD_BUILD_TESTS   "Build the host-side tests" OFF)

# Shared, portable container + renderer (also compiled by the calculator Makefile).
add_library(svd STATIC common/svd_format.c common/svd_render.c)
target_include_directories(svd PUBLIC common)

if(SVD_BUILD_DESKTOP)
    # The manifest (vcpkg.json) lives in this top-level directory, so vcpkg's
    # manifest mode picks it up when the toolchain file is passed.
    add_subdirectory(desktop)
endif()

if(SVD_BUILD_TESTS)
    enable_testing()
    add_executable(test_svd tests/test_svd.c)
    target_link_libraries(test_svd PRIVATE svd)
    add_test(NAME svd COMMAND test_svd)
endif()
