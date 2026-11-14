/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Wall drawing follows FreeSims (SimsVille/World/components/WallComponent.cs,
 * ContentManager/WorldWallProvider.cs, SimsAntics/entities/VMGameObject.cs,
 * SimsAntics/utils/VMArchitectureTools.cs).
 */
#include "core/env.h"
#include "core/fast_clock.h"
#include "render/wall_painter.h"

#include "core/png.h"

#include <chrono>
#include <set>
#include <array>
#include <map>
#include <algorithm>

#include "render/depth.h"
#include "render/lot_painter.h"
#include "world/rooms.h"

namespace ts {

namespace {

constexpr int W = kLotArraySize;

// Where each wall face sits in a tile's sprite box: top-left, top-right,
// horizontal diagonal, vertical diagonal. Indexed [zoom index][side].
const int kFaceX[3][4] = { { 1, 17, 1, 15 }, { 2, 34, 2, 30 }, { 4, 68, 4, 60 } };
const int kFaceY[3][4] = { { 20, 20, 28, 37 }, { 39, 39, 55, 73 }, { 77, 77, 109, 145 } };
// Where a junction cap sits in the sprite box of its corner.
const int kJunctionX[3] = { 1, 2, 4 }, kJunctionY[3] = { 80, 159, 317 };

// In walls.iff: styles are SPR# 2*id (+1 walls down), patterns 1536 + id;
// each zoom adds 512 (styles) or 256 (patterns). Junction caps are 4096-4098.
constexpr int kStyleZoomStep = 512, kPatternBase = 1536, kPatternZoomStep = 256, kJunctionBase = 4096;
// In a custom .wll file: SPR# for the far, medium and near pattern.
const uint16_t kCustomPattern[3] = { 1, 1793, 2049 };

constexpr uint16_t kPlainWall = 1, kDiagonalWall = 255;
// Styles and overlay patterns for a wall at the edge of the cutaway, sloping
// down to the left or to the right.
constexpr uint16_t kStyleDownLeft = 7, kStyleDownRight = 8, kOverlayDownLeft = 252, kOverlayDownRight = 253;
constexpr int kObjFlagField2 = 40, kObjWallPlacement = 13;
constexpr uint16_t kArchitecturalWindow = 1 << 14, kArchitecturalDoor = 1 << 15;

// JunctionFlags: which walls meet at a tile's top corner.
enum : uint8_t {
    JTopLeft = 1, JTopRight = 2, JBottomRight = 4, JBottomLeft = 8,
    JDiagLeft = 16, JDiagTop = 32, JDiagRight = 64, JDiagBottom = 128,
};

// FreeSims JunctionMap: junction flags -> frame of the junction sprite.
struct JunctionFrames {
    int frame[256];
    JunctionFrames()
    {
        const uint8_t order[] = {
            JTopRight, JTopLeft, JTopLeft | JTopRight, JBottomLeft, JBottomLeft | JTopRight, JBottomLeft | JTopLeft,
            JBottomLeft | JTopLeft | JTopRight, JBottomRight, JBottomRight | JTopRight, JBottomRight | JTopLeft,
            JBottomRight | JTopRight | JTopLeft, JBottomRight | JBottomLeft, JBottomRight | JBottomLeft | JTopRight,
            JBottomRight | JBottomLeft | JTopLeft, 15,
            JDiagTop, JDiagTop | JBottomLeft, JDiagTop | JBottomRight, JDiagTop | JBottomLeft | JBottomRight,
            JDiagLeft, JDiagLeft | JTopRight, JDiagLeft | JBottomRight, JDiagLeft | JBottomRight | JTopRight,
            JDiagLeft | JDiagTop, JDiagLeft | JDiagTop | JBottomRight,
            JDiagBottom, JDiagBottom | JTopRight, JDiagBottom | JTopLeft, JDiagBottom | JTopLeft | JTopRight,
            JDiagBottom | JDiagTop, JDiagBottom | JDiagLeft, JDiagBottom | JDiagLeft | JTopRight,
            JDiagBottom | JDiagLeft | JDiagTop,
            JDiagRight, JDiagRight | JTopLeft, JDiagRight | JBottomLeft, JDiagRight | JTopLeft | JBottomLeft,
            JDiagRight | JDiagTop, JDiagRight | JDiagTop | JBottomLeft, JDiagRight | JDiagLeft,
            JDiagRight | JDiagLeft | JDiagTop, JDiagRight | JDiagBottom, JDiagRight | JDiagBottom | JTopLeft,
            JDiagRight | JDiagBottom | JDiagTop, JDiagRight | JDiagBottom | JDiagLeft, 240,
        };
        for (int &f : frame)
            f = -1;
        for (int i = 0; i < int(sizeof order); i++)
            frame[order[i]] = i;
    }
};
const JunctionFrames kJunctionFrames;

uint8_t rotate_junction(int rot, uint8_t in)
{
    const int lower = (in & 15) << rot, higher = (in & 240) << rot;
    return uint8_t((lower & 15) | (lower >> 4) | ((higher | higher >> 4) & 240));
}

// Only these styles (fences, banisters) survive from the saved style bytes;
// everything else is drawn as a plain wall (FreeSims ValidWallStyles).
bool kept_style(uint16_t s)
{
    return !solid_wall_style(s);
}

bool thick_tl(const WallPainter::View &v)
{
    return (v.segments & WallTopLeft) && (v.tl_style == kPlainWall || v.tl_style == kDiagonalWall);
}

bool thick_tr(const WallPainter::View &v)
{
    return (v.segments & WallTopRight) && (v.tr_style == kPlainWall || v.tr_style == kDiagonalWall);
}

} // namespace

WallPainter::WallPainter(const Lot &lot, Content &content, SpriteCache &sprites, const Projector &projector,
                         WallMode mode, int top_level, const std::vector<uint16_t> *cut_rooms)
    : lot_(lot), content_(content), sprites_(sprites), p_(projector), zoom_index_(projector.m.zoom_index),
      rotation_(projector.rotation & 3), mode_(mode), top_level_(top_level)
{
    for (int level = 0; level < kLotLevels; level++) {
        views_[level].resize(size_t(W) * W);
        for (size_t i = 0; i < views_[level].size(); i++) {
            const WallTile &w = lot.walls[level][i];
            View &v = views_[level][i];
            v.segments = w.segments;
            v.tl_style = w.top_left_style;
            v.tr_style = w.top_right_style;
            v.tl_pattern = w.top_left_pattern;
            v.tr_pattern = w.top_right_pattern;
            v.bl_pattern = w.bottom_left_pattern;
            v.br_pattern = w.bottom_right_pattern;
            if (!(w.segments & (WallHorizontalDiag | WallVerticalDiag))) {
                v.tl_style = !(w.segments & WallTopLeft) ? 0 : kept_style(v.tl_style) ? v.tl_style : kPlainWall;
                v.tr_style = !(w.segments & WallTopRight) ? 0 : kept_style(v.tr_style) ? v.tr_style : kPlainWall;
            }
        }
    }

    // Doors and windows put their own style (a hole) on the wall they face.
    for (const LotObject &o : lot.objects) {
        const uint16_t flags2 = uint16_t(o.data[kObjFlagField2]);
        if (o.x < 0 || o.y < 0 || !(flags2 & (kArchitecturalWindow | kArchitecturalDoor)))
            continue;
        const ObjectInfo *info = content.object(o.guid);
        if (!info)
            continue;
        StyleRef style{ info->objd.get(Objd::WallStyle), 0 };
        if (style.id > 21 && style.id < 256)   // sprites stored in the object's own file
            style.object = o.guid;
        const bool door = flags2 & kArchitecturalDoor;
        const int placement = o.data[kObjWallPlacement];
        const int facing = (o.direction() / 2) % 4;   // north, east, south, west
        View &v = views_[o.level][o.x + o.y * W];
        for (int k = 0; k < 4; k++) {
            if (!(placement & (1 << k)))   // wall required in front, on right, behind, on left
                continue;
            // 0 north (this tile's top right), 3 west (its top left); 1 east and 2 south are
            // the next tile's top left and top right (FreeSims ignores those: the game's
            // own lots don't use them, but build mode can put a window either way).
            const int side = (facing + k) % 4;
            View *w = &v;
            if (side == 1 && o.x + 1 < W)
                w = &views_[o.level][(o.x + 1) + o.y * W];
            else if (side == 2 && o.y + 1 < W)
                w = &views_[o.level][o.x + (o.y + 1) * W];
            // A window of several tiles comes in pairs of parts, one each side of the wall, whose hole
            // shapes are each other's mirror (the Western window's ends, ExpansionPack2 windows4.iff:
            // part 1 inside 2450, outside 2470; part 3 inside 2470, outside 2450). Each is the shape
            // seen from its own side, so the wall keeps both and the camera's side picks (rotated()).
            // Ours kept whichever came last: the far side's end shape on the Granges' shed, a slit by
            // the corner (the user's report, 2026-11-13). TS_WINDOW_SIDES_OLD=1: as before.
            static const bool sides_old = TS_ENV("TS_WINDOW_SIDES_OLD") != nullptr;
            const bool back = !sides_old && (side == 1 || side == 2);   // on the far side of the tile's wall
            if (side == 0 || side == 2) {
                if (style.id)
                    (back ? w->tr_object_back : w->tr_object) = style;
                w->tr_door = w->tr_door || door;
            } else {
                if (style.id)
                    (back ? w->tl_object_back : w->tl_object) = style;
                w->tl_door = w->tl_door || door;
            }
        }
    }

    cutaway_.assign(size_t(W) * W, false);
    if (top_level_ >= 0 && top_level_ < kLotLevels)
        make_cutaway(top_level_, cut_rooms);
    for (int level = 0; level < kLotLevels; level++)
        make_cuts_and_junctions(level);
    list_wants();
}

bool WallPainter::cut(int x, int y, int level) const
{
    return level == top_level_ && in_lot(x, y) && cutaway_[size_t(x) + size_t(y) * W];
}

bool WallPainter::edge_cut(int x, int y, int segment, int level) const
{
    if (level != top_level_ || level < 0 || level >= kLotLevels || cuts_[level].empty())
        return false;
    // The piece belongs to the tile on its north or west side (make_cuts_and_junctions).
    int px = x, py = y;
    bool left = true;
    switch (segment) {
    case WallTopLeft: left = true; break;
    case WallTopRight: left = false; break;
    case WallBottomRight: px = x + 1; left = true; break;
    case WallBottomLeft: py = y + 1; left = false; break;
    default: return false;
    }
    if (!in_lot(px, py))
        return false;
    const Cuts &c = cuts_[level][size_t(px) + size_t(py) * W];
    return (left ? c.tl : c.tr) != Up;
}

// VMArchitectureTools.GenerateRoomCut: a tile is cut when a room to cut lies just beyond it,
// away from the camera: those in `cut_rooms` (the game's, following the cursor), or with
// none given every indoor room.
void WallPainter::make_cutaway(int level, const std::vector<uint16_t> *cut_rooms)
{
    if (mode_ == WallMode::Down) {
        cutaway_.assign(cutaway_.size(), true);
        return;
    }
    if (mode_ == WallMode::Up)
        return;
    const RoomMap rooms = build_rooms(lot_, level);
    static const int kAway[4][2] = { { -1, -1 }, { -1, 1 }, { 1, 1 }, { 1, -1 } };
    const int dx = kAway[rotation_][0], dy = kAway[rotation_][1];
    auto empty = [&](int x, int y) { return !in_lot(x, y) || base(x, y, level).segments == 0; };
    for (int y1 = 0; y1 < W; y1++)
        for (int x1 = 0; x1 < W; x1++) {
            if (empty(x1, y1) && empty(x1 + 1, y1) && empty(x1 - 1, y1) && empty(x1, y1 + 1) && empty(x1, y1 - 1))
                continue;
            bool c = false;
            for (int i = 0; i < 3 && !c; i++) {
                int x = x1 + (i == 1 ? dx : 0), y = y1 + (i == 2 ? dy : 0);
                for (int j = 0; j < (i > 0 ? 4 : 5); j++) {
                    if (!in_lot(x, y))
                        break;
                    const bool cut_here =
                        cut_rooms ? std::find(cut_rooms->begin(), cut_rooms->end(), rooms.room(x, y)) != cut_rooms->end()
                                  : rooms.indoors(x, y);
                    if (cut_here) {
                        c = true;
                        break;
                    }
                    x += dx;
                    y += dy;
                }
            }
            cutaway_[size_t(x1) + size_t(y1) * W] = c;
        }
}

// WallComponent.GenerateWallData: how each wall is cut, and the junction caps.
void WallPainter::make_cuts_and_junctions(int level)
{
    cuts_[level].assign(size_t(W) * W, Cuts());
    junctions_[level].assign(size_t(W) * W, {});
    std::vector<uint8_t> up(size_t(W) * W, 0), down(size_t(W) * W, 0);
    const bool can_cut = level == top_level_;
    auto at = [](int x, int y) { return size_t(x) + size_t(y) * W; };
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            const View &wall = base(x, y, level);
            if (!wall.segments)
                continue;
            Cuts r;
            if (can_cut && cut(x, y, level)) {
                // GetCutEdges: which neighbours keep their walls up.
                auto up_at = [&](int ax, int ay) { return !cut(ax, ay, level); };
                const bool px = up_at(x + 1, y), nx = up_at(x - 1, y), py = up_at(x, y + 1), ny = up_at(x, y - 1);
                const bool snxny = up_at(x - 1, y - 1), spy = up_at(x - 1, y + 1), spx = up_at(x + 1, y - 1);
                const bool any = px || nx || py || ny || snxny || spy || spx;
                if (thick_tl(wall) && wall.tl_style != kDiagonalWall) {
                    if (!any)
                        r.tl = Down;
                    else if (nx)
                        r.tl = Up;   // on the very edge of the cut
                    else if (py || spy)
                        r.tl = (ny || snxny) ? Down : DownRightUpLeft;
                    else if (ny || snxny)
                        r.tl = DownLeftUpRight;
                    else
                        r.tl = Down;
                }
                if (wall.tr_style == kPlainWall) {   // diagonals too
                    // A diagonal facing the camera (the horizontal one from the front and the
                    // back, the vertical one from the sides) is up, as a straight wall is, when
                    // the tiles beyond it, away from the camera, aren't cut: the back corners of
                    // a bay stay up with the walls either side. (FreeSO puts a diagonal with
                    // uncut tiles on both sides down, which cut those corners.)
                    static const int kFar[4][2] = { { -1, -1 }, { -1, 1 }, { 1, 1 }, { 1, -1 } };
                    const int fx = kFar[rotation_][0], fy = kFar[rotation_][1];
                    const bool facing = (wall.segments == WallHorizontalDiag) == (fx == fy);
                    if ((wall.segments == WallHorizontalDiag || wall.segments == WallVerticalDiag) && facing &&
                        (up_at(x + fx, y) || up_at(x, y + fy))) {
                        r.tr = Up;
                    } else if (wall.segments == WallHorizontalDiag || wall.segments == WallVerticalDiag) {
                        bool left, right;
                        if (wall.segments == WallHorizontalDiag) {
                            left = py || nx;
                            right = ny || px;
                        } else {
                            left = py || px;
                            right = ny || nx;
                        }
                        r.tr = !any || (left && right) ? Down : left ? DownRightUpLeft : right ? DownLeftUpRight : Down;
                    } else if (!any) {
                        r.tr = Down;
                    } else if (ny) {
                        r.tr = Up;
                    } else if (px || spx) {
                        r.tr = (nx || snxny) ? Down : DownLeftUpRight;
                    } else if (nx || snxny) {
                        r.tr = DownRightUpLeft;
                    } else {
                        r.tr = Down;
                    }
                }
            }
            cuts_[level][at(x, y)] = r;
#if !defined(__vita__)
            if (static const char *ct = TS_ENV("TS_CUT_TILE"); ct && can_cut) {   // debug: "x y": how that tile's walls were cut
                int cx = -1, cy = -1;
                if (std::sscanf(ct, "%d %d", &cx, &cy) == 2 && cx == x && cy == y) {
                    auto c = [&](int ax, int ay) { return int(cut(ax, ay, level)); };
                    std::printf("cut tile (%d, %d): cut %d; x+1 %d x-1 %d y+1 %d y-1 %d, (x-1,y-1) %d (x-1,y+1) %d (x+1,y-1) %d; "
                                "tl %d tr %d, doors tl %d tr %d\n",
                                x, y, c(x, y), c(x + 1, y), c(x - 1, y), c(x, y + 1), c(x, y - 1), c(x - 1, y - 1), c(x - 1, y + 1),
                                c(x + 1, y - 1), int(r.tl), int(r.tr), int(wall.tl_door), int(wall.tr_door));
                }
            }
#endif

            auto add = [&](bool is_down, int jx, int jy, uint8_t flag) {
                if (in_lot(jx, jy))
                    (is_down ? down : up)[at(jx, jy)] |= flag;
            };
            if ((wall.segments & WallTopLeft) && !(wall.tl_door && r.tl) && thick_tl(wall)) {
                add(r.tl, x, y, JBottomLeft);
                add(r.tl, x, y + 1, JTopRight);
            }
            if ((wall.segments & WallTopRight) && !(wall.tr_door && r.tr) && thick_tr(wall)) {
                add(r.tr, x, y, JBottomRight);
                add(r.tr, x + 1, y, JTopLeft);
            }
            const bool diag_style = wall.tr_style == kPlainWall || wall.tr_style == kDiagonalWall;
            if (wall.segments == WallVerticalDiag && diag_style) {
                add(r.tr, x, y, JDiagBottom);
                add(r.tr, x + 1, y + 1, JDiagTop);
            } else if (wall.segments == WallHorizontalDiag && diag_style) {
                add(r.tr, x + 1, y, JDiagLeft);
                add(r.tr, x, y + 1, JDiagRight);
            }
        }

