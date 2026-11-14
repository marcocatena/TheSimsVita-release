/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Chunk layouts from FreeSims (sims.files/formats/iff/chunks/NBRS.cs, FAMI.cs).
 */

// The neighbourhood (UserData/Neighborhood.iff): every Sim the game knows,
// with the person data and relationships that outlive a lot, the families
// and their money, and the careers (work.iff). Read-only: the engine never
// writes the player's saves.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <set>
#include <string>
#include <vector>

namespace ts {

class Content;

// A token of a Sim's inventory (Token Operations, opcode 51): a kind (0 gift, 1/3 date, 2 sim
// data, 4 purchase, 5 souvenir, 6 skill, 7 magic, 8 ingredient) and an object's GUID.
struct Token {
    uint8_t kind = 0;
    uint32_t guid = 0;
    int16_t count = 1;   // how many (Makin' Magic's ingredients, its MagiCoins): the original's entry is {kind, guid, count}
};

struct Neighbor {
    int16_t id = 0;
    uint32_t guid = 0;     // the Sim's person object
    std::string name;
    std::vector<int16_t> person_data;   // 88 values, empty when not saved
    // Relationships with other neighbours, by their id: [0] daily, [1] lifetime, ...
    std::map<int, std::vector<int16_t>> relationships;
    std::vector<Token> tokens;          // the Sim's token inventory, carried between lots (the engine's saves keep it)
    std::vector<std::string> body;      // the bodystring as changed in play (clothes bought at the racks); empty: the character's own
};

// A Sim the engine made (Create-A-Sim, a baby growing up): a copy of the game's template
// person with a GUID and a bodystring of its own. The engine's saves keep it.
struct MadeSim {
    uint32_t guid = 0;
    std::string name;
    std::string bio;
    std::vector<std::string> body;   // bodystring
    int16_t neighbor = 0;
    std::vector<int16_t> person_data;
    uint32_t template_guid = 0;   // the person object copied: 0 the template person (a Sim's), a pet's template otherwise
};

struct Family {
    int16_t id = 0;        // FAMI chunk id
    int house = 0;         // house number (House##.iff), 0 = none
    std::string name;      // the family name (FAMs, English)
    int number = 0;
    int budget = 0;
    int net_worth = 0;     // what they own besides cash; the game's Net Worth is this + budget (the Goths: 33,116 + 2,542)
    int friends = 0;
    std::vector<uint32_t> members;   // person object GUIDs
    bool made = false;     // made in Create-A-Family (kept in the engine's neighbourhood save)
    bool imported = false; // brought in from a .FAM (world/hood_import.h): a made family, known as that from Neighborhood::imports
};

// A family brought in from a family file (the install's UserData*/Patch/*.FAM: Unleashed's Kats,
// Burbs, ...; world/hood_import.h). The engine's neighbourhood save keeps this: the file's name is
// the mark that it has been brought in (never twice, even once the family is gone). Its Sims are
// made Sims (Neighborhood::made) the save leaves out: their names, bios and bodystrings are the
// file's, read from it again on loading; what changes (person data, relationships) is their
// neighbours', saved as everyone's.
struct FamilyImport {
    std::string file;      // "Kat_6.FAM"
    int16_t family = 0;    // the id it was given here
    int house = 0;         // the house it took (the .FAM is that lot's file from then on), 0: none, to the bin
    struct Member {
        int16_t old_id = 0;     // its neighbour id in the file (its uChr, CTSS 2000 + id and BMP_ chunks)
        uint32_t old_guid = 0;  // its person object there: what the file's lot has its Sims under
        uint32_t guid = 0;      // the made Sim here
        int16_t neighbor = 0;   // and its neighbour id
    };
    std::vector<Member> members;
    std::string path;      // where the file is now (worked out on loading, not saved)
};

// A townie the engine made (world/hood_import.h): what its made Sim is built from again on each
// loading (a name, a body and a head: the neighbourhood's save keeps these 100 bytes, not the
// whole bodystring it builds, a kilobyte a Sim).
struct TownieLook {
    uint32_t guid = 0;
    int16_t neighbor = 0;
    std::string name;
    std::string body_suit, body_texture, head_suit, head_texture;
    std::string undies;                        // the underwear's texture for its kind (found once, when it was made)
    uint8_t child = 0, female = 0, tone = 0;   // tone: 0 light, 1 medium, 2 dark
};

// A lot that arrived from another console (Ad hoc Share: world/share.h), until this one has saved
// it itself. The lot's save is the sender's, with the sender's Sims in it under their GUIDs and
// neighbour ids there: while the save is still that one (its hash), loading it takes each of the
// family that came with it for the Sim made for them here, and leaves the sender's other
// neighbours (visitors) out (vm/save.cpp, load_state).
struct ShareArrival {
    int house = 0;
    uint32_t save_hash = 0;   // share_hash of the save as it arrived; 0: the lot came with no save (never played): its file's Sims are swapped (Vm::load_lot)
    int16_t family = 0;       // the family that came with it, as numbered here; 0: the lot alone
    struct Member {
        uint32_t old_guid = 0;      // the Sim's person object on the sender
        int16_t old_neighbor = 0;   // and neighbour id there
        uint32_t guid = 0;          // here
        int16_t neighbor = 0;
    };
    std::vector<Member> members;
    // The sender's neighbour ids the lot's objects hold (a tombstone's ghost, vm::ghost_attribute)
    // and ours for the same Sim, 0 where it isn't here: put in their place at the lot's first load
    // here (with the exe's "import lot" trees, Vm::run_import_lot).
    std::vector<std::pair<int16_t, int16_t>> neighbors;
};

// A career (CARR in work.iff): what each level asks and pays.
struct Career {
    // Signed, as the exe and the game's code take career numbers (school is CARR 65535, which the
    // code passes as -1; kept unsigned, school was never found: no school motive changes, children's
    // needs stood still at school; the second audit, 2026-11-04).
    int16_t id = 0;
    std::string name;
    struct Level {
        int min_required[10] = {};   // friends, then skills
        int motive_delta[7] = {};
        int salary = 0, start = 0, end = 0, car = 0;
        std::string name;
        std::string male_uniform, female_uniform, uniform_skin;   // empty: their own clothes
    };
    std::vector<Level> levels;
};

// A trip between lots (FreeSO's neighbourhood GameState and the inventory the
// game keeps per Sim for it): who went, from which family, and each
// traveller's needs and departure time, which the lot they arrive on reads.
struct Travel {
    int16_t family = 0;            // the family on the move (FAMI id), 0 = none
    uint32_t sim_guid = 0;         // the Sim who set off (the one who called the cab)
    int16_t transit = 0;           // 0 that Sim alone, 1 the whole family (global 34 of the lot left; vacations)
    int from_house = 0;            // the lot left (to come home to)
    int to_house = 0;              // where they are going
    int arrive_hour = -1, arrive_minute = 0;   // the clock there (-1: not set, kept out of saves)
    // Per neighbour id, the game's "sim data" inventory (type 2): 0 bladder, 1 comfort,
    // 2 energy, 3 fun, 4 hunger, 5 hygiene, 6 social, 7/8 the hour and minute they left,
    // 9 family size, 10 the date coming along, 11 the Sim being taken home.
    std::map<int16_t, std::map<int, int16_t>> sim_data;
};

struct Neighborhood {
    std::vector<Neighbor> neighbors;   // by id, ascending
    std::vector<Family> families;
    std::vector<MadeSim> made;         // the Sims made in Create-A-Sim (their families are `made`)
    std::vector<TownieLook> townies;   // the townies made here (world/hood_import.h), kept in the engine's save
    std::vector<uint16_t> careers;     // CARR chunk ids in work.iff, ascending
    std::vector<Career> career_data;
    // What the game's code reads as job data (FreeSims CARR.GetJobData): field
    // `field` of level `level` of career `career`.
    int job_data(int career, int level, int field) const;
    // Lot zoning by house number (UserData/LotZoning.iff): 0 residential, 1 community.
    std::map<int, int> zoning;
    Travel travel;
    std::set<int> houses;                  // the house numbers with a lot file
    // A lot's objects and architecture value once it's been built on (house -> the pair); the
    // lot file's SIMI otherwise. Worked out as the game saves a lot (Vm::lot_values).
    std::map<int, std::pair<int, int>> lot_values;
    std::set<int> bulldozed;               // houses bulldozed to an empty lot (their file opens cleared)
    std::map<int, std::string> lot_names;  // downtown, vacation, ... lots: their names (the *Desc.iff files)
    std::map<int, std::string> lot_descs;  // and their descriptions
    // Each house's street, as a format with the number in it ("%s Maple Street"): StreetNames.iff in
    // the neighbourhood's folder, STR# 2001 "Lot Assignments" (entry house - 1: a street's number,
    // or none) into STR# 2000 "Street Names" (eleven a language). Houses it leaves out: the area's.
    std::map<int, std::string> lot_streets;
    // The names and descriptions the player gave lots (saving a lot with no family, as the original
    // asks), house -> (name, description): kept in the neighbourhood save, put over the files' own.
    std::map<int, std::pair<std::string, std::string>> lot_texts;
    // Attributes shared by every object of a type, by GUID (the TATT chunk): the game keeps
    // them for the whole neighbourhood (the help system's "don't ask again", ...).
    std::map<uint32_t, std::vector<int16_t>> type_attributes;
    // The families brought in from family files (world/hood_import.h), and what follows from them,
    // worked out on loading: the lot file of a house one of them took (house -> the .FAM, read in
    // place of Houses/House##.iff: the exe moves the .FAM there) and the made Sim for each person
    // object GUID the files' lots have (the exe gives each Sim a new character file and GUID too).
    std::vector<FamilyImport> imports;
    std::map<int, std::string> house_files;
    std::map<uint32_t, uint32_t> guid_alias;
    // Ad hoc Share (world/share.h), kept in the engine's neighbourhood save. The tags: a number
    // given to a family and to each of its Sims the first time they are shared, the same on every
    // console they reach, so a later share is known for the same family (its Sims replaced by
    // their new versions, not doubled). And the lots that arrived and haven't been saved here yet.
    std::map<int16_t, uint64_t> family_tags;   // family id -> tag
    std::map<uint32_t, uint64_t> share_tags;   // a Sim's GUID -> tag
    std::vector<ShareArrival> arrivals;
    // The houses whose lot arrived by a share: their lot's file is the sender's, kept beside the
    // saves (the engine never writes the game's folders) and read in place of Houses/House##.iff
    // from then on (a save takes its walls, floors and ground from the lot's file unless build
    // mode changed them: the first try brought the furniture and the Sims onto bare grass).
    std::set<int> arrived_lots;
    // NGBH's version (0x3e/0x3f in the install's copies, 0x49 as the Legacy Collection saves it):
    // under 0x45 the exe takes the neighbourhood's Patch families as not brought in yet (FUN_0054a6c0).
    int ngbh_version = 0;
    // The highest neighbour id the engine's neighbourhood save names, whether or not that Sim is
    // known now (a Sim a lot's game made, a pet from the pen, is in its lot's save alone until that
    // lot is opened): new neighbours are numbered past it.
    int16_t saved_neighbor_top = 0;

