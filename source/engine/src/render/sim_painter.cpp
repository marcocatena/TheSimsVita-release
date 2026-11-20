/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Skinning follows FreeSims (sims.common/rendering/vitaboy/Mesh.cs and
 * SimsVille/Content/Effects/Vitaboy.fx): each vertex follows one bone, and
 * blend vertices on a second bone pull it by a weight.
 */
#include "core/env.h"
#include "render/sim_painter.h"

#include "core/fast_clock.h"

#include <atomic>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "core/strings.h"

namespace ts {

namespace {
std::atomic<uint64_t> g_pose_us[7];
std::atomic<uint64_t> g_copy_us[4];
std::atomic<uint64_t> g_pose_count{ 0 }, g_pose_triangles{ 0 };
inline uint64_t pose_us(FastClock::time_point from)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(FastClock::now() - from).count());
}
} // namespace

bool g_pose_neon = TS_ENV("TS_POSE_NEON_OFF") == nullptr;

PoseProfile take_pose_profile()
{
    PoseProfile p;
    for (int i = 0; i < 7; i++)
        p.us[i] = g_pose_us[i].exchange(0);
    for (int i = 0; i < 4; i++)
        p.copy_us[i] = g_copy_us[i].exchange(0);
    p.poses = g_pose_count.exchange(0);
    p.triangles = g_pose_triangles.exchange(0);
    return p;
}

