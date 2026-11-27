#include "runtime/ps2_async_vif.h"
#include "runtime/ps2_memory.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "ThreadPriority.h"
#include "ThreadNaming.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <algorithm>
#include <xmmintrin.h>

void ps2xGsReleaseStall();
bool ps2xGsStallNeedsRelease();
extern thread_local int g_gsTracePath; // gs_frontend.cpp

namespace ps2x::asyncvif
{
    namespace
    {
        constexpr size_t kMaxQueuedJobs = 64u;
        constexpr size_t kMaxQueuedGifBytes = 8u * 1024u * 1024u;

        struct Job
        {
            std::vector<Item> items;
            uint32_t mxcsr = 0u;
            uint32_t fbrst = 0u;
            uint64_t tag = 0u; // STAR: setJobTag
            std::vector<uint8_t> note; // STAR: setJobNoteProvider
        };
        std::atomic<uint64_t> s_jobTag{0u};
        uint64_t s_currentJobTag = 0u; // the worker's
        JobNoteProvider s_jobNoteProvider;       // set once, before the game runs
        std::vector<uint8_t> s_currentJobNote;   // the worker's

        struct GifPacketRef
        {
            uint32_t offset = 0u;
            uint32_t size = 0u;
            uint32_t mxcsr = 0u;
            int path = -1; // GIF path that produced it (g_gsTracePath), for PS2X_GS_TRACE and PS2X_MTVU_TRACE
        };

        struct GifChunk
        {
            std::vector<uint8_t> data;
            std::vector<GifPacketRef> packets;
        };

        struct GifStage
        {
            std::mutex mutex;
            std::condition_variable workCv;
            std::condition_variable idleCv;
            std::deque<GifChunk> chunks;
            std::vector<GifChunk> spare;
            size_t queuedBytes = 0u;
            bool busy = false;
            bool stop = false;
            GifChunk current;
            std::thread thread;
        };

        constexpr size_t kGifChunkBytes = 256u * 1024u;

        void publishGif(GifStage &stage)
        {
            if (stage.current.packets.empty())
                return;
            GifChunk next;
            {
                std::unique_lock<std::mutex> lock(stage.mutex);
                stage.idleCv.wait(lock, [&]
                                  { return stage.queuedBytes < kMaxQueuedGifBytes; });
                stage.queuedBytes += stage.current.data.size();
                stage.chunks.push_back(std::move(stage.current));
                if (!stage.spare.empty())
                {
                    next = std::move(stage.spare.back());
                    stage.spare.pop_back();
                }
            }
            stage.workCv.notify_one();
            stage.current = std::move(next);
            stage.current.data.clear();
            stage.current.packets.clear();
        }

        struct Worker
        {
            PS2Memory *memory = nullptr;
            std::mutex mutex;
            std::condition_variable workCv;
            std::condition_variable idleCv;
            std::deque<Job> jobs;
            std::vector<ps2x::ByteBuffer> spare;
            bool busy = false;
            bool background = false; // busy with extra passes (setFrameDoubler): not the game's work
            bool stop = false;
            std::thread thread;
            GifStage gif;
        };

        std::function<void(const uint8_t *, uint32_t)> s_gifProcessor;
        thread_local bool t_onGifStage = false;
        std::atomic<bool> s_stallReleaseRequested{false}; // postGifStallRelease -> runGif

        std::atomic<bool> s_defaultEnabled{false};
        std::atomic<bool> s_hostPacingDefault{false};
        std::mutex s_bindMutex;
        std::atomic<Worker *> s_worker{nullptr};
        std::function<uint32_t()> s_fbrstProvider;
        thread_local bool t_onWorker = false;
        std::function<uint64_t()> s_vu1CycleReader;
        std::atomic<bool> s_vu1ReaderReady{false};
        std::atomic<uint64_t> s_vu1Completed{0u};
        thread_local uint32_t t_fbrst = 0u;
        FrameDoublePrepare s_frameDouble; // set before the game starts (setFrameDoubler), read by the worker
        std::atomic<uint64_t> s_extraVuCycles{0u};  // VU1 cycles of the extra passes: not charged to the game (vu1Busy)
        std::atomic<double> s_ownMs{0.0}, s_passMs{0.0}; // frameDoubleTimes
        std::atomic<bool> s_cancelPasses{false};          // the game waits for the worker (sync): no more extra passes

        bool enabled()
        {
            static const int fromEnvironment = []
            {
                const char *value = std::getenv("PS2X_MTVU");
                if (!value || !*value)
                    return -1;
                return *value != '0' ? 1 : 0;
            }();
            return fromEnvironment >= 0 ? fromEnvironment != 0 : s_defaultEnabled.load(std::memory_order_relaxed);
        }

