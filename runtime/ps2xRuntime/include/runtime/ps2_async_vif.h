#pragma once

#include "ps2x/state_io.h"
#include "runtime/ps2_byte_buffer.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

class PS2Memory;

namespace ps2x::asyncvif
{
    enum class ItemKind : uint8_t
    {
        GifPath3,
        Vif1,
    };

    struct Item
    {
        ItemKind kind = ItemKind::Vif1;
        ps2x::ByteBuffer data;
    };

    void setDefaultEnabled(bool enabled);
    void setHostPacingDefault(bool enabled);
    void setFbrstProvider(std::function<uint32_t()> provider);
    void setGifProcessor(std::function<void(const uint8_t *, uint32_t)> processor);
    bool submitGif(const uint8_t *data, uint32_t sizeBytes);
    // MTVU: the EE cleared CSR.SIGNAL; the GIF stage thread replays the GIF data the GS deferred at the stalled
    // SIGNAL (ps2xGsReleaseStall), so the GS front end only ever runs on that thread. False when there is no
    // GIF stage (the caller then replays itself).
    bool postGifStallRelease();
    bool active(PS2Memory &memory);
    bool onWorker();
    bool onAsyncThread(); // VU worker or GIF stage thread
    // PS2X_MTVU_TRACE=<seconds>: from that many seconds after start, record timestamped events from every thread
    // (job kicks, VU jobs, GIF chunks, SIGNAL/FINISH, the GS handler, game frames, pacing sleeps, vblanks) and write
    // them to mtvu_trace.txt once the buffer is full. For finding which thread waits for which in a frame.
    void traceEvent(char code, uint32_t arg = 0u) noexcept;
    uint32_t currentFbrst();
    ps2x::ByteBuffer takeBuffer();
    void enqueue(PS2Memory &memory, std::vector<Item> &&items);
    // STAR 2026-11-23: extra passes over a frame (pictures for the screen's own refresh rate). Before the worker
    // executes a job, `prepare` (worker thread) may fill a plan for it. The job itself always runs first and exactly
    // as it would without this, so the game's own timing and the state it finds afterwards do not change. Then:
    //   - for each id in plan.shown: a marker that has the picture just drawn presented under that id (the first
    //     keeps it, the others repeat it);
    //   - for each pass in plan.passes: its patches are written into a copy of the job's items, the copy is
    //     executed, and markers have that picture presented under the pass's ids. Before the passes a marker has the
    //     GS remember the game's own picture and after them one puts it back; VIF1's registers and VU1's data
    //     memory are put back as well, and the passes' VU1 cycles are not counted as the game's.
    // While the passes run the worker does not count as busy for the emulated vblank, and they are given up when the
    // next job arrives. A marker is a write to the NOP register (A+D address 0x0F) with kFrameDoubleMagic in the
    // upper word; the lower word is kind | id << 8 (kinds: 1 keep, 2 repeat, 3 remember, 4 put back). When the
    // pictures are shown is the host's and the GS's business (PCSX2's GS: the option fd_times, GSRenderer.h).
    constexpr uint32_t kFrameDoubleMagic = 0x53544644u; // "STFD"
    struct FramePatch
    {
        uint32_t item = 0;   // index into the job's items
        uint32_t offset = 0; // byte offset in that item's data
        uint8_t bytes[64]{}; // a 4x4 float matrix
    };
    struct FramePass
    {
        std::vector<uint32_t> ids; // 24 bits each
        std::vector<FramePatch> patches;
    };
    struct FramePlan;
    struct FrameBlank
    {
        uint32_t item = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
    };
    struct FramePlan
    {
        std::vector<uint32_t> shown;
        std::vector<FramePass> passes;
        // Parts of the job's VIF1 items that the passes leave out (zeroed in the copy: VIF NOPs), e.g. the texture
        // uploads: the game's own run of the job has just done them.
        std::vector<FrameBlank> blanks;
        // STAR 2026-11-25: called on the worker right before a pass's patches are written into the copy (index: the
        // pass's place in `passes`), so that a patch can be made from something read at the last moment (the mouse).
        // Empty: none. Cleared for every job.
        std::function<void(size_t index, FramePass &pass)> beforePass;
        // STAR 2026-11-25: patches for the game's OWN run of the job (written into the job's items before it runs,
        // after `beforeOwn` had its say): the own run then is one of the frame's pictures in its own right, and one
        // pass fewer is needed. Its ids go into `shown` as ever. Empty: the own run is the game's, untouched.
        FramePass own;
        std::function<void(FramePass &own)> beforeOwn;
    };
    // STAR 2026-11-25: a number the game thread leaves for its next job that holds a scene (a VIF1 item of 4096
    // bytes or more); that job takes it (the tag is 0 again afterwards) and the frame doubler reads it with
    // currentJobTag() while it plans the job. 0 = none. Ties what the game thread knew when it built a frame to the
    // frame as the worker meets it later.
    void setJobTag(uint64_t tag);
    uint64_t currentJobTag();
    // STAR 2026-11-25: bytes the game thread adds to the job that takes the tag, at the moment the job is made (the
    // provider is called on the game thread, with the job's data just copied from the game's memory); the frame
    // doubler reads them with currentJobNote() while it plans that job. For what only the game knows about a frame's
    // drawing (which matrices are the player's own model's).
    using JobNoteProvider = std::function<void(std::vector<uint8_t> &note)>;
    void setJobNoteProvider(JobNoteProvider provider);
    const std::vector<uint8_t> &currentJobNote();
    using FrameDoublePrepare = std::function<void(const std::vector<Item> &items, FramePlan &plan)>;
    void setFrameDoubler(FrameDoublePrepare prepare);
    // How long the worker took lately (running averages, milliseconds): the game's own run of a job that got passes,
    // and one extra pass. 0 until measured. For the host to plan no more passes than fit in a frame's time.
    void frameDoubleTimes(double &ownMs, double &passMs);
    // STAR 2026-11-24 (the flight recorder, src/star/frame_double.cpp): a line about the last job's own run and extra
    // passes (how many were planned and run, why the rest was given up, a digest of the VIF1 registers at the start of
    // the own run and of every pass). Called on the worker (from the frame doubler); empties the line.
    std::string takePassInfo();
    // True while the worker has nothing of the game's left to do and is only running extra passes. The game's own work
    // is done then, and VIF1's and the GIF's registers read as they will when the passes end (the VIF1 interpreter
    // writes none of the values those reads return), so a read of them need not wait (ps2_memory.cpp).
    bool onlyExtraPasses(PS2Memory &memory);
    void sync(PS2Memory &memory);
    // ownOnly: waits for the game's own work only, not for extra passes over a frame (setFrameDoubler). For reads of
    // the VIF1/GIF registers: their values do not depend on the passes.
    void syncAt(PS2Memory &memory, int site, bool ownOnly = false);
    void stop(PS2Memory &memory);

