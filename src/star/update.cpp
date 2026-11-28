#include "star/update.h"

#include "star/sha256.h"

#include "miniz.h" // tinfl (inflate), from libchdr's copy: the zip container itself is read here

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <system_error>

// The launcher's update (update.h).
namespace star::update
{
    namespace fs = std::filesystem;

    namespace
    {
        fs::path fromUtf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

        // --- a JSON reader, as much as GitHub's answer needs ---
        struct Json
        {
            enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
            bool boolean = false;
            double number = 0.0;
            std::string text;
            std::vector<Json> items;
            std::vector<std::pair<std::string, Json>> members;

            const Json *get(const char *key) const
            {
                for (const auto &[name, value] : members)
                    if (name == key)
                        return &value;
                return nullptr;
            }
            std::string string(const char *key) const
            {
                const Json *value = get(key);
                return value && value->kind == Kind::String ? value->text : std::string();
            }
        };

        class JsonReader
        {
        public:
            JsonReader(const std::string &text) : m_at(text.data()), m_end(text.data() + text.size()) {}

            bool read(Json &out)
            {
                const bool ok = value(out, 0);
                space();
                return ok && m_at == m_end;
            }

        private:
            void space()
            {
                while (m_at < m_end && (*m_at == ' ' || *m_at == '\t' || *m_at == '\r' || *m_at == '\n'))
                    ++m_at;
            }
            bool word(const char *text)
            {
                const size_t length = std::strlen(text);
                if (static_cast<size_t>(m_end - m_at) < length || std::memcmp(m_at, text, length) != 0)
                    return false;
                m_at += length;
                return true;
            }
            static void utf8(std::string &out, unsigned code)
            {
                if (code < 0x80u)
                    out += static_cast<char>(code);
                else if (code < 0x800u)
                {
                    out += static_cast<char>(0xC0u | (code >> 6));
                    out += static_cast<char>(0x80u | (code & 0x3Fu));
                }
                else if (code < 0x10000u)
                {
                    out += static_cast<char>(0xE0u | (code >> 12));
                    out += static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
                    out += static_cast<char>(0x80u | (code & 0x3Fu));
                }
                else
                {
                    out += static_cast<char>(0xF0u | (code >> 18));
                    out += static_cast<char>(0x80u | ((code >> 12) & 0x3Fu));
                    out += static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
                    out += static_cast<char>(0x80u | (code & 0x3Fu));
                }
            }
            bool hex4(unsigned &code)
            {
                if (m_end - m_at < 4)
                    return false;
                code = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const char c = *m_at++;
                    code <<= 4;
                    if (c >= '0' && c <= '9')
                        code |= static_cast<unsigned>(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        code |= static_cast<unsigned>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        code |= static_cast<unsigned>(c - 'A' + 10);
                    else
                        return false;
                }
                return true;
            }
            bool string(std::string &out)
            {
                if (m_at >= m_end || *m_at != '"')
                    return false;
                ++m_at;
                while (m_at < m_end && *m_at != '"')
                {
                    if (*m_at != '\\')
                    {
                        out += *m_at++;
                        continue;
                    }
                    if (++m_at >= m_end)
                        return false;
                    const char c = *m_at++;
                    switch (c)
                    {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u':
                    {
                        unsigned code = 0;
                        if (!hex4(code))
                            return false;
                        if (code >= 0xD800u && code < 0xDC00u && m_end - m_at >= 6 && m_at[0] == '\\' && m_at[1] == 'u')
                        {
                            m_at += 2;
                            unsigned low = 0;
                            if (!hex4(low))
                                return false;
                            code = 0x10000u + ((code - 0xD800u) << 10) + ((low - 0xDC00u) & 0x3FFu);
                        }
                        utf8(out, code);
                        break;
                    }
                    default: out += c; break; // " \ /
                    }
                }
                if (m_at >= m_end)
                    return false;
                ++m_at;
                return true;
            }
            bool value(Json &out, int depth)
            {
                if (depth > 48)
                    return false;
                space();
                if (m_at >= m_end)
                    return false;
                if (*m_at == '{')
                {
                    ++m_at;
                    out.kind = Json::Kind::Object;
                    space();
                    if (m_at < m_end && *m_at == '}')
                        return ++m_at, true;
                    for (;;)
                    {
                        space();
                        std::string name;
                        if (!string(name))
                            return false;
                        space();
                        if (m_at >= m_end || *m_at++ != ':')
                            return false;
                        out.members.emplace_back(std::move(name), Json());
                        if (!value(out.members.back().second, depth + 1))
                            return false;
                        space();
                        if (m_at >= m_end)
                            return false;
                        if (*m_at == ',')
                        {
                            ++m_at;
                            continue;
                        }
                        return *m_at++ == '}';
                    }
                }
                if (*m_at == '[')
                {
                    ++m_at;
                    out.kind = Json::Kind::Array;
                    space();
                    if (m_at < m_end && *m_at == ']')
                        return ++m_at, true;
                    for (;;)
                    {
                        out.items.emplace_back();
                        if (!value(out.items.back(), depth + 1))
                            return false;
                        space();
                        if (m_at >= m_end)
                            return false;
                        if (*m_at == ',')
                        {
                            ++m_at;
                            continue;
                        }
                        return *m_at++ == ']';
                    }
                }
                if (*m_at == '"')
                {
                    out.kind = Json::Kind::String;
                    return string(out.text);
                }
                if (word("true"))
                    return out.kind = Json::Kind::Bool, out.boolean = true, true;
                if (word("false"))
                    return out.kind = Json::Kind::Bool, true;
                if (word("null"))
                    return true;
                const char *start = m_at;
                while (m_at < m_end && (std::strchr("+-.eE", *m_at) || (*m_at >= '0' && *m_at <= '9')))
                    ++m_at;
                if (m_at == start)
                    return false;
                out.kind = Json::Kind::Number;
                out.number = std::strtod(std::string(start, m_at).c_str(), nullptr);
                return true;
            }

