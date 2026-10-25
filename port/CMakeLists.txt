cmake_minimum_required(VERSION 3.24)

# Runs on macOS 12 and later. The architectures default to this Mac's; for a
# universal app (Apple silicon and Intel) configure with
# -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64". Both must be set before project();
# older build directories have the target cached empty.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0 CACHE STRING "Oldest macOS the programs run on" FORCE)
endif()

project(tmc_native VERSION 0.1 LANGUAGES C OBJC ASM)

# Native (macOS, arm64 and/or x86_64) build of the decompiled game code. The GBA build in the
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

option(TMC_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(TMC_SANITIZE)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
# Anything newer than the deployment target needs an @available check.
add_compile_options(-Werror=unguarded-availability-new)

file(GLOB_RECURSE TMC_GAME_SOURCES CONFIGURE_DEPENDS ${TMC_ROOT}/src/*.c)

# Game C files left out of the native build. eeprom.c drives cartridge hardware
# (port/src/eeprom.c emulates it).
set(TMC_NATIVE_PENDING
    src/eeprom.c
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
    # Writable globals into the save state segment (port/include/port/state.h).
    -include ${CMAKE_CURRENT_SOURCE_DIR}/include/port/state_segment.h
    # The game relies on signed overflow and sign-bit shifts wrapping, as on the GBA.
    -fwrapv
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
#
# TMC_BAKE_DATA=ON (the default, for development) builds the game's assets (what
# asset extraction copies out of baserom.gba) into the programs. OFF leaves them
# out: the programs extract them from the player's ROM on first launch
# (port/src/romdata.c), so the build can be distributed. Data written in the
# repository's sources is built in either way.
option(TMC_BAKE_DATA "Build the game's assets from the ROM into the programs (not distributable)" ON)
set(TMC_PYTHON ${TMC_ROOT}/.venv/bin/python CACHE FILEPATH "Python with port/requirements.txt installed")
set(TMC_DATA_DIR ${CMAKE_BINARY_DIR}/data)
if(TMC_BAKE_DATA)
    set(TMC_DATA_MODE bake)
    set(TMC_DATAGEN_MODE_ARG)
else()
    set(TMC_DATA_MODE rom)
    set(TMC_DATAGEN_MODE_ARG --no-bake)
endif()
set(TMC_DATAGEN_COMMAND
    ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/tools
    ${TMC_PYTHON} -m datagen --root ${TMC_ROOT} --gba-build ${TMC_ASSETS_BUILD_DIR} --out ${TMC_DATA_DIR}
    ${TMC_DATAGEN_MODE_ARG}
)
set(TMC_DATA_MODE_FOUND none)
if(EXISTS ${TMC_DATA_DIR}/mode.txt)
    file(STRINGS ${TMC_DATA_DIR}/mode.txt TMC_DATA_MODE_FOUND)
endif()
# datagen's inputs: its own code and hints, and the GBA link (tmc.elf changes
# with any game data or code). When one is newer than the last run, the build
# reconfigures and datagen runs again, so the data never goes stale.
file(GLOB TMC_DATAGEN_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/datagen/*.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/datagen/hints.txt)
list(APPEND TMC_DATAGEN_INPUTS ${TMC_ROOT}/tmc.elf ${TMC_ROOT}/linker.ld)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TMC_DATAGEN_INPUTS})
set(TMC_DATAGEN_STAMP ${TMC_DATA_DIR}/datagen.stamp)
set(TMC_DATAGEN_REASON)
if(NOT EXISTS ${TMC_DATA_DIR}/sources.txt OR NOT EXISTS ${TMC_DATAGEN_STAMP})
    set(TMC_DATAGEN_REASON "no converted data yet")
elseif(NOT TMC_DATA_MODE_FOUND STREQUAL TMC_DATA_MODE)
    set(TMC_DATAGEN_REASON "data mode changed")
else()
    foreach(input ${TMC_DATAGEN_INPUTS})
        if(EXISTS ${input} AND ${input} IS_NEWER_THAN ${TMC_DATAGEN_STAMP})
            set(TMC_DATAGEN_REASON "${input} changed")
            break()
        endif()
    endforeach()
endif()
if(TMC_DATAGEN_REASON)
    message(STATUS "Converting GBA data (datagen): ${TMC_DATAGEN_REASON}")
    execute_process(COMMAND ${TMC_DATAGEN_COMMAND} RESULT_VARIABLE datagen_result)
    if(NOT datagen_result EQUAL 0)
        message(FATAL_ERROR "datagen failed; see the output above")
    endif()
    file(TOUCH ${TMC_DATAGEN_STAMP})
endif()
file(STRINGS ${TMC_DATA_DIR}/sources.txt TMC_DATA_SOURCES)
add_custom_target(datagen
    COMMAND ${TMC_DATAGEN_COMMAND}
    COMMAND ${CMAKE_COMMAND} -E touch ${TMC_DATAGEN_STAMP}
    COMMAND ${CMAKE_COMMAND} ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Regenerating native game data"
)

add_library(tmc_data OBJECT ${TMC_DATA_SOURCES})

file(GLOB TMC_PORT_ASM_SOURCES CONFIGURE_DEPENDS src/asm/*.c)
# The m4a engine's assembly in C, the CGB channels, WAV output (port/src/sound).
file(GLOB TMC_PORT_SOUND_SOURCES CONFIGURE_DEPENDS src/sound/*.c)
add_library(tmc_port STATIC
    src/hw.c
    src/bios.c
    src/interrupts.c
    src/data_glue.c
    src/eeprom.c
    src/trace.c
    src/mapdump.c
    src/entitylistcheck.c
    src/ppu.c
    src/frame.c
    src/gfxdebug.c
    src/romdata.c
    src/state.c
    ${TMC_PORT_ASM_SOURCES}
    ${TMC_PORT_SOUND_SOURCES}
)
# The port's game state (emulated hardware, EEPROM, sound engine) goes into the
# save state segment with the game's; renderers, tools and caches stay out.
set(TMC_PORT_STATE_SOURCES
    src/hw.c
    src/bios.c
    src/interrupts.c
    src/data_glue.c
    src/eeprom.c
    src/sound/m4a_engine.c
    src/sound/m4a_mixer.c
    src/sound/psg.c
    ${TMC_PORT_ASM_SOURCES}
)
set_source_files_properties(${TMC_PORT_STATE_SOURCES} TARGET_DIRECTORY tmc_port PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/include/port/state_segment.h")
target_include_directories(tmc_port PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(tmc_port PRIVATE ${TMC_ROOT}/include ${TMC_ASSETS_BUILD_DIR} ${TMC_DATA_DIR})
target_compile_definitions(tmc_port PRIVATE PLATFORM_NATIVE=1 NON_MATCHING=1 ${TMC_GAME_VERSION} ENGLISH REVISION=0)

# `link-report` lists unresolved symbols if the link ever breaks again.
add_executable(tmc src/headless.c $<TARGET_OBJECTS:tmc_game> $<TARGET_OBJECTS:tmc_data>)
target_link_libraries(tmc PRIVATE tmc_port z)

# The macOS app: a window with the Metal renderer, keyboard and gamepad input,
# Core Audio sound, running the game at 59.73 Hz. Builds build/native/TMC.app.
set(TMC_APP_SHADERS src/macos/shaders.metal)
# The window's subtitle names the room the player is in.
set(TMC_ROOM_NAMES ${CMAKE_BINARY_DIR}/room_names.c)
add_custom_command(
    OUTPUT ${TMC_ROOM_NAMES}
    COMMAND ${TMC_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/room_names.py
        ${TMC_ROOT}/include/area.h ${TMC_ROOT}/include/roomid.h ${TMC_ROOM_NAMES}
    DEPENDS scripts/room_names.py ${TMC_ROOT}/include/area.h ${TMC_ROOT}/include/roomid.h
    COMMENT "Generating the room name table"
)
add_executable(tmc_app MACOSX_BUNDLE
    src/macos/app.m
    src/macos/audio.c
    src/macos/debugwindows.m
    src/macos/displaysync.m
    src/macos/game.c
    src/macos/input.m
    src/macos/latency.c
    src/macos/renderer.m
    src/macos/romsetup.m
    src/macos/roomname.c
    ${TMC_ROOM_NAMES}
    src/macos/selftest.m
    src/macos/settings.m
    ${TMC_APP_SHADERS}
    $<TARGET_OBJECTS:tmc_game>
    $<TARGET_OBJECTS:tmc_data>
)
# Shaders ship as source and are compiled at launch, so building needs no
# separately installed Metal toolchain.
set_source_files_properties(${TMC_APP_SHADERS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_target_properties(tmc_app PROPERTIES
    OUTPUT_NAME TMC
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/macos/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "The Minish Cap"
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.awkitsune.tmc-macos
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
)
# The Debug menu is on by default except in Release builds (Settings switches
# it); its Load Checkpoint lists port/states in this source tree. Distributable
# builds leave the path out, so the app carries no path from the build machine.
if(TMC_BAKE_DATA)
    set(TMC_CHECKPOINT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/states")
else()
    set(TMC_CHECKPOINT_DIR "")
endif()
target_compile_definitions(tmc_app PRIVATE
    TMC_DEBUG_MENU_DEFAULT=$<IF:$<CONFIG:Release,MinSizeRel>,0,1>
    TMC_CHECKPOINT_DIR="${TMC_CHECKPOINT_DIR}"
)
# roomname.c reads the game's globals, so it builds with the game's headers.
set_source_files_properties(src/macos/roomname.c PROPERTIES
    INCLUDE_DIRECTORIES "${TMC_ROOT}/include;${TMC_ASSETS_BUILD_DIR}"
    COMPILE_DEFINITIONS "PLATFORM_NATIVE=1;NON_MATCHING=1;${TMC_GAME_VERSION};ENGLISH;REVISION=0")
target_include_directories(tmc_app PRIVATE src/macos)
target_compile_options(tmc_app PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
target_compile_options(tmc_app PRIVATE -Wall -Wno-unused-parameter)
target_link_libraries(tmc_app PRIVATE
    tmc_port z
    "-framework Cocoa" "-framework Metal" "-framework MetalKit"
    "-framework GameController" "-framework QuartzCore"
    "-framework AudioToolbox" "-framework CoreAudio" "-framework UniformTypeIdentifiers"
)
# Ad-hoc sign the whole bundle so macOS treats it as one app (Gatekeeper,
# controller and window permissions).
add_custom_command(TARGET tmc_app POST_BUILD
    COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:tmc_app>
    COMMENT "Signing TMC.app (ad hoc)"
)

# tmc-songs: lists the songs and renders them to WAV with the native m4a engine.
set(TMC_SONG_NAMES ${CMAKE_BINARY_DIR}/song_names.c)
add_custom_command(
    OUTPUT ${TMC_SONG_NAMES}
    COMMAND ${TMC_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/song_names.py
        ${TMC_ROOT}/include/sound.h ${TMC_ROOT}/src/sound.c ${TMC_SONG_NAMES}
    DEPENDS scripts/song_names.py ${TMC_ROOT}/include/sound.h ${TMC_ROOT}/src/sound.c
    COMMENT "Generating the song name table"
)
add_executable(tmc_songs src/tools/tmc_songs.c ${TMC_SONG_NAMES} $<TARGET_OBJECTS:tmc_game> $<TARGET_OBJECTS:tmc_data>)
set_target_properties(tmc_songs PROPERTIES OUTPUT_NAME tmc-songs)
target_include_directories(tmc_songs PRIVATE ${TMC_ROOT}/include ${TMC_ASSETS_BUILD_DIR} src/tools)
target_compile_definitions(tmc_songs PRIVATE PLATFORM_NATIVE=1 NON_MATCHING=1 ${TMC_GAME_VERSION} ENGLISH REVISION=0)
target_link_libraries(tmc_songs PRIVATE tmc_port z)

# Scanline renderer checks (per-line HBlank DMA effects), no game code needed:
#   cmake --build build/native --target scanline_test && build/native/scanline_test
add_executable(scanline_test EXCLUDE_FROM_ALL src/tools/scanline_test.c src/frame.c src/hw.c src/ppu.c)
target_include_directories(scanline_test PRIVATE include)
target_link_libraries(scanline_test PRIVATE z)

# Mosaic and tile-fetch checks of the picture processor without the game:
#   cmake --build build/native --target ppu_test && build/native/ppu_test
add_executable(ppu_test EXCLUDE_FROM_ALL src/tools/ppu_test.c src/frame.c src/hw.c src/ppu.c)
target_include_directories(ppu_test PRIVATE include)
target_link_libraries(ppu_test PRIVATE z)


add_custom_target(link-report
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target tmc > ${CMAKE_BINARY_DIR}/link.log 2>&1 || true
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/link_report.py ${CMAKE_BINARY_DIR}/link.log ${TMC_ROOT} ${TMC_ASSETS_BUILD_DIR}
    DEPENDS tmc_game tmc_data tmc_port
    USES_TERMINAL
    COMMENT "Linking tmc and summarizing unresolved symbols"
)
