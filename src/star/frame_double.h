#pragma once

class PS2Runtime;

namespace star
{
    // Smooth motion out of the game's 30 frames a second (STAR_60FPS=0 turns it off): every game frame is drawn again
    // for each screen refresh that falls into its time, from a camera (and objects) moved to that moment between the
    // previous frame and this one, and each picture is shown at its own refresh. See frame_double.cpp.
    void installFrameDouble(PS2Runtime &runtime);
}
