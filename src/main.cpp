// STAR.exe: Shadow Tower Abyss (SLPS-25217, English-patched) start-up.
//
// Checks the player's disc image (the boot program's SHA-256 must match the one the code was recompiled from),
// points the runtime at it, applies the runtime defaults the God Hand: Rearmed / Urban Reign / SotC ports ship with, and
// runs the recompiled game. Modelled on GHR's src/main.cpp, without GHR's game-specific parts.
//
// usage: STAR [--iso <disc image>] [--hidden] [--seconds <n>]
//   --iso       the disc image (.iso or .chd), straight into the game; without it the launcher shows first (the disc
//               image of the settings file, else the STAR_ISO environment variable); a hidden run takes STAR_ISO
//   --hidden    no visible window, no sound (test runs)
//   --seconds   stop after n seconds (test runs; exit code 124)
// exit codes: 0 ok, 1 init failed, 2 no disc, 3 wrong disc, 124 time limit

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_async_vif.h"
#include "runtime/ps2_cd_image.h"
#include "runtime/ps2_save_state.h"
#if defined(_WIN32)
// Declared here instead of including <windows.h> (its names clash with the window library's).
extern "C" __declspec(dllimport) void *__stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void *process, unsigned int exitCode);
#endif
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "star/game_fixes.h"
#include "star/frame_double.h"
#include "star/host_sampler.h"
#include "star/live_settings.h"
#include "star/debug_menu.h"
#include "star/window_watch.h"
#include "star/sha256.h"
#include "star/options.h"
#include "star/bindings.h"
#if defined(STAR_HAS_LAUNCHER)
#include "star/launcher.h"
#endif
#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif
#include "raylib.h"

#include <xmmintrin.h>
#include <pmmintrin.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#if defined(STAR_HAS_VU1_PROGRAMS)
void registerGeneratedVu1Programs(); // generated/vu1/vu1_programs.cpp (VU1 microcode compiled ahead of time)
#endif

namespace
{
    constexpr const char *kTitle = "STAR: Shadow Tower Abyss";
    constexpr const char *kSerial = "SLPS-25217";
    constexpr const char *kBootElf = "SLPS_252.17";
    constexpr const char *kBootCnfLine = "cdrom0:\\SLPS_252.17;1";
    // SLPS_252.17 of the Japanese disc with the 2011 English patch (NOTES.md).
    constexpr const char *kBootElfSha256 = "3c2097cd15ae6957ce7dfd5c424a073156ac22eb9ef21a744dc2f34ded12c6d8";

    constexpr int kExitOk = 0;
    constexpr int kExitInitFailed = 1;
    constexpr int kExitNoDisc = 2;
    constexpr int kExitBadDisc = 3;
    constexpr int kExitTimeLimit = 124;

    std::atomic<PS2Runtime *> g_runtime{nullptr};
    PS2Runtime::RecompiledFunction g_originalEntry = nullptr;
    // The main game thread's registers (set when the entry function starts): its pc is read by the guest sampler.
    std::atomic<const R5900Context *> g_gameContext{nullptr};

    void say(const std::string &line)
    {
        std::cout << "[STAR] " << line << std::endl;
    }

    void setDefaultEnv(const char *name, const char *value)
    {
        if (!std::getenv(name))
        {
#if defined(_WIN32)
            _putenv_s(name, value);
#else
            setenv(name, value, 0);
#endif
        }
    }

    // STAR 2026-11-23: "STAR settings.ini" (next to the program, or in the project folder two levels up): lines of
    // name = value, '#' or ';' starts a comment. Each known name sets the variable the program reads, unless that
    // variable is already set (a launcher or a test wins over the file).
    fs::path g_settingsFile; // where findSettingsFile found the settings (the launcher and the F2 menu write there)

    void findSettingsFile(const fs::path &exeDir)
    {
        std::error_code error;
        g_settingsFile.clear();
        for (const fs::path &candidate : {exeDir / "STAR settings.ini", exeDir.parent_path().parent_path() / "STAR settings.ini"})
            if (g_settingsFile.empty() && fs::exists(candidate, error))
                g_settingsFile = candidate;
        if (g_settingsFile.empty())
            g_settingsFile = exeDir / "STAR settings.ini"; // none yet: the launcher's or the menu's first change makes it
        // STAR_SETTINGS=<file> (tests): another settings file, so that a test never reads or writes the player's
        if (const char *other = std::getenv("STAR_SETTINGS"); other && *other)
            g_settingsFile = other;
    }

