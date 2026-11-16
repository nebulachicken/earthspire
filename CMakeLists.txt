cmake_minimum_required(VERSION 3.21)
project(SentinelViewer VERSION 0.4.2 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SVD_BUILD_DESKTOP  "Build the desktop app (needs curl, GDAL, nlohmann-json)" ON)
option(SVD_BUILD_TESTS    "Build the host-side tests" OFF)
option(SVD_STATIC_RUNTIME "MinGW only: link libgcc/libstdc++ statically" OFF)

# Shared, portable container + renderer (the calculator Makefile compiles the
# same files). Plain C99, standard library only.
add_library(svd STATIC common/svd_format.c common/svd_render.c)
target_include_directories(svd PUBLIC common)

if(SVD_BUILD_DESKTOP)
    add_subdirectory(desktop)
endif()

if(SVD_BUILD_TESTS)
    enable_testing()
    add_executable(test_svd tests/test_svd.c)
    target_link_libraries(test_svd PRIVATE svd)
    add_test(NAME svd COMMAND test_svd)
endif()
