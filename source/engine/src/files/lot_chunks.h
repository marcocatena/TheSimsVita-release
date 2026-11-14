/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (sims.files/formats/iff/chunks: ARRY, WALm, OBJT, OBJM, HOUS, SIMI, SLOT).
 */

// Chunks found in lot (House##.iff) files, plus SLOT from object files.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ts {

// Arry: a run-length encoded grid. `stride` bytes per cell: 1 floors/flags,
// 2 object ids, 4 altitude, 8 walls (14 in newer files).
struct LotArray {
    int width = 0, height = 0, stride = 0;
    std::vector<uint8_t> data;   // width * height * stride, row-major as stored

    const uint8_t *cell(int x, int y) const { return &data[(size_t(y) * width + x) * stride]; }
};

bool parse_arry(const uint8_t *data, size_t size, LotArray &out, std::string &error);

// WALm / FLRm: lot-local wall or floor style numbers -> style file names.
struct StyleEntry {
    std::string name;   // e.g. "FloorJadeTile.flr"
    uint8_t id = 0;
};

bool parse_style_map(const uint8_t *data, size_t size, uint16_t chunk_id, std::vector<StyleEntry> &out,
                     std::string &error);

// objt: the object types a lot uses.
struct ObjtEntry {
    uint32_t guid = 0;            // 0 marks an empty slot
    uint16_t type_id = 0;
    uint16_t objd_type = 0;
    std::string name;
};

bool parse_objt(const uint8_t *data, size_t size, std::vector<ObjtEntry> &out, std::string &error);

// ObjM: the lot's object instances. The layout is only partly understood, so
// the bit-packed values are kept raw: a 0-terminated id table, then the rest.
struct Objm {
    uint32_t version = 0;
    std::vector<uint16_t> id_table;
    std::vector<int16_t> values;
};

bool parse_objm(const uint8_t *data, size_t size, Objm &out, std::string &error);

// HOUS: lot-wide settings.
struct Hous {
    uint32_t version = 0;
    int16_t camera_dir = 0;
    float roof_pitch = 0.66f;   // height gained per tile of run, as a fraction of 1 tile; game's choices 0.4-1.0
    std::string roof_name;      // texture in GameData/Roofs, without ".bmp"
};

bool parse_hous(const uint8_t *data, size_t size, Hous &out, std::string &error);

// SIMI: the simulation's global variables for a lot, plus its value.
struct Simi {
    uint32_t version = 0;
    std::vector<int16_t> globals;   // 32 or 64; [23] = lot size, [35] = lot type
    int32_t lot_value = 0, objects_value = 0, architecture_value = 0;
};

bool parse_simi(const uint8_t *data, size_t size, Simi &out, std::string &error);

// SLOT: where Sims stand, sit or place things around an object.
struct SlotItem {
    uint16_t type = 0;
    float x = 0, y = 0, z = 0;
    int32_t standing = 1, sitting = 0, ground = 0;
    int32_t flags = 0;
    int32_t snap_target = -1;
    int32_t min_proximity = 0, max_proximity = 0, optimal_proximity = 0;
    float gradient = 0.1875f;   // score lost per sixteenth off the optimal distance (the original's default)
    int32_t height = 0;
    int32_t facing = -2;          // -3 anywhere, -2 towards object, -1 away
    int32_t resolution = 16;
};

bool parse_slot(const uint8_t *data, size_t size, std::vector<SlotItem> &out, std::string &error);

} // namespace ts