    // A wall's cap is drawn with the front-most of the four tiles round its corner, over
    // what stands behind the wall. A cut-down wall's cap went with the back-most, so that
    // furniture in front of it (a porch bench in House05) covered it; but walls drawn after
    // it covered it too (the Goths' iron fence ending at the bedroom's back corner hid half
    // the corner's cap: a step in the wall's top). Now furniture against a lowered cap is
    // sorted by the depth pass (the cap's owner, in draw()). TS_OLD_CAPS: debug, as it was.
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            const bool is_up = up[at(x, y)] != 0;
            const uint8_t flags = is_up ? up[at(x, y)] : down[at(x, y)];
            if (!flags || kJunctionFrames.frame[flags] < 0)
                continue;
            int best_x = -1, best_y = -1;
            float best = -1e9f;
            static const bool old_caps = TS_ENV("TS_OLD_CAPS") != nullptr;
            const float sign = is_up || !old_caps ? 1.0f : -1.0f;   // the front-most (or the back-most)
            for (auto [tx, ty] : { std::pair{ x - 1, y - 1 }, std::pair{ x, y - 1 }, std::pair{ x - 1, y },
                                   std::pair{ x, y } }) {
                if (!in_lot(tx, ty))
                    continue;
                float sx, sy;
                p_.box(float(tx), float(ty), 0, sx, sy);
                if (sy * sign > best) {
                    best = sy * sign;
                    best_x = tx;
                    best_y = ty;
                }
            }
            junctions_[level][at(best_x, best_y)].push_back({ x, y, flags, is_up });
        }
}

