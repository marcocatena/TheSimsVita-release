/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (sims.files/formats/iff/chunks).
 */
#include "files/chunks.h"

#include "core/bytes.h"
#include "core/field_reader.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <optional>

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

uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xff000000u | uint32_t(b) << 16 | uint32_t(g) << 8 | r;
}

} // namespace

// ---------------------------------------------------------------- strings

namespace {
uint8_t g_text_language = 1;
} // namespace

void set_text_language(uint8_t language)
{
    g_text_language = language == 0 ? 1 : language;
}

uint8_t text_language()
{
    return g_text_language;
}

const std::string *StringTable::get_english(size_t index) const
{
    size_t n = 0;
    for (const StringItem &it : items)
        if (it.language == 1 && n++ == index)
            return &it.value;
    return nullptr;
}

const std::string *StringTable::get(size_t index, uint8_t language) const
{
    if (indexed && language == 1)   // (the shown language, or English where it has none)
        return index < english.size() ? &items[english[index]].value : nullptr;
    for (uint8_t lang : { language, uint8_t(1) }) {
        size_t n = 0;
        for (const StringItem &it : items) {
            if (it.language != lang)
                continue;
            if (n++ == index)
                return &it.value;
        }
    }
    return nullptr;
}

static bool parse_str_items(const uint8_t *data, size_t size, StringTable &out, std::string &error);

bool is_string_chunk(const char *type)
{
    return std::strcmp(type, "STR#") == 0 || std::strcmp(type, "CTSS") == 0 || std::strcmp(type, "TTAs") == 0 ||
           std::strcmp(type, "FAMs") == 0;
}

// Strings that are the game's data, not words to show: numbers ("1, 480, 84": a lot's place on the
// neighbourhood's map), names of animations, suits and files ("a2o-idle-neutral",
// "b001fafat_01,BODY=b001fafatlgt_01", "terrafina.flr"). Maxis kept only English (US)'s up to date:
// the other languages' places in such tables hold leftovers (LotLocations.iff's are hashes and old
// places: the neighbourhood drawn in English (UK) had its houses all over, the user's Vita,
// 2026-11-16; the pets' and Makin' Magic's animation and suit tables other animations and suits,
// the spells' ingredients other numbers; Build.iff's prices older prices), so they are read in
// English whatever the language shown. Scratchpad str_audit3.py over the install's 8,662 tables:
// 100 tables of data with leftovers, none of words taken for data but UIText STR# 148's two.
static bool numbers_only(const std::string &s)   // digits with spaces, commas and minus signs
{
    bool digit = false;
    for (unsigned char c : s) {
        if (c >= '0' && c <= '9')
            digit = true;
        else if (c != ' ' && c != ',' && c != '-')
            return false;
    }
    return digit;
}

static bool data_string(const std::string &s)
{
    if (s.empty())
        return false;
    if (numbers_only(s))
        return true;
    {   // a number, a comma and more ("61, community": LotZoning.iff's lots)
        size_t i = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
            i++;
        while (i > 0 && i < s.size() && s[i] == ' ')
            i++;
        if (i > 0 && i < s.size() && s[i] == ',')
            return true;
    }
    bool mark = false;   // a digit, '_' or '=': not in a word of any language's
    for (unsigned char c : s) {
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && !std::strchr("_-.,=;:#", c))
            return false;   // (a space too: words)
        mark = mark || digit || c == '_' || c == '=';
    }
    if (mark)
        return true;
    const size_t n = s.size();   // a file's name: ".flr"
    return n > 4 && s[n - 4] == '.' && std::isalnum(static_cast<unsigned char>(s[n - 3])) &&
           std::isalnum(static_cast<unsigned char>(s[n - 2])) && std::isalnum(static_cast<unsigned char>(s[n - 1]));
}

