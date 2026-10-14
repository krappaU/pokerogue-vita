# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(pokerogue_runtime C)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(QJS third_party/quickjs)
add_library(quickjs STATIC ${QJS}/quickjs.c ${QJS}/libregexp.c ${QJS}/libunicode.c ${QJS}/dtoa.c)
target_include_directories(quickjs PUBLIC ${QJS})
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
target_compile_options(quickjs PRIVATE -w)

file(GLOB PLUTOVG_SRC third_party/plutovg/source/*.c)
add_library(plutovg STATIC ${PLUTOVG_SRC})
target_include_directories(plutovg PUBLIC third_party/plutovg/include)
target_compile_definitions(plutovg PUBLIC PLUTOVG_BUILD_STATIC)
target_compile_options(plutovg PRIVATE -w)

add_executable(pokerogue-rt
  src/main.c src/platform_sdl.c src/js_host.c src/net.c src/gl.c src/canvas2d.c src/fastpath.c)
target_include_directories(pokerogue-rt PRIVATE src third_party/stb)
target_compile_options(pokerogue-rt PRIVATE -Wall -Wno-unused-function -Wno-missing-braces)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(GLES REQUIRED glesv2)
target_include_directories(pokerogue-rt PRIVATE ${SDL2_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS} ${GLES_INCLUDE_DIRS})
target_link_libraries(pokerogue-rt quickjs plutovg ${SDL2_LIBRARIES} ${CURL_LIBRARIES} ${GLES_LIBRARIES} m pthread)
