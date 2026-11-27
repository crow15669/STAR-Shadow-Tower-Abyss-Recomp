#include "star/live_settings.h"
#include "star/options.h"
#include "star/bindings.h"

#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/ps2_game_menu.h"

#include "pcsx2gs_api.h"
#include "runtime/ps2_input_options.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern std::atomic<int> g_ps2xLastPicturesKeep; // ps2_runtime.cpp: how many F1 catches are kept (0 = all)
extern std::atomic<bool> g_ps2xDumpKeys;        // ps2_runtime.cpp: the developer's dump keys (F8, F10)

// STAR 2026-11-23: the Graphics page of the in-game menu (F2; the runtime's ps2_game_menu, drawn over the game's
// picture by PCSX2's GS). The user asked for a way to change the picture settings while the game runs, to compare
// them without restarting. Every change takes effect at once and is written back to "STAR settings.ini". The labels,
// notes and choices are the table's in star/options.cpp (the launcher shows the same).
namespace star
{
    LiveSettings &live()
    {
        static LiveSettings settings;
        return settings;
    }

    namespace
    {
        std::filesystem::path g_file;
        std::mutex g_fileMutex;
        std::string g_saved; // the line shown under the options after a change

        bool envOn(const char *name, bool fallback)
        {
            const char *value = std::getenv(name);
            if (!value || !*value)
                return fallback;
            return *value != '0';
        }

        // Rewrites the known names in the settings file, keeping every other line (the notes) as it is. A change made here
        // makes the player's own set (the launcher's Custom preset, as a change there does; the user's word, 2026-11-26).
        // Only called after a change.
        void save()
        {
            std::lock_guard<std::mutex> lock(g_fileMutex);
            if (g_file.empty())
                return;
            LiveSettings &s = live();
            std::vector<std::pair<std::string, std::string>> values = {
                {"resolution", std::to_string(s.resolution.load())},
                {"fxaa", s.fxaa.load() ? "on" : "off"},
                {"smooth_60fps", s.smooth60.load() ? "on" : "off"},
                {"smooth_pictures", s.pictureEvery.load() > 0 ? std::to_string(s.pictureEvery.load()) : std::string("auto")},
                {"flicker_filter", s.flickerFilter.load() ? "on" : "off"},
                {"glow", s.glow.load() ? "on" : "off"},
                {"sharp_shadows", s.sharpShadows.load() ? "on" : "off"},
                {"self_shadows", s.noSelfShadow.load() ? "off" : "on"},
                {"near_shadows", s.nearShadows.load() ? "on" : "off"},
                {"shadow_walls", s.shadowWalls.load() ? "on" : "off"},
                {"late_input", s.lateInput.load() ? "on" : "off"},
                {"record_pictures", s.recordPictures.load() ? "on" : "off"},
                {"record_keep", std::to_string(s.recordKeep.load())},
                {"text_fixes", s.textFixes.load() ? "on" : "off"},
                {"analog_aim", s.analogAim.load() ? "on" : "off"},
                {"aim_turn_speed", std::to_string(s.aimTurnPercent.load())},
                {"aim_look_speed", std::to_string(s.aimLookPercent.load())},
                {"mouse_keyboard", s.mouseKeyboard.load() ? "on" : "off"},
                {"mouse_sensitivity", std::to_string(s.mouseSensitivity.load())},
                {"mouse_invert_y", s.mouseInvertY.load() ? "on" : "off"},
                {"gun_button", s.gunHold.load() ? "hold" : "toggle"},
                {"melee_swings", s.meleeMode.load() == 2 ? "buttons" : s.meleeMode.load() == 1 ? "flick" : "direction"},
                {"mouse_slowdown", s.mouseSlowdown.load() == 2 ? "full" : s.mouseSlowdown.load() == 1 ? "soft" : "off"},
                {"mouse_live_view", s.liveLook.load() ? "on" : "off"},
                {"debug_menu", s.debugMenu.load() ? "on" : "off"},
            };
            const bindings::Binds binds = bindings::live();
            for (const bindings::Action &one : bindings::actions())
            {
                const auto found = binds.find(one.id);
                const bool none = found == binds.end() || found->second.empty();
                values.push_back({std::string("bind_") + one.id, none ? std::string("none") : bindings::encode(found->second)});
            }
            const options::Values current(values.begin(), values.end());
            values.push_back({"preset", options::presetId(-1)});
            values.push_back({"custom", options::encodeCustom(current)});
            const bool ok = options::writeFile(g_file, values);
            g_saved = ok ? "Saved to " + g_file.filename().string() : "Could not write " + g_file.string();
        }

