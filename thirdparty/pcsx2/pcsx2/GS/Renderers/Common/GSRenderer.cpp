// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <chrono>
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "PerformanceMetrics.h"
#include "pcsx2/Config.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "fmt/format.h"
#include "IconsFontAwesome.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace pcsx2gs
{
	void SetPresentRect(float x0, float y0, float x1, float y1, float width, float height); // compat/HostStubs.cpp: the overlay
	extern std::atomic<bool> g_presentArea; // compat/Bridge.cpp: the option present_area (ShrinkForPresent below)
}

// pcsx2gs (STAR 2026-11-23): shrinking the picture to the window. PCSX2 presents with one bilinear sample per window
// pixel, whatever the size of the rendered picture: shrinking by 1.5 (3x rendering in a 1296x972 window) each window
// pixel then gets from one to four source pixels depending on where it falls, so the sharpness of every detail changes
// as it moves across the pixel grid (a texture "vibrates" while the camera turns), and past 2 (4x-5x) source pixels
// are skipped outright (worse, as the user saw). Here the picture is first halved while it is at least twice the size
// it is shown at (a bilinear sample at the centre of 2x2 pixels is their exact average), then resampled to exactly
// twice the shown size and halved once more, so every window pixel is an even average of the same area of the source.
// Returns the texture to present (shown 1:1) or nullptr when nothing was done (not shrinking, or no memory).
static GSTexture* ShrinkForPresent(GSTexture* current, const GSVector4i& src_rect, const GSVector4& draw_rect)
{
	static GSTexture* s_steps[5] = {};
	const int dw = static_cast<int>(std::lround(draw_rect.z - draw_rect.x));
	const int dh = static_cast<int>(std::lround(draw_rect.w - draw_rect.y));
	if (!pcsx2gs::g_presentArea.load(std::memory_order_relaxed) || dw < 16 || dh < 16 ||
		(src_rect.width() <= dw + dw / 20 && src_rect.height() <= dh + dh / 20))
		return nullptr;
	const auto target = [&](int index, int w, int h) -> GSTexture* {
		GSTexture*& t = s_steps[index];
		if (t && (t->GetWidth() != w || t->GetHeight() != h))
		{
			g_gs_device->Recycle(t);
			t = nullptr;
		}
		if (!t)
			t = g_gs_device->CreateRenderTarget(w, h, GSTexture::Format::Color, false);
		return t;
	};
	GSTexture* src = current;
	GSVector4 uv = GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy();
	int w = src_rect.width(), h = src_rect.height();
	int step = 0;
	while (step < 3 && w >= dw * 2 && h >= dh * 2 && (w >= dw * 4 || h >= dh * 4 || (w == dw * 2 && h == dh * 2)))
	{
		// at least four times the shown size (or exactly twice): halve
		GSTexture* half = target(step, w / 2, h / 2);
		if (!half)
			return nullptr;
		g_gs_device->StretchRect(src, uv, half, GSVector4(0.0f, 0.0f, static_cast<float>(w / 2), static_cast<float>(h / 2)),
			ShaderConvert::COPY, Biln);
		src = half;
		uv = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
		w /= 2;
		h /= 2;
		++step;
	}
	if (w != dw || h != dh)
	{
		// to exactly twice the shown size, then the last halving
		GSTexture* twice = target(3, dw * 2, dh * 2);
		GSTexture* shown = target(4, dw, dh);
		if (!twice || !shown)
			return nullptr;
		g_gs_device->StretchRect(src, uv, twice, GSVector4(0.0f, 0.0f, static_cast<float>(dw * 2), static_cast<float>(dh * 2)),
			ShaderConvert::COPY, Biln);
		g_gs_device->StretchRect(twice, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), shown,
			GSVector4(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh)), ShaderConvert::COPY, Biln);
		src = shown;
	}
	return src;
}
#include <deque>
#include <thread>
#include <mutex>

static void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename);

static constexpr std::array<PresentShader, 8> s_tv_shader_indices = {
	PresentShader::COPY, PresentShader::SCANLINE,
	PresentShader::DIAGONAL_FILTER, PresentShader::TRIANGULAR_FILTER,
	PresentShader::COMPLEX_FILTER, PresentShader::LOTTES_FILTER,
	PresentShader::SUPERSAMPLE_4xRGSS, PresentShader::SUPERSAMPLE_AUTO};

static std::deque<std::thread> s_screenshot_threads;
static std::mutex s_screenshot_threads_mutex;

std::unique_ptr<GSRenderer> g_gs_renderer;

// Since we read this on the EE thread, we can't put it in the renderer, because
// we might be switching while the other thread reads it.
static GSVector4 s_last_draw_rect;

// Last time we reset the renderer due to a GPU crash, if any.
static Common::Timer::Value s_last_gpu_reset_time;

// Screen alignment
static GSDisplayAlignment s_display_alignment = GSDisplayAlignment::Center;

GSRenderer::GSRenderer()
	: m_shader_time_start(Common::Timer::GetCurrentValue())
{
	s_last_draw_rect = GSVector4::zero();
}

GSRenderer::~GSRenderer() = default;

void GSRenderer::Reset(bool hardware_reset)
{
	// Clear the current display texture.
	if (hardware_reset)
		g_gs_device->ClearCurrent();

	GSState::Reset(hardware_reset);
}

void GSRenderer::Destroy()
{
	GSCapture::EndCapture();
}

void GSRenderer::UpdateRenderFixes()
{
}

bool GSRenderer::Merge(int field)
{
	GSVector2i fs(0, 0);
	GSTexture* tex[3] = { nullptr, nullptr, nullptr };
	float tex_scale[3] = { 0.0f, 0.0f, 0.0f };
	int y_offset[3] = { 0, 0, 0 };
	const bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;

	// Need to do this here, if the user has Anti-Blur enabled, these offsets can get wiped out/changed.
	const bool game_deinterlacing = (PCRTCDisplays.PCRTCDisplays[0].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[0].framebufferOffsets.y) !=
	                                (PCRTCDisplays.PCRTCDisplays[1].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[1].framebufferOffsets.y);

	// Only need to check the right/bottom on software renderer, hardware always gets the full texture then cuts a bit out later.
	if (PCRTCDisplays.FrameRectMatch() && !PCRTCDisplays.FrameWrap() && !feedback_merge)
	{
		tex[0] = GetOutput(-1, tex_scale[0], y_offset[0]);
		tex[1] = tex[0]; // saves one texture fetch
		y_offset[1] = y_offset[0];
		tex_scale[1] = tex_scale[0];
	}
	else
	{
		const bool use_rc1 =
			PCRTCDisplays.PCRTCDisplays[0].enabled &&                    // RC1 enabled.
				(!(m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 0) || // Blend RC1 with non-zero alpha.
				(m_regs->PMODE.AMOD == 0) ||                             // Use alpha of RC1.
				(feedback_merge && m_regs->EXTBUF.FBIN == 0));           // Use RC1 for feedback merge.

		// The following two flags determine if RC1 output completely overwrites RC2 output
		// due to the alpha used for blending and the respective rectangles of the outputs.
		const bool rc1_contains_rc2 =
			PCRTCDisplays.PCRTCDisplays[0].displayRect.rcontains(PCRTCDisplays.PCRTCDisplays[1].displayRect);

		const bool rc1_overwrites_rc2 = use_rc1 && rc1_contains_rc2 && m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 255;

		const bool use_rc2 =
			PCRTCDisplays.PCRTCDisplays[1].enabled &&                // RC2 enabled.
				((m_regs->PMODE.SLBG == 0 && !rc1_overwrites_rc2) || // Blending RC2 and not overwritten by RC1.
				(m_regs->PMODE.AMOD == 1) ||                         // Use alpha of RC2.
				(feedback_merge && m_regs->EXTBUF.FBIN == 1));       // Use RC2 for feedback merge.

		if (use_rc1)
			tex[0] = GetOutput(0, tex_scale[0], y_offset[0]);
		if (use_rc2)
			tex[1] = GetOutput(1, tex_scale[1], y_offset[1]);
		if (feedback_merge)
			tex[2] = GetFeedbackOutput(tex_scale[2]);
	}

	if (!tex[0] && !tex[1])
	{
		// Clear out the MAD buffer as some remnants of the previously shown frame came be left over, causing a flash for one frame.
		if (GSConfig.InterlaceMode == GSInterlaceMode::Automatic || GSConfig.InterlaceMode >= GSInterlaceMode::AdaptiveTFF)
		{
			GSTexture* mad_tex = g_gs_device->GetMAD();

			if (mad_tex)
			{
				g_gs_device->ClearRenderTarget(mad_tex, 0);
				mad_tex = nullptr;
			}
		}

		// Both circuits off still outputs BGCOLOR on real hardware.
		if (PCRTCDisplays.PCRTCDisplays[0].enabled || PCRTCDisplays.PCRTCDisplays[1].enabled)
		{
			m_real_size = GSVector2i(0, 0);
			return false;
		}
	}

	IncDraw();

	GSVector4 src_gs_read[2] = {};
	GSVector4 dst[3] = {};

	// Use offset for bob deinterlacing always, extra offset added later for FFMD mode.
	const bool scanmask_frame = m_scanmask_used && abs(PCRTCDisplays.PCRTCDisplays[0].displayRect.y - PCRTCDisplays.PCRTCDisplays[1].displayRect.y) != 1;
	int field2 = 0;
	int mode = 3; // If the game is manually deinterlacing then we need to bob (if we want to get away with no deinterlacing).
	bool is_bob = GSConfig.InterlaceMode == GSInterlaceMode::BobTFF || GSConfig.InterlaceMode == GSInterlaceMode::BobBFF;

	// FFMD (half frames) requires blend deinterlacing, so automatically use that. Same when SCANMSK is used but not blended in the merge circuit (Alpine Racer 3).
	if (GSConfig.InterlaceMode != GSInterlaceMode::Automatic || (!game_deinterlacing && !m_regs->SMODE2.FFMD && !scanmask_frame))
	{
		field2 = ((static_cast<int>(GSConfig.InterlaceMode) - 2) & 1);
		mode = ((static_cast<int>(GSConfig.InterlaceMode) - 2) >> 1);
	}

	for (int i = 0; i < 2; i++)
	{
		 const GSPCRTCRegs::PCRTCDisplay& curCircuit = PCRTCDisplays.PCRTCDisplays[i];

		if (!curCircuit.enabled || !tex[i])
			continue;

		const GSVector4 scale = GSVector4(tex_scale[i]);

		// dst is the final destination rect with offset on the screen.
		dst[i] = scale * GSVector4(curCircuit.displayRect);

		// src_gs_read is the size which we're really reading from GS memory.
		src_gs_read[i] = ((GSVector4(curCircuit.framebufferRect) + GSVector4(0, y_offset[i], 0, y_offset[i])) * scale) / GSVector4(tex[i]->GetSize()).xyxy();

		float interlace_offset = 0.0f;
		if (isReallyInterlaced() && m_regs->SMODE2.FFMD && !is_bob && !GSConfig.DisableInterlaceOffset && GSConfig.InterlaceMode != GSInterlaceMode::Off)
		{
			interlace_offset = (scale.y) * static_cast<float>(field ^ field2);
		}
		// Scanmask frame offsets. It's gross, I'm sorry but it sucks.
		if (m_scanmask_used)
		{
			int displayIntOffset = PCRTCDisplays.PCRTCDisplays[i].displayRect.y - PCRTCDisplays.PCRTCDisplays[1 - i].displayRect.y;

			if (displayIntOffset > 0)
			{
				displayIntOffset &= 1;
				dst[i].y -= displayIntOffset * scale.y;
				dst[i].w -= displayIntOffset * scale.y;
				interlace_offset += displayIntOffset;
			}
		}

		dst[i] += GSVector4(0.0f, interlace_offset, 0.0f, interlace_offset);
	}

	if (feedback_merge && tex[2])
	{
		const GSVector4 scale = GSVector4(tex_scale[2]);
		GSVector4i feedback_rect;

		feedback_rect.left = m_regs->EXTBUF.WDX;
		feedback_rect.right = feedback_rect.left + ((m_regs->EXTDATA.WW + 1) / ((m_regs->EXTDATA.SMPH - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGH) + 1));
		feedback_rect.top = m_regs->EXTBUF.WDY;
		feedback_rect.bottom = ((m_regs->EXTDATA.WH + 1) * (2 - m_regs->EXTBUF.WFFMD)) / ((m_regs->EXTDATA.SMPV - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGV) + 1);

		dst[2] = GSVector4(scale * GSVector4(feedback_rect.rsize()));
	}

	const GSVector2i resolution = PCRTCDisplays.GetResolution();
	fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * GetUpscaleMultiplier()),
		static_cast<int>(static_cast<float>(resolution.y) * GetUpscaleMultiplier()));

	m_real_size = GSVector2i(fs.x, fs.y);

	if ((tex[0] || tex[1]) && (tex[0] == tex[1]) && (src_gs_read[0] == src_gs_read[1]).alltrue() && (dst[0] == dst[1]).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].displayRect == PCRTCDisplays.PCRTCDisplays[1].displayRect).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].framebufferRect == PCRTCDisplays.PCRTCDisplays[1].framebufferRect).alltrue() &&
		!feedback_merge && !m_regs->PMODE.SLBG)
	{
		// the two outputs are identical, skip drawing one of them (the one that is alpha blended)
		tex[0] = nullptr;
	}

	const u32 c = (m_regs->BGCOLOR.U32[0] & 0x00FFFFFFu) | (m_regs->PMODE.ALP << 24);
	g_gs_device->Merge(tex, src_gs_read, dst, fs, m_regs->PMODE, m_regs->EXTBUF, c);

	if ((tex[0] || tex[1]) && isReallyInterlaced() && GSConfig.InterlaceMode != GSInterlaceMode::Off)
	{
		const float offset = is_bob ? (tex[1] ? tex_scale[1] : tex_scale[0]) : 0.0f;

		g_gs_device->Interlace(fs, field ^ field2, mode, offset);
	}

	if (GSConfig.ShadeBoost)
		g_gs_device->ShadeBoost();

	if (GSConfig.FXAA)
		g_gs_device->FXAA();

	// Sharpens biinear at lower resolutions, almost nearest but with more uniform pixels.
	if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y))
	{
		g_gs_device->Resize(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight());
	}

	if (m_scanmask_used)
		m_scanmask_used--;

	return true;
}

