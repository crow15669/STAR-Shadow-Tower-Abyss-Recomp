#include "star/bindings.h"

#include "star/pad_map.h"
#include "runtime/ps2_input_options.h"

#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sstream>

namespace star::bindings
{
    namespace
    {
        // The PS2 buttons (the pad words' bits).
        constexpr uint32_t kSelect = 0x0001u, kL3 = 0x0002u, kStart = 0x0008u, kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u,
                           kLeft = 0x0080u, kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;

        // The defaults keep what mouse and keyboard play had before it could be changed, with one change: Tab switches
        // weapons in every swing style (it was the menu key unless "Binds for each swing" took the wheel).
        std::vector<Action> makeActions()
        {
            using T = Tab;
            using K = Kind;
            return {
                {"move_forward", T::Movement, K::General, "Move forward", "", nullptr, "UP", kUp, "SDL-0/DPadUp", {"W", "Up"}},
                {"move_back", T::Movement, K::General, "Move back", "", nullptr, "DOWN", kDown, "SDL-0/DPadDown", {"S", "Down"}},
                {"strafe_left", T::Movement, K::General, "Strafe left", "", nullptr, "LEFT", kLeft, "SDL-0/DPadLeft", {"A", "Left"}},
                {"strafe_right", T::Movement, K::General, "Strafe right", "", nullptr, "RIGHT", kRight, "SDL-0/DPadRight", {"D", "Right"}},
                {"walk", T::Movement, K::General, "Walk", "Hold while moving.", nullptr, "L3", kL3, "SDL-0/LeftStick", {"LeftShift"}},
                {"fire", T::Combat, K::Ranged, "Fire a gun, draw a bow", "Hold to keep firing. A drawn bow shoots when you let go.",
                 nullptr, nullptr, 0u, nullptr, {"MouseLeft"}},
                {"gun", T::Combat, K::Ranged, "Draw or put away a gun", "Also lowers a drawn bow.", nullptr, nullptr, 0u, nullptr,
                 {"MouseRight"}},
                {"swing_view", T::Combat, K::Melee, "Swing the way the view last moved", "", "direction|flick", nullptr, 0u, nullptr,
                 {"MouseLeft"}},
                {"swing_flick", T::Combat, K::Melee, "Hold, then flick the mouse to swing", "", "flick", nullptr, 0u, nullptr,
                 {"MouseRight"}},
                {"swing_right_left", T::Combat, K::Melee, "Swing right to left", "", "buttons", nullptr, 0u, nullptr, {"MouseLeft"}},
                {"swing_left_right", T::Combat, K::Melee, "Swing left to right", "", "buttons", nullptr, 0u, nullptr, {"MouseRight"}},
                {"stab", T::Combat, K::Melee, "Stab", "", "buttons", nullptr, 0u, nullptr, {"WheelUp"}},
                {"overhead", T::Combat, K::Melee, "Overhead swing", "", "buttons", nullptr, 0u, nullptr, {"WheelDown"}},
                {"switch_weapons", T::Combat, K::General, "Switch weapons", "", nullptr, "TRIANGLE", kTriangle, "SDL-0/FaceNorth",
                 {"R", "Tab", "Wheel"}},
                {"magic", T::Combat, K::General, "Magic", "", nullptr, "SQUARE", kSquare, "SDL-0/FaceWest", {"Q", "MouseMiddle"}},
                {"use_item", T::Combat, K::General, "Use shortcut item", "", nullptr, "START", kStart, "SDL-0/Start", {"F"}},
                {"act", T::Menus, K::General, "Act, OK", "", nullptr, "CIRCLE", kCircle, "SDL-0/FaceEast",
                 {"E", "Space", "Return", "Keypad+Enter"}},
                {"menu", T::Menus, K::General, "Menu, back", "", nullptr, "CROSS", kCross, "SDL-0/FaceSouth", {"Escape", "Backspace"}},
                {"pause", T::Menus, K::General, "Pause", "", nullptr, "SELECT", kSelect, "SDL-0/Back", {"P"}},
            };
        }

