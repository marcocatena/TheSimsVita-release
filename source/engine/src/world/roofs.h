/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/World/components/RoofComponent.cs).
 */

// The game builds roofs itself; a lot only stores their texture and pitch
// (HOUS). Over every indoor area of a storey that has nothing on top, it
// covers half-tile rectangles with hipped roofs.
#pragma once

#include <vector>

#include "world/lot.h"
#include "world/rooms.h"

namespace ts {

// One sloped side of a roof: a quad in lot space (tile centres at whole
// numbers, z in tiles above the ground), with texture coordinates in texture
// repeats and a brightness per corner.
struct RoofFace {
    float x[4], y[4], z[4];
    float u[4], v[4];
    float shade[4];
};

// The roof over `storey` (0 = over the ground floor). `rooms` holds the room
// map of every storey.
std::vector<RoofFace> build_roof(const Lot &lot, const RoomMap (&rooms)[kLotLevels], int storey);

// Whether the roof over `storey` covers tile (x, y): indoors there, nothing indoors or floored
// on the storey above (as build_roof decides).
bool under_roof(const Lot &lot, const RoomMap (&rooms)[kLotLevels], int storey, int x, int y);
} // namespace ts
