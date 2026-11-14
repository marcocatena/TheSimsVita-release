/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Screen-space metrics and sprite placement follow FreeSims
 * (SimsVille/World: WorldState.cs, FloorComponent.cs, utils/DGRPRenderer.cs).
 */
#include "core/env.h"
#include "core/memory_kind.h"
#include "core/fast_clock.h"
#include "render/lot_painter.h"

#include "core/work_pool.h"

#include "render/wall_painter.h"
#include "render/depth.h"
#include "render/grass.h"
#include "render/sim_painter.h"
#include "world/object_code.h"
#include "world/roofs.h"
#include "world/rooms.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <unordered_map>

namespace ts {

namespace {

// FreeSims DGRP.GetImage: the object's facing, turned by the camera rotation.
// DGRP numbers its zooms the other way round: 1 is far, 3 is near.
const DrawImage *pick_image(const DrawGroup &g, uint32_t direction_flag, Zoom zoom, int rotation)
{
    uint32_t bits = direction_flag << (rotation * 2);
    uint32_t dir = (bits & 255) | (bits >> 8);
    const uint32_t dgrp_zoom = 4 - uint32_t(zoom);
    for (const DrawImage &img : g.images)
        if (img.direction == dir && img.zoom == dgrp_zoom)
            return &img;
    return nullptr;
}

enum class Cut { None, Left, Right, Top, Bottom };

// One floor sprite, or half of one on a tile split by a diagonal wall.
void draw_floor_sprite(DrawList &out, const Projector &p, Content &content, SpriteCache &sprites, const Lot &lot,
                       uint16_t pattern, int x, int y, int level, int rotation, Cut cut, PaintStats &stats)
{
    int cx, cy;
    const float z = level * kLevelHeight + lot.ground(float(x), float(y));
    p.point(float(x), float(y), z, cx, cy);
    float bx, by;
    p.box(float(x), float(y), z, bx, by);
    // A floor sits a few z-buffer steps behind its own plane, so what lies on it wins the ties: a rug's
    // own z-buffer runs along the floor's plane a step either side, and the floor came through it in
    // a speckle of tiles and grout (the Scraps Ranch Rag Rug in Lisa's kitchen, the user, 2026-11-14).
    // (A higher start is further back; the wall caps sit 2 behind for the same reason. The rug was solid
    // from 2; the puddles' joins, a line of floor between two water tiles (the user's shot, 2026-11-14),
    // closed only from 8.) Not the pool's or a pond's water: swimmers sink into it by its depth.
    // TS_FLOOR_BIAS=<steps> to try others (0: as before).
    static const float floor_bias = [] {
        const char *b = TS_ENV("TS_FLOOR_BIAS");
        return b ? float(std::atof(b)) : 8.0f;
    }();
    const bool water = pattern == kFloorPool || pattern == kFloorWater;
    const DepthPlane depth = floor_depth(back_corner_depth(p, float(x), float(y), z), p.m.zoom_index,
                                         float(int(by) + p.m.floor_y), water ? 255.0f : 255.0f + floor_bias);
    if (pattern == kFloorPool || pattern == kFloorWater) {
        // (The lot's outermost row is past the edge of its ground: the Kats' file has water all along one side.)
        if (x <= 0 || y <= 0 || x >= lot.size - 1 || y >= lot.size - 1)
            return;
        // The pool's picture for a tile depends on which sides have more pool (FreeSims FloorComponent):
        // the eight round it, clockwise from the top right as bits, turned with the camera; the four
        // straight sides pick one of 16 sprites, and a corner piece goes on where two sides meet
        // but the tile between them isn't pool.
        static const int kAround[8][2] = { { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 } };
        enum { TopRight = 1, Right = 2, BottomRight = 4, Bottom = 8, BottomLeft = 16, Left = 32, TopLeft = 64, Top = 128 };
        int around = 0;
        for (int i = 0; i < 8; i++) {
            const int tx = x + kAround[i][0], ty = y + kAround[i][1];
            if (tx <= 0 || tx >= lot.size - 1 || ty <= 0 || ty >= lot.size - 1 || lot.floor(tx, ty, level) == pattern)
                around |= 1 << i;
        }
        const int turn = (rotation & 3) * 2;
        const int adj = ((around << turn) & 255) | (around >> ((8 - turn) % 8));
        const int picture = ((adj & TopRight) ? 1 : 0) | ((adj & TopLeft) ? 2 : 0) | ((adj & BottomLeft) ? 4 : 0) | ((adj & BottomRight) ? 8 : 0);
        static const FileRef *const pool_file = content.file("floors.iff");
        const int zoom = p.m.zoom_index;   // 0 far, 1 medium, 2 near
        const int x0 = int(bx) + p.m.floor_x, y0 = int(by) + p.m.floor_y;
        const Image *tile = !pool_file ? nullptr
                            : pattern == kFloorPool ? sprites.any(*pool_file, uint16_t(0x400 + 0x10 * zoom + picture), 0)
                                                    : sprites.any(*pool_file, uint16_t(0x800 + picture), uint32_t(2 - zoom));
        if (!tile) {
            out.diamond(cx, cy, int(p.m.half_w), int(p.m.half_h), pattern == kFloorPool ? 0xffd0a040u : 0xffc08040u).set_plane(depth);
            stats.floors++;
            return;
        }
        out.sprite(*tile, x0, y0).set_plane(depth);
        stats.floors++;
        if (pattern == kFloorPool) {
            const int w = tile->width, h = tile->height;
            auto corner = [&](int sides, int between, int index, auto place) {
                if ((adj & sides) != sides || (adj & between))
                    return;
                if (const Image *c = sprites.any(*pool_file, uint16_t(0x430 + 4 * zoom + index), 0)) {
                    int px, py;
                    place(*c, px, py);
                    out.sprite(*c, px, py).set_plane(depth);
                }
            };
            corner(TopLeft | TopRight, Top, 0, [&](const Image &c, int &px, int &py) { px = x0 + w / 2 - c.width / 2, py = y0; });
            corner(TopLeft | BottomLeft, Left, 1, [&](const Image &c, int &px, int &py) { px = x0, py = y0 + h / 2 + 1 - c.height / 2; });
            corner(BottomLeft | BottomRight, Bottom, 2, [&](const Image &c, int &px, int &py) { px = x0 + w / 2 - c.width / 2, py = y0 + h - c.height; });
            corner(TopRight | BottomRight, Right, 3, [&](const Image &c, int &px, int &py) { px = x0 + w - c.width, py = y0 + h / 2 + 1 - c.height / 2; });
        }
        return;
    }
    const FileRef *ref = nullptr;
    int sprite_id;
    if (const std::string *name = lot.custom_floor(pattern)) {
        ref = content.file(*name);
        sprite_id = p.m.custom_floor_id;
    } else {
        static const FileRef *const floors_file = content.file("floors.iff");   // (the index never changes)
        ref = floors_file;
        sprite_id = pattern + p.m.floor_base;
    }
    // A pattern's picture found again by its numbers (the sprite cache's lookup keys on the file's
    // name: 1,566 floors at Solana, 100-130 ms of a scene rebuild on the Vita). The floors' pictures
    // stay in the cache through its trims; a clear (another lot) starts the table again.
    static struct {
        unsigned generation = ~0u;
        std::map<std::tuple<const FileRef *, int, int>, const Image *> found;
    } kept;
    if (kept.generation != sprites.generation()) {
        kept.generation = sprites.generation();
        kept.found.clear();
    }
    const Image *found = nullptr;
    if (ref) {
        const auto key = std::make_tuple(ref, sprite_id, int(p.m.zoom_index));
        if (auto it = kept.found.find(key); it != kept.found.end() && (!it->second || it->second->has_pixels()))
            found = it->second;
        else   // (new, or its pixels let go after all: decoded again into the same picture)
            kept.found[key] = found = sprites.any(*ref, uint16_t(sprite_id), 0);
        if (found)
            sprites.pin(found);   // (kept through the trims: see SpriteCache::pin)
    }
    if (!found) {
        if (stats.floors_missing++ < 5)
            stats.notes.push_back("no art for floor " + std::to_string(pattern) + " at " + std::to_string(x) + "," +
                                  std::to_string(y));
        out.diamond(cx, cy, int(p.m.half_w), int(p.m.half_h), 0xffff00ffu).set_plane(depth);
        return;
    }
    const Image &img = *found;
    // One floor image serves every camera angle, mirrored as the camera turns, counted from the way
    // the lot was made facing (its HOUS camera direction). In all 81 of the game's lots the road's
    // lines run with that: floor 10 along x and 11 along y with directions 0 and 2, the other way
    // round with 1 and 3 (the Kats' road drew its dashes crosswise, the 2026-11-02 report).
    const int turned = (rotation - lot.hous.camera_dir) & 3;
    const bool flip_v = turned == 1 || turned == 2, flip_h = turned > 1;
    int sx = 0, sy = 0, sw = img.width, sh = img.height;
    // `cut` is the half wanted on screen; a mirrored picture's that half comes from its other side.
    if (flip_v)
        cut = cut == Cut::Top ? Cut::Bottom : cut == Cut::Bottom ? Cut::Top : cut;
    if (flip_h)
        cut = cut == Cut::Left ? Cut::Right : cut == Cut::Right ? Cut::Left : cut;
    switch (cut) {
    case Cut::Left: sw /= 2; break;
    case Cut::Right: sx = sw / 2; sw -= sx; break;
    case Cut::Top: sh /= 2; break;
    case Cut::Bottom: sy = sh / 2; sh -= sy; break;
    case Cut::None: break;
    }
    out.sprite_part(img, int(bx) + p.m.floor_x, int(by) + p.m.floor_y, sx, sy, sw, sh, flip_h, flip_v)
        .set_plane(depth);
    stats.floors++;
}

constexpr uint16_t kFloorDiagonal = 255;   // split by a diagonal wall; halves are in the wall tile

void draw_floor(DrawList &out, const Projector &p, Content &content, SpriteCache &sprites, const Lot &lot,
                const WallPainter &walls, int x, int y, int level, int rotation, PaintStats &stats)
{
    uint16_t pattern = lot.floor(x, y, level);
    if (pattern == 0)
        return;
    if (pattern == kFloorDiagonal && (lot.wall(x, y, level).segments & (WallHorizontalDiag | WallVerticalDiag))) {
        // The wall tile's top-left pattern and style hold the two halves' floors: the pattern the
        // half at +x (+y on a horizontal diagonal, -y on a vertical), the style the other, whatever
        // the camera (the Mashugas' octagon: 16 tiles with one half floored, inside every time).
        // Which half of the picture that is goes by where that corner falls on screen. (Halves picked
        // by the rotated wall, before the picture's mirroring, put the carpet outside the house.)
        const WallTile &w = lot.wall(x, y, level);
        const bool horizontal = (w.segments & WallHorizontalDiag) != 0;
        int ox, oy, tx, ty;
        p.point(float(x), float(y), 0, ox, oy);
        p.point(float(x + 1), float(y + (horizontal ? 1 : -1)), 0, tx, ty);
        const int dx = tx - ox, dy = ty - oy;
        const bool up_down = std::abs(dy) * 2 > std::abs(dx);
        const Cut towards = up_down ? (dy > 0 ? Cut::Bottom : Cut::Top) : (dx > 0 ? Cut::Right : Cut::Left);
        const Cut away = up_down ? (dy > 0 ? Cut::Top : Cut::Bottom) : (dx > 0 ? Cut::Left : Cut::Right);
        if (w.top_left_pattern)
            draw_floor_sprite(out, p, content, sprites, lot, w.top_left_pattern, x, y, level, rotation, towards, stats);
        if (w.top_left_style)
            draw_floor_sprite(out, p, content, sprites, lot, w.top_left_style, x, y, level, rotation, away, stats);
        return;
    }
    draw_floor_sprite(out, p, content, sprites, lot, pattern, x, y, level, rotation, Cut::None, stats);
}

// An object to draw and where: its tile, or a slot on the object holding it.
struct Placed {
    const LotObject *object;
    float x, y, z;   // lot space; z in tiles above the ground
    int depth;       // 0 on the floor, 1 in a slot, 2 in a slot of something in a slot, ...
};

// A placed object with the storey and tile whose turn draws it.
struct PlacedOn {
    Placed pl;
    int level, tx, ty;
    uint32_t index = 0;   // the object's place in the caller's list (pointed at again each frame: the list is made anew)
};

} // namespace

// The placements kept in ObjectDrawCache::placed_on while the objects' places are the same
// (ObjectDrawCache::position_version), in the frame's order back to front for the scene `generation`.
struct PlacedList {
    std::vector<PlacedOn> items;        // the objects standing or in slots, in the frame's order
    std::vector<uint32_t> free_index;   // the free ones (in a Sim's hand): placed afresh each frame, they move every tick
    uint64_t version = 0;
    size_t objects = 0;   // how many the caller listed when they were made
    uint32_t generation = 0;
};

namespace {

// FreeSims SLOT.HeightOffsets, in fifths of a tile, by slot height code - 1.
const float kSlotHeights[] = { 0, 2.5f, 4, 4, 0, 0, 7, 4, 0 };

// Where slot `index` of `container` is, relative to the container (FreeSims
// ObjectComponent.GetSLOTPosition). Container slots are the type-0 entries.
bool slot_offset(Content &content, const LotObject &container, int index, float &dx, float &dy, float &dz,
                 SlotCache *cache)
{
    std::vector<SlotItem> local;
    const std::vector<SlotItem> *slots = &local;
    auto cached = cache ? cache->find(container.guid) : SlotCache::iterator();
    if (cache && cached != cache->end()) {
        slots = &cached->second;
    } else {
        const ObjectInfo *info = content.object(container.guid);
        const LoadedIff *f = info ? content.load(info->ref) : nullptr;
        const IffChunk *c = f ? f->iff.find("SLOT", info->objd.get(Objd::SlotID)) : nullptr;
        std::string err;
        if (c)
            parse_slot(f->body(*c), c->size, local, err);
        if (cache)
            slots = &((*cache)[container.guid] = std::move(local));
    }
    for (const SlotItem &s : *slots) {
        if (s.type != 0 || index-- != 0)
            continue;
        float ox = s.x / 16, oy = s.y / 16;
        float oz = (s.height != 5 && s.height >= 1 && s.height <= 9 ? kSlotHeights[s.height - 1] : s.z) / 5;
        const float angle = (container.direction() & 7) / 8.0f * 2 * 3.14159265f;
        dx = ox * std::cos(angle) - oy * std::sin(angle);
        dy = ox * std::sin(angle) + oy * std::cos(angle);
        dz = oz;
        return true;
    }
    return false;
}

// Where each object is drawn: objects in slots go with the tile of whatever
// ultimately holds them; objects sharing a tile with another (a window over
// a counter) with that object.
std::vector<PlacedOn> place_objects(const Lot &lot, const std::vector<LotObject> &objects, Content &content,
                                    PaintStats &stats, SlotCache *slots)
{
    std::vector<PlacedOn> out;
    out.reserve(objects.size());
    // By id: a flat table (a map's ~650 insertions each time were most of placing them downtown).
    uint16_t top = 0;
    for (const LotObject &o : objects)
        top = std::max(top, o.id);
    std::vector<const LotObject *> by_id(size_t(top) + 1, nullptr);
    for (const LotObject &o : objects)
        by_id[o.id] = &o;
    auto find = [&by_id](uint16_t id) -> const LotObject * { return id < by_id.size() ? by_id[id] : nullptr; };
    for (size_t index = 0; index < objects.size(); index++) {
        const LotObject &o = objects[index];
        if (o.free) {   // in a Sim's hand: where the hand is, drawn on the tile under it
            const int tx = std::clamp(int(std::floor(o.fx)), 0, lot.size - 1);
            const int ty = std::clamp(int(std::floor(o.fy)), 0, lot.size - 1);
            const float above = o.fz - lot.ground(float(tx), float(ty));
            const int level = std::clamp(int(std::lround(above / kLevelHeight - 0.25f)), 0, kLotLevels - 1);
            out.push_back({ Placed{ &o, o.fx, o.fy, o.fz, 1 }, level, tx, ty, uint32_t(index) });
            continue;
        }
        Placed pl{ &o, 0, 0, 0, 0 };   // offsets from the holder's tile, added below
        const LotObject *holder = &o;
        bool placed = o.x >= 0;
        // Walk up the chain of containers, adding each slot's offset.
        for (int hops = 0; !placed && holder->container() != 0 && hops < 4; hops++) {
            const LotObject *container = find(holder->container());
            float dx, dy, dz;
            if (!container || !slot_offset(content, *container, holder->data[ObjSlotNumber], dx, dy, dz, slots))
                break;
            pl.x += dx;
            pl.y += dy;
            pl.z += dz;
            pl.depth++;
            holder = container;
            placed = holder->x >= 0;
        }
        // Not in a slot either: the lot's object grid holds one object per
        // tile, and the others on that tile link to it through ParentId
        // (windows above counters, the upper half of a tall window, ...).
        if (!placed && o.container() == 0) {
            const LotObject *root = &o;
            for (int hops = 0; root->x < 0 && root->data[ObjParentId] != 0 && hops < 8; hops++) {
                const LotObject *parent = find(uint16_t(root->data[ObjParentId]));
                if (!parent)
                    break;
                root = parent;
            }
            if (root->x >= 0) {
                const ObjectInfo *mine = content.object(o.guid), *theirs = content.object(root->guid);
                int level = root->level + (mine ? int16_t(mine->objd.get(Objd::LevelOffset)) : 0) -
                            (theirs ? int16_t(theirs->objd.get(Objd::LevelOffset)) : 0);
                if (level >= 0 && level < kLotLevels) {
                    placed = true;
                    holder = root;
                    pl.z = (level - root->level) * kLevelHeight;
                    pl.depth = 0;
                }
            }
        }
        if (!placed || holder->x >= lot.size || holder->y >= lot.size) {
            stats.objects_offlot++;
            continue;
        }
        pl.x += holder->x + holder->off_x;
        pl.y += holder->y + holder->off_y;
        pl.z += holder->level * kLevelHeight;
        const int draw_level = std::clamp(int(std::lround(pl.z / kLevelHeight - 0.25f)), 0, kLotLevels - 1);
        pl.z += lot.ground(float(holder->x), float(holder->y));   // standing on the terrain
        out.push_back({ pl, draw_level, holder->x, holder->y, uint32_t(index) });
    }
    // On each tile, what's on the floor before what's in slots on it.
    std::stable_sort(out.begin(), out.end(), [](const PlacedOn &a, const PlacedOn &b) { return a.pl.depth < b.pl.depth; });
    return out;
}

// A sprite's z-buffer averaged over its opaque pixels (255 is the back of the
// tile), worked out once per image: what orders an object's layers below.
float mean_sprite_z(const Image &img)
{
    static std::mutex lock;   // (objects are drawn afresh on three threads)
    static std::map<const Image *, float> known;
    {
        std::lock_guard<std::mutex> hold(lock);
        auto it = known.find(&img);
        if (it != known.end())
            return it->second;
    }
    double sum = 0;
    size_t n = 0;
    if (!img.depth.empty())
        for (size_t i = 0; i < img.rgba.size(); i++)
            if (img.rgba[i] >> 24) {
                sum += img.depth[i];
                n++;
            }
    std::lock_guard<std::mutex> hold(lock);
    return known[&img] = n ? float(sum / double(n)) : 204.0f;   // none: the tile's middle, as draw_sprite takes it
}

// An object's DGRP, read once (each object drawn afresh parsed it again: ~1 ms an object on the
// Vita downtown, where a few animate every tick). Kept by the object's definition, whose address
// lasts as long as the install's index; let go of all at once past a few thousand.
std::shared_ptr<const DrawGroup> draw_group(const ObjectInfo &info, const LoadedIff &file, uint16_t id)
{
    static std::mutex lock;
    static std::map<std::pair<const ObjectInfo *, uint16_t>, std::shared_ptr<const DrawGroup>> kept;
    {
        std::lock_guard<std::mutex> hold(lock);
        if (auto it = kept.find({ &info, id }); it != kept.end())
            return it->second;
    }
    const IffChunk *c = file.iff.find("DGRP", id);
    auto group = std::make_shared<DrawGroup>();
    std::string err;
    if (!c || !parse_dgrp(file.body(*c), c->size, *group, err))
        group.reset();
    std::lock_guard<std::mutex> hold(lock);
    if (kept.size() > 4000)
        kept.clear();
    kept[{ &info, id }] = group;
    return group;
}

void draw_object(DrawList &out, const Projector &p, Content &content, SpriteCache &sprites, const Placed &placed,
                 Zoom zoom, int rotation, PaintStats &stats)
{
    const LotObject &o = *placed.object;
    const ObjectInfo *info = content.object(o.guid);
    const LoadedIff *f = info ? content.load(info->ref) : nullptr;
    if (!f) {
        stats.objects_no_art++;
        stats.notes.push_back("no object file for " + o.type_name);
        return;
    }
    const Objd &objd = info->objd;   // (as read when the install was indexed)
    if (objd.get(Objd::ObjectType) == 2) {   // a Sim: drawn in 3D, not from sprites
        stats.people++;
        return;
    }
    uint16_t dgrp_id = uint16_t(objd.get(Objd::BaseGraphicID) + o.graphic());
    const std::shared_ptr<const DrawGroup> group = draw_group(*info, *f, dgrp_id);
    const DrawImage *image = group ? pick_image(*group, 1u << (o.direction() & 7), zoom, rotation) : nullptr;
    if (!image) {
        stats.objects_no_art++;
        stats.notes.push_back("no DGRP image " + std::to_string(dgrp_id) + " (direction " +
                              std::to_string(o.direction()) + ") for " + o.type_name + " / " + info->label);
        return;
    }

    const uint16_t dyn_base = objd.get(Objd::DynamicSpriteBaseId), dyn_count = objd.get(Objd::NumDynamicSprites);
    float bx, by;
    p.box(placed.x, placed.y, placed.z, bx, by);
    const float angle = (o.direction() & 7) / 8.0f * 2 * 3.14159265f;
    // The game resolves an object's layers per pixel with their z-buffers; the GL
    // renderer draws in list order, so the layers go back to front by their average
    // depth (a TV's screen and an open fridge door come after the body they sit on).
    std::vector<std::pair<float, DrawCmd>> layers;
    for (const DrawSprite &s : image->sprites) {
        // Dynamic sprites (dirty dishes, lit burners, ...) show when the running game turns them on.
        if (dyn_count && s.sprite_id >= dyn_base && s.sprite_id < uint32_t(dyn_base + dyn_count)) {
            const uint32_t bit = s.sprite_id - dyn_base;
            if (bit >= 64 || !((o.dynamic_sprites >> bit) & 1))
                continue;
        }
        const Image *found = sprites.any(info->ref, uint16_t(s.sprite_id), s.frame);
        if (!found)
            continue;
        const Image &img = *found;
        // ObjectOffset is in 1/16 tile (x, y) and 1/5 tile (z), turned with the object.
        float ox = s.object_x / 16, oy = s.object_y / 16, oz = s.object_z / 5;
        float rx = ox * std::cos(angle) - oy * std::sin(angle), ry = ox * std::sin(angle) + oy * std::cos(angle);
        Projector rel = p;
        rel.off_x = rel.off_y = 0;
        float px, py;
        rel.box(rx, ry, oz, px, py);
        int dx = int(bx + p.m.cadge_w / 2.0f + s.offset_x + px);
        int dy = int(by + p.m.baseline - img.height + s.offset_y + py);
        // Its z-buffer counts from the back corner of the tile it stands for.
        DrawList one;
        DrawCmd &c = one.sprite(img, dx, dy, s.flags & 1);
        c.owner = o.id;
        c.depth = DepthMode::Sprite;
        c.dc = p.depth(placed.x + rx, placed.y + ry, placed.z + oz) - Projector::kDepthPerDiagonal;
        layers.emplace_back(c.dc + (255 - mean_sprite_z(img)) * kSpriteDepthScale, c);
    }
    std::stable_sort(layers.begin(), layers.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    if (layers.size() < 2) {
        for (auto &layer : layers)
            out.cmds.push_back(layer.second);
        stats.objects_drawn++;
        return;
    }
    // Several layers (a fridge's body, inside and open door; a TV and its screen): the game
    // picks each pixel from the nearest layer by their z-buffers. Merged here once into one
    // picture with its own z-buffer, as the game would show it (whole layers sorted by their
    // mean depth put the fridge's white inside over its body's edge).
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    float base = 1e30f;
    std::string key = info->ref.path + ":" + std::to_string(info->ref.offset);
    for (const auto &[mean, c] : layers) {
        x0 = std::min(x0, c.x), y0 = std::min(y0, c.y);
        x1 = std::max(x1, c.x + c.image->width), y1 = std::max(y1, c.y + c.image->height);
        base = std::min(base, c.dc);
    }
    // A merge too big to be worth it (a vacation lot's scenery at the near zoom: 25.7 MB and six
    // seconds of the main thread, once, on the Vita, 2026-11-05): the layers as they are, sorted
    // by their mean depth.
    constexpr size_t kMaxMergedPixels = size_t(2) << 20;   // 8 MB
    if (size_t(std::max(0, x1 - x0)) * size_t(std::max(0, y1 - y0)) > kMaxMergedPixels) {
        for (auto &layer : layers)
            out.cmds.push_back(layer.second);
        stats.objects_drawn++;
        return;
    }
    for (const auto &[mean, c] : layers) {
        char part[96];
        std::snprintf(part, sizeof part, "|%p,%d,%d,%d,%.4f", static_cast<const void *>(c.image), c.x - x0, c.y - y0,
                      int(c.flip_h), double(c.dc - base));
        key += part;
    }
    std::lock_guard<std::mutex> merging(sprites.merge_lock());   // (filled in by one thread at a time)
    bool fresh = false;
    MemScope scope(MemKind::Sprites);   // (let go with the sprites: placed among them on the Vita)
    Image &merged = sprites.made(key, fresh);
    if (fresh) {
        const int w = x1 - x0, h = y1 - y0;
        merged.width = w;
        merged.height = h;
        merged.rgba.assign(size_t(w) * size_t(h), 0);
        merged.depth.assign(size_t(w) * size_t(h), 255);
        struct Hit {
            float d;
            uint32_t colour;
        };
        std::vector<Hit> hits;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                hits.clear();
                for (const auto &[mean, c] : layers) {
                    const Image &img = *c.image;
                    int sx = x + x0 - c.x;
                    const int sy = y + y0 - c.y;
                    if (sx < 0 || sy < 0 || sx >= img.width || sy >= img.height)
                        continue;
                    if (c.flip_h)
                        sx = img.width - 1 - sx;
                    const size_t i = size_t(sy) * size_t(img.width) + size_t(sx);
                    const uint32_t colour = img.rgba[i];
                    if (!(colour >> 24))
                        continue;
                    const float z = img.depth.empty() ? 255.0f : float(img.depth[i]);
                    hits.push_back({ (c.dc - base) + (255.0f - z) * kSpriteDepthScale, colour });
                }
                if (hits.empty())
                    continue;
                std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.d < b.d; });   // far first
                float r = 0, g = 0, b = 0, a = 0, depth = hits.back().d;
                bool solid = false;
                for (const Hit &hit : hits) {
                    const float sa = float(hit.colour >> 24) / 255.0f;
                    r = float(hit.colour & 0xff) * sa + r * (1 - sa);
                    g = float((hit.colour >> 8) & 0xff) * sa + g * (1 - sa);
                    b = float((hit.colour >> 16) & 0xff) * sa + b * (1 - sa);
                    a = sa + a * (1 - sa);
                    if (sa >= 0.5f) {   // the depth: the nearest solid layer's
                        depth = hit.d;
                        solid = true;
                    }
                }
                if (!solid)
                    depth = hits.back().d;
                // (colours above are pre-multiplied by coverage; back to straight alpha)
                const float inv = a > 0 ? 1.0f / a : 0.0f;
                auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
                const size_t o = size_t(y) * size_t(w) + size_t(x);
                merged.rgba[o] = byte(a * 255.0f) << 24 | byte(b * inv) << 16 | byte(g * inv) << 8 | byte(r * inv);
                merged.depth[o] = uint8_t(std::clamp(255.0f - depth / kSpriteDepthScale, 0.0f, 255.0f));
            }
    }
    DrawCmd c = layers.back().second;   // the nearest layer's settings (owner, depth mode)
    c.image = &merged;
    c.x = x0;
    c.y = y0;
    c.src_x = c.src_y = 0;
    c.src_w = merged.width;
    c.src_h = merged.height;
    c.flip_h = c.flip_v = false;
    c.dc = base;
    out.cmds.push_back(c);
    stats.objects_drawn++;
}

