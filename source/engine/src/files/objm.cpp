/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSO (tso.files/Formats/IFF/Chunks/OBJM.cs, IffFieldEncode.cs).
 */
#include "files/objm.h"

#include <cstring>

namespace ts {

namespace {

// IffFieldEncode with FreeSO's string handling: strings sit byte-aligned in
// the bit stream, padded to keep the stream's 2-byte alignment.
class Fields {
public:
    Fields(const uint8_t *p, size_t n, bool odd_offset, std::vector<ObjmToken> *log = nullptr) : p_(p), n_(n), log_(log)
    {
        cur_ = next_byte();
        odd_ = !odd_offset;
    }

    bool failed() const { return failed_; }
    bool more() const { return pos_ < n_; }
    // Bits not read yet: the bytes after the one in hand, and what is left of that one.
    size_t bits_left() const { return pos_ > n_ ? 0 : (n_ - pos_) * 8 + (bit_pos_ < 8 ? size_t(8 - bit_pos_) : 0); }
    size_t bit_index() const { return n_ * 8 - bits_left(); }   // (bits read so far)

    uint8_t u8() { return uint8_t(logged('b', field(kByte))); }
    int16_t s16() { return int16_t(logged('h', field(kNarrow))); }
    int32_t s32() { return int32_t(logged('w', field(kWide))); }
    float f32()
    {
        const uint32_t v = uint32_t(logged('w', field(kWide)));
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }

    std::string str(bool next_field)
    {
        std::string s = read_str(next_field);
        if (log_) {
            ObjmToken t;
            t.kind = next_field ? 't' : 's';
            t.text = s;
            log_->push_back(std::move(t));
        }
        return s;
    }

private:
    int64_t logged(char kind, int64_t v)
    {
        if (log_) {
            ObjmToken t;
            t.kind = kind;
            t.value = v;
            log_->push_back(t);
        }
        return v;
    }

    std::string read_str(bool next_field)
    {
        if (bit_pos_ == 0) {   // the byte in hand hasn't been started: the string begins with it
            pos_--;
            odd_ = !odd_;
        }
        std::string s;
        while (true) {
            if (pos_ >= n_) {
                failed_ = true;
                break;
            }
            const char c = char(p_[pos_++]);
            if (c == 0)
                break;
            s += c;
        }
        if ((s.size() % 2 == 0) == !odd_)
            pos_++;   // pad
        bit_pos_ = 8;
        if (next_field && pos_ < n_) {
            cur_ = p_[pos_++];
            odd_ = true;
            bit_pos_ = 0;
        } else {
            odd_ = false;
        }
        return s;
    }

    static constexpr uint8_t kNarrow[4] = { 5, 8, 13, 16 };
    static constexpr uint8_t kWide[4] = { 6, 11, 21, 32 };
    static constexpr uint8_t kByte[4] = { 2, 4, 6, 8 };

    uint8_t next_byte()
    {
        if (pos_ < n_)
            return p_[pos_++];
        failed_ = true;
        return 0;
    }

    int64_t field(const uint8_t (&widths)[4])
    {
        if (!bit())
            return 0;
        const int width = widths[bits(2)];
        int64_t v = bits(width);
        if (v & (int64_t(1) << (width - 1)))
            v -= int64_t(1) << width;
        return v;
    }

    uint32_t bits(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++)
            v = v << 1 | bit();
        return v;
    }

    uint32_t bit()
    {
        const uint32_t b = bit_pos_ < 8 ? (cur_ >> (7 - bit_pos_)) & 1 : 0;   // after a string: one empty bit
        if (++bit_pos_ > 7) {
            bit_pos_ = 0;
            if (pos_ < n_) {
                cur_ = p_[pos_++];
            } else {
                cur_ = 0;
                if (pos_ > n_ + 64)
                    failed_ = true;
                pos_++;
            }
            odd_ = !odd_;
        }
        return b;
    }

    const uint8_t *p_;
    size_t n_, pos_ = 0;
    std::vector<ObjmToken> *log_ = nullptr;
    uint8_t cur_ = 0;
    int bit_pos_ = 0;
    bool odd_ = false;
    bool failed_ = false;
};

// Fields run backwards: the reader's state followed step by step (its byte in hand, how many of
// its bits are used, its odd/even flag), so that what is written is read back the same.
class FieldWriter {
public:
    explicit FieldWriter(bool odd_offset) : odd_(!odd_offset) {}

