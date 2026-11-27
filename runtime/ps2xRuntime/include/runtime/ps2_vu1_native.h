#pragma once

#include <cstdint>

struct VU1State;

// Native VU1 programs: a game registers hand-written replacements for microprograms it knows (its own code, not the
// runtime's); a run that starts at a registered entry of a matching program goes to the native function instead of
// the compiled (AOT) code, which stays the fallback for everything else (other programs, other microcode, a native
// function that declines). PS2X_VU1_NATIVE (GHR.ini [Runtime]): unset, the programs marked checked; 1, every program
// (to check a new one); 0, none.
//
// A native function gets the VU state at the program's entry (no pending writes: every run starts after the previous
// one ended and its pipelines were flushed) and must leave it as the program would at its end, flushed: registers,
// flags, pc, data memory, its GIF packets kicked, and the VU cycles the run took (the AOT's count, used for timing).
struct Vu1NativeRun
{
    VU1State *state = nullptr;    // registers in and out (vf, vi, acc, q, p, i, clip, mac, status, pc, top, itop)
    const uint8_t *code = nullptr; // VU1 micro memory (the program reads patchable instructions from it)
    uint8_t *data = nullptr;      // VU1 data memory
    uint32_t dataSize = 0;        // 16 KB
    uint64_t budget = 0;          // VU cycles this call may take (the runtime's slice); more: decline
    uint64_t cycles = 0;          // out: VU cycles from the entry to the end, the final XGKICK transfer included
    // XGKICK: sends the GIF packet at that data quadword on PATH1 now (kickPc: the XGKICK's own address, for the
    // runtime's PATH1 filters). The data memory must already hold the packet.
    void (*kick)(void *user, uint32_t qwordAddress, uint32_t kickPc) = nullptr;
    void *user = nullptr;
};

// false: declined before changing anything (the AOT runs the program instead).
using Vu1NativeProgram = bool (*)(Vu1NativeRun &run);

struct Vu1NativeEntry
{
    const char *name = nullptr;
    // The program is recognized by its code: micro memory bytes [codeBegin, codeEnd) hash (FNV-1a 64) to codeHash
    // after the words listed in variable (byte offsets, at most 4) are zeroed: instructions the runtime may patch at
    // upload (e.g. near_geometry.cpp's clip mask), which the native function reads from the code itself.
    uint32_t codeBegin = 0;
    uint32_t codeEnd = 0;
    uint64_t codeHash = 0;
    uint32_t variable[4] = {};
    uint32_t variableCount = 0;
    uint32_t entryPcs[4] = {}; // byte addresses where a run may start (MSCAL / MSCNT)
    uint32_t entryCount = 0;
    Vu1NativeProgram program = nullptr;
    bool checked = false; // checked in the game (claude/native-vu1.md "Done"): on without PS2X_VU1_NATIVE=1
};

void registerVu1NativeProgram(const Vu1NativeEntry &entry);
uint64_t vu1NativeCodeHash(const uint8_t *code, const Vu1NativeEntry &entry);