namespace {

constexpr float kUnitsPerTile = 3;   // the people's model units
// Which way a model faces and which way round its triangles go, as seen
// from the default camera. kKeepClockwise was false until 2026-10-25, which
// kept each Sim's far side and dropped the near one: heads looked backwards
// and hair showed through faces (whole-body renders without culling, with
// the depth buffer, settled which side is which).
constexpr bool kMirror = true;
constexpr float kFacingOffset = 3.14159265f;   // draw_sim's direction = the VM's facing (3 looks at the camera)
constexpr bool kKeepClockwise = true;
// The files' quaternions rotate the other way from Quat::rotate.
Quat file_rotation(const Quat &q)
{
    return { -q.x, -q.y, -q.z, q.w };
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string &s, const std::string &tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

struct Tri {
    float px[3], py[3], u[3], v[3];
    uint32_t tint[3];
    float depth;
    float dv[3];   // each corner's (the GL renderer depth-tests a Sim's parts among themselves)
    const Image *texture;
};

} // namespace

const CharAnimation *standing_idle(CharacterLibrary &library, const SimLook &look)
{
    // Pets (Unleashed: skeletons dog and kat) have no plain idle; the display pens' standing loop
    // stands them still (the pet pickers' pictures).
    const std::initializer_list<const char *> names =
        look.skeleton == "dog"   ? std::initializer_list<const char *>{ "d2o-displaypen-stand-loop", "d2o-cathouse-ground-stand-loop" }
        : look.skeleton == "kat" ? std::initializer_list<const char *>{ "k2o-displaypen-stand-loop", "k2o-cathouse-ground-stand-loop" }
        : look.child             ? std::initializer_list<const char *>{ "c2o-idle-armsdown", "c2o-idle1" }
                                 : std::initializer_list<const char *>{ "adult-idle-armsdown", "adult-stand" };
    for (const char *name : names)
        if (const CharAnimation *a = library.animation(name))
            return a;
    return nullptr;
}

namespace {

// draw_sim with a facing that may lie between the eight (`facing` 0-8, as the
// VM's) and the model tilted by `pitch` radians about its own side axis (up is
// positive): the pie menu's head looks at the choice.
// A bone's world transform as a matrix (rows) and translation: the same rotation as its
// quaternion, applied with nine multiplications a point instead of the quaternion's ~15.
struct BoneMatrix {
    float m[9];
    Vec3 t;
    Vec3 apply(const Vec3 &v) const
    {
        return { m[0] * v.x + m[1] * v.y + m[2] * v.z + t.x, m[3] * v.x + m[4] * v.y + m[5] * v.z + t.y,
                 m[6] * v.x + m[7] * v.y + m[8] * v.z + t.z };
    }
    Vec3 apply_dir(const Vec3 &v) const
    {
        return { m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                 m[6] * v.x + m[7] * v.y + m[8] * v.z };
    }
    static BoneMatrix of(const Transform &w)
    {
        const Quat &q = w.rotation;
        BoneMatrix b;
        b.m[0] = 1 - 2 * (q.y * q.y + q.z * q.z), b.m[1] = 2 * (q.x * q.y - q.w * q.z), b.m[2] = 2 * (q.x * q.z + q.w * q.y);
        b.m[3] = 2 * (q.x * q.y + q.w * q.z), b.m[4] = 1 - 2 * (q.x * q.x + q.z * q.z), b.m[5] = 2 * (q.y * q.z - q.w * q.x);
        b.m[6] = 2 * (q.x * q.z - q.w * q.y), b.m[7] = 2 * (q.y * q.z + q.w * q.x), b.m[8] = 1 - 2 * (q.x * q.x + q.y * q.y);
        b.t = w.translation;
        return b;
    }
};

// `count` points (or directions) from `in` through the bone into `out`. On the Vita four at
// a time with NEON: the same multiplications and additions in the same order as one at a
// time (NEON flushes the tiniest numbers to zero, nothing a Sim's size shows).
void skin(const BoneMatrix &w, const Vec3 *in, Vec3 *out, int count, bool directions)
{
    int i = 0;
#if defined(__ARM_NEON)
    static_assert(sizeof(Vec3) == 12, "Vec3 packs three floats");
    const float32x4_t m0 = vdupq_n_f32(w.m[0]), m1 = vdupq_n_f32(w.m[1]), m2 = vdupq_n_f32(w.m[2]);
    const float32x4_t m3 = vdupq_n_f32(w.m[3]), m4 = vdupq_n_f32(w.m[4]), m5 = vdupq_n_f32(w.m[5]);
    const float32x4_t m6 = vdupq_n_f32(w.m[6]), m7 = vdupq_n_f32(w.m[7]), m8 = vdupq_n_f32(w.m[8]);
    const float32x4_t tx = vdupq_n_f32(directions ? 0.0f : w.t.x), ty = vdupq_n_f32(directions ? 0.0f : w.t.y),
                      tz = vdupq_n_f32(directions ? 0.0f : w.t.z);
    for (; i + 4 <= count; i += 4) {
        const float32x4x3_t v = vld3q_f32(&in[i].x);   // x, y and z of four points apart
        float32x4x3_t r;
        r.val[0] = vmlaq_f32(vmlaq_f32(vmulq_f32(m0, v.val[0]), m1, v.val[1]), m2, v.val[2]);
        r.val[1] = vmlaq_f32(vmlaq_f32(vmulq_f32(m3, v.val[0]), m4, v.val[1]), m5, v.val[2]);
        r.val[2] = vmlaq_f32(vmlaq_f32(vmulq_f32(m6, v.val[0]), m7, v.val[1]), m8, v.val[2]);
        if (!directions) {
            r.val[0] = vaddq_f32(r.val[0], tx);
            r.val[1] = vaddq_f32(r.val[1], ty);
            r.val[2] = vaddq_f32(r.val[2], tz);
        }
        vst3q_f32(&out[i].x, r);
    }
#endif
    for (; i < count; i++)
        out[i] = directions ? w.apply_dir(in[i]) : w.apply(in[i]);
}

bool draw_sim_turned(DrawList *out, DrawMesh *mesh_out, const Projector &p, CharacterLibrary &library, const SimLook &look,
                     float x, float y, float z, float facing, float pitch, const SimPose &pose_in, std::string &problem,
                     uint32_t colour, const char *mesh_filter = nullptr, PoseLookups *lookups = nullptr)
{
    const SimPose *pose_used = &pose_in;
#if !defined(__vita__)
    // Debug: TS_FORCE_ANIM=<skeleton>:<animation> poses every such skeleton in that animation.
    SimPose forced;
    if (static const char *force = TS_ENV("TS_FORCE_ANIM"); force) {
        const std::string f = force;
        const size_t colon = f.find(':');
        if (colon != std::string::npos && look.skeleton == f.substr(0, colon)) {
            std::lock_guard<std::recursive_mutex> hold(library.lock());
            forced = pose_in;
            forced.layers[0].animation = forced.layers[1].animation = nullptr;
            forced.carry = nullptr;
            forced.animation = library.animation(f.substr(colon + 1));
            pose_used = &forced;
        }
    }
#endif
    const SimPose &pose = *pose_used;
    FastClock::time_point t_part = FastClock::now();
    uint64_t skin_us = 0, project_us = 0, tris_us = 0;
    // Everything looked up by name (and the files it reads), in one go with the library
    // held: Sims are posed on several threads at once. The sums after need nothing shared.
    struct Use {
        const CharMesh *mesh;
        const Image *texture;
        const std::vector<int> *bones;   // each binding's bone, by index
    };
    thread_local std::vector<Use> uses;
    uses.clear();
    // The look as a number (its skeleton, suits and textures), and whether the lookups kept from
    // this Sim's last pose still hold: then nothing is looked up by name and the library's lock
    // isn't taken (see PoseLookups).
    uint64_t look_key = 1469598103934665603ull;
    {
        auto mix_text = [&look_key](const std::string &t) {
            for (char ch : t)
                look_key = (look_key ^ uint8_t(ch)) * 1099511628211ull;
            look_key = (look_key ^ 0xffu) * 1099511628211ull;
        };
        mix_text(look.skeleton);
        for (const SimLook::Part &part : look.parts) {
            mix_text(part.suit);
            for (const auto &[tag, tex] : part.textures) {
                mix_text(tag);
                mix_text(tex);
            }
        }
        if (look_key == 0)
            look_key = 1;
    }
    const unsigned let_go = library.let_go_count();
    bool fast = lookups && !mesh_filter && lookups->look_key == look_key && lookups->let_go == let_go && lookups->skeleton &&
                lookups->animation == pose.animation && lookups->layers[0] == pose.layers[0].animation &&
                lookups->layers[1] == pose.layers[1].animation && lookups->carry == pose.carry;
    if (fast)
        for (const PoseLookups::Use &u : lookups->uses)
            if (!u.texture->has_pixels()) {   // (let go by a trim: read in again below)
                fast = false;
                break;
            }
    const size_t problem_before = problem.size();
    const CharSkeleton *sk = nullptr;
    const std::vector<Transform> *rest_pose = nullptr;
    const std::vector<int> *parent_of = nullptr, *motion_bone = nullptr;
    const std::vector<int> *layer_bone[2] = { nullptr, nullptr };
    const std::vector<int> *carry_bone = nullptr;
    int pelvis_bone = 0;
    const std::vector<int> *censor_bones = nullptr;
    if (fast) {
        sk = lookups->skeleton;
        rest_pose = lookups->rest;
        parent_of = lookups->parents;
        pelvis_bone = lookups->pelvis;
        censor_bones = lookups->censor;
        motion_bone = lookups->motion_bone;
        carry_bone = lookups->carry_bone;
        layer_bone[0] = lookups->layer_bone[0];
        layer_bone[1] = lookups->layer_bone[1];
        for (const PoseLookups::Use &u : lookups->uses)
            uses.push_back({ u.mesh, u.texture, u.bones });
        problem += lookups->problem;
    } else {
        {
            std::lock_guard<std::recursive_mutex> hold(library.lock());
            sk = library.skeleton(look.skeleton);
            if (!sk) {
                problem = "no skeleton " + look.skeleton;
                return false;
            }
            PoseCache &cache = library.pose_cache();
            const size_t n = sk->bones.size();
            // The skeleton's own pose and each bone's parent, by index (worked out once).
            std::vector<Transform> &rest = cache.rest[sk];
            std::vector<int> &parents = cache.parents[sk];
            if (rest.size() != n) {
                rest.resize(n);
                parents.resize(n);
                for (size_t i = 0; i < n; i++) {
                    rest[i] = { file_rotation(sk->bones[i].rotation), sk->bones[i].translation };
                    parents[i] = sk->find(sk->bones[i].parent);
                }
            }
            rest_pose = &rest;
            parent_of = &parents;
            if (auto it = cache.pelvis.find(sk); it != cache.pelvis.end()) {
                pelvis_bone = it->second;
            } else {
                pelvis_bone = std::max(0, sk->find("PELVIS"));
                cache.pelvis[sk] = pelvis_bone;
            }
            std::vector<int> &censor = cache.censor_bones[sk];
            if (censor.empty())
                for (const char *bone : { "PELVIS", "SPINE2", "HEAD", "L_HAND", "R_HAND", "L_FOOT", "R_FOOT" })
                    censor.push_back(sk->find(bone));
            censor_bones = &censor;
            if (const CharAnimation *a = pose.animation) {
                std::vector<int> &bones = cache.motion_bones[{ a, sk }];
                if (bones.size() != a->motions.size()) {
                    bones.clear();
                    for (const AnimMotion &m : a->motions)
                        bones.push_back(sk->find(m.bone));
                }
                motion_bone = &bones;
    #if !defined(__vita__)
                static const char *bones_log = TS_ENV("TS_BONES");   // debug: a skeleton and an animation's motions on it
                if (bones_log && look.skeleton.find(bones_log) != std::string::npos) {
                    static std::set<std::pair<const void *, const void *>> told;
                    if (told.insert({ a, sk }).second) {
                        std::printf("bones of %s:\n", look.skeleton.c_str());
                        for (size_t i = 0; i < n; i++) {
                            const auto &b = sk->bones[i];
                            std::printf("  %2lu %-12s parent %-12s t (%.2f %.2f %.2f) r (%.3f %.3f %.3f %.3f) can t%d r%d b%d\n",
                                        (unsigned long)i, b.name.c_str(), b.parent.c_str(), b.translation.x, b.translation.y,
                                        b.translation.z, b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w, int(b.can_translate),
                                        int(b.can_rotate), int(b.can_blend));
                        }
                        std::printf("motions of %s:\n", a->name.c_str());
                        for (size_t k = 0; k < a->motions.size(); k++) {
                            const AnimMotion &m = a->motions[k];
                            std::printf("  %-12s -> %d, %u frames, t %d r %d", m.bone.c_str(), bones[k], m.frames, int(m.has_translation),
                                        int(m.has_rotation));
                            if (m.has_rotation && m.first_rotation < a->rotations.size()) {
                                const auto &q = a->rotations[m.first_rotation];
                                std::printf(" first r (%.3f %.3f %.3f %.3f)", q.x, q.y, q.z, q.w);
                                if (m.first_rotation + m.frames - 1 < a->rotations.size()) {
                                    const auto &e = a->rotations[m.first_rotation + m.frames - 1];
                                    std::printf(" last r (%.3f %.3f %.3f %.3f)", e.x, e.y, e.z, e.w);
                                }
                            }
                            std::printf("\n");
                        }
                    }
                }
    #endif
            }
            if (const CharAnimation *a = pose.carry) {
                std::vector<int> &bones = cache.motion_bones[{ a, sk }];
                if (bones.size() != a->motions.size()) {
                    bones.clear();
                    for (const AnimMotion &m : a->motions)
                        bones.push_back(sk->find(m.bone));
                }
                carry_bone = &bones;
            }
            for (int l = 0; l < 2; l++)
                if (const CharAnimation *a = pose.layers[l].animation) {
                    std::vector<int> &bones = cache.motion_bones[{ a, sk }];
                    if (bones.size() != a->motions.size()) {
                        bones.clear();
                        for (const AnimMotion &m : a->motions)
                            bones.push_back(sk->find(m.bone));
                    }
                    layer_bone[l] = &bones;
                }
            for (const SimLook::Part &part : look.parts) {
                if (part.suit.empty())
                    continue;   // (no hand: a gesture or an outfit with none)
                // Its meshes and their textures (the Sim's choice for each mesh's tag, else the
                // mesh's own), found by name once.
                uint64_t key = 1469598103934665603ull;
                auto mix = [&key](const std::string &str) {
                    for (char ch : str)
                        key = (key ^ uint8_t(ch)) * 1099511628211ull;
                    key = (key ^ 0xffu) * 1099511628211ull;
                };
                mix(part.suit);
                for (const auto &[tag, tex] : part.textures) {
                    mix(tag);
                    mix(tex);
                }
                const PoseCache::Part *found = nullptr;
                for (auto [it, last] = cache.parts.equal_range(key); it != last; ++it)
                    if (it->second.part.suit == part.suit && it->second.part.textures == part.textures)
                        found = &it->second;
                if (!found) {
                    PoseCache::Part made;
                    made.part = part;
                    if (const Suit *suit = library.suit(part.suit)) {
                        for (const SuitBinding &binding : suit->bindings) {
                            const CharMesh *mesh = library.mesh(binding.mesh);
                            if (!mesh) {
                                made.problem += "no mesh " + binding.mesh + "; ";
                                continue;
                            }
                            std::string tex_name = mesh->texture;
                            const std::string mesh_name = lower(mesh->name);
                            // The Sim's texture for the mesh's tag: "-BODY", and the naked bodies' "-MBODY"
                            // (women), "-BODYB" (men) and "-BODYU" (children), so a one-word tag matches the
                            // last word of the mesh's name that has it. (Matched by the end only, the naked
                            // men and children kept their mesh's own texture, "x": grey, the 2026-10-27 report.)
                            // Matched against the suit's name for the mesh (the file's: "...-PELVIS-BODY") as
                            // well as the mesh's own, which isn't always the same: Deluxe's disco jumpsuit 2
                            // calls itself "...-PELVIS-JUMPSUIT_2", kept its own texture (not in the
                            // install) and was drawn without a body (the 2026-11-10 report).
                            const auto matches = [](const std::string &name, const std::string &t) {
                                const size_t last_dash = name.rfind('-');
                                const std::string last_word = last_dash == std::string::npos ? name : name.substr(last_dash + 1);
                                // A two-word tag ("head-head") matches the men's "-HEAD-HEADB" too (their head
                                // meshes end in a B, as the naked bodies': Bob's head grey, 2026-10-30).
                                return ends_with(name, t) ||
                                       (t.find('-') == std::string::npos && !t.empty() && last_word.find(t) != std::string::npos) ||
                                       (t.find('-') != std::string::npos && name.find("-" + t) != std::string::npos);
                            };
                            static const bool binding_off = TS_ENV("TS_TEX_BINDING_OFF") != nullptr;   // as before
                            const std::string binding_name = lower(binding.mesh);
                            for (const auto &[tag, tex] : part.textures) {
                                const std::string t = lower(tag);
                                if (tag == "*" || matches(mesh_name, t) || (!binding_off && matches(binding_name, t)))
                                    tex_name = tex;
                            }
                            const Image *tex = library.texture(tex_name);
                            if (TS_ENV("TS_SUIT_LOG"))   // debug: each suit's meshes and the texture each takes
                                std::printf("suit %s: mesh %s (its texture %s) -> %s%s\n", part.suit.c_str(), mesh->name.c_str(),
                                            mesh->texture.c_str(), tex_name.c_str(), tex ? "" : " (missing)");
                            if (!tex) {
                                made.problem += "no texture " + tex_name + "; ";
                                continue;
                            }
                            made.meshes.push_back({ mesh, tex });
                        }
                    } else {
                        made.problem += "no suit " + part.suit + "; ";
                    }
                    found = &cache.parts.emplace(key, std::move(made))->second;
                }
                problem += found->problem;
                for (const PoseCache::Mesh &resolved : found->meshes) {
                    if (mesh_filter && resolved.mesh->name.find(mesh_filter) == std::string::npos)
                        continue;   // (a pet's head alone: its coat's "-HEAD-DOGBODY-HEAD" mesh)
                    std::vector<int> &binding_bones = cache.binding_bones[{ resolved.mesh, sk }];
                    if (binding_bones.size() != resolved.mesh->bindings.size()) {
                        binding_bones.clear();
                        for (const MeshBoneBinding &b : resolved.mesh->bindings)
                            binding_bones.push_back(sk->find(resolved.mesh->bones[size_t(b.bone)]));
                    }
                    uses.push_back({ resolved.mesh, resolved.texture, &binding_bones });
                }
            }
        }
        if (lookups) {   // kept for the next pose
            lookups->look_key = look_key;
            lookups->let_go = let_go;
            lookups->animation = pose.animation;
            lookups->layers[0] = pose.layers[0].animation;
            lookups->layers[1] = pose.layers[1].animation;
            lookups->carry = pose.carry;
            lookups->skeleton = sk;
            lookups->rest = rest_pose;
            lookups->parents = parent_of;
            lookups->pelvis = pelvis_bone;
            lookups->censor = censor_bones;
            lookups->motion_bone = motion_bone;
            lookups->carry_bone = carry_bone;
            lookups->layer_bone[0] = layer_bone[0];
            lookups->layer_bone[1] = layer_bone[1];
            lookups->uses.clear();
            for (const Use &u : uses)
                lookups->uses.push_back({ u.mesh, u.texture, u.bones });
            lookups->problem = problem.substr(problem_before);
        }
    }
    g_pose_us[0] += pose_us(t_part);
    t_part = FastClock::now();
    const size_t n = sk->bones.size();

    // Each bone's local transform: the skeleton's, or this frame of the animation.
    thread_local std::vector<Transform> local, world;
    thread_local std::vector<int> done;
    thread_local std::vector<BoneMatrix> mats;
    local.assign(rest_pose->begin(), rest_pose->end());
    // One animation's frame into `into` (bones it doesn't move keep what they have).
    auto sample = [&](const CharAnimation *a, const std::vector<int> &bone_of, float time_ms, std::vector<Transform> &into,
                      bool hold_end = false) {
        for (size_t k = 0; k < a->motions.size(); k++) {
            const AnimMotion &m = a->motions[k];
            const int b = bone_of[k];
            if (b < 0 || m.frames == 0)
                continue;
            const float d = m.duration > 0 ? m.duration : a->duration;
            const float t = d <= 0 ? 0 : hold_end ? std::clamp(time_ms / d, 0.0f, 1.0f) : std::fmod(time_ms, d) / d;
            const uint32_t frame = std::min(m.frames - 1, uint32_t(t * float(m.frames)));
            if (m.has_translation && m.first_translation + frame < a->translations.size())
                into[size_t(b)].translation = a->translations[m.first_translation + frame];
            if (m.has_rotation && m.first_rotation + frame < a->rotations.size())
                into[size_t(b)].rotation = file_rotation(a->rotations[m.first_rotation + frame]);
        }
    };
    if (const CharAnimation *a = pose.animation)
        sample(a, *motion_bone, pose.time_ms, local, pose.hold_end);
    if ((pose.layers[0].animation && pose.layers[0].weight > 0) || (pose.layers[1].animation && pose.layers[1].weight > 0)) {
        // Mixed: each bone the weighted mean of the animations' (translations averaged,
        // rotations by their normalised weighted sum, each turned to the first's side).
        thread_local std::vector<Transform> other;
        const float w0 = pose.animation ? std::max(0.0f, pose.weight) : 0.0f;
        float total = w0;
        thread_local std::vector<Vec3> tsum;
        thread_local std::vector<Quat> qsum;
        tsum.assign(n, Vec3{});
        qsum.assign(n, Quat{ 0, 0, 0, 0 });
        auto add = [&](const std::vector<Transform> &t, float w) {
            for (size_t i = 0; i < n; i++) {
                tsum[i] = tsum[i] + t[i].translation * w;
                Quat q = t[i].rotation;
                const Quat &ref = local[i].rotation;
                if (q.x * ref.x + q.y * ref.y + q.z * ref.z + q.w * ref.w < 0)
                    q = Quat{ -q.x, -q.y, -q.z, -q.w };
                qsum[i] = Quat{ qsum[i].x + q.x * w, qsum[i].y + q.y * w, qsum[i].z + q.z * w, qsum[i].w + q.w * w };
            }
        };
        if (w0 > 0)
            add(local, w0);
        for (int l = 0; l < 2; l++) {
            const SimPose::Layer &layer = pose.layers[l];
            if (!layer.animation || layer.weight <= 0 || !layer_bone[l])
                continue;
            other.assign(rest_pose->begin(), rest_pose->end());
            sample(layer.animation, *layer_bone[l], layer.time_ms, other);
            add(other, layer.weight);
            total += layer.weight;
        }
        if (total > 0)
            for (size_t i = 0; i < n; i++) {
                local[i].translation = tsum[i] * (1.0f / total);
                local[i].rotation = qsum[i].normalized();
            }
    }
    if (pose.carry && carry_bone)   // the carry pose over it all, its first frame
        sample(pose.carry, *carry_bone, 0, local);
    if (pose.in_place && !local.empty()) {
        // The root bone kept over the rest pose's spot, its height the animation's own: Create-A-Sim's
        // wave and celebration lift and drop the root, and held at the rest height the Sim sank through
        // the window's floor with the arm out of its top (the shots of 2026-11-10).
        const Vec3 rest = (*rest_pose)[0].translation;
        local[0].translation.x = rest.x;
        local[0].translation.z = rest.z;
    }
    // World transforms, parents first.
    world.resize(n);
    done.assign(n, 0);
    struct Solver {
        const std::vector<int> &parents;
        std::vector<Transform> &local, &world;
        std::vector<int> &done;
        void solve(size_t i)
        {
            if (done[i])
                return;
            done[i] = 1;
            const int parent = parents[i];
            if (parent >= 0 && size_t(parent) != i) {
                solve(size_t(parent));
                world[i] = world[size_t(parent)] * local[i];
            } else {
                world[i] = local[i];
            }
        }
    } solver{ *parent_of, local, world, done };
    for (size_t i = 0; i < n; i++)
        solver.solve(i);
    // The head turned (the game's head seek): it and every bone below it, about the up axis
    // through the head bone.
    if (pose.head_yaw != 0) {
        const int head = sk->find("HEAD");
        if (head >= 0) {
            const float hc = std::cos(pose.head_yaw * 0.5f), hs = std::sin(pose.head_yaw * 0.5f);
            const Quat turn{ 0, hs, 0, hc };
            const Vec3 pivot = world[size_t(head)].translation;
            for (size_t i = 0; i < n; i++) {
                bool below = false;
                for (int b = int(i), guard = 0; b >= 0 && guard < 64; b = (*parent_of)[size_t(b)], guard++) {
                    if (b == head) {
                        below = true;
                        break;
                    }
                    if ((*parent_of)[size_t(b)] == b)
                        break;
                }
                if (!below)
                    continue;
                const Vec3 d = world[i].translation - pivot;
                world[i].translation = pivot + turn.rotate(d);
                world[i].rotation = (turn * world[i].rotation).normalized();
            }
        }
    }
    mats.resize(n);
    for (size_t i = 0; i < n; i++)
        mats[i] = BoneMatrix::of(world[i]);
    const BoneMatrix no_bone = BoneMatrix::of(Transform{});
    g_pose_us[1] += pose_us(t_part);

    // Model space to lot space: turned to face the Sim's direction.
    const float angle = facing / 8 * 2 * 3.14159265f + kFacingOffset;
    const float pc = std::cos(pitch), ps = std::sin(pitch);
    auto tilt = [&](const Vec3 &m) { return pitch == 0 ? m : Vec3{ m.x, m.y * pc + m.z * ps, -m.y * ps + m.z * pc }; };
    const float c = std::cos(angle), s = std::sin(angle);
    auto to_lot = [&](const Vec3 &model, float &lx, float &ly, float &lz) {
        const Vec3 m = tilt(model);
        const float mx = (kMirror ? -m.x : m.x) / kUnitsPerTile, mz = m.z / kUnitsPerTile;
        lx = x + mx * c - mz * s;
        ly = y + mx * s + mz * c;
        lz = z + m.y / kUnitsPerTile;
    };
    // The original's (the Legacy's character shaders, 0x5b6970 and 0x5b6310, set up by 0x5d09d0):
    // c = clamp(L + L * max(0, n.S), 0, 1) times the texture, L the light the Sim stands in (full
    // by day) and S GameData/Lights/stdlight.lgt's sun, turned with the camera so it comes from
    // the camera's side, 24.7 degrees up. So by day a Sim is its textures as they are, unshaded.
    // Ours was a guess, 0.58 + 0.55 from above-left: by day 0.7 of that, the user saw our Sims
    // darker than the original's (Bella's dress 1.4 times brighter there). Not done: the room's
    // lamps' own light on them (warm, by distance), L eased over 250 ms. TS_OLD_SIM_LIGHT: debug,
    // the old light.
    static const bool old_light = TS_ENV("TS_OLD_SIM_LIGHT") != nullptr;
    static const int kSide[4][2] = { { 1, 1 }, { 1, -1 }, { -1, -1 }, { -1, 1 } };   // the camera's side
    const Vec3 light = old_light ? Vec3{ -0.45f, -0.6f, 0.75f }.normalized()
                                 : Vec3{ 0.6425f * float(kSide[p.rotation & 3][0]), 0.6425f * float(kSide[p.rotation & 3][1]), 0.4177f };
    const float level = std::clamp(pose.light, 0.0f, 1.0f);
    // The lamps (0x626590, 0x64eda0): warm (255, 251, 209), added on top, not scaled by the light.
    Vec3 lamp_dir[6];
    float lamp_strength[6];
    const int lamps = old_light ? 0 : std::clamp(pose.lamp_count, 0, 6);
    for (int j = 0; j < lamps; j++) {
        lamp_dir[j] = Vec3{ pose.lamps[j].dx, pose.lamps[j].dy, pose.lamps[j].dz }.normalized();
        lamp_strength[j] = pose.lamps[j].strength;
    }

    thread_local std::vector<Tri> tris;
    thread_local std::vector<Vec3> pos, nrm, blend_pos;
    thread_local std::vector<float> sx, sy, depth;
    thread_local std::vector<uint32_t> tint;
    tris.clear();
    int meshes = 0;
    for (const Use &use : uses) {
        const CharMesh *mesh = use.mesh;
        const Image *tex = use.texture;
        const std::vector<int> &binding_bones = *use.bones;
        // Skin: every real vertex on its bone, then blend vertices pull theirs.
        t_part = FastClock::now();
        const size_t vc = mesh->positions.size();
        pos.assign(vc, Vec3{});
        nrm.assign(vc, Vec3{});
        blend_pos.assign(mesh->blend_positions.size(), Vec3{});
        for (size_t k = 0; k < mesh->bindings.size(); k++) {
            const MeshBoneBinding &b = mesh->bindings[k];
            const int bone = binding_bones[k];
            const BoneMatrix &w = bone >= 0 ? mats[size_t(bone)] : no_bone;
            if (b.vertex_count > 0) {
                skin(w, &mesh->positions[size_t(b.first_vertex)], &pos[size_t(b.first_vertex)], b.vertex_count, false);
                skin(w, &mesh->normals[size_t(b.first_vertex)], &nrm[size_t(b.first_vertex)], b.vertex_count, true);
            }
            if (b.blend_count > 0)
                skin(w, &mesh->blend_positions[size_t(b.first_blend)], &blend_pos[size_t(b.first_blend)], b.blend_count, false);
        }
        for (size_t i = 0; i < mesh->blends.size(); i++) {
            const MeshBlend &bl = mesh->blends[i];
            pos[size_t(bl.vertex)] = pos[size_t(bl.vertex)] * (1 - bl.weight) + blend_pos[i] * bl.weight;
        }
#if !defined(__vita__)
        if (static const char *ext = TS_ENV("TS_MESH_EXTENT"); ext && look.skeleton.find(ext) != std::string::npos && vc) {
            Vec3 lo = pos[0], hi = pos[0];   // debug: each mesh's skinned extent (model space, y up)
            for (const Vec3 &q : pos) {
                lo = { std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z) };
                hi = { std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z) };
            }
            int unbound = 0;
            for (int bb : binding_bones)
                unbound += bb < 0;
            std::printf("mesh %s: x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f, %lu bindings, %d unbound; %s %.0f ms, layers %s %.2f / %s %.2f, carry %s\n",
                        mesh->name.c_str(), lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, (unsigned long)binding_bones.size(), unbound,
                        pose.animation ? pose.animation->name.c_str() : "-", pose.time_ms,
                        pose.layers[0].animation ? pose.layers[0].animation->name.c_str() : "-", pose.layers[0].weight,
                        pose.layers[1].animation ? pose.layers[1].animation->name.c_str() : "-", pose.layers[1].weight,
                        pose.carry ? pose.carry->name.c_str() : "-");
        }
#endif
        skin_us += pose_us(t_part);
        t_part = FastClock::now();
        // Project and light.
        sx.resize(vc);
        sy.resize(vc);
        depth.resize(vc);
        tint.resize(vc);
        size_t v0 = 0;
#if defined(__ARM_NEON)
        // On the Vita four vertices at a time (NEON), the same steps in the same order, but for two: the model's
        // units over 3 by multiplying by a third, and the normal's length by NEON's reciprocal square root taken
        // twice nearer (the vector unit divides by neither). Both to about the last bit of a float: a vertex moves
        // ~0.00001 of a pixel, a tint now and then one step of 255. This part was 0.65-0.8 ms of a ~2.3 ms pose
        // (2026-11-17). Not with a tilt (the portraits) or the old light. TS_POSE_NEON_OFF=1 (the remote's
        // "poseneon 0"): one at a time, as before (and as the PC always does).
        if (g_pose_neon && pitch == 0 && !old_light) {
            static const float cos30 = std::cos(3.14159265f / 6);
            const float third = 1.0f / kUnitsPerTile;
            const float32x4_t vx = vdupq_n_f32(x), vy = vdupq_n_f32(y), vz = vdupq_n_f32(z);
            const float32x4_t vc4 = vdupq_n_f32(c), vs4 = vdupq_n_f32(s), v3 = vdupq_n_f32(third);
            const float32x4_t hw = vdupq_n_f32(p.m.half_w), hh = vdupq_n_f32(p.m.half_h);
            const float32x4_t up = vdupq_n_f32(p.m.one_unit), c30 = vdupq_n_f32(cos30);
            const float32x4_t offx0 = vdupq_n_f32(p.off_x), offy0 = vdupq_n_f32(p.off_y);
            const float32x4_t cadge = vdupq_n_f32(float(p.m.cadge_w) / 2.0f), base = vdupq_n_f32(float(p.m.baseline));
            const float32x4_t kdiag = vdupq_n_f32(Projector::kDepthPerDiagonal), kheight = vdupq_n_f32(Projector::kDepthPerHeight);
            const float32x4_t lx4 = vdupq_n_f32(light.x), ly4 = vdupq_n_f32(light.y), lz4 = vdupq_n_f32(light.z);
            const float32x4_t lev = vdupq_n_f32(level), zero = vdupq_n_f32(0.0f), one = vdupq_n_f32(1.0f);
            const float32x4_t g251 = vdupq_n_f32(251.0f / 255.0f), b209 = vdupq_n_f32(209.0f / 255.0f), half = vdupq_n_f32(0.5f);
            const float32x4_t cr = vdupq_n_f32(float(colour & 255)), cg = vdupq_n_f32(float((colour >> 8) & 255)),
                              cb = vdupq_n_f32(float((colour >> 16) & 255));
            const uint32x4_t alpha = vdupq_n_u32(colour & 0xff000000u);
            const float32x4_t tiny = vdupq_n_f32(1e-18f);   // (length over 1e-9, squared)
            const int rot = p.rotation & 3;
            for (; v0 + 4 <= vc; v0 += 4) {
                const float32x4x3_t m = vld3q_f32(&pos[v0].x);
                const float32x4_t mx = vmulq_f32(vnegq_f32(m.val[0]), v3);   // (kMirror)
                const float32x4_t mz = vmulq_f32(m.val[2], v3);
                const float32x4_t lx = vsubq_f32(vaddq_f32(vx, vmulq_f32(mx, vc4)), vmulq_f32(mz, vs4));
                const float32x4_t ly = vaddq_f32(vaddq_f32(vy, vmulq_f32(mx, vs4)), vmulq_f32(mz, vc4));
                const float32x4_t lz = vaddq_f32(vz, vmulq_f32(m.val[1], v3));
                float32x4_t ax, ay, h;   // Projector::box's and depth's sums for this rotation
                switch (rot) {
                case 0: ax = vsubq_f32(lx, ly), ay = vaddq_f32(lx, ly), h = vaddq_f32(lx, ly); break;
                case 1: ax = vsubq_f32(vnegq_f32(lx), ly), ay = vsubq_f32(lx, ly), h = vsubq_f32(lx, ly); break;
                case 2: ax = vaddq_f32(vnegq_f32(lx), ly), ay = vsubq_f32(vnegq_f32(lx), ly), h = vsubq_f32(vnegq_f32(lx), ly); break;
                default: ax = vaddq_f32(lx, ly), ay = vaddq_f32(vnegq_f32(lx), ly), h = vaddq_f32(vnegq_f32(lx), ly); break;
                }
                float32x4_t sx4 = vmulq_f32(ax, hw), sy4 = vmulq_f32(ay, hh);
                sy4 = vsubq_f32(sy4, vmulq_f32(vmulq_f32(lz, up), c30));
                sx4 = vaddq_f32(vaddq_f32(sx4, offx0), cadge);
                sy4 = vaddq_f32(vaddq_f32(sy4, offy0), base);
                vst1q_f32(&sx[v0], sx4);
                vst1q_f32(&sy[v0], sy4);
                vst1q_f32(&depth[v0], vaddq_f32(vmulq_f32(h, kdiag), vmulq_f32(lz, kheight)));
                // The normal in lot space (z up), made unit length.
                const float32x4x3_t n = vld3q_f32(&nrm[v0].x);
                const float32x4_t nx = vnegq_f32(n.val[0]), nz = n.val[2];   // (kMirror)
                const float32x4_t ux = vsubq_f32(vmulq_f32(nx, vc4), vmulq_f32(nz, vs4));
                const float32x4_t uy = vaddq_f32(vmulq_f32(nx, vs4), vmulq_f32(nz, vc4));
                const float32x4_t uz = n.val[1];
                const float32x4_t len2 = vaddq_f32(vaddq_f32(vmulq_f32(ux, ux), vmulq_f32(uy, uy)), vmulq_f32(uz, uz));
                float32x4_t r = vrsqrteq_f32(len2);
                r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(len2, r), r));
                r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(len2, r), r));
                const uint32x4_t long_enough = vcgtq_f32(len2, tiny);
                r = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(r), long_enough));   // (none: the zero vector)
                const float32x4_t nlx = vmulq_f32(ux, r), nly = vmulq_f32(uy, r), nlz = vmulq_f32(uz, r);
                auto dot = [&](const float32x4_t &dx, const float32x4_t &dy, const float32x4_t &dz) {
                    return vaddq_f32(vaddq_f32(vmulq_f32(nlx, dx), vmulq_f32(nly, dy)), vmulq_f32(nlz, dz));
                };
                const float32x4_t k = vaddq_f32(lev, vmulq_f32(lev, vmaxq_f32(zero, dot(lx4, ly4, lz4))));
                float32x4_t lamp = zero;
                for (int j = 0; j < lamps; j++)
                    lamp = vaddq_f32(lamp, vmulq_f32(vdupq_n_f32(lamp_strength[j]),
                                                     vmaxq_f32(zero, dot(vdupq_n_f32(lamp_dir[j].x), vdupq_n_f32(lamp_dir[j].y),
                                                                         vdupq_n_f32(lamp_dir[j].z)))));
                const float32x4_t kr = vminq_f32(one, vaddq_f32(k, lamp));
                const float32x4_t kg = vminq_f32(one, vaddq_f32(k, vmulq_f32(lamp, g251)));
                const float32x4_t kb = vminq_f32(one, vaddq_f32(k, vmulq_f32(lamp, b209)));
                const uint32x4_t tr = vcvtq_u32_f32(vaddq_f32(vmulq_f32(kr, cr), half));
                const uint32x4_t tg = vcvtq_u32_f32(vaddq_f32(vmulq_f32(kg, cg), half));
                const uint32x4_t tb = vcvtq_u32_f32(vaddq_f32(vmulq_f32(kb, cb), half));
                const uint32x4_t t4 = vorrq_u32(vorrq_u32(alpha, vshlq_n_u32(tb, 16)), vorrq_u32(vshlq_n_u32(tg, 8), tr));
                vst1q_u32(&tint[v0], t4);
            }
        }
