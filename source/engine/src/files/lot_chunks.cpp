/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (sims.files/formats/iff/chunks: ARRY, WALm, OBJT, OBJM, HOUS, SIMI, SLOT).
 */
#include "files/lot_chunks.h"

#include <cstdlib>

#include "core/bytes.h"
#include "core/field_reader.h"

namespace ts {

namespace {

bool fail(std::string &error, const std::string &message)
{
    error = message;
    return false;
}

bool check(const ByteReader &r, std::string &error)
{
    return r.ok() || fail(error, "chunk is truncated");
}

// Strings in these chunks are NUL-terminated and padded to an even length.
std::string padded_cstring(ByteReader &r)
{
    std::string s = r.cstring();
    if (s.size() % 2 == 0)
        r.u8();
    return s;
}

} // namespace

bool parse_arry(const uint8_t *data, size_t size, LotArray &out, std::string &error)
{
    ByteReader r(data, size);
    r.s32le();   // zero
    out.width = r.s32le();
    out.height = r.s32le();
    out.stride = r.s32le();
    r.s32le();   // unknown
    if (!r.ok() || out.width <= 0 || out.height <= 0 || out.width > 256 || out.height > 256 ||
        out.stride <= 0 || out.stride > 16)
        return fail(error, "bad Arry header");
    const size_t total = size_t(out.width) * out.height * out.stride;
    out.data.assign(total, 0);

    // A stream of 16-bit markers. High bit clear: that many literal bytes.
    // High bit set: skip that many bytes, filling them with a pad byte, then
    // optionally a literal run. Positions wrap at the end of the grid.
    size_t pos = 0;
    while (r.remaining() >= 2) {
        uint16_t marker = r.u16le();
        if (marker == 0)
            continue;
        if (!(marker & 0x8000)) {
            for (int i = 0; i < marker; i++) {
                if (r.remaining() == 0 || pos >= total)
                    return true;
                out.data[pos++] = r.u8();
            }
            if (marker & 1)
                r.u8();
            continue;
        }
        size_t last = pos;
        const size_t skip = marker & 0x7fff;
        pos = (pos + skip) % total;
        uint8_t pad = r.u8();
        r.u8();
        // A skip that reaches the grid's end is that run to the end: filled too (it was left 0,
        // so the last run of every lot's ground tile flags, its buildable 0x20 over the lot's far
        // rows, read as 0: found writing the grids back, 2026-11-12). TS_ARRY_END_OLD=1: as before.
        static const bool end_old = std::getenv("TS_ARRY_END_OLD") != nullptr;
        if (last + skip >= total) {
            if (!end_old)
                while (last < total)
                    out.data[last++] = pad;
            return true;
        }
        while (last < pos)
            out.data[last++] = pad;
        if (r.remaining() < 2)
            return true;
        int16_t literal = r.s16le();
        if (literal < 0) {   // another skip marker follows
            r.seek(r.pos() - 2);
            continue;
        }
        for (int i = 0; i < literal; i++) {
            out.data[pos] = r.u8();
            pos = (pos + 1) % total;
        }
        if (literal & 1)
            r.u8();
    }
    return check(r, error);
}

bool parse_style_map(const uint8_t *data, size_t size, uint16_t chunk_id, std::vector<StyleEntry> &out,
                     std::string &error)
{
    ByteReader r(data, size);
    r.u32le();   // zero
    r.u32le();   // version
    r.u32le();   // "mLAW" / "mRLF"
    uint32_t count = r.u32le();
    if (!r.ok() || count > 256)
        return fail(error, "bad style map header");
    out.resize(count);
    for (StyleEntry &e : out) {
        e.name = padded_cstring(r);
        r.s32le();   // unknown, usually 1
        e.id = r.u8();
        r.skip(5 + size_t(chunk_id) * 2);   // the trailing size depends on the chunk id
    }
    return check(r, error);
}

bool parse_objt(const uint8_t *data, size_t size, std::vector<ObjtEntry> &out, std::string &error)
{
    ByteReader r(data, size);
    r.s32le();   // zero
    int32_t version = r.s32le();
    r.s32le();   // "tjbo"
    out.clear();
    while (r.ok() && r.remaining() >= 4) {
        ObjtEntry e;
        e.guid = r.u32le();
        if (e.guid != 0) {
            r.u16le();   // likely the number of attributes
            r.u16le();   // init tree version
            r.u16le();
            r.u16le();   // main tree version
            e.type_id = r.u16le();
            e.objd_type = r.u16le();
            e.name = padded_cstring(r);
            if (version > 2)
                r.s32le();
        }
        out.push_back(std::move(e));
    }
    return check(r, error);
}

bool parse_objm(const uint8_t *data, size_t size, Objm &out, std::string &error)
{
    ByteReader r(data, size);
    r.u32le();   // pad
    out.version = r.u32le();
    r.u32le();   // "MjbO"
    if (r.u8() != 1 || !r.ok())
        return fail(error, "unknown ObjM compression");
    FieldReader f(r);
    out.id_table.clear();
    out.values.clear();
    while (!f.at_end()) {
        uint16_t v = f.u16();
        if (v == 0)
            break;
        out.id_table.push_back(v);
    }
    while (!f.at_end())
        out.values.push_back(f.s16());
    return true;
}

bool parse_hous(const uint8_t *data, size_t size, Hous &out, std::string &error)
{
    ByteReader r(data, size);
    r.u32le();   // zero
    out.version = r.u32le();
    r.u32le();   // "suoh"
    r.skip(16);  // unknown flag, one, number, negative
    out.camera_dir = r.s16le();
    r.skip(4);
    out.roof_pitch = r.f32le();   // FreeSims reads these 4 bytes as a GUID; it is the roof pitch (0.4 - 1.0)
    if (!r.ok())
        return fail(error, "HOUS is truncated");
    out.roof_name = r.remaining() ? r.cstring() : std::string();
    return check(r, error);
}

bool parse_simi(const uint8_t *data, size_t size, Simi &out, std::string &error)
{
    ByteReader r(data, size);
    r.u32le();   // pad
    out.version = r.u32le();
    r.u32le();   // magic
    out.globals.resize(out.version > 0x3f ? 0x40 : 0x20);
    for (int16_t &g : out.globals)
        g = r.s16le();
    r.s16le();
    r.skip(20);  // unknowns and the two lot GUIDs
    out.lot_value = r.s32le();
    out.objects_value = r.s32le();
    out.architecture_value = r.s32le();
    for (int d = 0; d < 6 && r.remaining() >= 36; d++) {
        r.s32le();   // (kept: 1)
        for (int32_t &v : out.books[d])
            v = r.s32le();
    }
    return check(r, error);
}

bool parse_slot(const uint8_t *data, size_t size, std::vector<SlotItem> &out, std::string &error)
{
    ByteReader r(data, size);
    r.u32le();   // zero
    uint32_t version = r.u32le();
    r.u32le();   // magic
    uint32_t count = r.u32le();
    if (!r.ok() || count > size / 30)
        return fail(error, "bad SLOT header");
    out.resize(count);
    for (SlotItem &s : out) {
        s.type = r.u16le();
        s.x = r.f32le();
        s.y = r.f32le();
        s.z = r.f32le();
        s.standing = r.s32le();
        s.sitting = r.s32le();
        s.ground = r.s32le();
        s.flags = r.s32le();
        s.snap_target = r.s32le();
        if (version >= 6) {
            s.min_proximity = r.s32le();
            s.max_proximity = r.s32le();
            s.optimal_proximity = r.s32le();
            r.skip(8);
        }
        if (version <= 9) {   // older versions count proximity in tiles
            s.min_proximity *= 16;
            s.max_proximity *= 16;
            s.optimal_proximity *= 16;
        }
        if (version >= 7)
            s.gradient = r.f32le();
        else
            s.gradient = 0.1875f * 16;   // the original's default (per tile, scaled below)
        if (version <= 9)   // per tile in the older ones: the original's loader makes it per sixteenth
            s.gradient *= 0.0625f;
        if (version >= 8)
            s.height = r.s32le();
        if (s.height == 0)
            s.height = 5;   // non-standard: use the offset height
        if (version >= 9)
            s.facing = r.s32le();
        if (version >= 10)
            s.resolution = r.s32le();
        if (version <= 8) {   // the older ones said "any facing" and "facing away" with flags (FUN_0062c4b0)
            if (s.flags & 0x200)
                s.facing = -3;
            else if (s.flags & 0x400)
                s.facing = -1;
            s.flags &= ~0x600;
        }
    }
    return check(r, error);
}

} // namespace ts
