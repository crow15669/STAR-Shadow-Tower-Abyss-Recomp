#include "star/window_watch.h"

#if defined(_WIN32)

#include "runtime/ps2_display_options.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <mmsystem.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

namespace star
{
    // STAR 2026-11-23 (user's report: after switching to other windows during start-up and clicking back, the game
    // window showed "(Not Responding)" and a white tint while the game ran on and took controller input).
    // God Hand: Rearmed met the same (its src/kamite/crash_history.cpp, Oct 2026): Windows flags the window as hung now
    // and then although its loop takes messages every frame, and then shows its ghost copy over the picture. As there,
    // ghosting is turned off for the process, and a watcher notes what was true when the flag was raised: the loop's
    // last message pump (ps2_host_display::hostLoopHeartbeat) and whether the window answers a sent message.
    void startWindowWatch()
    {
        if (const char *ghosting = std::getenv("STAR_WINDOW_GHOSTING"); !ghosting || *ghosting != '1')
            DisableProcessWindowsGhosting();
        std::thread([]
                    {
                        struct Find
                        {
                            DWORD pid;
                            HWND hung;
                        };
                        int seconds = 0;
                        int notes = 0;
                        for (;;)
                        {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            Find find{GetCurrentProcessId(), nullptr};
                            EnumWindows([](HWND window, LPARAM param) -> BOOL
                                        {
                                            auto *f = reinterpret_cast<Find *>(param);
                                            DWORD pid = 0;
                                            GetWindowThreadProcessId(window, &pid);
                                            if (pid == f->pid && IsWindowVisible(window) && IsHungAppWindow(window))
                                            {
                                                f->hung = window;
                                                return FALSE;
                                            }
                                            return TRUE; },
                                        reinterpret_cast<LPARAM>(&find));
                            if (!find.hung)
                            {
                                if (seconds >= 2 && notes <= 8)
                                {
                                    std::printf("[window] the flag cleared after about %d s\n", seconds + 5);
                                    std::fflush(stdout);
                                }
                                seconds = 0;
                                continue;
                            }
                            if (++seconds != 2 || notes >= 8)
                                continue;
                            ++notes;
                            uint64_t iterations = 0, lastPump = 0;
                            ps2_host_display::hostLoopHeartbeat(iterations, lastPump);
                            const uint64_t sincePump = lastPump ? ps2_host_display::hostSteadyMs() - lastPump : 0;
                            DWORD_PTR result = 0;
                            const uint64_t start = ps2_host_display::hostSteadyMs();
                            const LRESULT answered = SendMessageTimeoutW(find.hung, WM_NULL, 0, 0, SMTO_BLOCK, 1500, &result);
                            const uint64_t took = ps2_host_display::hostSteadyMs() - start;
                            std::printf("[window] Windows flags the game window as not responding: host loop %llu iterations, last "
                                        "message pump %llu ms ago, a test message %s (%llu ms) -> %s\n",
                                        static_cast<unsigned long long>(iterations), static_cast<unsigned long long>(sincePump),
                                        answered ? "was answered" : "was NOT answered", static_cast<unsigned long long>(took),
                                        sincePump < 2000u && answered ? "the loop runs: a false flag" : "the window's loop has STOPPED");
                            std::fflush(stdout);
                        } })
            .detach();
    }

    // STAR 2026-11-23: the screen's refresh, for pictures made for the screen's own rate (frame_double.cpp).
    // std::chrono::steady_clock is the performance counter on Windows, so the compositor's counter values convert
    // to its nanoseconds by the counter's frequency.
    bool screenRefresh(long long &periodNs, long long &lastRefreshNs)
    {
        static const double madeUpHz = []
        { const char *v = std::getenv("STAR_REFRESH_HZ"); return v && *v ? std::atof(v) : 0.0; }();
        if (madeUpHz >= 10.0)
        {
            static const long long epoch =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            periodNs = static_cast<long long>(1e9 / madeUpHz);
            lastRefreshNs = epoch;
            return true;
        }
        DWM_TIMING_INFO info{};
        info.cbSize = sizeof(info);
        LARGE_INTEGER frequency{};
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info)) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
            info.qpcRefreshPeriod == 0)
            return false;
        const double toNs = 1e9 / static_cast<double>(frequency.QuadPart);
        periodNs = static_cast<long long>(static_cast<double>(info.qpcRefreshPeriod) * toNs);
        lastRefreshNs = static_cast<long long>(static_cast<double>(info.qpcVBlank) * toNs);
        return periodNs > 1000000ll && periodNs < 100000000ll; // 10 to 1000 a second
    }

    void enableFineTimers()
    {
        timeBeginPeriod(1);
    }
}

#else

namespace star
{
    void startWindowWatch() {}
    bool screenRefresh(long long &, long long &) { return false; }
    void enableFineTimers() {}
}

#endif
