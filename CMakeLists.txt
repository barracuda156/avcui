cmake_minimum_required(VERSION 3.16)
project(avcui_core LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL   REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)
find_package(Threads REQUIRED)

add_library(avcui_core STATIC
    src/pornhub.cpp
    src/missav.cpp
    src/config.cpp
    src/player.cpp
    src/mpv_ipc.cpp
)
target_include_directories(avcui_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# nlohmann/json: the vendored copy when present, else the system package
# (same rule as the Makefile's SYSTEM_JSON handling).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/vendor/nlohmann/json.hpp")
    target_include_directories(avcui_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include/vendor")
else()
    find_package(nlohmann_json CONFIG REQUIRED)
    target_link_libraries(avcui_core PUBLIC nlohmann_json::nlohmann_json)
endif()

target_compile_definitions(avcui_core PUBLIC
    $<$<PLATFORM_ID:Darwin>:YTUI_MACOS>
    $<$<PLATFORM_ID:Linux>:YTUI_LINUX>
    $<$<PLATFORM_ID:FreeBSD>:YTUI_FREEBSD>
)
target_link_libraries(avcui_core PUBLIC PkgConfig::CURL PkgConfig::CRYPTO Threads::Threads)