// WallComponent.RotateCuts.
WallPainter::Cuts WallPainter::rotated_cuts(int x, int y, int level) const
{
    const std::vector<Cuts> &cuts = cuts_[level];
    auto flip = [](Cut c) { return c == DownLeftUpRight ? DownRightUpLeft : c == DownRightUpLeft ? DownLeftUpRight : c; };
    const Cuts in = cuts[size_t(x) + size_t(y) * W];
    if (base(x, y, level).segments >= 16) {   // diagonals just flip on the two far rotations
        if (rotation_ <= 1)
            return in;
        return { flip(in.tl), flip(in.tr) };
    }
    Cuts out;
    switch (rotation_) {
    case 0:
        return in;
    case 1:
        out.tr = in.tl;
        if (y + 1 < W)
            out.tl = flip(cuts[size_t(x) + size_t(y + 1) * W].tr);
        return out;
    case 2:
        out.tr = in.tl;
        if (y + 1 < W)
            out.tr = cuts[size_t(x) + size_t(y + 1) * W].tr;
        if (x + 1 < W)
            out.tl = cuts[size_t(x + 1) + size_t(y) * W].tl;
        return { flip(out.tl), flip(out.tr) };
    default:
        out.tl = in.tr;
        if (x + 1 < W)
            out.tr = flip(cuts[size_t(x + 1) + size_t(y) * W].tl);
        return out;
    }
}

