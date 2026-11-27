// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSState.h"
#include <memory>
#include <deque>
#include <string>
#include <vector>

class GSRenderer : public GSState
{
private:
	bool Merge(int field);
	bool BeginPresentFrame(bool frame_skip);
	void EndPresentFrame();

	u64 m_shader_time_start = 0;

	std::string m_snapshot;
	u32 m_dump_frames = 0;
	u32 m_skipped_duplicate_frames = 0;
	bool m_saving_metrics = false;

	// Tracking draw counters for idle frame detection.
	u64 m_last_draw_n = 0;
	u64 m_last_transfer_n = 0;

protected:
	GSVector2i m_real_size{0, 0};

	virtual GSTexture* GetOutput(int i, float& scale, int& y_offset) = 0;
	virtual GSTexture* GetFeedbackOutput(float& scale) { return nullptr; }

public:
	GSRenderer();
	virtual ~GSRenderer();

	virtual void Reset(bool hardware_reset) override;

	virtual void Destroy();

	virtual void UpdateRenderFixes();

	virtual void VSync(u32 field, bool registers_written, bool idle_frame);
	virtual bool CanUpscale() { return false; }
	virtual float GetUpscaleMultiplier() { return 1.0f; }
	virtual float GetTextureScaleFactor() { return 1.0f; }
	GSVector2i GetInternalResolution();
	float GetModXYOffset();

	virtual GSTexture* LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size);

	bool IsIdleFrame() const;

	bool SaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
		u32* width, u32* height, std::vector<u32>* pixels);

	void QueueSnapshot(const std::string& path, const u32 gsdump_frames);
	void StopGSDump();
	void StartSavingMetrics(u32 seconds);
	void DumpSavedMetrics();
	bool IsSavingMetrics();
	void PresentCurrentFrame();
	bool BeginCapture(std::string filename, const GSVector2i& size = GSVector2i(0, 0));
	void EndCapture();

	// pcsx2gs (STAR 2026-11-23): pictures for the screen's own refresh rate (GSRenderer.cpp, "kept pictures").
	// The host draws each game frame several times, once for every screen refresh in the frame's time; after each
	// pass a marker (GSState::GIFRegHandlerNOP -> GSRendererHW::FrameDoubleMarker) has the picture kept, with the
	// time it is to be handed to the screen (pcsx2gs::TakeFrameTime). PresentPoll, called by the host on this thread
	// between its GIF data and when it idles, presents each kept picture at its time. While such pictures come, VSync
	// presents nothing itself.
protected:
	u32 m_kept_force_bp = 0xFFFFFFFFu; // GetOutput looks up this frame buffer instead of the displayed one
	void KeepPicture(u32 id);          // the picture now in the frame buffer `m_kept_force_bp`: queued for `id`'s time
	void RepeatPicture(u32 id);        // the picture kept last, once more at `id`'s time
public:
	s64 PresentPoll();                 // presents what is due; microseconds until the next one is (-1: nothing queued)
	bool KeptPicturesActive() const;
