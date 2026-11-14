/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Build mode, after FreeSO (tso.content/WorldFloorProvider.cs and
 * WorldWallProvider.cs for the catalogue, VMArchitectureTools for the walls).
 */

// Walls, floors and wall coverings, as the player changes them. Prices and
// names are the game's own (Build.iff STR# 0x81 wall tools, 0x82 floors,
// 0x83 wall coverings; .flr/.wll files carry theirs in STR# 0). A wall is
// kept on both tiles it separates (a north edge is one tile's top right and
// the other's bottom left), and each tile keeps the covering of that wall's
// face seen from it.
#include <mutex>
#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "files/iff.h"
#include "files/install.h"
#include "vm/vm.h"
#include "world/rooms.h"

namespace ts::vm {

namespace {

// Wall tools: style id in the lot, the pattern a new one gets, and its
// entry in Build.iff STR# 0x81 (FreeSO WallStyleIDs / WallStylePatterns).
struct WallTool {
    uint16_t style, pattern;
    int strings;
};
constexpr WallTool kWallTools[] = { { 1, 0, 0 }, { 2, 248, 1 }, { 0xD, 250, 2 }, { 0xC, 249, 3 }, { 0xE, 251, 4 } };
// A tile's floor split by a diagonal wall: the halves' floors are the wall tile's top-left
// pattern and style.
constexpr uint16_t kFloorDiagonal = 255;

// A STR# table of a file as triples or pairs, English.
std::vector<std::string> strings_of(const uint8_t *data, const IffFile &iff, uint16_t id)
{
    std::vector<std::string> out;
    const IffChunk *c = iff.find("STR#", id);
    StringTable t;
    std::string err;
    if (!c || !parse_str(data + c->offset, c->size, t, err))
        return out;
    for (size_t i = 0;; i++) {
        const std::string *s = t.get(i);
        if (!s)
            break;
        out.push_back(*s);
    }
    return out;
}

bool read_iff(const FileRef &ref, std::vector<uint8_t> &bytes, IffFile &iff)
{
    std::string err;
    return read_ref(ref, bytes) && parse_iff(bytes.data(), bytes.size(), iff, err);
}

std::string folded(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(uint8_t(c)));
    return s;
}

// Turns wall sides (bits 0-3) by `rotate` quarters (as buy.cpp).
int rotate_sides(int sides, int rotate)
{
    return ((sides << (4 - rotate)) & 15) | ((sides & 15) >> rotate);
}

} // namespace

uint16_t wall_tool_pattern(uint16_t style)
{
    for (const WallTool &t : kWallTools)
        if (t.style == style)
            return t.pattern;
    return 0;
}

