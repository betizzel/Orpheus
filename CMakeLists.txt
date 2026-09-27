cmake_minimum_required(VERSION 3.16)
project(orpheus VERSION 1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_TESTS "Enable unit tests" OFF)
option(ENABLE_COVERAGE "Enable code coverage" OFF)
option(ENABLE_SANITIZERS "Enable sanitizers for debugging" OFF)
# AUTO: use FFmpeg when it is installed. ON forces it (hard error when missing),
# OFF builds a cut-down player that only decodes mp3/flac/wav.
set(ENABLE_FFMPEG "AUTO" CACHE STRING "FFmpeg decoding backend for M4A/AAC/ALAC/Opus/OGG (AUTO|ON|OFF)")
set_property(CACHE ENABLE_FFMPEG PROPERTY STRINGS AUTO ON OFF)

if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/ncurses")
    set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/opt/ncurses/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES IMPORTED_TARGET ncursesw)
if(NOT NCURSES_FOUND)
    pkg_check_modules(NCURSES REQUIRED IMPORTED_TARGET ncurses)
endif()
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(LUA IMPORTED_TARGET lua)
if(NOT LUA_FOUND)
    # pkg_search_module takes the FIRST match; pkg_check_modules would demand
    # every listed module (Debian/Ubuntu ship lua5.4.pc but no lua.pc).
    pkg_search_module(LUA REQUIRED IMPORTED_TARGET lua5.4 lua-5.4 lua54 lua5.3 lua-5.3 lua53)
endif()

# Optional FFmpeg for AAC/ALAC/M4A/Opus/OGG/WMA
set(ORPHEUS_HAVE_FFMPEG OFF)
if(NOT ENABLE_FFMPEG STREQUAL "OFF")
    if(ENABLE_FFMPEG STREQUAL "ON")
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswresample)
    else()
        pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavformat libavutil libswresample)
    endif()
    if(FFMPEG_FOUND)
        set(ORPHEUS_HAVE_FFMPEG ON)
    endif()
endif()

# Sources shared by the player and the server.
set(CORE_SOURCES
    src/library/library.cpp
    src/net/proto.cpp
    src/ui/art.cpp
    src/util/log.cpp
    src/util/util.cpp
)

add_library(orpheus_core STATIC ${CORE_SOURCES})
target_include_directories(orpheus_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(orpheus_core PUBLIC PkgConfig::TAGLIB)

# The TUI client. Decodes and plays; audio always comes out here.
set(CLIENT_SOURCES
    src/main.cpp
    src/ui/ui.cpp
    src/ui/visualizer.cpp
    src/net/remote.cpp
    src/net/source.cpp
    src/player/player.cpp
    src/player/ffdecoder.cpp
    src/util/config.cpp
    src/util/playlist.cpp
    src/util/miniaudio.c
)

add_executable(orpheus ${CLIENT_SOURCES})
target_link_libraries(orpheus PRIVATE
    orpheus_core
    PkgConfig::NCURSES
    PkgConfig::LUA
)

# The library server. Never decodes, never plays: no miniaudio, no ncurses.
add_executable(orpheusd
    src/orpheusd.cpp
    src/net/server.cpp
)
target_link_libraries(orpheusd PRIVATE orpheus_core)

find_package(Threads REQUIRED)
target_link_libraries(orpheus_core PUBLIC Threads::Threads)

if(ORPHEUS_HAVE_FFMPEG)
    target_compile_definitions(orpheus_core PUBLIC ORPHEUS_FFMPEG)
    target_link_libraries(orpheus PRIVATE PkgConfig::FFMPEG)
    message(STATUS "Orpheus: FFmpeg decoding backend ENABLED (m4a/aac/alac/opus/ogg/wma)")
else()
    message(STATUS "Orpheus: FFmpeg decoding backend DISABLED (mp3/flac/wav only)")
endif()

if(APPLE)
    target_link_libraries(orpheus PRIVATE
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework AudioToolbox"
    )
    target_link_libraries(orpheus PRIVATE pthread m)
endif()

foreach(tgt orpheus orpheusd orpheus_core)
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

# miniaudio.c and the bundled stb headers are vendored: their warnings are not
# actionable and would drown out ours.
set_source_files_properties(src/util/miniaudio.c PROPERTIES COMPILE_OPTIONS "-w")

set_target_properties(orpheus orpheusd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS orpheus orpheusd DESTINATION bin)

if(ENABLE_TESTS)
    enable_testing()
endif()

if(ENABLE_COVERAGE)
    target_compile_options(orpheus PRIVATE --coverage)
    target_link_options(orpheus PRIVATE --coverage)
endif()

if(ENABLE_SANITIZERS)
    target_compile_options(orpheus PRIVATE -fsanitize=address -fsanitize=undefined)
    target_link_options(orpheus PRIVATE -fsanitize=address -fsanitize=undefined)
endif()
