cmake_minimum_required(VERSION 3.16)
project(nwpad VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Werror)

option(NWPAD_DEBUG_SURFACES "Build the control socket and RE commands (never in release)" ON)

# Pure logic: no SDL, no game, no I/O. Unit-tested.
add_library(nwpad_core STATIC src/core/nwpad_core.c)
target_include_directories(nwpad_core PUBLIC src/core)
target_link_libraries(nwpad_core PUBLIC m)
set_target_properties(nwpad_core PROPERTIES C_VISIBILITY_PRESET hidden)

# Signatures, compiled from signatures/ee.yaml (override to test other entries).
set(NWPAD_SIGNATURES ${CMAKE_SOURCE_DIR}/signatures/ee.yaml CACHE FILEPATH "Signature file")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(NWPAD_SIG_HEADER ${CMAKE_BINARY_DIR}/gen/signatures_gen.h)
add_custom_command(
  OUTPUT ${NWPAD_SIG_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
  COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_signatures.py
          ${NWPAD_SIGNATURES} ${NWPAD_SIG_HEADER}
  DEPENDS ${NWPAD_SIGNATURES} ${CMAKE_SOURCE_DIR}/tools/gen_signatures.py
  COMMENT "Generating signatures_gen.h from ${NWPAD_SIGNATURES}")

# The preload library.
add_library(nwpad SHARED src/hook/nwpad.c src/hook/sigs.c src/hook/backend.c
  ${NWPAD_SIG_HEADER}
  $<$<BOOL:${NWPAD_DEBUG_SURFACES}>:src/hook/control.c>)
target_include_directories(nwpad PRIVATE ${CMAKE_BINARY_DIR}/gen)
find_package(Threads REQUIRED)
target_link_libraries(nwpad PRIVATE nwpad_core dl Threads::Threads)
target_compile_definitions(nwpad PRIVATE NWPAD_VERSION="${PROJECT_VERSION}"
  $<$<BOOL:${NWPAD_DEBUG_SURFACES}>:NWPAD_DEBUG_SURFACES=1>)
set_target_properties(nwpad PROPERTIES C_VISIBILITY_PRESET hidden)

enable_testing()
add_executable(test_core tests/unit/test_core.c)
target_link_libraries(test_core PRIVATE nwpad_core)
add_test(NAME core COMMAND test_core)

# Preload smoke test: the real preload library against a fake SDL that is
# statically linked and exported, like the game's. Linux x86-64 only (the fake
# SDL's stubs are x86-64 assembly, and LD_PRELOAD is Linux).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_executable(test_preload tests/unit/test_preload.c tests/unit/fake_sdl.c)
  set_target_properties(test_preload PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(test_preload PRIVATE Threads::Threads)
  add_test(NAME preload COMMAND test_preload)
  set(preload_env "LD_PRELOAD=$<TARGET_FILE:nwpad>;HOME=${CMAKE_BINARY_DIR}")
  if(NWPAD_DEBUG_SURFACES)
    list(APPEND preload_env "NWPAD_SOCKET=1;XDG_RUNTIME_DIR=${CMAKE_BINARY_DIR}")
  endif()
  set_tests_properties(preload PROPERTIES ENVIRONMENT "${preload_env}")
endif()
