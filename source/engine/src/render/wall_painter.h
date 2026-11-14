/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Wall drawing follows FreeSims (SimsVille/World/components/WallComponent.cs,
 * ContentManager/WorldWallProvider.cs, SimsAntics/entities/VMGameObject.cs,
 * SimsAntics/utils/VMArchitectureTools.cs).
 */

// Draws a lot's walls from the game's art: a wallpaper pattern cut to the
// shape of a wall style (full height, walls down, fences, door and window
// holes set by objects), the cutaway around rooms, the rounded ends of
// walls and the caps where walls meet.
#pragma once

#include <cstdint>
#include <vector>

#include "files/content.h"
#include "render/draw_list.h"
#include "render/projection.h"
#include "render/sprite_cache.h"
#include "world/lot.h"

namespace ts {

struct PaintStats;

enum class WallMode {
    Up,        // every wall full height
    Cutaway,   // walls in front of rooms down, the rest up (the game's default)
    Down,      // every wall on the viewed storey down
};

class WallPainter {
public:
    // Walls are cut only on `top_level`, the highest storey drawn; the
    // storeys below keep theirs up, as in the game.
    WallPainter(const Lot &lot, Content &content, SpriteCache &sprites, const Projector &projector, WallMode mode,
                int top_level, const std::vector<uint16_t> *cut_rooms = nullptr);

    // Draws the walls on the back edges of tile (x, y) as seen from this
    // camera, the wall ends they need, and the junction caps whose
    // front-most tile this is.
    void draw(DrawList &out, int x, int y, int level, PaintStats &stats);

    // Wall faces composited (paper on shape) by this painter's draws: the cost of a new look.
    int faces_made() const { return faces_made_; }

    // The faces the other three camera angles would composite (each a card read on the Vita: a
    // turn cost 60-90 ms of them, 2026-10-30), made a few at a time in the frames' slack: at
    // most budget_ms of them; returns how many are still to make.
    int warm_faces(double budget_ms);

    // True where walls are cut down on `level` (objects on walls there hide).
    bool cut(int x, int y, int level) const;
    // Whether the wall piece on `segment` (WallTopLeft/TopRight/BottomRight/BottomLeft) of tile
    // (x, y) is drawn lowered by the cutaway: what a door or window in it follows.
    bool edge_cut(int x, int y, int segment, int level) const;

    struct StyleRef {
        uint16_t id = 0;          // global style (1 = plain wall), or an object's own style
        uint32_t object = 0;      // GUID whose file holds the style sprites, 0 for global
    };
    struct View {
        uint8_t segments = 0;
        uint16_t tl_style = 0, tr_style = 0;
        uint16_t tl_pattern = 0, tr_pattern = 0, bl_pattern = 0, br_pattern = 0;
        StyleRef tl_object, tr_object;   // styles set by doors and windows
        // (Base views only: the style a door or window on the wall's far side sets, the side away from
        // the tile it belongs to. rotated() gives the one of the side the camera sees in tl/tr_object.)
        StyleRef tl_object_back, tr_object_back;
        bool tl_door = false, tr_door = false;
    };

    // The walls of tile (x, y) as seen from this camera. For diagonal walls,
    // tl_pattern and tl_style hold the floors of the two half-tiles.
    View rotated(int x, int y, int level) const;

private:
    enum Cut : uint8_t { Up = 0, DownLeftUpRight = 1, DownRightUpLeft = 2, Down = 3 };
    struct Cuts {
        Cut tl = Up, tr = Up;
    };
    struct Junction {
        int x, y;          // the tile whose top corner (x - 0.5, y - 0.5) it caps
        uint8_t flags;     // unrotated JunctionFlags
        bool up;
    };

    mutable int faces_made_ = 0;
    struct Want {
        uint16_t pattern;
        StyleRef style;
        int side;
        bool down;
    };
    std::vector<Want> wants_;   // faces the other angles need, the next angle's first
    size_t warmed_ = 0;
    void list_wants();
    const View &base(int x, int y, int level) const { return views_[level][x + y * kLotArraySize]; }
    static bool in_lot(int x, int y) { return x >= 0 && y >= 0 && x < kLotArraySize && y < kLotArraySize; }
    void make_cutaway(int level, const std::vector<uint16_t> *cut_rooms);
    void make_cuts_and_junctions(int level);
    Cuts rotated_cuts(int x, int y, int level) const;
    void offset(int dx, int dy, int &rx, int &ry) const;   // FreeSims RotateOffset

    // The wallpaper cut to a style's shape (no style: the pattern's own shape).
    const Image *face_image(uint16_t pattern, StyleRef style, int side, bool down);
    const Image *make_face_image(uint16_t pattern, StyleRef style, int side, bool down);
    void draw_face(DrawList &out, int tx, int ty, int level, int side, const Image &img, int src_x, int src_w);
    void draw_side(DrawList &out, int x, int y, int level, const View &v, Cut cut, bool left, PaintStats &stats);

    const Lot &lot_;
    Content &content_;
    SpriteCache &sprites_;
    Projector p_;
    int zoom_index_;    // 0 far, 1 medium, 2 near
    int rotation_;
    WallMode mode_;
    int top_level_;
    std::vector<View> views_[kLotLevels];
    std::vector<bool> cutaway_;                 // top level only
    std::vector<Cuts> cuts_[kLotLevels];
    std::vector<std::vector<Junction>> junctions_[kLotLevels];   // by the tile that draws them
};

} // namespace ts