        // The table's wording (star/options.cpp) for the widgets below.
        struct Page
        {
            const Pcsx2GsUi *ui;
            std::string section; // the last section heading drawn

            const options::Option &at(const char *key)
            {
                const options::Option &one = options::option(key);
                if (section != one.section)
                {
                    section = one.section;
                    ui->separator_text(one.section);
                }
                return one;
            }
            void help(const options::Option &one) const
            {
                if (one.help && *one.help)
                    ui->text_note(one.help);
            }
            bool toggle(const char *key, bool &on)
            {
                const options::Option &one = at(key);
                int value = on ? 1 : 0;
                const bool changed = ui->checkbox(one.label, &value) != 0;
                on = value != 0;
                help(one);
                return changed;
            }
            // index = the choice's place in the table; the help of the chosen one follows the option's own
            bool combo(const char *key, int &index, float width = 360.0f)
            {
                const options::Option &one = at(key);
                std::vector<const char *> labels;
                for (const options::Choice &choice : one.choices)
                    labels.push_back(choice.label);
                index = std::clamp(index, 0, static_cast<int>(labels.size()) - 1);
                ui->set_next_width(width);
                const bool changed = ui->combo(one.label, &index, labels.data(), static_cast<int>(labels.size())) != 0;
                help(one);
                if (const char *chosen = one.choices[static_cast<size_t>(index)].help)
                    ui->text_note(chosen);
                return changed;
            }
            // a slider (the table's range; "100% (PS2 Default)" at the mark), its warning under it while the value is above
            // the mark. The value follows the drag at once; letGo is set when the file should be written (let go after a
            // change), not at every step of a drag.
            bool slider(const char *key, int &value, bool &letGo)
            {
                static std::map<std::string, bool> dragged; // (the GS thread only)
                const options::Option &one = at(key);
                int number = std::clamp(value, one.slider.min, one.slider.max);
                const char *format = one.slider.formatAtMark && number == one.slider.mark ? one.slider.formatAtMark : one.slider.format;
                ui->set_next_width(320.0f);
                const bool changed = ui->slider_int(one.label, &number, one.slider.min, one.slider.max, format) != 0;
                const bool active = ui->is_item_active() != 0;
                help(one);
                if (changed)
                {
                    value = number;
                    dragged[key] = true;
                }
                if (one.slider.warning && value > one.slider.mark)
                    ui->text_colored(0xFF29BFE8u, one.slider.warning); // (the launcher's amber)
                letGo = dragged[key] && !active;
                if (letGo)
                    dragged[key] = false;
                return changed;
            }
        };

        void savedLine(const Pcsx2GsUi *ui)
        {
            ui->spacing();
            std::string saved;
            {
                std::lock_guard<std::mutex> lock(g_fileMutex);
                saved = g_saved;
            }
            ui->text_disabled(saved.empty() ? "Changes apply at once and are kept for next time." : saved.c_str());
        }

