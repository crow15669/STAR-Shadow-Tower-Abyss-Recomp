// STAR 2026-11-25: the captured mouse read between the window loop's own polls.
//
// The window loop polls its messages once per emulated field (60 a second), so the mouse's movement reached the game
// in steps of 16.7 ms: too coarse for a view that follows the mouse in every picture shown (src/star, "live look").
// While raylib has the cursor captured, GLFW has raw mouse input registered for the window, and the movements wait
// as WM_INPUT messages in the window thread's queue until the next poll. drainRawMouse takes those messages (and only
// those: keys, buttons, the wheel and everything else stay for the poll) and returns their sum, so the loop can read
// the mouse about every millisecond without touching raylib's per-poll key and button edges. What GLFW still gets to
// see at a poll (the few that arrived since the last call here) is in raylib's GetMouseDelta: the two never overlap.
// Kept apart from ps2_host_input.cpp because windows.h and raylib.h do not mix.
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef unsigned __int64 QWORD; // NEXTRAWINPUTBLOCK's, left out of the lean headers
#endif

namespace ps2_host_input
{
    // The window thread only. False when nothing waited. Absolute devices (a tablet, a remote desktop) are left out.
    bool drainRawMouse(long &dx, long &dy)
    {
        dx = 0;
        dy = 0;
#if defined(_WIN32)
        alignas(8) static unsigned char buffer[16384];
        bool any = false;
        for (int round = 0; round < 8; ++round)
        {
            UINT size = sizeof(buffer);
            const UINT count = GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(buffer), &size, sizeof(RAWINPUTHEADER));
            if (count == 0u || count == static_cast<UINT>(-1))
                break;
            PRAWINPUT raw = reinterpret_cast<PRAWINPUT>(buffer);
            for (UINT i = 0; i < count; ++i)
            {
                if (raw->header.dwType == RIM_TYPEMOUSE && (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
                {
                    dx += raw->data.mouse.lLastX;
                    dy += raw->data.mouse.lLastY;
                    any = true;
                }
                raw = NEXTRAWINPUTBLOCK(raw);
            }
        }
        return any;
#else
        return false;
#endif
    }
}
