// Pro Logic II decoding (runtime/prologic_decoder.h, issue #18): sounds panned into stereo with real gains, the rear
// ones out of phase (as God Hand's Pro Logic II mode does with the SPU2's negative voice volumes), decoded to 5.1, 4.0
// and 7.1. Checks where each one's energy lands, that it keeps its loudness, that a plain stereo pair stays on its own
// sides, that a rear sound moving round the back pans smoothly, and that the bass reaches the LFE.
// Exit code 0 = all passed. ctest runs it (KAMITE_BUILD_TESTS=ON).
#include "runtime/prologic_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

namespace
{
    using ps2x::ProLogicDecoder;
    using Kind = ProLogicDecoder::Speaker::Kind;
    constexpr double kPi = 3.14159265358979323846;

    int g_failures = 0, g_checks = 0;
    void check(bool ok, const std::string &what)
    {
        ++g_checks;
        if (!ok)
            ++g_failures;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    }

    // 5.1 as miniaudio's default map has it: FL FR FC LFE SL SR.
    std::vector<ProLogicDecoder::Speaker> layout51()
    {
        return {{Kind::Directional, -30.0f}, {Kind::Directional, 30.0f}, {Kind::Directional, 0.0f},
                {Kind::Lfe, 0.0f},           {Kind::Directional, -110.0f}, {Kind::Directional, 110.0f}};
    }
    std::vector<ProLogicDecoder::Speaker> layoutQuad()
    {
        return {{Kind::Directional, -30.0f}, {Kind::Directional, 30.0f}, {Kind::Directional, -110.0f},
                {Kind::Directional, 110.0f}};
    }
    std::vector<ProLogicDecoder::Speaker> layout71()
    {
        return {{Kind::Directional, -30.0f}, {Kind::Directional, 30.0f}, {Kind::Directional, 0.0f},
                {Kind::Lfe, 0.0f},           {Kind::Directional, -150.0f}, {Kind::Directional, 150.0f},
                {Kind::Directional, -90.0f}, {Kind::Directional, 90.0f}};
    }

    // Band-limited noise (200 Hz - 8 kHz: above the LFE band), amplitude ~0.25.
    std::vector<float> noise(size_t frames, unsigned seed)
    {
        std::mt19937 rng(seed);
        std::normal_distribution<float> n(0.0f, 0.25f);
        std::vector<float> out(frames);
        float hp = 0.0f, lp = 0.0f, prev = 0.0f;
        for (size_t i = 0u; i < frames; ++i)
        {
            const float x = n(rng);
            hp = 0.974f * (hp + x - prev); // ~200 Hz high-pass
            prev = x;
            lp += 0.65f * (hp - lp);       // ~8 kHz low-pass
            out[i] = lp * 2.0f;
        }
        return out;
    }

    struct Result
    {
        std::vector<double> energy; // per output channel
        double input = 0.0;         // stereo input energy
    };

    // Runs stereo through a decoder in uneven chunks (like a sound card's callbacks) and measures per-channel energy,
    // skipping the first 100 ms (smoothing settling) and allowing for the decoder's delay.
    Result run(const std::vector<ProLogicDecoder::Speaker> &layout, const std::vector<float> &left, const std::vector<float> &right)
    {
        ProLogicDecoder decoder(layout);
        const size_t frames = left.size(), channels = decoder.channels();
        std::vector<int16_t> in(frames * 2u);
        for (size_t i = 0u; i < frames; ++i)
        {
            in[i * 2u] = static_cast<int16_t>(std::lround(std::clamp(left[i], -1.0f, 1.0f) * 32767.0f));
            in[i * 2u + 1u] = static_cast<int16_t>(std::lround(std::clamp(right[i], -1.0f, 1.0f) * 32767.0f));
        }
        std::vector<float> out(frames * channels);
        static const size_t chunks[] = {441u, 512u, 333u, 1024u, 96u, 700u};
        for (size_t pos = 0u, c = 0u; pos < frames; ++c)
        {
            const size_t n = std::min(chunks[c % 6u], frames - pos);
            decoder.process(in.data() + pos * 2u, out.data() + pos * channels, n);
            pos += n;
        }
        Result r;
        r.energy.assign(channels, 0.0);
        const size_t skip = 4800u, delay = ProLogicDecoder::kFft;
        for (size_t i = skip; i + delay < frames; ++i)
        {
            for (size_t c = 0u; c < channels; ++c)
                r.energy[c] += static_cast<double>(out[(i + delay) * channels + c]) * out[(i + delay) * channels + c];
            r.input += static_cast<double>(in[i * 2u]) * in[i * 2u] / (32768.0 * 32768.0) +
                       static_cast<double>(in[i * 2u + 1u]) * in[i * 2u + 1u] / (32768.0 * 32768.0);
        }
        return r;
    }

