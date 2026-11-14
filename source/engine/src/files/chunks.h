/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (sims.files/formats/iff/chunks).
 */

// Decoders for the IFF chunk types the engine needs. Each parse_* takes a
// chunk body and returns false (with `error` set) if it is malformed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ts {

// STR#, CTSS, TTAs and other string tables. Strings keep the file's bytes
// (Windows-1252 in practice).
struct StringItem {
    uint8_t language = 1;   // 1 = English (US); see STRLangCode in FreeSims
    std::string value;
    std::string comment;
};

struct StringTable {
    int16_t format = 0;
    std::vector<StringItem> items;   // every language, in file order
    // English (US)'s places in items, made as it's read (parse_str): get() was a walk of the list,
    // asked for every action's name every time free will looked at it.
    std::vector<uint32_t> english;
    bool indexed = false;

    // The index-th string of a language, falling back to English (US).
    const std::string *get(size_t index, uint8_t language = 1) const;
};

bool parse_str(const uint8_t *data, size_t size, StringTable &out, std::string &error);
// A string chunk type (STR#, CTSS, TTAs, FAMs: all in the STR# layout).
bool is_string_chunk(const char *type);
// The chunk with English (US) alone, in the -3 layout: the game's string chunks carry every
// language (format -3 a language byte a string, -4 a set a language) and only English is read
// (StringTable::get): half to nine tenths of a file's strings held for nothing, 8 MB of the
// Vita's region at the Goths (2026-11-05). False when nothing can go (another format, or English
// is all there is); the strings' order and indices within English are unchanged.
bool shrink_str_chunk(const uint8_t *data, size_t size, std::vector<uint8_t> &out);

// PALT: up to 256 RGB colours.
struct Palette {
    std::vector<uint32_t> colors;   // 0xAABBGGRR, alpha 255
};

bool parse_palt(const uint8_t *data, size_t size, Palette &out, std::string &error);

// A decoded sprite frame: RGBA pixels plus the per-pixel depth the renderer
// uses to sort sprites into the scene (255 = farthest / empty).
struct Image {
    int width = 0, height = 0;
    std::vector<uint32_t> rgba;    // 0xAABBGGRR
    std::vector<uint8_t> depth;    // empty if the frame has no z-buffer
    // Whether its pixels are here: a trim lets go of a cached picture's pixels and keeps the Image
    // (its address is what draw lists hold); one without draws nothing.
    bool has_pixels() const { return width > 0 && height > 0 && rgba.size() >= size_t(width) * size_t(height); }
};

// SPR2: object sprites with z-buffer and alpha channels.
struct Spr2Frame {
    uint16_t width = 0, height = 0;
    uint32_t flags = 0;            // 1 = pixels, 2 = z-buffer, 4 = alpha
    uint16_t palette_id = 0;
    bool keyed = false;   // a water tile (floors.iff 0x800-0x80f): its transparent index is transparent in every mode
    uint16_t transparent_index = 0;
    int16_t x = 0, y = 0;
    const uint8_t *rle = nullptr;  // encoded rows, inside the chunk body
    size_t rle_size = 0;
};

struct Spr2 {
    uint32_t version = 0;
    uint32_t default_palette = 0;
    std::vector<Spr2Frame> frames;
};

bool parse_spr2(const uint8_t *data, size_t size, Spr2 &out, std::string &error);
bool decode_spr2_frame(const Spr2Frame &frame, const Palette *palette, Image &out, std::string &error);

// SPR#: older sprites (walls, floors, UI), palette only.
struct SprFrame {
    uint16_t width = 0, height = 0;
    bool big_endian = false;
    const uint8_t *rle = nullptr;
    size_t rle_size = 0;
};

struct Spr {
    uint32_t version = 0;
    uint16_t palette_id = 0;
    std::vector<SprFrame> frames;
};

bool parse_spr(const uint8_t *data, size_t size, Spr &out, std::string &error);
bool decode_spr_frame(const SprFrame &frame, const Palette *palette, Image &out, std::string &error);

// DGRP: which sprites draw an object for each direction and zoom.
struct DrawSprite {
    uint32_t sprite_id = 0;
    uint32_t frame = 0;
    uint32_t flags = 0;            // 1 = flip horizontally
    int32_t offset_x = 0, offset_y = 0;
    float object_x = 0, object_y = 0, object_z = 0;
};

struct DrawImage {
    uint32_t direction = 0;        // 0x01, 0x04, 0x10 or 0x40
    uint32_t zoom = 0;             // 1 = closest, 3 = farthest
    std::vector<DrawSprite> sprites;
};

struct DrawGroup {
    uint16_t version = 0;
    std::vector<DrawImage> images;
};

bool parse_dgrp(const uint8_t *data, size_t size, DrawGroup &out, std::string &error);

