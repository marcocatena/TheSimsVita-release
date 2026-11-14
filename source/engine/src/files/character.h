/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Structures follow FreeSims (sims.common/rendering/vitaboy: Skeleton.cs,
 * Appearance.cs, Mesh.cs, Animation.cs, CFP.cs); the binary layouts were
 * checked against every file in the install (see tools/scan_install.cpp).
 */

// The people's 3D files. Character files (.cmx text, .bcf binary) hold
// skeletons, suits (which mesh goes on which bone) and animations; meshes
// (.skn text, .bmf binary) are skinned to bones; animation keyframes live
// in separate .cfp files.
//
// Coordinates are converted as FreeSims does (x mirrored, quaternions
// conjugated), so that 3 units are one tile and y is up.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math3d.h"

namespace ts {

struct CharBone {
    std::string name, parent;
    std::vector<std::pair<std::string, std::string>> properties;
    Vec3 translation;
    Quat rotation;
    bool can_translate = true, can_rotate = true, can_blend = false;
    float wiggle_value = 0, wiggle_power = 0;
};

struct CharSkeleton {
    std::string name;
    std::vector<CharBone> bones;
    int find(const std::string &bone) const;   // case-insensitive; -1 if absent
};

struct SuitBinding {
    std::string bone, mesh;
};

struct Suit {
    std::string name;
    uint32_t type = 0;
    std::vector<SuitBinding> bindings;
};

struct AnimMotion {
    std::string bone;
    uint32_t frames = 0;
    float duration = 0;
    bool has_translation = false, has_rotation = false;
    uint32_t first_translation = 0, first_rotation = 0;
};

// A timed marker in an animation: "xevt" (an event for the code that plays
// it), "righthand"/"lefthand" (gestures), "sound", ...
struct AnimEvent {
    uint32_t time_ms = 0;
    std::vector<std::pair<std::string, std::string>> properties;

    const std::string *get(const char *key) const;
};

struct CharAnimation {
    std::string name, xskill;   // keyframes are in <xskill>.cfp
    float duration = 0;         // milliseconds
    float distance = 0;
    bool moving = false;
    uint32_t translation_count = 0, rotation_count = 0;
    std::vector<AnimMotion> motions;
    std::vector<AnimEvent> events;    // of every motion, by time
    std::vector<Vec3> translations;   // filled from the .cfp
    std::vector<Quat> rotations;
};

struct CharacterFile {
    std::vector<CharSkeleton> skeletons;
    std::vector<Suit> suits;
    std::vector<CharAnimation> animations;
};

bool parse_bcf(const uint8_t *data, size_t size, CharacterFile &out, std::string &error);
bool parse_cmx(const uint8_t *data, size_t size, CharacterFile &out, std::string &error);   // the text form

// Fills an animation's keyframes from its .cfp file.
bool decode_cfp(const uint8_t *data, size_t size, CharAnimation &anim, std::string &error);

struct MeshBoneBinding {
    int bone = 0;   // index into CharMesh::bones
    int first_vertex = 0, vertex_count = 0;
    int first_blend = 0, blend_count = 0;
};

struct MeshBlend {
    float weight = 0;   // 0-1, of the blend vertex
    int vertex = 0;     // the real vertex it pulls on
};

// A skinned mesh: each real vertex belongs to one bone (in that bone's space);
// blend vertices, positioned in another bone's space, pull some real vertices
// towards them by a weight.
struct CharMesh {
    std::string name, texture;   // texture: default, overridden by the Sim's body string
    std::vector<std::string> bones;
    std::vector<uint16_t> indices;   // triangles
    std::vector<MeshBoneBinding> bindings;
    std::vector<float> uvs;          // 2 per real vertex
    std::vector<MeshBlend> blends;
    std::vector<Vec3> positions, normals;               // real vertices
    std::vector<Vec3> blend_positions, blend_normals;   // blend vertices
};

bool parse_bmf(const uint8_t *data, size_t size, CharMesh &out, std::string &error);
bool parse_skn(const uint8_t *data, size_t size, CharMesh &out, std::string &error);   // the text form

} // namespace ts