        // The keys: the runtime's names (ps2_host_input.cpp keyName / keyCode) with a name for the screen.
        struct KeyName
        {
            int code;
            const char *name;
            const char *display;
        };
        const std::vector<KeyName> &keyNames()
        {
            static const std::vector<KeyName> names = []
            {
                static std::deque<std::string> owned; // the letters', digits' and keypad's names (a deque: they stay put)
                std::vector<KeyName> list;
                const auto add = [&](int code, std::string name, std::string display)
                {
                    owned.push_back(std::move(name));
                    const char *n = owned.back().c_str();
                    owned.push_back(std::move(display));
                    list.push_back({code, n, owned.back().c_str()});
                };
                for (int i = 0; i < 26; ++i)
                    add(KEY_A + i, std::string(1, static_cast<char>('A' + i)), std::string(1, static_cast<char>('A' + i)));
                for (int i = 0; i < 10; ++i)
                    add(KEY_ZERO + i, std::string(1, static_cast<char>('0' + i)), std::string(1, static_cast<char>('0' + i)));
                for (int i = 0; i < 10; ++i)
                    add(KEY_KP_0 + i, "Keypad+" + std::string(1, static_cast<char>('0' + i)), "Keypad " + std::string(1, static_cast<char>('0' + i)));
                const KeyName rest[] = {
                    {KEY_ESCAPE, "Escape", "Esc"}, {KEY_TAB, "Tab", "Tab"}, {KEY_BACKSPACE, "Backspace", "Backspace"},
                    {KEY_ENTER, "Return", "Enter"}, {KEY_KP_ENTER, "Keypad+Enter", "Keypad Enter"}, {KEY_INSERT, "Insert", "Insert"},
                    {KEY_DELETE, "Delete", "Delete"}, {KEY_PAUSE, "Pause", "Pause key"}, {KEY_HOME, "Home", "Home"},
                    {KEY_END, "End", "End"}, {KEY_LEFT, "Left", "Left arrow"}, {KEY_UP, "Up", "Up arrow"},
                    {KEY_RIGHT, "Right", "Right arrow"}, {KEY_DOWN, "Down", "Down arrow"}, {KEY_PAGE_UP, "PageUp", "Page Up"},
                    {KEY_PAGE_DOWN, "PageDown", "Page Down"}, {KEY_CAPS_LOCK, "CapsLock", "Caps Lock"}, {KEY_SPACE, "Space", "Space"},
                    {KEY_COMMA, "Comma", ","}, {KEY_MINUS, "Minus", "-"}, {KEY_PERIOD, "Period", "."}, {KEY_SLASH, "Slash", "/"},
                    {KEY_SEMICOLON, "Semicolon", ";"}, {KEY_EQUAL, "Equal", "="}, {KEY_LEFT_BRACKET, "BracketLeft", "["},
                    {KEY_BACKSLASH, "Backslash", "\\"}, {KEY_RIGHT_BRACKET, "BracketRight", "]"}, {KEY_APOSTROPHE, "Apostrophe", "'"},
                    {KEY_GRAVE, "QuoteLeft", "`"}, {KEY_LEFT_SHIFT, "LeftShift", "Left Shift"}, {KEY_RIGHT_SHIFT, "RightShift", "Right Shift"},
                    {KEY_LEFT_CONTROL, "LeftControl", "Left Ctrl"}, {KEY_RIGHT_CONTROL, "RightControl", "Right Ctrl"},
                    {KEY_LEFT_ALT, "LeftAlt", "Left Alt"}, {KEY_RIGHT_ALT, "RightAlt", "Right Alt"}, {KEY_NUM_LOCK, "NumLock", "Num Lock"},
                    {KEY_SCROLL_LOCK, "ScrollLock", "Scroll Lock"}, {KEY_PRINT_SCREEN, "Print", "Print Screen"}, {KEY_KB_MENU, "Menu", "Menu key"},
                };
                for (const KeyName &one : rest)
                    list.push_back(one);
                return list;
            }();
            return names;
        }

