/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Depth conventions follow FreeSims (sims.common/utils/TextureGenerator.cs
 * WallZBufferConfig, SimsVille/Content/Effects/2DWorldBatch.fx).
 */

// Per-pixel depth for walls and floors. The game's sprites carry z-buffers
// measured from the back corner of their tile; walls and floors get the
// same kind of z-buffer as a gradient (FreeSims' values, tuned against the
// game's sprites). Both turn into depth the same way: the back corner's
// depth plus (255 - z) * kSpriteDepthScale.
#pragma once

#include "render/draw_list.h"
#include "render/projection.h"

namespace ts {

// The depth of the back corner of the tile whose sprite box is box(x, y, z).
inline float back_corner_depth(const Projector &p, float x, float y, float z)
{
    return p.depth(x, y, z) - Projector::kDepthPerDiagonal;
}

// Depth over a z-buffer gradient z = start + dx * (x - left) + dy * (y - top),
// for a sprite whose top-left is at (left, top) on screen.
inline DepthPlane gradient_depth(float back, float left, float top, float start, float dx, float dy)
{
    DepthPlane p;
    p.a = -dx * kSpriteDepthScale;
    p.b = -dy * kSpriteDepthScale;
    p.c = back + (255 - start + dx * left + dy * top) * kSpriteDepthScale;
    return p;
}

// A wall face drawn with its top-left at (left, top). Sides: 0 top-left,
// 1 top-right, 2 horizontal diagonal, 3 vertical diagonal; zoom index 0 far,
// 2 near.
inline DepthPlane wall_depth(float back, int zoom_index, int side, float left, float top)
{
    static const float kStart[4] = { 74, 135, 89.5f, 45 };
    static const float kDx[4] = { 1, -1, 0, 0 };
    const float scale = float(1 << (2 - zoom_index));   // the gradient steepens as pixels get bigger
    return gradient_depth(back, left, top, kStart[side], kDx[side] * scale, 0.5f * scale);
}

// A floor tile (or a cap on top of walls) drawn with its top at `top`:
// 255 along the back corner, falling 1.525 per row at near zoom.
inline DepthPlane floor_depth(float back, int zoom_index, float top, float start = 255)
{
    const float scale = float(1 << (2 - zoom_index));
    return gradient_depth(back, 0, top, start, 0, -1.525f * scale);
}

} // namespace ts
