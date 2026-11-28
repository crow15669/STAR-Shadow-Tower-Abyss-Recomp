#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

// STAR 2026-11-27: the set-up of a controller the program does not know by itself (the user: "it would just be the
// player mapping their generic controller buttons to a playstation controller"). SDL knows most controllers as
// gamepads (their buttons by name: those play with the fixed layout in main.cpp and are not set up here, at the
// user's word: the game's own control types stay the way to choose what a button does). A controller SDL does not
// know (many generic USB ones) only has numbered buttons, axes and hats: the player says once, in the launcher,
// which of them is which button of the PS2 controller. "STAR settings.ini" keeps it as
//   joy_device = <the controller's name>
//   joy_cross = Button 3        joy_up = Hat 1 North        joy_lstick_up = -Axis 2
// Each is added to player 1's bindings beside the gamepad's (the runtime's "SDL-0/Button 3"), and the runtime reads
// them from the controller of that name only (ps2_host_input::setRawJoystickName).
namespace star::padmap
{
    struct Entry
    {
        const char *id;       // joy_<id> in the settings file
        const char *group;    // the heading it is listed under
        const char *label;    // "Cross"
        const char *where;    // "bottom" ("" = none)
        const char *padKey;   // the runtime's binding key (PS2X_PAD1_<padKey>)
        const char *opposite; // a stick's row: the key of the other direction (the same axis, the other sign);
                              // nullptr: a button
    };
    const std::vector<Entry> &entries();
    const Entry *entry(const std::string &id);

    struct Map
    {
        std::string device;                        // the controller's name
        std::map<std::string, std::string> inputs; // entry id -> "Button 3", "+Axis 2", "Hat 1 North"
    };
    int count(const Map &map); // the entries that are set
    // From the settings file's values (joy_device, joy_<id>).
    Map fromValues(const std::map<std::string, std::string> &values);
    bool inValues(const std::map<std::string, std::string> &values); // the file has joy_ lines
    // The file's lines: joy_device and one joy_<id> for every entry ("" = not set).
    void addLines(const Map &map, std::vector<std::pair<std::string, std::string>> &lines);

    std::string canonical(const std::string &input); // the input's own spelling ("Button 3"), "" when it is none
    std::string displayName(const Entry &entry, const std::string &input); // "Button 3", "Hat 1 up", "Axis 2 (-)"
    // `input` for the entry (a stick's row takes an axis only: false otherwise). Taken from any other entry that has
    // it; `taken` says so ("Button 3 was Circle's"), or is "".
    bool assign(Map &map, const std::string &id, const std::string &input, std::string &taken);
    // What the set-up adds to PS2X_PAD1_<padKey>: "SDL-0/Button 3" ("" = nothing).
    std::string envPart(const Map &map, const std::string &padKey);

    // The set-up the game plays with: set once at start-up (it also tells the runtime the controller's name).
    void setLive(const Map &map);
    std::string liveEnvPart(const std::string &padKey);
}