    void field(int64_t v, const uint8_t (&widths)[4])
    {
        if (v == 0) {
            bit(0);
            return;
        }
        int w = 3;
        for (int i = 0; i < 4; i++)
            if (v >= -(int64_t(1) << (widths[i] - 1)) && v < (int64_t(1) << (widths[i] - 1))) {
                w = i;
                break;
            }
        bit(1);
        bits(uint32_t(w), 2);
        bits(uint32_t(uint64_t(v) & ((widths[w] == 32) ? 0xffffffffu : ((1u << widths[w]) - 1))), widths[w]);
    }
    void str(const std::string &s, bool next_field)
    {
        if (bit_pos_ == 8) {
            // after a string another string: it starts where the last one ended, the flag as it was left
        } else if (bit_pos_ == 0) {
            odd_ = !odd_;   // (the reader steps back onto its unused byte in hand)
        } else {
            out_.push_back(cur_);   // (the rest of the byte in hand is left; the string starts at the next, the flag as it was)
        }
        for (char c : s)
            out_.push_back(uint8_t(c));
        out_.push_back(0);
        if ((s.size() % 2 == 0) == !odd_)
            out_.push_back(0);
        cur_ = 0;
        if (next_field) {
            bit_pos_ = 0;
            odd_ = true;
        } else {
            bit_pos_ = 8;
            odd_ = false;
        }
    }
    std::vector<uint8_t> finish()
    {
        if (bit_pos_ > 0 && bit_pos_ < 8)
            flush();
        return std::move(out_);
    }

private:
    void bit(uint32_t b)
    {
        if (bit_pos_ == 8) {   // after a string, a field: the reader's one empty bit first
            bit_pos_ = 0;
            cur_ = 0;
            odd_ = !odd_;
            if (b == 0)
                return;
        }
        if (b)
            cur_ = uint8_t(cur_ | (0x80 >> bit_pos_));
        if (++bit_pos_ > 7)
            flush();
    }
    void bits(uint32_t v, int n)
    {
        for (int i = n - 1; i >= 0; i--)
            bit((v >> i) & 1);
    }
    void flush()
    {
        out_.push_back(cur_);
        cur_ = 0;
        bit_pos_ = 0;
        odd_ = !odd_;
    }
    std::vector<uint8_t> out_;
    uint8_t cur_ = 0;
    int bit_pos_ = 0;
    bool odd_ = false;
};

// The id table's bits: each value present (1) or 0, then two bits of width and the value.
class TableWriter {
public:
    void u16(uint16_t v)
    {
        if (v == 0) {
            bit(0);
            return;
        }
        static constexpr uint8_t w[4] = { 5, 8, 13, 16 };
        int k = 3;
        for (int i = 0; i < 4; i++)
            if (v < (1u << (w[i] - 1))) {   // (the widths as for signed values: 16-31 take 8 bits, as the PC game's files have them)
                k = i;
                break;
            }
        bit(1);
        for (int i = 1; i >= 0; i--)
            bit((uint32_t(k) >> i) & 1);
        for (int i = w[k] - 1; i >= 0; i--)
            bit((uint32_t(v) >> i) & 1);
    }
    void finish(std::vector<uint8_t> &out)
    {
        if (bit_pos_ > 0)
            out_.push_back(cur_);
        out.insert(out.end(), out_.begin(), out_.end());
    }

private:
    void bit(uint32_t b)
    {
        if (b)
            cur_ = uint8_t(cur_ | (0x80 >> bit_pos_));
        if (++bit_pos_ > 7) {
            out_.push_back(cur_);
            cur_ = 0;
            bit_pos_ = 0;
        }
    }
    std::vector<uint8_t> out_;
    uint8_t cur_ = 0;
    int bit_pos_ = 0;
};

uint32_t u32le(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// The ID table's own bit reader, which also tells where it stopped (FreeSO's Interrupt()).
class TableReader {
public:
    TableReader(const uint8_t *p, size_t n, size_t at) : p_(p), n_(n), pos_(at) { cur_ = pos_ < n_ ? p_[pos_++] : 0; }
    uint16_t u16()
    {
        if (!bit())
            return 0;
        static constexpr uint8_t w[4] = { 5, 8, 13, 16 };
        return uint16_t(bits(w[bits(2)]));
    }
    // Where the next byte-level read starts.
    size_t interrupt() const { return bit_pos_ == 0 ? pos_ - 1 : pos_; }
    bool more() const { return pos_ < n_; }

private:
    uint32_t bits(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++)
            v = v << 1 | bit();
        return v;
    }
    uint32_t bit()
    {
        const uint32_t b = (cur_ >> (7 - bit_pos_)) & 1;
        if (++bit_pos_ > 7) {
            bit_pos_ = 0;
            cur_ = pos_ < n_ ? p_[pos_] : 0;
            pos_++;
        }
        return b;
    }
    const uint8_t *p_;
    size_t n_, pos_;
    uint8_t cur_ = 0;
    int bit_pos_ = 0;
};

} // namespace