    struct Vu1TimingHooks
    {
        std::function<uint64_t()> eeCycle;
        std::function<uint64_t()> vu1Cycles;
        std::function<uint64_t(uint64_t)> consumeEeCycles;
    };

    void setVu1Timing(Vu1TimingHooks hooks);
    // How long a VU1 program keeps the guest waiting, in EE cycles per VU1 cycle (PS2X_VU1_TIMING, default 1). The
    // port's 60 FPS mode halves it: the game then asks twice the PS2's work per second of the emulated VU1, which
    // (at 1) could not finish the casino's heaviest views in one field and dropped them to 30 frames a second while
    // the real VU work took 12 of the 16.7 ms (Oct 26 trace). PS2X_VU1_TIMING, when set, wins.
    void setVu1TimingScale(double scale);
    void vu1Kick();
    bool vu1Busy(PS2Memory &memory);

    // MTVU timing model (docs/PROGRESS.md §50). PS2X_MTVU_VBLANK_WAIT=0 turns it off.
    // The emulated vblank waits until the VU/GIF workers have no runnable work (data the GS deferred at a SIGNAL waits for
    // the EE and does not count), as in PCSX2: a slow VU1 slows the game down instead of making a frame miss the field.
    // With it on, the host pacing of vu1Busy and the EE scheduler's catch-up are off.
    bool vblankWaitEnabled();
    // True while the VU worker or the GIF stage has queued or running work.
    bool pipelineBusy(PS2Memory &memory);
    int lastBusyReason(); // diagnostic: why pipelineBusy last returned true (see ps2_async_vif.cpp)
    // Diagnostic (PS2X_VBLANK_WAIT_LOG): real time the EE thread spent waiting, by place. 0 = the vblank's wait for the
    // workers, 1 = the pacing check, 2 = sync, 3 = handing over a job, 4 = the vblank's call into the renderer,
    // 5 = the pacing sleep.
    void diagWaited(int slot, long long ns);
    void diagTake(long long out[6]);
    // Waits up to `timeout` for the workers to go idle; returns pipelineBusy() afterwards.
    bool waitPipelineIdle(PS2Memory &memory, std::chrono::microseconds timeout);
    void saveTimingState(ps2x::state::Writer &out);
    void loadTimingState(ps2x::state::Reader &in);
}