            const char *m_at, *m_end;
        };

        std::vector<int> numbers(const std::string &version)
        {
            std::vector<int> out;
            size_t at = !version.empty() && (version[0] == 'v' || version[0] == 'V') ? 1u : 0u;
            while (at < version.size() && version[at] >= '0' && version[at] <= '9')
            {
                long value = 0;
                while (at < version.size() && version[at] >= '0' && version[at] <= '9')
                    value = std::min(value * 10 + (version[at++] - '0'), 1000000L);
                out.push_back(static_cast<int>(value));
                if (at < version.size() && version[at] == '.')
                    ++at;
                else
                    break;
            }
            return out;
        }

        bool endsWith(const std::string &text, const char *end)
        {
            const size_t length = std::strlen(end);
            return text.size() >= length && text.compare(text.size() - length, length, end) == 0;
        }

        // --- the zip container ---
        uint32_t crc32(const uint8_t *data, size_t size)
        {
            static const std::vector<uint32_t> table = []
            {
                std::vector<uint32_t> t(256);
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < size; ++i)
                crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }
        uint16_t u16(const uint8_t *at) { return static_cast<uint16_t>(at[0] | (at[1] << 8)); }
        uint32_t u32(const uint8_t *at) { return at[0] | (at[1] << 8) | (at[2] << 16) | (static_cast<uint32_t>(at[3]) << 24); }

        // A file's place as the zip names it, made safe: forward slashes, no empty part, no "." or "..", no drive, not
        // from the root. "" when it is not.
        std::string safeName(std::string name)
        {
            std::replace(name.begin(), name.end(), '\\', '/');
            if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos || name.size() > 240u)
                return {};
            size_t at = 0;
            while (at < name.size())
            {
                const size_t slash = name.find('/', at);
                const std::string part = name.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
                if (part.empty() || part == "." || part == ".." || part.back() == ' ' || part.back() == '.')
                    return {};
                for (const char c : part)
                    if (static_cast<unsigned char>(c) < 0x20u || std::strchr("<>\"|?*", c))
                        return {};
                if (slash == std::string::npos)
                    break;
                at = slash + 1u;
            }
            return name;
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        // What an update never writes, whatever a zip holds: the player's own.
        bool playersOwn(const std::string &file)
        {
            static const char *const folders[] = {"memcards", "memcards_test", "mc0", "mc1", "states", "logs", "cache", "shadercache_pcsx2", "update"};
            const std::string first = lower(file.substr(0, file.find('/')));
            for (const char *folder : folders)
                if (first == folder)
                    return true;
            return first == "star places.txt";
        }

