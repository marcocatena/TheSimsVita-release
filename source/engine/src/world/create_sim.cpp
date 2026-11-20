/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * After FreeSO's Utils/SimitoneNeighbourGenerator.cs and Simitone's TS1CASScreen.cs.
 */
#include "world/create_sim.h"

#include <algorithm>
#include <mutex>
#include <cctype>
#include <cstdlib>

#include "files/content.h"

namespace ts {

namespace {

constexpr uint32_t kTemplatePerson = 0x7FD96B54;
const char *const kTones[3] = { "lgt", "med", "drk" };

// Outfits in `names` (sorted): "<first><3 digits><one of kinds>_<letters and digits>.cmx" (or
// .cmx.bcf), as their base names ("b001fafit_01"). Only names starting with `first` are looked
// at: matching every one of the install's thousands of files took most of a second (seconds on
// the Vita) at each change of sex or tone. (By hand, not std::regex: its code alone pushed the
// Vita's program past what vita-elf-create can take.)
std::vector<std::string> outfits(const std::vector<std::string> &names, char first, const std::vector<std::string> &kinds)
{
    auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); };
    std::vector<std::string> out;
    for (auto it = std::lower_bound(names.begin(), names.end(), std::string(1, first));
         it != names.end() && !it->empty() && (*it)[0] == first; ++it) {
        const std::string &n = *it;
        size_t end = n.size();
        if (end > 8 && n.compare(end - 8, 8, ".cmx.bcf") == 0)
            end -= 8;
        else if (end > 4 && n.compare(end - 4, 4, ".cmx") == 0)
            end -= 4;
        else
            continue;
        if (end < 4 || !std::isdigit(uint8_t(n[1])) || !std::isdigit(uint8_t(n[2])) || !std::isdigit(uint8_t(n[3])))
            continue;
        for (const std::string &kind : kinds) {
            const size_t under = 4 + kind.size();
            if (end <= under + 1 || n.compare(4, kind.size(), kind) != 0 || n[under] != '_')
                continue;
            bool ok = true;
            for (size_t i = under + 1; i < end && ok; i++)
                ok = alnum(n[i]);
            if (ok)
                out.push_back(n.substr(0, end));
            break;
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The first texture for `suit` in this tone, from `names` (sorted): suit "b009fcchd_01" -> a
// .bmp named b009fcchdmed_* (FreeSO's ExtractID).
std::string texture_for(const std::vector<std::string> &names, const std::string &suit, const std::string &tone)
{
    const std::string prefix = suit.substr(0, suit.find('_')) + tone;
    for (auto it = std::lower_bound(names.begin(), names.end(), prefix);
         it != names.end() && it->compare(0, prefix.size(), prefix) == 0; ++it)
        if (it->size() > prefix.size() + 4 && it->compare(it->size() - 4, 4, ".bmp") == 0)
            return it->substr(0, it->size() - 4);
    return {};
}

// The signs' personalities (Neat, Outgoing, Active, Playful, Nice: 25 points each), as the
// game's Create-A-Sim has them: its table of floats in the exe's data, file offset 0x64ff98.
constexpr int kSigns[12][kCasTraits] = {
    { 5, 8, 6, 3, 3 },   // Aries
    { 5, 5, 3, 8, 4 },   // Taurus
    { 4, 7, 8, 3, 3 },   // Gemini
    { 6, 3, 6, 4, 6 },   // Cancer
    { 4, 10, 4, 4, 3 },  // Leo
    { 9, 2, 6, 3, 5 },   // Virgo
    { 2, 8, 2, 6, 7 },   // Libra
    { 6, 5, 8, 3, 3 },   // Scorpio
    { 2, 3, 9, 7, 4 },   // Sagittarius
    { 7, 4, 1, 8, 5 },   // Capricorn
    { 4, 4, 4, 7, 6 },   // Aquarius
    { 5, 3, 7, 3, 7 },   // Pisces
};

} // namespace

CasParts cas_parts(const Content &content, bool child, bool female, int skin)
{
    std::vector<std::string> names = content.file_names();
    std::sort(names.begin(), names.end());
    return cas_parts(names, child, female, skin);
}

CasParts cas_parts(const std::vector<std::string> &names, bool child, bool female, int skin)
{
    CasParts out;
    const std::string tone = kTones[std::clamp(skin, 0, 2)];
    const char sex = female ? 'f' : 'm';
    // Bodies and heads of this age and sex with a texture in this tone (FreeSO picks the same way).
    const std::string sx(1, sex);
    const std::vector<std::string> body_kinds = child ? std::vector<std::string>{ sx + "cchd" }
                                                      : std::vector<std::string>{ sx + "afit", sx + "afat", sx + "askn" };
    for (const std::string &suit : outfits(names, 'b', body_kinds))
        if (std::string tex = texture_for(names, suit, tone); !tex.empty())
            out.bodies.push_back({ suit, tex });
    for (const std::string &suit : outfits(names, 'c', { sx + (child ? "c" : "a") }))
        if (std::string tex = texture_for(names, suit, tone); !tex.empty())
            out.heads.push_back({ suit, tex });
    return out;
}

CasParts cas_pool(const std::vector<std::string> &names, bool child, bool female, int skin, bool magic)
{
    CasParts out;
    const std::string tone = kTones[std::clamp(skin, 0, 2)];
    const std::string sx(1, female ? 'f' : 'm');
    std::vector<std::string> body_kinds, head_kinds;
    for (const std::string &who : { sx, std::string("u") }) {   // (the unisex suits too: "b011ucchd_01")
        if (child)
            body_kinds.push_back(who + "cchd");
        else
            for (const char *k : { "afit", "afat", "askn" })
                body_kinds.push_back(who + k);
        head_kinds.push_back(who + (child ? "c" : "a"));
    }
    auto textures = [&](std::vector<CasPart> &into, const std::string &suit) {   // each of the suit's textures in the tone
        const std::string prefix = suit.substr(0, suit.find('_')) + tone;
        for (auto it = std::lower_bound(names.begin(), names.end(), prefix); it != names.end() && it->compare(0, prefix.size(), prefix) == 0; ++it)
            if (it->size() > prefix.size() + 4 && it->compare(it->size() - 4, 4, ".bmp") == 0)
                into.push_back({ suit, it->substr(0, it->size() - 4) });
    };
    for (const std::string &suit : outfits(names, 'b', body_kinds)) {
        if (magic && (child || suit.compare(0, 3, "b20") != 0 || suit.size() < 4 || suit[3] < '2' || suit[3] > '9'))
            continue;   // (Makin' Magic's: b202-b209, not b200)
        textures(out.bodies, suit);
    }
    for (const std::string &suit : outfits(names, 'c', head_kinds))
        textures(out.heads, suit);
    return out;
}

// The nearest sign by squared distance, the first on a tie: checked against the original's Create
// A Sim (2026-10-30: one point of Neat, Active or Nice gives Cancer, of Outgoing or Playful Aquarius,
// one of each Cancer, Neat 3 + Outgoing 4 Aries); the sum of differences got the ties wrong.
int zodiac_of(const int traits[kCasTraits])
{
    int best = 0, best_d = 1 << 30;
    for (int s = 0; s < 12; s++) {
        int d = 0;
        for (int t = 0; t < kCasTraits; t++)
            d += (traits[t] - kSigns[s][t]) * (traits[t] - kSigns[s][t]);
        if (d < best_d) {
            best_d = d;
            best = s;
        }
    }
    return best;
}

std::string underwear_texture(const Content &content, bool child, bool female, int skin)
{
    const std::string prefix = "u" + std::string(1, female ? 'f' : 'm') + (child ? "chd" : "fit") + kTones[std::clamp(skin, 0, 2)];
    for (const std::string &name : content.file_names())
        if (name.compare(0, prefix.size(), prefix) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".bmp") == 0)
            return name.substr(0, name.size() - 4);
    return {};
}

std::vector<std::string> make_body_strings(const Content &content, bool child, bool female, int skin, int age,
                                           const CasPart &body, const CasPart &head)
{
    return body_strings(child, female, skin, age, body, head, underwear_texture(content, child, female, skin));
}

std::vector<std::string> body_strings(bool child, bool female, int skin, int age, const CasPart &body, const CasPart &head,
                                      const std::string &undies)
{
    const std::string tone = kTones[std::clamp(skin, 0, 2)];
    const char sex = female ? 'f' : 'm';
    std::vector<std::string> b(30);
    b[0] = child ? "child" : "adult";
    b[1] = body.suit + ",BODY=" + body.texture;
    b[2] = head.suit + ",HEAD-HEAD=" + head.texture;
    b[3] = "HEAD";
    b[4] = "Top";
    b[5] = "R_HAND";
    b[6] = "Palm";
    b[12] = female ? "female" : "male";
    b[13] = std::to_string(age);
    b[14] = tone;
    // Naked and underwear (bath, bed, pool), as the game's own Sims have them.
    const std::string fit = child ? "uchd" : std::string(1, sex) + "fit";
    b[15] = "n" + fit + "_01,BODY=n" + fit + tone + "_01";
    b[16] = undies.empty() ? b[15] : "n" + fit + "_01,BODY=" + undies;
    const char hand = child ? 'u' : sex;
    const char *const kHands[6][2] = { { "LO", "huao" }, { "RO", "huao" }, { "LP", "huap" },
                                       { "RP", "huap" }, { "LC", "huac" }, { "RC", "huac" } };
    for (int i = 0; i < 6; i++)
        b[size_t(17 + i)] = std::string("H") + char(std::toupper(hand)) + kHands[i][0] + ",HAND=" + kHands[i][1] + tone;
    b[27] = ",BODY=";
    return b;
}

std::vector<std::pair<std::string, std::string>> pet_coats(const Content &content, bool dog)
{
    std::vector<std::string> names = content.file_names();
    std::sort(names.begin(), names.end());
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string &suit : outfits(names, 'b', { dog ? "dog" : "cat" })) {
        std::string label = suit.substr(suit.find('_') + 1);   // "lab", "pinkpoodle"
        if (!label.empty())
            label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
        out.emplace_back(suit, label);
    }
    return out;
}

