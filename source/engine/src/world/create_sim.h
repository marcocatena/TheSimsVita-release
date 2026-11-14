/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * After FreeSO's Utils/SimitoneNeighbourGenerator.cs and Simitone's TS1CASScreen.cs.
 */

// Create-A-Family and Create-A-Sim: the heads and bodies the install has for an age, sex
// and skin tone, a Sim made from the choices (a copy of the game's template person with
// its own GUID and bodystring), and a new family of them in the neighbourhood with the
// game's §20,000 and no house.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "world/neighborhood.h"

namespace ts {

class Content;
struct ObjectInfo;

// A head or body suit and its texture in the chosen tone ("b004mafit_01", "b004mafitlgt_01").
struct CasPart {
    std::string suit, texture;
};
struct CasParts {
    std::vector<CasPart> heads, bodies;
};
// skin: 0 light, 1 medium, 2 dark. In the install's own order (by name).
CasParts cas_parts(const Content &content, bool child, bool female, int skin);
// The same from the install's file names already sorted (listing all twelve kinds at once: the
// copy and sort of every name was nearly all of each listing's time, 460 ms on the Vita).
CasParts cas_parts(const std::vector<std::string> &sorted_names, bool child, bool female, int skin);
// Create-A-Sim's lists as the exe makes them for the Sims it makes itself (the townies:
// FUN_005e2bd0 / FUN_005e3b40 / FUN_005e5140): every suit of this age and sex or unisex ("u"),
// once for each of its textures in this tone; `magic`: the bodies of Makin' Magic's Strange
// family only (b202-b209, adults: FUN_005e2fb0). Create-A-Sim's own screens use cas_parts.
CasParts cas_pool(const std::vector<std::string> &sorted_names, bool child, bool female, int skin, bool magic = false);

// The personality as Create-A-Sim shows it, 0-10 points each (25 to share out).
enum CasTrait { kNeat, kOutgoing, kActive, kPlayful, kNice, kCasTraits };
constexpr int kCasPoints = 25;
// Each trait's person data (FreeSO VMPersonDataVariable), at 100 a point.
constexpr int kCasTraitData[kCasTraits] = { 7, 6, 3, 5, 2 };

struct SimChoice {
    std::string first_name, bio;
    bool child = false, female = false;
    int skin = 1;   // 0 light, 1 medium, 2 dark
    CasPart head, body;
    int traits[kCasTraits] = {};
};

// The astrological sign (UIText STR# 164: 0 Aries ... 11 Pisces) closest to a personality.
int zodiac_of(const int traits[kCasTraits]);

// A bodystring for a Sim of this age, sex and tone with this body and head: the outfits
// (naked, underwear), hands and the rest as the game's own Sims have them.
std::vector<std::string> make_body_strings(const Content &content, bool child, bool female, int skin, int age,
                                           const CasPart &body, const CasPart &head);
// The same with the underwear's texture given (underwear_texture finds it among every file's name:
// a search worth doing once a kind of Sim, not once a Sim: the townies' 124 took 260 ms of it on
// the PC at each loading).
std::vector<std::string> body_strings(bool child, bool female, int skin, int age, const CasPart &body, const CasPart &head,
                                      const std::string &undies);
std::string underwear_texture(const Content &content, bool child, bool female, int skin);

// The pets' coats the install has (Unleashed: "b000dog_lab" ... "b023dog_pinkpoodle", the
// cats' "b###cat_*"), sorted, as (suit, label): the pet pens' chooser lists them and a bought
// pet's bodystring takes one, by its index here.
std::vector<std::pair<std::string, std::string>> pet_coats(const Content &content, bool dog);

// The outfits a Sim could buy of a clothing type at Hot Date's racks (FreeSO
// VMTS1PurchasableOutfitHelper): 0 everyday ("b###"), 1 formal ("f"), 2 swimwear ("s"), 3 pyjamas
// ("l"), 4 winter ("w"), 5 high fashion ("h"), for the Sim's build and sex (bodystring 1:
// "b004fafit_01" -> "fafit") with a texture in the Sim's tone (bodystring 14), each as
// "suit,BODY=texture", in the install's order by name.
std::vector<std::string> purchasable_outfits(const Content &content, const std::vector<std::string> &body, int type);
// Where a bought outfit of that type goes in the bodystring: 1 everyday, 30 formal, 31
// swimwear, 32 pyjamas, 33 winter, 34 high fashion (-1 for another type).
int outfit_slot(int type);

// Puts a made Sim in the content (as a copy of the template person) and, when missing,
// in the neighbourhood as a neighbour.
const ObjectInfo *register_made(Content &content, Neighborhood *hood, const MadeSim &m);

// A Sim made in Create-A-Sim (GUID 0xF1xxxxxx): what the game's own Sims carry and ours left at 0,
// put right: person data 36, the autonomy level (50, as every Sim of the game's has it; at 0 free
// will offers nothing at all: the Bobs' John and Linda stood about doing nothing, 2026-11-02/03).
inline bool is_made_guid(uint32_t guid) { return (guid & 0xff000000u) == 0xf1000000u; }
// Person data 60, the skin tone, as the game numbers it (the exe's FUN_00549ac0 reads 1 "lgt",
// 2 "drk", 3 "med"; the shipped Sims carry it so, and the baby's "create child" passes 1-3), from
// a tone index (0 light, 1 medium, 2 dark).
inline int16_t skin_code(int tone)
{
    static const int16_t kCodes[3] = { 1, 3, 2 };
    return kCodes[tone < 0 ? 0 : tone > 2 ? 2 : tone];
}

// `body` the Sim's bodystring: its tone (entry 14) puts person data 60 right (ours wrote the
// tone's index, 0-2, before 2026-11-03: a light Sim read as none, a dark one as dark-by-luck).
inline void give_made_defaults(std::vector<int16_t> &person_data, const std::vector<std::string> *body = nullptr)
{
    if (person_data.size() > 36 && person_data[36] == 0)
        person_data[36] = 50;
    static const char *const kToneNames[3] = { "lgt", "med", "drk" };
    if (body && body->size() > 14 && person_data.size() > 60)
        for (int i = 0; i < 3; i++)
            if ((*body)[14] == kToneNames[i])
                person_data[60] = skin_code(i);
}

// A new family of these Sims, named `last_name`, in the neighbourhood: the family's id, 0 if
// it can't (no template person). `random` moves on.
int16_t add_family(Content &content, Neighborhood &hood, const std::string &last_name, const std::vector<SimChoice> &sims,
                   uint32_t &random);

} // namespace ts
