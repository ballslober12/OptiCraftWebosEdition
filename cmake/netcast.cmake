# netcast.cmake -- LG NetCast 4.x (ARM Cortex-A9, PowerVR SGX, EGL + OpenGL ES 1.1,
# lgncopenapi) build branch for OptiCraft.
#
# Included from the top of CMakeLists.txt when -DPLATFORM=NETCAST (the netcast-*
# presets do this together with cmake/netcast_toolchain.cmake), then return()s so
# none of the desktop SDL2/glad/OpenGL configuration runs.
#
#   export NC4_SDK=/path/to/arm-lgtv-linux-gnueabi_sdk-buildroot
#   cmake --preset netcast-release
#   cmake --build --preset netcast-release
#   cmake --install build/netcast-release --prefix <usb>/nc4-homebrew/opticraft
#
# Libraries: liblgncopenapi.so.2 / libEGL.so.1 / libGLESv1_CM.so.1 are NOT
# bundled -- they live on the TV. The link step only needs stubs exporting the
# same symbols (webos-userland's `nc4` preset, installed to <sysroot>/opt/nc4 as
# for cube-2005), or point NETCAST_LIB_DIR at a folder with copies from the TV.

cmake_minimum_required(VERSION 3.21)

include(${CMAKE_SOURCE_DIR}/cmake/SourceSelection.cmake)

set(NETCAST_LIB_DIR "" CACHE PATH "Extra directory searched for liblgncopenapi / libEGL / libGLESv1_CM")
option(NETCAST_LOWEND_PROFILE "Use the game's PC_LEGACY low-end profile (small chunk radius, budgeted chunk/light work, 16-bit terrain vertices, no particles/rain). Needed for 320 MB RAM." ON)
option(NETCAST_LTO "Link-time optimisation: faster game code, slower and more memory-hungry link (untested with the real SDK, so off by default)" OFF)
option(NETCAST_LINK_IMG "Also link the PowerVR user-mode libs (-lIMGegl -lsrv_um -lusc) like build.zip's build.sh" OFF)
set(MC_LOG_LEVEL "0" CACHE STRING "Unified diagnostic verbosity: 0=off, 1=info, 2=debug, 3=trace")
set_property(CACHE MC_LOG_LEVEL PROPERTY STRINGS 0 1 2 3)

# --- compiler sanity ---------------------------------------------------------
# The code base is C++17 and uses <filesystem>; that needs GCC 8 or newer.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8.0)
    message(FATAL_ERROR
        "NetCast build: g++ ${CMAKE_CXX_COMPILER_VERSION} is too old for C++17 <filesystem> (need GCC >= 8).\n"
        "Check `arm-lgtv-linux-gnueabi-g++ --version` in your NC4 SDK.")
endif()

