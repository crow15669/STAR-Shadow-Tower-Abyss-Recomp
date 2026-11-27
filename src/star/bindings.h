#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// STAR 2026-11-26: rebindable mouse and keyboard controls (the user: "click on the action they want to rebind and
// have it bring up a screen of all possible actions with tabs for different categories"; they chose "pick an action,
// press a key", in the launcher and the F2 menu). Each action has up to four inputs: keyboard keys (the runtime's
// names: "W", "LeftShift", "Return", "Keypad+5"), mouse buttons ("MouseLeft", "MouseRight", "MouseMiddle", "Mouse4",
// "Mouse5") and the wheel ("WheelUp", "WheelDown", or "Wheel": either way). "STAR settings.ini" keeps them as
// bind_<action> = W, Up.
//
// Three kinds of action:
//   - general: one PS2 button (moving, magic, menus...). Their keys are player 1's bindings in the runtime (the
//     PS2X_PAD1_* variables), so they work in the game's menus too; their mouse buttons and wheel are pressed by
//     game_fixes.cpp in play.
//   - ranged: what the gun and the bow do (fire / draw, gun out / away), with one of them in hand.
//   - melee: the swings of the sword swing style chosen (melee_swings); the other styles' are not in use.
// An input may be in only one general action and in only one ranged action, and not in both kinds at once; in only
// one swing of a style. A general action and a swing in use may share one: the swing wins (the wheel switches
// weapons except while "Binds for each swing" makes it the stab and the overhead swing).
namespace star::bindings
{
    enum class Tab
    {
        Movement,
        Combat,
        Menus,
    };
    enum class Kind
    {
        General,
        Ranged,
        Melee,
    };

    struct Action
    {
        const char *id;    // bind_<id> in the settings file
        Tab tab;
        Kind kind;
        const char *label;
        const char *note;  // under the label ("" = none)
        const char *style; // melee: the melee_swings value it belongs to
        const char *padKey;       // general: the runtime's binding key (PS2X_PAD1_<padKey>)
        uint32_t padButton;       // general: the PS2 button bit it presses
        const char *controller;   // general: the controller's binding kept with the keys
        std::vector<const char *> defaults;
    };
    const std::vector<Action> &actions();
    const Action *action(const std::string &id);
    constexpr size_t kMaxInputs = 4;

    using Binds = std::map<std::string, std::vector<std::string>>; // action id -> inputs
    Binds defaults();
    // From the settings file's values (bind_<id>; names lower-cased by the reader are put back): a missing line or one
    // that reads as nothing takes the defaults.
    Binds fromValues(const std::map<std::string, std::string> &values);
    std::string encode(const std::vector<std::string> &inputs); // "W, Up"
    std::vector<std::string> decode(const std::string &line);
    std::string canonical(const std::string &input); // the input's own spelling, "" when it is none
    std::string displayName(const std::string &input); // "Left Shift", "Left button", "Wheel up"
    bool isKey(const std::string &input);
    int keyCode(const std::string &input); // raylib's, 0 for a mouse input
    std::string fromKeyCode(int raylibKey); // "" for a key that cannot be bound (Esc may: it is the default menu key)
    std::string fromMouseButton(int raylibButton);
    std::string fromRuntimeBinding(const std::string &binding); // "Keyboard/J", "Pointer-0/LeftButton" ...
    const std::vector<int> &keyCodes(); // every key that can be bound (raylib codes; Esc among them)

    bool inUse(const Action &action, const std::string &swings); // (a swing of another style is not)
    bool clash(const Action &a, const Action &b);
    bool sameInput(const std::string &a, const std::string &b); // ("Wheel" is the same as "WheelUp" and "WheelDown")
    // `input` into the action's slot (slot >= the inputs' count: added); taken from any action it clashes with.
    // Returns what was taken ("Q was Magic's"), or "".
    std::string assign(Binds &binds, const std::string &id, size_t slot, const std::string &input);
    void remove(Binds &binds, const std::string &id, size_t slot);

    // What game_fixes.cpp reads each frame: for each action its inputs as the game layer sees them.
    struct Input
    {
        enum Type : uint8_t
        {
            MouseButton, // bit = the button's bit in MouseGameInput
            Wheel,       // bit: 1 up, 2 down, 3 either
            Key,         // bit = the key's place in setGameKeys
        } type;
        uint8_t bit;
    };
    struct Resolved
    {
        std::string swings;
        std::vector<std::vector<Input>> inputs; // per action (actions()' order); a general action: its mouse inputs only
        std::vector<int> gameKeys;              // keys of ranged and in-use melee actions (setGameKeys)
        uint16_t tabButton = 0;                 // the PS2 button Tab presses (it is not in the runtime's bindings)
    };
    std::shared_ptr<const Resolved> resolve(const Binds &binds, const std::string &swings);
    // The value of PS2X_PAD1_<padKey>: the keys of the action (not Tab, not those a swing in use took), then its
    // controller binding.
    std::string envValue(const Binds &binds, const std::string &swings, const Action &action);

    // The binds the game plays with (F2 and game_fixes): set at start-up and by the F2 menu. apply() also hands the
    // runtime the keys (at start-up: the environment; later: requestEnvBinding), the game keys and Tab.
    void setLive(const Binds &binds, const std::string &swings, bool runtimeStarted);
    Binds live();
    std::shared_ptr<const Resolved> liveResolved();
}
