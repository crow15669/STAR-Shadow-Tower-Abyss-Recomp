#pragma once

namespace star
{
    // Windows only (a no-op elsewhere). Turns off Windows' "Not Responding" ghost copy of the game window (the white
    // tint over a game that is in fact running) unless STAR_WINDOW_GHOSTING=1, and starts a small watcher thread that
    // writes one log line when Windows flags the window as not responding, saying whether the window's loop was
    // still taking messages (a false flag) or had really stopped. After God Hand: Rearmed's window watch.
    void startWindowWatch();

    // The screen's refresh: its period and the time of a recent refresh, both in nanoseconds of
    // std::chrono::steady_clock. From the desktop compositor (Windows); false when it cannot be told (then 60 a
    // second is assumed by the caller). STAR_REFRESH_HZ=<rate> replaces it with a made-up screen (tests).
    bool screenRefresh(long long &periodNs, long long &lastRefreshNs);
    // Waits of a millisecond are a millisecond (Windows' timers are coarser unless asked). Once, at start.
    void enableFineTimers();
}