constexpr uint16_t kPersonType = 2;   // OBJD ObjectType of a Sim

// A Sim, in 3D, standing where the lot has them, in a standing idle.
void draw_person(DrawList &out, const Projector &p, Content &content, SpriteCache &sprites, const ObjectInfo &info,
                 const Placed &pl, const PaintOptions &options, PaintStats &stats, LotScene &scene)
{
    stats.people++;
    SimLook look;
    if (!read_sim_look(content, info, look)) {
        stats.notes.push_back("no bodystring for " + info.label);
        return;
    }
    if (options.people_later) {
        scene.people.push_back({ out.cmds.size(), look, pl.x, pl.y, pl.z, pl.object->direction(), pl.object->id });
        stats.people_drawn++;
        return;
    }
    CharacterLibrary &library = sprites.characters();
    SimPose pose{ standing_idle(library, look), options.sim_time_ms };
    std::string problem;
    if (draw_sim(out, p, library, look, pl.x, pl.y, pl.z, pl.object->direction(), pose, problem))
        stats.people_drawn++;
    if (!problem.empty())
        stats.notes.push_back(info.label + ": " + problem);
}

constexpr int kObjFlags = 8, kObjWallPlacement = 13;

// ---------------------------------------------------------------- shadows under objects

// The original's shadows under objects (the Legacy's renderer: its tile drawing, 0x561b90, after
// each floor tile and before what stands on it). A tile with an object that casts one has its
// floor multiplied by a tile-sized grey picture: floors.iff's round blob (the object's OBJD Shadow
// 0) or whole-tile one (16), or for a part of a multi-tile object one worked out from which of its
// neighbouring parts cast one too (0x63fae0), so the shadow runs under the whole object with soft
// outer edges. The OBJD's ShadowBrightness lightens it: a dining table's middle at about 39% of
// the floor. Ours draws it black with the darkening as its opacity (the same on a grey picture).
// Not done: the original fades it in dim rooms (by the tile's light).

// A step of the camera or the object turning: each side's bit to the side before it.
int rotate_shadow_code(int code, int steps)
{
    for (int i = 0; i < (steps & 3); i++) {
        const int edges = code & 15, corners = (code >> 4) & 15;
        code = ((edges >> 1) | (edges & 1) << 3) | ((corners >> 1) | (corners & 1) << 3) << 4;
    }
    return code;
}

// A multi-tile part's sides (0x63fae0's curves, 0x63f280): by its neighbours on that side.
// The sine kept at 0 or more before the power, as the original's (0x640f40): sin(pi) in floats
// is a hair below 0 and its power is NaN, which made the far edge's pixels black, a dotted line
// beside a two-tile object at medium zoom (the toy box and the bed, the 2026-11-01 report).
float shadow_curve(int c, float t)
{
    constexpr float kPi = 3.14159265f;
    const auto curve = [](float v) { return std::pow(std::max(v, 0.0f), 0.6f); };
    switch (c) {
    case 1: return curve(std::sin(kPi * t));
    case 3: return curve(std::sin(kPi * t / 2));
    case 5: return curve(std::sin(kPi * (1 - t) / 2));
    case 7: return 1;
    default: return 0;
    }
}

// The shadow picture for a code at a zoom: black, as opaque as the floor is darkened.
const Image *shadow_picture(Content &content, SpriteCache &sprites, int code, int zoom_index, int brightness)
{
    bool fresh = false;
    MemScope scope(MemKind::Sprites);   // (as above)
    Image &out = sprites.made("shadow " + std::to_string(code) + " " + std::to_string(zoom_index) + " " +
                                  std::to_string(brightness),
                              fresh);
    if (!fresh)
        return out.width ? &out : nullptr;
    const FileRef *floors = content.file("floors.iff");
    if (!floors)
        return nullptr;
    std::vector<int> value;   // 0 black - 255 the floor as it is
    if (code == 0 || code == 16) {
        const Image *img = sprites.indices(*floors, uint16_t((code ? 0x310 : 0x300) + 0x20 * zoom_index), 0);
        if (!img)
            return nullptr;
        out.width = img->width;
        out.height = img->height;
        for (uint32_t px : img->rgba)
            value.push_back(px >> 24 ? int(px & 0xff) : 255);
    } else {
        const Image *diamond = sprites.any(*floors, uint16_t(0x100 * zoom_index), 0);   // the floor tile's shape
        if (!diamond)
            return nullptr;
        out.width = diamond->width;
        out.height = diamond->height;
        auto bit = [&](int b) { return (code >> b) & 1; };
        const int c[4] = { bit(0) + 2 * bit(7) + 4 * bit(4), bit(1) + 2 * bit(5) + 4 * bit(4),
                           bit(2) + 2 * bit(6) + 4 * bit(5), bit(3) + 2 * bit(6) + 4 * bit(7) };
        const float w = float((out.width + 1) & ~1), h = float(out.height);
        for (int y = 0; y < out.height; y++)
            for (int x = 0; x < out.width; x++) {
                if (!(diamond->rgba[size_t(y) * diamond->width + x] >> 24)) {
                    value.push_back(255);
                    continue;
                }
                const float a = std::clamp(((y - h / 2) + (x - w / 2) / 2) * 2 / w + 0.5f, 0.0f, 1.0f);
                const float b = std::clamp(((y - h / 2) - (x - w / 2) / 2) * 2 / w + 0.5f, 0.0f, 1.0f);
                const float s = shadow_curve(c[0], a) * (1 - b) + shadow_curve(c[1], b) * (1 - a) + shadow_curve(c[2], a) * b +
                                shadow_curve(c[3], b) * a;
                value.push_back(int(std::lround(255 * std::clamp(1 - s / 2, 0.0f, 1.0f))));
            }
    }
    out.rgba.resize(value.size());
    for (size_t i = 0; i < value.size(); i++) {
        const int keep = std::clamp(value[i] + brightness, 0, 255);   // of the floor's brightness
        out.rgba[i] = uint32_t(255 - keep) << 24;
    }
    return &out;
}

