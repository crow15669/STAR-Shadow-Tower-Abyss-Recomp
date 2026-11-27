#pragma once

#include <filesystem>
#include <string>

// STAR 2026-11-27: the play log, written by the program itself (the user: why a .cmd to get a log? "what does the
// .cmd do differently that can't be [in the program]"). When STAR is started the ordinary way (its output goes
// nowhere), everything it prints goes to logs/play_last.log next to the program; the three runs before are kept as
// play_previous_1..3. A run whose output is already taken (a test, a script's "> file") is left alone.
namespace star
{
    constexpr const char *kVersion = "0.9.0";

    // Starts the log when the output is not taken already, and makes the program's folder the working folder (logs,
    // F1 catches, memory cards and caches then all lie next to the program, whatever the shortcut's "start in" says).
    // True when the log is the program's own.
    bool startPlayLog(const std::filesystem::path &exeDir);

    // A path as the log shows it: the file's name without its folders, which can hold a person's name
    // (C:\Users\<name>\...). Logs get posted with bug reports.
    std::string logName(const std::filesystem::path &path);
}
