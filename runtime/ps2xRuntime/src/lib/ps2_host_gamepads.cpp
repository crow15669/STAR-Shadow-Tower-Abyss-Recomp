#include "ps2_host_gamepads.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>

#if defined(PS2X_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

namespace ps2_host_gamepads
{
    namespace
    {
        extern "C"
        {
            struct GlfwGamepadState
            {
                unsigned char buttons[15];
                float axes[6];
            };
            int glfwJoystickIsGamepad(int jid);
            int glfwGetGamepadState(int jid, GlfwGamepadState *state);
            const char *glfwGetGamepadName(int jid);
        }

        bool sdlRequested()
        {
            static const bool requested = []
            {
                const char *v = std::getenv("PS2X_SDL_INPUT");
                return !(v && (std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0));
            }();
            return requested;
        }

        struct Motors
        {
            uint16_t large = 0u;
            uint16_t small = 0u;
            bool operator==(const Motors &) const = default;
        };

        struct Device
        {
#if defined(PS2X_HAS_SDL3)
            SDL_Gamepad *gamepad = nullptr;
            SDL_Joystick *joystick = nullptr;
            SDL_JoystickID id = 0;
#endif
            std::string name;
            Style style = Style::Unknown; // from the name, set with it
            Motors wanted;
            Motors sent;
            std::chrono::steady_clock::time_point sentAt;
        };

        std::array<Device, kMaxDevices> g_devices;
#if defined(PS2X_HAS_SDL3)
        std::atomic<SDL_Joystick *> g_testJoystick{nullptr};      // STAR: PS2X_TEST_JOYSTICK's last (testJoystick)
        std::atomic<SDL_Joystick *> g_testJoystickFirst{nullptr}; // and its first
#endif
        bool g_sdl = false; // SDL is open and owns the devices; false = GLFW's gamepads

        // GLFW's names come from SDL_GameControllerDB ("PS4 Controller", "PS5 Controller", "Xbox Controller",
        // "XInput Gamepad (GLFW)"); SDL3's SDL_GetGamepadName gives the same kind of names.
        Style styleFromName(const std::string &name)
        {
            std::string lower(name);
            for (char &ch : lower)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            auto has = [&](const char *part) { return lower.find(part) != std::string::npos; };
            if (has("playstation") || has("dualshock") || has("dualsense") || has("sony") || has("ps3") || has("ps4") || has("ps5"))
                return Style::PlayStation;
            if (has("nintendo") || has("switch") || has("joy-con") || has("joycon"))
                return Style::Nintendo;
            // Steam Input's virtual pad ("Steam Virtual Gamepad") stands for whatever controller Steam is reading: Xbox
            // letters unless SDL could tell the type (above). A Steam Controller or Steam Deck by name: Steam's art.
#if !defined(_WIN32)
            // On a Steam Deck (SteamDeck=1: Steam sets it, and GHR does when the board says so) Steam Input's virtual pad
            // is the Deck's own controls unless SDL named another controller: Steam's art.
            if (has("steam virtual") || has("steam deck"))
            {
                const char *deck = std::getenv("SteamDeck");
                if (has("steam deck") || (deck && deck[0] == '1' && deck[1] == '\0'))
                    return Style::Steam;
            }
#endif
            if (has("steam virtual") || has("xbox") || has("x-box") || has("xinput"))
                return Style::Xbox;
            if (has("steam"))
                return Style::Steam;
            return Style::Unknown;
        }

        // The GLFW gamepad button for each Button (-1: GLFW has no such button).
        constexpr std::array<int, ButtonCount> kGlfwButtons{0, 1, 2, 3, 6, 8, 7, 9, 10, 4, 5, 11, 13, 14, 12, -1, -1, -1, -1, -1, -1};

#if defined(PS2X_HAS_SDL3)
        static_assert(SDL_GAMEPAD_BUTTON_SOUTH == South && SDL_GAMEPAD_BUTTON_DPAD_RIGHT == DPadRight &&
                      SDL_GAMEPAD_BUTTON_TOUCHPAD == Touchpad && SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == RightTrigger);

