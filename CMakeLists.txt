cmake_minimum_required(VERSION 3.21)

# STAR: Shadow Tower Abyss (SLPS-25217, English-patched) native PC port by static recompilation.
# Modelled on God Hand: Rearmed's top-level CMakeLists.txt (upstream\God-Hand-Rearmed, v0.6.9.5 c329b9c).
project(STAR LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# runtime/: GHR's copy of the PS2Recomp runtime (EE/IOP/VU/GS emulation) and recompiler, see runtime/README.md.
option(STAR_BUILD_RECOMP "Build ps2_recomp from runtime/ps2xRecomp" OFF)
set(URBAN_BUILD_RECOMP ${STAR_BUILD_RECOMP}) # runtime/CMakeLists.txt reads these two names
# runtime/ps2xTest: the offline tools, of which STAR uses vu1_recomp (VU1 microcode ahead of time; tools/build.py vu).
option(STAR_BUILD_TOOLS "Build the offline tools in runtime/ps2xTest (vu1_recomp...)" OFF)
set(URBAN_BUILD_TOOLS ${STAR_BUILD_TOOLS})

# In-game ImGui runtime debugger (threads, kernel, IOP/SIF, GS, DMA, memory).
option(STAR_DEBUG_UI "Build the ImGui runtime debugger into STAR.exe" ON)
set(PS2X_ENABLE_DEBUG_UI ${STAR_DEBUG_UI} CACHE BOOL "" FORCE)
# FFmpeg MPEG decoding stays off: the runtime's own IPU decoder (from the Ace Combat 5 recomp) handles PSS movies.
set(PS2X_ENABLE_FFMPEG OFF CACHE BOOL "" FORCE)
# Controllers through SDL3 (XInput, PlayStation, Switch, Steam controllers).
option(STAR_SDL_INPUT "Read controllers through SDL3" ON)
set(PS2X_ENABLE_SDL_INPUT ${STAR_SDL_INPUT} CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    add_compile_options(-msse4.1)
endif()

# No build machine's folders in the binaries.
if(MSVC)
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/" STAR_SOURCE_PREFIX)
    add_compile_options("/d1trimfile:${STAR_SOURCE_PREFIX}" "/d1trimfile:${CMAKE_SOURCE_DIR}/")
    add_link_options("/PDBALTPATH:%_PDB%")
else()
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=")
endif()

add_subdirectory(runtime EXCLUDE_FROM_ALL)
add_subdirectory(src)

# PCSX2's GS (hardware renderer: Vulkan or Direct3D 11) as pcsx2gs.dll, loaded at run time and the default renderer
# (PS2X_GS_RENDERER=native keeps the runtime's own GS; it is also the fallback when the DLL is missing). From God Hand:
# Rearmed's thirdparty/ (PCSX2 2954ebe + later GS fixes; thirdparty/README.md), with Shadow Tower Abyss's serial and no
# game fixes (thirdparty/pcsx2-gs/compat/Bridge.cpp).
option(STAR_PCSX2_GS "Build PCSX2's GS (pcsx2gs.dll)" ON)
if(STAR_PCSX2_GS AND WIN32)
    add_subdirectory(thirdparty/pcsx2-gs EXCLUDE_FROM_ALL)
    if(TARGET star) # (not in a first configure, before the game code is recompiled: src/CMakeLists.txt)
        add_dependencies(star pcsx2gs)
        target_link_libraries(star PRIVATE pcsx2gs_api)
    endif()
    target_link_libraries(ps2_runtime PRIVATE pcsx2gs_api)
endif()
