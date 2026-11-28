#include "star/launcher.h"

#include "star/options.h"
#include "star/bindings.h"
#include "star/pad_map.h"

#include "runtime/ps2_display_options.h"
#include "ps2_host_gamepads.h"

#include "raylib.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "rlImGui.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// STAR 2026-11-26: the launcher (launcher.h). One page: the disc image and its check, the presets (Original,
// Remastered, STAR and the player's own, Custom), every option of the settings file with its note (star/options.cpp,
// the F2 menu's wording) on three tabs (Graphics, Gameplay, Experimental), and Play. Each change is written to
// "STAR settings.ini" at once, as the F2 menu does.
namespace star::launcher
{
    namespace
    {
        namespace fs = std::filesystem;
        using options::Values;

        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }
        fs::path fromUtf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

        // The F2 menu's colours (thirdparty/pcsx2-gs/compat/HostStubs.cpp ApplyMenuStyle), on an opaque background.
        const ImVec4 kRed(0.79f, 0.12f, 0.09f, 1.0f), kRedHot(0.89f, 0.20f, 0.16f, 1.0f), kRedDark(0.45f, 0.07f, 0.05f, 1.0f);
        const ImVec4 kGood(0.50f, 0.82f, 0.50f, 1.0f), kBad(0.96f, 0.45f, 0.38f, 1.0f), kAsk(0.91f, 0.75f, 0.16f, 1.0f);
        const ImVec4 kKey(0.93f, 0.60f, 0.52f, 1.0f); // a key in the controls list
        const ImVec4 kCard(0.12f, 0.12f, 0.14f, 1.0f), kCardHover(0.20f, 0.11f, 0.10f, 1.0f), kCardEdge(0.26f, 0.26f, 0.29f, 1.0f);

