#include "star/frame_double.h"

#include "star/live_settings.h"
#include "star/live_look.h"
#include "star/play_log.h"

#include "ps2_runtime.h"
#include "runtime/ps2_async_vif.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "star/window_watch.h"

#include <deque>
#include <mutex>
#include <iterator>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

// STAR 2026-11-23: pictures at the screen's own refresh rate.
//
// What the game does (NOTES.md, "Camera turning"): its logic runs 30 times a second; each frame is one VIF1 stream
// (about 1.1 MB: ~1500 UNPACKs, ~540 VU1 runs) drawn into one of two frame buffers. The camera reaches VU1 as 20
// qwords at the start of the stream (and again for the shadow buffer, with another projection):
//   q16-q19  V: world -> view (row vectors: view = world * V; rows 0-2 the rotation, row 3 the translation)
//   q0-q3    V * Ps: world -> screen (Ps is q12-q15)
//   q4-q7    V * Pc: world -> clip
//   q8-q15   projection constants (no camera in them)
// Object matrices and lights are in world space, so a frame can be drawn from another camera by replacing those
// three matrices: for a camera V', each camera matrix M = V * K becomes V' * K = D * M with D = V' * inverse(V).
// An object is an UNPACK of its world matrix to VU1 address 24 (and 28 for a part skinned to two bones) followed by
// its vertices; the vertex data names it, so the same object is found again in the next frame.
//
// What this does. The screen shows a picture at every refresh (60, 75, 120, 144, 165, 240 a second); the game makes
// 30 states a second. When frame N arrives, the time from the previous frame's state to this one is laid over the
// next stretch of screen refreshes (as long as a frame lasts, starting a few milliseconds from now): each refresh in
// it gets a picture of the scene at its own fraction t of the way from frame N-1 to frame N: the camera's rotation
// and the eye position at t, every object's matrices at t. The runtime's VIF worker (ps2x::asyncvif::setFrameDoubler)
// first runs the game's frame as it is, then one more pass per refresh with those matrices written into a copy of
// the stream; PCSX2's GS keeps each pass's picture and presents it at the refresh it was made for (GSRenderer.h,
// kept pictures; the times go there by the option fd_times). On a 60 Hz screen that is two pictures a frame, on
// 144 Hz four or five, with no picture ever on screen for two refreshes while another gets three: that mismatch
// (60 pictures a second on a 144 Hz screen: 2, 3, 2, 2, 3 refreshes each) was the "vibration" the user saw.
// The picture is as late as before: the state of frame N is fully on screen one frame time after it arrived.
// When nothing moves, or with the setting off, the frame's own picture is presented at each of its refreshes.
// Left at the frame's own state: screen-space things the game places itself (the 2D program at VU1 0x3c30:
// particles, effects), texture animation, lights. The game's own frame, its timing and what it leaves behind (VU1,
// VIF1, GS memory, the frame buffer) are as without this.
extern void (*g_ps2xLastPicturesHook)(const char *folder); // ps2_runtime.cpp: the LastPictures hotkey

namespace star
{
    namespace
    {
        struct Mat4
        {
            double m[4][4];
        };

        Mat4 load(const uint8_t *bytes)
        {
            Mat4 r{};
            float f[16];
            std::memcpy(f, bytes, sizeof(f));
            for (int i = 0; i < 16; ++i)
                r.m[i / 4][i % 4] = f[i];
            return r;
        }

        void store(const Mat4 &a, uint8_t *bytes)
        {
            float f[16];
            for (int i = 0; i < 16; ++i)
                f[i] = static_cast<float>(a.m[i / 4][i % 4]);
            std::memcpy(bytes, f, sizeof(f));
        }

        Mat4 mul(const Mat4 &a, const Mat4 &b)
        {
            Mat4 r{};
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                {
                    double sum = 0.0;
                    for (int k = 0; k < 4; ++k)
                        sum += a.m[i][k] * b.m[k][j];
                    r.m[i][j] = sum;
                }
            return r;
        }

        // A view matrix as the game builds it: orthonormal rows, last column (0, 0, 0, 1).
        bool isView(const Mat4 &v)
        {
            for (int i = 0; i < 3; ++i)
            {
                double length = 0.0;
                for (int j = 0; j < 3; ++j)
                    length += v.m[i][j] * v.m[i][j];
                if (std::fabs(length - 1.0) > 1e-3 || std::fabs(v.m[i][3]) > 1e-6)
                    return false;
            }
            for (int i = 0; i < 3; ++i)
                for (int j = i + 1; j < 3; ++j)
                {
                    double dot = 0.0;
                    for (int k = 0; k < 3; ++k)
                        dot += v.m[i][k] * v.m[j][k];
                    if (std::fabs(dot) > 1e-3)
                        return false;
                }
            return std::fabs(v.m[3][3] - 1.0) < 1e-6 && std::isfinite(v.m[3][0]) && std::isfinite(v.m[3][1]) && std::isfinite(v.m[3][2]);
        }

        struct Quat
        {
            double w, x, y, z;
        };

        Quat toQuat(const Mat4 &v)
        {
            const double (*m)[4] = v.m;
            Quat q{};
            const double trace = m[0][0] + m[1][1] + m[2][2];
            if (trace > 0.0)
            {
                const double s = std::sqrt(trace + 1.0) * 2.0;
                q = {0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
            }
            else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
            {
                const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
                q = {(m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
            }
            else if (m[1][1] > m[2][2])
            {
                const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
                q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s};
            }
            else
            {
                const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
                q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s};
            }
            return q;
        }

        void setRotation(Mat4 &v, const Quat &q)
        {
            const double w = q.w, x = q.x, y = q.y, z = q.z;
            v.m[0][0] = 1.0 - 2.0 * (y * y + z * z);
            v.m[0][1] = 2.0 * (x * y - z * w);
            v.m[0][2] = 2.0 * (x * z + y * w);
            v.m[1][0] = 2.0 * (x * y + z * w);
            v.m[1][1] = 1.0 - 2.0 * (x * x + z * z);
            v.m[1][2] = 2.0 * (y * z - x * w);
            v.m[2][0] = 2.0 * (x * z - y * w);
            v.m[2][1] = 2.0 * (y * z + x * w);
            v.m[2][2] = 1.0 - 2.0 * (x * x + y * y);
        }

        // The camera's place in the world: view = world * R + t, so the eye (view 0) is -t * transpose(R).
        void eyeOf(const Mat4 &v, double eye[3])
        {
            for (int c = 0; c < 3; ++c)
                eye[c] = -(v.m[3][0] * v.m[c][0] + v.m[3][1] * v.m[c][1] + v.m[3][2] * v.m[c][2]);
        }

        Mat4 inverseView(const Mat4 &v)
        {
            Mat4 r{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    r.m[i][j] = v.m[j][i];
            double eye[3];
            eyeOf(v, eye);
            for (int c = 0; c < 3; ++c)
                r.m[3][c] = eye[c];
            r.m[3][3] = 1.0;
            return r;
        }

        struct Upload
        {
            size_t item;
            size_t offset; // of the payload inside the item's data
        };

        // One drawn object: the stream gives it a world matrix at VU1 address 24 (and for a part skinned to more bones
        // the others at 28, 32, 36), a few settings, then its vertices in batches. The vertex data (the UNPACKs of
        // 16-bit vectors relative to TOPS) is the model's own and the same every frame, so its hash names the object.
        // STAR 2026-11-25 (the user: "streaks of the player's viewmodel that lag behind and look like black spikes"
        // in fast turns, F1 catch pictures_25): the program at VU address 0 builds a bone's matrices for 24 and for
        // (VU 40.w & 3) more bones at 28, 32, 36. Six finger pieces of the hands (three per hand, held around the
        // RPK's handguard: 40.w = 6) send a third matrix at 32, which was not read: it was never moved in between nor
        // turned with the live view, and those fingers' vertices stayed where the game had put them for its own
        // camera while the rest of the hand went with the picture's camera (dump of a frame: PS2X_VIF_DUMP).
        static constexpr int kMoreBones = 3; // VU 28, 32, 36
        struct Object
        {
            Upload first{};
            Upload more[kMoreBones]{};
            uint8_t moreMask = 0; // bit k: a matrix at 28 + 4k was sent
            uint64_t key = 0xCBF29CE484222325ull;
            uint32_t vertexBytes = 0;
            float a[16]{};                  // the matrices as sent (kept for the next frame)
            float m[kMoreBones][16]{};
            bool still = false;             // it stood still in its own frame (the very same matrices as the frame before)
            int bones() const { return 1 + ((moreMask & 1) != 0) + ((moreMask & 2) != 0) + ((moreMask & 4) != 0); }
            bool sameMore(const Object &other) const
            {
                if (moreMask != other.moreMask)
                    return false;
                for (int k = 0; k < kMoreBones; ++k)
                    if ((moreMask & (1u << k)) != 0u && std::memcmp(m[k], other.m[k], 64) != 0)
                        return false;
                return true;
            }
        };

        struct Scene
        {
            std::vector<Upload> projected; // UNPACK of 16 qwords to VU1 address 0: q0-q3 and q4-q7 carry the camera
            std::vector<Upload> view;      // UNPACK of 4 qwords to address 16
            std::vector<Object> objects;
            std::vector<ps2x::asyncvif::FrameBlank> uploads; // DIRECT commands that carry image data (textures, palettes)
        };

        // Does a DIRECT command's GIF data hold an IMAGE-mode tag (a transfer to GS memory)?
        bool holdsImage(const uint8_t *data, size_t size)
        {
            size_t p = 0;
            while (p + 16 <= size)
            {
                uint64_t lo;
                std::memcpy(&lo, data + p, 8);
                p += 16;
                const uint64_t loops = lo & 0x7FFFu, mode = (lo >> 58) & 3u;
                uint64_t registers = (lo >> 60) & 15u;
                if (registers == 0u)
                    registers = 16u;
                if (mode == 2u)
                    return true;
                p += static_cast<size_t>(mode == 0u ? loops * registers : (loops * registers + 1u) / 2u) * 16u;
            }
            return false;
        }

        uint64_t mix(uint64_t h, const uint8_t *data, size_t size)
        {
            size_t i = 0;
            for (; i + 8 <= size; i += 8)
            {
                uint64_t w;
                std::memcpy(&w, data + i, 8);
                h = (h ^ w) * 0x100000001B3ull;
                h ^= h >> 29;
            }
            for (; i < size; ++i)
                h = (h ^ data[i]) * 0x100000001B3ull;
            return h;
        }

        // Walks one VIF1 stream. False when it does not parse to its end (then nothing is touched).
        bool readScene(const uint8_t *data, size_t size, size_t item, Scene &out)
        {
            uint32_t cl = 1u, wl = 1u;
            size_t p = 0;
            size = size & ~static_cast<size_t>(3);
            bool inObject = false;
            while (p < size)
            {
                uint32_t code;
                std::memcpy(&code, data + p, 4);
                const uint32_t cmd = (code >> 24) & 0x7Fu, num = (code >> 16) & 0xFFu, imm = code & 0xFFFFu;
                p += 4;
                size_t payload = 0;
                if (cmd == 0x01u)
                {
                    cl = imm & 0xFFu;
                    wl = (imm >> 8) & 0xFFu;
                }
                else if (cmd == 0x20u)
                    payload = 4;
                else if (cmd == 0x30u || cmd == 0x31u)
                    payload = 16;
                else if (cmd == 0x4Au)
                    payload = static_cast<size_t>(num ? num : 256u) * 8u;
                else if (cmd == 0x50u || cmd == 0x51u)
                {
                    payload = static_cast<size_t>(imm ? imm : 65536u) * 16u;
                    if (p + payload > size)
                        return false;
                    if (holdsImage(data + p, payload))
                        out.uploads.push_back({static_cast<uint32_t>(item), static_cast<uint32_t>(p - 4), static_cast<uint32_t>(payload + 4)});
                }
                else if (cmd >= 0x60u)
                {
                    const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u, count = num ? num : 256u;
                    const uint32_t vectors = (wl <= cl || wl == 0u) ? count : cl * (count / wl) + std::min(count % wl, cl);
                    uint32_t bits = (32u >> vl) * (vn + 1u);
                    if (vn == 3u && vl == 3u)
                        bits = 16u;
                    payload = (static_cast<size_t>(vectors) * bits + 31u) / 32u * 4u;
                    if (p + payload > size)
                        return false;
                    const bool plain = (cmd & 0x10u) == 0u && wl <= cl;
                    const bool relative = (imm & 0x8000u) != 0u;
                    const uint32_t address = imm & 0x3FFu;
                    if (plain && !relative && vn == 3u && vl == 0u)
                    {
                        if (address == 0u && count == 16u)
                        {
                            out.projected.push_back({item, p});
                            inObject = false; // another pass (the shadow buffer, the screen again) begins
                        }
                        else if (address == 16u && count == 4u)
                            out.view.push_back({item, p});
                        else if (address == 24u && count == 4u)
                        {
                            Object object;
                            object.first = {item, p};
                            std::memcpy(object.a, data + p, 64);
                            out.objects.push_back(object);
                            inObject = true;
                        }
                        else if ((address == 28u || address == 32u || address == 36u) && count == 4u && inObject &&
                                 (out.objects.back().moreMask & (1u << ((address - 28u) / 4u))) == 0u)
                        {
                            Object &object = out.objects.back();
                            const uint32_t k = (address - 28u) / 4u;
                            object.more[k] = {item, p};
                            object.moreMask = static_cast<uint8_t>(object.moreMask | (1u << k));
                            std::memcpy(object.m[k], data + p, 64);
                        }
                    }
                    else if (plain && relative && vn == 3u && vl == 1u && inObject)
                    {
                        Object &object = out.objects.back();
                        object.key = mix(object.key ^ payload, data + p, payload);
                        object.vertexBytes += static_cast<uint32_t>(payload);
                    }
                }
                else if (cmd > 0x17u && cmd != 0x20u)
                    return false; // not a VIF code this stream should hold: out of step
                p += payload;
            }
            return p == size;
        }

        Quat slerp(const Quat &a, Quat b, double t)
        {
            double dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
            if (dot < 0.0)
            {
                b = {-b.w, -b.x, -b.y, -b.z};
                dot = -dot;
            }
            double wa = 1.0 - t, wb = t;
            if (dot < 0.9995)
            {
                const double angle = std::acos(std::min(1.0, dot));
                const double s = std::sin(angle);
                wa = std::sin((1.0 - t) * angle) / s;
                wb = std::sin(t * angle) / s;
            }
            Quat h{wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z};
            const double length = std::sqrt(h.w * h.w + h.x * h.x + h.y * h.y + h.z * h.z);
            return {h.w / length, h.x / length, h.y / length, h.z / length};
        }

        // An object matrix (rows 0-2 the axes, possibly scaled; row 3 the position) the fraction t of the way from
        // one to another. Prepared once per pair, used for every picture of the frame.
        struct Between
        {
            bool rigid = false;
            Quat qa{}, qb{};
            double scaleA[3]{}, scaleB[3]{};
            float from[16]{}, to[16]{};

            // False when the two are too far apart to be one movement (the object then keeps its own frame's place).
            bool set(const float *a, const float *b)
            {
                std::memcpy(from, a, sizeof(from));
                std::memcpy(to, b, sizeof(to));
                double position = 0.0;
                for (int c = 12; c < 15; ++c)
                    position += (static_cast<double>(b[c]) - a[c]) * (static_cast<double>(b[c]) - a[c]);
                if (position > 1500.0 * 1500.0)
                    return false;
                Mat4 ma{}, mb{};
                rigid = true;
                for (int r = 0; r < 3 && rigid; ++r)
                {
                    scaleA[r] = std::sqrt(static_cast<double>(a[r * 4]) * a[r * 4] + static_cast<double>(a[r * 4 + 1]) * a[r * 4 + 1] +
                                          static_cast<double>(a[r * 4 + 2]) * a[r * 4 + 2]);
                    scaleB[r] = std::sqrt(static_cast<double>(b[r * 4]) * b[r * 4] + static_cast<double>(b[r * 4 + 1]) * b[r * 4 + 1] +
                                          static_cast<double>(b[r * 4 + 2]) * b[r * 4 + 2]);
                    if (scaleA[r] < 1e-6 || scaleB[r] < 1e-6)
                    {
                        rigid = false;
                        break;
                    }
                    for (int c = 0; c < 3; ++c)
                    {
                        ma.m[r][c] = a[r * 4 + c] / scaleA[r];
                        mb.m[r][c] = b[r * 4 + c] / scaleB[r];
                    }
                }
                const auto rotation = [](const Mat4 &m)
                {
                    for (int i = 0; i < 3; ++i)
                        for (int j = i + 1; j < 3; ++j)
                            if (std::fabs(m.m[i][0] * m.m[j][0] + m.m[i][1] * m.m[j][1] + m.m[i][2] * m.m[j][2]) > 0.02)
                                return false;
                    // a mirrored matrix is no rotation: the quaternion would be of another matrix
                    return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) - m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
                               m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]) > 0.0;
                };
                rigid = rigid && rotation(ma) && rotation(mb);
                if (rigid)
                {
                    qa = toQuat(ma);
                    qb = toQuat(mb);
                    if (std::fabs(qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z) < 0.5)
                        return false; // more than 120 degrees in one frame: not one movement
                }
                return true;
            }