    // STAR 2026-11-26: an option without a line in the file (or no file at all) takes the table's fallback, the STAR
    // preset's value (star/options.cpp), so that a fresh install plays as the launcher shows it.
    void loadSettings()
    {
        const fs::path file = g_settingsFile;
        std::error_code error;
        const bool haveFile = !file.empty() && fs::exists(file, error);
        std::ifstream in;
        if (haveFile)
            in.open(file);
        const auto trim = [](std::string text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n");
            const size_t last = text.find_last_not_of(" \t\r\n");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        };
        const auto onOff = [](const std::string &value) -> const char *
        {
            if (value == "on" || value == "yes" || value == "1" || value == "true")
                return "1";
            if (value == "off" || value == "no" || value == "0" || value == "false")
                return "0";
            return nullptr;
        };
        std::vector<std::pair<std::string, std::string>> entries;
        std::string line, applied, fallbacks;
        while (haveFile && std::getline(in, line))
        {
            const size_t comment = line.find_first_of("#;");
            if (comment != std::string::npos)
                line.erase(comment);
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            std::string name = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
            for (char &c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char &c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name == "disc_image" || name == "custom" || name == "preset")
                continue; // the launcher's (the disc image to show, the player's own preset, the preset chosen)
            if (name.rfind("bind_", 0) == 0)
                continue; // the mouse and keyboard controls (star/bindings.h: read for the bindings below)
            entries.push_back({name, value});
        }
        for (const star::options::Option &one : star::options::table())
            if (std::none_of(entries.begin(), entries.end(), [&](const auto &entry) { return entry.first == one.key && !entry.second.empty(); }))
            {
                entries.push_back({one.key, one.fallback});
                fallbacks += (fallbacks.empty() ? "" : ", ") + std::string(one.key) + " = " + one.fallback;
            }
        for (const auto &[name, value] : entries)
        {
            const char *variable = nullptr;
            std::string setting;
            if (name == "smooth_pictures")
            {
                // auto, or 1-4: an in-between picture for every n-th screen refresh
                const int every = value == "auto" ? 0 : std::atoi(value.c_str());
                if (value == "auto" || (every >= 1 && every <= 4))
                {
                    variable = "STAR_SMOOTH_EVERY";
                    setting = std::to_string(every);
                }
            }
            else if (name == "aim_turn_speed" || name == "aim_look_speed")
            {
                // percent of the game's own top speed
                const int percent = std::atoi(value.c_str());
                if (percent >= 25 && percent <= 400)
                {
                    variable = name == "aim_turn_speed" ? "STAR_AIM_TURN" : "STAR_AIM_LOOK";
                    setting = std::to_string(percent);
                }
            }
            else if (name == "record_keep")
            {
                // how many F1 catches (logs/pictures_<n>, about 250 MB each) are kept; 0 = all
                const bool number = !value.empty() && value.find_first_not_of("0123456789") == std::string::npos;
                if (number && std::atoi(value.c_str()) <= 999)
                {
                    variable = "STAR_RECORD_KEEP";
                    setting = value;
                }
            }
            else if (name == "mouse_sensitivity")
            {
                const int percent = std::atoi(value.c_str());
                if (percent >= 10 && percent <= 1000)
                {
                    variable = "STAR_MOUSE_SENSITIVITY";
                    setting = std::to_string(percent);
                }
            }
            else if (name == "melee_swings")
            {
                // direction: the left button swings the way the view last moved; flick: holding the right button
                // locks the view and a flick swings; buttons: left, right, wheel up and wheel down are the four swings
                if (value == "direction" || value == "flick" || value == "buttons")
                {
                    variable = "STAR_MELEE_SWINGS";
                    setting = value;
                }
            }
            else if (name == "self_shadows")
            {
                // on (as the game draws them) / off (none). ("clean", lit side only, was taken out on 2026-11-25 at the
                // user's word: an old ini's "clean" is read as off, the nearer of the two.)
                if (value == "on" || value == "off" || value == "clean")
                {
                    variable = "STAR_SELF_SHADOWS";
                    setting = value == "on" ? "1" : "0";
                }
            }
            else if (name == "mouse_slowdown")
            {
                if (value == "off" || value == "soft" || value == "full")
                {
                    variable = "STAR_MOUSE_SLOWDOWN";
                    setting = value;
                }
            }
            else if (name == "gun_button")
            {
                // toggle: the right button draws a gun and puts it away; hold: the gun is out while it is down
                if (value == "toggle" || value == "hold")
                {
                    variable = "STAR_GUN_HOLD";
                    setting = value == "hold" ? "1" : "0";
                }
            }
            else if (name == "resolution")
            {
                const int scale = std::atoi(value.c_str());
                if (scale >= 1 && scale <= 8)
                {
                    variable = "PS2X_GS_SCALE";
                    setting = std::to_string(scale);
                }
            }
            else if (const char *flag = onOff(value))
            {
                if (name == "fxaa")
                    variable = "PS2X_PCSX2_FXAA";
                else if (name == "smooth_60fps")
                    variable = "STAR_60FPS";
                else if (name == "flicker_filter")
                    variable = "STAR_FLICKER_FILTER";
                else if (name == "glow")
                    variable = "STAR_GLOW";
                else if (name == "sharp_shadows")
                    variable = "STAR_SHARP_SHADOWS";
                else if (name == "near_shadows")
                    variable = "STAR_NEAR_SHADOWS";
                else if (name == "shadow_walls")
                    variable = "STAR_SHADOW_WALLS";
                else if (name == "late_input")
                    variable = "STAR_LATE_INPUT";
                else if (name == "analog_aim")
                    variable = "STAR_ANALOG_AIM";
                else if (name == "text_fixes")
                    variable = "STAR_TEXT_FIXES";
                else if (name == "mouse_keyboard")
                    variable = "STAR_MOUSE_KEYBOARD";
                else if (name == "mouse_invert_y")
                    variable = "STAR_MOUSE_INVERT_Y";
                else if (name == "combat_mode")
                    variable = "STAR_COMBAT_MODE";
                else if (name == "mouse_live_view")
                    variable = "STAR_LIVE_LOOK";
                else if (name == "debug_menu")
                    variable = "STAR_DEBUG_MENU";
                else if (name == "record_pictures")
                    variable = "PS2X_PCSX2_RING";
                setting = flag;
            }
            if (!variable)
            {
                if (!name.empty())
                    say("settings: \"" + name + " = " + value + "\" is not understood and was left out");
                continue;
            }
            setDefaultEnv(variable, setting.c_str());
            applied += (applied.empty() ? "" : ", ") + name + " = " + value;
        }
        say("settings from " + (haveFile ? file.string() : std::string("(no file)")) + ": " + (applied.empty() ? "nothing" : applied));
        if (!fallbacks.empty())
            say("settings without a line (the STAR preset's): " + fallbacks);
    }