std::vector<BuildItem> build_mode_catalog(const Content &content)
{
    std::vector<BuildItem> out;
    std::vector<uint8_t> bytes;
    IffFile iff;
    // The game's own: terrain tools, wall tools, floors and wall coverings in Build.iff.
    if (const FileRef *ref = content.file("build.iff"); ref && read_iff(*ref, bytes, iff)) {
        const auto terrain = strings_of(bytes.data(), iff, 0x80);   // raise, lower, level, grass (English first)
        for (int t = 0; t < 4 && size_t(t) * 3 + 2 < terrain.size(); t++) {
            BuildItem item;
            item.kind = BuildItem::Terrain;
            item.pattern = uint16_t(t);
            item.price = std::atoi(terrain[size_t(t) * 3].c_str());
            item.name = terrain[size_t(t) * 3 + 1];
            item.description = terrain[size_t(t) * 3 + 2];
            out.push_back(item);
            if (t == BuildItem::kTerrainGrass && !TS_ENV("TS_SAND_TOOL_OFF")) {   // ours: the grass taken away, its own tool
                item.pattern = BuildItem::kTerrainSand;
                item.name = "Sand";
                item.description = "Takes the grass away, down to the sand under it. The grass tool puts it back.\n\n"
                                   "Landscaping: Click or click and drag to take the grass from the terrain.";
                out.push_back(item);
            }
        }
        const auto tools = strings_of(bytes.data(), iff, 0x81);
        for (const WallTool &t : kWallTools)
            if (size_t(t.strings) * 3 + 2 < tools.size()) {
                BuildItem item;
                item.kind = BuildItem::WallStyle;
                item.pattern = t.style;
                item.price = std::atoi(tools[size_t(t.strings) * 3].c_str());
                item.name = tools[size_t(t.strings) * 3 + 1];
                item.description = tools[size_t(t.strings) * 3 + 2];
                out.push_back(item);
            }
        for (const auto &[table, kind] : { std::pair<uint16_t, BuildItem::Kind>{ 0x82, BuildItem::Floor },
                                           std::pair<uint16_t, BuildItem::Kind>{ 0x83, BuildItem::Wallpaper } }) {
            const auto s = strings_of(bytes.data(), iff, table);
            for (size_t i = 0; i + 2 < s.size(); i += 3) {
                BuildItem item;
                item.kind = kind;
                item.pattern = uint16_t(i / 3 + 1);
                item.price = std::atoi(s[i].c_str());
                item.name = s[i + 1];
                item.description = s[i + 2];
                out.push_back(item);
            }
        }
    }
    // The Pool and Water Tools (build.iff STR# 133): a price, a name and a description each, with
    // the pool's objects in the catalogue, laying the two special floors.
    if (const FileRef *ref = content.file("build.iff"); ref && read_iff(*ref, bytes, iff)) {
        const auto s = strings_of(bytes.data(), iff, 133);
        const uint16_t kinds[2] = { kFloorPool, kFloorWater };
        for (size_t i = 0; i < 2 && i * 3 + 2 < s.size(); i++) {
            BuildItem item;
            item.kind = BuildItem::Floor;
            item.category = 7;
            item.pattern = kinds[i];
            item.price = std::atoi(s[i * 3].c_str());
            item.name = s[i * 3 + 1];
            item.description = s[i * 3 + 2];
            out.push_back(item);
        }
    }
    // The roof tool (UIText STR# 147, the original's roof panel): the four pitches in its order,
    // top to bottom, then the patterns in GameData/Roofs by name, as its panel lists them
    // (case aside: gravel1, MMGypsyTentRoof, ..., r1_ .. r4_, Superstar..., tar1).
    {
        std::vector<std::string> ui;
        if (const FileRef *ref = content.file("uitext.iff"); ref && read_iff(*ref, bytes, iff))
            ui = strings_of(bytes.data(), iff, 147);
        auto text = [&](size_t i, const char *fallback) {
            return i < ui.size() && !ui[i].empty() ? ui[i] : std::string(fallback);
        };
        static const struct { int pitch; size_t name; const char *fallback; } kPitches[4] = {
            { 3, 5, "Steep Roof Pitch" }, { 2, 3, "Medium Roof Pitch" }, { 1, 1, "Shallow Roof Pitch" }, { 0, 12, "Flat Roof" } };
        for (const auto &p : kPitches) {
            BuildItem item;
            item.kind = BuildItem::Roof;
            item.category = 0;
            item.pattern = uint16_t(p.pitch);
            item.name = text(p.name, p.fallback);
            item.description = text(15, "");
            out.push_back(item);
        }
        std::vector<std::string> names;
        for (const std::string &f : list_folder(content.root() + "/GameData/Roofs"))
            if (f.size() > 4 && folded(f.substr(f.size() - 4)) == ".bmp")
                names.push_back(f.substr(0, f.size() - 4));
        std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) { return folded(a) < folded(b); });
        for (size_t i = 0; i < names.size(); i++) {
            BuildItem item;
            item.kind = BuildItem::Roof;
            item.category = 1;
            item.file = names[i];
            // The original's patterns have no names (its panel shows the textures alone): ours, from
            // the look of each; one the game doesn't ship (a download) is numbered.
            static const std::pair<const char *, const char *> kNames[] = {
                { "gravel1", "Gravel" },
                { "mmgypsytentroof", "Gypsy Tent Stripes" },
                { "mmoldwood1roof", "Weathered Wood Shingles" },
                { "mmoldwood2roof", "Mossy Scalloped Shingles" },
                { "mmsuburban1roof", "Grey Suburban Shingles" },
                { "mmsuburban2roof", "Green Suburban Shingles" },
                { "mmtent1roof", "Big Top Canvas" },
                { "mmtent2roof", "Sunset Canvas" },
                { "r1_", "Blue Slate" },
                { "r2_", "Limestone Shingles" },
                { "r3_", "Charcoal Asphalt Shingles" },
                { "r4_", "Terracotta Tiles" },
                { "superstardirtyroof", "Sooty Shingles" },
                { "superstarfashioncopperroof", "Copper Tiles" },
                { "superstarfashionroof", "Ice Blue Tiles" },
                { "superstargoldroof", "Gold Tiles" },
                { "superstartentroof", "White Marquee Canvas" },
                { "tar1", "Tar and Pitch" },
            };
            item.name = "Roof Pattern " + std::to_string(i + 1);
            for (const auto &[file, name] : kNames)
                if (folded(names[i]) == file)
                    item.name = name;
            item.description = text(17, "");
            out.push_back(item);
        }
    }
    // The rest, in their own files (expansions and downloads).
    for (const std::string &name : content.file_names()) {
        const bool floor = name.size() > 4 && name.compare(name.size() - 4, 4, ".flr") == 0;
        const bool wall = name.size() > 4 && name.compare(name.size() - 4, 4, ".wll") == 0;
        if (!floor && !wall)
            continue;
        const FileRef *ref = content.file(name);
        if (!ref || !read_iff(*ref, bytes, iff))
            continue;
        const auto s = strings_of(bytes.data(), iff, 0);
        if (s.size() < 2)
            continue;
        BuildItem item;
        item.kind = floor ? BuildItem::Floor : BuildItem::Wallpaper;
        item.file = name;
        item.name = s[0];
        item.price = std::atoi(s[1].c_str());
        item.description = s.size() > 2 ? s[2] : std::string();
        out.push_back(item);
    }
    // Doors, windows, stairs and the rest: objects with a build category.
    for (const auto &entry : content.objects().by_guid) {
        const ObjectInfo &info = entry.second;
        const Objd &d = info.objd;
        const uint16_t type = d.get(Objd::BuildModeType);
        const uint16_t master = d.get(Objd::MasterID), sub = d.get(Objd::SubIndex);
        if (type == 0 || d.get(Objd::FunctionFlags) != 0 || d.get(Objd::Disabled) != 0 ||
            (master == 0 && d.get(Objd::NumGraphics) == 0) || (master != 0 && sub != 0xffff))
            continue;
        BuildItem item;
        item.kind = BuildItem::Object;
        item.info = &info;
        item.category = type;
        item.name = info.name.empty() ? info.label : info.name;
        item.price = d.get(Objd::Price);
        if (TS_ENV("TS_BUILD_TYPES"))   // debug: each build-mode object with its type
            std::printf("build type %d: %s (%s)\n", int(type), item.name.c_str(), info.label.c_str());
        out.push_back(item);
    }
    std::stable_sort(out.begin(), out.end(), [](const BuildItem &a, const BuildItem &b) {
        // By price, cheapest first (the user's choice: the original lists the wall tool first,
        // then the fences in its own order); the terrain tools (a simoleon each) in the game's order.
        if (a.kind != b.kind)
            return a.kind < b.kind;
        if (a.kind == BuildItem::Terrain || a.kind == BuildItem::Roof)
            return a.kind == BuildItem::Roof && a.category < b.category;   // the game's order
        if (a.kind == BuildItem::WallStyle)
            return a.price < b.price;
        return a.category != b.category ? a.category < b.category : a.price < b.price;
    });
    return out;
}

// Build mode's sub-groups: ours (the original lists floors, wall coverings, doors and windows
// whole, and its files have nothing to group them by; the user, 2026-11-10: "a bit of a pain to
// find what you want", by material or style). Each group a list of words; an item goes in the
// first group a word of its name matches, else the first a word of its description matches (the
// description's flavour text only, not the shared tips). A word ending in '*' matches any word it
// begins; the others a whole word, or with an "s" after it.
namespace {

struct Subgroup {
    const char *name;
    std::vector<const char *> words;
};

const std::vector<Subgroup> &subgroups_of(BuildItem::Kind kind, int category)
{
    static const std::vector<Subgroup> floors = {
        { "Carpet & Rugs", { "carpet*", "rug", "shag", "berber", "plush", "mat", "tatami", "felt" } },
        { "Wood", { "wood*", "hardwood*", "board*", "floorboard*", "plank*", "parquet", "deck*", "oak", "maple", "pine",
                    "walnut", "teak", "slat*", "timber*", "bamboo", "cedar", "birch", "cherry", "mahogany", "creak" } },
        { "Tile & Linoleum", { "tile*", "lino*", "ceramic*", "checkerboard", "mosaic", "vinyl", "porcelain" } },
        { "Stone & Brick", { "stone*", "brick*", "marble*", "granite", "slate", "cobble*", "flagstone", "terracotta",
                             "travertine", "paver*", "shale", "earthenware", "rock*", "boulder*", "corundum", "jade" } },
        { "Concrete & Metal", { "concrete", "cement*", "steel", "metal*", "iron", "grate*", "grating", "manhole", "industrial",
                                "macadam", "asphalt", "sidewalk", "pavement", "epoxi*", "epoxy", "aluminum" } },
        { "Outdoor & Natural", { "grass*", "gravel", "dirt", "soil", "sand*", "moss*", "hay", "straw", "terrain", "path",
                                 "earth*", "mud", "lawn", "turf", "lava" } },
    };
    static const std::vector<Subgroup> walls = {
        { "Wallpaper & Fabric", { "wallpaper*", "paper", "fabric*", "velvet", "lace", "canvas", "rice", "carpet*", "woven",
                                  "print", "linings" } },
        { "Brick & Stone", { "brick*", "stone*", "marble*", "granite", "rock*", "travertine", "castle", "ashlar", "block",
                             "keep", "brownstone", "slate", "cement", "concrete", "jade" } },
        { "Tile & Metal", { "tile*", "steel", "metal*", "aluminum", "tin", "plastic", "tetraplastic", "vinyl", "glass",
                            "chrome", "ceramic" } },
        { "Wood & Siding", { "wood*", "panelling", "paneling", "clapboard", "shingle*", "siding", "plank*", "log*",
                             "timber*", "bamboo", "wainscot*", "walnut", "fir*", "tudor*", "shoji", "barn*", "wagon" } },
        { "Paint & Plaster", { "paint*", "stucco*", "plaster", "whitewash*", "adobe", "fresco", "sponge", "color*",
                               "colour*", "pigment*" } },
        { "Themed", { "japan*", "samurai", "polynesian", "space", "galaxy", "interstellar", "frigate", "dragon*", "haunted",
                      "spooky", "bats", "coffin", "tropical", "island", "hawaiian", "tiki", "western", "pioneer*", "movie*",
                      "film", "jungle", "carnival*", "medieval", "samurai", "renaissance", "historical", "stars" } },
    };
    static const std::vector<Subgroup> doors = {
        { "Doors", {} },   // (the rest)
        { "Glass & Sliding", { "glass", "sliding", "french", "lattice", "window", "screen" } },
        { "Arches & Openings", { "arch*", "doorway", "doorless", "frame", "curtain", "opening", "fornix" } },
    };
    static const std::vector<Subgroup> windows = {
        { "Windows", {} },   // (the rest)
        { "Shuttered & Casement", { "shutter*", "casement", "hung", "sash" } },
        { "Stained & Decorative", { "stained", "cathedral", "deko", "deco", "bubble", "trapezoi*", "pill", "starry",
                                    "slit", "round", "porthole" } },
    };
    static const std::vector<Subgroup> none;
    if (kind == BuildItem::Floor)
        return floors;
    if (kind == BuildItem::Wallpaper)
        return walls;
    if (kind == BuildItem::Object && category == 1)
        return doors;
    if (kind == BuildItem::Object && category == 2)
        return windows;
    return none;
}

std::vector<std::string> words_of(const std::string &text)
{
    std::vector<std::string> out;
    std::string w;
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c >= 0x80)
            w += char(std::tolower(c));
        else if (!w.empty())
            out.push_back(std::move(w)), w.clear();
    }
    if (!w.empty())
        out.push_back(std::move(w));
    return out;
}

