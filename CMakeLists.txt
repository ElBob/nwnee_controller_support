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

# The preload library.
add_library(nwpad SHARED src/hook/nwpad.c src/hook/backend_stub.c)
target_link_libraries(nwpad PRIVATE nwpad_core dl)
target_compile_definitions(nwpad PRIVATE NWPAD_VERSION="${PROJECT_VERSION}"
  $<$<BOOL:${NWPAD_DEBUG_SURFACES}>:NWPAD_DEBUG_SURFACES=1>)
set_target_properties(nwpad PROPERTIES C_VISIBILITY_PRESET hidden)

enable_testing()
add_executable(test_core tests/unit/test_core.c)
target_link_libraries(test_core PRIVATE nwpad_core)
add_test(NAME core COMMAND test_core)

# Preload smoke test: fake SDL + the real preload library.
add_library(fake_sdl SHARED tests/unit/fake_sdl.c)
add_executable(test_preload tests/unit/test_preload.c)
target_link_libraries(test_preload PRIVATE fake_sdl)
add_test(NAME preload COMMAND test_preload)
set_tests_properties(preload PROPERTIES
  ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:nwpad>;HOME=${CMAKE_BINARY_DIR}")
