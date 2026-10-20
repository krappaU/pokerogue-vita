# SPDX-License-Identifier: AGPL-3.0-only
# Linux:  cmake -S . -B build-linux
# Vita:   cmake -S . -B build-vita -DVITA=ON -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake_minimum_required(VERSION 3.16)
if(VITA)
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
endif()
project(pokerogue_runtime C)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(QJS third_party/quickjs)
add_library(quickjs STATIC ${QJS}/quickjs.c ${QJS}/libregexp.c ${QJS}/libunicode.c ${QJS}/dtoa.c)
target_include_directories(quickjs PUBLIC ${QJS})
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
# PKR_PROFILE_RATE=1000: the interrupt handler (and the built-in profiler) every 1,000 operations
# instead of 10,000, for detailed desktop profiles. Not for builds handed out.
set(PKR_PROFILE_RATE "" CACHE STRING "QuickJS operations between interrupt checks (empty: default)")
if(PKR_PROFILE_RATE)
  target_compile_definitions(quickjs PRIVATE JS_INTERRUPT_COUNTER_INIT=${PKR_PROFILE_RATE})
endif()
target_compile_options(quickjs PRIVATE -w)

file(GLOB PLUTOVG_SRC third_party/plutovg/source/*.c)
add_library(plutovg STATIC ${PLUTOVG_SRC})
target_include_directories(plutovg PUBLIC third_party/plutovg/include)
target_compile_definitions(plutovg PUBLIC PLUTOVG_BUILD_STATIC PRIVATE PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD)
target_compile_options(plutovg PRIVATE -w)

# Version: runtime/VERSION ("0.18"); the build scripts pass the git commit as PKR_COMMIT.
file(STRINGS VERSION PKR_VERSION LIMIT_COUNT 1)
set(PKR_TITLE_ID PKRV00001)
# Dev build (scripts/build_vita.sh with PKR_DEV=1): adds src/devtools.c,
# which installs test builds and uploads logs. Never in builds handed out to players.
option(PKR_DEV "dev build: test builds from the dev-build release, log upload to tools/devserver" OFF)
if(NOT PKR_COMMIT)
  set(PKR_COMMIT unknown)
endif()
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)$" _v "${PKR_VERSION}")
if(NOT _v)
  message(FATAL_ERROR "VERSION must look like 0.18, got '${PKR_VERSION}'")
endif()
# LiveArea/package version: two digits each, "00.18".
math(EXPR _major "${CMAKE_MATCH_1}")
math(EXPR _minor "${CMAKE_MATCH_2}")
if(_major LESS 10)
  set(_major "0${_major}")
endif()
if(_minor LESS 10)
  set(_minor "0${_minor}")
endif()
set(PKR_SFO_VERSION "${_major}.${_minor}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)

add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
add_library(qrcodegen STATIC third_party/qrcodegen/qrcodegen.c)
target_include_directories(qrcodegen PUBLIC third_party/qrcodegen)
target_compile_definitions(miniz PUBLIC MINIZ_NO_TIME)  # changes struct layouts: users must agree
target_compile_options(miniz PRIVATE -w)

set(RT_SRC src/main.c src/js_host.c src/precompile.c src/net.c src/gl.c src/canvas2d.c src/fastpath.c src/overlay.c src/osk.c src/profiler.c src/launcher.c src/offline_install.c src/audio.c src/audio_m4a.c src/savecrypt.c src/dialogs.c)
if(PKR_DEV)
  list(APPEND RT_SRC src/devtools.c)
  set(PKR_VPK_SUFFIX -dev)
endif()

if(VITA)
  # NDEBUG: without it QuickJS keeps ~140 internal assert checks, some on hot paths (atoms, shapes,
  # the GC's marking); the desktop build (CMake's default flags) always had it.
  set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG -marm -mcpu=cortex-a9 -mfpu=neon")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG -marm -mcpu=cortex-a9 -mfpu=neon")
  add_executable(pokerogue-rt ${RT_SRC} src/platform_vita.c src/vita_compat.c)
  # Our own libcurl build (scripts/build_vita_curl.sh): the prebuilt one is ABI-incompatible with
  # VitaSDK's OpenSSL 1.1.
  target_include_directories(pokerogue-rt BEFORE PRIVATE third_party/vita-curl/include)
  target_link_directories(pokerogue-rt BEFORE PRIVATE third_party/vita-curl/lib)
  # Our own vitaGL build (scripts/build_vita_vitagl.sh): same as VitaSDK's, without the boot splash.
  target_include_directories(pokerogue-rt BEFORE PRIVATE third_party/vita-vitagl/include)
  target_link_directories(pokerogue-rt BEFORE PRIVATE third_party/vita-vitagl/lib)
  target_include_directories(pokerogue-rt PRIVATE src third_party/stb third_party)
  target_compile_options(pokerogue-rt PRIVATE -Wall -Wno-unused-function -Wno-missing-braces -Wno-unused-variable)
  target_link_libraries(pokerogue-rt quickjs plutovg miniz qrcodegen
    curl ssl crypto z zstd
    vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
    SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceAppMgr_stub SceCommonDialog_stub SceIme_stub
    SceCtrl_stub SceTouch_stub ScePower_stub SceSysmodule_stub SceNet_stub SceNetCtl_stub SceAppUtil_stub
    SceLibKernel_stub SceIofilemgr_stub SceRtc_stub SceAudio_stub faad
    stdc++ pthread m)
  # vita-elf-create appends its tables after the code segment; with the default 64 KiB alignment the
  # gap before the data segment depends on code size and was once too small ("segment 1 overlaps").
  target_link_options(pokerogue-rt PRIVATE -Wl,--defsym,__sce_headroom=0x4000)
  vita_create_self(eboot.bin pokerogue-rt UNSAFE)
  file(GLOB JS_FILES js/*.js)
  set(VPK_FILES FILE third_party/cacert.pem cacert.pem
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
  foreach(f ${JS_FILES})
    get_filename_component(n ${f} NAME)
    list(APPEND VPK_FILES FILE ${f} js/${n})
  endforeach()
  # Extended memory mode: lets the app use more of the Vita's main RAM.
  set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
  vita_create_vpk(pokerogue-${PKR_VERSION}${PKR_VPK_SUFFIX}.vpk "${PKR_TITLE_ID}" eboot.bin VERSION "${PKR_SFO_VERSION}" NAME "PokeRogue" ${VPK_FILES})
  if(PKR_DEV)
    # The updater app (updater/updater.c): installs test builds into the game's folder, which the
    # running game can't write. A separate .vpk, installed once with VitaShell.
    add_executable(pokerogue-updater updater/updater.c)
    target_compile_options(pokerogue-updater PRIVATE -Wall)
    target_link_libraries(pokerogue-updater SceAppMgr_stub SceLibKernel_stub)
    vita_create_self(updater.bin pokerogue-updater UNSAFE)
    # vita_create_vpk is a macro that appends to these in this scope: without a reset the updater
    # would get the game's files, memory mode and TITLE_ID (and replace the game when installed).
    set(VITA_MKSFOEX_FLAGS "")
    set(VITA_PACK_VPK_FLAGS "")
    # Its own bubble (scripts/make_livearea.py), so it isn't taken for the game.
    vita_create_vpk(pokerogue-updater.vpk PKRVUPD01 updater.bin VERSION "01.00" NAME "PokeRogue Updater"
      FILE updater/sce_sys/icon0.png sce_sys/icon0.png
      FILE updater/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
      FILE updater/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
      FILE updater/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
  endif()
else()
  add_executable(pokerogue-rt ${RT_SRC} src/platform_sdl.c)
  target_include_directories(pokerogue-rt PRIVATE src third_party/stb third_party)
  target_compile_options(pokerogue-rt PRIVATE -Wall -Wno-unused-function -Wno-missing-braces)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  pkg_check_modules(CURL REQUIRED libcurl)
  pkg_check_modules(GLES REQUIRED glesv2)
  pkg_check_modules(SSL REQUIRED openssl)
  target_include_directories(pokerogue-rt PRIVATE ${SDL2_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS} ${GLES_INCLUDE_DIRS} ${SSL_INCLUDE_DIRS})
  target_link_libraries(pokerogue-rt quickjs plutovg miniz qrcodegen ${SDL2_LIBRARIES} faad ${CURL_LIBRARIES} ${GLES_LIBRARIES} ${SSL_LIBRARIES} m pthread)
endif()

target_compile_definitions(pokerogue-rt PRIVATE PKR_VERSION="${PKR_VERSION}" PKR_COMMIT="${PKR_COMMIT}" PKR_TITLE_ID="${PKR_TITLE_ID}")
if(PKR_DEV)
  target_compile_definitions(pokerogue-rt PRIVATE PKR_DEV=1)
endif()
# Profiling build: the built-in profiler samples from the start, every 1,000 JS operations, and
# reports to the log every 30 s (src/profiler.c). A test build for one measurement, not for play:
# a branch that wants one turns this on here (CI's builds then come out profiling).
option(PKR_PROFILE_BUILD "profiling build: sample the JS profile from the start, 10x as often" OFF)
if(PKR_PROFILE_BUILD)
  target_compile_definitions(pokerogue-rt PRIVATE PKR_PROFILE_BUILD=1)
endif()
# Debug features (log file by default, control channel, profiler, debug line, watchdog, debug.js,
# config.txt's debugging options): dev and profiling builds and the desktop runtime. Vita builds
# for players have none of them (a log only with config.txt log=1).
# -DPKR_DEBUG=OFF on the desktop builds the player variant for testing.
if(PKR_DEV OR PKR_PROFILE_BUILD OR NOT VITA)
  set(PKR_DEBUG_DEFAULT ON)
else()
  set(PKR_DEBUG_DEFAULT OFF)
endif()
option(PKR_DEBUG "debug features (default: on except in Vita builds for players)" ${PKR_DEBUG_DEFAULT})
if(PKR_DEBUG)
  target_compile_definitions(pokerogue-rt PRIVATE PKR_DEBUG=1)
endif()