int group_matching(const std::vector<Subgroup> &groups, const std::vector<std::string> &words)
{
    for (size_t g = 0; g < groups.size(); g++)
        for (const char *key : groups[g].words) {
            const std::string k = key;
            const bool prefix = !k.empty() && k.back() == '*';
            const std::string stem = prefix ? k.substr(0, k.size() - 1) : k;
            for (const std::string &w : words)
                if (prefix ? w.compare(0, stem.size(), stem) == 0 : (w == stem || w == stem + "s"))
                    return int(g);
        }
    return -1;
}

} // namespace

std::vector<std::string> build_subgroup_names(BuildItem::Kind kind, int category)
{
    std::vector<std::string> out;
    for (const Subgroup &g : subgroups_of(kind, category))
        out.push_back(g.name);
    return out;
}

int build_subgroup(const BuildItem &item)
{
    const std::vector<Subgroup> &groups = subgroups_of(item.kind, item.category);
    if (groups.empty())
        return -1;
    int g = group_matching(groups, words_of(item.name));
    if (g < 0) {
        std::string flavour = item.description;   // (not the tips every floor or covering shares)
        for (const char *tips : { "Decorating Tips", "Redecorating:", "Construction Tips", "Putting Up", "Building " })
            if (const size_t at = flavour.find(tips); at != std::string::npos)
                flavour.resize(at);
        g = group_matching(groups, words_of(flavour));
    }
    // Doors and windows: the first group is the rest; floors and coverings: "Other" (-1).
    return g >= 0 ? g : groups.front().words.size() == 0 ? 0 : -1;
}

const char *build_object_category_name(int type)
{
    switch (type) {
    case 1: return "Doors";
    case 2: return "Windows";
    case 3: return "Stairs";
    case 4: return "Plants";
    case 5: return "Fireplaces";
    case 6: return "Fences & Columns";
    case 7: return "Pool";
    default: return "Other";
    }
}

// ---------------------------------------------------------------- the lot

namespace {

void ensure_level(Lot &lot, int l)
{
    if (lot.floors[l].empty())
        lot.floors[l].assign(size_t(kLotArraySize) * kLotArraySize, 0);
    if (lot.walls[l].empty())
        lot.walls[l].assign(size_t(kLotArraySize) * kLotArraySize, WallTile());
}

WallTile &wall_at(Lot &lot, int x, int y, int l)
{
    return lot.walls[l][size_t(x) + size_t(y) * kLotArraySize];
}

uint16_t &floor_at(Lot &lot, int x, int y, int l)
{
    return lot.floors[l][size_t(x) + size_t(y) * kLotArraySize];
}

} // namespace

// Read once a session, not once a lot: Build.iff is 671 KB, and the first footstep on each lot
// read it whole on the game's thread (a 110 ms tick on the Vita downtown). The viewer asks for it
// as it starts, so even the first lot's first step doesn't.
const std::vector<std::string> &game_floor_names(const Content &content)
{
    static std::mutex once_lock;
    static std::vector<std::string> shared;
    static bool read = false;
    std::lock_guard<std::mutex> hold(once_lock);
    if (!read) {
        read = true;
        std::vector<uint8_t> bytes;
        IffFile iff;
        if (const FileRef *ref = content.file("build.iff"); ref && read_iff(*ref, bytes, iff)) {
            const auto s = strings_of(bytes.data(), iff, 0x82);
            for (size_t i = 0; i + 2 < s.size(); i += 3)
                shared.push_back(s[i + 1]);
        }
    }
    return shared;
}

const char *Vm::footstep_sound(const Entity &walker)
{
    const int x = walker.tile_x(), y = walker.tile_y(), level = std::clamp<int>(walker.level, 1, kLotLevels) - 1;
    if (x < 0 || y < 0 || x >= kLotArraySize || y >= kLotArraySize || lot.floors[level].empty())
        return "footstep_terrain";
    const uint16_t f = lot.floor(x, y, level);
    if (f == kFloorPool || f == kFloorWater)
        return nullptr;
    if (f == 0)
        return "footstep_terrain";   // no floor: the ground
    auto known = footstep_kinds_.find(f);
    if (known != footstep_kinds_.end())
        return known->second;
    std::string name;
    if (f >= kCustomStyle) {   // a floor of its own file: its name, and the file's
        const size_t i = size_t(f - kCustomStyle);
        if (i < lot.floor_styles.size()) {
            name = lot.floor_styles[i].name;
            std::vector<uint8_t> bytes;
            IffFile iff;
            if (const FileRef *ref = content.file(name); ref && read_iff(*ref, bytes, iff)) {
                const auto s = strings_of(bytes.data(), iff, 0);
                if (!s.empty())
                    name += " " + s[0];
            }
        }
    } else {
        if (floor_names_.empty())   // the game's own, as Build.iff lists them
            floor_names_ = game_floor_names(content);
        if (f >= 1 && size_t(f - 1) < floor_names_.size())
            name = floor_names_[size_t(f - 1)];
    }
    for (char &c : name)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](std::initializer_list<const char *> words) {
        for (const char *w : words)
            if (name.find(w) != std::string::npos)
                return true;
        return false;
    };
    const char *kind = "footstep_medium";   // wood, and anything not known
    if (has({ "carpet", "rug", "shag", "berber", "tatami", "mat", "plush" }))
        kind = "footstep_soft";
    else if (has({ "grass", "dirt", "sand", "gravel", "lawn", "mulch", "soil", "earth", "hay", "straw" }))
        kind = "footstep_terrain";
    else if (has({ "tile", "stone", "marble", "slate", "brick", "concrete", "cement", "lino", "terracotta", "granite",
                   "cobble", "flagstone", "ceramic", "terrazzo", "metal", "steel", "checker", "mosaic", "porcelain",
                   "quarry", "glass", "asphalt", "pavement", "tiling" }))
        kind = "footstep_hard";
    footstep_kinds_[f] = kind;
    return kind;
}

