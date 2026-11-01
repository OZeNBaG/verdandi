# Verdandi for android: libmain.so (matron, the sidecar, the window) and libSDL2.so.
# SDL, lua 5.3 and libmonome are built from android/deps (android/fetch-deps.sh); cairo,
# freetype, liblo, libsndfile and nng come from vcpkg (vcpkg.json next to this file).

cmake_minimum_required(VERSION 3.22)
project(verdandi C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

get_filename_component(NORNS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(DEPS "${NORNS}/android/deps")
set(M "${NORNS}/matron/src")

#--- SDL (shared: SDLActivity loads libSDL2.so, then libmain.so)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/SDL" sdl EXCLUDE_FROM_ALL)

#--- lua 5.3 (C, as norns' C modules expect: see platform/android/compat.h)
file(GLOB LUA_SOURCES "${DEPS}/lua/l*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*/(lua|luac|ltests|onelua)\\.c$")
add_library(lua53 STATIC ${LUA_SOURCES})
target_include_directories(lua53 PUBLIC "${DEPS}/lua")
target_compile_definitions(lua53 PUBLIC LUA_COMPAT_5_2 LUA_USE_POSIX LUA_USE_DLOPEN)

#--- lua-cjson
add_library(luacjson STATIC
    "${NORNS}/third-party/lua-cjson/lua_cjson.c"
    "${NORNS}/third-party/lua-cjson/strbuf.c"
    "${NORNS}/third-party/lua-cjson/fpconv.c")
target_link_libraries(luacjson PUBLIC lua53)

#--- libmonome (grids and arcs over usb serial)
set(LM "${DEPS}/libmonome/src")
add_library(monome STATIC
    "${LM}/libmonome.c" "${LM}/monobright.c" "${LM}/rotation.c"
    "${LM}/proto/40h.c" "${LM}/proto/mext.c" "${LM}/proto/series.c"
    "${LM}/platform/embed.c" "${LM}/platform/linux.c" "${LM}/platform/linux_sysfs.c" "${LM}/platform/posix.c")
target_compile_definitions(monome PRIVATE EMBED_PROTOS _GNU_SOURCE)
target_include_directories(monome PRIVATE "${LM}/private" "${LM}")
target_include_directories(monome PUBLIC "${DEPS}/libmonome/public")

#--- ableton link (sync with other apps on the network)
set(LINK "${NORNS}/third-party/link")
add_library(linkc STATIC "${LINK}/extensions/abl_link/src/abl_link.cpp")
target_include_directories(linkc PUBLIC "${LINK}/extensions/abl_link/include")
target_include_directories(linkc PRIVATE "${LINK}/include" "${LINK}/modules/asio-standalone/asio/include")
target_compile_definitions(linkc PRIVATE LINK_PLATFORM_LINUX=1 ASIO_STANDALONE=1)
target_compile_options(linkc PRIVATE -Wno-multichar)

#--- vcpkg libraries, through pkg-config (static: the full dependency chains)
find_package(PkgConfig REQUIRED)
# only vcpkg's .pc files: never the build machine's own cairo &c
set(VCPKG_PC "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
set(ENV{PKG_CONFIG_LIBDIR} "${VCPKG_PC}/lib/pkgconfig:${VCPKG_PC}/share/pkgconfig")
pkg_check_modules(VDEPS REQUIRED cairo cairo-ft freetype2 liblo sndfile)
find_package(nng CONFIG REQUIRED)

#--- matron
set(MATRON_SOURCES
    ${M}/config.cc
    ${M}/device/device.cc
    ${M}/device/device_list.cc
    ${M}/device/device_midi.cc
    ${M}/device/device_monome.cc
    ${M}/device/device_crow.cc
    ${M}/device/device_serial.cc
    ${M}/osc.cc
    ${M}/hardware/input.cc
    ${M}/hardware/io.cc
    ${M}/hardware/platform.cc
    ${M}/hardware/screen.cc
    ${M}/hardware/stat.cc
    ${M}/hardware/screen/sdl.cc
    ${M}/hardware/input/sdl.cc
    ${M}/args.cc
    ${M}/events.cc
    ${M}/hello.cc
    ${M}/input.cc
    ${M}/lua_eval.cc
    ${M}/lua_shell.cc
    ${M}/main.cc
    ${M}/metro.cc
    ${M}/oracle.cc
    ${M}/weaver.cc
    ${M}/screen_events.cc
    ${M}/screen_results.cc
    ${M}/snd_file.cc
    ${M}/system_cmd.cc
    ${M}/clock.cc
    ${M}/clocks/clock_internal.cc
    ${M}/clocks/clock_midi.cc
    ${M}/clocks/clock_crow.cc
    ${M}/clocks/clock_scheduler.cc
    ${M}/time_since.cc
    ${M}/clocks/clock_link.cc
    # the pi's battery gauge, i2c and oled: no-ops (shared with the macOS port)
    ${M}/platform/macos/hardware_null.cc
    # the device drawn in the window (pure cairo, shared with the macOS port)
    ${M}/platform/macos/device_skin.c
    ${M}/platform/android/android_compat.c
    ${M}/platform/android/android_main.cc
    ${M}/platform/android/device_android.cc
    ${M}/platform/android/fake_sclang.cc
    ${M}/jack_client.cpp
    ${M}/platform/android/jacklite/jacklite.cc
    ${M}/platform/android/sdl_bridge_android.cc
    # crone: the mixer, softcut, tape (as on norns, in-process; jack is jacklite)
    ${NORNS}/crone/src/crone_param_dispatch.cpp
    ${NORNS}/crone/src/crone.cpp
    ${NORNS}/crone/src/main.cpp
    ${NORNS}/crone/src/BufDiskWorker.cpp
    ${NORNS}/crone/src/Commands.cpp
    ${NORNS}/crone/src/MixerClient.cpp
    ${NORNS}/crone/src/OscInterface.cpp
    ${NORNS}/crone/src/SoftcutClient.cpp
    ${NORNS}/crone/src/Taper.cpp
    ${NORNS}/crone/src/Window.cpp
    ${NORNS}/crone/softcut/softcut-lib/src/FadeCurves.cpp
    ${NORNS}/crone/softcut/softcut-lib/src/ReadWriteHead.cpp
    ${NORNS}/crone/softcut/softcut-lib/src/SubHead.cpp
    ${NORNS}/crone/softcut/softcut-lib/src/Svf.cpp
    ${NORNS}/crone/softcut/softcut-lib/src/Voice.cpp
    ${NORNS}/norns/sidecar.cpp
    ${NORNS}/norns/sidecar_lines.cpp
    ${NORNS}/norns/sidecar_msg.cpp
    ${NORNS}/norns/sidecar_shell.cpp
)

add_library(main SHARED ${MATRON_SOURCES})

target_include_directories(main PRIVATE
    ${M}/platform/android/include
    ${M}/platform/android/jacklite/include
    ${M}
    ${M}/device
    ${M}/hardware
    ${NORNS}/norns
    ${NORNS}/third-party/concurrentqueue
    ${NORNS}/third-party/readerwriterqueue
    ${NORNS}/crone
    ${NORNS}/crone/src
    ${NORNS}/crone/softcut/softcut-lib/include
    ${VDEPS_STATIC_INCLUDE_DIRS})

target_compile_definitions(main PRIVATE
    _GNU_SOURCE
    NORNS_DESKTOP=1
    HAVE_LUA_CJSON=1
    HAVE_ABLETON_LINK=1
    VERSION_MAJOR=0
    VERSION_MINOR=0
    VERSION_PATCH=0
    VERSION_HASH="android")

target_compile_options(main PRIVATE
    -include "${M}/platform/android/compat.h"
    -O2
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -Wno-sign-compare
    -Wno-deprecated-declarations
    -Wno-c99-designator
    -Wno-reorder-init-list
    -Wno-writable-strings
    -Wno-format)

target_link_directories(main PRIVATE ${VDEPS_STATIC_LIBRARY_DIRS})
target_link_libraries(main PRIVATE
    SDL2::SDL2
    luacjson
    lua53
    linkc
    monome
    nng::nng
    ${VDEPS_STATIC_LIBRARIES}
    log
    android
    dl
    m
    z)

# lua C modules from scripts (dlopen'ed) resolve the lua API against libmain.so
target_link_options(main PRIVATE -Wl,--export-dynamic -Wl,--no-undefined)