// (The files spell their suits "B401FAfit_...", a made Sim's are lower case.)
static std::string lower_key(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(uint8_t(c)));
    return s;
}

uint32_t townie_template(Content &content, int family, const std::vector<std::string> *body)
{
    static const bool strange_old = std::getenv("TS_STRANGE_TEMPLATE_OLD") != nullptr;
    static const bool townie_old = std::getenv("TS_TOWNIE_TEMPLATE_OLD") != nullptr;
    static const bool kinds_old = std::getenv("TS_TOWNIE_KINDS_OLD") != nullptr;
    constexpr uint32_t kHotDate = 0x4FD5DD92u, kCelebrity = 0x6BD3636Au, kStrange = 0x22320029u;
    uint32_t guid = 0;
    if (family == 6000)
        guid = strange_old ? 0u : kStrange;
    else if (townie_old)
        guid = 0;
    else if (family == 5000)
        guid = kCelebrity;
    else if (family == 5500 || (family >= 3000 && family <= 3009))
        guid = kHotDate;
    else if (family == 2000) {
        guid = kHotDate;
        // A ready-made townie: the kind's file whose bodystring has this body and head (entries 1 and 2).
        static std::mutex lock;
        static std::map<std::string, uint32_t> kinds;
        static bool listed = false;
        if (!kinds_old && body && body->size() > 2) {
            std::lock_guard<std::mutex> hold(lock);
            if (!listed) {
                listed = true;
                std::vector<std::pair<uint32_t, std::string>> files;   // (listed first: loading a file may add to the index)
                for (const auto &[g, o] : content.objects().by_guid) {
                    std::string w = o.where;
                    for (char &c : w)
                        c = char(std::tolower(uint8_t(c)));
                    const size_t at = w.find("templatenpcs/hdt_");
                    if (at != std::string::npos && w.find("generic") == std::string::npos)
                        files.emplace_back(g, w.substr(at + 13));
                }
                for (const auto &[g, name] : files) {
                    const FileRef *ref = content.file(name);
                    const LoadedIff *iff = ref ? content.load(*ref) : nullptr;
                    const IffChunk *c = iff ? iff->iff.find("STR#", 200) : nullptr;
                    StringTable t;
                    std::string err;
                    if (!c || !parse_str(iff->body(*c), c->size, t, err))
                        continue;
                    const std::string *b = t.get(1), *h = t.get(2);
                    if (b && h && !b->empty() && !h->empty())
                        kinds[lower_key(*b + "|" + *h)] = g;
                }
            }
            if (auto it = kinds.find(lower_key((*body)[1] + "|" + (*body)[2])); it != kinds.end())
                guid = it->second;
            if (std::getenv("TS_STRANGE_LOG"))   // debug
                std::printf("townie look %s | %s: %s (%lu kinds listed%s%s)\n", (*body)[1].c_str(), (*body)[2].c_str(),
                            guid == kHotDate ? "no kind's" : "a kind's", (unsigned long)kinds.size(), kinds.empty() ? "" : ", e.g. ",
                            kinds.empty() ? "" : kinds.begin()->first.c_str());
        }
    }
    return guid && content.object(guid) ? guid : 0u;
}