// The shadow code an object casts, or -1 (the original's FUN_00596620 / FUN_00595a10 when it's
// made): a single-tile object's OBJD Shadow; for a part of a multi-tile one, which of its
// neighbouring parts (by SubIndex: low byte across, high byte down) cast one too.
// The lot's placed objects by tile, for finding those near one without a pass over all of them
// (every part of every multi-tile object did that: ~650 x 650 on a Downtown lot, each tick).
using TileObjects = std::unordered_map<uint32_t, std::vector<const LotObject *>>;
uint32_t tile_key(int level, int x, int y)
{
    return uint32_t(level & 0xff) << 16 | uint32_t(x & 0xff) << 8 | uint32_t(y & 0xff);
}

int shadow_code(const LotObject &o, const ObjectInfo &info, const TileObjects &by_tile, Content &content)
{
    const int16_t own = int16_t(info.objd.get(Objd::Shadow));
    if (own == -1)
        return -1;
    const uint16_t master = info.objd.get(Objd::MasterID);
    if (master == 0)
        return own;
    const int16_t sub = int16_t(info.objd.get(Objd::SubIndex));
    const int lo = int8_t(sub & 0xff), hi = int8_t(sub >> 8);
    int code = 0;
    static const int kSide[8][3] = { { 0, -1, 0x01 }, { -1, 0, 0x02 }, { 0, 1, 0x04 }, { 1, 0, 0x08 },
                                     { -1, -1, 0x10 }, { -1, 1, 0x20 }, { 1, 1, 0x40 }, { 1, -1, 0x80 } };
    std::vector<const LotObject *> near;
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
            if (auto it = by_tile.find(tile_key(o.level, o.x + dx, o.y + dy)); it != by_tile.end() && o.x + dx >= 0 && o.y + dy >= 0)
                near.insert(near.end(), it->second.begin(), it->second.end());
    for (const LotObject *np : near) {
        const LotObject &n = *np;
        if (&n == &o)
            continue;
        const ObjectInfo *ni = content.object(n.guid);
        // (Master ids are numbered within each object file: the Goths' bed and stairs share 4.)
        if (!ni || ni->objd.get(Objd::MasterID) != master || ni->ref.path != info.ref.path ||
            ni->ref.offset != info.ref.offset || int16_t(ni->objd.get(Objd::Shadow)) == -1 ||
            ni->objd.get(Objd::LevelOffset) != 0)
            continue;
        const int16_t nsub = int16_t(ni->objd.get(Objd::SubIndex));
        if (nsub == -1)
            continue;
        const int nlo = int8_t(nsub & 0xff), nhi = int8_t(nsub >> 8);
        for (const auto &side : kSide)
            if (nhi == hi + side[0] && nlo == lo + side[1])
                code |= side[2];
    }
    // A corner without both its sides: the plain blob instead (the original's table of good codes).
    static const int kCorner[4][3] = { { 0x10, 0x01, 0x02 }, { 0x20, 0x02, 0x04 }, { 0x40, 0x04, 0x08 }, { 0x80, 0x08, 0x01 } };
    for (const auto &k : kCorner)
        if ((code & k[0]) && (code & (k[1] | k[2])) != (k[1] | k[2]))
            return 0;
    return code;
}
constexpr int kHideForCutawayFlag = 11;   // Flags bit 10, numbered from 1 as SimAntics does

// FreeSims ObjectComponent.Update: an object flagged to hide for the cutaway
// (doors and windows) disappears when the wall it is in is cut, i.e. its
// tile and the tile across that wall are both cut. Objects set the flag in
// their init code, so saves don't hold it: it is read from the code.
// (tx, ty, level) is the tile it is drawn on: its own, or that of the object it shares a tile with.
bool hidden_by_cutaway(const LotObject &o, int tx, int ty, int level, const Lot &lot, const WallPainter &walls,
                       Content &content, ObjectCode &code)
{
    if (o.in_hand || !walls.cut(tx, ty, level))
        return false;
    const ObjectInfo *info = content.object(o.guid);
    const bool flagged = (uint16_t(o.data[kObjFlags]) & (1 << (kHideForCutawayFlag - 1))) ||
                         (info && code.sets_own_flag(*info, kObjFlags, kHideForCutawayFlag));
    if (!flagged)
        return false;
    // The walls it needs (front, right, behind, left), turned to the tile's
    // segments by its facing (VMEntity.SetWallUse).
    const int placement = o.data[kObjWallPlacement] % 15;
    const int rotate = (8 - ((o.direction() / 2) % 4 + 1)) % 4;
    const int wall = (((placement << (4 - rotate)) & 15) | ((placement & 15) >> rotate)) &
                     lot.wall(tx, ty, level).segments;
    // Hidden when the wall piece it sits in is drawn lowered (the painter leaves a gap there for
    // a door): before, both tiles had to be cut, and a door in an outside wall (the tile beyond
    // isn't a room's) stood full height in the lowered wall, a slice of wall with its frame (the
    // user's 07:52 screenshot, 2026-11-02).
    if (wall == WallTopLeft || wall == WallTopRight || wall == WallBottomRight || wall == WallBottomLeft)
        return walls.edge_cut(tx, ty, wall, level);
    return true;   // not on one wall piece: with its tile, as before
}

} // namespace

// The room whose light a wall or an object at (x, y) shows: on a tile a diagonal wall splits,
// the half nearer the camera (rooms.cpp: a horizontal diagonal's low half is its (-x, -y) corner,
// a vertical one's its (+x, -y) corner); 1, the outdoors, off the lot.
uint16_t camera_room(const RoomMap &rooms, const Lot &lot, const Projector &p, int level, int x, int y)
{
    if (rooms.map.empty() || x < 0 || y < 0 || x >= kLotArraySize || y >= kLotArraySize)
        return 1;
    const uint32_t v = rooms.map[size_t(x) + size_t(y) * kLotArraySize];
    const uint16_t low = uint16_t(v), high = uint16_t(v >> 16);
    const uint16_t segments = lot.walls[level][size_t(x) + size_t(y) * kLotArraySize].segments;
    if (low == high || !(segments & (WallHorizontalDiag | WallVerticalDiag)))
        return low ? low : 1;
    const bool horizontal = segments & WallHorizontalDiag;
    float lx, ly, hx, hy, unused;
    p.box(float(x) + (horizontal ? -0.5f : 0.5f), float(y) - 0.5f, 0, unused, ly);
    p.box(float(x) + (horizontal ? 0.5f : -0.5f), float(y) + 0.5f, 0, unused, hy);
    (void)lx, (void)hx;
    const uint16_t front = hy > ly ? high : low;
    return front ? front : (low ? low : 1);
}

BuildProfile g_build_profile;

