/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/World/components/RoofComponent.cs).
 */
#include "core/env.h"
#include "world/roofs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ts {

namespace {

constexpr int W = kLotArraySize;
constexpr int kHalf = 8, kTile = 16;   // positions are in 1/16 tile, like FreeSims' LotTilePos

struct Rect {
    int x1, y1, x2, y2;
    int expand = 0;   // next side to grow: 0 +x, 1 +y, 2 -x, 3 -y; -1 done

    int get(int dir) const { return dir == 0 ? x2 : dir == 1 ? y2 : dir == 2 ? x1 : y1; }
    void set(int dir, int v) { (dir == 0 ? x2 : dir == 1 ? y2 : dir == 2 ? x1 : y1) = v; }
    bool contains(int px, int py) const { return px >= x1 && px <= x2 && py >= y1 && py <= y2; }
};

struct Builder {
    const Lot &lot;
    const RoomMap (&rooms)[kLotLevels];
    int below;   // the storey being roofed; the roof sits on the storey above it

    bool indoors(int x, int y, int level) const
    {
        if (level < 0 || level >= kLotLevels || x < 0 || y < 0 || x >= W || y >= W)
            return false;
        const RoomMap &r = rooms[level];
        const uint32_t v = r.map[size_t(x) + size_t(y) * W];
        const uint32_t a = v & 0xffff, b = v >> 16;
        return (a < r.outside.size() && !r.outside[a]) || (b > 0 && b < r.outside.size() && !r.outside[b]);
    }
    bool indoors_or_floor(int x, int y, int level) const
    {
        if (level < 0 || level >= kLotLevels || x < 0 || y < 0 || x >= W || y >= W)
            return false;
        return indoors(x, y, level) || lot.floor(x, y, level) != 0;
    }

    // RoofComponent.IsRoofable: a half tile over indoors (or half a tile
    // from it), with nothing indoors or floored on the storey above.
    bool roofable(int px, int py) const
    {
        const int tx = px / kTile, ty = py / kTile, above = below + 1;
        if (tx <= 0 || tx >= W - 1 || ty <= 0 || ty >= W - 1)
            return false;
        const int sx = px % kTile == kHalf ? 1 : -1, sy = py % kTile == kHalf ? 1 : -1;
        bool half = false;
        if (!indoors(tx, ty, below)) {
            if (!indoors(tx + sx, ty, below) && !indoors(tx, ty + sy, below) && !indoors(tx + sx, ty + sy, below))
                return false;
            half = true;
        }
        if (indoors_or_floor(tx, ty, above))
            return false;
        if (half && (indoors_or_floor(tx + sx, ty, above) || indoors_or_floor(tx, ty + sy, above) ||
                     indoors_or_floor(tx + sx, ty + sy, above)))
            return false;
        return true;
    }

    // RoofComponent.RoofSpread: grow a rectangle from a roofable half tile
    // side by side until each side meets something that isn't roofable.
    void spread(int sx, int sy, std::vector<bool> &evaluated, int width, std::vector<Rect> &out) const
    {
        static const int kAdvance[4][2] = { { kHalf, 0 }, { 0, kHalf }, { -kHalf, 0 }, { 0, -kHalf } };
        static const int kNext[4] = { 2, 3, 1, -1 };
        Rect rect{ sx, sy, sx + kHalf, sy + kHalf };
        while (rect.expand != -1) {
            const int dir = rect.expand;
            int px, py;
            switch (dir) {
            case 0: px = rect.x2; py = rect.y1; break;
            case 1: px = rect.x2 - kHalf; py = rect.y2; break;
            case 2: px = rect.x1 - kHalf; py = rect.y2 - kHalf; break;
            default: px = rect.x1; py = rect.y1 - kHalf; break;
            }
            const int ix = kAdvance[(dir + 1) % 4][0], iy = kAdvance[(dir + 1) % 4][1];
            const int count = std::abs(rect.get((dir + 1) % 4) - rect.get((dir + 3) % 4)) / kHalf;
            bool grow = true;
            for (int i = 0, x = px, y = py; i < count; i++, x += ix, y += iy)
                if (!roofable(x, y)) {
                    grow = false;
                    break;
                }
            if (!grow) {
                rect.expand = kNext[rect.expand];
                continue;
            }
            for (int i = 0, x = px, y = py; i < count; i++, x += ix, y += iy)
                evaluated[size_t(x / kHalf) + size_t(y / kHalf) * width] = true;
            // Growing into an existing rectangle that spans us: jump to its far side.
            const int mx = px + ix * count / 2 + kHalf / 2, my = py + iy * count / 2 + kHalf / 2;
            const Rect *into = nullptr;
            for (const Rect &r : out) {
                const bool spans = dir % 2 == 0 ? (rect.y1 > r.y1 && rect.y2 < r.y2) : (rect.x1 > r.x1 && rect.x2 < r.x2);
                if (r.contains(mx, my) && spans) {
                    into = &r;
                    break;
                }
            }
            rect.set(dir, into ? into->get(dir) : rect.get(dir) + (dir > 1 ? -kHalf : kHalf));
        }
        out.push_back(rect);
    }
};

} // namespace

