# webos.cmake -- LG webOS (ARM, SDL2 + EGL/OpenGL ES 1.x) build branch for
# OptiCraft.
#
# Included from the top of CMakeLists.txt when -DPLATFORM=WEBOS (the webos-*
# presets do this together with the webOS toolchain file), then return()s so
# none of the desktop SDL2/glad/OpenGL configuration runs.
#
#   cmake -B build/webos -DPLATFORM=WEBOS -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/buildroot/toolchainfile.cmake
#   cmake --build build/webos --parallel
#   cmake --build build/webos --target webos-package     # -> dist/*.ipk
#
# Libraries: libGLESv1_CM comes from the webOS sysroot (it lives on the TV, so
# it is not bundled); SDL2 is the pinned webosbrew/SDL-webOS release and IS
# bundled in lib/ because the TV's own libSDL2 may lack the Wayland/xkbcommon
# backends this app needs.

cmake_minimum_required(VERSION 3.21)

include(${CMAKE_SOURCE_DIR}/cmake/SourceSelection.cmake)

set(WEBOS_LIB_DIR "" CACHE PATH "Extra directory searched for libGLESv1_CM")
option(WEBOS_LOWEND_PROFILE "Use the game's PC_LEGACY low-end profile (small chunk radius, budgeted chunk/light work, 16-bit terrain vertices, no particles/rain)" ON)
option(WEBOS_LTO "Link-time optimisation: faster game code, slower and more memory-hungry link" OFF)
set(MC_LOG_LEVEL "0" CACHE STRING "Unified diagnostic verbosity: 0=off, 1=info, 2=debug, 3=trace")
set_property(CACHE MC_LOG_LEVEL PROPERTY STRINGS 0 1 2 3)

# --- compiler sanity ---------------------------------------------------------
# The code base is C++17 and uses <filesystem>; that needs GCC 8 or newer.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8.0)
    message(FATAL_ERROR
        "webOS build: g++ ${CMAKE_CXX_COMPILER_VERSION} is too old for C++17 <filesystem> (need GCC >= 8).\n"
        "Check `arm-webos-linux-gnueabi-g++ --version` in your webOS SDK.")
endif()