namespace {
double ms_between(ts::FastClock::time_point a, ts::FastClock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Below this grass value a tile has no blades (the original's fGrassLowDensityValue, -73):
// sand at -128, and the bare patches of a lawn, are the plain speckle at the near zoom too.
constexpr float kBladesBelow = -73.0f;

// The grass under the whole lot in one view: a speckle fixed to the screen, tinted by how worn
// each corner's tiles are, blended across the tile, as triangles into `out` (two a tile).
void make_grass_cmds(const Lot &lot, const Projector &p, Zoom zoom, DrawList &out, const int *box = nullptr)
{
    const GroundStyle &style = ground_style(lot.house());
    // The near zoom: blades where the grass grows, else the fine near speckle; the other zooms
    // the coarse speckle everywhere.
    const Image &blades = grass_texture(), &near_plain = ground_near_speckle_texture(), &speckle = grass_speckle_texture(),
                &fine = ground_speckle_texture();
    auto grass_at = [&](int x, int y) {   // (signed: averaging sand, -128, with grass must not wrap)
        x = std::clamp(x, 0, lot.size - 1);
        y = std::clamp(y, 0, lot.size - 1);
        return float(lot.grass.empty() ? 0 : int8_t(lot.grass[size_t(x) + size_t(y) * kLotArraySize]));
    };
    for (int y = 1; y < lot.size - 1; y++)   // (the lot's outer ring isn't shown: the original's lot is
        for (int x = 1; x < lot.size - 1; x++) {   // 62 tiles across on screen, not 64; a strip of grass past the road's end)
            if (box && (x < box[0] || y < box[1] || x > box[2] || y > box[3]))
                continue;
            // Corners: top-left, top-right, bottom-right, bottom-left in lot space.
            static const int kCorner[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            const bool near = zoom == Zoom::Near;
            const bool plain = near && (!style.blades || grass_at(x, y) <= kBladesBelow);
            const bool lawn = style.green && grass_at(x, y) >= 0;   // (as grass_tint's lawn half)
            const Image &img = !near ? (lawn ? speckle : fine) : plain ? near_plain : blades;
            float sx[4], sy[4];
            uint32_t tint[4];
            for (int i = 0; i < 4; i++) {
                const int cx = x + kCorner[i][0], cy = y + kCorner[i][1];   // the corner at (cx - 0.5, cy - 0.5)
                p.box(cx - 0.5f, cy - 0.5f, lot.ground(cx - 0.5f, cy - 0.5f), sx[i], sy[i]);
                sx[i] += p.m.cadge_w / 2.0f;
                sy[i] += float(p.m.baseline);
                const float value = (grass_at(cx - 1, cy - 1) + grass_at(cx, cy - 1) + grass_at(cx - 1, cy) + grass_at(cx, cy)) / 4;
                tint[i] = plain ? ground_near_tint(value, style) : grass_tint(value, style, img);
                // Slopes are lit from behind the default camera (-x, -y), as in
                // the game's thumbnails: ground facing the camera is darker.
                const float gx = (lot.ground(cx + 0.5f, cy - 0.5f) - lot.ground(cx - 1.5f, cy - 0.5f)) / 2;
                const float gy = (lot.ground(cx - 0.5f, cy + 0.5f) - lot.ground(cx - 0.5f, cy - 1.5f)) / 2;
                const float lx = -0.5f, ly = -0.5f, lz = 0.7071f;   // towards the light
                const float lambert = (-gx * lx - gy * ly + lz) / std::sqrt(gx * gx + gy * gy + 1);
                // Half strength, kept near 1: the original shades its slopes only faintly
                // (compared with the Legacy Collection, the Goths' hill, 2026-10-26).
                const float shade = std::clamp(1.0f + (lambert / lz - 1.0f) * 0.5f, 0.82f, 1.1f);
                uint32_t lit = 0xff000000u;
                for (int s = 0; s < 24; s += 8)
                    lit |= uint32_t(std::min(255, int(((tint[i] >> s) & 0xff) * shade + 0.5f))) << s;
                tint[i] = lit;
            }
            if (TS_ENV("TS_GRASS_LOG") && x == y)   // debug: a diagonal's values and textures
                std::printf("grass tile %d,%d value %.0f %s\n", x, y, grass_at(x, y), plain ? "plain" : near ? "blades" : "speckle");
            static const int kTris[2][3] = { { 0, 1, 2 }, { 2, 3, 0 } };
            for (const auto &tri : kTris) {
                float px[3], py[3];
                uint32_t t[3];
                for (int k = 0; k < 3; k++) {
                    px[k] = sx[tri[k]];
                    py[k] = sy[tri[k]];
                    t[k] = tint[tri[k]];
                }
                out.triangle(img, px, py, px, py, t).room = kGrassLight;   // the background: no depth
            }
        }
}

// The kept grass lists: one a view (zoom and rotation) of a lot's ground, at most four (the
// four rotations at a zoom; the oldest goes: a lot edit changes every key).
struct GrassKept {
    uint64_t key = 0;
    float off_x = 0, off_y = 0;   // the scene offsets the commands were made at
    std::vector<DrawCmd> cmds;
};
std::vector<GrassKept> grass_kept_all;

uint64_t grass_key_for(const Lot &lot, Zoom zoom, int rotation)
{
    uint64_t key = 1469598103934665603ull;
    auto mix = [&key](const void *d, size_t n) {
        for (size_t k = 0; k < n; k++)
            key = (key ^ static_cast<const uint8_t *>(d)[k]) * 1099511628211ull;
    };
    const int head[4] = { int(zoom), rotation & 3, lot.size, lot.terrain ? 1 : 0 };
    mix(head, sizeof head);
    mix(lot.grass.data(), lot.grass.size());
    mix(lot.altitude.data(), lot.altitude.size());
    return key;
}

GrassKept *grass_kept_find(const Lot &lot, Zoom zoom, int rotation)
{
    const uint64_t key = grass_key_for(lot, zoom, rotation);
    for (GrassKept &g : grass_kept_all)
        if (g.key == key && !g.cmds.empty())
            return &g;
    return nullptr;
}

void grass_kept_add(const Lot &lot, Zoom zoom, int rotation, const Projector &p, std::vector<DrawCmd>::const_iterator first,
                    std::vector<DrawCmd>::const_iterator last)
{
    if (grass_kept_all.size() >= 4)
        grass_kept_all.erase(grass_kept_all.begin());
    grass_kept_all.push_back(GrassKept{});
    grass_kept_all.back().key = grass_key_for(lot, zoom, rotation);
    grass_kept_all.back().off_x = p.off_x;
    grass_kept_all.back().off_y = p.off_y;
    grass_kept_all.back().cmds.assign(first, last);
}
} // namespace

void drop_grass_lists()
{
    grass_kept_all.clear();
}

int warm_grass(const Lot &lot, Zoom zoom)
{
    int made = 0;
    for (int rotation = 0; rotation < 4; rotation++) {
        if (grass_kept_find(lot, zoom, rotation))
            continue;
        const Projector p{ metrics(zoom), rotation };
        DrawList list;
        make_grass_cmds(lot, p, zoom, list);
        grass_kept_add(lot, zoom, rotation, p, list.cmds.begin(), list.cmds.end());
        made++;
    }
    return made;
}

LotScene build_lot_scene(const Lot &lot, Content &content, SpriteCache &sprites, const PaintOptions &options,
                         PaintStats &stats, LotScene *room)
{
    using BuildClock = ts::FastClock;
    BuildProfile &prof = g_build_profile;
    prof = BuildProfile();
    auto mark = BuildClock::now();
    auto lap = [&](double &into) {
        const auto now = BuildClock::now();
        into += ms_between(mark, now);
        mark = now;
    };
    const Zoom zoom = options.zoom;
    const int rotation = options.rotation & 3;
    Projector p{ metrics(zoom), rotation };
    const int top_level = std::clamp(options.view_level, 1, kLotLevels) - 1;

    // Roofs show with the walls up and every storey in view, as in the game.
    std::vector<RoofFace> roofs[kLotLevels];
    std::vector<uint8_t> roofed[kLotLevels];   // the tiles each storey's roof covers, x + y * kLotArraySize
    const Image *roof_texture = nullptr;
    if (options.roofs && options.walls == WallMode::Up && top_level == kLotLevels - 1) {
        const std::string name = lot.hous.roof_name.empty() ? std::string("r1_") : lot.hous.roof_name;
        roof_texture = sprites.loose_bmp("GameData/Roofs/" + name + ".bmp");
        if (roof_texture) {
            RoomMap rooms[kLotLevels];
            for (int level = 0; level < kLotLevels; level++)
                rooms[level] = build_rooms(lot, level);
            static const char *only = TS_ENV("TS_ROOF_STOREY");   // debug: one storey's roof alone
            for (int storey = 0; storey < kLotLevels; storey++)
                if (!only || std::atoi(only) == storey)
                    roofs[storey] = build_roof(lot, rooms, storey);
            // Nothing under the roof is hidden: the original draws the rooms and shows them through
            // the windows (the Goths' tub through the lower room's window, 2026-11-03). The holes
            // in roofs were the depth pass redrawing Sims and furniture by walls over the roof without
            // putting the roof back (fixed there, SpriteRenderer::composite_depth). TS_ROOF_HIDE (debug)
            // hides what's under a roof as before, to compare.
            static const bool hide = TS_ENV("TS_ROOF_HIDE") != nullptr;
            for (int storey = 0; storey < kLotLevels && hide; storey++) {
                if (roofs[storey].empty())
                    continue;
                roofed[storey].assign(size_t(kLotArraySize) * kLotArraySize, 0);
                for (int y = 0; y < kLotArraySize; y++)
                    for (int x = 0; x < kLotArraySize; x++)
                        roofed[storey][size_t(x + y * kLotArraySize)] = under_roof(lot, rooms, storey, x, y) ? 1 : 0;
            }
        } else {
            stats.notes.push_back("no roof texture " + name);
        }
    }

    lap(prof.roofs_ms);
    float highest = 0;   // the highest ground, so tall terrain stays inside the bounds
    for (uint8_t h : lot.altitude)
        highest = std::max(highest, lot.terrain ? h * kAltitudeStep : 0.0f);

    // Bounds of the lot on screen, both storeys and the roofs, plus room for tall sprites.
    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    auto include = [&](float sx, float sy) {
        min_x = std::min(min_x, sx);
        min_y = std::min(min_y, sy);
        max_x = std::max(max_x, sx);
        max_y = std::max(max_y, sy);
    };
    for (float x : { 0.0f, float(lot.size) })
        for (float y : { 0.0f, float(lot.size) })
            for (float z : { 0.0f, kLevelHeight + highest }) {
                float sx, sy;
                p.box(x, y, z, sx, sy);
                include(sx, sy);
            }
    for (const auto &faces : roofs)
        for (const RoofFace &f : faces)
            for (int i = 0; i < 4; i++) {   // the corner itself, in the same box coordinates
                float sx, sy;
                p.box(f.x[i], f.y[i], f.z[i], sx, sy);
                include(sx + p.m.cadge_w / 2.0f, sy + p.m.baseline);
            }
    p.off_x = -min_x;
    p.off_y = -min_y;
    LotScene scene;
    if (room) {
        scene.list.cmds.swap(room->list.cmds);
        scene.list.cmds.clear();
        scene.bounds.swap(room->bounds);
        scene.bounds.clear();
    }
    scene.width = int(max_x - min_x) + p.m.cadge_w;
    scene.height = int(max_y - min_y) + p.m.baseline + int(p.m.half_h) * 2;
    DrawList &out = scene.list;
    scene.walls = std::make_shared<WallPainter>(lot, content, sprites, p, options.walls, top_level,
                                                options.cut_follows ? &options.cut_rooms : nullptr);
    scene.code = std::make_shared<ObjectCode>(content);
    static uint32_t generation = 0;
    scene.generation = ++generation;
    scene.zoom = zoom;
    scene.rotation = rotation;
    scene.top_level = top_level;
    WallPainter &walls = *scene.walls;
    ObjectCode &code = *scene.code;
    // The rooms come from the walls and floors alone: kept from the last scene while those stay
    // (a cutaway move rebuilds the scene: 15 ms of it on the Vita was working the rooms out again).
    {
        static uint64_t rooms_key = 0;
        static RoomMap rooms_kept[kLotLevels];
        uint64_t key = 1469598103934665603ull;
        auto mix = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
        mix(uint64_t(lot.size));
        for (int level = 0; level < kLotLevels; level++) {
            mix(lot.walls[level].size());
            for (const WallTile &w : lot.walls[level])
                mix(uint64_t(w.segments) | uint64_t(w.top_left_style) << 8 | uint64_t(w.top_right_style) << 24 |
                    uint64_t(w.top_left_pattern) << 40);
            mix(lot.floors[level].size());
            for (uint16_t f : lot.floors[level])
                mix(f);
        }
        if (key != rooms_key) {
            rooms_key = key;
            for (int level = 0; level < kLotLevels; level++)
                rooms_kept[level] = build_rooms(lot, level);
        }
        for (int level = 0; level < kLotLevels; level++)
            scene.rooms[level] = rooms_kept[level];
    }
    lap(prof.rooms_ms);
    // The room a tile's draws take their light from (off the lot: the outdoors).
    auto room_tag = [&](int level, int x, int y) {   // floors: a split tile's indoor half
        if (x < 0 || y < 0 || x >= kLotArraySize || y >= kLotArraySize || scene.rooms[level].map.empty())
            return light_room(level, 1);
        return light_room(level, scene.rooms[level].lit_room(x, y));
    };
    auto seen_tag = [&](int level, int x, int y) {   // walls and objects: the side the camera sees
        return light_room(level, camera_room(scene.rooms[level], lot, p, level, x, y));
    };
    auto tag_from = [&](size_t first, uint16_t tag) {
        for (size_t i = first; i < out.cmds.size(); i++)
            out.cmds[i].room = tag;
    };

    // Grass under the whole lot: a speckle fixed to the screen, tinted by how
    // worn each corner's tiles are, blended across the tile.
    const Image &grass = zoom == Zoom::Near && ground_style(lot.house()).blades ? grass_texture() : grass_speckle_texture();
    const GroundStyle &ground = ground_style(lot.house());
    auto grass_at = [&](int x, int y) {   // (signed, as make_grass_cmds)
        x = std::clamp(x, 0, lot.size - 1);
        y = std::clamp(y, 0, lot.size - 1);
        return float(lot.grass.empty() ? 0 : int8_t(lot.grass[size_t(x) + size_t(y) * kLotArraySize]));
    };
    // The same ground in the same view draws the same: kept from the last scene (a cutaway
    // move rebuilds the scene, and the grass was 40% of it: ~40 ms on the Vita). A few kept
    // lists, by key (the four rotations at a zoom): turning the camera back to a side seen
    // before costs nothing (the Vita spent 43 ms of a 147 ms turn on the grass, 2026-10-30), and
    // warm_grass() makes the other rotations' lists as a lot opens, so a first turn finds them.
    // The key leaves the scene's offsets out: a kept list is shifted to the scene it joins.
    (void)grass_at;
    const int *box = options.tile_box[0] >= 0 ? options.tile_box : nullptr;   // (a lot's picture: its own ground only)
    auto in_box = [box](int x, int y) { return !box || (x >= box[0] && y >= box[1] && x <= box[2] && y <= box[3]); };
    GrassKept *kept_hit = box ? nullptr : grass_kept_find(lot, zoom, rotation);
    const bool grass_same = kept_hit != nullptr;
    if (grass_same) {
        const float dx = p.off_x - kept_hit->off_x, dy = p.off_y - kept_hit->off_y;
        for (const DrawCmd &c : kept_hit->cmds) {
            out.cmds.push_back(c);
            DrawCmd &d = out.cmds.back();
            if (!is_grass_texture(d.image))   // (a tile keeps its blades or speckle; the images are static)
                d.image = &grass;
            for (int k = 0; k < 3; k++) {
                d.px[k] += dx;
                d.py[k] += dy;
            }
        }
    } else {
        const size_t grass_first = out.cmds.size();
        make_grass_cmds(lot, p, zoom, out, box);
        if (!box)
            grass_kept_add(lot, zoom, rotation, p, out.cmds.begin() + std::ptrdiff_t(grass_first), out.cmds.end());
    }
    lap(prof.grass_ms);
    // Objects by tile, so each level can be drawn back to front.
    std::map<std::pair<int, int>, std::vector<Placed>> by_tile[kLotLevels];
    if (!options.objects_later)
        for (const PlacedOn &on : place_objects(lot, lot.objects, content, stats, nullptr))
            if (in_box(on.tx, on.ty))
                by_tile[on.level][{ on.tx, on.ty }].push_back(on.pl);
    for (auto &level_tiles : by_tile)
        for (auto &tile : level_tiles)
            std::stable_sort(tile.second.begin(), tile.second.end(),
                             [](const Placed &a, const Placed &b) { return a.depth < b.depth; });

    // Back to front: sort tiles by their screen depth for this rotation. One
    // row past the lot's edge holds the caps at the ends of its edge walls.
    const int edge = std::min(lot.size + 1, kLotArraySize);
    scene.edge = edge;
    for (int storey = 0; storey < kLotLevels; storey++)
        if (!roofed[storey].empty()) {
            scene.roofed[storey].assign(size_t(edge) * edge, 0);
            for (int y = 0; y < edge; y++)
                for (int x = 0; x < edge; x++)
                    scene.roofed[storey][size_t(x + y * edge)] = roofed[storey][size_t(x + y * kLotArraySize)];
        }
    auto under_roof_at = [&](int level, int x, int y) {
        return level >= 0 && level < kLotLevels && !scene.roofed[level].empty() && x >= 0 && y >= 0 && x < edge && y < edge &&
               scene.roofed[level][size_t(x + y * edge)];
    };
    for (auto &ends : scene.tile_end)
        ends.assign(size_t(edge) * size_t(edge), UINT32_MAX);
    // The tiles' order depends on the lot's size and the camera's angle and zoom only: kept.
    static std::vector<std::pair<int, int>> order_kept;
    static int order_key[3] = { -1, -1, -1 };
    if (order_key[0] != edge || order_key[1] != rotation || order_key[2] != int(zoom)) {
        order_key[0] = edge, order_key[1] = rotation, order_key[2] = int(zoom);
        order_kept.clear();
        for (int y = 0; y < edge; y++)
            for (int x = 0; x < edge; x++)
                order_kept.push_back({ x, y });
        std::stable_sort(order_kept.begin(), order_kept.end(), [&](const auto &a, const auto &b) {
            float ax, ay, bx, by;
            p.box(float(a.first), float(a.second), 0, ax, ay);
            p.box(float(b.first), float(b.second), 0, bx, by);
            return ay < by;
        });
    }
    const std::vector<std::pair<int, int>> &order = order_kept;
    scene.tile_rank.assign(size_t(edge) * size_t(edge), 0);
    for (size_t k = 0; k < order.size(); k++)
        scene.tile_rank[size_t(order[k].first + order[k].second * edge)] = uint32_t(k);

    // A roof sits on the storey above the one it covers, so it is drawn in
    // that storey's pass: each sloped side right after the front-most tile
    // under it, so walls behind it go first and walls in front go after.
    std::map<std::pair<int, int>, std::vector<const RoofFace *>> roof_at[kLotLevels + 1];
    for (int storey = 0; storey < kLotLevels; storey++)
        for (const RoofFace &f : roofs[storey]) {
            float cx = 0, cy = 0;
            for (int i = 0; i < 4; i++) {
                cx += f.x[i] / 4;
                cy += f.y[i] / 4;
            }
            std::pair<int, int> key{ 0, 0 };
            float best = -1e9f;
            for (int i = 0; i < 4; i++) {
                // A corner pulled a little toward the middle, so it falls in a tile under the roof.
                const int tx = int(std::lround(f.x[i] + (cx - f.x[i]) * 0.25f));
                const int ty = int(std::lround(f.y[i] + (cy - f.y[i]) * 0.25f));
                float sx, sy;
                p.box(float(tx), float(ty), 0, sx, sy);
                if (sy > best) {
                    best = sy;
                    key = { std::clamp(tx, 0, edge - 1), std::clamp(ty, 0, edge - 1) };
                }
            }
            roof_at[storey + 1][key].push_back(&f);
        }
    auto draw_roof = [&](const RoofFace &f, bool lower) {
        float sx[4], sy[4], u[4], v[4];
        for (int i = 0; i < 4; i++) {
            p.box(f.x[i], f.y[i], f.z[i], sx[i], sy[i]);
            sx[i] += p.m.cadge_w / 2.0f;
            sy[i] += float(p.m.baseline);
            u[i] = f.u[i] * roof_texture->width;
            v[i] = f.v[i] * roof_texture->height;
        }
        static const int kTris[2][3] = { { 0, 1, 2 }, { 2, 3, 0 } };
        for (const auto &tri : kTris) {
            float px[3], py[3], tu[3], tv[3];
            uint32_t tint[3];
            for (int k = 0; k < 3; k++) {
                px[k] = sx[tri[k]];
                py[k] = sy[tri[k]];
                tu[k] = u[tri[k]];
                tv[k] = v[tri[k]];
                const uint32_t g = uint32_t(std::clamp(f.shade[tri[k]], 0.0f, 1.0f) * 255 + 0.5f);
                tint[k] = 0xff000000u | g << 16 | g << 8 | g;
            }
            DrawCmd &c = out.triangle(*roof_texture, px, py, tu, tv, tint);
            c.depth = DepthMode::Corners;
            if (lower && g_roof_depth)
                c.owner = kLowerRoofOwner;
            c.room = light_room(0, 1);   // outdoors
            for (int k = 0; k < 3; k++)
                c.dv[k] = p.depth(f.x[tri[k]], f.y[tri[k]], f.z[tri[k]]);
        }
        stats.roofs++;
    };

    lap(prof.order_ms);
    for (int level = 0; level <= top_level; level++) {
        for (const auto &t : order) {
            if (!in_box(t.first, t.second))
                continue;
            const size_t first = out.cmds.size();
            draw_floor(out, p, content, sprites, lot, walls, t.first, t.second, level, rotation, stats);
            tag_from(first, room_tag(level, t.first, t.second));
        }
        if (level == 0 && zoom == Zoom::Near && ground.blades) {
            // The grass's ragged edge over a floor behind it (the original's, at the near zoom
            // only: its blades poke up over the floor's front edges; none on snow).
            const Image &fringe = grass_fringe_texture();
            auto paved = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= lot.size || y >= lot.size)
                    return true;   // (off the lot: no grass to show)
                const uint16_t f = lot.floor(x, y, 0);
                return f != 0;
            };
            auto at = [&](float lx, float ly, float &sx, float &sy) {
                p.box(lx, ly, lot.ground(lx, ly), sx, sy);
                sx += p.m.cadge_w / 2.0f;
                sy += float(p.m.baseline);
            };
            static const int kSide[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
            for (int y = 0; y < lot.size; y++)
                for (int x = 0; x < lot.size; x++) {
                    const uint16_t f = lot.floor(x, y, 0);
                    if (f == 0 || f == kFloorPool || f == kFloorWater)
                        continue;
                    for (const auto &side : kSide) {
                        const int nx = x + side[0], ny = y + side[1];
                        if (paved(nx, ny))
                            continue;
                        float ux, uy, vx, vy;
                        at(float(x), float(y), ux, uy);
                        at(float(nx), float(ny), vx, vy);
                        if (vy <= uy)
                            continue;   // the grass is behind the floor: no edge shows
                        // The edge between them: its two corners.
                        const float ex = side[0] ? float(x) + 0.5f * float(side[0]) : float(x) - 0.5f;
                        const float ey = side[1] ? float(y) + 0.5f * float(side[1]) : float(y) - 0.5f;
                        const float fx = side[0] ? ex : float(x) + 0.5f, fy = side[1] ? ey : float(y) + 0.5f;
                        float ax, ay, bx, by;
                        at(ex, ey, ax, ay);
                        at(fx, fy, bx, by);
                        const float h = float(fringe.height);
                        const float px0[3] = { ax, bx, bx }, py0[3] = { ay + 1 - h, by + 1 - h, by + 1 };
                        const float px1[3] = { bx, ax, ax }, py1[3] = { by + 1, ay + 1, ay + 1 - h };
                        const float v0[3] = { 0, 0, h }, v1[3] = { h, h, 0 };
                        if (grass_at(nx, ny) <= kBladesBelow)   // (no blades to poke over the edge)
                            continue;
                        const uint32_t c = grass_tint(grass_at(nx, ny), ground, grass_texture());
                        const uint32_t tint[3] = { c, c, c };
                        out.triangle(fringe, px0, py0, px0, v0, tint).room = kGrassLight;
                        out.triangle(fringe, px1, py1, px1, v1, tint).room = kGrassLight;
                    }
                }
        }
        scene.floors_end[level] = uint32_t(out.cmds.size());
        lap(prof.floors_ms);
        for (const auto &t : order) {
            if (!in_box(t.first, t.second))
                continue;
            // A wall drawn with a tile is its back edge's, seen from the tile's side: the tile's room.
            const size_t first = out.cmds.size();
            // (the split's clocks only when asked: four a tile cost the Vita ~30 ms a build)
            static const bool timed = TS_ENV("TS_BUILD_LOG") != nullptr;
            const auto t_wall = timed ? ts::FastClock::now() : ts::FastClock::time_point{};
            walls.draw(out, t.first, t.second, level, stats);
            const auto t_objs = timed ? ts::FastClock::now() : ts::FastClock::time_point{};
            if (timed)
                prof.walls_only_ms += std::chrono::duration<double, std::milli>(t_objs - t_wall).count();
            auto it = by_tile[level].find(t);
            if (it != by_tile[level].end())
                for (const Placed &pl : it->second) {
                    if (hidden_by_cutaway(*pl.object, t.first, t.second, level, lot, walls, content, code)) {
                        stats.objects_cut++;
                        continue;
                    }
                    if (TS_ENV("TS_ROOFED") && !scene.roofed[std::clamp(level, 0, kLotLevels - 1)].empty())   // debug
                        if (const ObjectInfo *oi = content.object(pl.object->guid))
                            std::printf("roofed? %s #%d at (%d, %d) storey %d: %d\n", oi->label.c_str(), int(pl.object->id),
                                        t.first, t.second, level, int(under_roof_at(level, t.first, t.second)));
                    if (under_roof_at(level, t.first, t.second))
                        continue;   // under the roof shown (see LotScene::roofed)
                    const ObjectInfo *info = content.object(pl.object->guid);
                    if (info && info->objd.get(Objd::ObjectType) == kPersonType) {
                        if (options.people)
                            draw_person(out, p, content, sprites, *info, pl, options, stats, scene);
                        continue;
                    }
                    draw_object(out, p, content, sprites, pl, zoom, rotation, stats);
                }
            if (timed)
                prof.objects_ms += std::chrono::duration<double, std::milli>(ts::FastClock::now() - t_objs).count();
            tag_from(first, seen_tag(level, t.first, t.second));
            if (t.first >= 0 && t.second >= 0 && t.first < edge && t.second < edge)
                scene.tile_end[level][size_t(t.first + t.second * edge)] = uint32_t(out.cmds.size());
            auto roof = roof_at[level].find(t);
            if (roof != roof_at[level].end())
                for (const RoofFace *f : roof->second)
                    draw_roof(*f, true);
        }
        lap(prof.walls_ms);
        prof.faces_made = walls.faces_made();
    }
    // The roof over the top storey has nothing above it to sort against.
    for (const auto &t : order) {
        auto roof = roof_at[kLotLevels].find(t);
        if (roof != roof_at[kLotLevels].end())
            for (const RoofFace *f : roof->second)
                draw_roof(*f, false);
    }
    for (auto &ends : scene.tile_end)   // tiles not drawn (storeys above the top one): the end
        for (uint32_t &e : ends)
            if (e == UINT32_MAX)
                e = uint32_t(out.cmds.size());
    scene.projector = p;
    scene.bounds.reserve(scene.list.cmds.size());
    for (const DrawCmd &cmd : scene.list.cmds)
        scene.bounds.push_back(bounds_of(cmd));
    lap(prof.bounds_ms);
    return scene;
}

// Room for `n` draws in a frame's list, which keeps its room from frame to frame: grown a
// quarter beyond what's asked, so a few more objects don't ask the heap for the whole block
// again (a megabyte and more), and left as it is if the heap can't give it (the list then grows
// as it goes). Asked for exactly, every frame on a heap broken into smaller pieces failed there
// and was dropped, on and on (the 2026-10-29 Vita tests: 1.54 MB, home with 2 Sims).
static void reserve_draws(std::vector<DrawCmd> &cmds, size_t n)
{
    if (n <= cmds.capacity())
        return;
    try {
        cmds.reserve(n + n / 4);
    } catch (const std::bad_alloc &) {
        try {
            cmds.reserve(n);
        } catch (const std::bad_alloc &) {
        }
    }
}

void compose_frame(const LotScene &scene, CharacterLibrary &library, float time_ms, DrawList &out)
{
    out.cmds.clear();
    reserve_draws(out.cmds, scene.list.cmds.size() + scene.people.size() * 1500);   // (a Sim: its triangles)
    size_t from = 0;
    for (const LotScene::PersonSlot &slot : scene.people) {
        out.cmds.insert(out.cmds.end(), scene.list.cmds.begin() + std::ptrdiff_t(from),
                        scene.list.cmds.begin() + std::ptrdiff_t(slot.at));
        from = slot.at;
        // Each Sim starts its loop at its own point, so they don't move in step.
        SimPose pose{ standing_idle(library, slot.look), time_ms + float(slot.id) * 977.0f };
        std::string problem;
        const size_t first = out.cmds.size();
        draw_sim(out, scene.projector, library, slot.look, slot.x, slot.y, slot.z, slot.direction, pose, problem);
        for (size_t i = first; i < out.cmds.size(); i++)
            out.cmds[i].owner = slot.id;   // so the cursor can pick the Sim
    }
    out.cmds.insert(out.cmds.end(), scene.list.cmds.begin() + std::ptrdiff_t(from), scene.list.cmds.end());
}

void compose_frame(const LotScene &scene, CharacterLibrary &library, const std::vector<LiveSim> &sims, DrawList &out)
{
    out.cmds.clear();
    reserve_draws(out.cmds, scene.list.cmds.size() + sims.size() * 1500);   // (a Sim: its triangles)
    // Each Sim goes after everything on its tile (levels above it are later in the list anyway).
    std::vector<std::pair<uint32_t, const LiveSim *>> at;
    for (const LiveSim &s : sims) {
        const int tx = std::clamp(int(std::floor(s.x + 0.5f)), 0, std::max(scene.edge - 1, 0));
        const int ty = std::clamp(int(std::floor(s.y + 0.5f)), 0, std::max(scene.edge - 1, 0));
        const int level = std::clamp(s.level, 0, kLotLevels - 1);
        const auto &ends = scene.tile_end[level];
        const size_t i = size_t(tx + ty * scene.edge);
        at.push_back({ i < ends.size() ? ends[i] : uint32_t(scene.list.cmds.size()), &s });
    }
    std::stable_sort(at.begin(), at.end(), [](auto &a, auto &b) { return a.first < b.first; });
    size_t from = 0;
    for (auto &[index, s] : at) {
        out.cmds.insert(out.cmds.end(), scene.list.cmds.begin() + std::ptrdiff_t(from),
                        scene.list.cmds.begin() + std::ptrdiff_t(index));
        from = index;
        SimPose pose{ s->animation, s->time_ms, s->weight, { s->layers[0], s->layers[1] } };
        pose.hold_end = s->hold_end;
        pose.carry = s->carry;
        pose.head_yaw = s->head_yaw;
        pose.light = s->light;
        pose.lamp_count = s->lamp_count;
        std::copy(std::begin(s->lamps), std::end(s->lamps), std::begin(pose.lamps));
        std::string problem;
        const size_t first = out.cmds.size();
        draw_sim(out, scene.projector, library, s->look, s->x, s->y, s->z, s->direction, pose, problem, s->colour);
        for (size_t i = first; i < out.cmds.size(); i++)
            out.cmds[i].owner = s->id;
    }
    out.cmds.insert(out.cmds.end(), scene.list.cmds.begin() + std::ptrdiff_t(from), scene.list.cmds.end());
}

ComposeProfile g_compose_profile;
double g_compose_time_left = -1;
bool g_pose_overlap = TS_ENV("TS_POSE_OVERLAP_OFF") == nullptr;

namespace {

// Drawn the same when posed the same, in the same place, look and colour.
uint64_t sim_version(const LiveSim &s)
{
    uint64_t v = 1469598103934665603ull;
    auto mix = [&v](const void *p, size_t n) {
        for (size_t k = 0; k < n; k++)
            v = (v ^ static_cast<const uint8_t *>(p)[k]) * 1099511628211ull;
    };
    auto mix_text = [&](const std::string &t) {
        mix(t.data(), t.size());
        mix("", 1);
    };
    mix(&s.x, sizeof s.x), mix(&s.y, sizeof s.y), mix(&s.z, sizeof s.z), mix(&s.direction, sizeof s.direction);
    mix(&s.animation, sizeof s.animation), mix(&s.time_ms, sizeof s.time_ms), mix(&s.hold_end, sizeof s.hold_end), mix(&s.colour, sizeof s.colour);
    mix(&s.weight, sizeof s.weight);
    mix(&s.carry, sizeof s.carry);
    mix(&s.light, sizeof s.light);
    mix(&s.lamp_count, sizeof s.lamp_count);
    for (int j = 0; j < s.lamp_count && j < 6; j++)
        mix(&s.lamps[j], sizeof s.lamps[j]);
    for (const SimPose::Layer &l : s.layers)
        mix(&l.animation, sizeof l.animation), mix(&l.time_ms, sizeof l.time_ms), mix(&l.weight, sizeof l.weight);
    mix_text(s.look.skeleton);
    for (const SimLook::Part &part : s.look.parts) {
        mix_text(part.suit);
        for (const auto &[tag, tex] : part.textures) {
            mix_text(tag);
            mix_text(tex);
        }
    }
    return v;
}

} // namespace

// The threads Sims are posed on: the caller's and two more (the Vita gives a game three
// cores; a PC has those to spare). TS_POSE_THREADS sets how many more (0: one thread).
WorkPool &pose_pool()
{
    static WorkPool pool([] {
        if (const char *n = TS_ENV("TS_POSE_THREADS"))
            return std::clamp(std::atoi(n), 0, 7);
        return 2;
    }());
    return pool;
}

bool draws_bounds(const DrawList &list, size_t first, size_t last, float &x0, float &y0, float &x1, float &y1)
{
    bool any = false;
    for (size_t i = first; i < last && i < list.cmds.size(); i++) {
        float a, b, c, d;
        list.cmds[i].bounds(a, b, c, d);
        if (!any) {
            x0 = a, y0 = b, x1 = c, y1 = d;
            any = true;
        } else {
            x0 = std::min(x0, a), y0 = std::min(y0, b), x1 = std::max(x1, c), y1 = std::max(y1, d);
        }
    }
    if (!any)
        x0 = y0 = x1 = y1 = 0;
    return any;
}

const char *volatile g_compose_step = "idle";
volatile int g_compose_item = 0;

// The pieces of its file an object's sprites would be read back from, for those taken out (as
// draw_object picks its sprites: the DGRP's image for its direction, zoom and the camera's turn).
static void sprite_windows(const LotObject &o, Content &content, Zoom zoom, int rotation, std::vector<FileRef> &out)
{
    const ObjectInfo *info = content.object(o.guid);
    const LoadedIff *f = info ? content.load(info->ref) : nullptr;
    if (!f || info->objd.get(Objd::ObjectType) == 2)
        return;
    const uint16_t dgrp_id = uint16_t(info->objd.get(Objd::BaseGraphicID) + o.graphic());
    const std::shared_ptr<const DrawGroup> group = draw_group(*info, *f, dgrp_id);
    const DrawImage *image = group ? pick_image(*group, 1u << (o.direction() & 7), zoom, rotation) : nullptr;
    if (!image)
        return;
    const uint16_t dyn_base = info->objd.get(Objd::DynamicSpriteBaseId), dyn_count = info->objd.get(Objd::NumDynamicSprites);
    for (const DrawSprite &s : image->sprites) {
        if (dyn_count && s.sprite_id >= dyn_base && s.sprite_id < uint32_t(dyn_base + dyn_count)) {
            const uint32_t bit = s.sprite_id - dyn_base;
            if (bit >= 64 || !((o.dynamic_sprites >> bit) & 1))
                continue;
        }
        const IffChunk *c = f->iff.find("SPR2", uint16_t(s.sprite_id));
        if (!c)
            c = f->iff.find("SPR#", uint16_t(s.sprite_id));
        FileRef w;
        if (c && !f->body(*c) && content.chunk_window(*f, *c, w))
            out.push_back(w);
    }
}

void compose_frame(const LotScene &scene, const Lot &lot, const std::vector<LotObject> &objects, Content &content,
                   SpriteCache &sprites, ObjectDrawCache &cache, const std::vector<LiveSim> &sims, DrawList &out,
                   std::vector<FrameItem> *items, std::vector<DrawBounds> *out_bounds, const DrawBounds *view)
{
    g_compose_step = "start";
    g_compose_item = 0;
    using PClock = ts::FastClock;
    auto since = [](PClock::time_point t) { return std::chrono::duration<double, std::milli>(PClock::now() - t).count(); };
    const auto t_objects = PClock::now();
    double sims_ms = 0;
    g_compose_profile.frames++;
    // The frame's deadline (g_compose_time_left): how much of it is left now; no limit under 0.
    const double left_at_start = g_compose_time_left;
    auto time_left = [&] { return left_at_start < 0 ? 1e9 : left_at_start - since(t_objects); };
    bool lean = false;
    if (cache.generation != scene.generation) {
        // Another camera or wall mode: everything drawn afresh. Only the cutaway's rooms changed
        // (the cursor into another room, the walls cut again): the draws kept, placed again, and
        // the objects the cutaway can hide looked at again. (Drawing everything afresh then, the
        // 2026-10-27 Vita log's hitches: 14-31 ms a frame of sprites decoded and read back from
        // the card each time the cutaway followed the cursor.)
        if (scene.look_generation == 0 || scene.look_generation != cache.look_generation) {
            cache.entries.clear();
            cache.sims.clear();
            g_compose_profile.scenes_new++;
        } else {
            g_compose_profile.scenes_kept++;
        }
        cache.generation = scene.generation;
        cache.look_generation = scene.look_generation ? scene.look_generation : scene.generation;
        cache.placed_version = 0;
    }
    for (auto &entry : cache.sims)
        entry.second.used = false;
    if (items)
        items->clear();
    // Each object's draws, redrawn only when something that shows changed.
    struct Insert {
        uint32_t at;
        int order;   // by tile back to front; on a tile, objects (by depth) before Sims
        const std::vector<DrawCmd> *cmds;
        const LiveSim *sim;
        const std::vector<DrawBounds> *bounds = nullptr;
    };
    std::vector<Insert> inserts;
    constexpr int kShadowOrder = std::numeric_limits<int>::min();
    // The Sims in view, posed (those whose pose changed): each on its own thread of the pool.
    std::function<void(int)> pose_job;
    std::vector<const LiveSim *> shown;
    for (const LiveSim &s : sims) {
        if (view) {   // far off the screen (a Sim stands well under 400 pixels tall at any zoom)
            float fx, fy;
            scene.projector.screen(s.x, s.y, s.z, fx, fy);
            constexpr float kMargin = 400;
            if (fx < view->x0 - kMargin || fx > view->x1 + kMargin || fy < view->y0 - kMargin || fy > view->y1 + kMargin)
                continue;
        }
        shown.push_back(&s);
    }
    struct PoseJob {
        const LiveSim *sim;
        uint64_t version;
        ObjectDrawCache::SimEntry *kept;
    };
    std::vector<PoseJob> pose_jobs;
    for (const LiveSim *sp : shown) {
        const LiveSim &s = *sp;
        const uint64_t v = sim_version(s);
        ObjectDrawCache::SimEntry &kept = cache.sims[s.id];
        kept.used = true;
        if (kept.version != v || !kept.posed)
            pose_jobs.push_back({ &s, v, &kept });
    }
    // Late: only the Sims never posed, and half of the rest, the longest since their last pose
    // first (the others keep the pose they had: a tick behind, for one frame). Posing is ~0.5 ms
    // a Sim on the Vita, and at Solana's 11 Sims it's what a late frame can best spare.
    // At most `cap` Sims posed a frame, the longest-waiting first (the rest the next frame):
    // 25 Sims at downtown were 12 ms of the frame on two threads, and at the 20 fps that made,
    // the late-frame rule below was deferring half of them anyway. A late frame still halves.
    // In order of need (never posed first, then the longest since their last pose): the poses
    // are taken in this order, and when the frame runs short (finish, below) the ones left
    // undone are the least urgent.
    constexpr size_t cap = 12;
    std::stable_sort(pose_jobs.begin(), pose_jobs.end(), [](const PoseJob &a, const PoseJob &b) {
        if (a.kept->posed != b.kept->posed)
            return !a.kept->posed;
        return a.kept->posed_frame < b.kept->posed_frame;
    });
    if (pose_jobs.size() > 2 && (pose_jobs.size() > cap || time_left() < 6.0 + 0.5 * double(pose_jobs.size()))) {
        size_t keep = (pose_jobs.size() + 1) / 2;
        if (pose_jobs.size() > cap)
            keep = std::min(keep, cap);
        while (keep < pose_jobs.size() && !pose_jobs[keep].kept->posed)
            keep++;
        g_compose_profile.sims_deferred += int(pose_jobs.size() - keep);
        pose_jobs.resize(keep);
        lean = true;
    }
    // Started now, on the pool's workers, and finished after the objects' compose below: the
    // poses run under it (the main thread's ~5 ms of placing, looks and shadows), where before
    // they came after it, 7 of ~2.7 ms on three threads as 8-10 ms of a frame at lot 26's
    // restaurant on the Vita (2026-11-07).
    g_compose_step = "posing the Sims";
    g_compose_item = int(pose_jobs.size());
    pose_job = [&](int i) {
        const PoseJob &job = pose_jobs[size_t(i)];
        const LiveSim &s = *job.sim;
        g_compose_item = s.id;
        SimPose pose{ s.animation, s.time_ms, s.weight, { s.layers[0], s.layers[1] } };
        pose.hold_end = s.hold_end;
        pose.carry = s.carry;
        pose.head_yaw = s.head_yaw;
        pose.light = s.light;
        pose.lamp_count = s.lamp_count;
        std::copy(std::begin(s.lamps), std::end(s.lamps), std::begin(pose.lamps));
        std::string problem;
        ObjectDrawCache::SimEntry &kept = *job.kept;
        if (!draw_sim_mesh(*kept.mesh, scene.projector, sprites.characters(), s.look, s.x, s.y, s.z, s.direction, pose, problem,
                           s.colour, &kept.lookups) &&
            TS_ENV("TS_SIM_PROBLEM"))   // debug: why a Sim isn't drawn
            std::printf("can't draw Sim %d: %s\n", int(s.id), problem.c_str());
        if (TS_ENV("TS_SIM_PROBLEM")) {   // debug: what was made of it
            const DrawMesh &dm = *kept.mesh;
            uint32_t a0 = dm.tris.empty() ? 0 : dm.tris[0].tint[0];
            size_t untextured = 0;
            for (const MeshTri &t : dm.tris)
                untextured += t.texture == nullptr || !t.texture->width;
            std::printf("Sim %d: %zu of its triangles without a texture; at lot (%.2f, %.2f, %.2f) level %d\n", int(s.id),
                        untextured, s.x, s.y, s.z, s.level);
            std::printf("Sim %d: %zu triangles, bounds %.0f,%.0f - %.0f,%.0f, first tint %08x, light %.2f lamps %d\n", int(s.id),
                        dm.tris.size(), dm.x0, dm.y0, dm.x1, dm.y1, a0, s.light, s.lamp_count);
        }
        kept.version = job.version;
        kept.posed = true;
        kept.posed_frame = g_compose_profile.frames;
    };
    pose_pool().start(int(pose_jobs.size()), pose_job);
    // Finished whatever leaves this function: a bad_alloc out of the objects' compose (the Vita drops
    // the frame) left the pool's workers on pose_job, a local gone, and its jobs' meshes.
    struct PosesFinished {
        ~PosesFinished() { pose_pool().finish([] { return true; }); }   // (what's not begun stays undone)
    } poses_finished;
    if (!g_pose_overlap) {   // (the remote's "overlap 0": the poses done here, before the objects, as an A/B)
        const auto t_sim = PClock::now();
        const int undone = pose_pool().finish([&] { return time_left() < 2.0; });
        if (undone > 0) {
            g_compose_profile.sims_deferred += undone;
            lean = true;
        }
        sims_ms += since(t_sim);
    }
    const bool same_objects = cache.objects_version != 0 && cache.objects_version == cache.placed_version &&
                              !cache.pending_left;
    if (!same_objects) {
    cache.entries.for_each([](uint16_t, ObjectDrawCache::Entry &e) { e.used = false; });
    cache.placed.clear();
    cache.placed_items.clear();
    cache.placed_in_order = 0;
    g_compose_step = "placing the objects";
    auto t_part = PClock::now();
    // Where each object is: kept while the objects' places are the same (cache.position_version),
    // pointed at the caller's list again; else placed anew. Then in the frame's order for this
    // scene: by the tile's place back to front, and on a tile what's on the floor before what's in
    // slots (sorted once here, so the frame's list below needs only the shadows and the Sims
    // merged in, not a sort of all ~600 draws a frame).
    if (!cache.placed_on)
        cache.placed_on = std::make_shared<PlacedList>();
    PlacedList &kept_places = *cache.placed_on;
    if (cache.position_version == 0 || kept_places.version != cache.position_version || kept_places.objects != objects.size()) {
        kept_places.items = place_objects(lot, objects, content, cache.stats, &cache.slots);
        kept_places.free_index.clear();
        for (size_t k = 0; k < kept_places.items.size();) {
            if (kept_places.items[k].pl.object->free) {
                kept_places.free_index.push_back(kept_places.items[k].index);
                kept_places.items.erase(kept_places.items.begin() + std::ptrdiff_t(k));
            } else {
                k++;
            }
        }
        kept_places.version = cache.position_version;
        kept_places.objects = objects.size();
        kept_places.generation = 0;
    } else {
        for (PlacedOn &on : kept_places.items)
            on.pl.object = &objects[on.index];
    }
    auto order_key = [&](const PlacedOn &on, uint32_t &at, int &order) {
        const auto &ends = scene.tile_end[std::clamp(on.level, 0, kLotLevels - 1)];
        const size_t i = size_t(on.tx + on.ty * scene.edge);
        const int rank = i < scene.tile_rank.size() ? int(scene.tile_rank[i]) : 0;
        at = i < ends.size() ? ends[i] : uint32_t(scene.list.cmds.size());
        order = rank * 2048 + on.pl.depth;
    };
    if (kept_places.generation != scene.generation) {
        std::stable_sort(kept_places.items.begin(), kept_places.items.end(), [&](const PlacedOn &a, const PlacedOn &b) {
            uint32_t at_a, at_b;
            int order_a, order_b;
            order_key(a, at_a, order_a);
            order_key(b, at_b, order_b);
            return at_a != at_b ? at_a < at_b : order_a < order_b;
        });
        kept_places.generation = scene.generation;
    }
    // The frame's placements: the kept ones, then the free objects where their hands are now (as
    // place_objects puts them: on the tile under the hand, as if in a slot).
    thread_local std::vector<PlacedOn> placed;
    placed.assign(kept_places.items.begin(), kept_places.items.end());
    cache.placed_sorted = placed.size();
    for (const uint32_t index : kept_places.free_index) {
        if (index >= objects.size())
            continue;
        const LotObject &o = objects[index];
        const int tx = std::clamp(int(std::floor(o.fx)), 0, lot.size - 1);
        const int ty = std::clamp(int(std::floor(o.fy)), 0, lot.size - 1);
        const float above = o.fz - lot.ground(float(tx), float(ty));
        const int level = std::clamp(int(std::lround(above / kLevelHeight - 0.25f)), 0, kLotLevels - 1);
        placed.push_back({ Placed{ &o, o.fx, o.fy, o.fz, 1 }, level, tx, ty, index });
    }
    g_compose_profile.place_ms += since(t_part);
    t_part = PClock::now();
    g_compose_step = "object";
    struct Job {
        ObjectDrawCache::Entry *e;
        const PlacedOn *on;
        float dist;
        DrawList list;
        PaintStats stats;
        double ms;
        bool drawn = false;
        // Its appearance (instancing): the key, where its tile is on screen and how deep, and
        // whether its draws came from another object's.
        uint64_t key = 0;
        float bx = 0, by = 0, depth = 0;
        bool instanced = false;
    };
    std::vector<Job> jobs;
    std::vector<ObjectDrawCache::Entry *> entry_of;   // each placed object's entry, in `placed` order
    entry_of.reserve(placed.size());
    const float mid_x = view ? (view->x0 + view->x1) / 2 : 0.0f, mid_y = view ? (view->y0 + view->y1) / 2 : 0.0f;
    for (const PlacedOn &on : placed) {
        const LotObject &o = *on.pl.object;
        g_compose_item = o.id;
        ObjectDrawCache::Entry *found = cache.entries.find(o.id);
        bool same = found && found->guid == o.guid &&
                          found->graphic == o.graphic() && found->direction == o.direction() &&
                          found->flags == o.data[kObjFlags] && found->placement == o.data[kObjWallPlacement] &&
                          found->x == on.pl.x && found->y == on.pl.y && found->z == on.pl.z &&
                          found->level == on.level && found->dynamic_sprites == o.dynamic_sprites;
        // (A pending entry, its draws not made yet, is the same entry still: it stays pending and
        // keeps its count of waits. Counted as different, it was made anew every frame, its waits
        // back to 0, so an object whose sprites the read-ahead had let go waited for ever, and at
        // Solana some 160 objects were queued again every frame, 24 looked at, none drawn: 6 ms a
        // frame for nothing, the compose counters showed, 2026-10-30.)
        // Kept from a scene before (the cutaway moved): drawn again only if it's hidden now and
        // wasn't then, or the other way round.
        bool cut_known = false, cut_now = false;
        if (same && found->cut_for != scene.generation) {
            const auto t_cut = PClock::now();
            g_compose_profile.cut_calls++;
            cut_now = hidden_by_cutaway(o, on.tx, on.ty, on.level, lot, *scene.walls, content, *scene.code);
            g_compose_profile.cutaway_ms += since(t_cut);
            cut_known = true;
            if (cut_now != found->cut_hidden)
                same = false;
            else
                found->cut_for = scene.generation;
        }
        ObjectDrawCache::Entry &e = cache.entries[o.id];
        entry_of.push_back(&e);
        // Its cutaway answer from this frame's scene already (an object animating every tick is
        // drawn afresh every frame, and the check cost 0.65 ms each on the Vita: 1.3 ms a frame at
        // Solana): kept while what the check reads is the same (tile, storey, flags, wall
        // placement, facing, the object's kind).
        const bool cut_reuse = found && !cut_known && found->cut_for == scene.generation && found->guid == o.guid &&
                               found->tx == on.tx && found->ty == on.ty && found->level == on.level &&
                               found->flags == o.data[kObjFlags] && found->placement == o.data[kObjWallPlacement] &&
                               found->direction == o.direction();
        if (cut_reuse) {
            cut_known = true;
            cut_now = found->cut_hidden;
        }
        g_compose_profile.placed_n++;
        if (!same) {
            g_compose_profile.remade_n++;
            // Its draws from before kept while the new ones wait for their sprites, when it's
            // where it was (an object animating: the shower's water; it vanished for the frames
            // the read-ahead took, the Sim in it shown through, the user saw, 2026-10-30).
            const bool keep = found && !found->pending && !found->cmds.empty() && found->x == on.pl.x && found->y == on.pl.y &&
                              found->z == on.pl.z && found->level == on.level && found->direction == o.direction() &&
                              found->placement == o.data[kObjWallPlacement];
            ObjectDrawCache::Entry old;
            if (keep) {
                old.cmds = std::move(e.cmds);
                old.bounds = std::move(e.bounds);
                old.version = e.version;
                old.x0 = e.x0, old.y0 = e.y0, old.x1 = e.x1, old.y1 = e.y1;
            }
            e = ObjectDrawCache::Entry();
            if (keep) {
                e.cmds = std::move(old.cmds);
                e.bounds = std::move(old.bounds);
                e.version = old.version;
                e.x0 = old.x0, e.y0 = old.y0, e.x1 = old.x1, e.y1 = old.y1;
            }
            e.guid = o.guid;
            e.graphic = int16_t(o.graphic());
            e.direction = int16_t(o.direction());
            e.flags = o.data[kObjFlags];
            e.placement = o.data[kObjWallPlacement];
            e.x = on.pl.x;
            e.y = on.pl.y;
            e.z = on.pl.z;
            e.level = on.level;
            e.dynamic_sprites = o.dynamic_sprites;
            e.tx = on.tx;
            e.ty = on.ty;
            bool cut_hidden = cut_now;
            if (!cut_known) {   // (timed only when run: the clock's own cost showed as 1 ms a frame)
                const auto t_cut = PClock::now();
                g_compose_profile.cut_calls++;
                cut_hidden = hidden_by_cutaway(o, on.tx, on.ty, on.level, lot, *scene.walls, content, *scene.code);
                g_compose_profile.cutaway_ms += since(t_cut);
            }
            e.cut_hidden = cut_hidden;
            e.cut_for = scene.generation;
            if (cut_hidden) {   // (nothing of it shows)
                e.cmds.clear();
                e.bounds.clear();
            }
            if (TS_ENV("TS_OBJDBG"))   // debug: objects whose label has this, drawn or not and why
                if (const ObjectInfo *di = content.object(o.guid);
                    di && di->label.find(TS_ENV("TS_OBJDBG")) != std::string::npos)
                    std::printf("object #%d %s at (%d, %d) level %d, graphic %d direction %d, category %d: %s\n", int(o.id),
                                di->label.c_str(), on.tx, on.ty, on.level, int(o.graphic()), int(o.direction()),
                                59 < ObjDataCount ? int(o.data[59]) : -1, cut_hidden ? "hidden by the cutaway" : "drawn");
            if (e.cmds.empty())
                e.version = ++cache.next_version;
            e.pending = !cut_hidden;
        }
        e.used = true;
        if (TS_ENV("TS_CUT_CHECK")) {   // debug: the kept cutaway answer against a fresh one, each frame
            const bool fresh = hidden_by_cutaway(o, on.tx, on.ty, on.level, lot, *scene.walls, content, *scene.code);
            if (fresh != e.cut_hidden || (fresh && !e.cmds.empty()))
                if (const ObjectInfo *di = content.object(o.guid))
                    std::printf("cut check: #%d %s at (%d, %d): fresh %d, kept %d, draws %lu, generation %u, cut_for %u\n",
                                int(o.id), di->label.c_str(), on.tx, on.ty, int(fresh), int(e.cut_hidden),
                                (unsigned long)e.cmds.size(), scene.generation, e.cut_for);
        }
        if (e.pending) {   // its draws made below: the nearest the view's middle first
            float bx, by;
            scene.projector.box(on.pl.x, on.pl.y, on.pl.z, bx, by);
            const float dx = bx - mid_x, dy = by - mid_y;
            jobs.push_back({ &e, &on, dx * dx + dy * dy, {}, {}, 0 });
        }
    }
    // The draws made afresh: on the work pool's threads (the Vita gives three cores) in batches,
    // until this frame's budget is spent (TS_REDRAW_MS; the Vita 20 ms, the PC none: a shot is
    // whole); the rest stay pending and come over the next frames, the nearest the view first.
    // A camera turn at Solana drew 452 objects afresh one after another, 2.2 s in one frame.
    g_compose_step = "drawing objects afresh";
    g_compose_profile.entries_ms += since(t_part);
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job &a, const Job &b) { return a.dist < b.dist; });
    static const double fixed_budget_ms = [] {
        if (const char *b = TS_ENV("TS_REDRAW_MS"))
            return std::atof(b);
#if defined(__vita__)
        return 20.0;
#else
        return 0.0;
#endif
    }();
    // With a deadline: what's left of the frame after the Sims, the lighting and the drawing to
    // come (~12 ms at Solana), at least 3 ms so pending objects always come in; else the fixed budget.
    const double budget_ms = left_at_start < 0 || TS_ENV("TS_REDRAW_MS")
                                 ? fixed_budget_ms
                                 : std::clamp(time_left() - 12.0, 3.0, fixed_budget_ms);
    if (left_at_start >= 0 && budget_ms < fixed_budget_ms && jobs.size() > 1)
        lean = true;
    // With a reader (the viewer's thread: cache.read_ahead), the card is its alone: an object whose
    // sprites are taken out of its file and not read ahead yet asks for them and waits (up to two
    // seconds, then reads them itself), so a batch never waits on the card (three threads reading
    // it beside the reader were a frame of 940 ms); the ready ones are drawn, three at a time.
    const auto t_jobs = PClock::now();
    const size_t batch = size_t(pose_pool().workers() + 1);
    std::vector<Job *> ready;
    size_t drawn = 0, looked = 0, skipped = 0;
    auto run_batch = [&] {
        auto draw_one = [&](int i) {
            Job &j = *ready[size_t(i)];
            const auto t = PClock::now();
            draw_object(j.list, scene.projector, content, sprites, j.on->pl, scene.zoom, scene.rotation, j.stats);
            j.ms = since(t);
            j.drawn = true;
        };
        if (pose_pool().busy())   // (the Sims' poses on the workers: these few here)
            for (int i = 0; i < int(ready.size()); i++)
                draw_one(i);
        else
            pose_pool().run(int(ready.size()), draw_one);
        drawn += ready.size();
        ready.clear();
    };
    g_compose_profile.jobs_n += int(jobs.size());
    // What was asked of the read-ahead is asked again after a while: the store lets go of what
    // wasn't used, and an object waiting on sprites let go waited for ever.
    if (++cache.requested_age >= 60) {
        cache.requested.clear();
        cache.requested_age = 0;
    }
    for (Job &j : jobs) {
        if (budget_ms > 0 && (since(t_jobs) > budget_ms || looked > 24))   // (the rest another frame)
            break;
        // Late (the deadline): an object well off the screen isn't drawn afresh this frame. The
        // tills and signs at Solana animate every tick, and drawing the ones out of view again
        // each frame was ~1 ms apiece; pending, they're drawn as soon as the view reaches them.
        // (Not counted as looked at: some 200 of them queue up, and the ones on screen behind
        // them in the list must still be reached.)
        if (lean && view) {
            float bx, by;
            scene.projector.box(j.on->pl.x, j.on->pl.y, j.on->pl.z, bx, by);
            constexpr float kMargin = 400;
            if (bx < view->x0 - kMargin || bx > view->x1 + kMargin || by < view->y0 - kMargin || by > view->y1 + kMargin) {
                g_compose_profile.jobs_offscreen++;
                if (++skipped > 64)   // (nearest first: past this many off screen, the rest are further out)
                    break;
                continue;
            }
        }
        looked++;
        {   // An object of the same type in the same state already composed this view: its draws, moved.
            const LotObject &o = *j.on->pl.object;
            uint64_t key = 1469598103934665603ull;
            auto mix = [&key](uint64_t v) {
                key ^= v;
                key *= 1099511628211ull;
            };
            mix(o.guid);
            mix(uint64_t(uint16_t(j.e->graphic)) | uint64_t(uint16_t(j.e->direction)) << 16 | uint64_t(uint16_t(j.e->flags)) << 32 |
                uint64_t(uint16_t(j.e->placement)) << 48);
            mix(j.e->dynamic_sprites);
            // (The view's zoom and rotation, not the scene's generation: a rebuilt scene — the cutaway
            // changing as Sims cross rooms, every second or two at the shop — would throw every
            // appearance away; the offsets it brings are in the box difference.)
            mix(uint64_t(int(scene.zoom)) | uint64_t(uint32_t(scene.rotation & 3)) << 8 | uint64_t(uint32_t(j.on->level)) << 32);
            j.key = key;
            scene.projector.box(j.on->pl.x, j.on->pl.y, j.on->pl.z, j.bx, j.by);
            j.depth = scene.projector.depth(j.on->pl.x, j.on->pl.y, j.on->pl.z);
            static const bool no_instance = TS_ENV("TS_NO_INSTANCE") != nullptr;   // debug: every object composed itself
            auto found = no_instance ? cache.appearances.end() : cache.appearances.find(key);
            if (found != cache.appearances.end()) {
                const ObjectDrawCache::Appearance &a = found->second;
                const float fdx = j.bx - a.bx, fdy = j.by - a.by;
                const float dx = std::round(fdx), dy = std::round(fdy);
                if (std::fabs(fdx - dx) < 0.01f && std::fabs(fdy - dy) < 0.01f) {   // whole pixels apart: the same rounding
                    const float dd = j.depth - a.depth;
                    j.list.cmds = a.cmds;
                    for (DrawCmd &c : j.list.cmds) {
                        c.x += int(dx);
                        c.y += int(dy);
                        c.owner = o.id;
                        if (c.kind == DrawKind::Triangle)
                            for (int k = 0; k < 3; k++) {
                                c.px[k] += dx;
                                c.py[k] += dy;
                            }
                        switch (c.depth) {
                        case DepthMode::Sprite: c.dc += dd; break;
                        case DepthMode::Plane: c.dc += dd - c.da * dx - c.db * dy; break;
                        case DepthMode::Corners:
                        case DepthMode::Own:
                            for (float &v : c.dv)
                                v += dd;
                            break;
                        default: break;
                        }
                    }
                    j.ms = 0;
                    j.drawn = true;
                    j.instanced = true;
                    drawn++;
                    g_compose_profile.jobs_instanced++;
                    continue;
                }
            }
        }
        if (cache.read_ahead && j.e->waits < 60) {
            std::vector<FileRef> windows;
            sprite_windows(*j.on->pl.object, content, scene.zoom, scene.rotation, windows);
            bool missing = false;
            for (const FileRef &w : windows)
                if (!read_ahead_has(w)) {
                    missing = true;
                    if (cache.requested.insert(std::make_tuple(w.path, w.offset, w.size)).second)
                        cache.read_ahead(w);
                }
            if (missing) {
                j.e->waits++;
                g_compose_profile.jobs_waiting++;
                continue;
            }
        }
        ready.push_back(&j);
        if (ready.size() == batch)
            run_batch();
    }
    if (!ready.empty())
        run_batch();
    static const bool afresh_log = TS_ENV("TS_AFRESH") != nullptr;   // debug: each object composed anew (not instanced), with its cost
    for (Job &j : jobs) {
        if (!j.drawn)
            continue;
        ObjectDrawCache::Entry &e = *j.e;
        const PlacedOn &on = *j.on;
        g_compose_profile.draw_object_ms += j.ms;
        g_compose_profile.objects_drawn++;
        if (afresh_log && !j.instanced) {
            const LotObject &o = *on.pl.object;
            const ObjectInfo *di = content.object(o.guid);
            std::printf("afresh: #%d %s at (%d, %d) graphic %d direction %d dyn %llx: %.2f ms, %lu draws\n", int(o.id),
                        di ? di->label.c_str() : "?", on.tx, on.ty, int(o.graphic()), int(o.direction()),
                        (unsigned long long)o.dynamic_sprites, j.ms, (unsigned long)j.list.cmds.size());
        }
        cache.stats.objects_drawn += j.stats.objects_drawn;
        cache.stats.objects_no_art += j.stats.objects_no_art;
        cache.stats.people += j.stats.people;
        for (std::string &note : j.stats.notes)
            cache.stats.notes.push_back(std::move(note));
        {   // the room of the tile it's on
            const int lv = std::clamp(on.level, 0, kLotLevels - 1);
            const uint16_t r = camera_room(scene.rooms[lv], lot, scene.projector, lv, on.tx, on.ty);
            for (DrawCmd &cmd : j.list.cmds)
                cmd.room = light_room(lv, r);
        }
        draws_bounds(j.list, 0, j.list.cmds.size(), e.x0, e.y0, e.x1, e.y1);
        e.bounds.clear();
        for (const DrawCmd &cmd : j.list.cmds)
            e.bounds.push_back(bounds_of(cmd));
        if (!j.instanced && j.key) {   // the first of its kind in this state: its draws kept for the others
            if (cache.appearances.size() >= 768)   // (a lot's worth; stale views and states go with them)
                cache.appearances.clear();
            ObjectDrawCache::Appearance &a = cache.appearances[j.key];
            a.cmds = j.list.cmds;
            a.bounds = e.bounds;
            a.bx = j.bx;
            a.by = j.by;
            a.depth = j.depth;
        }
        e.cmds = std::move(j.list.cmds);
        e.version = ++cache.next_version;
        e.pending = false;
    }
    cache.pending_left = drawn < jobs.size();
    if (!cache.pending_left)
        cache.requested.clear();
    g_compose_step = "placing the draws";
    g_compose_profile.jobs_ms += since(t_jobs);
    const auto t_place_draws = PClock::now();
    for (size_t k = 0; k < placed.size(); k++) {
        const PlacedOn &on = placed[k];
        const LotObject &o = *on.pl.object;
        const ObjectDrawCache::Entry &e = *entry_of[k];
        if (e.cmds.empty() || on.level > scene.top_level || on.tx < 0 || on.ty < 0 || on.tx >= scene.edge ||
            on.ty >= scene.edge)
            continue;
        if (TS_ENV("TS_ROOFED"))   // debug
            std::printf("roofed? %s #%d at (%d, %d) storey %d: %s\n",
                        content.object(o.guid) ? content.object(o.guid)->label.c_str() : "?", int(o.id), on.tx, on.ty, on.level,
                        scene.roofed[std::clamp(on.level, 0, kLotLevels - 1)].empty() ? "no roof"
                        : scene.roofed[std::clamp(on.level, 0, kLotLevels - 1)][size_t(on.tx + on.ty * scene.edge)] ? "1" : "0");
        static const bool keep_roofed = TS_ENV("TS_ROOF_KEEP") != nullptr;   // debug: draw them anyway
        if (const auto &roofed = scene.roofed[std::clamp(on.level, 0, kLotLevels - 1)];
            !keep_roofed && !roofed.empty() && roofed[size_t(on.tx + on.ty * scene.edge)])
            continue;   // under the roof shown (see LotScene::roofed)
        cache.placed_items.push_back({ uint32_t(o.id), e.version, e.x0, e.y0, e.x1, e.y1 });
        // Ordered by the tile's place back to front, then what's on the floor before what's in
        // slots (`placed` is sorted so above: cache.placed comes out in the frame's order).
        uint32_t at;
        int order;
        order_key(on, at, order);
        if (k < cache.placed_sorted)   // (a kept placement, in order; the free ones come after, merged in below)
            cache.placed_in_order = cache.placed.size() + 1;
        cache.placed.push_back({ at, order, &e.cmds, &e.bounds });
    }
    g_compose_profile.place_draws_ms += since(t_place_draws);
    cache.entries.erase_unused();   // objects that are gone
    cache.placed_version = cache.objects_version;
    g_compose_profile.redraw_ms += since(t_part);
    t_part = PClock::now();
    // The shadows: on each tile, the first object there that casts one (0x53b5b0: not hidden,
    // not without extent, not a person or the cursor).
    g_compose_step = "shadows";
    static const bool no_shadows = TS_ENV("TS_NO_SHADOWS") != nullptr;   // debug
    // Made again only when what they're made of changed: where objects stand and face, their
    // shadow flags, the view. Downtown something animates every tick (the chef, the tills), which
    // placed the objects again but left their shadows as they were.
    uint64_t key = 1469598103934665603ull;
    auto mix = [&key](int64_t v) {
        key ^= uint64_t(v);
        key *= 1099511628211ull;
    };
    mix(scene.generation);
    mix(scene.top_level);
    // Debug (TS_SHADOW_WHY=1): what changed the fingerprint, object by object, the first 40 times.
    static const bool shadow_why = TS_ENV("TS_SHADOW_WHY") != nullptr;
    static std::map<uint16_t, std::pair<uint64_t, std::string>> why_last;
    std::map<uint16_t, std::pair<uint64_t, std::string>> why_now;
    for (const PlacedOn &on : placed) {
        const LotObject &o = *on.pl.object;
        if (on.pl.depth != 0 || o.x < 0 || o.free)
            continue;
        const int64_t a = int64_t(o.id) << 32 | o.guid;
        const int64_t b = int64_t(o.x) << 48 | int64_t(uint16_t(o.y)) << 32 | int64_t(uint16_t(o.level)) << 16 | uint16_t(on.level);
        const int64_t c = int64_t(uint16_t(o.direction())) << 32 | int64_t(uint16_t(o.data[34])) << 16 | (o.data[kObjFlags] & 4);
        mix(a);
        mix(b);
        mix(c);
        if (shadow_why)
            why_now[o.id] = { uint64_t(a) * 31 + uint64_t(b) * 17 + uint64_t(c),
                              "(" + std::to_string(o.x) + "," + std::to_string(o.y) + " l" + std::to_string(o.level) + " on" +
                                  std::to_string(on.level) + " d" + std::to_string(o.direction()) + " h" + std::to_string(o.data[34]) +
                                  " f" + std::to_string(o.data[kObjFlags] & 4) + ")" };
    }
    if (shadow_why && key != cache.shadows_key && cache.shadows_key != 0) {
        static int told = 0;
        if (told < 40) {
            told++;
            std::string what;
            int n = 0;
            for (const auto &[id, v] : why_now) {
                const auto it = why_last.find(id);
                if (it == why_last.end() || it->second.first != v.first)
                    what += (n++ < 4 ? " #" + std::to_string(id) + " " + (it == why_last.end() ? v.second + " new" : it->second.second + " -> " + v.second) : "");
            }
            for (const auto &[id, v] : why_last)
                if (!why_now.count(id))
                    what += (n++ < 6 ? " #" + std::to_string(id) + " " + v.second + " (gone)" : "");
            std::printf("shadows made again (%d changed):%s\n", n, what.c_str());
        }
    }
    if (shadow_why)
        why_last = std::move(why_now);
    const bool shadows_changed = key != cache.shadows_key || cache.shadows_key == 0;
    if (shadows_changed && cache.shadows_key != 0 && time_left() < 8.0) {
        // Late: the shadows as they were this frame, made again next frame (the key kept old, so
        // they are); a shadow a frame late is nothing to the eye. Downtown something's hidden flag
        // changes most ticks, and remaking them all is 6 ms on the Vita.
        g_compose_profile.shadows_deferred++;
        lean = true;
    } else if (shadows_changed) {
    cache.shadows_key = key;
    g_compose_profile.shadow_rebuilds++;
    for (int l = 0; l < kLotLevels; l++) {
        cache.shadows[l].clear();
        cache.shadow_bounds[l].clear();
        cache.shadow_items[l].clear();
    }
    std::set<std::pair<int, int>> shaded[kLotLevels];
    // The tiles' objects, for the multi-tile shapes (shadow_code): made only if an object's shadow
    // has to be worked out afresh.
    TileObjects by_tile;
    // Each multi-tile group's state (its parts' ids and places): a part's shadow shape depends on
    // its group's other parts, so a part's memo holds while its group is as it was.
    static const bool memo_off = TS_ENV("TS_SHADOW_MEMO_OFF") != nullptr;   // debug: every shadow afresh, as before
    const auto t_pre = PClock::now();
    std::unordered_map<uint64_t, uint64_t> group_fp;
    auto group_of = [&](const ObjectInfo &info) -> uint64_t {
        const uint16_t master = info.objd.get(Objd::MasterID);
        if (master == 0)
            return 0;
        return std::hash<std::string>{}(info.ref.path) * 31 + uint64_t(info.ref.offset) * 7 + master;
    };
    // Each placed object's info looked up once (the objects' pass reuses it), the multi-tile parts'
    // groups and tiles noted: shadow_code looks only for parts of the same object, so its tile map
    // holds only those (all 760 objects of a downtown lot were put in it at every rebuild).
    std::vector<const ObjectInfo *> infos(placed.size(), nullptr);
    std::vector<uint64_t> groups(placed.size(), 0);
    for (size_t k = 0; k < placed.size(); k++) {
        const LotObject &o = *placed[k].pl.object;
        if (o.x < 0)
            continue;
        const ObjectInfo *info = content.object(o.guid);
        infos[k] = info;
        if (!info)
            continue;
        if (const uint64_t g = group_of(*info)) {
            groups[k] = g;
            uint64_t &h = group_fp[g];
            h = (h ^ (uint64_t(o.id) << 40 | uint64_t(uint16_t(o.x)) << 24 | uint64_t(uint16_t(o.y)) << 8 | uint64_t(uint8_t(o.level)))) *
                1099511628211ull;
            by_tile[tile_key(o.level, o.x, o.y)].push_back(&o);
        }
    }
    g_compose_profile.shadow_pre_ms += since(t_pre);
    const auto t_loop = PClock::now();
    const uint32_t stamp = ++cache.shadow_stamp;
    for (size_t k = 0; k < placed.size(); k++) {
        const PlacedOn &on = placed[k];
        const LotObject &o = *on.pl.object;
        if (no_shadows || on.pl.depth != 0 || o.x < 0 || o.free || on.level < 0 || on.level >= kLotLevels ||
            on.level > scene.top_level || shaded[on.level].count({ o.x, o.y }))
            continue;
        const ObjectInfo *info = infos[k];
        if (!info || o.data[34] != 0 || (o.data[kObjFlags] & 4))
            continue;
        const uint16_t type = info->objd.get(Objd::ObjectType);
        if (type == 2 || type == 9)
            continue;
        if (info->objd.get(Objd::LevelOffset) != 0)
            continue;   // a part on the storey above (the stairs' top): its shadow would fall in the stairwell
        // What this shadow is made from: the object as it stands, its group, the view.
        uint64_t fp = 1469598103934665603ull;
        for (const int64_t v : { int64_t(o.id) << 32 | o.guid, int64_t(o.x) << 32 | uint32_t(o.y), int64_t(o.level) << 32 | uint32_t(on.level),
                                 int64_t(o.direction()), int64_t(scene.generation), int64_t(scene.projector.m.zoom_index),
                                 int64_t(scene.rotation) }) {
            fp ^= uint64_t(v);
            fp *= 1099511628211ull;
        }
        if (groups[k])
            fp = (fp ^ group_fp[groups[k]]) * 1099511628211ull;
        if (!memo_off)
            if (auto m = cache.shadow_memo.find(o.id); m != cache.shadow_memo.end() && m->second.fp == fp &&
                                                       (!m->second.casts || (m->second.cmd.image && m->second.cmd.image->has_pixels()))) {
                if (m->second.casts) {
                    shaded[on.level].insert({ o.x, o.y });
                    cache.shadows[on.level].push_back(m->second.cmd);
                    cache.shadow_bounds[on.level].push_back(m->second.bounds);
                    cache.shadow_items[on.level].push_back(m->second.item);
                }
                m->second.stamp = stamp;
                g_compose_profile.shadow_hits++;
                continue;
            }
        ObjectDrawCache::ShadowMemo memo;
        memo.stamp = stamp;
        g_compose_profile.shadow_misses++;
        memo.fp = fp;
        int code = shadow_code(o, *info, by_tile, content);
        if (code < 0) {
            cache.shadow_memo[o.id] = memo;   // (casts none: remembered too)
            continue;
        }
        if (code < 0)
            continue;
        shaded[on.level].insert({ o.x, o.y });
        if (TS_ENV("TS_SHADOW_LIST"))   // debug: each object casting a shadow
            std::printf("shadow under #%d %s at (%d, %d) level %d: code %d brightness %d\n", int(o.id), info->label.c_str(), o.x, o.y,
                        on.level, code, int(info->objd.get(Objd::ShadowBrightness)));
        if (code != 0 && code != 16)
            code = rotate_shadow_code(code, o.direction() / 2 + scene.rotation);
        // (The brightness kept to 0-255: a negative one made a solid black blob, the 2026-11-01 report.)
        const int brightness = std::clamp(int(int16_t(info->objd.get(Objd::ShadowBrightness))), 0, 255);
        const Image *img = shadow_picture(content, sprites, code, scene.projector.m.zoom_index, brightness);
        if (!img) {
            cache.shadow_memo[o.id] = memo;
            continue;
        }
        const Projector &p = scene.projector;
        const float z = on.level * kLevelHeight + lot.ground(float(o.x), float(o.y));
        float bx, by;
        p.box(float(o.x), float(o.y), z, bx, by);
        DrawCmd cmd;
        {
            DrawList one;
            one.sprite(*img, int(bx) + p.m.floor_x, int(by) + p.m.floor_y)
                .set_plane(floor_depth(back_corner_depth(p, float(o.x), float(o.y), z), p.m.zoom_index, float(int(by) + p.m.floor_y)));
            cmd = one.cmds.back();
        }
        cache.shadows[on.level].push_back(cmd);
        cache.shadow_bounds[on.level].push_back(bounds_of(cmd));
        {   // its item, so the kept picture is redrawn where it was when it goes (an object sold left
            // its shadow behind: the 2026-11-01 report)
            FrameItem item;
            item.key = 0x40000000u | uint32_t(on.level) << 16 | uint32_t(uint16_t(o.id));
            uint64_t v = 1469598103934665603ull;
            for (const int64_t x : { int64_t(code), int64_t(o.x), int64_t(o.y), int64_t(brightness),
                                     int64_t(scene.projector.m.zoom_index) }) {
                v ^= uint64_t(x);
                v *= 1099511628211ull;
            }
            item.version = v;
            const DrawBounds &b = cache.shadow_bounds[on.level].back();
            item.x0 = b.x0, item.y0 = b.y0, item.x1 = b.x1, item.y1 = b.y1;
            cache.shadow_items[on.level].push_back(item);
        }
        memo.casts = true;
        memo.cmd = cmd;
        memo.bounds = cache.shadow_bounds[on.level].back();
        memo.item = cache.shadow_items[on.level].back();
        cache.shadow_memo[o.id] = memo;
    }
    for (auto m = cache.shadow_memo.begin(); m != cache.shadow_memo.end();)   // (objects gone, or casting none now, forgotten)
        m = m->second.stamp != stamp ? cache.shadow_memo.erase(m) : std::next(m);
    g_compose_profile.shadow_loop_ms += since(t_loop);
    }
    g_compose_profile.shadows_ms += since(t_part);
    }
    const auto t_order = PClock::now();
    for (const ObjectDrawCache::Placed &p : cache.placed)
        inserts.push_back({ p.at, p.order, p.cmds, nullptr, p.bounds });
    for (int l = 0; l < kLotLevels; l++)   // the shadows, over the storey's floors before all else
        if (!cache.shadows[l].empty())
            inserts.push_back({ scene.floors_end[l], kShadowOrder, &cache.shadows[l], nullptr, &cache.shadow_bounds[l] });
    if (items) {
        items->insert(items->end(), cache.placed_items.begin(), cache.placed_items.end());
        for (int l = 0; l < kLotLevels; l++)
            items->insert(items->end(), cache.shadow_items[l].begin(), cache.shadow_items[l].end());
    }
    // The Sims in view, posed (those whose pose changed, each on its own thread of the pool:
    // the posing is the most of a frame's work), then put among the lot where their body is.
    {   // The poses, finished: the parts left here taken, until the frame has no more time.
        const auto t_sim = PClock::now();
        g_compose_step = "posing the Sims";
        const int undone = pose_pool().finish([&] { return time_left() < 2.0; });
        if (undone > 0) {
            g_compose_profile.sims_deferred += undone;
            lean = true;
        }
        sims_ms += since(t_sim);
    }
    if (lean)
        g_compose_profile.lean_frames++;
    for (const LiveSim *sp : shown) {
        const LiveSim &s = *sp;
        const DrawMesh &mesh = *cache.sims[s.id].mesh;
        // Where the body is: the storey and tile of its pelvis (climbing, it leaves the tile
        // the Sim stands on and comes up through the stairwell), else where the Sim stands.
        float bx = s.x, by = s.y;
        int level = s.level;
        if (mesh.has_anchor) {
            bx = mesh.ax;
            by = mesh.ay;
            // The storey of the feet under it (a Sim's pelvis stands about 0.9 tiles up), upstairs
            // once three quarters of the way; but never below the Sim's own storey while the pelvis
            // is still above its floor (kneeling or sitting on the floor upstairs, at a doll house,
            // brings it low enough to round down a storey; going down the stairs it drops below).
            // (Heights above the ground there: the pelvis's height has the terrain in it, and on a
            // raised lot, the Newbies' 3.2 tiles up, it read as upstairs and they weren't drawn.)
            const float az = mesh.az - lot.ground(mesh.ax, mesh.ay);
            const int by_pelvis = int(std::floor((az - 0.9f) / kLevelHeight + 0.25f));
            level = by_pelvis < s.level && az > float(s.level) * kLevelHeight - 0.1f ? s.level : by_pelvis;
        }
        level = std::clamp(level, 0, kLotLevels - 1);
        cache.sims[s.id].in_frame = level <= scene.top_level;
        if (level > scene.top_level)
            continue;   // upstairs, while the storey below is in view
        const int tx = std::clamp(int(std::floor(bx + 0.5f)), 0, std::max(scene.edge - 1, 0));
        const int ty = std::clamp(int(std::floor(by + 0.5f)), 0, std::max(scene.edge - 1, 0));
        const auto &ends = scene.tile_end[level];
        const size_t i = size_t(tx + ty * scene.edge);
        const int rank = i < scene.tile_rank.size() ? int(scene.tile_rank[i]) : 0;
        inserts.push_back({ i < ends.size() ? ends[i] : uint32_t(scene.list.cmds.size()), rank * 2048 + 1000, nullptr, &s });
    }
    g_compose_step = "placing the Sims";
    // The objects' draws come in order (cache.placed, sorted with the placements); the shadows and
    // the Sims after them are sorted and merged in (a sort of all 600-odd a frame was 0.8 ms on
    // the Vita downtown). Equal places keep the objects first, then the shadows, then the Sims,
    // as the stable sort of all had them.
    auto before = [](const Insert &a, const Insert &b) { return a.at != b.at ? a.at < b.at : a.order < b.order; };
    const auto extras = inserts.begin() + std::ptrdiff_t(std::min(cache.placed_in_order, cache.placed.size()));
    std::stable_sort(extras, inserts.end(), before);
    std::inplace_merge(inserts.begin(), extras, inserts.end(), before);
    g_compose_step = "the frame's list";
    g_compose_profile.objects_ms += since(t_objects);
    g_compose_profile.order_ms += since(t_order) - sims_ms;
    const auto t_copy = PClock::now();
    out.cmds.clear();
    // (A Sim is one draw here, its mesh: 1500 each, as for its triangles before, was 2.4 MB of the
    // block for nothing with Solana's 11.)
    reserve_draws(out.cmds, scene.list.cmds.size() + cache.placed.size() * 4 + sims.size());
    size_t from = 0;
    CharacterLibrary &library = sprites.characters();
    // The scene's list from `from` up to `to`, noting where each storey's floors end in the frame.
    // The draws' bounds alongside, where asked (the scene's and the objects' worked out before).
    if (out_bounds) {
        out_bounds->clear();
        out_bounds->reserve(out.cmds.capacity());
    }
    const bool have_bounds = out_bounds && scene.bounds.size() == scene.list.cmds.size();
    // Only what is on or near the screen goes into the frame: the rest of a lot's thousands of
    // draws (the grass's triangles, the far rooms' floors and walls, the objects out of view)
    // would be lit, copied, diffed and culled by the renderer every frame for nothing (the pet
    // shop's 9,400 draws with 230 on screen: ~15 ms a frame on the Vita, 2026-10-31). A margin
    // keeps what a pan exposes before the next frame; the frame's own diff sees what enters and
    // leaves. TS_NO_FRAME_CULL=1 keeps every draw, to compare.
    static const bool no_cull = TS_ENV("TS_NO_FRAME_CULL") != nullptr;
    const bool cull = view && have_bounds && !no_cull;
    constexpr float kCullMargin = 96;
    auto near_view = [&](const DrawBounds &b) {
        return !(b.x1 < view->x0 - kCullMargin || b.x0 > view->x1 + kCullMargin || b.y1 < view->y0 - kCullMargin ||
                 b.y0 > view->y1 + kCullMargin);
    };
    auto copy_to = [&](size_t to) {
        if (!cull) {
            for (int l = 0; l < kLotLevels; l++)
                if (scene.floors_end[l] >= from && scene.floors_end[l] <= to)
                    scene.frame_floors_end[l] = out.cmds.size() + (scene.floors_end[l] - from);
            out.cmds.insert(out.cmds.end(), scene.list.cmds.begin() + std::ptrdiff_t(from),
                            scene.list.cmds.begin() + std::ptrdiff_t(to));
            if (have_bounds)
                out_bounds->insert(out_bounds->end(), scene.bounds.begin() + std::ptrdiff_t(from),
                                   scene.bounds.begin() + std::ptrdiff_t(to));
            from = to;
            return;
        }
        for (size_t i = from; i < to; i++) {
            for (int l = 0; l < kLotLevels; l++)
                if (scene.floors_end[l] == i)
                    scene.frame_floors_end[l] = out.cmds.size();
            if (!near_view(scene.bounds[i]))
                continue;
            out.cmds.push_back(scene.list.cmds[i]);
            out_bounds->push_back(scene.bounds[i]);
        }
        for (int l = 0; l < kLotLevels; l++)
            if (scene.floors_end[l] == to)
                scene.frame_floors_end[l] = out.cmds.size();
        from = to;
    };
    for (const Insert &in : inserts) {
        copy_to(in.at);
        if (in.cmds) {
            if (cull && in.bounds && in.bounds->size() == in.cmds->size()) {
                for (size_t i = 0; i < in.cmds->size(); i++)
                    if (near_view((*in.bounds)[i])) {
                        out.cmds.push_back((*in.cmds)[i]);
                        out_bounds->push_back((*in.bounds)[i]);
                    }
            } else {
                out.cmds.insert(out.cmds.end(), in.cmds->begin(), in.cmds->end());
                if (have_bounds)
                    out_bounds->insert(out_bounds->end(), in.bounds->begin(), in.bounds->end());
            }
            if (in.order == kShadowOrder)   // shadows count with the floors
                for (int l = 0; l < kLotLevels; l++)
                    if (scene.floors_end[l] == in.at)
                        scene.frame_floors_end[l] = out.cmds.size();
            continue;
        }
        const LiveSim &s = *in.sim;
        ObjectDrawCache::SimEntry &kept = cache.sims[s.id];   // posed above (or kept from before)
        const DrawMesh &mesh = *kept.mesh;
        if (!mesh.tris.empty()) {
            const DrawCmd &cmd = out.mesh(mesh, s.id);
            if (have_bounds)
                out_bounds->push_back(bounds_of(cmd));
        }
        if (items)
            items->push_back({ 0x10000u + s.id, kept.version, mesh.x0, mesh.y0, mesh.x1, mesh.y1 });
    }
    copy_to(scene.list.cmds.size());
    for (auto it = cache.sims.begin(); it != cache.sims.end();)   // Sims gone or out of view
        it = it->second.used ? std::next(it) : cache.sims.erase(it);
    if (out_bounds && !have_bounds)   // a scene without them: all now
        for (const DrawCmd &cmd : out.cmds)
            out_bounds->push_back(bounds_of(cmd));
    g_compose_profile.sims_ms += sims_ms;
    g_compose_profile.copy_ms += since(t_copy) - sims_ms;
    g_compose_step = "done";
}