        struct MouseName
        {
            const char *name;
            const char *display;
            int raylib;    // -1: the wheel
            uint8_t bit;   // MouseGameInput's bit; the wheel: 1 up, 2 down, 3 either
            const char *runtime;
        };
        constexpr MouseName kMouse[] = {
            {"MouseLeft", "Left button", MOUSE_BUTTON_LEFT, 0, "Pointer-0/LeftButton"},
            {"MouseRight", "Right button", MOUSE_BUTTON_RIGHT, 1, "Pointer-0/RightButton"},
            {"MouseMiddle", "Middle button", MOUSE_BUTTON_MIDDLE, 2, "Pointer-0/MiddleButton"},
            {"Mouse4", "Mouse 4", MOUSE_BUTTON_SIDE, 3, "Pointer-0/Button4"},
            {"Mouse5", "Mouse 5", MOUSE_BUTTON_EXTRA, 4, "Pointer-0/Button5"},
            {"WheelUp", "Wheel up", -1, 1, "Pointer-0/WheelUp"},
            {"WheelDown", "Wheel down", -1, 2, "Pointer-0/WheelDown"},
            {"Wheel", "Wheel", -1, 3, nullptr},
        };

        bool same(const std::string &a, const char *b)
        {
            if (a.size() != std::char_traits<char>::length(b))
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        const MouseName *mouseName(const std::string &input)
        {
            for (const MouseName &one : kMouse)
                if (input == one.name)
                    return &one;
            return nullptr;
        }

        std::string trim(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n");
            const size_t last = text.find_last_not_of(" \t\r\n");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        }

        // The wheel's ways an input covers (0 for none).
        uint8_t wheelBits(const std::string &input)
        {
            const MouseName *m = mouseName(input);
            return m && m->raylib < 0 ? m->bit : 0u;
        }

        std::mutex g_liveMutex;
        Binds g_live;
        std::string g_liveSwings = "buttons";
        std::shared_ptr<const Resolved> g_liveResolved;
    }

    const std::vector<Action> &actions()
    {
        static const std::vector<Action> list = makeActions();
        return list;
    }

    const Action *action(const std::string &id)
    {
        for (const Action &one : actions())
            if (id == one.id)
                return &one;
        return nullptr;
    }

    Binds defaults()
    {
        Binds binds;
        for (const Action &one : actions())
            binds[one.id] = std::vector<std::string>(one.defaults.begin(), one.defaults.end());
        return binds;
    }

    std::string canonical(const std::string &input)
    {
        const std::string text = trim(input);
        for (const KeyName &one : keyNames())
            if (same(text, one.name))
                return one.name;
        for (const MouseName &one : kMouse)
            if (same(text, one.name))
                return one.name;
        return {};
    }

    std::vector<std::string> decode(const std::string &line)
    {
        std::vector<std::string> inputs;
        std::stringstream in(line);
        std::string part;
        while (std::getline(in, part, ','))
            if (const std::string input = canonical(part); !input.empty() && inputs.size() < kMaxInputs &&
                                                           std::find(inputs.begin(), inputs.end(), input) == inputs.end())
                inputs.push_back(input);
        return inputs;
    }

    std::string encode(const std::vector<std::string> &inputs)
    {
        std::string line;
        for (const std::string &input : inputs)
            line += (line.empty() ? "" : ", ") + input;
        return line;
    }

    Binds fromValues(const std::map<std::string, std::string> &values)
    {
        Binds binds = defaults();
        for (const Action &one : actions())
            if (const auto found = values.find(std::string("bind_") + one.id); found != values.end())
            {
                const std::string text = trim(found->second);
                // "none": deliberately nothing; an empty line or one with nothing readable: the defaults
                if (same(text, "none"))
                    binds[one.id].clear();
                else if (std::vector<std::string> inputs = decode(text); !inputs.empty())
                    binds[one.id] = std::move(inputs);
            }
        return binds;
    }

    std::string displayName(const std::string &input)
    {
        for (const KeyName &one : keyNames())
            if (input == one.name)
                return one.display;
        if (const MouseName *m = mouseName(input))
            return m->display;
        return input;
    }

    bool isKey(const std::string &input) { return keyCode(input) != 0; }

    int keyCode(const std::string &input)
    {
        for (const KeyName &one : keyNames())
            if (input == one.name)
                return one.code;
        return 0;
    }

