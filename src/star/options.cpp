#include "star/options.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace star::options
{
    namespace
    {
        const std::vector<Choice> kOnOff = {{"off", "Off"}, {"on", "On"}};
        // The sliders' warning above 100% (the user's words, 2026-11-26).
        constexpr const char *kFastCamera =
            "Going above 100% may introduce unwanted or unexpected behaviors, because the game's camera was never meant to "
            "move fast. It also removes much of the challenge of the original game: going above 100% is not recommended.";

        // The wording (2026-11-26, the user's "streamlining the wording"): a short label, one or two plain sentences under
        // it. Presets: Original, Remastered, STAR. The fallbacks (an option without a line in the file) are the STAR
        // preset's, so that a fresh install starts on it (the user's choice, 2026-11-26).
        std::vector<Option> makeTable()
        {
            std::vector<Option> t;
            // ---- Graphics ----
            t.push_back({"resolution", Page::Graphics, "Picture", "Resolution", "3x suits a 1080p screen, 4x a 1440p one.",
                         {{"1", "1x  640x448 (the PS2's own)"},
                          {"2", "2x  1280x896"},
                          {"3", "3x  1920x1344"},
                          {"4", "4x  2560x1792"},
                          {"5", "5x  3200x2240"},
                          {"6", "6x  3840x2688"}},
                         false, "3", {"1", "3", "3"}});
            t.push_back({"fxaa", Page::Graphics, "Picture", "FXAA", "Smooths the jagged edges that are left. Softens the picture slightly.", kOnOff,
                         true, "on", {"off", "on", "on"}});
            // (2026-11-26, the user's: "most people will be looking for a checkbox that says 60fps": Smooth motion became
            // Framerate, and In-between pictures shows only under Unlocked. The file keeps smooth_60fps = on / off.)
            t.push_back({"smooth_60fps", Page::Graphics, "Picture", "Framerate", "",
                         {{"off", "30fps", "The game's own 30 pictures a second."},
                          {"on", "Unlocked",
                           "In-between pictures for the camera and everything that moves, up to your screen's refresh rate."}},
                         false, "on", {"off", "on", "on"}});
            t.push_back({"smooth_pictures", Page::Graphics, "Picture", "In-between pictures",
                         "Every refresh is the smoothest; in heavy scenes the PC may not keep up.",
                         {{"auto", "Auto (as many as there is time for)"},
                          {"1", "Every refresh"},
                          {"2", "Every 2nd refresh"},
                          {"3", "Every 3rd refresh"},
                          {"4", "Every 4th refresh"}},
                         false, "auto", {nullptr, nullptr, nullptr}, false, {}, "smooth_60fps"});
            t.push_back({"sharp_shadows", Page::Graphics, "Shadows", "Sharp shadows",
                         "The game draws shadows into a small buffer and stretches it, which gives them stepped edges. On: drawn at the "
                         "full resolution.",
                         kOnOff, true, "on", {"off", "on", "on"}});
            t.push_back({"near_shadows", Page::Graphics, "Shadows", "Close-up creature shadows",
                         "The game hides a creature's shadow when it is right beside you and shortens it when the light is behind you. "
                         "On: drawn in full.",
                         kOnOff, true, "on", {"off", "on", "on"}});
            t.push_back({"self_shadows", Page::Graphics, "Shadows", "Self-shadows",
                         "The game's self-shadows are very buggy: hard patches on creatures' bodies that jump around as they breathe and "
                         "flicker as you move. None gives the cleanest picture (best with Sharp shadows on); creatures still cast "
                         "shadows on the ground, walls and others.",
                         {{"on", "As the game draws them"}, {"off", "None"}}, false, "off", {"on", "on", "off"}});
            t.push_back({"shadow_walls", Page::Graphics, "Shadows", "Shadows stop at walls",
                         "The game's creature shadows pass through pillars and walls, so the same shadow shows again on the far side and "
                         "on the ground behind. On: a shadow stops at the first surface in its way (scenery on screen only).",
                         kOnOff, true, "on", {"off", "off", "on"}});
            t.push_back({"flicker_filter", Page::Graphics, "Effects", "Flicker filter",
                         "The game blends each line with the one above, made for CRT TVs. On a modern screen it only blurs.", kOnOff, true,
                         "off", {"on", "off", "off"}});
            t.push_back({"glow", Page::Graphics, "Effects", "Glow", "The game's soft glow over the picture.", kOnOff, true, "on",
                         {"on", "on", "on"}});
            // ---- Gameplay ----
            t.push_back({"text_fixes", Page::Gameplay, "Text", "Text fixes",
                         "Fixes overlapping and broken text in the English patch: Item Repair's cost line, and a broken letter in four "
                         "item texts (Yin-Yang Charm, Hide Gloves, Stun Gun, S&W No. 2). The wording stays as it is.",
                         kOnOff, true, "on", {"off", "on", "on"}});
            // (2026-11-26, the user's: "what it does isn't really clear ... can the language be simplified a bit, as well as
            // add a note that says it's recommended". Was "Late input"; the file keeps late_input.)
            t.push_back({"late_input", Page::Gameplay, "Controls", "Reduced input lag",
                         "Your controls respond sooner: about 45 ms less delay between pressing a button and seeing it happen. It "
                         "never slows the game down. (recommended)",
                         kOnOff, true, "on", {"off", "on", "on"}});
            t.push_back({"analog_aim", Page::Gameplay, "Aiming", "True analog aiming",
                         "The right stick turns and tilts the view smoothly, by how far it is pushed, instead of in steps. For the control "
                         "types that look with the right stick (Type 4; Type 3 for up and down).",
                         kOnOff, true, "on", {"off", "on", "on"}});
            t.push_back({"aim_turn_speed", Page::Gameplay, "Aiming", "Turning speed", "With the stick pushed all the way.", {}, false, "100",
                         {nullptr, nullptr, nullptr}, false, {75, 300, "%d%%", 100, kFastCamera, "%d%% (PS2 Default)"}});
            t.push_back({"aim_look_speed", Page::Gameplay, "Aiming", "Up and down speed", "", {}, false, "100", {nullptr, nullptr, nullptr},
                         false, {75, 300, "%d%%", 100, kFastCamera, "%d%% (PS2 Default)"}});
            // ---- Experimental ----
            t.push_back({"mouse_keyboard", Page::Experimental, "Mouse and keyboard", "Mouse and keyboard",
                         "Click the game to give it the mouse; Alt+Tab or the F2 menu take it back. A controller keeps working. "
                         "The controls:",
                         kOnOff, true, "on", {"off", "off", "on"}});
            t.push_back({"mouse_sensitivity", Page::Experimental, "Mouse and keyboard", "Mouse sensitivity", "", {}, false, "100",
                         {nullptr, nullptr, nullptr}, false, {25, 400, "%d%%", 100, kFastCamera}});
            t.push_back({"mouse_live_view", Page::Experimental, "Mouse and keyboard", "Low-delay mouse look",
                         "Every picture shown follows the mouse. Off: the view turns in the game's 30 steps a second, about 50 ms later. "
                         "Needs Framerate: Unlocked.",
                         kOnOff, true, "on", {nullptr, nullptr, nullptr}});
            t.push_back({"mouse_invert_y", Page::Experimental, "Mouse and keyboard", "Invert mouse up and down", "", kOnOff, true, "off",
                         {nullptr, nullptr, nullptr}});
            t.push_back({"gun_button", Page::Experimental, "Mouse and keyboard", "Gun (right button)", "",
                         {{"toggle", "Toggle: press to aim, press again to holster"}, {"hold", "Hold: out while the button is down"}}, false,
                         "toggle", {nullptr, nullptr, nullptr}});
            t.push_back({"melee_swings", Page::Experimental, "Mouse and keyboard", "Sword swings", "",
                         {{"direction", "Camera-based directional swings",
                           "The left button swings the way the view last moved; a small arrow shows which way. Think Mount & Blade, "
                           "Chivalry, etc."},
                          {"flick", "Hold the right button and flick the mouse",
                           "Holding the right button keeps the view still; flick the mouse left, right, up or down to swing."},
                          {"buttons", "Binds for each swing",
                           "Left button: right to left. Right button: left to right. Wheel up: stab. Wheel down: overhead. Tab then "
                           "switches weapons instead of the wheel. (recommended)"}},
                         false, "buttons", {nullptr, nullptr, nullptr}});
            t.push_back({"mouse_slowdown", Page::Experimental, "Mouse and keyboard", "Turning slow-downs",
                         "How much the game's slow-downs of turning (ailments, a heavy load, some ground) apply to the mouse.",
                         {{"off", "Off"}, {"soft", "Softened (half speed becomes 71%)"}, {"full", "Full (as with a controller)"}}, false, "full",
                         {nullptr, nullptr, nullptr}});
            t.push_back({"debug_menu", Page::Experimental, "Testing", "Debug page in the F2 menu",
                         "Change settings, item spawner, creature spawner, location teleport, create teleport points.\n"
                         "Also turns on two developer keys: F8 saves a memory dump (about 32 MB a press) and F10 saves one "
                         "frame's drawing data, both into the program's folder.",
                         kOnOff, true, "off", {nullptr, nullptr, nullptr}, true});
            t.push_back({"record_pictures", Page::Experimental, "Bug reports", "Keep the last few seconds (F1 saves them)",
                         "Saw a glitch? Press F1 right away: the last few seconds of pictures are saved as a catch (the game pauses a "
                         "few seconds).",
                         kOnOff, true, "off", {nullptr, nullptr, nullptr}});
            // (2026-11-27, the user's: the catches had grown to 6.5 GB unnoticed. "a setting for how many F1 snapshots are
            // kept at a time, with a disclaimer about how much space each one takes up, how they can balloon in filesize,
            // and where they're stored")
            t.push_back({"record_keep", Page::Experimental, "Bug reports", "F1 catches kept",
                         "Each catch takes about 250 MB of disk space (512 uncompressed pictures), so they add up quickly: 10 "
                         "catches are 2.5 GB. When a new one is saved, the oldest beyond this number are deleted. They are stored "
                         "in the logs folder next to STAR.exe, as pictures_1, pictures_2, ...",
                         {{"1", "1 (250 MB)"},
                          {"3", "3 (750 MB)"},
                          {"5", "5 (1.25 GB)"},
                          {"10", "10 (2.5 GB)"},
                          {"20", "20 (5 GB)"},
                          {"0", "All (no limit)"}},
                         false, "5", {nullptr, nullptr, nullptr}});
            return t;
        }

        const Preset kPresets[kPresetCount] = {
            {"original", "Original", "As it was on the PlayStation 2.",
             "The game's own look: 640x448, 30fps, the game's shadows, flicker filter and glow. No changes to how it plays."},
            {"remastered", "Remastered", "The original, sharper and smoother.",
             "3x resolution, FXAA, unlocked framerate, sharp and close-up shadows, no flicker filter, reduced input lag, "
             "the text fixes and true analog aiming."},
            {"star", "STAR", "Remastered, plus shadow fixes and mouse play.",
             "Everything in Remastered, plus no self-shadows (the game's are very buggy), shadows that stop at walls, and mouse "
             "and keyboard play."},
        };

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string trim(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n");
            const size_t last = text.find_last_not_of(" \t\r\n");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        }
    }

    const Preset kCustom = {"custom", "Custom", "Your own settings.",
                            "Change any option below to make your own set. It is kept while you try another preset: choose Custom "
                            "to go back to it."};

    const char *pageNote(Page page)
    {
        return page == Page::Experimental ? "These settings fundamentally go against the game's intended experience and are "
                                            "recommended for repeat playthroughs."
                                          : nullptr;
    }

    const std::vector<Option> &table()
    {
        static const std::vector<Option> options = makeTable();
        return options;
    }

    const Option &option(const char *key)
    {
        for (const Option &one : table())
            if (std::string(one.key) == key)
                return one;
        std::fprintf(stderr, "[STAR] options: no option \"%s\"\n", key);
        std::abort();
    }

    int choiceIndex(const Option &option, const std::string &value)
    {
        for (size_t i = 0; i < option.choices.size(); ++i)
            if (value == option.choices[i].value)
                return static_cast<int>(i);
        return -1;
    }

    const char *choiceLabel(const Option &option, const std::string &value)
    {
        const int index = choiceIndex(option, value);
        return index >= 0 ? option.choices[static_cast<size_t>(index)].label : "";
    }

    const Preset &preset(int index) { return kPresets[std::clamp(index, 0, kPresetCount - 1)]; }

    bool matchesPreset(int index, const Values &values)
    {
        if (index < 0 || index >= kPresetCount)
            return false;
        for (const Option &one : table())
            if (one.presets[index])
            {
                const auto found = values.find(one.key);
                if (found == values.end() || found->second != one.presets[index])
                    return false;
            }
        return true;
    }

    int presetOf(const Values &values)
    {
        for (int p = 0; p < kPresetCount; ++p)
            if (matchesPreset(p, values))
                return p;
        return -1;
    }

    const char *presetId(int index) { return index >= 0 && index < kPresetCount ? kPresets[index].id : kCustom.id; }

    int chosenPreset(const Values &fromFile, const Values &values)
    {
        if (const auto saved = fromFile.find("preset"); saved != fromFile.end())
        {
            if (saved->second == kCustom.id)
                return -1;
            for (int p = 0; p < kPresetCount; ++p)
                if (saved->second == kPresets[p].id && matchesPreset(p, values))
                    return p;
        }
        return presetOf(values);
    }

    void applyPreset(int index, Values &values)
    {
        if (index < 0 || index >= kPresetCount)
            return;
        for (const Option &one : table())
            if (one.presets[index])
                values[one.key] = one.presets[index];
    }

    std::string encodeCustom(const Values &values)
    {
        std::string line;
        for (const Option &one : table())
            if (one.presets[0])
                if (const auto found = values.find(one.key); found != values.end())
                    line += (line.empty() ? "" : " ") + std::string(one.key) + ":" + found->second;
        return line;
    }

    Values decodeCustom(const std::string &line)
    {
        Values values;
        std::istringstream in(line);
        std::string pair;
        while (in >> pair)
        {
            const size_t colon = pair.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= pair.size())
                continue;
            const std::string key = lower(pair.substr(0, colon));
            for (const Option &one : table())
                if (one.presets[0] && key == one.key)
                    values[key] = lower(pair.substr(colon + 1));
        }
        return values;
    }

    Values readFile(const std::filesystem::path &file)
    {
        Values values;
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3); // a UTF-8 byte order mark (Notepad)
            const size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#' || line[start] == ';')
                continue;
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            std::string value = line.substr(equals + 1);
            // a note after the value ('#' or ';'), except in the disc image's path, where both can be part of a name
            const std::string name = lower(trim(line.substr(0, equals)));
            if (name != "disc_image")
                if (const size_t note = value.find_first_of("#;"); note != std::string::npos)
                    value.erase(note);
            values[name] = name == "disc_image" ? trim(value) : lower(trim(value));
        }
        return values;
    }

    bool writeFile(const std::filesystem::path &file, const std::vector<std::pair<std::string, std::string>> &values)
    {
        std::vector<std::string> lines;
        {
            std::ifstream in(file);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }
        }
        std::vector<bool> written(values.size(), false);
        for (std::string &line : lines)
        {
            const size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#' || line[start] == ';')
                continue;
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string name = lower(trim(line.substr(start, equals - start)));
            for (size_t i = 0; i < values.size(); ++i)
                if (name == values[i].first)
                {
                    line = values[i].first + " = " + values[i].second;
                    written[i] = true;
                }
        }
        for (size_t i = 0; i < values.size(); ++i)
            if (!written[i])
                lines.push_back(values[i].first + " = " + values[i].second);
        std::ofstream out(file, std::ios::trunc);
        for (const std::string &line : lines)
            out << line << "\n";
        return out.good();
    }

    Values withFallbacks(const Values &fromFile)
    {
        Values values;
        for (const Option &one : table())
        {
            const auto found = fromFile.find(one.key);
            std::string value = found != fromFile.end() && !found->second.empty() ? found->second : std::string(one.fallback);
            // the other spellings the program reads (main.cpp loadSettings), for every on / off option (a switch, or
            // Framerate's two choices)
            const bool onOff = one.toggle || (one.choices.size() == 2 && std::string(one.choices[0].value) == "off" &&
                                              std::string(one.choices[1].value) == "on");
            if (onOff && (value == "yes" || value == "1" || value == "true"))
                value = "on";
            else if (onOff && (value == "no" || value == "0" || value == "false"))
                value = "off";
            else if (std::string(one.key) == "self_shadows" && value == "clean")
                value = "off"; // (lit side only, taken out on 2026-11-25: read as off)
            values[one.key] = value;
        }
        return values;
    }
}