Canvas paint_lot(const Lot &lot, Content &content, const PaintOptions &options, PaintStats &stats)
{
    SpriteCache sprites(content);
    LotScene scene = build_lot_scene(lot, content, sprites, options, stats);
    Canvas canvas(scene.width, scene.height, 0xff302010u);
    rasterize(scene.list, canvas, 0, 0, options.depth_test);
    return canvas;
}

bool lot_ground_box(const Lot &lot, int box[4])
{
    box[0] = box[1] = kLotArraySize;
    box[2] = box[3] = -1;
    const std::vector<uint8_t> &flags = lot.tile_flags[0];
    for (int y = 1; y < lot.size - 1; y++)
        for (int x = 1; x < lot.size - 1; x++)
            if (flags.empty() || !(flags[size_t(x) + size_t(y) * kLotArraySize] & 0x20)) {
                box[0] = std::min(box[0], x), box[1] = std::min(box[1], y);
                box[2] = std::max(box[2], x), box[3] = std::max(box[3], y);
            }
    return box[2] >= box[0] && box[3] >= box[1];
}

bool make_lot_thumbnail(const Lot &lot, Content &content, LotThumbnail &out)
{
    if (!lot_ground_box(lot, out.box))
        return false;
    PaintOptions options;
    options.zoom = Zoom::Far;
    options.rotation = 0;
    options.walls = WallMode::Up;
    options.people = false;
    for (int k = 0; k < 4; k++)
        options.tile_box[k] = out.box[k];
    SpriteCache sprites(content);
    int anchor_x = 0, anchor_y = 0;   // in the full-size scene
    int crop[4] = { 0, 0, 0, 0 };     // the closed picture's opaque bounds, used for both
    for (int pass = 0; pass < 2; pass++) {
        options.roofs = pass == 0;
        PaintStats stats;
        LotScene scene = build_lot_scene(lot, content, sprites, options, stats);
        Canvas canvas(scene.width, scene.height, 0u);   // (transparent: alpha 0 where nothing is drawn)
        rasterize(scene.list, canvas, 0, 0, false);
        if (pass == 0) {
            const float fx = float(out.box[2]) + 0.5f, fy = float(out.box[3]) + 0.5f;
            float sx, sy;
            scene.projector.box(fx, fy, lot.ground(fx, fy), sx, sy);
            anchor_x = int(sx + scene.projector.m.cadge_w / 2.0f);
            anchor_y = int(sy + float(scene.projector.m.baseline));
            crop[0] = canvas.width, crop[1] = canvas.height, crop[2] = -1, crop[3] = -1;
            for (int y = 0; y < canvas.height; y++)
                for (int x = 0; x < canvas.width; x++)
                    if (canvas.rgba[size_t(y) * size_t(canvas.width) + size_t(x)] >> 24) {
                        crop[0] = std::min(crop[0], x), crop[1] = std::min(crop[1], y);
                        crop[2] = std::max(crop[2], x), crop[3] = std::max(crop[3], y);
                    }
            if (crop[2] < crop[0])
                return false;
            // Whole blocks of four from the anchor, so it lands on a pixel of the small picture.
            crop[0] = anchor_x - ((anchor_x - crop[0] + 3) / 4) * 4;
            crop[1] = anchor_y - ((anchor_y - crop[1] + 3) / 4) * 4;
            out.anchor_x = (anchor_x - crop[0]) / 4;
            out.anchor_y = (anchor_y - crop[1]) / 4;
        }
        // A quarter size: each block of 4 x 4 the mean of its drawn pixels, drawn where at least
        // half of it is.
        const int w = (crop[2] - crop[0]) / 4 + 1, h = (crop[3] - crop[1]) / 4 + 1;
        Image &img = pass == 0 ? out.closed : out.open;
        img.width = w;
        img.height = h;
        img.rgba.assign(size_t(w) * size_t(h), 0u);
        for (int by = 0; by < h; by++)
            for (int bx = 0; bx < w; bx++) {
                uint32_t sum[3] = {}, n = 0;
                for (int dy = 0; dy < 4; dy++)
                    for (int dx = 0; dx < 4; dx++) {
                        const int x = crop[0] + bx * 4 + dx, y = crop[1] + by * 4 + dy;
                        if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height)
                            continue;
                        const uint32_t c = canvas.rgba[size_t(y) * size_t(canvas.width) + size_t(x)];
                        if (!(c >> 24))
                            continue;
                        for (int s = 0; s < 3; s++)
                            sum[s] += (c >> (8 * s)) & 0xff;
                        n++;
                    }
                if (n >= 8)
                    img.rgba[size_t(by) * size_t(w) + size_t(bx)] =
                        0xff000000u | (sum[2] / n) << 16 | (sum[1] / n) << 8 | (sum[0] / n);
            }
    }
    return true;
}