// A table of data: six in ten of English's strings are, or three in ten of four or more with
// hardly a string of several words among them (a suit table's "adult", "lgt" beside its suits).
static bool data_table(const std::vector<StringItem> &items, const std::vector<uint32_t> &english)
{
    int full = 0, data = 0, spaced = 0;
    for (uint32_t at : english) {
        const std::string &v = items[at].value;
        if (v.empty())
            continue;
        full++;
        if (data_string(v))
            data++;
        else if (v.find(' ') != std::string::npos)
            spaced++;
    }
    if (!full)
        return false;
    return data * 10 >= full * 6 || (full >= 4 && data * 10 >= full * 3 && spaced * 10 < full);
}

// The shown language's places in a table's items: for each of English (US)'s strings, the shown
// language's string of that index where it has a non-empty one, else English's own. Data stays
// English's (above): a table of it whole, a number anywhere. TS_STR_DATA_OLD=1: as before.
static std::vector<uint32_t> shown_places(const std::vector<StringItem> &items)
{
    std::vector<uint32_t> english, own;
    const uint8_t lang = g_text_language;
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].language == 1)
            english.push_back(uint32_t(i));
        else if (lang != 1 && items[i].language == lang)
            own.push_back(uint32_t(i));
    }
    if (own.empty())
        return english;
    static const bool data_old = std::getenv("TS_STR_DATA_OLD") != nullptr;
    if (!data_old && data_table(items, english))
        return english;
    for (size_t i = 0; i < english.size() && i < own.size(); i++)
        if (!items[own[i]].value.empty() && (data_old || !numbers_only(items[english[i]].value)))
            english[i] = own[i];
    return english;
}

bool shrink_str_chunk(const uint8_t *data, size_t size, std::vector<uint8_t> &out)
{
    StringTable table;
    std::string err;
    if (!parse_str(data, size, table, err) || (table.format != -3 && table.format != -4))
        return false;
    const size_t kept = table.english.size();
    if (kept == table.items.size() || kept > 0xffff)
        return false;
    out.clear();
    out.reserve(size);
    auto put16 = [&](int v) {
        out.push_back(uint8_t(v & 0xff));
        out.push_back(uint8_t((v >> 8) & 0xff));
    };
    put16(-3);
    put16(int(kept));
    for (uint32_t place : table.english) {   // (the shown language's, parse_str: marked English)
        const StringItem &it = table.items[place];
        out.push_back(1);
        out.insert(out.end(), it.value.begin(), it.value.end());
        out.push_back(0);
        out.insert(out.end(), it.comment.begin(), it.comment.end());
        out.push_back(0);
    }
    return true;
}

bool parse_str(const uint8_t *data, size_t size, StringTable &out, std::string &error)
{
    out.english.clear();
    out.indexed = false;
    const bool ok = parse_str_items(data, size, out, error);
    out.english = shown_places(out.items);
    out.indexed = true;
    return ok;
}

static bool parse_str_items(const uint8_t *data, size_t size, StringTable &out, std::string &error)
{
    ByteReader r(data, size);
    out.items.clear();
    out.format = r.s16le();
    if (r.remaining() == 0)
        return true;

    if (out.format == -4) {
        // Language sets with their own counts (The Sims Online, some later files).
        uint8_t sets = r.u8();
        for (uint8_t s = 0; s < sets && r.ok(); s++) {
            uint16_t n = r.u16le();
            for (uint16_t i = 0; i < n && r.ok(); i++) {
                StringItem it;
                it.language = uint8_t(r.u8() + 1);
                it.value = r.varint_string();
                it.comment = r.varint_string();
                out.items.push_back(std::move(it));
            }
        }
        return check(r, error);
    }

    if (out.format > 0) {
        // Oldest layout, no format code: a big-endian count, then Pascal strings.
        r.seek(0);
        uint16_t n = r.u16be();
        for (uint16_t i = 0; i < n && r.ok(); i++) {
            StringItem it;
            it.value = r.pascal_string();
            out.items.push_back(std::move(it));
        }
        out.format = 0;
        return check(r, error);
    }

    uint16_t n = r.u16le();
    out.items.reserve(n);
    for (uint16_t i = 0; i < n && r.ok(); i++) {
        StringItem it;
        switch (out.format) {
        case 0:   // Pascal strings
            it.value = r.pascal_string();
            break;
        case -1:  // C strings
            it.value = r.cstring();
            break;
        case -2:  // value + comment
            it.value = r.cstring();
            it.comment = r.cstring();
            break;
        case -3:  // language code + value + comment
            it.language = r.u8();
            if (it.language == 0)
                it.language = 1;
            it.value = r.cstring();
            it.comment = r.cstring();
            break;
        default:
            return fail(error, "unknown string table format " + std::to_string(out.format));
        }
        out.items.push_back(std::move(it));
    }
    return check(r, error);
}