#endif
        for (size_t v = v0; v < vc; v++) {
            float lx, ly, lz;
            to_lot(pos[v], lx, ly, lz);
            p.screen(lx, ly, lz, sx[v], sy[v]);
            depth[v] = p.depth(lx, ly, lz);
            // The normal in lot space (z up).
            const Vec3 nm = tilt(nrm[v]);
            const float nx = (kMirror ? -nm.x : nm.x), nz = nm.z;
            const Vec3 nl = Vec3{ nx * c - nz * s, nx * s + nz * c, nm.y }.normalized();
            const float k = old_light ? std::clamp(0.58f + 0.55f * std::max(0.0f, nl.dot(light)), 0.0f, 1.0f)
                                      : level + level * std::max(0.0f, nl.dot(light));
            float lamp = 0;
            for (int j = 0; j < lamps; j++)
                lamp += lamp_strength[j] * std::max(0.0f, nl.dot(lamp_dir[j]));
            const float kr = std::min(1.0f, k + lamp), kg = std::min(1.0f, k + lamp * (251.0f / 255.0f)),
                        kb = std::min(1.0f, k + lamp * (209.0f / 255.0f));
            auto channel = [&](int shift) {
                const float kk = shift == 0 ? kr : shift == 8 ? kg : kb;   // 0xAABBGGRR
                return uint32_t(kk * float((colour >> shift) & 255) + 0.5f) << shift;
            };
            tint[v] = (colour & 0xff000000u) | channel(16) | channel(8) | channel(0);
        }
        project_us += pose_us(t_part);
        t_part = FastClock::now();
        for (size_t f = 0; f + 2 < mesh->indices.size(); f += 3) {
            const uint16_t a = mesh->indices[f], b = mesh->indices[f + 1], cc = mesh->indices[f + 2];
            const float cross = (sx[b] - sx[a]) * (sy[cc] - sy[a]) - (sx[cc] - sx[a]) * (sy[b] - sy[a]);
            if ((cross > 0) != kKeepClockwise)
                continue;   // facing away
            Tri t;
            const uint16_t idx[3] = { a, b, cc };
            for (int k = 0; k < 3; k++) {
                t.px[k] = sx[idx[k]];
                t.py[k] = sy[idx[k]];
                t.u[k] = mesh->uvs[size_t(idx[k]) * 2] * float(tex->width);
                t.v[k] = mesh->uvs[size_t(idx[k]) * 2 + 1] * float(tex->height);
                t.tint[k] = tint[idx[k]];
                t.dv[k] = depth[idx[k]];
            }
            t.depth = (depth[a] + depth[b] + depth[cc]) / 3;
            t.texture = tex;
            tris.push_back(t);
        }
        tris_us += pose_us(t_part);
        meshes++;
    }
    g_pose_us[2] += skin_us;
    g_pose_us[3] += project_us;
    g_pose_us[4] += tris_us;
    t_part = FastClock::now();
    // Sorted back to front all the same (by index: the same stable order): the software
    // renderer draws in this order, and the see-through parts of a ghost blend over what
    // is behind them.
    // (Sorted as (depth, index) pairs: the same order a stable sort of the indices by depth gives,
    // without its buffer; a stable sort of ~390 indices was 0.45 ms of a 3.3 ms pose on the Vita.)
    thread_local std::vector<std::pair<float, uint32_t>> keyed;
    keyed.resize(tris.size());
    for (size_t i = 0; i < keyed.size(); i++)
        keyed[i] = { tris[i].depth, uint32_t(i) };
    std::sort(keyed.begin(), keyed.end());
    thread_local std::vector<uint32_t> order;
    order.resize(tris.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = keyed[i].second;
    g_pose_us[5] += pose_us(t_part);
    t_part = FastClock::now();
    if (mesh_out) {   // kept together: one draw for the lot's frame
        if (size_t(pelvis_bone) < world.size()) {
            to_lot(world[size_t(pelvis_bone)].translation, mesh_out->ax, mesh_out->ay, mesh_out->az);
            mesh_out->has_anchor = true;
        }
        mesh_out->parts_found = 0;
        mesh_out->tile_w = 2 * p.m.half_w;
        for (size_t i = 0; censor_bones && i < censor_bones->size() && i < 7; i++) {
            const int bone = (*censor_bones)[i];
            if (bone < 0 || size_t(bone) >= world.size())
                continue;
            float lx, ly, lz;
            to_lot(world[size_t(bone)].translation, lx, ly, lz);
            p.screen(lx, ly, lz, mesh_out->part_x[i], mesh_out->part_y[i]);
            mesh_out->part_d[i] = p.depth(lx, ly, lz);
            mesh_out->part_lx[i] = lx, mesh_out->part_ly[i] = ly, mesh_out->part_lz[i] = lz;
            mesh_out->parts_found |= uint8_t(1u << i);
        }
        g_copy_us[0] += pose_us(t_part);
        FastClock::time_point t_copy = FastClock::now();
        mesh_out->tris.clear();
        mesh_out->tris.reserve(order.size());
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (const uint32_t i : order) {
            const Tri &t = tris[i];
            MeshTri m;
            for (int k = 0; k < 3; k++) {
                m.px[k] = t.px[k], m.py[k] = t.py[k], m.u[k] = t.u[k], m.v[k] = t.v[k], m.dv[k] = t.dv[k];
                m.tint[k] = t.tint[k];
                x0 = std::min(x0, t.px[k]), y0 = std::min(y0, t.py[k]), x1 = std::max(x1, t.px[k]), y1 = std::max(y1, t.py[k]);
            }
            m.texture = t.texture;
            mesh_out->tris.push_back(m);
        }
        g_copy_us[1] += pose_us(t_copy);
        t_copy = FastClock::now();
        if (++mesh_out->pose_stamp == 0)   // (a new pose; never 0, which means no vertices kept)
            mesh_out->pose_stamp = 1;
        {   // (summed in the order the viewer's sort summed them, so a Sim's mean is the same number)
            float sum = 0;
            for (const MeshTri &m : mesh_out->tris)
                sum += (m.dv[0] + m.dv[1] + m.dv[2]) / 3;
            mesh_out->depth_sum = sum;
            mesh_out->depth_count = uint32_t(mesh_out->tris.size());
            mesh_out->depth_for = mesh_out->pose_stamp;
        }
        // Its vertices as the GL renderer takes them (DrawMesh::gpu), made here on the posing
        // thread rather than by the depth pass on the frame's thread (the same work there was ~1.2
        // ms a Sim a frame on the Vita, 2026-11-07): back to front for the depth pass, and by
        // texture, in the order the textures first come, for the colour pass.
        {
            DrawMesh::Gpu &g = mesh_out->gpu;
            const std::vector<MeshTri> &mt = mesh_out->tris;
            g.depth.clear();
            g.colour.clear();
            g.buckets.clear();
            auto vertex = [](const MeshTri &m, int i) {
                const float iw = 1.0f / float(m.texture->width), ih = 1.0f / float(m.texture->height);
                const float zc = std::clamp(-m.dv[i] / kDepthRange, -1.0f, 1.0f);
                return GpuVertex{ m.px[i], m.py[i], zc, m.u[i] * iw, m.v[i] * ih, m.tint[i] };
            };
            g.depth.reserve(mt.size() * 3);
            for (const MeshTri &m : mt)
                for (int i = 0; i < 3; i++)
                    g.depth.push_back(vertex(m, i));
            g_copy_us[2] += pose_us(t_copy);
            t_copy = FastClock::now();
            for (const MeshTri &m : mt) {
                size_t b = 0;
                while (b < g.buckets.size() && g.buckets[b].first != m.texture)
                    b++;
                if (b == g.buckets.size())
                    g.buckets.push_back({ m.texture, 0 });
                g.buckets[b].second += 3;
            }
            thread_local std::vector<uint32_t> at;
            at.assign(g.buckets.size(), 0);
            for (size_t b = 1; b < g.buckets.size(); b++)
                at[b] = at[b - 1] + g.buckets[b - 1].second;
            g.colour.resize(mt.size() * 3);
            for (size_t i = 0; i < mt.size(); i++) {   // (each vertex made once, above: copied into its texture's run)
                size_t b = 0;
                while (g.buckets[b].first != mt[i].texture)
                    b++;
                for (int k = 0; k < 3; k++)
                    g.colour[at[b]++] = g.depth[i * 3 + size_t(k)];
            }
            g.built_for = mesh_out->pose_stamp;
            g_copy_us[3] += pose_us(t_copy);
        }
        if (mesh_out->tris.empty())
            x0 = y0 = x1 = y1 = 0;
        // As a triangle draw's bounds: whole pixels, one past the far edge.
        mesh_out->x0 = std::floor(x0);
        mesh_out->y0 = std::floor(y0);
        mesh_out->x1 = std::ceil(x1) + 1;
        mesh_out->y1 = std::ceil(y1) + 1;
        g_pose_us[6] += pose_us(t_part);
        g_pose_count++;
        g_pose_triangles += order.size();
        return meshes > 0;
    }
    out->cmds.reserve(out->cmds.size() + order.size());
    for (const uint32_t i : order) {
        const Tri &t = tris[i];
        DrawCmd &cmd = out->triangle(*t.texture, t.px, t.py, t.u, t.v, t.tint);
        cmd.depth = DepthMode::Own;
        for (int k = 0; k < 3; k++)
            cmd.dv[k] = t.dv[k];
    }
    return meshes > 0;
}
} // namespace

