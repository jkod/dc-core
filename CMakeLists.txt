cmake_minimum_required(VERSION 3.16)
project(dc_core C)

set(CMAKE_C_STANDARD 11)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(dccore STATIC runtime/src/dispatch.c runtime/src/disc.c runtime/src/bios_hle.c runtime/src/hw.c runtime/src/ta.c runtime/src/arm7.c runtime/src/aica.c runtime/src/coro.c runtime/src/pvr_sw.c runtime/src/maple.c runtime/src/pvr_gl.c runtime/src/texpack.c runtime/src/interp.c)
target_include_directories(dccore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include)
# Windows CE games (dccore/wince.h): the loader and native COREDLL calls
add_library(dccore_wince STATIC runtime/src/wince/ce_kernel.c runtime/src/wince/ce_loader.c runtime/src/wince/ce_api.c
            runtime/src/wince/ce_gwes.c)
target_link_libraries(dccore_wince PUBLIC dccore)
option(DCCORE_RAM_32MB "32 MB of RAM (Windows CE games: the whole user address space has physical addresses)" OFF)
if(DCCORE_RAM_32MB)
  target_compile_definitions(dccore PUBLIC DC_RAM_32MB)
endif()
# CHD disc images (libchdr, BSD-3-Clause): fetched at a pinned commit. Offline builds can point
# FETCHCONTENT_SOURCE_DIR_LIBCHDR at a checkout, or turn this off (.gdi / .cue still work).
option(DCCORE_CHD "Read .chd disc images (libchdr)" ON)
if(DCCORE_CHD)
  include(FetchContent)
  FetchContent_Declare(libchdr
    GIT_REPOSITORY https://github.com/rtissera/libchdr.git
    GIT_TAG 47d794151b2b64f6aba60304e6480f4f5a2bc24b)
  set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
  set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
  set(dccore_shared ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)                     # chdr-static only
  FetchContent_MakeAvailable(libchdr)
  set(BUILD_SHARED_LIBS ${dccore_shared})
  target_link_libraries(dccore PUBLIC chdr-static)
  target_compile_definitions(dccore PRIVATE DCCORE_CHD)
