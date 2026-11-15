find_package(CURL CONFIG REQUIRED)
find_package(GDAL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(SentinelFetch main.cpp)
target_link_libraries(SentinelFetch PRIVATE
    svd
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