    const Family *family_by_id(int id) const;
    Family *family_by_id(int id);

    const Neighbor *by_id(int id) const;
    Neighbor *by_id(int id);
    const Neighbor *by_guid(uint32_t guid) const;
    // The lookups' index, by id and by GUID, made again whenever `neighbors` has changed (its
    // storage or length) or a hit no longer matches: the game's loops over every neighbour
    // ("Count walkby potential", every 600 ticks a pedestrian marker) read each one by id, and a
    // search of the list for each made them quadratic, 40-50 ms a tick downtown on the Vita.
    // (A copy of the neighbourhood shares it; every lookup checks it against its own list.)
    struct Index {
        std::mutex lock;
        const Neighbor *data = nullptr;
        size_t size = 0;
        std::unordered_map<int, size_t> by_id;
        std::unordered_map<uint32_t, size_t> by_guid;
    };
    std::shared_ptr<Index> index_ = std::make_shared<Index>();
    Neighbor *by_guid(uint32_t guid) { return const_cast<Neighbor *>(static_cast<const Neighborhood *>(this)->by_guid(guid)); }
    // The family's friends as the game keeps them live (its relationship code reads the count
    // before and after a change for "new family friend"): the Sims outside the family who are
    // friends with one of them both ways (each one's daily relationship at least `threshold`),
    // each counted once.
    int family_friends(const Family &family, int threshold = 50) const;
    int neighbor_friends(const Neighbor &n, int threshold = 50) const;   // Get Neighbor Data field 5
    bool in_a_family(uint32_t guid) const;   // a member of a family with a number
    const Family *family_in_house(int house) const;
    Family *family_in_house(int house);
};

// Reads Neighborhood.iff and LotZoning.iff in a neighbourhood's folder
// (UserData, UserData2, ...) and work.iff from the content.
bool load_neighborhood(Content &content, const std::string &user_data, Neighborhood &out, std::string &error);

// The neighbours of an NBRS chunk's body (Neighborhood.iff's, a family file's), in file order.
void read_neighbors(const uint8_t *data, size_t size, std::vector<Neighbor> &out);

} // namespace ts