    bool readWholeFile(PS2CdImage &image, const char *name, std::vector<uint8_t> &out)
    {
        PS2CdImage::Entry entry{};
        if (!image.find(name, entry) || entry.directory)
            return false;
        out.resize(static_cast<size_t>(entry.size));
        return out.empty() || image.read(static_cast<uint64_t>(entry.lba) * PS2CdImage::kSectorSize, out.data(), out.size());
    }

    // Opens the disc image and checks SYSTEM.CNF and the boot program's hash: "" when it is the disc STAR is made for,
    // else what is wrong, in the player's words (the launcher shows it). The program is left in elf.
    std::string checkDisc(const fs::path &iso, std::vector<uint8_t> &elf, int &code)
    {
        PS2CdImage &image = PS2CdImage::instance();
        code = kExitBadDisc;
        if (!image.open(iso))
        {
            code = kExitNoDisc;
            return "This file cannot be read as a disc image (ISO or CHD).";
        }
        std::vector<uint8_t> cnf;
        if (!readWholeFile(image, "SYSTEM.CNF", cnf))
            return "This is not a PlayStation 2 game disc (it has no SYSTEM.CNF).";
        const std::string cnfText(cnf.begin(), cnf.end());
        if (cnfText.find(kBootCnfLine) == std::string::npos)
            return std::string("This is another game's disc, not Shadow Tower Abyss (") + kSerial + ").";
        if (!readWholeFile(image, kBootElf, elf))
            return std::string("This disc image has no ") + kBootElf + " (its program).";
        const std::string hash = star::Sha256::hex(elf.data(), elf.size());
        if (hash != kBootElfSha256)
        {
            say(std::string(kBootElf) + " sha256 " + hash + " differs from the English-patched program STAR was built from (" +
                kBootElfSha256 + ")");
            return "This is Shadow Tower Abyss, but not with the English patch STAR is made for (its program differs).";
        }
        say(std::string(kBootElf) + " sha256 " + hash + " (matches)");
        code = kExitOk;
        return std::string();
    }

    // checkDisc, then the program written to cacheDir.
    int prepareDisc(const fs::path &iso, const fs::path &cacheDir, fs::path &bootElfPath)
    {
        std::vector<uint8_t> elf;
        int code = kExitOk;
        if (const std::string problem = checkDisc(iso, elf, code); !problem.empty())
        {
            say(iso.string() + ": " + problem);
            return code;
        }
        std::error_code error;
        fs::create_directories(cacheDir, error);
        bootElfPath = cacheDir / kBootElf;
        std::ofstream out(bootElfPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(elf.data()), static_cast<std::streamsize>(elf.size()));
        if (!out)
        {
            say("cannot write " + bootElfPath.string());
            return kExitInitFailed;
        }
        return kExitOk;
    }