GSVector2i GSRenderer::GetInternalResolution()
{
	return m_real_size;
}

float GSRenderer::GetModXYOffset()
{
	if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Normal)
	{
		float mod_xy = GetUpscaleMultiplier();
		const int rounded_mod_xy = static_cast<int>(std::round(mod_xy));
		if (rounded_mod_xy > 1)
		{
			if (!(rounded_mod_xy & 1))
				return mod_xy += 0.2f;
			else if (!(rounded_mod_xy & 2))
				return mod_xy += 0.3f;
			else
				return mod_xy += 0.1f;
		}
	}

	return 0.0f;
}

static float GetCurrentAspectRatioFloat(bool is_progressive)
{
	switch (GSConfig.AspectRatio)
	{
		default:
		// We don't know the AR of the display here, nor we care about it
		case AspectRatioType::Stretch:
		case AspectRatioType::RAuto4_3_3_2:
			if (EmuConfig.CurrentCustomAspectRatio > 0.f)
				return EmuConfig.CurrentCustomAspectRatio;
			else if (is_progressive)
				return 3.0f / 2.0f;
			else
				return 4.0f / 3.0f;
		case AspectRatioType::R4_3:
			return 4.0f / 3.0f;
		case AspectRatioType::R16_9:
			return 16.0f / 9.0f;
		case AspectRatioType::R10_7:
			return 10.0f / 7.0f;
	}
}

static GSVector4 CalculateDrawDstRect(s32 window_width, s32 window_height, const GSVector4i& src_rect, const GSVector2i& src_size, GSDisplayAlignment alignment, bool flip_y, bool is_progressive)
{
	const float f_width = static_cast<float>(window_width);
	const float f_height = static_cast<float>(window_height);
	const float clientAr = f_width / f_height;

	float targetAr = clientAr;
	if (EmuConfig.CurrentAspectRatio == AspectRatioType::RAuto4_3_3_2)
	{
		if (is_progressive)
			targetAr = 3.0f / 2.0f;
		else
			targetAr = 4.0f / 3.0f;
		// Fall back on the custom aspect ratio set by patches (e.g. 16:9, 21:9)
		if (EmuConfig.CurrentCustomAspectRatio > 0.f)
			targetAr = EmuConfig.CurrentCustomAspectRatio;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R4_3)
	{
		targetAr = 4.0f / 3.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R16_9)
	{
		targetAr = 16.0f / 9.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R10_7)
	{
		targetAr = 10.0f / 7.0f;
	}

	const float crop_adjust = (static_cast<float>(src_rect.width()) / static_cast<float>(src_size.x)) /
		(static_cast<float>(src_rect.height()) / static_cast<float>(src_size.y));

	const double arr = (targetAr * crop_adjust) / clientAr;
	float target_width = f_width;
	float target_height = f_height;
	if (arr < 1)
		target_width = std::floor(f_width * arr + 0.5f);
	else if (arr > 1)
		target_height = std::floor(f_height / arr + 0.5f);

	target_height *= GSConfig.StretchY / 100.0f;

	if (GSConfig.IntegerScaling)
	{
		// make target width/height an integer multiple of the texture width/height
		float t_width = static_cast<double>(src_rect.width());
		float t_height = static_cast<double>(src_rect.height());

		// If using Bilinear (Shape) the image will be prescaled to larger than the window, so we need to unscale it.
		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && src_rect.width() > 0 && src_rect.height() > 0)
		{
			const GSVector2i resolution = g_gs_renderer->PCRTCDisplays.GetResolution();
			const GSVector2i fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * g_gs_renderer->GetUpscaleMultiplier()),
				static_cast<int>(static_cast<float>(resolution.y) * g_gs_renderer->GetUpscaleMultiplier()));

			if (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y)
			{
				t_width *= static_cast<float>(fs.x) / src_rect.width();
				t_height *= static_cast<float>(fs.y) / src_rect.height();
			}
		}

		float scale;
		if ((t_width / t_height) >= 1.0)
			scale = target_width / t_width;
		else
			scale = target_height / t_height;

		if (scale > 1.0)
		{
			const float adjust = std::floor(scale) / scale;
			target_width = target_width * adjust;
			target_height = target_height * adjust;
		}
	}

	float target_x, target_y;
	if (target_width >= f_width)
	{
		target_x = -((target_width - f_width) * 0.5f);
	}
	else
	{
		switch (alignment)
		{
			case GSDisplayAlignment::Center:
				target_x = (f_width - target_width) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_x = (f_width - target_width);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				target_x = 0.0f;
				break;
		}
	}
	if (target_height >= f_height)
	{
		target_y = -((target_height - f_height) * 0.5f);
	}
	else
	{
		switch (alignment)
		{
			case GSDisplayAlignment::Center:
				target_y = (f_height - target_height) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_y = (f_height - target_height);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				target_y = 0.0f;
				break;
		}
	}

	GSVector4 ret(target_x, target_y, target_x + target_width, target_y + target_height);

	if (flip_y)
	{
		const float height = ret.w - ret.y;
		ret.y = static_cast<float>(window_height) - ret.w;
		ret.w = ret.y + height;
	}

	return ret;
}

static GSVector4i CalculateDrawSrcRect(const GSTexture* src, const GSVector2i real_size)
{
	const GSVector2i size(src->GetSize());
	const GSVector2 scale = GSVector2(size.x, size.y) / GSVector2(real_size.x, real_size.y).max(GSVector2(0.1f, 0.1f));
	const float upscale = GSIsHardwareRenderer() ? GSConfig.UpscaleMultiplier : 1;
	const int left = static_cast<int>(static_cast<float>(GSConfig.Crop[0] * scale.x) * upscale);
	const int top = static_cast<int>(static_cast<float>(GSConfig.Crop[1] * scale.y) * upscale);
	const int right =  size.x - static_cast<int>(static_cast<float>(GSConfig.Crop[2] * scale.x) * upscale);
	const int bottom = size.y - static_cast<int>(static_cast<float>(GSConfig.Crop[3] * scale.y) * upscale);
	return GSVector4i(left, top, right, bottom);
}

static const char* GetScreenshotSuffix()
{
	static constexpr const char* suffixes[static_cast<u8>(GSScreenshotFormat::Count)] = {
		"png", "jpg", "webp"};
	return suffixes[static_cast<u8>(GSConfig.ScreenshotFormat)];
}

