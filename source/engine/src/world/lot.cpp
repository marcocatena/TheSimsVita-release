/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Lot layout follows FreeSims (SimsVille/SimsAntics/utils/VMWorldActivator.cs).
 */
#include <cstdio>
#include <cstdlib>
#include "core/env.h"
#include "core/memory_kind.h"
#include "world/lot.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "files/iff.h"
#include "files/objm.h"

namespace ts {

const std::string *Lot::custom_floor(uint16_t pattern) const
{
    const size_t i = size_t(pattern) - kCustomStyle;
    return pattern >= kCustomStyle && i < floor_styles.size() ? &floor_styles[i].name : nullptr;
}

const std::string *Lot::custom_wall(uint16_t pattern) const
{
    const size_t i = size_t(pattern) - kCustomStyle;
    return pattern >= kCustomStyle && i < wall_styles.size() ? &wall_styles[i].name : nullptr;
}

float Lot::ground(float x, float y) const
{
    constexpr int V = kLotArraySize + 1;
    if (!terrain || altitude.empty())
        return 0;
    // Corner coordinates: corner (cx, cy) sits at (cx - 0.5, cy - 0.5).
    const float fx = std::clamp(x + 0.5f, 0.0f, float(V - 1)), fy = std::clamp(y + 0.5f, 0.0f, float(V - 1));
    const int x0 = std::min(int(fx), V - 2), y0 = std::min(int(fy), V - 2);
    const float tx = fx - x0, ty = fy - y0;
    // The lot's corners are 1 to size - 1 (Goths' lot: corner 0's row and column hold a flat 20
    // and corner `size`'s 0, a rim the game doesn't show): the edges take their inner neighbour's.
    const int last = std::max(1, std::min(size, V) - 1);
    auto h = [&](int cx, int cy) {
        cx = std::clamp(cx, 1, last - 1);
        cy = std::clamp(cy, 1, last - 1);
        return float(altitude[size_t(cx) + size_t(cy) * V]);
    };
    static const float step = TS_ENV("TS_ALT_STEP") ? float(std::atof(TS_ENV("TS_ALT_STEP"))) : kAltitudeStep;
    const float top = h(x0, y0) + (h(x0 + 1, y0) - h(x0, y0)) * tx;
    const float bottom = h(x0, y0 + 1) + (h(x0 + 1, y0 + 1) - h(x0, y0 + 1)) * tx;
    return (top + (bottom - top) * ty) * step;
}

namespace {

constexpr size_t kCells = size_t(kLotArraySize) * kLotArraySize;

// Lot arrays are stored with x and y swapped relative to lot coordinates.
const uint8_t *lot_cell(const LotArray &a, int x, int y)
{
    return a.cell(y, x);
}

uint16_t u16_at(const uint8_t *p)
{
    return uint16_t(p[0] | p[1] << 8);
}

bool usable(const LotArray *a, int stride)
{
    return a && a->stride == stride && a->width == kLotArraySize && a->height == kLotArraySize;
}

void decode_floors(const LotArray *a, std::vector<uint16_t> &out)
{
    out.assign(kCells, 0);
    if (!a)
        return;
    for (int y = 0; y < kLotArraySize; y++)
        for (int x = 0; x < kLotArraySize; x++) {
            const uint8_t *c = lot_cell(*a, x, y);
            out[x + y * kLotArraySize] = a->stride == 2 ? u16_at(c) : c[0];
        }
}

void decode_walls(const LotArray *a, std::vector<WallTile> &out)
{
    out.assign(kCells, {});
    if (!a)
        return;
    const bool wide = a->stride == 14;   // newer lots store 16-bit styles and patterns
    for (int y = 0; y < kLotArraySize; y++)
        for (int x = 0; x < kLotArraySize; x++) {
            const uint8_t *c = lot_cell(*a, x, y);
            WallTile &w = out[x + y * kLotArraySize];
            w.segments = c[0];
            if (wide) {
                w.top_left_style = u16_at(c + 2);
                w.top_right_style = u16_at(c + 4);
                w.top_left_pattern = u16_at(c + 6);
                w.top_right_pattern = u16_at(c + 8);
                w.bottom_left_pattern = u16_at(c + 10);
                w.bottom_right_pattern = u16_at(c + 12);
            } else {
                w.top_left_style = c[2];
                w.top_right_style = c[3];
                w.top_left_pattern = c[4];
                w.top_right_pattern = c[5];
                w.bottom_left_pattern = c[6];
                w.bottom_right_pattern = c[7];
            }
        }
}

bool plausible_date(const int16_t *d)
{
    return d[ObjBirthYear] >= 1900 && d[ObjBirthYear] <= 2200 && d[ObjBirthMonth] >= 1 &&
           d[ObjBirthMonth] <= 12 && d[ObjBirthDay] >= 1 && d[ObjBirthDay] <= 31;
}

} // namespace

namespace {

void place_portals_on_street(Lot &lot);   // (below)

} // namespace

bool load_lot(const uint8_t *data, size_t size, Lot &out, std::string &error)
{
    MemScope scope(MemKind::Lot);   // (counted as the lot's on the Vita: core/memory_kind.h)
    IffFile iff;
    if (!parse_iff(data, size, iff, error))
        return false;
    auto chunk = [&](const char *type, uint16_t id) { return iff.find(type, id); };

    const IffChunk *simi = chunk("SIMI", 1);
    if (!simi || !parse_simi(data + simi->offset, simi->size, out.simi, error))
        return error = "no SIMI chunk: " + error, false;
    out.size = out.simi.globals[23];
    if (out.size <= 0 || out.size > kLotArraySize)
        return error = "bad lot size " + std::to_string(out.size), false;
    if (const IffChunk *h = chunk("HOUS", 0))
        parse_hous(data + h->offset, h->size, out.hous, error);
    out.exported = chunk("EXPi", 0) != nullptr;

    for (uint16_t id : { uint16_t(1), uint16_t(0) }) {
        if (const IffChunk *c = chunk("FLRm", id); c && out.floor_styles.empty())
            parse_style_map(data + c->offset, c->size, c->id, out.floor_styles, error);
        if (const IffChunk *c = chunk("WALm", id); c && out.wall_styles.empty())
            parse_style_map(data + c->offset, c->size, c->id, out.wall_styles, error);
    }

    std::map<uint16_t, LotArray> arrays;
    for (const IffChunk &c : iff.chunks) {
        if (std::strcmp(c.type, "Arry") != 0)
            continue;
        LotArray a;
        std::string err;
        if (parse_arry(data + c.offset, c.size, a, err))
            arrays[c.id] = std::move(a);
        else
            out.warnings.push_back("Arry #" + std::to_string(c.id) + ": " + err);
    }
    auto arr = [&](uint16_t id, int stride) -> const LotArray * {
        auto it = arrays.find(id);
        return it != arrays.end() && usable(&it->second, stride) ? &it->second : nullptr;
    };

    for (int level = 0; level < 2 && level < kLotLevels; level++)
        if (const LotArray *a = arr(level ? 108 : 8, 1)) {
            out.tile_flags[level].assign(kCells, 0);
            for (int y = 0; y < kLotArraySize; y++)
                for (int x = 0; x < kLotArraySize; x++)
                    out.tile_flags[level][size_t(x) + size_t(y) * kLotArraySize] = lot_cell(*a, x, y)[0];
        }
    // Newer lots have 16-bit floors (11/111) and walls (12/112).
    if (arr(11, 2)) {
        decode_floors(arr(11, 2), out.floors[0]);
        decode_floors(arr(111, 2), out.floors[1]);
        decode_walls(arr(12, 14), out.walls[0]);
        decode_walls(arr(112, 14), out.walls[1]);
    } else {
        decode_floors(arr(1, 1), out.floors[0]);
        decode_floors(arr(101, 1), out.floors[1]);
        decode_walls(arr(2, 8), out.walls[0]);
        decode_walls(arr(102, 8), out.walls[1]);
    }
    // Arry 0 "altitude": four corner heights per tile. Every corner shared by
    // neighbouring tiles agrees under this layout: byte 0 = corner (x, y+1),
    // 1 = (x, y), 2 = (x+1, y), 3 = (x+1, y+1).
    constexpr int V = kLotArraySize + 1;
    out.altitude.assign(size_t(V) * V, 0);
    if (const LotArray *a = arr(0, 4))
        for (int y = 0; y < kLotArraySize; y++)
            for (int x = 0; x < kLotArraySize; x++) {
                const uint8_t *c = lot_cell(*a, x, y);
                out.altitude[size_t(x) + size_t(y + 1) * V] = c[0];
                out.altitude[size_t(x) + size_t(y) * V] = c[1];
                out.altitude[size_t(x + 1) + size_t(y) * V] = c[2];
                out.altitude[size_t(x + 1) + size_t(y + 1) * V] = c[3];
            }
    out.grass.assign(kCells, 0);
    if (const LotArray *g = arr(6, 1))
        for (int y = 0; y < kLotArraySize; y++)
            for (int x = 0; x < kLotArraySize; x++)
                out.grass[size_t(x) + size_t(y) * kLotArraySize] = lot_cell(*g, x, y)[0];
    const LotArray *pools = arr(9, 1), *water = arr(10, 1);
#if !defined(__vita__)
    if (const char *pl = TS_ENV("TS_POOL_LOG"); pl && pools) {   // debug: "x0 y0 x1 y1", the raw pool values there
        int b[4] = { 0, 0, 0, 0 };
        if (std::sscanf(pl, "%d %d %d %d", &b[0], &b[1], &b[2], &b[3]) == 4)
            for (int y = b[1]; y <= b[3]; y++) {
                std::printf("pool row %2d:", y);
                for (int x = b[0]; x <= b[2]; x++)
                    std::printf(" %3d", int(lot_cell(*pools, x, y)[0]));
                std::printf("\n");
            }
    }
#endif
    for (int y = 0; y < kLotArraySize; y++)
        for (int x = 0; x < kLotArraySize; x++) {
            uint16_t &f = out.floors[0][x + y * kLotArraySize];
            if (pools) {
                uint8_t v = lot_cell(*pools, x, y)[0];
                // 1-15 say which of a tile's corners are water; 16 is the dry ring round a pool. Taken
                // for water, every pool was a tile too big all round, its ladders and boards standing
                // in it and the spot behind a board "another room" (the Mashugas', 2026-11-02).
                if (v != 0 && v != 255 && v != 16)
                    f = kFloorPool;
            }
            if (water) {
                uint8_t v = lot_cell(*water, x, y)[0];
                if (v != 0 && v != 255)
                    f = kFloorWater;
            }
        }
    // (No longer: the street was carried onto the lot's outer ring of tiles, which the
    // original doesn't show (its grass isn't drawn there either): the road stuck out a tile past
    // the grass at both ends. The lot file's own street, tiles 1 to size - 2, ends with the grass,
    // as the original's does (compared at far zoom, 2026-10-26).)

    // Floors and walls from .flr/.wll files rather than the global ones: the
    // low byte of the saved id picks the entry of the lot's FLRm/WALm map
    // (FreeSims VMWorldActivator.RemapFloors/RemapWalls). They are
    // renumbered from kCustomStyle so they can't be mistaken for global ids.
    auto remap = [](uint16_t &id, const std::vector<StyleEntry> &map) {
        for (size_t i = 0; i < map.size(); i++)
            if (map[i].id == (id & 255)) {
                id = uint16_t(kCustomStyle + i);
                return;
            }
    };
    const LotArray *flags = arr(8, 1);
    for (int level = 0; level < kLotLevels; level++)
        for (int y = 0; y < kLotArraySize; y++)
            for (int x = 0; x < kLotArraySize; x++) {
                const size_t i = size_t(x) + size_t(y) * kLotArraySize;
                uint16_t &f = out.floors[level][i];
                // Low ids are global floors on tiles flagged 0x20.
                const bool flagged = flags && (lot_cell(*flags, x, y)[0] & 0x20);
                if (f != 0 && f < kFloorWater && (!flagged || f > 30))
                    remap(f, out.floor_styles);
                WallTile &w = out.walls[level][i];
                for (uint16_t *p : { &w.bottom_left_pattern, &w.bottom_right_pattern })
                    if (*p)
                        remap(*p, out.wall_styles);
                if (w.segments & (WallHorizontalDiag | WallVerticalDiag)) {
                    for (uint16_t *p : { &w.top_left_pattern, &w.top_left_style })   // the two half-tile floors
                        if (*p)
                            remap(*p, out.floor_styles);
                } else {
                    for (uint16_t *p : { &w.top_left_pattern, &w.top_right_pattern })
                        if (*p)
                            remap(*p, out.wall_styles);
                }
            }

    // Object types, then instances: ObjM's id table pairs each instance id
    // with its objt entry, and each instance's record ends in its 72
    // object-data fields (found by their id and creation date).
    std::vector<ObjtEntry> objt;
    if (const IffChunk *c = chunk("objt", 0))
        parse_objt(data + c->offset, c->size, objt, error);
    Objm objm;
    const IffChunk *objm_chunk = chunk("ObjM", 1);
    if (!objm_chunk || !parse_objm(data + objm_chunk->offset, objm_chunk->size, objm, error)) {
        out.warnings.push_back("no ObjM chunk; lot has no objects");
        return true;
    }

    std::map<uint16_t, size_t> record;   // instance id -> start of its object data in objm.values
    for (size_t p = 0; p + ObjDataCount <= objm.values.size(); p++) {
        const int16_t *d = &objm.values[p];
        if (d[ObjObjectId] > 0 && plausible_date(d))
            record.emplace(uint16_t(d[ObjObjectId]), p);
    }

    std::map<uint16_t, size_t> by_id;
    for (size_t i = 0; i + 1 < objm.id_table.size(); i += 2) {
        LotObject o;
        o.id = objm.id_table[i];
        uint16_t type = objm.id_table[i + 1];
        if (type == 0 || type > objt.size()) {
            out.warnings.push_back("object " + std::to_string(o.id) + " has unknown type " + std::to_string(type));
            continue;
        }
        o.guid = objt[type - 1].guid;
        o.type_name = objt[type - 1].name;
        auto rec = record.find(o.id);
        if (rec == record.end()) {
            out.warnings.push_back("no data record for object " + std::to_string(o.id));
            continue;
        }
        std::memcpy(o.data, &objm.values[rec->second], sizeof(o.data));
        {
            // The values between the previous record's data and the next one's.
            size_t prev_end = 0, next_start = objm.values.size();
            for (const auto &[id, at] : record) {
                if (at + ObjDataCount <= rec->second)
                    prev_end = std::max(prev_end, at + ObjDataCount);
                if (at > rec->second)
                    next_start = std::min(next_start, at);
            }
            o.before.assign(objm.values.begin() + std::ptrdiff_t(prev_end), objm.values.begin() + std::ptrdiff_t(rec->second));
            o.after.assign(objm.values.begin() + std::ptrdiff_t(rec->second + ObjDataCount),
                           objm.values.begin() + std::ptrdiff_t(std::max(next_start, rec->second + ObjDataCount)));
        }
        by_id[o.id] = out.objects.size();
        out.objects.push_back(std::move(o));
    }

    // The full records (FreeSO's OBJM reader): attributes, needs, ...
    {
        ObjmFull full;
        std::string objm_error;
        int decoded = 0, failed = 0;
        if (parse_objm_records(data + objm_chunk->offset, objm_chunk->size, full, objm_error)) {
            for (size_t i = 0; i < full.records.size() && i < full.id_to_type.size(); i++) {
                const uint16_t type = full.id_to_type[i].second;
                if (type == 0 || type > objt.size())
                    continue;
                const ObjtEntry &t = objt[type - 1];
                ObjmInstance inst;
                // Without the object's definition at hand, a blank name means a
                // part of a multi-tile object (as FreeSO guesses).
                if (!decode_objm_instance(full, full.records[i], t.objd_type, t.name.empty(), inst, objm_error)) {
                    failed++;
                    continue;
                }
                auto it = by_id.find(uint16_t(inst.data[ObjObjectId]));
                if (it == by_id.end())
                    continue;
                LotObject &o = out.objects[it->second];
                std::memcpy(o.data, inst.data, sizeof(inst.data));
                o.attributes = inst.attributes;
                o.relationships.clear();
                for (const auto &r : inst.relationships)
                    if (r.target > 0 && r.target < 65536)
                        o.relationships.emplace_back(uint16_t(r.target),
                                                     std::vector<int16_t>(r.values.begin(), r.values.end()));
                // One entry a dynamic sprite, not zero when it shows (FreeSO VMTS1ActivatorNew). Ours took
                // each entry as 16 of them: Old Town's pet shop's right "Shutters Provençal", its entries
                // (0, 1), sprite 1 on, came out as sprite 16, which it hasn't, and the shutter wasn't drawn
                // (the original draws both by the door). TS_DYN_BITS_OLD=1: as before.
                o.dynamic_sprites = 0;
                o.dynamic_sprites_old = 0;
                for (size_t b = 0; b < inst.dynamic_sprite_flags.size() * 16 && b < 64; b++)
                    if ((uint16_t(inst.dynamic_sprite_flags[b / 16]) >> (b % 16)) & 1)
                        o.dynamic_sprites_old |= uint64_t(1) << b;
                static const bool dyn_bits_old = std::getenv("TS_DYN_BITS_OLD") != nullptr;
                if (dyn_bits_old) {
                    for (size_t b = 0; b < inst.dynamic_sprite_flags.size() * 16 && b < 64; b++)
                        if ((uint16_t(inst.dynamic_sprite_flags[b / 16]) >> (b % 16)) & 1)
                            o.dynamic_sprites |= uint64_t(1) << b;
                } else {
                    for (size_t b = 0; b < inst.dynamic_sprite_flags.size() && b < 64; b++)
                        if (inst.dynamic_sprite_flags[b] != 0)
                            o.dynamic_sprites |= uint64_t(1) << b;
                }
                if (std::getenv("TS_DYN_LOG") && !inst.dynamic_sprite_flags.empty()) {   // debug: the saved entries
                    std::string v;
                    for (int16_t d : inst.dynamic_sprite_flags)
                        v += " " + std::to_string(d);
                    std::printf("dynamic sprites of #%d (%s):%s -> %llx\n", int(inst.data[ObjObjectId]), t.name.c_str(), v.c_str(),
                                (unsigned long long)o.dynamic_sprites);
                }
                if (inst.person) {
                    o.person = true;
                    std::memcpy(o.motives, inst.sim.motives, sizeof(o.motives));
                    o.person_data = inst.sim.person_data;
                }
                o.restored = true;
                decoded++;
            }
        }
        if (failed)
            out.warnings.push_back(std::to_string(failed) + " ObjM records not decoded (" + objm_error + ")");
    }

    // Tile positions come from the object arrays (3 and 103). Inside the lot's size first: an id
    // the arrays also hold past the lot (stale cells: three parts of a Vacation lot's archery
    // range at (58, 62), (41, 62), (31, 62) on a lot 38 across, their own tiles in the range's
    // row at x 4-7) keeps its place on the lot. Read in order, the last cell won: the parts sat
    // alone past the edge, their group broken, their code looking for the rest round and round.
    // Past the lot only what has no place on it (the car and pedestrian portals some lots keep
    // there: see place_portals_on_street).
    std::vector<bool> placed(out.objects.size(), false);
    for (int pass = 0; pass < 2; pass++)
    for (int level = 0; level < kLotLevels; level++) {
        const LotArray *a = arr(level == 0 ? 3 : 103, 2);
        if (!a)
            continue;
        for (int y = 0; y < kLotArraySize; y++)
            for (int x = 0; x < kLotArraySize; x++) {
                const bool inside = x < out.size && y < out.size;
                if (inside != (pass == 0))
                    continue;
                uint16_t id = u16_at(lot_cell(*a, x, y));
                auto it = by_id.find(id);
                if (id == 0)
                    continue;
                if (it == by_id.end()) {
                    out.warnings.push_back("tile object " + std::to_string(id) + " has no ObjM entry");
                    continue;
                }
                if (pass == 1 && placed[it->second])
                    continue;
                // (nor past the lot for one that goes where its parent or container is: a window's
                // inside part shares its outside part's wall tile and takes its place from it; a
                // stale cell put Solana's at (62, 47), its object broken, its room code at a breakpoint)
                if (pass == 1 && (out.objects[it->second].data[ObjParentId] != 0 || out.objects[it->second].container() != 0))
                    continue;
                placed[it->second] = true;
                LotObject &o = out.objects[it->second];
                o.x = x;
                o.y = y;
                o.level = level;
                if (TS_ENV("TS_EDGE_OBJECTS") && (x >= out.size || y >= out.size))   // debug: what lies past the lot's size
                    std::printf("past the lot (size %d): object id %d guid %08x at array (%d, %d) level %d\n", out.size, id,
                                o.guid, x, y, level);
            }
    }
    place_portals_on_street(out);
    return true;
}

namespace {

void place_portals_on_street(Lot &lot)
{
    // The car portal (where the school bus, carpools and visitors' cars stop) and the pedestrian
    // portals (where walkers come and go) normally stand on the lot's own street: the car portal
    // in the road's middle, about halfway along (House01: x 49.5 on road x 47-53; House03: x 4.5
    // on x 1-7), the pedestrian ones at the pavement's ends. A lot can store them past its own size,
    // where there's no street (House05, the Goths: road x 43-49, car portal at (23, 53)); the
    // original then still has its bus stop in the road's middle (measured: x 46.5). So those go
    // back onto the street, as the original's do.
    constexpr uint32_t kCarPortal = 0x865A6812u, kPedestrianPortal = 0x81E6BEF9u;
    const int n = lot.size;
    if (n < 8 || n > kLotArraySize || lot.floors[0].empty())
        return;
    // The street's lines (whole lines 1..size-2 of road floor), across x or y.
    auto road = [](uint16_t f) { return f == 9 || f == 10 || f == 11; };
    auto pave = [](uint16_t f) { return f == 12; };
    for (int across = 0; across < 2; across++) {   // 0: lines down y at each x; 1: lines along x at each y
        auto at = [&](int line, int i) { return across == 0 ? lot.floor(line, i, 0) : lot.floor(i, line, 0); };
        auto whole = [&](int line, bool (*kind)(uint16_t)) {
            for (int i = 1; i <= n - 2; i++)
                if (!kind(at(line, i)))
                    return false;
            return true;
        };
        int first = -1, last = -1, pavement = -1;
        for (int line = 0; line < n; line++) {
            if (whole(line, road)) {
                first = first < 0 ? line : first;
                last = line;
            } else if (whole(line, pave)) {
                pavement = line;
            }
        }
        if (first < 0)
            continue;
        const int middle = (first + last) / 2, along = std::min(n - 2, n / 2 + 3);
        int pedestrian_end = 0;   // the pavement's ends, the far one first if one is taken already
        for (LotObject &o : lot.objects) {
            if (o.level != 0 || (o.x < n && o.y < n) || o.x < 0)
                continue;
            if (o.guid == kCarPortal) {
                (across == 0 ? o.x : o.y) = middle;
                (across == 0 ? o.y : o.x) = along;
            } else if (o.guid == kPedestrianPortal && pavement >= 0) {
                bool near_taken = false;
                for (const LotObject &p : lot.objects)
                    if (&p != &o && p.guid == kPedestrianPortal && (across == 0 ? p.x : p.y) == pavement &&
                        (across == 0 ? p.y : p.x) >= n / 2)
                        near_taken = true;
                const int end = near_taken || pedestrian_end > 0 ? 1 : n - 2;
                (across == 0 ? o.x : o.y) = pavement;
                (across == 0 ? o.y : o.x) = end;
                pedestrian_end++;
            } else {
                continue;
            }
            if (TS_ENV("TS_EDGE_OBJECTS"))
                std::printf("  put on the street: guid %08x to (%d, %d)\n", o.guid, o.x, o.y);
        }
        return;
    }
}

} // namespace

} // namespace ts