void WallPainter::offset(int dx, int dy, int &rx, int &ry) const
{
    switch (rotation_) {
    case 0: rx = dx; ry = dy; break;
    case 1: rx = dy; ry = -dx; break;
    case 2: rx = -dx; ry = -dy; break;
    default: rx = -dy; ry = dx; break;
    }
}

// FreeSims WallComponent.RotateWall: the walls on a tile's back edges as seen
// from the current camera, borrowing styles from neighbours where needed.
WallPainter::View WallPainter::rotated(int x, int y, int level) const
{
    const View &in = base(x, y, level);
    // A wall's door or window hole as seen from one side: that side's own, else the other side's.
    auto seen = [](const StyleRef &near, const StyleRef &far) { return near.id ? near : far; };
    if (rotation_ == 0) {   // (a tile's own back walls, seen from the tile)
        View out = in;
        out.tl_object = seen(in.tl_object, in.tl_object_back);
        out.tr_object = seen(in.tr_object, in.tr_object_back);
        return out;
    }
    View out;
    const bool hdiag = in.segments == WallHorizontalDiag, vdiag = in.segments == WallVerticalDiag;
    if (hdiag || vdiag) {
        out.segments = rotation_ % 2 == 0 ? in.segments : uint8_t(hdiag ? WallVerticalDiag : WallHorizontalDiag);
        out.tr_style = in.tr_style;
        const bool keep = (hdiag && rotation_ == 3) || (vdiag && rotation_ == 1);
        // The two floor halves (top-left pattern and style) go with the tile as the floor's
        // pictures turn: swapped when the diagonal changes kind (the side views), else kept.
        // (They swapped with the wall's sides, which put grass in the Goths' bay corners
        // seen from the back, and the porch's tiles inside it from one side.)
        const bool keep_floor = rotation_ % 2 == 0;
        out.br_pattern = keep ? in.br_pattern : in.bl_pattern;
        out.bl_pattern = keep ? in.bl_pattern : in.br_pattern;
        out.tl_style = keep_floor ? in.tl_style : in.tl_pattern;
        out.tl_pattern = keep_floor ? in.tl_pattern : in.tl_style;
        return out;
    }
    auto has = [&](uint8_t s) { return (in.segments & s) != 0; };
    const bool below = y + 1 < W, right = x + 1 < W;
    auto take_tl = [&](const View &from) {   // the neighbour's top-left wall becomes ours (seen from its far side)
        out.tl_style = from.tl_style;
        out.tl_object = seen(from.tl_object_back, from.tl_object);
        out.tl_door = from.tl_door;
    };
    switch (rotation_) {
    case 1:
        out.segments = (has(WallTopLeft) ? WallTopRight : 0) | (has(WallTopRight) ? WallBottomRight : 0) |
                       (has(WallBottomRight) ? WallBottomLeft : 0) | (has(WallBottomLeft) ? WallTopLeft : 0);
        out.tl_pattern = in.bl_pattern;
        out.tr_pattern = in.tl_pattern;
        out.br_pattern = in.tr_pattern;
        out.bl_pattern = in.br_pattern;
        if (below) {
            const View &n = base(x, y + 1, level);
            out.tl_style = n.tr_style;
            out.tl_object = seen(n.tr_object_back, n.tr_object);
            out.tl_door = n.tr_door;
        }
        out.tr_style = in.tl_style;
        out.tr_object = seen(in.tl_object, in.tl_object_back);
        out.tr_door = in.tl_door;
        break;
    case 2:
        out.segments = (has(WallTopLeft) ? WallBottomRight : 0) | (has(WallTopRight) ? WallBottomLeft : 0) |
                       (has(WallBottomRight) ? WallTopLeft : 0) | (has(WallBottomLeft) ? WallTopRight : 0);
        out.tl_pattern = in.br_pattern;
        out.tr_pattern = in.bl_pattern;
        out.br_pattern = in.tl_pattern;
        out.bl_pattern = in.tr_pattern;
        if (below) {
            const View &n = base(x, y + 1, level);
            out.tr_style = n.tr_style;
            out.tr_object = seen(n.tr_object_back, n.tr_object);
            out.tr_door = n.tr_door;
        }
        if (right)
            take_tl(base(x + 1, y, level));
        break;
    default:
        out.segments = (has(WallTopLeft) ? WallBottomLeft : 0) | (has(WallTopRight) ? WallTopLeft : 0) |
                       (has(WallBottomRight) ? WallTopRight : 0) | (has(WallBottomLeft) ? WallBottomRight : 0);
        out.tl_pattern = in.tr_pattern;
        out.tr_pattern = in.br_pattern;
        out.br_pattern = in.bl_pattern;
        out.bl_pattern = in.tl_pattern;
        out.tl_style = in.tr_style;
        out.tl_object = seen(in.tr_object, in.tr_object_back);
        out.tl_door = in.tr_door;
        if (right) {
            const View &n = base(x + 1, y, level);
            out.tr_style = n.tl_style;
            out.tr_object = seen(n.tl_object_back, n.tl_object);
            out.tr_door = n.tl_door;
        }
        break;
    }
    return out;
}