endif()
set(DCCORE_MSVC_JOBS 4 CACHE STRING "MSVC: generated files compiled at the same time (/MP)")
if(MSVC)
  target_compile_definitions(dccore PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
  target_link_libraries(dccore PUBLIC m)
endif()

set(DCCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
set(DCCORE_PYTHON ${Python3_EXECUTABLE} CACHE INTERNAL "")   # visible to ports calling dccore_add_recomp

# Frontend (window, OpenGL 3.3, audio, input): built when SDL2 is available, as a CMake package
# (desktop) or through pkg-config (devkitPro portlibs on Switch: switch-sdl2 + switch-mesa)
find_package(SDL2 QUIET CONFIG)
set(dccore_sdl)
if(SDL2_FOUND)
  if(TARGET SDL2::SDL2)
    set(dccore_sdl SDL2::SDL2)
  endif()
else()
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(SDL2PC QUIET IMPORTED_TARGET sdl2)
    if(SDL2PC_FOUND)
      set(SDL2_FOUND 1)
      set(dccore_sdl PkgConfig::SDL2PC)
    endif()
  endif()
endif()
if(SDL2_FOUND)
  add_library(dccore_front STATIC frontend/dccore_front.c)
  target_include_directories(dccore_front PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/frontend)
  if(dccore_sdl)
    target_link_libraries(dccore_front PUBLIC dccore ${dccore_sdl})
  else()
    target_include_directories(dccore_front PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(dccore_front PUBLIC dccore ${SDL2_LIBRARIES})
  endif()
  message(STATUS "dc-core: frontend enabled (SDL2)")
else()
  message(STATUS "dc-core: SDL2 not found, frontend disabled")
endif()

# dccore_add_player(<target> SOURCES <files...> [TITLE t] [AUTHOR a] [VERSION v]
#                   [ICON <file> [ICON_ARGS args...]])
#
# An executable on the frontend; on Switch also <target>.nro (homebrew menu app).
# ICON: the game's VMU icon data, taken from your disc by the setup step (palette + 32x32 frames,
# tools/icon/vmuicon.py; ICON_ARGS e.g. --flip). It becomes the Windows executable's icon, the
# window / taskbar / Dock icon and the Switch homebrew-menu icon. Skipped when the file isn't there.
function(dccore_add_player target)
  cmake_parse_arguments(A "" "TITLE;AUTHOR;VERSION;ICON" "SOURCES;ICON_ARGS" ${ARGN})
  set(icon_dir)
  if(A_ICON AND EXISTS "${A_ICON}")
    set(icon_dir ${CMAKE_CURRENT_BINARY_DIR}/${target}_icon)
    execute_process(COMMAND ${DCCORE_PYTHON} ${DCCORE_DIR}/tools/icon/vmuicon.py "${A_ICON}" ${icon_dir} ${A_ICON_ARGS}
                    RESULT_VARIABLE rc ERROR_VARIABLE err OUTPUT_QUIET)
    if(NOT rc EQUAL 0)
      message(WARNING "icon skipped: vmuicon.py failed: ${err}")
      set(icon_dir)
    else()
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${A_ICON}")
      list(APPEND A_SOURCES ${icon_dir}/icon_rgba.c)
      if(WIN32)
        file(WRITE ${icon_dir}/icon.rc "1 ICON \"${icon_dir}/icon.ico\"\n")
        list(APPEND A_SOURCES ${icon_dir}/icon.rc)
      endif()
    endif()
  endif()
  add_executable(${target} ${A_SOURCES})
  target_link_libraries(${target} PRIVATE dccore_front)
  if(icon_dir)
    target_compile_definitions(${target} PRIVATE DCCORE_ICON=1)
  endif()
  # Windows: SDL2.dll next to the executable (vcpkg does this by itself; SDL2_DIR from SDL's own
  # development package doesn't). The executable itself is in the list (copied onto itself: a no-op)
  # so the command stays valid when there are no DLLs.
  if(WIN32 AND NOT CMAKE_VERSION VERSION_LESS 3.21)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${target}> $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
      COMMAND_EXPAND_LISTS)
  endif()
  if(NINTENDO_SWITCH)
    if(NOT A_TITLE)
      set(A_TITLE ${target})
    endif()
    if(NOT A_AUTHOR)
      set(A_AUTHOR "dc-core")
    endif()
    if(NOT A_VERSION)
      set(A_VERSION "0.1")
    endif()
    nx_generate_nacp(${target}.nacp NAME "${A_TITLE}" AUTHOR "${A_AUTHOR}" VERSION "${A_VERSION}")
    if(icon_dir)
      nx_create_nro(${target} NACP ${target}.nacp ICON ${icon_dir}/icon.jpg)
    else()
      nx_create_nro(${target} NACP ${target}.nacp)
    endif()
  endif()
endfunction()

# Demo: draws through the PowerVR model with no game code (a quick check of a new platform)
option(DCCORE_DEMO "Build the frontend demo" OFF)
if(DCCORE_DEMO AND TARGET dccore_front)
  # a made-up icon (no game data) so CI builds the icon path on every platform
  set(demo_icon ${CMAKE_CURRENT_BINARY_DIR}/demo_icon.bin)
  execute_process(COMMAND ${Python3_EXECUTABLE} -c
    "import struct,sys;p=b''.join(struct.pack('<H',0xF000|(k*0x111)) for k in range(16));b=bytes(((x//2+y)%16)*17 for y in range(32) for x in range(16));open(sys.argv[1],'wb').write(p+b)"
    ${demo_icon})
  dccore_add_player(dccore_demo SOURCES examples/demo.c TITLE "dc-core demo" VERSION "0.1" ICON ${demo_icon})
endif()

# dccore_add_recomp(<target> BIN <1ST_READ.BIN> FUNCS <funcs.txt> [BASE 0x8C010000] [SETJMP addr...]
#                   [MAX_FUNC_BYTES n] [NO_DISCOVER] [PREFIX name] [FOUND <found_funcs.txt>])
#
# Recompiles the player's own executable at configure time and builds it as a static
# library. The generated C is derived from the game and stays in the build directory.
# FOUND: function starts a run found by interpreting (dc_found_funcs_path); when the file changes,
# the next build recompiles with them. Missing is fine.
function(dccore_add_recomp target)
  cmake_parse_arguments(A "NO_DISCOVER" "BIN;FUNCS;BASE;MAX_FUNC_BYTES;PREFIX;FOUND" "SETJMP" ${ARGN})
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
  # MAX_FUNC_BYTES: cap a function's span (default 0x40000); a lower cap keeps data that follows the
  # last function before a gap from turning into huge generated files
  if(A_MAX_FUNC_BYTES)
    list(APPEND sj --max-func-bytes ${A_MAX_FUNC_BYTES})
  endif()
  # NO_DISCOVER: FUNCS is complete (e.g. pruned of data that decodes as code); don't add call
  # targets found by scanning, which would bring back starts inside data areas
  if(A_NO_DISCOVER)
    list(APPEND sj --no-discover)
  endif()
  # PREFIX: a second executable of the same program (symbols PREFIX_dc_fn_table, ...; dc_use_code)
  if(A_PREFIX)
    list(APPEND sj --prefix ${A_PREFIX})
  endif()
  if(A_FOUND)
    list(APPEND sj --extra-funcs ${A_FOUND})
    if(EXISTS ${A_FOUND})
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${A_FOUND})
    endif()
  endif()
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
    # generated code is huge and flat: many sections per object, warnings off; functions of 4 KB or
    # more are built without optimisation (sh4recomp.py: MSVC takes many minutes on them otherwise)
    # /MP: compile the generated files in parallel (one cl.exe batch is otherwise serial), at most
    # DCCORE_MSVC_JOBS at a time: with one compiler per core, large generated files (Tennis 2K2's)
    # ran out of memory and the build stopped with D8040
    target_compile_options(${target} PRIVATE /bigobj /w /MP${DCCORE_MSVC_JOBS})
  else()
    # -O1 keeps compile times sane, aliasing must stay off. On the Switch the game is CPU-bound:
    # -O2 there (DCCORE_RECOMP_OPT overrides, e.g. -O1 for a quicker build)
    if(NOT DCCORE_RECOMP_OPT)
      if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch" OR NINTENDO_SWITCH)
        set(DCCORE_RECOMP_OPT -O2)
      else()
        set(DCCORE_RECOMP_OPT -O1)
      endif()
    endif()
    target_compile_options(${target} PRIVATE ${DCCORE_RECOMP_OPT} -w -fno-strict-aliasing)
  endif()
endfunction()

# Tests (no game code): cmake -DDCCORE_TESTS=ON, then ctest
option(DCCORE_TESTS "Build the dc-core tests" OFF)
if(DCCORE_TESTS)
  enable_testing()
  foreach(t runtime pvr_sw disc cdda)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE dccore)
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()
  # CHD: the synthetic GD-ROM as .gdi and as the .chd chdman makes of it (skipped without chdman),
  # read by the runtime (libchdr) and by tools/disc (pure Python)
  set(fx ${CMAKE_CURRENT_BINARY_DIR}/gd_fixture)
  find_program(CHDMAN chdman)
  if(CHDMAN AND NOT EXISTS ${fx}/disc.chd)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_gd_fixture.py ${fx})
    execute_process(COMMAND ${CHDMAN} createcd -i ${fx}/disc.gdi -o ${fx}/disc.chd OUTPUT_QUIET ERROR_QUIET)
  endif()
  add_test(NAME chd_python COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chd.py ${fx})
  add_test(NAME steam_shortcut COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_steam.py)
  if(DCCORE_CHD)
    add_executable(test_chd tests/test_chd.c)
    target_link_libraries(test_chd PRIVATE dccore)
    add_test(NAME chd COMMAND test_chd ${fx})
  endif()
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
    # the same program again, every instruction through the interpreter (runtime/src/interp.c)
    add_test(NAME interp_${n} COMMAND recomp_${n})
    set_tests_properties(interp_${n} PROPERTIES ENVIRONMENT "DC_INTERP_ALL=1")
  endforeach()
endif()
