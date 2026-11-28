// The update's system part (Windows): one HTTPS request through WinHTTP, the browser, starting the program again.
// Kept apart from the launcher so that <windows.h> and raylib's names never meet.
#include "star/update.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <fstream>
#include <vector>

namespace star::update
{
    namespace
    {
        std::wstring wide(const std::string &text)
        {
            if (text.empty())
                return {};
            const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(std::max(size, 0)), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
            return out;
        }

        struct Handle
        {
            HINTERNET handle = nullptr;
            ~Handle()
            {
                if (handle)
                    WinHttpCloseHandle(handle);
            }
            operator HINTERNET() const { return handle; }
        };

        std::string noConnection(const char *where)
        {
            const DWORD code = GetLastError();
            switch (code)
            {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            case ERROR_WINHTTP_CANNOT_CONNECT:
            case ERROR_WINHTTP_CONNECTION_ERROR:
                return "No connection to the internet (or GitHub cannot be reached).";
            case ERROR_WINHTTP_TIMEOUT:
                return "The connection is too slow or was lost.";
            case ERROR_WINHTTP_SECURE_FAILURE:
                return "The secure connection could not be made (is the PC's date right?).";
            default:
                return std::string("The connection failed (") + where + ", error " + std::to_string(code) + ").";
            }
        }
    }

    std::string httpGet(const std::string &url, std::string *body, const std::filesystem::path *file, uint64_t most,
                        const std::function<bool(uint64_t, uint64_t)> &progress)
    {
        const std::wstring address = wide(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {}, path[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 255;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 2047;
        if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts) || (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP))
            return "The address of the download is not one this program opens.";
        Handle session, connection, request;
        session.handle = WinHttpOpen(L"STAR-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            session.handle = WinHttpOpen(L"STAR-launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            return noConnection("start");
        WinHttpSetTimeouts(session, 8000, 8000, 15000, 30000);
        connection.handle = WinHttpConnect(session, host, parts.nPort, 0);
        if (!connection)
            return noConnection("connect");
        request.handle = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request)
            return noConnection("request");
        // (GitHub's API wants these two; the download itself ignores them)
        const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(request, body ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, body ? static_cast<DWORD>(-1) : 0u, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
            return noConnection("answer");
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 403 || status == 429)
            return "GitHub is not answering this network just now (too many requests). Try again in an hour.";
        if (status == 404)
            return "No release was found there.";
        if (status != 200)
            return "The server answered with error " + std::to_string(status) + ".";
        uint64_t total = 0;
        {
            wchar_t text[32] = {};
            DWORD length = sizeof(text);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, text, &length, WINHTTP_NO_HEADER_INDEX))
                total = _wcstoui64(text, nullptr, 10);
        }
        if (total > most)
            return "The download is larger than it should be.";
        std::ofstream out;
        if (file)
        {
            out.open(*file, std::ios::binary | std::ios::trunc);
            if (!out)
                return "Could not write into the program's folder.";
        }
        std::vector<char> block(64u * 1024u);
        uint64_t done = 0;
        for (;;)
        {
            DWORD got = 0;
            if (!WinHttpReadData(request, block.data(), static_cast<DWORD>(block.size()), &got))
                return noConnection("download");
            if (got == 0)
                break;
            done += got;
            if (done > most)
                return "The download is larger than it should be.";
            if (body)
                body->append(block.data(), got);
            if (file)
            {
                out.write(block.data(), got);
                if (!out)
                    return "Could not write the download (is the disk full?).";
            }
            if (progress && !progress(done, total))
                return "Stopped.";
        }
        if (file)
        {
            out.close();
            if (!out)
                return "Could not write the download (is the disk full?).";
        }
        if (total && done != total)
            return "The download broke off. Try again.";
        return {};
    }

    void openPage(const std::string &url)
    {
        if (url.compare(0, 8, "https://") != 0)
            return;
        ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    bool startProgram(const std::filesystem::path &exe, const std::string &arguments)
    {
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        if (!arguments.empty())
            command += L" " + wide(arguments);
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        PROCESS_INFORMATION process{};
        const std::wstring folder = exe.parent_path().wstring();
        if (!CreateProcessW(exe.wstring().c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &start, &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }
}
#else
namespace star::update
{
    std::string httpGet(const std::string &, std::string *, const std::filesystem::path *, uint64_t, const std::function<bool(uint64_t, uint64_t)> &)
    {
        return "Updates are not built for this system.";
    }
    void openPage(const std::string &) {}
    bool startProgram(const std::filesystem::path &, const std::string &) { return false; }
}
#endif
