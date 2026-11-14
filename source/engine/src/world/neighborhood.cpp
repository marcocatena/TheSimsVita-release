/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (sims.files/formats/iff/chunks/NBRS.cs, FAMI.cs).
 */
#include "core/env.h"
#include "core/memory_kind.h"
#include "world/neighborhood.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "files/chunks.h"
#include "files/content.h"
#include "files/iff.h"
#include "files/install.h"

namespace ts {

namespace {

// Little-endian reader that stops (and remembers) at the end.
struct Reader {
    const uint8_t *p;
    size_t size, at = 0;
    bool bad = false;

    bool has(size_t n)
    {
        if (at + n > size)
            bad = true;
        return !bad;
    }
    int32_t i32()
    {
        if (!has(4))
            return 0;
        int32_t v;
        std::memcpy(&v, p + at, 4);
        at += 4;
        return v;
    }
    int16_t i16()
    {
        if (!has(2))
            return 0;
        int16_t v;
        std::memcpy(&v, p + at, 2);
        at += 2;
        return v;
    }
    std::string cstring()
    {
        std::string s;
        while (has(1) && p[at] != 0)
            s += char(p[at++]);
        if (has(1))
            at++;
        return s;
    }
    void skip(size_t n)
    {
        if (has(n))
            at += n;
    }
};

// Maxis' packed fields with strings in between (FreeSims IffFieldEncode),
// as CARR uses them: the byte parity decides the padding after a string.
struct Packed {
    const uint8_t *p;
    size_t n, pos = 0;
    uint8_t cur = 0;
    int bit_pos = 0;
    bool odd = false;

