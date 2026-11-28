#pragma once

#include <string>
#include <vector>

// Host gamepads and unmapped SDL joysticks behind the "SDL-<n>/<element>" bindings of controls.ini (PCSX2's syntax).
// With SDL3 (the default) the numbers follow PCSX2 for gamepads; unmapped joysticks take the lowest free number.
// PS2X_SDL_INPUT=0 (controls.ini [InputSources] SDL = false) reads GLFW's gamepads instead. Everything here runs on
// the window thread.
namespace ps2_host_gamepads
{
    constexpr int kMaxDevices = 16;
    constexpr int kRawButtonBase = 64;
    constexpr int kRawAxisBase = 64;
    constexpr int kRawHatBase = 512;

    // SDL3's SDL_GamepadButton / SDL_GamepadAxis numbering.
    enum Button
    {
        South,
        East,
        West,
        North,
        Back,
        Guide,
        Start,
        LeftStick,
        RightStick,
        LeftShoulder,
        RightShoulder,
        DPadUp,
        DPadDown,
        DPadLeft,
        DPadRight,
        Misc1,
        RightPaddle1,
        LeftPaddle1,
        RightPaddle2,
        LeftPaddle2,
        Touchpad,
        ButtonCount
    };

    enum Axis
    {
        LeftX,
        LeftY,
        RightX,
        RightY,
        LeftTrigger,
        RightTrigger,
        AxisCount
    };

    // Opens SDL on the first call, then handles controllers being plugged in and out.
    void update();
    [[nodiscard]] bool connected(int device);
    [[nodiscard]] bool button(int device, Button button);
    // Whether the controller reports that button at all (SDL3: SDL_GamepadHasButton; the back paddles of an Elite, a
    // DualSense Edge, a Steam pad). GLFW: only the standard buttons.
    [[nodiscard]] bool hasButton(int device, Button button);
    // Buttons, axes and hats on SDL joysticks without a gamepad mapping.
    [[nodiscard]] int rawButtonCount(int device);
    [[nodiscard]] bool rawButton(int device, int button);
    [[nodiscard]] int rawAxisCount(int device);
    [[nodiscard]] float rawAxis(int device, int axis);
    [[nodiscard]] int rawHatCount(int device);
    [[nodiscard]] bool rawHat(int device, int hat, unsigned char direction);
    // STAR 2026-11-27 (the controller set-up screen, star/pad_map.h): whether a device is an unmapped joystick (one SDL
    // does not know as a gamepad: numbered buttons, axes and hats only), and a connected device's name ("" = none).
    // Window thread.
    [[nodiscard]] bool isRaw(int device);
    [[nodiscard]] std::string name(int device);
    // A test's own unmapped joystick: PS2X_TEST_JOYSTICK=<name> attaches a virtual one (16 buttons, 6 axes, 1 hat) when
    // SDL starts; <name>|<name> attaches several, in that order. testJoystick sets one of the last one's elements:
    // kind 0 a button (value 0 / 1), 1 an axis (-32768 .. 32767), 2 a hat (SDL's mask: 1 up, 2 right, 4 down,
    // 8 left); kind + 10: the same of the first one. Any thread.
    void testJoystick(int kind, int index, int value);
    // Sticks -1..1 (down and right positive), triggers 0..1.
    [[nodiscard]] float axis(int device, Axis axis);
    // 0..1 each (large = low frequency motor); sent when it changes and refreshed while it runs.
    void setRumble(int device, float large, float small);

    struct DeviceInfo
    {
        int device = -1;
        std::string name;
    };
    [[nodiscard]] std::vector<DeviceInfo> devices();

    // The button art a controller's name points to (button prompts; GLFW gives no controller type, only the name).
    // Unknown: not connected, or the name says neither.
    enum class Style
    {
        Unknown,
        Xbox,
        PlayStation,
        Nintendo, // Switch Pro, Joy-Cons, Switch-style pads: positional buttons, Nintendo's letters
        Steam     // Steam Deck, Steam Controller
    };
    [[nodiscard]] Style style(int device);
}
