#include "star/host_sampler.h"

#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace star
{
    namespace
    {
        std::vector<DWORD> otherThreadIds()
        {
            std::vector<DWORD> ids;
            const DWORD self = GetCurrentThreadId();
            const DWORD pid = GetCurrentProcessId();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return ids;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry))
                if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self)
                    ids.push_back(entry.th32ThreadID);
            CloseHandle(snap);
            return ids;
        }

        std::string symbolName(HANDLE process, DWORD64 address)
        {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, address, &displacement, symbol))
                return symbol->Name;
            char text[32];
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
            return text;
        }
    }

    void sampleHostThreads(int milliseconds)
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (!SymInitialize(process, nullptr, TRUE))
        {
            std::printf("[sampler] SymInitialize failed (%lu)\n", GetLastError());
            return;
        }
        std::map<DWORD, std::map<DWORD64, int>> hits; // thread -> instruction pointer -> samples
        std::map<DWORD, int> totals;
        std::map<DWORD, std::vector<DWORD64>> stacks; // one full call stack per thread (the first sample)
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        int rounds = 0;
        while (std::chrono::steady_clock::now() < end)
        {
            for (DWORD id : otherThreadIds())
            {
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
                if (!thread)
                    continue;
                if (SuspendThread(thread) != static_cast<DWORD>(-1))
                {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(thread, &context))
                    {
                        ++hits[id][context.Rip];
                        ++totals[id];
                        if (!stacks.count(id))
                        {
                            std::vector<DWORD64> &frames = stacks[id];
                            STACKFRAME64 frame{};
                            frame.AddrPC.Offset = context.Rip;
                            frame.AddrPC.Mode = AddrModeFlat;
                            frame.AddrFrame.Offset = context.Rbp;
                            frame.AddrFrame.Mode = AddrModeFlat;
                            frame.AddrStack.Offset = context.Rsp;
                            frame.AddrStack.Mode = AddrModeFlat;
                            CONTEXT walk = context;
                            while (frames.size() < 40 &&
                                   StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &walk, nullptr,
                                               SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
                                   frame.AddrPC.Offset)
                                frames.push_back(frame.AddrPC.Offset);
                        }
                    }
                    ResumeThread(thread);
                }
                CloseHandle(thread);
            }
            ++rounds;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        std::printf("[sampler] %d rounds over %d ms\n", rounds, milliseconds);
        for (const auto &[id, byIp] : hits)
        {
            // Group by function name; a thread parked in the OS (waits, sleeps) shows one ntdll/kernel function.
            std::map<std::string, int> byName;
            for (const auto &[ip, count] : byIp)
                byName[symbolName(process, ip)] += count;
            std::vector<std::pair<int, std::string>> ranked;
            for (const auto &[name, count] : byName)
                ranked.emplace_back(count, name);
            std::sort(ranked.rbegin(), ranked.rend());
            std::printf("[sampler] thread %lu (%d samples):\n", id, totals[id]);
            for (size_t i = 0; i < ranked.size() && i < 6; ++i)
                std::printf("[sampler]     %5.1f%%  %s\n", 100.0 * ranked[i].first / totals[id], ranked[i].second.c_str());
            // The call stack, STAR's own frames only (the OS's wait functions say nothing new).
            int shown = 0;
            for (DWORD64 pc : stacks[id])
            {
                if (SymGetModuleBase64(process, pc) != reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr)))
                    continue;
                std::printf("[sampler]       at %s\n", symbolName(process, pc).c_str());
                if (++shown == 14)
                    break;
            }
        }
        std::fflush(stdout);
        SymCleanup(process);
    }
}

#else

namespace star
{
    void sampleHostThreads(int)
    {
        std::printf("[sampler] host sampling is Windows only\n");
    }
}

#endif
