/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Making new Sims while playing, after FreeSO's Primitives/VMTS1MakeNewCharacter.cs
 * (and its SimitoneNeighbourGenerator): a baby growing into a child.
 */

// A Sim made in the game is a copy of the game's template person
// (People\TemplatePerson.iff: its code is every Sim's code) with a GUID and
// a bodystring of its own: a body and head of the right age and sex, picked
// at random among those the install has textures for in its skin tone. It
// joins the neighbourhood as a new neighbour. The engine's own saves keep it.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#include "core/env.h"
#include "vm/vm.h"
#include "world/characters.h"
#include "world/create_sim.h"

namespace ts::vm {

namespace {

constexpr uint32_t kTemplatePerson = 0x7FD96B54;
const char *const kSkins[3] = { "lgt", "med", "drk" };

const char *const kBoys[] = { "Tommy", "Billy", "Jimmy", "Danny", "Bobby", "Johnny", "Charlie", "Sammy" };
const char *const kGirls[] = { "Lucy", "Annie", "Molly", "Sally", "Emma", "Rosie", "Daisy", "Katie" };

} // namespace

const ObjectInfo *Vm::register_made(const MadeCharacter &m)
{
    return ts::register_made(content, hood, m);
}

int16_t Vm::make_character(int age, int gender, int skin_number, const std::string &wanted_name)
{
    if (!hood || !content.object(kTemplatePerson))
        return 0;
    // The game's skin number (person data 60: 1 light, 2 dark, 3 medium; the baby's "create child"
    // cycles 1-3) as a tone index. Ours took it for the index: a light baby's child came out
    // medium, a medium one's dark.
    const int skin = skin_number == 1 ? 0 : skin_number == 2 ? 2 : 1;
    const bool child = age < 18;
    const char sex = gender > 0 ? 'f' : 'm';
    const std::string tone = kSkins[std::clamp(skin, 0, 2)];
    auto pick = [&](size_t n) {
        random_state = random_state * 1103515245u + 12345u;
        return n ? size_t((random_state >> 8) % n) : 0;
    };
    // Bodies and heads of this age and sex with a texture in this tone (FreeSO picks the same
    // way), as Create-A-Sim lists them.
    const CasParts parts = cas_parts(content, child, sex == 'f', skin);
    std::vector<std::pair<std::string, std::string>> bodies, heads;
    for (const CasPart &c : parts.bodies)
        bodies.emplace_back(c.suit, c.texture);
    for (const CasPart &c : parts.heads)
        heads.emplace_back(c.suit, c.texture);
    if (bodies.empty() || heads.empty()) {
        warn(nullptr, "no " + std::string(child ? "child " : "adult ") + sex + " body or head in tone " + tone);
        return 0;
    }
    const auto &body = bodies[pick(bodies.size())];
    const auto &head = heads[pick(heads.size())];

    MadeCharacter m;
    m.name = wanted_name;
    if (m.name.empty())
        m.name = sex == 'f' ? kGirls[pick(std::size(kGirls))] : kBoys[pick(std::size(kBoys))];
    m.body = make_body_strings(content, child, sex == 'f', std::clamp(skin, 0, 2), age, { body.first, body.second },
                               { head.first, head.second });

    // A GUID nothing has, and the next neighbour id.
    do {
        random_state = random_state * 1103515245u + 12345u;
        m.guid = 0xF1000000u | (random_state >> 8 & 0xffffffu);
    } while (content.object(m.guid));
    int16_t id = 1;
    for (const Neighbor &n : hood->neighbors)
        id = std::max<int16_t>(id, int16_t(n.id + 1));
    m.neighbor = id;
    // Person data: the template's, with this one's age, sex, skin and an even personality.
    if (const Neighbor *t = hood->by_guid(kTemplatePerson))
        m.person_data = t->person_data;
    if (m.person_data.size() < 88)
        m.person_data.resize(88, 0);
    for (int trait : { 2, 3, 5, 6, 7 })
        m.person_data[size_t(trait)] = 500;
    m.person_data[58] = int16_t(age);       // PersonsAge
    m.person_data[60] = skin_code(skin);    // SkinColor, the game's number
    m.person_data[65] = int16_t(gender > 0 ? 1 : 0);   // Gender
    m.person_data[31] = id;                 // NeighborId
    if (!register_made(m))
        return 0;
    made_characters.push_back(m);
    return id;
}

// After the strays' bodystrings (TemplateFamilyUnleashed/Strays_4000.FAM): the skeleton, the coat
// as body, no head, the sex's animation set, "27", a tone, and nothing else.
int16_t Vm::make_pet(bool dog, int coat, bool female, const std::string &wanted_name)
{
    constexpr uint32_t kTemplateDog = 0x4A70DF92, kTemplateCat = 0x7BEA0977;
    const uint32_t tmpl = dog ? kTemplateDog : kTemplateCat;
    if (!hood || !content.object(tmpl)) {
        warn(nullptr, std::string("a pet can't be made: ") + (!hood ? "no neighbourhood" : "no template dog/cat object"));
        return 0;
    }
    const auto coats = pet_coats(content, dog);
    if (coats.empty()) {
        warn(nullptr, std::string("no ") + (dog ? "dog" : "cat") + " coats in the install");
        return 0;
    }
    const std::string &suit = coats[size_t(std::clamp<int>(coat, 0, int(coats.size()) - 1))].first;
    MadeCharacter m;
    m.template_guid = tmpl;
    m.name = wanted_name.empty() ? (dog ? "Rover" : "Kitty") : wanted_name;
    m.body = { dog ? "dog" : "kat", suit + ",BODY=" + suit, "NONE", "HEAD", "Top", "R_HAND", "Palm",
               std::string(dog ? "dog" : "cat") + (female ? "female" : "male"), "27", "lgt", "NONE", "NONE", "NONE", "NONE" };
    do {
        random_state = random_state * 1103515245u + 12345u;
        m.guid = 0xF1000000u | (random_state >> 8 & 0xffffffu);
    } while (content.object(m.guid));
    int16_t id = 1;
    for (const Neighbor &n : hood->neighbors)
        id = std::max<int16_t>(id, int16_t(n.id + 1));
    m.neighbor = id;
    if (const Neighbor *t = hood->by_guid(tmpl))
        m.person_data = t->person_data;
    if (m.person_data.size() < 88)
        m.person_data.resize(88, 0);
    for (int trait : { 2, 3, 5, 6, 7 })
        m.person_data[size_t(trait)] = 500;
    m.person_data[65] = int16_t(female ? 1 : 0);   // Gender
    m.person_data[31] = id;                        // NeighborId
    if (!register_made(m)) {
        warn(nullptr, "a pet can't be made: its object couldn't be registered");
        return 0;
    }
    made_characters.push_back(m);
    if (TS_ENV("TS_QUEUE_WHY"))   // debug
        std::printf("pet made: %s, neighbour %d, %s %s, coat %s\n", m.name.c_str(), int(id), female ? "female" : "male",
                    dog ? "dog" : "cat", suit.c_str());
    return id;
}

int16_t Vm::grow_up(Entity &sim)
{
    // A child of a family (a neighbour), as FUN_00549ac0 finds them; their sex (person data 65)
    // and tone (60, 1-3 in the exe's tests, 0-2 here) kept, a new adult body and head.
    if (!sim.person || !sim.info || pet_kind(sim) || sim.person_data[58] <= 0 || sim.person_data[58] >= 18 || !hood)
        return 0;
    Neighbor *n = hood->by_guid(sim.info->guid);
    if (!n)
        return 0;
    const bool female = (sim.person_data[65] & 1) != 0;
    std::vector<std::string> before = sim_body_strings(content, *sim.info);
    int skin = 1;   // the tone the Sim's bodystring wears (its entry 14)
    for (int i = 0; i < 3; i++)
        if (before.size() > 14 && before[14] == kSkins[i])
            skin = i;
    const CasParts parts = cas_parts(content, false, female, skin);
    if (parts.bodies.empty() || parts.heads.empty()) {
        warn(nullptr, "grow up: no adult body or head in tone " + std::string(kSkins[skin]));
        return 0;
    }
    auto pick = [&](size_t count) {
        random_state = random_state * 1103515245u + 12345u;
        return size_t((random_state >> 8) % count);
    };
    const CasPart &body = parts.bodies[pick(parts.bodies.size())];
    const CasPart &head = parts.heads[pick(parts.heads.size())];
    constexpr int kAdultAge = 27;   // as the exe's grown pets (FUN_00548770's 0x1b) and Create-A-Sim's adults
    std::vector<std::string> grown = make_body_strings(content, false, female, skin, kAdultAge, body, head);
    content.set_body_strings(sim.info->guid, grown);
    n->body = std::move(grown);
    sim.person_data[58] = kAdultAge;
    if (n->person_data.size() > 58)
        n->person_data[58] = kAdultAge;
    set_body_animations(sim);
    sim.animations.clear();   // the child's pose: the next animation starts on the adult skeleton
    looks_changed++;
    if (TS_ENV("TS_QUEUE_WHY"))   // debug
        std::printf("grown up: %s (neighbour %d), %s, body %s, head %s\n", n->name.c_str(), int(n->id), kSkins[skin],
                    body.suit.c_str(), head.suit.c_str());
    return int16_t(sim.id);
}

int16_t Vm::pet_to_person(Entity &pet)
{
    if (!pet.person || !pet.info || !pet_kind(pet) || !hood || !content.object(kTemplatePerson))
        return 0;
    Neighbor *n = hood->by_guid(pet.info->guid);
    if (!n)
        return 0;
    // The pet's sex: bit 0 of its person data 65 (the exe's FUN_00548770; the Kats' pets carry 4 and
    // 5), or its bodystring's "...female". A tone at random (the exe's FUN_008248c0 over 1-3).
    const std::vector<std::string> pet_body = sim_body_strings(content, *pet.info);
    bool female = (pet.person_data[65] & 1) != 0;
    if (pet_body.size() > 7 && pet_body[7].find("female") != std::string::npos)
        female = true;
    auto pick = [&](size_t count) {
        random_state = random_state * 1103515245u + 12345u;
        return size_t((random_state >> 8) % count);
    };
    const int skin = int(pick(3));
    const CasParts parts = cas_parts(content, false, female, skin);
    if (parts.bodies.empty() || parts.heads.empty()) {
        warn(nullptr, "pet to person: no adult body or head in tone " + std::string(kSkins[skin]));
        return 0;
    }
    const CasPart &body = parts.bodies[pick(parts.bodies.size())];
    const CasPart &head = parts.heads[pick(parts.heads.size())];
    constexpr int kAdultAge = 27;   // FUN_00548770's 0x1b
    MadeCharacter m;
    // The pet's own name, as shown (its character's); the neighbour record's is the file's
    // ("user00036" for the Kats' cats).
    m.name = !pet.info->name.empty() ? pet.info->name : n->name;
    m.body = make_body_strings(content, false, female, skin, kAdultAge, body, head);
    do {
        random_state = random_state * 1103515245u + 12345u;
        m.guid = 0xF1000000u | (random_state >> 8 & 0xffffffu);
    } while (content.object(m.guid));
    // A neighbour record of its own, the next number, kept with the neighbourhood's made Sims (the
    // exe writes the new character under the pet's number; a record's GUID isn't in our saves, so
    // one shared with the pet went back to the pet on loading). What the pet was carries over: its
    // person data but the age, sex and tone; its relationships, both ways; its tokens; its place
    // in the family.
    int16_t id = 1;
    for (const Neighbor &o : hood->neighbors)
        id = std::max<int16_t>(id, int16_t(o.id + 1));
    const int16_t pet_id = n->id;
    m.neighbor = id;
    m.person_data = n->person_data;
    if (m.person_data.size() < 88)
        m.person_data.resize(88, 0);
    m.person_data[58] = kAdultAge;
    m.person_data[60] = skin_code(skin);
    m.person_data[65] = female ? 1 : 0;
    m.person_data[31] = id;
    const uint32_t pet_guid = n->guid;
    const auto pet_relationships = n->relationships;
    const auto pet_tokens = n->tokens;
    const ObjectInfo *info = register_made(m);   // (adds the neighbour: `n` may move)
    Neighbor *made = hood->by_id(id);
    if (!info || !made)
        return 0;
    hood->made.push_back(m);
    made->relationships = pet_relationships;
    made->tokens = pet_tokens;
    for (Neighbor &o : hood->neighbors) {
        auto it = o.relationships.find(pet_id);
        if (it != o.relationships.end() && o.id != id)
            o.relationships[id] = it->second;
    }
    if (Neighbor *was = hood->by_id(pet_id); was && was->person_data.size() > 61)
        was->person_data[61] = 0;   // the pet's record: no family's any more
    for (Family &fam : hood->families)   // in the pet's place in its family
        std::replace(fam.members.begin(), fam.members.end(), pet_guid, m.guid);
    // Where the pet stood, facing its way; the pet gone (the exe removes every object of its kind).
    Entity *e = create(*info, pet.tile_x(), pet.tile_y(), pet.level - 1, pet.direction);
    if (!e)
        return 0;
    e->x16 = pet.x16;
    e->y16 = pet.y16;
    e->tokens = pet_tokens;
    e->relationships = pet.relationships;   // (on the lot: by object, the others' to it too)
    for (auto &o : entities)
        if (o.get() != e && !o->dead) {
            auto it = o->relationships.find(pet.id);
            if (it != o->relationships.end())
                o->relationships[e->id] = it->second;
        }
    e->run_entry(*this, 0, true);   // init
    refresh_people();
    e->run_entry(*this, 1, false);  // main, on its thread
    if (TS_ENV("TS_QUEUE_WHY"))   // debug
        std::printf("pet made a person: %s (neighbour %d, was %d) #%d -> #%d, %s %s\n", m.name.c_str(), int(id), int(pet_id),
                    int(pet.id), int(e->id), female ? "female" : "male", kSkins[skin]);
    return int16_t(e->id);
}

std::vector<std::string> Vm::suits_of(const Entity &sim, int type)
{
    if (!sim.person || !sim.info)
        return {};
    return purchasable_outfits(content, sim_body_strings(content, *sim.info), type);
}

int16_t Vm::suit_index(const Entity &sim, int type)
{
    if (!sim.person || !sim.info)
        return -1;
    const std::vector<std::string> body = sim_body_strings(content, *sim.info);
    const int slot = outfit_slot(type);
    if (slot < 0 || size_t(slot) >= body.size())
        return -1;
    auto lower = [](std::string s) {
        for (char &c : s)
            c = char(std::tolower(uint8_t(c)));
        return s;
    };
    const std::string worn = lower(body[size_t(slot)].substr(0, body[size_t(slot)].find(';')));
    const std::vector<std::string> suits = purchasable_outfits(content, body, type);
    for (size_t i = 0; i < suits.size(); i++)
        if (lower(suits[i]) == worn)
            return int16_t(i);
    return -1;
}

int16_t Vm::set_suit(Entity &sim, int type, int index)
{
    if (!sim.person || !sim.info)
        return -1;
    std::vector<std::string> body = sim_body_strings(content, *sim.info);
    const std::vector<std::string> suits = purchasable_outfits(content, body, type);
    const int slot = outfit_slot(type);
    if (slot < 0 || index < 0 || size_t(index) >= suits.size() || body.size() < 15)
        return -1;
    const int16_t before = suit_index(sim, type);
    if (body.size() < 35)
        body.resize(35);
    body[size_t(slot)] = suits[size_t(index)];
    content.set_body_strings(sim.info->guid, body);
    if (hood)
        if (Neighbor *n = hood->by_guid(sim.info->guid))
            n->body = body;
    looks_changed++;
    if (TS_ENV("TS_SUITS"))   // debug
        std::printf("suit set: %s type %d outfit %d (%s), was %d\n", sim.info->label.c_str(), type, index,
                    suits[size_t(index)].c_str(), int(before));
    return before;
}

} // namespace ts::vm