uint16_t Vm::style_for(const BuildItem &item)
{
    if (item.file.empty())
        return item.pattern;
    std::vector<StyleEntry> &styles = item.kind == BuildItem::Floor ? lot.floor_styles : lot.wall_styles;
    for (size_t i = 0; i < styles.size(); i++)
        if (folded(styles[i].name) == folded(item.file))
            return uint16_t(kCustomStyle + i);
    StyleEntry e;
    e.name = item.file;
    for (const StyleEntry &s : styles)   // a lot-local id the file doesn't use yet
        e.id = uint8_t(std::max<int>(e.id, s.id + 1));
    styles.push_back(e);
    return uint16_t(kCustomStyle + styles.size() - 1);
}

int Vm::shape_terrain(int x0, int y0, int x1, int y1, int how, std::string &why)
{
    constexpr int V = kLotArraySize + 1;
    if (lot.altitude.size() < size_t(V) * V)
        lot.altitude.assign(size_t(V) * V, 0);
    auto height = [&](int cx, int cy) -> uint8_t & { return lot.altitude[size_t(cx) + size_t(cy) * V]; };
    // The corners the game lets change: inside the lot's rim (its own edge corners stay), and not
    // where a tile round them has a floor, a wall or an object standing on the ground.
    std::vector<uint8_t> held(size_t(kLotArraySize) * kLotArraySize, 0);
    for (int y = 0; y < lot.size; y++)
        for (int x = 0; x < lot.size; x++)
            if ((!lot.floors[0].empty() && floor_at(lot, x, y, 0) != 0) || (!lot.walls[0].empty() && wall_at(lot, x, y, 0).segments))
                held[size_t(x) + size_t(y) * kLotArraySize] = 1;
    for (auto &e : entities)
        if (!e->dead && !e->person && e->in_world() && !e->container && e->level == 1 && e->tile_x() >= 0 &&
            e->tile_y() >= 0 && e->tile_x() < kLotArraySize && e->tile_y() < kLotArraySize)
            held[size_t(e->tile_x()) + size_t(e->tile_y()) * kLotArraySize] = 1;
    // (And the tiles round it editable: the exe's terrain tool, FUN_006572d0, tests the tile for
    // the lot's 0x20 mark as well. TS_EDITABLE_OLD=1: not tested, as before.)
    static const bool editable_old = TS_ENV("TS_EDITABLE_OLD") != nullptr;
    auto free_corner = [&](int cx, int cy) {
        if (cx < 1 || cy < 1 || cx > lot.size - 1 || cy > lot.size - 1)
            return false;
        for (int ty = cy - 1; ty <= cy; ty++)
            for (int tx = cx - 1; tx <= cx; tx++)
                if (held[size_t(tx) + size_t(ty) * kLotArraySize] || (!editable_old && !lot.user_tile(tx, ty, move_objects)))
                    return false;
        return true;
    };
    const int target = how == 0 && x0 >= 0 && y0 >= 0 && x0 < V && y0 < V ? height(x0, y0) : 0;
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    std::vector<std::pair<int, int>> change;
    bool blocked = false;
    for (int cy = std::max(y0, 0); cy <= std::min(y1, V - 1); cy++)
        for (int cx = std::max(x0, 0); cx <= std::min(x1, V - 1); cx++) {
            const int h = height(cx, cy);
            const int want = how > 0 ? std::min(255, h + 1) : how < 0 ? std::max(0, h - 1) : target;
            if (want == h)
                continue;
            if (!free_corner(cx, cy)) {
                blocked = true;
                continue;
            }
            change.emplace_back(cx, cy);
        }
    if (change.empty()) {
        why = blocked ? "Something is in the way" : "Nothing to change";
        return 0;
    }
    const int cost = int(change.size());   // the game's price: a simoleon each
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    for (auto [cx, cy] : change) {
        uint8_t &h = height(cx, cy);
        h = uint8_t(how > 0 ? h + 1 : how < 0 ? h - 1 : target);
    }
    add_money(-cost);
    lot_edited();
    return std::max(cost, 1);
}

int Vm::paint_grass(int x0, int y0, int x1, int y1, bool grow, std::string &why)
{
    if (lot.grass.size() < size_t(kLotArraySize) * kLotArraySize)
        lot.grass.assign(size_t(kLotArraySize) * kLotArraySize, 0);
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    const uint8_t want = grow ? 0 : 200;   // 0 lush; 200 as worn as the lot files have it
    std::vector<size_t> change;
    for (int y = std::max(y0, 0); y <= std::min(y1, lot.size - 1); y++)
        for (int x = std::max(x0, 0); x <= std::min(x1, lot.size - 1); x++)
            if (lot.grass[size_t(x) + size_t(y) * kLotArraySize] != want)
                change.push_back(size_t(x) + size_t(y) * kLotArraySize);
    if (change.empty()) {
        why = "Nothing to change";
        return 0;
    }
    const int cost = int(change.size());
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    for (size_t i : change)
        lot.grass[i] = want;
    add_money(-cost);
    lot_edited();
    return std::max(cost, 1);
}

bool Vm::set_roof(const BuildItem &item)
{
    if (item.kind != BuildItem::Roof)
        return false;
    if (item.category == 0) {
        const float pitch = BuildItem::kRoofPitches[std::min<int>(item.pattern, 3)];
        if (lot.hous.roof_pitch == pitch)
            return false;
        lot.hous.roof_pitch = pitch;
    } else {
        if (item.file.empty() || folded(lot.hous.roof_name) == folded(item.file))
            return false;
        lot.hous.roof_name = item.file;
    }
    lot_edited();
    return true;
}

void Vm::lot_edited()
{
    edits++;
    lot_changed = true;
    lot_version++;
    build_room_map();
    refresh_rooms();
    walls_changed();   // wall-adjacent objects learn of it (buy.cpp)
}