        void runGif(GifStage &stage)
        {
            ThreadNaming::SetCurrentThreadName("GIFThread");
            ThreadPriority::RaiseCurrentThread(ThreadPriority::Level::AboveNormal);
            t_onGifStage = true;
            for (;;)
            {
                GifChunk chunk;
                {
                    std::unique_lock<std::mutex> lock(stage.mutex);
                    // Wake every 2 ms even without work: a SIGNAL stall whose release got lost between threads (the EE
                    // cleared CSR.SIGNAL while this thread was deferring the data) is replayed by the safety net below.
                    const bool woke = stage.workCv.wait_for(lock, std::chrono::milliseconds(2), [&]
                                                            { return stage.stop || !stage.chunks.empty() || s_stallReleaseRequested.load(std::memory_order_acquire); });
                    if (!woke)
                    {
                        lock.unlock();
                        const bool lost = ps2xGsStallNeedsRelease();
                        lock.lock();
                        if (lost)
                        {
                            static std::atomic<uint32_t> s_lostReleases{0u};
                            if (s_lostReleases.fetch_add(1u) < 20u)
                                std::fprintf(stderr, "[mtvu] lost SIGNAL stall release recovered\n");
                            stage.busy = true;
                            lock.unlock();
                            ps2xGsReleaseStall();
                            lock.lock();
                            stage.busy = false;
                            stage.idleCv.notify_all();
                        }
                        continue;
                    }
                    if (s_stallReleaseRequested.exchange(false, std::memory_order_acq_rel))
                    {
                        // Replay outside the lock, on this thread, before any chunk queued after the clear.
                        stage.busy = true;
                        lock.unlock();
                        ps2xGsReleaseStall();
                        lock.lock();
                        stage.busy = false;
                        stage.idleCv.notify_all();
                        continue;
                    }
                    if (stage.stop && stage.chunks.empty())
                        return;
                    chunk = std::move(stage.chunks.front());
                    stage.chunks.pop_front();
                    stage.queuedBytes -= chunk.data.size();
                    stage.busy = true;
                }
                stage.idleCv.notify_all();
                traceEvent('g', static_cast<uint32_t>(chunk.packets.size()));
                for (const GifPacketRef &packet : chunk.packets)
                {
                    _mm_setcsr(packet.mxcsr);
                    g_gsTracePath = packet.path;
                    if (s_gifProcessor)
                        s_gifProcessor(chunk.data.data() + packet.offset, packet.size);
                }
                g_gsTracePath = -1;
                traceEvent('G');
                {
                    std::lock_guard<std::mutex> lock(stage.mutex);
                    if (stage.spare.size() < 8u)
                        stage.spare.push_back(std::move(chunk));
                    stage.busy = false;
                }
                stage.idleCv.notify_all();
            }
        }

    namespace
    {
        std::string s_passInfo; // the worker thread only
        uint32_t vifDigest(const VIFRegisters &r)
        {
            uint32_t h = 2166136261u;
            const uint8_t *p = reinterpret_cast<const uint8_t *>(&r);
            for (size_t i = 0; i < sizeof(r); ++i)
                h = (h ^ p[i]) * 16777619u;
            return h;
        }
    }