bool parse_objm_records(const uint8_t *data, size_t size, ObjmFull &out, std::string &error)
{
    out = ObjmFull();
    if (size < 13) {
        error = "ObjM is too short";
        return false;
    }
    out.version = u32le(data + 4);
    if (u32le(data + 8) != 0x4f626a4d) {
        error = "ObjM has no MjbO";
        return false;
    }
    const size_t base = 12;   // record offsets count from here
    if (data[base] != 1) {
        error = "ObjM is not compressed";
        return false;
    }
    TableReader t(data, size, base + 1);
    while (t.more()) {
        const uint16_t id = t.u16();
        if (id == 0)
            break;
        out.id_to_type.emplace_back(id, t.u16());
    }
    size_t pos = t.interrupt();
    for (size_t i = 0; i < out.id_to_type.size(); i++) {
        if (pos + 4 > size) {
            error = "ObjM records are cut short";
            return false;
        }
        const size_t skip = base + u32le(data + pos);
        pos += 4;
        if (skip < pos || skip > size) {
            error = "ObjM record " + std::to_string(i) + " runs past the chunk";
            return false;
        }
        ObjmFull::Record r;
        r.odd = ((pos - base) & 1) != 0;
        r.bytes.assign(data + pos, data + skip);
        out.records.push_back(std::move(r));
        pos = skip;
    }
    // The last link (0), then the trailer.
    if (pos + 4 <= size) {
        out.trailer_at = pos + 4 - base;
        out.trailer.assign(data + pos + 4, data + size);
    }
    return true;
}

bool decode_objm_trailer(const ObjmFull &objm, std::vector<ObjmToken> &tokens)
{
    tokens.clear();
    if (objm.trailer.empty())
        return false;
    Fields f(objm.trailer.data(), objm.trailer.size(), (objm.trailer_at & 1) != 0, &tokens);
    f.s16();   // an object's id (the Sim selected, it seems: 0 or one of the lot's)
    const int32_t n = f.s32();
    for (int32_t i = 0; i < n && i < 4096 && !f.failed(); i++)
        f.s16();
    f.str(false);
    return !f.failed();
}

std::vector<uint8_t> encode_objm_record(const std::vector<ObjmToken> &tokens, bool odd)
{
    static constexpr uint8_t kNarrow[4] = { 5, 8, 13, 16 };
    static constexpr uint8_t kWide[4] = { 6, 11, 21, 32 };
    static constexpr uint8_t kByte[4] = { 2, 4, 6, 8 };
    FieldWriter w(odd);
    for (const ObjmToken &t : tokens) {
        switch (t.kind) {
        case 'b': w.field(t.value, kByte); break;
        case 'h': w.field(t.value, kNarrow); break;
        case 'w': w.field(t.value, kWide); break;
        case 's': w.str(t.text, false); break;
        case 't': w.str(t.text, true); break;
        default: break;
        }
    }
    return w.finish();
}

