#include "ps2_audio_surround.h"

#if defined(PS2X_HAS_MINIAUDIO) && !defined(PLATFORM_VITA)

#include "runtime/prologic_decoder.h"
#include "runtime/ps2_time_control.h"

#include <algorithm>
#include <vector>

// raylib compiles miniaudio's implementation into raudio.c; this is only its header, with raudio.c's own MA_NO_*
// switches (they change ma_device's layout, so they must match).
#define MA_NO_JACK
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#include "miniaudio.h"

namespace ps2x::audio_surround
{
    struct Output::Impl
    {
        ma_device device{};
        bool open = false;
        Source source = nullptr;
        std::unique_ptr<ProLogicDecoder> decoder;
        std::vector<int16_t> stereo;

        static void callback(ma_device *device, void *output, const void *, ma_uint32 frames)
        {
            auto *self = static_cast<Impl *>(device->pUserData);
            if (self->stereo.size() < static_cast<size_t>(frames) * 2u)
                self->stereo.resize(static_cast<size_t>(frames) * 2u);
            self->source(self->stereo.data(), frames);
            auto *out = static_cast<float *>(output);
            self->decoder->process(self->stereo.data(), out, frames);
            if (ps2x::timecontrol::muted()) // the F2 menu's mute and hidden runs (PS2X_AUDIO_MUTE)
                std::fill_n(out, static_cast<size_t>(frames) * self->decoder->channels(), 0.0f);
        }
    };

    namespace
    {
        // The device's channels as speakers round the listener (ITU positions; a single rear pair at +-110, side and
        // back pairs at +-90 and +-150).
        std::vector<ProLogicDecoder::Speaker> speakersOf(const ma_channel *map, ma_uint32 channels, std::string &names)
        {
            using Kind = ProLogicDecoder::Speaker::Kind;
            bool side = false, back = false;
            for (ma_uint32 c = 0u; c < channels; ++c)
            {
                side = side || map[c] == MA_CHANNEL_SIDE_LEFT || map[c] == MA_CHANNEL_SIDE_RIGHT;
                back = back || map[c] == MA_CHANNEL_BACK_LEFT || map[c] == MA_CHANNEL_BACK_RIGHT;
            }
            const float sideAz = back ? 90.0f : 110.0f, backAz = side ? 150.0f : 110.0f;
            std::vector<ProLogicDecoder::Speaker> speakers(channels);
            for (ma_uint32 c = 0u; c < channels; ++c)
            {
                ProLogicDecoder::Speaker &s = speakers[c];
                const char *name = "-";
                switch (map[c])
                {
                case MA_CHANNEL_FRONT_LEFT: s = {Kind::Directional, -30.0f}, name = "FL"; break;
                case MA_CHANNEL_FRONT_RIGHT: s = {Kind::Directional, 30.0f}, name = "FR"; break;
                case MA_CHANNEL_FRONT_CENTER: s = {Kind::Directional, 0.0f}, name = "C"; break;
                case MA_CHANNEL_LFE: s = {Kind::Lfe, 0.0f}, name = "LFE"; break;
                case MA_CHANNEL_FRONT_LEFT_CENTER: s = {Kind::Directional, -15.0f}, name = "FLC"; break;
                case MA_CHANNEL_FRONT_RIGHT_CENTER: s = {Kind::Directional, 15.0f}, name = "FRC"; break;
                case MA_CHANNEL_SIDE_LEFT: s = {Kind::Directional, -sideAz}, name = "SL"; break;
                case MA_CHANNEL_SIDE_RIGHT: s = {Kind::Directional, sideAz}, name = "SR"; break;
                case MA_CHANNEL_BACK_LEFT: s = {Kind::Directional, -backAz}, name = "BL"; break;
                case MA_CHANNEL_BACK_RIGHT: s = {Kind::Directional, backAz}, name = "BR"; break;
                case MA_CHANNEL_BACK_CENTER: s = {Kind::Directional, 180.0f}, name = "BC"; break;
                default: s = {Kind::None, 0.0f}; break;
                }
                names += (c ? " " : "");
                names += name;
            }
            return speakers;
        }
    }

    Output::Output() : m_impl(std::make_unique<Impl>()) {}
    Output::~Output() { close(); }
    bool Output::isOpen() const { return m_impl->open; }

    bool Output::open(Source source, std::string &about)
    {
        close();
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 0; // the device's own
        config.sampleRate = 48000;
        config.dataCallback = &Impl::callback;
        config.pUserData = m_impl.get();
        if (ma_device_init(nullptr, &config, &m_impl->device) != MA_SUCCESS)
        {
            about = "no output device opened";
            return false;
        }
        const ma_uint32 channels = m_impl->device.playback.channels;
        if (channels < 4u)
        {
            ma_device_uninit(&m_impl->device);
            about = "the output device has " + std::to_string(channels) + " channel" + (channels == 1u ? "" : "s");
            return false;
        }
        std::string names;
        m_impl->decoder = std::make_unique<ProLogicDecoder>(speakersOf(m_impl->device.playback.channelMap, channels, names));
        m_impl->source = source;
        m_impl->stereo.assign(4096u * 2u, 0);
        if (ma_device_start(&m_impl->device) != MA_SUCCESS)
        {
            ma_device_uninit(&m_impl->device);
            m_impl->decoder.reset();
            about = "the output device would not start";
            return false;
        }
        m_impl->open = true;
        about = std::to_string(channels) + " channels (" + names + ")";
        return true;
    }

    void Output::close()
    {
        if (!m_impl->open)
            return;
        ma_device_uninit(&m_impl->device); // stops it: the callback is done when this returns
        m_impl->open = false;
        m_impl->decoder.reset();
    }
}

#else // no miniaudio (the Vita's prebuilt raylib): stereo only

namespace ps2x::audio_surround
{
    struct Output::Impl
    {
    };
    Output::Output() : m_impl(std::make_unique<Impl>()) {}
    Output::~Output() = default;
    bool Output::isOpen() const { return false; }
    bool Output::open(Source, std::string &about)
    {
        about = "not in this build";
        return false;
    }
    void Output::close() {}
}

#endif