// The Pool or Water Tool over a rectangle, all of it or none: on the ground, on flat land of the lot
// itself, clear of walls, objects and Sims. Taking it away gives the ground back.
int Vm::build_pool(int x0, int y0, int x1, int y1, int level, const BuildItem &item, bool erase, std::string &why)
{
    if (level != 1) {
        why = "Pools go on the ground";
        return 0;
    }
    const int l = 0;
    ensure_level(lot, l);
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    x0 = std::max(x0, 1), y0 = std::max(y0, 1);
    x1 = std::min(x1, lot.size - 2), y1 = std::min(y1, lot.size - 2);
    const int n = kLotArraySize + 1;
    auto sloped = [&](int x, int y) {
        if (lot.altitude.size() < size_t(n) * size_t(n))
            return false;
        const uint8_t a = lot.altitude[size_t(x + y * n)];
        return lot.altitude[size_t(x + 1 + y * n)] != a || lot.altitude[size_t(x + (y + 1) * n)] != a ||
               lot.altitude[size_t(x + 1 + (y + 1) * n)] != a;
    };
    std::vector<std::pair<int, int>> tiles;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const uint16_t f = floor_at(lot, x, y, l);
            if (erase ? f != item.pattern : f == item.pattern)
                continue;
            if (!erase) {
                if (!lot.user_tile(x, y, move_objects)) {   // (the exe's FUN_0058aa90)
                    why = "That's outside the area you can build on";
                    return 0;
                }
                if (wall_at(lot, x, y, l).segments) {
                    why = "A wall is in the way";
                    return 0;
                }
                if (sloped(x, y)) {
                    why = "The ground must be flat";
                    return 0;
                }
            }
            for (const auto &up : entities) {
                const Entity &e = *up;
                if (!e.dead && e.in_world() && !e.container && e.level == level && e.tile_x() == x && e.tile_y() == y) {
                    why = e.person ? "Someone is in the way" : "Something is in the way";
                    return 0;
                }
            }
            tiles.emplace_back(x, y);
        }
    if (tiles.empty()) {
        why = "Nothing to change";
        return 0;
    }
    const int cost = erase ? 0 : item.price * int(tiles.size());
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    for (auto [x, y] : tiles)
        floor_at(lot, x, y, l) = erase ? 0 : item.pattern;
    add_money(-cost);
    lot_edited();
    return std::max(cost, 1);
}

// The exe's floorable rule for an upper storey (FUN_005826b0's last pass over x, y = 1 .. size - 2):
// the ground under the tile flat (its four corners' heights the same); then, with `below` the tile
// under it on the storey below: inside a room there (not reached from the lot's corner without
// crossing a wall; fences let through), or with a column there (an object of build type 6 on that
// tile: FUN_0053c260 sets the tile's bit 0x08), floorable. Else only if a tile beside it (its four
// sides) already has floor on this storey and, in the 5 x 5 tiles round it on the storey below, one
// is inside a room, or one of the inner eight has a column. (Ours: a room below, nothing else.)
bool Vm::floorable(int x, int y, int level) const
{
    const int l = level - 1;
    if (l < 1 || l >= kLotLevels || x < 1 || y < 1 || x > lot.size - 2 || y > lot.size - 2)
        return false;
    constexpr int V = kLotArraySize + 1;
    if (lot.altitude.size() >= size_t(V) * V) {
        auto h = [&](int cx, int cy) { return lot.altitude[size_t(cx) + size_t(cy) * V]; };
        const uint8_t a = h(x, y);
        if (h(x + 1, y) != a || h(x, y + 1) != a || h(x + 1, y + 1) != a)
            return false;
    }
    auto outside = [&](int tx, int ty) {   // on the storey below
        if (tx < 0 || ty < 0 || tx >= lot.size || ty >= lot.size)
            return true;
        const uint16_t r = room_at(level - 1, tx, ty);
        return r == 0 || r >= rooms.size() || rooms[r].outside;
    };
    auto column = [&](int tx, int ty) {
        for (const auto &e : entities)
            if (!e->dead && !e->person && e->info && e->in_world() && !e->container && e->level == level - 1 &&
                e->tile_x() == tx && e->tile_y() == ty && e->info->objd.get(Objd::BuildModeType) == 6)
                return true;
        return false;
    };
    if (!outside(x, y) || column(x, y))
        return true;
    auto floored = [&](int tx, int ty) {
        return tx >= 0 && ty >= 0 && tx < lot.size && ty < lot.size && !lot.floors[l].empty() && lot.floor(tx, ty, l) != 0;
    };
    if (!floored(x, y - 1) && !floored(x - 1, y) && !floored(x + 1, y) && !floored(x, y + 1))
        return false;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            if (!dx && !dy)
                continue;
            if (!outside(x + dx, y + dy))
                return true;
            if (std::abs(dx) <= 1 && std::abs(dy) <= 1 && column(x + dx, y + dy))
                return true;
        }
    return false;
}

int Vm::build_floor(int x0, int y0, int x1, int y1, int level, const BuildItem *item, std::string &why)
{
    if (item && item->category == 7)
        return build_pool(x0, y0, x1, y1, level, *item, false, why);
    const int l = level - 1;
    if (l < 0 || l >= kLotLevels) {
        why = "No such floor";
        return -1;
    }
    ensure_level(lot, l);
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    x0 = std::max(x0, 0), y0 = std::max(y0, 0);
    x1 = std::min(x1, lot.size - 1), y1 = std::min(y1, lot.size - 1);
    const uint16_t pattern = item ? style_for(*item) : 0;
    std::vector<std::pair<int, int>> tiles, split;
    int outside = 0;   // tiles of the rectangle off the editable area
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const uint16_t f = floor_at(lot, x, y, l);
            if (f == kFloorPool || f == kFloorWater || f == pattern)
                continue;
            // A new floor only on the lot's editable tiles (the exe's FUN_0069e8d0); taking one up
            // anywhere. TS_EDITABLE_OLD=1: any tile, as before.
            static const bool editable_old = TS_ENV("TS_EDITABLE_OLD") != nullptr;
            if (item && !editable_old && !lot.user_tile(x, y, move_objects)) {
                outside++;
                continue;
            }
            if (const WallTile &w = wall_at(lot, x, y, l); w.segments & (WallHorizontalDiag | WallVerticalDiag)) {
                // Split by a diagonal wall: its halves' floors are the wall tile's (top-left
                // pattern and style); a rectangle over it covers both.
                if (w.top_left_pattern != pattern || w.top_left_style != pattern)
                    split.emplace_back(x, y);
                continue;
            }
            if (item && l > 0) {   // upstairs: the exe's floorable tiles (a room or a column below, or beside floor)
                static const bool old_rule = TS_ENV("TS_FLOORABLE_OLD") != nullptr;   // (a room below only)
                if (old_rule) {
                    const uint16_t below = room_at(level - 1, x, y);
                    if (below == 0 || below >= rooms.size() || rooms[below].outside)
                        continue;
                } else if (!floorable(x, y, level))
                    continue;
            }
            tiles.emplace_back(x, y);
        }
    if (tiles.empty() && split.empty()) {
        why = outside ? "That's outside the area you can build on" : item && l > 0 ? "Upstairs floors need walls below" : "Nothing to change";
        return 0;
    }
    const int cost = item ? item->price * int(tiles.size() + split.size()) : 0;
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    for (auto [x, y] : tiles)
        floor_at(lot, x, y, l) = pattern;
    for (auto [x, y] : split) {
        WallTile &w = wall_at(lot, x, y, l);
        w.top_left_pattern = w.top_left_style = pattern;
        floor_at(lot, x, y, l) = pattern ? kFloorDiagonal : 0;
    }
    add_money(-cost);
    lot_edited();
    return std::max(cost, 1);
}