        constexpr const char *kOld = ".star-old";
    }

    int compareVersions(const std::string &a, const std::string &b)
    {
        const std::vector<int> x = numbers(a), y = numbers(b);
        for (size_t i = 0; i < std::max(x.size(), y.size()); ++i)
        {
            const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
            if (p != q)
                return p < q ? -1 : 1;
        }
        return 0;
    }

    std::string parseRelease(const std::string &json, Release &release, bool anyHost)
    {
        Json root;
        if (!JsonReader(json).read(root) || root.kind != Json::Kind::Object)
            return "The answer about the latest version could not be read.";
        const std::string tag = root.string("tag_name");
        if (numbers(tag).empty())
            return "The answer about the latest version names no version.";
        release = Release();
        release.version = tag.substr(tag[0] == 'v' || tag[0] == 'V' ? 1u : 0u);
        release.page = root.string("html_url");
        const std::string home = std::string("https://github.com/") + kRepository + "/";
        if (!anyHost && release.page.compare(0, home.size(), home) != 0)
            release.page = home + "releases";
        if (const Json *assets = root.get("assets"); assets && assets->kind == Json::Kind::Array)
            for (const Json &asset : assets->items)
            {
                const std::string name = asset.string("name"), url = asset.string("browser_download_url");
                if (!endsWith(lower(name), "-windows-x64.zip"))
                    continue;
                if (!anyHost && url.compare(0, home.size(), home) != 0)
                    continue; // only the project's own downloads
                release.url = url;
                if (const Json *size = asset.get("size"); size && size->kind == Json::Kind::Number && size->number > 0.0)
                    release.size = static_cast<uint64_t>(size->number);
                const std::string digest = lower(asset.string("digest"));
                if (digest.compare(0, 7, "sha256:") == 0 && digest.size() == 7u + 64u)
                    release.sha256 = digest.substr(7);
                break;
            }
        return {};
    }