bool under_roof(const Lot &lot, const RoomMap (&rooms)[kLotLevels], int storey, int x, int y)
{
    const Builder b{ lot, rooms, storey };
    return b.indoors(x, y, storey) && !b.indoors_or_floor(x, y, storey + 1);
}
std::vector<RoofFace> build_roof(const Lot &lot, const RoomMap (&rooms)[kLotLevels], int storey)
{
    std::vector<RoofFace> faces;
    if (storey < 0 || storey >= kLotLevels)
        return faces;
    Builder b{ lot, rooms, storey };
    const int width = W * 2;
    std::vector<bool> evaluated(size_t(width) * width, false);
    std::vector<Rect> rects;
    for (int y = 2; y < width; y++)
        for (int x = 2; x < width; x++) {
            const size_t i = size_t(x) + size_t(y) * width;
            if (evaluated[i])
                continue;
            evaluated[i] = true;
            if (b.roofable(x * kHalf, y * kHalf))
                b.spread(x * kHalf, y * kHalf, evaluated, width, rects);
        }
    // Two rectangles nearly one over the other (FreeSims' spread makes a second one from the half
    // tiles the first couldn't grow into at its corners, against a taller storey's wall): one roof
    // over both, as the original has. Each stood a hipped roof, and the narrower one's taller hip rose
    // out of the other by the wall: the strip of roof standing up at the Kats' wing (House56, the
    // user's photo, 2026-11-03). An L's two arms overlap only at the corner and stay apart.
    if (!TS_ENV("TS_ROOF_NO_MERGE"))
        for (bool merged = true; merged;) {
            merged = false;
            for (size_t i = 0; i < rects.size() && !merged; i++)
                for (size_t j = i + 1; j < rects.size() && !merged; j++) {
                    Rect &a = rects[i];
                    const Rect &c = rects[j];
                    const int ox = std::min(a.x2, c.x2) - std::max(a.x1, c.x1), oy = std::min(a.y2, c.y2) - std::max(a.y1, c.y1);
                    if (ox <= 0 || oy <= 0)
                        continue;
                    const long overlap = long(ox) * oy;
                    const long smaller = std::min(long(a.x2 - a.x1) * (a.y2 - a.y1), long(c.x2 - c.x1) * (c.y2 - c.y1));
                    if (overlap * 4 < smaller * 3)   // under three quarters of the smaller
                        continue;
                    // And only where the one roof stays over what's roofable: an L or a stepped house's
                    // rectangles overlap as much, and merged step by step they became one square roof
                    // over the whole house's box, out over the lawn (the Granges, the user, 2026-11-10).
                    // The half tiles of the merged rectangle that aren't roofable, against all of them: up
                    // to 4 of them, or 3% (the Kats' wing 2 of 126; the Granges' first step 24 of 336, 7%).
                    // TS_ROOF_MERGE_MOST=100: every such pair merged, as from 2026-11-03.
                    {
                        const int ux1 = std::min(a.x1, c.x1), uy1 = std::min(a.y1, c.y1);
                        const int ux2 = std::max(a.x2, c.x2), uy2 = std::max(a.y2, c.y2);
                        int outside = 0, all = 0;
                        for (int py = uy1; py < uy2; py += kHalf)
                            for (int px = ux1; px < ux2; px += kHalf) {
                                all++;
                                if (!b.roofable(px, py))
                                    outside++;
                            }
                        if (TS_ENV("TS_ROOF_RECTS"))   // debug
                            std::printf("roof merge? (%.1f, %.1f) - (%.1f, %.1f): %d of %d half tiles not roofable\n",
                                        ux1 / 16.0, uy1 / 16.0, ux2 / 16.0, uy2 / 16.0, outside, all);
                        static const char *most = TS_ENV("TS_ROOF_MERGE_MOST");   // debug: the share allowed, percent
                        if (outside > 4 && outside * 100 > all * (most ? std::atoi(most) : 3))
                            continue;
                    }
                    a.x1 = std::min(a.x1, c.x1);
                    a.y1 = std::min(a.y1, c.y1);
                    a.x2 = std::max(a.x2, c.x2);
                    a.y2 = std::max(a.y2, c.y2);
                    rects.erase(rects.begin() + std::ptrdiff_t(j));
                    merged = true;
                }
        }
    if (const char *m = TS_ENV("TS_ROOF_MAP")) {   // debug: "x0 y0 x1 y1": I indoors, f floor only, . neither
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::sscanf(m, "%d %d %d %d", &x0, &y0, &x1, &y1);
        std::printf("storey %d (rows y %d-%d, columns x %d-%d):\n", storey, y0, y1, x0, x1);
        for (int y = y0; y <= y1; y++) {
            std::printf("  %2d ", y);
            for (int x = x0; x <= x1; x++)
                std::printf("%c", b.indoors(x, y, storey) ? 'I' : lot.floor(x, y, storey) ? 'f' : '.');
            std::printf("\n");
        }
    }
    if (TS_ENV("TS_ROOF_RECTS"))   // debug: each rectangle, in tiles
        for (const Rect &r : rects)
            std::printf("roof storey %d: (%.1f, %.1f) - (%.1f, %.1f)\n", storey, r.x1 / 16.0, r.y1 / 16.0, r.x2 / 16.0,
                        r.y2 / 16.0);

    // RoofComponent.MeshRects: a hipped roof on each rectangle, its ridge
    // as high as half the shorter side allows at this pitch.
    // 0 is the original's Flat Roof (set and saved in the original, 2026-11-03: HOUS stores 0.0; the
    // game's own pictures of the downtown and community lots that store 0 show flat roofs). The rest:
    // Shallow 0.4, Medium 0.6, Steep 1.0. A lot without HOUS is given 0.66 when read.
    const float pitch = std::max(lot.hous.roof_pitch, 0.0f);
    const float t = std::clamp(pitch, 0.0f, 1.0f);
    auto grey = [&](float to) { return 1 + (to / 255.0f - 1) * t; };
    const float shade_top = grey(175), shade_right = 1, shade_bottom = grey(200), shade_left = grey(150);
    for (const Rect &r : rects) {
        // On the walls below, which stand on the ground in the middle of the rectangle.
        const float base = (storey + 1) * 2.95f + lot.ground((r.x1 + r.x2) / 32.0f - 0.5f, (r.y1 + r.y2) / 32.0f - 0.5f);
        const int h = std::min(r.x2 - r.x1, r.y2 - r.y1) / 2;
        const float lift = h / 400.0f / 3;   // FreeSims' tiny offset against z-fighting, in tiles
        struct P {
            int x, y, z;
        };
        const P tl{ r.x1, r.y1, 0 }, tr{ r.x2, r.y1, 0 }, bl{ r.x1, r.y2, 0 }, br{ r.x2, r.y2, 0 };
        const P mtl{ r.x1 + h, r.y1 + h, h }, mtr{ r.x2 - h, r.y1 + h, h }, mbl{ r.x1 + h, r.y2 - h, h },
            mbr{ r.x2 - h, r.y2 - h, h };
        // Each side: eave corners, then ridge corners; how u and v follow x and y.
        struct Side {
            P p[4];
            float shade;
            int ux, uy, vx, vy;   // u = ux*X + uy*Y, v = vx*X + vy*Y (FreeSims world units: 3 per tile)
        };
        const Side sides[4] = {
            { { tl, tr, mtr, mtl }, shade_top, 1, 0, 0, -1 },
            { { tr, br, mbr, mtr }, shade_right, 0, 1, 1, 0 },
            { { br, bl, mbl, mbr }, shade_bottom, 1, 0, 0, 1 },
            { { bl, tl, mtl, mbl }, shade_left, 0, 1, -1, 0 },
        };
        for (const Side &s : sides) {
            RoofFace f;
            for (int i = 0; i < 4; i++) {
                const float wx = s.p[i].x / float(kTile), wy = s.p[i].y / float(kTile);   // tiles, from the corner
                f.x[i] = wx - 0.5f;   // to lot space, where tiles are centred on whole numbers
                f.y[i] = wy - 0.5f;
                f.z[i] = base + s.p[i].z * pitch / kTile + lift;
                f.u[i] = (s.ux * wx + s.uy * wy) * 3 * (2 / 3.0f);
                f.v[i] = (s.vx * wx + s.vy * wy) * 3;
                f.shade[i] = s.shade * (i < 2 ? 0.8f : 1.0f);   // eaves a little darker
            }
            faces.push_back(f);
        }
    }
    return faces;
}

} // namespace ts