bool draw_sim_mesh(DrawMesh &out, const Projector &p, CharacterLibrary &library, const SimLook &look, float x, float y,
                   float z, float direction, const SimPose &pose, std::string &problem, uint32_t colour, PoseLookups *lookups)
{
    return draw_sim_turned(nullptr, &out, p, library, look, x, y, z, std::fmod(direction, 8.0f), 0, pose, problem, colour, nullptr,
                           lookups);
}

bool draw_sim(DrawList &out, const Projector &p, CharacterLibrary &library, const SimLook &look, float x, float y,
              float z, float direction, const SimPose &pose, std::string &problem, uint32_t colour)
{
    return draw_sim_turned(&out, nullptr, p, library, look, x, y, z, std::fmod(direction, 8.0f), 0, pose, problem, colour);
}

bool draw_sim_portrait(DrawList &out, CharacterLibrary &library, const SimLook &look, float cx, float cy, float scale,
                       float facing, float pitch, const SimPose &pose, bool head_only, std::string &problem,
                       const char *mesh_filter, const SimPose *anchor)
{
    Projector p{ metrics(Zoom::Near), 0 };
    p.m.half_w *= scale;
    p.m.half_h *= scale;
    p.m.one_unit *= scale;
    SimLook part = look;
    if (head_only) {
        if (look.parts.size() < 2)
            return false;
        part.parts = { look.parts[1] };   // body, head, hands
    }
    DrawList own;
    if (!draw_sim_turned(&own, nullptr, p, library, part, 0, 0, 0, facing, pitch, pose, problem, 0xffffffffu, mesh_filter) ||
        own.cmds.empty())
        return false;
    // Placed by its outline's middle: this pose's, or with `anchor` that pose's (Create-A-Sim: the
    // standing one, so a raised arm or a hop moves the arm or the Sim, not the whole figure the other
    // way: placed by each frame's own outline the wave sank the body through the window's floor).
    DrawList still;
    std::string anchor_problem;
    const bool anchored = anchor && draw_sim_turned(&still, nullptr, p, library, part, 0, 0, 0, facing, pitch, *anchor, anchor_problem,
                                                    0xffffffffu, mesh_filter) && !still.cmds.empty();
    float l = 1e9f, r = -1e9f, t = 1e9f, b = -1e9f;
    for (const DrawCmd &c : (anchored ? still : own).cmds)
        for (int k = 0; k < 3; k++) {
            l = std::min(l, c.px[k]);
            r = std::max(r, c.px[k]);
            t = std::min(t, c.py[k]);
            b = std::max(b, c.py[k]);
        }
    const float dx = cx - (l + r) / 2, dy = cy - (t + b) / 2;
    for (DrawCmd &c : own.cmds) {
        for (int k = 0; k < 3; k++) {
            c.px[k] += dx;
            c.py[k] += dy;
        }
        out.cmds.push_back(c);
    }
    return true;
}