    std::string unpack(const fs::path &zip, const fs::path &into, std::vector<std::string> &files, const std::atomic<bool> &cancel)
    {
        files.clear();
        std::vector<uint8_t> data;
        {
            std::ifstream in(zip, std::ios::binary | std::ios::ate);
            const std::streamoff size = in ? static_cast<std::streamoff>(in.tellg()) : 0;
            if (size < 22 || size > (1ll << 30))
                return "The download is not a zip file.";
            data.resize(static_cast<size_t>(size));
            in.seekg(0);
            in.read(reinterpret_cast<char *>(data.data()), size);
            if (!in)
                return "The download could not be read back.";
        }
        // the end record, searched from the back (a comment of up to 64 KB may follow it)
        size_t end = data.size() - 22u;
        const size_t lowest = data.size() > 22u + 65535u ? data.size() - 22u - 65535u : 0u;
        while (u32(&data[end]) != 0x06054B50u)
        {
            if (end == lowest)
                return "The download is not a zip file.";
            --end;
        }
        const size_t count = u16(&data[end + 10u]), directorySize = u32(&data[end + 12u]), directory = u32(&data[end + 16u]);
        if (count == 0u || count == 0xFFFFu || directory == 0xFFFFFFFFu || directory + directorySize > end)
            return "The download is a kind of zip file this program does not read.";
        struct Entry
        {
            std::string name;
            uint32_t method, crc, packed, size, local;
        };
        std::vector<Entry> entries;
        uint64_t total = 0;
        size_t at = directory;
        for (size_t i = 0; i < count; ++i)
        {
            if (at + 46u > end || u32(&data[at]) != 0x02014B50u)
                return "The download's list of files is damaged.";
            const size_t nameSize = u16(&data[at + 28u]), extraSize = u16(&data[at + 30u]), noteSize = u16(&data[at + 32u]);
            if (at + 46u + nameSize + extraSize + noteSize > end)
                return "The download's list of files is damaged.";
            Entry entry;
            entry.name.assign(reinterpret_cast<const char *>(&data[at + 46u]), nameSize);
            entry.method = u16(&data[at + 10u]);
            entry.crc = u32(&data[at + 16u]);
            entry.packed = u32(&data[at + 20u]);
            entry.size = u32(&data[at + 24u]);
            entry.local = u32(&data[at + 42u]);
            if (u16(&data[at + 8u]) & 1u)
                return "The download is locked with a password.";
            at += 46u + nameSize + extraSize + noteSize;
            if (!entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\'))
                continue; // a folder
            entry.name = safeName(entry.name);
            if (entry.name.empty() || entry.packed == 0xFFFFFFFFu || entry.size == 0xFFFFFFFFu || (entry.method != 0u && entry.method != 8u))
                return "The download holds a file this program will not unpack.";
            total += entry.size;
            entries.push_back(std::move(entry));
        }
        if (entries.empty() || total > (2ull << 30))
            return "The download holds nothing to install.";
        // one folder around everything (the release zip's "STAR-v...-windows-x64/"): left out
        {
            const size_t slash = entries.front().name.find('/');
            bool wrapped = slash != std::string::npos;
            const std::string top = wrapped ? entries.front().name.substr(0, slash + 1u) : std::string();
            for (const Entry &entry : entries)
                wrapped = wrapped && entry.name.compare(0, top.size(), top) == 0;
            if (wrapped)
                for (Entry &entry : entries)
                    entry.name.erase(0, top.size());
        }
        bool program = false;
        for (const Entry &entry : entries)
            program = program || entry.name == "STAR.exe";
        if (!program)
            return "The download is not a STAR release (no STAR.exe in it).";

        std::error_code error;
        fs::remove_all(into, error);
        fs::create_directories(into, error);
        if (error)
            return "Could not make a folder for the new version (" + error.message() + ").";
        std::vector<uint8_t> out;
        for (const Entry &entry : entries)
        {
            if (cancel.load())
                return "Stopped.";
            if (static_cast<uint64_t>(entry.local) + 30u > data.size() || u32(&data[entry.local]) != 0x04034B50u)
                return "The download is damaged (" + entry.name + ").";
            const uint64_t start = static_cast<uint64_t>(entry.local) + 30u + u16(&data[entry.local + 26u]) + u16(&data[entry.local + 28u]);
            if (start + entry.packed > data.size())
                return "The download is damaged (" + entry.name + ").";
            const uint8_t *packed = &data[static_cast<size_t>(start)];
            const uint8_t *bytes = packed;
            if (entry.method == 8u)
            {
                out.resize(entry.size);
                if (entry.size != 0u && tinfl_decompress_mem_to_mem(out.data(), out.size(), packed, entry.packed, 0) != entry.size)
                    return "The download is damaged (" + entry.name + " does not unpack).";
                bytes = out.data();
            }
            else if (entry.packed != entry.size)
                return "The download is damaged (" + entry.name + ").";
            if (crc32(bytes, entry.size) != entry.crc)
                return "The download is damaged (" + entry.name + " fails its check).";
            const fs::path target = into / fromUtf8(entry.name);
            fs::create_directories(target.parent_path(), error);
            std::ofstream file(target, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(bytes), entry.size);
            file.close();
            if (!file)
                return "Could not write " + entry.name + " (is the disk full?).";
            files.push_back(entry.name);
        }
        return {};
    }

    std::string install(const fs::path &from, const std::vector<std::string> &files, const fs::path &program)
    {
        struct Step
        {
            fs::path target, old;
            bool hadOld;
        };
        std::vector<Step> steps;
        std::error_code error;
        const auto putBack = [&]
        {
            for (auto step = steps.rbegin(); step != steps.rend(); ++step)
            {
                std::error_code ignored;
                fs::remove(step->target, ignored);
                if (step->hadOld)
                    fs::rename(step->old, step->target, ignored);
            }
        };
        // The program itself goes in last (the .dll files, then STAR.exe): if the PC stops in the middle of this, what
        // is left starts as the version before, with at worst some newer resources. (The whole of it takes a fraction
        // of a second.)
        std::vector<std::string> ordered = files;
        const auto rank = [](const std::string &file)
        {
            const std::string name = lower(file);
            return name == "star.exe" ? 2 : endsWith(name, ".dll") || endsWith(name, ".exe") ? 1 : 0;
        };
        std::stable_sort(ordered.begin(), ordered.end(), [&](const std::string &a, const std::string &b) { return rank(a) < rank(b); });
        for (const std::string &file : ordered)
        {
            if (playersOwn(file))
                continue;
            const fs::path target = program / fromUtf8(file), source = from / fromUtf8(file);
            // the settings are the player's: a new version's file only where there is none
            if (lower(file) == "star settings.ini" && fs::exists(target, error))
                continue;
            fs::create_directories(target.parent_path(), error);
            Step step{target, fs::path(target).concat(kOld), false};
            if (fs::exists(target, error))
            {
                fs::remove(step.old, error); // (one left by an update before)
                fs::rename(target, step.old, error);
                if (error)
                {
                    putBack();
                    return "Could not replace " + file + " (" + error.message() + "). Nothing was changed.";
                }
                step.hadOld = true;
            }
            steps.push_back(step);
            fs::rename(source, target, error);
            if (error)
            {
                error.clear();
                fs::copy_file(source, target, fs::copy_options::overwrite_existing, error);
            }
            if (error)
            {
                putBack();
                return "Could not put " + file + " in place (" + error.message() + "). Nothing was changed.";
            }
        }
        return {};
    }

    void cleanUp(const fs::path &program)
    {
        std::error_code error;
        const fs::path folder = program / "update";
        if (!fs::is_directory(folder, error))
            return;
        bool all = true;
        const std::string old = kOld;
        for (fs::recursive_directory_iterator it(program, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error))
        {
            const std::u8string name8 = it->path().filename().u8string();
            const std::string name(name8.begin(), name8.end());
            if (it->is_directory(error))
            {
                if (it.depth() == 0 && playersOwn(name))
                    it.disable_recursion_pending();
                continue;
            }
            if (name.size() > old.size() && name.compare(name.size() - old.size(), old.size(), old) == 0)
            {
                std::error_code failed;
                fs::remove(it->path(), failed);
                all = all && !failed; // (the version before may still be closing: the next start tries again)
            }
        }
        if (all)
            fs::remove_all(folder, error);
    }

    // --- the session ---
    struct Session::Core
    {
        fs::path program;
        std::string current, api;
        bool test = false;
        std::mutex mutex;
        View view;
        std::atomic<bool> cancel{false}, finished{true};

        void set(Stage stage, const std::string &note = std::string())
        {
            std::lock_guard<std::mutex> lock(mutex);
            view.stage = stage;
            view.note = note;
        }

        void check()
        {
            set(Stage::Checking);
            std::string body;
            std::string problem = httpGet(api, &body, nullptr, 4u << 20, [this](uint64_t, uint64_t) { return !cancel.load(); });
            Release release;
            if (problem.empty())
                problem = parseRelease(body, release, test);
            if (!problem.empty())
            {
                std::printf("[update] the check failed: %s\n", problem.c_str());
                std::fflush(stdout);
                set(Stage::CheckFailed, problem);
                return;
            }
            const bool newer = compareVersions(release.version, current) > 0;
            std::printf("[update] this is version %s, the latest release is %s%s\n", current.c_str(), release.version.c_str(),
                        newer ? (release.url.empty() ? " (without a Windows download)" : "") : ": up to date");
            std::fflush(stdout);
            std::lock_guard<std::mutex> lock(mutex);
            view.release = release;
            view.stage = newer ? Stage::Available : Stage::UpToDate;
            view.note.clear();
        }

        std::string fetch(const Release &release, const fs::path &folder)
        {
            std::error_code error;
            fs::create_directories(folder, error);
            if (error)
                return "Could not write into the program's folder (" + error.message() + ").";
            const fs::path zip = folder / "download.zip";
            {
                std::lock_guard<std::mutex> lock(mutex);
                view.stage = Stage::Downloading;
                view.done = 0;
                view.total = release.size;
            }
            std::string problem = httpGet(release.url, nullptr, &zip, release.size ? release.size : (512ull << 20), [this](uint64_t done, uint64_t total)
                                          {
                                              std::lock_guard<std::mutex> lock(mutex);
                                              view.done = done;
                                              if (total)
                                                  view.total = total;
                                              return !cancel.load();
                                          });
            if (cancel.load())
                return "Stopped.";
            if (!problem.empty())
                return problem;
            set(Stage::Installing);
            const uint64_t size = fs::file_size(zip, error);
            if (error || (release.size && size != release.size))
                return "The download came out the wrong size. Try again.";
            if (!release.sha256.empty())
            {
                Sha256 hash;
                std::ifstream in(zip, std::ios::binary);
                std::vector<char> block(1u << 20);
                while (in)
                {
                    in.read(block.data(), static_cast<std::streamsize>(block.size()));
                    hash.update(block.data(), static_cast<size_t>(in.gcount()));
                }
                if (hash.finishHex() != release.sha256)
                    return "The download does not match the published one (its check failed). Nothing was changed.";
            }
            std::printf("[update] the download is whole: %llu bytes, %s\n", static_cast<unsigned long long>(size),
                        release.sha256.empty() ? "no published check to hold it against" : "matches the published check (SHA-256)");
            std::fflush(stdout);
            std::vector<std::string> files;
            problem = unpack(zip, folder / "new", files, cancel);
            if (!problem.empty())
                return problem;
            if (cancel.load())
                return "Stopped.";
            problem = install(folder / "new", files, program);
            if (!problem.empty())
                return problem;
            fs::remove_all(folder / "new", error);
            fs::remove(zip, error);
            std::printf("[update] version %s is in place (%zu files); starting again\n", release.version.c_str(), files.size());
            std::fflush(stdout);
            return {};
        }

        void update()
        {
            Release release;
            {
                std::lock_guard<std::mutex> lock(mutex);
                release = view.release;
            }
            const fs::path folder = program / "update";
            const std::string problem = fetch(release, folder);
            if (problem.empty())
            {
                set(Stage::Installed);
                return;
            }
            std::error_code error;
            fs::remove_all(folder, error); // (nothing of the program was changed: install puts everything back)
            if (problem == "Stopped.")
                set(Stage::Available);
            else
            {
                std::printf("[update] the update failed: %s\n", problem.c_str());
                std::fflush(stdout);
                set(Stage::Failed, problem);
            }
        }
    };

    Session::Session(fs::path program, std::string current) : m_core(std::make_shared<Core>())
    {
        m_core->program = std::move(program);
        m_core->current = std::move(current);
        m_core->api = std::string("https://api.github.com/repos/") + kRepository + "/releases/latest";
        // Tests: STAR_UPDATE_API=<url> asks there instead (a local server; its zip may then lie anywhere),
        // STAR_UPDATE_AS=<version> has the program take itself for that version.
        // ("real": GitHub itself, from a test of the launcher, which otherwise asks nobody)
        if (const char *api = std::getenv("STAR_UPDATE_API"); api && *api && std::strcmp(api, "real") != 0)
        {
            m_core->api = api;
            m_core->test = true;
        }
        if (const char *as = std::getenv("STAR_UPDATE_AS"); as && *as)
            m_core->current = as;
    }

    Session::~Session()
    {
        m_core->cancel.store(true);
        if (!m_thread.joinable())
            return;
        // files being put in place are waited for; a request that hangs is not
        for (int i = 0; i < 40 && !m_core->finished.load(); ++i)
        {
            if (view().stage == Stage::Installing)
                i = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (m_core->finished.load())
            m_thread.join();
        else
            m_thread.detach();
    }

    Session::View Session::view() const
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        return m_core->view;
    }

    const std::string &Session::current() const { return m_core->current; }

    bool Session::busy() const
    {
        const Stage stage = view().stage;
        return !m_core->finished.load() || stage == Stage::Checking || stage == Stage::Downloading || stage == Stage::Installing;
    }

    void Session::launch(bool update)
    {
        if (busy())
            return;
        if (m_thread.joinable())
            m_thread.join();
        m_core->cancel.store(false);
        m_core->finished.store(false);
        m_core->set(update ? Stage::Downloading : Stage::Checking);
        m_thread = std::thread([core = m_core, update]
                               {
                                   if (update)
                                       core->update();
                                   else
                                       core->check();
                                   core->finished.store(true);
                               });
    }

    void Session::check() { launch(false); }

    void Session::start()
    {
        const View now = view();
        if ((now.stage == Stage::Available || now.stage == Stage::Failed) && !now.release.url.empty())
            launch(true);
    }

    void Session::cancel() { m_core->cancel.store(true); }
}
