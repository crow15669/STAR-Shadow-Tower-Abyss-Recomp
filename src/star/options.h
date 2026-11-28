#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

// STAR 2026-11-26: the settings players can change, in one table: the name in "STAR settings.ini", where it shows, its
// wording and its choices. The F2 menu (settings_menu.cpp) and the launcher (launcher.cpp) both take their texts from
// here, and the presets (Original, Remastered, STAR) say what each sets.
namespace star::options
{
    struct Choice
    {
        const char *value; // as written in the settings file
        const char *label;
        const char *help = nullptr; // shown while this choice is the one chosen
    };

    enum class Page
    {
        Graphics,
        Gameplay,
        Experimental, // (2026-11-26, the user's: mouse and keyboard, the Debug page, the F1 recorder)
    };
    // A page's own note, shown at its top in the launcher (nullptr = none).
    const char *pageNote(Page page);

    constexpr int kPresetCount = 3; // Original, Remastered, STAR

    // A number on a slider instead of a list of choices (mouse sensitivity, the aim speeds).
    struct Slider
    {
        int min = 0, max = 0;               // max 0: not a slider
        const char *format = "%d";          // printf, as ImGui shows it
        int mark = 0;                       // the warning shows for values above this
        const char *warning = nullptr;
        const char *formatAtMark = nullptr; // the format at the mark itself ("100% (PS2 Default)"; nullptr = format)
    };

    struct Option
    {
        const char *key; // the name in the settings file
        Page page;
        const char *section;
        const char *label;
        const char *help;             // under the option ("" = none)
        std::vector<Choice> choices;  // {off, on} for a switch (none for a slider)
        bool toggle;                  // a checkbox (choices[0] = off, choices[1] = on)
        const char *fallback;         // the program's own value when the file has no line for it
        const char *presets[kPresetCount]; // what each preset sets (nullptr: not part of the presets)
        bool startOnly = false;       // read once at start-up (the F2 menu says it takes effect at the next start)
        Slider slider = {};
        const char *onlyWith = nullptr; // shown only while this option is "on" (In-between pictures: Framerate Unlocked)
    };

    const std::vector<Option> &table();

    // The mouse and keyboard controls (star/bindings.h) are listed under this option (launcher and F2).
    constexpr const char *kControlsUnder = "mouse_keyboard";
    // The set-up of a controller the program does not know (star/pad_map.h) is offered under this option (launcher).
    constexpr const char *kPadSetupUnder = "late_input";
    const Option &option(const char *key); // (a name not in the table is a programming error: aborts)
    // The label of a value ("" for one that is not among the choices).
    const char *choiceLabel(const Option &option, const std::string &value);
    int choiceIndex(const Option &option, const std::string &value); // -1 when not among them

    struct Preset
    {
        const char *id; // in the settings file's "preset" line
        const char *name;
        const char *tagline;
        const char *description;
    };
    const Preset &preset(int index);
    extern const Preset kCustom;

    using Values = std::map<std::string, std::string>; // key -> value as in the file
    // The preset these values match (every option a preset sets has its value), or -1 (Custom).
    int presetOf(const Values &values);
    bool matchesPreset(int index, const Values &values);
    const char *presetId(int index); // "original", "remastered", "star"; "custom" for -1
    // The preset to show as chosen: the file's "preset" line while the values still match it ("custom" always holds),
    // else the one the values match (-1 = Custom).
    int chosenPreset(const Values &fromFile, const Values &values);
    // The values a preset sets, on top of `values`.
    void applyPreset(int index, Values &values);

    // "custom = resolution:3 fxaa:on ...": the player's own set, kept for the Custom preset while another is chosen.
    std::string encodeCustom(const Values &values); // the options the presets set
    Values decodeCustom(const std::string &line);

    // The settings file: name = value lines; '#' and ';' start a note. Values as written (not lower-cased); names in
    // lower case.
    Values readFile(const std::filesystem::path &file);
    // Each name's line is rewritten in place (every other line, the notes, kept as it is); names not in the file yet are
    // added at its end. CRLF line ends on Windows (the file is opened as text). False when the file cannot be written.
    bool writeFile(const std::filesystem::path &file, const std::vector<std::pair<std::string, std::string>> &values);

    // Every option of the table at its value: the file's, or the program's own.
    Values withFallbacks(const Values &fromFile);
}
