/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Structures follow FreeSims (sims.common/rendering/vitaboy); the layouts
 * were checked against every character file in the install.
 */
#include "files/character.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/bytes.h"
#include "core/strings.h"

namespace ts {

int CharSkeleton::find(const std::string &bone) const
{
    for (size_t i = 0; i < bones.size(); i++)
        if (iequals_path(bones[i].name, bone))
            return int(i);
    return -1;
}

namespace {

// The same records come as little-endian binary (.bcf, .bmf) or as text,
// one value per line (.cmx, .skn). A source reads either.
class Source {
public:
    virtual ~Source() = default;
    virtual bool ok() const = 0;
    virtual bool at_end() = 0;
    virtual uint32_t u32() = 0;   // counts and flags
    virtual int32_t s32() = 0;
    virtual float f32() = 0;
    virtual std::string str() = 0;
};

class BinarySource : public Source {
public:
    BinarySource(const uint8_t *data, size_t size) : r_(data, size) {}
    bool ok() const override { return r_.ok(); }
    bool at_end() override { return r_.remaining() == 0; }
    uint32_t u32() override { return r_.u32le(); }
    int32_t s32() override { return int32_t(r_.u32le()); }
    float f32() override { return r_.f32le(); }
    std::string str() override { return r_.pascal_string(); }

private:
    ByteReader r_;
};

// Text: values are whitespace-separated, except strings, which take a whole
// line. Lines starting with // are comments; "version N" lines are skipped.
class TextSource : public Source {
public:
    TextSource(const uint8_t *data, size_t size) : p_(reinterpret_cast<const char *>(data)), end_(p_ + size) {}
    bool ok() const override { return ok_; }
    bool at_end() override
    {
        skip_blank();
        return p_ >= end_;
    }
    uint32_t u32() override { return uint32_t(std::strtoul(word().c_str(), nullptr, 10)); }
    int32_t s32() override { return int32_t(std::strtol(word().c_str(), nullptr, 10)); }
    float f32() override { return std::strtof(word().c_str(), nullptr); }
    std::string str() override
    {
        skip_blank();
        const char *start = p_;
        while (p_ < end_ && *p_ != '\n' && *p_ != '\r')
            p_++;
        std::string s(start, p_);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        if (start == end_)
            ok_ = false;
        return s;
    }

private:
    void skip_blank()
    {
        for (;;) {
            while (p_ < end_ && std::isspace(static_cast<unsigned char>(*p_)))
                p_++;
            const bool comment = end_ - p_ >= 2 && p_[0] == '/' && p_[1] == '/';
            const bool version = end_ - p_ >= 7 && std::strncmp(p_, "version", 7) == 0;
            if (!comment && !version)
                return;
            while (p_ < end_ && *p_ != '\n')
                p_++;
        }
    }
    std::string word()
    {
        skip_blank();
        const char *start = p_;
        while (p_ < end_ && !std::isspace(static_cast<unsigned char>(*p_)))
            p_++;
        if (start == p_)
            ok_ = false;
        return std::string(start, p_);
    }

