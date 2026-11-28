# STAR.exe: src/ (the Shadow Tower layer) + the recompiled game code (generated/) + the runtime.
# The game-code settings are GHR's (src/CMakeLists.txt there), tuned on ~10k-file generated trees.

set(STAR_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH "The recompiled game code (tools/build.py generate)")
if(NOT EXISTS "${STAR_GENERATED_DIR}/register_functions.cpp")
    # A first build from scratch: the recompiler has to be built before there is any recompiled code, so this is not
    # an error. Only the tools are configured; tools/build.py generate configures again once the code is there.
    message(STATUS "STAR: no recompiled game code in ${STAR_GENERATED_DIR} yet: only the tools are configured "
                   "(run 'python -I tools/build.py generate').")
    return()
endif()
set(STAR_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/include")
file(GLOB STAR_GENERATED_SOURCES CONFIGURE_DEPENDS "${STAR_GENERATED_DIR}/*.cpp")

# /d2OptimizeHugeFunctions forces full optimization of functions MSVC would otherwise skip; one huge function can take
# 40+ minutes with it (Urban Reign). Files above the threshold keep MSVC's default handling, two at a time (memory).
set(STAR_HUGE_SOURCE_BYTES 900000 CACHE STRING "Generated files larger than this skip /d2OptimizeHugeFunctions")
set(STAR_HUGE_SOURCES "")
foreach(source IN LISTS STAR_GENERATED_SOURCES)
    file(SIZE "${source}" sourceBytes)
    if(sourceBytes GREATER STAR_HUGE_SOURCE_BYTES AND NOT source MATCHES "register_functions\\.cpp$")
        list(APPEND STAR_HUGE_SOURCES "${source}")
    endif()
endforeach()
if(STAR_HUGE_SOURCES)
    list(REMOVE_ITEM STAR_GENERATED_SOURCES ${STAR_HUGE_SOURCES})
endif()
list(LENGTH STAR_HUGE_SOURCES STAR_HUGE_COUNT)
message(STATUS "STAR: ${STAR_HUGE_COUNT} generated files over ${STAR_HUGE_SOURCE_BYTES} bytes go to star_game_code_huge")

set(STAR_HUGE_STUB "${CMAKE_CURRENT_BINARY_DIR}/star_generated/huge_stub.cpp")
file(WRITE "${STAR_HUGE_STUB}.tmp" "// Keeps star_game_code_huge non-empty.\nnamespace star { int hugeStub() { return 0; } }\n")
file(COPY_FILE "${STAR_HUGE_STUB}.tmp" "${STAR_HUGE_STUB}" ONLY_IF_DIFFERENT)
add_library(star_game_code_huge STATIC ${STAR_HUGE_SOURCES} "${STAR_HUGE_STUB}")
target_include_directories(star_game_code_huge PUBLIC "${STAR_GENERATED_DIR}")
target_link_libraries(star_game_code_huge PUBLIC ps2_runtime)
set_property(GLOBAL APPEND PROPERTY JOB_POOLS star_huge=2)
set_property(TARGET star_game_code_huge PROPERTY JOB_POOL_COMPILE star_huge)

add_library(star_game_code STATIC ${STAR_GENERATED_SOURCES})
target_link_libraries(star_game_code PUBLIC star_game_code_huge)
target_include_directories(star_game_code PUBLIC "${STAR_GENERATED_DIR}")
target_link_libraries(star_game_code PUBLIC ps2_runtime)
set_target_properties(star_game_code PROPERTIES UNITY_BUILD OFF)
target_precompile_headers(star_game_code PRIVATE
    "${STAR_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
    "${STAR_RUNTIME_INCLUDE}/ps2_runtime.h"
    "${STAR_GENERATED_DIR}/ps2_recompiled_functions.h"
    "${STAR_GENERATED_DIR}/ps2_recompiled_stubs.h"
    "${STAR_RUNTIME_INCLUDE}/ps2_syscalls.h"
    "${STAR_RUNTIME_INCLUDE}/ps2_stubs.h"
)
if(MSVC)
    target_compile_options(star_game_code_huge PRIVATE /bigobj /fp:precise /GS- /W0)
    # No /GL over the generated code, ever (link time and memory blow up).
    target_compile_options(star_game_code PRIVATE /bigobj /fp:precise /GS- /d2OptimizeHugeFunctions /W0 /MP $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    # The function table's initializer runs once at start-up: no optimizer, no precompiled header.
    set_source_files_properties("${STAR_GENERATED_DIR}/register_functions.cpp" PROPERTIES
        COMPILE_OPTIONS "/Od" SKIP_PRECOMPILE_HEADERS ON)
endif()

add_executable(star main.cpp star/sha256.cpp star/host_sampler.cpp star/game_fixes.cpp star/window_watch.cpp star/frame_double.cpp star/settings_menu.cpp star/debug_menu.cpp star/shadow_volumes.cpp star/options.cpp star/bindings.cpp star/pad_map.cpp star/play_log.cpp)
set_target_properties(star PROPERTIES OUTPUT_NAME STAR RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(star PRIVATE star_game_code ps2_runtime)
# The launcher (star/launcher.cpp): ImGui on the game's raylib window, as the runtime's F12 debugger (PS2X_ENABLE_DEBUG_UI).
if(TARGET rlImGui)
    target_sources(star PRIVATE star/launcher.cpp star/launcher_win32.cpp)
    target_link_libraries(star PRIVATE rlImGui)
    # The launcher's update (star/update.cpp): WinHTTP for the one request, and inflate (tinfl) from the miniz that
    # the runtime's libchdr brings along, to unpack the release zip.
    target_sources(star PRIVATE star/update.cpp star/update_win32.cpp)
    target_link_libraries(star PRIVATE miniz)
    target_include_directories(star PRIVATE "$<TARGET_PROPERTY:miniz,SOURCE_DIR>")
    if(WIN32)
        target_link_libraries(star PRIVATE winhttp shell32)
    endif()
    target_compile_definitions(star PRIVATE STAR_HAS_LAUNCHER=1)
endif()
target_include_directories(star PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib")

if(PS2X_ENABLE_DEBUG_UI)
    target_sources(star PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/ps2_debug_panel.cpp")
    target_link_libraries(star PRIVATE rlImGui imgui)
    target_compile_definitions(star PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

if(MSVC)
    target_compile_options(star PRIVATE /fp:precise /W3 /utf-8)
    target_compile_definitions(star PRIVATE _CRT_SECURE_NO_WARNINGS)
    # /FORCE:MULTIPLE: WinAPI vs raylib CloseWindow clash. Large stack: deep guest call chains run on the host stack.
    # /LTCG: ps2_runtime is compiled with /GL in release mode (runtime/ps2xRuntime/cmake/ReleaseMode.cmake).
    target_link_options(star PRIVATE /FORCE:MULTIPLE /STACK:16777216 $<$<CONFIG:RelWithDebInfo>:/LTCG>)
    # No console window behind the game (as the runtime's own runner): the program writes its log itself
    # (star/play_log.cpp). A test's or a script's "STAR.exe > file" works as before.
    target_link_options(star PRIVATE $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                                     $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>)
    target_link_libraries(star PRIVATE dbghelp winmm)
endif()

# VU1 microcode compiled ahead of time (tools/build.py vu: the uploads captured with PS2X_VU1_CAPTURE and found in the
# program, through vu1_recomp, into generated/vu1). Code not compiled stays interpreted (slow: the first in-game runs
# were at a third of full speed, the VU1 thread all in the interpreter). As GHR's src/CMakeLists.txt.
file(GLOB STAR_VU1_SOURCES CONFIGURE_DEPENDS "${STAR_GENERATED_DIR}/vu1/*.cpp")
if(STAR_VU1_SOURCES)
    add_library(star_vu1_code STATIC ${STAR_VU1_SOURCES})
    target_include_directories(star_vu1_code PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/vu")
    target_link_libraries(star_vu1_code PUBLIC ps2_runtime)
    target_link_libraries(star PRIVATE star_vu1_code)
    target_compile_definitions(star PRIVATE STAR_HAS_VU1_PROGRAMS=1)
    if(MSVC)
        target_compile_options(star_vu1_code PRIVATE /bigobj /fp:precise /W0 /MP /d2OptimizeHugeFunctions $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
endif()

# SDL3.dll next to STAR.exe (controllers).
if(TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET star POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:star>)
endif()
