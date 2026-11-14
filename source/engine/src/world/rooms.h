/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/SimsAntics/model/VMRoomMap.cs).
 */

// Rooms: areas of a storey closed off by solid walls, found by flood fill.
#pragma once

#include <cstdint>
#include <vector>

#include "world/lot.h"

namespace ts {

struct RoomMap {
    // Per tile (x + y * 64): the room number in the low 16 bits. A tile
    // split by a diagonal wall holds the other half's room in the high 16 bits.
    std::vector<uint32_t> map;
    std::vector<bool> outside;   // by room number; room 0 is unused, room 1 is the outdoors

    uint16_t room(int x, int y) const { return uint16_t(map[size_t(x) + size_t(y) * kLotArraySize]); }
    bool indoors(int x, int y) const { return !outside[room(x, y)]; }
    // The room a tile's draws take their light from: on a tile a diagonal wall splits, the
    // indoor half's (a bay's walls and floor are inside); 1, the outdoors, if none.
    uint16_t lit_room(int x, int y) const
    {
        const uint32_t v = map[size_t(x) + size_t(y) * kLotArraySize];
        const uint16_t a = uint16_t(v), b = uint16_t(v >> 16);
        if (a && a < outside.size() && !outside[a])
            return a;
        if (b && b < outside.size() && !outside[b])
            return b;
        return a ? a : 1;
    }
};

// Solid walls (normal walls, not fences or banisters) split rooms; pools and
// water are rooms of their own and count as outdoors.
RoomMap build_rooms(const Lot &lot, int level);

// True for a saved wall style that is a solid wall: anything but the fences
// and banisters the game keeps (FreeSims ValidWallStyles).
bool solid_wall_style(uint16_t style);

} // namespace ts
