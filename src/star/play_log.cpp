#include "star/play_log.h"

#include <chrono>
#include <cstdio>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#define NOUSER
#include <windows.h>
#include <io.h>
#endif

namespace star
{
    namespace fs = std::filesystem;

    std::string logName(const fs::path &path)
    {
        const std::u8string name = path.filename().u8string();
        return name.empty() ? std::string("(none)") : std::string(name.begin(), name.end());
    }

    namespace
    {
        // A log can never grow without end: past this size nothing more is written in this run.
        constexpr long long kLogLimit = 20ll * 1024 * 1024;

#if defined(_WIN32)
        bool outputTaken()
        {
            // a file or a pipe (a test, "STAR.exe > log"), or a console to print to: left as it is
            const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            if (!out || out == INVALID_HANDLE_VALUE)
                return false;
            const DWORD type = GetFileType(out);
            return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE || type == FILE_TYPE_CHAR;
        }
#endif
    }

    bool startPlayLog(const fs::path &exeDir)
    {
#if defined(_WIN32)
        if (outputTaken())
            return false;
        std::error_code error;
        fs::current_path(exeDir, error);
        const fs::path logs = exeDir / "logs";
        fs::create_directories(logs, error);
        // the runs before: play_previous_1 is the newest
        const fs::path last = logs / "play_last.log";
        for (int n = 2; n >= 0; --n)
        {
            const fs::path from = n == 0 ? last : logs / ("play_previous_" + std::to_string(n) + ".log");
            const fs::path to = logs / ("play_previous_" + std::to_string(n + 1) + ".log");
            if (fs::exists(from, error))
            {
                fs::remove(to, error);
                fs::rename(from, to, error);
            }
        }
        // Both streams append to the one file (appends do not overwrite each other), unbuffered: a crash loses nothing.
        // (_wfreopen, not _wfreopen_s: the _s kind takes the file for itself, the second open then fails, and the
        // program died on the closed stream at once; found by starting it as a player does)
        if (!_wfreopen(last.c_str(), L"a", stdout))
            return false;
        const bool errorsToo = _wfreopen(last.c_str(), L"a", stderr) != nullptr;
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        if (errorsToo)
            std::setvbuf(stderr, nullptr, _IONBF, 0);
        // for code that writes through the process's handles (libraries loaded later take them from here)
        if (const int fd = _fileno(stdout); fd >= 0)
            SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(fd)));
        if (const int fd = _fileno(stderr); errorsToo && fd >= 0)
            SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(fd)));
        std::thread([last]
        {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::error_code sizeError;
                const auto size = fs::file_size(last, sizeError);
                if (!sizeError && static_cast<long long>(size) > kLogLimit)
                {
                    std::fprintf(stderr, "[STAR] the log has reached %lld MB: nothing more is written in this run%c",
                                 kLogLimit / (1024 * 1024), 10);
                    FILE *none = nullptr;
                    freopen_s(&none, "NUL", "w", stdout);
                    freopen_s(&none, "NUL", "w", stderr);
                    return;
                }
            }
        }).detach();
        return true;
#else
        (void)exeDir;
        return false;
#endif
    }
}