        void applyStyle()
        {
            ImGui::StyleColorsDark();
            ImGuiStyle &style = ImGui::GetStyle();
            style.WindowRounding = 0.0f;
            style.FrameRounding = 3.0f;
            style.TabRounding = 3.0f;
            style.ItemSpacing = ImVec2(10.0f, 8.0f);
            style.FramePadding = ImVec2(8.0f, 5.0f);
            style.WindowBorderSize = 0.0f;
            ImVec4 *colors = style.Colors;
            colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 1.0f);
            colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
            colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.11f, 0.98f);
            colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.10f, 0.09f, 1.0f);
            colors[ImGuiCol_TabHovered] = kRedHot;
            colors[ImGuiCol_TabSelected] = kRed;
            colors[ImGuiCol_TabSelectedOverline] = kRedHot;
            colors[ImGuiCol_Button] = ImVec4(0.30f, 0.31f, 0.33f, 1.0f);
            colors[ImGuiCol_ButtonHovered] = kRedHot;
            colors[ImGuiCol_ButtonActive] = kRed;
            colors[ImGuiCol_CheckMark] = kRedHot;
            colors[ImGuiCol_SliderGrab] = kRed;
            colors[ImGuiCol_SliderGrabActive] = kRedHot;
            colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.16f, 1.0f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.12f, 0.11f, 1.0f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.12f, 0.10f, 1.0f);
            colors[ImGuiCol_Header] = ImVec4(0.30f, 0.09f, 0.07f, 1.0f);
            colors[ImGuiCol_HeaderHovered] = kRedHot;
            colors[ImGuiCol_HeaderActive] = kRed;
            colors[ImGuiCol_Separator] = ImVec4(0.26f, 0.26f, 0.29f, 1.0f);
            colors[ImGuiCol_NavCursor] = ImVec4(0.91f, 0.75f, 0.16f, 1.0f);
        }

        struct State
        {
            const Context *context = nullptr;
            Values values; // every option of the table, at its value
            Values custom; // the player's own set (the Custom preset), "" when none; while Custom is chosen, the values
            int chosen = -1; // the preset chosen (the file's "preset" line), -1 = Custom
            fs::path disc;
            std::string discProblem; // "" = it can be played
            ImFont *font = nullptr;
            float unit = 1.0f;
            std::string saveNote; // a failed write
            bool done = false;
            Result result = Result::Quit;
            // tests (STAR_LAUNCHER_*, run): the page to open, a scroll of the list, a preset to choose
            int testPage = 0;
            float testScroll = 0.0f;
            int testPreset = -2;
            std::string testSet; // "name=value": an option changed at frame 10, as a click does
            int testControls = -1; // the Controls screen opened on this tab at frame 3
            std::string testBind;  // "action=input": a control changed at frame 10, as a capture does
            // the mouse and keyboard controls (star/bindings.h) and their screen
            bindings::Binds binds;
            bool controlsWanted = false; // open the Controls screen (drawControlsScreen)
            int controlsTab = 0;
            std::string controlsFocus;   // the action clicked in the list, marked on the screen
            std::string captureId;       // the action whose input is being taken
            size_t captureSlot = 0;      // (its inputs' count: a new one)
            int captureStep = 0;         // 1 waiting for every key and button to go up, 2 listening, 3 taken: waiting again
            std::string controlsNote;    // what the last change took from another action
            // a controller the program does not know by itself (star/pad_map.h) and its set-up screen
            padmap::Map padMap;          // the file's
            bool padLines = false;       // the file has joy_ lines (they are written from then on)
            bool padWanted = false;      // open the screen (drawPadScreen)
            std::string padDevice;       // the controller the screen is for, by name
            std::string padCaptureId;    // the entry whose button is being taken
            int padCaptureStep = 0;      // 1 waiting for everything on it to be let go, 2 listening, 3 taken: waiting again
            bool padAll = false;         // every entry in turn
            std::string padNote;         // what the last change took from another entry
            std::array<float, 32> padRest{}; // its axes when the taking began (a trigger may rest at one end)
            int testPad = 0;             // tests: 1 the screen opened at frame 5, 2 and every entry in turn from frame 8
            std::string testJoy;         // tests: "frame:kind:index:value,...": the test joystick's elements set at frames
        };

        float px(const State &state, float size) { return std::round(size * state.unit); }

        void textColored(const ImVec4 &colour, const char *text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();
        }
        void note(const char *text) { textColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], text); }

        // Every option to the file (and the player's own set, and the disc image once it passed its check).
        void save(State &state, bool withDisc)
        {
            // a preset whose values no longer match (a change of any option makes Custom already: drawPage)
            if (state.chosen >= 0 && !options::matchesPreset(state.chosen, state.values))
                state.chosen = -1;
            if (state.chosen < 0)
                state.custom = options::decodeCustom(options::encodeCustom(state.values));
            std::vector<std::pair<std::string, std::string>> lines;
            for (const options::Option &one : options::table())
                lines.push_back({one.key, state.values[one.key]});
            lines.push_back({"preset", options::presetId(state.chosen)});
            for (const bindings::Action &one : bindings::actions())
            {
                const std::vector<std::string> &inputs = state.binds[one.id];
                lines.push_back({std::string("bind_") + one.id, inputs.empty() ? "none" : bindings::encode(inputs)});
            }
            if (state.padLines)
                padmap::addLines(state.padMap, lines);
            if (!state.custom.empty())
                lines.push_back({"custom", options::encodeCustom(state.custom)});
            if (withDisc && !state.disc.empty() && state.discProblem.empty())
                lines.push_back({"disc_image", utf8(state.disc)});
            state.saveNote = options::writeFile(state.context->settingsFile, lines) ? std::string()
                                                                                  : "Could not write " + utf8(state.context->settingsFile);
        }

        void useDisc(State &state, const fs::path &disc)
        {
            state.disc = disc;
            state.discProblem = disc.empty() ? std::string() : state.context->checkDisc(disc);
            if (!disc.empty() && state.discProblem.empty())
                save(state, true);
        }

        // An option was changed: any change makes the player's own set (the user's word, 2026-11-26).
        void optionChanged(State &state)
        {
            state.chosen = -1;
            save(state, false);
        }

        // The player's own set is kept in state.custom while Custom is chosen (save), so a preset leaves it for later.
        // Custom with no set of its own yet (a preset was all there was): the values as they are become it.
        void choosePreset(State &state, int index)
        {
            if (index >= 0)
                options::applyPreset(index, state.values);
            else
                for (const auto &[key, value] : state.custom)
                    state.values[key] = value;
            state.chosen = index;
            save(state, false);
        }

        void drawHeader(State &state)
        {
            ImGui::PushFont(nullptr, px(state, 46.0f));
            ImGui::TextUnformatted("STAR");
            ImGui::PopFont();
            ImGui::SameLine(0.0f, px(state, 14.0f));
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(0.0f, px(state, 1.0f)));
            ImGui::PushFont(nullptr, px(state, 21.0f));
            ImGui::TextUnformatted("Shadow Tower Abyss");
            ImGui::PopFont();
            ImGui::PushFont(nullptr, px(state, 16.0f));
            ImGui::TextDisabled("Recompiled from the PS2 game");
            ImGui::PopFont();
            ImGui::EndGroup();
        }

        void drawDisc(State &state)
        {
            ImGui::Dummy(ImVec2(0.0f, px(state, 6.0f)));
            ImGui::SeparatorText("Disc image");
            if (state.disc.empty())
                ImGui::TextDisabled("None chosen");
            else
            {
                ImGui::TextUnformatted(utf8(state.disc.filename()).c_str());
                note(utf8(state.disc.parent_path()).c_str());
            }
            if (state.disc.empty())
                textColored(kAsk, "Choose your disc image of Shadow Tower Abyss (SLPS-25217) with the English patch (ISO or CHD), or "
                                  "drop it on this window.");
            else if (!state.discProblem.empty())
                textColored(kBad, state.discProblem.c_str());
            else
                textColored(kGood, "Ready: Shadow Tower Abyss (SLPS-25217) with the English patch.");
            if (ImGui::Button(state.disc.empty() ? "Choose..." : "Choose another...", ImVec2(px(state, 170.0f), 0.0f)))
            {
                fs::path picked = state.disc;
                if (pickDiscImage(GetWindowHandle(), picked))
                    useDisc(state, picked);
            }
        }

        void drawPresets(State &state)
        {
            ImGui::Dummy(ImVec2(0.0f, px(state, 6.0f)));
            ImGui::SeparatorText("Preset");
            const int current = state.chosen; // -1 = Custom
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = px(state, 54.0f), pad = px(state, 12.0f);
            int hovered = -2; // -2 = none, -1 = Custom
            for (int i = 0; i < 4; ++i)
            {
                const int index = i < 3 ? i : -1;
                const options::Preset &shown = i < 3 ? options::preset(i) : options::kCustom;
                const bool enabled = true; // (Custom too, 2026-11-26: the user could not choose it while a preset matched)
                ImGui::PushID(i);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::BeginDisabled(!enabled);
                const bool clicked = ImGui::InvisibleButton("##preset", ImVec2(width, height));
                ImGui::EndDisabled();
                const bool hover = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                const bool focused = ImGui::IsItemFocused();
                if (hover || focused)
                    hovered = index;
                const bool chosen = index == current;
                ImDrawList *draw = ImGui::GetWindowDrawList();
                const ImVec2 end(at.x + width, at.y + height);
                draw->AddRectFilled(at, end, ImGui::GetColorU32(chosen ? kRedDark : (hover && enabled) ? kCardHover : kCard), px(state, 5.0f));
                draw->AddRect(at, end, ImGui::GetColorU32(chosen ? kRedHot : kCardEdge), px(state, 5.0f), 0, chosen ? px(state, 2.0f) : 1.0f);
                const ImU32 nameColour = ImGui::GetColorU32(enabled ? ImGui::GetStyle().Colors[ImGuiCol_Text] : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                const ImU32 tagColour = ImGui::GetColorU32(chosen ? ImVec4(0.86f, 0.78f, 0.76f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                const ImVec4 clip(at.x + pad, at.y, end.x - pad, end.y);
                const float nameSize = px(state, 21.0f), tagSize = px(state, 16.0f);
                draw->AddText(ImGui::GetFont(), nameSize, ImVec2(at.x + pad, at.y + px(state, 5.0f)), nameColour, shown.name, nullptr, 0.0f, &clip);
                draw->AddText(ImGui::GetFont(), tagSize, ImVec2(at.x + pad, at.y + px(state, 6.0f) + nameSize + px(state, 2.0f)), tagColour,
                              shown.tagline, nullptr, 0.0f, &clip);
                if (clicked && enabled && !chosen)
                    choosePreset(state, index);
                ImGui::PopID();
            }
            const options::Preset &described = hovered != -2 ? (hovered >= 0 ? options::preset(hovered) : options::kCustom)
                                                             : (current >= 0 ? options::preset(current) : options::kCustom);
            ImGui::TextWrapped("%s", described.description);
            note("Presets leave speeds, mouse buttons and the Debug page as they are.");
        }

        // Play and Quit at the bottom of the left column.
        void drawButtons(State &state)
        {
            const float tall = px(state, 48.0f), spacing = ImGui::GetStyle().ItemSpacing.x;
            const float width = ImGui::GetContentRegionAvail().x, quit = std::floor(width * 0.3f);
            const bool ready = !state.disc.empty() && state.discProblem.empty();
            const float lines = ImGui::GetTextLineHeightWithSpacing() * (ready ? 1.0f : 2.0f);
            const float top = ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y - tall - lines - ImGui::GetStyle().ItemSpacing.y;
            if (top > ImGui::GetCursorPosY())
                ImGui::SetCursorPosY(top);
            if (!state.saveNote.empty())
                ImGui::TextColored(kBad, "%s", state.saveNote.c_str());
            else
                ImGui::TextDisabled("Saved as you go. In the game: F2.");
            if (!ready)
                ImGui::TextColored(kAsk, "%s", "Play needs a disc image that passes the check.");
            ImGui::BeginDisabled(!ready);
            ImGui::PushStyleColor(ImGuiCol_Button, kRed);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedHot);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRedDark);
            ImGui::PushFont(nullptr, px(state, 25.0f));
            if (ImGui::Button("Play", ImVec2(width - quit - spacing, tall)) && ready)
            {
                save(state, true);
                state.result = Result::Play;
                state.done = true;
            }
            ImGui::PopFont();
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Quit", ImVec2(quit, tall)))
            {
                state.result = Result::Quit;
                state.done = true;
            }
        }

        // One section's rows: the label and its note on the left, the control on the right.
        bool drawRow(State &state, const options::Option &one)
        {
            bool changed = false;
            std::string &value = state.values[one.key];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextWrapped("%s", one.label); // (wrapped: the column narrows on a page with long choices)
            if (one.help && *one.help)
                note(one.help);
            const int index = options::choiceIndex(one, value);
            if (index >= 0)
                if (const char *chosen = one.choices[static_cast<size_t>(index)].help)
                    note(chosen);
            const bool slider = one.slider.max > 0;
            int number = 0;
            if (slider)
            {
                number = std::atoi(value.c_str());
                if (number <= 0)
                    number = std::atoi(one.fallback);
                if (one.slider.warning && number > one.slider.mark)
                    textColored(kAsk, one.slider.warning);
            }
            ImGui::Dummy(ImVec2(0.0f, px(state, 4.0f)));
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(one.key);
            if (slider)
            {
                // the value follows the drag; the file is written when it is let go
                ImGui::SetNextItemWidth(-FLT_MIN);
                const char *format = one.slider.formatAtMark && number == one.slider.mark ? one.slider.formatAtMark : one.slider.format;
                if (ImGui::SliderInt("##value", &number, one.slider.min, one.slider.max, format, ImGuiSliderFlags_AlwaysClamp))
                    value = std::to_string(number);
                changed = ImGui::IsItemDeactivatedAfterEdit();
            }
            else if (one.toggle)
            {
                bool on = value == "on";
                if (ImGui::Checkbox(on ? "On" : "Off", &on))
                {
                    value = on ? "on" : "off";
                    changed = true;
                }
            }
            else
            {
                const std::string other = value + " (from the settings file)";
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##value", index >= 0 ? one.choices[static_cast<size_t>(index)].label : other.c_str()))
                {
                    for (size_t i = 0; i < one.choices.size(); ++i)
                    {
                        const bool selected = static_cast<int>(i) == index;
                        if (ImGui::Selectable(one.choices[i].label, selected) && !selected)
                        {
                            value = one.choices[i].value;
                            changed = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::PopID();
            return changed;
        }

        // The mouse and keyboard controls (star/bindings.h), the list's whole width: the keys in light red, what they
        // do beside them, every other row a shade lighter (the user: the old one-paragraph note was "very cramped").
        // (2026-11-26: the rows are the player's controls, star/bindings.h; a click opens the Controls screen there)
        void drawControls(State &state)
        {
            const std::string swings = state.values["melee_swings"];
            struct Row
            {
                std::string keys;
                const bindings::Action *action; // nullptr: the mouse's look
            };
            std::vector<Row> rows{{"Mouse", nullptr}};
            for (const bindings::Action &one : bindings::actions())
                if (bindings::inUse(one, swings))
                {
                    std::string keys;
                    for (const std::string &input : state.binds[one.id])
                        keys += (keys.empty() ? "" : " / ") + bindings::displayName(input);
                    rows.push_back({keys.empty() ? "(none)" : keys, &one});
                }
            float keyWidth = 0.0f;
            for (const Row &row : rows)
                keyWidth = std::max(keyWidth, ImGui::CalcTextSize(row.keys.c_str()).x);
            keyWidth = std::min(std::ceil(keyWidth) + px(state, 16.0f), std::floor(ImGui::GetContentRegionAvail().x * 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(state, 10.0f), px(state, 4.0f)));
            ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(1.0f, 1.0f, 1.0f, 0.02f));
            ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.0f, 1.0f, 1.0f, 0.055f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.34f, 0.12f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.45f, 0.12f, 0.10f, 1.0f));
            if (ImGui::BeginTable("##controls", 2, ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, keyWidth);
                ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
                for (const Row &row : rows)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, kKey);
                    if (row.action)
                    {
                        if (ImGui::Selectable((row.keys + "##" + row.action->id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                        {
                            state.controlsWanted = true;
                            state.controlsTab = static_cast<int>(row.action->tab);
                            state.controlsFocus = row.action->id;
                        }
                    }
                    else
                        ImGui::TextUnformatted(row.keys.c_str());
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", row.action ? row.action->label : "Look around");
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            if (ImGui::Button("Change controls..."))
            {
                state.controlsWanted = true;
                state.controlsTab = 0;
                state.controlsFocus.clear();
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("or click a control above.");
            ImGui::Dummy(ImVec2(0.0f, px(state, 8.0f)));
        }

        // A control changed: any change makes the player's own set, as an option's does.
        void controlsChanged(State &state)
        {
            state.chosen = -1;
            save(state, false);
        }

        void startCapture(State &state, const std::string &id, size_t slot)
        {
            state.captureId = id;
            state.captureSlot = slot;
            state.captureStep = 1;
            state.controlsNote.clear();
        }

        // Before each frame of the screen: the key, mouse button or wheel turn for the slot being changed. Esc cancels.
        void updateCapture(State &state)
        {
            if (state.captureStep == 0)
                return;
            const auto anyDown = []
            {
                for (const int code : bindings::keyCodes())
                    if (IsKeyDown(code))
                        return true;
                for (int button = MOUSE_BUTTON_LEFT; button <= MOUSE_BUTTON_EXTRA; ++button)
                    if (IsMouseButtonDown(button))
                        return true;
                return false;
            };
            if (state.captureStep == 1 || state.captureStep == 3)
            {
                if (!anyDown())
                {
                    if (state.captureStep == 3)
                        state.captureId.clear();
                    state.captureStep = state.captureStep == 1 ? 2 : 0;
                }
                return;
            }
            if (IsKeyPressed(KEY_ESCAPE) || !IsWindowFocused())
            {
                state.captureStep = 3;
                return;
            }
            std::string input;
            for (const int code : bindings::keyCodes())
                if (code != KEY_ESCAPE && IsKeyPressed(code))
                {
                    input = bindings::fromKeyCode(code);
                    break;
                }
            for (int button = MOUSE_BUTTON_LEFT; input.empty() && button <= MOUSE_BUTTON_EXTRA; ++button)
                if (IsMouseButtonPressed(button))
                    input = bindings::fromMouseButton(button);
            if (input.empty())
                if (const float wheel = GetMouseWheelMove(); wheel != 0.0f)
                    input = wheel > 0.0f ? "WheelUp" : "WheelDown";
            if (input.empty())
                return;
            state.controlsNote = bindings::assign(state.binds, state.captureId, state.captureSlot, input);
            state.captureStep = 3;
            controlsChanged(state);
        }

        // "Only with Sword swings: ..." for a swing of another style
        std::string stylesText(const char *styles)
        {
            std::string text, one;
            std::string list = styles ? styles : "";
            size_t at = 0;
            while (at <= list.size())
            {
                const size_t bar = list.find('|', at);
                one = list.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
                text += (text.empty() ? "" : " or ") + std::string(options::choiceLabel(options::option("melee_swings"), one));
                if (bar == std::string::npos)
                    break;
                at = bar + 1;
            }
            return "Only with Sword swings: " + text + ".";
        }

        // The keys of one action as buttons ("keycaps"): a click takes a new one for that place, x takes it away, + adds.
        void drawInputs(State &state, const bindings::Action &one, bool usable)
        {
            std::vector<std::string> &inputs = state.binds[one.id];
            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            const ImGuiStyle &style = ImGui::GetStyle();
            bool first = true;
            const auto place = [&](float width)
            {
                if (!first && ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right)
                    ImGui::SameLine();
                first = false;
            };
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.17f, 0.17f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.34f, 0.12f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRed);
            ImGui::BeginDisabled(!usable);
            bool removed = false;
            for (size_t i = 0; i < inputs.size() && !removed; ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                const bool taking = state.captureStep != 0 && state.captureId == one.id && state.captureSlot == i;
                const std::string label = taking ? "Press a key..." : bindings::displayName(inputs[i]);
                const float width = ImGui::CalcTextSize(label.c_str()).x + style.FramePadding.x * 2.0f + ImGui::GetFrameHeight() + 2.0f;
                place(width);
                ImGui::BeginGroup();
                if (taking)
                    ImGui::PushStyleColor(ImGuiCol_Text, kAsk);
                if (ImGui::Button(label.c_str()))
                    startCapture(state, one.id, i);
                if (taking)
                    ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::Button("\xC3\x97", ImVec2(ImGui::GetFrameHeight(), 0.0f))) // x: takes it away
                {
                    bindings::remove(state.binds, one.id, i);
                    state.controlsNote.clear();
                    controlsChanged(state);
                    removed = true;
                }
                ImGui::EndGroup();
                ImGui::PopID();
            }
            if (!removed && inputs.size() < bindings::kMaxInputs)
            {
                const bool taking = state.captureStep != 0 && state.captureId == one.id && state.captureSlot >= inputs.size();
                const char *label = taking ? "Press a key..." : "+";
                place(ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f);
                if (taking)
                    ImGui::PushStyleColor(ImGuiCol_Text, kAsk);
                if (ImGui::Button((std::string(label) + "##add").c_str()))
                    startCapture(state, one.id, inputs.size());
                if (taking)
                    ImGui::PopStyleColor();
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
        }

        // The Controls screen: every action in tabs, each with its keys.
        void drawControlsScreen(State &state)
        {
            static int opened = 0; // frames since it was opened (the tab asked for is chosen then)
            if (state.controlsWanted)
            {
                ImGui::OpenPopup("##controlsScreen");
                state.controlsWanted = false;
                state.controlsNote.clear();
                opened = 0;
            }
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowSize(ImVec2(std::floor(display.x * 0.86f), std::floor(display.y * 0.88f)));
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(state, 26.0f), px(state, 20.0f)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.09f, 0.09f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.6f));
            const bool open = ImGui::BeginPopupModal("##controlsScreen", nullptr,
                                                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                         ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
            if (!open)
                return;
            ++opened;
            const bool busy = state.captureStep != 0;
            ImGui::PushFont(nullptr, px(state, 30.0f));
            ImGui::TextUnformatted("Controls");
            ImGui::PopFont();
            note("Click a key to change it, then press the new key or mouse button (Esc cancels). \xC3\x97 takes one away, + adds "
                 "another, up to four. The mouse always looks around.");
            const std::string swings = state.values["melee_swings"];
            const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f + px(state, 8.0f);
            ImGui::BeginDisabled(busy);
            if (ImGui::BeginTabBar("##controlTabs"))
            {
                const char *names[] = {"Movement", "Combat", "Menus"};
                for (int tab = 0; tab < 3; ++tab)
                {
                    const ImGuiTabItemFlags flags = opened <= 2 && tab == state.controlsTab ? ImGuiTabItemFlags_SetSelected : 0;
                    if (!ImGui::BeginTabItem(names[tab], nullptr, flags))
                        continue;
                    if (ImGui::BeginChild("##rows", ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y - footer, px(state, 120.0f)))))
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(state, 10.0f), px(state, 7.0f)));
                        if (ImGui::BeginTable("##actions", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
                        {
                            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                            for (const bindings::Action &one : bindings::actions())
                            {
                                if (static_cast<int>(one.tab) != tab)
                                    continue;
                                const bool usable = bindings::inUse(one, swings);
                                ImGui::TableNextRow();
                                if (state.controlsFocus == one.id)
                                {
                                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.30f, 0.09f, 0.07f, 0.55f)));
                                    if (opened == 3)
                                        ImGui::SetScrollHereY(0.3f);
                                }
                                ImGui::TableSetColumnIndex(0);
                                ImGui::AlignTextToFramePadding();
                                if (usable)
                                    ImGui::TextWrapped("%s", one.label);
                                else
                                    ImGui::TextDisabled("%s", one.label);
                                if (one.note && *one.note)
                                    note(one.note);
                                if (!usable)
                                    note(stylesText(one.style).c_str());
                                ImGui::TableSetColumnIndex(1);
                                ImGui::PushID(one.id);
                                drawInputs(state, one, usable);
                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                        ImGui::PopStyleVar();
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndDisabled();
            // the footer: a capture's question, what the last change took, Reset and Done
            if (busy && state.captureStep <= 2)
            {
                const bindings::Action *taking = bindings::action(state.captureId);
                textColored(kAsk, ("Press a key or mouse button (or turn the wheel) for " + std::string(taking ? taking->label : "") +
                                   ". Esc cancels.").c_str());
            }
            else if (!state.controlsNote.empty())
                textColored(kAsk, state.controlsNote.c_str());
            else
                note("Changes are saved as you make them.");
            ImGui::BeginDisabled(busy);
            const float done = px(state, 140.0f);
            const float doneX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - done;
            if (ImGui::Button("Reset to defaults"))
            {
                state.binds = bindings::defaults();
                state.controlsNote = "Every control is back to its default.";
                controlsChanged(state);
            }
            ImGui::SameLine(doneX);
            if (ImGui::Button("Done", ImVec2(done, 0.0f)))
            {
                state.controlsFocus.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        // ---- a controller the program does not know by itself: its set-up (star/pad_map.h) ----
        namespace gp = ps2_host_gamepads;

        // The controllers connected that SDL does not know as gamepads: (device number, name).
        std::vector<std::pair<int, std::string>> rawControllers()
        {
            std::vector<std::pair<int, std::string>> list;
            for (int device = 0; device < gp::kMaxDevices; ++device)
                if (gp::isRaw(device))
                    list.push_back({device, gp::name(device)});
            return list;
        }

        int padDeviceNumber(const State &state) // the screen's controller; -1: not connected
        {
            for (const auto &[device, name] : rawControllers())
                if (name == state.padDevice)
                    return device;
            return -1;
        }

        float padMoved(const State &state, int device, int axis)
        {
            return gp::rawAxis(device, axis) - state.padRest[static_cast<size_t>(axis)];
        }

        bool padAnyDown(const State &state, int device)
        {
            for (int b = 0; b < gp::rawButtonCount(device); ++b)
                if (gp::rawButton(device, b))
                    return true;
            for (int h = 0; h < gp::rawHatCount(device); ++h)
                for (int side = 0; side < 4; ++side)
                    if (gp::rawHat(device, h, static_cast<unsigned char>(1u << side)))
                        return true;
            for (int a = 0; a < std::min(gp::rawAxisCount(device), static_cast<int>(state.padRest.size())); ++a)
                if (std::fabs(padMoved(state, device, a)) > 0.3f)
                    return true;
            return false;
        }

        void endPadCapture(State &state)
        {
            state.padCaptureStep = 0;
            state.padCaptureId.clear();
            state.padAll = false;
        }

        void startPadCapture(State &state, const std::string &id, bool all)
        {
            const int device = padDeviceNumber(state);
            if (device < 0)
                return;
            state.padCaptureId = id;
            state.padAll = all;
            state.padCaptureStep = 1;
            state.padNote.clear();
            for (size_t a = 0; a < state.padRest.size(); ++a)
                state.padRest[a] = static_cast<int>(a) < gp::rawAxisCount(device) ? gp::rawAxis(device, static_cast<int>(a)) : 0.0f;
        }

        // An entry was taken or passed over: the next one when going through them all.
        void padCaptureNext(State &state)
        {
            if (state.padAll)
            {
                const std::vector<padmap::Entry> &all = padmap::entries();
                for (size_t i = 0; i + 1 < all.size(); ++i)
                    if (state.padCaptureId == all[i].id)
                    {
                        state.padCaptureId = all[i + 1].id;
                        state.padCaptureStep = 1;
                        return;
                    }
                state.padNote = "That was the last one.";
            }
            endPadCapture(state);
        }

        // Before each frame of the screen: the button, hat or axis for the entry being set. Esc stops.
        void updatePadCapture(State &state)
        {
            if (state.padCaptureStep == 0)
                return;
            const int device = padDeviceNumber(state);
            if (device < 0)
            {
                endPadCapture(state);
                state.padNote = "The controller is no longer connected.";
                return;
            }
            if (IsKeyPressed(KEY_ESCAPE))
            {
                endPadCapture(state);
                return;
            }
            if (state.padCaptureStep == 1 || state.padCaptureStep == 3)
            {
                if (!padAnyDown(state, device))
                {
                    if (state.padCaptureStep == 1)
                        state.padCaptureStep = 2;
                    else
                        padCaptureNext(state);
                }
                return;
            }
            const padmap::Entry *entry = padmap::entry(state.padCaptureId);
            if (!entry)
            {
                endPadCapture(state);
                return;
            }
            std::string input;
            if (!entry->opposite) // (a stick's row takes an axis only)
            {
                for (int b = 0; input.empty() && b < gp::rawButtonCount(device); ++b)
                    if (gp::rawButton(device, b))
                        input = "Button " + std::to_string(b + 1);
                static const char *const kSides[] = {"North", "East", "South", "West"};
                for (int h = 0; input.empty() && h < gp::rawHatCount(device); ++h)
                    for (int side = 0; input.empty() && side < 4; ++side)
                        if (gp::rawHat(device, h, static_cast<unsigned char>(1u << side)))
                            input = "Hat " + std::to_string(h + 1) + " " + kSides[side];
            }
            if (input.empty())
            {
                // the axis moved furthest from where it rested (a stick pushed a little askew moves two)
                int axis = -1;
                float most = 0.6f;
                for (int a = 0; a < std::min(gp::rawAxisCount(device), static_cast<int>(state.padRest.size())); ++a)
                    if (const float moved = std::fabs(padMoved(state, device, a)); moved > most)
                    {
                        most = moved;
                        axis = a;
                    }
                if (axis >= 0)
                    input = std::string(padMoved(state, device, axis) < 0.0f ? "-" : "+") + "Axis " + std::to_string(axis + 1);
            }
            if (input.empty())
                return;
            if (state.padMap.device != state.padDevice) // another controller's set-up: this one's replaces it
            {
                state.padMap = {};
                state.padMap.device = state.padDevice;
            }
            std::string taken;
            if (padmap::assign(state.padMap, entry->id, input, taken))
            {
                state.padNote = taken;
                state.padLines = true;
                save(state, false);
            }
            state.padCaptureStep = 3;
        }

        // Under "Reduced input lag" (Gameplay): only while a controller SDL does not know is connected, or one is set up.
        bool padSetupShown(const State &state)
        {
            return padmap::count(state.padMap) > 0 || !rawControllers().empty();
        }

        void drawPadSetup(State &state)
        {
            const auto raw = rawControllers();
            const bool saved = padmap::count(state.padMap) > 0;
            // the controller this is about: the one set up while it is connected, else the first one connected
            std::string name;
            bool connected = false;
            for (const auto &[device, one] : raw)
                if (saved && one == state.padMap.device)
                {
                    name = one;
                    connected = true;
                }
            if (!connected && !raw.empty())
            {
                name = raw.front().second;
                connected = true;
            }
            if (!connected)
                name = state.padMap.device;
            const bool itsOwn = saved && name == state.padMap.device;
            ImGui::Dummy(ImVec2(0.0f, px(state, 2.0f)));
            ImGui::TextWrapped("Controller: %s", name.c_str());
            if (!connected)
                note("Its buttons are set up, but it is not connected now.");
            else if (itsOwn)
            {
                const int set = padmap::count(state.padMap), all = static_cast<int>(padmap::entries().size());
                textColored(kGood, set == all ? "Its buttons are set up."
                                              : ("Set up: " + std::to_string(set) + " of " + std::to_string(all) + " buttons and sticks.").c_str());
            }
            else
                textColored(kAsk, "The program does not know this controller's buttons by itself. Set them up once to play with it.");
            if (ImGui::Button(itsOwn ? "Change its buttons..." : "Set up its buttons..."))
            {
                state.padWanted = true;
                state.padDevice = name;
            }
            ImGui::Dummy(ImVec2(0.0f, px(state, 8.0f)));
        }

        // The set-up screen: every button and stick of the PS2 controller, each with the controller's own.
        void drawPadScreen(State &state)
        {
            if (state.padWanted)
            {
                ImGui::OpenPopup("##padScreen");
                state.padWanted = false;
                state.padNote.clear();
            }
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowSize(ImVec2(std::floor(display.x * 0.86f), std::floor(display.y * 0.88f)));
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(state, 26.0f), px(state, 20.0f)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.09f, 0.09f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.6f));
            const bool open = ImGui::BeginPopupModal("##padScreen", nullptr,
                                                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                         ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
            if (!open)
                return;
            const bool busy = state.padCaptureStep != 0;
            const bool connected = padDeviceNumber(state) >= 0;
            static const padmap::Map kNone;
            const bool itsOwn = state.padMap.device == state.padDevice;
            const padmap::Map &map = itsOwn ? state.padMap : kNone;
            ImGui::PushFont(nullptr, px(state, 30.0f));
            ImGui::TextUnformatted("Controller set-up");
            ImGui::PopFont();
            // the controller: its name, or a choice when several are connected
            const auto raw = rawControllers();
            if (raw.size() > 1 && connected)
            {
                ImGui::BeginDisabled(busy);
                ImGui::SetNextItemWidth(std::min(px(state, 460.0f), ImGui::GetContentRegionAvail().x));
                if (ImGui::BeginCombo("##padDevice", state.padDevice.c_str()))
                {
                    for (const auto &[device, name] : raw)
                        if (ImGui::Selectable((name + "##" + std::to_string(device)).c_str(), name == state.padDevice))
                        {
                            state.padDevice = name;
                            state.padNote.clear();
                        }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
            }
            else
                textColored(kKey, (state.padDevice + (connected ? "" : " (not connected)")).c_str());
            note("The program does not know this controller by itself, so it has to be told once which of its buttons is which "
                 "button of the PlayStation 2 controller. Click a row and press that button on the controller, or go through "
                 "them all in order. Leave out what the controller does not have. What each button does in the game is still "
                 "chosen in the game's own options (Control Type).");
            if (!itsOwn && padmap::count(state.padMap) > 0)
                textColored(kAsk, ("The set-up kept now is for " + state.padMap.device + ". Setting this controller up replaces it.").c_str());
            const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f + px(state, 8.0f);
            ImGui::BeginDisabled(busy);
            if (ImGui::BeginChild("##padRows", ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y - footer, px(state, 120.0f)))))
            {
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(state, 10.0f), px(state, 5.0f)));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.17f, 0.17f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.34f, 0.12f, 0.10f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRed);
                if (ImGui::BeginTable("##padEntries", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
                {
                    ImGui::TableSetupColumn("button", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                    ImGui::TableSetupColumn("its own", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                    std::string group;
                    for (const padmap::Entry &one : padmap::entries())
                    {
                        if (group != one.group)
                        {
                            group = one.group;
                            ImGui::TableNextRow();
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.30f, 0.09f, 0.07f, 0.35f)));
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(one.group);
                        }
                        const bool taking = busy && state.padCaptureId == one.id;
                        ImGui::TableNextRow();
                        if (taking)
                        {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.30f, 0.09f, 0.07f, 0.55f)));
                            ImGui::SetScrollHereY(0.5f);
                        }
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Indent(px(state, 14.0f));
                        ImGui::TextUnformatted(one.label);
                        if (*one.where)
                        {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(%s)", one.where);
                        }
                        ImGui::Unindent(px(state, 14.0f));
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID(one.id);
                        const auto found = map.inputs.find(one.id);
                        const bool set = found != map.inputs.end() && !found->second.empty();
                        const std::string label = taking ? (one.opposite ? "Push it now..." : "Press it now...")
                                                         : set ? padmap::displayName(one, found->second) : "Not set";
                        ImGui::PushStyleColor(ImGuiCol_Text, taking ? kAsk : set ? kKey : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                        ImGui::BeginDisabled(!connected);
                        if (ImGui::Button(label.c_str(), ImVec2(px(state, 190.0f), 0.0f)))
                            startPadCapture(state, one.id, false);
                        ImGui::EndDisabled();
                        ImGui::PopStyleColor();
                        if (set && !taking)
                        {
                            ImGui::SameLine(0.0f, 2.0f);
                            if (ImGui::Button("\xC3\x97", ImVec2(ImGui::GetFrameHeight(), 0.0f))) // x: takes it away
                            {
                                state.padMap.inputs.erase(one.id);
                                state.padNote.clear();
                                save(state, false);
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar();
            }
            ImGui::EndChild();
            ImGui::EndDisabled();
            // the footer: what is being asked for, what the last change took, and the buttons
            if (busy)
            {
                const padmap::Entry *taking = padmap::entry(state.padCaptureId);
                std::string ask = taking ? std::string(taking->label) : std::string();
                if (taking && *taking->where)
                    ask += std::string(" (") + taking->where + ")";
                textColored(kAsk, state.padCaptureStep == 2 ? ((taking && taking->opposite ? "Push and hold on the controller: " : "Press on the controller: ") + ask +
                                                              ". Esc on the keyboard stops.").c_str()
                                                            : "Let go of everything on the controller...");
            }
            else if (!state.padNote.empty())
                textColored(kAsk, state.padNote.c_str());
            else
                note(connected ? "Changes are saved as you make them." : "Connect the controller to change its buttons.");
            const float done = px(state, 140.0f);
            const float doneX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - done;
            if (busy && state.padAll)
            {
                if (ImGui::Button("Skip this one"))
                    padCaptureNext(state);
                ImGui::SameLine();
                if (ImGui::Button("Stop"))
                    endPadCapture(state);
            }
            else
            {
                ImGui::BeginDisabled(busy || !connected);
                if (ImGui::Button("Go through them all in order"))
                    startPadCapture(state, padmap::entries().front().id, true);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(busy || !itsOwn || padmap::count(state.padMap) == 0);
                if (ImGui::Button("Clear all"))
                {
                    state.padMap.inputs.clear();
                    state.padNote = "Every button is cleared.";
                    save(state, false);
                }
                ImGui::EndDisabled();
            }
            ImGui::SameLine(doneX);
            if (ImGui::Button("Done", ImVec2(done, 0.0f)))
            {
                endPadCapture(state);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        void drawPage(State &state, options::Page page)
        {
            // the controls' column: wide enough for the page's longest choice (the gun's "Toggle: press to aim, press
            // again to holster"), at least 300, at most 55% of the list
            float control = px(state, 300.0f);
            for (const options::Option &one : options::table())
                if (one.page == page && !one.toggle)
                    for (const options::Choice &choice : one.choices)
                        control = std::max(control, ImGui::CalcTextSize(choice.label).x + ImGui::GetFrameHeight() +
                                                        ImGui::GetStyle().FramePadding.x * 2.0f + px(state, 6.0f));
            control = std::min(std::ceil(control), std::floor(ImGui::GetContentRegionAvail().x * 0.55f));
            const auto openTable = [&](const std::string &id)
            {
                const bool opened = ImGui::BeginTable(id.c_str(), 2, ImGuiTableFlags_SizingStretchProp);
                if (opened)
                {
                    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, control);
                }
                return opened;
            };
            std::string section;
            bool open = false, changed = false;
            for (const options::Option &one : options::table())
            {
                if (one.page != page || (one.onlyWith && state.values[one.onlyWith] != "on"))
                    continue;
                if (section != one.section)
                {
                    if (open)
                        ImGui::EndTable();
                    section = one.section;
                    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1.0f)
                        ImGui::Dummy(ImVec2(0.0f, px(state, 6.0f)));
                    ImGui::SeparatorText(one.section);
                    open = openTable(one.section);
                }
                if (open && drawRow(state, one))
                    changed = true;
                if (open && std::string(one.key) == options::kControlsUnder)
                {
                    // the controls list takes the whole width: the section's table ends and goes on after it
                    ImGui::EndTable();
                    drawControls(state);
                    open = openTable(section + "##after");
                }
                if (open && std::string(one.key) == options::kPadSetupUnder && padSetupShown(state))
                {
                    ImGui::EndTable();
                    drawPadSetup(state);
                    open = openTable(section + "##afterPad");
                }
            }
            if (open)
                ImGui::EndTable();
            if (changed)
                optionChanged(state);
        }

        void drawOptions(State &state, float height)
        {
            if (!ImGui::BeginTabBar("##pages"))
                return;
            const struct
            {
                const char *name;
                options::Page page;
            } pages[] = {{"Graphics", options::Page::Graphics}, {"Gameplay", options::Page::Gameplay}, {"Experimental", options::Page::Experimental}};
            static int frames = 0;
            ++frames;
            for (const auto &one : pages)
                if (ImGui::BeginTabItem(one.name, nullptr,
                                        frames <= 2 && static_cast<int>(one.page) == state.testPage ? ImGuiTabItemFlags_SetSelected : 0))
                {
                    if (ImGui::BeginChild("##list", ImVec2(0.0f, std::max(height - ImGui::GetFrameHeightWithSpacing(), px(state, 80.0f))),
                                          ImGuiChildFlags_None))
                    {
                        if (frames > 2 && frames <= 6 && state.testScroll > 0.0f)
                            ImGui::SetScrollY(state.testScroll * state.unit);
                        ImGui::Dummy(ImVec2(0.0f, px(state, 2.0f)));
                        if (const char *pageNote = options::pageNote(one.page))
                            textColored(kAsk, pageNote);
                        drawPage(state, one.page);
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            ImGui::EndTabBar();
        }

    }

    Result run(const Context &context, fs::path &disc)
    {
        State state;
        state.context = &context;
        const Values fromFile = options::readFile(context.settingsFile);
        state.values = options::withFallbacks(fromFile);
        if (const auto custom = fromFile.find("custom"); custom != fromFile.end())
            state.custom = options::decodeCustom(custom->second);
        state.chosen = options::chosenPreset(fromFile, state.values);
        state.binds = bindings::fromValues(fromFile);
        state.padMap = padmap::fromValues(fromFile);
        state.padLines = padmap::inValues(fromFile);
        if (state.chosen < 0)
            state.custom = options::decodeCustom(options::encodeCustom(state.values));
        state.disc = disc;
        if (!disc.empty())
            state.discProblem = context.checkDisc(disc);

        // The game's window (PS2Runtime::initialize keeps it: ps2_host_display::openHostWindow), with its flags.
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_ALWAYS_RUN | (ps2_host_display::vsyncEnabled() ? FLAG_VSYNC_HINT : 0u));
        InitWindow(1120, 790, context.title);
        SetExitKey(KEY_NULL);
        {
            const int monitor = GetCurrentMonitor();
            const int mw = GetMonitorWidth(monitor), mh = GetMonitorHeight(monitor);
            if (mw > 0 && mh > 0)
            {
                int h = std::clamp(mh * 3 / 4, 560, 1200), w = h * 7 / 5;
                if (w > mw * 9 / 10)
                    w = mw * 9 / 10;
                const Vector2 origin = GetMonitorPosition(monitor);
                SetWindowSize(w, h);
                SetWindowPosition(static_cast<int>(origin.x) + (mw - w) / 2, static_cast<int>(origin.y) + (mh - h) / 2);
            }
            SetWindowMinSize(820, 560);
        }
        if (!ps2_host_display::vsyncEnabled())
            SetTargetFPS(60);
        rlImGuiSetup(true);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        {
            std::error_code error;
            const fs::path font = context.resources / "fonts" / "Roboto-Regular.ttf";
            if (fs::is_regular_file(font, error))
                state.font = io.Fonts->AddFontFromFileTTF(utf8(font).c_str(), 20.0f);
        }
        applyStyle();
        std::printf("[STAR] launcher: shown (disc image %s)\n", state.disc.empty() ? "none" : state.discProblem.empty() ? "ready" : "not usable");
        std::fflush(stdout);
        // Tests: STAR_LAUNCHER_SHOT=<file.png> saves the window's picture at frame 30 and then quits, or plays with
        // STAR_LAUNCHER_PLAY=1; STAR_LAUNCHER_PAGE=1 opens Gameplay, 2 Experimental; STAR_LAUNCHER_SCROLL=<n> scrolls its list;
        // STAR_LAUNCHER_PRESET=<0-2, -1 = Custom> chooses a preset at frame 10; STAR_LAUNCHER_SET=<name>=<value> changes an
        // option then; STAR_LAUNCHER_CONTROLS=<tab 0-2, +10: Magic marked> opens the Controls screen at frame 3;
        // STAR_LAUNCHER_BIND=<action>=<input> puts an input into an action's first place at frame 10.
        const char *shot = std::getenv("STAR_LAUNCHER_SHOT");
        const bool testPlay = [] { const char *v = std::getenv("STAR_LAUNCHER_PLAY"); return v && *v == '1'; }();
        if (const char *v = std::getenv("STAR_LAUNCHER_PAGE"))
            state.testPage = std::atoi(v);
        if (const char *v = std::getenv("STAR_LAUNCHER_SCROLL"))
            state.testScroll = static_cast<float>(std::atof(v));
        if (const char *v = std::getenv("STAR_LAUNCHER_PRESET"); v && *v)
            state.testPreset = std::atoi(v);
        if (const char *v = std::getenv("STAR_LAUNCHER_SET"); v && std::strchr(v, '='))
            state.testSet = v;
        if (const char *v = std::getenv("STAR_LAUNCHER_CONTROLS"); v && *v)
            state.testControls = std::atoi(v);
        if (const char *v = std::getenv("STAR_LAUNCHER_BIND"); v && std::strchr(v, '='))
            state.testBind = v;
        // STAR_LAUNCHER_PAD=1 opens the controller set-up screen at frame 5 (2: and goes through every entry from frame
        // 8); STAR_LAUNCHER_JOY=<frame>:<kind>:<index>:<value>,... sets the test joystick's elements
        // (PS2X_TEST_JOYSTICK, ps2_host_gamepads.h) at those frames; STAR_LAUNCHER_SHOT_AT=<frame> takes the picture then.
        if (const char *v = std::getenv("STAR_LAUNCHER_PAD"); v && *v)
            state.testPad = std::atoi(v);
        if (const char *v = std::getenv("STAR_LAUNCHER_JOY"); v && *v)
            state.testJoy = v;
        int shotAt = 30;
        if (const char *v = std::getenv("STAR_LAUNCHER_SHOT_AT"); v && *v)
            shotAt = std::max(std::atoi(v), 12);
        int frame = 0;

        while (!state.done)
        {
            if (WindowShouldClose())
            {
                state.result = Result::Quit;
                break;
            }
            if (IsFileDropped())
            {
                FilePathList dropped = LoadDroppedFiles();
                if (dropped.count > 0)
                    useDisc(state, fromUtf8(dropped.paths[0]));
                UnloadDroppedFiles(dropped);
            }
            state.unit = std::clamp(static_cast<float>(GetScreenHeight()) / 790.0f, 0.75f, 2.0f);
            updateCapture(state);
            gp::update(); // (the controllers: SDL starts here, and the game keeps it)
            if (!state.testJoy.empty())
                for (const char *text = state.testJoy.c_str(); text && *text; text = std::strchr(text, ','), text = text ? text + 1 : nullptr)
                {
                    int at = 0, kind = 0, index = 0, value = 0;
                    if (std::sscanf(text, "%d:%d:%d:%d", &at, &kind, &index, &value) == 4 && at == frame)
                        gp::testJoystick(kind, index, value);
                }
            if (state.testPad > 0 && frame == 5 && !rawControllers().empty())
            {
                state.padWanted = true;
                state.padDevice = rawControllers().front().second;
            }
            if (state.testPad == 2 && frame == 8)
                startPadCapture(state, padmap::entries().front().id, true);
            updatePadCapture(state);
            BeginDrawing();
            ClearBackground(Color{18, 18, 20, 255});
            rlImGuiBegin();
            ImGui::PushFont(state.font, px(state, 19.0f));
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(state, 30.0f), px(state, 20.0f)));
            if (ImGui::Begin("##launcher", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse))
            {
                // left: the header, the disc image, the presets, Play; right: the options, the whole height
                const float side = std::clamp(std::floor(ImGui::GetContentRegionAvail().x * 0.34f), px(state, 330.0f), px(state, 460.0f));
                const float tall = ImGui::GetContentRegionAvail().y;
                if (ImGui::BeginChild("##side", ImVec2(side, tall), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                {
                    drawHeader(state);
                    drawDisc(state);
                    drawPresets(state);
                    drawButtons(state);
                }
                ImGui::EndChild();
                ImGui::SameLine(0.0f, px(state, 34.0f));
                if (ImGui::BeginChild("##options", ImVec2(0.0f, tall)))
                    drawOptions(state, ImGui::GetContentRegionAvail().y);
                ImGui::EndChild();
            }
            ImGui::End();
            ImGui::PopStyleVar();
            drawControlsScreen(state);
            drawPadScreen(state);
            ImGui::PopFont();
            rlImGuiEnd();
            ++frame;
            if (frame == 10 && state.testPreset >= -1 && state.testPreset < options::kPresetCount)
            {
                choosePreset(state, state.testPreset);
                std::printf("[STAR] launcher test: preset %d chosen; now %d (values match %d)\n", state.testPreset, state.chosen,
                            options::presetOf(state.values));
            }
            if (frame == 3 && state.testControls >= 0)
            {
                state.controlsWanted = true;
                state.controlsTab = std::clamp(state.testControls % 10, 0, 2);
                state.controlsFocus = state.testControls >= 10 ? "magic" : ""; // (1x: the Combat tab with Magic marked)
            }
            if (frame == 10 && !state.testBind.empty())
            {
                const size_t equals = state.testBind.find('=');
                const std::string id = state.testBind.substr(0, equals);
                state.controlsNote = bindings::assign(state.binds, id, 0, bindings::canonical(state.testBind.substr(equals + 1)));
                controlsChanged(state);
                std::printf("[STAR] launcher test: %s bound; note \"%s\"; preset now %d\n", state.testBind.c_str(), state.controlsNote.c_str(),
                            state.chosen);
            }
            if (frame == 10 && !state.testSet.empty())
            {
                const size_t equals = state.testSet.find('=');
                state.values[state.testSet.substr(0, equals)] = state.testSet.substr(equals + 1);
                optionChanged(state);
                std::printf("[STAR] launcher test: %s set; preset now %d\n", state.testSet.c_str(), state.chosen);
            }
            if (shot && *shot && frame == shotAt)
            {
                Image picture = LoadImageFromScreen();
                const bool saved = ExportImage(picture, shot);
                UnloadImage(picture);
                std::printf("[STAR] launcher test: picture %s %s\n", saved ? "saved to" : "NOT saved to", shot);
                std::fflush(stdout);
                if (testPlay)
                    save(state, true);
                state.result = testPlay && state.discProblem.empty() && !state.disc.empty() ? Result::Play : Result::Quit;
                state.done = true;
            }
            EndDrawing();
        }
        rlImGuiShutdown(); // the F12 debugger sets ImGui up again later
        std::printf("[STAR] launcher: %s\n", state.result == Result::Play ? "play" : "quit");
        std::fflush(stdout);
        if (state.result != Result::Play)
        {
            CloseWindow();
            return state.result;
        }
        // one empty frame, so that the window does not show the launcher until the game's first picture
        BeginDrawing();
        ClearBackground(BLACK);
        EndDrawing();
        disc = state.disc;
        return state.result;
    }
}