    Packed(const uint8_t *data, size_t size, size_t at) : p(data), n(size), pos(at)
    {
        cur = next();
        odd = !odd;
    }
    uint8_t next() { return pos < n ? p[pos++] : 0; }
    int bit()
    {
        int b = bit_pos <= 7 ? (cur >> (7 - bit_pos)) & 1 : 0;
        if (++bit_pos > 7) {
            bit_pos = 0;
            cur = next();
            odd = !odd;
        }
        return b;
    }
    uint32_t bits(int count)
    {
        uint32_t v = 0;
        for (int i = 0; i < count; i++)
            v = v << 1 | uint32_t(bit());
        return v;
    }
    int32_t s32()
    {
        static const int kWidths[4] = { 6, 11, 21, 32 };
        if (!bit())
            return 0;
        const int width = kWidths[bits(2)];
        int64_t v = bits(width);
        if (v & (int64_t(1) << (width - 1)))
            v -= int64_t(1) << width;
        return int32_t(v);
    }
    std::string str(bool next_field)
    {
        if (bit_pos == 0) {   // the byte loaded for bits isn't used: the string starts there
            pos--;
            odd = !odd;
        }
        std::string s;
        while (pos < n && p[pos] != 0)
            s += char(p[pos++]);
        if (pos < n)
            pos++;
        if ((s.size() % 2 == 0) == !odd && pos < n)
            pos++;   // padding
        bit_pos = 8;
        if (next_field && pos < n) {
            cur = next();
            odd = true;
            bit_pos = 0;
        } else {
            odd = false;
        }
        return s;
    }
};

bool read_career(const uint8_t *data, size_t size, Career &out)
{
    Reader r{ data, size };
    r.i32();
    r.i32();
    r.skip(4);   // "MjbO"
    if (!r.has(1) || data[r.at] != 1)
        return false;   // compression code
    r.skip(1);
    out.name = r.cstring();
    if (out.name.size() % 2 == 1)
        r.skip(1);
    if (r.bad)
        return false;
    Packed f(data, size, r.at);
    const int levels = f.s32();
    if (levels < 0 || levels > 64)
        return false;
    for (int i = 0; i < levels; i++) {
        Career::Level l;
        for (int &v : l.min_required)
            v = f.s32();
        for (int &v : l.motive_delta)
            v = f.s32();
        l.salary = f.s32();
        l.start = f.s32();
        l.end = f.s32();
        l.car = f.s32();
        l.name = f.str(false);
        l.male_uniform = f.str(false);   // the uniform's body meshes and skin ($g gender, $b body, $c skin tone)
        l.female_uniform = f.str(false);
        l.uniform_skin = f.str(false);
        f.str(true);
        out.levels.push_back(std::move(l));
    }
    return true;
}

bool read_neighbor(Reader &r, Neighbor &n)
{
    if (r.i32() != 1)
        return false;   // an empty slot
    const int version = r.i32();
    if (version == 0xA)
        r.i32();
    n.name = r.cstring();
    if (n.name.size() % 2 == 0)
        r.skip(1);   // padded to an even length, counting the terminator
    r.i32();
    const int person_mode = r.i32();
    if (person_mode > 0) {
        const size_t size = version == 0x4 ? 0xa0 : 0x200;
        for (size_t i = 0; i < size; i += 2) {
            const int16_t v = r.i16();
            if (n.person_data.size() < 88)
                n.person_data.push_back(v);
        }
    }
    n.id = r.i16();
    n.guid = uint32_t(r.i32());
    r.i32();
    const int entries = r.i32();
    for (int i = 0; i < entries && !r.bad; i++) {
        r.i32();
        const int key = r.i32();
        const int count = r.i32();
        std::vector<int16_t> values;
        for (int j = 0; j < count && !r.bad; j++)
            values.push_back(int16_t(r.i32()));
        n.relationships[key] = std::move(values);
    }
    return !r.bad;
}

} // namespace

int Neighborhood::job_data(int career, int level, int field) const
{
    if (career == -1)
        level = 0;   // school: its one level, whatever is asked (the exe's job data)
    for (const Career &c : career_data) {
        if (c.id != int16_t(career))
            continue;
        if (field == 0)
            return int(c.levels.size());
        if (level < 0 || size_t(level) >= c.levels.size())
            return 0;
        const Career::Level &l = c.levels[size_t(level)];
        switch (field) {
        case 1: return l.salary;
        case 12: return l.start;
        case 13: return l.end;
        case 21: return l.car;
        case 22: return l.male_uniform.empty() ? 0 : 1;   // a uniform ("Dress Work" puts it on, outfit 3)
        default:
            if (field >= 2 && field < 12)
                return l.min_required[field - 2];
            if (field >= 14 && field < 21)
                return l.motive_delta[field - 14];
            return 0;
        }
    }
    return 0;
}

namespace {
// The index made again for the list as it is now (the caller holds its lock).
void reindex(Neighborhood::Index &x, const std::vector<Neighbor> &neighbors)
{
    x.by_id.clear();
    x.by_guid.clear();
    for (size_t i = 0; i < neighbors.size(); i++) {
        x.by_id.emplace(neighbors[i].id, i);   // (the first of an id, as the search found)
        x.by_guid.emplace(neighbors[i].guid, i);
    }
    x.data = neighbors.data();
    x.size = neighbors.size();
}

template <typename Key, typename Get>
const Neighbor *look_up(Neighborhood::Index *x, const std::vector<Neighbor> &neighbors, Key key,
                        std::unordered_map<Key, size_t> Neighborhood::Index::*map, Get get)
{
    static const bool linear = TS_ENV("TS_NEIGHBOR_LINEAR") != nullptr;   // debug: the search, as before
    if (!x || linear) {
        for (const Neighbor &n : neighbors)
            if (get(n) == key)
                return &n;
        return nullptr;
    }
    std::lock_guard<std::mutex> hold(x->lock);
    if (x->data != neighbors.data() || x->size != neighbors.size())
        reindex(*x, neighbors);
    const auto &m = x->*map;
    auto it = m.find(key);
    if (it != m.end() && it->second < neighbors.size() && get(neighbors[it->second]) == key)
        return &neighbors[it->second];
    // A miss or a stale entry (a record changed in place): searched, and the index made again
    // only if it's there after all (a plain miss costs the search, as before).
    for (const Neighbor &n : neighbors)
        if (get(n) == key) {
            reindex(*x, neighbors);
            return &n;
        }
    return nullptr;
}
} // namespace

const Neighbor *Neighborhood::by_id(int id) const
{
    return look_up<int>(index_.get(), neighbors, id, &Index::by_id, [](const Neighbor &n) { return int(n.id); });
}

Neighbor *Neighborhood::by_id(int id)
{
    return const_cast<Neighbor *>(static_cast<const Neighborhood *>(this)->by_id(id));
}

const Neighbor *Neighborhood::by_guid(uint32_t guid) const
{
    return look_up<uint32_t>(index_.get(), neighbors, guid, &Index::by_guid, [](const Neighbor &n) { return n.guid; });
}

int Neighborhood::family_friends(const Family &family, int threshold) const
{
    std::vector<const Neighbor *> members;
    for (uint32_t g : family.members)
        if (const Neighbor *n = by_guid(g))
            members.push_back(n);
    std::vector<int> friends;
    for (const Neighbor *m : members)
        for (const auto &[other, values] : m->relationships) {
            if (values.empty() || values[0] < threshold)
                continue;
            if (std::any_of(members.begin(), members.end(), [&](const Neighbor *f) { return f->id == other; }))
                continue;   // family
            const Neighbor *o = by_id(other);
            if (!o)
                continue;
            // Only someone in a family and not a pet, as the exe's count (FUN_00553d50, the family
            // number its own FUN_00553ef0 rebuilds from the families' lists; gender 2+ a pet): ours
            // counted the maid, the gardener and the cats.
            if (!in_a_family(o->guid) || (o->person_data.size() > 65 && o->person_data[65] >= 2))
                continue;
            auto back = o->relationships.find(m->id);
            if (back == o->relationships.end() || back->second.empty() || back->second[0] < threshold)
                continue;
            if (std::find(friends.begin(), friends.end(), other) == friends.end())
                friends.push_back(other);
        }
    return int(friends.size());
}

bool Neighborhood::in_a_family(uint32_t guid) const
{
    for (const Family &f : families)
        if (f.id != 0 && std::find(f.members.begin(), f.members.end(), guid) != f.members.end())
            return true;
    return false;
}

// A neighbour's own friends (Get Neighbor Data field 5, "Nag Friends" reads it), as the exe's
// (FUN_0054b540): every other neighbour in a family whose daily score with them and theirs back are
// both at the friendship threshold. Ours counted one way, anyone, and themselves.
int Neighborhood::neighbor_friends(const Neighbor &n, int threshold) const
{
    int count = 0;
    for (const Neighbor &o : neighbors) {
        if (o.id == n.id || !in_a_family(o.guid))
            continue;
        auto there = n.relationships.find(o.id);
        auto back = o.relationships.find(n.id);
        if (there != n.relationships.end() && !there->second.empty() && there->second[0] >= threshold &&
            back != o.relationships.end() && !back->second.empty() && back->second[0] >= threshold)
            count++;
    }
    return count;
}

const Family *Neighborhood::family_in_house(int house) const
{
    for (const Family &f : families)
        if (f.house == house && house != 0)
            return &f;
    return nullptr;
}

Family *Neighborhood::family_in_house(int house)
{
    return const_cast<Family *>(static_cast<const Neighborhood *>(this)->family_in_house(house));
}

const Family *Neighborhood::family_by_id(int id) const
{
    for (const Family &f : families)
        if (f.id == id && id != 0)
            return &f;
    return nullptr;
}

Family *Neighborhood::family_by_id(int id)
{
    return const_cast<Family *>(static_cast<const Neighborhood *>(this)->family_by_id(id));
}

// TATT: 8 bytes, "TTAT", a byte, then field-encoded (IffFieldEncode): how many types, and
// for each its GUID, how many attributes and their values.
void read_type_attributes(const uint8_t *p, size_t n, std::map<uint32_t, std::vector<int16_t>> &out)
{
    if (n < 14 || std::memcmp(p + 8, "TTAT", 4) != 0)
        return;
    p += 13;
    n -= 13;
    size_t pos = 0;
    int bit_pos = 0;
    auto bit = [&]() -> uint32_t {
        if (pos >= n)
            return 0;
        const uint32_t b = (p[pos] >> (7 - bit_pos)) & 1;
        if (++bit_pos > 7) {
            bit_pos = 0;
            pos++;
        }
        return b;
    };
    auto bits = [&](int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; i++)
            v = v << 1 | bit();
        return v;
    };
    auto field = [&](const uint8_t (&widths)[4]) -> int64_t {
        if (!bit())
            return 0;
        const int width = widths[bits(2)];
        int64_t v = bits(width);
        if (v & (int64_t(1) << (width - 1)))
            v -= int64_t(1) << width;
        return v;
    };
    static constexpr uint8_t kNarrow[4] = { 5, 8, 13, 16 }, kWide[4] = { 6, 11, 21, 32 };
    const int64_t types = field(kWide);
    for (int64_t t = 0; t < types && t < 4096 && pos < n; t++) {
        const uint32_t guid = uint32_t(field(kWide));
        const int64_t count = field(kWide);
        std::vector<int16_t> values;
        for (int64_t i = 0; i < count && i < 4096 && pos < n; i++)
            values.push_back(int16_t(field(kNarrow)));
        out[guid] = std::move(values);
    }
}