static void CompressAndWriteScreenshot(std::string filename, u32 width, u32 height, std::vector<u32> pixels)
{
	RGBA8Image image;
	image.SetPixels(width, height, std::move(pixels));

	std::string key(fmt::format("GSScreenshot_{}", filename));

	if (!GSDumpReplayer::IsRunner())
	{
		Host::AddIconOSDMessage(key, ICON_FA_CAMERA,
			fmt::format(TRANSLATE_FS("GS", "Saving screenshot to '{}'."), Path::GetFileName(filename)), 60.0f);
	}

	// maybe std::async would be better here.. but it's definitely worth threading, large screenshots take a while to compress.
	std::unique_lock lock(s_screenshot_threads_mutex);
	s_screenshot_threads.emplace_back([key = std::move(key), filename = std::move(filename), image = std::move(image),
										  quality = GSConfig.ScreenshotQuality]() {
		if (image.SaveToFile(filename.c_str(), quality))
		{
			if (!GSDumpReplayer::IsRunner())
			{
				Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
					fmt::format(TRANSLATE_FS("GS", "Saved screenshot to '{}'."), Path::GetFileName(filename)),
					Host::OSD_INFO_DURATION);
			}
		}
		else
		{
			Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
				fmt::format(TRANSLATE_FS("GS", "Failed to save screenshot to '{}'."), Path::GetFileName(filename),
					Host::OSD_ERROR_DURATION));
		}

		// remove ourselves from the list, if the GS thread is waiting for us, we won't be in there
		const auto this_id = std::this_thread::get_id();
		std::unique_lock lock(s_screenshot_threads_mutex);
		for (auto it = s_screenshot_threads.begin(); it != s_screenshot_threads.end(); ++it)
		{
			if (it->get_id() == this_id)
			{
				it->detach();
				s_screenshot_threads.erase(it);
				break;
			}
		}
	});
}

void GSJoinSnapshotThreads()
{
	std::unique_lock lock(s_screenshot_threads_mutex);
	while (!s_screenshot_threads.empty())
	{
		std::thread save_thread(std::move(s_screenshot_threads.front()));
		s_screenshot_threads.pop_front();
		lock.unlock();
		save_thread.join();
		lock.lock();
	}
}

bool GSRenderer::BeginPresentFrame(bool frame_skip)
{
	Host::BeginPresentFrame();

	if (GSDumpReplayer::IsReplayingDump())
		GSDumpReplayer::UpdateGSStats();

	const GSDevice::PresentResult res = g_gs_device->BeginPresent(frame_skip);
	if (res == GSDevice::PresentResult::FrameSkipped)
	{
		// If we're skipping a frame, we need to reset imgui's state, since
		// we won't be calling EndPresentFrame().
		ImGuiManager::SkipFrame();
		return false;
	}
	else if (res == GSDevice::PresentResult::OK)
	{
		// All good!
		return true;
	}

	// If we're constantly crashing on something in particular, we don't want to end up in an
	// endless reset loop.. that'd probably end up leaking memory and/or crashing us for other
	// reasons. So just abort in such case.
	const Common::Timer::Value current_time = Common::Timer::GetCurrentValue();
	if (s_last_gpu_reset_time != 0 &&
		Common::Timer::ConvertValueToSeconds(current_time - s_last_gpu_reset_time) < 15.0f)
	{
		pxFailRel("Host GPU lost too many times, device is probably completely wedged.");
	}
	s_last_gpu_reset_time = current_time;

	// Device lost, something went really bad.
	// Let's just toss out everything, and try to hobble on.
	if (!GSreopen(true, false, GSGetCurrentRenderer(), std::nullopt))
	{
		pxFailRel("Failed to recreate GS device after loss.");
		return false;
	}

	// First frame after reopening is definitely going to be trash, so skip it.
	Host::AddIconOSDMessage("GSDeviceLost", ICON_FA_TRIANGLE_EXCLAMATION,
		TRANSLATE_SV("GS", "Host GPU device encountered an error and was recovered. This may have broken rendering."),
		Host::OSD_CRITICAL_ERROR_DURATION);
	return false;
}

// pcsx2gs (STAR, tests): PS2X_PCSX2_GPU_LOG=1 prints, once a second, the GPU's own time for the pictures presented
static void StarGpuLog()
{
	static const bool gpu_log = std::getenv("PS2X_PCSX2_GPU_LOG") != nullptr;
	if (!gpu_log)
		return;
	static auto window = std::chrono::steady_clock::now();
	static double gpu_ms = 0.0, gpu_max = 0.0;
	static int presents = 0;
	const double gpu_time = g_gs_device->GetAndResetAccumulatedGPUTime();
	gpu_ms += gpu_time;
	gpu_max = std::max(gpu_max, gpu_time);
	++presents;
	const auto now = std::chrono::steady_clock::now();
	if (now - window >= std::chrono::seconds(1))
	{
		std::printf("[gpu] last second: %.1f ms of GPU time over %d pictures (%.2f ms each, the most %.2f)%c", gpu_ms, presents,
			presents ? gpu_ms / presents : 0.0, gpu_max, 10);
		window = now;
		gpu_ms = gpu_max = 0.0;
		presents = 0;
	}
}

void GSRenderer::EndPresentFrame()
{
	if (GSDumpReplayer::IsReplayingDump())
		GSDumpReplayer::RenderUI();

	FullscreenUI::Render();
	ImGuiManager::RenderOSD();
	g_gs_device->EndPresent();
	StarGpuLog();
	ImGuiManager::NewFrame();
}

// PS2X_PCSX2_PRESENT_DUMP=<folder>:<first>:<last>: the picture as it goes to the window (after the shrink, or
// PCSX2's single bilinear sample when that is off), one file per present in that range of presents; a kept picture's
// file name also carries its id. For tests: it works in a hidden run and reads nothing from the screen.
static void DumpPresented(GSTexture* shown, GSTexture* current, const GSVector4& src_uv, const GSVector4& draw_rect, const char* tag)
{
	static const struct PresentDump
	{
		std::string folder;
		u64 first = 1, last = 0;
		PresentDump()
		{
			const char* v = std::getenv("PS2X_PCSX2_PRESENT_DUMP");
			const std::string text(v ? v : "");
			const size_t b = text.rfind(':');
			const size_t a = b == std::string::npos || b == 0 ? std::string::npos : text.rfind(':', b - 1);
			if (a != std::string::npos && a >= 2)
			{
				folder = text.substr(0, a);
				first = std::strtoull(text.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
				last = std::strtoull(text.substr(b + 1).c_str(), nullptr, 10);
			}
		}
	} dump;
	static u64 s_present = 0;
	++s_present;
	if (s_present < dump.first || s_present > dump.last)
		return;
	GSTexture* temp = nullptr;
	if (!shown && current)
	{
		const int dw = static_cast<int>(std::lround(draw_rect.z - draw_rect.x)), dh = static_cast<int>(std::lround(draw_rect.w - draw_rect.y));
		temp = (dw > 0 && dh > 0) ? g_gs_device->CreateRenderTarget(dw, dh, GSTexture::Format::Color, false) : nullptr;
		if (temp)
			g_gs_device->StretchRect(current, src_uv, temp, GSVector4(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh)),
				ShaderConvert::COPY, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
		shown = temp;
	}
	if (shown)
		shown->Save(dump.folder + "/present-" + std::to_string(s_present) + tag + ".png");
	if (temp)
		g_gs_device->Recycle(temp);
}

// pcsx2gs (STAR): kept pictures (GSRenderer.h). Times are QueryPerformanceCounter ticks on Windows.
namespace pcsx2gs
{
	bool TakeFrameTime(u32 id, s64& time); // compat/Bridge.cpp: the option fd_times
	s64 FrameClock();                       // compat/Bridge.cpp: now, in the unit of those times
	s64 FrameClockPerSecond();
	void NotePresented();                   // compat/Bridge.cpp: PS2X_PRESENT_STATS
	extern std::atomic<int> g_keptHold;     // compat/Bridge.cpp: the option fd_hold
	extern std::atomic<int> g_presentHold;  // compat/Bridge.cpp: for the present being made (VKDxgiPresenter::Present)
	bool ScreenGrid(s64& period, s64& phase, bool& holds); // compat/Bridge.cpp: the screen's refreshes
	u32 LastPresentCount();                                // compat/Bridge.cpp: the swap chain's statistics
	extern std::atomic<bool> g_ringOn;                     // compat/Bridge.cpp: the options ring and ring_save
	bool TakeRingSave(std::string& folder);
	bool ShownPresent(u32& count, s64& time, s64 period);
}

// PS2X_PCSX2_PACE_LOG=<file>: a line for every ordinary picture queued (q) and presented (p), for tools/pacecheck.py.
// STAR 2026-11-27, for tools/pacewhy.py and paceruns.py: of the host's in-between pictures, a = ready (kept), r = ready
// again (repeated), k = its present called, e = that call returned (the id in the target column); m = a marker of the
// host's runs over a frame reached this thread (its kind in the hold column); V = a vsync handled, from now to time.
static std::FILE* PaceLogFile()
{
	static std::FILE* const file = []() -> std::FILE* {
		const char* path = std::getenv("PS2X_PCSX2_PACE_LOG");
		std::FILE* f = (path && *path) ? std::fopen(path, "w") : nullptr;
		if (f)
			std::fputs("kind,now,target,interval,time,hold,period,off_line\n", f);
		return f;
	}();
	return file;
}

namespace pcsx2gs
{
	// m = a marker of the host's runs over a frame reached this thread (GSRendererHW::FrameDoubleMarker): its kind in
	// the hold column, its id in the target column.
	void PaceNote(u32 kind, u32 id)
	{
		if (std::FILE* const log = PaceLogFile())
			std::fprintf(log, "m,%lld,%u,0,0,%u,0,0\n", static_cast<long long>(FrameClock()), id, kind);
	}
}

// The last pictures handed to the screen, kept small (480x360) in a ring of 512: 3.5 seconds at 144 a second. For a
// fault that shows for a split second and will not repeat: the player presses the key (the host's "ring_save") right
// after seeing it and the pictures are written out, each with its kind and id.
void GSRenderer::RingNote(GSTexture* src, const GSVector4& uv, u32 id, char kind, const float* stats)
{
	if (!pcsx2gs::g_ringOn.load(std::memory_order_relaxed))
	{
		if (!m_ring.empty())
		{
			for (RingEntry& entry : m_ring)
				if (entry.tex)
					g_gs_device->Recycle(entry.tex);
			m_ring.clear();
			m_ring_next = 0;
		}
		return;
	}
	if (!src)
		return;
	// PS2X_PCSX2_RING_COUNT=<n> (tests): a longer ring (0.7 MB of video memory a picture).
	static const size_t kCount = [] {
		const char* value = std::getenv("PS2X_PCSX2_RING_COUNT");
		const int n = value && *value ? std::atoi(value) : 0;
		return static_cast<size_t>(n >= 16 && n <= 20000 ? n : 512);
	}();
	constexpr int kWidth = 480, kHeight = 360;
	if (m_ring.empty())
		m_ring.resize(kCount);
	RingEntry& entry = m_ring[m_ring_next];
	if (!entry.tex)
		entry.tex = g_gs_device->CreateRenderTarget(kWidth, kHeight, GSTexture::Format::Color, false);
	if (!entry.tex)
		return;
	g_gs_device->StretchRect(src, uv, entry.tex, GSVector4(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)),
		ShaderConvert::COPY, Biln);
	entry.id = id;
	entry.kind = kind;
	entry.time = pcsx2gs::FrameClock();
	entry.used = true;
	for (int i = 0; i < 9; i++)
		entry.stats[i] = stats ? stats[i] : 0.0f;
	m_ring_next = (m_ring_next + 1) % kCount;
}