            void at(double t, uint8_t *bytes) const
            {
                float out[16];
                if (rigid)
                {
                    Mat4 m{};
                    setRotation(m, slerp(qa, qb, t));
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            out[r * 4 + c] = static_cast<float>(m.m[r][c] * ((1.0 - t) * scaleA[r] + t * scaleB[r]));
                }
                else
                {
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            out[r * 4 + c] = static_cast<float>((1.0 - t) * from[r * 4 + c] + t * to[r * 4 + c]);
                }
                for (int c = 12; c < 15; ++c)
                    out[c] = static_cast<float>((1.0 - t) * from[c] + t * to[c]);
                for (int r = 0; r < 4; ++r)
                    out[r * 4 + 3] = to[r * 4 + 3];
                std::memcpy(bytes, out, sizeof(out));
            }
        };

        // STAR 2026-11-25, live look (live_look.h): a view turned by `yaw` about the world's upright axis and by
        // `pitch` about its own sideways axis, the eye staying where it is. The game builds its view from the two
        // angles it keeps (camera + 0x24, + 0x20) as V = T * Yaw * Pitch (row vectors), so turning by a little more
        // of each is V * (the yaw turn about the world's Y as the view sees it) * (the pitch turn about view X).
        // The signs are the game's (found with STAR_LIVE_LOG=1, which tries all four on the game's own frames).
        Mat4 axisTurn(const double a[3], double angle)
        {
            const double c = std::cos(angle), sn = std::sin(angle), k = 1.0 - c;
            const double column[3][3] = {
                {c + k * a[0] * a[0], k * a[0] * a[1] - sn * a[2], k * a[0] * a[2] + sn * a[1]},
                {k * a[1] * a[0] + sn * a[2], c + k * a[1] * a[1], k * a[1] * a[2] - sn * a[0]},
                {k * a[2] * a[0] - sn * a[1], k * a[2] * a[1] + sn * a[0], c + k * a[2] * a[2]},
            };
            Mat4 r{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    r.m[i][j] = column[j][i]; // for row vectors
            r.m[3][3] = 1.0;
            return r;
        }

        Mat4 viewTurnedRaw(const Mat4 &v, double yaw, double pitch)
        {
            const double up[3] = {v.m[1][0], v.m[1][1], v.m[1][2]}; // the world's Y axis as the view sees it
            const double side[3] = {1.0, 0.0, 0.0};
            return mul(mul(v, axisTurn(up, yaw)), axisTurn(side, pitch));
        }

        constexpr double kYawSign = -1.0, kPitchSign = -1.0;
        Mat4 viewTurned(const Mat4 &v, double yaw, double pitch)
        {
            return viewTurnedRaw(v, kYawSign * yaw, kPitchSign * pitch);
        }

        // What the worker needs to give each of a frame's pictures its camera at the last moment (lateCamera).
        struct LateMatrix
        {
            size_t patch;                // its place in the pass's patches
            std::array<float, 16> base;  // the object's matrix at the picture's time fraction
            // (or, `now`: as the game placed it for the frame's own camera. A piece with no in-between matrix: one that
            // did not move, or one that is new this frame, like a muzzle flash, made at the gun's bone as the frame
            // has it. Read through the in-between camera it sat off the barrel by the camera's turn of that frame.)
            bool now = false;
            // (or, `point`: not a matrix but 64 bytes of the stream that begin with a sprite's place in the world;
            // only the place is turned)
            bool point = false;
        };

        // A matrix (or, `point`, a place in the first three floats of 64 bytes) seen through the camera `w` is made
        // of: base x w, the rest of the bytes kept.
        void throughCamera(const std::array<float, 16> &base, const Mat4 &w, bool point, uint8_t *bytes)
        {
            float out[16];
            std::memcpy(out, base.data(), sizeof(out));
            if (point)
            {
                for (int c = 0; c < 3; ++c)
                    out[c] = static_cast<float>(base[0] * w.m[0][c] + base[1] * w.m[1][c] + base[2] * w.m[2][c] + w.m[3][c]);
            }
            else
            {
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 3; ++c)
                        out[r * 4 + c] = static_cast<float>(base[r * 4] * w.m[0][c] + base[r * 4 + 1] * w.m[1][c] + base[r * 4 + 2] * w.m[2][c] +
                                                            (r == 3 ? w.m[3][c] : 0.0));
            }
            std::memcpy(bytes, out, sizeof(out));
        }
        struct LatePass
        {
            Mat4 at;          // the camera at the picture's time fraction, between the two frames
            double t;
            long long shown;  // the picture's turn on screen (steady clock, ns)
            std::vector<LateMatrix> attached; // the matrices of what goes with the camera (the player's own weapon)
        };
        struct LateLook
        {
            LookFrame look;
            // How far a picture's camera may be turned from the camera the game chose the frame's scenery with (the
            // job before's: at t = 0), radians each way of the game's two angles; from the look frame after this one
            // (live_look.h). And whether the pictures follow the mouse at all (the "low delay" setting).
            double yawNeg = 0.0, yawPos = 0.0, pitchNeg = 0.0, pitchPos = 0.0;
            bool margins = false, ahead = true;
            // The game goes on taking the mouse after this frame (the look frame after this one exists and follows
            // it). If not (an event or a menu takes the controls: the input is no longer read), what the mouse does
            // now never reaches the game's camera, and a picture turned by it would jump back in the next frame:
            // the user's "texture flicker" at the Chasm Nexus door, one frame of the whole view 2 and 4 degrees off
            // (F1 catch pictures_13, frame 4357).
            bool taken = false;
            // (then the pictures go as far as that last look frame's own turn, which the game did take, and no further)
            double lastYaw = 0.0, lastPitch = 0.0;
            // Where the frame before's limits ended (at t = 1), for the yaw and the pitch: this frame's begin there.
            double beforeLow[2] = {0.0, 0.0}, beforeHigh[2] = {0.0, 0.0};
            bool before = false;
            // The amount the picture before was turned on by (yaw, pitch) and its turn on screen: the next one's may
            // not grow faster than kLiveGrowth (lateCamera).
            double lastOn[2] = {0.0, 0.0};
            long long lastShown = 0;
            bool ownFirst = false;         // passes[0] is the game's own run (FramePlan::own), the plan's passes follow
            Mat4 inverse{};                // of the frame's own camera
            std::vector<Mat4> originals;   // the frame's camera matrices, in the order of a pass's first patches
            std::vector<LatePass> passes;
            double leadMs = 25.0;          // the mouse is taken as it was this long before a picture's turn
            double frameGapMs = 0.0;       // the longest wait, from a pass's start to its turn, in the frame at work
            double frameAhead = 0.0;       // the furthest a picture of that frame was turned past the game's camera, radians
            bool fixedLead = false;
            bool log = false;
            uint64_t frames = 0;
        };
        LateLook g_late; // the worker's

