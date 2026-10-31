cmake_minimum_required(VERSION 3.16)
project(dc_core C)

set(CMAKE_C_STANDARD 11)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(dccore STATIC runtime/src/dispatch.c runtime/src/disc.c runtime/src/bios_hle.c runtime/src/hw.c runtime/src/ta.c runtime/src/arm7.c runtime/src/aica.c)
target_include_directories(dccore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include)
if(NOT MSVC)
  target_link_libraries(dccore PUBLIC m)
endif()

set(DCCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")

# dccore_add_recomp(<target> BIN <1ST_READ.BIN> FUNCS <funcs.txt> [BASE 0x8C010000])
#
# Recompiles the player's own executable at configure time and builds it as a static
# library. The generated C is derived from the game and stays in the build directory.
function(dccore_add_recomp target)
  cmake_parse_arguments(A "" "BIN;FUNCS;BASE" "" ${ARGN})
  if(NOT A_BASE)
    set(A_BASE 0x8C010000)
  endif()
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${target}_gen)
  if(NOT EXISTS ${A_BIN})
    message(FATAL_ERROR "${A_BIN} not found - run the setup step that extracts it from your GDI first")
  endif()
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${DCCORE_DIR}/tools/recomp/sh4recomp.py ${A_BIN}
            --base ${A_BASE} --funcs ${A_FUNCS} --out ${out}
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "sh4recomp failed")
  endif()
  file(GLOB gen_src ${out}/*.c)
  add_library(${target} STATIC ${gen_src})
  target_link_libraries(${target} PUBLIC dccore)
  if(NOT MSVC)
    # generated code is huge and flat; -O1 keeps compile times sane, aliasing must stay off
    target_compile_options(${target} PRIVATE -O1 -w -fno-strict-aliasing)
  endif()
endfunction()
