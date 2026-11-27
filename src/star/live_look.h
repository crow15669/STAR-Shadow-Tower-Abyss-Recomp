#pragma once

#include <cstdint>

// STAR 2026-11-25: the view follows the mouse in every picture shown ("live look").
//
// The game turns the view once per game frame (30 a second) by what the mouse moved since the frame before
// (game_fixes.cpp, mouse and keyboard play), and the in-between pictures (frame_double.cpp) lead from one frame's
// camera to the next over the following frame's time: on its own that puts a mouse movement on screen 30 to 75 ms
// late. So the game thread leaves, for each frame, what the mouse did to its view (a LookFrame, tied to the frame's
// VIF job by a tag), and the VIF worker, right before it runs each in-between picture, turns that picture's camera to
// where the mouse was a fixed short time before the picture's turn on screen (the mouse's whole movement is recorded
// about every millisecond: ps2_host_input::mouseTotalAt).
namespace star
{
    struct LookFrame
    {
        uint64_t seq = 0;
        double yaw = 0.0, pitch = 0.0;   // what the mouse turned the view by in this game frame, radians, as added to
                                         // the game's angles (camera + 0x24 and + 0x20)
        double rawX = 0.0, rawY = 0.0;   // the mouse's whole movement the game has taken up to this frame, counts
        double perCountYaw = 0.0;        // radians per count from here on; 0: the view is not following the mouse now
        double perCountPitch = 0.0;      // (signed: the user's up/down choice)
        double pitchNow = 0.0;           // the view's pitch after this frame (the game keeps it within -60..+35 degrees)
        // The margins of the game's "in view" test from this frame's input read on (game_fixes.cpp, inViewHook):
        // how far a camera may be turned from the one the test uses, radians, each way of the game's two angles
        // (0 0 0 0: the test is the game's own). They hold for the scenery chosen in this frame, which is the job
        // tagged with the frame BEFORE this one.
        double marginYawNeg = 0.0, marginYawPos = 0.0, marginPitchNeg = 0.0, marginPitchPos = 0.0;
        // The game stopped reading the input after this frame (an event or a menu took the controls): what the
        // mouse does from here on never reaches the game's camera.
        bool last = false;
    };

    // The game thread, once per frame of play with the mouse: keeps the frame and tags the frame's VIF job with it.
    void publishLookFrame(const LookFrame &frame);
    // The game thread, as a frame's job is made: the game will not read the input next frame, so the newest look
    // frame is the last of its run (LookFrame::last).
    void markLastLookFrame();

    // In a job's note (the player's own pieces' matrices, 64 bytes each): a record whose words 3 to 15 are this is not a
    // matrix but the place in the world (three floats) of a sprite of one of the player's effects.
    constexpr uint32_t kOwnPointMark = 0x53504F57u;
    // The VIF worker: the frame a job's tag (ps2x::asyncvif::currentJobTag) names. False: none (any more).
    bool takeLookFrame(uint64_t seq, LookFrame &frame);
    // The mouse's whole movement at a time of the steady clock (nanoseconds), and when it last moved. The real mouse,
    // or a test's (STAR_KBM_TEST, STAR_LIVE_TEST). Any thread.
    bool lookMouseAt(long long steadyNs, double &x, double &y);
    long long lookMouseMovedNs();
}