        // Like PCSX2's SDLInputSource: the player index SDL gives the controller (an Xbox controller's XInput number)
        // when it is free, else the lowest free number.
        void addGamepad(SDL_JoystickID id)
        {
            for (const Device &device : g_devices)
                if (device.gamepad && device.id == id)
                    return;
            for (Device &device : g_devices)
                if (device.joystick && device.id == id)
                {
                    SDL_CloseJoystick(device.joystick);
                    device = Device{};
                }
            SDL_Gamepad *gamepad = SDL_OpenGamepad(id);
            if (!gamepad)
            {
                std::cerr << "[input] could not open controller " << id << ": " << SDL_GetError() << std::endl;
                return;
            }
            // Controllers number as if no unmapped joystick were there (#44's follow-up: a joystick plugged in or found
            // first doesn't renumber the controller players already bound as SDL-0): one in the way moves up.
            int slot = SDL_GetGamepadPlayerIndex(gamepad);
            if (slot < 0 || slot >= kMaxDevices || g_devices[static_cast<size_t>(slot)].gamepad)
            {
                slot = -1;
                for (int i = 0; i < kMaxDevices && slot < 0; ++i)
                    if (!g_devices[static_cast<size_t>(i)].gamepad)
                        slot = i;
            }
            if (slot < 0)
            {
                SDL_CloseGamepad(gamepad);
                return;
            }
            if (Device &raw = g_devices[static_cast<size_t>(slot)]; raw.joystick)
            {
                int free = -1;
                for (int i = 0; i < kMaxDevices && free < 0; ++i)
                    if (i != slot && !g_devices[static_cast<size_t>(i)].gamepad && !g_devices[static_cast<size_t>(i)].joystick)
                        free = i;
                if (free >= 0)
                {
                    std::cerr << "[input] SDL-" << slot << " (" << raw.name << ", raw joystick) is now SDL-" << free << std::endl;
                    g_devices[static_cast<size_t>(free)] = raw;
                }
                else
                    SDL_CloseJoystick(raw.joystick);
                raw = Device{};
            }
            Device &device = g_devices[static_cast<size_t>(slot)];
            device = Device{};
            device.gamepad = gamepad;
            device.id = id;
            const char *name = SDL_GetGamepadName(gamepad);
            device.name = name ? name : "Controller";
            // SDL knows the controller family (an XInput pad is an Xbox 360 controller whatever its name); the name
            // decides for the rest.
            // Valve's own hardware (vendor 0x28DE) is a Steam Deck or Steam Controller, except Steam Input's virtual pad
            // (product 0x11FF), which SDL reports as the controller it stands for when Steam says so.
#if defined(_WIN32)
            const bool valveHardware = SDL_GetGamepadVendor(gamepad) == 0x28DE && SDL_GetGamepadProduct(gamepad) != 0x11FF;
#else
            // On a Steam Deck the virtual pad SDL could not name after another controller (it reads as an Xbox 360
            // pad called "Steam Virtual Gamepad") is the Deck's own controls: Steam's art too.
            const char *deck = std::getenv("SteamDeck");
            const bool deckVirtual = deck && std::strcmp(deck, "1") == 0 && SDL_GetGamepadVendor(gamepad) == 0x28DE &&
                                     SDL_GetGamepadProduct(gamepad) == 0x11FF && styleFromName(device.name) == Style::Steam;
            const bool valveHardware =
                deckVirtual || (SDL_GetGamepadVendor(gamepad) == 0x28DE && SDL_GetGamepadProduct(gamepad) != 0x11FF);
#endif
            switch (valveHardware ? SDL_GAMEPAD_TYPE_UNKNOWN : SDL_GetGamepadType(gamepad))
            {
            case SDL_GAMEPAD_TYPE_XBOX360:
            case SDL_GAMEPAD_TYPE_XBOXONE:
                device.style = Style::Xbox;
                break;
            case SDL_GAMEPAD_TYPE_PS3:
            case SDL_GAMEPAD_TYPE_PS4:
            case SDL_GAMEPAD_TYPE_PS5:
                device.style = Style::PlayStation;
                break;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                device.style = Style::Nintendo;
                break;
            default:
                device.style = valveHardware ? Style::Steam : styleFromName(device.name);
                break;
            }
            static const char *const kStyleNames[] = {"other", "Xbox", "PlayStation", "Nintendo", "Steam"};
            std::cerr << "[input] SDL-" << slot << ": " << device.name << " (" << kStyleNames[static_cast<int>(device.style)]
                      << " buttons)" << std::endl;
        }