FaceFraming &face_framing()
{
    static FaceFraming f = [] {
        FaceFraming old;
        if (TS_ENV("TS_FACE_FRAME_OLD")) {   // as before 2026-11-12
            old.facing = 2.8f, old.pitch = 0.55f, old.fill = 1.08f, old.bottom = 0.97f, old.shift = 0.0f;
        }
        return old;
    }();
    return f;
}

const FaceFraming &relation_framing()
{
    static const FaceFraming f = [] {
        FaceFraming r;
        r.facing = 3.4f, r.pitch = 0.12f, r.fill = 1.19f, r.bottom = 1.035f, r.shift = 0.05f, r.by_height = true;
        return r;
    }();
    return f;
}

void tone_like_portraits(Image &img)
{
    static const bool off = TS_ENV("TS_FACE_TONE_OFF") != nullptr;
    if (off)
        return;
    static const float kA[3] = { 0.815f, 0.831f, 0.845f }, kB[3] = { 16.4f, 12.0f, 3.2f };
    for (uint32_t &p : img.rgba) {
        if (!(p >> 24))
            continue;
        uint32_t q = p & 0xff000000u;
        for (int ch = 0; ch < 3; ch++) {
            const float c = float((p >> (8 * ch)) & 0xffu) * kA[ch] + kB[ch];
            q |= uint32_t(std::clamp(c, 0.0f, 255.0f) + 0.5f) << (8 * ch);
        }
        p = q;
    }
}