    double share(const Result &r, std::initializer_list<size_t> channels)
    {
        double total = 0.0, part = 0.0;
        for (size_t c = 0u; c < r.energy.size(); ++c)
            total += r.energy[c];
        for (size_t c : channels)
            part += r.energy[c];
        return total > 0.0 ? part / total : 0.0;
    }

    double directionalEnergy(const Result &r, const std::vector<ProLogicDecoder::Speaker> &layout)
    {
        double e = 0.0;
        for (size_t c = 0u; c < layout.size(); ++c)
            if (layout[c].kind == Kind::Directional)
                e += r.energy[c];
        return e;
    }

    std::string describe(const Result &r)
    {
        double total = 0.0;
        for (double e : r.energy)
            total += e;
        std::string s = " [";
        for (double e : r.energy)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f%% ", total > 0.0 ? 100.0 * e / total : 0.0);
            s += buf;
        }
        s.back() = ']';
        return s;
    }

    Result panned(const std::vector<ProLogicDecoder::Speaker> &layout, float gl, float gr, unsigned seed = 1u)
    {
        const std::vector<float> s = noise(48000u, seed);
        std::vector<float> l(s.size()), r(s.size());
        for (size_t i = 0u; i < s.size(); ++i)
            l[i] = s[i] * gl, r[i] = s[i] * gr;
        return run(layout, l, r);
    }
}