        void addJoystick(SDL_JoystickID id)
        {
            if (SDL_IsGamepad(id))
            {
                addGamepad(id);
                return;
            }
            for (const Device &device : g_devices)
                if ((device.gamepad || device.joystick) && device.id == id)
                    return;
            SDL_Joystick *joystick = SDL_OpenJoystick(id);
            if (!joystick)
            {
                std::cerr << "[input] could not open joystick " << id << ": " << SDL_GetError() << std::endl;
                return;
            }
            int slot = -1;
            for (int i = 0; i < kMaxDevices && slot < 0; ++i)
                if (!g_devices[static_cast<size_t>(i)].gamepad && !g_devices[static_cast<size_t>(i)].joystick)
                    slot = i;
            if (slot < 0)
            {
                SDL_CloseJoystick(joystick);
                return;
            }
            Device &device = g_devices[static_cast<size_t>(slot)];
            device = Device{};
            device.joystick = joystick;
            device.id = id;
            const char *name = SDL_GetJoystickName(joystick);
            device.name = name ? name : "Joystick";
            std::cerr << "[input] SDL-" << slot << ": " << device.name << " (raw joystick)" << std::endl;
        }

        void removeGamepad(SDL_JoystickID id)
        {
            for (size_t i = 0; i < g_devices.size(); ++i)
            {
                Device &device = g_devices[i];
                if ((device.gamepad || device.joystick) && device.id == id)
                {
                    std::cerr << "[input] SDL-" << i << " disconnected: " << device.name << std::endl;
                    if (device.gamepad)
                        SDL_CloseGamepad(device.gamepad);
                    if (device.joystick)
                        SDL_CloseJoystick(device.joystick);
                    device = Device{};
                }
            }
        }

        bool openSdl()
        {
            // SDL has no window of its own here (the window is raylib's), so it must read controllers in the
            // background; it must not take over Ctrl+C in the console either.
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
            if (!SDL_Init(SDL_INIT_GAMEPAD))
            {
                std::cerr << "[input] SDL could not start (" << SDL_GetError() << "); using GLFW's gamepads" << std::endl;
                return false;
            }
            std::cerr << "[input] controllers through SDL " << SDL_MAJOR_VERSION << "." << SDL_MINOR_VERSION << "."
                      << SDL_MICRO_VERSION << std::endl;
            // STAR 2026-11-27: a test's unmapped joystick (ps2_host_gamepads.h). Its type is not "gamepad", so SDL
            // gives it no mapping: it arrives as a raw joystick, as a generic USB controller does.
            if (const char *test = std::getenv("PS2X_TEST_JOYSTICK"); test && *test)
            {
                std::string names(test);
                for (size_t at = 0; at <= names.size();)
                {
                    const size_t bar = std::min(names.find('|', at), names.size());
                    const std::string name = names.substr(at, bar - at);
                    at = bar + 1;
                    if (name.empty())
                        continue;
                    SDL_VirtualJoystickDesc desc;
                    SDL_INIT_INTERFACE(&desc);
                    desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
                    desc.naxes = 6;
                    desc.nbuttons = 16;
                    desc.nhats = 1;
                    desc.name = name.c_str();
                    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
                    SDL_Joystick *joystick = id ? SDL_OpenJoystick(id) : nullptr;
                    g_testJoystick.store(joystick, std::memory_order_release);
                    if (!g_testJoystickFirst.load(std::memory_order_relaxed))
                        g_testJoystickFirst.store(joystick, std::memory_order_release);
                    std::cerr << "[input] test joystick \"" << name << "\": " << (joystick ? "attached" : SDL_GetError()) << std::endl;
                }
            }
            return true;
        }

        void updateRumble()
        {
            constexpr auto kRefresh = std::chrono::milliseconds(500);
            constexpr uint32_t kDurationMs = 1000u; // stops by itself if the game stops sending
            const auto now = std::chrono::steady_clock::now();
            for (Device &device : g_devices)
            {
                if (!device.gamepad)
                    continue;
                const bool running = device.wanted.large != 0u || device.wanted.small != 0u;
                if (device.wanted == device.sent && !(running && now - device.sentAt >= kRefresh))
                    continue;
                SDL_RumbleGamepad(device.gamepad, device.wanted.large, device.wanted.small, running ? kDurationMs : 0u);
                device.sent = device.wanted;
                device.sentAt = now;
            }
        }
#endif

        void updateGlfwNames()
        {
            for (int i = 0; i < kMaxDevices; ++i)
            {
                Device &device = g_devices[static_cast<size_t>(i)];
                const bool present = glfwJoystickIsGamepad(i) != 0;
                if (present && device.name.empty())
                {
                    const char *name = glfwGetGamepadName(i);
                    device.name = name ? name : "Controller";
                    device.style = styleFromName(device.name);
                    std::cerr << "[input] SDL-" << i << " (GLFW gamepad " << i << "): " << device.name << std::endl;
                }
                else if (!present && !device.name.empty())
                {
                    std::cerr << "[input] SDL-" << i << " disconnected: " << device.name << std::endl;
                    device.name.clear();
                    device.style = Style::Unknown;
                }
            }
        }
    }