int catalogue_facing(const ObjectInfo &info)
{
    if (const char *d = TS_ENV("TS_PICTURE_FACING"))   // debug: try each way
        return std::atoi(d) & 6;
    // Stairs (build mode type 3) facing 4 show their back and underside: turned so their
    // steps come down towards the camera.
    if (info.objd.get(Objd::BuildModeType) == 3)
        return 6;
    return 4;
}

bool draw_object_picture(DrawList &out, Content &content, SpriteCache &sprites, const ObjectInfo &info, float cx,
                         float cy, int max_w, int max_h, int direction, int graphic, Zoom first, uint64_t dynamic_sprites)
{
    if (direction < 0)
        direction = catalogue_facing(info);
    // The catalogue's picture is the graphic the object names for it (OBJD ThumbnailGraphic: the
    // fridges' 5), not its first: the Porcina fridge's first showed its back (the user, 2026-11-13).
    // TS_THUMB_GRAPHIC_OLD=1: the first, as before.
    if (graphic < 0 && !TS_ENV("TS_THUMB_GRAPHIC_OLD")) {
        const uint16_t thumb = info.objd.get(Objd::ThumbnailGraphic);
        if (thumb != 0 && thumb != 0xffff && thumb < info.objd.get(Objd::NumGraphics))
            graphic = int(thumb);
    }
    // The parts' offsets are for facing 4: turned a quarter for each step of 2 from it.
    const int turns = ((direction - 4) / 2 + 4) % 4;
    // Its parts: a multi-tile object's master names it and the other definitions in its
    // file with the same master id are the tiles (as Vm::create_with_parts finds them).
    std::vector<LotObject> parts;
    const uint16_t master = info.objd.get(Objd::MasterID);
    if (master != 0 && info.objd.get(Objd::SubIndex) == 0xffff)
        for (const auto &entry : content.objects().by_guid) {
            const ObjectInfo &o = entry.second;
            if (o.ref.path != info.ref.path || o.ref.offset != info.ref.offset ||
                o.objd.get(Objd::MasterID) != master || o.objd.get(Objd::SubIndex) == 0xffff)
                continue;
            const uint16_t sub = o.objd.get(Objd::SubIndex);
            LotObject part;
            part.guid = o.guid;
            int ox = -int8_t(sub >> 8), oy = -int8_t(sub & 0xff);   // facing 4 (as create_with_parts)
            for (int k = 0; k < turns; k++) {   // a quarter clockwise: (x, y) -> (-y, x)
                const int t = ox;
                ox = -oy;
                oy = t;
            }
            part.x = int16_t(ox);
            part.y = int16_t(oy);
            part.level = int16_t(o.objd.get(Objd::LevelOffset));
            parts.push_back(part);
        }
    if (parts.empty()) {
        LotObject one;
        one.guid = info.guid;
        one.x = one.y = 0;
        parts.push_back(one);
    }
    // Back to front, as on a lot.
    std::stable_sort(parts.begin(), parts.end(),
                     [](const LotObject &a, const LotObject &b) { return a.x + a.y < b.x + b.y; });
    // From `first` out (the catalogue asks for the medium zoom for every item, further out only
    // when even that doesn't fit the box: with the nearest zoom that fit, a slightly shorter
    // counter, the Tiled Counter, came out at the near zoom beside the others at the medium, a
    // size bigger than its neighbours in the list, the 2026-11-02 report; the original's
    // catalogue draws its items at one size too).
    for (Zoom zoom : { Zoom::Near, Zoom::Medium, Zoom::Far }) {
        if (int(zoom) < int(first))
            continue;
        Projector p{ metrics(zoom), 0 };
        DrawList list;
        PaintStats stats;
        for (const LotObject &part : parts) {
            LotObject o = part;
            o.data[ObjDirection] = int16_t(direction);
            if (graphic >= 0)
                o.data[ObjGraphic] = int16_t(graphic);
            o.dynamic_sprites = dynamic_sprites;
            Placed pl{ &o, float(part.x), float(part.y), float(part.level) * kLevelHeight, 0 };
            draw_object(list, p, content, sprites, pl, zoom, 0, stats);
        }
        if (list.cmds.empty())
            return false;
        int l = 1 << 30, t = 1 << 30, r = -(1 << 30), b = -(1 << 30);
        for (const DrawCmd &c : list.cmds) {
            l = std::min(l, c.x);
            t = std::min(t, c.y);
            r = std::max(r, c.x + c.image->width);
            b = std::max(b, c.y + c.image->height);
        }
        if ((r - l > max_w || b - t > max_h) && zoom != Zoom::Far)
            continue;   // too big: one zoom further out
        const int dx = int(cx) - (l + r) / 2, dy = int(cy) - (t + b) / 2;
        for (DrawCmd c : list.cmds) {
            c.x += dx;
            c.y += dy;
            c.depth = DepthMode::None;
            c.owner = 0;
            out.cmds.push_back(c);
        }
        return true;
    }
    return false;
}

} // namespace ts

namespace ts {

bool draw_object_at(DrawList &out, const Projector &p, Content &content, SpriteCache &sprites, const ObjectInfo &info,
                    float x, float y, float z, int direction, int graphic, Zoom zoom, int rotation, uint16_t owner)
{
    LotObject o;
    o.id = owner;
    o.guid = info.guid;
    o.data[ObjDirection] = int16_t(direction);
    o.data[ObjGraphic] = int16_t(graphic);
    Placed pl{ &o, x, y, z, 0 };
    PaintStats stats;
    const size_t was = out.cmds.size();
    draw_object(out, p, content, sprites, pl, zoom, rotation, stats);
    return out.cmds.size() > was;
}

} // namespace ts