// A face once made is found again by its numbers alone: made afresh, each was four lookups by
// the file's name and three reads of the environment, ~1.4 ms a wall tile on the Vita, 230-520 ms
// of a camera turn's scene rebuild at Solana. The faces stay in the sprite cache through its
// trims (keep_only); a clear (another lot) starts the table again.
// The faces draw() would ask for at each of the other camera angles, in the order the angles
// come (R turns), as draw_side and the diagonal case pick them: the same keys, so a turn finds
// them made.
void WallPainter::list_wants()
{
    std::set<std::array<uint32_t, 3>> seen;
    auto want = [&](uint16_t pattern, StyleRef style, int side, bool down) {
        const std::array<uint32_t, 3> key{ uint32_t(pattern) | uint32_t(side) << 16 | (down ? 1u : 0u) << 24, style.id,
                                           style.object };
        if (seen.insert(key).second)
            wants_.push_back({ pattern, style, side, down });
    };
    auto side_wants = [&](const View &v, Cut cut_type, bool left) {
        if (!(v.segments & (left ? WallTopLeft : WallTopRight)))
            return;
        if (cut_type != Up && (left ? v.tl_door : v.tr_door))
            return;
        const uint16_t own = (left ? v.tl_style : v.tr_style) ? (left ? v.tl_style : v.tr_style) : kPlainWall;
        const StyleRef object = left ? v.tl_object : v.tr_object;
        const uint16_t pattern = left ? v.tl_pattern : v.tr_pattern;
        const int side = left ? 0 : 1;
        StyleRef style{ own, 0 };
        uint16_t overlay = 0;
        bool down = false;
        switch (cut_type) {
        case Down: down = true; break;
        case DownLeftUpRight: style = { kStyleDownLeft, 0 }; overlay = kOverlayDownLeft; break;
        case DownRightUpLeft: style = { kStyleDownRight, 0 }; overlay = kOverlayDownRight; break;
        case Up:
            if (own == kPlainWall && object.id)
                style = object;
            break;
        }
        want(pattern, style, side, down);
        if (overlay)
            want(overlay, StyleRef(), side, down);
        if (left ? thick_tl(v) : thick_tr(v)) {   // the rounded-off ends
            want(pattern, { own, 0 }, side, down);
            want(pattern, { own, 0 }, 1 - side, down);
        }
    };
    const int was = rotation_;
    for (int turn = 1; turn <= 4; turn++) {
        rotation_ = (was + turn) & 3;
        for (int level = 0; level < kLotLevels; level++)
            for (int y = 0; y < W; y++)
                for (int x = 0; x < W; x++) {
                    if (!base(x, y, level).segments)
                        continue;
                    const View v = rotated(x, y, level);
                    const Cuts c = rotated_cuts(x, y, level);
                    if (v.segments == WallHorizontalDiag || v.segments == WallVerticalDiag) {
                        const uint16_t own = v.tr_style ? v.tr_style : kPlainWall;
                        StyleRef style{ own, 0 };
                        uint16_t overlay = 0;
                        bool down = false;
                        switch (c.tr) {
                        case Down: down = true; break;
                        case DownLeftUpRight: style = { kStyleDownLeft, 0 }; overlay = kOverlayDownLeft; break;
                        case DownRightUpLeft: style = { kStyleDownRight, 0 }; overlay = kOverlayDownRight; break;
                        case Up:
                            if (own == kPlainWall && v.tr_object.id)
                                style = v.tr_object;
                            break;
                        }
                        const int side = v.segments == WallHorizontalDiag ? 2 : 3;
                        want(v.br_pattern, style, side, down);
                        if (overlay)
                            want(overlay, StyleRef(), side, down);
                    } else {
                        side_wants(v, c.tl, true);
                        side_wants(v, c.tr, false);
                    }
                }
    }
    rotation_ = was;
}