namespace {

// Whether something needs the wall on the west (vertical) or north edge of
// tile (x, y): a door, window or picture hung on it, from either side.
bool wall_in_use(Vm &vm, int x, int y, int level, bool vertical)
{
    for (auto &up : vm.entities) {
        const Entity &e = *up;
        if (e.dead || e.person || !e.in_world() || e.container || e.level != level)
            continue;
        const int needs = e.value(VarWallPlacementFlags) & 15;
        if (!needs)
            continue;
        const int sides = rotate_sides(needs, (8 - ((e.direction / 2) + 1)) % 4);   // in the tile's own terms
        const int ex = e.tile_x(), ey = e.tile_y();
        if (vertical && ((ex == x && ey == y && (sides & WallTopLeft)) || (ex == x - 1 && ey == y && (sides & WallBottomRight))))
            return true;
        if (!vertical && ((ex == x && ey == y && (sides & WallTopRight)) || (ex == x && ey == y - 1 && (sides & WallBottomLeft))))
            return true;
    }
    return false;
}

// Whether one object stands on both sides of the edge (a wall can't cut it).
bool object_across(Vm &vm, int x, int y, int level, bool vertical)
{
    const int ox = vertical ? x - 1 : x, oy = vertical ? y : y - 1;
    for (auto &up : vm.entities) {
        const Entity &e = *up;
        if (e.dead || e.person || !e.in_world() || e.container || e.level != level || e.group.empty())
            continue;
        if (e.tile_x() != x || e.tile_y() != y)
            continue;
        for (const Entity *part : e.group)
            if (part != &e && part->in_world() && part->level == level && part->tile_x() == ox && part->tile_y() == oy)
                return true;
    }
    return false;
}

} // namespace

int Vm::build_walls(int x0, int y0, int x1, int y1, int level, const BuildItem &tool, bool erase, std::string &why)
{
    const int l = level - 1;
    if (l < 0 || l >= kLotLevels) {
        why = "No such floor";
        return -1;
    }
    if (x0 != x1 && y0 != y1) {
        if (std::abs(x1 - x0) != std::abs(y1 - y0)) {
            why = "Walls go along the grid or across it at 45 degrees";
            return -1;
        }
        return build_diagonal(x0, y0, x1, y1, level, tool, erase, why);
    }
    ensure_level(lot, l);
    const bool vertical = x0 == x1;   // along y: west edges of tiles (x0, y)
    const int a = vertical ? std::min(y0, y1) : std::min(x0, x1), b = vertical ? std::max(y0, y1) : std::max(x0, x1);
    const int line = vertical ? x0 : y0;
    if (line <= 0 || line >= lot.size) {
        why = "That's the edge of the lot";
        return -1;
    }
    uint16_t pattern = 0;
    for (const WallTool &t : kWallTools)
        if (t.style == tool.pattern)
            pattern = t.pattern;
    // Each unit of the run: tile (x, y) is the one whose west or north edge it is.
    std::vector<std::pair<int, int>> edges;
    for (int i = std::max(a, 0); i < std::min(b, lot.size); i++) {
        const int x = vertical ? line : i, y = vertical ? i : line;
        const WallTile &w = wall_at(lot, x, y, l);
        const bool there = (w.segments & (vertical ? WallTopLeft : WallTopRight)) != 0;
        if (erase ? !there : there)
            continue;
        if (erase) {
            if (wall_in_use(*this, x, y, level, vertical)) {
                why = "Something is on that wall";
                return -1;
            }
        } else {
            // Both tiles either side of the wall editable (the exe's FUN_00639120): no wall on the
            // rim of the editable area or beyond. TS_EDITABLE_OLD=1: only the lot's edge, as before.
            static const bool editable_old = TS_ENV("TS_EDITABLE_OLD") != nullptr;
            if (!editable_old &&
                (!lot.user_tile(x, y, move_objects) || !lot.user_tile(vertical ? x - 1 : x, vertical ? y : y - 1, move_objects))) {
                why = "That's outside the area you can build on";
                return -1;
            }
            if (object_across(*this, x, y, level, vertical)) {
                why = "Something is in the way";
                return -1;
            }
            if (l > 0) {
                // Upstairs, as the exe's wall tool (FUN_006366b0): a tile either side of the edge floorable
                // on this storey (the build mode grid's marks, Vm::floorable); no wall below or floor needed.
                // Ours wanted a wall below or a floor beside, so the walls couldn't go where the grid showed
                // (the user, 2026-11-13). TS_UPSTAIRS_WALLS_OLD=1: as before.
                static const bool old_rule = TS_ENV("TS_UPSTAIRS_WALLS_OLD") != nullptr;
                const int nx = vertical ? x - 1 : x, ny = vertical ? y : y - 1;
                bool held;
                if (old_rule) {
                    const WallTile &below = wall_at(lot, x, y, l - 1);
                    held = (below.segments & (vertical ? WallTopLeft : WallTopRight)) || floor_at(lot, x, y, l) != 0 ||
                           floor_at(lot, nx, ny, l) != 0;
                } else
                    held = floorable(x, y, level) || floorable(nx, ny, level);
                if (!held) {
                    why = old_rule ? "Upstairs walls need a wall below or a floor beside them"
                                   : "Upstairs walls go only where the grid shows";
                    return -1;
                }
            }
        }
        edges.emplace_back(x, y);
    }
    if (edges.empty()) {
        why = erase ? "No wall there" : "Already built";
        return 0;
    }
    const int cost = erase ? 0 : tool.price * int(edges.size());
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    for (auto [x, y] : edges) {
        WallTile &w = wall_at(lot, x, y, l);
        WallTile &other = wall_at(lot, vertical ? x - 1 : x, vertical ? y : y - 1, l);
        if (vertical) {
            if (erase) {
                w.segments &= uint8_t(~WallTopLeft);
                other.segments &= uint8_t(~WallBottomRight);
            } else {
                w.segments |= WallTopLeft;
                w.top_left_style = tool.pattern;
                w.top_left_pattern = pattern;
                other.segments |= WallBottomRight;
                other.bottom_right_pattern = pattern;
            }
        } else {
            if (erase) {
                w.segments &= uint8_t(~WallTopRight);
                other.segments &= uint8_t(~WallBottomLeft);
            } else {
                w.segments |= WallTopRight;
                w.top_right_style = tool.pattern;
                w.top_right_pattern = pattern;
                other.segments |= WallBottomLeft;
                other.bottom_left_pattern = pattern;
            }
        }
    }
    add_money(-cost);
    lot_edited();
    return std::max(cost, 1);
}