// BHAV: one SimAntics routine.
struct BhavInstruction {
    uint16_t opcode = 0;
    uint8_t true_pointer = 0, false_pointer = 0;
    uint8_t operand[8] = {};
};

struct Bhav {
    uint16_t version = 0;
    uint8_t type = 0, args = 0;
    uint16_t locals = 0, flags = 0;
    std::vector<BhavInstruction> instructions;
};

bool parse_bhav(const uint8_t *data, size_t size, Bhav &out, std::string &error);

// BCON: constants that BHAVs read.
struct Bcon {
    uint8_t flags = 0;
    std::vector<uint16_t> constants;
};

bool parse_bcon(const uint8_t *data, size_t size, Bcon &out, std::string &error);

// GLOB: name of the semi-global IFF an object file shares code from.
bool parse_glob(const uint8_t *data, size_t size, std::string &out, std::string &error);

// OBJf: per-object function table (condition/action BHAV pairs).
struct ObjFunction {
    uint16_t condition = 0, action = 0;
};

bool parse_objf(const uint8_t *data, size_t size, std::vector<ObjFunction> &out, std::string &error);

// OBJD: an object definition, a versioned list of 16-bit fields.
struct Objd {
    // Field positions, the same in every version (FreeSims VERSION_142_Fields).
    enum Field {
        StackSize, BaseGraphicID, NumGraphics, BhavMain, BhavGardening, TreeTableID,
        InteractionGroupID, ObjectType, MasterID, SubIndex, BhavWashHands, AnimationTableID,
        GUID1, GUID2, Disabled, BhavPortal, Price, BodyStringID, SlotID, BhavAllowIntersection,
        UsesFnTable, BitField1, BhavPrepareFood, BhavCookFood, BhavPlaceSurface, BhavDispose,
        BhavEat, BhavPickupFromSlot, BhavWashDish, BhavEatSurface, BhavSit, BhavStand,
        SalePrice, InitialDepreciation, DailyDepreciation, SelfDepreciating, DepreciationLimit,
        RoomFlags, FunctionFlags, CatalogStringsID, Global, BhavInit, BhavPlace, BhavUserPickup,
        WallStyle, BhavLoad, BhavUserPlace, ObjectVersion, BhavRoomChange, MotiveEffectsID,
        BhavCleanup, BhavLevelInfo, CatalogID, BhavServingSurface, LevelOffset, Shadow,
        NumAttributes, BhavClean, BhavQueueSkipped, FrontDirection, BhavWallAdjacencyChanged,
        MyLeadObject, DynamicSpriteBaseId, NumDynamicSprites, ChairEntryFlags, TileWidth,
        LotCategories, BuildModeType, OriginalGUID1, OriginalGUID2, SuitGUID1, SuitGUID2,
        BhavPickup, ThumbnailGraphic, ShadowFlags, FootprintMask, BhavDynamicMultiTileUpdate,
        ShadowBrightness, BhavRepair, WallStyleSpriteID, RatingHunger, RatingComfort,
        RatingHygiene, RatingBladder, RatingEnergy, RatingFun, RatingRoom, RatingSkillFlags,
        NumTypeAttributes, MiscFlags, TypeAttrGUID1, TypeAttrGUID2,
        // Sims 1 (version 138, FreeSO VERSION_138b_Extra_Fields): buy mode's subcategories by lot kind.
        FunctionSubsort, DTSubsort, KeepBuying, VacationSubsort, ResetLotAction, CommunitySubsort,
        DreamFlags, RenderFlags, VitaboyFlags, STSubsort, MTSubsort,
    };

    uint32_t version = 0;
    std::vector<uint16_t> fields;

    uint16_t get(Field f) const { return size_t(f) < fields.size() ? fields[f] : 0; }
    uint32_t guid() const { return get(GUID1) | uint32_t(get(GUID2)) << 16; }
};

bool parse_objd(const uint8_t *data, size_t size, Objd &out, std::string &error);

// TTAB: an object's interactions (pie menu entries and autonomy data).
struct TtabMotive {
    int16_t min = 0, max = 0;
    uint16_t personality = 0;
};

struct TtabInteraction {
    uint16_t action = 0, test = 0;   // BHAV ids
    uint32_t flags = 0;
    uint32_t tta_index = 0;          // string index into the TTAs chunk
    uint32_t attenuation_code = 0;
    float attenuation_value = 0;
    uint32_t autonomy_threshold = 0;
    int32_t joining_index = 0;
    uint32_t flags2 = 0x1f;
    std::vector<TtabMotive> motives;
};

struct Ttab {
    uint16_t version = 0;
    std::vector<TtabInteraction> interactions;
};

bool parse_ttab(const uint8_t *data, size_t size, Ttab &out, std::string &error);

} // namespace ts
