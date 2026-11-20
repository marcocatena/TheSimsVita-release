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
// A string table's strings in the language shown (`english`: English's own; empty where it is
// English that is shown, as nothing then differs).
std::vector<std::string> strings_of(const uint8_t *data, const IffFile &iff, uint16_t id, bool english = false)
{
    std::vector<std::string> out;
    if (english && text_language() == 1)
        return out;
    const IffChunk *c = iff.find("STR#", id);
    StringTable t;
    std::string err;
    if (!c || !parse_str(data + c->offset, c->size, t, err))
        return out;
    for (size_t i = 0;; i++) {
        const std::string *s = english ? t.get_english(i) : t.get(i);
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
            const auto en = strings_of(bytes.data(), iff, table, true);
            for (size_t i = 0; i + 2 < s.size(); i += 3) {
                BuildItem item;
                item.kind = kind;
                item.pattern = uint16_t(i / 3 + 1);
                item.price = std::atoi(s[i].c_str());
                item.name = s[i + 1];
                item.description = s[i + 2];
                if (i + 2 < en.size()) {
                    item.english_name = en[i + 1];
                    item.english_description = en[i + 2];
                }
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
        const auto en = strings_of(bytes.data(), iff, 0, true);
        BuildItem item;
        item.kind = floor ? BuildItem::Floor : BuildItem::Wallpaper;
        item.file = name;
        item.name = s[0];
        item.price = std::atoi(s[1].c_str());
        item.description = s.size() > 2 ? s[2] : std::string();
        if (en.size() >= 2) {
            item.english_name = en[0];
            item.english_description = en.size() > 2 ? en[2] : std::string();
        }
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
    // (By English words: another language shown, the English kept for it, or an object's label.)
    const bool other = text_language() != 1;
    const std::string &name = other && !item.english_name.empty() ? item.english_name : item.name;
    int g = group_matching(groups, words_of(name));
    if (g < 0 && other && item.info)
        g = group_matching(groups, words_of(item.info->label));
    if (g < 0) {
        std::string flavour = other && !item.english_name.empty() ? item.english_description : item.description;   // (not the tips every floor or covering shares)
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

// The ground under a tile: whether its four corners differ, and the height of its first.
bool ground_sloped(const Lot &lot, int x, int y)
{
    const int n = kLotArraySize + 1;
    if (lot.altitude.size() < size_t(n) * size_t(n) || x < 0 || y < 0 || x >= kLotArraySize || y >= kLotArraySize)
        return false;
    const uint8_t a = lot.altitude[size_t(x + y * n)];
    return lot.altitude[size_t(x + 1 + y * n)] != a || lot.altitude[size_t(x + (y + 1) * n)] != a ||
           lot.altitude[size_t(x + 1 + (y + 1) * n)] != a;
}

int ground_height(const Lot &lot, int x, int y)
{
    const int n = kLotArraySize + 1;
    if (lot.altitude.size() < size_t(n) * size_t(n) || x < 0 || y < 0 || x >= kLotArraySize || y >= kLotArraySize)
        return 0;
    return lot.altitude[size_t(x + y * n)];
}

// What the exe pays back for architecture taken away: 80% of its price (walls cut down, sims.c 443717;
// floors, coverings and pool tiles rounded, 303842, 533305, 447040). TS_NO_REFUNDS=1: nothing, as before.
bool refunds_on()
{
    static const bool off = TS_ENV("TS_NO_REFUNDS") != nullptr;
    return !off;
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

// The terrain tools as the exe's (the 2026-11-20 build rules' audit; sims.c 471588-472909):
//  - a corner's height is 0 to 64 ("Altitude is already as high / low as possible");
//  - a corner can't change if any of the four tiles round it has a floor or a wall on either storey, is
//    water or pool, isn't editable, or holds an object without the on-slope flag (placement 0x100: a hedge
//    lets it through, a chair doesn't);
//  - after a corner moves, each of its four neighbours more than 5 away is pulled to within 5, and so on
//    outward, a simoleon a unit; if one of those can't change the stroke fails ("Tile cannot be modified");
//  - the level tool is all or nothing, a simoleon a unit of height a corner, objects inside its rectangle
//    going with the ground.
// Ours had 0-255, no pull, a simoleon a corner whatever the height, any object holding its tile, and a
// level that did what it could. TS_TERRAIN_OLD=1: as before (shape_terrain_old).
int Vm::shape_terrain(int x0, int y0, int x1, int y1, int how, std::string &why)
{
    static const bool terrain_old = TS_ENV("TS_TERRAIN_OLD") != nullptr;
    if (terrain_old)
        return shape_terrain_old(x0, y0, x1, y1, how, why);
    constexpr int V = kLotArraySize + 1;
    constexpr int kTop = 64, kSteepest = 5;
    if (lot.altitude.size() < size_t(V) * V)
        lot.altitude.assign(size_t(V) * V, 0);
    std::vector<uint8_t> work = lot.altitude;
    auto at = [&](std::vector<uint8_t> &a, int cx, int cy) -> uint8_t & { return a[size_t(cx) + size_t(cy) * V]; };
    const int target = how == 0 && x0 >= 0 && y0 >= 0 && x0 < V && y0 < V ? at(work, x0, y0) : 0;
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    // What holds a tile: architecture on either storey, water, the lot's uneditable part; and objects
    // without the on-slope flag (the level tool lets those wholly inside its rectangle go with the ground).
    std::vector<uint8_t> held(size_t(kLotArraySize) * kLotArraySize, 0), object_held = held;
    for (int y = 0; y < lot.size; y++)
        for (int x = 0; x < lot.size; x++) {
            bool h = !lot.user_tile(x, y, move_objects) || (lot.ring_at(x, y) && !TS_ENV("TS_POOL_RING_OLD"));
            for (int l = 0; l < kLotLevels && !h; l++)
                h = (!lot.floors[l].empty() && floor_at(lot, x, y, l) != 0) || (!lot.walls[l].empty() && wall_at(lot, x, y, l).segments);
            held[size_t(x) + size_t(y) * kLotArraySize] = h ? 1 : 0;
        }
    int moved_objects = 0;
    for (auto &e : entities)
        if (!e->dead && !e->person && e->in_world() && !e->container && e->level == 1 && e->tile_x() >= 0 && e->tile_y() >= 0 &&
            e->tile_x() < kLotArraySize && e->tile_y() < kLotArraySize && !(e->value(VarPlacementFlags) & 0x100) &&
            !(uint16_t(e->value(VarFlags)) & FlagHasZeroExtent)) {
            const bool inside = how == 0 && e->tile_x() >= x0 && e->tile_x() < x1 && e->tile_y() >= y0 && e->tile_y() < y1;
            if (inside)
                moved_objects += e->group.empty() ? 0 : 1;   // (5 more for each tile of a many-tiled one: sims.c 472839)
            else
                object_held[size_t(e->tile_x()) + size_t(e->tile_y()) * kLotArraySize] = 1;
        }
    auto free_corner = [&](int cx, int cy) {
        if (cx < 1 || cy < 1 || cx > lot.size - 1 || cy > lot.size - 1)
            return false;
        for (int ty = cy - 1; ty <= cy; ty++)
            for (int tx = cx - 1; tx <= cx; tx++)
                if (held[size_t(tx) + size_t(ty) * kLotArraySize] || object_held[size_t(tx) + size_t(ty) * kLotArraySize])
                    return false;
        return true;
    };
    // A corner set, and its neighbours pulled to within 5 of it, outward; the units moved, or -1 if a
    // corner that has to follow can't.
    auto set_corner = [&](std::vector<uint8_t> &a, int cx, int cy, int to) {
        int units = std::abs(int(at(a, cx, cy)) - to);
        at(a, cx, cy) = uint8_t(to);
        std::vector<std::pair<int, int>> todo{ { cx, cy } };
        static const int kSide[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (size_t i = 0; i < todo.size(); i++) {
            const auto [px, py] = todo[i];
            const int ph = at(a, px, py);
            for (const auto &s : kSide) {
                const int nx = px + s[0], ny = py + s[1];
                if (nx < 1 || ny < 1 || nx > lot.size - 1 || ny > lot.size - 1)
                    continue;   // (the lot's rim stays as it is)
                const int d = int(at(a, nx, ny)) - ph;
                if (d <= kSteepest && d >= -kSteepest)
                    continue;
                if (!free_corner(nx, ny))
                    return -1;
                const int to_n = d > 0 ? ph + kSteepest : ph - kSteepest;
                units += std::abs(int(at(a, nx, ny)) - to_n);
                at(a, nx, ny) = uint8_t(to_n);
                todo.emplace_back(nx, ny);
            }
        }
        return units;
    };
    int cost = 0, changed = 0;
    bool blocked = false, at_limit = false, follow_failed = false;
    if (how == 0) {   // level: all of the rectangle's corners, or none
        for (int cy = std::max(y0, 0); cy <= std::min(y1, V - 1); cy++)
            for (int cx = std::max(x0, 0); cx <= std::min(x1, V - 1); cx++) {
                if (at(work, cx, cy) == target)
                    continue;
                if (!free_corner(cx, cy)) {
                    why = "Tile cannot be modified";
                    return 0;
                }
                const int units = set_corner(work, cx, cy, target);
                if (units < 0) {
                    why = "Tile cannot be modified";
                    return 0;
                }
                cost += units;
                changed++;
            }
        cost += 5 * moved_objects;
    } else {
        for (int cy = std::max(y0, 0); cy <= std::min(y1, V - 1); cy++)
            for (int cx = std::max(x0, 0); cx <= std::min(x1, V - 1); cx++) {
                const int h = at(work, cx, cy);
                const int want = std::clamp(h + (how > 0 ? 1 : -1), 0, kTop);
                if (want == h) {
                    at_limit = true;
                    continue;
                }
                if (!free_corner(cx, cy)) {
                    blocked = true;
                    continue;
                }
                std::vector<uint8_t> trial = work;
                const int units = set_corner(trial, cx, cy, want);
                if (units < 0) {
                    follow_failed = true;
                    continue;
                }
                work.swap(trial);
                cost += units;
                changed++;
            }
    }
    if (!changed) {
        why = follow_failed ? "Tile cannot be modified" : blocked ? "Something is in the way"
              : at_limit ? (how > 0 ? "Altitude is already as high as possible" : "Altitude is already as low as possible") : "Nothing to change";
        return 0;
    }
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    lot.altitude.swap(work);
    add_money(-cost);
    book(kBookArchitecture, cost);
    lot_edited();
    return std::max(cost, 1);
}

int Vm::shape_terrain_old(int x0, int y0, int x1, int y1, int how, std::string &why)
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
    book(kBookArchitecture, cost);
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
    book(kBookArchitecture, cost);
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
                // The Water Tool doesn't go on a pool or its ring (the exe, sims.c 447990: error 0x13, STR# 137's
                // "Must be placed on terrain"). TS_POOL_RING_OLD=1: as before.
                if (item.pattern == kFloorWater && !TS_ENV("TS_POOL_RING_OLD") &&
                    (floor_at(lot, x, y, l) == kFloorPool || lot.ring_at(x, y))) {
                    why = "Must be placed on terrain";
                    return 0;
                }
                // A wall on a pool tile is in the way only if it is diagonal, or stands between two pool
                // tiles (existing ones or this rectangle's): a pool flush against the house is fine (the
                // exe, sims.c 446563-446597). Ours refused any tile with a wall on any edge.
                // TS_POOL_WALL_OLD=1: as before.
                static const bool pool_wall_old = TS_ENV("TS_POOL_WALL_OLD") != nullptr;
                const uint8_t segs = wall_at(lot, x, y, l).segments;
                auto wet = [&](int tx, int ty) {
                    return (tx >= x0 && tx <= x1 && ty >= y0 && ty <= y1) || floor_at(lot, tx, ty, l) == item.pattern;
                };
                const bool wall_between = (segs & (WallHorizontalDiag | WallVerticalDiag)) ||
                                          ((segs & WallTopLeft) && wet(x - 1, y)) || ((segs & WallTopRight) && wet(x, y - 1)) ||
                                          ((segs & WallBottomRight) && wet(x + 1, y)) || ((segs & WallBottomLeft) && wet(x, y + 1));
                if (pool_wall_old ? segs != 0 : wall_between) {
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
    // A pool with Sims in it keeps every tile (the exe, sims.c 446403: "Can't delete pool with Sims in it").
    if (erase && item.pattern == kFloorPool && !TS_ENV("TS_POOL_SWIMMERS_OLD"))
        for (const auto &up : entities) {
            const Entity &e = *up;
            if (e.dead || !e.person || !e.in_world() || e.level != level)
                continue;
            const uint16_t in_room = room_at(level, e.tile_x(), e.tile_y());
            if (floor_at(lot, e.tile_x(), e.tile_y(), l) != kFloorPool)
                continue;
            for (auto [x, y] : tiles)
                if (room_at(level, x, y) == in_room) {
                    why = "Someone is swimming in that pool";
                    return 0;
                }
        }
    // Taken away: 80% of the Pool Tool's price a tile back; water tiles nothing (sims.c 447040, 448376).
    const int cost = !erase ? item.price * int(tiles.size())
                     : refunds_on() && item.pattern == kFloorPool ? -int(0.5 + double(tiles.size()) * architecture_price(3, 0) * 0.8) : 0;
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    for (auto [x, y] : tiles) {
        floor_at(lot, x, y, l) = erase ? 0 : item.pattern;
        if (!lot.pool_ring.empty() && !erase)
            lot.pool_ring[size_t(x) + size_t(y) * kLotArraySize] = 0;
    }
    if (item.pattern == kFloorPool && !TS_ENV("TS_POOL_RING_OLD"))
        make_pool_ring();
    add_money(-cost);
    book(kBookArchitecture, cost);
    lot_edited();
    return std::max(cost, 1);
}

// The exe's FUN_0063a6a0 (sims.c 446193), run after the Pool Tool: each pool tile's eight neighbours, the four
// sides first ((0,-1), (0,1), (-1,0), (1,0), then (-1,-1), (1,1), (1,-1), (-1,1): DAT_00a51138), become ring
// when they are none (not pool, not ring), editable, unfloored, flat (four corners alike), with no diagonal
// wall, and no wall between them and the pool tile; a corner one only when both side tiles beside it (towards
// the pool tile) are pool or ring. It only ever adds: an erased pool's ring stays, as the exe's.
void Vm::make_pool_ring()
{
    constexpr int N = kLotArraySize, V = kLotArraySize + 1;
    if (lot.floors[0].empty())
        return;
    if (lot.pool_ring.size() != size_t(N) * N)
        lot.pool_ring.assign(size_t(N) * N, 0);
    auto pool = [&](int x, int y) { return floor_at(lot, x, y, 0) == kFloorPool; };
    auto ring = [&](int x, int y) -> uint8_t & { return lot.pool_ring[size_t(x) + size_t(y) * N]; };
    auto flat = [&](int x, int y) {
        if (lot.altitude.size() < size_t(V) * V)
            return true;
        const uint8_t a = lot.altitude[size_t(x + y * V)];
        return lot.altitude[size_t(x + 1 + y * V)] == a && lot.altitude[size_t(x + (y + 1) * V)] == a &&
               lot.altitude[size_t(x + 1 + (y + 1) * V)] == a;
    };
    // The tile's walls as the exe's FUN_00585a30 gathers them: its own top-left (1) and top-right (2) and
    // diagonals (16, 32), the next tile's top-left as its bottom-right (4), the one below's top-right as its
    // bottom-left (8).
    auto walls = [&](int x, int y) {
        int m = wall_at(lot, x, y, 0).segments & (WallTopLeft | WallTopRight | WallHorizontalDiag | WallVerticalDiag);
        if (x + 1 < N && (wall_at(lot, x + 1, y, 0).segments & WallTopLeft))
            m |= WallBottomRight;
        if (y + 1 < N && (wall_at(lot, x, y + 1, 0).segments & WallTopRight))
            m |= WallBottomLeft;
        return m;
    };
    static const int kDir[8][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 }, { -1, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 } };
    // The wall on the neighbour's side facing the pool tile (FUN_0063e020(FUN_0063e0e0(dir))).
    static const int kFacing[4] = { WallBottomLeft, WallTopRight, WallBottomRight, WallTopLeft };
    for (int y = 0; y < lot.size; y++)
        for (int x = 0; x < lot.size; x++) {
            if (!pool(x, y))
                continue;
            bool side[4] = { false, false, false, false };
            for (int d = 0; d < 8; d++) {
                const int nx = x + kDir[d][0], ny = y + kDir[d][1];
                if (nx < 0 || ny < 0 || nx >= N || ny >= N)
                    continue;
                if (pool(nx, ny) || ring(nx, ny)) {
                    if (d < 4)
                        side[d] = true;
                    continue;
                }
                const uint16_t f = floor_at(lot, nx, ny, 0);
                if (!lot.user_tile(nx, ny, move_objects) || (f != 0 && f != kFloorWater) || !flat(nx, ny))
                    continue;
                const int m = walls(nx, ny);
                if (m & (WallHorizontalDiag | WallVerticalDiag))
                    continue;
                bool mark;
                if (d < 4)
                    mark = !(m & kFacing[d]);
                else if (d == 4)   // (-1,-1): the (0,-1) and (-1,0) sides
                    mark = side[0] && side[2];
                else if (d == 5)   // (1,1): (0,1) and (1,0)
                    mark = side[1] && side[3];
                else if (d == 6)   // (1,-1): (0,-1) and (1,0)
                    mark = side[0] && side[3];
                else               // (-1,1): (0,1) and (-1,0)
                    mark = side[1] && side[2];
                if (mark) {
                    ring(nx, ny) = 1;
                    if (d < 4)
                        side[d] = true;
                }
            }
        }
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
    int sloped_tiles = 0;   // and those on a slope, or under something that isn't for floors
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
            // On the ground the floor tool needs flat ground under the tile, and no object on it that isn't
            // for floors (a tree, a flower: placement flag 1 missing): the exe, sims.c 303801 and 305073.
            // TS_FLOOR_SLOPE_OLD=1: anywhere, as before.
            static const bool floor_slope_old = TS_ENV("TS_FLOOR_SLOPE_OLD") != nullptr;
            if (item && !floor_slope_old) {   // (the stairs' flight upstairs is such an object: no floor over it)
                if (l == 0 && ground_sloped(lot, x, y)) {
                    sloped_tiles++;
                    continue;
                }
                bool blocked = false;
                for (const auto &up : entities) {
                    const Entity &e = *up;
                    if (!e.dead && !e.person && e.in_world() && !e.container && e.level == level && e.tile_x() == x &&
                        e.tile_y() == y && !(e.value(VarPlacementFlags) & 1) && !(uint16_t(e.value(VarFlags)) & FlagHasZeroExtent))
                        blocked = true;
                }
                if (blocked) {
                    sloped_tiles++;
                    continue;
                }
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
        why = outside ? "That's outside the area you can build on" : sloped_tiles ? "Floors need flat, clear ground"
              : item && l > 0 ? "Upstairs floors need walls below" : "Nothing to change";
        return 0;
    }
    // A floor taken up pays 80% of its price back, a split tile's two halves half each (the exe, sims.c
    // 303842); a new one over an old one costs its own price in full, nothing for the old.
    int cost = item ? item->price * int(tiles.size() + split.size()) : 0;
    if (!item && refunds_on()) {
        double back = 0;
        for (auto [x, y] : tiles)
            back += architecture_price(0, floor_at(lot, x, y, l)) * 0.8;
        for (auto [x, y] : split) {
            const WallTile &w = wall_at(lot, x, y, l);
            back += architecture_price(0, w.top_left_pattern) * 0.4 + architecture_price(0, w.top_left_style) * 0.4;
        }
        cost = -int(0.5 + back);
    }
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
    book(kBookArchitecture, cost);
    lot_edited();
    return std::max(cost, 1);
}

namespace {

// What needs the wall on the west (vertical) or north edge of tile (x, y): a door, window or
// picture hung on it, from either side (each object once, by its lead part).
std::vector<Entity *> wall_users(Vm &vm, int x, int y, int level, bool vertical)
{
    std::vector<Entity *> out;
    auto add = [&](Entity *e) {
        e = e->group.empty() ? e : e->group[0];   // (one entry an object: Vm::sell takes it whole from any part)
        if (std::find(out.begin(), out.end(), e) == out.end())
            out.push_back(e);
    };
    for (auto &up : vm.entities) {
        Entity &e = *up;
        if (e.dead || e.person || !e.in_world() || e.container || e.level != level)
            continue;
        const int needs = e.value(VarWallPlacementFlags) & 15;
        if (!needs)
            continue;
        const int sides = rotate_sides(needs, (8 - ((e.direction / 2) + 1)) % 4);   // in the tile's own terms
        const int ex = e.tile_x(), ey = e.tile_y();
        if (vertical && ((ex == x && ey == y && (sides & WallTopLeft)) || (ex == x - 1 && ey == y && (sides & WallBottomRight))))
            add(&e);
        if (!vertical && ((ex == x && ey == y && (sides & WallTopRight)) || (ex == x && ey == y - 1 && (sides & WallBottomLeft))))
            add(&e);
    }
    return out;
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
    // Erasing: what hangs on those walls (doors, windows, pictures) is sold with them, as the original
    // (the user, 2026-11-14: "it should delete the wall and sell the window"); refused only if one of
    // them can't be sold. Ours refused the wall. TS_WALL_ERASE_OLD=1: refused, as before.
    static const bool erase_old = TS_ENV("TS_WALL_ERASE_OLD") != nullptr;
    std::vector<Entity *> hung;
    int run_height = -1;   // the ground's height where a new wall's run began
    for (int i = std::max(a, 0); i < std::min(b, lot.size); i++) {
        const int x = vertical ? line : i, y = vertical ? i : line;
        const WallTile &w = wall_at(lot, x, y, l);
        const bool there = (w.segments & (vertical ? WallTopLeft : WallTopRight)) != 0;
        if (erase ? !there : there)
            continue;
        if (erase) {
            for (Entity *e : wall_users(*this, x, y, level, vertical)) {
                // Only doors and windows (object type 8) go with their wall; anything else hung on it (a
                // picture, a wall lamp) keeps the wall (the exe's 0xb, sims.c 443053-443140, 443281).
                // TS_WALL_HUNG_OLD=1: all sold with it, as before.
                const bool goes_with = e->info && e->info->objd.get(Objd::ObjectType) == 8;
                if (erase_old || !movable_problem(e, true).empty() || (!goes_with && !TS_ENV("TS_WALL_HUNG_OLD"))) {
                    why = "Something is on that wall";
                    return -1;
                }
                if (std::find(hung.begin(), hung.end(), e) == hung.end())
                    hung.push_back(e);
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
            {
                const int nx = vertical ? x - 1 : x, ny = vertical ? y : y - 1;
                // No wall between two pool tiles (the exe's 0x1d "Can't place over water", sims.c 443288).
                // TS_WALL_POOL_OLD=1: as before.
                if (l == 0 && floor_at(lot, x, y, l) == kFloorPool && floor_at(lot, nx, ny, l) == kFloorPool &&
                    !TS_ENV("TS_WALL_POOL_OLD")) {
                    why = "Walls can't cross a pool";
                    return -1;
                }
                // The wall tool (not the fences: styles 2, 12, 13, 14) needs the two tiles beside a segment
                // flat and at the height where the run began (the exe's 0x11 "Can't place on slope", sims.c
                // 443157). TS_WALL_SLOPE_OLD=1: anywhere, as before.
                if (l == 0 && tool.pattern == 1 && !TS_ENV("TS_WALL_SLOPE_OLD")) {
                    if (run_height < 0)
                        run_height = ground_height(lot, x, y);
                    if (ground_sloped(lot, x, y) || ground_sloped(lot, nx, ny) || ground_height(lot, x, y) != run_height ||
                        ground_height(lot, nx, ny) != run_height) {
                        why = "Walls need flat ground";
                        return -1;
                    }
                }
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
    // A wall taken away pays 80% of its style's tool price back, cut down to whole Simoleons (the exe,
    // sims.c 443717: wall 56, picket 8, privacy 28, balustrades 36).
    int cost = erase ? 0 : tool.price * int(edges.size());
    if (erase && refunds_on())
        for (auto [x, y] : edges) {
            const WallTile &w = wall_at(lot, x, y, l);
            cost -= int(architecture_price(2, vertical ? w.top_left_style : w.top_right_style) * 0.8);
        }
    if (cost > budget()) {
        why = "Not enough money";
        return -1;
    }
    for (Entity *e : hung) {   // (sold first: their own code sees the wall they hang on)
        // (A door or window that goes with its wall brings 80% of its value: the exe, sims.c 445030.)
        const int value = sell_value(e);
        std::string not_sold;
        if (sell(e, not_sold) && refunds_on()) {
            const int kept = value - int(value * 0.8 + 0.5);
            add_money(-kept);
            book(kBookArchitecture, kept);
        }
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
    book(kBookArchitecture, cost);
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
    book(kBookArchitecture, cost);
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
    book(kBookArchitecture, cost);
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

int Vm::build_wallpaper(int x, int y, int level, int edge, const BuildItem &item, std::string &why, bool erase)
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
    if (erase) {   // the face back to bare wall, 80% of the covering's price back (the exe, sims.c 533292-533309)
        if (*field == 0) {
            why = "Nothing to change";
            return 0;
        }
        const int back = refunds_on() ? int(0.5 + architecture_price(1, *field) * 0.8) : 0;
        *field = 0;
        add_money(back);
        book(kBookArchitecture, -back);
        lot_edited();
        return 1;
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
    book(kBookArchitecture, item.price);
    lot_edited();
    return std::max(item.price, 1);
}

} // namespace ts::vm