int Vm::build_diagonal(int x0, int y0, int x1, int y1, int level, const BuildItem &tool, bool erase, std::string &why)
{
    // Corner c is the lot point c - 0.5: from corner (x0, y0) to (x1, y1) a tile at a time.
    // Corners (x, y) to (x + 1, y + 1) cross tile (x, y) as a vertical diagonal; (x + 1, y) to
    // (x, y + 1) as a horizontal one (as the wall painter joins them). The tile's floor goes
    // to both halves (floor 255: the halves are the wall tile's top-left pattern and style).
    const int l = level - 1;
    ensure_level(lot, l);
    const int sx = x1 > x0 ? 1 : -1, sy = y1 > y0 ? 1 : -1, n = std::abs(x1 - x0);
    const uint8_t flag = sx == sy ? WallVerticalDiag : WallHorizontalDiag;
    uint16_t pattern = 0;
    for (const WallTool &t : kWallTools)
        if (t.style == tool.pattern)
            pattern = t.pattern;
    std::vector<std::pair<int, int>> tiles;
    for (int i = 0; i < n; i++) {
        const int cx = x0 + sx * i, cy = y0 + sy * i;
        const int x = std::min(cx, cx + sx), y = std::min(cy, cy + sy);
        if (x <= 0 || y <= 0 || x >= lot.size - 1 || y >= lot.size - 1) {
            why = "That's the edge of the lot";
            return -1;
        }
        static const bool editable_old = TS_ENV("TS_EDITABLE_OLD") != nullptr;
        if (!erase && !editable_old && !lot.user_tile(x, y, move_objects)) {   // (as the straight walls)
            why = "That's outside the area you can build on";
            return -1;
        }
        // Upstairs, as the exe's (FUN_006366b0): the tile itself floorable. Ours had no rule here.
        if (!erase && l > 0 && !TS_ENV("TS_UPSTAIRS_WALLS_OLD") && !floorable(x, y, level)) {
            why = "Upstairs walls go only where the grid shows";
            return -1;
        }
        const WallTile &w = wall_at(lot, x, y, l);
        if (erase) {
            if (w.segments != flag)
                continue;
        } else {
            if (w.segments == flag)
                continue;
            if (w.segments != 0) {
                why = "Another wall is in the way";
                return -1;
            }
            for (auto &up : entities) {
                const Entity &e = *up;
                if (!e.dead && !e.person && e.in_world() && !e.container && e.level == level && e.tile_x() == x &&
                    e.tile_y() == y) {
                    why = "Something is in the way";
                    return -1;
                }
            }
        }
        tiles.emplace_back(x, y);
    }
    if (tiles.empty()) {
        why = erase ? "No wall there" : "Already built";
        return 0;
    }
    const int cost = erase ? 0 : tool.price * int(tiles.size());
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    for (auto [x, y] : tiles) {
        WallTile &w = wall_at(lot, x, y, l);
        uint16_t &floor = floor_at(lot, x, y, l);
        if (erase) {
            const uint16_t kept = w.top_left_pattern ? w.top_left_pattern : w.top_left_style;
            w = WallTile();
            if (floor == kFloorDiagonal)
                floor = kept;
        } else {
            const uint16_t under = floor;
            w = WallTile();
            w.segments = flag;
            w.top_right_style = tool.pattern;
            w.bottom_left_pattern = w.bottom_right_pattern = pattern;
            if (under != 0 && under != kFloorPool && under != kFloorWater) {
                w.top_left_pattern = w.top_left_style = under;
                floor = kFloorDiagonal;
            }
        }
    }
    add_money(-cost);
    lot_edited();
    return std::max(cost, 1);
}

int Vm::build_room(int x0, int y0, int x1, int y1, int level, const BuildItem &tool, std::string &why)
{
    if (x0 == x1 || y0 == y1) {
        why = "A room needs room inside";
        return -1;
    }
    // The four sides: what they cost together first.
    const int ax = std::min(x0, x1), bx = std::max(x0, x1), ay = std::min(y0, y1), by = std::max(y0, y1);
    const int length = 2 * (bx - ax) + 2 * (by - ay);
    if (tool.price * length > budget()) {
        why = "Not enough money";
        return -1;
    }
    int spent = 0;
    const int sides[4][4] = { { ax, ay, bx, ay }, { bx, ay, bx, by }, { ax, by, bx, by }, { ax, ay, ax, by } };
    for (const auto &s : sides) {
        std::string side_why;
        const int r = build_walls(s[0], s[1], s[2], s[3], level, tool, false, side_why);
        if (r > 1)
            spent += r;
        else if (r < 0)
            why = side_why;
    }
    return spent > 0 ? spent : (why.empty() ? (why = "Already built", 0) : -1);
}

int Vm::fill_floor(int x, int y, int level, const BuildItem *item, std::string &why)
{
    // Every tile of the room the tile is in (the game's whole-room floor), not split ones.
    const int l = level - 1;
    if (l < 0 || l >= kLotLevels || x < 0 || y < 0 || x >= lot.size || y >= lot.size) {
        why = "No room there";
        return -1;
    }
    const uint16_t room = room_at(level, x, y);
    if (room == 0 || room >= rooms.size() || rooms[room].outside) {
        why = "Fill works inside a room";
        return -1;
    }
    const uint16_t pattern = item ? style_for(*item) : 0;
    std::vector<std::pair<int, int>> tiles;
    std::vector<std::tuple<int, int, bool>> halves;   // a split tile's half in the room: its floor in the style (else the pattern)
    const RoomMap &map = room_maps_[l];
    for (int ty = 0; ty < lot.size; ty++)
        for (int tx = 0; tx < lot.size; tx++) {
            const WallTile &w = wall_at(lot, tx, ty, l);
            if (w.segments & (WallHorizontalDiag | WallVerticalDiag)) {
                // Split by a diagonal wall: only the half on the room's side. The rooms map has
                // a low half (the one reached from -x or -y across a horizontal diagonal, from -y
                // or +x across a vertical one) and a high one; the wall tile keeps their floors
                // as the painter draws them: across a vertical diagonal the low (right) half's in
                // the top-left pattern, the high in the style; across a horizontal one the other
                // way round (the pattern is the bottom half, the high one).
                if (map.map.empty())
                    continue;
                const bool horizontal = w.segments & WallHorizontalDiag;
                const uint32_t both = map.map[size_t(tx) + size_t(ty) * kLotArraySize];
                for (const bool high : { false, true }) {
                    const uint16_t local = uint16_t(high ? both >> 16 : both & 0xffff);
                    const bool in_style = high != horizontal;   // the half's floor: the style, else the pattern
                    if (local != 0 && local < map.outside.size() && uint16_t(room_base_[l] + local) == room &&
                        (in_style ? w.top_left_style : w.top_left_pattern) != pattern)
                        halves.emplace_back(tx, ty, in_style);
                }
                continue;
            }
            if (room_at(level, tx, ty) != room)
                continue;
            const uint16_t f = floor_at(lot, tx, ty, l);
            if (f == kFloorPool || f == kFloorWater || f == pattern)
                continue;
            tiles.emplace_back(tx, ty);
        }
    if (tiles.empty() && halves.empty()) {
        why = "Nothing to change";
        return 0;
    }
    const int cost = item ? item->price * int(tiles.size() + halves.size()) : 0;
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    for (auto [tx, ty] : tiles)
        floor_at(lot, tx, ty, l) = pattern;
    for (auto [tx, ty, in_style] : halves) {
        WallTile &w = wall_at(lot, tx, ty, l);
        (in_style ? w.top_left_style : w.top_left_pattern) = pattern;
        floor_at(lot, tx, ty, l) = (w.top_left_pattern || w.top_left_style) ? kFloorDiagonal : 0;
    }
    add_money(-cost);
    lot_edited();
    return std::max(cost, 1);
}

