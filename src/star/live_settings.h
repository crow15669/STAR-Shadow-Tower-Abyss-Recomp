#pragma once

#include <atomic>
#include <filesystem>

class PS2Runtime;

namespace star
{
    // Settings that can change while the game runs (the F2 menu's Graphics page, settings_menu.cpp). Read on the
    // threads that use them: the game thread (the picture passes), the VIF worker (60 fps), the GS thread (the menu).
    struct LiveSettings
    {
        std::atomic<int> resolution{1};         // rendering resolution, a multiple of 640x448
        std::atomic<bool> fxaa{false};
        std::atomic<bool> nearShadows{false};   // creature shadows kept up close (shadow_volumes.cpp; STAR_NEAR_SHADOWS)
        std::atomic<bool> shadowWalls{false};   // creature shadows stop at walls (shadow_volumes.cpp; STAR_SHADOW_WALLS)
        std::atomic<bool> lateInput{false};     // the pads read one field later (game_fixes.cpp; STAR_LATE_INPUT)
        std::atomic<bool> noSelfShadow{false};  // creatures do not shadow themselves (main.cpp, STAR_SELF_SHADOWS=0)
        std::atomic<bool> sharpShadows{false};  // the shadow buffer at the picture's own resolution (main.cpp, STAR_SHARP_SHADOWS)
        std::atomic<bool> smooth60{true};       // in-between pictures (frame_double.cpp)
        std::atomic<bool> flickerFilter{false}; // the game's line blend (game_fixes.cpp)
        std::atomic<bool> glow{true};           // the game's glow (game_fixes.cpp)
        // Gameplay tweaks (the menu's Gameplay page; game_fixes.cpp). Off = the game as it was made.
        std::atomic<bool> textFixes{true};      // texts of the English patch that overlap on screen are laid out anew
        std::atomic<bool> analogAim{false};     // right stick: turning and looking up/down follow how far it is pushed
        std::atomic<int> aimTurnPercent{100};   // turning speed with the stick fully over, 100 = the game's own
        std::atomic<int> aimLookPercent{100};   // the same for looking up and down
        std::atomic<bool> mouseKeyboard{false}; // mouse and keyboard play: mouse look, clicks and wheel (game_fixes.cpp)
        std::atomic<int> mouseSensitivity{100}; // percent; 100 = 0.05 degrees a count
        std::atomic<bool> mouseInvertY{false};  // the mouse pushed away looks down
        std::atomic<bool> gunHold{false};       // the right button keeps a gun out only while held (off: press, press)
        // melee swings with the mouse: 0 = the left button swings the way the view last moved; 1 = holding the right
        // button locks the view and a flick swings ("combat mode"); 2 = a button for each swing (left, right, wheel
        // up, wheel down), and Tab is the other weapon
        std::atomic<int> meleeMode{2};
        // the game's own slow-downs of turning (ailments, load, ground) on the mouse: 0 = none, 1 = softened (the
        // square root of the game's factor: half speed in the game = 71% with the mouse), 2 = in full
        std::atomic<int> mouseSlowdown{2};
        std::atomic<bool> liveLook{true};       // every picture shown follows the mouse (live_look.h); off: per game frame
        std::atomic<bool> recordPictures{false}; // the renderer keeps the last pictures shown; F1 writes them out
        std::atomic<int> recordKeep{5};          // how many F1 catches are kept (0 = all); record_keep
        std::atomic<bool> debugMenu{false};      // the F2 menu's Debug page (debug_menu): read at start-up, kept for the next
        // For finding faults, not kept in the settings file:
        std::atomic<bool> moveObjects{true};    // in-between pictures move objects, not only the camera
        std::atomic<int> pictureEvery{0};       // 0 = chosen by the program, n = a picture for every n-th screen refresh
        // Told by frame_double.cpp for the menu: the screen's rate and the pictures made per second, both x100.
        std::atomic<int> screenHz100{0};
        std::atomic<int> pictures100{0};
    };
    LiveSettings &live();

    // Takes the starting values from the environment (which "STAR settings.ini" has already filled in) and adds the
    // Graphics page to the in-game menu. `settingsFile` is where changes are written back (empty: nowhere).
    void installSettingsMenu(const std::filesystem::path &settingsFile);
    // After live().mouseKeyboard changed: the mouse is captured for the game, or left alone (game_fixes.cpp).
    void applyMouseKeyboard();
}
