#include "star/game_fixes.h"
#include "star/bindings.h"

#include "ps2_runtime.h"
#include "star/live_settings.h"
#include "star/live_look.h"
#include "star/debug_menu.h"
#include "star/shadow_volumes.h"
#include "runtime/ps2_async_vif.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_pad_tap.h"
#include "runtime/ps2_input_options.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace star
{
    namespace
    {
        // ---- Sound RPC server thread (0x102a) --------------------------------------------------------------------
        //
        // FUN_001d8210 (sound set-up) creates and starts a thread at priority 0x30 (FUN_001d8be0) that registers the
        // EE-side RPC server 0x102a, the one the sound module on the IOP calls for more stream data. The main thread
        // runs at priority 1 (InitThread), so on a PS2 the server thread first runs when the main thread blocks, which
        // it does inside every synchronous sceSifCallRpc (WaitSema until the IOP answers). The runtime answers those
        // calls without blocking, so the server thread never ran: the IOP module retried its bind 140,000+ times and
        // the opening movie's audio looped a 128 ms block (boot logs of 2026-11-22). From the movie on, priority-1
        // threads (an idle loop among them) leave it no chance at all.
        //
        // Fix: the main thread drops below the server thread (as if it were waiting for the IOP) at the moment
        // FUN_001d8210 sends its first command after starting that thread: FUN_001d8960(0, 1, 0, 4, 0x20000), the
        // command on which the IOP driver binds to 0x102a. The server thread registers 0x102a and returns (the
        // runtime's sceSifRpcLoop does not loop: the runtime serves the requests itself), and on its way out gives the
        // main thread its priority back; the command then goes out with the server in place, as on a PS2, where the
        // server thread runs while the main thread waits for that command's answer.
        //
        // Two earlier tries (2026-11-22): yielding after FUN_001d8210 returned registered the server too late: the
        // driver had made its 142,855 bind attempts during command 0x20000 and given up, so every later call from the
        // IOP failed ("client not bound": no music trigger on the first stairs, and a newly equipped weapon never
        // became usable). And host code placed after transferIfRequested never runs: a thread switch unwinds to the
        // scheduler and the thread later resumes at its guest pc. Hence the yield before the call, with pc set to the
        // call itself: on resume the dispatcher enters this hook again, which then lets the command through.
        constexpr uint32_t kSoundCommand = 0x001D8960;
        constexpr uint32_t kServerThread = 0x001D8BE0;
        constexpr int kServerPriority = 0x30;
        PS2Runtime::RecompiledFunction g_soundCommand = nullptr;
        PS2Runtime::RecompiledFunction g_serverThread = nullptr;
        int g_mainThreadId = -1;
        int g_mainPriority = -1; // >= 0 while the main thread waits below the server thread
        bool g_serverYieldDone = false;

        void soundCommandHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_serverYieldDone && getRegU32(ctx, 8) == 0x20000u)
            {
                g_serverYieldDone = true;
                EeScheduler &ee = runtime->eeScheduler();
                GuestThread *self = ee.currentThread();
                int oldPriority = 0;
                if (self && ee.changePriority(self->id, kServerPriority + 1, false, oldPriority) == 0)
                {
                    g_mainThreadId = self->id;
                    g_mainPriority = oldPriority;
                    std::printf("[STAR fix] main thread %d waits below the sound RPC server thread (priority %d -> %d)\n",
                                self->id, oldPriority, kServerPriority + 1);
                    std::fflush(stdout);
                    ctx->pc = kSoundCommand;       // resume = this call again
                    ee.transferIfRequested(false); // the server thread runs from here; this host frame does not come back
                }
            }
            g_soundCommand(rdram, ctx, runtime);
        }

        void serverThreadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_serverThread(rdram, ctx, runtime); // registers 0x102a
            if (g_mainPriority >= 0)
            {
                int ignored = 0;
                runtime->eeScheduler().changePriority(g_mainThreadId, g_mainPriority, false, ignored);
                std::printf("[STAR fix] sound RPC server 0x102a registered; main thread %d back to priority %d\n",
                            g_mainThreadId, g_mainPriority);
                std::fflush(stdout);
                g_mainPriority = -1;
                // No transfer here: this thread returns and exits, and the scheduler picks the main thread next.
            }
        }

        // ---- Movie display trace (STAR_TRACE_MOVIE=1; diagnosis only) ----------------------------------------------
        // The opening movie stalled 0.6 s in (2026-11-22). Its display: the vblank handler 0x1e8cb0 sends a decoded
        // picture as two fields (state 2 -> 1 -> 0) and sets the flag at gp-0x777c (0x2023f4); the GIF DMA-end handler
        // 0x1e8c50 then frees the buffer (decoded count at 0x4bdecc - 1) if the flag is set. This logs their order.
        constexpr uint32_t kMovieVblank = 0x001E8CB0;
        constexpr uint32_t kMovieGifEnd = 0x001E8C50;
        constexpr uint32_t kFlagFieldDone = 0x002023F4; // uGpffff8884
        constexpr uint32_t kDecodedCount = 0x004BDECC;  // iRam004bdecc
        PS2Runtime::RecompiledFunction g_movieVblank = nullptr;
        PS2Runtime::RecompiledFunction g_movieGifEnd = nullptr;
        int g_inVblank = 0;
        int g_traceLines = 0;

        uint32_t guestWord(uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + address, 4);
            return value;
        }

        void movieVblankTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t before = guestWord(rdram, kDecodedCount);
            ++g_inVblank;
            g_movieVblank(rdram, ctx, runtime);
            --g_inVblank;
            if (g_traceLines < 400 && (guestWord(rdram, kFlagFieldDone) != 0 || before != guestWord(rdram, kDecodedCount)))
            {
                ++g_traceLines;
                std::printf("[movie] vblank: decoded %u -> %u, field-done flag %u\n", before, guestWord(rdram, kDecodedCount),
                            guestWord(rdram, kFlagFieldDone));
            }
        }

        void movieGifEndTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t flag = guestWord(rdram, kFlagFieldDone);
            const uint32_t before = guestWord(rdram, kDecodedCount);
            g_movieGifEnd(rdram, ctx, runtime);
            if (g_traceLines < 400)
            {
                ++g_traceLines;
                std::printf("[movie] gif-end%s: flag %u, decoded %u -> %u\n", g_inVblank ? " INSIDE vblank" : "", flag, before,
                            guestWord(rdram, kDecodedCount));
            }
        }

        // sceSdRemote (libsdr, 0x116360): the sound commands sent to the IOP. Traced: block transfers (0x80e0) and their
        // status queries (0x8100), whose answer tells the game when to refill the IOP's stream buffer.
        constexpr uint32_t kSceSdRemote = 0x00116360;
        PS2Runtime::RecompiledFunction g_sdRemote = nullptr;
        int g_sdLines = 0;
        unsigned g_sdStatusCalls = 0;

        void sdRemoteTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t command = getRegU32(ctx, 5);
            const uint32_t a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7), t0 = getRegU32(ctx, 8), t1 = getRegU32(ctx, 9);
            g_sdRemote(rdram, ctx, runtime);
            if (command == 0x8100u)
                ++g_sdStatusCalls;
            if ((command == 0x80E0u || command == 0x8100u) && g_sdLines < 120)
            {
                ++g_sdLines;
                std::printf("[sound] sceSdRemote 0x%04x (%x, %x, %x, %x) -> 0x%08x  [status calls so far %u]\n", command, a2, a3,
                            t0, t1, getRegU32(ctx, 2), g_sdStatusCalls);
            }
        }

        // sceCdStRead (0x117d58, a runtime handler): the movie's disc stream. Traced: sectors asked, sectors got, error.
        constexpr uint32_t kSceCdStRead = 0x00117D58;
        PS2Runtime::RecompiledFunction g_cdStRead = nullptr;
        int g_stLines = 0;
        unsigned g_stCalls = 0, g_stSectors = 0;

        void cdStReadTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t asked = getRegU32(ctx, 4), mode = getRegU32(ctx, 6), errAddr = getRegU32(ctx, 7);
            g_cdStRead(rdram, ctx, runtime);
            const uint32_t got = getRegU32(ctx, 2);
            const uint32_t err = errAddr && errAddr < 0x02000000u ? guestWord(rdram, errAddr & 0x01FFFFFFu) : 0xFFFFFFFFu;
            ++g_stCalls;
            g_stSectors += got;
            if (g_stLines < 80 || (g_stCalls % 500u) == 0u)
            {
                ++g_stLines;
                std::printf("[stream] sceCdStRead #%u: asked %u (mode %u) -> got %u, err %d; %u sectors so far\n", g_stCalls,
                            asked, mode, got, static_cast<int>(err), g_stSectors);
            }
        }

        // The movie player's libmpeg callbacks (sceMpegAddCallback in FUN_001ec5e0; Sony's ezmpeg sample): 1 nodata
        // (send audio, feed the IPU the next video block), 2 stop DMA, 3 restart DMA, 4 background (send audio),
        // 5 time stamp. Counted per second of run time; the counts print at the end of each test run second.
        constexpr uint32_t kMpegCallbacks[] = {0x001E9EA0, 0x001E9E70, 0x001E9E40, 0x001E9E10, 0x001E9DC0};
        constexpr const char *kMpegCallbackNames[] = {"nodata", "stopdma", "restartdma", "background", "timestamp"};
        PS2Runtime::RecompiledFunction g_mpegCallback[5] = {};
        std::atomic<unsigned> g_mpegCount[5] = {};

        // IPU and toIPU DMA registers around stop / restart / nodata: calls 1-6 (the healthy first frames) and 61-72
        // (after the stall).
        void dumpIpu(PS2Runtime *runtime, const char *when, unsigned call)
        {
            PS2Memory &m = runtime->memory();
            std::printf("[ipu-state] %s #%u: CTRL %08x BP %08x | toIPU CHCR %08x MADR %08x QWC %04x TADR %08x | "
                        "fromIPU CHCR %08x QWC %04x\n",
                        when, call, m.readIORegister(0x10002010u), m.readIORegister(0x10002020u),
                        m.readIORegister(0x1000B400u), m.readIORegister(0x1000B410u), m.readIORegister(0x1000B420u),
                        m.readIORegister(0x1000B430u), m.readIORegister(0x1000B000u), m.readIORegister(0x1000B020u));
        }

        template <int I>
        void mpegCallbackTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const unsigned call = g_mpegCount[I].fetch_add(1, std::memory_order_relaxed) + 1u;
            // STAR_TRACE_IPU=<first>:<last>: the register dumps while the stopdma call count is in that range.
            static const std::pair<unsigned, unsigned> range = [] {
                unsigned a = 0, b = 0;
                if (const char *v = std::getenv("STAR_TRACE_IPU"))
                    std::sscanf(v, "%u:%u", &a, &b);
                return std::make_pair(a, b);
            }();
            const unsigned stops = g_mpegCount[1].load(std::memory_order_relaxed);
            const bool dump = (I <= 2) && stops >= range.first && stops <= range.second && range.second != 0u;
            if (dump)
                dumpIpu(runtime, (std::string(kMpegCallbackNames[I]) + " before").c_str(), call);
            g_mpegCallback[I](rdram, ctx, runtime);
            if (dump)
                dumpIpu(runtime, (std::string(kMpegCallbackNames[I]) + " after ").c_str(), call);
        }
        constexpr PS2Runtime::RecompiledFunction kMpegTraces[] = {&mpegCallbackTrace<0>, &mpegCallbackTrace<1>,
                                                                  &mpegCallbackTrace<2>, &mpegCallbackTrace<3>,
                                                                  &mpegCallbackTrace<4>};

        void printMpegCounts()
        {
            static unsigned last[5] = {};
            static unsigned lastSectors = 0;
            std::printf("[mpeg-cb] stream %u sectors |", g_stSectors - lastSectors);
            lastSectors = g_stSectors;
            for (int i = 0; i < 5; ++i)
            {
                const unsigned now = g_mpegCount[i].load(std::memory_order_relaxed);
                std::printf(" %s %u", kMpegCallbackNames[i], now - last[i]);
                last[i] = now;
            }
            std::printf("\n");
            std::fflush(stdout);
        }

        // ---- Memory card trace (STAR_TRACE_MC=1; diagnosis only) --------------------------------------------------
        // sceMcGetInfo (0x118cb0) and sceMcSync (0x118b78): arguments, return values, and libmc's state words
        // (current function 0x1f1388, the RPC client's server pointer 0x208124).
        constexpr uint32_t kSceMcGetInfo = 0x00118CB0;
        constexpr uint32_t kSceMcSync = 0x00118B78;
        PS2Runtime::RecompiledFunction g_mcGetInfo = nullptr;
        PS2Runtime::RecompiledFunction g_mcSync = nullptr;
        int g_mcLines = 0;
        int g_mcLastSync = 0x7fffffff;

        void mcGetInfoTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t port = getRegU32(ctx, 4), slot = getRegU32(ctx, 5);
            g_mcGetInfo(rdram, ctx, runtime);
            if (g_mcLines < 60)
            {
                ++g_mcLines;
                std::printf("[mc] sceMcGetInfo(%u, %u) -> %d; curFunc %u, client.server 0x%08x\n", port, slot,
                            static_cast<int>(getRegU32(ctx, 2)), guestWord(rdram, 0x001F1388u), guestWord(rdram, 0x00208124u));
            }
        }

        void mcSyncTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t mode = getRegU32(ctx, 4), resultAddr = getRegU32(ctx, 6);
            g_mcSync(rdram, ctx, runtime);
            const int ret = static_cast<int>(getRegU32(ctx, 2));
            if (ret != g_mcLastSync && g_mcLines < 60)
            {
                ++g_mcLines;
                g_mcLastSync = ret;
                std::printf("[mc] sceMcSync(%u) -> %d (result %d); curFunc %u\n", mode, ret,
                            resultAddr ? static_cast<int>(guestWord(rdram, resultAddr & 0x01FFFFFFu)) : 0,
                            guestWord(rdram, 0x001F1388u));
            }
        }

        // ---- Controller trace (STAR_TRACE_PAD=1; diagnosis only) ---------------------------------------------------
        // scePadRead (0x115550)(port, slot, data): the 32 bytes the game gets, printed whenever they change (port 0):
        // [0] status, [1] mode/length, [2..3] buttons (0 = pressed), [4..7] right X/Y, left X/Y, [8..19] pressures.
        constexpr uint32_t kScePadRead = 0x00115550;
        PS2Runtime::RecompiledFunction g_padRead = nullptr;
        uint8_t g_padLast[20] = {};
        int g_padLines = 0;

        void padReadTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t port = getRegU32(ctx, 4), data = getRegU32(ctx, 6) & 0x01FFFFFFu;
            g_padRead(rdram, ctx, runtime);
            if (port != 0u || data == 0u || g_padLines >= 400 || getRegU32(ctx, 2) == 0u)
                return; // 0 = no data this time (the buffer is left as it was: stack contents)
            const uint8_t *now = rdram + data;
            if (std::memcmp(now, g_padLast, sizeof(g_padLast)) == 0)
                return;
            std::memcpy(g_padLast, now, sizeof(g_padLast));
            ++g_padLines;
            char text[3 * 20 + 1];
            for (int i = 0; i < 20; ++i)
                std::snprintf(text + i * 3, 4, "%02x ", now[i]);
            std::printf("[pad] ret %d: %s\n", static_cast<int>(getRegU32(ctx, 2)), text);
        }

        // STAR 2026-11-23: PCSX2's "Remove Scanmask Blur" patch for this disc (pcsx2_patches SLPS-25217_38145680.pnach, by
        // val & refraction: "Disables scanmask usage to reduce blur when turning"; patch=1,EE,20158CA8,extended,24050004).
        // FUN_00158bf0 sets the GS register SCANMSK (0x22) to 3 through FUN_001543e0(register, value) before a
        // full-screen pass, so that pass draws on every other line only; on a deinterlaced picture the lines it leaves
        // show the frame before (the user's report: floor tile edges flickering through the walls while turning, absent
        // in their PCSX2, which has the patch on). The patch makes the value 4 (mask bits 0 = off). A compiled program
        // cannot be patched in memory, so the same is done here on the call. STAR_SCANMASK=1 keeps the game's own.
        constexpr uint32_t kSetGsRegister = 0x1543E0u;
        constexpr uint32_t kGsRegScanmask = 0x22u;
        PS2Runtime::RecompiledFunction g_setGsRegister = nullptr;
        std::atomic<uint32_t> g_scanmaskChanged{0};

        void setGsRegisterHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 4) == kGsRegScanmask && (getRegU32(ctx, 5) & 3u) >= 2u)
            {
                if (g_scanmaskChanged.fetch_add(1u) == 0u)
                {
                    std::printf("[STAR fix] scanmask %u asked for (return address 0x%x): turned off\n", getRegU32(ctx, 5),
                                getRegU32(ctx, 31));
                    std::fflush(stdout);
                }
                SET_GPR_U32(ctx, 5, 4u);
            }
            // STAR_TRACE_GSREG=<register, hex>: every distinct caller (return address) that sets that GS register,
            // with the first value it gave (finding the code behind a pass seen in a PS2X_GS_TRACE file).
            static const int traceReg = []
            { const char *v = std::getenv("STAR_TRACE_GSREG"); return v && *v ? static_cast<int>(std::strtoul(v, nullptr, 16)) : -1; }();
            if (traceReg >= 0 && getRegU32(ctx, 4) == static_cast<uint32_t>(traceReg))
            {
                static uint32_t seen[64];
                static uint32_t seenCount = 0;
                const uint32_t ra = getRegU32(ctx, 31);
                bool known = false;
                for (uint32_t i = 0; i < seenCount; ++i)
                    known = known || seen[i] == ra;
                if (!known && seenCount < 64u)
                {
                    seen[seenCount++] = ra;
                    std::printf("[gsreg] register 0x%x <- 0x%llx from 0x%x\n", traceReg,
                                static_cast<unsigned long long>(GPR_U64(ctx, 5)), ra);
                    std::fflush(stdout);
                }
            }
            g_setGsRegister(rdram, ctx, runtime); // nothing after the call (it may switch threads)
        }

        // STAR 2026-11-23 (user: the picture loses detail while the camera turns and snaps when it stops; on the PS2 and
        // in PCSX2 too). FUN_00158bf0(x shift, y shift) copies the finished picture onto itself in twenty 32-pixel strips
        // (texture = the frame buffer, nearest, opaque), moved by the shift (1/16 pixel units; 8 pixels at the full turn
        // rate of 2.8 degrees a frame), with SCANMSK=3 so that only every other line takes the copy: for an interlaced
        // TV the second field's lines sit about half a turn step ahead, which fakes 60 pictures a second out of 30.
        // On a progressive picture that is combing (the game's own), or, with the scanmask off (PCSX2's "Remove
        // Scanmask Blur", STAR's default), the whole picture moved 8 pixels with a doubled column band at every strip
        // edge while turning, and a jump back by 8 pixels one frame after the camera stops. With a shift of 0 the
        // pass is what it is whenever the camera is still. STAR_TURN_SHIFT=1 keeps the game's shift.
        constexpr uint32_t kTurnShiftCopy = 0x158BF0u;
        PS2Runtime::RecompiledFunction g_turnShiftCopy = nullptr;
        std::atomic<uint32_t> g_turnShiftZeroed{0};

        // Later the same day: the pass is skipped altogether, which is what the game does when bit 2 of the byte at
        // 0x2f2078 is clear (FUN_00159680). With a shift of 0 it still copied the picture onto itself half a pixel
        // up and left, strip by strip: nothing to see at the PS2's resolution, but at 2x-4x rendering each strip
        // edge showed as a faint vertical seam (the column gradient there 1.4 to 2 times the mean).
        void turnShiftCopyHook(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (g_turnShiftZeroed.fetch_add(1u) == 0u)
            {
                std::printf("[STAR fix] turn shift pass (%d, %d)/16 pixels: skipped\n", static_cast<int32_t>(getRegU32(ctx, 4)),
                            static_cast<int32_t>(getRegU32(ctx, 5)));
                std::fflush(stdout);
            }
            ctx->pc = getRegU32(ctx, 31); // return to the caller at once
        }

        // STAR 2026-11-23: two picture options (off unless asked for: the game's own picture is the default).
        // The post-processing driver FUN_00159680 runs its passes by the bits of the byte at 0x2f2078:
        //   bit 0 / bit 1  FUN_00158900(0x2f19c0 / 0x2f19cc): the picture blended over itself, moved by the struct's
        //                  (x, y) (1 line down) with its alpha (0x40 = half): a flicker filter for interlaced TVs,
        //                  which on a progressive picture is a vertical blur.            STAR_FLICKER_FILTER=0 skips it
        //   bit 4 / bit 5  FUN_001580e0(0x2f1a60 / 0x2f1a80): the picture reduced to 256x256 and added back over
        //                  itself up to four times with small offsets: a soft glow.      STAR_GLOW=0 skips it
        // Skipping a pass is what the game does itself when the bit is clear, so nothing else depends on it.
        constexpr uint32_t kFlickerFilter = 0x158900u;
        constexpr uint32_t kGlow = 0x1580E0u;

        // The two passes are switched while the game runs (star::live(), the F2 menu): skipped = return at once.
        PS2Runtime::RecompiledFunction g_flickerFilter = nullptr;
        PS2Runtime::RecompiledFunction g_glow = nullptr;

        void flickerFilterHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!live().flickerFilter.load(std::memory_order_relaxed))
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_flickerFilter(rdram, ctx, runtime); // nothing after the call
        }

        void glowHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!live().glow.load(std::memory_order_relaxed))
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_glow(rdram, ctx, runtime); // nothing after the call
        }

        // STAR 2026-11-24, a gameplay tweak (off unless the user turns it on): analog aiming with the right stick.
        // The game turns every stick into "amounts" (FUN_001c4760: 14 floats; [0]/[1] turn right/left, [6]/[7] look
        // up/down, each 0..1) through a square dead zone of 30 of 127 per axis, and then:
        //   - FUN_001c2db0 (walking on the ground) turns only once an amount is over 0.2 (the stick 40% over), at
        //     amount x 2.8 degrees a frame: nothing, then a fifth of full speed at once;
        //   - FUN_001c27c0 looks up or down at ONE speed (1.6 degrees a frame) for any amount over 0.1.
        // The user: "very robotic". Here, for the control types that look with the right stick, the stick is read
        // from the game's raw pad data with a round dead zone and a curve, and both hooks move the view by it
        // themselves and hand the game zero amounts: any direction, any speed up to the game's own top speed (times
        // the menu's percentages). The game's own factors stay: the half speed of some states and the slow-down of
        // heavy loads and ailments (player + 0x5ac).
        // Control type: the byte at (*0x201E10) + 7, modulo 6: 4 and 5 turn with the right stick's X and look with
        // its Y (5: Y the other way round); 2 and 3 strafe with its X and look with its Y; 0 and 1 use it otherwise.
        // STAR_AIM_LOG=1: the first 400 frames with the stick moved: stick, yaw, pitch.
        constexpr uint32_t kWalkOnGround = 0x1C2DB0u; // FUN_001c2db0(a0 = player, a1 = amounts; f12 = frame time)
        constexpr uint32_t kLookUpDown = 0x1C27C0u;   // FUN_001c27c0(a0 = player, a1 = amounts)
        constexpr uint32_t kOptionsPointer = 0x201E10u;
        constexpr uint32_t kPadRaw = 0x4B6A00u;       // pad 0 as scePadRead gave it: [4] right X, [5] right Y
        constexpr uint32_t kPadState = 0x317230u;     // the game's pad 0: +0x53 bits 6-7 = 1 or 2 when the sticks count
        constexpr uint32_t kGuestRam = 0x02000000u;
        PS2Runtime::RecompiledFunction g_walkOnGround = nullptr;
        PS2Runtime::RecompiledFunction g_lookUpDown = nullptr;

        float guestFloat(uint8_t *rdram, uint32_t address)
        {
            float value = 0.0f;
            std::memcpy(&value, rdram + address, 4);
            return value;
        }

        void setGuestFloat(uint8_t *rdram, uint32_t address, float value)
        {
            std::memcpy(rdram + address, &value, 4);
        }

        bool inRam(uint32_t address)
        {
            return address >= 0x00100000u && address <= kGuestRam - 0x40u; // a thread's stack may sit at the very top
        }

        // Mouse and keyboard play (the hooks are further down): what one frame's input left for the hooks after it.
        struct MouseKeys
        {
            bool active = false;            // this frame the keyboard and mouse play: the game is told control type 4
            float yaw = 0.0f, pitch = 0.0f; // the mouse's turn of this frame, radians, waiting for the look hook
            uint32_t heldInPlay = 0u;       // the mouse buttons pressed during play and still down (1 left, 2 right)
            uint32_t keysInPlay = 0u;       // the same for the game layer's own keys (bindings.h: setGameKeys)
            float flickX = 0.0f, flickY = 0.0f; // the mouse's recent movement, fading (the swing gesture)
            bool flickArmed = true;         // the mouse has slowed down since the last swing it started
            uint32_t swing = 0u;            // the wanted swing's button
            int swingFrames = 0;            // a swing is wanted, for this many more frames
            int swingLock = 0;              // a button for each swing: no new swing is taken for this many more frames
            bool swingSent = false;         // its button has been given to the game
            uint32_t aimSwing = 0x0400u;    // the way the view last moved, as a swing button: the left click's swing
            double totalX = 0.0, totalY = 0.0; // the mouse's whole movement taken so far (lookMouseAt), counts
            bool totalValid = false;
            bool wasLocked = false;         // the view was locked (combat mode) in the frame before
            uint64_t seq = 0u;              // LookFrames published
            int gunWish = 0, gunWishFrames = 0; // a gun wanted out (+1) or away (-1), for this many more frames
            int fireFrames = 0;             // a shot asked for, for this many more frames
            uint64_t frame = 0u;            // frames of play counted (tests)
            double lastYaw = 0.0, lastPitch = 0.0; // what the frame before turned the view by (the view margins)
        };
        MouseKeys g_mouseKeys; // the game thread only

        struct Aim
        {
            int type = 0;                // the control type, 0..5
            float x = 0.0f, y = 0.0f;    // the right stick, -1..1 each, after the dead zone and the curve (y: down > 0)
            uint32_t inner = 0, camera = 0;
            float factor = 0.0f;         // the game's own slow-downs
        };

        // False when the tweak is off, the pad is not an analog one, or the player's data is not where expected.
        bool readAim(uint8_t *rdram, uint32_t player, Aim &aim)
        {
            if (!live().analogAim.load(std::memory_order_relaxed) || !inRam(player))
                return false;
            const uint32_t options = guestWord(rdram, kOptionsPointer);
            if (!inRam(options))
                return false;
            // STAR_AIM_TEST_TYPE=<0..5> (tests): the game's control type option is set to this.
            static const int testType = [] { const char *v = std::getenv("STAR_AIM_TEST_TYPE"); return v && *v ? std::atoi(v) : -1; }();
            if (testType >= 0 && testType <= 5)
                rdram[options + 7u] = static_cast<uint8_t>(testType);
            aim.type = g_mouseKeys.active ? 4 : rdram[options + 7u] % 6; // mouse and keyboard play: the game is told 4
            const uint8_t mode = rdram[kPadState + 0x53u] >> 6;
            {
                static const bool log = [] { const char *v = std::getenv("STAR_AIM_LOG"); return v && *v == '1'; }();
                static int lines = 0;
                if (log && lines < 3)
                {
                    ++lines;
                    std::printf("[aim] control type %d, pad mode %u, pad wait %u, raw right stick %u %u, player 0x%x%c", aim.type, mode,
                                rdram[kPadState + 0x54u], rdram[kPadRaw + 4u], rdram[kPadRaw + 5u], player, 10);
                }
            }
            if (aim.type < 2 || (mode != 1u && mode != 2u) || rdram[kPadState + 0x54u] != 0u)
                return false;
            aim.inner = guestWord(rdram, player + 0x518u);
            if (!inRam(aim.inner))
                return false;
            aim.camera = guestWord(rdram, aim.inner + 0x198u);
            const uint32_t state = guestWord(rdram, aim.inner + 0x510u);
            if (!inRam(aim.camera) || !inRam(state))
                return false;
            aim.factor = guestFloat(rdram, aim.inner + 0x5ACu) * (guestWord(rdram, state + 0x48u) == 0u ? 1.0f : 0.5f);
            // The stick: a round dead zone of 15% (the game's is a square of 24%), full speed from 95%, and a curve
            // (power 1.5) so that a little push is a slow, fine movement.
            const float x = (static_cast<float>(rdram[kPadRaw + 4u]) - 127.5f) / 127.5f;
            const float y = (static_cast<float>(rdram[kPadRaw + 5u]) - 127.5f) / 127.5f;
            const float length = std::sqrt(x * x + y * y);
            constexpr float kDead = 0.15f, kFull = 0.95f;
            if (length > kDead)
            {
                const float amount = std::pow(std::min((length - kDead) / (kFull - kDead), 1.0f), 1.5f);
                aim.x = x / length * amount;
                aim.y = y / length * amount;
            }
            return true;
        }

        void aimLog(uint8_t *rdram, const Aim &aim, const char *what)
        {
            static const bool log = [] { const char *v = std::getenv("STAR_AIM_LOG"); return v && *v == '1'; }();
            static int lines = 0;
            if (log && (aim.x != 0.0f || aim.y != 0.0f) && lines < 400)
            {
                ++lines;
                std::printf("[aim] %s type %d stick %+.3f %+.3f yaw %.4f pitch %.4f factor %.2f%c", what, aim.type, aim.x, aim.y,
                            guestFloat(rdram, aim.camera + 0x24u), guestFloat(rdram, aim.camera + 0x20u), aim.factor, 10);
            }
        }

        void walkOnGroundHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Aim aim;
            const uint32_t amounts = getRegU32(ctx, 5) & 0x1FFFFFFFu; // on the caller's stack
            if (readAim(rdram, getRegU32(ctx, 4), aim) && aim.type >= 4 && inRam(amounts))
            {
                // the game: yaw += (amount right - amount left) x 0.04886922 x the factors
                const float speed = 0.04886922f * static_cast<float>(live().aimTurnPercent.load(std::memory_order_relaxed)) / 100.0f;
                setGuestFloat(rdram, aim.camera + 0x24u, guestFloat(rdram, aim.camera + 0x24u) + aim.x * speed * aim.factor);
                setGuestFloat(rdram, amounts + 0u, 0.0f);
                setGuestFloat(rdram, amounts + 4u, 0.0f);
                aimLog(rdram, aim, "turn");
            }
            g_walkOnGround(rdram, ctx, runtime); // nothing after the call
        }

        void lookUpDownHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Aim aim;
            const uint32_t amounts = getRegU32(ctx, 5) & 0x1FFFFFFFu; // on the caller's stack
            if (readAim(rdram, getRegU32(ctx, 4), aim) && inRam(amounts))
            {
                // Not while the game brings the view back to level (the amounts' button word, bit 2): that is its own.
                uint32_t buttons = 0;
                std::memcpy(&buttons, rdram + amounts + 0x34u, 4);
                if ((buttons & 4u) == 0u)
                {
                    // the game: pitch += 0.06981317 x 0.4 x the factors, up for [6] and down for [7]; the stick pushed
                    // up is [6] in types 2 and 4 and [7] in the reversed types 3 and 5
                    const float speed = 0.06981317f * 0.4f * static_cast<float>(live().aimLookPercent.load(std::memory_order_relaxed)) / 100.0f;
                    const float direction = (aim.type == 2 || aim.type == 4) ? -1.0f : 1.0f;
                    setGuestFloat(rdram, aim.camera + 0x20u, guestFloat(rdram, aim.camera + 0x20u) + direction * aim.y * speed * aim.factor);
                    setGuestFloat(rdram, amounts + 0x18u, 0.0f);
                    setGuestFloat(rdram, amounts + 0x1Cu, 0.0f);
                    aimLog(rdram, aim, "look");
                }
            }
            // The mouse (readInputHook left its movement of this frame): straight onto the view. The game limits
            // the pitch itself right after (FUN_001befa0: -60 to +35 degrees).
            if (MouseKeys &mk = g_mouseKeys; mk.yaw != 0.0f || mk.pitch != 0.0f)
            {
                const uint32_t player = getRegU32(ctx, 4);
                const uint32_t inner = inRam(player) ? guestWord(rdram, player + 0x518u) : 0u;
                const uint32_t camera = inRam(inner) ? guestWord(rdram, inner + 0x198u) : 0u;
                if (inRam(camera))
                {
                    setGuestFloat(rdram, camera + 0x24u, guestFloat(rdram, camera + 0x24u) + mk.yaw);
                    setGuestFloat(rdram, camera + 0x20u, guestFloat(rdram, camera + 0x20u) + mk.pitch);
                }
                mk.yaw = mk.pitch = 0.0f;
            }
            g_lookUpDown(rdram, ctx, runtime); // nothing after the call
        }

        // STAR 2026-11-25: late input (setting late_input; the user: "some very slight input delay with keyboard
        // movement"). The play loop FUN_0012b350, per frame of the game (two fields: 30 a second):
        //   FUN_00155370   waits until the drawing of the frame before is done and for the next odd field's vblank,
        //                  then swaps the display buffers;
        //   FUN_0015fc10   reads the pads (libpad: the newer of the two halves of the buffer the IOP's PADMAN sends,
        //                  pointer at 0x207a10 + port * 0x70, frame counters at + 0x58 and + 0xd8, FUN_001154a0; the
        //                  game's pad words at 0x317230);
        //   FUN_001296c0   the view's matrices for the frame built last time (no pad);
        //   FUN_001552a0   hands the frame built last time to VIF1 (DMA; the smooth pictures start from it);
        //   FUN_0012a520(1/30)  the game's logic (the mouse and keyboard hook FUN_001c4760 among it);
        //   FUN_0012a070   builds the next frame's drawing list, which waits for the NEXT odd vblank to be handed over.
        // And the keys: the window's loop takes them in just after each vblank (ps2_host_input::sample). The emulated
        // IOP's PADMAN polls the pad once a field at a time of its own (measured 1.1 M EE cycles after the EE's
        // vblank, drifting 4,000 cycles a second: its vblank is counted in IOP time), and its SIF DMA reaches EE
        // memory when the EE next catches the IOP up (the IOP runs on a worker thread, up to 4 M EE cycles behind).
        // Measured: at a read right after a vblank the newest pad data was the poll of the field before. So the
        // usual frame carried keys taken in two to four fields before it left (50 to 83 ms).
        // With late input:
        //   1. FUN_0015fc10 is passed over where the loop calls it; FUN_0012a520 first waits for the next vblank (the
        //      even field's, half way; the scheduler's own wait, the game's other threads run meanwhile);
        //   2. the window's loop takes the keys in (waited for, at most 2 ms; it is a fraction of a millisecond), the
        //      IOP is caught up (ps2x::pad::catchUpIop: every poll it owes is in the buffer), and the newest half of
        //      each open pad's buffer gets the pad as it is now: the bytes the emulated DualShock 2 answers a poll
        //      with for that state (iop_dualshock2.cpp pollByte: the same state PADMAN's next poll gets), in the mode
        //      the half already has (digital, analog or DualShock 2; a config reply is left alone);
        //   3. the pad is read (FUN_0015fc10, returning into FUN_0012a520), then the logic runs and the frame is built.
        // The frame leaves with the keys taken in at the vblank just before it (17 to 33 ms: one frame, 33 ms, sooner
        // than the usual, always). Everything the thread needs is in its own registers (the wait resumes at
        // FUN_0012a520 itself; the loop passes only f12 = 1/30), so a save state taken during the wait loads fine
        // (that frame just keeps the pad of the frame before).
        // If a frame's work ever ends after the odd field's vblank it would cost a whole frame: then, and while the
        // longest work of the last 64 frames is over 11 ms or over 70% of a field in EE cycles, the pad is read at
        // the usual place (for 300 frames after a miss; a load, a frame of more than four fields, is neither).
        // STAR_LATE_LOG=1: once a second what it did; =2 each frame. STAR_LATE_TEST=1: the time from a key to its
        // frame (below).
        long long steadyNowNs(); // (below)
        constexpr uint32_t kPadUpdate = 0x15FC10u, kGameLogic = 0x12A520u, kFrameSwap = 0x155370u;
        constexpr uint32_t kPadUpdateReturn = 0x12B418u, kGameLogicReturn = 0x12B43Cu, kFrameSwapReturn = 0x12B3C8u;
        constexpr uint32_t kGameLogicStep = 0x3D088889u; // the loop's f12 for FUN_0012a520 (1/30 s)
        constexpr uint32_t kPadBuffer = 0x207A10u;       // libpad: port 0 slot 0's buffer
        PS2Runtime::RecompiledFunction g_padUpdate = nullptr, g_gameLogic = nullptr, g_frameSwap = nullptr;
        struct LateInput
        {
            enum class Step { Idle, PadSkipped, Waiting, PadRead } step = Step::Idle;
            uint64_t waitedTick = 0;                    // the vblank waited for
            uint64_t blankCycle = 0;                    // the EE cycle at the vblank
            long long sampleAge = 0;                    // (log) how old the keys were when read, ns
            long long sampleWait = 0;                   // (log) how long they were waited for, ns
            long long blankNs = 0;                      // the host time at the vblank (the work of a late frame counts from it)
            int differs[2] = {-1, -1};                  // (log) bytes the fresh pad changed in each port's half, -1 none
            long long workStart = 0;                    // the logic's start (ns), for its length
            uint64_t workStartCycle = 0;
            double work[64] = {};                       // the last frames' work, ms
            uint64_t workCycles[64] = {};               // and EE cycles (from the vblank for a late read)
            uint32_t workCount = 0;
            int holdOff = 0;                            // frames read at the usual place
            uint64_t waited = 0, usual = 0, misses = 0, fresh = 0; // (log)
            long long lastLog = 0;
        } g_lateIn;

        double lateWorstWork()
        {
            double worst = 0.0;
            for (uint32_t i = 0; i < std::min<uint32_t>(g_lateIn.workCount, 64u); ++i)
                worst = std::max(worst, g_lateIn.work[i]);
            return worst;
        }

        uint64_t lateWorstCycles()
        {
            uint64_t worst = 0;
            for (uint32_t i = 0; i < std::min<uint32_t>(g_lateIn.workCount, 64u); ++i)
                worst = std::max(worst, g_lateIn.workCycles[i]);
            return worst;
        }

        // The bytes the emulated DualShock 2 answers a poll with (iop_dualshock2.cpp, pollByte): the buttons (low,
        // high; a pressed button 0), the right stick x, y, the left stick x, y, then the 12 pressures.
        uint8_t padByte(const ps2x::iop::PadInputState &state, size_t index)
        {
            const uint16_t wire = static_cast<uint16_t>(~state.buttons);
            switch (index)
            {
            case 0: return static_cast<uint8_t>(wire & 0xFFu);
            case 1: return static_cast<uint8_t>(wire >> 8u);
            case 2: return state.rightX;
            case 3: return state.rightY;
            case 4: return state.leftX;
            case 5: return state.leftY;
            default: break;
            }
            const size_t pressure = index - 6u;
            return pressure < state.pressure.size() ? state.pressure[pressure] : uint8_t{0u};
        }

        // Step 2's pad for one port: the newest half of libpad's buffer (the one FUN_001154a0 takes) gets the pad as it
        // is now. The bytes it changed, or -1 (the port not open, no good reply in the half, a config reply, no pad).
        int putFreshPad(uint8_t *rdram, uint32_t port)
        {
            uint32_t buffer = 0, open = 0;
            std::memcpy(&buffer, rdram + kPadBuffer + port * 0x70u, 4);
            std::memcpy(&open, rdram + kPadBuffer + 0x10u + port * 0x70u, 4);
            buffer &= 0x1FFFFFFFu;
            if (open == 0u || buffer == 0u || !inRam(buffer) || !inRam(buffer + 0xFFu))
                return -1;
            int32_t frame0 = 0, frame1 = 0;
            std::memcpy(&frame0, rdram + buffer + 0x58u, 4);
            std::memcpy(&frame1, rdram + buffer + 0xD8u, 4);
            uint8_t *half = rdram + buffer + (frame0 < frame1 ? 0x80u : 0u);
            const uint8_t id = half[1];
            const size_t bytes = static_cast<size_t>(id & 0x0Fu) * 2u;
            const uint8_t kind = id >> 4u;
            if (half[0] != 0u || (kind != 4u && kind != 7u) || bytes == 0u || 2u + bytes > 0x58u)
                return -1;
            ps2x::iop::PadInputState state;
            if (!ps2x::pad::hostPadState(port, 0u, state))
                return -1;
            int differs = 0;
            for (size_t i = 0; i < bytes; ++i)
            {
                const uint8_t value = padByte(state, i);
                differs += half[2u + i] != value ? 1 : 0;
                half[2u + i] = value;
            }
            return differs;
        }

        // STAR_LATE_TEST=1 (tests): the time from a key going down to the frame that shows it being handed over. A key
        // (d-pad up) goes down 8 s after the start and every 523 ms after (31.35 fields: each press at another point
        // of the field), for 250 ms, put in where the window's loop takes the keyboard in (a pad filter), so it goes
        // the keyboard's whole way; the frame whose logic first sees it held is handed over at the loop's next pad call
        // (right after the odd vblank's swap, just before FUN_001552a0).
        struct LateTest
        {
            std::atomic<long long> origin{0};
            int lastPress = -1;      // the game thread: the press last seen
            long long seenKey = 0;   // its time, waiting for the hand-over
            bool wasHeld = false;
            double sum = 0.0, low = 1e9, high = 0.0;
            int count = 0;
        } g_lateTest;
        constexpr long long kTestStart = 8000000000ll, kTestPeriod = 523000000ll, kTestDown = 250000000ll;

        void lateTestFilter(uint32_t player, ps2x::iop::PadInputState &state)
        {
            if (player != 0u)
                return;
            const long long now = steadyNowNs();
            long long origin = g_lateTest.origin.load(std::memory_order_relaxed);
            if (origin == 0)
            {
                origin = now;
                g_lateTest.origin.store(now, std::memory_order_relaxed);
            }
            const long long t = now - origin - kTestStart;
            if (t >= 0 && t % kTestPeriod < kTestDown)
            {
                state.buttons |= ps2x::iop::PadButton::Up;
                state.pressure[static_cast<size_t>(ps2x::iop::PadPressure::Up)] = 0xFFu;
            }
        }

        void lateTestSeen(const uint8_t *rdram)
        {
            uint32_t held = 0;
            std::memcpy(&held, rdram + 0x317230u, 4);
            const bool up = (held & 0x10u) != 0u;
            const long long origin = g_lateTest.origin.load(std::memory_order_relaxed);
            if (up && !g_lateTest.wasHeld && origin != 0)
            {
                const long long t = steadyNowNs() - origin - kTestStart;
                const int press = t >= 0 ? static_cast<int>(t / kTestPeriod) : -1;
                if (press >= 0 && press != g_lateTest.lastPress)
                {
                    g_lateTest.lastPress = press;
                    g_lateTest.seenKey = origin + kTestStart + static_cast<long long>(press) * kTestPeriod;
                }
            }
            g_lateTest.wasHeld = up;
        }

        void lateTestHandedOver()
        {
            if (g_lateTest.seenKey == 0)
                return;
            const double ms = static_cast<double>(steadyNowNs() - g_lateTest.seenKey) / 1e6;
            g_lateTest.seenKey = 0;
            g_lateTest.sum += ms;
            g_lateTest.low = std::min(g_lateTest.low, ms);
            g_lateTest.high = std::max(g_lateTest.high, ms);
            ++g_lateTest.count;
            std::printf("[late test] press %d: handed over %.1f ms after the key went down (%d presses: average %.1f, %.1f to %.1f)%c", g_lateTest.lastPress, ms,
                        g_lateTest.count, g_lateTest.sum / g_lateTest.count, g_lateTest.low, g_lateTest.high, 10);
        }

        void padUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            LateInput &l = g_lateIn;
            static const bool test = [] { const char *v = std::getenv("STAR_LATE_TEST"); return v && *v == '1'; }();
            if (test && getRegU32(ctx, 31) == kPadUpdateReturn)
                lateTestHandedOver();
            if (getRegU32(ctx, 31) == kPadUpdateReturn) // the loop's own call
            {
                l.step = LateInput::Step::Idle;
                if (live().lateInput.load(std::memory_order_relaxed))
                {
                    const uint64_t field = runtime->eeScheduler().vblankPeriodCycles();
                    if (l.holdOff > 0)
                        --l.holdOff;
                    else if (lateWorstWork() < 11.0 && (field == 0u || lateWorstCycles() < field * 7u / 10u))
                    {
                        l.step = LateInput::Step::PadSkipped;
                        ctx->pc = kPadUpdateReturn; // read later (gameLogicHook)
                        return;
                    }
                    ++l.usual;
                }
            }
            g_padUpdate(rdram, ctx, runtime); // nothing after the call
        }

        void gameLogicHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            LateInput &l = g_lateIn;
            EeScheduler &scheduler = runtime->eeScheduler();
            const uint32_t ra = getRegU32(ctx, 31);
            bool late = false;
            if (ra == kGameLogic)
            {
                // the late pad read (step 3) returns here: FUN_0012a520 as the loop calls it
                SET_GPR_U32(ctx, 31, kGameLogicReturn);
                std::memcpy(&ctx->f[12], &kGameLogicStep, 4);
                late = l.step == LateInput::Step::PadRead;
            }
            else if (ra == kGameLogicReturn && l.step == LateInput::Step::PadSkipped)
            {
                // 1. the next vblank (the thread comes back here, at FUN_0012a520, with its registers as they are)
                l.step = LateInput::Step::Waiting;
                const uint64_t tick = scheduler.currentVSyncTick();
                l.waitedTick = tick + 1u;
                scheduler.waitVSyncResume(tick, 0, kGameLogic); // does not return
            }
            else if (ra == kGameLogicReturn && l.step == LateInput::Step::Waiting)
            {
                // 2. at the vblank: the keys taken in, the IOP caught up, the pad as it is now in libpad's buffer
                l.blankCycle = scheduler.currentCycle();
                const long long start = steadyNowNs();
                l.blankNs = start;
                while (ps2_host_input::lastSampleNs() < start - 1000000ll && steadyNowNs() - start < 4000000ll)
                    std::this_thread::yield();
                l.sampleAge = steadyNowNs() - ps2_host_input::lastSampleNs();
                l.sampleWait = steadyNowNs() - start;
                ps2x::pad::catchUpIop(*runtime);
                for (uint32_t port = 0; port < 2u; ++port)
                    l.differs[port] = putFreshPad(rdram, port);
                if (l.differs[0] >= 0)
                    ++l.fresh;
                // 3. the pad read, returning here
                l.step = LateInput::Step::PadRead;
                SET_GPR_U32(ctx, 31, kGameLogic);
                ctx->pc = kPadUpdate;
                return;
            }
            if (ra == kGameLogic || ra == kGameLogicReturn)
            {
                static const bool test = [] { const char *v = std::getenv("STAR_LATE_TEST"); return v && *v == '1'; }();
                if (test)
                    lateTestSeen(rdram);
                l.workStart = late ? l.blankNs : steadyNowNs();
                l.workStartCycle = late ? l.blankCycle : scheduler.currentCycle();
                if (late)
                    ++l.waited;
                else
                    l.waitedTick = 0;
                // STAR_LATE_LOG=2 (tests): each frame
                static const bool frameLog = [] { const char *v = std::getenv("STAR_LATE_LOG"); return v && *v == '2'; }();
                if (frameLog)
                {
                    uint32_t held = 0;
                    std::memcpy(&held, rdram + 0x317230u, 4);
                    if (late)
                        std::printf("[late pad] field %llu: read late, %llu cycles after the vblank, keys %.2f ms old (waited %.2f ms), fresh pad changed %d / %d bytes; held %08x%c",
                                    static_cast<unsigned long long>(scheduler.currentVSyncTick()),
                                    static_cast<unsigned long long>(scheduler.currentCycle() - l.blankCycle),
                                    static_cast<double>(l.sampleAge) / 1e6, static_cast<double>(l.sampleWait) / 1e6, l.differs[0], l.differs[1], held, 10);
                    else
                        std::printf("[late pad] field %llu: read at the usual place; held %08x%c", static_cast<unsigned long long>(scheduler.currentVSyncTick()), held, 10);
                }
            }
            l.step = LateInput::Step::Idle;
            g_gameLogic(rdram, ctx, runtime); // nothing after the call
        }

        void frameSwapHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            LateInput &l = g_lateIn;
            if (getRegU32(ctx, 31) == kFrameSwapReturn && l.workStart != 0)
            {
                // the frame's work is done: how long it took, and whether it ended after the odd field's vblank
                EeScheduler &scheduler = runtime->eeScheduler();
                const double ms = static_cast<double>(steadyNowNs() - l.workStart) / 1e6;
                const uint64_t cycles = scheduler.currentCycle() - l.workStartCycle;
                // A frame of more than four fields is a load (a new area, a door: 1.35 s measured), not the cost of
                // play: it neither counts as a miss nor as work.
                const uint64_t field = scheduler.vblankPeriodCycles();
                const bool load = ms > 70.0 || (field != 0u && cycles > 4u * field);
                if (!load)
                {
                    l.work[l.workCount % 64u] = ms;
                    l.workCycles[l.workCount % 64u] = cycles;
                    ++l.workCount;
                }
                l.workStart = 0;
                const uint64_t tick = scheduler.currentVSyncTick();
                if (l.waitedTick != 0 && tick > l.waitedTick && !load)
                {
                    ++l.misses;
                    l.holdOff = 300;
                    std::printf("[STAR] late input: a frame's work (%.1f ms, %llu EE cycles from the vblank) ended after its vblank; the pad is read at the usual place for 300 frames%c",
                                ms, static_cast<unsigned long long>(cycles), 10);
                }
                l.waitedTick = 0;
                static const bool log = [] { const char *v = std::getenv("STAR_LATE_LOG"); return v && (*v == '1' || *v == '2'); }();
                if (log)
                {
                    const long long now = steadyNowNs();
                    if (now - l.lastLog > 1000000000ll)
                    {
                        l.lastLog = now;
                        std::printf("[late] read late %llu (with the fresh pad %llu), at the usual place %llu, misses %llu; work %.1f ms %llu cycles, longest of the last 64 %.1f ms %llu cycles (a field %llu)%c",
                                    static_cast<unsigned long long>(l.waited), static_cast<unsigned long long>(l.fresh),
                                    static_cast<unsigned long long>(l.usual), static_cast<unsigned long long>(l.misses), ms,
                                    static_cast<unsigned long long>(cycles), lateWorstWork(), static_cast<unsigned long long>(lateWorstCycles()),
                                    static_cast<unsigned long long>(scheduler.vblankPeriodCycles()), 10);
                    }
                }
            }
            g_frameSwap(rdram, ctx, runtime); // nothing after the call
        }

        // STAR 2026-11-24, a gameplay tweak (off unless the user turns it on): mouse and keyboard play.
        //
        // The keyboard is player 1's pad through the bindings main.cpp sets (W A S D and the arrows the d-pad, E /
        // Enter / Space circle, Esc / Tab / Backspace cross, Q square, R triangle, F start, P select, Left Shift L3).
        // While the keyboard or the mouse was the device used last, the game is told that the control type is 4
        // (FUN_001a9450, asked only by the player's own code: the option and its menu are untouched), where the d-pad
        // walks and strafes, L1 R1 L2 R2 are the four swings and R1 / L1 draw, fire and put away a gun. Then, before
        // the game turns the pad into amounts (FUN_001c4760, which runs only while the player may act):
        //   - the mouse's movement turns the view directly (applied in the look hook): no steps, no top speed;
        //   - the wheel presses triangle (the other weapon), the middle button square (magic);
        //   - a gun (kinds 7, 8, 10-18; state byte player + 0x555: 0 put away, 3 out): the right button draws it and
        //     puts it away (R1 / L1), pressed again or held as the user chose; the left button fires (R1);
        //   - a bow-like weapon (kinds 9, 0x13): the left button is R1, held to draw and let go to shoot; the right
        //     button while drawn is L1, which lets it down;
        //   - any other weapon (2026-11-25, the user's second design; they found the first "a bit clunky"): the way
        //     the view last moved is remembered and shown by a small fading arrow head, and the left button swings
        //     that way (right L1, left R1, up L2, down R2: what the right stick's four directions do in Type 1).
        //     A swing asked for while the game cannot swing yet waits up to a second. The first design stays as an
        //     option (combat_mode): holding the right button locks the view and a flick of the mouse (about 6
        //     degrees of look within a few frames; the next needs the mouse to slow down first) swings that way;
        //   - Left Shift (L3, which play does not use) limits the d-pad's amounts to walking pace.
        // The buttons are added to the game's pad words for this frame (held 0x317230, newly pressed 0x317238), so
        // the game's own code does the rest, the ailments that stop input included.
        // STAR_KBM_LOG=1: the first 600 frames with anything to say. STAR_KBM_TEST: see testMouse.
        constexpr uint32_t kReadInput = 0x1C4760u;   // FUN_001c4760(a0 = player, a1 = amounts)
        constexpr uint32_t kControlType = 0x1A9450u; // FUN_001a9450(): the control type option, a byte
        PS2Runtime::RecompiledFunction g_readInput = nullptr;
        PS2Runtime::RecompiledFunction g_controlType = nullptr;
        constexpr uint32_t kPadL3 = 0x0002u, kPadUp = 0x0010u, kPadDown = 0x0040u, kPadL2 = 0x0100u, kPadR2 = 0x0200u,
                           kPadL1 = 0x0400u, kPadR1 = 0x0800u, kPadTriangle = 0x1000u, kPadCircle = 0x2000u,
                           kPadCross = 0x4000u, kPadSquare = 0x8000u;

        // The tests' mouse (lookMouseAt): the scripted steps taken so far, plus a steady movement.
        std::mutex g_testMouseMutex;
        double g_testMouseX = 0.0, g_testMouseY = 0.0;
        struct SteadyMouse
        {
            bool on = false;
            double x = 0.0, y = 0.0, from = 0.0, to = 1e9; // counts a second; seconds after the first frame of play
            double swing = 0.0;                            // seconds: the speed goes as a sine of this period instead
            std::atomic<long long> start{0};
            SteadyMouse()
            {
                // STAR_LIVE_TEST=<x>:<y>[:<from>:<to>[:<swing>]] (tests): the mouse moves x, y counts a second, without
                // end or only from..to seconds after play began: a perfectly even movement to judge the pictures by.
                // With <swing> (seconds) the speed is x, y times the sine of that period: it grows, falls and turns
                // back smoothly, as a hand does.
                if (const char *text = std::getenv("STAR_LIVE_TEST"); text && *text)
                    on = std::sscanf(text, "%lf:%lf:%lf:%lf:%lf", &x, &y, &from, &to, &swing) >= 2;
            }
        };
        SteadyMouse g_steadyMouse;

        long long steadyNowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // the margins of the game's "in view" test (inViewHook, further down)
        void setViewMargins(double yawNeg, double yawPos, double pitchNeg, double pitchPos);
        void viewMarginsFor(double before, double now, double &neg, double &pos);
        bool viewMarginsOn();

        // STAR_KBM_TEST=<from>-<to>:<dx>:<dy>:<buttons>:<wheel>[,...] (tests; a hidden run has no mouse): during the
        // frames from..to of play, counted from the first, the mouse moves dx, dy counts a frame with these buttons
        // held (1 left, 2 right, 4 middle, 8 Mouse 4, 16 Mouse 5; 256, 512 ...: the game layer's keys 0, 1 ... of
        // bindings::Resolved::gameKeys), and the wheel turns `wheel` notches at the first of them.
        void testMouse(uint64_t frame, ps2_host_input::MouseGameInput &mouse, bool &keys)
        {
            struct Step
            {
                unsigned long long from, to;
                float dx, dy;
                unsigned held;
                int wheel;
            };
            static const std::vector<Step> steps = []
            {
                std::vector<Step> list;
                const char *text = std::getenv("STAR_KBM_TEST");
                while (text && *text)
                {
                    Step step{};
                    if (std::sscanf(text, "%llu-%llu:%f:%f:%u:%d", &step.from, &step.to, &step.dx, &step.dy, &step.held, &step.wheel) == 6)
                        list.push_back(step);
                    text = std::strchr(text, ',');
                    if (text)
                        ++text;
                }
                return list;
            }();
            static const bool on = std::getenv("STAR_KBM_TEST") != nullptr || g_steadyMouse.on;
            if (!on)
                return;
            static uint32_t before = 0u;
            mouse = {};
            mouse.captured = true;
            keys = true;
            if (g_steadyMouse.start.load(std::memory_order_relaxed) == 0)
                g_steadyMouse.start.store(steadyNowNs(), std::memory_order_relaxed);
            double dx = 0.0, dy = 0.0;
            for (const Step &step : steps)
                if (frame >= step.from && frame <= step.to)
                {
                    dx += step.dx;
                    dy += step.dy;
                    mouse.held |= step.held;
                    if (frame == step.from)
                        mouse.wheel += step.wheel;
                }
            {
                std::lock_guard<std::mutex> lock(g_testMouseMutex);
                g_testMouseX += dx; // readInputHook takes the movement from lookMouseAt, like the real mouse's
                g_testMouseY += dy;
            }
            mouse.keysHeld = (mouse.held >> 8) & 0xFFu;
            mouse.held &= 0x1Fu;
            mouse.pressed = mouse.held & ~before;
            mouse.keysPressed = mouse.keysHeld & ~(before >> 8);
            before = mouse.held | (mouse.keysHeld << 8);
        }

        bool testMouseOn()
        {
            static const bool on = std::getenv("STAR_KBM_TEST") != nullptr || g_steadyMouse.on;
            return on;
        }

        uint16_t guestHalf(uint8_t *rdram, uint32_t address)
        {
            uint16_t value = 0;
            std::memcpy(&value, rdram + address, 2);
            return value;
        }

        void setGuestHalf(uint8_t *rdram, uint32_t address, uint16_t value)
        {
            std::memcpy(rdram + address, &value, 2);
        }

        void mouseKeysPlay(uint8_t *rdram, uint32_t player, const ps2_host_input::MouseGameInput &mouse)
        {
            MouseKeys &mk = g_mouseKeys;
            if (!inRam(player))
                return;
            const uint32_t inner = guestWord(rdram, player + 0x518u);
            if (!inRam(inner))
                return;
            // STAR 2026-11-26: the inputs are the player's (bindings.h; the defaults are the buttons this had before).
            const std::shared_ptr<const bindings::Resolved> binds = bindings::liveResolved();
            const std::vector<bindings::Action> &actions = bindings::actions();
            static const auto indexOf = [](const char *id)
            {
                const std::vector<bindings::Action> &list = bindings::actions();
                for (size_t i = 0; i < list.size(); ++i)
                    if (std::string(list[i].id) == id)
                        return i;
                return size_t{0};
            };
            static const size_t kFire = indexOf("fire"), kGun = indexOf("gun"), kSwingView = indexOf("swing_view"),
                                kSwingFlick = indexOf("swing_flick"), kSwingRightLeft = indexOf("swing_right_left"),
                                kSwingLeftRight = indexOf("swing_left_right"), kStab = indexOf("stab"), kOverhead = indexOf("overhead");
            uint32_t gunMouse = 0u, gunKeys = 0u; // what draws and puts away a gun
            for (const bindings::Input &in : binds->inputs[kGun])
            {
                if (in.type == bindings::Input::MouseButton)
                    gunMouse |= 1u << in.bit;
                else if (in.type == bindings::Input::Key)
                    gunKeys |= 1u << in.bit;
            }
            if (mouse.resumed || !mouse.captured)
            {
                // a button held since a menu is not a button of play: a click that closed a menu must not fire. The
                // gun's button is: it fires nothing, and with "hold" it keeps the gun out (held through a loaded
                // state it would otherwise put the gun away until it was let go and pressed again)
                mk.heldInPlay = mouse.captured ? (mouse.held & gunMouse) : 0u;
                mk.flickX = mk.flickY = 0.0f;
                mk.flickArmed = true;
                mk.swingFrames = 0;
                mk.gunWishFrames = mk.fireFrames = 0;
            }
            if (mouse.resumed)
                mk.keysInPlay = mouse.keysHeld & gunKeys; // (keys count without the mouse captured too)
            mk.heldInPlay = (mk.heldInPlay | mouse.pressed) & mouse.held;
            mk.keysInPlay = (mk.keysInPlay | mouse.keysPressed) & mouse.keysHeld;
            const auto inputHeld = [&](const bindings::Input &in)
            {
                if (in.type == bindings::Input::MouseButton)
                    return (mk.heldInPlay & (1u << in.bit)) != 0u;
                if (in.type == bindings::Input::Key)
                    return (mk.keysInPlay & (1u << in.bit)) != 0u;
                return false; // (the wheel turns, it is not held)
            };
            const auto inputPressed = [&](const bindings::Input &in)
            {
                if (in.type == bindings::Input::MouseButton)
                    return (mouse.pressed & (1u << in.bit)) != 0u;
                if (in.type == bindings::Input::Key)
                    return (mouse.keysPressed & (1u << in.bit)) != 0u;
                return ((in.bit & 1u) != 0u && mouse.wheel > 0) || ((in.bit & 2u) != 0u && mouse.wheel < 0);
            };
            const auto actionHeld = [&](size_t index)
            { return std::any_of(binds->inputs[index].begin(), binds->inputs[index].end(), inputHeld); };
            const auto actionPressed = [&](size_t index)
            { return std::any_of(binds->inputs[index].begin(), binds->inputs[index].end(), inputPressed); };
            // fire / draw a bow (left), draw / put away a gun (right): by default the left and the right button
            const bool left = actionHeld(kFire), right = actionHeld(kGun);
            const bool leftPress = actionPressed(kFire), rightPress = actionPressed(kGun);
            // which of the game's swings the wheel gives with "a button for each swing": up (L2) and down (R2)
            constexpr uint32_t kSwingWheelUp = kPadL2, kSwingWheelDown = kPadR2;
            const int meleeMode = live().meleeMode.load(std::memory_order_relaxed);
            const bool combatMode = meleeMode == 1, swingButtons = meleeMode == 2;
            uint32_t held = 0u, pressed = 0u; // the pad buttons this frame adds
            const auto press = [&](uint32_t button)
            {
                held |= button;
                pressed |= button;
            };
            // The general actions' mouse buttons and wheel (their keys are player 1's bindings in the runtime): by default
            // the wheel switches weapons (not while the swings in use take it) and the middle button is magic.
            for (size_t a = 0; a < actions.size() && a < binds->inputs.size(); ++a)
                if (actions[a].kind == bindings::Kind::General)
                    for (const bindings::Input &in : binds->inputs[a])
                    {
                        if (inputPressed(in))
                            press(actions[a].padButton);
                        else if (inputHeld(in))
                            held |= actions[a].padButton;
                    }

            const float percent = static_cast<float>(live().mouseSensitivity.load(std::memory_order_relaxed)) / 100.0f;
            const uint8_t kind = rdram[inner + 0x51Cu], state = rdram[inner + 0x555u];
            const bool bow = kind == 9u || kind == 0x13u;
            const bool gun = kind == 7u || kind == 8u || (kind >= 10u && kind <= 0x12u);
            bool lockView = false;
            // the mouse's recent movement, fading: a steady 50 counts a frame settles at 125
            const bool flickHeld = actionHeld(kSwingFlick), flickPress = actionPressed(kSwingFlick);
            if (rightPress || flickPress)
            {
                mk.flickX = mk.flickY = 0.0f;
                mk.flickArmed = true;
            }
            mk.flickX = mk.flickX * 0.6f + mouse.x * percent;
            mk.flickY = mk.flickY * 0.6f + mouse.y * percent;
            const float flick = std::sqrt(mk.flickX * mk.flickX + mk.flickY * mk.flickY);
            const auto flickButton = [&]() -> uint32_t
            {
                if (std::fabs(mk.flickX) >= std::fabs(mk.flickY))
                    return mk.flickX > 0.0f ? kPadL1 : kPadR1; // right : left
                return mk.flickY < 0.0f ? kPadL2 : kPadR2;     // up : down
            };
            if (gun)
            {
                // The game takes a button only in the states 0 and 3, not while the gun is being drawn, fired or
                // reloaded, so a click is remembered until it can act: out / away for 45 frames, a shot for 8.
                const bool hold = live().gunHold.load(std::memory_order_relaxed);
                if (hold)
                {
                    mk.gunWish = right ? 1 : -1;
                    mk.gunWishFrames = 2;
                }
                else if (rightPress || (leftPress && state == 0u))
                {
                    mk.gunWish = state == 0u ? 1 : -1;
                    mk.gunWishFrames = 45;
                }
                if (leftPress && state != 0u)
                    mk.fireFrames = 8;
                if (state == 0u)
                {
                    mk.fireFrames = 0;
                    if (mk.gunWish > 0 && mk.gunWishFrames > 0)
                        press(kPadR1); // draw
                    else if (mk.gunWish < 0)
                        mk.gunWishFrames = 0; // it is away
                }
                else if (state == 3u)
                {
                    if (mk.gunWish > 0)
                        mk.gunWishFrames = 0; // it is out
                    if (mk.gunWish < 0 && mk.gunWishFrames > 0)
                        press(kPadL1); // put away
                    else if (mk.fireFrames > 0)
                    {
                        press(kPadR1); // fire
                        mk.fireFrames = 0;
                    }
                    else if (left)
                        held |= kPadR1; // keeps firing the weapons that do
                }
                if (mk.gunWishFrames > 0)
                    --mk.gunWishFrames;
                if (mk.fireFrames > 0)
                    --mk.fireFrames;
                mk.swingFrames = 0;
            }
            else if (bow)
            {
                mk.gunWishFrames = mk.fireFrames = 0;
                if (state == 0u)
                {
                    if (leftPress)
                        press(kPadR1); // begin to draw
                }
                else
                {
                    if (left)
                        held |= kPadR1; // letting go shoots
                    if (rightPress && left)
                        press(kPadL1); // let it down
                }
                mk.swingFrames = 0;
            }
            else
            {
                constexpr float kFlick = 120.0f;
                mk.gunWishFrames = mk.fireFrames = 0;
                const auto codeOf = [](uint32_t button) { return button == kPadL2 ? 1 : button == kPadL1 ? 2 : button == kPadR2 ? 3 : 4; };
                const auto showMark = [&](uint32_t button)
                { GSPcsx2Backend::SetOption("swing_mark", std::to_string(codeOf(button)).c_str()); };
                uint32_t direction = 0u;
                if (swingButtons)
                {
                    // A button for each swing (the user's third way, 2026-11-25): the left button swings from right
                    // to left, the right button from left to right, the wheel up stabs, the wheel down is the
                    // overhead swing.
                    mk.flickArmed = true;
                    // After a swing has been asked for, nothing more is taken for 0.4 s (the user, 2026-11-24: one
                    // notch of the wheel too many queued a second swing, which costs stamina and the weapon's
                    // durability). A swing asked for later, while the first is still going, waits for it as before.
                    constexpr int kSwingLock = 12;
                    const uint32_t asked = actionPressed(kSwingRightLeft)   ? kPadR1
                                           : actionPressed(kSwingLeftRight) ? kPadL1
                                           : actionPressed(kStab)           ? kSwingWheelUp
                                           : actionPressed(kOverhead)       ? kSwingWheelDown
                                                                            : 0u;
                    if (mk.swingLock > 0)
                        --mk.swingLock;
                    else if (asked != 0u)
                    {
                        direction = asked;
                        mk.swingLock = kSwingLock;
                    }
                }
                else if (flickHeld && combatMode)
                {
                    lockView = true; // combat mode (an option): the mouse swings, the view stays
                    if (!mk.flickArmed)
                        mk.flickArmed = flick < kFlick * 0.5f;
                    else if (flick > kFlick)
                    {
                        direction = flickButton();
                        mk.flickArmed = false;
                        mk.flickX = mk.flickY = 0.0f;
                    }
                }
                else
                {
                    mk.flickArmed = true;
                    // The way the view last moved is the way of the next swing (the user, 2026-11-25, after the
                    // medieval combat games): a clear movement of the mouse, one axis well ahead of the other, sets
                    // it; a small arrow head near the middle of the picture shows it and fades (pcsx2gs, option
                    // swing_mark). The left button then swings that way.
                    constexpr float kAim = 45.0f;
                    const float ax = std::fabs(mk.flickX), ay = std::fabs(mk.flickY);
                    if (flick > kAim && std::max(ax, ay) > 1.4f * std::min(ax, ay))
                    {
                        const uint32_t way = flickButton();
                        if (way != mk.aimSwing)
                        {
                            mk.aimSwing = way;
                            showMark(way);
                        }
                    }
                    if (actionPressed(kSwingView))
                    {
                        direction = mk.aimSwing;
                        showMark(direction);
                    }
                }
                if (direction != 0u)
                {
                    mk.swing = direction; // the newest wish replaces one still waiting
                    mk.swingFrames = 30;
                    mk.swingSent = false;
                }
                // The game (FUN_001c23e0) starts a swing only when the weapon is at rest (state 0), the attack count
                // (player + 0x516) is back above 0, about half a second after the last swing ended, and the direction
                // is not the one it remembers (player + 0x54c; a frame without any direction forgets it). A flick
                // made too early would be lost, so it waits here, for a second at most, and its button is given at
                // the first frame the game can take it.
                if (mk.swingFrames > 0)
                {
                    --mk.swingFrames;
                    const uint8_t code = mk.swing == kPadL2 ? 1u : mk.swing == kPadL1 ? 2u : mk.swing == kPadR2 ? 3u : 4u;
                    if (state != 0u)
                    {
                        if (mk.swingSent)
                            mk.swingFrames = 0; // it began
                    }
                    else if (static_cast<int8_t>(rdram[inner + 0x516u]) > 0 && rdram[inner + 0x54Cu] != code)
                    {
                        held |= mk.swing;
                        mk.swingSent = true;
                    }
                }
            }

            // The view: 0.05 degrees a count at 100%. Not while the game holds the player still (the first 45 frames
            // of an area, FUN_001befa0; ailment 1, FUN_001ce200(1), which empties the amounts).
            const uint32_t stats = guestWord(rdram, inner + 0x510u);
            bool still = rdram[inner + 0x52Fu] < 0x2Du;
            if (inRam(stats) && guestWord(rdram, stats + 0x54u) == 0u && (guestWord(rdram, stats + 0x34u) >> 24) != 0u)
                still = true;
            const bool follows = !lockView && !still && mouse.captured;
            // The game's own slow-downs of turning (the user, 2026-11-25: keep them with the mouse, "without going
            // too far"). The pad's turn is its amount x 0.0489 x the factor at player + 0x5ac x a half with ailment 6
            // (FUN_001c2db0, FUN_001c27c0). The factor follows FUN_001c2640 by a fifth a frame: 1, or less on some
            // ground (0.5), with too much load or ailment 3 (2/3 or 1/3); it is also halved while the game makes the
            // player walk (player + 0x54d, a weapon held ready), which is not taken over: that is no ailment, and
            // the mouse aims with it.
            double slow = 1.0;
            if (const int mode = live().mouseSlowdown.load(std::memory_order_relaxed); mode != 0)
            {
                double factor = guestFloat(rdram, inner + 0x5ACu);
                if (rdram[inner + 0x54Du] != 0u)
                    factor *= 2.0;
                if (!(factor > 0.0) || factor > 1.0)
                    factor = 1.0;
                if (inRam(stats) && guestWord(rdram, stats + 0x48u) != 0u)
                    factor *= 0.5;
                // STAR_SLOWDOWN_TEST=<factor> (tests): the game's factor is taken to be this
                static const double test = [] { const char *e = std::getenv("STAR_SLOWDOWN_TEST"); return e && *e ? std::atof(e) : 0.0; }();
                if (test > 0.0)
                    factor = test;
                slow = mode == 2 ? factor : std::sqrt(factor);
            }
            const double perCount = 0.05 * 3.14159265358979 / 180.0 * percent * slow;
            const double pitchSign = live().mouseInvertY.load(std::memory_order_relaxed) ? 1.0 : -1.0;
            // (the frame in which the view is locked still takes the movement made up to it: the pictures have
            // already shown it, see live_look.h)
            if (mouse.captured && !still && (!lockView || !mk.wasLocked))
            {
                mk.yaw = static_cast<float>(mouse.x * perCount);
                mk.pitch = static_cast<float>(pitchSign * mouse.y * perCount);
            }
            mk.wasLocked = lockView;
            {
                // For the in-between pictures (live_look.h): what this frame does to the view, and from where the
                // mouse counts on.
                const uint32_t camera = guestWord(rdram, inner + 0x198u);
                LookFrame look;
                look.seq = ++mk.seq;
                look.yaw = mk.yaw;
                if (inRam(camera))
                {
                    const double before = guestFloat(rdram, camera + 0x20u);
                    look.pitchNow = std::clamp(before + mk.pitch, -1.0471976, 0.6108653);
                    look.pitch = look.pitchNow - before;
                }
                look.rawX = mk.totalX;
                look.rawY = mk.totalY;
                look.perCountYaw = follows ? perCount : 0.0;
                look.perCountPitch = follows ? pitchSign * perCount : 0.0;
                {
                    // the margins of the "in view" test for the scenery this frame chooses (inViewHook)
                    double margins[4];
                    viewMarginsFor(mk.lastYaw, follows ? look.yaw : 0.0, margins[0], margins[1]);
                    viewMarginsFor(mk.lastPitch, follows ? look.pitch : 0.0, margins[2], margins[3]);
                    // STAR_VIEW_MARGIN=<degrees>[:<degrees up and down>] (tests): fixed margins, the same each way;
                    // 0: none, the game's own test
                    static const char *const fixed = std::getenv("STAR_VIEW_MARGIN");
                    if (fixed && *fixed)
                    {
                        float yaw = 0.0f, pitch = 0.0f;
                        const int got = std::sscanf(fixed, "%f:%f", &yaw, &pitch);
                        margins[0] = margins[1] = got >= 1 ? yaw : 0.0;
                        margins[2] = margins[3] = got >= 2 ? pitch : margins[0];
                    }
                    setViewMargins(margins[0], margins[1], margins[2], margins[3]);
                    look.marginYawNeg = margins[0] / 57.29578;
                    look.marginYawPos = margins[1] / 57.29578;
                    look.marginPitchNeg = margins[2] / 57.29578;
                    look.marginPitchPos = margins[3] / 57.29578;
                    mk.lastYaw = look.yaw;
                    mk.lastPitch = look.pitch;
                }
                publishLookFrame(look);
            }

            const uint16_t padHeld = guestHalf(rdram, kPadState);
            if (held != 0u || pressed != 0u)
            {
                setGuestHalf(rdram, kPadState, static_cast<uint16_t>(padHeld | held));
                setGuestHalf(rdram, kPadState + 8u, static_cast<uint16_t>(guestHalf(rdram, kPadState + 8u) | pressed));
            }
            if ((padHeld & kPadL3) != 0u)
            {
                // walking pace: forward and back under the running amount of 1, sideways under the fast 0.4
                // (FUN_001c4760 multiplies the d-pad's pressures by 7 and by 3)
                const auto cap = [&](uint32_t address, float most)
                {
                    if (guestFloat(rdram, address) > most)
                        setGuestFloat(rdram, address, most);
                };
                cap(kPadState + 0x1Cu, 0.3f / 3.0f); // right
                cap(kPadState + 0x20u, 0.3f / 3.0f); // left
                cap(kPadState + 0x24u, 0.5f / 7.0f); // up
                cap(kPadState + 0x28u, 0.5f / 7.0f); // down
            }
            static const bool log = [] { const char *v = std::getenv("STAR_KBM_LOG"); return v && *v == '1'; }();
            static int lines = 0;
            if (log && lines < 600 && (held != 0u || pressed != 0u || mouse.x != 0.0f || mouse.y != 0.0f || mouse.held != 0u || padHeld != 0u))
            {
                ++lines;
                std::printf("[kbm] frame %llu weapon %u state %u mouse %+.0f %+.0f buttons %x pad %04x adds held %04x pressed %04x turn %+.4f %+.4f flick %.0f swings left %d last %u flags %02x speed %.1f%s%c",
                            static_cast<unsigned long long>(mk.frame), kind, state, mouse.x, mouse.y, mouse.held, padHeld, held, pressed,
                            mk.yaw, mk.pitch, flick, static_cast<int8_t>(rdram[inner + 0x516u]), rdram[inner + 0x54Cu], rdram[inner + 0x556u],
                            std::hypot(guestFloat(rdram, inner + 0x1D0u), guestFloat(rdram, inner + 0x1D8u)),
                            lockView ? " view locked" : "", 10);
            }
        }

        // STAR 2026-11-25 (the user saw it on the Debug page: "a typo on the description for the ying yang charm ... some
        // character isn't recognized"; "yes, fix the broken text please, in-game and the debug menu"). Four item texts of
        // the English patch have the two bytes 0xfb 0x71 where one letter belongs (and the Stun Gun's last line has two
        // letters too many). The items' texts are the table at *(*(0x202160)) (menu/dbitem.txm, loaded by FUN_00195c00):
        // "STA ", count, 8 zero bytes, records of {item, offset of the name, offset of the text}. A text is put right in
        // its place only while it is exactly the damaged one (shorter, so it fits); looked at every 32 frames, because a
        // loaded save state brings the damaged bytes back. With the text fixes (text_fixes) on.
        struct ItemText
        {
            uint32_t item;
            const char *damaged, *right;
        };
        constexpr ItemText kItemTexts[] = {
            {17, "Boosts spirit to resist\nl\xfbqght/dark temporarily", "Boosts spirit to resist\nlight/dark temporarily"},
            {301, "Slash-defiant\nleather \xfbqloves", "Slash-defiant\nleather gloves"},
            {904, "High voltage bill\xfbq-club\nRare\n+ Buqurdens", "High voltage billy-club\nRare\n+ Burdens"},
            {968, "Old-style 6-shot\xfbq\nrevolver called\nthe prototype\nfor all handguns", "Old-style 6-shot\nrevolver called\nthe prototype\nfor all handguns"},
        };

        void fixItemTexts(uint8_t *rdram)
        {
            if (!live().textFixes.load(std::memory_order_relaxed))
                return;
            const uint32_t holder = guestWord(rdram, 0x202160u) & 0x1FFFFFFFu;
            const uint32_t table = inRam(holder) ? guestWord(rdram, holder) & 0x1FFFFFFFu : 0u;
            if (!inRam(table) || std::memcmp(rdram + table, "STA ", 4) != 0)
                return;
            const uint32_t count = guestWord(rdram, table + 4u);
            if (count == 0u || count > 4000u || !inRam(table + 16u + count * 12u))
                return;
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t item = guestWord(rdram, table + 16u + k * 12u);
                for (const ItemText &text : kItemTexts)
                {
                    if (text.item != item)
                        continue;
                    const uint32_t at = table + guestWord(rdram, table + 24u + k * 12u);
                    const size_t was = std::strlen(text.damaged) + 1u, now = std::strlen(text.right) + 1u;
                    if (!inRam(at) || !inRam(at + static_cast<uint32_t>(was)) || std::memcmp(rdram + at, text.damaged, was) != 0)
                        continue;
                    std::memset(rdram + at, 0, was);
                    std::memcpy(rdram + at, text.right, now);
                    std::printf("[STAR fix] item %u's text had a damaged character: put right%c", item, 10);
                }
            }
        }

        void readInputHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            MouseKeys &mk = g_mouseKeys;
            if (static uint32_t looks = 0; (looks++ & 31u) == 0u)
                fixItemTexts(rdram);
            debugMenuGameThread(rdram, ctx, runtime, mk.frame); // the F2 menu's Debug page (debug_menu.cpp)
            {
                // The game takes no attack of any kind (FUN_001c1900) while the equipped weapon's sounds are not
                // loaded: the byte at 0x201ed7 (FUN_0012c400(3); FUN_0012da90 clears it and starts the loading,
                // FUN_0012c7c0 sets it when the sounds are in the sound chip's memory). Told in the log when it changes.
                static int ready = -1;
                static long long since = 0;
                const int now = rdram[0x201ED7u] != 0u ? 1 : 0;
                if (now != ready)
                {
                    const long long ns = steadyNowNs();
                    std::printf("[STAR] the weapon's sounds are %s%s%c", now ? "loaded" : "being loaded: the game takes no attack until they are",
                                now && ready == 0 ? (" (after " + std::to_string((ns - since) / 1000000ll) + " ms)").c_str() : "", 10);
                    ready = now;
                    since = ns;
                }
            }
            mk.yaw = mk.pitch = 0.0f;
            mk.active = false;
            if (live().mouseKeyboard.load(std::memory_order_relaxed))
            {
                ps2_host_input::MouseGameInput mouse = ps2_host_input::consumeMouseGameInput();
                bool keys = ps2_host_input::keysLastUsed();
                testMouse(mk.frame, mouse, keys);
                {
                    // The movement since the frame before, from the record that the in-between pictures read too
                    // (the window loop reads the mouse about every millisecond; ps2_host_input::mouseTotalAt).
                    double x = 0.0, y = 0.0;
                    const bool have = lookMouseAt(steadyNowNs(), x, y);
                    const bool counts = have && mk.totalValid && mouse.captured && !mouse.resumed;
                    mouse.x = counts ? static_cast<float>(x - mk.totalX) : 0.0f;
                    mouse.y = counts ? static_cast<float>(y - mk.totalY) : 0.0f;
                    mk.totalX = x;
                    mk.totalY = y;
                    mk.totalValid = have && mouse.captured;
                }
                mk.active = keys;
                if (keys)
                    mouseKeysPlay(rdram, getRegU32(ctx, 4), mouse);
                else
                {
                    mk.heldInPlay = mk.keysInPlay = 0u;
                    mk.swingFrames = 0;
                    mk.lastYaw = mk.lastPitch = 0.0;
                    setViewMargins(0.0, 0.0, 0.0, 0.0); // the pad: the game's own "in view" test
                }
                ++mk.frame;
            }
            else if (viewMarginsOn())
                setViewMargins(0.0, 0.0, 0.0, 0.0);
            g_readInput(rdram, ctx, runtime); // nothing after the call
        }

        void controlTypeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_mouseKeys.active)
            {
                SET_GPR_U32(ctx, 2, 4u);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_controlType(rdram, ctx, runtime); // nothing after the call
        }

        // STAR 2026-11-24: texts of the English patch that do not fit the place the game gives them (the user's
        // screenshot of Item Repair: "Consume" with the cost 581 drawn on top of "HP"). The window's code
        // (FUN_0019f050) draws its lines with FUN_001875c0(f12 = x, f13 = y, f14 = size, a0 = text, a1 = 1 centred on
        // x / 0 from x on, a2 = colour): the title and the item's name centred on 320, then the label (text 0x9e,
        // "Consume HP") from x 250 and the cost from x 340, a gap made for the Japanese label, then the question
        // centred. The user wants the patch's wording kept, so the layout is changed instead: the label's call draws
        // nothing (its text is remembered), and the cost's call draws "<label> <cost>" as one line centred on 320, by
        // the game's own centring, like the lines around it. live().textFixes off: as the game does it.
        // More such pairs can be added by their call sites (PS2X_CALL_WATCH_FN=1875c0 lists the calls).
        constexpr uint32_t kDrawText = 0x1875C0u;
        PS2Runtime::RecompiledFunction g_drawText = nullptr;
        struct LabelAndNumber
        {
            uint32_t labelReturn, numberReturn; // the return addresses of the two calls
            float centre;
        };
        constexpr LabelAndNumber kLabelAndNumber[] = {
            {0x19F150u, 0x19F2B0u, 320.0f}, // Item Repair: "Consume HP" + the cost
        };

        void drawTextHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (live().textFixes.load(std::memory_order_relaxed))
            {
                static std::string label; // the game thread only
                const uint32_t back = getRegU32(ctx, 31), text = getRegU32(ctx, 4) & 0x1FFFFFFFu;
                for (const LabelAndNumber &pair : kLabelAndNumber)
                {
                    if (text < 0x00100000u || text > 0x02000000u - 0x200u)
                        break;
                    if (back == pair.labelReturn)
                    {
                        // the label, in the caller's buffer: kept for the cost's call, and nothing drawn now
                        label.assign(reinterpret_cast<const char *>(rdram + text), strnlen(reinterpret_cast<const char *>(rdram + text), 96));
                        rdram[text] = 0u;
                    }
                    else if (back == pair.numberReturn && !label.empty())
                    {
                        // the cost, in the same buffer (256 bytes): the whole line, centred
                        const std::string number(reinterpret_cast<const char *>(rdram + text), strnlen(reinterpret_cast<const char *>(rdram + text), 32));
                        const std::string line = label + " " + number;
                        std::memcpy(rdram + text, line.c_str(), line.size() + 1);
                        SET_GPR_U32(ctx, 5, 1u);
                        ctx->f[12] = pair.centre;
                        label.clear();
                    }
                }
            }
            g_drawText(rdram, ctx, runtime); // nothing after the call
        }

        // STAR 2026-11-25: the game's "is it in view" test, FUN_00153fa0(a0 = a sphere: centre x y z in the world
        // and the radius). It takes the centre through the camera's matrix (0x2f1640; 0x2f1600 while the word at
        // 0x201f28 is set) and holds it against two sets of side planes, each a pair (cos h, sin h) for the half
        // angle h: the view's own (37.5 degrees sideways at 0x201f70, 28.2 up and down at 0x201f68) and the GS's
        // drawing space, the "guard band" (78.2 degrees both ways, 0x201f60 and 0x201f58). Its answer: -1 outside the
        // view (not drawn); 0 wholly inside it and 1 across its edge but inside the guard band (both drawn by the
        // fast VU1 program, which does not clip); 2 across the guard band (drawn by the clipping program).
        // That is right for exactly the camera the game calculated. A picture whose camera is turned from it (the
        // low-delay mouse view turns every picture to where the mouse is; an in-between picture of a fast turn)
        // lacks what was left out at the edge turned towards, and, far worse, gets unclipped triangles that reach
        // out of the GS's drawing space or behind the eye, where they wrap around: huge sheets across the screen
        // (the user's F1 catch `pictures_5`: "significant flickering when moving the camera around", worse the
        // faster; the game's own pictures show the same in a 20-degree-a-frame flick with smooth motion off).
        // With a margin (star::viewMargin, degrees sideways and up/down) the test here answers for every camera
        // within that margin of the game's: outside only if outside the view widened by the margin, unclipped only
        // if inside the guard band narrowed by it. Margin 0: the game's own function, untouched.
        //
        // Which camera, and how much margin (STAR_VIEW_WHEN=1 prints the order): in a frame the game reads the input,
        // then chooses the scenery with the camera C of the job it sent last, then sends the job, whose view matrix
        // (sent by reference) is by then C turned by what the frame BEFORE did. So the game itself draws one frame's
        // turn away from the camera it chose with: nothing with the pad's slow turns, the sheets above with a mouse.
        // The pictures of that job then go on from there by this frame's turn and by what the mouse does in the
        // next 40 ms or so. Hence the margins are set at every input read (viewMarginsFor), wide towards where the
        // view is turning and narrow the other way, and the pictures' cameras are kept inside them (frame_double.cpp,
        // lateCamera), so a turn too sudden or too fast for the margin costs a little delay, never a broken picture.
        constexpr uint32_t kInView = 0x153FA0u;
        PS2Runtime::RecompiledFunction g_inView = nullptr;
        // yaw -, yaw +, pitch -, pitch + (degrees; the signs are those of the game's angles, camera + 0x24 and + 0x20)
        std::atomic<float> g_viewMargin[4];
        std::atomic<long long> g_viewMarginSetNs{0};
        constexpr double kMarginMost = 45.0; // degrees
        // the margin for what cannot be foreseen (STAR_VIEW_BASE=<degrees>, tests)
        const double kMarginBase = [] { const char *e = std::getenv("STAR_VIEW_BASE"); return e && *e ? std::atof(e) : 10.0; }();

        void setViewMargins(double yawNeg, double yawPos, double pitchNeg, double pitchPos)
        {
            g_viewMargin[0].store(static_cast<float>(yawNeg), std::memory_order_relaxed);
            g_viewMargin[1].store(static_cast<float>(yawPos), std::memory_order_relaxed);
            g_viewMargin[2].store(static_cast<float>(pitchNeg), std::memory_order_relaxed);
            g_viewMargin[3].store(static_cast<float>(pitchPos), std::memory_order_relaxed);
            g_viewMarginSetNs.store(steadyNowNs(), std::memory_order_relaxed);
        }

        bool viewMarginsOn()
        {
            return g_viewMargin[0].load(std::memory_order_relaxed) != 0.0f || g_viewMargin[1].load(std::memory_order_relaxed) != 0.0f;
        }

        // The margins for the scenery chosen after this input read: the job it goes into is drawn with the camera
        // turned by `before` (the frame before's turn), and its pictures go on by `now` (this frame's) and by the
        // mouse's next 40 ms or so, taken as one and a half times `now`. Degrees each way, with a base for what
        // cannot be foreseen (the mouse starting, or turning back).
        void viewMarginsFor(double before, double now, double &neg, double &pos)
        {
            // (a turn that is speeding up is taken to go on speeding up: the next frame's as much faster again)
            const double grows = now * before >= 0.0 && std::fabs(now) > std::fabs(before) ? now - before : 0.0;
            const double a = before * 57.29578, b = (before + 2.5 * now + 1.5 * grows) * 57.29578;
            pos = std::min(kMarginMost, kMarginBase + std::max({0.0, a, b}));
            neg = std::min(kMarginMost, kMarginBase + std::max({0.0, -a, -b}));
        }

        void inViewHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            float margin[4];
            for (int i = 0; i < 4; ++i)
                margin[i] = g_viewMargin[i].load(std::memory_order_relaxed);
            // margins nobody has set for a while (a menu, an event: the input is not read) are not margins any more
            static thread_local uint32_t calls = 0u;
            static thread_local bool stale = true;
            if ((calls++ & 255u) == 0u)
                stale = steadyNowNs() - g_viewMarginSetNs.load(std::memory_order_relaxed) > 150000000ll;
            const uint32_t sphere = getRegU32(ctx, 4) & 0x1FFFFFFFu;
            if (stale || (margin[0] <= 0.0f && margin[1] <= 0.0f && margin[2] <= 0.0f && margin[3] <= 0.0f) || sphere >= 0x01FFFFF0u)
            {
                g_inView(rdram, ctx, runtime); // nothing after the call
                return;
            }
            float v[4], m[16], planes[8], farAway;
            std::memcpy(v, rdram + sphere, 16);
            const bool flipped = guestWord(rdram, 0x201F28u) != 0u; // the other matrix has the view's y turned over
            if (static const bool when = [] { const char *e = std::getenv("STAR_VIEW_WHEN"); return e && *e == '1'; }(); when)
            {
                // STAR_VIEW_WHEN=1 (a test): at a frame's first question, which way the test's camera and the view
                // matrix (0x2f16c0, sent to VU1 by reference) look, in degrees about the upright axis
                static uint64_t told = ~0ull;
                if (told != g_mouseKeys.frame)
                {
                    told = g_mouseKeys.frame;
                    float c[16], u[16];
                    std::memcpy(c, rdram + 0x2F1640u, 64);
                    std::memcpy(u, rdram + 0x2F16C0u, 64);
                    std::printf("[when] test at frame %llu: its camera %.2f, the view matrix %.2f%c", static_cast<unsigned long long>(told),
                                std::atan2(c[3], c[11]) * 57.29578, std::atan2(u[2], u[10]) * 57.29578, 10);
                }
            }
            std::memcpy(m, rdram + (flipped ? 0x2F1600u : 0x2F1640u), 64);
            std::memcpy(planes, rdram + 0x201F58u, 32); // guard y, guard x, view y, view x: (cos, sin) each
            std::memcpy(&farAway, rdram + 0x201C60u, 4);
            // The planes for the margins; worked out again only when the game's values, the margins or the camera's
            // tilt change. The four sides are the view's -x, +x, -y, +y.
            // STAR 2026-11-24: a turn about the world's upright axis is, for a camera that looks up or down, partly
            // a roll: the picture tilts, and what stood at its side edge is then above or below it. Margins added to
            // the planes side by side missed that (two broken pictures in 160 of a test flick sideways and up/down
            // at once). So the view's corner and edge rays are turned exactly as frame_double.cpp's viewTurned turns
            // a picture's camera (about the world's Y as the view sees it, then about the view's X), for the ends
            // and the middle of the margins, and each side takes the widest it meets. The guard band is narrowed to
            // one angle all round: the widest at which the rays of such a square view still lie inside the guard
            // band of every turned camera.
            struct Turned
            {
                float from[8] = {}, margin[4] = {-1.0f, -1.0f, -1.0f, -1.0f}, up[3] = {};
                bool flipped = false;
                float view[4][2] = {}, guard[4][2] = {};
            };
            static thread_local Turned turned;
            const float upNow[3] = {m[4], m[5], m[7]};
            if (std::memcmp(turned.margin, margin, sizeof(margin)) != 0 || std::memcmp(turned.from, planes, 32) != 0 ||
                std::memcmp(turned.up, upNow, sizeof(upNow)) != 0 || turned.flipped != flipped)
            {
                std::memcpy(turned.from, planes, 32);
                std::memcpy(turned.margin, margin, sizeof(margin));
                std::memcpy(turned.up, upNow, sizeof(upNow));
                turned.flipped = flipped;
                const double rad = 0.017453292519943;
                double up[3] = {upNow[0], upNow[1], upNow[2]};
                const double upLength = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
                for (double &c : up)
                    c = upLength > 1e-6 ? c / upLength : 0.0;
                if (upLength <= 1e-6)
                    up[1] = 1.0;
                // (x, y, z) here: the view's x, y and depth. In the mirrored matrix's space the turns go the other way.
                const double way = flipped ? -1.0 : 1.0;
                const auto about = [](const double a[3], double angle, const double in[3], double out[3])
                {
                    const double c = std::cos(angle), sn = std::sin(angle);
                    const double dot = a[0] * in[0] + a[1] * in[1] + a[2] * in[2];
                    const double cross[3] = {a[1] * in[2] - a[2] * in[1], a[2] * in[0] - a[0] * in[2], a[0] * in[1] - a[1] * in[0]};
                    for (int i = 0; i < 3; ++i)
                        out[i] = in[i] * c + cross[i] * sn + a[i] * dot * (1.0 - c);
                };
                const double side[3] = {1.0, 0.0, 0.0};
                // the turns to try: the game's yaw from -margin[0] to +margin[1], its pitch from -margin[2] to +margin[3]
                double yaws[5], pitches[3];
                for (int i = 0; i < 5; ++i)
                    yaws[i] = way * rad * (i < 2 ? -margin[0] * (i == 0 ? 1.0 : 0.5) : i == 2 ? 0.0 : margin[1] * (i == 3 ? 0.5 : 1.0));
                pitches[0] = way * rad * -margin[2];
                pitches[1] = 0.0;
                pitches[2] = way * rad * margin[3];
                static const double kRays[8][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                const double halfX = std::atan2(planes[7], planes[6]), halfY = std::atan2(planes[5], planes[4]);
                double widest[4] = {halfX, halfX, halfY, halfY};
                for (double yaw : yaws)
                    for (double pitch : pitches)
                        for (const double *ray : kRays)
                        {
                            // a ray of the turned camera's view, seen from the test's camera
                            const double in[3] = {ray[0] * std::tan(halfX), ray[1] * std::tan(halfY), 1.0};
                            double mid[3], out[3];
                            about(side, pitch, in, mid);
                            about(up, yaw, mid, out);
                            const double depth = std::max(out[2], 1e-3);
                            const double ax = std::atan2(out[0], depth), ay = std::atan2(out[1], depth);
                            widest[0] = std::max(widest[0], -ax);
                            widest[1] = std::max(widest[1], ax);
                            widest[2] = std::max(widest[2], -ay);
                            widest[3] = std::max(widest[3], ay);
                        }
                for (int i = 0; i < 4; ++i)
                {
                    const double h = std::min(widest[i] + 1.0 * rad, 1.55);
                    turned.view[i][0] = static_cast<float>(std::cos(h));
                    turned.view[i][1] = static_cast<float>(std::sin(h));
                }
                const double guardX = std::atan2(planes[3], planes[2]), guardY = std::atan2(planes[1], planes[0]);
                double narrow = std::min(guardX, guardY) - rad * std::max({margin[0], margin[1], margin[2], margin[3]});
                for (int tries = 0; tries < 24 && narrow > 0.06; ++tries)
                {
                    bool fine = true;
                    for (int y = 0; y < 5 && fine; ++y)
                        for (int q = 0; q < 3 && fine; ++q)
                            for (const double *ray : kRays)
                            {
                                // a ray of the narrowed square, seen from the turned camera
                                const double in[3] = {ray[0] * std::tan(narrow), ray[1] * std::tan(narrow), 1.0};
                                double mid[3], out[3];
                                about(up, -yaws[y], in, mid);
                                about(side, -pitches[q], mid, out);
                                if (out[2] <= 1e-3 || std::fabs(std::atan2(out[0], out[2])) > guardX - 0.5 * rad ||
                                    std::fabs(std::atan2(out[1], out[2])) > guardY - 0.5 * rad)
                                {
                                    fine = false;
                                    break;
                                }
                            }
                    if (fine)
                        break;
                    narrow -= 3.0 * rad;
                }
                narrow = std::max(narrow, 0.05);
                for (int i = 0; i < 4; ++i)
                {
                    turned.guard[i][0] = static_cast<float>(std::cos(narrow));
                    turned.guard[i][1] = static_cast<float>(std::sin(narrow));
                }
            }
            const float r = v[3];
            const float x = v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12];
            const float y = v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13];
            const float w = v[0] * m[3] + v[1] * m[7] + v[2] * m[11] + m[15];
            int32_t answer = 2;
            const auto inside = [&](const float (*side)[2], float by)
            {
                return x * side[0][0] + w * side[0][1] + by >= 0.0f && -x * side[1][0] + w * side[1][1] + by >= 0.0f &&
                       y * side[2][0] + w * side[2][1] + by >= 0.0f && -y * side[3][0] + w * side[3][1] + by >= 0.0f;
            };
            const auto insideOwn = [&](float by)
            {
                return x * planes[6] + w * planes[7] + by >= 0.0f && -x * planes[6] + w * planes[7] + by >= 0.0f &&
                       y * planes[4] + w * planes[5] + by >= 0.0f && -y * planes[4] + w * planes[5] + by >= 0.0f;
            };
            if (!inside(turned.view, r) || w + r < 0.0f || w - r > farAway)
                answer = -1;
            else if (inside(turned.guard, -r) && r >= 0.0f)
                answer = insideOwn(-r) ? 0 : 1;
            if (static const bool log = [] { const char *e = std::getenv("STAR_VIEW_LOG"); return e && *e == '1'; }(); log)
            {
                // STAR_VIEW_LOG=1 (a test): what the answers come to, every 20,000 questions
                static std::atomic<uint32_t> counts[4], asked{0};
                counts[answer + 1].fetch_add(1u, std::memory_order_relaxed);
                if (asked.fetch_add(1u, std::memory_order_relaxed) % 20000u == 19999u)
                    std::printf("[view] margins %.0f %.0f %.0f %.0f: of 20000 tests %u outside, %u inside, %u inside the guard band, %u clipped%c",
                                margin[0], margin[1], margin[2], margin[3], counts[0].exchange(0u), counts[1].exchange(0u), counts[2].exchange(0u), counts[3].exchange(0u), 10);
            }
            SET_GPR_S32(ctx, 2, answer);
            ctx->pc = getRegU32(ctx, 31);
        }

        // ---- The player's own model pieces, told to the in-between pictures -------------------------------------------
        // STAR 2026-11-25. The pictures of a frame are turned on to where the mouse is (frame_double.cpp, lateCamera),
        // and the player's own body, arms and weapon, which the game places in the world for ITS camera, must turn
        // with them or they slide across the picture. Which objects those are was first found out from how things
        // moved, then (for pieces never seen, in an animation) from how near the eye they were: the user's shrine orb
        // was near and moving too, and was dragged about with the view. Now the game says it:
        //   every model piece is drawn by FUN_0016dc70 or FUN_0016ded0 (a0 = the piece, a1 = the model's draw
        //   object, a2 = its instance; the piece's matrix, the one the VIF stream uploads to VU1 address 24, is at
        //   instance + *(instance + 0xb0) + 0xc0 + 0x140 x a3, or + 0x200 + 0x140 x piece[5]). A draw object is
        //   the player's if it is
        //     - the body's: *(player's inner + 0x1a8);
        //     - a first-person model's: bit 0 of the byte at draw object + 0x25, the game's own "never test this
        //       against the view" (FUN_0016cd80), which the arms have;
        //     - an equipped weapon's: **(weapon + 0x84), for the weapons at inner + 0x540 and + 0x544 (the chain
        //       FUN_0012da90 follows to the weapon's data).
        // The matrices' addresses are kept as the pieces are drawn, and when the frame's job is made their 64 bytes
        // each are read (what the stream holds too) and given to the job as its note. The worker marks the objects
        // with those very matrices, and no others.
        constexpr uint32_t kDrawPieceA = 0x16DC70u, kDrawPieceB = 0x16DED0u;
        PS2Runtime::RecompiledFunction g_drawPieceA = nullptr, g_drawPieceB = nullptr;
        std::vector<uint32_t> g_ownMatrices; // the game thread's: where the player's pieces' matrices of this frame are

        bool ownDrawObject(uint8_t *rdram, uint32_t object)
        {
            if (!inRam(object))
                return false;
            if ((rdram[object + 0x25u] & 1u) != 0u)
                return true;
            const uint32_t player = guestWord(rdram, 0x2022E4u);
            const uint32_t inner = inRam(player) ? guestWord(rdram, player + 0x518u) & 0x1FFFFFFFu : 0u;
            if (!inRam(inner))
                return false;
            if ((guestWord(rdram, inner + 0x1A8u) & 0x1FFFFFFFu) == object)
                return true;
            for (const uint32_t slot : {0x540u, 0x544u})
            {
                uint32_t at = guestWord(rdram, inner + slot) & 0x1FFFFFFFu;
                if (!inRam(at))
                    continue;
                at = guestWord(rdram, at + 0x84u) & 0x1FFFFFFFu;
                if (inRam(at))
                    at = guestWord(rdram, at) & 0x1FFFFFFFu;
                if (at == object)
                    return true;
            }
            return false;
        }

        // STAR 2026-11-25 (the user: "when shooting while moving the camera, a gun's muzzle flash becomes disconnected from
        // the barrel"). The flash is not a piece of the gun: it is a particle, a model of its own, which the game makes
        // at one of the gun's bones and then leaves where it is. So the in-between pictures turned the gun with the
        // view and left the flash behind with the world. What the game knows (`ghidra\decomp_fx*.c`):
        //   - FUN_001d1f20(a0 = a model's draw object, .., a3 = bone, ..) makes an effect at that bone (the animation
        //     events of weapons call it through FUN_001ad2c0; three other callers). Inside, at 0x1d20d0, it calls
        //     FUN_001677c0(emitter, matrix) to put the effect's emitter there.
        //   - Emitters: 0x100 of 0xed0 bytes at 0x3407e0; in use = bit 0 of the byte at + 0x188; what kind = the
        //     word at + 0x210.
        //   - Particles: word(0x201fd8) of them, 0x120 bytes each at *(0x201fc0); in use = bit 0 of the byte at
        //     + 0x114; the emitter at + 0x90. Each has two models, 0x30 bytes each at *(0x201fd0) + 0x60 x number;
        //     the word at + 0xc holds the model's draw object (FUN_00174230 makes it).
        //     A model's record is 0 0 0 holder ..; a sprite's (a flat picture that the VU program at 0x4ee turns to the
        //     view) is place x y z, .., sizes, colour, bits. The two records are used in turn, a frame each. The muzzle flash is both: a small
        //     model and a big sprite.
        // So: in the frame in which the game puts an emitter at a bone of the player's first-person arms or weapon,
        // that emitter's particles are where that bone is in that frame: they are listed once in the frame, their
        // models' pieces count as the player's own and their sprites' places are told too (the job's note). Only in
        // that frame: afterwards the game itself leaves the effect where it was made (smoke hangs in the air), and
        // so do the pictures. (Not the body's bones: what is made at its feet belongs to the ground.)
        constexpr uint32_t kEffectOnBone = 0x1D1F20u, kPlaceEmitter = 0x1677C0u, kPlaceEmitterFromBone = 0x1D20D8u;
        PS2Runtime::RecompiledFunction g_effectOnBone = nullptr, g_placeEmitter = nullptr;
        struct OwnEmitter
        {
            uint32_t at, kind;
            uint64_t frame; // g_ownFrame when it was put there
        };
        uint64_t g_ownFrame = 0; // counts the frames' jobs (the note provider)
        std::vector<OwnEmitter> g_ownEmitters;       // the game thread's
        std::vector<uint32_t> g_ownEffectObjects;    // this frame's draw objects of their particles
        std::vector<std::array<float, 3>> g_ownPoints; // and the places of their sprites (flat pictures facing the view)
        bool g_ownEffectsListed = false, g_effectOnOwn = false;

        bool ownViewModel(uint8_t *rdram, uint32_t object)
        {
            if (!inRam(object))
                return false;
            if ((rdram[object + 0x25u] & 1u) != 0u)
                return true;
            const uint32_t player = guestWord(rdram, 0x2022E4u);
            const uint32_t inner = inRam(player) ? guestWord(rdram, player + 0x518u) & 0x1FFFFFFFu : 0u;
            return inRam(inner) && (guestWord(rdram, inner + 0x1A8u) & 0x1FFFFFFFu) != object && ownDrawObject(rdram, object);
        }

        void effectOnBoneHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_effectOnOwn = ownViewModel(rdram, getRegU32(ctx, 4) & 0x1FFFFFFFu);
            g_effectOnBone(rdram, ctx, runtime); // nothing after the call
        }

        void placeEmitterHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 31) == kPlaceEmitterFromBone)
            {
                const uint32_t emitter = getRegU32(ctx, 4) & 0x1FFFFFFFu;
                g_ownEmitters.erase(std::remove_if(g_ownEmitters.begin(), g_ownEmitters.end(), [emitter](const OwnEmitter &e) { return e.at == emitter; }),
                                    g_ownEmitters.end());
                if (g_effectOnOwn && inRam(emitter) && g_ownEmitters.size() < 64u)
                    g_ownEmitters.push_back({emitter, guestWord(rdram, emitter + 0x210u), g_ownFrame});
                static const bool log = std::getenv("STAR_PIECE_LOG") != nullptr;
                if (log)
                    std::printf("[piece] an effect at a bone of %s model: emitter %08x%c", g_effectOnOwn ? "the player's own" : "another", emitter, 10);
                g_effectOnOwn = false;
            }
            g_placeEmitter(rdram, ctx, runtime); // nothing after the call
        }

        // Once a frame, at the first piece drawn: the draw objects of the particles of the player's emitters.
        void listOwnEffects(uint8_t *rdram)
        {
            g_ownEffectsListed = true;
            g_ownEffectObjects.clear();
            g_ownPoints.clear();
            // STAR_OWN_EFFECTS=0 (tests): as before, effects are things of the world
            static const bool off = [] { const char *v = std::getenv("STAR_OWN_EFFECTS"); return v && *v == '0'; }();
            if (off)
                return;
            g_ownEmitters.erase(std::remove_if(g_ownEmitters.begin(), g_ownEmitters.end(), [rdram](const OwnEmitter &e)
                                               { return e.frame != g_ownFrame || (rdram[e.at + 0x188u] & 1u) == 0u || guestWord(rdram, e.at + 0x210u) != e.kind; }),
                                g_ownEmitters.end());
            if (g_ownEmitters.empty())
                return;
            const uint32_t parts = guestWord(rdram, 0x201FC0u) & 0x1FFFFFFFu, models = guestWord(rdram, 0x201FD0u) & 0x1FFFFFFFu;
            const uint32_t count = std::min<uint32_t>(guestWord(rdram, 0x201FD8u), 4096u);
            if (!inRam(parts) || !inRam(models) || !inRam(parts + count * 0x120u) || !inRam(models + count * 0x60u))
                return;
            for (uint32_t slot = 0; slot < count; ++slot)
            {
                const uint32_t part = parts + slot * 0x120u;
                if ((rdram[part + 0x114u] & 1u) == 0u)
                    continue;
                const uint32_t emitter = guestWord(rdram, part + 0x90u) & 0x1FFFFFFFu;
                bool own = false;
                for (const OwnEmitter &e : g_ownEmitters)
                    own = own || e.at == emitter;
                if (!own)
                    continue;
                for (uint32_t k = 0; k < 2u; ++k)
                {
                    // (a model's record; a sprite's has no holder: those are read when the frame is done, listOwnSprites)
                    const uint32_t face = models + slot * 0x60u + k * 0x30u;
                    const uint32_t holder = guestWord(rdram, face + 0xCu) & 0x1FFFFFFFu;
                    const uint32_t object = inRam(holder) ? guestWord(rdram, holder) & 0x1FFFFFFFu : 0u;
                    if (inRam(object) && g_ownEffectObjects.size() < 256u)
                        g_ownEffectObjects.push_back(object);
                }
            }
        }

        // When the frame's job is made: the places of the sprites of the player's emitters of this frame. Not earlier:
        // the game fills a sprite's record (place, sizes, colour, and bit 26 of the last word) while it draws the
        // particles, and in some maps models are drawn before that (the hub: at the first piece the records were
        // still empty, and the flash's glow was left to the world there; the user's catches pictures_17 and _18).
        // A particle has two records, used in turn; both are told, the stream holds one of them.
        void listOwnSprites(uint8_t *rdram)
        {
            g_ownPoints.clear();
            if (!g_ownEffectsListed || g_ownEmitters.empty())
                return;
            const uint32_t parts = guestWord(rdram, 0x201FC0u) & 0x1FFFFFFFu, models = guestWord(rdram, 0x201FD0u) & 0x1FFFFFFFu;
            const uint32_t count = std::min<uint32_t>(guestWord(rdram, 0x201FD8u), 4096u);
            if (!inRam(parts) || !inRam(models) || !inRam(parts + count * 0x120u) || !inRam(models + count * 0x60u))
                return;
            static const bool log = std::getenv("STAR_PIECE_LOG") != nullptr;
            for (uint32_t slot = 0; slot < count; ++slot)
            {
                const uint32_t part = parts + slot * 0x120u;
                if ((rdram[part + 0x114u] & 1u) == 0u)
                    continue;
                const uint32_t emitter = guestWord(rdram, part + 0x90u) & 0x1FFFFFFFu;
                bool own = false;
                for (const OwnEmitter &e : g_ownEmitters)
                    own = own || e.at == emitter;
                if (!own)
                    continue;
                for (uint32_t k = 0; k < 2u; ++k)
                {
                    // not a model's record (those have their holder in word 3), and not an empty one. (The last word's
                    // bits are no help: they differ from map to map, 0x44084958 in one and 0x00000958 in the hub. A
                    // place that is not a sprite's does no harm: the worker takes only what the stream sends as one.)
                    const uint32_t face = models + slot * 0x60u + k * 0x30u;
                    const uint32_t holder = guestWord(rdram, face + 0xCu) & 0x1FFFFFFFu;
                    if (inRam(holder) && inRam(guestWord(rdram, holder) & 0x1FFFFFFFu))
                        continue;
                    std::array<float, 3> at;
                    std::memcpy(at.data(), rdram + face, 12);
                    if ((guestWord(rdram, face) | guestWord(rdram, face + 4u) | guestWord(rdram, face + 8u)) << 1 == 0u)
                        continue; // (0 or -0 in all three)
                    if (log)
                        std::printf("[piece] frame %llu: a sprite of the player's particle %u, record %u, at %.1f %.1f %.1f%c",
                                    static_cast<unsigned long long>(g_ownFrame), slot, k, at[0], at[1], at[2], 10);
                    if (g_ownPoints.size() < 64u && std::find(g_ownPoints.begin(), g_ownPoints.end(), at) == g_ownPoints.end())
                        g_ownPoints.push_back(at);
                }
            }
        }

        void noteOwnPiece(uint8_t *rdram, uint32_t owner, uint32_t matrix, uint32_t caller = 0u)
        {
            matrix &= 0x1FFFFFFFu;
            owner &= 0x1FFFFFFFu;
            if (!g_ownEffectsListed)
                listOwnEffects(rdram);
            const bool own = inRam(matrix) && (ownDrawObject(rdram, owner) ||
                                               std::find(g_ownEffectObjects.begin(), g_ownEffectObjects.end(), owner) != g_ownEffectObjects.end());
            if (own && g_ownMatrices.size() < 512u)
                g_ownMatrices.push_back(matrix);
            // STAR_PIECE_LOG=<units> (tests): every model piece drawn within that far of the player's eye: whose it is
            // (the draw object, and what its first word names), where, and who asked for it
            static const float logWithin = []
            { const char *v = std::getenv("STAR_PIECE_LOG"); return v ? static_cast<float>(std::atof(v)) : 0.0f; }();
            if (logWithin > 0.0f && inRam(matrix))
            {
                const uint32_t player = guestWord(rdram, 0x2022E4u);
                const uint32_t inner = inRam(player) ? guestWord(rdram, player + 0x518u) & 0x1FFFFFFFu : 0u;
                float at[3], eye[3] = {0.0f, 0.0f, 0.0f};
                std::memcpy(at, rdram + matrix + 0x30u, 12);
                if (inRam(inner))
                    std::memcpy(eye, rdram + inner, 12);
                const float dx = at[0] - eye[0], dy = at[1] - eye[1], dz = at[2] - eye[2];
                if (dx * dx + dy * dy + dz * dz < logWithin * logWithin)
                    std::printf("[piece] %s object %08x of %08x matrix %08x at %.1f %.1f %.1f from %08x%c", own ? "own  " : "other", owner & 0x1FFFFFFFu,
                                inRam(owner & 0x1FFFFFFFu) ? guestWord(rdram, owner & 0x1FFFFFFFu) : 0u, matrix, at[0], at[1], at[2], caller, 10);
            }
        }

        // STAR 2026-11-25, tests only (STAR_SHADOW_TEST_LOG=1): the creature shadow volumes' way through the game, for
        // the user's report "Rurufon's shadow on the wall ... disappears when you get close to it" (NOTES.md). The shadow
        // loop FUN_0016c350 first asks FUN_0016bd00(matrix, sphere, light in view space + length, the view's side
        // planes, near/far at [2],[3] -> the visible part of the length at [0],[1]; -1 = none) whether the model's
        // bounding sphere swept along the light can be in view, then hands each piece to FUN_00132a70 (VU1 program
        // 0x678 makes the volume). Printed: each test (sphere radius, centre in view space, light, length, answer) and,
        // before a large model's test, how many pieces were handed over since the one before.
        // Found: the same 28 pieces are handed over every frame near and far, but VU1 lets ~17,000 triangles through
        // far and 42 near (the renderer's PS2X_PCSX2_VOLUME_LOG): the microprogram throws away every triangle with a
        // corner it cannot put on the screen (behind the camera or outside the GS's coordinate range), and from close
        // up every strip from her body to the wall has such a corner. Also seen: the test sweeps TOWARD the light
        // (c - L*d) while the volumes go away from it, so with the light behind the camera it shortens the volumes
        // where that wrong sweep passes behind the camera (82% far, 50% near); not the cause (with the sweep turned
        // round the length was 100% and still 42 triangles came through).
        constexpr uint32_t kShadowInView = 0x16BD00u, kShadowVolume = 0x132A70u;
        PS2Runtime::RecompiledFunction g_shadowInView = nullptr, g_shadowVolume = nullptr;
        uint64_t g_volumeCalls = 0;

        void shadowVolumeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_volumeCalls;
            g_shadowVolume(rdram, ctx, runtime); // nothing after the call
        }

        // STAR 2026-11-25 (setting near_shadows, with star/shadow_volumes.cpp): the visibility test swept the right
        // way. FUN_0016bd00 sweeps the model's sphere from its centre c to c - L*d (d = the light direction in view
        // space, param_3[0..2]), but the volumes go to c + L*d (VU1 program 0x678 adds the light vector to each
        // corner). With the light behind the camera the wrong sweep passes behind it and the test cuts every volume
        // short there: the shorter the nearer the creature (82% of the length far, 50% near, in the user's room), so a
        // close creature's shadow no longer reached the wall. The test is given -d (put back after it): it then cuts
        // the volumes where they really leave the view. Pure arithmetic, so it is called to its end.
        bool g_shadowTestLog = false; // STAR_SHADOW_TEST_LOG=1

        void shadowInViewHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t matrix = getRegU32(ctx, 4) & 0x1FFFFFFFu, sphere = getRegU32(ctx, 5) & 0x1FFFFFFFu;
            const uint32_t light = getRegU32(ctx, 6) & 0x1FFFFFFFu, planes = getRegU32(ctx, 7) & 0x1FFFFFFFu;
            const uint32_t range = getRegU32(ctx, 8) & 0x1FFFFFFFu;
            const bool flip = live().nearShadows.load(std::memory_order_relaxed);
            if ((!flip && !g_shadowTestLog) || !inRam(matrix) || !inRam(sphere) || !inRam(light) || !inRam(planes) || !inRam(range))
            {
                g_shadowInView(rdram, ctx, runtime); // nothing after the call
                return;
            }
            auto f = [&](uint32_t a) { float v = 0.0f; std::memcpy(&v, rdram + a, 4); return v; };
            const float radius = f(sphere + 0xC);
            if (g_shadowTestLog && radius > 400.0f)
            {
                std::printf("[shadow test] volume pieces handed over since the last: %llu%c", static_cast<unsigned long long>(g_volumeCalls), 10);
                g_volumeCalls = 0;
            }
            float saved[3] = {};
            std::memcpy(saved, rdram + light, sizeof(saved));
            if (flip)
                for (int i = 0; i < 3; ++i)
                {
                    const float turned = -saved[i];
                    std::memcpy(rdram + light + static_cast<uint32_t>(i) * 4u, &turned, 4);
                }
            runtime->eeScheduler().beginHostCall();
            g_shadowInView(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            std::memcpy(rdram + light, saved, sizeof(saved));
            if (g_shadowTestLog)
            {
                // the sphere's centre in view space, as the function makes it (row vectors: v * M, then the view 0x2f15c0)
                float c[4] = {f(sphere), f(sphere + 4), f(sphere + 8), 1.0f}, w[4] = {}, v[4] = {};
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i)
                        w[j] += c[i] * f(matrix + static_cast<uint32_t>(i * 16 + j * 4));
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i)
                        v[j] += w[i] * f(0x2F15C0u + static_cast<uint32_t>(i * 16 + j * 4));
                std::printf("[shadow test] r %.0f centre %.0f %.0f %.0f light %.2f %.2f %.2f length %.0f near %.0f far %.0f -> %d range %.2f %.2f%s%c", radius,
                            v[0], v[1], v[2], f(light), f(light + 4), f(light + 8), f(light + 12), f(range + 8), f(range + 12),
                            static_cast<int32_t>(getRegU32(ctx, 2)), f(range), f(range + 4), flip ? " (swept the right way)" : "", 10);
            }
        }

        // STAR 2026-11-25: owner labels, for "creatures do not shadow themselves" (live().noSelfShadow; the renderer's
        // GSRendererHW::ApplyOwnerShadow says what they are for). In front of each model piece the game sends - the
        // piece functions FUN_0016dc70 / FUN_0016ded0, which a creature's shadow pieces go through too (its shadow
        // loop FUN_0016c350 calls FUN_0016ded0 for each piece's matrix, then FUN_00132a70 for the volume) - a VIF MARK
        // with the number of the piece's instance (a2) this frame, 1 to 126. It is put into the game's own packet the
        // game's way: FUN_00154a80(1) gives the next quad word of the packet being built (0x2f1990.. 0x2f1994; the piece
        // function's own data follows it), a DMA tag CNT with nothing after it whose upper half carries the VIF codes
        // MARK and NOP, as the game's own tags carry MSCALF and FLUSHE. A MARK is nothing to the PS2.
        constexpr uint32_t kPacketQuadword = 0x154A80u;
        PS2Runtime::RecompiledFunction g_packetQuadword = nullptr;
        std::vector<uint32_t> g_ownerInstances; // this frame's: an instance's number is its place here + 1

        void labelPiece(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if ((!live().noSelfShadow.load(std::memory_order_relaxed) && !live().nearShadows.load(std::memory_order_relaxed) &&
                 !live().shadowWalls.load(std::memory_order_relaxed)) ||
                !g_packetQuadword)
                return;
            const uint32_t instance = getRegU32(ctx, 6) & 0x1FFFFFFFu;
            if (!inRam(instance))
                return;
            uint32_t id = 0;
            for (size_t i = 0; i < g_ownerInstances.size() && id == 0; ++i)
                if (g_ownerInstances[i] == instance)
                    id = static_cast<uint32_t>(i + 1);
            if (id == 0)
            {
                if (g_ownerInstances.size() >= 126u)
                    return; // (more than 126 in a frame: the rest go unlabelled and shadow themselves as the game does; the
                            // renderer keeps 7 bits for the number)
                g_ownerInstances.push_back(instance);
                id = static_cast<uint32_t>(g_ownerInstances.size());
            }
            R5900Context scratch = *ctx;
            SET_GPR_U32(&scratch, 4, 1u);
            SET_GPR_U32(&scratch, 29, getRegU32(ctx, 29) - 0x100u);
            SET_GPR_U32(&scratch, 31, 0u);
            scratch.pc = kPacketQuadword;
            g_packetQuadword(rdram, &scratch, runtime);
            const uint32_t at = getRegU32(&scratch, 2) & 0x1FFFFFFFu;
            if (!inRam(at) || (at & 15u) != 0u)
                return;
            // bit 8: one of the player's own pieces (body, first-person arms, weapon: ownDrawObject on the draw object, a1);
            // star/shadow_volumes.cpp leaves their shadows as the game has them
            if (ownDrawObject(rdram, getRegU32(ctx, 5) & 0x1FFFFFFFu))
                id |= 0x100u;
            const uint32_t words[4] = {0x10000000u, 0u, 0x07000000u | id, 0u}; // DMA CNT 0; VIF MARK id, NOP
            std::memcpy(rdram + at, words, sizeof(words));
        }

        void drawPieceAHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            labelPiece(rdram, ctx, runtime);
            const uint32_t instance = getRegU32(ctx, 6) & 0x1FFFFFFFu;
            if (inRam(instance))
                noteOwnPiece(rdram, getRegU32(ctx, 5), instance + getRegU32(ctx, 7) * 0x140u + 0xC0u + guestWord(rdram, instance + 0xB0u), getRegU32(ctx, 31));
            g_drawPieceA(rdram, ctx, runtime); // nothing after the call
        }

        void drawPieceBHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            labelPiece(rdram, ctx, runtime);
            const uint32_t instance = getRegU32(ctx, 6) & 0x1FFFFFFFu, piece = getRegU32(ctx, 4) & 0x1FFFFFFFu;
            if (inRam(instance) && inRam(piece))
                noteOwnPiece(rdram, getRegU32(ctx, 5), instance + rdram[piece + 5u] * 0x140u + 0x200u + guestWord(rdram, instance + 0xB0u), getRegU32(ctx, 31));
            g_drawPieceB(rdram, ctx, runtime); // nothing after the call
        }

        bool replace(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction hook,
                     PS2Runtime::RecompiledFunction &original)
        {
            if (!runtime.hasFunction(address))
                return false;
            original = runtime.lookupFunction(address);
            if (runtime.replaceFunction(address, hook))
                return true;
            original = nullptr;
            return false;
        }
    }

    // live_look.h
    namespace
    {
        std::mutex g_lookMutex;
        std::array<LookFrame, 64> g_lookFrames{};
    }

    void publishLookFrame(const LookFrame &frame)
    {
        {
            std::lock_guard<std::mutex> lock(g_lookMutex);
            g_lookFrames[frame.seq % g_lookFrames.size()] = frame;
        }
        // The game sends a frame's drawing to VIF1 at the start of the NEXT frame, after that frame's input has been
        // read (found with STAR_LIVE_LOG=1: the job met after frame N's input held frame N-1's camera). So the next
        // job is the frame before's.
        ps2x::asyncvif::setJobTag(frame.seq > 1u ? frame.seq - 1u : 0u);
        if (static const bool when = [] { const char *e = std::getenv("STAR_VIEW_WHEN"); return e && *e == '1'; }(); when)
            std::printf("[when] input read: look frame %llu, turned %.2f%c", static_cast<unsigned long long>(frame.seq), frame.yaw * 57.29578, 10);
    }

    void markLastLookFrame()
    {
        std::lock_guard<std::mutex> lock(g_lookMutex);
        const uint64_t seq = g_mouseKeys.seq;
        LookFrame &frame = g_lookFrames[seq % g_lookFrames.size()];
        if (seq != 0u && frame.seq == seq && !frame.last)
        {
            frame.last = true;
            if (static const bool log = [] { const char *e = std::getenv("STAR_LIVE_LOG"); return e && *e == '1'; }(); log)
                std::printf("[late] the controls are taken away after look frame %llu%c", static_cast<unsigned long long>(seq), 10);
        }
    }

    bool takeLookFrame(uint64_t seq, LookFrame &frame)
    {
        std::lock_guard<std::mutex> lock(g_lookMutex);
        frame = g_lookFrames[seq % g_lookFrames.size()];
        return seq != 0u && frame.seq == seq;
    }

    bool lookMouseAt(long long steadyNs, double &x, double &y)
    {
        if (testMouseOn())
        {
            {
                std::lock_guard<std::mutex> lock(g_testMouseMutex);
                x = g_testMouseX;
                y = g_testMouseY;
            }
            if (const long long start = g_steadyMouse.start.load(std::memory_order_relaxed); g_steadyMouse.on && start != 0)
            {
                double seconds = std::clamp(static_cast<double>(steadyNs - start) / 1e9, g_steadyMouse.from, g_steadyMouse.to) - g_steadyMouse.from;
                if (g_steadyMouse.swing > 0.0)
                    seconds = g_steadyMouse.swing / 6.2831853 * (1.0 - std::cos(6.2831853 * seconds / g_steadyMouse.swing));
                x += g_steadyMouse.x * seconds;
                y += g_steadyMouse.y * seconds;
            }
            return true;
        }
        return ps2_host_input::mouseTotalAt(steadyNs, x, y);
    }

    long long lookMouseMovedNs()
    {
        return testMouseOn() ? steadyNowNs() : ps2_host_input::mouseLastMovedNs();
    }

    void applyMouseKeyboard()
    {
        // the mouse is captured by a click on the game and read by readInputHook; off: the mouse is left alone
        ps2_host_input::setMouseCameraEnabled(live().mouseKeyboard.load(std::memory_order_relaxed));
        // the controls (bindings.h) for the swing style chosen: the swings in use win over the general actions' inputs
        // (the runtime's key bindings, the game keys, Tab)
        const int mode = live().meleeMode.load(std::memory_order_relaxed);
        bindings::setLive(bindings::live(), mode == 2 ? "buttons" : mode == 1 ? "flick" : "direction", true);
    }

    void installGameFixes(PS2Runtime &runtime)
    {
        const bool sound = replace(runtime, kServerThread, &serverThreadHook, g_serverThread) &&
                           replace(runtime, kSoundCommand, &soundCommandHook, g_soundCommand);
        std::printf("[STAR fix] sound RPC server thread: %s\n", sound ? "installed" : "NOT installed");
        if (const char *keep = std::getenv("STAR_SCANMASK"); !(keep && *keep == '1'))
        {
            const bool scanmask = replace(runtime, kSetGsRegister, &setGsRegisterHook, g_setGsRegister);
            std::printf("[STAR fix] scanmask off (PCSX2's Remove Scanmask Blur): %s\n", scanmask ? "installed" : "NOT installed");
        }
        if (const char *keep = std::getenv("STAR_TURN_SHIFT"); !(keep && *keep == '1'))
        {
            const bool shift = replace(runtime, kTurnShiftCopy, &turnShiftCopyHook, g_turnShiftCopy);
            std::printf("[STAR fix] no picture shift while the camera turns: %s\n", shift ? "installed" : "NOT installed");
        }
        // Both hooks are always there; star::live() says whether the pass runs (flicker filter: off unless
        // STAR_FLICKER_FILTER=1, the user's choice of 2026-11-23; glow: on unless STAR_GLOW=0).
        {
            const bool filter = replace(runtime, kFlickerFilter, &flickerFilterHook, g_flickerFilter);
            const bool glow = replace(runtime, kGlow, &glowHook, g_glow);
            std::printf("[STAR option] flicker filter %s, glow %s (switches: %s)\n", live().flickerFilter.load() ? "on" : "off",
                        live().glow.load() ? "on" : "off", filter && glow ? "installed" : "NOT installed");
        }
        {
            const bool aim = replace(runtime, kWalkOnGround, &walkOnGroundHook, g_walkOnGround) &&
                             replace(runtime, kLookUpDown, &lookUpDownHook, g_lookUpDown);
            std::printf("[STAR option] analog aiming %s (switch: %s)\n", live().analogAim.load() ? "on" : "off", aim ? "installed" : "NOT installed");
        }
        {
            // Player 1's bindings are main.cpp's everywhere (no menu layer); the mouse never moves the right stick,
            // Esc is a game button (Alt+Tab and the F2 menu free the mouse), and in the game's own menus the left
            // click is circle (OK), the right click cross (quit) and the wheel the d-pad up and down.
            ps2_host_input::setMenuLayer(false);
            ps2_host_input::setMouseStickEnabled(false);
            ps2_host_input::setMouseEscapeReleases(false);
            ps2_host_input::setMouseMenuButtons(static_cast<uint16_t>(kPadCircle), static_cast<uint16_t>(kPadCross),
                                                static_cast<uint16_t>(kPadUp), static_cast<uint16_t>(kPadDown));
            applyMouseKeyboard();
            installShadowVolumes(); // star/shadow_volumes.cpp
            if (const char *v = std::getenv("STAR_SHADOW_TEST_LOG"); v && *v == '1')
            {
                g_shadowTestLog = true;
                replace(runtime, kShadowVolume, &shadowVolumeHook, g_shadowVolume);
            }
            const bool sweep = replace(runtime, kShadowInView, &shadowInViewHook, g_shadowInView);
            std::printf("[STAR option] shadow visibility test swept the right way with shadows up close (switch: %s)%c", sweep ? "installed" : "NOT installed", 10);
            const bool late = replace(runtime, kPadUpdate, &padUpdateHook, g_padUpdate) &&
                              replace(runtime, kGameLogic, &gameLogicHook, g_gameLogic) &&
                              replace(runtime, kFrameSwap, &frameSwapHook, g_frameSwap);
            if (const char *v = std::getenv("STAR_LATE_TEST"); v && *v == '1')
                ps2_host_input::setPadFilter(&lateTestFilter);
            std::printf("[STAR option] late input %s (switch: %s)%c", live().lateInput.load() ? "on" : "off", late ? "installed" : "NOT installed", 10);
            const bool keys = replace(runtime, kReadInput, &readInputHook, g_readInput) &&
                              replace(runtime, kControlType, &controlTypeHook, g_controlType);
            std::printf("[STAR option] mouse and keyboard play %s (switch: %s)%c", live().mouseKeyboard.load() ? "on" : "off",
                        keys ? "installed" : "NOT installed", 10);
        }
        {
            const bool pieces = replace(runtime, kDrawPieceA, &drawPieceAHook, g_drawPieceA) &&
                                replace(runtime, kDrawPieceB, &drawPieceBHook, g_drawPieceB);
            if (pieces)
            {
                uint8_t *ram = runtime.memory().getRDRAM();
                ps2x::asyncvif::setJobNoteProvider([ram](std::vector<uint8_t> &note)
                {
                    // (the game thread, as the frame's job is made: the matrices as the job's stream holds them)
                    // And: will the game read the input next frame? Not while 0x201ea0 is set (FUN_0012c020: an event),
                    // nor with bit 2 of 0x222a52 unless 0x222a11 (FUN_001befa0's conditions for FUN_001c4760), nor
                    // while 0x201eb8 is set (the game's own menu: it becomes 1 in the frame cross is pressed, seen
                    // with STAR_STOP_LOG).
                    if (guestWord(ram, 0x201EA0u) != 0u || guestWord(ram, 0x201EB8u) != 0u || ((ram[0x222A52u] & 4u) != 0u && ram[0x222A11u] == 0u))
                        markLastLookFrame();
                    if (static const bool watch = std::getenv("STAR_STOP_LOG") != nullptr; watch)
                    {
                        // STAR_STOP_LOG (a test): the words around 0x201ea0 and 0x222a10.. as they change, per job
                        static uint8_t before[2][0x80];
                        static uint64_t jobs = 0;
                        ++jobs;
                        const uint32_t bases[2] = {0x201E60u, 0x222A00u};
                        for (int k = 0; k < 2; ++k)
                            for (uint32_t o = 0u; o < 0x80u; o += 4u)
                                if (std::memcmp(before[k] + o, ram + bases[k] + o, 4) != 0)
                                {
                                    std::printf("[stop] job %llu (look frame %llu): %08x = %08x%c", static_cast<unsigned long long>(jobs),
                                                static_cast<unsigned long long>(g_mouseKeys.seq), bases[k] + o, guestWord(ram, bases[k] + o), 10);
                                    std::memcpy(before[k] + o, ram + bases[k] + o, 4);
                                }
                    }
                    note.resize(g_ownMatrices.size() * 64u);
                    for (size_t i = 0; i < g_ownMatrices.size(); ++i)
                        std::memcpy(note.data() + i * 64u, ram + g_ownMatrices[i], 64);
                    g_ownMatrices.clear();
                    // and the places of the player's effects' sprites: 64 bytes each too, the place and then a mark no
                    // matrix has (star::kOwnPointMark, live_look.h)
                    listOwnSprites(ram);
                    for (const std::array<float, 3> &at : g_ownPoints)
                    {
                        uint32_t words[16];
                        std::memcpy(words, at.data(), 12);
                        for (int k = 3; k < 16; ++k)
                            words[k] = kOwnPointMark;
                        const size_t was = note.size();
                        note.resize(was + 64u);
                        std::memcpy(note.data() + was, words, 64);
                    }
                    g_ownPoints.clear();
                    g_ownerInstances.clear(); // the next frame numbers its instances anew
                    g_ownEffectsListed = false; // the next frame lists the player's effects anew
                    ++g_ownFrame;
                });
            }
            std::printf("[STAR fix] the player's own model pieces are told to the in-between pictures (switch: %s)%c", pieces ? "installed" : "NOT installed", 10);
            if (pieces && runtime.hasFunction(kPacketQuadword))
                g_packetQuadword = runtime.lookupFunction(kPacketQuadword);
            std::printf("[STAR option] creatures shadowing themselves: %s (labels: %s)%c", live().noSelfShadow.load() ? "off" : "as the game does",
                        g_packetQuadword ? "ready" : "NOT available", 10);
            const bool effects = pieces && replace(runtime, kEffectOnBone, &effectOnBoneHook, g_effectOnBone) &&
                                 replace(runtime, kPlaceEmitter, &placeEmitterHook, g_placeEmitter);
            std::printf("[STAR fix] effects made on the player's weapon and arms go with them (switch: %s)%c", effects ? "installed" : "NOT installed", 10);
            const bool view = replace(runtime, kInView, &inViewHook, g_inView);
            std::printf("[STAR fix] view margins for turned pictures, with the mouse (switch: %s)%c", view ? "installed" : "NOT installed", 10);
        }
        {
            const bool text = replace(runtime, kDrawText, &drawTextHook, g_drawText);
            std::printf("[STAR option] text layout fixes %s (switch: %s)\n", live().textFixes.load() ? "on" : "off", text ? "installed" : "NOT installed");
        }
        if (const char *test = std::getenv("STAR_TEXT_TEST"); test && *test)
        {
            // STAR_TEXT_TEST=<hex address>:<text as it is>:<text to put there> (a test): every 20 ms, where the game's
            // memory holds the first text at that address, the second is written over it (not longer than the first).
            const std::string spec(test);
            const size_t c1 = spec.find(':'), c2 = c1 == std::string::npos ? c1 : spec.find(':', c1 + 1);
            if (c2 != std::string::npos)
            {
                const uint32_t address = static_cast<uint32_t>(std::strtoul(spec.substr(0, c1).c_str(), nullptr, 16));
                const std::string from = spec.substr(c1 + 1, c2 - c1 - 1), to = spec.substr(c2 + 1);
                uint8_t *ram = runtime.memory().getRDRAM();
                if (address < 0x01FFF000u && !from.empty() && to.size() <= from.size())
                    std::thread([ram, address, from, to] {
                        for (;;)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                            if (std::memcmp(ram + address, from.c_str(), from.size() + 1) == 0)
                            {
                                std::memcpy(ram + address, to.c_str(), to.size() + 1);
                                std::printf("[text] 0x%x: \"%s\" -> \"%s\"%c", address, from.c_str(), to.c_str(), 10);
                                std::fflush(stdout);
                            }
                        }
                    }).detach();
            }
        }
        if (const char *trace = std::getenv("STAR_TRACE_PAD"); trace && *trace == '1')
        {
            // The game's own copy of the pad data (FUN_001d7110 stores scePadRead's 32 bytes at pad + 0x100; pad 0 is
            // at 0x4b6900): polled every millisecond and printed when it changes. Bytes [2..3] buttons (stored
            // inverted: 1 = pressed), [4..7] right X/Y, left X/Y, [8..19] pressures. (A hook on scePadRead is no use:
            // the call can be switched out and the hook's code after it then runs early.)
            const uint8_t *pad = runtime.memory().getRDRAM() + 0x004B6A00u;
            std::thread([pad] {
                uint8_t last[20] = {};
                uint8_t lastKey[20] = {};
                for (int lines = 0; lines < 4000;)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    uint8_t now[20];
                    std::memcpy(now, pad, sizeof(now));
                    // A real controller's sticks and pressures jitter: a line only when the buttons change, a stick
                    // moves to another quarter of its range, or a pressure starts or stops.
                    uint8_t key[20];
                    std::memcpy(key, now, sizeof(key));
                    for (int i = 4; i < 8; ++i)
                        key[i] = static_cast<uint8_t>(now[i] >> 6);
                    for (int i = 8; i < 20; ++i)
                        key[i] = now[i] ? 1u : 0u;
                    if (std::memcmp(key, lastKey, sizeof(key)) == 0)
                        continue;
                    std::memcpy(lastKey, key, sizeof(key));
                    std::memcpy(last, now, sizeof(now));
                    char text[3 * 20 + 1];
                    for (int i = 0; i < 20; ++i)
                        std::snprintf(text + i * 3, 4, "%02x ", now[i]);
                    std::printf("[pad] %s\n", text);
                    std::fflush(stdout);
                    ++lines;
                }
            }).detach();
            std::printf("[STAR trace] pad data at 0x4b6a00: polled\n");
        }
        if (const char *trace = std::getenv("STAR_TRACE_MC"); trace && *trace == '1')
        {
            const bool info = replace(runtime, kSceMcGetInfo, &mcGetInfoTrace, g_mcGetInfo);
            const bool sync = replace(runtime, kSceMcSync, &mcSyncTrace, g_mcSync);
            std::printf("[STAR trace] memory card: sceMcGetInfo %s, sceMcSync %s\n", info ? "traced" : "MISSING",
                        sync ? "traced" : "MISSING");
        }
        if (const char *trace = std::getenv("STAR_TRACE_MOVIE"); trace && *trace == '1')
        {
            const bool vblank = replace(runtime, kMovieVblank, &movieVblankTrace, g_movieVblank);
            const bool gifEnd = replace(runtime, kMovieGifEnd, &movieGifEndTrace, g_movieGifEnd);
            std::printf("[STAR trace] movie display handlers: vblank %s, gif-end %s\n", vblank ? "traced" : "MISSING",
                        gifEnd ? "traced" : "MISSING");
            g_traceLines = 400; // the display handshake works (2026-11-22): keep the hooks, mute their lines
            const bool sd = replace(runtime, kSceSdRemote, &sdRemoteTrace, g_sdRemote);
            std::printf("[STAR trace] sceSdRemote: %s\n", sd ? "traced" : "MISSING");
            g_sdLines = 120; // the IOP's play position advances (2026-11-22): mute
            const bool st = replace(runtime, kSceCdStRead, &cdStReadTrace, g_cdStRead);
            std::printf("[STAR trace] sceCdStRead: %s\n", st ? "traced" : "MISSING");
            g_stLines = 80; // only 5 reads: the ring stays full (2026-11-22)
            int callbacks = 0;
            for (int i = 0; i < 5; ++i)
                callbacks += replace(runtime, kMpegCallbacks[i], kMpegTraces[i], g_mpegCallback[i]) ? 1 : 0;
            std::printf("[STAR trace] libmpeg callbacks: %d of 5 traced\n", callbacks);
            std::thread([] {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    printMpegCounts();
                }
            }).detach();
        }
    }
}
