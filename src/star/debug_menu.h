#pragma once

#include <cstdint>
#include <filesystem>

class PS2Runtime;
struct R5900Context;

// STAR 2026-11-25: the F2 menu's Debug page (setting debug_menu, off unless asked for): any item into the inventory,
// jumps to the game's maps and to remembered places, any creature in front of the player. For finding faults faster; it changes the game's own data, so it is a cheat too.
namespace star
{
    // From main, after the settings menu. Adds the page when STAR_DEBUG_MENU=1 (the ini's debug_menu = on). The
    // remembered places are kept in "STAR places.txt" next to the settings file.
    void installDebugMenu(PS2Runtime &runtime, const std::filesystem::path &settingsFile);
    // The game thread, once a frame of play (game_fixes.cpp, readInputHook): does what the page asked for.
    // (frame: the frames of play counted so far, for the test switch STAR_DEBUG_TEST)
    void debugMenuGameThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint64_t frame);
}