// ---------------------------------------------------------------- palettes

bool parse_palt(const uint8_t *data, size_t size, Palette &out, std::string &error)
{
    ByteReader r(data, size);
    r.u32le();   // version
    uint32_t n = r.u32le();
    r.skip(8);
    if (!r.ok() || n > 256)
        return fail(error, "bad palette header");
    out.colors.assign(256, rgb(0, 0, 0));
    for (uint32_t i = 0; i < n; i++) {
        uint8_t cr = r.u8(), cg = r.u8(), cb = r.u8();
        out.colors[i] = rgb(cr, cg, cb);
    }
    return check(r, error);
}

static uint32_t palette_color(const Palette *p, uint8_t index)
{
    return p && index < p->colors.size() ? p->colors[index] : rgb(0, 0, 0);
}

// ---------------------------------------------------------------- SPR2

static bool read_spr2_frame_header(ByteReader &r, size_t end, uint32_t version, uint32_t default_palette,
                                   Spr2Frame &f)
{
    f.width = r.u16le();
    f.height = r.u16le();
    f.flags = r.u32le();
    f.palette_id = r.u16le();
    f.transparent_index = r.u16le();
    f.y = r.s16le();
    f.x = r.s16le();
    if (!r.ok() || r.pos() > end)
        return false;
    f.keyed = f.palette_id == 0xA3A3 && default_palette >= 0x800 && default_palette < 0x810;
    if (version == 1000 || f.palette_id == 0 || f.palette_id == 0xA3A3)
        f.palette_id = uint16_t(default_palette);
    f.rle = r.ptr();
    f.rle_size = end - r.pos();
    return true;
}

bool parse_spr2(const uint8_t *data, size_t size, Spr2 &out, std::string &error)
{
    ByteReader r(data, size);
    out.frames.clear();
    out.version = r.u32le();
    if (out.version == 1000) {
        uint32_t count = r.u32le();
        out.default_palette = r.u32le();
        if (!r.ok() || count > size / 4)
            return fail(error, "bad SPR2 header");
        std::vector<uint32_t> offsets(count);
        for (uint32_t &o : offsets)
            o = r.u32le();
        if (!r.ok())
            return fail(error, "SPR2 offset table is truncated");
        out.frames.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            size_t end = i + 1 < count ? offsets[i + 1] : size;
            if (offsets[i] > end || end > size)
                return fail(error, "SPR2 frame offset out of range");
            r.seek(offsets[i]);
            if (!read_spr2_frame_header(r, end, out.version, out.default_palette, out.frames[i]))
                return fail(error, "SPR2 frame header is truncated");
        }
        return true;
    }
    if (out.version == 1001) {
        out.default_palette = r.u32le();
        uint32_t count = r.u32le();
        if (!r.ok() || count > size / 8)
            return fail(error, "bad SPR2 header");
        out.frames.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            r.u32le();   // frame version
            uint32_t frame_size = r.u32le();
            size_t end = r.pos() + frame_size;
            if (!r.ok() || end > size)
                return fail(error, "SPR2 frame overruns the chunk");
            if (!read_spr2_frame_header(r, end, out.version, out.default_palette, out.frames[i]))
                return fail(error, "SPR2 frame header is truncated");
            r.seek(end);
        }
        return true;
    }
    return fail(error, "unknown SPR2 version " + std::to_string(out.version));
}

