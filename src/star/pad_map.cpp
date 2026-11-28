#include "star/pad_map.h"

#include "runtime/ps2_input_options.h"

#include <cctype>
#include <cstdlib>
#include <mutex>

// STAR 2026-11-27 (pad_map.h)
namespace star::padmap
{
    namespace
    {
        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }
        std::string trim(const std::string &text)
        {
            const size_t a = text.find_first_not_of(" \t"), b = text.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
        }

        // An input taken apart: kind 'b' a button, 'a' an axis (sign -1 / +1), 'h' a hat (side 0 up, 1 right, 2 down,
        // 3 left), 0: none. The numbers start at 1, as the runtime's.
        struct Parsed
        {
            char kind = 0;
            int number = 0;
            int sign = 0;
            int side = 0;
        };
        const char *const kSides[] = {"North", "East", "South", "West"};
        const char *const kSideWords[] = {"up", "right", "down", "left"};

        bool number(const std::string &text, size_t at, int &value, size_t &end)
        {
            while (at < text.size() && text[at] == ' ')
                ++at;
            const size_t start = at;
            value = 0;
            while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) && value < 100000)
                value = value * 10 + (text[at++] - '0');
            end = at;
            return at > start && value >= 1 && value <= 256;
        }

        Parsed parse(const std::string &input)
        {
            const std::string text = lower(trim(input));
            Parsed p;
            size_t end = 0;
            if (text.rfind("button", 0) == 0)
            {
                if (number(text, 6, p.number, end) && end == text.size())
                    p.kind = 'b';
            }
            else if (text.size() > 5 && (text[0] == '+' || text[0] == '-') && text.compare(1, 4, "axis") == 0)
            {
                if (number(text, 5, p.number, end) && end == text.size())
                {
                    p.kind = 'a';
                    p.sign = text[0] == '-' ? -1 : 1;
                }
            }
            else if (text.rfind("hat", 0) == 0)
            {
                if (number(text, 3, p.number, end))
                {
                    const std::string side = trim(text.substr(end));
                    for (int i = 0; i < 4; ++i)
                        if (side == lower(kSides[i]) || side == kSideWords[i])
                        {
                            p.kind = 'h';
                            p.side = i;
                        }
                }
            }
            return p;
        }

        std::string spell(const Parsed &p)
        {
            switch (p.kind)
            {
            case 'b': return "Button " + std::to_string(p.number);
            case 'a': return std::string(p.sign < 0 ? "-" : "+") + "Axis " + std::to_string(p.number);
            case 'h': return "Hat " + std::to_string(p.number) + " " + kSides[p.side];
            default: return {};
            }
        }

        bool isStick(const Entry &one) { return one.opposite != nullptr; }

        // Two entries cannot both have these: the same input, or the same axis when one of them is a stick's row
        // (a stick takes both ways of its axis).
        bool clash(const Entry &a, const Parsed &pa, const Entry &b, const Parsed &pb)
        {
            if (pa.kind == 0 || pa.kind != pb.kind || pa.number != pb.number)
                return false;
            if (pa.kind == 'a')
                return pa.sign == pb.sign || isStick(a) || isStick(b);
            return pa.kind == 'b' || pa.side == pb.side;
        }

        std::mutex g_liveMutex;
        Map g_live;
    }

    const std::vector<Entry> &entries()
    {
        static const std::vector<Entry> list = {
            {"up", "Directional buttons", "Up", "", "UP", nullptr},
            {"down", "Directional buttons", "Down", "", "DOWN", nullptr},
            {"left", "Directional buttons", "Left", "", "LEFT", nullptr},
            {"right", "Directional buttons", "Right", "", "RIGHT", nullptr},
            {"triangle", "Face buttons", "Triangle", "the top one", "TRIANGLE", nullptr},
            {"circle", "Face buttons", "Circle", "the right one", "CIRCLE", nullptr},
            {"cross", "Face buttons", "Cross", "the bottom one", "CROSS", nullptr},
            {"square", "Face buttons", "Square", "the left one", "SQUARE", nullptr},
            {"l1", "Shoulder buttons", "L1", "left, the upper one", "L1", nullptr},
            {"l2", "Shoulder buttons", "L2", "left, the lower one", "L2", nullptr},
            {"r1", "Shoulder buttons", "R1", "right, the upper one", "R1", nullptr},
            {"r2", "Shoulder buttons", "R2", "right, the lower one", "R2", nullptr},
            {"select", "Middle buttons", "Select", "", "SELECT", nullptr},
            {"start", "Middle buttons", "Start", "", "START", nullptr},
            {"lstick_up", "Sticks", "Left stick: up", "push it up", "LUP", "LDOWN"},
            {"lstick_right", "Sticks", "Left stick: right", "push it right", "LRIGHT", "LLEFT"},
            {"l3", "Sticks", "L3", "press the left stick in", "L3", nullptr},
            {"rstick_up", "Sticks", "Right stick: up", "push it up", "RUP", "RDOWN"},
            {"rstick_right", "Sticks", "Right stick: right", "push it right", "RRIGHT", "RLEFT"},
            {"r3", "Sticks", "R3", "press the right stick in", "R3", nullptr},
        };
        return list;
    }

    const Entry *entry(const std::string &id)
    {
        for (const Entry &one : entries())
            if (id == one.id)
                return &one;
        return nullptr;
    }

    int count(const Map &map)
    {
        int set = 0;
        for (const Entry &one : entries())
            if (const auto found = map.inputs.find(one.id); found != map.inputs.end() && !found->second.empty())
                ++set;
        return set;
    }

    Map fromValues(const std::map<std::string, std::string> &values)
    {
        Map map;
        if (const auto found = values.find("joy_device"); found != values.end())
            map.device = trim(found->second);
        for (const Entry &one : entries())
            if (const auto found = values.find(std::string("joy_") + one.id); found != values.end())
            {
                const Parsed p = parse(found->second);
                if (p.kind != 0 && (!isStick(one) || p.kind == 'a'))
                    map.inputs[one.id] = spell(p);
            }
        if (map.device.empty())
            map.inputs.clear();
        return map;
    }

    bool inValues(const std::map<std::string, std::string> &values)
    {
        return values.find("joy_device") != values.end();
    }

    void addLines(const Map &map, std::vector<std::pair<std::string, std::string>> &lines)
    {
        lines.push_back({"joy_device", map.device});
        for (const Entry &one : entries())
        {
            const auto found = map.inputs.find(one.id);
            lines.push_back({std::string("joy_") + one.id, found != map.inputs.end() ? found->second : std::string()});
        }
    }

    std::string canonical(const std::string &input)
    {
        return spell(parse(input));
    }

    std::string displayName(const Entry &one, const std::string &input)
    {
        (void)one;
        const Parsed p = parse(input);
        switch (p.kind)
        {
        case 'b': return "Button " + std::to_string(p.number);
        case 'h': return "Hat " + std::to_string(p.number) + " " + kSideWords[p.side];
        case 'a': return "Axis " + std::to_string(p.number) + (p.sign < 0 ? " (-)" : " (+)"); // (the way it reads when pushed)
        default: return {};
        }
    }

    bool assign(Map &map, const std::string &id, const std::string &input, std::string &taken)
    {
        taken.clear();
        const Entry *target = entry(id);
        const Parsed p = parse(input);
        if (!target || p.kind == 0 || (isStick(*target) && p.kind != 'a'))
            return false;
        for (const Entry &other : entries())
        {
            if (&other == target)
                continue;
            const auto found = map.inputs.find(other.id);
            if (found == map.inputs.end() || found->second.empty() || !clash(*target, p, other, parse(found->second)))
                continue;
            taken += (taken.empty() ? "" : " ") + displayName(other, found->second) + " was " + other.label + "'s.";
            map.inputs.erase(found);
        }
        map.inputs[id] = spell(p);
        return true;
    }

    std::string envPart(const Map &map, const std::string &padKey)
    {
        if (map.device.empty())
            return {};
        for (const Entry &one : entries())
        {
            const bool own = padKey == one.padKey, other = one.opposite && padKey == one.opposite;
            if (!own && !other)
                continue;
            const auto found = map.inputs.find(one.id);
            if (found == map.inputs.end())
                continue;
            Parsed p = parse(found->second);
            if (p.kind == 0)
                continue;
            if (other)
                p.sign = -p.sign;
            // (the gamepad's device number: the runtime reads both from whichever controller is player 1's)
            return "SDL-0/" + spell(p);
        }
        return {};
    }

    void setLive(const Map &map)
    {
        {
            std::lock_guard<std::mutex> lock(g_liveMutex);
            g_live = map;
        }
        ps2_host_input::setRawJoystickName(count(map) > 0 ? map.device : std::string());
    }

    std::string liveEnvPart(const std::string &padKey)
    {
        std::lock_guard<std::mutex> lock(g_liveMutex);
        return envPart(g_live, padKey);
    }
}