void GSRenderer::RingSaveIfAsked()
{
	std::string folder;
	if (!pcsx2gs::TakeRingSave(folder))
		return;
	size_t written = 0;
	s64 newest = 0;
	for (const RingEntry& entry : m_ring)
		if (entry.used)
			newest = std::max(newest, entry.time);
	std::FILE* const list = std::fopen((folder + "/list.txt").c_str(), "w");
	for (size_t i = 0; i < m_ring.size(); i++)
	{
		const RingEntry& entry = m_ring[(m_ring_next + i) % m_ring.size()]; // the oldest first
		if (!entry.used || !entry.tex)
			continue;
		char name[64];
		std::snprintf(name, sizeof(name), "%03zu-%c%u.png", written, entry.kind, entry.id);
		if (entry.tex->Save(folder + "/" + name))
		{
			if (list)
				std::fprintf(list, "%s  %.1f ms before the newest; draws %.0f prims %.0f uploads %.0f copies %.0f passes %.0f barriers %.0f readbacks %.0f; frame buffer %.0fx%.0f\n",
					name, static_cast<double>(newest - entry.time) / 1e6, entry.stats[0], entry.stats[1], entry.stats[2], entry.stats[3],
					entry.stats[4], entry.stats[5], entry.stats[6], entry.stats[7], entry.stats[8]);
			written++;
		}
	}
	if (list)
		std::fclose(list);
	// (STAR: the folder's own name only; logs get posted with bug reports and the folders above can hold a person's name)
	const size_t folderSlash = folder.find_last_of("/\\");
	std::fprintf(stderr, "[pictures] %zu of the last pictures written to %s%s (the newest at clock %.1f ms)\n", written,
		folder.c_str() + (folderSlash == std::string::npos ? 0 : folderSlash + 1),
		m_ring.empty() ? " (the recorder is off: nothing was kept)" : "", static_cast<double>(newest) / 1e6);
	std::fflush(stderr);
}

void GSRenderer::TestPresentNow()
{
	if (!m_kept_last)
		return;
	KeptEntry entry{m_kept_last, pcsx2gs::FrameClock(), 0xFFFFFFFFu, 0, true}; // id -1: not put into the recorder
	PresentKept(entry, 0);
}

bool GSRenderer::KeptDrawing() const
{
	return m_kept_in_passes && pcsx2gs::FrameClock() - m_kept_passes_since < pcsx2gs::FrameClockPerSecond() * 15 / 100;
}

bool GSRenderer::KeptPicturesActive() const
{
	if (pcsx2gs::FrameClock() < m_kept_active_until)
		return true;
	for (const KeptEntry& entry : m_kept_queue)
	{
		if (!entry.paced)
			return true;
	}
	return false;
}

void GSRenderer::DropPaced()
{
	for (auto it = m_kept_queue.begin(); it != m_kept_queue.end();)
	{
		if (it->paced)
		{
			ReleaseKept(it->picture);
			it = m_kept_queue.erase(it);
		}
		else
			++it;
	}
}

// The picture of an ordinary vsync, and the screen refresh it goes to.
// The fields come 59.94 times a second, handled by this thread a little earlier or later each time; the screen
// refreshes at a rate of its own. Presented as they come, each picture lands on whichever refresh is next: on a
// 144 Hz screen 2 or 3 refreshes each in no order, on a 60 Hz one a burst of doubled and lost pictures every 17
// seconds while the two rhythms pass each other. Here the fields are put on an even time line that follows their
// arrival slowly, and each goes to the first refresh after its place on it: the most even row the two rates allow
// (144 Hz: 2 3 2 2 3; 120: all 2; 60: all 1 with one doubled every 17 s; 50: one in six left out), whatever the rate.
int GSRenderer::PaceOrdinary(s64& time, int& hold)
{
	// Off unless PS2X_PCSX2_PACE=1 (STAR 2026-11-24, later the same day): the even row costs delay (the lead, and on a
	// real screen a row that slips a refresh later whenever a present misses its turn and never comes back), and the
	// user saw no difference in the menus but wants no delay. PCSX2's way: each picture at once.
	static const bool s_on = [] {
		const char* value = std::getenv("PS2X_PCSX2_PACE");
		return value && *value == '1';
	}();
	static const double s_lead_ms = [] {
		const char* value = std::getenv("PS2X_PCSX2_PACE_LEAD_MS");
		return (value && *value) ? std::atof(value) : -1.0;
	}();
	s64 period = 0, phase = 0;
	bool holds = false;
	if (!s_on || !g_gs_device || g_gs_device->GetVSyncMode() == GSVSyncMode::Disabled || !pcsx2gs::ScreenGrid(period, phase, holds))
	{
		m_pace_running = false; // no sync (a variable-rate screen shows each picture as it comes) or no screen known
		return 0;
	}
	const s64 now = pcsx2gs::FrameClock();
	const double arrival = static_cast<double>(now);
	const double off_line = arrival - (m_pace_t + m_pace_f);
	bool fresh = false;
	if (!m_pace_running || std::abs(off_line) > 0.75 * m_pace_f)
	{
		// the first one, or the game stalled or jumped: the line begins anew
		const double since = static_cast<double>(now - m_pace_arrival);
		if (m_pace_running && since > 8e6 && since < 50e6)
			m_pace_f += 0.1 * (since - m_pace_f); // fields of another length (the game too slow for its 59.94)
		m_pace_t = arrival;
		m_pace_running = true;
		fresh = true;
	}
	else
	{
		// follows the arrivals slowly (place and length), not each one's earliness or lateness
		m_pace_t += m_pace_f + 0.1 * off_line;
		m_pace_f = std::clamp(m_pace_f + 0.0025 * off_line, 8e6, 50e6);
	}
	m_pace_arrival = now;

	// The picture is wanted on screen `lead` after its place on the line: time for a late arrival (2 ms), for the
	// present to get through Windows before the refresh (up to 7 ms), and the band below.
	const double p = static_cast<double>(period);
	const double band = 0.12 * p;
	const double lead = s_lead_ms >= 0.0 ? s_lead_ms * 1e6 : std::min(p, 7e6) + 2e6 + band;
	const double usual = m_pace_f / p; // a field in refreshes
	const auto at = [&](double n) { return phase + static_cast<s64>(n) * period; };
	const auto steps = [&](s64 from, s64 to) { return static_cast<s64>(std::llround(static_cast<double>(to - from) / p)); };
	// The refresh for a field at `line` on the time line, after a picture at the refresh `before` (0: none).
	// unsure: right at a refresh, where the other choice was as likely.
	const auto pick = [&](double line, s64 before, bool& unsure) {
		const double x = (line + lead - static_cast<double>(phase)) / p; // in refreshes, counted from `phase`
		const double nearest = std::round(x);
		unsure = false;
		if (before == 0 || std::abs(x - nearest) * p >= band)
			return at(std::ceil(x));
		// Right at a refresh: the line's small unrest would send one picture to this refresh and the next to the one
		// after, back and forth. Of the two, the one that keeps the step from the picture before nearest to the usual.
		unsure = true;
		const s64 a = at(nearest), b = at(nearest + 1.0);
		return std::abs(static_cast<double>(steps(before, a)) - usual) <= std::abs(static_cast<double>(steps(before, b)) - usual) ? a : b;
	};
	bool unsure = false;
	const s64 target = pick(m_pace_t, fresh ? 0 : m_pace_target, unsure);
	const s64 interval = (fresh || m_pace_target == 0) ? 1 : steps(m_pace_target, target);
	std::FILE* const log = PaceLogFile();
	if (interval <= 0)
	{
		// the refresh of the picture before (a screen slower than the fields): this one is left out
		if (log)
			std::fprintf(log, "s,%lld,%lld,%lld,0,0,%lld,%.0f\n", static_cast<long long>(now), static_cast<long long>(target),
				static_cast<long long>(interval), static_cast<long long>(period), off_line);
		return 2;
	}
	m_pace_target = target;
	// Handed over just after the refresh before its own. On a real screen Windows is told how long this picture stays
	// (DXGI's sync interval: the picture's own length, measured 2026-11-24 with PS2X_PRESENT_STATS: 99.7% stayed as
	// long as their own present said), so one handed over late moves the row on by a refresh once instead of making
	// an uneven pair. Its length is the step to the next picture's refresh, worked out from the line; where that is
	// unsure the shorter one (too long would push the whole row later for good), and 4 at most (DXGI's limit).
	time = target - period + 500000;
	hold = 0;
	if (holds)
	{
		s64 next_steps = 0;
		for (int ahead = 1; ahead <= 4 && next_steps <= 0; ahead++)
		{
			bool next_unsure = false;
			const s64 next = pick(m_pace_t + ahead * m_pace_f, target, next_unsure);
			next_steps = steps(target, next);
			if (next_unsure && next_steps > 1)
			{
				// the earlier of the two refreshes it may go to
				const double x = (m_pace_t + ahead * m_pace_f + lead - static_cast<double>(phase)) / p;
				next_steps = std::max<s64>(1, std::min(next_steps, steps(target, at(std::round(x)))));
			}
		}
		hold = static_cast<int>(std::clamp<s64>(next_steps, 1, 4));
	}
	if (log)
		std::fprintf(log, "q,%lld,%lld,%lld,%lld,%d,%lld,%.0f\n", static_cast<long long>(now), static_cast<long long>(target),
			static_cast<long long>(interval), static_cast<long long>(time), hold, static_cast<long long>(period), off_line);
	return 1;
}