    const char *p_, *end_;
    bool ok_ = true;
};

constexpr uint32_t kSane = 1 << 20;   // no count in these files comes near this

bool fail(std::string &error, const std::string &what)
{
    error = what;
    return false;
}

} // namespace

const std::string *AnimEvent::get(const char *key) const
{
    for (auto &[k, v] : properties)
        if (k == key)
            return &v;
    return nullptr;
}

namespace {

void read_properties(Source &in, std::vector<std::pair<std::string, std::string>> &out)
{
    const uint32_t pairs = in.u32();
    for (uint32_t i = 0; i < pairs && in.ok() && i < kSane; i++) {
        std::string key = in.str();
        out.emplace_back(std::move(key), in.str());
    }
}

bool read_character(Source &in, CharacterFile &out, std::string &error)
{
    const uint32_t skeletons = in.u32();
    if (!in.ok() || skeletons > 64)
        return fail(error, "bad skeleton count");
    for (uint32_t s = 0; s < skeletons; s++) {
        CharSkeleton sk;
        sk.name = in.str();
        const uint32_t bones = in.u32();
        if (!in.ok() || bones > 1024)
            return fail(error, "bad bone count");
        for (uint32_t b = 0; b < bones; b++) {
            CharBone bone;
            bone.name = in.str();
            bone.parent = in.str();
            if (in.u32())
                read_properties(in, bone.properties);
            bone.translation = { in.f32(), in.f32(), in.f32() };
            bone.rotation = { in.f32(), in.f32(), in.f32(), in.f32() };
            bone.can_translate = in.u32() != 0;
            bone.can_rotate = in.u32() != 0;
            bone.can_blend = in.u32() != 0;
            bone.wiggle_value = in.f32();
            bone.wiggle_power = in.f32();
            sk.bones.push_back(std::move(bone));
        }
        out.skeletons.push_back(std::move(sk));
    }
    const uint32_t suits = in.u32();
    if (!in.ok() || suits > kSane)
        return fail(error, "bad suit count");
    for (uint32_t s = 0; s < suits; s++) {
        Suit suit;
        suit.name = in.str();
        suit.type = in.u32();
        in.u32();   // always 0
        const uint32_t bindings = in.u32();
        if (!in.ok() || bindings > 1024)
            return fail(error, "bad binding count");
        for (uint32_t b = 0; b < bindings; b++) {
            SuitBinding binding;
            binding.bone = in.str();
            binding.mesh = in.str();
            in.u32();   // censorship flags and the like, unused here
            in.u32();
            suit.bindings.push_back(std::move(binding));
        }
        out.suits.push_back(std::move(suit));
    }
    const uint32_t animations = in.u32();
    if (!in.ok() || animations > kSane)
        return fail(error, "bad animation count");
    for (uint32_t a = 0; a < animations; a++) {
        CharAnimation anim;
        anim.name = in.str();
        anim.xskill = in.str();
        anim.duration = in.f32();
        anim.distance = in.f32();
        anim.moving = in.u32() != 0;
        anim.translation_count = in.u32();
        anim.rotation_count = in.u32();
        const uint32_t motions = in.u32();
        if (!in.ok() || motions > 1024 || anim.translation_count > kSane || anim.rotation_count > kSane)
            return fail(error, "bad motion count");
        for (uint32_t m = 0; m < motions; m++) {
            AnimMotion motion;
            motion.bone = in.str();
            motion.frames = in.u32();
            motion.duration = in.f32();
            motion.has_translation = in.u32() != 0;
            motion.has_rotation = in.u32() != 0;
            motion.first_translation = in.u32();
            motion.first_rotation = in.u32();
            std::vector<std::pair<std::string, std::string>> unused;
            if (in.u32())   // properties of the whole motion
                read_properties(in, unused);
            if (in.u32()) {   // events at times (sounds, "xevt" markers): count, then time + pairs each
                const uint32_t events = in.u32();
                for (uint32_t e = 0; e < events && in.ok() && e < kSane; e++) {
                    AnimEvent event;
                    event.time_ms = in.u32();
                    read_properties(in, event.properties);
                    anim.events.push_back(std::move(event));
                }
            }
            anim.motions.push_back(std::move(motion));
        }
        std::stable_sort(anim.events.begin(), anim.events.end(),
                         [](const AnimEvent &x, const AnimEvent &y) { return x.time_ms < y.time_ms; });
        out.animations.push_back(std::move(anim));
    }
    if (!in.ok())
        return fail(error, "truncated");
    if (!in.at_end())
        return fail(error, "data after the last record");
    return true;
}

// Blends are weight then vertex in .bmf files, vertex then weight in .skn files.
bool read_mesh(Source &in, CharMesh &out, std::string &error, bool vertex_first)
{
    out.name = in.str();
    out.texture = in.str();
    const uint32_t bones = in.u32();
    if (!in.ok() || bones > 1024)
        return fail(error, "bad bone count");
    for (uint32_t i = 0; i < bones; i++)
        out.bones.push_back(in.str());
    const uint32_t faces = in.u32();
    if (!in.ok() || faces > kSane)
        return fail(error, "bad face count");
    out.indices.resize(size_t(faces) * 3);
    for (uint16_t &i : out.indices)
        i = uint16_t(in.u32());
    const uint32_t bindings = in.u32();
    if (!in.ok() || bindings > 1024)
        return fail(error, "bad binding count");
    for (uint32_t i = 0; i < bindings; i++) {
        MeshBoneBinding b;
        b.bone = in.s32();
        b.first_vertex = in.s32();
        b.vertex_count = in.s32();
        b.first_blend = in.s32();
        b.blend_count = in.s32();
        out.bindings.push_back(b);
    }
    const uint32_t uvs = in.u32();
    if (!in.ok() || uvs > kSane)
        return fail(error, "bad vertex count");
    out.uvs.resize(size_t(uvs) * 2);
    for (float &f : out.uvs)
        f = in.f32();
    const uint32_t blends = in.u32();
    if (!in.ok() || blends > kSane)
        return fail(error, "bad blend count");
    for (uint32_t i = 0; i < blends; i++) {
        MeshBlend b;
        const int32_t first = in.s32(), second = in.s32();
        b.weight = float(vertex_first ? second : first) / 0x8000;
        b.vertex = vertex_first ? first : second;
        out.blends.push_back(b);
    }
    const uint32_t total = in.u32();
    if (!in.ok() || total != uvs + blends)
        return fail(error, "vertex count doesn't match");
    for (uint32_t i = 0; i < total; i++) {
        const Vec3 p{ in.f32(), in.f32(), in.f32() }, n{ in.f32(), in.f32(), in.f32() };
        (i < uvs ? out.positions : out.blend_positions).push_back(p);
        (i < uvs ? out.normals : out.blend_normals).push_back(n);
    }
    if (!in.ok())
        return fail(error, "truncated");
    for (const MeshBoneBinding &b : out.bindings)
        if (b.bone < 0 || b.bone >= int(bones) || (b.vertex_count > 0 && b.first_vertex + b.vertex_count > int(uvs)) ||
            (b.blend_count > 0 && b.first_blend + b.blend_count > int(blends)))
            return fail(error, "binding out of range");
    for (uint16_t i : out.indices)
        if (i >= uvs)
            return fail(error, "face index out of range");
    for (const MeshBlend &b : out.blends)
        if (b.vertex < 0 || b.vertex >= int(uvs))
            return fail(error, "blend vertex out of range");
    if (!in.at_end())
        return fail(error, "data after the last record");
    return true;
}

// CFP: each component array is a run of codes: 0xFF + float sets the value,
// 0xFE + u16 repeats it, anything else adds a fixed cubic step.
bool read_floats(ByteReader &r, size_t count, float *out, size_t stride)
{
    static float delta[256];
    static bool made = false;
    if (!made) {
        for (int x = 0; x < 256; x++)
            delta[x] = float(3.9676e-10 * std::pow(double(x) - 126, 3) * std::fabs(double(x) - 126));
        made = true;
    }
    float last = 0;
    for (size_t i = 0; i < count && r.ok();) {
        const uint8_t code = r.u8();
        if (code == 0xff) {
            last = r.f32le();
        } else if (code == 0xfe) {
            const uint16_t repeats = r.u16le();
            for (uint16_t j = 0; j < repeats && i < count; j++)
                out[(i++) * stride] = last;
        } else {
            last += delta[code];
        }
        if (i < count)
            out[(i++) * stride] = last;
    }
    return r.ok();
}

} // namespace

bool parse_bcf(const uint8_t *data, size_t size, CharacterFile &out, std::string &error)
{
    BinarySource in(data, size);
    return read_character(in, out, error);
}

bool parse_cmx(const uint8_t *data, size_t size, CharacterFile &out, std::string &error)
{
    TextSource in(data, size);
    return read_character(in, out, error);
}

bool parse_bmf(const uint8_t *data, size_t size, CharMesh &out, std::string &error)
{
    BinarySource in(data, size);
    return read_mesh(in, out, error, false);
}

bool parse_skn(const uint8_t *data, size_t size, CharMesh &out, std::string &error)
{
    TextSource in(data, size);
    return read_mesh(in, out, error, true);
}

bool decode_cfp(const uint8_t *data, size_t size, CharAnimation &anim, std::string &error)
{
    ByteReader r(data, size);
    anim.translations.assign(anim.translation_count, Vec3{});
    anim.rotations.assign(anim.rotation_count, Quat{});
    const size_t tc = anim.translation_count, rc = anim.rotation_count;
    bool ok = true;
    if (tc) {
        ok = ok && read_floats(r, tc, &anim.translations[0].x, 3);
        ok = ok && read_floats(r, tc, &anim.translations[0].y, 3);
        ok = ok && read_floats(r, tc, &anim.translations[0].z, 3);
    }
    if (rc) {
        ok = ok && read_floats(r, rc, &anim.rotations[0].x, 4);
        ok = ok && read_floats(r, rc, &anim.rotations[0].y, 4);
        ok = ok && read_floats(r, rc, &anim.rotations[0].z, 4);
        ok = ok && read_floats(r, rc, &anim.rotations[0].w, 4);
    }
    if (!ok)
        return fail(error, "cfp truncated");
    return true;
}

} // namespace ts