std::vector<std::array<int, 3>> Vm::fill_wallpaper_faces(int x, int y, int level, int edge) const
{
    std::vector<std::array<int, 3>> faces;
    const uint16_t room = room_at(level, x, y);
    const int l = level - 1;
    if (room == 0 || room >= rooms.size() || l < 0 || l >= kLotLevels || lot.walls[l].empty())
        return faces;
    // A wall piece's corners (corner (cx, cy) is the lot point cx - 0.5, cy - 0.5): the wall on
    // edge e of tile (tx, ty), as build_wallpaper takes it; false: none there.
    const int n = kLotArraySize + 1;
    auto seg = [&](int tx, int ty) -> uint8_t {
        return tx >= 0 && ty >= 0 && tx < kLotArraySize && ty < kLotArraySize ? lot.wall(tx, ty, l).segments : uint8_t(0);
    };
    auto piece = [&](int tx, int ty, int e, int &a) {
        switch (e & 3) {
        case 0: a = tx + ty * n; return (seg(tx, ty) & WallTopRight) != 0;
        case 3: a = tx + ty * n; return (seg(tx, ty) & WallTopLeft) != 0;
        case 1: a = tx + 1 + ty * n; return (seg(tx + 1, ty) & WallTopLeft) != 0;
        default: a = tx + (ty + 1) * n; return (seg(tx, ty + 1) & WallTopRight) != 0;
        }
    };
    // Outdoors: the walls joined to the pointed one, corner to corner (diagonals too).
    std::vector<int> group;
    int picked = -1;
    static const bool joined_off = std::getenv("TS_FILL_JOINED_OFF") != nullptr;   // every building's outside, as before
    if (rooms[room].outside && edge >= 0 && !joined_off) {
        group.resize(size_t(n * n));
        for (size_t i = 0; i < group.size(); i++)
            group[i] = int(i);
        auto find = [&](int c) {
            while (group[size_t(c)] != c)
                c = group[size_t(c)] = group[size_t(group[size_t(c)])];
            return c;
        };
        auto join = [&](int a, int b) { group[size_t(find(a))] = find(b); };
        for (int ty = 0; ty < kLotArraySize; ty++)
            for (int tx = 0; tx < kLotArraySize; tx++) {
                const uint8_t s = seg(tx, ty);
                if (s & WallTopLeft)
                    join(tx + ty * n, tx + (ty + 1) * n);
                if (s & WallTopRight)
                    join(tx + ty * n, tx + 1 + ty * n);
                if (s & WallHorizontalDiag)
                    join(tx + ty * n, tx + 1 + (ty + 1) * n);
                if (s & WallVerticalDiag)
                    join(tx + 1 + ty * n, tx + (ty + 1) * n);
            }
        int a = 0;
        if (!piece(x, y, edge, a))
            return faces;
        for (size_t i = 0; i < group.size(); i++)
            group[i] = find(int(i));
        picked = group[size_t(a)];
    }
    for (int ty = 0; ty < lot.size; ty++)
        for (int tx = 0; tx < lot.size; tx++)
            if (room_at(level, tx, ty) == room)
                for (int e = 0; e < 4; e++) {
                    int a = 0;
                    if (!piece(tx, ty, e, a))
                        continue;
                    if (picked >= 0 && group[size_t(a)] != picked)
                        continue;
                    faces.push_back({ tx, ty, e });
                }
    return faces;
}

int Vm::fill_wallpaper(int x, int y, int level, int edge, const BuildItem &item, std::string &why)
{
    // Every wall face that looks into the room the tile is in (the game's whole-room covering).
    // Outdoors too: the outside of the building pointed at (the user, 2026-11-10: whole room on an
    // outside wall; 2026-11-12: only that building). TS_FILL_OUTSIDE_OFF=1: refused there.
    const uint16_t room = room_at(level, x, y);
    static const bool outside_off = std::getenv("TS_FILL_OUTSIDE_OFF") != nullptr;
    if (room == 0 || room >= rooms.size() || (outside_off && rooms[room].outside)) {
        why = "Fill works inside a room";
        return -1;
    }
    int spent = 0;
    for (const auto &f : fill_wallpaper_faces(x, y, level, edge)) {
        std::string face_why;
        const int r = build_wallpaper(f[0], f[1], level, f[2], item, face_why);
        if (r > 1)
            spent += r;
        else if (r < 0 && face_why == "Not enough money") {
            why = face_why;
            break;
        }
    }
    if (spent == 0 && why.empty())
        why = "Nothing to change";
    return spent > 0 ? spent : 0;
}

int Vm::build_wallpaper(int x, int y, int level, int edge, const BuildItem &item, std::string &why)
{
    const int l = level - 1;
    if (l < 0 || l >= kLotLevels || lot.walls[l].empty() || x < 0 || y < 0 || x >= lot.size || y >= lot.size) {
        why = "No wall there";
        return -1;
    }
    // The wall on that edge of tile (x, y), and the field that holds its face seen from the tile.
    WallTile &w = wall_at(lot, x, y, l);
    uint16_t *field = nullptr;
    uint16_t style = 0;
    switch (edge & 3) {
    case 0:   // north: this tile's top right
        if (w.segments & WallTopRight)
            field = &w.top_right_pattern, style = w.top_right_style;
        break;
    case 3:   // west: this tile's top left
        if (w.segments & WallTopLeft)
            field = &w.top_left_pattern, style = w.top_left_style;
        break;
    case 1:   // east: the next tile's top left
        if (x + 1 < kLotArraySize && (wall_at(lot, x + 1, y, l).segments & WallTopLeft))
            field = &w.bottom_right_pattern, style = wall_at(lot, x + 1, y, l).top_left_style;
        break;
    case 2:   // south: the next tile's top right
        if (y + 1 < kLotArraySize && (wall_at(lot, x, y + 1, l).segments & WallTopRight))
            field = &w.bottom_left_pattern, style = wall_at(lot, x, y + 1, l).top_right_style;
        break;
    }
    if (!field || !solid_wall_style(style ? style : 1)) {
        why = "No wall there";
        return -1;
    }
    const uint16_t pattern = style_for(item);
    if (*field == pattern) {
        why = "Already that";
        return 0;
    }
    if (item.price > budget()) {
        why = "Not enough money";
        return -1;
    }
    *field = pattern;
    add_money(-item.price);
    lot_edited();
    return std::max(item.price, 1);
}

} // namespace ts::vm