    std::string fromKeyCode(int raylibKey)
    {
        if (raylibKey >= KEY_F1 && raylibKey <= KEY_F12)
            return {}; // the port's shortcuts (F1 recorder, F2 menu ...)
        for (const KeyName &one : keyNames())
            if (one.code == raylibKey)
                return one.name;
        return {};
    }

    const std::vector<int> &keyCodes()
    {
        static const std::vector<int> codes = []
        {
            std::vector<int> list;
            for (const KeyName &one : keyNames())
                list.push_back(one.code);
            return list;
        }();
        return codes;
    }

    std::string fromMouseButton(int raylibButton)
    {
        for (const MouseName &one : kMouse)
            if (one.raylib >= 0 && one.raylib == raylibButton)
                return one.name;
        return {};
    }

    std::string fromRuntimeBinding(const std::string &binding)
    {
        if (binding.rfind("Keyboard/", 0) == 0)
            return canonical(binding.substr(9));
        for (const MouseName &one : kMouse)
            if (one.runtime && binding == one.runtime)
                return one.name;
        return {};
    }

    namespace
    {
        // A swing's styles: "direction|flick" (the left button swings the way the view last moved in the flick style
        // too: the right button's flick is added to it).
        bool hasStyle(const char *styles, const std::string &swings)
        {
            std::stringstream in(styles ? styles : "");
            std::string one;
            while (std::getline(in, one, '|'))
                if (one == swings)
                    return true;
            return false;
        }
    }

    bool inUse(const Action &action, const std::string &swings) { return action.kind != Kind::Melee || hasStyle(action.style, swings); }

    bool clash(const Action &a, const Action &b)
    {
        if (std::string(a.id) == b.id)
            return false;
        if (a.kind == Kind::Melee || b.kind == Kind::Melee)
        {
            if (a.kind != Kind::Melee || b.kind != Kind::Melee)
                return false;
            for (const char *style : {"direction", "flick", "buttons"})
                if (hasStyle(a.style, style) && hasStyle(b.style, style))
                    return true; // (styles in common)
            return false;
        }
        return true; // general and ranged, among themselves and with each other
    }

    bool sameInput(const std::string &a, const std::string &b)
    {
        if (a == b)
            return true;
        const uint8_t wa = wheelBits(a), wb = wheelBits(b);
        return (wa & wb) != 0u;
    }

    std::string assign(Binds &binds, const std::string &id, size_t slot, const std::string &input)
    {
        const Action *target = action(id);
        if (!target || input.empty())
            return {};
        std::vector<std::string> &list = binds[id];
        // no second copy within the action
        for (size_t i = 0; i < list.size(); ++i)
            if (i != slot && list[i] == input)
            {
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                if (slot > i)
                    --slot;
                break;
            }
        if (slot < list.size())
            list[slot] = input;
        else if (list.size() < kMaxInputs)
            list.push_back(input);
        else
            list.back() = input;
        std::string taken;
        for (const Action &other : actions())
        {
            if (!clash(*target, other))
                continue;
            std::vector<std::string> &theirs = binds[other.id];
            const size_t before = theirs.size();
            theirs.erase(std::remove_if(theirs.begin(), theirs.end(), [&](const std::string &held) { return sameInput(held, input); }),
                         theirs.end());
            if (theirs.size() != before)
                taken += (taken.empty() ? "" : ", ") + std::string(other.label);
        }
        return taken.empty() ? std::string() : displayName(input) + " was taken from " + taken + ".";
    }

    void remove(Binds &binds, const std::string &id, size_t slot)
    {
        std::vector<std::string> &list = binds[id];
        if (slot < list.size())
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(slot));
    }

    namespace
    {
        // The inputs the swings in use took (they win over a general action's).
        std::vector<std::string> swingInputs(const Binds &binds, const std::string &swings)
        {
            std::vector<std::string> taken;
            for (const Action &one : actions())
                if (one.kind == Kind::Melee && inUse(one, swings))
                    if (const auto found = binds.find(one.id); found != binds.end())
                        taken.insert(taken.end(), found->second.begin(), found->second.end());
            return taken;
        }

        bool takenBySwing(const std::vector<std::string> &taken, const std::string &input)
        {
            return std::any_of(taken.begin(), taken.end(), [&](const std::string &t) { return t == input; });
        }