int WallPainter::warm_faces(double budget_ms)
{
    const auto t0 = ts::FastClock::now();
    while (warmed_ < wants_.size()) {
        const Want &w = wants_[warmed_++];
        face_image(w.pattern, w.style, w.side, w.down);
        if (std::chrono::duration<double, std::milli>(ts::FastClock::now() - t0).count() >= budget_ms)
            break;
    }
    return int(wants_.size() - warmed_);
}

const Image *WallPainter::face_image(uint16_t pattern, StyleRef style, int side, bool down)
{
    static struct {
        unsigned generation = ~0u;
        std::map<std::array<uint32_t, 3>, const Image *> faces;
    } kept;
    if (kept.generation != sprites_.generation()) {
        kept.generation = sprites_.generation();
        kept.faces.clear();
    }
    const std::array<uint32_t, 3> key{ uint32_t(pattern) | uint32_t(side) << 16 | (down ? 1u : 0u) << 24 |
                                           uint32_t(zoom_index_) << 26,
                                       style.id, style.object };
    if (auto it = kept.faces.find(key); it != kept.faces.end())
        return it->second;
    const Image *done = make_face_image(pattern, style, side, down);
    faces_made_++;
    kept.faces[key] = done;
    return done;
}

const Image *WallPainter::make_face_image(uint16_t pattern, StyleRef style, int side, bool down)
{
    static const FileRef *const globals = content_.file("walls.iff");   // (the index never changes)
    if (!globals)
        return nullptr;

    // The wallpaper: a custom .wll file, or a global pattern.
    const Image *paper = nullptr;
    if (const std::string *name = lot_.custom_wall(pattern)) {
        if (const FileRef *ref = content_.file(*name))
            paper = sprites_.spr(*ref, kCustomPattern[zoom_index_], uint32_t(side));
    } else
        paper = sprites_.spr(*globals, uint16_t(kPatternBase + zoom_index_ * kPatternZoomStep + pattern), uint32_t(side));
    if (!paper)
        return nullptr;

    // The shape: an object's own style sprites, or a global style.
    const Image *mask = nullptr;
    if (style.object) {
        if (const ObjectInfo *info = content_.object(style.object)) {
            uint16_t first = info->objd.get(Objd::WallStyleSpriteID);
            mask = sprites_.spr(info->ref, uint16_t(first + zoom_index_ + (down ? 3 : 0)), uint32_t(side));
        }
    } else if (style.id) {
        uint16_t id = uint16_t(style.id * 2 + zoom_index_ * kStyleZoomStep);
        if (down)
            mask = sprites_.spr(*globals, uint16_t(id + 1), uint32_t(side));
        if (!mask)
            mask = sprites_.spr(*globals, id, uint32_t(side));
    }
    if (TS_ENV("TS_FACE_LOG") && side >= 2)   // debug: a diagonal face's pictures
        std::printf("face pattern %d style %d side %d zoom %d: paper %dx%d mask %dx%d\n", int(pattern), int(style.id), side,
                    zoom_index_, paper->width, paper->height, mask ? mask->width : 0, mask ? mask->height : 0);
    if (const char *dir = TS_ENV("TS_FACE_PNG"); dir && (side >= 2 || TS_ENV("TS_FACE_ALL"))) {   // debug: a diagonal face's pictures as PNGs
        const std::string stem = std::string(dir) + "/face_p" + std::to_string(pattern) + "_s" + std::to_string(style.id) + "_" +
                                 std::to_string(side) + "_z" + std::to_string(zoom_index_) + (down ? "_down" : "");
        write_png(stem + "_paper.png", paper->width, paper->height, paper->rgba);
        if (mask)
            write_png(stem + "_mask.png", mask->width, mask->height, mask->rgba);
    }
    const Image *done = sprites_.masked(*paper, mask ? *mask : *paper);
    if (const char *dir = TS_ENV("TS_FACE_PNG"); dir && done && (side >= 2 || TS_ENV("TS_FACE_ALL")))   // debug
        write_png(std::string(dir) + "/face_p" + std::to_string(pattern) + "_s" + std::to_string(style.id) + "_" +
                      std::to_string(side) + "_z" + std::to_string(zoom_index_) + (down ? "_down" : "") + "_masked.png",
                  done->width, done->height, done->rgba);
    return done;
}

void WallPainter::draw_face(DrawList &out, int tx, int ty, int level, int side, const Image &img, int src_x, int src_w)
{
    float bx, by;
    const float z = level * kLevelHeight + lot_.ground(float(tx), float(ty));
    p_.box(float(tx), float(ty), z, bx, by);
    const int left = int(bx) + kFaceX[zoom_index_][side], top = int(by) + kFaceY[zoom_index_][side];
    DrawCmd &c = out.sprite_part(img, left, top, src_x, 0, src_w, img.height, false, false);
    c.set_plane(wall_depth(back_corner_depth(p_, float(tx), float(ty), z), zoom_index_, side, float(left), float(top)));
    static int skip_x = -1, skip_y = -1;   // debug: TS_WALL_SKIP="x y" leaves that tile's wall faces out
    static const bool skip_read = [] {
        if (const char *t = TS_ENV("TS_WALL_SKIP"))
            std::sscanf(t, "%d %d", &skip_x, &skip_y);
        return true;
    }();
    (void)skip_read;
    (void)c;
    if (tx == skip_x && ty == skip_y)
        out.cmds.pop_back();
}