bool decode_spr2_frame(const Spr2Frame &f, const Palette *palette, Image &out, std::string &error)
{
    const bool has_pixels = f.flags & 1, has_z = f.flags & 2;
    const int w = f.width, h = f.height;
    out.from_palette = true;
    out.width = w;
    out.height = h;
    out.rgba.assign(size_t(w) * h, 0);
    if (has_z)
        out.depth.assign(size_t(w) * h, 255);
    else
        out.depth.clear();
    (void)has_pixels;

    ByteReader r(f.rle, f.rle_size);
    int y = 0;
    for (;;) {
        // Some frames (FlowersOutdoor "dead") fill every row and stop without an end marker.
        if (r.remaining() == 0 && y >= h)
            return true;
        uint16_t marker = r.u16le();
        if (!r.ok())
            return fail(error, "SPR2 frame has no end marker");
        int command = marker >> 13, count = marker & 0x1fff;
        if (command == 5)
            return true;
        if (command == 4) {   // `count` fully transparent rows
            y += count;
            continue;
        }
        if (command != 0)
            return fail(error, "unknown SPR2 row command " + std::to_string(command));
        if (y >= h)
            return fail(error, "SPR2 row past the bottom of the frame");

        // A row: `count` bytes including this marker, made of pixel runs.
        int bytes = count - 2, x = 0;
        size_t row = size_t(y) * w;
        while (bytes > 0) {
            uint16_t px = r.u16le();
            int px_command = px >> 13, n = px & 0x1fff;
            bytes -= 2;
            if (!r.ok() || x + (px_command == 3 ? n : 0) > w)
                return fail(error, "SPR2 row overruns the frame");
            switch (px_command) {
            case 1:   // z, colour
            case 2:   // z, colour, alpha (0-31)
                for (int i = 0; i < n; i++, x++) {
                    uint8_t z = r.u8(), index = r.u8();
                    uint32_t c = palette_color(palette, index);
                    bytes -= 2;
                    if (px_command == 2) {
                        uint8_t a = r.u8();
                        bytes--;
                        c = (c & 0x00ffffffu) | uint32_t(a * 255 / 31) << 24;
                    }
                    if (x >= w)
                        return fail(error, "SPR2 row overruns the frame");
                    // The water tiles spell their transparent corners out in this mode (magenta,
                    // drawn). Anywhere else the mode draws that index solid (FreeSims' note: the
                    // theatre's backdrop).
                    // The Water Tool's tiles (floors.iff 0x800-0x80f): the byte a sprite keeps its depth in
                    // is how much of the pixel is water or bank: 254 the water, 0 the ground outside (the
                    // key's magenta, and the lawn painted into the corner tiles), the bank's edge graded
                    // between. Drawn solid but for the key, a shore on sand had wedges of lawn at every
                    // step and a bank a few pixels wide where the original's is a thin line on the sand,
                    // its corners round (the user's lot 28 against their capture of the original,
                    // 2026-11-18); and a bank pixel of the key's own colour (index 77, an olive) was a
                    // hole. TS_WATER_COVER_OLD=1: the key alone.
                    static const bool cover_old = std::getenv("TS_WATER_COVER_OLD") != nullptr;
                    if (f.keyed && !cover_old) {
                        if (z == 0)
                            continue;
                        out.rgba[row + x] = (c & 0x00ffffffu) | uint32_t(z >= 254 ? 255 : z) << 24;
                        if (has_z)
                            out.depth[row + x] = z;
                        continue;
                    }
                    if (f.keyed && index == f.transparent_index)
                        continue;
                    out.rgba[row + x] = c;
                    if (has_z)
                        out.depth[row + x] = z;
                }
                if (px_command == 2 && (n * 3) % 2) {
                    r.u8();
                    bytes--;
                }
                break;
            case 3:   // transparent run
                x += n;
                break;
            case 6:   // colour only
                for (int i = 0; i < n; i++, x++) {
                    uint8_t index = r.u8();
                    bytes--;
                    if (x >= w)
                        return fail(error, "SPR2 row overruns the frame");
                    out.rgba[row + x] = palette_color(palette, index);
                    if (has_z)
                        out.depth[row + x] = 0;
                }
                if (n % 2) {
                    r.u8();
                    bytes--;
                }
                break;
            default:
                return fail(error, "unknown SPR2 pixel command " + std::to_string(px_command));
            }
        }
        y++;
    }
}