        // GS thread, while the menu is open.
        void drawGraphics(const Pcsx2GsUi *ui)
        {
            LiveSettings &s = live();
            bool changed = false;
            Page page{ui, {}};

            int resolution = s.resolution.load() - 1;
            if (resolution < 0 || resolution > 5)
                resolution = 0;
            if (page.combo("resolution", resolution, 320.0f))
            {
                s.resolution.store(resolution + 1);
                GSPcsx2Backend::SetScale(static_cast<float>(resolution + 1));
                changed = true;
            }
            bool fxaa = s.fxaa.load();
            if (page.toggle("fxaa", fxaa))
            {
                s.fxaa.store(fxaa);
                GSPcsx2Backend::SetOption("fxaa", fxaa ? "1" : "0");
                changed = true;
            }
            int framerate = s.smooth60.load() ? 1 : 0; // (30fps, Unlocked)
            if (page.combo("smooth_60fps", framerate, 320.0f))
            {
                s.smooth60.store(framerate == 1);
                changed = true;
            }
            if (s.smooth60.load()) // (In-between pictures: only under Unlocked, as in the launcher)
            {
                int every = std::clamp(s.pictureEvery.load(), 0, 4); // (0 = auto: the table's first choice)
                if (page.combo("smooth_pictures", every, 360.0f))
                {
                    s.pictureEvery.store(every);
                    changed = true;
                }
            }
            {
                char now[160];
                const int hz = s.screenHz100.load(), pictures = s.pictures100.load();
                if (hz > 0 && s.smooth60.load())
                    std::snprintf(now, sizeof(now), "Now: %.0f pictures a second on a %.0f Hz screen (in 3D scenes).", pictures / 100.0, hz / 100.0);
                else
                    std::snprintf(now, sizeof(now), "Now: the game's own 30 pictures a second.");
                ui->text_disabled(now);
            }

            bool shadows = s.sharpShadows.load();
            if (page.toggle("sharp_shadows", shadows))
            {
                s.sharpShadows.store(shadows);
                GSPcsx2Backend::SetOption("target_boost", shadows ? "2.5" : "1");
                changed = true;
            }
            bool near = s.nearShadows.load();
            if (page.toggle("near_shadows", near))
            {
                s.nearShadows.store(near);
                changed = true;
            }
            int self = s.noSelfShadow.load() ? 1 : 0; // (the table's choices: "on" = as the game draws them, "off" = none)
            if (page.combo("self_shadows", self, 320.0f))
            {
                s.noSelfShadow.store(self == 1);
                GSPcsx2Backend::SetOption("no_self_shadow", self == 1 ? "1" : "0");
                changed = true;
            }
            bool walls = s.shadowWalls.load();
            if (page.toggle("shadow_walls", walls))
            {
                s.shadowWalls.store(walls);
                GSPcsx2Backend::SetOption("shadow_walls", walls ? "1" : "0");
                changed = true;
            }

            bool filter = s.flickerFilter.load();
            if (page.toggle("flicker_filter", filter))
            {
                s.flickerFilter.store(filter);
                changed = true;
            }
            bool glow = s.glow.load();
            if (page.toggle("glow", glow))
            {
                s.glow.store(glow);
                changed = true;
            }

            ui->separator_text("Testing (not saved)");
            int objects = s.moveObjects.load() ? 1 : 0;
            if (ui->checkbox("In-between pictures move objects too", &objects))
                s.moveObjects.store(objects != 0);
            ui->text_note("Off: only the camera moves in the in-between pictures.");

            // How pictures are handed to the screen (the renderer's "vsync" option: 2 = the newest picture at each
            // refresh, 0 = at once, which lets a G-SYNC / FreeSync screen follow the game's 60 a second, 1 = wait).
            static const char *const kSync[] = {"Newest picture at each refresh (default)", "At once (G-SYNC / FreeSync; tears on other screens)",
                                                "Wait for each refresh"};
            static const char *const kSyncValue[] = {"2", "0", "1"};
            static int sync = 0;
            ui->set_next_width(420.0f);
            if (ui->combo("Screen sync", &sync, kSync, 3))
                GSPcsx2Backend::SetOption("vsync", kSyncValue[sync]);
            static int area = 1;
            if (ui->checkbox("Downscale by averaging", &area))
                GSPcsx2Backend::SetOption("present_area", area ? "1" : "0");
            ui->text_note("When the picture is larger than the window. Off: one bilinear sample, as in PCSX2.");
            // Texture filtering of the renderer (PCSX2's): the game itself uses plain bilinear without mip levels.
            static int trilinear = 0, anisotropic = 0;
            if (ui->checkbox("Trilinear filtering (forced mipmaps)", &trilinear))
                GSPcsx2Backend::SetOption("trilinear", trilinear ? "2" : "-1");
            if (ui->checkbox("16x anisotropic filtering", &anisotropic))
                GSPcsx2Backend::SetOption("anisotropy", anisotropic ? "16" : "0");

            if (changed)
                save();
            savedLine(ui);
        }
    }

