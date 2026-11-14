/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/SimsAntics/model/VMRoomMap.cs).
 */
#include "world/rooms.h"

#include <utility>

namespace ts {

bool solid_wall_style(uint16_t style)
{
    return !(style == 2 || style == 12 || style == 13 || style == 14);
}

namespace {

constexpr int W = kLotArraySize;

// FreeSims SpreadOnto: claims (x, y) for `room` if its floor matches, and
// queues it. A diagonal tile is claimed one half at a time; `in_dir` is the
// side the fill comes from (0 from -x, 1 from -y, 2 from +x, 3 from +y).
void spread_onto(const Lot &lot, int level, int x, int y, int in_dir, std::vector<uint32_t> &map, uint16_t room,
                 uint16_t expected, std::vector<std::pair<int, int>> &stack)
{
    const size_t i = size_t(x) + size_t(y) * W;
    const WallTile &wall = lot.walls[level][i];
    const uint16_t floor = lot.floors[level][i];
    if ((expected > 0 && expected != floor) || (expected == 0 && floor > 65533))
        return;
    if (wall.segments & WallHorizontalDiag) {
        const bool low = in_dir < 2;
        if (map[i] & (low ? 0x0000ffffu : 0xffff0000u))
            return;
        map[i] |= low ? uint32_t(room) : uint32_t(room) << 16;
    } else if (wall.segments & WallVerticalDiag) {
        const bool low = in_dir > 0 && in_dir < 3;
        if (map[i] & (low ? 0x0000ffffu : 0xffff0000u))
            return;
        map[i] |= low ? uint32_t(room) : uint32_t(room) << 16;
    } else {
        if (map[i])
            return;
        map[i] = uint32_t(room) | uint32_t(room) << 16;
    }
    stack.push_back({ x, y });
}

} // namespace

RoomMap build_rooms(const Lot &lot, int level)
{
    RoomMap out;
    out.map.assign(size_t(W) * W, 0);
    out.outside.push_back(true);   // room 0: none
    const auto &walls = lot.walls[level];
    // Blocks movement across a tile's top-left / top-right edge.
    auto blocks_tl = [&](const WallTile &w) { return (w.segments & WallTopLeft) && solid_wall_style(w.top_left_style); };
    auto blocks_tr = [&](const WallTile &w) {
        return (w.segments & WallTopRight) && solid_wall_style(w.top_right_style);
    };

    bool first = true;
    for (size_t start = 0; start < out.map.size(); start++) {
        if (out.map[start])
            continue;
        const uint16_t room = uint16_t(out.outside.size());
        uint16_t expected = lot.floors[level][start];
        if (expected < 65534)
            expected = 0;
        out.map[start] = room;
        std::vector<std::pair<int, int>> stack{ { int(start % W), int(start / W) } };
        bool walled = false;   // a wall anywhere on its edge
        while (!stack.empty()) {
            auto [x, y] = stack.back();
            stack.pop_back();
            const WallTile &here = walls[size_t(x) + size_t(y) * W];
            if (here.segments > 15) {
                walled = true;
                continue;   // no spreading out of diagonal tiles
            }
            const int px = (x + 1) % W, mx = (x + W - 1) % W, py = (y + 1) % W, my = (y + W - 1) % W;
            if (!blocks_tl(walls[size_t(px) + size_t(y) * W]))
                spread_onto(lot, level, px, y, 0, out.map, room, expected, stack);
            else
                walled = true;
            if (!blocks_tl(here))
                spread_onto(lot, level, mx, y, 2, out.map, room, expected, stack);
            else
                walled = true;
            if (!blocks_tr(walls[size_t(x) + size_t(py) * W]))
                spread_onto(lot, level, x, py, 1, out.map, room, expected, stack);
            else
                walled = true;
            if (!blocks_tr(here))
                spread_onto(lot, level, x, my, 3, out.map, room, expected, stack);
            else
                walled = true;
        }
        // Ground cut off by a pool alone, with no wall round it (the Porte Rouge Villa's tile in the
        // corner of its pool) is outdoors, as the pool is: as a room of its own it was indoors, lit
        // as an unlit room, dark where the original shows it in daylight (the user, 2026-11-03).
        out.outside.push_back(first || expected > 65533 || (!walled && level == 0));
        first = false;
    }
    return out;
}

} // namespace ts
