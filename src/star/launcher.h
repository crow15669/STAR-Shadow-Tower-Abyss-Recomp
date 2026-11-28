#pragma once

#include <filesystem>
#include <functional>
#include <string>

// STAR 2026-11-26: the launcher (the user: "a launcher similar to GHR"; core first: the disc image, every option with
// its note, the presets, Play). A plain dark window before the game, drawn with ImGui on the game's own window (raylib),
// which the game then keeps (ps2_host_display::openHostWindow). Shown when STAR starts without --iso.
namespace star::launcher
{
    struct Context
    {
        const char *title;
        std::filesystem::path settingsFile;
        std::filesystem::path resources; // the folder with fonts/Roboto-Regular.ttf
        // "" when the disc image can be played, else what is wrong with it (in the player's words)
        std::function<std::string(const std::filesystem::path &)> checkDisc;
        // the update (star/update.h): the folder the program is in ("" = no updates offered), this version, and
        // the version before when the program has just been started by an update
        std::filesystem::path program;
        const char *version = "";
        std::string updatedFrom;
    };

    enum class Result
    {
        Play,
        Quit,
        Restart, // an update is in place: the caller starts the program again and ends
    };

    // disc: the disc image to show first (the settings file's, or STAR_ISO); on Play, the one chosen (checked, and
    // saved to the settings file as disc_image). Main thread.
    Result run(const Context &context, std::filesystem::path &disc);

    // The system's file dialog for a disc image (launcher_win32.cpp); false when cancelled. Main thread.
    bool pickDiscImage(void *window, std::filesystem::path &path);
}
