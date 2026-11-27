// STAR 2026-11-25: creature shadow volumes near the camera, and stopped at walls. See shadow_volumes.h.
//
// VU1 program 0x678 (micro memory 0x33c0..0x3c30; tools/vu/vudis.py of build/vu1_capture), per batch (one piece):
//   input at TOP: 3 qwords per triangle, each corner x, y, z as 12.4 integers and the bone number in w (bits 0-15;
//   bone matrix at qword 24 + 4 n, row vectors: p * M);
//   header at TOP + 0x121 (FUN_001323c0, 2 x 17 qwords, double-buffered output): q0 GIF tag (PACKED, PRE, PRIM 4 =
//   triangle strip, 16 registers: XYZF2 and A+D), q1.x = triangles - 1, q2 = viewport centre (1728, 1824),
//   q3 / q5 = A+D PRMODE (0x1b) 0x240 (context 2: ALPHA_2 = 0xa1, subtract) / 0x40 (context 1: ALPHA_1 = 0x68, add),
//   q4 = viewport scale (shadow buffer / 640x448), q16 = light direction x length (w = length);
//   VU 4..7 = world -> clip (|x|, |y| <= w is about 3.4 screens wide: a guard band; z = w - 1.98, so the near plane
//   z >= -w is w >= ~1), VU 8..11 = clip -> screen (X = 1089 x/w + 2048, Y = 993 y/w + 2048, Z = 1 + 1,036,000/w).
//   For each triangle whose normal (v1 - v0) x (v2 - v0) points against the light (dot < 0), every corner c and its
//   extrusion c + L are projected; the extrusion is cut back to the view's edges; then one strip of 8 triangles is
//   kicked: the triangle itself, its three sides, the far end. Each triangle adds or subtracts by its winding on
//   screen (z-pass counting). BUT: a corner of the triangle itself outside the clip volume (CLIP flags of the first
//   point, "iand vi01, vi04, 0xfc0; ibne ..." at 0x3888) drops the whole prism.
//
// 1. Shadows up close (live().nearShadows): before each start of the program, the prisms it will drop are made on
//    the PC: clipped in clip space against the same volume (so every corner maps into the GS's range with a valid
//    depth), projected as the program projects, each piece adding or subtracting by its winding on screen, and where
//    the near plane cuts a prism, the cut (the part of the near plane inside the prism) added as a cap: the rays that
//    start inside that prism then count it. Checked: made here for every prism (STAR_VOLUME_ALL=1) the pictures match
//    the program's own but for 0.1-0.2% of pixels along shadow edges (rounding).
#include "star/shadow_volumes.h"

