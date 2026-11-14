/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Buy mode, ported from FreeSO (tso.content/TS1/TS1ObjectProvider.cs,
 * tso.simantics/VMContext.cs GetObjPlace, Entities/VMEntity.cs PositionValid,
 * WallChangeValid, FloorChangeValid, IndividualUserMovable, UpdateDynamicMultitile,
 * Entities/VMGameObject.cs PositionChange, NetPlay/Model/Commands/VMNetBuyObjectCmd.cs,
 * VMNetMoveObjectCmd.cs, VMNetDeleteObjectCmd.cs).
 */

// The catalogue, and placing, moving and selling objects as the player
// does in buy mode. Placement asks the objects themselves, as the game
// does: their placement flags (floor, ground, water, walls needed and not
// allowed on each side) and their Allow Intersection entry point.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/env.h"
#include "files/iff.h"
#include "files/install.h"
#include "vm/vm.h"
#include "vm/footprint.h"

namespace ts::vm {

namespace {

// Object data fields used here (FreeSO VMStackObjectVariable).
constexpr int kVarSupportStrength = 28, kVarExclusivePlacementFlags = 63;
// Flags (VMEntityFlags), WallPlacementFlags, VMPlacementFlags, VMMovementFlags.
constexpr uint16_t kDisallowPersonIntersection = 1 << 1, kAllowPersonIntersection = 1 << 4;
constexpr int kWallDiagonalRequired = 1 << 6, kWallDiagonalAllowed = 1 << 7;
constexpr int kPlaceOnFloor = 1, kPlaceOnTerrain = 1 << 1, kPlaceOnWater = 1 << 2, kPlaceOnSlope = 1 << 8, kPlaceInAir = 1 << 9,
              kPlaceAllowOnPool = 1 << 11, kPlaceRequirePool = 1 << 12, kPlaceRequireWater = 1 << 13;
constexpr int kPlayersCanMove = 1 << 1, kPlayersCanDelete = 1 << 3;
// Wall adjacency flags (VMWallAdjacencyFlags).
constexpr int kWallOnLeft = 1, kWallOnRight = 2, kWallInFront = 4, kWallBehind = 8;

// Turns wall sides (bits 0-3: top left, top right, bottom right, bottom left) by `rotate` quarters.
int rotate_walls(int sides, int rotate)
{
    return ((sides << (4 - rotate)) & 15) | ((sides & 15) >> rotate);
}

std::vector<Entity *> parts_of(Entity *e)
{
    if (!e)
        return {};
    if (e->group.empty())
        return { e };
    return e->group;
}

bool same_object(const Entity *a, const Entity *b)
{
    return a == b || (!a->group.empty() && std::find(a->group.begin(), a->group.end(), b) != a->group.end());
}

int level_offset(const Entity &e)
{
    return e.info ? int16_t(e.info->objd.get(Objd::LevelOffset)) : 0;
}

// The first container slot (SLOT type 0) of an object: where things are put on it.
const SlotItem *surface_slot(Vm &vm, Entity &e)
{
    for (const SlotItem &s : e.slot_items(vm))
        if (s.type == 0)
            return &s;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------- the catalogue

std::vector<CatalogItem> build_catalog(const Content &content)
{
    std::vector<CatalogItem> out;
    for (const auto &entry : content.objects().by_guid) {
        const ObjectInfo &info = entry.second;
        const Objd &d = info.objd;
        const uint16_t function = d.get(Objd::FunctionFlags);
        const uint16_t master = d.get(Objd::MasterID), sub = d.get(Objd::SubIndex);
        if (function == 0 || d.get(Objd::Disabled) != 0 || (master == 0 && d.get(Objd::NumGraphics) == 0) ||
            (master != 0 && sub != 0xffff) || d.get(Objd::ObjectType) == 2)
            continue;
        CatalogItem item;
        item.info = &info;
        item.name = info.name.empty() ? info.label : info.name;
        item.price = d.get(Objd::Price);
        int bit = 0;
        while (bit < 15 && !(function & (1 << bit)))
            bit++;
        item.category = bit;
        item.subsort = d.get(Objd::FunctionSubsort);
        item.room_flags = d.get(Objd::RoomFlags);
        item.area_sort[0] = d.get(Objd::DTSubsort);
        item.area_sort[1] = d.get(Objd::VacationSubsort);
        item.area_sort[2] = d.get(Objd::CommunitySubsort);
        item.area_sort[3] = d.get(Objd::STSubsort);
        item.area_sort[4] = d.get(Objd::MTSubsort);
        out.push_back(std::move(item));
    }
    std::sort(out.begin(), out.end(), [](const CatalogItem &a, const CatalogItem &b) {
        return a.category != b.category ? a.category < b.category : a.price != b.price ? a.price < b.price : a.name < b.name;
    });
    return out;
}

const char *catalog_category_name(int category)
{
    static const char *const kNames[kCatalogCategories] = { "Comfort", "Surfaces", "Appliances", "Electronics",
                                                            "Plumbing", "Decorative", "Miscellaneous", "Lighting" };
    return category >= 0 && category < kCatalogCategories ? kNames[category] : "Other";
}

CatalogNames catalog_names(Content &content, int category)
{
    CatalogNames out;
    out.category = catalog_category_name(category);
    // UIText.iff: STR# 150 "BuyModeCatalogSortTips" (8-15 the functions, in the catalogue's
    // order), STR# 200-207 each function's sub-sorts (seating, surfaces, decorative,
    // electronics, appliances, plumbing, lighting, miscellaneous), STR# 210 the extras.
    static const char *const kEnglish[kCatalogCategories][4] = {
        { "Dining Chairs", "Lounge Chairs", "Sofas", "Beds" },
        { "Counters", "Tables", "End Tables", "Desks" },
        { "Stoves", "Refrigerators", "Small Appliances", "Large Appliances" },
        { "Entertainment", "Video", "Audio", "Phones" },
        { "Toilets", "Showers/Tubs", "Sinks", "Hot Tubs" },
        { "Paintings", "Sculptures", "Rugs", "Plants" },
        { "Recreation", "Knowledge", "Creativity", "Wardrobe" },
        { "Table Lamps", "Standing Lamps", "Wall Lamps", "Hanging Lamps" } };
    if (category >= 0 && category < kCatalogCategories)
        for (int i = 0; i < 4; i++)
            out.subsort[i] = kEnglish[category][i];
    const FileRef *ref = content.file("uitext.iff");
    const LoadedIff *file = ref ? content.load(*ref) : nullptr;
    if (!file || category < 0 || category >= kCatalogCategories)
        return out;
    auto table = [&](uint16_t id) {
        StringTable t;
        std::string err;
        if (const IffChunk *c = file->iff.find("STR#", id))
            parse_str(file->body(*c), c->size, t, err);
        return t;
    };
    int shown_at = 0;   // its place in the catalogue's order
    while (shown_at < kCatalogCategories && kCatalogOrder[shown_at] != category)
        shown_at++;
    const StringTable sorts = table(150);
    if (const std::string *s = sorts.get(size_t(8 + shown_at)); s && !s->empty())
        out.category = *s == "Seating" ? "Comfort" : *s;   // the user's word: it holds the beds too
    const StringTable subs = table(uint16_t(200 + shown_at));
    for (int i = 0; i < 4; i++)
        if (const std::string *s = subs.get(size_t(i)))
            out.subsort[i] = *s;
    const StringTable extra = table(210);
    for (auto [i, field] : { std::pair<int, std::string *>{ 1, &out.other }, { 2, &out.all }, { 3, &out.pets }, { 4, &out.magic } })
        if (const std::string *s = extra.get(size_t(i)); s && !s->empty())
            *field = *s;
    return out;
}

int catalog_area(const Vm &vm)
{
    const int h = vm.house;
    if (h >= 21 && h <= 30)
        return 0;   // downtown
    if (h >= 40 && h <= 49)
        return 1;   // vacation
    if (h >= 81 && h <= 89)
        return 3;   // Studio Town
    if (h >= 90 && h <= 99)
        return 4;   // Magic Town
    return vm.residential() ? -1 : 2;   // a home, or an Old Town community lot
}

int catalog_subsort(uint16_t subsort)
{
    for (int bit : { 0, 1, 2, 3, 5, 6 })
        if (subsort == (1u << bit))
            return bit;
    return -1;
}

std::string Vm::catalog_description(const ObjectInfo &info)
{
    // From the object's file as the content cache has it (the viewer reads the catalogue's
    // in a scratch scope, its picture and description together, then lets it go): parsed
    // here, as nothing may keep a pointer into a file that goes.
    const LoadedIff *file = content.load(info.ref);
    const IffChunk *c = file ? file->iff.find("CTSS", info.objd.get(Objd::CatalogStringsID)) : nullptr;
    StringTable t;
    std::string err;
    if (!c || !parse_str(file->body(*c), c->size, t, err))
        return {};
    const std::string *s = t.get(1);
    return s ? *s : std::string();
}

// ---------------------------------------------------------------- placing

namespace {

// The wall sides at a tile, with the ones its neighbours hold on the shared edges.
int wall_sides(const Lot &lot, int x, int y, int lot_level, bool &diagonal)
{
    diagonal = false;
    if (lot_level < 0 || lot_level >= kLotLevels || lot.walls[lot_level].empty())
        return 0;
    const int n = std::min(lot.size + 1, kLotArraySize);
    const auto at = [&](int tx, int ty) -> int {
        return tx >= 0 && ty >= 0 && tx < n && ty < n ? lot.wall(tx, ty, lot_level).segments : 0;
    };
    const int own = at(x, y);
    diagonal = (own & (WallHorizontalDiag | WallVerticalDiag)) != 0;
    int sides = own & 15;
    if (at(x + 1, y) & WallTopLeft)
        sides |= WallBottomRight;
    if (at(x, y + 1) & WallTopRight)
        sides |= WallBottomLeft;
    if (at(x - 1, y) & WallBottomRight)
        sides |= WallTopLeft;
    if (at(x, y - 1) & WallBottomLeft)
        sides |= WallTopRight;
    return sides;
}

} // namespace

// Whether a ground tile's four corners aren't all at one height.
static bool tile_sloped(const Lot &lot, int x, int y)
{
    const int n = kLotArraySize + 1;
    if (lot.altitude.size() < size_t(n) * size_t(n) || x < 0 || y < 0 || x + 1 >= n || y + 1 >= n)
        return false;
    const uint8_t a = lot.altitude[size_t(x + y * n)];
    return lot.altitude[size_t(x + 1 + y * n)] != a || lot.altitude[size_t(x + (y + 1) * n)] != a ||
           lot.altitude[size_t(x + 1 + (y + 1) * n)] != a;
}

namespace {

// The wall side an object without extent hangs on (the exe's same test: wall placement flags & 5,
// 1 the front, 4 the back, turned with the object), 0 none.
int wall_side(const Entity &e, int direction)
{
    const int w = e.value(VarWallPlacementFlags) & 5;
    const int side = w == 1 ? 0 : w == 4 ? 2 : -1;
    if (side < 0)
        return 0;
    return 1 + ((side + (direction & 6) / 2) & 3);
}

} // namespace

std::string Vm::placement_problem(int tx, int ty, int level, int direction, Entity **onto)
{
    return placement_problem_of(held.obj, tx, ty, level, direction, onto);
}

std::string Vm::placement_problem_of(Entity *lead, int tx, int ty, int level, int direction, Entity **onto, bool in_play)
{
    if (onto)
        *onto = nullptr;
    if (!lead)
        return "Nothing in hand";
    direction &= 6;
    int mx, my;
    part_offset(*lead, direction, mx, my);
    Entity *surface = nullptr;
    const std::vector<Entity *> parts = parts_of(lead);
    for (Entity *part : parts) {
        int px, py;
        part_offset(*part, direction, px, py);
        const int x = tx + px - mx, y = ty + py - my;
        const int plevel = level + level_offset(*part) - level_offset(*lead);
        if (x < 0 || y < 0 || x >= lot.size || y >= lot.size || plevel < 1 || plevel > kLotLevels)
            return "That's off the lot";
        // The player's own placing: only on the lot's editable tiles (the exe's FUN_0053efc0 /
        // FUN_0053ee40, its error 0x27): not the margin, the pavement or the road. Ours took any
        // tile of the lot. TS_EDITABLE_OLD=1: as before.
        static const bool editable_old = TS_ENV("TS_EDITABLE_OLD") != nullptr;
        if (!in_play && !editable_old && !lot.user_tile(x, y, move_objects))
            return "That's outside the area you can build on";
        const int lot_level = plevel - 1;

        // Walls (WallChangeValid): the sides it needs walls on, and may not have them on.
        bool diagonal = false;
        const int sides = wall_sides(lot, x, y, lot_level, diagonal);
        int used = 0;   // sides that something exclusive already hangs on
        for (auto &up : entities) {
            const Entity &o = *up;
            if (o.dead || o.person || !o.in_world() || o.container || o.level != plevel || o.tile_x() != x ||
                o.tile_y() != y || same_object(lead, &o) || !(o.value(kVarExclusivePlacementFlags) & 2))
                continue;
            used |= rotate_walls(o.value(VarWallPlacementFlags) & 15, (8 - ((o.direction / 2) + 1)) % 4);
        }
        const int wall_flags = part->value(VarWallPlacementFlags);
        if (diagonal && !(wall_flags & kWallDiagonalAllowed))
            return "A wall is in the way";
        if (!diagonal && (wall_flags & kWallDiagonalRequired))
            return "That goes on a diagonal wall";
        const int rotate = ((direction / 2) + 1) % 4;
        const int turned = rotate_walls(sides, rotate), turned_used = rotate_walls(used, rotate);
        if ((wall_flags & turned) != (wall_flags & 15))
            return "That goes against a wall";
        if ((part->value(kVarExclusivePlacementFlags) & 2) && (wall_flags & turned_used))
            return "Something is already on that wall";
        if (wall_flags & (turned << 8))
            return "A wall is in the way";

        // The floor (FloorChangeValid).
        const int place = part->value(VarPlacementFlags);
        const uint16_t floor = lot.floors[lot_level].empty() ? 0 : lot.floor(x, y, lot_level);
        if (floor == kFloorPool) {
            if (!(place & (kPlaceAllowOnPool | kPlaceRequirePool)))
                return "That can't go in the pool";
        } else {
            if (place & kPlaceRequirePool)
                return "That goes in a pool";
            if (floor == kFloorWater) {
                if (!(place & (kPlaceOnWater | kPlaceRequireWater)))
                    return "That can't go in water";
            } else {
                if (place & kPlaceRequireWater)
                    return "That goes in water";
                if (floor == 0) {
                    if (plevel == 1) {
                        if (!(place & kPlaceOnTerrain))
                            return "That goes indoors, on a floor";
                    } else if (!(place & kPlaceInAir) && level_offset(*part) == 0) {
                        return "There's no floor there";
                    }
                } else if (!(place & kPlaceOnFloor) && (level_offset(*part) == 0 || !(place & kPlaceInAir))) {
                    return "That can't go on a floor";
                }
            }
        }

        // The ground (FreeSO SlopeValid): on a sloped tile, only what may stand on sloped ground;
        // something that goes on floors needs the flag for it. (Not checked, anything went on a
        // hillside: the 2026-10-27 report.)
        if (plevel == 1 && tile_sloped(lot, x, y) && (place & kPlaceOnFloor) && !(place & kPlaceOnSlope))
            return "That can't go on a slope";

        // Other objects (GetObjPlace): each may let it share the tile, or carry it.
        const int allowed_heights = part->value(VarAllowedHeightFlags);
        const bool no_floor = !(allowed_heights & 1);
        const uint16_t flags = uint16_t(part->value(VarFlags));
        const bool allow_sims = !(flags & kDisallowPersonIntersection) && (flags & kAllowPersonIntersection);
        std::string problem = no_floor ? "That goes on a counter or table" : "";
        // As the exe's placement test (FUN_0053e1d0): the objects on this tile and the eight round it.
        // Two objects with extent meet only where their footprints overlap (FUN_0053a470: the tile
        // width's square, each side pulled in by a nibble of the footprint mask, turned with the
        // object), and then only if neither's "allow intersection" (entry 5) lets the other in. One
        // without extent (a window, a rug...) meets nothing with extent; on its own tile it meets an
        // object both of whose exclusive-placement bit 1 is set, or another without extent on the same
        // wall side. Ours took any two objects on one tile as meeting: a pennant over a counter, a rug
        // under a table, a window over a counter were refused (the user's report, 2026-11-05).
        // TS_PLACE_OLD=1: as before.
        static const bool place_old = TS_ENV("TS_PLACE_OLD") != nullptr;
        const bool mine_flat = (uint16_t(part->value(VarFlags)) & FlagHasZeroExtent) != 0;
        const Footprint mine_rect = footprint(*part, int16_t(x * 16 + 8), int16_t(y * 16 + 8), direction);
        for (auto &up : entities) {
            Entity &o = *up;
            if (o.dead || !o.in_world() || o.container || o.level != plevel || same_object(lead, &o))
                continue;
            const int ox = o.tile_x(), oy = o.tile_y();
            const bool here = ox == x && oy == y;
            if (place_old ? !here : (std::abs(ox - x) > 1 || std::abs(oy - y) > 1))
                continue;
            if (o.person) {
                if (in_play) {
                    // FUN_0053e1d0 for a person beside the new object: its flags (bit 2 clear: bit 4
                    // lets people in; set: bit 1 keeps them out), then either's entry 5, then the boxes.
                    const bool blocks = (flags & 4) ? (flags & 2) != 0 : (flags & 0x10) == 0;
                    if (!blocks)
                        continue;
                    const int16_t me_args[4] = { int16_t(o.id), 0, 0, 0 }, them_args[4] = { int16_t(part->id), 0, 0, 0 };
                    if (part->run_entry(*this, 5, true, 0, me_args) || o.run_entry(*this, 5, true, 0, them_args))
                        continue;
                    if (!mine_rect.overlaps(person_box(o, o.x16, o.y16)))
                        continue;
                    return "Someone is in the way";
                }
                if (here && !allow_sims && on_lot(o))
                    return "Someone is in the way";
                continue;
            }
            const bool theirs_flat = (uint16_t(o.value(VarFlags)) & FlagHasZeroExtent) != 0;
            if (place_old) {
                // No extent, no footprint (FreeSO: such an object has no obstacle): a window's inside
                // part sits on the tile under it and kept a counter from going back there (the user,
                // 2026-10-30).
                if (theirs_flat)
                    continue;
            } else if (mine_flat || theirs_flat) {
                if (!here)
                    continue;
                if (part->value(kVarExclusivePlacementFlags) & o.value(kVarExclusivePlacementFlags) & 1)
                    return "Something is in the way";
                if (mine_flat && theirs_flat) {
                    const int a = wall_side(*part, direction), b = wall_side(o, o.direction);
                    if (a != 0 && a == b)
                        return "Something is already on that wall";
                }
                continue;
            }
            const int16_t mine[4] = { int16_t(o.id), 0, 0, 0 }, theirs[4] = { int16_t(part->id), 0, 0, 0 };
            if (part->run_entry(*this, 5, true, 0, mine) || o.run_entry(*this, 5, true, 0, theirs))
                continue;
            if (!place_old && !mine_rect.overlaps(footprint(o, o.x16, o.y16, o.direction)))
                continue;
            problem = "Something is in the way";
            if (TS_ENV("TS_PLACEDBG"))   // debug: what's in the way
                std::printf("placement: #%d %s in the way at (%d, %d) level %d (x16 %d, %d; container %d; parent %d)%s\n", int(o.id),
                            o.info->label.c_str(), x, y, plevel, int(o.x16), int(o.y16), o.container ? int(o.container->id) : 0,
                            int(o.value(VarParentId)), o.group.empty() ? "" : " (a part)");
            // Solid: it may still go on top (the surface's slot 0, if free, at a height it allows, strong enough).
            if (allowed_heights > 1 && parts.size() == 1) {
                const SlotItem *slot = surface_slot(*this, o);
                const bool free = o.slots.empty() || !o.slots[0] || o.slots[0] == part;
                if (slot && free) {
                    if (slot->height >= 1 && ((1 << (slot->height - 1)) & allowed_heights)) {
                        if (part->value(VarWeight) < o.value(kVarSupportStrength)) {
                            surface = &o;
                            problem.clear();
                            break;
                        }
                        problem = "That's too heavy to go there";
                        if (TS_ENV("TS_PLACEDBG"))   // debug: the weights
                            std::printf("placement: #%d %s weighs %d, #%d %s bears %d\n", int(part->id), part->info->label.c_str(),
                                        int(part->value(VarWeight)), int(o.id), o.info->label.c_str(), int(o.value(kVarSupportStrength)));
                    } else if (no_floor) {
                        problem = (allowed_heights & (1 << 3)) ? "That goes on a counter" : "That doesn't fit there";
                    }
                }
            }
        }
        if (!problem.empty())
            return problem;
    }
    if (onto)
        *onto = surface;
    return {};
}

namespace {

// What a placed object tells the game (FreeSO VMGameObject.PositionChange):
// the walls beside it (entry 6), its neighbours of the same kind (entry 8,
// counters and the like joining up), then Placement (entry 9).
void wall_adjacency(Vm &vm, Entity &e)
{
    if (e.entry_points.size() <= 6 || e.entry_points[6].action == 0)
        return;
    bool diagonal;
    const int sides = wall_sides(vm.lot, e.tile_x(), e.tile_y(), e.level - 1, diagonal);
    const int rotate = (8 - ((e.direction / 2) + 1)) % 4;
    const int main = rotate_walls(sides, (4 - rotate) % 4);
    int adjacency = 0;
    if (main & WallTopLeft)
        adjacency |= kWallInFront;
    if (main & WallTopRight)
        adjacency |= kWallOnRight;
    if (main & WallBottomRight)
        adjacency |= kWallBehind;
    if (main & WallBottomLeft)
        adjacency |= kWallOnLeft;
    e.set_value(VarWallAdjacencyFlags, int16_t(adjacency));
    e.run_entry(vm, 6, true, int16_t(e.id));
}

// Entry 8 with which sides (relative to its facing) have the same object next to them.
void dynamic_multitile(Vm &vm, Entity &e)
{
    if (e.entry_points.size() <= 8 || e.entry_points[8].action == 0 || !e.in_world() || !e.info)
        return;
    int flags = 0;
    for (auto &up : vm.entities) {
        const Entity &o = *up;
        if (&o == &e || o.dead || !o.in_world() || o.container || !o.info || o.info->guid != e.info->guid ||
            o.level != e.level)
            continue;
        const int dx = o.tile_x() - e.tile_x(), dy = o.tile_y() - e.tile_y();
        if (std::abs(dx) + std::abs(dy) != 1)
            continue;
        const int toward = dy < 0 ? 0 : dx > 0 ? 2 : dy > 0 ? 4 : 6;   // north, east, south, west
        flags |= 1 << ((toward - (e.direction & 6) + 8) % 8);
    }
    const int16_t args[4] = { int16_t(flags), 0, 0, 0 };
    e.run_entry(vm, 8, true, 0, args);
}

void neighbours_changed(Vm &vm, int x, int y, int level, uint32_t guid)
{
    for (auto &up : vm.entities) {
        Entity &o = *up;
        if (o.dead || !o.in_world() || o.container || !o.info || o.info->guid != guid || o.level != level)
            continue;
        if (std::abs(o.tile_x() - x) + std::abs(o.tile_y() - y) == 1)
            dynamic_multitile(vm, o);
    }
}

void position_changed(Vm &vm, Entity *lead)
{
    for (Entity *part : parts_of(lead)) {
        if (!part->in_world())
            continue;
        wall_adjacency(vm, *part);
        if (part->info)
            neighbours_changed(vm, part->tile_x(), part->tile_y(), part->level, part->info->guid);
        dynamic_multitile(vm, *part);
        part->run_entry(vm, 9, true);   // Placement
    }
}

void remove_whole(Vm &vm, Entity *lead)
{
    for (Entity *part : parts_of(lead)) {
        part->leave_container();
        for (Entity *s : part->slots)
            if (s)
                s->leave_container();
        part->dead = true;
        if (part->thread)
            part->thread->stack.clear();
        Vm::SoundEvent quiet;   // its sounds end with it
        quiet.source = part->id;
        quiet.stop = true;
        vm.sounds.push_back(quiet);
    }
}

// Whether an object is a door or window (the walls are drawn with its hole).
bool architectural(Entity *lead)
{
    for (Entity *part : parts_of(lead))
        if (uint16_t(part->value(VarFlagField2)) & 0xC000)   // ArchitecturalWindow, ArchitecturalDoor
            return true;
    return false;
}

// The lead part of an object: the one its OBJD names the lead (MyLeadObject: the single bed's middle,
// which keeps the value, data 41), else the one with SubIndex 0, else the first.
Entity *lead_of(Entity *e)
{
    if (!e || e->group.empty())
        return e;
    for (Entity *part : e->group)
        if (part->info && part->info->objd.get(Objd::MyLeadObject) != 0)
            return part;
    for (Entity *part : e->group)
        if (part->info && part->info->objd.get(Objd::SubIndex) == 0)
            return part;
    return e->group[0];
}

} // namespace

bool Vm::hold_new(const CatalogItem &item, std::string &why)
{
    edits++;
    drop_held();
    if (!item.info) {
        why = "Nothing to buy";
        return false;
    }
    if (item.price > budget()) {
        why = "Not enough money";
        return false;
    }
    Entity *obj = create_with_parts(*item.info, -1, -1, 0, 0);
    if (!obj) {
        why = "Can't make that";
        return false;
    }
    obj = lead_of(obj);
    // Set up as the game sets up a new object (FreeSO VMEntity.Init), main
    // not started until it is placed.
    for (Entity *part : parts_of(obj)) {
        part->run_entry(*this, 0, true);
        const int16_t none[4] = { 0, 0, 0, 0 };
        part->run_entry(*this, 8, true, 0, none);
    }
    held = Held();
    held.obj = obj;
    held.bought = true;
    held.price = item.price;
    return true;
}

std::string Vm::movable_problem(Entity *obj, bool selling)
{
    obj = lead_of(obj);
    if (!obj || obj->dead)
        return "Nothing there";
    if (obj->person)
        return "That's a Sim";
    for (Entity *part : parts_of(obj)) {
        const int movement = part->value(VarMovementFlags);
        if (TS_ENV("TS_MOVE_WHY"))   // debug: the movement flags looked at
            std::printf("movable: #%d %s movement flags %d\n", int(part->id), part->info ? part->info->label.c_str() : "?",
                        movement);
        if (!(movement & kPlayersCanMove) && !move_objects)
            return "That can't be moved";
        if (selling && !(movement & kPlayersCanDelete) && !move_objects)
            return "That can't be sold";
        if (part->flag(FlagOccupied))
            return "Someone is using it";
        for (auto &up : entities)
            if (up->person && !up->dead && up->thread)
                for (const Frame &f : up->thread->stack)
                    if (f.callee == part)
                        return "Someone is using it";
        for (Entity *s : part->slots)
            if (s && (selling || s->person || !movable_problem(s, false).empty()))
                return "Take what's on it off first";
    }
    return {};
}

bool Vm::hold_existing(Entity *obj, std::string &why)
{
    edits++;
    drop_held();
    obj = lead_of(obj);
    why = movable_problem(obj, false);
    if (!why.empty())
        return false;
    held = Held();
    held.obj = obj;
    if (architectural(obj))
        lot_version++;
    held.from_x16 = obj->x16;
    held.from_y16 = obj->y16;
    held.from_level = obj->level;
    held.from_direction = obj->direction;
    held.from_container = obj->container;
    held.from_slot = obj->container_slot;
    // Off the lot while in hand; what it holds goes with it.
    for (Entity *part : parts_of(obj)) {
        const int x = part->tile_x(), y = part->tile_y(), level = part->level;
        part->set_position(-1, -1, part->level);
        if (part->info)
            neighbours_changed(*this, x, y, level, part->info->guid);
    }
    return true;
}

bool Vm::place_held(int tx, int ty, int level, int direction, std::string &why)
{
    edits++;
    Entity *obj = held.obj;
    if (!obj) {
        why = "Nothing in hand";
        return false;
    }
    Entity *onto = nullptr;
    why = placement_problem(tx, ty, level, direction, &onto);
    if (!why.empty())
        return false;
    if (held.bought && held.price > budget()) {
        why = "Not enough money";
        return false;
    }
    direction &= 6;
    if (onto) {
        obj->direction = direction;
        onto->place_in_slot(obj, 0);
    } else {
        obj->place_whole(int16_t(tx * 16 + 8), int16_t(ty * 16 + 8), int8_t(level), direction);
    }
    position_changed(*this, obj);
    for (Entity *part : parts_of(obj))
        part->run_entry(*this, 11, true);   // User Placement
    if (architectural(obj))
        lot_version++;   // the walls get its hole
    if (held.bought) {
        add_money(-held.price);
        bought[obj->id] = { held.price, day_number() };
        for (Entity *part : parts_of(obj)) {
            // Born today (FreeSO VMContext.SetBirthTime), then main on its thread.
            part->data[21] = int16_t(clock.minutes);
            part->data[24] = int16_t(clock.hours);
            part->data[ObjBirthDay] = int16_t(clock.day);
            part->data[ObjBirthMonth] = int16_t(clock.month);
            part->data[ObjBirthYear] = int16_t(clock.year);
            if (part->data[VarCurrentValue] == 0)
                part->data[VarCurrentValue] = int16_t(held.price);
            part->run_entry(*this, 1, false);
        }
    }
    held = Held();
    return true;
}

// The game's own code making or removing an object whose picture depends on its like neighbours (entry
// 8, "dynamic multi-tile update": a flood's sixteen pictures by which sides have a flood too, flood.iff
// 4104). FreeSO runs it on every position change (VMGameObject.PositionChange); ours only from buy
// mode, so floods made by a broken sink or toilet never joined (the user's report, 2026-11-10).
// TS_DYNAMIC_PLACED_OFF=1: buy mode only, as before.
void Vm::dynamic_placed(Entity &lead)
{
    static const bool off = TS_ENV("TS_DYNAMIC_PLACED_OFF") != nullptr;
    if (off)
        return;
    for (Entity *part : parts_of(&lead)) {
        if (!part->in_world() || part->container || !part->info || part->entry_points.size() <= 8 ||
            part->entry_points[8].action == 0)
            continue;
        neighbours_changed(*this, part->tile_x(), part->tile_y(), part->level, part->info->guid);
        dynamic_multitile(*this, *part);
    }
}

void Vm::dynamic_left(int x, int y, int level, uint32_t guid)
{
    static const bool off = TS_ENV("TS_DYNAMIC_PLACED_OFF") != nullptr;
    if (!off)
        neighbours_changed(*this, x, y, level, guid);
}

void Vm::walls_changed()
{
    edits++;
    for (auto &e : entities)
        if (!e->dead && !e->person && e->in_world() && !e->container)
            wall_adjacency(*this, *e);
}

void Vm::drop_held()
{
    edits++;
    Entity *obj = held.obj;
    if (!obj)
        return;
    if (held.bought) {
        remove_whole(*this, obj);
    } else if (held.from_container && !held.from_container->dead) {
        held.from_container->place_in_slot(obj, held.from_slot);
    } else {
        obj->place_whole(held.from_x16, held.from_y16, held.from_level, held.from_direction);
        position_changed(*this, obj);
    }
    held = Held();
}

int Vm::sell_value(const Entity *obj) const
{
    // The exe's FUN_0053adc0: the lead part's current value (object data 41), halved when any part is
    // broken (data 15, the repair state). The value itself goes down at midnight (new_day); bought
    // today, it's still the price paid. (Ours used to work it out from the birth date, FreeSims', on
    // top of the value the lot's file already kept: the shipped lots' objects came out low.)
    if (!obj || !obj->info)
        return 0;
    const Entity *lead = lead_of(const_cast<Entity *>(obj));
    bool broken = false;
    for (const Entity *part : lead->group.empty() ? std::vector<Entity *>{ const_cast<Entity *>(lead) } : lead->group)
        broken = broken || part->data[VarRepairState] != 0;
    const int value = lead->data[VarCurrentValue];
    return std::max(0, broken ? value / 2 : value);
}

Vm::HouseStats Vm::house_stats() const
{
    HouseStats s;
    // Rooms: inside ones' area, and which have a bed or a bathroom fixture (objects shown, in a room).
    std::vector<uint8_t> bed(rooms.size(), 0), bath(rooms.size(), 0);
    int outdoor_good = 0, outdoor_bad = 0, upkeep_count = 0, upkeep_penalty = 0;
    for (const auto &up : entities) {
        const Entity &e = *up;
        if (e.dead || e.person || !e.info)
            continue;
        // Upkeep (FUN_0052e0e0): every object with room or function flags or a build-mode type.
        if (e.info->objd.get(Objd::RoomFlags) || e.info->objd.get(Objd::FunctionFlags) || e.info->objd.get(Objd::BuildModeType)) {
            upkeep_count++;
            if (e.value(VarRepairState) != 0)
                upkeep_penalty += 6;
            else if (e.value(VarDirtyLevel) > 0)
                upkeep_penalty += 3;
        }
        if (!e.in_world() || e.container || e.value(VarHidden) != 0)
            continue;
        const uint16_t r = room_at(e.level, e.tile_x(), e.tile_y());
        if (r == 0 || r >= rooms.size())
            continue;
        const int cat = e.value(VarCategory);
        if (cat == 4)
            bed[r] = 1;
        else if (cat >= 5 && cat <= 7)
            bath[r] = 1;
        if (rooms[r].outside) {   // the yard's sum (FUN_00627420): good ones once, bad ones' impact twice
            const int impact = e.value(VarRoomImpact);
            if (impact >= 0)
                outdoor_good++;
            else
                outdoor_bad += impact;
        }
    }
    int pool_tiles = 0;
    for (size_t r = 1; r < rooms.size(); r++) {
        const Room &room = rooms[r];
        if (room.pool)
            pool_tiles += room.area;
        if (room.outside || room.area <= 0)
            continue;
        s.sqft += room.area * 9;
        s.bedrooms += bed[r];
        s.bathrooms += bath[r];
    }
    s.lot_word = lot.size < 40 ? 0 : lot.size < 50 ? 1 : 2;
    // STR# 505's curves (FUN_0056b8a0): straight between the points, the ends beyond them, truncated.
    auto curve = [](const std::vector<std::pair<float, float>> &pts, float x) {
        if (x <= pts.front().first)
            return int(pts.front().second);
        for (size_t i = 1; i < pts.size(); i++)
            if (x <= pts[i].first) {
                const auto [x0, y0] = pts[i - 1];
                const auto [x1, y1] = pts[i];
                return int(y0 + (y1 - y0) * (x - x0) / (x1 - x0));
            }
        return int(pts.back().second);
    };
    static const std::vector<std::pair<float, float>> kSize = { { 0, 0 }, { 150, 25 }, { 300, 50 }, { 600, 75 }, { 1200, 100 } };
    static const std::vector<std::pair<float, float>> kFurnish = { { 0, 100 }, { 1, 60 }, { 2, 35 }, { 3, 20 }, { 4, 10 }, { 5, 0 } };
    const Family *fam = family();
    const int members = fam ? int(fam->members.size()) : 0;
    s.bars[0] = members > 0 ? curve(kSize, float(s.sqft / members)) : 0;
    const LotValues v = lot_values();
    s.bars[1] = curve(kFurnish, v.objects > 0 ? float(v.architecture) / float(v.objects) : 100.0f);
    s.bars[2] = int(std::clamp(float(outdoor_good + pool_tiles + 2 * outdoor_bad) * 0.33f, 0.0f, 100.0f));
    s.bars[3] = upkeep_count > 0 ? int(std::clamp(float(upkeep_count - upkeep_penalty) / float(upkeep_count), 0.0f, 1.0f) * 100) : 0;
    int t = 0, b = 0;
    for (int i = 0; i < 6; i++) {
        t += layout_t[i];
        b += layout_b[i];
    }
    s.bars[4] = int(std::clamp(float(t - 2 * b) / float(std::max(t, 1)), 0.0f, 1.0f) * 100);
    for (int &bar : s.bars)
        bar = std::clamp(bar, 0, 100);
    return s;
}

void Vm::new_day()
{
    {   // the Layout bar's days (FUN_0056aef0): slot 0 into 1-4 (the original's shift), today into 0
        const int t0 = layout_t[0], b0 = layout_b[0];
        for (int i = 1; i <= 4; i++) {
            layout_t[i] = t0;
            layout_b[i] = b0;
        }
        layout_t[0] = layout_t[5];
        layout_b[0] = layout_b[5];
        layout_t[5] = layout_b[5] = 0;
    }
    // The exe's midnight (FUN_005576f0 -> FUN_0052cef0, FUN_0053a640 on every object): each object's
    // age from its birth date (FUN_0053e760: data 18 whole years, data 48 months), then the lead part's
    // value down by its OBJD depreciation unless it does its own (SelfDepreciating): the initial
    // depreciation the first night (the value still the price), the daily one after, never under the
    // limit.
    for (auto &up : entities) {
        Entity &e = *up;
        if (e.dead || !e.info || e.person)
            continue;
        {   // (every object, as the exe: one its code made has no birth date, and counts from year 0)
            int months = clock.month - e.data[ObjBirthMonth], years = clock.year - e.data[ObjBirthYear];
            if (clock.day - e.data[ObjBirthDay] < 0)
                months--;
            if (months < 0) {
                months += 12;
                years--;
            }
            e.data[VarSimAge] = int16_t(years);
            e.data[48] = int16_t(months + years * 12);
        }
        if (lead_of(&e) != &e)
            continue;
        const Objd &d = (e.master ? e.master : e.info)->objd;
        if (d.get(Objd::SelfDepreciating) != 0)
            continue;
        const int value = e.data[VarCurrentValue];
        const int initial = int16_t(d.get(Objd::InitialDepreciation));
        const int loss = value == int16_t(d.get(Objd::Price)) && initial != 0 ? initial : int16_t(d.get(Objd::DailyDepreciation));
        // Down to the limit, never up to it: something never bought is worth 0 and stays so. Ours took the
        // larger of the limit and the new value, so the first midnight gave every such object its limit:
        // the roach controller, made from the VR set's template (limit 460), became the most valuable
        // thing on the Granges' lot, and a burglar "stole" it where it was, off the lot, and left ten
        // ticks after his "Burglar!" (the user heard the alert and saw nobody, 2026-11-10).
        // TS_DEPRECIATE_OLD=1: as before.
        static const bool depreciate_old = TS_ENV("TS_DEPRECIATE_OLD") != nullptr;
        const int limit = int16_t(d.get(Objd::DepreciationLimit));
        if (depreciate_old)
            e.data[VarCurrentValue] = int16_t(std::max<int>(limit, value - loss));
        else if (value > limit)
            e.data[VarCurrentValue] = int16_t(std::max<int>(limit, value - loss));
    }
}

Vm::LotValues Vm::lot_values() const
{
    // A pool tile: the exe's own figure (DAT_00a51898, 100), not the Pool Tool's price (build.iff STR# 133:
    // 75); water as the Water Tool. A diagonal tile's floor id, and a diagonal wall's style.
    constexpr int kWaterTile = 15;
    const int kPoolTile = TS_ENV("TS_OLD_ARCH_VALUE") ? 75 : 100;
    constexpr uint16_t kFloorDiagonal = 255, kDiagonalStyle = 255;
    LotValues out;
    for (const auto &up : entities) {   // FUN_0052ca90
        const Entity &e = *up;
        // As the exe (FUN_0052ca90 with FUN_0053bd80/FUN_0053bdd0): lead parts only, not object type 7;
        // the objects' sum takes what's on the lot (it or what holds it within the lot's edge), without
        // movement flag 0x10, a person or of build-mode type 0; the rest of build-mode types other than
        // 0 and 4 go to the architecture. (Ours skipped what players can't delete, an approximation.)
        if (e.dead || !e.info || (!e.group.empty() && lead_of(const_cast<Entity *>(&e)) != &e))
            continue;
        const Objd &d = (e.master ? e.master : e.info)->objd;
        if (e.info->objd.get(Objd::ObjectType) == 7)
            continue;
        const int type = d.get(Objd::BuildModeType);
        const Entity *top = &e;
        while (top->container)
            top = top->container;
        const bool on_lot = top->in_world() && top->tile_x() >= 1 && top->tile_y() >= 1 && top->tile_x() < lot.size - 1 &&
                            top->tile_y() < lot.size - 1;
        if (TS_ENV("TS_LOT_VALUE_LIST")) {   // debug: each object's part in the sums
            std::printf("value #%d %s: %d (price %d, type %d, movement %04x, %s)\n", int(e.id), e.info->label.c_str(),
                        sell_value(&e), int(d.get(Objd::Price)), type, unsigned(uint16_t(e.value(VarMovementFlags))),
                        on_lot ? "on the lot" : "off");
            for (const Entity *p : e.group)
                std::printf("    part #%d %s: sub %04x lead flag %d, value %d\n", int(p->id), p->info->label.c_str(),
                            unsigned(p->info->objd.get(Objd::SubIndex)), int(p->info->objd.get(Objd::MyLeadObject)),
                            int(p->data[VarCurrentValue]));
        }
        if (on_lot && !(e.value(VarMovementFlags) & 0x10) && (e.person || type == 0))
            out.objects += sell_value(&e);
        else if (type != 0 && type != 4)
            out.arch_objects += sell_value(&e);
    }
    static std::map<uint16_t, int> floor_price, cover_price, style_price;   // FUN_00582410
    static std::map<std::string, int> file_price;   // the expansions' and downloads' .flr/.wll, by file name
    auto lower = [](std::string t) {
        for (char &c : t)
            c = char(std::tolower(uint8_t(c)));
        return t;
    };
    if (floor_price.empty())
        for (const BuildItem &b : build_mode_catalog(content))
            if (!b.file.empty() && (b.kind == BuildItem::Floor || b.kind == BuildItem::Wallpaper))
                file_price[lower(b.file)] = b.price;
            else if (b.kind == BuildItem::Floor)
                floor_price[b.pattern] = b.price;
            else if (b.kind == BuildItem::Wallpaper)
                cover_price[b.pattern] = b.price;
            else if (b.kind == BuildItem::WallStyle)
                style_price[b.pattern] = b.price;
    // A floor or covering from its own file (the lot's FLRm/WALm map) is priced by that file, as the
    // exe's FUN_0059b3b0/FUN_0059f9d0 look the pattern up in what's loaded. Ours priced only the
    // game's own (Build.iff) and took the rest as free: the community lots' walls bare.
    static const bool old_files = TS_ENV("TS_OLD_ARCH_VALUE") != nullptr;
    auto of = [&](const std::map<uint16_t, int> &m, uint16_t k) {
        const std::string *file = old_files ? nullptr : &m == &floor_price ? lot.custom_floor(k) : &m == &cover_price ? lot.custom_wall(k) : nullptr;
        if (file) {
            auto it = file_price.find(lower(*file));
            return it == file_price.end() ? 0 : it->second;
        }
        auto it = m.find(k);
        return it == m.end() ? 0 : it->second;
    };
    // The exe's style table (DAT_00a7bac0, filled at start-up by FUN_00536aa0) holds the five wall
    // tools' prices at their styles (1, 2, 12, 13, 14) and the plain wall's at the diagonal style
    // (255); every other style (3, 22, 23: the pieces beside doors and diagonals) is worth nothing.
    // Ours priced those as plain walls, the lots' architecture 12-20% over what they store.
    auto style_of = [&](uint16_t k) {
        static const bool old_arch = TS_ENV("TS_OLD_ARCH_VALUE") != nullptr;
        auto it = style_price.find(k);
        if (it != style_price.end())
            return it->second;
        return old_arch || k == kDiagonalStyle ? of(style_price, 1) : 0;
    };
    for (int level = 0; level < kLotLevels; level++) {
        if (lot.walls[level].empty())
            continue;
        for (int y = 0; y < lot.size; y++)
            for (int x = 0; x < lot.size; x++) {
                if (!lot.tile_flags[level].empty() && (lot.tile_flags[level][size_t(x) + size_t(y) * kLotArraySize] & 0x20))
                    continue;   // the road and pavement
                // As the exe's FUN_00582410: every wall once (it counts each from both sides and halves),
                // a face's covering only on wall styles 1, 7, 8 and 9 (not fences), a diagonal tile's
                // two floors (the wall tile's top-left style and pattern) at half price each.
                static const bool old_arch = TS_ENV("TS_OLD_ARCH_VALUE") != nullptr;   // debug: the old sums
                auto papered = [](uint16_t style) { return style == 1 || style == 7 || style == 8 || style == 9; };
                const uint16_t f = lot.floors[level].empty() ? 0 : lot.floor(x, y, level);
                const WallTile &w = lot.wall(x, y, level);
                const bool diagonal = (w.segments & (WallHorizontalDiag | WallVerticalDiag)) != 0;
                if (f == kFloorPool)
                    out.pool_tiles++;
                else if (f == kFloorWater)
                    out.floors += kWaterTile;
                else if (f == kFloorDiagonal && diagonal && !old_arch)
                    out.floors += of(floor_price, w.top_left_style) / 2 + of(floor_price, w.top_left_pattern) / 2;
                else if (f != 0)
                    out.floors += of(floor_price, f);
                if (diagonal && !old_arch) {
                    out.walls += style_of(w.top_right_style);
                    if (papered(w.top_right_style) || w.top_right_style == kDiagonalStyle)
                        out.coverings += of(cover_price, w.bottom_right_pattern) + of(cover_price, w.bottom_left_pattern);
                    continue;
                }
                if (w.segments & WallTopLeft) {
                    out.walls += style_of(w.top_left_style);
                    if (old_arch || papered(w.top_left_style))
                        out.coverings += of(cover_price, w.top_left_pattern);
                }
                if (w.segments & WallTopRight) {
                    out.walls += style_of(w.top_right_style);
                    if (old_arch || papered(w.top_right_style))
                        out.coverings += of(cover_price, w.top_right_pattern);
                }
                // The far sides' faces are of the neighbours' walls: their styles decide.
                if (x + 1 < kLotArraySize) {
                    const WallTile &e = lot.wall(x + 1, y, level);
                    if ((e.segments & WallTopLeft) && (old_arch || papered(e.top_left_style)))
                        out.coverings += of(cover_price, w.bottom_right_pattern);
                }
                if (y + 1 < kLotArraySize) {
                    const WallTile &s = lot.wall(x, y + 1, level);
                    if ((s.segments & WallTopRight) && (old_arch || papered(s.top_right_style)))
                        out.coverings += of(cover_price, w.bottom_left_pattern);
                }
            }
    }
    out.architecture = out.arch_objects + out.walls + out.coverings + out.floors + out.pool_tiles * kPoolTile;
    return out;
}

bool Vm::sell(Entity *obj, std::string &why)
{
    edits++;
    obj = lead_of(obj);
    why = movable_problem(obj, true);
    if (!why.empty())
        return false;
    const int value = sell_value(obj);
    obj->run_entry(*this, 12, true);   // User Pickup
    const std::vector<Entity *> parts = parts_of(obj);
    std::vector<std::tuple<int, int, int, uint32_t>> places;
    for (Entity *part : parts)
        if (part->in_world() && part->info)
            places.emplace_back(part->tile_x(), part->tile_y(), part->level, part->info->guid);
    if (architectural(obj))
        lot_version++;
    remove_whole(*this, obj);
    for (auto &[x, y, level, guid] : places)
        neighbours_changed(*this, x, y, level, guid);
    bought.erase(obj->id);
    add_money(value);
    return true;
}

void Vm::bulldoze()
{
    drop_held();
    auto street = [&](int x, int y) {   // the road and pavement (floors 9-12): the lot's street
        const uint16_t f = lot.floors[0].empty() ? 0 : lot.floor(x, y, 0);
        return f >= 9 && f <= 12;
    };
    for (size_t i = 0; i < entities.size(); i++) {
        Entity *e = entities[i].get();
        if (e->dead || e->person || !e->in_world() || lead_of(e) != e)
            continue;
        bool deletable = true, on_street = false;
        for (Entity *part : parts_of(e)) {
            if (!(part->value(VarMovementFlags) & kPlayersCanDelete))
                deletable = false;
            if (part->level == 1 && street(part->tile_x(), part->tile_y()))
                on_street = true;
        }
        if (deletable && !on_street) {
            remove_whole(*this, e);
            bought.erase(e->id);
        }
    }
    for (int l = 0; l < kLotLevels; l++) {
        for (WallTile &w : lot.walls[size_t(l)])
            w = WallTile();
        for (int y = 0; y < kLotArraySize; y++)
            for (int x = 0; x < kLotArraySize; x++) {
                const size_t i = size_t(x + y * kLotArraySize);
                if (i < lot.floors[size_t(l)].size() && !(l == 0 && street(x, y)))
                    lot.floors[size_t(l)][i] = 0;
            }
    }
    lot_edited();
}

} // namespace ts::vm