        // STAR 2026-11-24, the flight recorder. While the user's "keep the last pictures" setting is on, a record of
        // each of the last 400 game frames is kept: its pictures' ids and time fractions, the cameras, every object
        // (model, place, how it was paired with the frame before) and what the worker did with the passes. F1 writes
        // them next to the pictures (frames.txt), so a bad picture's id leads to the data it was drawn from.
        std::mutex g_historyMutex;
        std::deque<std::string> g_history;

        void keepRecord(std::string &record)
        {
            if (record.empty())
                return;
            std::lock_guard<std::mutex> lock(g_historyMutex);
            g_history.push_back(std::move(record));
            record.clear();
            while (g_history.size() > 400u)
                g_history.pop_front();
        }

        void writeHistory(const char *folder)
        {
            // (2026-11-27, the user's idea: the settings as they are at the catch; the log has only the start's)
            if (FILE *out = std::fopen((std::string(folder) + "/settings.txt").c_str(), "w"))
            {
                std::fprintf(out, "STAR %s, the settings when F1 was pressed%c%c", kVersion, 10, 10);
                std::fputs(settingsSnapshot().c_str(), out);
                std::fclose(out);
            }
            std::lock_guard<std::mutex> lock(g_historyMutex);
            if (FILE *out = std::fopen((std::string(folder) + "/frames.txt").c_str(), "w"))
            {
                for (const std::string &record : g_history)
                    std::fputs(record.c_str(), out);
                std::fclose(out);
            }
        }

        long long nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        struct State
        {
            bool havePrevious = false;
            Mat4 previous{};
            std::vector<Object> previousObjects;
            uint64_t previousTick = 0;
            // The screen's time laid over the game's frames.
            long long lastArrival = 0;
            double framePeriod = 1e9 * 2.0 * 1.001 / 60.0; // two fields of 59.94 a second, until measured
            long long intervalEnd = 0;                     // where the previous frame's stretch of refreshes ended
            long long gridEpoch = 0;                       // a refresh long ago: which refreshes are used when not all are
            uint32_t nextId = 1;
            uint64_t frames = 0, moving = 0, still = 0, cuts = 0, unparsed = 0, pictures = 0, resyncs = 0;
            long long refreshPeriod = 0;
            bool log = false;
            long long marginNs = 5000000;   // from a frame's arrival to its first picture's turn
            long long submitNs = 1000000;   // a picture is handed over this long after its refresh (it shows at a later one)
            int maxPerFrame = 8;
            // Of a frame's time, this much for the game's run plus the passes. Over 1: a pass that does not fit is simply
            // left out (the worker gives the rest up when the game's next frame arrives; the game keeps its speed), and
            // a picture missing now and then is far less visible than half the rate (STAR 2026-11-24: the user found
            // "every refresh" by far the best also in the heavy hub views, where about 0.5% of the pictures are lost).
            double workShare = 1.3;
            bool marginFixed = false;       // STAR_SMOOTH_MARGIN_MS given
            int stride = 0;                 // every stride-th refresh gets a picture (0: not chosen yet)
            int strideCalm = 0;
            int strideTight = 0;            // frames in a row that wanted fewer pictures
            bool screenKnown = false;       // Windows gave the screen's refresh timing
            int holdTold = -1;              // the hold last given to the renderer (fd_hold)
            PS2Runtime *runtime = nullptr;
        };
        State g_state;

        // The worker, right before a pass runs: the pass's camera turned to where the mouse was `leadMs` before the
        // picture's turn on screen. The in-between camera at t already holds the part t of what the mouse did in this
        // game frame; the rest of it, and what the mouse has done since the game last looked, is added here. The lead
        // is the same for every picture (the longest wait any of them has between being drawn and being shown: the
        // frame's last one), so that the pictures show the mouse's movement at an even pace.
        void lateCamera(size_t index, ps2x::asyncvif::FramePass &pass);
        void lateCameraOwn(ps2x::asyncvif::FramePass &own)
        {
            g_late.ownFirst = false; // (for this call: the own run is passes[0])
            lateCamera(0u, own);
            g_late.ownFirst = true;
        }

        void lateCamera(size_t index, ps2x::asyncvif::FramePass &pass)
        {
            LateLook &late = g_late;
            if (late.ownFirst)
                ++index;
            if (index >= late.passes.size())
                return;
            const LatePass &info = late.passes[index];
            const long long now = nowNs();
            late.frameGapMs = std::max(late.frameGapMs, static_cast<double>(info.shown - now) / 1e6);
            // What the mouse has done since the game last looked (the pitch within the game's limits).
            double aheadYaw = 0.0, aheadPitch = 0.0;
            if (late.ahead && !late.taken)
            {
                aheadYaw = late.lastYaw;
                aheadPitch = late.lastPitch;
            }
            if (late.ahead && late.taken && late.look.perCountYaw != 0.0)
            {
                double x = 0.0, y = 0.0;
                if (lookMouseAt(info.shown - static_cast<long long>(late.leadMs * 1e6), x, y))
                {
                    aheadYaw = (x - late.look.rawX) * late.look.perCountYaw;
                    aheadPitch = std::clamp((y - late.look.rawY) * late.look.perCountPitch, -1.0471976 - late.look.pitchNow, 0.6108653 - late.look.pitchNow);
                    late.frameAhead = std::max(late.frameAhead, std::fabs(aheadYaw));
                }
            }
            // STAR 2026-11-25: the camera stays within the margins of the game's "in view" test (game_fixes.cpp,
            // inViewHook), counted from the camera the scenery was chosen with, which is the in-between camera at
            // t = 0. Beyond them things are missing and, worse, unclipped triangles wrap around into huge sheets (the
            // user's "significant flickering when moving the camera around"). `turn` is the frame's own turn, and
            // the wanted camera is the in-between one at t turned on by `on`. Where that asks too much, first the
            // whole frame's pictures are held back by the same amount (the turn stays even from picture to picture,
            // and just lags a little more), and only then each one for itself.
            // (STAR 2026-11-24, the user's F1 catch pictures_7: with the amount held back fixed per frame, a turn
            // that sped up beyond the margins made every frame's first picture step back from the last picture of
            // the frame before, by as much as the turn had grown: a jitter in fast turns. The amount now runs from
            // where the frame before ended to where this one ends, so the pictures go on at the pace of the frame
            // before, without a seam.)
            const auto within = [&](int axis, double turn, double ahead, double neg, double pos)
            {
                double on = (1.0 - info.t) * turn + ahead;
                if (!late.margins)
                    return 0.0; // (no margins known: the camera in between as it is)
                // the margins' edges at the frame's end (t = 1), counted from the in-between camera there
                double low = -neg - turn, high = pos - turn;
                if (late.before)
                {
                    low = (1.0 - info.t) * late.beforeLow[axis] + info.t * low;
                    high = (1.0 - info.t) * late.beforeHigh[axis] + info.t * high;
                }
                // Towards a limit the amount is eased in over the limit's last 30% (a hard stop there made the view
                // lose speed at one stroke when a flick outran the margins: a hitch in the middle of the flick).
                const auto eased = [](double x, double limit) // both >= 0
                {
                    const double knee = 0.7 * limit;
                    return x <= knee || limit <= 0.0 ? std::min(x, limit) : limit - (limit - knee) * std::exp(-(x - knee) / (limit - knee));
                };
                if (low <= high)
                {
                    if (on > 0.0 && high > 0.0)
                        on = eased(on, high);
                    else if (on < 0.0 && low < 0.0)
                        on = -eased(-on, -low);
                    on = std::clamp(on, low, high);
                }
                // STAR 2026-11-24 (the user: "still stuttering ... when moving the camera at really fast speeds"). The
                // amount may fall back towards nothing as fast as it likes (the view never overshoots the mouse), but
                // grows by no more than kLiveGrowth: a flick used to begin with a stall at the edge of the small
                // margin, a burst when the next frame's wide margin opened, and a dip when that was used up too
                // (steps of 3, 0, 0, 4, 6, 6, 2, 1, 1, 3 degrees). Now it begins as one ramp. Aiming does not ask
                // for growth this fast; a flick gets a little of the delay back while it speeds up.
                // (Not while the mouse has turned back against the game's own, later turn: there the amount must run
                // against the in-between camera at once, or the view is dragged on the old way.)
                constexpr double kLiveGrowth = 216.0 / 57.29578; // radians a second
                if (ahead * turn >= 0.0)
                {
                    const double seconds = late.lastShown != 0 ? std::clamp(static_cast<double>(info.shown - late.lastShown) / 1e9, 0.002, 0.05) : 0.05;
                    const double last = late.lastOn[axis], grow = kLiveGrowth * seconds;
                    on = std::clamp(on, std::min(last, 0.0) - grow, std::max(last, 0.0) + grow);
                }
                on = std::clamp(info.t * turn + on, -neg, pos) - info.t * turn;
                late.lastOn[axis] = on;
                return on;
            };
            const double yaw = within(0, late.look.yaw, aheadYaw, late.yawNeg, late.yawPos);
            const double pitch = within(1, late.look.pitch, aheadPitch, late.pitchNeg, late.pitchPos);
            late.lastShown = info.shown;
            const Mat4 turnedView = viewTurned(info.at, yaw, pitch);
            const Mat4 d = mul(turnedView, late.inverse);
            if (late.log)
            {
                // how far this picture's camera is turned from the one the game drew the frame for
                const double trace = d.m[0][0] + d.m[1][1] + d.m[2][2];
                std::printf("[late] frame %llu pass %zu t %.3f: turn %+.4f %+.4f, %.1f degrees from the game's camera; looks %.2f; from the test's camera wanted %+.2f given %+.2f of %.1f..%.1f%c",
                            static_cast<unsigned long long>(late.look.seq), index, info.t, yaw, pitch,
                            std::acos(std::clamp((trace - 1.0) / 2.0, -1.0, 1.0)) * 57.29578,
                            std::atan2(turnedView.m[0][2], turnedView.m[2][2]) * 57.29578, (late.look.yaw + aheadYaw) * 57.29578,
                            (info.t * late.look.yaw + yaw) * 57.29578, -late.yawNeg * 57.29578, late.yawPos * 57.29578, 10);
            }
            for (size_t n = 0; n < late.originals.size() && n < pass.patches.size(); ++n)
                store(mul(d, late.originals[n]), pass.patches[n].bytes);
            // What goes with the camera (the player's weapon and arms are objects in the world, placed by the game
            // for its own camera) turns with the view about the eye, so that it stays where it was in the picture:
            // world -> the view at t -> back to the world through the turned view.
            if (!info.attached.empty())
            {
                const Mat4 back = inverseView(turnedView);
                const Mat4 wBetween = mul(info.at, back), wNow = mul(inverseView(late.inverse), back);
                // STAR_OWN_LOG=1 (tests): what is placed by the frame's own camera, per picture
                static const bool ownLog = [] { const char *v = std::getenv("STAR_OWN_LOG"); return v && *v == '1'; }();
                for (const LateMatrix &matrix : info.attached)
                    if (matrix.patch < pass.patches.size())
                    {
                        throughCamera(matrix.base, matrix.now ? wNow : wBetween, matrix.point, pass.patches[matrix.patch].bytes);
                        if (ownLog && matrix.now)
                        {
                            float out[16];
                            std::memcpy(out, pass.patches[matrix.patch].bytes, 64);
                            const int k = matrix.point ? 0 : 12;
                            std::printf("[own] look %llu picture %zu %s at offset %u: %.1f %.1f %.1f -> %.1f %.1f %.1f%c", static_cast<unsigned long long>(late.look.seq),
                                        index, matrix.point ? "sprite" : "piece", pass.patches[matrix.patch].offset, matrix.base[k], matrix.base[k + 1],
                                        matrix.base[k + 2], out[k], out[k + 1], out[k + 2], 10);
                        }
                    }
            }
        }

