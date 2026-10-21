cmake_minimum_required(VERSION 3.24)

project(tmc_native LANGUAGES C ASM)

# Native (arm64 macOS) build of the decompiled game code. The GBA build in the
# repository root is unaffected; everything here is opt-in via PLATFORM_NATIVE.
#
#   cmake -S port -B build/native
#   cmake --build build/native -j
#
# Assets must have been extracted first (make extract_assets / see port/README.md).

set(TMC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(TMC_GAME_VERSION USA CACHE STRING "Game version whose extracted assets to use")
set(TMC_ASSETS_BUILD_DIR ${TMC_ROOT}/build/${TMC_GAME_VERSION} CACHE PATH "Directory holding assets/*.h from asset extraction")

if(NOT EXISTS ${TMC_ASSETS_BUILD_DIR}/assets/gfx_offsets.h)
    message(FATAL_ERROR "Extracted assets not found in ${TMC_ASSETS_BUILD_DIR}. Run asset extraction first (see port/README.md).")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_OSX_ARCHITECTURES arm64)

file(GLOB_RECURSE TMC_GAME_SOURCES CONFIGURE_DEPENDS ${TMC_ROOT}/src/*.c)

# Game C files that cannot be built natively yet (none at the moment).
set(TMC_NATIVE_PENDING
)
foreach(pending ${TMC_NATIVE_PENDING})
    list(REMOVE_ITEM TMC_GAME_SOURCES ${TMC_ROOT}/${pending})
endforeach()

add_library(tmc_game OBJECT ${TMC_GAME_SOURCES})
target_include_directories(tmc_game PRIVATE
    ${TMC_ROOT}/include
    ${TMC_ASSETS_BUILD_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(tmc_game PRIVATE
    PLATFORM_NATIVE=1
    NON_MATCHING=1
    ${TMC_GAME_VERSION}
    ENGLISH
    REVISION=0
)
target_compile_options(tmc_game PRIVATE
    # Decomp idioms that are fine on GBA but noisy here; revisit as code gets ported.
    -Wno-missing-braces
    -Wno-deprecated-non-prototype
    -Wno-shift-negative-value
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-pointer-sign
    -Wno-macro-redefined
    -Wno-extra-semi
    # Keep the 64-bit hazards visible: these are the Phase 1 worklist.
    -Wint-to-pointer-cast
    -Wpointer-to-int-cast
    -Wcast-function-type-mismatch
)

# Game data: the GBA data objects converted to native assembly by port/tools/datagen.
# This needs the GBA build (build/<version>/data/*.o and tmc.elf) and the Python
# environment from port/README.md.
set(TMC_PYTHON ${TMC_ROOT}/.venv/bin/python CACHE FILEPATH "Python with port/requirements.txt installed")
set(TMC_DATA_DIR ${CMAKE_BINARY_DIR}/data)
set(TMC_DATAGEN_COMMAND
    ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/tools
    ${TMC_PYTHON} -m datagen --root ${TMC_ROOT} --gba-build ${TMC_ASSETS_BUILD_DIR} --out ${TMC_DATA_DIR}
)
if(NOT EXISTS ${TMC_DATA_DIR}/sources.txt)
    message(STATUS "Converting GBA data (datagen)")
    execute_process(COMMAND ${TMC_DATAGEN_COMMAND} RESULT_VARIABLE datagen_result)
    if(NOT datagen_result EQUAL 0)
        message(FATAL_ERROR "datagen failed; see the output above")
    endif()
endif()
file(STRINGS ${TMC_DATA_DIR}/sources.txt TMC_DATA_SOURCES)
add_custom_target(datagen
    COMMAND ${TMC_DATAGEN_COMMAND}
    COMMAND ${CMAKE_COMMAND} ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Regenerating native game data"
)

add_library(tmc_data OBJECT ${TMC_DATA_SOURCES})

add_library(tmc_port STATIC
    src/hw.c
    src/data_glue.c
)
target_include_directories(tmc_port PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(tmc_port PRIVATE ${TMC_ROOT}/include ${TMC_ASSETS_BUILD_DIR})
target_compile_definitions(tmc_port PRIVATE PLATFORM_NATIVE=1 NON_MATCHING=1 ${TMC_GAME_VERSION} ENGLISH REVISION=0)

# The executable cannot link until the asm-only code, data tables and BIOS calls
# exist natively, so it is not part of the default build. Build `tmc` explicitly,
# or run `link-report` for the list of unresolved symbols.
add_executable(tmc EXCLUDE_FROM_ALL src/main.c $<TARGET_OBJECTS:tmc_game> $<TARGET_OBJECTS:tmc_data>)
target_link_libraries(tmc PRIVATE tmc_port)

add_custom_target(link-report
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target tmc > ${CMAKE_BINARY_DIR}/link.log 2>&1 || true
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/link_report.py ${CMAKE_BINARY_DIR}/link.log ${TMC_ROOT} ${TMC_ASSETS_BUILD_DIR}
    DEPENDS tmc_game tmc_data tmc_port
    USES_TERMINAL
    COMMENT "Linking tmc and summarizing unresolved symbols"
)