private:
	struct KeptPicture
	{
		GSTexture* tex = nullptr;
		GSVector4 draw_rect = GSVector4::zero();
		int refs = 0;
		// what the renderer did to make it (since the kept picture before): draws, primitives, texture uploads,
		// texture copies, render passes, barriers, readbacks; then the frame buffer's width and height
		float stats[9] = {};
	};
	struct KeptEntry
	{
		KeptPicture* picture = nullptr;
		s64 time = 0; // QueryPerformanceCounter ticks (microseconds off Windows)
		u32 id = 0;
		int hold = -1;      // the present's hold in refreshes (pcsx2gs::g_presentHold); -1: the host's (fd_hold)
		bool paced = false; // an ordinary vsync's picture (PaceOrdinary), not one of the host's in-between pictures
	};
	// pcsx2gs (STAR 2026-11-24): the pictures of an ordinary vsync (menus, logos, movies: anything without the host's
	// in-between pictures) go through the same queue, each to a refresh of the screen chosen on an even time line,
	// instead of straight to the screen whenever the vsync happens to be handled (GSRenderer.cpp, PaceOrdinary).
	int PaceOrdinary(s64& time, int& hold); // 0: present now, as PCSX2 does; 1: queue it for `time`; 2: leave it out
	bool QueueKept(GSTexture* src, const GSVector4& uv, int w, int h, const GSVector4& draw_rect, s64 time, u32 id, int hold, bool paced);
	void DropPaced();
	bool m_pace_running = false;
	double m_pace_t = 0.0;           // the even time line: the latest field's place on it (FrameClock units)
	double m_pace_f = 1e9 / 59.94;   // a field's length on it
	s64 m_pace_arrival = 0;          // when the latest field came
	s64 m_pace_target = 0;           // the refresh the latest queued picture is for
	std::deque<KeptEntry> m_kept_queue;
	std::vector<GSTexture*> m_kept_pool;
	KeptPicture* m_kept_last = nullptr;
	s64 m_kept_active_until = 0;
	u32 m_kept_field = 0;
	u64 m_kept_presented = 0, m_kept_dropped = 0, m_kept_late = 0;
	void ReleaseKept(KeptPicture* picture);
	void PresentKept(const KeptEntry& entry, int hold);
	// pcsx2gs (STAR 2026-11-24): the row of kept pictures on a real screen (GSRenderer.cpp, PresentPoll).
	bool KeptSlipFix(s64 now, int& hold); // the row has slipped later than planned: true = leave this picture out
	struct KeptShown
	{
		u32 count = 0;   // the swap chain's number of the present
		s64 planned = 0; // the refresh the picture was meant to come on screen at
	};
	KeptShown m_kept_ring[64];
	s64 m_kept_period = 0, m_kept_grid_until = 0; // the screen's refresh period, asked again at that time
	bool m_kept_holds = false;                    // a real screen: presents carry holds
	int m_kept_slip = 0;                          // refreshes the last picture seen on screen came after its plan
	int m_kept_slip_floor = 0;                    // a slip a correction did not remove: this screen's way, left alone
	int m_kept_fix_from = 0;                      // the slip the last correction was made at, until it is judged
	int m_kept_fix_fails = 0;                     // corrections in a row that changed nothing
	s64 m_kept_slip_since = 0, m_kept_fix_time = 0, m_kept_floor_until = 0;
	u64 m_kept_fixes = 0, m_kept_overdue = 0;
protected:
	// pcsx2gs (STAR 2026-11-24): the host's extra passes are drawing into the frame buffer (between the markers 3
	// and 4, GSRendererHW::FrameDoubleMarker): its contents are not a picture to show. Good for 150 ms, in case the
	// end marker never comes.
	bool m_kept_in_passes = false;
	s64 m_kept_passes_since = 0;
	u32 m_kept_boundaries = 0; // counts the markers between two runs over a frame (a kept picture, the end)
public:
	// pcsx2gs (STAR 2026-11-24): the host is in the middle of a run over a frame (the game's own or an extra pass):
	// compat/Bridge.cpp holds a vsync back until the run's end (KeptBoundaries changes there).
	bool KeptDrawing() const;
	void TestPresentNow(); // a test: the picture kept last is presented once more, wherever the drawing is
	u32 KeptBoundaries() const { return m_kept_boundaries; }
private:
	// pcsx2gs (STAR 2026-11-24): the last pictures handed to the screen, small, for a fault report (the option
	// "ring" turns it on, "ring_save" = a folder writes them out as PNG files; GSRenderer.cpp, RingNote).
	struct RingEntry
	{
		GSTexture* tex = nullptr;
		u32 id = 0;
		char kind = 0;  // k = one of the host's in-between pictures, v = an ordinary vsync's picture
		float stats[9] = {};
		s64 time = 0;
		bool used = false;
	};
	std::vector<RingEntry> m_ring;
	size_t m_ring_next = 0;
	void RingNote(GSTexture* src, const GSVector4& uv, u32 id, char kind, const float* stats = nullptr);
	float m_kept_stats[9] = {};      // for the picture being kept (KeepPicture -> QueueKept)
	double m_kept_counters[7] = {};  // the performance counters at the kept picture before
	void RingSaveIfAsked();
};

extern std::unique_ptr<GSRenderer> g_gs_renderer;