#include "runtime/ps2_vu1_hooks.h"
#include "runtime/ps2_vu1_program_hook.h"
#include "star/live_settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace star
{
    namespace
    {
        constexpr uint32_t kVolumeProgram = 0x33C0u; // VU1 program 0x678 (pairs)
        constexpr uint32_t kVolumeKick = 0x37D8u;    // its XGKICK
        constexpr uint32_t kHeader = 0x121u;         // header / output, qwords after TOP

        struct V4
        {
            double x = 0.0, y = 0.0, z = 0.0, w = 0.0;
        };

        float vuFloat(const uint8_t *vu, uint32_t qword, int lane)
        {
            float v = 0.0f;
            std::memcpy(&v, vu + qword * 16u + static_cast<uint32_t>(lane) * 4u, 4);
            return v;
        }
        int32_t vuInt(const uint8_t *vu, uint32_t qword, int lane)
        {
            int32_t v = 0;
            std::memcpy(&v, vu + qword * 16u + static_cast<uint32_t>(lane) * 4u, 4);
            return v;
        }
        uint64_t vuU64(const uint8_t *vu, uint32_t qword, int half)
        {
            uint64_t v = 0;
            std::memcpy(&v, vu + qword * 16u + static_cast<uint32_t>(half) * 8u, 8);
            return v;
        }

        struct Matrix
        {
            double m[4][4] = {};
            void load(const uint8_t *vu, uint32_t qword)
            {
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        m[r][c] = vuFloat(vu, qword + static_cast<uint32_t>(r), c);
            }
            // row vector p * M
            V4 apply(const V4 &p) const
            {
                V4 o;
                o.x = p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + p.w * m[3][0];
                o.y = p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + p.w * m[3][1];
                o.z = p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2] + p.w * m[3][2];
                o.w = p.x * m[0][3] + p.y * m[1][3] + p.z * m[2][3] + p.w * m[3][3];
                return o;
            }
        };

        // the program's CLIP test of one corner, a hair inside the planes too (the VU rounds differently: a prism
        // the program also draws is only counted twice, which still reads as shadow; one it drops must not be missed)
        bool nearOrOutside(const V4 &c)
        {
            const double aw = std::fabs(c.w) * (1.0 - 1e-5);
            return c.x > aw || c.x < -aw || c.y > aw || c.y < -aw || c.z > aw || c.z < -aw;
        }

        V4 lerp(const V4 &a, const V4 &b, double t)
        {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        }

        // Sutherland-Hodgman against the clip volume in homogeneous coordinates: w - x, w + x, w - y, w + y >= 0 and
        // z + w >= a hair (the near plane; w > 0 then too).
        struct Poly
        {
            V4 p[16];
            int n = 0;
            void add(const V4 &v)
            {
                if (n < 16)
                    p[n++] = v;
            }
        };
        double planeDistance(const V4 &p, int plane)
        {
            switch (plane)
            {
            case 0: return p.w - p.x;
            case 1: return p.w + p.x;
            case 2: return p.w - p.y;
            case 3: return p.w + p.y;
            default: return p.z + p.w - 1e-3;
            }
        }
        unsigned outcode(const V4 &p)
        {
            unsigned code = 0;
            for (int plane = 0; plane < 5; ++plane)
                if (planeDistance(p, plane) < 0.0)
                    code |= 1u << plane;
            return code;
        }
        void clipPoly(Poly &poly, unsigned planes)
        {
            for (int plane = 0; plane < 5 && poly.n >= 3; ++plane)
            {
                if (!(planes & (1u << plane)))
                    continue;
                Poly out;
                for (int i = 0; i < poly.n; ++i)
                {
                    const V4 &a = poly.p[i], &b = poly.p[(i + 1) % poly.n];
                    const double da = planeDistance(a, plane), db = planeDistance(b, plane);
                    if (da >= 0.0)
                        out.add(a);
                    if ((da >= 0.0) != (db >= 0.0))
                        out.add(lerp(a, b, da / (da - db)));
                }
                poly = out;
            }
        }

        struct Batch
        {
            Matrix clip, screen;
            double centre[3] = {}, scale[3] = {};
            uint64_t prmodeAdd = 0x40u, prmodeSub = 0x240u;
            std::vector<uint8_t> *out = nullptr;
            uint32_t triangles = 0;
        };

        struct ScreenPoint
        {
            int32_t x, y, z;
            double fx, fy;
        };
        ScreenPoint project(const Batch &b, const V4 &c)
        {
            const V4 s = b.screen.apply(c);
            const double q = 1.0 / s.w;
            const double X = (s.x * q - b.centre[0]) * b.scale[0] + b.centre[0];
            const double Y = (s.y * q - b.centre[1]) * b.scale[1] + b.centre[1];
            const double Z = (s.z * q - b.centre[2]) * b.scale[2] + b.centre[2];
            // ftoi4: to 28.4 fixed, towards zero
            return {static_cast<int32_t>(X * 16.0), static_cast<int32_t>(Y * 16.0), static_cast<int32_t>(Z * 16.0), X, Y};
        }

        void put(std::vector<uint8_t> &out, uint64_t lo, uint64_t hi)
        {
            const size_t at = out.size();
            out.resize(at + 16u);
            std::memcpy(out.data() + at, &lo, 8);
            std::memcpy(out.data() + at + 8, &hi, 8);
        }
        void putVertex(std::vector<uint8_t> &out, const ScreenPoint &p)
        {
            const uint32_t words[4] = {static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y), static_cast<uint32_t>(p.z), 16u};
            const size_t at = out.size();
            out.resize(at + 16u);
            std::memcpy(out.data() + at, words, 16);
        }

        // One convex polygon (clip space, wound outward; `planes` = the planes some corner is outside of), cut to the
        // view, as triangles adding (front on screen) or subtracting (back), or always adding (the near cap).
        void emitPoly(Batch &b, Poly poly, unsigned planes, bool forceAdd)
        {
            if (planes)
                clipPoly(poly, planes);
            if (poly.n < 3)
                return;
            ScreenPoint pts[16];
            for (int i = 0; i < poly.n; ++i)
                pts[i] = project(b, poly.p[i]);
            // winding on screen (GS: x right, y down), the polygon's own area
            double area = 0.0;
            for (int i = 0; i < poly.n; ++i)
            {
                const ScreenPoint &p = pts[i], &q = pts[(i + 1) % poly.n];
                area += p.fx * q.fy - q.fx * p.fy;
            }
            const bool add = forceAdd || area > 0.0;
            for (int i = 1; i + 1 < poly.n; ++i)
            {
                put(*b.out, add ? b.prmodeAdd : b.prmodeSub, 0x1Bu);
                putVertex(*b.out, pts[0]);
                putVertex(*b.out, pts[i]);
                putVertex(*b.out, pts[i + 1]);
                ++b.triangles;
            }
        }

        // The near plane's cut through a convex prism (6 corners, 9 edges): the points where edges cross z + w = 0,
        // in order around their middle on screen.
        void emitNearCap(Batch &b, const std::array<V4, 6> &c)
        {
            static constexpr int kEdges[9][2] = {{0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5}, {5, 3}, {0, 3}, {1, 4}, {2, 5}};
            Poly cut;
            for (const auto &e : kEdges)
            {
                const double da = c[e[0]].z + c[e[0]].w, db = c[e[1]].z + c[e[1]].w;
                if ((da >= 0.0) != (db >= 0.0))
                    cut.add(lerp(c[e[0]], c[e[1]], da / (da - db)));
            }
            if (cut.n < 3)
                return;
            // on the plane z = -w: order by angle of (x/w, y/w) around the middle; nudge onto the kept side
            double mx = 0.0, my = 0.0;
            for (int i = 0; i < cut.n; ++i)
            {
                cut.p[i].z = -cut.p[i].w + 2e-3;
                mx += cut.p[i].x / cut.p[i].w;
                my += cut.p[i].y / cut.p[i].w;
            }
            mx /= cut.n;
            my /= cut.n;
            std::sort(cut.p, cut.p + cut.n, [&](const V4 &a, const V4 &d)
                      { return std::atan2(a.y / a.w - my, a.x / a.w - mx) < std::atan2(d.y / d.w - my, d.x / d.w - mx); });
            unsigned planes = 0;
            for (int i = 0; i < cut.n; ++i)
                planes |= outcode(cut.p[i]);
            emitPoly(b, cut, planes, true);
        }

        // One prism: corners 0-2 the triangle (world space after its bones), 3-5 swept along the light.
        void emitPrism(Batch &b, const std::array<V4, 6> &world)
        {
            std::array<V4, 6> c;
            unsigned anyOut = 0;
            bool behind = false, front = false;
            for (size_t i = 0; i < 6; ++i)
            {
                c[i] = b.clip.apply(world[i]);
                anyOut |= outcode(c[i]);
                (c[i].z + c[i].w < 0.0 ? behind : front) = true;
            }
            // faces, each wound outward (checked against the prism's middle in world space)
            static constexpr int kFaces[5][4] = {{0, 1, 2, -1}, {3, 5, 4, -1}, {0, 3, 4, 1}, {1, 4, 5, 2}, {2, 5, 3, 0}};
            double mx = 0.0, my = 0.0, mz = 0.0;
            for (const V4 &p : world)
            {
                mx += p.x / 6.0;
                my += p.y / 6.0;
                mz += p.z / 6.0;
            }
            for (const auto &f : kFaces)
            {
                const int n = f[3] < 0 ? 3 : 4;
                const V4 &a = world[static_cast<size_t>(f[0])], &p1 = world[static_cast<size_t>(f[1])], &p2 = world[static_cast<size_t>(f[2])];
                const double ux = p1.x - a.x, uy = p1.y - a.y, uz = p1.z - a.z, vx = p2.x - a.x, vy = p2.y - a.y, vz = p2.z - a.z;
                const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const bool inward = nx * (a.x - mx) + ny * (a.y - my) + nz * (a.z - mz) < 0.0;
                Poly poly;
                for (int i = 0; i < n; ++i)
                    poly.add(c[static_cast<size_t>(f[inward ? n - 1 - i : i])]);
                emitPoly(b, poly, anyOut, false);
            }
            if (behind && front)
                emitNearCap(b, c);
        }

        std::atomic<uint64_t> g_made{0}, g_prisms{0};

        // STAR_VOLUME_ALL=1 (tests): every prism made here and the program's own packets left undrawn (each
        // vertex gets ADC), to compare with the game's own pictures.
        bool allHere()
        {
            static const bool on = [] { const char *v = std::getenv("STAR_VOLUME_ALL"); return v && *v == '1'; }();
            return on;
        }

        void path1Filter(uint8_t *packet, uint32_t size, uint32_t, const uint8_t *, uint32_t, uint32_t kickPc,
                         std::vector<uint8_t> &, std::vector<uint8_t> &)
        {
            if (kickPc != kVolumeKick || !allHere())
                return;
            for (uint32_t at = 16u; at + 16u <= size; at += 16u)
            {
                uint32_t w3 = 0;
                std::memcpy(&w3, packet + at + 12, 4);
                w3 |= 0x8000u; // ADC (on an A+D qword the unused upper word)
                std::memcpy(packet + at + 12, &w3, 4);
            }
        }

        // the batch's bone matrices, each loaded once
        struct Bones
        {
            const uint8_t *vu = nullptr;
            uint32_t qwords = 0;
            int16_t slot[256];
            Matrix m[64];
            int used = 0;
            explicit Bones(const uint8_t *data, uint32_t count) : vu(data), qwords(count) { std::fill(std::begin(slot), std::end(slot), int16_t{-1}); }
            const Matrix *get(uint32_t bone)
            {
                if (bone >= 256u || 24u + 4u * bone + 4u > qwords)
                    return nullptr;
                if (slot[bone] < 0)
                {
                    if (used >= 64)
                        return nullptr;
                    slot[bone] = static_cast<int16_t>(used);
                    m[used++].load(vu, 24u + 4u * bone);
                }
                return &m[slot[bone]];
            }
        };

        // 2. Creature shadows stop at walls (live().shadowWalls): the renderer leaves a creature's shadow off surfaces
        // turned away from the light and off surfaces with scenery between them and the creature
        // (GSRendererHW::ApplyOwnerShadow, tfx.glsl PS_OWNER_WALLS). For that it needs, in the stream, before each
        // piece's volumes: the piece's projection into the shadow buffer (world -> GS x, y, z times w, and w: the
        // program's world -> clip, clip -> screen and viewport, as project() above) and its inverse, the eye, the
        // direction to the light, and the piece's middle and size in the world. Sent as NOP register writes
        // "STW"/"STV"/"STE"/"STL"/"STC" + index with the float in the lower word, each group when it changes.
        struct WallsMarkers
        {
            float projection[16] = {};
            bool projectionSent = false;
            float light[3] = {};
            bool lightSent = false;
            float caster[4] = {};
            bool casterSent = false;
        } g_walls; // the VIF thread only

        void putMarker(std::vector<uint8_t> &out, uint32_t tag, uint32_t index, float value)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, 4);
            put(out, (static_cast<uint64_t>((tag << 8) | index) << 32) | bits, 0x0Fu);
        }

        // a GIF packet of A+D NOP writes: its tag first, then the markers
        void openMarkers(std::vector<uint8_t> &out, size_t &tagAt)
        {
            tagAt = out.size();
            put(out, 0, 0);
        }
        void closeMarkers(std::vector<uint8_t> &out, size_t tagAt)
        {
            const uint64_t n = (out.size() - tagAt) / 16u - 1u;
            if (n == 0u)
            {
                out.resize(tagAt);
                return;
            }
            const uint64_t tag = n | (1ull << 15) | (1ull << 60); // NLOOP n, EOP, PACKED, 1 register
            const uint64_t regs = 0xEull;                         // A+D
            std::memcpy(out.data() + tagAt, &tag, 8);
            std::memcpy(out.data() + tagAt + 8, &regs, 8);
        }

        bool invert4(const double m[4][4], double inv[4][4])
        {
            double a[4][8];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 8; ++c)
                    a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.0 : 0.0);
            for (int c = 0; c < 4; ++c)
            {
                int pivot = c;
                for (int r = c + 1; r < 4; ++r)
                    if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                        pivot = r;
                if (std::fabs(a[pivot][c]) < 1e-30)
                    return false;
                if (pivot != c)
                    for (int k = 0; k < 8; ++k)
                        std::swap(a[c][k], a[pivot][k]);
                const double d = a[c][c];
                for (int k = 0; k < 8; ++k)
                    a[c][k] /= d;
                for (int r = 0; r < 4; ++r)
                    if (r != c)
                    {
                        const double f = a[r][c];
                        for (int k = 0; k < 8; ++k)
                            a[r][k] -= f * a[c][k];
                    }
            }
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    inv[r][c] = a[r][c + 4];
            return true;
        }

        void sendWalls(const uint8_t *vu, uint32_t h, uint32_t top, int32_t count, Bones &bones, const V4 &light, double length,
                       std::vector<uint8_t> &out)
        {
            // the projection: p * clip * screen * viewport (row vectors), the viewport as in project()
            Matrix clip, screen;
            clip.load(vu, 4u);
            screen.load(vu, 8u);
            double cs[4][4] = {}, f[4][4] = {};
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    for (int k = 0; k < 4; ++k)
                        cs[r][c] += clip.m[r][k] * screen.m[k][c];
            double view[4][4] = {};
            for (int i = 0; i < 3; ++i)
            {
                const double centre = vuFloat(vu, h + 2u, i), scale = vuFloat(vu, h + 4u, i);
                view[i][i] = scale;
                view[3][i] = (1.0 - scale) * centre;
            }
            view[3][3] = 1.0;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    for (int k = 0; k < 4; ++k)
                        f[r][c] += cs[r][k] * view[k][c];
            float projection[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    projection[r * 4 + c] = static_cast<float>(f[r][c]);
            size_t tagAt = 0;
            openMarkers(out, tagAt);
            if (!g_walls.projectionSent || std::memcmp(g_walls.projection, projection, sizeof(projection)) != 0)
            {
                double inv[4][4];
                if (invert4(f, inv) && std::fabs(inv[2][3]) > 1e-30)
                {
                    std::memcpy(g_walls.projection, projection, sizeof(projection));
                    g_walls.projectionSent = true;
                    // the shader's row j (h_j = dot(row_j, s)) = column j of the matrix (row vectors: h = s * M)
                    for (int j = 0; j < 4; ++j)
                        for (int i = 0; i < 4; ++i)
                        {
                            putMarker(out, 0x535457u, static_cast<uint32_t>(j * 4 + i), static_cast<float>(f[i][j]));
                            putMarker(out, 0x535456u, static_cast<uint32_t>(j * 4 + i), static_cast<float>(inv[i][j]));
                        }
                    // the eye: the point every pixel's ray starts from, (0, 0, 1, 0) * inverse
                    for (int i = 0; i < 3; ++i)
                        putMarker(out, 0x535445u, static_cast<uint32_t>(i), static_cast<float>(inv[2][i] / inv[2][3]));
                }
            }
            const float toLight[3] = {static_cast<float>(-light.x / length), static_cast<float>(-light.y / length), static_cast<float>(-light.z / length)};
            if (!g_walls.lightSent || std::memcmp(g_walls.light, toLight, sizeof(toLight)) != 0)
            {
                std::memcpy(g_walls.light, toLight, sizeof(toLight));
                g_walls.lightSent = true;
                for (int i = 0; i < 3; ++i)
                    putMarker(out, 0x53544Cu, static_cast<uint32_t>(i), toLight[i]);
            }
            // the piece in the world: the middle of its corners and the farthest corner's distance from it
            double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
            std::vector<V4> corners;
            corners.reserve(static_cast<size_t>(count) * 3u);
            for (uint32_t at = top; at < top + static_cast<uint32_t>(count) * 3u; ++at)
            {
                const Matrix *m = bones.get(static_cast<uint32_t>(vuInt(vu, at, 3)) & 0xFFFFu);
                if (!m)
                    continue;
                const V4 w = m->apply({vuInt(vu, at, 0) / 16.0, vuInt(vu, at, 1) / 16.0, vuInt(vu, at, 2) / 16.0, 1.0});
                corners.push_back(w);
                lo[0] = std::min(lo[0], w.x), lo[1] = std::min(lo[1], w.y), lo[2] = std::min(lo[2], w.z);
                hi[0] = std::max(hi[0], w.x), hi[1] = std::max(hi[1], w.y), hi[2] = std::max(hi[2], w.z);
            }
            if (!corners.empty())
            {
                const double mid[3] = {(lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5};
                double radius = 0.0;
                for (const V4 &c : corners)
                    radius = std::max(radius, std::sqrt((c.x - mid[0]) * (c.x - mid[0]) + (c.y - mid[1]) * (c.y - mid[1]) + (c.z - mid[2]) * (c.z - mid[2])));
                const float caster[4] = {static_cast<float>(mid[0]), static_cast<float>(mid[1]), static_cast<float>(mid[2]), static_cast<float>(radius)};
                if (!g_walls.casterSent || std::memcmp(g_walls.caster, caster, sizeof(caster)) != 0)
                {
                    std::memcpy(g_walls.caster, caster, sizeof(caster));
                    g_walls.casterSent = true;
                    for (int i = 0; i < 4; ++i)
                        putMarker(out, 0x535443u, static_cast<uint32_t>(i), caster[i]);
                }
            }
            closeMarkers(out, tagAt);
        }

        void programStart(uint32_t startPc, uint32_t top, uint8_t *vu, uint32_t vuSize, uint32_t owner, std::vector<uint8_t> &out)
        {
            const bool near = live().nearShadows.load(std::memory_order_relaxed);
            const bool walls = live().shadowWalls.load(std::memory_order_relaxed);
            if (startPc != kVolumeProgram || (!near && !walls))
                return;
            const uint32_t qwords = vuSize / 16u;
            const uint32_t h = top + kHeader;
            if (h + 17u > qwords || vuU64(vu, h, 1) != 0x4E4E44E44E4E4E44ull || (vuU64(vu, h, 0) & 0xFFFFu) != 0x8001u)
                return;
            const int32_t count = vuInt(vu, h + 1u, 0) + 1;
            if (count <= 0 || static_cast<uint32_t>(count) * 3u > kHeader)
                return;
            Batch b;
            b.out = &out;
            b.clip.load(vu, 4u);
            b.screen.load(vu, 8u);
            const V4 light{vuFloat(vu, h + 16u, 0), vuFloat(vu, h + 16u, 1), vuFloat(vu, h + 16u, 2), 0.0};
            const double length = std::sqrt(light.x * light.x + light.y * light.y + light.z * light.z);
            if (!(length > 0.0))
                return;
            Bones bones(vu, qwords);
            if (walls)
                sendWalls(vu, h, top, count, bones, light, length, out); // 2.

            // STAR_VOLUME_MESH_LOG=<n> (tests): from the n-th batch, 80 batches: is each piece's shadow mesh closed?
            static const long meshLog = [] { const char *v = std::getenv("STAR_VOLUME_MESH_LOG"); return v ? std::atol(v) : 0L; }();
            static long batchNumber = 0;
            ++batchNumber;
            if (meshLog > 0 && batchNumber >= meshLog && batchNumber < meshLog + 400)
            {
                std::vector<std::array<long long, 3>> pts;
                auto key = [&](uint32_t at) -> int
                {
                    const Matrix *m = bones.get(static_cast<uint32_t>(vuInt(vu, at, 3)) & 0xFFFFu);
                    if (!m)
                        return -1;
                    const V4 w = m->apply({vuInt(vu, at, 0) / 16.0, vuInt(vu, at, 1) / 16.0, vuInt(vu, at, 2) / 16.0, 1.0});
                    const std::array<long long, 3> k{std::llround(w.x * 4.0), std::llround(w.y * 4.0), std::llround(w.z * 4.0)};
                    for (size_t i = 0; i < pts.size(); ++i)
                        if (pts[i] == k)
                            return static_cast<int>(i);
                    pts.push_back(k);
                    return static_cast<int>(pts.size() - 1);
                };
                std::vector<std::array<int, 3>> edges; // a, b, uses (a < b)
                int lit = 0;
                for (int32_t k = 0; k < count; ++k)
                {
                    int id[3];
                    for (int j = 0; j < 3; ++j)
                        id[j] = key(top + static_cast<uint32_t>(k) * 3u + static_cast<uint32_t>(j));
                    for (int j = 0; j < 3; ++j)
                    {
                        const int a1 = std::min(id[j], id[(j + 1) % 3]), b1 = std::max(id[j], id[(j + 1) % 3]);
                        bool found = false;
                        for (auto &e : edges)
                            if (e[0] == a1 && e[1] == b1)
                            {
                                ++e[2];
                                found = true;
                            }
                        if (!found)
                            edges.push_back({a1, b1, 1});
                    }
                    (void)lit;
                }
                int once = 0, twice = 0, more = 0;
                for (const auto &e : edges)
                    (e[2] == 1 ? once : e[2] == 2 ? twice : more)++;
                char boneList[256] = {};
                int used = 0;
                bool seen[256] = {};
                for (uint32_t at = top; at < top + static_cast<uint32_t>(count) * 3u; ++at)
                {
                    const uint32_t bone = static_cast<uint32_t>(vuInt(vu, at, 3)) & 0xFFu;
                    if (!seen[bone] && used < 240)
                    {
                        seen[bone] = true;
                        used += std::snprintf(boneList + used, sizeof(boneList) - static_cast<size_t>(used), "%u ", bone);
                    }
                }
                std::printf("[volumes] batch %ld: %d triangles, %zu corners, %zu edges: %d open, %d shared by two, %d by more; bones %s; light %.4f %.4f %.4f x %.1f%c", batchNumber, count,
                            pts.size(), edges.size(), once, twice, more, boneList, light.x / length, light.y / length, light.z / length, length, 10);
            }

            // STAR_VOLUME_SKIP=a,b,.. (tests, the user's Rurufon room): batches counted from the one with 16 triangles
            // (her first) cast nothing (their triangles collapsed to a point)
            static const std::vector<int> skipList = []
            {
                std::vector<int> list;
                if (const char *v = std::getenv("STAR_VOLUME_SKIP"))
                    for (const char *s = v; *s;)
                    {
                        list.push_back(std::atoi(s));
                        while (*s && *s != ',')
                            ++s;
                        if (*s == ',')
                            ++s;
                    }
                return list;
            }();
            static int sinceFirst = 1000;
            sinceFirst = count == 16 ? 0 : sinceFirst + 1;
            if (!skipList.empty() && std::find(skipList.begin(), skipList.end(), sinceFirst) != skipList.end())
                for (int32_t k = 0; k < count; ++k)
                    for (int j = 1; j < 3; ++j)
                        std::memcpy(vu + (top + static_cast<uint32_t>(k) * 3u + static_cast<uint32_t>(j)) * 16u, vu + (top + static_cast<uint32_t>(k) * 3u) * 16u, 16);

            // The player's own pieces (game_fixes.cpp labelPiece: bit 8 of the label; body, first-person arms, weapon)
            // are next to the camera: the program would drop their prisms, so the game never showed their shadow, and
            // in the in-between pictures they turn with the view while a shadow made here would not. Left as the game
            // has them. (A precaution: in the tests they sent no volume batches at all, STAR_VOLUME_LOG "of the
            // player's own". The black spikes in fast turns were another thing: frame_double.cpp, kMoreBones.)
            if (!near || (owner & 0x100u) != 0u)
                return;

            // 1. the prisms the program will drop
            for (int i = 0; i < 3; ++i)
            {
                b.centre[i] = vuFloat(vu, h + 2u, i);
                b.scale[i] = vuFloat(vu, h + 4u, i);
            }
            b.prmodeSub = vuU64(vu, h + 3u, 0);
            b.prmodeAdd = vuU64(vu, h + 5u, 0);
            const bool all = allHere();
            const size_t tagAt = out.size();
            put(out, 0, 0); // the GIF tag, filled in below
            uint64_t prisms = 0;
            for (int32_t k = 0; k < count; ++k)
            {
                std::array<V4, 6> world;
                bool ok = true;
                for (int j = 0; j < 3 && ok; ++j)
                {
                    const uint32_t at = top + static_cast<uint32_t>(k) * 3u + static_cast<uint32_t>(j);
                    const Matrix *m = bones.get(static_cast<uint32_t>(vuInt(vu, at, 3)) & 0xFFFFu);
                    if (!m)
                    {
                        ok = false;
                        break;
                    }
                    const V4 p{vuInt(vu, at, 0) / 16.0, vuInt(vu, at, 1) / 16.0, vuInt(vu, at, 2) / 16.0, 1.0};
                    world[static_cast<size_t>(j)] = m->apply(p);
                    world[static_cast<size_t>(j)].w = 1.0;
                }
                if (!ok)
                    continue;
                const V4 &v0 = world[0], &v1 = world[1], &v2 = world[2];
                const double ax = v1.x - v0.x, ay = v1.y - v0.y, az = v1.z - v0.z, bx = v2.x - v0.x, by = v2.y - v0.y, bz = v2.z - v0.z;
                const double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                if (nx * light.x + ny * light.y + nz * light.z >= 0.0)
                    continue; // not facing the light: the program draws nothing for it either
                if (!all)
                {
                    bool dropped = false;
                    for (int j = 0; j < 3 && !dropped; ++j)
                        dropped = nearOrOutside(b.clip.apply(world[static_cast<size_t>(j)]));
                    if (!dropped)
                        continue; // the program draws this one
                }
                for (size_t j = 0; j < 3; ++j)
                    world[j + 3u] = {world[j].x + light.x, world[j].y + light.y, world[j].z + light.z, 1.0};
                emitPrism(b, world);
                ++prisms;
            }
            if (b.triangles == 0u)
            {
                out.resize(tagAt);
                return;
            }
            // GIF tag: NLOOP triangles, EOP, PRE, PRIM 3 (triangles), PACKED, 4 registers: A+D (PRMODE), 3 x XYZF2
            const uint64_t tag = static_cast<uint64_t>(b.triangles) | (1ull << 15) | (1ull << 46) | (3ull << 47) | (4ull << 60);
            const uint64_t regs = 0x444Eull;
            std::memcpy(out.data() + tagAt, &tag, 8);
            std::memcpy(out.data() + tagAt + 8, &regs, 8);
            g_made.fetch_add(b.triangles, std::memory_order_relaxed);
            g_prisms.fetch_add(prisms, std::memory_order_relaxed);
        }

        void hook(uint32_t startPc, uint32_t top, uint8_t *vu, uint32_t vuSize, uint32_t owner, std::vector<uint8_t> &out)
        {
            // STAR_VOLUME_LOG=1 (tests): once a second, the time spent here and what was made
            static const bool log = [] { const char *v = std::getenv("STAR_VOLUME_LOG"); return v && *v == '1'; }();
            if (!log)
            {
                programStart(startPc, top, vu, vuSize, owner, out);
                return;
            }
            static auto windowStart = std::chrono::steady_clock::now();
            static double spentMs = 0.0;
            static uint64_t starts = 0, made = 0, prisms = 0, own = 0;
            const auto t0 = std::chrono::steady_clock::now();
            const uint64_t m0 = g_made.load(), p0 = g_prisms.load();
            programStart(startPc, top, vu, vuSize, owner, out);
            const auto t1 = std::chrono::steady_clock::now();
            spentMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
            starts += startPc == kVolumeProgram ? 1u : 0u;
            own += startPc == kVolumeProgram && (owner & 0x100u) != 0u ? 1u : 0u;
            made += g_made.load() - m0;
            prisms += g_prisms.load() - p0;
            if (t1 - windowStart >= std::chrono::seconds(1))
            {
                std::printf("[volumes] last second: %.2f ms in %llu volume batches (%llu of the player's own), %llu prisms, %llu triangles made here%c", spentMs,
                            static_cast<unsigned long long>(starts), static_cast<unsigned long long>(own), static_cast<unsigned long long>(prisms),
                            static_cast<unsigned long long>(made), 10);
                windowStart = t1;
                spentMs = 0.0;
                starts = made = prisms = own = 0;
            }
        }
    }

    void installShadowVolumes()
    {
        ps2x::vu1hooks::setProgramStartHook(&hook);
        if (allHere())
            ps2x::vu1hooks::setPath1Filter(&path1Filter);
        std::printf("[STAR option] shadows up close %s, stop at walls %s%s%c", live().nearShadows.load() ? "on" : "off",
                    live().shadowWalls.load() ? "on" : "off", allHere() ? " (test: every prism made here)" : "", 10);
    }

    uint64_t shadowVolumesMade(uint64_t &prisms)
    {
        prisms = g_prisms.load(std::memory_order_relaxed);
        return g_made.load(std::memory_order_relaxed);
    }
}