# --- sources -----------------------------------------------------------------
# Start from the desktop (PC) tree, then swap the SDL/desktop-GL pieces for the
# NetCast ones in src/netcast.
mcbeta_collect_platform_sources(NETCAST_SOURCES pc)
file(GLOB_RECURSE _netcast_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/netcast/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/netcast/*.c")
list(APPEND NETCAST_SOURCES ${_netcast_sources})

mcbeta_exclude_sources(NETCAST_SOURCES
    "[/\\\\]pc[/\\\\]win32[/\\\\]"
    "[/\\\\]pc[/\\\\]Main\\.cpp$"
    "[/\\\\]pc[/\\\\]CrashHandler\\.cpp$"
    "[/\\\\]pc[/\\\\]lwjgl[/\\\\](Display|GLContext|Keyboard|Mouse)\\.cpp$"
    "[/\\\\]pc[/\\\\]render[/\\\\]PcOpenGlRenderBackend\\.cpp$"
    "[/\\\\]pc[/\\\\]render[/\\\\]d3d9[/\\\\]"
    "[/\\\\]pc[/\\\\]external[/\\\\]miniaudio\\.cpp$"
    "[/\\\\]pc[/\\\\]external[/\\\\]SDLException\\.h$")

# Same facade selection as the desktop target, then NetCast backends for the
# three that touched SDL / need the TV.
# Multiplayer: POSIX TCP backend in src/netcast/JavaNetwork_netcast.cpp
# replaces the SDL_net desktop one.
mcbeta_exclude_sources(NETCAST_SOURCES "[/\\\\]java[/\\\\]JavaNetwork\\.cpp$")
mcbeta_select_platform_backends(NETCAST_SOURCES PC PC PC)
mcbeta_select_backend(NETCAST_SOURCES platform Resources NETCAST
    "[/\\\\]platform[/\\\\]Resources_(PC|WII|PS2)\\.cpp$")
mcbeta_select_backend(NETCAST_SOURCES platform InputBackend NETCAST
    "[/\\\\]platform[/\\\\]InputBackend_(PC|WII|PS2)\\.cpp$")
mcbeta_select_backend(NETCAST_SOURCES platform/audio SoundManager NETCAST
    "[/\\\\]platform[/\\\\]audio[/\\\\]SoundManager_(PC|WII|PS2)\\.cpp$")

list(APPEND NETCAST_SOURCES
    "${CMAKE_SOURCE_DIR}/src/ps2/audio/Ps2AdpcmStreamDecoder.cpp"   # PS2-format sound assets (.adp)
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/ioapi.c"
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/unzip.c")

# --- zlib (built from source for the target) ---------------------------------
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/zlib EXCLUDE_FROM_ALL)

# --- target ------------------------------------------------------------------
add_executable(OptiCraft ${NETCAST_SOURCES})
set_target_properties(OptiCraft PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(OptiCraft PRIVATE cxx_std_17)

target_compile_definitions(OptiCraft PRIVATE
    NETCAST_PLATFORM
    MC_LINUX
    PLATFORM_LOCAL_RESOURCES_ONLY=1   # multiplayer sockets only; no HTTP downloads
    LGNC_PLATFORM_NC4=1        # only declare the lgncopenapi functions that exist on NC4
    MC_LOG_LEVEL=${MC_LOG_LEVEL})

if(NETCAST_LOWEND_PROFILE)
    target_compile_definitions(OptiCraft PRIVATE PC_LEGACY_BUILD=1)
endif()
if(NETCAST_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # The game is built from many tiny cross-file accessors (getBlockId, light
    # getters ...) called millions of times per frame; same reason the desktop
    # release builds use LTO.
    set_target_properties(OptiCraft PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
endif()

target_compile_options(OptiCraft PRIVATE
    $<$<CONFIG:Release>:-O2>
    -mcpu=cortex-a9 -mtune=cortex-a9
    -fno-math-errno
    -fno-trapping-math
    -Wno-psabi)

target_include_directories(OptiCraft PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/pc"
    "${CMAKE_SOURCE_DIR}/src/net/minecraft/src"
    "${CMAKE_SOURCE_DIR}/src/mods"
    "${CMAKE_SOURCE_DIR}/src/netcast/sdk/include"
    "${CMAKE_SOURCE_DIR}/external/stb"
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip")

# --- libraries ---------------------------------------------------------------
set(_nc_hints "${NETCAST_LIB_DIR}")
if(CMAKE_SYSROOT)
    list(APPEND _nc_hints "${CMAKE_SYSROOT}/opt/nc4/lib")
endif()
function(netcast_find out)
    find_library(${out} NAMES ${ARGN} HINTS ${_nc_hints})
    if(NOT ${out})
        message(FATAL_ERROR
            "NetCast build: could not find ${ARGN}.\n"
            "Install webos-userland's nc4 stubs into <sysroot>/opt/nc4 (as for cube-2005) "
            "or set -DNETCAST_LIB_DIR=<dir with the .so files>.")
    endif()
endfunction()
netcast_find(NC_LGNC  lgncopenapi liblgncopenapi.so.2)
netcast_find(NC_EGL   EGL libEGL.so.1)
netcast_find(NC_GLES1 GLESv1_CM libGLESv1_CM.so.1)

set(_nc_libs zlibstatic ${NC_LGNC} ${NC_EGL} ${NC_GLES1} dl pthread rt m)
if(NETCAST_LINK_IMG)
    list(APPEND _nc_libs IMGegl srv_um usc)
endif()
target_link_libraries(OptiCraft PRIVATE ${_nc_libs})
# libstdc++ in the firmware is old and its app environment preloads libraries
# that pull it in -- same fix as cube-2005.
target_link_options(OptiCraft PRIVATE
    -static-libstdc++ -static-libgcc
    -Wl,--disable-new-dtags
    -Wl,-rpath-link,${NETCAST_LIB_DIR}
    $<$<CONFIG:Release>:-s>)
set_target_properties(OptiCraft PROPERTIES INSTALL_RPATH "$ORIGIN/lib")

# --- install: a self-contained folder for <usb>/nc4-homebrew/opticraft --------
install(TARGETS OptiCraft RUNTIME DESTINATION .)
install(PROGRAMS deploy/netcast/opticraft.sh DESTINATION .)
install(FILES deploy/netcast/netcast.cfg DESTINATION .)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets" DESTINATION . OPTIONAL)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/resources")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources" DESTINATION . OPTIONAL)
endif()
