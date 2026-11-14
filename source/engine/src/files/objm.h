/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSO (tso.files/Formats/IFF/Chunks/OBJM.cs, IffFieldEncode.cs).
 */
// ObjM: the saved state of every object on a Sims 1 lot (its attributes,
// object data, thread, relationships, slots and, for people, their needs,
// outfits and person data).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ts {

struct ObjmInstance {
    int x = 0, y = 0, level = 0;
    std::vector<int16_t> attributes;
    int16_t temps[8] = {};
    int16_t data[68] = {};
    int16_t data_extra[5] = {};
    struct Frame {
        int16_t stack_object = 0, tree = 0, node = 0;
        std::vector<int16_t> params, locals;
        int32_t primitive_state = 0;
        int16_t code_owner_type = 0;
    };
    std::vector<Frame> stack;
    struct Relationship {
        int32_t target = 0;
        std::vector<int32_t> values;
    };
    std::vector<Relationship> relationships;
    std::vector<std::pair<int16_t, int16_t>> slots;   // the first is the slot it's linked into
    std::vector<int16_t> dynamic_sprite_flags;        // entry i not zero: dynamic sprite i shows (FreeSO)
    struct Person {
        std::string body, body_tex, left_hand, left_hand_tex, right_hand, right_hand_tex, head, head_tex;
        std::string animation, carry_animation, base_animation;
        float motives[16] = {};
        std::vector<int16_t> person_data;
        struct MotiveDelta {
            int32_t motive = 0;
            float tick_delta = 0, stop_at = 0;
        };
        std::vector<MotiveDelta> motive_deltas;
    };
    bool person = false;
    Person sim;
};

struct ObjmFull {
    uint32_t version = 0;
    std::vector<std::pair<uint16_t, uint16_t>> id_to_type;   // instance id -> objt entry (1-based)
    // Each instance's record, undecoded, and whether it starts at an odd offset.
    struct Record {
        std::vector<uint8_t> bytes;
        bool odd = false;
    };
    std::vector<Record> records;
    // After the records (a last link of 0): the chunk's trailer, its bytes as they are (the exe's
    // FUN_0052d5f0: an s16 object id, an s32 count and that many s16 ids, a string), and where it
    // starts from the chunk's base (for its odd/even flag).
    std::vector<uint8_t> trailer;
    size_t trailer_at = 0;
};

// Splits the chunk into its records (no decoding yet).
bool parse_objm_records(const uint8_t *data, size_t size, ObjmFull &out, std::string &error);

// One field of a record as it was read, in order: what writing it back needs (the PC <-> Vita
// family files, docs/ROADMAP.md stage 2). `kind`: 'b' a byte field, 'h' a 16-bit one, 'w' a
// 32-bit one (floats as their bits), 's' a string another string follows, 't' a string fields follow.
struct ObjmToken {
    char kind = 'h';
    int64_t value = 0;
    std::string text;
};

// Decodes one record. `objd_type` is the object's OBJD type (2 person, 8 portal),
// `multitile` whether it is a part of a multi-tile object. `tokens`: every field read, in order.
// `guid`: the object's type (the scheduler word, which every record of version > 0x35 ends with,
// is not written for a few: the exe's FUN_0052d5f0), 0 when not known.
bool decode_objm_instance(const ObjmFull &objm, const ObjmFull::Record &rec, int objd_type, bool multitile,
                          ObjmInstance &out, std::string &error, std::vector<ObjmToken> *tokens = nullptr, uint32_t guid = 0);
// The trailer's fields (an s16, an s32 count and that many s16, a string), as tokens.
bool decode_objm_trailer(const ObjmFull &objm, std::vector<ObjmToken> &tokens);

// A record's bytes from its fields, the reader above run backwards: each field at the narrowest
// of its four widths (absent when 0), strings byte-aligned and padded as the reader expects them.
// `odd`: the record starts at an odd offset from the chunk's base (ObjmFull::Record::odd).
std::vector<uint8_t> encode_objm_record(const std::vector<ObjmToken> &tokens, bool odd);
// A whole ObjM chunk's body: its header (`pad`, `version`, "MjbO", compressed), the id table and
// each record with the offset of its end.
// `trailer`: its fields (decode_objm_trailer's; empty: the plain one, nobody selected, no ids, no string).
std::vector<uint8_t> write_objm(uint32_t pad, uint32_t version, const std::vector<std::pair<uint16_t, uint16_t>> &id_to_type,
                                const std::vector<std::vector<ObjmToken>> &records, const std::vector<ObjmToken> &trailer = {});

} // namespace ts