void GSRenderer::ReleaseKept(KeptPicture* picture)
{
	if (!picture || --picture->refs > 0)
		return;
	if (picture->tex)
	{
		if (m_kept_pool.size() < 24)
			m_kept_pool.push_back(picture->tex);
		else
			g_gs_device->Recycle(picture->tex);
	}
	delete picture;
}

void GSRenderer::KeepPicture(u32 id)
{
	s64 time = 0;
	if (!pcsx2gs::TakeFrameTime(id, time))
		return;
	// The merge of the frame buffer named by m_kept_force_bp, as a vsync would make it of the displayed one.
	if (!Merge(static_cast<int>(m_kept_field)))
		return;
	GSTexture* current = g_gs_device->GetCurrent();
	if (!current)
		return;
	const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
	const GSVector4 draw_rect(CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
		src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
		GetVideoMode() == GSVideoMode::SDTV_480P));
	GSTexture* src = ShrinkForPresent(current, src_rect, draw_rect);
	GSVector4 uv(0.0f, 0.0f, 1.0f, 1.0f);
	int w, h;
	if (src)
	{
		w = src->GetWidth();
		h = src->GetHeight();
	}
	else
	{
		src = current;
		uv = GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy();
		w = src_rect.width();
		h = src_rect.height();
	}
	{
		// for the recorder: what the renderer did since the kept picture before (the counters start over now and
		// then: a smaller value counts from zero)
		static const GSPerfMon::counter_t kCounters[7] = {GSPerfMon::Draw, GSPerfMon::Prim, GSPerfMon::TextureUploads,
			GSPerfMon::TextureCopies, GSPerfMon::RenderPasses, GSPerfMon::Barriers, GSPerfMon::Readbacks};
		for (int i = 0; i < 7; i++)
		{
			const double now = g_perfmon.GetCounter(kCounters[i]);
			m_kept_stats[i] = static_cast<float>(now >= m_kept_counters[i] ? now - m_kept_counters[i] : now);
			m_kept_counters[i] = now;
		}
		m_kept_stats[7] = static_cast<float>(current->GetWidth());
		m_kept_stats[8] = static_cast<float>(current->GetHeight());
	}
	QueueKept(src, uv, w, h, draw_rect, time, id, -1, false);
}

bool GSRenderer::QueueKept(GSTexture* src, const GSVector4& uv, int w, int h, const GSVector4& draw_rect, s64 time, u32 id, int hold, bool paced)
{
	if (w <= 0 || h <= 0)
		return false;
	GSTexture* tex = nullptr;
	for (size_t i = 0; i < m_kept_pool.size(); i++)
	{
		if (m_kept_pool[i]->GetWidth() == w && m_kept_pool[i]->GetHeight() == h)
		{
			tex = m_kept_pool[i];
			m_kept_pool.erase(m_kept_pool.begin() + i);
			break;
		}
	}
	if (!tex)
	{
		// a pool of another size (the window or the resolution changed): let it go
		for (GSTexture* old : m_kept_pool)
			g_gs_device->Recycle(old);
		m_kept_pool.clear();
		tex = g_gs_device->CreateRenderTarget(w, h, GSTexture::Format::Color, false);
		if (!tex)
			return false;
	}
	g_gs_device->StretchRect(src, uv, tex, GSVector4(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)), ShaderConvert::COPY, Nearest);
	KeptPicture* picture = new KeptPicture();
	if (!paced)
		std::memcpy(picture->stats, m_kept_stats, sizeof(picture->stats));
	picture->tex = tex;
	picture->draw_rect = draw_rect;
	if (paced)
		picture->refs = 1; // the queue entry
	else
	{
		picture->refs = 2; // the queue entry and m_kept_last
		ReleaseKept(m_kept_last);
		m_kept_last = picture;
		m_kept_active_until = pcsx2gs::FrameClock() + pcsx2gs::FrameClockPerSecond() / 4;
	}
	m_kept_queue.push_back(KeptEntry{picture, time, id, hold, paced});
	// the log: a = an in-between picture is ready (its id in the target column)
	if (!paced)
		if (std::FILE* const log = PaceLogFile())
			std::fprintf(log, "a,%lld,%u,0,%lld,0,0,0\n", static_cast<long long>(pcsx2gs::FrameClock()), id, static_cast<long long>(time));
	return true;
}

void GSRenderer::RepeatPicture(u32 id)
{
	s64 time = 0;
	if (!m_kept_last || !pcsx2gs::TakeFrameTime(id, time))
		return;
	m_kept_last->refs++;
	m_kept_queue.push_back(KeptEntry{m_kept_last, time, id, -1, false});
	if (std::FILE* const log = PaceLogFile()) // r = the picture kept last is ready again under this id
		std::fprintf(log, "r,%lld,%u,0,%lld,0,0,0\n", static_cast<long long>(pcsx2gs::FrameClock()), id, static_cast<long long>(time));
	m_kept_active_until = pcsx2gs::FrameClock() + pcsx2gs::FrameClockPerSecond() / 4;
}

void GSRenderer::PresentKept(const KeptEntry& entry, int hold)
{
	GSTexture* tex = entry.picture->tex;
	if (!entry.paced)
		DumpPresented(tex, nullptr, GSVector4::zero(), entry.picture->draw_rect, ("-id" + std::to_string(entry.id)).c_str());
	// the log: p = an ordinary picture, k = one of the host's in-between pictures (its hold: the host's)
	if (std::FILE* const log = PaceLogFile())
		std::fprintf(log, "%c,%lld,%u,0,%lld,%d,0,0\n", entry.paced ? 'p' : 'k', static_cast<long long>(pcsx2gs::FrameClock()),
			entry.paced ? 0u : entry.id, static_cast<long long>(entry.time), hold);
	if (entry.id != 0xFFFFFFFFu)
		RingNote(tex, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), entry.id, entry.paced ? 'v' : 'k', entry.picture->stats);
	pcsx2gs::g_presentHold.store(hold, std::memory_order_relaxed);
	if (BeginPresentFrame(false))
	{
		s_last_draw_rect = entry.picture->draw_rect;
		const u64 current_time = Common::Timer::GetCurrentValue();
		const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));
		g_gs_device->PresentRect(tex, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), nullptr, entry.picture->draw_rect,
			s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
		EndPresentFrame();
		pcsx2gs::NotePresented();
		// the log: e = the present call of the picture above has returned
		if (std::FILE* const log = PaceLogFile())
			std::fprintf(log, "e,%lld,%u,0,%lld,%d,0,0\n", static_cast<long long>(pcsx2gs::FrameClock()), entry.paced ? 0u : entry.id,
				static_cast<long long>(entry.time), hold);
		if (!entry.paced && m_kept_holds && m_kept_period > 0)
		{
			// for KeptSlipFix: handed over 1 ms after a refresh (the host's times), meant for the refresh after it
			const u32 count = pcsx2gs::LastPresentCount();
			m_kept_ring[count & 63u] = KeptShown{count, entry.time - 1000000 + m_kept_period};
		}
	}
	++m_kept_presented;
}

// On a real screen each present says how long its picture stays (the hold), so Windows shows the pictures in an
// even row. A present that misses its turn then moves the whole row a refresh later, and it stays there: 7 ms of
// delay at 144 Hz that nobody planned (measured 2026-11-24 with tools/pacedelay.py: the row between +0 and +1 in
// play). Here the swap chain's statistics say when the picture last seen on screen really came on; when the row has
// been a refresh or more late for a quarter of a second, one picture gives it back: shown a refresh shorter (holds
// of 2 and more) or left out (a hold of 1: the pictures before and after it take its place). If that changes
// nothing the slip is this screen's own way to the glass (some compose a refresh later): left alone, tried again
// after 30 s. PS2X_PCSX2_KEPT_SLIP=0: off.
bool GSRenderer::KeptSlipFix(s64 now, int& hold)
{
	static const bool s_on = [] {
		const char* value = std::getenv("PS2X_PCSX2_KEPT_SLIP");
		return !(value && *value == '0');
	}();
	u32 count = 0;
	s64 shown_time = 0;
	if (!s_on || !pcsx2gs::ShownPresent(count, shown_time, m_kept_period))
		return false;
	const KeptShown& shown = m_kept_ring[count & 63u];
	if (shown.count != count || shown.planned == 0)
		return false; // not a kept picture
	const s64 second = pcsx2gs::FrameClockPerSecond();
	const int slip = static_cast<int>(std::llround(static_cast<double>(shown_time - shown.planned) / static_cast<double>(m_kept_period)));
	m_kept_slip = slip;
	if (m_kept_fix_from > 0 && now - m_kept_fix_time >= second / 10)
	{
		// judged a tenth of a second on. Three in a row without effect: this screen's own way (one alone may be a
		// present that missed its turn again right after)
		if (slip < m_kept_fix_from)
			m_kept_fix_fails = 0;
		else if (++m_kept_fix_fails >= 3)
		{
			m_kept_slip_floor = m_kept_fix_from;
			m_kept_floor_until = now + 30 * second;
			m_kept_fix_fails = 0;
		}
		m_kept_fix_from = 0;
	}
	if (m_kept_slip_floor > 0 && now >= m_kept_floor_until)
		m_kept_slip_floor = 0;
	if (slip <= 0 || slip <= m_kept_slip_floor)
	{
		m_kept_slip_since = 0;
		return false;
	}
	if (m_kept_slip_since == 0)
		m_kept_slip_since = now;
	if (now - m_kept_slip_since < second / 4 || m_kept_fix_from > 0 || (m_kept_fix_time != 0 && now - m_kept_fix_time < second / 2))
		return false;
	m_kept_fix_time = now;
	m_kept_fix_from = slip;
	m_kept_slip_since = 0;
	++m_kept_fixes;
	if (hold >= 2)
	{
		hold -= 1;
		return false;
	}
	return true;
}

