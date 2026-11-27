#pragma once

#include <cstdint>
#include <vector>

// STAR 2026-11-25: a hook on VU1 program starts, for a game layer that adds to what a microprogram draws (Shadow Tower
// Abyss: the creature shadow volumes its program drops near the camera, star/shadow_volumes.cpp). Kept apart from
// ps2_vu1_hooks.h, which ps2_runtime.h includes (every generated file would be rebuilt for a change there).
namespace ps2x::vu1hooks
{
    // Called by the VIF1 interpreter for a VIF MSCAL / MSCALF just before VU1 runs the program at startPc (micro
    // memory byte address): vuData = VU1 data memory as the program will find it (the hook may change it: the program
    // then reads the changed data), top = the TOP it gets (quadwords).
    // owner = the object label in force (the game layer's VIF MARK numbers, which the interpreter passes on as owner
    // labels; 0 = none). GIF packets the hook appends to `packets` (whole PACKED packets, each one GIF tag with EOP
    // and its data) go to the GS on PATH1 before the program runs. On the thread that runs VIF1, in VIF order.
    using ProgramStartHook = void (*)(uint32_t startPc, uint32_t top, uint8_t *vuData, uint32_t vuDataSize, uint32_t owner,
                                      std::vector<uint8_t> &packets);
    void setProgramStartHook(ProgramStartHook hook);
    [[nodiscard]] ProgramStartHook programStartHook();
}