// ---------------------------------------------------------------- SPR#

bool parse_spr(const uint8_t *data, size_t size, Spr &out, std::string &error)
{
    ByteReader r(data, size);
    out.frames.clear();
    uint16_t v1 = r.u16le(), v2 = r.u16le();
    bool big = v1 == 0;
    out.version = big ? uint32_t((v2 & 0xff) << 8 | v2 >> 8) : v1;
    uint32_t count = r.u32(big);
    out.palette_id = uint16_t(r.u32(big));
    if (!r.ok() || count > size / 4)
        return fail(error, "bad SPR# header");

    auto read_frame = [&](size_t end, SprFrame &f) {
        r.u32(big);   // reserved
        f.height = r.u16(big);
        f.width = r.u16(big);
        f.big_endian = big;
        if (!r.ok() || r.pos() > end)
            return false;
        f.rle = r.ptr();
        f.rle_size = end - r.pos();
        return true;
    };

    if (out.version != 1001) {
        std::vector<uint32_t> offsets(count);
        for (uint32_t &o : offsets)
            o = r.u32(big);
        if (!r.ok())
            return fail(error, "SPR# offset table is truncated");
        out.frames.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            size_t end = i + 1 < count ? offsets[i + 1] : size;
            if (offsets[i] > end || end > size)
                return fail(error, "SPR# frame offset out of range");
            r.seek(offsets[i]);
            if (!read_frame(end, out.frames[i]))
                return fail(error, "SPR# frame header is truncated");
        }
        return true;
    }
    while (r.remaining() > 0) {
        r.u32(big);   // frame version
        uint32_t frame_size = r.u32(big);
        size_t end = r.pos() + frame_size;
        SprFrame f;
        if (!r.ok() || end > size || !read_frame(end, f))
            return fail(error, "SPR# frame overruns the chunk");
        out.frames.push_back(f);
        r.seek(end);
    }
    return true;
}

bool decode_spr_frame(const SprFrame &f, const Palette *palette, Image &out, std::string &error)
{
    const int w = f.width, h = f.height;
    out.from_palette = true;
    out.width = w;
    out.height = h;
    out.rgba.assign(size_t(w) * h, 0);
    out.depth.clear();

    ByteReader r(f.rle, f.rle_size);
    int y = 0;
    for (;;) {
        uint8_t command = r.u8(), count = r.u8();
        if (!r.ok())
            return fail(error, "SPR# frame has no end marker");
        switch (command) {
        case 0x00:
        case 0x10:   // start markers
            break;
        case 0x05:
            return true;
        case 0x09:   // `count` transparent rows
            y += count;
            break;
        case 0x04: { // a row of `count` bytes including this marker
            if (y >= h)
                return fail(error, "SPR# row past the bottom of the frame");
            int bytes = count - 2, x = 0;
            size_t row = size_t(y) * w;
            while (bytes > 0) {
                uint8_t px_command = r.u8(), n = r.u8();
                bytes -= 2;
                if (!r.ok())
                    return fail(error, "SPR# row is truncated");
                switch (px_command) {
                case 0x01:   // transparent run
                    x += n;
                    break;
                case 0x02: { // run of one colour
                    uint8_t index = r.u8();
                    r.u8();
                    bytes -= 2;
                    for (int i = 0; i < n; i++, x++)
                        if (x < w)
                            out.rgba[row + x] = palette_color(palette, index);
                    break;
                }
                case 0x03:   // literal colours, padded to 2 bytes
                    for (int i = 0; i < n; i++, x++) {
                        uint8_t index = r.u8();
                        if (x < w)
                            out.rgba[row + x] = palette_color(palette, index);
                    }
                    bytes -= n;
                    if (n % 2) {
                        r.u8();
                        bytes--;
                    }
                    break;
                default:
                    return fail(error, "unknown SPR# pixel command " + std::to_string(px_command));
                }
                if (x > w)
                    return fail(error, "SPR# row overruns the frame");
            }
            y++;
            break;
        }
        default:
            return fail(error, "unknown SPR# row command " + std::to_string(command));
        }
    }
}