    namespace
    {
        // STAR 2026-11-24: the Gameplay page, for tweaks to how the game plays.
        void drawGameplay(const Pcsx2GsUi *ui)
        {
            LiveSettings &s = live();
            bool changed = false;
            Page page{ui, {}};

            bool text = s.textFixes.load();
            if (page.toggle("text_fixes", text))
            {
                s.textFixes.store(text);
                changed = true;
            }
            bool late = s.lateInput.load();
            if (page.toggle("late_input", late))
            {
                s.lateInput.store(late);
                changed = true;
            }
            bool aim = s.analogAim.load();
            if (page.toggle("analog_aim", aim))
            {
                s.analogAim.store(aim);
                changed = true;
            }
            bool letGo = false;
            int turn = s.aimTurnPercent.load();
            if (page.slider("aim_turn_speed", turn, letGo))
                s.aimTurnPercent.store(turn);
            changed = changed || letGo;
            int look = s.aimLookPercent.load();
            if (page.slider("aim_look_speed", look, letGo))
                s.aimLookPercent.store(look);
            changed = changed || letGo;

            if (changed)
                save();
            savedLine(ui);
        }

        std::string swingsName()
        {
            const int mode = live().meleeMode.load();
            return mode == 2 ? "buttons" : mode == 1 ? "flick" : "direction";
        }

        // STAR 2026-11-26: the Controls screen of the F2 menu (the launcher's, star/launcher.cpp drawControlsScreen): every
        // action in tabs with its keys; a click takes the next key, mouse button or wheel turn (the runtime's capture,
        // which holds the game's input meanwhile). Each change applies at once (bindings::setLive) and is saved.
        struct ControlsScreen
        {
            bool open = false;
            bool capturing = false;
            std::string id;   // the action whose input is being taken
            size_t slot = 0;
            std::string note; // what the last change took from another action
        } g_controls;

        // "Only with Sword swings: ..." for a swing of another style (as the launcher's)
        std::string stylesText(const char *styles)
        {
            std::string text, list = styles ? styles : "";
            size_t at = 0;
            while (at <= list.size())
            {
                const size_t bar = list.find('|', at);
                const std::string one = list.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
                text += (text.empty() ? "" : " or ") + std::string(options::choiceLabel(options::option("melee_swings"), one));
                if (bar == std::string::npos)
                    break;
                at = bar + 1;
            }
            return "Only with Sword swings: " + text + ".";
        }

        void controlsChanged(const bindings::Binds &binds)
        {
            bindings::setLive(binds, swingsName(), true);
            save();
        }