const ObjectInfo *register_made(Content &content, Neighborhood *hood, const MadeSim &m)
{
    if (const ObjectInfo *known = content.object(m.guid))
        return known;
    // Makin' Magic's townies (the "Strange" family, person data 61 = 6000) are copies of the game's own
    // template for them, TemplateNPCs/MMClownieGeneric.iff (22320029), in the original: all 240 of the eight
    // first-run neighbourhoods' are (private/fresh_userdata: its seven routines, "Pick a Job" and "init
    // skills" among them). Its "init tree" gives one its wand when it has none (Token Operations on "Your
    // Magic Wand", 1FEC6005), its skills and its job. Ours made them from the plain person template, so in
    // a neighbourhood made on the Vita no Magic Town townie ever held a wand, and the duelling arena, which
    // offers the adults there who have one, offered nobody ("Interaction - Duel - TEST" false: found by the
    // duel run on the Vita, 2026-11-18; the stock neighbourhood's townies, the original's, duel). Taken
    // from the game's template, those made before too. TS_STRANGE_TEMPLATE_OLD=1: the plain template.
    // (And the other townies likewise, each family from its own template: townie_template.)
    uint32_t tmpl = m.template_guid;
    const uint32_t townie = !tmpl && m.person_data.size() > 61 ? townie_template(content, m.person_data[61], &m.body) : 0u;
    if (townie)
        tmpl = townie;
    const ObjectInfo *base = content.object(tmpl ? tmpl : kTemplatePerson);
    if (!base)
        return nullptr;
    ObjectInfo info = *base;
    // (The game's template is not a person by its own type: the Sim copied from it is, as the original's
    // copies are. Left as the template's, the townie was made as a thing, not a Sim.)
    if (townie && info.objd.fields.size() > Objd::ObjectType)
        info.objd.fields[Objd::ObjectType] = 2;
    info.guid = m.guid;
    if (info.objd.fields.size() > Objd::GUID2) {
        info.objd.fields[Objd::GUID1] = uint16_t(m.guid & 0xffff);
        info.objd.fields[Objd::GUID2] = uint16_t(m.guid >> 16);
    }
    info.name = m.name;
    info.label = "made - " + m.name;
    info.body_strings = m.body;
    const ObjectInfo *made = content.add_object(std::move(info));
    if (hood && !hood->by_id(m.neighbor)) {
        Neighbor n;
        n.id = m.neighbor;
        n.guid = m.guid;
        n.name = m.name;
        n.person_data = m.person_data;
        // In order of id (a made Sim's is the next, so at the end), moving nothing already there
        // (the game's thread adds while the drawing reads the list): the room made first.
        if (hood->neighbors.size() == hood->neighbors.capacity())
            hood->neighbors.reserve(hood->neighbors.size() + 256);
        auto at = std::upper_bound(hood->neighbors.begin(), hood->neighbors.end(), n.id,
                                   [](int id, const Neighbor &o) { return id < o.id; });
        hood->neighbors.insert(at, std::move(n));
    }
    return made;
}

