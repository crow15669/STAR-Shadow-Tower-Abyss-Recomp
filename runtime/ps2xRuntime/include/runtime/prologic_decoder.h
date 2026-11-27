#pragma once

// Dolby Pro Logic II decoding for surround speakers (issue #18). God Hand's "Dolby Pro Logic II" sound mode puts sounds
// behind the camera into the stereo mix out of phase (the SPU2's negative voice volumes): a receiver's decoder steers
// them to the rear speakers. A PC with a 4.0 / 5.1 / 7.1 sound card has no decoder: the card was handed stereo, and its
// driver at most copies the front pair to the rear (the report). PCSX2 decodes in software (its audio expansion);
// GHR now does too, whenever the game is in Pro Logic II mode and the output device has 4 or more channels
// (ps2_audio_surround.cpp opens it).
//
// Our own decoder (no code from elsewhere): a short-time Fourier transform (1024-frame sqrt-Hann windows, 512-frame hop,
// 21 ms delay). Per frequency bin the stereo pair's real covariance [[LL, LR], [LR, RR]] (Re of the cross term, smoothed
// over neighbouring bins and ~15 ms) is split into its principal direction (a, b) and the rest:
//   - a sound panned with real gains (a, b) (b < 0: out of phase) is coherent: its energy is lambda1 - lambda2. Its
//     angle alpha = atan2(b, a) in (-90, 90] (a >= 0; (0, -1) is (0, 1)) maps onto the listener's circle through the
//     Pro Logic II encoder's own points: L (1, 0) at -30 deg, C (.707, .707) at 0, R (0, 1) at +30, Ls (.87, -.49) at
//     -110, Rs (.49, -.87) at +110, the rear centre (.707, -.707) at 180; between them linearly. It is panned to the
//     nearest pair of speakers (constant power).
//   - the rest (uncorrelated, reverb, ambience) stays on its own side, split evenly between the front and rear speakers
//     of that side.
// The bass below 80 Hz (faded out by 120 Hz) also goes to the LFE channel, the mains keep it. Energy is kept: a sound
// panned anywhere comes out as loud as it went in.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ps2x
{
    class ProLogicDecoder
    {
    public:
        static constexpr double kPi = 3.14159265358979323846;
        static constexpr size_t kFft = 1024u;
        static constexpr size_t kHop = kFft / 2u;
        static constexpr size_t kBins = kFft / 2u + 1u;
        static constexpr size_t kMaxRing = 16u; // directional speakers used (more are left silent)

        // One output channel: a speaker at an azimuth (degrees, 0 = front, + = right), the LFE, or nothing (a channel
        // the decoder leaves silent, e.g. a height speaker).
        struct Speaker
        {
            enum class Kind : uint8_t
            {
                Directional,
                Lfe,
                None,
            };
            Kind kind = Kind::None;
            float azimuth = 0.0f;
        };

        explicit ProLogicDecoder(std::vector<Speaker> speakers, double sampleRate = 48000.0)
            : m_speakers(std::move(speakers))
        {
            m_channels = m_speakers.size();
            for (size_t i = 0u; i < kFft; ++i)
                m_window[i] = static_cast<float>(std::sin(kPi * (static_cast<double>(i) + 0.5) / kFft)); // sqrt-Hann
            for (size_t i = 0u; i < kFft / 2u; ++i)
                m_twiddle[i] = std::polar(1.0f, static_cast<float>(-2.0 * kPi * static_cast<double>(i) / kFft));
            for (size_t i = 0u; i < kFft; ++i)
            {
                size_t r = 0u;
                for (size_t bit = 1u, j = i; bit < kFft; bit <<= 1u, j >>= 1u)
                    r = (r << 1u) | (j & 1u);
                m_reverse[i] = r;
            }
            for (size_t k = 0u; k < kBins; ++k)
            {
                const double hz = static_cast<double>(k) * sampleRate / kFft;
                m_lfeWeight[k] = static_cast<float>(std::clamp((120.0 - hz) / 40.0, 0.0, 1.0));
            }
            // Directional speakers sorted round the circle; the diffuse sides: every directional speaker left (right) of
            // the centre line, evenly.
            for (size_t c = 0u; c < m_channels; ++c)
            {
                if (m_speakers[c].kind == Speaker::Kind::Directional && m_ring.size() < kMaxRing)
                    m_ring.push_back(c);
                else if (m_speakers[c].kind == Speaker::Kind::Lfe)
                    m_lfe.push_back(c);
            }
            std::sort(m_ring.begin(), m_ring.end(), [&](size_t a, size_t b) { return m_speakers[a].azimuth < m_speakers[b].azimuth; });
            for (size_t c : m_ring)
            {
                const float az = m_speakers[c].azimuth;
                if (az < -1.0f && az > -179.0f)
                    m_leftSide.push_back(c);
                else if (az > 1.0f && az < 179.0f)
                    m_rightSide.push_back(c);
            }
            m_out.assign(m_channels, {});
            for (auto &spectrum : m_out)
                spectrum.assign(kBins, {});
            m_overlap.assign(m_channels * kFft, 0.0f);
            // One hop of silence queued: every call can be answered at once (latency kFft frames in all).
            m_ready.assign(m_channels * kHop, 0.0f);
        }

        size_t channels() const { return m_channels; }

        // Interleaved stereo int16 in, interleaved float [-1, 1] out (channels() per frame), frames each.
        void process(const int16_t *stereo, float *out, size_t frames)
        {
            for (size_t f = 0u; f < frames; ++f)
            {
                m_input[m_inputFill * 2u] = static_cast<float>(stereo[f * 2u]) / 32768.0f;
                m_input[m_inputFill * 2u + 1u] = static_cast<float>(stereo[f * 2u + 1u]) / 32768.0f;
                if (++m_inputFill == kFft)
                {
                    hop();
                    std::copy(m_input.begin() + kHop * 2u, m_input.end(), m_input.begin());
                    m_inputFill = kFft - kHop;
                }
            }
            const size_t samples = frames * m_channels;
            std::copy(m_ready.begin(), m_ready.begin() + static_cast<std::ptrdiff_t>(samples), out);
            m_ready.erase(m_ready.begin(), m_ready.begin() + static_cast<std::ptrdiff_t>(samples));
        }

        // Where the decoder puts a sound panned with real gains (left, right): the azimuth, in degrees (tests).
        static float azimuthOf(float left, float right)
        {
            if (left < 0.0f || (left == 0.0f && right < 0.0f))
                left = -left, right = -right;
            return azimuthOfAngle(static_cast<float>(std::atan2(right, left) * 180.0 / kPi));
        }

    private:
        using Complex = std::complex<float>;

        // alpha (degrees, (-90, 90]) to the listener's azimuth, through the Pro Logic II encoder's points.
        static float azimuthOfAngle(float alpha)
        {
            static constexpr float kLs = -29.39f; // atan2(-0.4899, 0.8718)
            static constexpr std::array<std::array<float, 2>, 7> kPoints = {{
                {90.0f, 30.0f}, {45.0f, 0.0f}, {0.0f, -30.0f}, {kLs, -110.0f}, {-45.0f, -180.0f}, {-90.0f - kLs, -250.0f},
                {-90.0f, -330.0f},
            }};
            alpha = std::clamp(alpha, -90.0f, 90.0f);
            float az = kPoints.back()[1];
            for (size_t i = 0u; i + 1u < kPoints.size(); ++i)
                if (alpha <= kPoints[i][0] && alpha >= kPoints[i + 1u][0])
                {
                    const float t = (kPoints[i][0] - alpha) / (kPoints[i][0] - kPoints[i + 1u][0]);
                    az = kPoints[i][1] + (kPoints[i + 1u][1] - kPoints[i][1]) * t;
                    break;
                }
            while (az <= -180.0f)
                az += 360.0f;
            return az;
        }

        // Constant-power gains over the nearest pair of directional speakers.
        void pan(float az, std::array<float, kMaxRing> &gains) const
        {
            gains.fill(0.0f);
            const size_t n = m_ring.size();
            if (n == 0u)
                return;
            if (n == 1u)
            {
                gains[0] = 1.0f;
                return;
            }
            for (size_t i = 0u; i < n; ++i)
            {
                const size_t j = (i + 1u) % n;
                float a = m_speakers[m_ring[i]].azimuth, b = m_speakers[m_ring[j]].azimuth;
                if (b <= a)
                    b += 360.0f;
                float x = az;
                if (x < a)
                    x += 360.0f;
                if (x >= a && x <= b)
                {
                    const float t = (x - a) / (b - a);
                    gains[i] = std::cos(t * static_cast<float>(kPi) * 0.5f);
                    gains[j] = std::sin(t * static_cast<float>(kPi) * 0.5f);
                    return;
                }
            }
        }

        void fft(std::array<Complex, kFft> &x, bool inverse) const
        {
            for (size_t i = 0u; i < kFft; ++i)
                if (m_reverse[i] > i)
                    std::swap(x[i], x[m_reverse[i]]);
            for (size_t size = 2u; size <= kFft; size <<= 1u)
            {
                const size_t half = size / 2u, step = kFft / size;
                for (size_t start = 0u; start < kFft; start += size)
                    for (size_t k = 0u; k < half; ++k)
                    {
                        Complex w = m_twiddle[k * step];
                        if (inverse)
                            w = std::conj(w);
                        const Complex t = w * x[start + k + half];
                        x[start + k + half] = x[start + k] - t;
                        x[start + k] += t;
                    }
            }
        }

        void hop()
        {
            // Both channels in one transform: z = l + i r.
            for (size_t i = 0u; i < kFft; ++i)
                m_work[i] = Complex(m_input[i * 2u] * m_window[i], m_input[i * 2u + 1u] * m_window[i]);
            fft(m_work, false);
            for (size_t k = 0u; k < kBins; ++k)
            {
                const Complex z = m_work[k], zc = std::conj(m_work[(kFft - k) % kFft]);
                m_left[k] = (z + zc) * 0.5f;
                m_right[k] = (z - zc) * Complex(0.0f, -0.5f);
            }
            for (size_t k = 0u; k < kBins; ++k)
            {
                float ll = 0.0f, rr = 0.0f, lr = 0.0f;
                for (size_t n = (k < kSpread ? 0u : k - kSpread); n <= std::min(k + kSpread, kBins - 1u); ++n)
                {
                    ll += std::norm(m_left[n]);
                    rr += std::norm(m_right[n]);
                    lr += (m_left[n] * std::conj(m_right[n])).real();
                }
                m_ll[k] = m_ll[k] * kSmooth + ll * (1.0f - kSmooth);
                m_rr[k] = m_rr[k] * kSmooth + rr * (1.0f - kSmooth);
                m_lr[k] = m_lr[k] * kSmooth + lr * (1.0f - kSmooth);
            }
            for (auto &spectrum : m_out)
                std::fill(spectrum.begin(), spectrum.end(), Complex{});
            std::array<float, kMaxRing> gains{};
                        for (size_t k = 0u; k < kBins; ++k)
            {
                const Complex l = m_left[k], r = m_right[k];
                const float a = m_ll[k], d = m_rr[k], b = m_lr[k];
                const float trace = a + d;
                Complex coherent{}, projection{};
                float x = 1.0f, y = 0.0f, share = 0.0f;
                if (trace > 1e-12f)
                {
                    const float spread = std::sqrt((a - d) * (a - d) * 0.25f + b * b);
                    const float l1 = trace * 0.5f + spread, l2 = std::max(trace * 0.5f - spread, 0.0f);
                    if (std::fabs(b) > 1e-20f)
                        x = l1 - d, y = b;
                    else if (a >= d)
                        x = 1.0f, y = 0.0f;
                    else
                        x = 0.0f, y = 1.0f;
                    const float norm = std::sqrt(x * x + y * y);
                    x /= norm, y /= norm;
                    if (x < 0.0f || (x == 0.0f && y < 0.0f))
                        x = -x, y = -y;
                    // How much of the bin is one panned sound: (l1 - l2) / (l1 + l2), 1 for a single sound. Below
                    // kDiffuse it is taken as unrelated sound (an estimate over a few bins and hops is never exactly 0).
                    const float direct = (l1 - l2) / (l1 + l2);
                    share = std::sqrt(std::clamp((direct - kDiffuse) / (1.0f - kDiffuse), 0.0f, 1.0f));
                    projection = l * x + r * y;
                    coherent = projection * share;
                    pan(azimuthOfAngle(static_cast<float>(std::atan2(y, x) * 180.0 / kPi)), gains);
                    for (size_t s = 0u; s < m_ring.size(); ++s)
                        if (gains[s] != 0.0f)
                            m_out[m_ring[s]][k] += coherent * gains[s];
                }
                // The rest stays on its side, evenly over that side's speakers (both fronts when there are none): the
                // orthogonal part plus what the direction left of the projection, so the energy adds up to the bin's.
                const Complex kept = projection * (std::sqrt(1.0f - share * share) - 1.0f);
                const Complex restLeft = l + kept * x, restRight = r + kept * y;
                spread(restLeft, m_leftSide, k);
                spread(restRight, m_rightSide, k);
                if (m_lfeWeight[k] > 0.0f)
                    for (size_t c : m_lfe)
                        m_out[c][k] += (l + r) * (0.5f * m_lfeWeight[k]);
            }
            // Back to time: two channels per inverse transform (a + i b), windowed, overlap-added.
            for (size_t c = 0u; c < m_channels; c += 2u)
            {
                const bool pair = c + 1u < m_channels;
                for (size_t k = 0u; k < kBins; ++k)
                {
                    const Complex p = m_out[c][k], q = pair ? m_out[c + 1u][k] : Complex{};
                    m_work[k] = p + Complex(0.0f, 1.0f) * q;
                    if (k != 0u && k != kFft / 2u)
                        m_work[kFft - k] = std::conj(p) + Complex(0.0f, 1.0f) * std::conj(q);
                }
                fft(m_work, true);
                for (size_t i = 0u; i < kFft; ++i)
                {
                    const float w = m_window[i] / static_cast<float>(kFft);
                    m_overlap[c * kFft + i] += m_work[i].real() * w;
                    if (pair)
                        m_overlap[(c + 1u) * kFft + i] += m_work[i].imag() * w;
                }
            }
            const size_t start = m_ready.size();
            m_ready.resize(start + kHop * m_channels);
            for (size_t i = 0u; i < kHop; ++i)
                for (size_t c = 0u; c < m_channels; ++c)
                    m_ready[start + i * m_channels + c] = std::clamp(m_overlap[c * kFft + i], -1.0f, 1.0f);
            for (size_t c = 0u; c < m_channels; ++c)
            {
                float *o = m_overlap.data() + c * kFft;
                std::copy(o + kHop, o + kFft, o);
                std::fill(o + kFft - kHop, o + kFft, 0.0f);
            }
        }

        void spread(Complex value, const std::vector<size_t> &side, size_t k)
        {
            const std::vector<size_t> &to = side.empty() ? m_ring : side;
            if (to.empty())
                return;
            const float g = 1.0f / std::sqrt(static_cast<float>(to.size()));
            for (size_t c : to)
                m_out[c][k] += value * g;
        }

        static constexpr float kSmooth = 0.6f;   // per 10.7 ms hop: ~20 ms
        static constexpr size_t kSpread = 3u;    // bins either side (47 Hz each) in the covariance
        static constexpr float kDiffuse = 0.2f;  // directness below this: unrelated sound

        std::vector<Speaker> m_speakers;
        size_t m_channels = 0u;
        std::vector<size_t> m_ring, m_lfe, m_leftSide, m_rightSide;
        std::array<float, kFft> m_window{};
        std::array<Complex, kFft / 2u> m_twiddle{};
        std::array<size_t, kFft> m_reverse{};
        std::array<float, kBins> m_lfeWeight{};
        std::array<float, kFft * 2u> m_input{};
        size_t m_inputFill = kFft - kHop; // the first window starts with half a window of silence
        std::array<Complex, kFft> m_work{};
        std::array<Complex, kBins> m_left{}, m_right{};
        std::array<float, kBins> m_ll{}, m_rr{}, m_lr{};
        std::vector<std::vector<Complex>> m_out;
        std::vector<float> m_overlap;
        std::vector<float> m_ready;
    };
}