        void drawControls(const Pcsx2GsUi *ui)
        {
            ControlsScreen &c = g_controls;
            if (c.capturing)
            {
                std::string binding;
                const int result = ps2_host_input::bindCaptureResult(binding);
                if (result == 1)
                {
                    if (const std::string input = bindings::fromRuntimeBinding(binding); !input.empty())
                    {
                        bindings::Binds binds = bindings::live();
                        c.note = bindings::assign(binds, c.id, c.slot, input);
                        controlsChanged(binds);
                    }
                    c.capturing = false;
                }
                else if (result == 2)
                    c.capturing = false;
            }
            ui->heading("Controls");
            if (c.capturing)
            {
                // nothing to click while it listens: a click is the answer
                const bindings::Action *taking = bindings::action(c.id);
                ui->text_colored(0xFF29BFE8u, ("Press a key or mouse button (or turn the wheel) for " + std::string(taking ? taking->label : "") +
                                               ". Esc cancels.").c_str());
                return;
            }
            if (ui->button("< Back"))
            {
                c.open = false;
                return;
            }
            ui->text_note("Click a key to change it, then press the new key or mouse button (Esc cancels). x takes one away, + adds "
                          "another, up to four. The mouse always looks around.");
            if (!c.note.empty())
                ui->text_colored(0xFF29BFE8u, c.note.c_str());
            const std::string swings = swingsName();
            bindings::Binds binds = bindings::live();
            bool changed = false, start = false;
            if (ui->begin_tab_bar("##controlTabs"))
            {
                const char *names[] = {"Movement", "Combat", "Menus"};
                for (int tab = 0; tab < 3; ++tab)
                {
                    if (!ui->begin_tab(names[tab]))
                        continue;
                    if (ui->begin_table("##actions", 2, 250.0f))
                    {
                        int row = 0;
                        for (const bindings::Action &one : bindings::actions())
                        {
                            if (static_cast<int>(one.tab) != tab)
                                continue;
                            const bool usable = bindings::inUse(one, swings);
                            ui->table_next_row();
                            ui->table_next_column();
                            if (usable)
                                ui->text_wrapped(one.label);
                            else
                                ui->text_disabled(one.label);
                            if (one.note && *one.note)
                                ui->text_note(one.note);
                            if (!usable)
                                ui->text_note(stylesText(one.style).c_str());
                            ui->table_next_column();
                            ui->push_id(1000 + row++);
                            ui->begin_disabled(usable ? 0 : 1);
                            std::vector<std::string> &inputs = binds[one.id];
                            for (size_t i = 0; i < inputs.size(); ++i)
                            {
                                ui->push_id(static_cast<int>(i));
                                if (i > 0)
                                    ui->same_line();
                                if (ui->button(bindings::displayName(inputs[i]).c_str()))
                                {
                                    c.id = one.id;
                                    c.slot = i;
                                    start = true;
                                }
                                ui->same_line();
                                if (ui->small_button("x"))
                                {
                                    bindings::remove(binds, one.id, i);
                                    c.note.clear();
                                    changed = true;
                                    ui->pop_id();
                                    break;
                                }
                                ui->pop_id();
                            }
                            if (!changed && inputs.size() < bindings::kMaxInputs)
                            {
                                if (!inputs.empty())
                                    ui->same_line();
                                if (ui->button("+##add"))
                                {
                                    c.id = one.id;
                                    c.slot = inputs.size();
                                    start = true;
                                }
                            }
                            ui->end_disabled();
                            ui->pop_id();
                        }
                        ui->end_table();
                    }
                    ui->end_tab();
                }
                ui->end_tab_bar();
            }
            if (start && !changed)
            {
                ps2_host_input::beginBindCapture();
                c.capturing = true;
                c.note.clear();
            }
            ui->spacing();
            if (ui->button("Reset to defaults"))
            {
                binds = bindings::defaults();
                c.note = "Every control is back to its default.";
                changed = true;
            }
            if (changed)
            {
                c.id.clear();
                controlsChanged(binds);
            }
        }

