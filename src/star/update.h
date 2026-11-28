#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// STAR 2026-11-27: the launcher's update (the user: "checks for new versions and lets the user automatically update
// their version with one click while keeping their in-game saves and save states intact").
//
// The check asks GitHub for the project's latest release (one HTTPS request; nothing but the request itself is sent).
// The update downloads that release's Windows zip into "update" next to the program, checks its size and (when
// GitHub gives one) its SHA-256, unpacks it there and then puts each of its files in the program's place: the file
// that is there is renamed (a running .exe or a loaded .dll can be renamed, not overwritten), the new one moved in.
// Only what the zip holds is touched, and never the settings file, the memory cards, the save states, the logs or
// the caches (the zip holds none of them, and install() leaves them out whatever a zip holds). The program is then
// started again; its next start removes the renamed files (cleanUp).
namespace star::update
{
    constexpr const char *kRepository = "crow15669/STAR-Shadow-Tower-Abyss-Recomp";

    struct Release
    {
        std::string version; // "0.9.1" (the tag without its v)
        std::string page;    // the release's page, for "What's new"
        std::string url;     // the Windows zip
        std::string sha256;  // of the zip, lower-case hex; "" when not given
        uint64_t size = 0;   // of the zip; 0 when not given
    };

    // <0, 0, >0 as a is older than, the same as, newer than b. The numbers between the dots count ("0.10.0" is
    // newer than "0.9.1"); a leading v and anything after the numbers are ignored.
    int compareVersions(const std::string &a, const std::string &b);

    // The release in GitHub's answer to .../releases/latest. "" when read, else what is wrong with it.
    // anyHost: a test's answer (STAR_UPDATE_API), whose zip may lie anywhere.
    std::string parseRelease(const std::string &json, Release &release, bool anyHost = false);

    // Unpacks a release zip into a folder (made empty first); files: every file's place in it, with forward
    // slashes, a single top folder of the zip left out. "" when done, else what went wrong.
    std::string unpack(const std::filesystem::path &zip, const std::filesystem::path &into, std::vector<std::string> &files,
                       const std::atomic<bool> &cancel);

    // Puts the unpacked files into the program's folder (see above). "" when done; else what went wrong, with
    // everything put back as it was.
    std::string install(const std::filesystem::path &from, const std::vector<std::string> &files, const std::filesystem::path &program);

    // At every start: what an update left behind (the renamed files, the "update" folder). Costs nothing when
    // there is no "update" folder.
    void cleanUp(const std::filesystem::path &program);

    // --- the system's part (update_win32.cpp) ---
    // One GET. The answer goes into `body` or into the file `file`; progress(done, total; 0 = not known) may end it by
    // answering false. "" when done (status 200), else what went wrong in a player's words.
    std::string httpGet(const std::string &url, std::string *body, const std::filesystem::path *file, uint64_t most,
                        const std::function<bool(uint64_t, uint64_t)> &progress);
    void openPage(const std::string &url); // in the player's browser (https only)
    bool startProgram(const std::filesystem::path &exe, const std::string &arguments);

    enum class Stage
    {
        Idle,
        Checking,
        UpToDate,
        Available,   // a newer release: `release`
        CheckFailed, // `note`
        Downloading, // `done` of `total`
        Installing,
        Installed,   // the program is to be started again
        Failed,      // the update itself: `note`; the release is still there to try again
    };

    // One check and one update at a time, each on a thread of its own. Main thread: everything here. Destroyed while
    // a check or a download is under way, it ends them and does not wait; while files are being put in place it waits.
    class Session
    {
    public:
        Session(std::filesystem::path program, std::string current);
        ~Session();
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        struct View
        {
            Stage stage = Stage::Idle;
            Release release;
            std::string note;
            uint64_t done = 0, total = 0;
        };
        View view() const;
        const std::string &current() const;
        bool busy() const;
        void check();  // unless busy
        void start();  // the update, from Available or Failed
        void cancel(); // a download under way

    private:
        struct Core;
        void launch(bool update);
        std::shared_ptr<Core> m_core;
        std::thread m_thread;
    };
}