int main()
{
    // The mapping itself.
    check(std::fabs(ProLogicDecoder::azimuthOf(1.0f, 0.0f) + 30.0f) < 0.5f, "L alone maps to -30");
    check(std::fabs(ProLogicDecoder::azimuthOf(0.0f, 1.0f) - 30.0f) < 0.5f, "R alone maps to +30");
    check(std::fabs(ProLogicDecoder::azimuthOf(0.707f, 0.707f)) < 0.5f, "in phase, equal: centre");
    check(std::fabs(std::fabs(ProLogicDecoder::azimuthOf(0.707f, -0.707f)) - 180.0f) < 0.5f, "out of phase, equal: behind");
    check(std::fabs(ProLogicDecoder::azimuthOf(0.8718f, -0.4899f) + 110.0f) < 0.5f, "Pro Logic II's Ls: -110");
    check(std::fabs(ProLogicDecoder::azimuthOf(-0.4899f, 0.8718f) - 110.0f) < 0.5f, "Pro Logic II's Rs: +110");
    check(std::fabs(ProLogicDecoder::azimuthOf(0.0f, -1.0f) - 30.0f) < 0.5f, "R inverted alone is still R");

    const auto l51 = layout51();
    // 5.1: FL 0, FR 1, FC 2, LFE 3, SL 4, SR 5.
    {
        const Result r = panned(l51, 0.707f, -0.707f);
        check(share(r, {4u, 5u}) > 0.97 && std::fabs(r.energy[4] - r.energy[5]) < 0.05 * r.energy[4],
              "5.1: a sound behind the camera plays on both rears, evenly" + describe(r));
        check(std::fabs(directionalEnergy(r, l51) / r.input - 1.0) < 0.05, "5.1: ... at the loudness it went in with");
    }
    {
        const Result r = panned(l51, 0.8718f, -0.4899f);
        check(share(r, {4u}) > 0.95, "5.1: Pro Logic II's left surround plays on the left rear" + describe(r));
    }
    {
        const Result r = panned(l51, -0.4899f, 0.8718f);
        check(share(r, {5u}) > 0.95, "5.1: right surround on the right rear" + describe(r));
    }
    {
        const Result r = panned(l51, 1.0f, 0.0f);
        check(share(r, {0u}) > 0.97, "5.1: hard left stays front left" + describe(r));
        check(std::fabs(directionalEnergy(r, l51) / r.input - 1.0) < 0.05, "5.1: ... at its loudness");
    }
    {
        const Result r = panned(l51, 0.707f, 0.707f);
        check(share(r, {2u}) > 0.97, "5.1: a centred sound plays on the centre" + describe(r));
    }
    {
        const Result r = panned(l51, 0.95f, 0.31f);
        check(share(r, {0u, 2u}) > 0.97 && r.energy[0] > r.energy[2], "5.1: half left, in phase: between front left and centre" + describe(r));
    }
    {
        const Result r = panned(l51, 0.95f, -0.31f);
        check(share(r, {0u, 4u}) > 0.97, "5.1: mostly left, a little out of phase: front left to left rear, beside you" + describe(r));
    }
    // A plain stereo pair (unrelated sounds left and right): no centre or crossing over, each stays on its own side.
    {
        const std::vector<float> a = noise(48000u, 7u), b = noise(48000u, 8u);
        const Result r = run(l51, a, b);
        check(share(r, {0u, 4u}) > 0.4 && share(r, {1u, 5u}) > 0.4 && share(r, {2u}) < 0.1,
              "5.1: unrelated left and right stay on their own sides" + describe(r));
        check(std::fabs(directionalEnergy(r, l51) / r.input - 1.0) < 0.1, "5.1: ... at their loudness");
    }
    // A sound moving round behind (front left -> left rear -> behind -> right rear -> front right): the rear pair's
    // balance moves one way, no jumps.
    {
        bool monotonic = true, rearMostly = true;
        double previous = -1.0;
        for (int step = 0; step <= 8; ++step)
        {
            const double angle = -kPi / 2.0 * (0.2 + 0.6 * step / 8.0); // alpha from -18 to -72 degrees
            const Result r = panned(l51, static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)), 11u + static_cast<unsigned>(step));
            const double balance = r.energy[5] / (r.energy[4] + r.energy[5] + 1e-12);
            monotonic = monotonic && balance >= previous - 0.02;
            previous = balance;
            rearMostly = rearMostly && (step < 2 || step > 6 || share(r, {4u, 5u}) > 0.9);
        }
        check(monotonic, "5.1: a sound circling behind pans smoothly from the left rear to the right rear");
        check(rearMostly, "5.1: ... and stays in the rears while behind");
    }
    // Bass: a 50 Hz tone reaches the LFE.
    {
        std::vector<float> l(48000u), r(48000u);
        for (size_t i = 0u; i < l.size(); ++i)
            l[i] = r[i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 50.0 * i / 48000.0));
        const Result res = run(l51, l, r);
        check(res.energy[3] > 0.2 * res.energy[2], "5.1: a 50 Hz tone also reaches the LFE" + describe(res));
    }
    // Quad (no centre): behind -> the rear pair; centre -> both fronts evenly.
    {
        const auto quad = layoutQuad();
        const Result back = panned(quad, 0.707f, -0.707f), centre = panned(quad, 0.707f, 0.707f);
        check(share(back, {2u, 3u}) > 0.97, "4.0: behind plays on the rears" + describe(back));
        check(share(centre, {0u, 1u}) > 0.97 && std::fabs(centre.energy[0] - centre.energy[1]) < 0.05 * centre.energy[0],
              "4.0: centre plays on both fronts evenly" + describe(centre));
    }
    // 7.1: FL FR FC LFE BL BR SL SR. Behind -> the backs; Ls (-110) -> side left and back left.
    {
        const auto l71 = layout71();
        const Result back = panned(l71, 0.707f, -0.707f), ls = panned(l71, 0.8718f, -0.4899f);
        check(share(back, {4u, 5u}) > 0.97, "7.1: behind plays on the back pair" + describe(back));
        check(share(ls, {6u, 4u}) > 0.95 && ls.energy[6] > ls.energy[4], "7.1: left surround between side left and back left" + describe(ls));
    }
    std::printf("%d of %d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