void read_neighbors(const uint8_t *data, size_t size, std::vector<Neighbor> &out)
{
    Reader r{ data, size };
    r.i32();
    r.i32();
    r.skip(4);   // "SRBN"
    const int count = r.i32();
    for (int i = 0; i < count && !r.bad; i++) {
        Neighbor n;
        if (read_neighbor(r, n))
            out.push_back(std::move(n));
    }
}

bool load_neighborhood(Content &content, const std::string &user_data, Neighborhood &out, std::string &error)
{
    MemScope scope(MemKind::Hood);   // (counted as the neighbourhood's on the Vita: core/memory_kind.h)
    out = Neighborhood();
    if (const FileRef *work = content.file("work.iff"))
        if (const LoadedIff *f = content.load(*work))
            for (const IffChunk &c : f->iff.chunks)
                if (std::strcmp(c.type, "CARR") == 0) {
                    out.careers.push_back(c.id);
                    Career career;
                    career.id = int16_t(c.id);
                    if (read_career(f->body(c), c.size, career))
                        out.career_data.push_back(std::move(career));
                }
    std::sort(out.careers.begin(), out.careers.end());

    std::vector<uint8_t> bytes;
    IffFile iff;
    if (!read_file(user_data + "/Neighborhood.iff", bytes) || !parse_iff(bytes.data(), bytes.size(), iff, error)) {
        if (error.empty())
            error = "can't read " + user_data + "/Neighborhood.iff";
        return false;
    }
    for (const IffChunk &c : iff.chunks) {
        Reader r{ bytes.data() + c.offset, c.size };
        if (std::strcmp(c.type, "TATT") == 0) {
            read_type_attributes(bytes.data() + c.offset, c.size, out.type_attributes);
            continue;
        }
        if (std::strcmp(c.type, "NGBH") == 0) {
            r.i32();
            out.ngbh_version = r.i32();
            continue;
        }
        if (std::strcmp(c.type, "NBRS") == 0) {
            read_neighbors(bytes.data() + c.offset, c.size, out.neighbors);
            out.neighbors.reserve(out.neighbors.size() + 256);   // Sims made while playing append without moving these
        } else if (std::strcmp(c.type, "FAMI") == 0) {
            Family f;
            f.id = int16_t(c.id);
            r.i32();
            r.i32();
            r.skip(4);   // "IMAF"
            f.house = r.i32();
            f.number = r.i32();
            f.budget = r.i32();
            f.net_worth = r.i32();
            f.friends = r.i32();
            r.i32();
            const int members = r.i32();
            for (int i = 0; i < members && !r.bad; i++)
                f.members.push_back(uint32_t(r.i32()));
            if (!r.bad)
                out.families.push_back(std::move(f));
        }
    }
    std::sort(out.neighbors.begin(), out.neighbors.end(), [](auto &a, auto &b) { return a.id < b.id; });
    // The premade Sims (Bob, Betty, the Goths, ...) were shipped with every interest (person
    // data 46-55) at 0, which makes them very hard to befriend; the Legacy Collection gives them
    // a set (riperiperi's technical overview; this install's copies already have them). So do
    // we for a neighbourhood that has none, as Create-A-Sim deals them (one none,
    // three each low, middling and high), from the Sim's own id so it's the same set every time
    // (a save then keeps whatever it became).
    for (Neighbor &n : out.neighbors) {
        if (n.person_data.size() < 56)
            continue;
        bool none = true;
        for (int i = 46; i < 56 && none; i++)
            none = n.person_data[size_t(i)] == 0;
        if (!none)
            continue;
        uint32_t seed = 0x9e3779b9u ^ uint32_t(n.guid) ^ (uint32_t(uint16_t(n.id)) << 16);
        auto next = [&](uint32_t m) {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) % m;
        };
        int need[4] = { 1, 3, 3, 3 };
        for (int i = 46; i < 56;) {
            const int v = int(next(10));
            const int band = v == 0 ? 0 : v < 4 ? 1 : v < 7 ? 2 : 3;
            if (need[band] > 0) {
                need[band]--;
                n.person_data[size_t(i++)] = int16_t(v + (v >= 7 ? int(next(2)) : 0));   // 0-10, as the game's
            }
        }
    }
    for (Family &f : out.families)   // the family's name: its FAMs string table
        if (const IffChunk *c = iff.find("FAMs", uint16_t(f.id))) {
            StringTable t;
            std::string err;
            if (parse_str(bytes.data() + c->offset, c->size, t, err))
                if (const std::string *s = t.get(0))
                    f.name = *s;
        }

    // Zoning: strings like "5, residential".
    std::vector<uint8_t> zbytes;
    IffFile ziff;
    std::string zerr;
    if (read_file(user_data + "/LotZoning.iff", zbytes) && parse_iff(zbytes.data(), zbytes.size(), ziff, zerr))
        if (const IffChunk *c = ziff.find("STR#", 1)) {
            StringTable t;
            if (parse_str(zbytes.data() + c->offset, c->size, t, zerr))
                for (size_t i = 0;; i++) {
                    const std::string *s = t.get(i);
                    if (!s)
                        break;
                    const size_t comma = s->find(',');
                    if (comma == std::string::npos)
                        continue;
                    const int house = std::atoi(s->substr(0, comma).c_str());
                    out.zoning[house] = s->find("community") != std::string::npos ? 1 : 0;
                }
        }

    // The lots there are (Houses/House##.iff), and the names the travel choosers give
    // them: STR# 2000 + house number in the folder's description files, string 0.
    std::error_code ec;
    for (std::filesystem::directory_iterator it(std::filesystem::u8path(user_data + "/Houses"), ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() == 11 && (name.compare(0, 5, "House") == 0 || name.compare(0, 5, "house") == 0) &&
            std::isdigit(uint8_t(name[5])) && std::isdigit(uint8_t(name[6])))
            out.houses.insert(std::atoi(name.c_str() + 5));
    }
    for (const char *desc : { "DTDesc.iff", "VIDesc.iff", "STDesc.iff", "MTDesc.iff", "NeighborhoodDesc.iff" }) {
        std::vector<uint8_t> dbytes;
        IffFile diff;
        std::string derr;
        if (!read_file(user_data + "/Houses/" + desc, dbytes) || !parse_iff(dbytes.data(), dbytes.size(), diff, derr))
            continue;
        for (const IffChunk &c : diff.chunks) {
            StringTable t;
            if (std::strcmp(c.type, "STR#") != 0 || c.id < 2000 || c.id > 2099 ||
                !parse_str(dbytes.data() + c.offset, c.size, t, derr))
                continue;
            if (const std::string *s = t.get(0); s && !s->empty())
                out.lot_names[c.id - 2000] = *s;
            if (const std::string *s = t.get(1); s && s->find_first_not_of(" \t\r\n") != std::string::npos)
                out.lot_descs[c.id - 2000] = *s;
        }
    }
    // The streets (the original's rollover: "77 Maple Street", "76 Gunther Goth Highway"; ours had
    // every house of the main area on Sim Lane, the user's 2026-10-29 report).
    {
        std::vector<uint8_t> sbytes;
        IffFile siff;
        std::string serr;
        if (read_file(user_data + "/StreetNames.iff", sbytes) && parse_iff(sbytes.data(), sbytes.size(), siff, serr)) {
            StringTable names, assigned;
            for (const IffChunk &c : siff.chunks)
                if (std::strcmp(c.type, "STR#") == 0 && (c.id == 2000 || c.id == 2001))
                    parse_str(sbytes.data() + c.offset, c.size, c.id == 2000 ? names : assigned, serr);
            for (size_t i = 0; i < 200; i++) {
                const std::string *a = assigned.get(i);
                if (!a || a->empty())
                    continue;
                const int street = std::atoi(a->c_str());
                if (street < 1 || street > 11)
                    continue;
                if (const std::string *fmt = names.get(size_t(street - 1)); fmt && !fmt->empty())
                    out.lot_streets[int(i) + 1] = *fmt;
            }
        }
    }
    return true;
}

} // namespace ts