    void update()
    {
        static const bool started = []
        {
#if defined(PS2X_HAS_SDL3)
            if (sdlRequested())
                g_sdl = openSdl();
#else
            if (sdlRequested())
                std::cerr << "[input] built without SDL3; using GLFW's gamepads" << std::endl;
#endif
            if (!g_sdl)
                std::cerr << "[input] controllers through GLFW (no vibration)" << std::endl;
            return true;
        }();
        (void)started;
#if defined(PS2X_HAS_SDL3)
        if (g_sdl)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_JOYSTICK_ADDED)
                    addJoystick(event.jdevice.which);
                else if (event.type == SDL_EVENT_GAMEPAD_ADDED)
                    addGamepad(event.gdevice.which);
                else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
                    removeGamepad(event.gdevice.which);
                else if (event.type == SDL_EVENT_JOYSTICK_REMOVED)
                    removeGamepad(event.jdevice.which);
            }
            updateRumble();
            return;
        }
#endif
        static auto lastScan = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastScan >= std::chrono::seconds(1))
        {
            lastScan = now;
            updateGlfwNames();
        }
    }

    bool connected(int device)
    {
        if (device < 0 || device >= kMaxDevices)
            return false;
#if defined(PS2X_HAS_SDL3)
        if (g_sdl)
            return g_devices[static_cast<size_t>(device)].gamepad != nullptr ||
                   g_devices[static_cast<size_t>(device)].joystick != nullptr;
#endif
        return glfwJoystickIsGamepad(device) != 0;
    }

    bool button(int device, Button button)
    {
        if (device < 0 || device >= kMaxDevices || button < 0 || button >= ButtonCount)
            return false;
#if defined(PS2X_HAS_SDL3)
        if (g_sdl)
        {
            SDL_Gamepad *gamepad = g_devices[static_cast<size_t>(device)].gamepad;
            return gamepad && SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(button));
        }
#endif
        const int glfw = kGlfwButtons[static_cast<size_t>(button)];
        GlfwGamepadState state{};
        return glfw >= 0 && glfwGetGamepadState(device, &state) && state.buttons[glfw] != 0u;
    }

    bool hasButton(int device, Button button)
    {
        if (device < 0 || device >= kMaxDevices || button < 0 || button >= ButtonCount)
            return false;
#if defined(PS2X_HAS_SDL3)
        if (g_sdl)
        {
            SDL_Gamepad *gamepad = g_devices[static_cast<size_t>(device)].gamepad;
            return gamepad && SDL_GamepadHasButton(gamepad, static_cast<SDL_GamepadButton>(button));
        }
#endif
        return kGlfwButtons[static_cast<size_t>(button)] >= 0 && glfwJoystickIsGamepad(device) != 0;
    }

    int rawButtonCount(int device)
    {
        if (device < 0 || device >= kMaxDevices)
            return 0;
#if defined(PS2X_HAS_SDL3)
        if (g_sdl)
        {
            SDL_Joystick *joystick = g_devices[static_cast<size_t>(device)].joystick;
            return joystick ? std::min(SDL_GetNumJoystickButtons(joystick), kRawHatBase - kRawButtonBase) : 0;
        }
#endif
        return 0;
    }

    bool rawButton(int device, int button)
    {
        if (device < 0 || device >= kMaxDevices || button < 0 || button >= rawButtonCount(device))
            return false;
#if defined(PS2X_HAS_SDL3)
        SDL_Joystick *joystick = g_devices[static_cast<size_t>(device)].joystick;
        return joystick && SDL_GetJoystickButton(joystick, button);
#else
        return false;
#endif
    }

    int rawAxisCount(int device)
    {
        if (device < 0 || device >= kMaxDevices)
            return 0;
#if defined(PS2X_HAS_SDL3)
        if (g_sdl)
        {
            SDL_Joystick *joystick = g_devices[static_cast<size_t>(device)].joystick;
            return joystick ? std::min(SDL_GetNumJoystickAxes(joystick), kRawHatBase - kRawAxisBase) : 0;
        }
#endif
        return 0;
    }

    float rawAxis(int device, int axis)
    {
        if (device < 0 || device >= kMaxDevices || axis < 0 || axis >= rawAxisCount(device))
            return 0.0f;
#if defined(PS2X_HAS_SDL3)
        SDL_Joystick *joystick = g_devices[static_cast<size_t>(device)].joystick;
        return joystick ? std::clamp(static_cast<float>(SDL_GetJoystickAxis(joystick, axis)) / 32767.0f, -1.0f, 1.0f) : 0.0f;
#else
        return 0.0f;
#endif
    }

    int rawHatCount(int device)
    {
        if (device < 0 || device >= kMaxDevices)
            return 0;
#if defined(PS2X_HAS_SDL3)
        if (g_sdl)
        {
            SDL_Joystick *joystick = g_devices[static_cast<size_t>(device)].joystick;
            return joystick ? std::min(SDL_GetNumJoystickHats(joystick), (32767 - kRawHatBase) / 8) : 0;
        }
#endif
        return 0;
    }

    bool rawHat(int device, int hat, unsigned char direction)
    {
        if (device < 0 || device >= kMaxDevices || hat < 0 || hat >= rawHatCount(device) || direction == 0)
            return false;
#if defined(PS2X_HAS_SDL3)
        SDL_Joystick *joystick = g_devices[static_cast<size_t>(device)].joystick;
        return joystick && (SDL_GetJoystickHat(joystick, hat) & direction) == direction;
#else
        return false;
#endif
    }

    float axis(int device, Axis axis)
    {
        if (device < 0 || device >= kMaxDevices || axis < 0 || axis >= AxisCount)
            return 0.0f;
        const bool trigger = axis == LeftTrigger || axis == RightTrigger;
#if defined(PS2X_HAS_SDL3)
        if (g_sdl)
        {
            SDL_Gamepad *gamepad = g_devices[static_cast<size_t>(device)].gamepad;
            if (!gamepad)
                return 0.0f;
            const float value = static_cast<float>(SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(axis))) / 32767.0f;
            return std::clamp(value, trigger ? 0.0f : -1.0f, 1.0f);
        }