        // STAR 2026-11-26: the Experimental page (the user's: the launcher's Experimental tab, in the same order): mouse
        // and keyboard play, the Debug page, the F1 recorder; the user's note at its top.
        void drawExperimental(const Pcsx2GsUi *ui)
        {
            if (g_controls.open)
            {
                drawControls(ui); // the Controls screen in the page's place
                return;
            }
            LiveSettings &s = live();
            bool changed = false;
            Page page{ui, {}};
            ui->text_colored(0xFF29BFE8u, options::pageNote(options::Page::Experimental));

            bool keys = s.mouseKeyboard.load();
            if (page.toggle("mouse_keyboard", keys))
            {
                s.mouseKeyboard.store(keys);
                applyMouseKeyboard();
                changed = true;
            }
            {
                // the controls as a list (as in the launcher): the player's, for the sword swings chosen
                const std::string swings = swingsName();
                const bindings::Binds binds = bindings::live();
                std::vector<std::string> keyTexts{"Mouse"};
                std::vector<const char *> keyNames, actions{"Look around"};
                for (const bindings::Action &one : bindings::actions())
                    if (bindings::inUse(one, swings))
                    {
                        std::string text;
                        if (const auto found = binds.find(one.id); found != binds.end())
                            for (const std::string &input : found->second)
                                text += (text.empty() ? "" : " / ") + bindings::displayName(input);
                        keyTexts.push_back(text.empty() ? "(none)" : text);
                        actions.push_back(one.label);
                    }
                for (const std::string &text : keyTexts)
                    keyNames.push_back(text.c_str());
                ui->key_table(keyNames.data(), actions.data(), static_cast<int>(keyNames.size()));
                if (ui->button("Change controls..."))
                {
                    g_controls.open = true;
                    g_controls.note.clear();
                }
                ui->spacing();
            }
            bool letGo = false;
            int sens = s.mouseSensitivity.load();
            if (page.slider("mouse_sensitivity", sens, letGo))
                s.mouseSensitivity.store(sens);
            changed = changed || letGo;
            bool liveView = s.liveLook.load();
            if (page.toggle("mouse_live_view", liveView))
            {
                s.liveLook.store(liveView);
                changed = true;
            }
            bool invert = s.mouseInvertY.load();
            if (page.toggle("mouse_invert_y", invert))
            {
                s.mouseInvertY.store(invert);
                changed = true;
            }
            int gun = s.gunHold.load() ? 1 : 0; // (toggle, hold)
            if (page.combo("gun_button", gun, 430.0f)) // ("Toggle: press to aim, press again to holster")
            {
                s.gunHold.store(gun != 0);
                changed = true;
            }
            int melee = std::clamp(s.meleeMode.load(), 0, 2); // (direction, flick, buttons)
            if (page.combo("melee_swings", melee))
            {
                s.meleeMode.store(melee);
                applyMouseKeyboard();
                changed = true;
            }
            int slow = std::clamp(s.mouseSlowdown.load(), 0, 2); // (off, soft, full; full without a line, since 2026-11-26)
            if (page.combo("mouse_slowdown", slow))
            {
                s.mouseSlowdown.store(slow);
                changed = true;
            }

            // read at start-up (debug_menu.cpp adds its page then): written for the next start
            bool debug = s.debugMenu.load();
            if (page.toggle("debug_menu", debug))
            {
                s.debugMenu.store(debug);
                changed = true;
            }
            ui->text_note("Takes effect the next time STAR starts.");

            bool record = s.recordPictures.load();
            if (page.toggle("record_pictures", record))
            {
                s.recordPictures.store(record);
                GSPcsx2Backend::SetOption("ring", record ? "1" : "0");
                changed = true;
            }
            {
                const options::Option &keepOption = options::option("record_keep");
                int keep = options::choiceIndex(keepOption, std::to_string(s.recordKeep.load()));
                if (keep < 0)
                    keep = options::choiceIndex(keepOption, keepOption.fallback);
                if (page.combo("record_keep", keep, 320.0f))
                {
                    s.recordKeep.store(std::atoi(keepOption.choices[static_cast<size_t>(keep)].value));
                    g_ps2xLastPicturesKeep.store(s.recordKeep.load());
                    changed = true;
                }
            }

            if (changed)
                save();
            savedLine(ui);
        }
    }