        void prepare(const std::vector<ps2x::asyncvif::Item> &items, ps2x::asyncvif::FramePlan &plan)
        {
            State &s = g_state;
            Scene scene;
            for (size_t i = 0; i < items.size(); ++i)
            {
                const ps2x::asyncvif::Item &item = items[i];
                if (item.kind != ps2x::asyncvif::ItemKind::Vif1 || item.data.size() < 4096u)
                    continue;
                Scene found;
                if (readScene(item.data.data(), item.data.size(), i, found))
                {
                    scene.projected.insert(scene.projected.end(), found.projected.begin(), found.projected.end());
                    scene.view.insert(scene.view.end(), found.view.begin(), found.view.end());
                    scene.objects.insert(scene.objects.end(), found.objects.begin(), found.objects.end());
                    scene.uploads.insert(scene.uploads.end(), found.uploads.begin(), found.uploads.end());
                }
                else
                    ++s.unparsed;
            }
            if (scene.view.empty() || scene.projected.empty())
                return; // no 3D scene in this job (menus, movies, loading): the GS presents as it always did
            const Mat4 current = load(items[scene.view[0].item].data.data() + scene.view[0].offset);
            if (!isView(current))
                return;
            ++s.frames;
            {
                // STAR_SMOOTH_VIEW_LOG=1 (a diagnostic): the frame's camera uploads. How many "view" matrices (to VU1
                // address 16) and how many of them differ from the first, with the first that differs; the same for
                // the two matrices of each "projected" block (address 0) against the first block's.
                static const bool viewLog = [] { const char *v = std::getenv("STAR_SMOOTH_VIEW_LOG"); return v && *v == '1'; }();
                static int lines = 0;
                if (viewLog && lines < 400 && s.frames % 15u == 0u)
                {
                    ++lines;
                    size_t otherViews = 0, otherProjected = 0;
                    Mat4 firstOther{};
                    for (const Upload &u : scene.view)
                    {
                        const Mat4 m = load(items[u.item].data.data() + u.offset);
                        if (std::memcmp(&m, &current, sizeof(m)) != 0 && otherViews++ == 0u)
                            firstOther = m;
                    }
                    const uint8_t *p0 = items[scene.projected[0].item].data.data() + scene.projected[0].offset;
                    for (const Upload &u : scene.projected)
                        if (std::memcmp(items[u.item].data.data() + u.offset, p0, 128) != 0)
                            ++otherProjected;
                    std::printf("[views] frame %llu: %zu view uploads, %zu unlike the first; %zu projected blocks, %zu unlike the first; %zu objects",
                                static_cast<unsigned long long>(s.frames), scene.view.size(), otherViews, scene.projected.size(), otherProjected,
                                scene.objects.size());
                    if (otherViews)
                        std::printf("; first view row 3: %.1f %.1f %.1f, the other's: %.1f %.1f %.1f, its axes %.3f %.3f %.3f / first's %.3f %.3f %.3f",
                                    current.m[3][0], current.m[3][1], current.m[3][2], firstOther.m[3][0], firstOther.m[3][1], firstOther.m[3][2],
                                    firstOther.m[0][0], firstOther.m[1][1], firstOther.m[2][2], current.m[0][0], current.m[1][1], current.m[2][2]);
                    std::printf("%c", 10);
                }
            }
            const uint64_t tick = s.runtime ? s.runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
            const bool usable = s.havePrevious && tick - s.previousTick <= 6u;
            const Mat4 previous = s.previous;
            std::vector<Object> previousObjects;
            previousObjects.swap(s.previousObjects);
            s.previous = current;
            s.previousObjects = scene.objects;
            s.previousTick = tick;
            s.havePrevious = true;

            // The refreshes this frame's pictures are for.
            const long long now = nowNs();
            if (s.lastArrival != 0)
            {
                const double since = static_cast<double>(now - s.lastArrival);
                if (since > 20e6 && since < 120e6)
                    s.framePeriod += 0.1 * (since - s.framePeriod);
            }
            s.lastArrival = now;
            long long period = 0, refresh = 0;
            s.screenKnown = screenRefresh(period, refresh);
            if (!s.screenKnown)
            {
                period = 1000000000ll / 60;
                refresh = s.gridEpoch ? s.gridEpoch : now;
            }
            s.refreshPeriod = period;
            if (s.gridEpoch == 0)
                s.gridEpoch = refresh;
            // How many of the screen's refreshes get a picture of their own: all of them when the PC has the time, else
            // every second, third, ... one, so that every picture is on screen for the same number of refreshes
            // (144 Hz: 144, 72, 48 pictures a second). The time: the game's own run of the frame plus one pass per
            // picture, measured by the worker, must fit in a frame's time with room to spare, or the game slows down.
            {
                double ownMs = 0.0, passMs = 0.0;
                ps2x::asyncvif::frameDoubleTimes(ownMs, passMs);
                if (passMs <= 0.0)
                    passMs = ownMs > 0.0 ? ownMs : 4.0; // not measured yet: as the game's own run, or a guess
                if (ownMs <= 0.0)
                    ownMs = passMs;
                const double frameMs = s.framePeriod / 1e6;
                const double perFrame = s.framePeriod / static_cast<double>(period); // refreshes in a frame's time
                const double periodMs = static_cast<double>(period) / 1e6;
                // passes in a frame: the refreshes in its time (2.003 at 60 Hz is two, with a third now and then)
                const auto passes = [&](int stride)
                { return std::ceil(perFrame / stride - 0.05); };
                // all of them in the share of the frame's time, and each one done before the next one's turn
                // STAR 2026-11-24: in play with the mouse and keyboard the game's own run is the frame's first picture
                // (FramePlan::own), so one pass fewer is needed, and the passes have the time of one more before
                // their turns come: pass j is done at own + j passes and due at own + a pass + 3 ms + j refreshes.
                // (The user's F1 catches pictures_6 and _7 were at every second refresh throughout: the count here
                // still held the pass that is no longer run.)
                static const bool ownAlways = []
                {
                    const char *test = std::getenv("STAR_SMOOTH_SELFTEST"), *own = std::getenv("STAR_LIVE_OWN");
                    return !(test && *test == '1') && !(own && *own == '0');
                }();
                const bool ownIsPicture = ownAlways && ps2x::asyncvif::currentJobTag() != 0u;
                const auto fits = [&](int stride, double share)
                {
                    if (!ownIsPicture)
                        return ownMs + passes(stride) * passMs <= frameMs * share && passMs <= 1.15 * periodMs * stride;
                    const double extra = std::max(0.0, passes(stride) - 1.0);
                    return ownMs + extra * passMs <= frameMs * share && (extra - 1.0) * passMs <= 3.0 + extra * periodMs * stride;
                };
                int wanted = 1;
                while (wanted < 16 && (passes(wanted) > s.maxPerFrame || !fits(wanted, s.workShare)))
                    ++wanted;
                // The first picture's turn comes when the game's own run and one pass are done, with some room.
                if (!s.marginFixed)
                    s.marginNs = static_cast<long long>(std::clamp(ownMs + passMs + 3.0, 5.0, 25.0) * 1e6);
                if (s.stride == 0)
                    s.stride = wanted;
                else if (wanted > s.stride)
                {
                    // Not enough time: fewer pictures. At once when the work does not even fit in the frame's whole time
                    // (pictures would be lost), else only when it has been so for half a second: every change of the
                    // rate is a visible change of rhythm (STAR 2026-11-24: 72 <-> 48 a second on the user's 144 Hz
                    // screen came and went with the scene's weight, felt as the vibration returning).
                    s.strideCalm = 0;
                    if (!fits(s.stride, 1.6) || ++s.strideTight >= 15)
                    {
                        s.stride = wanted;
                        s.strideTight = 0;
                    }
                }
                else if (wanted < s.stride)
                {
                    s.strideTight = 0;
                    // more pictures only when there has been room for them for two seconds
                    int roomy = wanted;
                    while (roomy < s.stride && (passes(roomy) > s.maxPerFrame || !fits(roomy, s.workShare * 0.9)))
                        ++roomy;
                    if (roomy >= s.stride)
                        s.strideCalm = 0;
                    else if (++s.strideCalm >= 60)
                    {
                        s.stride = roomy;
                        s.strideCalm = 0;
                    }
                }
                else
                    s.strideTight = 0;
            }
            if (const int forced = live().pictureEvery.load(std::memory_order_relaxed); forced > 0)
                s.stride = std::min(forced, 8); // the menu's fault-finding choice: whatever the time says
            live().screenHz100.store(static_cast<int>(1e11 / static_cast<double>(period)), std::memory_order_relaxed);
            live().pictures100.store(static_cast<int>(1e11 / static_cast<double>(period) / s.stride), std::memory_order_relaxed);
            const int stride = s.stride;
            {
                // Windows is told for how many refreshes each picture stays (a real screen only), so that a picture handed
                // over a little late moves the whole row one refresh on instead of shortening its neighbour.
                // STAR_SMOOTH_HOLD=0: not told (each present replaces the one before at the next refresh).
                static const bool tell = []
                {
                    const char *off = std::getenv("STAR_SMOOTH_HOLD");
                    return !(off && *off == '0') && std::getenv("STAR_REFRESH_HZ") == nullptr;
                }();
                const int hold = tell && s.screenKnown ? stride : 0;
                if (hold != s.holdTold)
                {
                    s.holdTold = hold;
                    GSPcsx2Backend::SetOption("fd_hold", std::to_string(hold).c_str());
                }
            }
            const long long desired = now + s.marginNs;
            long long start;
            if (s.intervalEnd == 0 || std::llabs(desired - s.intervalEnd) > static_cast<long long>(s.framePeriod / 2.0))
            {
                start = desired; // the first frame, or the game stalled or jumped: begin anew
                ++s.resyncs;
            }
            else
                start = s.intervalEnd + (desired - s.intervalEnd) / 10; // follow slow drift, not each frame's jitter
            const long long end = start + static_cast<long long>(s.framePeriod);
            s.intervalEnd = end;
            struct Slot
            {
                uint32_t id;
                double t;
                long long submit;
                long long at;
            };
            std::vector<Slot> slots;
            {
                // refresh number k (counted from `refresh`) is at refresh + k * period; with a stride only those whose
                // number counted from the grid's epoch divides by it
                long long k = (start - refresh) / period;
                while (refresh + k * period < start)
                    ++k;
                for (; refresh + k * period < end && slots.size() < 16u; ++k)
                {
                    const long long at = refresh + k * period;
                    if (stride > 1)
                    {
                        const long long number = (at - s.gridEpoch + period / 2) / period;
                        if (number % stride != 0)
                            continue;
                    }
                    Slot slot;
                    slot.id = s.nextId++ & 0xFFFFFFu;
                    slot.t = std::clamp(static_cast<double>(at - start) / s.framePeriod, 0.0, 1.0);
                    slot.submit = at + s.submitNs;
                    slot.at = at;
                    slots.push_back(slot);
                }
            }
            if (s.log && s.frames % 300u == 0u)
            {
                std::printf("[smooth] %llu frames: %llu with motion, %llu still, %llu cuts, %llu streams not parsed; %llu pictures; screen %.2f Hz, "
                            "frame %.2f ms, %d of %d refreshes used, %llu restarts of the timing\n",
                            static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.moving),
                            static_cast<unsigned long long>(s.still), static_cast<unsigned long long>(s.cuts),
                            static_cast<unsigned long long>(s.unparsed), static_cast<unsigned long long>(s.pictures), 1e9 / static_cast<double>(period),
                            s.framePeriod / 1e6, 1, stride, static_cast<unsigned long long>(s.resyncs));
                double ownMs = 0.0, passMs = 0.0;
                ps2x::asyncvif::frameDoubleTimes(ownMs, passMs);
                std::printf("[smooth] pictures a second %.1f; the game's own run of a frame %.2f ms, a pass %.2f ms\n",
                            1e9 / static_cast<double>(period) / stride, ownMs, passMs);
                std::fflush(stdout);
            }
            if (slots.empty())
                return;
            {
                std::string times;
                for (const Slot &slot : slots)
                {
                    if (!times.empty())
                        times += ',';
                    times += std::to_string(slot.id) + ':' + std::to_string(slot.submit);
                }
                GSPcsx2Backend::SetOption("fd_times", times.c_str());
            }
            s.pictures += slots.size();
            // the flight recorder: this frame's record, and the worker's line about the frame before
            const bool recording = live().recordPictures.load(std::memory_order_relaxed);
            {
                // STAR_HISTORY_AT=<seconds>:<existing folder> (tests): the records written out that long after the start
                static const char *const testAt = std::getenv("STAR_HISTORY_AT");
                static bool testDone = false;
                static const long long start = nowNs();
                if (testAt && !testDone && std::strchr(testAt, ':') && nowNs() - start >= static_cast<long long>(std::atof(testAt) * 1e9))
                {
                    testDone = true;
                    writeHistory(std::strchr(testAt, ':') + 1);
                }
            }
            std::string record;
            {
                const std::string worker = ps2x::asyncvif::takePassInfo();
                if (recording)
                {
                    char text[256];
                    if (!worker.empty())
                    {
                        std::lock_guard<std::mutex> lock(g_historyMutex);
                        if (!g_history.empty())
                            g_history.back() += "W " + worker + "\n";
                    }
                    double ownMs = 0.0, passMs = 0.0;
                    ps2x::asyncvif::frameDoubleTimes(ownMs, passMs);
                    std::snprintf(text, sizeof(text), "F frame %llu tick %llu stride %d screen %.2f Hz, own run %.1f ms, a pass %.1f ms, pictures:",
                                  static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(tick), stride,
                                  1e9 / static_cast<double>(period), ownMs, passMs);
                    record = text;
                    for (const Slot &slot : slots)
                    {
                        std::snprintf(text, sizeof(text), " %u@%.3f", slot.id, slot.t);
                        record += text;
                    }
                    record += "\n";
                }
            }
            const auto showFrame = [&]
            {
                for (const Slot &slot : slots)
                    plan.shown.push_back(slot.id);
            };
            if (!usable || !live().smooth60.load(std::memory_order_relaxed))
            {
                showFrame(); // the frame's own picture at each of its refreshes
                if (recording)
                    record += "R the frame's own picture for all (no frame before to go from, or smooth motion off)\n";
                keepRecord(record);
                return;
            }