        void run(Worker &worker)
        {
            ThreadNaming::SetCurrentThreadName("VUThread");
            ThreadPriority::RaiseCurrentThread(ThreadPriority::Level::Highest);
            t_onWorker = true;
            const char *statsValue = std::getenv("PS2X_MTVU_STATS");
            const bool stats = statsValue && *statsValue && *statsValue != '0';
            using Clock = std::chrono::steady_clock;
            Clock::duration busyTime{};
            Clock::time_point windowStart = Clock::now();
            uint64_t windowField = worker.memory->gs().vsyncTick.load();
            uint64_t windowCycles = 0u;
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(worker.mutex);
                    worker.workCv.wait(lock, [&]
                                       { return worker.stop || !worker.jobs.empty(); });
                    if (worker.stop && worker.jobs.empty())
                        return;
                    job = std::move(worker.jobs.front());
                    worker.jobs.pop_front();
                    worker.busy = true;
                }
                worker.idleCv.notify_all();
                const Clock::time_point jobStart = Clock::now();
                traceEvent('S');
                _mm_setcsr(job.mxcsr);
                t_fbrst = job.fbrst;
                PS2Memory &memory = *worker.memory;
                // STAR 2026-11-23: PS2X_VIF_DUMP=<tick>:<file> writes the items of that vsync tick as they are
                // executed (per item: kind u32, size u32, bytes), for studying a frame's VIF1 / GIF stream.
                static const struct VifDump
                {
                    uint64_t tick = ~0ull;
                    std::FILE *file = nullptr;
                    VifDump()
                    {
                        const char *v = std::getenv("PS2X_VIF_DUMP");
                        const char *colon = v ? std::strchr(v, ':') : nullptr;
                        if (colon)
                        {
                            tick = std::strtoull(v, nullptr, 10);
                            file = std::fopen(colon + 1, "wb");
                        }
                    }
                } vifDump;
                if (vifDump.file && memory.gs().vsyncTick.load() == vifDump.tick)
                {
                    for (Item &item : job.items)
                    {
                        const uint32_t header[2] = {static_cast<uint32_t>(item.kind), static_cast<uint32_t>(item.data.size())};
                        std::fwrite(header, sizeof(header), 1, vifDump.file);
                        std::fwrite(item.data.data(), 1, item.data.size(), vifDump.file);
                    }
                    std::fflush(vifDump.file);
                }
                const auto execute = [&memory](std::vector<Item> &items)
                {
                    for (Item &item : items)
                    {
                        if (item.data.empty())
                            continue;
                        if (item.kind == ItemKind::GifPath3)
                            memory.submitGifPacket(GifPathId::Path3, item.data.data(), static_cast<uint32_t>(item.data.size()), false);
                        else
                            memory.processVIF1Data(item.data.data(), static_cast<uint32_t>(item.data.size()));
                    }
                };
                // STAR 2026-11-23: extra passes over the frame (ps2_async_vif.h, setFrameDoubler).
                const auto marker = [&memory](uint32_t kind, uint32_t id)
                {
                    alignas(16) const uint64_t packet[4] = {0x1000000000008001ull, 0xEull, // one A+D register, EOP
                                                            (static_cast<uint64_t>(kFrameDoubleMagic) << 32) | kind | (static_cast<uint64_t>(id & 0xFFFFFFu) << 8),
                                                            0x0Full};
                    memory.submitGifPacket(GifPathId::Path3, reinterpret_cast<const uint8_t *>(packet), sizeof(packet), false);
                    if (memory.m_gifArbiter)
                        memory.m_gifArbiter->drain();
                };
                // PS2X_FRAME_JOB_LOG=<n>: the first n jobs after two seconds of 3D frames: when each came and what it holds.
                {
                    static int left = []
                    { const char *v = std::getenv("PS2X_FRAME_JOB_LOG"); return v ? std::atoi(v) : 0; }();
                    static uint64_t seen = 0;
                    static auto last = Clock::now();
                    if (left > 0 && ++seen > 600u)
                    {
                        --left;
                        const auto now = Clock::now();
                        std::string text;
                        for (const Item &item : job.items)
                            text += (item.kind == ItemKind::GifPath3 ? " gif:" : " vif:") + std::to_string(item.data.size());
                        std::fprintf(stderr, "[job] +%.2f ms, tick %llu:%s%c", std::chrono::duration<double, std::milli>(now - last).count(),
                                     static_cast<unsigned long long>(memory.gs().vsyncTick.load()), text.c_str(), 10);
                        std::fflush(stderr);
                        last = now;
                    }
                    else if (left > 0)
                        last = Clock::now();
                }
                static FramePlan plan;
                plan.shown.clear();
                plan.passes.clear();
                plan.blanks.clear();
                plan.beforePass = nullptr;
                plan.own.ids.clear();
                plan.own.patches.clear();
                plan.beforeOwn = nullptr;
                s_currentJobTag = job.tag;
                s_currentJobNote = std::move(job.note);
                if (s_frameDouble)
                {
                    const auto f0 = Clock::now();
                    s_frameDouble(job.items, plan);
                    static const bool prepareLog = std::getenv("PS2X_FRAME_DOUBLE_LOG") != nullptr;
                    if (prepareLog && !plan.passes.empty())
                    {
                        static double ms = 0.0;
                        static uint64_t n = 0;
                        ms += std::chrono::duration<double, std::milli>(Clock::now() - f0).count();
                        if (++n % 100u == 0u)
                        {
                            std::fprintf(stderr, "[fd] worker: planning a frame's passes takes %.2f ms%c", ms / 100.0, 10);
                            std::fflush(stderr);
                            ms = 0.0;
                        }
                    }
                }
                // PS2X_FRAME_DOUBLE_LOG: how long the game's own run of a job and each extra pass take on this thread.
                static const bool passLog = std::getenv("PS2X_FRAME_DOUBLE_LOG") != nullptr;
                static double ownMs = 0.0, passMs = 0.0, prepareMs = 0.0;
                static uint64_t ownCount = 0, passCount = 0;
                const auto t0 = Clock::now();
                const uint32_t vifAtOwnStart = vifDigest(memory.vif1_regs);
                // STAR 2026-11-24: the renderer is told where a run over a frame begins and ends (markers 5 and 6, with 3
                // and 4 of the passes): a vsync that comes meanwhile waits for the end (pcsx2gs Bridge.cpp, Gif).
                const bool framed = !plan.shown.empty() || !plan.passes.empty();
                if (framed)
                    marker(5u, 0u);
                if (!plan.own.patches.empty())
                {
                    // STAR: the own run with the frame doubler's patches (FramePlan::own)
                    if (plan.beforeOwn)
                        plan.beforeOwn(plan.own);
                    for (const FramePatch &patch : plan.own.patches)
                        if (patch.item < job.items.size() && static_cast<size_t>(patch.offset) + sizeof(patch.bytes) <= job.items[patch.item].data.size())
                            std::memcpy(job.items[patch.item].data.data() + patch.offset, patch.bytes, sizeof(patch.bytes));
                }
                execute(job.items);
                if (memory.m_gifArbiter)
                    memory.m_gifArbiter->drain();
                // STAR 2026-11-24: also when the own run is the frame's only picture (FramePlan::own and no pass: every
                // sixth refresh or fewer). Then no pass is run, and without this both times stood still at whatever
                // had sent the rate down: after an F1 catch (the GS busy for a second and more) the user's rate
                // "was reduced and did not seem to return". An own run with patches costs what a pass costs, so it
                // stands in for one. And a run of over 50 ms is a stall of the renderer, not the work's time.
                if (!plan.passes.empty() || !plan.own.patches.empty())
                {
                    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                    if (ms < 50.0)
                    {
                        const double before = s_ownMs.load(std::memory_order_relaxed);
                        s_ownMs.store(before == 0.0 ? ms : before + 0.1 * (ms - before), std::memory_order_relaxed);
                        if (plan.passes.empty())
                        {
                            const double pass = s_passMs.load(std::memory_order_relaxed);
                            s_passMs.store(pass == 0.0 ? ms : pass + 0.1 * (ms - pass), std::memory_order_relaxed);
                        }
                    }
                }
                if (passLog && !plan.passes.empty())
                {
                    static double movingMs = 0.0;
                    static uint64_t movingCount = 0;
                    movingMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                    if (++movingCount % 100u == 0u)
                    {
                        std::fprintf(stderr, "[fd] worker: the game's own run of a frame that gets passes: %.2f ms%c", movingMs / 100.0, 10);
                        std::fflush(stderr);
                        movingMs = 0.0;
                    }
                }
                if (passLog && !plan.shown.empty() + !plan.passes.empty() != 0)
                {
                    ownMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                    if (++ownCount % 300u == 0u)
                    {
                        std::fprintf(stderr, "[fd] worker: the game's own run %.2f ms, an extra pass %.2f ms (%llu passes over %llu frames)\n",
                                     ownMs / 300.0, passCount ? passMs / static_cast<double>(passCount) : 0.0,
                                     static_cast<unsigned long long>(passCount), 300ull);
                        std::fflush(stderr);
                        ownMs = passMs = 0.0;
                        passCount = 0;
                    }
                }
                if (s_vu1ReaderReady.load(std::memory_order_acquire))
                    s_vu1Completed.store(s_vu1CycleReader() - s_extraVuCycles.load(std::memory_order_relaxed), std::memory_order_release);
                for (size_t i = 0; i < plan.shown.size(); ++i)
                    marker(i == 0 ? 1u : 2u, plan.shown[i]);
                if (framed && plan.passes.empty())
                    marker(6u, 0u);
                if (!plan.passes.empty())
                {
                    publishGif(worker.gif); // the game's own frame is on its way before the passes start
                    static const bool foreground = std::getenv("PS2X_FRAME_DOUBLE_FOREGROUND") != nullptr; // experiment
                    if (!foreground)
                    {
                        std::lock_guard<std::mutex> lock(worker.mutex);
                        worker.background = true;
                    }
                    worker.idleCv.notify_all();
                    const VIFRegisters vifBefore = memory.vif1_regs;
                    static std::vector<uint8_t> vuDataBefore;
                    if (memory.m_vu1Data)
                        vuDataBefore.assign(memory.m_vu1Data, memory.m_vu1Data + 0x4000u);
                    static std::vector<Item> work;
                    work.resize(job.items.size());
                    for (size_t i = 0; i < job.items.size(); ++i)
                    {
                        work[i].kind = job.items[i].kind;
                        work[i].data.assign(job.items[i].data.begin(), job.items[i].data.end());
                    }
                    for (const FrameBlank &blank : plan.blanks)
                        if (blank.item < work.size() && static_cast<size_t>(blank.offset) + blank.size <= work[blank.item].data.size())
                            std::memset(work[blank.item].data.data() + blank.offset, 0, blank.size);
                    marker(3u, 0u);
                    char info[96];
                    std::snprintf(info, sizeof(info), "vif at own start %08x, after it %08x; %zu passes planned", vifAtOwnStart,
                                  vifDigest(memory.vif1_regs), plan.passes.size());
                    s_passInfo = info;
                    size_t passesRun = 0;
                    const char *gaveUp = "";
                    for (size_t passIndex = 0; passIndex < plan.passes.size(); ++passIndex)
                    {
                        FramePass &pass = plan.passes[passIndex];
                        bool next;
                        {
                            std::lock_guard<std::mutex> lock(worker.mutex);
                            next = !worker.jobs.empty() || worker.stop;
                        }
                        if (next || s_cancelPasses.load(std::memory_order_acquire))
                        {
                            gaveUp = next ? " (the next job came)" : " (the game waited for the worker)";
                            break; // the game's next job is here, or the game waits for the worker: the rest is given up
                        }
                        ++passesRun;
                        std::snprintf(info, sizeof(info), ", pass %u at vif %08x", pass.ids.empty() ? 0u : pass.ids[0], vifDigest(memory.vif1_regs));
                        s_passInfo += info;
                        static const bool plainPasses = std::getenv("PS2X_FRAME_DOUBLE_PLAIN") != nullptr; // experiment: no patches
                        if (plan.beforePass)
                            plan.beforePass(passIndex, pass); // STAR: last-moment patches
                        if (!plainPasses)
                        for (const FramePatch &patch : pass.patches)
                            if (patch.item < work.size() && static_cast<size_t>(patch.offset) + sizeof(patch.bytes) <= work[patch.item].data.size())
                                std::memcpy(work[patch.item].data.data() + patch.offset, patch.bytes, sizeof(patch.bytes));
                        const uint64_t cyclesBefore = s_vu1ReaderReady.load(std::memory_order_acquire) ? s_vu1CycleReader() : 0u;
                        const auto p0 = Clock::now();
                        execute(work);
                        const auto p1 = Clock::now();
                        if (memory.m_gifArbiter)
                            memory.m_gifArbiter->drain();
                        passMs += std::chrono::duration<double, std::milli>(Clock::now() - p0).count();
                        ++passCount;
                        {
                            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - p0).count();
                            const double before = s_passMs.load(std::memory_order_relaxed);
                            if (ms < 50.0) // (a stall of the renderer is not the work's time)
                                s_passMs.store(before == 0.0 ? ms : before + 0.1 * (ms - before), std::memory_order_relaxed);
                        }
                        if (passLog)
                        {
                            static double execMs = 0.0, patchMs = 0.0;
                            static uint64_t n = 0;
                            execMs += std::chrono::duration<double, std::milli>(p1 - p0).count();
                            if (++n % 1000u == 0u)
                            {
                                std::fprintf(stderr, "[fd] worker: of an extra pass, executing the items takes %.2f ms%c", execMs / 1000.0, 10);
                                std::fflush(stderr);
                                execMs = 0.0;
                            }
                        }
                        if (s_vu1ReaderReady.load(std::memory_order_acquire))
                            s_extraVuCycles.fetch_add(s_vu1CycleReader() - cyclesBefore, std::memory_order_relaxed);
                        for (size_t i = 0; i < pass.ids.size(); ++i)
                            marker(i == 0 ? 1u : 2u, pass.ids[i]);
                        publishGif(worker.gif);
                    }
                    marker(4u, 0u);
                    std::snprintf(info, sizeof(info), "; %zu run%s", passesRun, gaveUp);
                    s_passInfo += info;
                    if (passLog)
                    {
                        static double ms = 0.0;
                        static uint64_t n = 0;
                        ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                        if (++n % 100u == 0u)
                        {
                            std::fprintf(stderr, "[fd] worker: a frame with passes, from the start of its own run to the end of the last pass: %.2f ms%c", ms / 100.0, 10);
                            std::fflush(stderr);
                            ms = 0.0;
                        }
                    }
                    memory.vif1_regs = vifBefore;
                    if (memory.m_vu1Data && vuDataBefore.size() == 0x4000u)
                        std::memcpy(memory.m_vu1Data, vuDataBefore.data(), 0x4000u);
                }
                publishGif(worker.gif);
                traceEvent('E');
                if (stats)
                {
                    const Clock::time_point now = Clock::now();
                    busyTime += now - jobStart;
                    if (now - windowStart >= std::chrono::seconds(2))
                    {
                        const uint64_t field = worker.memory->gs().vsyncTick.load();
                        const double window = std::chrono::duration<double>(now - windowStart).count();
                        const double busy = std::chrono::duration<double>(busyTime).count();
                        const uint64_t fields = field > windowField ? field - windowField : 1u;
                        const uint64_t cycles = s_vu1Completed.load(std::memory_order_acquire);
                        std::fprintf(stderr, "[mtvu] field %llu: %.1f fields/s, VU busy %.0f%%, %.2f ms VU work per field, %.2f M VU cycles per field\n",
                                     static_cast<unsigned long long>(field), fields / window, 100.0 * busy / window, 1000.0 * busy / fields,
                                     static_cast<double>(cycles >= windowCycles ? cycles - windowCycles : cycles) / fields / 1000000.0);
                        std::fflush(stderr);
                        busyTime = {};
                        windowStart = now;
                        windowField = field;
                        windowCycles = cycles;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(worker.mutex);
                    for (Item &item : job.items)
                    {
                        if (worker.spare.size() < 32u && item.data.capacity() != 0u)
                        {
                            item.data.clear();
                            worker.spare.push_back(std::move(item.data));
                        }
                    }
                    worker.busy = false;
                    worker.background = false;
                    s_cancelPasses.store(false, std::memory_order_release);
                }
                worker.idleCv.notify_all();
            }
        }
    }

    void setJobTag(uint64_t tag)
    {
        s_jobTag.store(tag, std::memory_order_relaxed);
    }

    uint64_t currentJobTag()
    {
        return s_currentJobTag;
    }

    void setJobNoteProvider(JobNoteProvider provider)
    {
        s_jobNoteProvider = std::move(provider);
    }

    const std::vector<uint8_t> &currentJobNote()
    {
        return s_currentJobNote;
    }

    void setFrameDoubler(FrameDoublePrepare prepare)
    {
        s_frameDouble = std::move(prepare);
    }

    bool onlyExtraPasses(PS2Memory &memory)
    {
        Worker *worker = s_worker;
        if (!worker || worker->memory != &memory || t_onWorker)
            return false;
        std::lock_guard<std::mutex> lock(worker->mutex);
        return worker->busy && worker->background && worker->jobs.empty();
    }

    std::string takePassInfo()
    {
        std::string out;
        out.swap(s_passInfo);
        return out;
    }

    void frameDoubleTimes(double &ownMs, double &passMs)
    {
        ownMs = s_ownMs.load(std::memory_order_relaxed);
        passMs = s_passMs.load(std::memory_order_relaxed);
    }

    void setDefaultEnabled(bool value)
    {
        s_defaultEnabled.store(value, std::memory_order_relaxed);
    }

    void setHostPacingDefault(bool value)
    {
        s_hostPacingDefault.store(value, std::memory_order_relaxed);
    }

    void setFbrstProvider(std::function<uint32_t()> provider)
    {
        std::lock_guard<std::mutex> lock(s_bindMutex);
        s_fbrstProvider = std::move(provider);
    }

    bool onWorker()
    {
        return t_onWorker;
    }

    bool onAsyncThread()
    {
        return t_onWorker || t_onGifStage;
    }

    void traceEvent(char code, uint32_t arg) noexcept
    {
        struct Record
        {
            int64_t us;
            uint32_t arg;
            char code;
            char thread;
        };
        constexpr size_t kCapacity = 200000u;
        static const double delay = [] { const char *v = std::getenv("PS2X_MTVU_TRACE"); return v && *v ? std::atof(v) : -1.0; }();
        if (delay < 0.0)
            return;
        static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        static Record *records = new Record[kCapacity];
        static std::atomic<size_t> next{0u};
        static std::atomic<size_t> written{0u};
        const auto now = std::chrono::steady_clock::now();
        const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        if (us < static_cast<int64_t>(delay * 1e6))
            return;
        const size_t index = next.fetch_add(1u, std::memory_order_relaxed);
        if (index >= kCapacity)
            return;
        // Thread letters: v = VUThread, g = GIFThread, e = any other (in practice the game thread).
        const char thread = t_onWorker ? 'v' : t_onGifStage ? 'g' : 'e';
        records[index] = Record{us, arg, code, thread};
        if (written.fetch_add(1u, std::memory_order_acq_rel) + 1u != kCapacity)
            return;
        if (FILE *out = std::fopen("mtvu_trace.txt", "w"))
        {
            for (size_t i = 0; i < kCapacity; ++i)
                std::fprintf(out, "%lld %c %x %c\n", static_cast<long long>(records[i].us), records[i].code, records[i].arg,
                             records[i].thread);
            std::fclose(out);
            std::fprintf(stderr, "[mtvu] trace written to mtvu_trace.txt\n");
        }
    }

    void setGifProcessor(std::function<void(const uint8_t *, uint32_t)> processor)
    {
        std::lock_guard<std::mutex> lock(s_bindMutex);
        s_gifProcessor = std::move(processor);
    }

    bool postGifStallRelease()
    {
        Worker *worker = s_worker;
        if (!worker)
            return false;
        {
            std::lock_guard<std::mutex> lock(worker->gif.mutex);
            s_stallReleaseRequested.store(true, std::memory_order_release);
        }
        worker->gif.workCv.notify_one();
        return true;
    }

    bool submitGif(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!t_onWorker)
            return false;
        Worker *worker = s_worker;
        if (!worker)
            return false;
        GifStage &stage = worker->gif;
        GifChunk &chunk = stage.current;
        GifPacketRef packet;
        packet.offset = static_cast<uint32_t>(chunk.data.size());
        packet.size = sizeBytes;
        packet.mxcsr = _mm_getcsr();
        packet.path = g_gsTracePath;
        if (chunk.data.capacity() < kGifChunkBytes)
            chunk.data.reserve(kGifChunkBytes);
        chunk.data.insert(chunk.data.end(), data, data + sizeBytes);
        chunk.packets.push_back(packet);
        // PATH1 (XGKICK) output goes to the GIF thread while the VU1 job still runs, instead of after it: the GS front
        // end overlaps the microprogram. Order is unchanged (chunks are processed in sequence). PS2X_MTVU_STREAM=0 off.
        static const bool stream = [] { const char *v = std::getenv("PS2X_MTVU_STREAM"); return !(v && *v == '0'); }();
        if (chunk.data.size() >= kGifChunkBytes || (stream && packet.path == 1 && chunk.data.size() >= 32u * 1024u))
            publishGif(stage);
        return true;
    }

    uint32_t currentFbrst()
    {
        return t_fbrst;
    }

    bool active(PS2Memory &memory)
    {
        if (t_onWorker)
            return false;
        Worker *worker = s_worker;
        if (worker)
            return worker->memory == &memory;
        if (!enabled())
            return false;
        std::lock_guard<std::mutex> lock(s_bindMutex);
        if (!s_worker)
        {
            auto *created = new Worker();
            created->memory = &memory;
            created->gif.thread = std::thread([created]
                                              { runGif(created->gif); });
            created->thread = std::thread([created]
                                          { run(*created); });
            s_worker.store(created);
        }
        return s_worker.load()->memory == &memory;
    }

    ps2x::ByteBuffer takeBuffer()
    {
        Worker *worker = s_worker;
        if (!worker)
            return {};
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (worker->spare.empty())
            return {};
        ps2x::ByteBuffer buffer = std::move(worker->spare.back());
        worker->spare.pop_back();
        return buffer;
    }

    void enqueue(PS2Memory &memory, std::vector<Item> &&items)
    {
        Worker *worker = s_worker;
        if (!worker || worker->memory != &memory || items.empty())
            return;
        Job job;
        job.items = std::move(items);
        job.mxcsr = _mm_getcsr();
        for (const Item &item : job.items)
            if (item.kind == ItemKind::Vif1 && item.data.size() >= 4096u)
            {
                job.tag = s_jobTag.exchange(0u, std::memory_order_relaxed); // STAR: the game thread's tag for this frame
                if (s_jobNoteProvider)
                    s_jobNoteProvider(job.note);
                if (static const bool when = std::getenv("STAR_VIEW_WHEN") != nullptr; when)
                    std::printf("[when] job sent, tag %llu%c", static_cast<unsigned long long>(job.tag), 10);
                break;
            }
        {
            std::lock_guard<std::mutex> lock(s_bindMutex);
            job.fbrst = s_fbrstProvider ? s_fbrstProvider() : 0u;
        }
        traceEvent('K', static_cast<uint32_t>(job.items.size()));
        const auto handStart = std::chrono::steady_clock::now();
        {
            std::unique_lock<std::mutex> lock(worker->mutex);
            worker->idleCv.wait(lock, [&]
                                { return worker->jobs.size() < kMaxQueuedJobs; });
            worker->jobs.push_back(std::move(job));
        }
        worker->workCv.notify_one();
        diagWaited(3, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - handStart).count());
    }

    namespace
    {
        struct Vu1Timing
        {
            Vu1TimingHooks hooks;
            bool enabled = false;
            double scale = 1.0;
            bool primed = false;
            uint64_t accounted = 0;
            uint64_t busyUntil = 0;
            bool kickPending = false;
            uint64_t kickCycle = 0;
            bool blocking = false;
            bool hostPacing = false;
            uint64_t hostKickCycle = 0u;
            std::chrono::steady_clock::time_point hostKickTime{};
        };

        Vu1Timing &vu1Timing()
        {
            static Vu1Timing timing;
            return timing;
        }

        void syncVu(PS2Memory &memory)
        {
            if (t_onWorker)
                return;
            Worker *worker = s_worker;
            if (!worker || worker->memory != &memory)
                return;
            std::unique_lock<std::mutex> lock(worker->mutex);
            worker->idleCv.wait(lock, [&]
                                { return worker->jobs.empty() && !worker->busy; });
        }
    }

    std::atomic<double> s_vu1ScaleRequest{-1.0}; // setVu1TimingScale before or after setVu1Timing; < 0 = none

    void setVu1TimingScale(double scale)
    {
        if (std::getenv("PS2X_VU1_TIMING") || !(scale > 0.0))
            return;
        s_vu1ScaleRequest.store(scale, std::memory_order_relaxed);
        Vu1Timing &timing = vu1Timing();
        if (timing.enabled)
            timing.scale = scale;
    }

    void setVu1Timing(Vu1TimingHooks hooks)
    {
        Vu1Timing &timing = vu1Timing();
        timing = Vu1Timing{};
        timing.hooks = std::move(hooks);
        double scale = 1.0;
        if (const double requested = s_vu1ScaleRequest.load(std::memory_order_relaxed); requested > 0.0)
            scale = requested;
        if (const char *value = std::getenv("PS2X_VU1_TIMING"))
            scale = std::atof(value);
        timing.scale = scale;
        timing.enabled = scale > 0.0 && timing.hooks.eeCycle && timing.hooks.vu1Cycles && timing.hooks.consumeEeCycles;
        const char *blocking = std::getenv("PS2X_VU1_TIMING_SYNC");
        timing.blocking = blocking && *blocking && *blocking != '0';
        const char *hostPacing = std::getenv("PS2X_VU1_HOST_PACING");
        timing.hostPacing = hostPacing && *hostPacing ? *hostPacing != '0' : s_hostPacingDefault.load(std::memory_order_relaxed);
        if (timing.enabled && !s_vu1ReaderReady.load(std::memory_order_acquire))
        {
            s_vu1CycleReader = timing.hooks.vu1Cycles;
            s_vu1ReaderReady.store(true, std::memory_order_release);
        }
    }

    void vu1Kick()
    {
        Vu1Timing &timing = vu1Timing();
        if (!timing.enabled || t_onWorker)
            return;
        if (timing.hostPacing)
        {
            timing.hostKickCycle = timing.hooks.eeCycle();
            timing.hostKickTime = std::chrono::steady_clock::now();
        }
        if (timing.kickPending)
            return;
        timing.kickPending = true;
        timing.kickCycle = timing.hooks.eeCycle();
    }

    bool vblankWaitEnabled()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_MTVU_VBLANK_WAIT"); return !(v && *v == '0'); }();
        return enabled;
    }

    namespace
    {
        // Diagnostic: why pipelineBusy last said yes. 1 = the game's own job runs, 2 = it runs and another waits,
        // 3 = a job waits behind an extra pass, 4 = a job waits (worker not started), 5 = the GIF stage.
        std::atomic<int> s_busyReason{0};
    }

    bool pipelineBusy(PS2Memory &memory)
    {
        Worker *worker = s_worker;
        if (!worker || worker->memory != &memory)
            return false;
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            if ((worker->busy && !worker->background) || !worker->jobs.empty())
            {
                s_busyReason.store(worker->busy ? (worker->background ? 3 : (worker->jobs.empty() ? 1 : 2)) : 4, std::memory_order_relaxed);
                return true;
            }
            if (worker->background)
                return false; // only extra passes are in the pipeline
        }
        std::lock_guard<std::mutex> lock(worker->gif.mutex);
        if (worker->gif.busy || !worker->gif.chunks.empty())
            s_busyReason.store(5, std::memory_order_relaxed);
        return worker->gif.busy || !worker->gif.chunks.empty();
    }

    int lastBusyReason()
    {
        return s_busyReason.load(std::memory_order_relaxed);
    }

    namespace
    {
        std::atomic<long long> s_diagWaited[6];
    }

    void diagWaited(int slot, long long ns)
    {
        if (slot >= 0 && slot < 6)
            s_diagWaited[slot].fetch_add(ns, std::memory_order_relaxed);
    }

    void diagTake(long long out[6])
    {
        for (int i = 0; i < 6; ++i)
            out[i] = s_diagWaited[i].exchange(0, std::memory_order_relaxed);
    }

    bool waitPipelineIdle(PS2Memory &memory, std::chrono::microseconds timeout)
    {
        Worker *worker = s_worker;
        if (!worker || worker->memory != &memory)
            return false;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            std::unique_lock<std::mutex> lock(worker->mutex);
            if (!worker->idleCv.wait_until(lock, deadline, [&] { return worker->jobs.empty() && (!worker->busy || worker->background); }))
                return true;
            if (worker->background)
                return false; // only extra passes are in the pipeline
        }
        {
            GifStage &stage = worker->gif;
            std::unique_lock<std::mutex> lock(stage.mutex);
            if (!stage.idleCv.wait_until(lock, deadline, [&] { return stage.chunks.empty() && !stage.busy; }))
                return true;
        }
        return pipelineBusy(memory);
    }

    bool vu1Busy(PS2Memory &memory)
    {
        Vu1Timing &timing = vu1Timing();
        if (!timing.enabled || t_onWorker)
            return false;
        Worker *worker = s_worker;
        if (timing.hostPacing && !vblankWaitEnabled() && timing.primed && timing.hostKickTime != std::chrono::steady_clock::time_point{} &&
            worker && worker->memory == &memory)
        {
            bool pending;
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                pending = worker->busy || !worker->jobs.empty();
            }
            if (pending)
            {
                const auto elapsed = std::chrono::steady_clock::now() - timing.hostKickTime;
                const uint64_t elapsedCycles = static_cast<uint64_t>(std::chrono::duration<double>(elapsed).count() * EeScheduler::kEeClockHz);
                const uint64_t target = timing.hostKickCycle + elapsedCycles;
                const uint64_t now = timing.hooks.eeCycle();
                if (target > now)
                    timing.hooks.consumeEeCycles(std::min<uint64_t>(target - now, 65536u));
                else
                    std::this_thread::yield();
                return true;
            }
        }
        const bool async = !timing.blocking && timing.primed && worker && worker->memory == &memory;
        if (!async)
            syncVu(memory);
        const uint64_t total = async ? std::max(timing.accounted, s_vu1Completed.load(std::memory_order_acquire)) : timing.hooks.vu1Cycles();
        uint64_t now = timing.hooks.eeCycle();
        if (!timing.primed)
        {
            timing.primed = true;
            timing.accounted = total;
            timing.busyUntil = now;
            timing.kickPending = false;
            return false;
        }
        if (total > timing.accounted)
        {
            const uint64_t work = static_cast<uint64_t>(static_cast<double>(total - timing.accounted) * timing.scale);
            const uint64_t start = timing.kickPending ? std::max(timing.busyUntil, timing.kickCycle) : timing.busyUntil;
            timing.busyUntil = start + work;
            timing.accounted = total;
            timing.kickPending = false;
        }
        if (now >= timing.busyUntil)
            return false;
        timing.hooks.consumeEeCycles(timing.busyUntil - now);
        now = timing.hooks.eeCycle();
        return now < timing.busyUntil;
    }

    namespace
    {
        thread_local int t_syncSite = 0;
        thread_local bool t_syncOwnOnly = false;
    }

    void syncAt(PS2Memory &memory, int site, bool ownOnly)
    {
        t_syncSite = site;
        t_syncOwnOnly = ownOnly;
        sync(memory);
        t_syncSite = 0;
        t_syncOwnOnly = false;
    }

    void sync(PS2Memory &memory)
    {
        if (t_onWorker)
            return;
        Worker *worker = s_worker;
        if (!worker || worker->memory != &memory)
            return;
        traceEvent('W', vu1Timing().hooks.eeCycle ? static_cast<uint32_t>(vu1Timing().hooks.eeCycle() >> 10) : 0u); // EE cycle / 1024
        // PS2X_FRAME_DOUBLE_LOG: how often and how long the game's side waits here, every 300 waits over 0.2 ms.
        static const bool syncLog = std::getenv("PS2X_FRAME_DOUBLE_LOG") != nullptr;
        const auto syncStart = syncLog ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto diagStart = std::chrono::steady_clock::now();
        bool sawBackground = false;
        {
            std::unique_lock<std::mutex> lock(worker->mutex);
            sawBackground = worker->background;
            if (t_syncOwnOnly)
            {
                // The game's own run of the job is what the caller waits for: the extra passes that follow it (the
                // worker says so before it starts them) go on meanwhile. Waiting for them too held the vblank up
                // 12-15 ms in every frame (STAR 2026-11-23: 58.5 fields a second while a scene was being smoothed).
                worker->idleCv.wait(lock, [&]
                                    { return worker->jobs.empty() && (!worker->busy || worker->background); });
                if (worker->background)
                {
                    diagWaited(2, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - diagStart).count());
                    traceEvent('w');
                    return;
                }
            }
            else
            {
                // The worker stops after the pass it is in: the caller needs it at rest. Checked again whenever the
                // worker's state changes, so a wait that began during the game's own run cancels the passes after it.
                worker->idleCv.wait(lock, [&]
                                    {
                                        if (worker->background)
                                        {
                                            sawBackground = true;
                                            s_cancelPasses.store(true, std::memory_order_release);
                                        }
                                        return worker->jobs.empty() && !worker->busy; });
            }
        }
        {
            GifStage &stage = worker->gif;
            std::unique_lock<std::mutex> lock(stage.mutex);
            stage.idleCv.wait(lock, [&]
                              { return stage.chunks.empty() && !stage.busy; });
        }
        diagWaited(2, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - diagStart).count());
        {
            static const bool siteLog = std::getenv("PS2X_VBLANK_WAIT_LOG") != nullptr;
            static std::atomic<int> siteShown{0};
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - diagStart).count();
            if (siteLog && ms > 3.0 && siteShown.fetch_add(1) < 30)
            {
                std::fprintf(stderr, "[fd] sync from ps2_memory.cpp line %d waited %.2f ms (extra passes at its start: %d)%c", t_syncSite, ms,
                             sawBackground ? 1 : 0, 10);
                std::fflush(stderr);
            }
        }
        if (syncLog)
        {
            static std::atomic<uint64_t> waits{0}, longWaits{0}, backgroundWaits{0};
            static std::atomic<int64_t> totalUs{0};
            const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - syncStart).count();
            waits.fetch_add(1);
            totalUs.fetch_add(us);
            if (sawBackground)
            {
                backgroundWaits.fetch_add(1);
                static std::atomic<int> shown{0};
                if (shown.fetch_add(1) < 12)
                {
                    std::fprintf(stderr, "[fd] sync met extra passes: called from ps2_memory.cpp line %d, waited %.2f ms%c", t_syncSite,
                                 static_cast<double>(us) / 1000.0, 10);
                    std::fflush(stderr);
                }
            }
            if (us > 200 && longWaits.fetch_add(1) % 300 == 299)
            {
                std::fprintf(stderr, "[fd] sync: %llu waits (%llu over 0.2 ms, %llu met extra passes), %.1f ms in all%c",
                             static_cast<unsigned long long>(waits.load()), static_cast<unsigned long long>(longWaits.load()),
                             static_cast<unsigned long long>(backgroundWaits.load()), static_cast<double>(totalUs.load()) / 1000.0, 10);
                std::fflush(stderr);
            }
        }
        traceEvent('w');
    }

    void stop(PS2Memory &memory)
    {
        Worker *worker = nullptr;
        {
            std::lock_guard<std::mutex> lock(s_bindMutex);
            if (!s_worker.load() || s_worker.load()->memory != &memory)
                return;
            worker = s_worker;
            s_worker.store(nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            worker->stop = true;
        }
        worker->workCv.notify_all();
        if (worker->thread.joinable())
            worker->thread.join();
        {
            std::lock_guard<std::mutex> lock(worker->gif.mutex);
            worker->gif.stop = true;
        }
        worker->gif.workCv.notify_all();
        if (worker->gif.thread.joinable())
            worker->gif.thread.join();
        delete worker;
    }

    void saveTimingState(ps2x::state::Writer &out)
    {
        Vu1Timing &timing = vu1Timing();
        ps2x::state::fields(out, timing.primed, timing.accounted, timing.busyUntil, timing.kickPending, timing.kickCycle);
    }

    void loadTimingState(ps2x::state::Reader &in)
    {
        Vu1Timing &timing = vu1Timing();
        ps2x::state::fields(in, timing.primed, timing.accounted, timing.busyUntil, timing.kickPending, timing.kickCycle);
        timing.hostKickCycle = timing.hooks.eeCycle ? timing.hooks.eeCycle() : 0u;
        timing.hostKickTime = std::chrono::steady_clock::now();
        if (s_vu1ReaderReady.load(std::memory_order_acquire))
        {
            // STAR 2026-11-24. The VU1's cycle counter in the file counts the extra passes (in-between pictures) of the
            // session that saved it; s_extraVuCycles is THIS session's count of them. Their difference was charged to
            // the game as VU1 work still to be done: after F9 sceGsSyncPath saw VU1 busy for as long as the saving
            // session's passes had run (the user: the game froze, then a burst of sounds; a state saved after 7 s of
            // play ran at 15 frames a second for a while). The save waits for the worker, so nothing is pending in the
            // file: the extra count becomes whatever makes the game's completed cycles equal the accounted ones.
            const uint64_t counter = s_vu1CycleReader();
            const uint64_t extra = counter >= timing.accounted ? counter - timing.accounted : 0u;
            s_extraVuCycles.store(extra, std::memory_order_relaxed);
            s_vu1Completed.store(counter - extra, std::memory_order_release);
        }
    }
}