int16_t add_family(Content &content, Neighborhood &hood, const std::string &last_name, const std::vector<SimChoice> &sims,
                   uint32_t &random)
{
    if (!content.object(kTemplatePerson) || sims.empty())
        return 0;
    auto next = [&](uint32_t n) {
        random = random * 1103515245u + 12345u;
        return n ? (random >> 8) % n : 0u;
    };
    Family fam;
    fam.made = true;
    fam.name = last_name;
    fam.budget = 20000;   // the game's starting funds (FreeSO SimitoneNeighbourGenerator)
    fam.net_worth = 0;     // a new family owns nothing but its cash
    for (const Family &f : hood.families) {
        fam.id = int16_t(std::max<int>(fam.id, f.id + 1));
        fam.number = std::max(fam.number, f.number + 1);
    }
    fam.id = int16_t(std::max<int>(fam.id, 1));
    // The id as the exe's new family takes it (FUN_0054f510): the lowest from 1 that no family has.
    // Ours was one past the highest, which in a neighbourhood with the Strays (4000) or the townies
    // (2000, 3000-3009, 5000, 5500, 6000) is among theirs: ids the game's code asks for by number.
    // TS_FAMILY_ID_OLD=1: one past the highest, as before.
    if (!std::getenv("TS_FAMILY_ID_OLD")) {
        int id = 1;
        while (std::any_of(hood.families.begin(), hood.families.end(), [&](const Family &f) { return f.id == id; }))
            id++;
        fam.id = int16_t(id);
    }
    std::vector<int16_t> base;
    if (const Neighbor *t = hood.by_guid(kTemplatePerson))
        base = t->person_data;
    for (const SimChoice &c : sims) {
        MadeSim m;
        m.name = c.first_name;
        m.bio = c.bio;
        const int age = c.child ? 9 : 27;
        m.body = make_body_strings(content, c.child, c.female, c.skin, age, c.body, c.head);
        do   // a GUID nothing has
            m.guid = 0xF1000000u | (next(1u << 24) & 0xffffffu);
        while (content.object(m.guid) || std::count(fam.members.begin(), fam.members.end(), m.guid));
        int16_t id = 1;
        for (const Neighbor &n : hood.neighbors)
            id = std::max<int16_t>(id, int16_t(n.id + 1));
        m.neighbor = id;
        // Person data as FreeSO's generator makes it: the personality, a random chat colour
        // and interests, the age, skin, family, sex and sign.
        m.person_data = base;
        m.person_data.resize(std::max<size_t>(m.person_data.size(), 88), 0);
        for (int t = 0; t < kCasTraits; t++)
            m.person_data[size_t(kCasTraitData[t])] = int16_t(std::clamp(c.traits[t], 0, 10) * 100);
        m.person_data[13] = int16_t(500 + next(5) * 100);
        m.person_data[14] = int16_t(500 + next(5) * 100);
        m.person_data[16] = 600;
        m.person_data[26] = 600;
        // Interests: one none, three each of low, middling, high; 0-10, the scale the game's own
        // Sims have them on (the Goths: 0-10; FreeSO's generator writes 0-900).
        int need[4] = { 1, 3, 3, 3 };
        for (int i = 46; i < 56;) {
            const int v = int(next(10));
            const int band = v == 0 ? 0 : v < 4 ? 1 : v < 7 ? 2 : 3;
            if (need[band] > 0) {
                need[band]--;
                m.person_data[size_t(i++)] = int16_t(v + (v >= 7 ? int(next(2)) : 0));
            }
        }
        m.person_data[31] = id;
        m.person_data[36] = 50;   // the autonomy level, as the game's Sims have it (give_made_defaults)
        m.person_data[58] = int16_t(age);
        m.person_data[60] = skin_code(c.skin);   // (the game's numbering: 1 light, 3 medium, 2 dark)
        m.person_data[61] = fam.id;
        m.person_data[65] = int16_t(c.female ? 1 : 0);
        m.person_data[70] = int16_t(zodiac_of(c.traits));
        if (!register_made(content, &hood, m))
            return 0;
        fam.members.push_back(m.guid);
        hood.made.push_back(std::move(m));
    }
    hood.families.push_back(std::move(fam));
    return hood.families.back().id;
}

