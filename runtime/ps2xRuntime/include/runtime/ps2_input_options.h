#pragma once

#include "ps2x/iop/iop_pad.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ps2_host_input
{
    constexpr uint32_t kInvertRightX = 1u;
    constexpr uint32_t kInvertRightY = 2u;

    struct MouseCameraInput
    {
        float x = 0.0f;
        float y = 0.0f;
        bool active = false;
    };

    void setRightStickInvertDefault(uint32_t axes);
    // The sticks' deadzone for every controller, in percent of the stick's reach (0 = none; stick drift): stick 0 the
    // left, 1 the right. Until set, PS2X_LEFT_STICK_DEADZONE / PS2X_RIGHT_STICK_DEADZONE (GHR.ini [Game]
    // LeftStickDeadzone / RightStickDeadzone). A controls.ini [PadN] Deadzone above it wins. Any thread.
    void setStickDeadzone(int stick, int percent);
    // The controller's sticks swapped (left-handed play; K, Nov 21): the right stick does the left's job and the left
    // the right's, in play and menus. Keys and the mouse keep theirs; the deadzones stay with the stick they name; the
    // stick presses are the Button Mapping page's. Until set, PS2X_SWAP_STICKS (GHR.ini [Game] SwapSticks). Any thread.
    void setSwapSticks(bool on);
    void setMouseCameraEnabled(bool enabled);
    void setMouseStickEnabled(bool enabled);
    [[nodiscard]] MouseCameraInput consumeMouseCameraInput();

    // STAR 2026-11-24: mouse and keyboard play for a game layer that reads the mouse itself (with
    // setMouseCameraEnabled(true) and setMouseStickEnabled(false)). While the mouse is captured:
    //   x, y      the movement since the last call (counts; right and down are positive)
    //   wheel     the notches since the last call (up is positive)
    //   held      the buttons down now: bit 0 left, 1 right, 2 middle, 3 side (Mouse 4), 4 extra (Mouse 5) (the click
    //             that captured the mouse not counted)
    //   pressed   the buttons pressed since the last call, the same bits
    //   keysHeld, keysPressed  the keys of setGameKeys (bit i = the i-th), down now / pressed since the last call;
    //             these whether or not the mouse is captured, while the window has the focus and nothing holds the
    //             input (the F2 menu, a capture); Tab not while Alt is down
    //   resumed   this call is the first after a break of 150 ms or more (a menu, a pause): the movement, the wheel
    //             and the presses of the break are dropped
    // Every call also tells this layer that the game is in play (see setMouseMenuButtons). The game thread.
    struct MouseGameInput
    {
        float x = 0.0f;
        float y = 0.0f;
        int wheel = 0;
        uint32_t held = 0u;
        uint32_t pressed = 0u;
        uint32_t keysHeld = 0u;
        uint32_t keysPressed = 0u;
        bool captured = false;
        bool resumed = false;
    };
    [[nodiscard]] MouseGameInput consumeMouseGameInput();
    // STAR 2026-11-26 (rebindable mouse and keyboard play): the keys (raylib codes, up to 32) the game layer reads
    // itself, through MouseGameInput's keysHeld / keysPressed: keys that are not in player 1's bindings. Any thread.
    void setGameKeys(const std::vector<int> &raylibKeys);
    // A binding of player n (0-based) from the environment, changed while the game runs: PS2X_PAD<n+1>_<KEY> becomes
    // `value` and the bindings are rebuilt, at the window thread's next sample. Any thread.
    void requestEnvBinding(uint32_t pad, const std::string &key, const std::string &value);
    // STAR 2026-11-27 (the controller set-up screen, star/pad_map.h): the name of the unmapped joystick (a controller
    // SDL does not know as a gamepad) the player set up. Its numbered elements ("SDL-0/Button 3", "+Axis 2",
    // "Hat 1 North") are in player 1's bindings beside the gamepad's, under the same device number, and:
    //   - they are read from joysticks of this name only (a wheel or a flight stick beside it presses nothing);
    //   - a button, hat or axis newly pressed on one makes it player 1's controller, as a gamepad's button does,
    //     and it is player 1's from the start when no gamepad is there.
    // "" (the default): numbered elements are read from whatever joystick has the number. Any thread.
    void setRawJoystickName(const std::string &name);
    // A capture for a controls screen: from begin, player 1's input is held from the game; the keys and buttons down
    // then must go up first, and the next key, mouse button or wheel turn is the result. Esc, an F-key or the window
    // losing the focus cancels. bindCaptureResult: 0 waiting (or none begun), 1 captured ("Keyboard/<name>",
    // "Pointer-0/LeftButton", RightButton, MiddleButton, Button4, Button5, "Pointer-0/WheelUp", WheelDown), 2 cancelled.
    // Taken once. Any thread.
    void beginBindCapture();
    void cancelBindCapture();
    [[nodiscard]] int bindCaptureResult(std::string &binding);
    // The pad buttons (PS2 button bits) the captured mouse presses for player 1 while the game layer is NOT consuming
    // the mouse (the game's own menus): the left click, the right click, and each wheel notch as a short tap of
    // wheelUp / wheelDown. 0 = nothing. Any thread.
    void setMouseMenuButtons(uint16_t left, uint16_t right, uint16_t wheelUp, uint16_t wheelDown);
    // STAR: the pad button (PS2 button bits) the Tab key presses for player 1; 0 = none. For a key whose job depends
    // on a setting of the game layer, so it is not in the bindings. Any thread.
    void setTabButton(uint16_t button);
    // Whether Esc frees a captured mouse (it does until this is set). Off: losing the focus (Alt+Tab) and the F2
    // menu free it, so Esc can be a game button.
    void setMouseEscapeReleases(bool on);
    // True while the keyboard or the mouse was the device used last, false for a controller. Any thread.
    [[nodiscard]] bool keysLastUsed();
    // STAR 2026-11-25: the captured mouse's whole movement since the start (counts; right and down positive), read
    // about every millisecond (ps2_host_rawmouse.cpp) and kept with the time of each reading for the last half
    // second. mouseTotalAt gives it at a time of the steady clock (nanoseconds since its epoch), between two readings
    // in proportion; a time after the newest reading gives the newest, one before the oldest the oldest. False while
    // nothing has been read yet. mouseLastMovedNs: when it last changed (0: never). Any thread.
    [[nodiscard]] bool mouseTotalAt(long long steadyNs, double &x, double &y);
    [[nodiscard]] long long mouseLastMovedNs();
    // STAR 2026-11-25: when the window's loop last took in the keys, the mouse buttons and the controllers for the
    // pads (sample(): just after each of the EE's vblanks), the steady clock in nanoseconds since its epoch; 0 before
    // the first time. Any thread.
    [[nodiscard]] long long lastSampleNs();
    // ps2_host_input.h's (declared here for a game layer outside the runtime): off = player 1's own bindings
    // everywhere, no menu layer.
    void setMenuLayer(bool on);
    // ps2_host_input.h's too: a filter on each player's state as the window's loop takes it in (sample()), before
    // anything reads it. nullptr removes it.
    using PadFilter = void (*)(uint32_t player, ps2x::iop::PadInputState &state);
    void setPadFilter(PadFilter filter);

    // Controller pad (0-based) settings when controls.ini has no [Pad<pad+1>] section: PCSX2's [PadN] keys in file
    // order, a key repeated once per binding. urban.exe writes controls.ini with them.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> defaultPadSettings(uint32_t pad);

    // The controls file ([Pad1]-[Pad8] in PCSX2's syntax) the Controls screen (F1) edits; call before the run loop.
    // Missing: written from `legacy` (the PS2X_PAD<n>=1 / PS2X_PAD<n>_<KEY> settings an older GHR.ini carried),
    // else from the defaults. Reloaded while the game runs when it changes. A pad with PS2X_PAD<n> in the environment
    // keeps using those variables. An empty path: no file (the old behaviour).
    void setControlsFile(const std::string &path, const std::vector<std::pair<std::string, std::string>> &legacy);
}
