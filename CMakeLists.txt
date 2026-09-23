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
    src/hls_proxy.cpp
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
# curl-impersonate when found: MissAV's pages and stream CDN reject stock
# libcurl's TLS fingerprint. Headers come from libcurl either way (it is ABI-
# compatible; its extra entry point is looked up at run time). Consumers must
# link avcui::curl rather than libcurl themselves — two libcurls in one process
# would leave Http::impersonate() disabled.
option(AVCUI_CURL_IMPERSONATE "Link curl-impersonate instead of libcurl when found" ON)
if(AVCUI_CURL_IMPERSONATE)
    # MacPorts' curl-impersonate port installs /opt/local/lib/libcurl-impersonate.dylib.
    find_library(CURL_IMPERSONATE_LIBRARY NAMES curl-impersonate
                 HINTS "$ENV{HOME}/.local/lib" /opt/local/lib /opt/homebrew/lib /usr/local/lib)
endif()
add_library(avcui_curl INTERFACE)
add_library(avcui::curl ALIAS avcui_curl)
if(AVCUI_CURL_IMPERSONATE AND CURL_IMPERSONATE_LIBRARY)
    message(STATUS "avcui: using curl-impersonate (${CURL_IMPERSONATE_LIBRARY})")
    target_include_directories(avcui_curl INTERFACE ${CURL_INCLUDE_DIRS})
    # curl-impersonate builds may carry a static LLVM libunwind and EXPORT its
    # _Unwind_* entry points. On ELF those interpose libgcc_s's for the whole
    # process, and then every C++ exception terminates even when caught.
    # Linking libgcc_s first puts its unwinder ahead in symbol lookup. Mach-O
    # binds symbols to the library they were linked against: not needed there.
    if(NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_libraries(avcui_curl INTERFACE
            "-Wl,--push-state,--no-as-needed" gcc_s "-Wl,--pop-state")
    endif()
    target_link_libraries(avcui_curl INTERFACE "${CURL_IMPERSONATE_LIBRARY}")
else()
    message(WARNING "avcui: curl-impersonate not found; MissAV pages and streams will 403")
    target_link_libraries(avcui_curl INTERFACE PkgConfig::CURL)
endif()

target_link_libraries(avcui_core PUBLIC avcui::curl PkgConfig::CRYPTO Threads::Threads ${CMAKE_DL_LIBS})