#endif
        GlfwGamepadState state{};
        if (!glfwGetGamepadState(device, &state))
            return 0.0f;
        const float value = state.axes[axis];
        return trigger ? std::clamp((value + 1.0f) * 0.5f, 0.0f, 1.0f) : std::clamp(value, -1.0f, 1.0f);
    }

    void setRumble(int device, float large, float small)
    {
        if (device < 0 || device >= kMaxDevices)
            return;
        auto motor = [](float value)
        { return static_cast<uint16_t>(std::clamp(value, 0.0f, 1.0f) * 65535.0f + 0.5f); };
        g_devices[static_cast<size_t>(device)].wanted = {motor(large), motor(small)};
    }

    // STAR 2026-11-27 (ps2_host_gamepads.h)
    bool isRaw(int device)
    {
#if defined(PS2X_HAS_SDL3)
        return device >= 0 && device < kMaxDevices && g_sdl && g_devices[static_cast<size_t>(device)].joystick != nullptr;
#else
        (void)device;
        return false;
#endif
    }

    std::string name(int device)
    {
        return connected(device) ? g_devices[static_cast<size_t>(device)].name : std::string();
    }

    void testJoystick(int kind, int index, int value)
    {
#if defined(PS2X_HAS_SDL3)
        SDL_Joystick *joystick = (kind >= 10 ? g_testJoystickFirst : g_testJoystick).load(std::memory_order_acquire);
        if (!joystick)
            return;
        kind %= 10;
        if (kind == 0)
            SDL_SetJoystickVirtualButton(joystick, index, value != 0);
        else if (kind == 1)
            SDL_SetJoystickVirtualAxis(joystick, index, static_cast<Sint16>(std::clamp(value, -32768, 32767)));
        else if (kind == 2)
            SDL_SetJoystickVirtualHat(joystick, index, static_cast<Uint8>(value));
#else
        (void)kind;
        (void)index;
        (void)value;
#endif
    }

    std::vector<DeviceInfo> devices()
    {
        std::vector<DeviceInfo> list;
        for (int i = 0; i < kMaxDevices; ++i)
            if (connected(i))
                list.push_back({i, g_devices[static_cast<size_t>(i)].name});
        return list;
    }

    Style style(int device)
    {
        // Cleared when the controller goes (SDL at once, GLFW within the 1-s name scan): no extra poll per call.
        if (device < 0 || device >= kMaxDevices)
            return Style::Unknown;
        return g_devices[static_cast<size_t>(device)].style;
    }
}