    // The EE's float behaviour on the game thread: round toward zero, denormals flushed (as GHR does at boot).
    void bootEntryWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        _MM_SET_ROUNDING_MODE(_MM_ROUND_TOWARD_ZERO);
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
        say("game thread started: EE FPU mode set (round toward zero, denormals flushed)");
        g_gameContext.store(ctx, std::memory_order_release);
        g_originalEntry(rdram, ctx, runtime);
    }

    const char *statusName(EeThreadStatus status)
    {
        switch (status)
        {
        case EeThreadStatus::Running: return "running";
        case EeThreadStatus::Ready: return "ready";
        case EeThreadStatus::Waiting: return "waiting";
        case EeThreadStatus::WaitingSuspended: return "wait+susp";
        case EeThreadStatus::Suspended: return "suspended";
        case EeThreadStatus::Dormant: return "dormant";
        }
        return "?";
    }

    const char *waitName(EeWaitReason reason)
    {
        switch (reason)
        {
        case EeWaitReason::None: return "-";
        case EeWaitReason::Sleep: return "sleep";
        case EeWaitReason::Semaphore: return "sema";
        case EeWaitReason::EventFlag: return "evflag";
        case EeWaitReason::VSync: return "vsync";
        case EeWaitReason::External: return "external";
        case EeWaitReason::Mpeg: return "mpeg";
        }
        return "?";
    }

    // Every guest thread: status, what it waits on, where it is (test runs: every 5 s and at the time limit).
    void dumpThreads(const char *why)
    {
        PS2Runtime *runtime = g_runtime.load();
        if (!runtime)
            return;
        const EeKernelSnapshot snapshot = runtime->eeScheduler().snapshot();
        std::printf("[threads] %s: cycle %llu, running thread %d\n", why, static_cast<unsigned long long>(snapshot.eeCycle),
                    snapshot.runningThreadId);
        for (const EeThreadSnapshot &t : snapshot.threads)
            std::printf("[threads]   %2d %-9s %-8s id=%-3d prio=%-3d pc=0x%08x ra=0x%08x entry=0x%08x\n", t.id,
                        statusName(t.status), waitName(t.waitReason), t.waitId, t.currentPriority, t.pc, t.ra, t.entry);
        for (const EeSemaphoreSnapshot &s : snapshot.semaphores)
            std::printf("[threads]   sema %d count %d/%d\n", s.id, s.count, s.maxCount);
        std::fflush(stdout);
    }

    // Where the main game thread spends its time: its pc read every 0.5 ms for `milliseconds` (approximate: the
    // generated code updates pc at function entries and branches). Test runs: STAR_GUEST_SAMPLE=1.
    void sampleGuestPc(int milliseconds)
    {
        const R5900Context *ctx = g_gameContext.load(std::memory_order_acquire);
        if (!ctx)
        {
            std::printf("[guest-pc] the game thread has not started\n");
            return;
        }
        std::map<uint32_t, int> hits;
        int samples = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < end)
        {
            const uint32_t pc = *static_cast<const volatile uint32_t *>(&ctx->pc);
            ++hits[pc];
            ++samples;
            std::this_thread::yield(); // dense: rare code (a state machine's few instructions per frame) must show
        }
        std::vector<std::pair<int, uint32_t>> ranked;
        for (const auto &[pc, count] : hits)
            ranked.emplace_back(count, pc);
        std::sort(ranked.rbegin(), ranked.rend());
        std::printf("[guest-pc] %d samples, %zu distinct pcs\n", samples, ranked.size());
        for (size_t i = 0; i < ranked.size(); ++i)
            std::printf("[guest-pc]   %5.1f%%  0x%08x\n", 100.0 * ranked[i].first / samples, ranked[i].second);
        std::fflush(stdout);
    }

    void startTimeLimit(int seconds)
    {
        std::thread([seconds] {
            const auto start = std::chrono::steady_clock::now();
            const char *sample = std::getenv("STAR_HOST_SAMPLE");
            for (int elapsed = 5; elapsed < seconds; elapsed += 5)
            {
                std::this_thread::sleep_until(start + std::chrono::seconds(elapsed));
                dumpThreads((std::to_string(elapsed) + " s").c_str());
                // STAR_SAMPLE_AT=<seconds, a multiple of 5> (default 5): when the samplers run.
                const char *atText = std::getenv("STAR_SAMPLE_AT");
                const int sampleAt = atText && *atText ? std::atoi(atText) : 5;
                if (sample && *sample == '1' && elapsed == sampleAt)
                    star::sampleHostThreads(2000);
                if (const char *guest = std::getenv("STAR_GUEST_SAMPLE"); guest && *guest == '1' && elapsed == sampleAt)
                    sampleGuestPc(2000);
            }
            std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
            if (g_runtime.load())
            {
                say(std::string("time limit of ") + std::to_string(seconds) + " s reached");
                dumpThreads("time limit");
            }
            else
            {
                say(std::string("time limit of ") + std::to_string(seconds) + " s reached before the runtime started");
            }
            std::cout.flush();
            std::cerr.flush();
            std::fflush(nullptr);
#if defined(_WIN32)
            // STAR 2026-11-23: _Exit still runs the DLLs' detach code, and the PCSX2 renderer's crashed there while its
            // thread was mid-frame (exit code 0xC0000005 instead of 124). TerminateProcess ends the run at once.
            TerminateProcess(GetCurrentProcess(), static_cast<unsigned int>(kExitTimeLimit));
#endif
            std::_Exit(kExitTimeLimit);
        }).detach();
    }
}