s64 GSRenderer::PresentPoll()
{
	RingSaveIfAsked();
	const s64 now = pcsx2gs::FrameClock();
	const s64 second = pcsx2gs::FrameClockPerSecond();
	// the screen, asked four times a second: its refresh period, and whether presents carry holds (a real screen)
	if (now >= m_kept_grid_until)
	{
		s64 phase = 0;
		if (!pcsx2gs::ScreenGrid(m_kept_period, phase, m_kept_holds))
		{
			m_kept_period = 0;
			m_kept_holds = false;
		}
		m_kept_grid_until = now + second / 4;
	}
	// STAR 2026-11-24. With holds a picture may be handed over before its time: Windows keeps the one before it on
	// screen for its hold whatever comes, so the row is the same, and a present that would have been a little late for
	// its refresh is in time instead (the row slips less often: KeptSlipFix). Up to 0.6 of a refresh early.
	// PS2X_PCSX2_KEPT_EARLY=0: off.
	static const bool s_early = [] {
		const char* value = std::getenv("PS2X_PCSX2_KEPT_EARLY");
		return !(value && *value == '0');
	}();
	// A picture that comes this far past its time is not shown (PS2X_PCSX2_KEPT_OVERDUE=0: shown all the same). After
	// a stop of this thread (a shader compile) the GIF data waiting for it holds pictures that are all past their
	// times; each present waits for its turn on the screen, so showing them all kept this thread exactly as far
	// behind as it was: the boot logos ran 0.2 s late for five seconds (tools/pacedelay.py, a row at +30 refreshes).
	// Not while PS2X_PCSX2_PRESENT_DUMP writes the presented pictures to files: that makes every one of them late.
	static const bool s_overdue = [] {
		const char* value = std::getenv("PS2X_PCSX2_KEPT_OVERDUE");
		const char* dump = std::getenv("PS2X_PCSX2_PRESENT_DUMP");
		return !(value && *value == '0') && !(dump && *dump);
	}();
	const auto early_of = [&](const KeptEntry& entry, int hold) -> s64 {
		return (s_early && !entry.paced && m_kept_holds && hold > 0 && m_kept_period > 0) ? m_kept_period * 6 / 10 : 0;
	};
	const auto hold_of = [](const KeptEntry& entry) {
		return entry.hold >= 0 ? entry.hold : pcsx2gs::g_keptHold.load(std::memory_order_relaxed);
	};
	while (!m_kept_queue.empty())
	{
		const KeptEntry entry = m_kept_queue.front();
		int hold = hold_of(entry);
		const s64 early = early_of(entry, hold);
		if (entry.time - early > now)
			break;
		m_kept_queue.pop_front();
		// Late by so much that the next one is due as well: this one is left out (the screen never saw its turn).
		if (!m_kept_queue.empty() && m_kept_queue.front().time <= now)
		{
			++m_kept_dropped;
			ReleaseKept(entry.picture);
			continue;
		}
		// Its own time on screen has passed (its hold, and 3 ms): left out, so that this thread catches up.
		const s64 own = (m_kept_period > 0 ? m_kept_period : second / 60) * std::max(hold, 1);
		if (s_overdue && now - entry.time > own + second / 333)
		{
			++m_kept_overdue;
			ReleaseKept(entry.picture);
			continue;
		}
		if (now - entry.time > second / 500)
			++m_kept_late; // more than 2 ms after its time
		if (early > 0 && KeptSlipFix(now, hold))
		{
			ReleaseKept(entry.picture); // left out to bring the row back to its plan
			continue;
		}
		PresentKept(entry, hold);
		ReleaseKept(entry.picture);
		static const bool s_log = std::getenv("PS2X_FRAME_DOUBLE_LOG") != nullptr;
		if (s_log && m_kept_presented % 600 == 0)
		{
			std::fprintf(stderr, "[fd] %llu pictures presented, %llu over 2 ms late, %llu left out, %llu past their time, %zu waiting; "
				"the row %+d refreshes from its plan, pulled back %llu times (left alone at +%d)\n",
				static_cast<unsigned long long>(m_kept_presented), static_cast<unsigned long long>(m_kept_late),
				static_cast<unsigned long long>(m_kept_dropped), static_cast<unsigned long long>(m_kept_overdue), m_kept_queue.size(),
				m_kept_slip, static_cast<unsigned long long>(m_kept_fixes), m_kept_slip_floor);
			std::fflush(stderr);
		}
		break; // one present a call: the next one has its own time
	}
	if (m_kept_queue.empty())
		return -1;
	const KeptEntry& next = m_kept_queue.front();
	const s64 wait = next.time - early_of(next, hold_of(next)) - pcsx2gs::FrameClock();
	return wait <= 0 ? 0 : wait * 1000000 / second;
}