// One of a tile's two back walls (left: top-left), as FreeSims draws it.
void WallPainter::draw_side(DrawList &out, int x, int y, int level, const View &v, Cut cut_type, bool left,
                            PaintStats &stats)
{
    if (!(v.segments & (left ? WallTopLeft : WallTopRight)))
        return;
    if (cut_type != Up && (left ? v.tl_door : v.tr_door))
        return;   // a door in a lowered wall leaves a gap
    const uint16_t own = (left ? v.tl_style : v.tr_style) ? (left ? v.tl_style : v.tr_style) : kPlainWall;
    const StyleRef object = left ? v.tl_object : v.tr_object;
    const uint16_t pattern = left ? v.tl_pattern : v.tr_pattern;
    const int side = left ? 0 : 1;

    StyleRef style{ own, 0 };
    uint16_t overlay = 0;
    bool down = false;
    switch (cut_type) {
    case Down: down = true; break;
    case DownLeftUpRight: style = { kStyleDownLeft, 0 }; overlay = kOverlayDownLeft; break;
    case DownRightUpLeft: style = { kStyleDownRight, 0 }; overlay = kOverlayDownRight; break;
    case Up:
        if (own == kPlainWall && object.id)
            style = object;   // a door or window's hole
        break;
    }
    const Image *img = face_image(pattern, style, side, down);
    if (!img) {
        if (stats.walls_missing++ < 5)
            stats.notes.push_back("no wall pattern " + std::to_string(pattern));
        return;
    }
    static const bool wall_list = TS_ENV("TS_WALL_LIST") != nullptr;   // debug: each wall face drawn
    if (wall_list)
        std::printf("wall (%d, %d) %s pattern %d cut %d\n", x, y, left ? "tl" : "tr", int(pattern), int(cut_type));
    draw_face(out, x, y, level, side, *img, 0, img->width);
    if (cut_type != Up)
        out.cmds.back().owner = kLoweredWallOwner;
    stats.walls++;
    if (overlay)
        if (const Image *o = face_image(overlay, StyleRef(), side, down)) {
            draw_face(out, x, y, level, side, *o, 0, o->width);
            out.cmds.back().owner = kLoweredWallOwner;
        }

    // Thick walls get a rounded-off end where they stop: a sliver of the
    // wall drawn on the neighbouring tile, at the far and the near end.
    if (!(left ? thick_tl(v) : thick_tr(v)))
        return;
    static const bool no_ends = TS_ENV("TS_NO_WALL_ENDS") != nullptr;   // debug: without the rounded-off ends
    if (no_ends)
        return;
    const StyleRef normal{ own, 0 };   // the ends use the wall's own shape even in a cut
    int w = 3 << zoom_index_;
    int fx, fy, nx, ny, n2x, n2y;
    if (left) {
        offset(0, -1, fx, fy);
        offset(0, 1, nx, ny);
        offset(-1, 1, n2x, n2y);
    } else {
        offset(-1, 0, fx, fy);
        offset(1, 0, nx, ny);
        offset(1, -1, n2x, n2y);
    }
    fx += x, fy += y, nx += x, ny += y, n2x += x, n2y += y;
    const bool behind_diag = x > 0 && y > 0 && (base(x - 1, y - 1, level).segments & WallVerticalDiag);
    // A diagonal on the next tile carries the wall on (the original fuses them, as if built as one
    // run): no rounded-off end there. Ours treated a straight wall meeting a diagonal as stopping,
    // and its end showed as a strip of slanted wall at the joint (the Goths' bay, the user's
    // "walls made individually" look; matched against the original 2026-10-27). TS_DIAG_ENDS=1
    // restores the old ends (debug).
    static const bool diag_ends = TS_ENV("TS_DIAG_ENDS") != nullptr;
    auto diagonal = [&](int tx, int ty) {
        return !diag_ends && in_lot(tx, ty) && (base(tx, ty, level).segments & (WallHorizontalDiag | WallVerticalDiag)) != 0;
    };
    // The wall carries on to a neighbour's wall, unless that's a door in a lowered wall: the door
    // leaves a gap there, and the wall stops at it with its end (ours ran the wall on into the
    // gap with no end, so a lowered wall broke off at a doorway with a bare cut; the user's
    // walls-down shots against the original, 2026-10-28). TS_DOOR_ENDS=0: debug, as it was.
    static const bool door_ends = !TS_ENV("TS_DOOR_ENDS") || std::atoi(TS_ENV("TS_DOOR_ENDS")) != 0;
    auto carries_on = [&](const View &n, int tx, int ty, bool tl) {
        if (!(tl ? thick_tl(n) : thick_tr(n)))
            return false;
        if (!door_ends || !(tl ? n.tl_door : n.tr_door))
            return true;
        const Cuts c = rotated_cuts(tx, ty, level);
        return (tl ? c.tl : c.tr) == Up;
    };
    if (!(left ? thick_tr(v) : thick_tl(v)) && in_lot(fx, fy) && !diagonal(fx, fy)) {
        const View far = rotated(fx, fy, level);
        if (!carries_on(far, fx, fy, left)) {
            if (const Image *e = face_image(pattern, normal, side, cut(fx, fy, level))) {
                const int fw = behind_diag ? w * 2 / 3 : w;
                draw_face(out, fx, fy, level, side, *e, left ? 0 : e->width - fw, fw);
            }
        }
    }
    if (in_lot(nx, ny) && in_lot(n2x, n2y) && !diagonal(nx, ny) && !diagonal(n2x, n2y)) {
        const View near = rotated(nx, ny, level);
        if (!carries_on(near, nx, ny, true) && !carries_on(near, nx, ny, false)) {
            const View near2 = rotated(n2x, n2y, level);
            if (!carries_on(near2, n2x, n2y, !left)) {
                // The end is lowered with the wall unless the wall's slope rises at this end: the
                // near end of a left wall (its +y end) is up in a down-right slope, a right wall's
                // (its +x end) in a down-left one. Both sides were taken as the left's, so a right
                // wall sloping down to a doorway ended in a full-height strip there (the Bobs'
                // patio door, the user's 2026-11-03 screenshot; the 2026-11-02 report's slice).
                const bool d = cut(x, y, level) && style.id != (left ? kStyleDownRight : kStyleDownLeft);
                if (const Image *e = face_image(pattern, normal, 1 - side, d))
                    draw_face(out, n2x, n2y, level, 1 - side, *e, left ? e->width - w : 0, w);
            }
        }
    }
}