std::vector<uint8_t> write_objm(uint32_t pad, uint32_t version, const std::vector<std::pair<uint16_t, uint16_t>> &id_to_type,
                                const std::vector<std::vector<ObjmToken>> &records, const std::vector<ObjmToken> &trailer)
{
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++)
            out.push_back(uint8_t(v >> (8 * i)));
    };
    u32(pad);
    u32(version);
    u32(0x4f626a4d);   // "MjbO"
    const size_t base = out.size();
    out.push_back(1);  // compressed
    TableWriter t;
    for (const auto &[id, type] : id_to_type) {
        t.u16(id);
        t.u16(type);
    }
    t.u16(0);
    t.finish(out);
    for (const std::vector<ObjmToken> &rec : records) {
        const size_t at = out.size() + 4;   // where the record starts
        const std::vector<uint8_t> bytes = encode_objm_record(rec, ((at - base) & 1) != 0);
        u32(uint32_t(at + bytes.size() - base));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    u32(0);   // the last link
    std::vector<ObjmToken> plain = { { 'h', 0, {} }, { 'w', 0, {} }, { 's', 0, {} } };
    const std::vector<uint8_t> tail = encode_objm_record(trailer.empty() ? plain : trailer, ((out.size() - base) & 1) != 0);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

bool decode_objm_instance(const ObjmFull &objm, const ObjmFull::Record &rec, int objd_type, bool multitile,
                          ObjmInstance &out, std::string &error, std::vector<ObjmToken> *tokens, uint32_t guid)
{
    out = ObjmInstance();
    Fields f(rec.bytes.data(), rec.bytes.size(), rec.odd, tokens);
    for (int i = 0; i < 4; i++)
        f.s32();   // footprint
    out.x = f.s32();
    out.y = f.s32();
    out.level = f.s32();
    f.s16();   // unknown, usually 1
    const int16_t attr_count = f.s16();
    if (attr_count < 0 || attr_count > 512) {
        error = "bad attribute count";
        return false;
    }
    out.attributes.resize(size_t(attr_count));
    for (int16_t &a : out.attributes)
        a = f.s16();
    for (int16_t &t : out.temps)
        t = f.s16();
    for (int16_t &d : out.data)
        d = f.s16();
    for (int16_t &d : out.data_extra)
        d = f.s16();
    const int32_t frames = f.s32();
    f.s32();   // stack flags, 512 usually
    if (frames < 0 || frames > 256) {
        error = "bad stack size";
        return false;
    }
    for (int32_t i = 0; i < frames; i++) {
        ObjmInstance::Frame fr;
        fr.stack_object = f.s16();
        fr.tree = f.s16();
        fr.node = f.s16();
        const uint8_t locals = f.u8(), params = f.u8();
        for (int j = 0; j < params; j++)
            fr.params.push_back(f.s16());
        for (int j = 0; j < locals; j++)
            fr.locals.push_back(f.s16());
        fr.primitive_state = f.s32();
        fr.code_owner_type = f.s16();
        out.stack.push_back(std::move(fr));
    }
    if (f.s32() >= 0) {
        error = "unknown relationship flag";
        return false;
    }
    const int32_t rels = f.s32();
    if (rels < 0 || rels > 4096) {
        error = "bad relationship count";
        return false;
    }
    for (int32_t i = 0; i < rels; i++) {
        ObjmInstance::Relationship r;
        if (f.s32() == 0)
            continue;   // an empty entry: nothing follows it (FUN_005eca80)
        r.target = f.s32();
        const int32_t n = f.s32();
        if (n < 0 || n > 64) {
            error = "bad relationship size";
            return false;
        }
        for (int32_t j = 0; j < n; j++)
            r.values.push_back(f.s32());
        out.relationships.push_back(std::move(r));
    }
    const int16_t slots = f.s16();
    if (slots < 0) {
        error = "bad slot count";
        return false;
    }
    for (int i = 0; i < slots; i++) {
        const int16_t unk = f.s16();
        out.slots.emplace_back(unk, f.s16());
    }
    const int16_t sprites = f.s16();
    for (int i = 0; i < sprites; i++)
        out.dynamic_sprite_flags.push_back(f.s16());
    if (multitile) {   // (FUN_006306c0: two, a third from version 0x3a, three more from 0x28, the lead's id)
        f.s32();
        f.s32();
        if (objm.version >= 0x3a)
            f.s32();
        if (objm.version >= 0x28)
            for (int i = 0; i < 3; i++)
                f.s32();
        f.s16();
    }
    if (objd_type == 8) {   // portal
        const int32_t n = f.s32();
        for (int32_t i = 0; i < n && i < 256; i++)
            f.f32();
    }
    if (objd_type == 2) {   // person
        out.person = true;
        ObjmInstance::Person &p = out.sim;
        f.s32();   // animation event count
        f.s32();   // engaged
        p.body = f.str(false);
        p.body_tex = f.str(false);
        f.str(false);
        f.str(false);
        p.left_hand = f.str(false);
        p.left_hand_tex = f.str(false);
        p.right_hand = f.str(false);
        p.right_hand_tex = f.str(false);
        p.head = f.str(false);
        p.head_tex = f.str(true);
        const int32_t accessories = f.s32();
        if (accessories < 0 || accessories > 64) {
            error = "bad accessory count";
            return false;
        }
        for (int32_t i = 0; i < accessories; i++) {
            f.str(false);
            f.str(false);
        }
        p.animation = f.str(false);
        p.carry_animation = f.str(false);
        p.base_animation = f.str(true);
        f.s32();   // routing state
        for (int i = 0; i < 9; i++)
            f.f32();
        for (int i = 0; i < 16; i++)
            f.f32();   // the needs before the last update
        for (float &m : p.motives)
            m = f.f32();
        p.person_data.resize(objm.version >= 0x47 ? 0x100 : 0x50);   // (FUN_005f4220: 0x100 from version 0x47)
        for (int16_t &d : p.person_data)
            d = f.s16();
        f.s32();   // routing frame count
        auto interaction = [&]() {
            f.s32();   // zero
            f.s32();   // uid
            f.s16();   // caller
            f.s16();   // target
            f.s16();   // icon
            f.s32();   // TTA index
            for (int i = 0; i < 4; i++)
                f.s16();
            f.s32();   // priority
            f.s16();   // action tree
            f.f32();   // attenuation
            f.s32();   // flags
        };
        interaction();   // active
        interaction();   // last
        const int32_t queued = f.s32();
        if (queued < 0 || queued > 64) {
            error = "bad queue length";
            return false;
        }
        for (int32_t i = 0; i < queued; i++)
            interaction();
        const int32_t uses = f.s32();
        if (uses < 0 || uses > 256) {
            error = "bad object use count";
            return false;
        }
        for (int32_t i = 0; i < uses; i++) {
            f.s16();
            f.s32();
            f.u8();
        }
        const int32_t deltas = f.s32();
        if (deltas < 0 || deltas > 64) {
            error = "bad motive delta count";
            return false;
        }
        for (int32_t i = 0; i < deltas; i++) {
            ObjmInstance::Person::MotiveDelta d;
            d.motive = f.s32();
            d.tick_delta = f.f32();
            d.stop_at = f.f32();
            p.motive_deltas.push_back(d);
        }
    }
    if (f.failed()) {
        error = "record cut short";
        return false;
    }
    // What follows the fields read above, which every record has (the PC game's own files: two
    // or three bytes): kept as 32-bit fields to the record's end, so it is written back the same.
    // The scheduler word (FUN_0052d5f0): the object's sleep countdown (bits 0-15, 0xffff awake) and
    // its flags (16 scheduled, 17 ticks while others are held, 19 open to free will), from version
    // 0x36, but for two kinds of object always and two more in some games (then a word is there
    // when 8 bits or more are left: less is the last byte's padding).
    const bool never = guid == 0x7FD96B54u || guid == 0x3C08965Fu;
    const bool sometimes = guid == 0x4A70DF92u || guid == 0x7BEA0977u;
    if (tokens)
        tokens->push_back({ '|', int64_t(f.bit_index()), {} });   // (the object's own fields end here)
    if (objm.version > 0x35 && !never && (!sometimes || f.bits_left() >= 8))
        f.s32();
    if (tokens)
        while (f.bits_left() > 0 && !f.failed())   // (the last byte's padding: zero bits, kept as zero fields)
            f.s16();
    return true;
}

} // namespace ts