int main(int argc, char *argv[])
{
    // STAR 2026-11-23: F5 saves a state and F9 loads it (the runtime's quick slot), next to the program.
    {
        std::error_code stateError;
        const fs::path stateDir = fs::absolute(fs::path(argv[0]), stateError).parent_path() / "states";
        fs::create_directories(stateDir, stateError);
        ps2x::savestate::setQuickSlotPath(stateDir / "quick.state");
    }
    fs::path iso;
    bool hidden = false;
    int seconds = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--iso" && i + 1 < argc)
            iso = argv[++i];
        else if (arg == "--hidden")
            hidden = true;
        else if (arg == "--seconds" && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
        else
        {
            std::cout << "usage: STAR [--iso <disc image>] [--hidden] [--seconds <n>]\n";
            return kExitInitFailed;
        }
    }
    const fs::path exeDir = fs::absolute(fs::path(argv[0])).parent_path();
    const fs::path cacheDir = exeDir / "cache";
    findSettingsFile(exeDir);
#if defined(STAR_HAS_LAUNCHER)
    // STAR 2026-11-26: without --iso, the launcher first (star/launcher.cpp): the disc image, the presets, every option.
    if (iso.empty() && !hidden)
    {
        fs::path disc;
        {
            const star::options::Values file = star::options::readFile(g_settingsFile);
            if (const auto saved = file.find("disc_image"); saved != file.end() && !saved->second.empty())
                disc = fs::path(std::u8string(saved->second.begin(), saved->second.end()));
            if (disc.empty())
                if (const char *env = std::getenv("STAR_ISO"); env && *env)
                    disc = env;
        }
        star::launcher::Context context;
        context.title = kTitle;
        context.settingsFile = g_settingsFile;
        context.resources = exeDir / "resources";
        context.checkDisc = [](const fs::path &image)
        {
            std::vector<uint8_t> elf;
            int code = kExitOk;
            return checkDisc(image, elf, code);
        };
        if (star::launcher::run(context, disc) != star::launcher::Result::Play)
        {
            say("quit from the launcher");
            std::fflush(nullptr);
            return kExitOk;
        }
        iso = disc;
    }
#endif
    if (iso.empty())
        if (const char *env = std::getenv("STAR_ISO"); env && *env)
            iso = env;
    if (iso.empty())
    {
        say("no disc image: pass --iso <path> or set STAR_ISO");
        return kExitNoDisc;
    }
    if (seconds > 0)
        startTimeLimit(seconds);

    say(std::string(kTitle) + "; disc image " + iso.string());
    // a hidden (test) run uses the program's own defaults, whatever the player has chosen (STAR_TEST_SETTINGS=1: reads
    // the file all the same, to test the file's names)
    const bool readSettings = !hidden || std::getenv("STAR_TEST_SETTINGS") != nullptr;
    if (readSettings)
        loadSettings();
    // STAR 2026-11-26: the mouse and keyboard controls (star/bindings.h): the file's bind_ lines, else the defaults; the
    // swings of the style chosen win over the other actions' inputs.
    star::bindings::Binds binds = star::bindings::defaults();
    if (readSettings)
        binds = star::bindings::fromValues(star::options::readFile(g_settingsFile));
    std::string swings = "buttons";
    if (const char *v = std::getenv("STAR_MELEE_SWINGS"); v && *v)
        swings = v;
    else if (const char *v = std::getenv("STAR_COMBAT_MODE"); v && *v == '1')
        swings = "flick";

    fs::path bootElfPath;
    if (const int code = prepareDisc(iso, cacheDir, bootElfPath); code != kExitOk)
        return code;

    PS2Runtime::IoPaths paths;
    paths.elfPath = bootElfPath;
    paths.elfDirectory = cacheDir;
    paths.hostRoot = cacheDir;
    paths.cdRoot = cacheDir;
    paths.mcRoot = exeDir / "memcards" / "mc0";
    paths.cdImage = fs::absolute(iso);
    {
        std::error_code error;
        fs::create_directories(paths.mcRoot, error);
    }
    if (hidden || (std::getenv("STAR_TEST_CARDS") != nullptr)) // STAR_TEST_CARDS: also in a visible test run
    {
        // STAR 2026-11-23: a hidden (test) run plays on a copy of the memory cards, made anew each time, so that a
        // scripted run that wanders into the save or load screens can never touch the player's saves.
        std::error_code error;
        const fs::path real = exeDir / "memcards", test = exeDir / "memcards_test";
        fs::create_directories(test / "mc0", error);
        for (const char *card : {"Mcd001.ps2", "Mcd002.ps2"})
            if (fs::exists(real / card, error))
                fs::copy_file(real / card, test / card, fs::copy_options::overwrite_existing, error);
        paths.mcRoot = test / "mc0";
    }
    setDefaultEnv("PS2X_GS_SHADER_CACHE_DIR", (cacheDir / "gs_shaders").string().c_str());

    // Runtime defaults the SotC, Urban Reign and GHR ports ship with (each still overridable by its PS2X_ variable).
    // PS2X_SIO2_RECV1=2: libpad otherwise never leaves DISCONN and no button reaches the game (PADMAN quirk).
    setDefaultEnv("PS2X_SIO2_RECV1", "2");
    // PCSX2's GS (the default renderer) with PCSX2's own defaults: the runtime's fallbacks are God Hand: Rearmed's
    // choices (FXAA on, 16x anisotropic filtering, God Hand's "upscaled" post-effect fix). Shadow Tower Abyss has no
    // fixes in PCSX2's game database. Each can still be set from the environment.
    setDefaultEnv("PS2X_PCSX2_FXAA", "0");
    setDefaultEnv("PS2X_PCSX2_ANISOTROPY", "0");
    setDefaultEnv("PS2X_PCSX2_NATIVE_SCALING", "off");
    // STAR 2026-11-25: sharp shadows (the setting sharp_shadows; the F2 menu switches it while playing). The game draws
    // its shadows as shadow volumes into a 256x256 buffer (GS page 0x15e, block 0x2bc0), tested against a 256x256
    // copy of the scene's depth (page 0x16e, block 0x2dc0) that it makes by drawing the depth buffer (page 0x118,
    // block 0x2300, 640x448) as a texture, and lays the result over the 640x448 picture. The renderer is told the two
    // buffers (which it then draws at 2.5 times the resolution of the rest: as many pixels across as the picture
    // has) and that the second is a copy of the scene's depth (taken straight from it, not through 16 bits).
    setDefaultEnv("PS2X_PCSX2_BOOST_TARGETS", "2bc0:4,2dc0:4");
    setDefaultEnv("PS2X_PCSX2_DEPTH_ALIAS", "2dc0=2300:640:448");
    if (const char *sharp = std::getenv("STAR_SHARP_SHADOWS"); sharp && *sharp == '1')
        setDefaultEnv("PS2X_PCSX2_TARGET_BOOST", "2.5");
    // STAR 2026-11-25: self_shadows = off: creatures do not shadow themselves (GSRendererHW::ApplyOwnerShadow; the game
    // thread labels each model piece it sends, star/game_fixes.cpp, and the runtime's VIF interpreter passes the labels
    // on). The labels are passed on whenever the game thread puts them in.
    setDefaultEnv("PS2X_VIF_OWNER_LABELS", "1");
    if (const char *self = std::getenv("STAR_SELF_SHADOWS"); self && *self == '0')
        setDefaultEnv("PS2X_PCSX2_NO_SELF_SHADOW", "1");
    // STAR 2026-11-26: shadow_walls = on: a creature's shadow is left off surfaces turned away from the shadow light and
    // off surfaces with scenery between them and the creature (GSRendererHW::ApplyOwnerShadow, star/shadow_volumes.cpp)
    if (const char *walls = std::getenv("STAR_SHADOW_WALLS"); walls && *walls == '1')
        setDefaultEnv("PS2X_PCSX2_SHADOW_WALLS", "1");
    // STAR 2026-11-23: two more God Hand changes inside the renderer copy, on unless told otherwise. Shadow Tower Abyss
    // also draws into a 16-bit depth buffer, so "precise depth" acted on it: depths refitted from Q, and every draw that
    // tests depth without writing it (shadows, decals) lifted by one whole depth step, which lets it pass over a
    // surface up to a step nearer (the user's report: floor tile edges showing through wall bases, a shadow patch on
    // an NPC's face). "Perspective shading" changes how vertex colours are interpolated. Off = what the PS2 and PCSX2 do.
    // STAR 2026-11-24: the game swaps its display buffer right after every vblank; the renderer takes each field
    // after that swap, so a finished picture goes to the screen at that vblank and not a field (16.7 ms) later
    // (EeScheduler.cpp, gsVsyncAfterFlip). 0 = the emulators' order.
    setDefaultEnv("PS2X_GS_VSYNC_AFTER_FLIP", "1");
    // STAR 2026-11-24: player 1's bindings (the runtime's own defaults are another game's). The controller as the
    // PS2 pad it stands for; the keyboard for mouse and keyboard play (game_fixes.cpp: W A S D and the arrows are
    // the d-pad, which walks and strafes in control type 4 and moves through the menus; Left Shift is L3, which the
    // game does not use in play and the tweak reads as "walk").
    {
        static const char *const kBindings[][2] = {
            {"PS2X_PAD1", "1"},
            {"PS2X_PAD1_TYPE", "DualShock2"},
            {"PS2X_PAD1_INVERTL", "0"},
            {"PS2X_PAD1_INVERTR", "0"},
            {"PS2X_PAD1_DEADZONE", "0"},
            {"PS2X_PAD1_AXISSCALE", "1.33"},
            {"PS2X_PAD1_BUTTONDEADZONE", "0"},
            {"PS2X_PAD1_LARGEMOTORSCALE", "1"},
            {"PS2X_PAD1_SMALLMOTORSCALE", "1"},
            {"PS2X_PAD1_UP", "Keyboard/W;Keyboard/Up;SDL-0/DPadUp"},
            {"PS2X_PAD1_RIGHT", "Keyboard/D;Keyboard/Right;SDL-0/DPadRight"},
            {"PS2X_PAD1_DOWN", "Keyboard/S;Keyboard/Down;SDL-0/DPadDown"},
            {"PS2X_PAD1_LEFT", "Keyboard/A;Keyboard/Left;SDL-0/DPadLeft"},
            {"PS2X_PAD1_TRIANGLE", "Keyboard/R;SDL-0/FaceNorth"},
            {"PS2X_PAD1_CIRCLE", "Keyboard/E;Keyboard/Return;Keyboard/Enter;Keyboard/Space;SDL-0/FaceEast"},
            // (Tab is the game layer's: the menu button too, or the other weapon; ps2_host_input::setTabButton)
            {"PS2X_PAD1_CROSS", "Keyboard/Escape;Keyboard/Backspace;SDL-0/FaceSouth"},
            {"PS2X_PAD1_SQUARE", "Keyboard/Q;SDL-0/FaceWest"},
            {"PS2X_PAD1_SELECT", "Keyboard/P;SDL-0/Back"},
            {"PS2X_PAD1_START", "Keyboard/F;SDL-0/Start"},
            {"PS2X_PAD1_L1", "SDL-0/LeftShoulder"},
            {"PS2X_PAD1_L2", "SDL-0/+LeftTrigger"},
            {"PS2X_PAD1_R1", "SDL-0/RightShoulder"},
            {"PS2X_PAD1_R2", "SDL-0/+RightTrigger"},
            {"PS2X_PAD1_L3", "Keyboard/LeftShift;SDL-0/LeftStick"},
            {"PS2X_PAD1_R3", "SDL-0/RightStick"},
            {"PS2X_PAD1_LUP", "SDL-0/-LeftY"},
            {"PS2X_PAD1_LRIGHT", "SDL-0/+LeftX"},
            {"PS2X_PAD1_LDOWN", "SDL-0/+LeftY"},
            {"PS2X_PAD1_LLEFT", "SDL-0/-LeftX"},
            {"PS2X_PAD1_RUP", "SDL-0/-RightY"},
            {"PS2X_PAD1_RRIGHT", "SDL-0/+RightX"},
            {"PS2X_PAD1_RDOWN", "SDL-0/+RightY"},
            {"PS2X_PAD1_RLEFT", "SDL-0/-RightX"},
            {"PS2X_PAD1_LARGEMOTOR", "SDL-0/LargeMotor"},
            {"PS2X_PAD1_SMALLMOTOR", "SDL-0/SmallMotor"},
        };
        if (!std::getenv("PS2X_PAD1"))
            for (const auto &binding : kBindings)
            {
                // the keys of the actions the player can rebind (star/bindings.h): theirs, with the controller's
                std::string value = binding[1];
                for (const star::bindings::Action &action : star::bindings::actions())
                    if (action.padKey && std::string("PS2X_PAD1_") + action.padKey == binding[0])
                        value = star::bindings::envValue(binds, swings, action);
                setDefaultEnv(binding[0], value.c_str());
            }
        star::bindings::setLive(binds, swings, false);
        std::string controls;
        for (const star::bindings::Action &action : star::bindings::actions())
            controls += (controls.empty() ? "" : "; ") + std::string(action.id) + " = " + star::bindings::encode(binds[action.id]);
        say("controls (" + swings + " swings): " + controls);
    }
    setDefaultEnv("PS2X_PCSX2_PRECISE_DEPTH", "0");
    setDefaultEnv("PS2X_PCSX2_PERSPECTIVE_SHADING", "0");
    GSThreadedBackend::SetEnabledByDefault(true);
    GSThreadedBackend::SetGpuByDefault(true);
    GSGpuBackend::SetAsyncPresentDefault(true);
    ps2x::asyncvif::setDefaultEnabled(true);
    ps2x::asyncvif::setHostPacingDefault(true);
    EeScheduler::setHostPacingDefault(true);

    if (hidden)
    {
        SetConfigFlags(FLAG_WINDOW_HIDDEN);
        setDefaultEnv("PS2X_AUDIO_MUTE", "1");
        setDefaultEnv("PS2X_VSYNC", "0");
        say("hidden run: no window, no sound");
    }
    else
    {
        // STAR 2026-11-23: no "Not Responding" ghost over a game that runs; a log line when Windows raises the flag.
        star::startWindowWatch();
    }

    try
    {
        // Never destroyed: the process ends while it is alive, so the time-limit thread can always use it.
        PS2Runtime &runtime = *new PS2Runtime();
        g_runtime.store(&runtime);
#if defined(PS2X_ENABLE_DEBUG_UI)
        // The runtime debugger: F12 toggles it; STAR_DEBUG_UI=1 opens it at start-up (hidden otherwise, as GHR).
        static PS2DebugPanel debugPanel;
        {
            const char *show = std::getenv("STAR_DEBUG_UI");
            debugPanel.setVisible(!hidden && show && *show == '1');
        }
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &, void *userData) { static_cast<PS2DebugPanel *>(userData)->initialize(); },
            [](PS2Runtime &rt, void *userData)
            {
                auto *panel = static_cast<PS2DebugPanel *>(userData);
                panel->draw(rt);
                GSPcsx2Backend::SetHostUiVisible(panel->isVisible()); // PCSX2's GS window covers this one: hidden meanwhile
            },
            [](PS2Runtime &, void *userData) { static_cast<PS2DebugPanel *>(userData)->shutdown(); },
            &debugPanel);