void GSRenderer::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// the log: V = this thread's handling of a vsync, from `now` to `time`
	struct VSyncSpan
	{
		s64 from = pcsx2gs::FrameClock();
		~VSyncSpan()
		{
			if (std::FILE* const log = PaceLogFile())
				std::fprintf(log, "V,%lld,0,0,%lld,0,0,0\n", static_cast<long long>(from), static_cast<long long>(pcsx2gs::FrameClock()));
		}
	} vsync_span;
	m_kept_field = field;
	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		if (GSConfig.SaveInfo)
		{
			DumpGSPrivRegs(*m_regs, GetDrawDumpPath("%05lld_f%05lld_vsync_gs_reg.txt", s_n, g_perfmon.GetFrame()));

			DumpDrawInfo(false, false, true);
		}

		if (GSConfig.SaveTransferImages)
			DumpTransferImages();

		if (GSConfig.SaveFrameStats)
		{
			m_perfmon_frame = g_perfmon - m_perfmon_frame;
			m_perfmon_frame.Dump(GetDrawDumpPath("%05lld_f%05lld_frame_stats.txt", s_n, g_perfmon.GetFrame()), GSIsHardwareRenderer());
			m_perfmon_frame = g_perfmon;
		}
	}

	const int fb_sprite_blits = g_perfmon.GetDisplayFramebufferSpriteBlits();
	const bool fb_sprite_frame = (fb_sprite_blits > 0);

	bool skip_frame = false;
	if (GSConfig.SkipDuplicateFrames && !GSCapture::IsCapturingVideo())
	{
		bool is_unique_frame;
		switch (PerformanceMetrics::GetInternalFPSMethod())
		{
		case PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister:
			is_unique_frame = registers_written;
			break;
		case PerformanceMetrics::InternalFPSMethod::DISPFBBlit:
			is_unique_frame = fb_sprite_frame;
			break;
		default:
			is_unique_frame = true;
			break;
		}

		if (!is_unique_frame && m_skipped_duplicate_frames < MAX_SKIPPED_DUPLICATE_FRAMES)
		{
			m_skipped_duplicate_frames++;
			skip_frame = true;
		}
		else
		{
			m_skipped_duplicate_frames = 0;
		}
	}

	const bool blank_frame = !Merge(field);

	m_last_draw_n = s_n;
	m_last_transfer_n = s_transfer_n;

	// Skip presentation when running uncapped while vsync is on.
	if (skip_frame || g_gs_device->ShouldSkipPresentingFrame())
	{
		if (BeginPresentFrame(true))
			EndPresentFrame();

		PerformanceMetrics::Update(registers_written, fb_sprite_frame, skip_frame);
	}
	else
	{
		if (!idle_frame)
			g_gs_device->AgePool();

		g_perfmon.EndFrame(idle_frame);

		if ((g_perfmon.GetFrame() & 0x1f) == 0)
			g_perfmon.Update();

		// Little bit ugly, but we can't do CAS inside the render pass.
		GSVector4i src_rect;
		GSVector4 src_uv, draw_rect;
		GSTexture* current = g_gs_device->GetCurrent();
		if (current && !blank_frame)
		{
			src_rect = CalculateDrawSrcRect(current, m_real_size);
			src_uv = GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy();
			draw_rect = CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P);
			s_last_draw_rect = draw_rect;
			// pcsx2gs (Kamite): the overlay lines (compat/HostStubs.cpp) are mapped onto this rectangle. Their
			// coordinates are native GS pixels, so the frame's size goes in native pixels too: m_real_size is at the
			// internal resolution (x the upscale), which drew the hitboxes a third of the way to the top-left at 3x.
			{
				const float upscale = std::max(GetUpscaleMultiplier(), 0.01f);
				pcsx2gs::SetPresentRect(draw_rect.x, draw_rect.y, draw_rect.z, draw_rect.w,
					static_cast<float>(m_real_size.x) / upscale, static_cast<float>(m_real_size.y) / upscale);
			}

			if (GSConfig.CASMode != GSCASMode::Disabled)
			{
				static bool cas_log_once = false;
				if (g_gs_device->Features().cas_sharpening)
				{
					// sharpen only if the IR is higher than the display resolution
					const bool sharpen_only = (GSConfig.CASMode == GSCASMode::SharpenOnly ||
					                           (current->GetWidth() > g_gs_device->GetWindowWidth() &&
					                            current->GetHeight() > g_gs_device->GetWindowHeight()));
					g_gs_device->CAS(current, src_rect, src_uv, draw_rect, sharpen_only);
				}
				else if (!cas_log_once)
				{
					Host::AddIconOSDMessage("CASUnsupported", ICON_FA_TRIANGLE_EXCLAMATION,
						TRANSLATE_SV("GS", "CAS is not available, your graphics driver does not support the required functionality."),
						10.0f);
					cas_log_once = true;
				}
			}
		}

		// pcsx2gs (STAR): a picture larger than it is shown is shrunk by averaging first (ShrinkForPresent).
		// pcsx2gs (STAR): while kept pictures come (PresentPoll shows them), this vsync presents nothing itself.
		// pcsx2gs (STAR 2026-11-24): also nothing while one of the host's extra passes is drawing into the frame
		// buffer (between its markers 3 and 4). Since the field comes after the game's display swap, the displayed
		// buffer is the one the passes draw into; with no kept picture for a quarter of a second (a pause of the
		// game, a pass that took long) this vsync would have shown that buffer half drawn: a cleared screen with
		// the first big triangles of the scene, for one refresh. A line in the log each of the first 20 times.
		const bool in_passes = KeptDrawing();
		if (in_passes && !KeptPicturesActive())
		{
			static int s_seen = 0;
			if (s_seen < 20)
			{
				std::fprintf(stderr, "[fd] a vsync came while an extra pass was drawing and no kept picture was active: its picture is not shown (%d)\n", ++s_seen);
				std::fflush(stderr);
			}
		}
		const bool kept_active = KeptPicturesActive() || in_passes;
		GSTexture* const shrunk = (current && !blank_frame && !kept_active) ? ShrinkForPresent(current, src_rect, draw_rect) : nullptr;
		if (current && !blank_frame && !kept_active)
			DumpPresented(shrunk, current, src_uv, draw_rect, "");

		// pcsx2gs (STAR): this vsync's own picture goes to a refresh of the screen picked for it (PaceOrdinary), through
		// the kept pictures' queue; presented here and now only without sync, without a known screen, or when blank.
		bool queued = false;
		if (!kept_active)
		{
			s64 pace_time = 0;
			int pace_hold = 0;
			const int pace = (current && !blank_frame) ? PaceOrdinary(pace_time, pace_hold) : 0;
			if (pace == 1)
			{
				queued = shrunk ? QueueKept(shrunk, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), shrunk->GetWidth(), shrunk->GetHeight(), draw_rect, pace_time, 0, pace_hold, true)
				                : QueueKept(current, src_uv, src_rect.width(), src_rect.height(), draw_rect, pace_time, 0, pace_hold, true);
			}
			else if (pace == 2)
				queued = true; // left out: the picture before it has this refresh
			if (!queued)
			{
				DropPaced(); // nothing older may come after a picture presented at once
				pcsx2gs::g_presentHold.store(0, std::memory_order_relaxed); // an ordinary present: the newest picture wins
			}
		}
		if (!kept_active && !queued && current && !blank_frame)
			RingNote(shrunk ? shrunk : current, shrunk ? GSVector4(0.0f, 0.0f, 1.0f, 1.0f) : src_uv, 0, 'v');
		if (!kept_active && !queued && BeginPresentFrame(false))
		{
			if (current && !blank_frame)
			{
				const u64 current_time = Common::Timer::GetCurrentValue();
				const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

				g_gs_device->PresentRect(shrunk ? shrunk : current, shrunk ? GSVector4(0.0f, 0.0f, 1.0f, 1.0f) : src_uv, nullptr, draw_rect,
					s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
			}

			EndPresentFrame();

			const float gpu_time = g_gs_device->GetAndResetAccumulatedGPUTime();
			GPUPipelineStatistics gpu_stats = g_gs_device->GetAndResetAccumulatedGPUPipelineStatistics();
			PerformanceMetrics::OnGPUPresent(gpu_time, gpu_stats.vs_invocations, gpu_stats.ps_invocations);
		}

		PerformanceMetrics::Update(registers_written, fb_sprite_frame, false);
	}

	// snapshot
	if (!m_snapshot.empty())
	{
		u32 screenshot_width, screenshot_height;
		std::vector<u32> screenshot_pixels;

		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp)
		{
			const GSTexture* current = g_gs_device->GetCurrent();
			const GSVector2i internal_res = GetInternalResolution();

			if (current && (current->GetWidth() > internal_res.x || current->GetHeight() > internal_res.y))
				g_gs_device->Resize(internal_res.x, internal_res.y);
		}

		if (!m_dump && m_dump_frames > 0)
		{
			if (GSConfig.UserHacks_ReadTCOnClose)
				ReadbackTextureCache();

			freezeData fd = {0, nullptr};
			Freeze(&fd, true);
			fd.data = new u8[fd.size];
			Freeze(&fd, false);

			// keep the screenshot relatively small so we don't bloat the dump
			static constexpr u32 DUMP_SCREENSHOT_WIDTH = 640;
			static constexpr u32 DUMP_SCREENSHOT_HEIGHT = 480;
			SaveSnapshotToMemory(DUMP_SCREENSHOT_WIDTH, DUMP_SCREENSHOT_HEIGHT, true, false,
				&screenshot_width, &screenshot_height, &screenshot_pixels);

			std::string_view compression_str;
			if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::Uncompressed)
			{
				m_dump = GSDumpBase::CreateUncompressedDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with no compression");
			}
			else if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::LZMA)
			{
				m_dump = GSDumpBase::CreateXzDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with LZMA compression");
			}
			else
			{
				m_dump = GSDumpBase::CreateZstDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with Zstandard compression");
			}

			delete[] fd.data;

			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saving {0} GS dump {1} to '{2}'"),
					(m_dump_frames == 1) ? TRANSLATE_SV("GS", "single frame") : TRANSLATE_SV("GS", "multi-frame"), compression_str,
					Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
		}

		const bool internal_resolution = (GSConfig.ScreenshotSize >= GSScreenshotSize::InternalResolution);
		const bool aspect_correct = (GSConfig.ScreenshotSize != GSScreenshotSize::InternalResolutionUncorrected);

		if (g_gs_device->GetCurrent() && SaveSnapshotToMemory(
			internal_resolution ? 0 : g_gs_device->GetWindowWidth(),
			internal_resolution ? 0 : g_gs_device->GetWindowHeight(),
			aspect_correct, true,
			&screenshot_width, &screenshot_height, &screenshot_pixels))
		{
			CompressAndWriteScreenshot(fmt::format("{}.{}", m_snapshot, GetScreenshotSuffix()),
				screenshot_width, screenshot_height, std::move(screenshot_pixels));
		}
		else
		{
			Host::AddIconOSDMessage("GSScreenshot", ICON_FA_CAMERA,
				TRANSLATE_SV("GS", "Failed to render/download screenshot."), Host::OSD_ERROR_DURATION);
		}

		m_snapshot = {};
	}
	else if (m_dump)
	{
		const bool last = (m_dump_frames == 0);
		if (m_dump->VSync(field, last, m_regs))
		{
			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saved GS dump to '{}'."), Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
			m_dump.reset();
		}
		else if (!last)
		{
			m_dump_frames--;
		}
	}

	// metrics
	if (m_saving_metrics && !PerformanceMetrics::IsSavingMetrics())
		DumpSavedMetrics();

	// capture
	if (GSCapture::IsCapturingVideo())
	{
		const GSVector2i size = GSCapture::GetSize();
		if (GSTexture* current = g_gs_device->GetCurrent())
		{
			// TODO: Maybe avoid this copy in the future? We can use swscale to fix it up on the dumping thread..
			if (current->GetSize() != size)
			{
				GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, false);
				if (temp)
				{
					g_gs_device->StretchRect(current, temp, GSVector4(0, 0, size.x, size.y), ShaderConvert::COPY, Biln);
					GSCapture::DeliverVideoFrame(temp);
					g_gs_device->Recycle(temp);
				}
			}
			else
			{
				GSCapture::DeliverVideoFrame(current);
			}
		}
		else
		{
			// Bit janky, but unless we want to make variable frame rate files, we need to deliver *a* frame to
			// the video file, so just grab a blank RT.
			GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, true);
			if (temp)
			{
				GSCapture::DeliverVideoFrame(temp);
				g_gs_device->Recycle(temp);
			}
		}
	}

	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()) && GSConfig.SaveTransferImages)
		DumpTransferImages();
}

void GSRenderer::QueueSnapshot(const std::string& path, const u32 gsdump_frames)
{
	if (!m_snapshot.empty())
		return;

	// Allows for providing a complete path
	if (path.size() > 4 && StringUtil::EndsWithNoCase(path, ".png"))
		m_snapshot = path.substr(0, path.size() - 4);
	else
		m_snapshot = GSGetBaseSnapshotFilename();

	// this is really gross, but wx we get the snapshot request after shift...
	m_dump_frames = gsdump_frames;
}

static std::string GSGetBaseFilename()
{
	std::string filename;

	// append the game serial and title
	if (std::string name(VMManager::GetTitle(true)); !name.empty())
	{
		Path::SanitizeFileName(&name);
		if (name.length() > 219)
			name.resize(219);
		filename += name;
	}
	if (std::string serial = VMManager::GetDiscSerial(); !serial.empty())
	{
		Path::SanitizeFileName(&serial);
		filename += '_';
		filename += serial;
	}

	const time_t cur_time = time(nullptr);
	char local_time[16];

	if (strftime(local_time, sizeof(local_time), "%Y%m%d%H%M%S", localtime(&cur_time)))
	{
		static time_t prev_snap;
		// The variable 'n' is used for labelling the screenshots when multiple screenshots are taken in
		// a single second, we'll start using this variable for naming when a second screenshot request is detected
		// at the same time as the first one. Hence, we're initially setting this counter to 2 to imply that
		// the captured image is the 2nd image captured at this specific time.
		static int n = 2;

		filename += '_';

		if (cur_time == prev_snap)
			filename += fmt::format("{0}_({1})", local_time, n++);
		else
		{
			n = 2;
			filename += fmt::format("{}", local_time);
		}
		prev_snap = cur_time;
	}

	return filename;
}

std::string GSGetBaseSnapshotFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeSnapshotsByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Snapshots, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}

	return Path::Combine(EmuFolders::Snapshots, GSGetBaseFilename());
}

std::string GSGetBaseVideoFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeVideoCaptureByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Videos, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}
	// prepend video directory
	return Path::Combine(EmuFolders::Videos, GSGetBaseFilename());
}

void GSRenderer::StopGSDump()
{
	m_snapshot = {};
	m_dump_frames = 0;
}

void GSRenderer::StartSavingMetrics(u32 seconds)
{
	m_saving_metrics = true;
	PerformanceMetrics::StartSavingMetrics(seconds);
	Host::AddKeyedOSDMessage("GSMetrics",
		fmt::format(TRANSLATE_FS("GS", "Started saving performance metrics{}"),
			(0 < seconds && seconds < UINT32_MAX) ? fmt::format(" ({} seconds)", seconds) : ""),
		Host::OSD_INFO_DURATION);
}

void GSRenderer::DumpSavedMetrics()
{
	m_saving_metrics = false;
	PerformanceMetrics::DumpSavedMetrics();
	Host::AddKeyedOSDMessage("GSMetrics", fmt::format(TRANSLATE_FS("GS", "Logging saved performance metrics")),
		Host::OSD_INFO_DURATION);
}

bool GSRenderer::IsSavingMetrics()
{
	return m_saving_metrics;
}