// The install's file names sorted, made once (a sort of every name is most of a listing's time).
static const std::vector<std::string> &sorted_file_names(const Content &content)
{
    static std::mutex lock;
    static std::vector<std::string> names;
    static const Content *made_for = nullptr;
    static uint32_t made_at = 0;   // (the index's generation: added content arrived or went since)
    std::lock_guard<std::mutex> hold(lock);
    if (made_for != &content || made_at != content.generation()) {
        names = content.file_names();
        std::sort(names.begin(), names.end());
        made_for = &content;
        made_at = content.generation();
    }
    return names;
}

int outfit_slot(int type)
{
    static const int slots[6] = { 1, 30, 31, 32, 33, 34 };
    return type >= 0 && type < 6 ? slots[type] : -1;
}

std::vector<std::string> purchasable_outfits(const Content &content, const std::vector<std::string> &body, int type)
{
    static const char kLetters[] = "bfslwh";
    std::vector<std::string> out;
    if (type < 0 || type > 5 || body.size() < 15)
        return out;
    auto lower = [](std::string s) {
        for (char &c : s)
            c = char(std::tolower(uint8_t(c)));
        return s;
    };
    std::string suit = lower(body[1]);
    suit = suit.substr(0, suit.find(','));
    const size_t under = suit.find('_');
    if (suit.size() < 6 || under == std::string::npos || under <= 4)
        return out;
    const std::string kind = suit.substr(4, under - 4);   // "fafit", "mcchd"
    const std::string tone = lower(body[14]);
    const std::vector<std::string> &names = sorted_file_names(content);
    for (const std::string &s : outfits(names, kLetters[type], { kind }))
        if (std::string tex = texture_for(names, s, tone); !tex.empty())
            out.push_back(s + ",BODY=" + tex);
    return out;
}

} // namespace ts