// ---------------------------------------------------------------- DGRP

bool parse_dgrp(const uint8_t *data, size_t size, DrawGroup &out, std::string &error)
{
    ByteReader r(data, size);
    out.version = r.u16le();
    const bool old = out.version < 20003;
    uint32_t image_count = old ? r.u16le() : r.u32le();
    if (!r.ok() || image_count > size)
        return fail(error, "bad DGRP header");
    out.images.resize(image_count);
    for (DrawImage &img : out.images) {
        uint32_t sprite_count;
        if (old) {
            sprite_count = r.u16le();
            img.direction = r.u8();
            img.zoom = r.u8();
        } else {
            img.direction = r.u32le();
            img.zoom = r.u32le();
            sprite_count = r.u32le();
        }
        if (!r.ok() || sprite_count > r.remaining())
            return fail(error, "DGRP image is truncated");
        img.sprites.resize(sprite_count);
        for (DrawSprite &s : img.sprites) {
            if (old) {
                r.u16le();   // type
                s.sprite_id = r.u16le();
                s.frame = r.u16le();
                s.flags = r.u16le();
                s.offset_x = r.s16le();
                s.offset_y = r.s16le();
                if (out.version == 20001)
                    s.object_z = r.f32le();
            } else {
                s.sprite_id = r.u32le();
                s.frame = r.u32le();
                s.offset_x = r.s32le();
                s.offset_y = r.s32le();
                s.object_z = r.f32le();
                s.flags = r.u32le();
                if (out.version == 20004) {
                    s.object_x = r.f32le();
                    s.object_y = r.f32le();
                }
            }
        }
    }
    return check(r, error);
}

// ---------------------------------------------------------------- BHAV, BCON, GLOB, OBJf

bool parse_bhav(const uint8_t *data, size_t size, Bhav &out, std::string &error)
{
    ByteReader r(data, size);
    out.version = r.u16le();
    uint32_t count = 0;
    switch (out.version) {
    case 0x8000:
    case 0x8001:
        count = r.u16le();
        r.skip(8);
        break;
    case 0x8002:
        count = r.u16le();
        out.type = r.u8();
        out.args = r.u8();
        out.locals = r.u16le();
        out.flags = r.u16le();
        r.skip(2);
        break;
    case 0x8003:
        out.type = r.u8();
        out.args = r.u8();
        out.locals = r.u8();
        r.skip(2);
        out.flags = r.u16le();
        count = r.u32le();
        break;
    default:
        return fail(error, "unknown BHAV version " + std::to_string(out.version));
    }
    if (!r.ok() || count > r.remaining() / 12)
        return fail(error, "BHAV instruction count overruns the chunk");
    out.instructions.resize(count);
    for (BhavInstruction &in : out.instructions) {
        in.opcode = r.u16le();
        in.true_pointer = r.u8();
        in.false_pointer = r.u8();
        for (uint8_t &b : in.operand)
            b = r.u8();
    }
    return check(r, error);
}

bool parse_bcon(const uint8_t *data, size_t size, Bcon &out, std::string &error)
{
    ByteReader r(data, size);
    uint8_t n = r.u8();
    out.flags = r.u8();
    out.constants.resize(n);
    for (uint16_t &c : out.constants)
        c = r.u16le();
    return check(r, error);
}

bool parse_glob(const uint8_t *data, size_t size, std::string &out, std::string &error)
{
    ByteReader r(data, size);
    uint8_t first = r.u8();
    if (!r.ok())
        return fail(error, "empty GLOB");
    if (first < 48) {   // below any filename character: a Pascal string
        out = r.fixed_string(first);
        return check(r, error);
    }
    // Otherwise a C string, possibly missing its terminator at the chunk end.
    out.assign(1, char(first));
    while (r.remaining() > 0) {
        char c = char(r.u8());
        if (c == 0)
            break;
        out += c;
    }
    return true;
}