# --- sources -----------------------------------------------------------------
# Start from the desktop (PC) tree, then swap the SDL/desktop-GL pieces for the
# webOS ones in src/webos.
mcbeta_collect_platform_sources(WEBOS_SOURCES pc)
file(GLOB_RECURSE _webos_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/webos/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/webos/*.c")
list(APPEND WEBOS_SOURCES ${_webos_sources})

mcbeta_exclude_sources(WEBOS_SOURCES
    "[/\\\\]pc[/\\\\]win32[/\\\\]"
    "[/\\\\]pc[/\\\\]Main\\.cpp$"
    "[/\\\\]pc[/\\\\]CrashHandler\\.cpp$"
    "[/\\\\]pc[/\\\\]lwjgl[/\\\\](Display|GLContext|Keyboard|Mouse)\\.cpp$"
    "[/\\\\]pc[/\\\\]render[/\\\\]PcOpenGlRenderBackend\\.cpp$"
    "[/\\\\]pc[/\\\\]render[/\\\\]d3d9[/\\\\]"
    "[/\\\\]pc[/\\\\]external[/\\\\]miniaudio\\.cpp$"
    "[/\\\\]java[/\\\\]JavaNetwork\\.cpp$"
    # NetCast's platform sources live in the same tree; they are not part of a
    # webOS link.
    "[/\\\\]platform[/\\\\]InputBackend_NETCAST\\.cpp$"
    "[/\\\\]platform[/\\\\]Resources_NETCAST\\.cpp$"
    "[/\\\\]platform[/\\\\]audio[/\\\\]SoundManager_NETCAST\\.cpp$")

# Same facade selection as the desktop target, then webOS backends for the ones
# that touch the TV:
#   Resources, InputBackend, ClientPlatformPolicy -> src/platform/*_WEBOS.cpp
#   the GL renderer -> src/webos/render/WebOSGlesRenderBackend.cpp, which
#   implements the PcOpenGlRenderBackend namespace on OpenGL ES 1.x (the PC
#   OpenGL file it replaces is excluded above).
# Multiplayer: POSIX TCP backend in src/webos/JavaNetwork_webos.cpp replaces the
# SDL_net desktop one.
# Audio: the desktop SDL2 mixer (SoundManager_PC) is used as-is.
mcbeta_select_platform_backends(WEBOS_SOURCES PC PC PC)
mcbeta_select_backend(WEBOS_SOURCES platform Resources WEBOS
    "[/\\\\]platform[/\\\\]Resources_(PC|WII|PS2)\\.cpp$")
mcbeta_select_backend(WEBOS_SOURCES platform InputBackend WEBOS
    "[/\\\\]platform[/\\\\]InputBackend_(PC|WII|PS2)\\.cpp$")
mcbeta_select_backend(WEBOS_SOURCES platform ClientPlatformPolicy WEBOS
    "[/\\\\]platform[/\\\\]ClientPlatformPolicy_(PC|WII|PS2)\\.cpp$")

list(APPEND WEBOS_SOURCES
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/ioapi.c"
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/unzip.c")

# --- zlib (built from source for the target) ---------------------------------
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/zlib EXCLUDE_FROM_ALL)

# --- target ------------------------------------------------------------------
add_executable(OptiCraft ${WEBOS_SOURCES})
set_target_properties(OptiCraft PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(OptiCraft PRIVATE cxx_std_17)

target_compile_definitions(OptiCraft PRIVATE
    WEBOS_PLATFORM
    MC_LINUX
    PLATFORM_LOCAL_RESOURCES_ONLY=1   # multiplayer sockets only; no HTTP downloads
    MC_LOG_LEVEL=${MC_LOG_LEVEL})

if(WEBOS_LOWEND_PROFILE)
    target_compile_definitions(OptiCraft PRIVATE PC_LEGACY_BUILD=1)
endif()
if(WEBOS_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(OptiCraft PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# No -mcpu here on purpose: the SDK's default ARMv7 baseline runs on every TV
# webOS still ships (A9 / A7 / A15 / A53 in 32-bit mode), while a hand-picked
# core would refuse to start on the others.
target_compile_options(OptiCraft PRIVATE
    $<$<CONFIG:Release>:-O2>
    -fno-math-errno
    -fno-trapping-math
    -Wno-psabi)

target_include_directories(OptiCraft PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/pc"
    "${CMAKE_SOURCE_DIR}/src/net/minecraft/src"
    "${CMAKE_SOURCE_DIR}/src/mods"
    "${CMAKE_SOURCE_DIR}/external/stb"
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip")

# --- SDL2 (webosbrew/SDL-webOS, pinned) --------------------------------------
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)

set(WEBOS_SDL2_RELEASE "release-2.30.12-webos.5")
FetchContent_Declare(sdl2_webos
    URL "https://github.com/webosbrew/SDL-webOS/releases/download/${WEBOS_SDL2_RELEASE}/SDL2-2.30.12-webos-abi.tar.gz"
    URL_HASH SHA256=750ea26b41425ed54a58aba40d70970d4c0bae239142781f03dfcd153da475d1)
FetchContent_GetProperties(sdl2_webos)
if(NOT sdl2_webos_POPULATED)
    FetchContent_Populate(sdl2_webos)
endif()
# Only this package: the SDK sysroot ships a different SDL2 build.
find_package(SDL2 2.30.12 EXACT CONFIG REQUIRED
    PATHS "${sdl2_webos_SOURCE_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

# --- libraries ---------------------------------------------------------------
set(_webos_hints "${WEBOS_LIB_DIR}")
if(CMAKE_SYSROOT)
    list(APPEND _webos_hints "${CMAKE_SYSROOT}/usr/lib")
endif()
find_library(WEBOS_GLES1 NAMES GLESv1_CM libGLESv1_CM.so.1 HINTS ${_webos_hints})
if(NOT WEBOS_GLES1)
    message(FATAL_ERROR
        "webOS build: could not find libGLESv1_CM.\n"
        "It ships in the webOS sysroot (arm-webos-linux-gnueabi/sysroot/usr/lib); "
        "set -DWEBOS_LIB_DIR=<dir with libGLESv1_CM.so.1> if your SDK keeps it elsewhere.")
endif()

target_link_libraries(OptiCraft PRIVATE
    zlibstatic
    SDL2::SDL2
    ${WEBOS_GLES1}
    dl pthread m)

# libstdc++ in the TV firmware is old and may preload libraries that pull it in
# -- keep the C++ runtime inside the executable, exactly like the McBetaCpp
# webOS port does.
target_link_options(OptiCraft PRIVATE
    -static-libstdc++ -static-libgcc
    -Wl,--disable-new-dtags
    $<$<CONFIG:Release>:-s>)
set_target_properties(OptiCraft PROPERTIES
    BUILD_RPATH "\$ORIGIN/lib"
    INSTALL_RPATH "\$ORIGIN/lib")

# --- packaging ---------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/PackageWebOS.cmake)
