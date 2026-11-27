#pragma once

#include "ps2x/iop/iop_pad.h"

#include <cstdint>

class PS2Runtime;

// A host tap on the pad the emulated IOP polls (ps2_iop_host.cpp readPad / readPadSlot: PADMAN's SIO2 commands ask
// the host for the DualShock's state once a vblank or so): called on the IOP thread with the state the host
// produced, which the tap may read or rewrite (the game's libpad sees what it leaves). One tap at a time; nullptr
// removes it. For input recording and playback (kamite/test_mode.cpp). The HLE scePadRead stub is not on this
// path: a game whose own libpad runs (the usual case) never calls it.
namespace ps2x::pad
{
    using StateTap = void (*)(uint32_t port, uint32_t slot, ps2x::iop::PadInputState &state);
    void setStateTap(StateTap tap);
    // STAR 2026-11-25: the emulated IOP caught up with the EE's time now: its batch on the worker thread joined (what
    // it did to the EE side, PADMAN's SIF DMA into the game's libpad buffer among it, done) and the EE cycles not yet
    // handed to it run. For a game layer that reads the pad at a moment of its own (star/game_fixes.cpp, late input).
    // The EE thread only.
    void catchUpIop(PS2Runtime &runtime);
    // STAR 2026-11-25: the pad as the emulated IOP's next poll of it would get it now (what the window's loop last took
    // in, the scripted pad, then the tap): the same as PADMAN's SIO2 commands ask the host for. False: not connected.
    bool hostPadState(uint32_t port, uint32_t slot, ps2x::iop::PadInputState &state);
}