bool parse_objf(const uint8_t *data, size_t size, std::vector<ObjFunction> &out, std::string &error)
{
    ByteReader r(data, size);
    r.u32le();   // pad
    r.u32le();   // version
    r.skip(4);   // magic
    uint32_t n = r.u32le();
    if (!r.ok() || n > r.remaining() / 4)
        return fail(error, "OBJf function count overruns the chunk");
    out.resize(n);
    for (ObjFunction &f : out) {
        f.condition = r.u16le();
        f.action = r.u16le();
    }
    return check(r, error);
}

// ---------------------------------------------------------------- OBJD

bool parse_objd(const uint8_t *data, size_t size, Objd &out, std::string &error)
{
    ByteReader r(data, size);
    out.version = r.u32le();
    if (!r.ok())
        return fail(error, "empty OBJD");
    // Field counts per version (FreeSims): 136 -> 80, 138 -> 95/108, 139 -> 96,
    // 140/141 -> 97, 142 -> 105. Keep every field present so later versions work too.
    out.fields.resize(r.remaining() / 2);
    for (uint16_t &f : out.fields)
        f = r.u16le();
    if (out.fields.size() < 78)   // version 136, the oldest
        return fail(error, "OBJD has only " + std::to_string(out.fields.size()) + " fields");
    return true;
}

// ---------------------------------------------------------------- TTAB

bool parse_ttab(const uint8_t *data, size_t size, Ttab &out, std::string &error)
{
    ByteReader r(data, size);
    uint16_t count = r.u16le();
    out.interactions.clear();
    if (!r.ok() || count == 0)
        return true;
    out.version = r.u16le();
    const bool packed = out.version == 9 || out.version == 10;
    if (packed && r.u8() != 1)
        return fail(error, "unknown TTAB compression");
    out.interactions.resize(count);
    if (out.version < 4) {
        // Versions 2-3: 16-bit motive count and flags, no string index, threshold or
        // joining index (a few leftover tables in Deluxe and Livin' Large objects).
        for (size_t i = 0; i < count; i++) {
            TtabInteraction &it = out.interactions[i];
            it.action = r.u16le();
            it.test = r.u16le();
            uint16_t motives = r.u16le();
            it.flags = r.u16le();
            it.tta_index = uint32_t(i);
            it.attenuation_value = r.f32le();
            if (!r.ok() || motives > 256)
                return fail(error, "TTAB interaction is truncated");
            it.motives.resize(motives);
            for (TtabMotive &m : it.motives)
                m.max = r.s16le();
        }
        return check(r, error);
    }
    // Versions 9-10 bit-pack the fields; earlier versions are plain little-endian.
    std::optional<FieldReader> bits;
    if (packed)
        bits.emplace(r);
    auto u16 = [&] { return bits ? bits->u16() : r.u16le(); };
    auto s16 = [&] { return bits ? bits->s16() : r.s16le(); };
    auto u32 = [&] { return bits ? bits->u32() : r.u32le(); };
    auto s32 = [&] { return bits ? bits->s32() : r.s32le(); };
    auto f32 = [&] {   // packed floats are the float's bits in a 32-bit field (FreeSO IffFieldEncode.ReadFloat)
        if (!bits)
            return r.f32le();
        const uint32_t v = bits->u32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    };
    for (TtabInteraction &it : out.interactions) {
        it.action = u16();
        it.test = u16();
        uint32_t motives = u32();
        it.flags = u32();
        it.tta_index = u32();
        if (out.version > 6)
            it.attenuation_code = u32();
        it.attenuation_value = f32();
        it.autonomy_threshold = u32();
        it.joining_index = s32();
        if (!r.ok() || motives > 256)
            return fail(error, "TTAB interaction is truncated");
        it.motives.resize(motives);
        for (TtabMotive &m : it.motives) {
            if (out.version > 6)
                m.min = s16();
            m.max = s16();
            if (out.version > 6)
                m.personality = u16();
        }
        if (out.version > 9)
            it.flags2 = u32();
    }
    return check(r, error);
}

} // namespace ts