    void installSettingsMenu(const std::filesystem::path &settingsFile)
    {
        LiveSettings &s = live();
        g_file = settingsFile;
        if (const char *scale = std::getenv("PS2X_GS_SCALE"); scale && *scale)
        {
            const int value = std::atoi(scale);
            s.resolution.store(value >= 1 && value <= 8 ? value : 1);
        }
        s.fxaa.store(envOn("PS2X_PCSX2_FXAA", false));
        s.smooth60.store(envOn("STAR_60FPS", true));
        s.flickerFilter.store(envOn("STAR_FLICKER_FILTER", false));
        s.glow.store(envOn("STAR_GLOW", true));
        s.sharpShadows.store(envOn("STAR_SHARP_SHADOWS", false));
        {
            const char *self = std::getenv("STAR_SELF_SHADOWS");
            const std::string selfValue = self ? self : "";
            s.noSelfShadow.store(selfValue == "0" || selfValue == "off");
        }
        s.lateInput.store(envOn("STAR_LATE_INPUT", false));
        s.nearShadows.store(envOn("STAR_NEAR_SHADOWS", false));
        s.shadowWalls.store(envOn("STAR_SHADOW_WALLS", false));
        s.moveObjects.store(envOn("STAR_60FPS_OBJECTS", true));
        if (const char *every = std::getenv("STAR_SMOOTH_EVERY"); every && *every)
            s.pictureEvery.store(std::clamp(std::atoi(every), 0, 4));
        ps2x::menu::setTitle("STAR: Shadow Tower Abyss", "F2 closes this menu. The game keeps running behind it.");
        s.recordPictures.store(envOn("PS2X_PCSX2_RING", false));
        if (const char *keep = std::getenv("STAR_RECORD_KEEP"); keep && *keep)
            s.recordKeep.store(std::clamp(std::atoi(keep), 0, 999));
        g_ps2xLastPicturesKeep.store(s.recordKeep.load());
        s.textFixes.store(envOn("STAR_TEXT_FIXES", true));
        s.analogAim.store(envOn("STAR_ANALOG_AIM", false));
        if (const char *turn = std::getenv("STAR_AIM_TURN"); turn && *turn)
            s.aimTurnPercent.store(std::clamp(std::atoi(turn), 25, 400));
        if (const char *look = std::getenv("STAR_AIM_LOOK"); look && *look)
            s.aimLookPercent.store(std::clamp(std::atoi(look), 25, 400));
        s.mouseKeyboard.store(envOn("STAR_MOUSE_KEYBOARD", false));
        if (const char *sens = std::getenv("STAR_MOUSE_SENSITIVITY"); sens && *sens)
            s.mouseSensitivity.store(std::clamp(std::atoi(sens), 10, 1000));
        s.mouseInvertY.store(envOn("STAR_MOUSE_INVERT_Y", false));
        s.gunHold.store(envOn("STAR_GUN_HOLD", false));
        {
            const char *melee = std::getenv("STAR_MELEE_SWINGS");
            const std::string value = melee ? melee : "";
            // (buttons without a line, since 2026-11-26: the user's "recommended" one)
            s.meleeMode.store(value == "buttons" ? 2 : value == "flick" ? 1 : value == "direction" ? 0 : envOn("STAR_COMBAT_MODE", false) ? 1 : 2);
            const char *slow = std::getenv("STAR_MOUSE_SLOWDOWN");
            const std::string slowValue = slow ? slow : "";
            s.mouseSlowdown.store(slowValue == "soft" ? 1 : slowValue == "off" ? 0 : 2); // (full unless set: the user's, 2026-11-26)
        }
        s.liveLook.store(envOn("STAR_LIVE_LOOK", true));
        s.debugMenu.store(envOn("STAR_DEBUG_MENU", false));
        // the developer's dump keys only with the Debug page (the user, 2026-11-27: off for players, said under the setting)
        g_ps2xDumpKeys.store(s.debugMenu.load());
        ps2x::menu::addPanel("Graphics", &drawGraphics);
        ps2x::menu::addPanel("Gameplay", &drawGameplay);
        ps2x::menu::addPanel("Experimental", &drawExperimental);
        // STAR_MENU_PAGE_FIRST=<page> (tests): that page as the first tab ("Gameplay", "Experimental"), so that a run
        // which opens the menu by itself (PS2X_GAME_MENU_OPEN_AT) draws it
        if (const char *first = std::getenv("STAR_MENU_PAGE_FIRST"); first && *first)
            ps2x::menu::setTabOrder({first});
        // STAR_F2_CONTROLS=1 (tests): the Experimental page opens on its Controls screen
        if (const char *v = std::getenv("STAR_F2_CONTROLS"); v && *v == '1')
            g_controls.open = true;
    }
}
