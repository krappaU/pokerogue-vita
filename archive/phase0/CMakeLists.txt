# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.16)

if(VITA)
  if(DEFINED ENV{VITASDK})
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
  else()
    message(FATAL_ERROR "VITASDK is not set")
  endif()
endif()

project(pokerogue_bench C)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")
if(VITA)
  # VitaSDK defaults to Thumb-2. The interpreter loop is usually faster as ARM code.
  set(CMAKE_C_FLAGS_RELEASE "-O2 -marm -mcpu=cortex-a9 -mfpu=neon")
endif()

set(QJS third_party/quickjs)
add_library(quickjs STATIC ${QJS}/quickjs.c ${QJS}/libregexp.c ${QJS}/libunicode.c ${QJS}/dtoa.c)
target_include_directories(quickjs PUBLIC ${QJS})
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
target_compile_options(quickjs PRIVATE -w)

set(SRC src/main.c src/fastpath.c)
if(VITA)
  list(APPEND SRC third_party/debugScreen/debugScreen.c)
endif()
add_executable(pokerogue_bench ${SRC})
target_link_libraries(pokerogue_bench quickjs m)

if(VITA)
  target_include_directories(pokerogue_bench PRIVATE third_party/debugScreen)
  target_link_libraries(pokerogue_bench pthread SceDisplay_stub SceCtrl_stub ScePower_stub SceSysmem_stub)
  vita_create_self(eboot.bin pokerogue_bench UNSAFE)
  vita_create_vpk(pokerogue_bench.vpk "PKRB00001" eboot.bin
    VERSION "01.00"
    NAME "PokeRogue Bench"
    FILE data/phaser.min.js data/phaser.min.js
    FILE js/shim.js data/js/shim.js
    FILE js/bench.js data/js/bench.js
    FILE js/fastpath.js data/js/fastpath.js
    FILE data/pokerogue data/pokerogue
  )
else()
  find_package(Threads REQUIRED)
  target_link_libraries(pokerogue_bench Threads::Threads)
endif()