            // What moved since the previous frame: the camera, and the objects found again (paired by their vertex data,
            // the nearest one when a model is there several times).
            Quat qa = toQuat(previous), qb = toQuat(current);
            const double dot = std::fabs(qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z);
            double eyeA[3], eyeB[3];
            eyeOf(previous, eyeA);
            eyeOf(current, eyeB);
            const double moved = std::sqrt((eyeA[0] - eyeB[0]) * (eyeA[0] - eyeB[0]) + (eyeA[1] - eyeB[1]) * (eyeA[1] - eyeB[1]) +
                                           (eyeA[2] - eyeB[2]) * (eyeA[2] - eyeB[2]));
            const double angle = 2.0 * std::acos(std::min(1.0, dot)); // radians between the two cameras
            // STAR 2026-11-24 (the user's F1 catch pictures_8, "stuttering ... at really fast speeds"): a turn of more
            // than 0.5 rad in a frame counted as a cut, and a mouse turns the view that fast in a flick (860 degrees
            // a second): 35 of the catch's 400 frames showed the game's own picture alone, 30 steps a second of 28
            // degrees and more. What the mouse turned the view by in this frame is no cut.
            double mouseTurn = 0.0;
            if (const uint64_t tag = ps2x::asyncvif::currentJobTag(); tag != 0u)
                if (LookFrame peek; takeLookFrame(tag, peek))
                    mouseTurn = std::hypot(peek.yaw, peek.pitch);
            if (angle - mouseTurn > 0.5 || angle > 2.4 || moved > 2000.0)
            {
                ++s.cuts;
                showFrame(); // a cut (a door, a cutscene's camera change): nothing sensible lies between
                if (recording)
                    record += "R the frame's own picture for all (a camera cut)\n";
                keepRecord(record);
                return;
            }
            if (recording)
            {
                char text[256];
                std::snprintf(text, sizeof(text), "C camera turned %.4f rad, moved %.2f; eye before %.1f %.1f %.1f, now %.1f %.1f %.1f; %zu view and %zu projected uploads\n",
                              angle, moved, eyeA[0], eyeA[1], eyeA[2], eyeB[0], eyeB[1], eyeB[2], scene.view.size(), scene.projected.size());
                record += text;
            }
            const bool cameraMoved = !(angle < 1e-5 && moved < 1e-3);
            // Live look: the frame the game thread tagged this job with. Its pictures get their cameras at the last
            // moment (lateCamera), also when nothing in the scene moved, as long as the mouse is or was just moving.
            LateLook &late = g_late;
            bool liveLook = false;  // the frame's pictures get their cameras at the last moment (mouse and keyboard play)
            bool mouseBusy = false; // the mouse is turning the view, or just was: pictures also when nothing else moves
            {
                static const bool selftestOn = [] { const char *v = std::getenv("STAR_SMOOTH_SELFTEST"); return v && *v == '1'; }();
                // (With the "low delay" setting off the pictures still go this way while the mouse turns the view:
                // their cameras are the in-between ones, kept within the margins of the "in view" test.)
                late.ahead = live().liveLook.load(std::memory_order_relaxed);
                // where the frame before's limits ended, if this frame follows it
                const bool follows = late.margins && late.look.seq != 0u && late.look.seq + 1u == ps2x::asyncvif::currentJobTag();
                if (follows)
                {
                    late.beforeLow[0] = -late.yawNeg - late.look.yaw;
                    late.beforeHigh[0] = late.yawPos - late.look.yaw;
                    late.beforeLow[1] = -late.pitchNeg - late.look.pitch;
                    late.beforeHigh[1] = late.pitchPos - late.look.pitch;
                }
                late.before = follows;
                late.margins = false;
                late.taken = false;
                late.lastYaw = late.lastPitch = 0.0;
                if (!follows)
                {
                    late.lastOn[0] = late.lastOn[1] = 0.0;
                    late.lastShown = 0;
                }
                if (const uint64_t tag = ps2x::asyncvif::currentJobTag(); tag != 0u && !selftestOn && takeLookFrame(tag, late.look))
                {
                    // Every frame of play with the mouse and keyboard goes this way, also one the mouse does not
                    // turn: its own run is its first picture then (one pass fewer), and the count of passes does
                    // not change with the mouse starting and stopping (the rate of pictures is chosen by it).
                    liveLook = true;
                    mouseBusy = late.look.yaw != 0.0 || late.look.pitch != 0.0 ||
                                (late.ahead && late.look.perCountYaw != 0.0 && now - lookMouseMovedNs() < 250000000ll);
                    // the margins the frame's scenery was chosen with: set at the input read after this frame's
                    LookFrame next;
                    const bool haveNext = takeLookFrame(tag + 1u, next);
                    late.taken = haveNext && next.perCountYaw != 0.0 && !next.last;
                    late.lastYaw = haveNext && next.last ? next.yaw : 0.0;
                    late.lastPitch = haveNext && next.last ? next.pitch : 0.0;
                    late.margins = haveNext &&
                                   (next.marginYawNeg > 0.0 || next.marginYawPos > 0.0 || next.marginPitchNeg > 0.0 || next.marginPitchPos > 0.0);
                    const double spare = 0.5 / 57.29578;
                    late.yawNeg = late.margins ? std::max(0.0, next.marginYawNeg - spare) : 0.0;
                    late.yawPos = late.margins ? std::max(0.0, next.marginYawPos - spare) : 0.0;
                    late.pitchNeg = late.margins ? std::max(0.0, next.marginPitchNeg - spare) : 0.0;
                    late.pitchPos = late.margins ? std::max(0.0, next.marginPitchPos - spare) : 0.0;
                    if (recording && mouseBusy)
                    {
                        char text[200];
                        std::snprintf(text, sizeof(text), "L look frame %llu: the mouse turned the view %.2f %.2f degrees; margins %.1f %.1f %.1f %.1f%s; lead %.1f ms%c",
                                      static_cast<unsigned long long>(tag), late.look.yaw * 57.29578, late.look.pitch * 57.29578, late.yawNeg * 57.29578,
                                      late.yawPos * 57.29578, late.pitchNeg * 57.29578, late.pitchPos * 57.29578, late.ahead ? "" : " (low delay off)", late.leadMs, 10);
                        record += text;
                    }
                }
                if (late.frameGapMs > 0.0 && !late.fixedLead)
                {
                    // the lead follows the longest wait of the frame before: up quickly, down slowly
                    const double wanted = late.frameGapMs + 1.5;
                    late.leadMs += (wanted > late.leadMs ? 0.3 : 0.02) * (wanted - late.leadMs);
                }
                if (static const bool when = [] { const char *e = std::getenv("STAR_VIEW_WHEN"); return e && *e == '1'; }(); when)
                    std::printf("[when] job tagged %llu: drawn with the view %.2f%c", static_cast<unsigned long long>(ps2x::asyncvif::currentJobTag()),
                                std::atan2(current.m[0][2], current.m[2][2]) * 57.29578, 10);
                if (late.log && liveLook)
                {
                    // STAR_LIVE_LOG=1: which signs of the two turns lead from the frame before's camera to this one
                    // (the smallest difference; with the mouse as the only thing turning the view), and the timing.
                    double best = 1e9;
                    int bestSigns = 0;
                    for (int signs = 0; signs < 4; ++signs)
                    {
                        const Mat4 guess = viewTurnedRaw(previous, (signs & 1 ? -1.0 : 1.0) * late.look.yaw, (signs & 2 ? -1.0 : 1.0) * late.look.pitch);
                        double worst = 0.0;
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c)
                                worst = std::max(worst, std::fabs(guess.m[r][c] - current.m[r][c]));
                        if (worst < best)
                        {
                            best = worst;
                            bestSigns = signs;
                        }
                    }
                    static uint64_t misfits = 0;
                    if (best > 2e-4)
                        ++misfits; // (a test with the mouse as the only thing that turns the view: should stay 0)
                    if (++late.frames % 10u == 1u)
                        std::printf("[live] frame %llu: mouse turn %+.5f %+.5f, camera turned %.5f; best signs yaw %c pitch %c (off by %.5f); lead %.1f ms, "
                                    "longest wait %.1f ms, furthest ahead %.1f degrees, %zu slots, %llu misfits so far%c",
                                    static_cast<unsigned long long>(late.look.seq), late.look.yaw, late.look.pitch, angle, bestSigns & 1 ? '-' : '+',
                                    bestSigns & 2 ? '-' : '+', best, late.leadMs, late.frameGapMs, late.frameAhead * 57.29578, slots.size(), static_cast<unsigned long long>(misfits), 10);
                }
                late.frameGapMs = 0.0;
                late.frameAhead = 0.0;
            }
            struct Move
            {
                Upload where;
                Between between;
                bool attached = false;
            };
            std::vector<Move> moves;
            struct Stuck
            {
                Upload where;
                std::array<float, 16> matrix;
                bool point = false; // a sprite of one of the player's effects: 64 bytes of the stream from its place on
            };
            // What goes with the camera and has no in-between matrix: it did not move in the world, or it is new in
            // this frame (a muzzle flash). Shown as the game placed it for the frame's own camera.
            std::vector<Stuck> stuck;
            // What goes with the camera: the player's own body, arms and weapon. The game thread tells which they are
            // (game_fixes.cpp, noteOwnPiece: the job's note holds their matrices, 64 bytes each, as the stream does).
            // (Before: found out from how things moved, and for a while from how near the eye they were; both went
            // wrong: an animation hid the answer, and the user's shrine orb was near and moving.)
            std::vector<uint8_t> attachedNow(scene.objects.size(), 0u);
            {
                const std::vector<uint8_t> &note = ps2x::asyncvif::currentJobNote();
                const size_t count = note.size() / 64u;
                for (size_t i = 0; i < scene.objects.size() && count != 0u; ++i)
                {
                    if (scene.objects[i].vertexBytes == 0u)
                        continue;
                    for (size_t n = 0; n < count; ++n)
                        if (std::memcmp(note.data() + n * 64u, scene.objects[i].a, 64) == 0)
                        {
                            attachedNow[i] = 1u;
                            break;
                        }
                }
            }
            if (live().moveObjects.load(std::memory_order_relaxed) && !previousObjects.empty())
            {
                std::vector<uint8_t> used(previousObjects.size(), 0u);
                // STAR 2026-11-24. First every object that has not moved takes its own twin of the frame before (the
                // same model with the very same matrices). Without this, where a new area comes into view (the hub's
                // door to the right walkway: 46 objects one frame, 180 the next) a newly drawn wall piece took the twin
                // of a piece that stood still, and that piece was then paired with another copy of the model 1,000 to
                // 13,000 units away: the in-between pictures slid walls out of place for one frame (the user: models
                // flicker at a threshold; STAR_SMOOTH_MOVE_LOG=1 shows every pairing that moved, with its distance).
                std::vector<uint8_t> settled(scene.objects.size(), 0u);
                for (size_t i = 0; i < scene.objects.size(); ++i)
                {
                    const Object &object = scene.objects[i];
                    if (object.vertexBytes == 0u)
                        continue;
                    for (size_t j = 0; j < previousObjects.size(); ++j)
                    {
                        const Object &candidate = previousObjects[j];
                        if (!used[j] && candidate.key == object.key && candidate.vertexBytes == object.vertexBytes &&
                            std::memcmp(candidate.a, object.a, 64) == 0 && candidate.sameMore(object))
                        {
                            used[j] = 1u;
                            settled[i] = 1u;
                            break;
                        }
                    }
                }
                // Then the ones that moved: the nearest twin left, if it is near enough to be the same object. Normal
                // movement is under 16 units a frame (99% of 18,915 pairings); farther than the limit it is another
                // copy of the model, and the object is drawn where the game put it (STAR_SMOOTH_MOVE_LIMIT, 300).
                static const double moveLimit = []
                { const char *v = std::getenv("STAR_SMOOTH_MOVE_LIMIT"); return v && *v ? std::atof(v) : 300.0; }();
                // STAR 2026-11-24 (the user's "large triangles" flicker, caught with the F1 recorder: two in-between
                // pictures covered by huge see-through triangles). A model with many copies that all move every frame
                // is a set of particles: behind the Chasm Nexus door one small model (448 bytes of vertices) is drawn
                // 10 times, each copy about 95 units (up to 191) from where any copy was the frame before, while every
                // other moving model has one copy and moves under 21. Paired by "the nearest copy" each particle slid
                // between two unrelated places, and one that slid through the camera's near plane was drawn across the
                // whole screen. Which copy was which cannot be known, so:
                //   - a model with three or more moved copies in the frame is not moved in between at all (its copies
                //     step with the game's frames, as they do without smooth motion);
                //   - with fewer, a pairing counts only when the nearest copy is clearly the nearest (under half the
                //     distance of the next one).
                // STAR_SMOOTH_PARTICLES=1: as before (for comparing).
                static const bool pairParticles = []
                { const char *v = std::getenv("STAR_SMOOTH_PARTICLES"); return v && *v == '1'; }();
                const auto sameModel = [](const Object &a, const Object &b)
                { return a.key == b.key && a.vertexBytes == b.vertexBytes && a.moreMask == b.moreMask; };
                const auto apart = [](const Object &a, const Object &b)
                {
                    double distance = 0.0;
                    for (int c = 12; c < 15; ++c)
                        distance += (static_cast<double>(a.a[c]) - b.a[c]) * (static_cast<double>(a.a[c]) - b.a[c]);
                    return distance; // squared
                };
                std::vector<int> pairOf(scene.objects.size(), -1);
                std::vector<uint8_t> byView(scene.objects.size(), 0u);
                // (a) Things that go with the camera (the player's own body, and behind the Chasm Nexus door ten copies
                // of one model 1,750-4,400 units out that turn with the view: light shafts by the look of the user's
                // GIF). In the world they move every frame, as far as their copies are apart, so "the nearest copy" pairs
                // them wrongly; seen from the camera they stand still. An object whose place in the view of the frame
                // before matches a copy's place in this frame's view (within 8 units, and clearly the nearest) is that
                // copy.
                if (!pairParticles)
                {
                    const auto inView = [](const Mat4 &view, const float *m, double out[3])
                    {
                        for (int c = 0; c < 3; ++c)
                            out[c] = m[12] * view.m[0][c] + m[13] * view.m[1][c] + m[14] * view.m[2][c] + view.m[3][c];
                    };
                    for (size_t i = 0; i < scene.objects.size(); ++i)
                    {
                        const Object &object = scene.objects[i];
                        if (object.vertexBytes == 0u || settled[i])
                            continue;
                        double mine[3];
                        inView(current, object.a, mine);
                        int best = -1;
                        double bestDistance = 0.0, nextDistance = -1.0;
                        for (size_t j = 0; j < previousObjects.size(); ++j)
                        {
                            const Object &candidate = previousObjects[j];
                            if (used[j] || !sameModel(candidate, object))
                                continue;
                            double theirs[3];
                            inView(previous, candidate.a, theirs);
                            const double distance = (theirs[0] - mine[0]) * (theirs[0] - mine[0]) + (theirs[1] - mine[1]) * (theirs[1] - mine[1]) +
                                                    (theirs[2] - mine[2]) * (theirs[2] - mine[2]);
                            if (best < 0 || distance < bestDistance)
                            {
                                nextDistance = best < 0 ? -1.0 : bestDistance;
                                best = static_cast<int>(j);
                                bestDistance = distance;
                            }
                            else if (nextDistance < 0.0 || distance < nextDistance)
                                nextDistance = distance;
                        }
                        if (best >= 0 && bestDistance <= 64.0 && (nextDistance < 0.0 || bestDistance < 0.25 || bestDistance * 16.0 <= nextDistance))
                        {
                            pairOf[i] = best;
                            byView[i] = 1u;
                            used[static_cast<size_t>(best)] = 1u;
                        }
                    }
                }
                // (b) The rest, by their place in the world.
                // STAR 2026-11-24 (the user's flicker, found with two F1 catches). The light streaks on the walls of the
                // chamber behind the Chasm Nexus door are one model set around the room many times, 15 degrees apart,
                // and the game draws now this copy, now that one. In a frame where one copy went and its neighbour came,
                // the newcomer was paired with the one that went ("moved 12 to 25 units") and turned from one to the
                // other in between: for that frame a sheet of light swept through the room, past the camera when it lay
                // in the way (both catches hold such a pairing in the bad frame; STAR_SMOOTH_MOVE_LOG: turned 14.9
                // degrees). Two rules against it:
                //   - a copy that stood still in the frame before is scenery: it is never "the same object, moved". A
                //     thing that starts to move is drawn where the game puts it for that one frame and moved from then on;
                //   - nothing is paired with a copy turned more than 20 degrees from it (STAR_SMOOTH_TURN_LIMIT).
                static const double turnLimit = []
                { const char *v = std::getenv("STAR_SMOOTH_TURN_LIMIT"); return v && *v ? std::atof(v) : 20.0; }();
                const auto turned = [](const Object &a, const Object &b)
                {
                    // the largest angle between an axis of the one and the same axis of the other, in degrees
                    double most = 0.0;
                    for (int r = 0; r < 3; ++r)
                    {
                        double dot = 0.0, la = 0.0, lb = 0.0;
                        for (int c = 0; c < 3; ++c)
                        {
                            dot += static_cast<double>(a.a[r * 4 + c]) * b.a[r * 4 + c];
                            la += static_cast<double>(a.a[r * 4 + c]) * a.a[r * 4 + c];
                            lb += static_cast<double>(b.a[r * 4 + c]) * b.a[r * 4 + c];
                        }
                        if (la > 0.0 && lb > 0.0)
                            most = std::max(most, std::acos(std::clamp(dot / std::sqrt(la * lb), -1.0, 1.0)) * 57.29578);
                    }
                    return most;
                };
                std::vector<uint8_t> loose(scene.objects.size(), 0u);
                for (size_t i = 0; i < scene.objects.size(); ++i)
                    loose[i] = scene.objects[i].vertexBytes != 0u && !settled[i] && pairOf[i] < 0;
                for (size_t i = 0; i < scene.objects.size(); ++i)
                {
                    const Object &object = scene.objects[i];
                    if (!loose[i])
                        continue;
                    // STAR 2026-11-24 (the user: "the sword swinging looks like it stutters"). A model that is in the
                    // frame once and was in the frame before once is that same thing, however far it moved or turned:
                    // the rules below are there to tell copies of one model apart, and they left about 13 pieces of
                    // the swinging arm and blade unpaired (over 8 units in the view, over 20 degrees a frame), which
                    // then stepped 30 times a second in a picture that moves 144 times. A limit on the way all the
                    // same: another creature of the kind coming into view as the first leaves is not "it, moved".
                    if (!pairParticles)
                    {
                        size_t now1 = 0, before1 = 0;
                        int only = -1;
                        for (size_t k = 0; k < scene.objects.size() && now1 < 2u; ++k)
                            if (scene.objects[k].vertexBytes != 0u && sameModel(scene.objects[k], object))
                                ++now1;
                        for (size_t j = 0; j < previousObjects.size() && before1 < 2u; ++j)
                            if (sameModel(previousObjects[j], object))
                            {
                                ++before1;
                                only = static_cast<int>(j);
                            }
                        if (now1 == 1u && before1 == 1u && !used[static_cast<size_t>(only)])
                        {
                            const Object &was = previousObjects[static_cast<size_t>(only)];
                            double inView = 0.0;
                            for (int c = 0; c < 3; ++c)
                            {
                                const double a = was.a[12] * previous.m[0][c] + was.a[13] * previous.m[1][c] + was.a[14] * previous.m[2][c] + previous.m[3][c];
                                const double b = object.a[12] * current.m[0][c] + object.a[13] * current.m[1][c] + object.a[14] * current.m[2][c] + current.m[3][c];
                                inView += (a - b) * (a - b);
                            }
                            if (std::min(apart(was, object), inView) <= 150.0 * 150.0)
                            {
                                pairOf[i] = only;
                                used[static_cast<size_t>(only)] = 1u;
                                continue;
                            }
                        }
                    }
                    if (!pairParticles)
                    {
                        size_t movedCopies = 0;
                        for (size_t k = 0; k < scene.objects.size() && movedCopies < 3u; ++k)
                            if (loose[k] && sameModel(scene.objects[k], object))
                                ++movedCopies;
                        if (movedCopies >= 3u)
                            continue;
                    }
                    int best = -1;
                    double bestDistance = 0.0, nextDistance = -1.0;
                    for (size_t j = 0; j < previousObjects.size(); ++j)
                    {
                        const Object &candidate = previousObjects[j];
                        if (used[j] || !sameModel(candidate, object))
                            continue;
                        if (!pairParticles && (candidate.still || turned(candidate, object) > turnLimit))
                            continue;
                        const double distance = apart(candidate, object);
                        if (best < 0 || distance < bestDistance)
                        {
                            nextDistance = best < 0 ? -1.0 : bestDistance;
                            best = static_cast<int>(j);
                            bestDistance = distance;
                        }
                        else if (nextDistance < 0.0 || distance < nextDistance)
                            nextDistance = distance;
                    }
                    if (best < 0 || bestDistance > moveLimit * moveLimit)
                        continue;
                    // distances are squared here: "under half" is "under a quarter"
                    if (!pairParticles && nextDistance >= 0.0 && bestDistance > 1.0 && bestDistance * 4.0 > nextDistance)
                        continue;
                    pairOf[i] = best;
                    used[static_cast<size_t>(best)] = 1u;
                }
                for (size_t i = 0; i < scene.objects.size() && i < s.previousObjects.size(); ++i)
                    s.previousObjects[i].still = settled[i] != 0u;
                if (recording)
                {
                    // every object: s = stood still, v = paired by its place in the view, p = by its place in the world,
                    // - = not paired (drawn where the game put it); its place, and how far from the paired one's
                    for (size_t i = 0; i < scene.objects.size(); ++i)
                    {
                        const Object &object = scene.objects[i];
                        if (object.vertexBytes == 0u)
                            continue;
                        char text[200];
                        const char how = settled[i] ? 's' : pairOf[i] < 0 ? '-' : byView[i] ? 'v' : 'p';
                        // (+2, +3, +4: a part skinned to that many bones)
                        char bones[8] = "";
                        if (object.bones() > 1)
                            std::snprintf(bones, sizeof(bones), "+%d", object.bones());
                        std::snprintf(text, sizeof(text), "O %zu %016llx %u%s %c%s at %.1f %.1f %.1f", i, static_cast<unsigned long long>(object.key),
                                      object.vertexBytes, bones, how, attachedNow[i] ? "a" : "", object.a[12], object.a[13], object.a[14]);
                        record += text;
                        if (pairOf[i] >= 0)
                        {
                            std::snprintf(text, sizeof(text), " moved %.2f turned %.1f", std::sqrt(apart(previousObjects[static_cast<size_t>(pairOf[i])], object)),
                                          turned(previousObjects[static_cast<size_t>(pairOf[i])], object));
                            record += text;
                        }
                        record += "\n";
                    }
                }
                for (size_t i = 0; i < scene.objects.size(); ++i)
                {
                    if (pairOf[i] < 0)
                        continue;
                    const Object &object = scene.objects[i];
                    const int best = pairOf[i];
                    const double bestDistance = apart(previousObjects[static_cast<size_t>(best)], object);
                    // STAR_SMOOTH_MOVE_LOG=1 (a diagnostic): every paired object that moved, with how far.
                    {
                        static const bool moveLog = [] { const char *v = std::getenv("STAR_SMOOTH_MOVE_LOG"); return v && *v == '1'; }();
                        if (moveLog && bestDistance > 0.0)
                        {
                            // how near the straight path from the old place to the new one comes to the camera
                            const float *from = previousObjects[static_cast<size_t>(best)].a;
                            double ab[3], ae[3], abab = 0.0, abae = 0.0;
                            for (int c = 0; c < 3; ++c)
                            {
                                ab[c] = static_cast<double>(object.a[12 + c]) - from[12 + c];
                                ae[c] = eyeB[c] - from[12 + c];
                                abab += ab[c] * ab[c];
                                abae += ab[c] * ae[c];
                            }
                            const double u = abab > 0.0 ? std::clamp(abae / abab, 0.0, 1.0) : 0.0;
                            double nearest = 0.0;
                            for (int c = 0; c < 3; ++c)
                                nearest += (ae[c] - u * ab[c]) * (ae[c] - u * ab[c]);
                            // how far its axes turned (the largest angle between an axis before and after) and how long they are
                            double turned = 0.0, size = 0.0;
                            for (int r = 0; r < 3; ++r)
                            {
                                double dot = 0.0, la = 0.0, lb = 0.0;
                                for (int c = 0; c < 3; ++c)
                                {
                                    dot += static_cast<double>(from[r * 4 + c]) * object.a[r * 4 + c];
                                    la += static_cast<double>(from[r * 4 + c]) * from[r * 4 + c];
                                    lb += static_cast<double>(object.a[r * 4 + c]) * object.a[r * 4 + c];
                                }
                                if (la > 0.0 && lb > 0.0)
                                    turned = std::max(turned, std::acos(std::clamp(dot / std::sqrt(la * lb), -1.0, 1.0)) * 57.29578);
                                size = std::max(size, std::sqrt(lb));
                            }
                            std::printf("[move] frame %llu key %016llx bytes %u moved %.2f (of %zu objects, %zu before) passes the camera at %.1f, paired by %s, turned %.1f degrees, size %.1f%c",
                                        static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(object.key), object.vertexBytes,
                                        std::sqrt(bestDistance), scene.objects.size(), previousObjects.size(), std::sqrt(nearest), byView[i] ? "view" : "place", turned, size, 10);
                        }
                    }
                    const Object &before = previousObjects[static_cast<size_t>(best)];
                    Move move;
                    move.attached = attachedNow[i] != 0u;
                    if (std::memcmp(before.a, object.a, 64) != 0 && move.between.set(before.a, object.a))
                    {
                        move.where = object.first;
                        moves.push_back(move);
                    }
                    for (int k = 0; k < kMoreBones; ++k)
                        if ((object.moreMask & (1u << k)) != 0u && std::memcmp(before.m[k], object.m[k], 64) != 0 &&
                            move.between.set(before.m[k], object.m[k]))
                        {
                            move.where = object.more[k];
                            moves.push_back(move);
                        }
                }
            }
            {
                // STAR_SMOOTH_MOVE_LOG=1: per frame, the pictures' ids, how far the camera went, and how many objects of
                // the frame before are not drawn any more (by model: before minus now, where positive).
                static const bool frameLog = [] { const char *v = std::getenv("STAR_SMOOTH_MOVE_LOG"); return v && *v == '1'; }();
                if (frameLog && !slots.empty())
                {
                    std::vector<uint64_t> before, now2;
                    for (const Object &o : previousObjects)
                        if (o.vertexBytes)
                            before.push_back(o.key);
                    for (const Object &o : scene.objects)
                        if (o.vertexBytes)
                            now2.push_back(o.key);
                    std::sort(before.begin(), before.end());
                    std::sort(now2.begin(), now2.end());
                    std::vector<uint64_t> gone, come;
                    std::set_difference(before.begin(), before.end(), now2.begin(), now2.end(), std::back_inserter(gone));
                    std::set_difference(now2.begin(), now2.end(), before.begin(), before.end(), std::back_inserter(come));
                    std::printf("[frame] %llu ids %u-%u angle %.4f moved %.1f objects %zu before %zu gone %zu come %zu%c",
                                static_cast<unsigned long long>(s.frames), slots.front().id, slots.back().id, angle, moved, now2.size(),
                                before.size(), gone.size(), come.size(), 10);
                }
            }
            if (!cameraMoved && moves.empty() && !mouseBusy)
            {
                ++s.still;
                showFrame(); // nothing moved: every picture would be the same
                if (recording)
                    record += "R the frame's own picture for all (nothing moved)\n";
                keepRecord(record);
                return;
            }
            ++s.moving;
            if (recording)
            {
                char text[96];
                std::snprintf(text, sizeof(text), "R passes; %zu matrices moved in between\n", moves.size());
                record += text;
            }
            keepRecord(record);

            // The passes repeat the frame's texture uploads. Leaving them out (STAR_SMOOTH_SKIP_UPLOADS=1, an experiment)
            // is wrong: the game loads different textures to the same place in GS memory within one frame, so a pass
            // without the uploads draws most of the scene with whatever was loaded last (STAR 2026-11-24: the user saw
            // broken textures everywhere; STAR_SMOOTH_SELFTEST found it). It also saved no measurable time.
            static const bool skipUploads = []
            { const char *v = std::getenv("STAR_SMOOTH_SKIP_UPLOADS"); return v && *v == '1'; }();
            if (skipUploads)
                plan.blanks = scene.uploads;
            const Mat4 inverse = inverseView(current);
            if (liveLook || cameraMoved)
            {
                // The sprites of the player's effects (game_fixes.cpp, listOwnEffects): the note holds their places,
                // marked with kOwnPointMark. In the stream a sprite is its 0x30-byte record as the game keeps it,
                // sent with the code 0x6c03c002 (three quad words to the stream's own base + 2) and drawn by the
                // program at 0x4ee: the place in the world first.
                {
                    const std::vector<uint8_t> &note = ps2x::asyncvif::currentJobNote();
                    std::vector<std::array<uint32_t, 3>> places;
                    for (size_t n = 0; n + 64u <= note.size(); n += 64u)
                    {
                        uint32_t words[16];
                        std::memcpy(words, note.data() + n, 64);
                        bool point = true;
                        for (int k = 3; k < 16; ++k)
                            point = point && words[k] == kOwnPointMark;
                        if (point)
                            places.push_back({words[0], words[1], words[2]});
                    }
                    for (size_t i = 0; i < items.size() && !places.empty(); ++i)
                    {
                        if (items[i].kind != ps2x::asyncvif::ItemKind::Vif1 || items[i].data.size() < 4096u)
                            continue;
                        const uint8_t *data = items[i].data.data();
                        const size_t size = items[i].data.size();
                        for (size_t q = 4u; q + 64u <= size; q += 4u)
                        {
                            uint32_t code = 0;
                            std::memcpy(&code, data + q - 4u, 4);
                            if (code != 0x6C03C002u)
                                continue;
                            for (const std::array<uint32_t, 3> &place : places)
                                if (std::memcmp(data + q, place.data(), 12) == 0)
                                {
                                    Stuck one;
                                    one.where.item = i;
                                    one.where.offset = q;
                                    std::memcpy(one.matrix.data(), data + q, 64);
                                    one.point = true;
                                    stuck.push_back(one);
                                    break;
                                }
                        }
                    }
                }
                // and the pieces: their matrices as they are
                for (size_t i = 0; i < scene.objects.size(); ++i)
                {
                    if (!attachedNow[i])
                        continue;
                    const Object &object = scene.objects[i];
                    const auto moving = [&](const Upload &where)
                    {
                        for (const Move &move : moves)
                            if (move.where.item == where.item && move.where.offset == where.offset)
                                return true;
                        return false;
                    };
                    Stuck one;
                    if (!moving(object.first))
                    {
                        one.where = object.first;
                        std::memcpy(one.matrix.data(), object.a, 64);
                        stuck.push_back(one);
                    }
                    for (int k = 0; k < kMoreBones; ++k)
                        if ((object.moreMask & (1u << k)) != 0u && !moving(object.more[k]))
                        {
                            one.where = object.more[k];
                            std::memcpy(one.matrix.data(), object.m[k], 64);
                            stuck.push_back(one);
                        }
                }
            }
            const bool cameraPatched = cameraMoved || liveLook;
            const size_t cameraPatches = cameraPatched ? scene.view.size() + scene.projected.size() * 2u : 0u;
            plan.passes.reserve(slots.size());
            late.passes.clear();
            late.originals.clear();
            if (liveLook)
            {
                late.inverse = inverse;
                for (const Upload &u : scene.view)
                    late.originals.push_back(load(items[u.item].data.data() + u.offset));
                for (const Upload &u : scene.projected)
                {
                    late.originals.push_back(load(items[u.item].data.data() + u.offset));
                    late.originals.push_back(load(items[u.item].data.data() + u.offset + 64u));
                }
                plan.beforePass = &lateCamera;
            }
            late.ownFirst = false;
            // STAR_SMOOTH_SELFTEST=1 (a check of the machinery): the first picture of a frame is the game's own, the others
            // are passes placed at the frame's own state (t = 1), so all of a frame's pictures must be identical. Their
            // ids are printed; tools\selftest.py compares the presented pictures (PS2X_PCSX2_PRESENT_DUMP).
            static const bool selftest = []
            { const char *v = std::getenv("STAR_SMOOTH_SELFTEST"); return v && *v == '1'; }();
            if (selftest)
            {
                std::string ids;
                for (const Slot &slot : slots)
                    ids += ' ' + std::to_string(slot.id);
                std::printf("[selftest]%s%c", ids.c_str(), 10);
            }
            for (Slot slot : slots)
            {
                // STAR_SMOOTH_TEST_T=<t> (a test): every pass at this time fraction instead of its own. With the self-test
                // and t = 0 a frame's pass is drawn from the camera of the frame before, so it must show what that
                // frame's own picture showed: toolsacktest.py compares the two and finds what a pass cannot draw
                // (pieces the game left out or cut for the frame's own camera).
                static const double testT = [] { const char *v = std::getenv("STAR_SMOOTH_TEST_T"); return v && *v ? std::atof(v) : 9.0; }();
                if (selftest)
                    slot.t = slot.id == slots.front().id ? 2.0 : (testT < 8.0 ? testT : 1.0);
                else if (testT < 8.0 && slot.t <= 0.9995)
                    slot.t = testT;
                if (!liveLook && (selftest ? slot.t > 1.5 : slot.t > 0.9995))
                {
                    plan.shown.push_back(slot.id); // this refresh falls on the frame's own state
                    continue;
                }
                ps2x::asyncvif::FramePass pass;
                pass.ids.push_back(slot.id);
                pass.patches.resize(cameraPatches + moves.size() + stuck.size());
                Mat4 nowThroughAt{}; // without the live view: what was placed for the frame's camera, seen through the camera at t
                for (int k = 0; k < 4; ++k)
                    nowThroughAt.m[k][k] = 1.0;
                size_t n = 0;
                if (cameraPatched)
                {
                    // The camera at t: the rotation along the arc between the two, the eye along the line.
                    Mat4 at{};
                    setRotation(at, slerp(qa, qb, slot.t));
                    at.m[3][3] = 1.0;
                    for (int c = 0; c < 3; ++c)
                    {
                        double translation = 0.0;
                        for (int r = 0; r < 3; ++r)
                            translation -= ((1.0 - slot.t) * eyeA[r] + slot.t * eyeB[r]) * at.m[r][c];
                        at.m[3][c] = translation;
                    }
                    if (liveLook)
                        late.passes.push_back({at, slot.t, slot.at, {}});
                    else
                        nowThroughAt = mul(current, inverseView(at));
                    const Mat4 d = mul(at, inverse); // world -> world: seen through `current` it looks like `at`
                    const auto patch = [&](const Upload &u, size_t offset)
                    {
                        ps2x::asyncvif::FramePatch &p = pass.patches[n++];
                        p.item = static_cast<uint32_t>(u.item);
                        p.offset = static_cast<uint32_t>(u.offset + offset);
                        store(mul(d, load(items[u.item].data.data() + u.offset + offset)), p.bytes);
                    };
                    for (const Upload &u : scene.view)
                        patch(u, 0u);
                    for (const Upload &u : scene.projected)
                    {
                        patch(u, 0u);  // q0-q3
                        patch(u, 64u); // q4-q7
                    }
                }
                for (const Move &move : moves)
                {
                    ps2x::asyncvif::FramePatch &p = pass.patches[n++];
                    p.item = static_cast<uint32_t>(move.where.item);
                    p.offset = static_cast<uint32_t>(move.where.offset);
                    move.between.at(slot.t, p.bytes);
                    if (liveLook && move.attached && !late.passes.empty())
                    {
                        LateMatrix matrix;
                        matrix.patch = n - 1u;
                        std::memcpy(matrix.base.data(), p.bytes, 64);
                        late.passes.back().attached.push_back(matrix);
                    }
                }
                for (const Stuck &one : stuck)
                {
                    ps2x::asyncvif::FramePatch &p = pass.patches[n++];
                    p.item = static_cast<uint32_t>(one.where.item);
                    p.offset = static_cast<uint32_t>(one.where.offset);
                    if (liveLook)
                    {
                        std::memcpy(p.bytes, one.matrix.data(), 64);
                        if (!late.passes.empty())
                            late.passes.back().attached.push_back({n - 1u, one.matrix, true, one.point});
                    }
                    else
                        throughCamera(one.matrix, nowThroughAt, one.point, p.bytes);
                }
                // STAR 2026-11-25: with the mouse turning the view every picture is drawn for its own camera, also
                // the first, so the game's own run can BE the first (it runs first, and its picture is the first
                // due): its matrices are patched like a pass's, and the frame needs one pass fewer. (Without this the
                // own run was drawn for nothing: at 144 Hz six runs a frame for five pictures. Measured on the
                // user's PC in a fight, F1 catch pictures_5: frames in which only 3 of 5 passes got done.)
                static const bool ownAlone = [] { const char *v = std::getenv("STAR_LIVE_OWN"); return v && *v == '0'; }();
                if (liveLook && !selftest && !ownAlone && plan.own.patches.empty() && plan.passes.empty() && plan.shown.empty() && !pass.patches.empty())
                {
                    plan.shown.push_back(slot.id);
                    plan.own = std::move(pass);
                    plan.beforeOwn = &lateCameraOwn;
                    late.ownFirst = true;
                    continue;
                }
                plan.passes.push_back(std::move(pass));
            }
        }
    }

    void installFrameDouble(PS2Runtime &runtime)
    {
        // Always installed: star::live().smooth60 (STAR_60FPS / the settings file at start, the F2 menu while running)
        // says whether in-between pictures are drawn. STAR_FRAME_DOUBLE=0 leaves the whole mechanism out (the GS then
        // presents at the emulated vsync, the frame the game displays).
        if (const char *off = std::getenv("STAR_FRAME_DOUBLE"); off && *off == '0')
        {
            std::printf("[smooth] not installed (STAR_FRAME_DOUBLE=0)\n");
            return;
        }
        State &s = g_state;
        s.runtime = &runtime;
        if (const char *log = std::getenv("STAR_60FPS_LOG"); log && *log == '1')
            s.log = true;
        if (const char *margin = std::getenv("STAR_SMOOTH_MARGIN_MS"); margin && *margin)
        {
            s.marginNs = static_cast<long long>(std::atof(margin) * 1e6);
            s.marginFixed = true;
        }
        if (const char *most = std::getenv("STAR_SMOOTH_MAX_PER_FRAME"); most && *most)
            s.maxPerFrame = std::clamp(std::atoi(most), 1, 16);
        if (const char *log = std::getenv("STAR_LIVE_LOG"); log && *log == '1')
            g_late.log = true;
        if (const char *lead = std::getenv("STAR_LIVE_LEAD_MS"); lead && *lead)
        {
            g_late.leadMs = std::clamp(std::atof(lead), 0.0, 100.0);
            g_late.fixedLead = true;
        }
        if (const char *share = std::getenv("STAR_SMOOTH_WORK_SHARE"); share && *share)
            s.workShare = std::clamp(std::atof(share), 0.1, 1.5);
        enableFineTimers();
        long long period = 0, refresh = 0;
        const bool known = screenRefresh(period, refresh);
        ps2x::asyncvif::setFrameDoubler(&prepare);
        g_ps2xLastPicturesHook = &writeHistory;
        std::printf("[smooth] pictures for the screen's refresh rate: %s; screen %s%.2f Hz\n",
                    live().smooth60.load() ? "on" : "off (the game's 30 a second; can be switched on in the F2 menu)",
                    known ? "" : "not known, taken as ", known ? 1e9 / static_cast<double>(period) : 60.0);
    }
}