#endif
        if (!runtime.initialize(kTitle))
        {
            say("runtime initialization failed");
            return kExitInitFailed;
        }
        if (!runtime.loadELF(bootElfPath.string()))
        {
            say("failed to load " + bootElfPath.string());
            return kExitInitFailed;
        }
        char entry[16];
        std::snprintf(entry, sizeof(entry), "%08x", runtime.cpu().pc);
        say(std::string("program loaded, entry 0x") + entry);
        PS2Runtime::setIoPaths(paths);
        runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);

        const uint32_t entryPc = runtime.cpu().pc;
        if (runtime.hasFunction(entryPc))
        {
            g_originalEntry = runtime.lookupFunction(entryPc);
            if (!runtime.replaceFunction(entryPc, &bootEntryWrapper))
                g_originalEntry = nullptr;
        }
        if (!g_originalEntry)
            say("warning: could not wrap the entry function; the EE FPU mode is not set");
        star::installSettingsMenu(g_settingsFile); // before the fixes: they read star::live()
        star::installDebugMenu(runtime, g_settingsFile);
        star::installGameFixes(runtime);
        star::installFrameDouble(runtime);
#if defined(STAR_HAS_VU1_PROGRAMS)
        registerGeneratedVu1Programs();
        say("VU1 microcode: ahead-of-time compiled programs registered");
#else
        say("VU1 microcode: no compiled programs in this build (interpreter: slow 3D); run tools/build.py vu");
#endif

        say("starting the game");
        runtime.run();
        say("the runtime returned");
    }
    catch (const std::exception &e)
    {
        say(std::string("fatal exception: ") + e.what());
        std::fflush(nullptr);
        std::_Exit(kExitInitFailed);
    }
    std::fflush(nullptr);
    std::_Exit(kExitOk);
}