        uint8_t wheelTaken(const std::vector<std::string> &taken)
        {
            uint8_t bits = 0u;
            for (const std::string &t : taken)
                bits |= wheelBits(t);
            return bits;
        }
    }

    std::shared_ptr<const Resolved> resolve(const Binds &binds, const std::string &swings)
    {
        auto resolved = std::make_shared<Resolved>();
        resolved->swings = swings;
        const std::vector<std::string> taken = swingInputs(binds, swings);
        const uint8_t wheelSwings = wheelTaken(taken);
        const auto keyIndex = [&](int code) -> int
        {
            auto &keys = resolved->gameKeys;
            const auto at = std::find(keys.begin(), keys.end(), code);
            if (at != keys.end())
                return static_cast<int>(at - keys.begin());
            if (keys.size() >= 32u)
                return -1;
            keys.push_back(code);
            return static_cast<int>(keys.size() - 1u);
        };
        for (const Action &one : actions())
        {
            std::vector<Input> inputs;
            const auto found = binds.find(one.id);
            const bool used = inUse(one, swings);
            if (found != binds.end() && used)
                for (const std::string &input : found->second)
                {
                    if (const MouseName *m = mouseName(input))
                    {
                        if (m->raylib >= 0)
                        {
                            if (one.kind != Kind::General || !takenBySwing(taken, input))
                                inputs.push_back({Input::MouseButton, m->bit});
                        }
                        else
                        {
                            const uint8_t bits = one.kind == Kind::General ? static_cast<uint8_t>(m->bit & ~wheelSwings) : m->bit;
                            if (bits != 0u)
                                inputs.push_back({Input::Wheel, bits});
                        }
                    }
                    else if (one.kind != Kind::General)
                    {
                        if (const int index = keyIndex(keyCode(input)); index >= 0)
                            inputs.push_back({Input::Key, static_cast<uint8_t>(index)});
                    }
                    else if (input == "Tab" && !takenBySwing(taken, input))
                        resolved->tabButton = static_cast<uint16_t>(one.padButton);
                }
            resolved->inputs.push_back(std::move(inputs));
        }
        return resolved;
    }

    std::string envValue(const Binds &binds, const std::string &swings, const Action &action)
    {
        std::string value;
        if (const auto found = binds.find(action.id); found != binds.end())
        {
            const std::vector<std::string> taken = swingInputs(binds, swings);
            for (const std::string &input : found->second)
                if (isKey(input) && input != "Tab" && !takenBySwing(taken, input))
                    value += (value.empty() ? "" : ";") + std::string("Keyboard/") + input;
        }
        if (action.controller)
            value += (value.empty() ? "" : ";") + std::string(action.controller);
        // STAR 2026-11-27: and the button of a controller the player set up (star/pad_map.h)
        if (action.padKey)
            if (const std::string joystick = padmap::liveEnvPart(action.padKey); !joystick.empty())
                value += (value.empty() ? "" : ";") + joystick;
        return value;
    }

    void setLive(const Binds &binds, const std::string &swings, bool runtimeStarted)
    {
        std::shared_ptr<const Resolved> resolved = resolve(binds, swings);
        {
            std::lock_guard<std::mutex> lock(g_liveMutex);
            g_live = binds;
            g_liveSwings = swings;
            g_liveResolved = resolved;
        }
        ps2_host_input::setGameKeys(resolved->gameKeys);
        ps2_host_input::setTabButton(resolved->tabButton);
        if (runtimeStarted && !std::getenv("STAR_TEST_PAD_ENV")) // (STAR_TEST_PAD_ENV: a test's own PS2X_PAD1_* stay)
            for (const Action &one : actions())
                if (one.kind == Kind::General && one.padKey)
                    ps2_host_input::requestEnvBinding(0u, one.padKey, envValue(binds, swings, one));
    }

    Binds live()
    {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        return g_live.empty() ? defaults() : g_live;
    }

    std::shared_ptr<const Resolved> liveResolved()
    {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        if (!g_liveResolved)
            g_liveResolved = resolve(g_live.empty() ? defaults() : g_live, g_liveSwings);
        return g_liveResolved;
    }
}