bool draw_sim_face(DrawList &out, CharacterLibrary &library, const SimLook &look, int x, int y, int size,
                   std::string &problem, const FaceFraming *framing)
{
    const FaceFraming &fr = framing ? *framing : face_framing();
    // The head, front on, drawn large, then sized to the square.
    DrawList head;
    // Level and straight on, as the portraits in the game's character files: the camera looks down
    // on the lot, so the head is turned up to meet it (drawn plain it looked at its feet, the
    // 2026-11-02 report; turn and pitch found side by side with Mortimer's and Bella's own).
    if (!draw_sim_portrait(head, library, look, 0, 0, 4.0f, fr.facing, fr.pitch, SimPose{}, true, problem) || head.cmds.empty())
        return false;
    float l = 1e9f, r = -1e9f, t = 1e9f, b = -1e9f;
    for (const DrawCmd &c : head.cmds)
        for (int k = 0; k < 3; k++) {
            l = std::min(l, c.px[k]);
            r = std::max(r, c.px[k]);
            t = std::min(t, c.py[k]);
            b = std::max(b, c.py[k]);
        }
    // Close, as the game's: the jaw just inside the bottom of the square, the hair running off the top.
    const float kh = float(size) * fr.fill / std::max(1.0f, b - t);
    const float k = fr.by_height ? kh : std::min(kh, float(size) * 1.0f / std::max(1.0f, r - l));
    const float ox = float(x) + float(size) * (0.5f + fr.shift) - (l + r) / 2 * k;
    const float oy = float(y) + float(size) * fr.bottom - b * k;
    // Each triangle cut to the square (Sutherland-Hodgman), its corners' texture
    // places, tints and depths carried along, then fanned back into triangles.
    struct V {
        float x, y, u, v, d;
        float tint[4];
    };
    const float edge[4] = { float(x), float(y), float(x + size), float(y + size) };
    for (const DrawCmd &c : head.cmds) {
        std::vector<V> poly;
        for (int i = 0; i < 3; i++) {
            V p{ ox + c.px[i] * k, oy + c.py[i] * k, c.u[i], c.v[i], c.dv[i], {} };
            for (int ch = 0; ch < 4; ch++)
                p.tint[ch] = float((c.tint[i] >> (8 * ch)) & 0xff);
            poly.push_back(p);
        }
        for (int side = 0; side < 4 && !poly.empty(); side++) {
            auto inside = [&](const V &p) {
                switch (side) {
                case 0: return p.x >= edge[0];
                case 1: return p.y >= edge[1];
                case 2: return p.x <= edge[2];
                default: return p.y <= edge[3];
                }
            };
            auto cut = [&](const V &a, const V &bb) {
                const float s = side == 0 || side == 2 ? (edge[side] - a.x) / (bb.x - a.x) : (edge[side] - a.y) / (bb.y - a.y);
                V m;
                m.x = a.x + (bb.x - a.x) * s;
                m.y = a.y + (bb.y - a.y) * s;
                m.u = a.u + (bb.u - a.u) * s;
                m.v = a.v + (bb.v - a.v) * s;
                m.d = a.d + (bb.d - a.d) * s;
                for (int ch = 0; ch < 4; ch++)
                    m.tint[ch] = a.tint[ch] + (bb.tint[ch] - a.tint[ch]) * s;
                return m;
            };
            std::vector<V> next;
            for (size_t i = 0; i < poly.size(); i++) {
                const V &a = poly[i], &bb = poly[(i + 1) % poly.size()];
                if (inside(a)) {
                    next.push_back(a);
                    if (!inside(bb))
                        next.push_back(cut(a, bb));
                } else if (inside(bb)) {
                    next.push_back(cut(a, bb));
                }
            }
            poly = std::move(next);
        }
        for (size_t i = 1; i + 1 < poly.size(); i++) {
            const V *corner[3] = { &poly[0], &poly[i], &poly[i + 1] };
            float px[3], py[3], u[3], v[3];
            uint32_t tint[3];
            for (int j = 0; j < 3; j++) {
                px[j] = corner[j]->x;
                py[j] = corner[j]->y;
                u[j] = corner[j]->u;
                v[j] = corner[j]->v;
                tint[j] = 0;
                for (int ch = 0; ch < 4; ch++)
                    tint[j] |= uint32_t(std::clamp(corner[j]->tint[ch], 0.0f, 255.0f) + 0.5f) << (8 * ch);
            }
            DrawCmd &d = out.triangle(*c.image, px, py, u, v, tint);
            d.depth = c.depth;
            for (int j = 0; j < 3; j++)
                d.dv[j] = corner[j]->d;
        }
    }
    return true;
}

} // namespace ts
