cmake_minimum_required(VERSION 3.16)
project(dc_core C)

set(CMAKE_C_STANDARD 11)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(dccore STATIC runtime/src/dispatch.c runtime/src/disc.c runtime/src/bios_hle.c runtime/src/hw.c runtime/src/ta.c runtime/src/arm7.c runtime/src/aica.c runtime/src/coro.c runtime/src/pvr_sw.c runtime/src/maple.c runtime/src/pvr_gl.c)
target_include_directories(dccore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include)
if(MSVC)
  target_compile_definitions(dccore PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
  target_link_libraries(dccore PUBLIC m)
endif()

set(DCCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
set(DCCORE_PYTHON ${Python3_EXECUTABLE} CACHE INTERNAL "")   # visible to ports calling dccore_add_recomp

# Desktop frontend (window, OpenGL 3.3, audio, input): built when SDL2 is available
find_package(SDL2 QUIET CONFIG)
if(SDL2_FOUND)
  add_library(dccore_front STATIC frontend/dccore_front.c)
  target_include_directories(dccore_front PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
  if(TARGET SDL2::SDL2)
    target_link_libraries(dccore_front PUBLIC dccore SDL2::SDL2)
  else()
    target_include_directories(dccore_front PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(dccore_front PUBLIC dccore ${SDL2_LIBRARIES})
  endif()
endif()

# dccore_add_recomp(<target> BIN <1ST_READ.BIN> FUNCS <funcs.txt> [BASE 0x8C010000] [SETJMP addr...])
#
# Recompiles the player's own executable at configure time and builds it as a static
# library. The generated C is derived from the game and stays in the build directory.
function(dccore_add_recomp target)
  cmake_parse_arguments(A "" "BIN;FUNCS;BASE" "SETJMP" ${ARGN})
  if(NOT A_BASE)
    set(A_BASE 0x8C010000)
  endif()
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${target}_gen)
  if(NOT EXISTS ${A_BIN})
    message(FATAL_ERROR "${A_BIN} not found - run the setup step that extracts it from your GDI first")
  endif()
  set(sj)
  foreach(a ${A_SETJMP})
    list(APPEND sj --setjmp ${a})
  endforeach()
  execute_process(
    COMMAND ${DCCORE_PYTHON} ${DCCORE_DIR}/tools/recomp/sh4recomp.py ${A_BIN}
            --base ${A_BASE} --funcs ${A_FUNCS} --out ${out} ${sj}
    RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "sh4recomp failed (${rc}): ${err}")
  endif()
  file(GLOB gen_src ${out}/*.c)
  add_library(${target} STATIC ${gen_src})
  target_link_libraries(${target} PUBLIC dccore)
  # dccore's dispatcher looks functions up in the generated table: a static-library cycle
  target_link_libraries(dccore INTERFACE ${target})
  set_property(TARGET dccore PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
  if(MSVC)
    # generated code is huge and flat: many sections per object, warnings off
    target_compile_options(${target} PRIVATE /bigobj /w)
  else()
    # -O1 keeps compile times sane, aliasing must stay off
    target_compile_options(${target} PRIVATE -O1 -w -fno-strict-aliasing)
  endif()
endfunction()

# Tests (no game code): cmake -DDCCORE_TESTS=ON, then ctest
option(DCCORE_TESTS "Build the dc-core tests" OFF)
if(DCCORE_TESTS)
  enable_testing()
  foreach(t runtime pvr_sw disc)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE dccore)
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()
  if(TARGET dccore_front AND CMAKE_SYSTEM_NAME STREQUAL "Linux")   # headless GL needs Mesa
    add_executable(test_front tests/test_front.c)
    target_link_libraries(test_front PRIVATE dccore_front)
    add_test(NAME front COMMAND test_front)
    set_tests_properties(front PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=offscreen;SDL_AUDIODRIVER=dummy")
  endif()
  # recompiler tests: small SH-4 programs recompiled at configure time, built with this compiler
  set(rdir ${CMAKE_CURRENT_BINARY_DIR}/recomp_tests)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_recomp.py --emit ${rdir}
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "test_recomp.py --emit failed")
  endif()
  file(READ ${rdir}/tests.txt recomp_tests)
  foreach(n ${recomp_tests})
    file(GLOB gsrc ${rdir}/${n}/gen/*.c)
    add_executable(recomp_${n} ${rdir}/${n}/main.c ${gsrc})
    target_link_libraries(recomp_${n} PRIVATE dccore)
    if(MSVC)
      target_compile_options(recomp_${n} PRIVATE /bigobj /w)
    else()
      target_compile_options(recomp_${n} PRIVATE -w -fno-strict-aliasing)
    endif()
    add_test(NAME recomp_${n} COMMAND recomp_${n})
  endforeach()
endif()