void GSRenderer::PresentCurrentFrame()
{
	if (BeginPresentFrame(false))
	{
		GSTexture* current = g_gs_device->GetCurrent();
		if (current)
		{
			const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
			const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());
			const GSVector4 draw_rect(CalculateDrawDstRect(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight(),
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P));
			s_last_draw_rect = draw_rect;

			const u64 current_time = Common::Timer::GetCurrentValue();
			const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

			g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
				s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
		}

		EndPresentFrame();
	}
}

void GSTranslateWindowToDisplayCoordinates(float window_x, float window_y, float* display_x, float* display_y)
{
	const float draw_width = s_last_draw_rect.z - s_last_draw_rect.x;
	const float draw_height = s_last_draw_rect.w - s_last_draw_rect.y;
	const float rel_x = window_x - s_last_draw_rect.x;
	const float rel_y = window_y - s_last_draw_rect.y;
	if (rel_x < 0 || rel_x > draw_width || rel_y < 0 || rel_y > draw_height)
	{
		*display_x = -1.0f;
		*display_y = -1.0f;
		return;
	}

	*display_x = rel_x / draw_width;
	*display_y = rel_y / draw_height;
}

void GSSetDisplayAlignment(GSDisplayAlignment alignment)
{
	s_display_alignment = alignment;
}

bool GSRenderer::BeginCapture(std::string filename, const GSVector2i& size)
{
	const GSVector2i capture_resolution = (size.x != 0 && size.y != 0) ?
											  size :
											  (GSConfig.VideoCaptureAutoResolution ?
													  GetInternalResolution() :
													  GSVector2i(GSConfig.VideoCaptureWidth, GSConfig.VideoCaptureHeight));

	return GSCapture::BeginCapture(GetTvRefreshRate(), capture_resolution,
		GetCurrentAspectRatioFloat(GetVideoMode() == GSVideoMode::SDTV_480P),
		std::move(filename));
}

void GSRenderer::EndCapture()
{
	GSCapture::EndCapture();
}

GSTexture* GSRenderer::LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size)
{
	return nullptr;
}

bool GSRenderer::IsIdleFrame() const
{
	return (m_last_draw_n == s_n && m_last_transfer_n == s_transfer_n);
}

bool GSRenderer::SaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
	u32* width, u32* height, std::vector<u32>* pixels)
{
	GSTexture* const current = g_gs_device->GetCurrent();
	if (!current)
	{
		*width = 0;
		*height = 0;
		pixels->clear();
		return false;
	}

	const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
	const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());

	const bool is_progressive = (GetVideoMode() == GSVideoMode::SDTV_480P);
	GSVector4 draw_rect;
	if (window_width == 0 || window_height == 0)
	{
		if (apply_aspect)
		{
			// use internal resolution of the texture
			const float aspect = GetCurrentAspectRatioFloat(is_progressive);
			const int tex_width = current->GetWidth();
			const int tex_height = current->GetHeight();

			// expand to the larger dimension
			const float tex_aspect = static_cast<float>(tex_width) / static_cast<float>(tex_height);
			if (tex_aspect >= aspect)
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_width), static_cast<float>(tex_width) / aspect);
			else
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_height) * aspect, static_cast<float>(tex_height));
		}
		else
		{
			// uncorrected aspect is only available at internal resolution
			draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(current->GetWidth()), static_cast<float>(current->GetHeight()));
		}
	}
	else
	{
		draw_rect = CalculateDrawDstRect(window_width, window_height, src_rect, current->GetSize(),
			GSDisplayAlignment::LeftOrTop, false, is_progressive);
	}
	const u32 draw_width = static_cast<u32>(draw_rect.z - draw_rect.x);
	const u32 draw_height = static_cast<u32>(draw_rect.w - draw_rect.y);
	const u32 image_width = crop_borders ? draw_width : std::max(draw_width, window_width);
	const u32 image_height = crop_borders ? draw_height : std::max(draw_height, window_height);

	// We're not expecting screenshots to be fast, so just allocate a download texture on demand.
	GSTexture* rt = g_gs_device->CreateRenderTarget(draw_width, draw_height, GSTexture::Format::Color, false);
	if (rt)
	{
		std::unique_ptr<GSDownloadTexture> dl(g_gs_device->CreateDownloadTexture(draw_width, draw_height, GSTexture::Format::Color));
		if (dl)
		{
			const GSVector4i rc(0, 0, draw_width, draw_height);
			g_gs_device->StretchRect(current, src_uv, rt, GSVector4(rc), ShaderConvert::TRANSPARENCY_FILTER, Biln);
			dl->CopyFromTexture(rc, rt, rc, 0);
			dl->Flush();

			if (dl->Map(rc))
			{
				const u32 pad_x = (image_width - draw_width) / 2;
				const u32 pad_y = (image_height - draw_height) / 2;
				pixels->clear();
				pixels->resize(image_width * image_height, 0);
				*width = image_width;
				*height = image_height;
				StringUtil::StrideMemCpy(pixels->data() + pad_y * image_width + pad_x, image_width * sizeof(u32), dl->GetMapPointer(),
					dl->GetMapPitch(), draw_width * sizeof(u32), draw_height);

				g_gs_device->Recycle(rt);
				return true;
			}
		}

		g_gs_device->Recycle(rt);
	}

	*width = 0;
	*height = 0;
	pixels->clear();
	return false;
}

void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename)
{
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wt");
	if (!fp)
		return;

	for (int i = 0; i < 2; i++)
	{
		if (i == 0 && !r.PMODE.EN1)
			continue;
		if (i == 1 && !r.PMODE.EN2)
			continue;

		std::fprintf(fp.get(), "DISPFB%d: { BP: 0x%05x, BW: %u, PSM: %u, DBX: %u, DBY: %u }\n",
			i,
			r.DISP[i].DISPFB.Block(),
			r.DISP[i].DISPFB.FBW,
			r.DISP[i].DISPFB.PSM,
			r.DISP[i].DISPFB.DBX,
			r.DISP[i].DISPFB.DBY);

		std::fprintf(fp.get(), "DISPLAY%d: { DX: %u, DY: %u, DW: %u, DH: %u, MAGH: %u, MAGV: %u }\n",
			i,
			r.DISP[i].DISPLAY.DX,
			r.DISP[i].DISPLAY.DY,
			r.DISP[i].DISPLAY.DW,
			r.DISP[i].DISPLAY.DH,
			r.DISP[i].DISPLAY.MAGH,
			r.DISP[i].DISPLAY.MAGV);
	}

	std::fprintf(fp.get(), "PMODE: { EN1: %u, EN2: %u, CRTMD: %u, MMOD: %u, AMOD: %u, SLBG: %u, ALP: %u }\n",
		r.PMODE.EN1,
		r.PMODE.EN2,
		r.PMODE.CRTMD,
		r.PMODE.MMOD,
		r.PMODE.AMOD,
		r.PMODE.SLBG,
		r.PMODE.ALP);

	std::fprintf(fp.get(),
		"SMODE1: { CLKSEL: %u, CMOD: %u, EX: %u, GCONT: %u, LC: %u, NVCK: %u, PCK2: %u, PEHS: %u, PEVS: %u, PHS: %u, PRST: %u, PVS: %u, RC: %u, SINT: %u, SLCK: %u, SLCK2: %u, SPML: %u, T1248: %u, VCKSEL: %u, VHP: %u, XPCK: %u }\n",
		r.SMODE1.CLKSEL,
		r.SMODE1.CMOD,
		r.SMODE1.EX,
		r.SMODE1.GCONT,
		r.SMODE1.LC,
		r.SMODE1.NVCK,
		r.SMODE1.PCK2,
		r.SMODE1.PEHS,
		r.SMODE1.PEVS,
		r.SMODE1.PHS,
		r.SMODE1.PRST,
		r.SMODE1.PVS,
		r.SMODE1.RC,
		r.SMODE1.SINT,
		r.SMODE1.SLCK,
		r.SMODE1.SLCK2,
		r.SMODE1.SPML,
		r.SMODE1.T1248,
		r.SMODE1.VCKSEL,
		r.SMODE1.VHP,
		r.SMODE1.XPCK);

	std::fprintf(fp.get(), "SMODE2: { INT: %u, FFMD: %u, DPMS: %u }\n",
		r.SMODE2.INT,
		r.SMODE2.FFMD,
		r.SMODE2.DPMS);

	std::fprintf(fp.get(), "SRFSH: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SRFSH.U32[0],
		r.SRFSH.U32[1]);

	std::fprintf(fp.get(), "SYNCH1: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH1.U32[0],
		r.SYNCH1.U32[1]);

	std::fprintf(fp.get(), "SYNCH2: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH2.U32[0],
		r.SYNCH2.U32[1]);

	std::fprintf(fp.get(), "SYNCV: { VBP: %u, VBPE: %u, VDP: %u, VFP: %u, VFPE: %u, VS: %u }\n",
		r.SYNCV.VBP,
		r.SYNCV.VBPE,
		r.SYNCV.VDP,
		r.SYNCV.VFP,
		r.SYNCV.VFPE,
		r.SYNCV.VS);

	std::fprintf(fp.get(), "CSR: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.CSR.U32[0],
		r.CSR.U32[1]);

	std::fprintf(fp.get(), "BGCOLOR: { B: %u, G: %u, R: %u }\n",
		r.BGCOLOR.B,
		r.BGCOLOR.G,
		r.BGCOLOR.R);

	std::fprintf(fp.get(), "EXTBUF: { BP: 0x%05x, BW: %u, FBIN: %u, WFFMD: %u, EMODA: %u, EMODC: %u, WDX: %u, WDY: %u }\n",
		r.EXTBUF.EXBP, r.EXTBUF.EXBW, r.EXTBUF.FBIN, r.EXTBUF.WFFMD,
		r.EXTBUF.EMODA, r.EXTBUF.EMODC, r.EXTBUF.WDX, r.EXTBUF.WDY);

	std::fprintf(fp.get(), "EXTDATA: { SX: %u, SY: %u, SMPH: %u, SMPV: %u, WW: %u, WH: %u }\n",
		r.EXTDATA.SX, r.EXTDATA.SY, r.EXTDATA.SMPH, r.EXTDATA.SMPV, r.EXTDATA.WW, r.EXTDATA.WH);

	std::fprintf(fp.get(), "EXTWRITE: { EN: %u }\n", r.EXTWRITE.WRITE);
}
