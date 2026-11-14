/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Screen-space metrics follow FreeSims (SimsVille/World: WorldState.cs, WorldSpace.cs).
 */

// How lot space maps to the screen at each of the game's zooms and camera angles.
#pragma once

#include <cmath>

namespace ts {

enum class Zoom { Near = 1, Medium = 2, Far = 3 };

constexpr float kLevelHeight = 2.95f;   // tiles of height per storey

struct Metrics {
    float half_w, half_h;         // half a tile's diamond
    int cadge_w, baseline;        // object sprite box: width, and the tile centre's y in it
    int floor_x, floor_y;         // floor sprite position in that box
    int floor_base;               // SPR2 id offset of this zoom's floor sprites
    int custom_floor_id;          // SPR2 id of this zoom in a .flr file
    float one_unit;               // pixels per world unit (for heights)
    int zoom_index;               // 0 far, 1 medium, 2 near (the order of the zoom sprites in the files)
};

inline Metrics metrics(Zoom z)
{
    switch (z) {
    case Zoom::Far:
        return { 16, 8, 34, 87, 2, 79, 0, 1, 32 / std::sqrt(2.0f), 0 };
    case Zoom::Medium:
        return { 32, 16, 68, 174, 3, 158, 256, 257, 64 / std::sqrt(2.0f), 1 };
    default:
        return { 64, 32, 136, 348, 5, 316, 512, 513, 128 / std::sqrt(2.0f), 2 };
    }
}

struct Projector {
    Metrics m;
    int rotation;
    float off_x = 0, off_y = 0;

    // FreeSims WorldSpace.GetScreenFromTile: the top-left of a tile's sprite box.
    void box(float x, float y, float z, float &sx, float &sy) const
    {
        switch (rotation & 3) {
        case 0:
            sx = (x - y) * m.half_w;
            sy = (x + y) * m.half_h;
            break;
        case 1:
            sx = (-x - y) * m.half_w;
            sy = (x - y) * m.half_h;
            break;
        case 2:
            sx = (-x + y) * m.half_w;
            sy = (-x - y) * m.half_h;
            break;
        default:
            sx = (x + y) * m.half_w;
            sy = (-x + y) * m.half_h;
            break;
        }
        sy -= z * m.one_unit * std::cos(3.14159265f / 6);
        sx += off_x;
        sy += off_y;
    }

    // A point in lot space on screen, unrounded.
    void screen(float x, float y, float z, float &sx, float &sy) const
    {
        box(x, y, z, sx, sy);
        sx += m.cadge_w / 2.0f;
        sy += float(m.baseline);
    }

    // How near a point is to the camera (bigger is nearer), in tiles along
    // the view direction. The camera looks down at 30 degrees: one tile of
    // horizontal diagonal is cos 30 / sqrt 2 nearer, one tile of height sin 30.
    float depth(float x, float y, float z) const
    {
        float h;   // the horizontal term of the screen y, see box()
        switch (rotation & 3) {
        case 0: h = x + y; break;
        case 1: h = x - y; break;
        case 2: h = -x - y; break;
        default: h = -x + y; break;
        }
        return h * kDepthPerDiagonal + z * kDepthPerHeight;
    }
    static constexpr float kDepthPerDiagonal = 0.61237244f, kDepthPerHeight = 0.5f;

    // A point in lot space (tile centres at whole numbers) on screen.
    void point(float x, float y, float z, int &px, int &py) const
    {
        float sx, sy;
        box(x, y, z, sx, sy);
        px = int(std::lround(sx + m.cadge_w / 2.0f));
        py = int(std::lround(sy + m.baseline));
    }

    // The ground-level lot position under a screen point (the inverse of point() at z = 0).
    void unproject(float px, float py, float z, float &x, float &y) const
    {
        const float a = (px - off_x - m.cadge_w / 2.0f) / m.half_w;
        const float b = (py - off_y - m.baseline + z * m.one_unit * std::cos(3.14159265f / 6)) / m.half_h;
        // a and b are (x - y, x + y) turned by the rotation; undo it.
        switch (rotation & 3) {
        case 0: x = (a + b) / 2; y = (b - a) / 2; break;
        case 1: x = (b - a) / 2; y = (-a - b) / 2; break;
        case 2: x = (-a - b) / 2; y = (a - b) / 2; break;
        default: x = (a - b) / 2; y = (a + b) / 2; break;
        }
    }
};

} // namespace ts
