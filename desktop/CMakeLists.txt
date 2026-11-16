# Desktop side: SentinelFetch (control panel + command line).
# Needs libcurl, GDAL and nlohmann-json. Try each package's CMake config first
# (vcpkg provides them), then CMake's built-in finders (MSYS2 may not ship
# configs for curl/GDAL).
find_package(Threads REQUIRED)

find_package(CURL CONFIG QUIET)
if(NOT TARGET CURL::libcurl)
    find_package(CURL REQUIRED)
endif()

find_package(GDAL CONFIG QUIET)
if(NOT TARGET GDAL::GDAL)
    find_package(GDAL REQUIRED)
endif()

find_package(nlohmann_json CONFIG REQUIRED)

# Everything except the curl/GDAL data source: colour grading, the local web
# server and the control-panel logic. Also used by the tests.
add_library(sv_core STATIC grade.cpp httpd.cpp app.cpp)
target_include_directories(sv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sv_core PUBLIC svd Threads::Threads)
if(WIN32)
    target_link_libraries(sv_core PUBLIC ws2_32)
endif()

add_executable(SentinelFetch main.cpp source_stac.cpp)
target_link_libraries(SentinelFetch PRIVATE
    sv_core
    CURL::libcurl
    GDAL::GDAL
    nlohmann_json::nlohmann_json
)
if(WIN32)
    target_compile_definitions(SentinelFetch PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(SentinelFetch PRIVATE /utf-8)
endif()
if(MINGW AND SVD_STATIC_RUNTIME)
    target_link_options(SentinelFetch PRIVATE -static-libgcc -static-libstdc++)
endif()

if(SVD_BUILD_TESTS)
    add_executable(grade_test ../tests/grade_test.cpp)
    target_link_libraries(grade_test PRIVATE sv_core)
    add_test(NAME grade COMMAND grade_test)

    add_executable(ui_fake ../tests/ui_fake_main.cpp)
    target_link_libraries(ui_fake PRIVATE sv_core)
endif()