void WallPainter::draw(DrawList &out, int x, int y, int level, PaintStats &stats)
{
    if (base(x, y, level).segments != 0) {
        const View v = rotated(x, y, level);
        const Cuts c = rotated_cuts(x, y, level);
        if (v.segments == WallHorizontalDiag || v.segments == WallVerticalDiag) {
            const uint16_t own = v.tr_style ? v.tr_style : kPlainWall;
            StyleRef style{ own, 0 };
            uint16_t overlay = 0;
            bool down = false;
            switch (c.tr) {
            case Down: down = true; break;
            case DownLeftUpRight: style = { kStyleDownLeft, 0 }; overlay = kOverlayDownLeft; break;
            case DownRightUpLeft: style = { kStyleDownRight, 0 }; overlay = kOverlayDownRight; break;
            case Up:
                if (own == kPlainWall && v.tr_object.id)
                    style = v.tr_object;
                break;
            }
            const int side = v.segments == WallHorizontalDiag ? 2 : 3;
            static const int skip_side = TS_ENV("TS_SKIP_DIAG") ? std::atoi(TS_ENV("TS_SKIP_DIAG")) : -1;   // debug
            if (side == skip_side) {
            } else if (const Image *img = face_image(v.br_pattern, style, side, down)) {
                draw_face(out, x, y, level, side, *img, 0, img->width);
                stats.walls++;
                if (overlay)
                    if (const Image *o = face_image(overlay, StyleRef(), side, down))
                        draw_face(out, x, y, level, side, *o, 0, o->width);
            } else if (stats.walls_missing++ < 5) {
                stats.notes.push_back("no wall pattern " + std::to_string(v.br_pattern));
            }
        } else {
            draw_side(out, x, y, level, v, c.tl, true, stats);
            draw_side(out, x, y, level, v, c.tr, false, stats);
        }
    }

    // (Looked up once: by name, for each of the 8,192 tiles of a scene, the index lookup alone was
    // most of the scene rebuild's "walls" at Solana on the Vita, 160-230 ms a camera turn.)
    static const FileRef *const globals = content_.file("walls.iff");
    for (const Junction &j : junctions_[level][size_t(x) + size_t(y) * W]) {
        const int frame = kJunctionFrames.frame[rotate_junction(rotation_, j.flags)];
        const Image *img = globals && frame >= 0
                               ? sprites_.spr(*globals, uint16_t(kJunctionBase + zoom_index_), uint32_t(frame))
                               : nullptr;
        static const bool no_caps = TS_ENV("TS_NO_CAPS") != nullptr;   // debug: without the caps
        if (TS_ENV("TS_WALL_LIST"))   // debug: each cap drawn
            std::printf("cap at corner (%d, %d) with tile (%d, %d): flags %d frame %d %s\n", j.x, j.y, x, y, int(j.flags), frame,
                        j.up ? "up" : "down");
        if (!img || no_caps)
            continue;
        const float z = (j.up ? (level + 1) * kLevelHeight : 0.3f + level * kLevelHeight) +
                        lot_.ground(j.x - 0.5f, j.y - 0.5f);
        float bx, by;
        p_.box(j.x - 0.5f, j.y - 0.5f, z, bx, by);
        // A lowered wall's top a pixel lower at near zoom: one row of furniture behind the wall
        // showed between it and the wall's face (the Goths' loveseat), measured against the original.
        const int top = int(by) + kJunctionY[zoom_index_] + (!j.up && zoom_index_ == 2 ? 1 : 0);
        DrawCmd &c = out.sprite(*img, int(bx) + kJunctionX[zoom_index_], top);
        // A lowered wall's cap a little nearer than its top row's depth: as it was, the Goths'
        // loveseat behind a lowered wall covered the wall's top, where the original shows the wall
        // over it. Measured, not from the original's code: 8 rows at near zoom; 16 or more and the
        // cap shows over Bella standing in front of the porch wall. TS_CAP_ROWS: debug, the rows;
        // TS_OLD_CAP_DEPTH: debug, as it was.
        static const bool old_depth = TS_ENV("TS_OLD_CAP_DEPTH") != nullptr;
        static const int rows = TS_ENV("TS_CAP_ROWS") ? std::atoi(TS_ENV("TS_CAP_ROWS")) : 8;
        const int corner_row = old_depth || j.up ? 0 : rows >> (2 - zoom_index_);
        c.set_plane(floor_depth(back_corner_depth(p_, j.x - 0.5f, j.y - 0.5f, z), zoom_index_, float(top - corner_row), 257));
        // A lowered wall's top goes through the depth pass with furniture touching it, as the wall
        // does (drawn with the back-most tile, furniture behind the wall came after it).
        if (!j.up && !old_depth)
            c.owner = kLoweredWallOwner;
    }
}

} // namespace ts
