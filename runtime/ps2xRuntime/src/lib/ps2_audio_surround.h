#pragma once

// The surround output for Dolby Pro Logic II (issue #18, runtime/prologic_decoder.h): the game's stereo, decoded to the
// output device's own speakers. raylib's audio device is fixed at stereo (raudio.c's AUDIO_DEVICE_CHANNELS), so this
// opens a second device of its own through the miniaudio built into raylib, at the device's own channel count, and
// only while it is used (ps2_audio.cpp: the game in Pro Logic II mode). Stereo play is untouched.

#include <cstdint>
#include <memory>
#include <string>

namespace ps2x::audio_surround
{
    // Fills `frames` interleaved stereo frames (the game's sound queue: ps2_audio.cpp's audioCallback).
    using Source = void (*)(int16_t *stereo, unsigned frames);

    class Output
    {
    public:
        Output();
        ~Output();
        // Opens the default output device at its own channel count and starts it. False, closed again, when it has
        // fewer than 4 channels (headphones, a TV, a receiver on optical: they get the stereo, a receiver decodes it
        // itself) or none opens. `about` says what was found, for the log.
        bool open(Source source, std::string &about);
        void close();
        bool isOpen() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
