/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/SimsAntics/engine/VMMemory.cs).
 */
#include <algorithm>

#include "core/env.h"
#include "vm/vm.h"

namespace ts::vm {

namespace {

int16_t temp(Frame &f, int i)
{
    return i >= 0 && i < int(f.thread->temps.size()) ? f.thread->temps[size_t(i)] : 0;
}

int16_t &temp_ref(Frame &f, int i, int16_t &dummy)
{
    return i >= 0 && i < int(f.thread->temps.size()) ? f.thread->temps[size_t(i)] : dummy;
}

int16_t motive(Entity *e, int i)
{
    return e && e->person && i >= 0 && i < int(e->motives.size()) ? e->motives[size_t(i)] : 0;
}

// Motives run from -100 to 100 (the happiness ones, 0-2, are kept as they come).
int16_t clamp_motive(int which, int16_t v)
{
    return which <= 2 ? v : int16_t(std::clamp<int>(v, -100, 100));
}

int16_t person_data(Entity *e, int i)
{
    return e && i >= 0 && i < int(e->person_data.size()) ? e->person_data[size_t(i)] : 0;
}

// OBJD fields as SimAntics numbers them: 0 and 1 the version, then the fields in order.
int16_t definition(Entity *e, int i)
{
    if (!e || !e->info)
        return 0;
    const Objd &d = e->info->objd;
    if (i == 0)
        return int16_t(d.version & 0xffff);
    if (i == 1)
        return int16_t(d.version >> 16);
    return int16_t(d.get(Objd::Field(i - 2)));
}

// The neighbour whose id is in the stack object (FreeSO VMMemory, scope 24).
int16_t neighbor_info(Frame &f, int data)
{
    Vm &vm = f.vm();
    const Neighbor *n = vm.hood ? vm.hood->by_id(f.stack_id) : nullptr;
    if (!n)
        return 0;
    auto house_of = [&]() -> int16_t {
        for (const Family &fam : vm.hood->families)
            for (uint32_t g : fam.members)
                if (g == n->guid)
                    return int16_t(fam.house);
        return 0;
    };
    switch (data) {
    case 0:   // the Sim's object on this lot, if here
        for (auto &e : vm.entities)
            if (e->person && !e->dead && e->person_data[31] == n->id)
                return int16_t(e->id);
        return 0;
    case 1: {   // of the family in play (the exe's FUN_0054b9d0 case 1: the current family, on a trip the
                // travellers', sims.c 67404, has this neighbour among its members). Ours gave the house's
                // number, as 6: "can mail neighbor?" (MailboxVacation 4105) reads "1 == 0" as "not one of
                // us", so no postcard could go to anyone with a house (the 2026-11-18 audit).
                // TS_NEIGHBOR_IN_FAMILY_OLD=1: as before.
        static const bool old = std::getenv("TS_NEIGHBOR_IN_FAMILY_OLD") != nullptr;
        if (old)
            return house_of();
        const Family *mine = vm.family();
        return mine && std::find(mine->members.begin(), mine->members.end(), n->guid) != mine->members.end() ? 1 : 0;
    }
    case 6: return house_of();
    case 2: return n->person_data.size() > 58 ? n->person_data[58] : 0;   // age
    case 5:   // friends (mutual, in a family, not themselves: Neighborhood::neighbor_friends)
        return int16_t(vm.hood->neighbor_friends(*n, int(vm.global_tuning(2, "friendship threshold", 25.0f))));
    case 7:   // the family's home lot has a phone; 8 a baby (the family record's flags: the exe's case 7 and 8,
    case 8: { // sims.c 256436-256446; 0 for a family with no house). Ours: 7 always 1, 8 always 0 (the base game's
              // audit, 2026-11-20: "Propose", "Move In", "Invite Home" refuse when 8 says a baby). TS_FAMILY_FLAGS_OLD=1.
        if (TS_ENV("TS_FAMILY_FLAGS_OLD"))
            return data == 7 ? 1 : 0;
        for (const Family &fam : vm.hood->families)
            for (uint32_t g : fam.members)
                if (g == n->guid)
                    return fam.house ? int16_t(data == 7 ? (fam.flags & 1) : ((fam.flags >> 1) & 1)) : int16_t(0);
        return 0;
    }
    case 9:   // the family's friends, worked out as the game keeps them (kept for the save)
        for (Family &fam : vm.hood->families)
            for (uint32_t g : fam.members)
                if (g == n->guid)
                    return int16_t(fam.friends = vm.hood->family_friends(fam));
        return 0;
    default: return 0;
    }
}

bool exe_room_numbers()
{
    static const bool old = TS_ENV("TS_ROOM_NUMBERS_OLD") != nullptr;
    return !old;
}

// An object variable as the game's code reads it. The room is where the object is now
// (as the original's and FreeSO's GetObjectRoom): ours is otherwise kept up only every
// 30 ticks, and a new fire, asking straight away who shares its room, read 0 (outdoors),
// so the Sims beside it went on with what they did and those outside panicked.
int16_t object_value(const Vm &vm, const Entity &e, int16_t var)
{
    if (var != VarRoom)
        return e.value(var);
    const Entity *where = &e;
    while (where->container)
        where = where->container;
    if (!where->in_world())
        return e.value(var);
    const uint16_t room = vm.room_at(where->level, where->tile_x(), where->tile_y());
    // The scripts' numbers are the exe's: each storey's rooms from (storey - 1) x 1024 counting from 0, the
    // outdoors (the region at the lot's corner) 0, and an object off the lot -5 (FUN_005826b0, sims.c 298411-298521;
    // FUN_0053d1a0). Ours count from 1 (0 "no room"), so every script's "room == 0" (outdoors: a pet's "Self
    // Outside" and "Dig", "Force me outside", Makin' Magic's toadstool quest) never held, and the quest's loop
    // ran on past the exe's whole limit (the open items' audit, 2026-11-20). The engine keeps its own; the
    // scripts see the exe's, and give them back through RoomByTemp0. TS_ROOM_NUMBERS_OLD=1: ours, as before.
    if (!exe_room_numbers())
        return int16_t(room);
    return room == 0 ? int16_t(-5) : int16_t(room - 1);
}

} // namespace

int16_t object_value_now(const Vm &vm, const Entity &e, int16_t var)
{
    return object_value(vm, e, var);
}

int16_t get(Frame &f, int scope, int16_t data)
{
    Vm &vm = f.vm();
    Entity *so = f.stack_object();
    switch (scope) {
    case MyObjectAttributes: return f.caller->attribute(data);
    case StackObjectAttributes: return so ? so->attribute(data) : 0;
    case MyObject: return object_value(vm, *f.caller, data);
    case StackObject: return so ? object_value(vm, *so, data) : 0;
    case Global:
        // "Stray pets visit" off (our option): the pedestrian portal, which walks the neighbours'
        // cats and dogs onto the lot ("create neighbor": a pet only with global 20 flag 6, Unleashed
        // installed), is told Unleashed isn't there. Everything else still sees it (the skunk and
        // raccoon, the Stray-Away sign). The option used to clear flag 7, which nothing reads.
        if (data == 20 && !vm.stray_pets && f.caller->info && f.caller->info->guid == 0x81E6BEF9u)   // Portal - Pedestrian
            return int16_t(vm.global(data) & ~32);
        // The family's money, 13 the units of 10,000 and 14 the tens of thousands, as the exe keeps
        // them with the budget at every change (FUN_005572f0: money % 10000, money / 10000; nothing
        // while money is free, global 32). Ours never wrote them: the game's code read the lot file's
        // stale amount. Worked out here from the budget, which is what those writes leave.
        if ((data == 13 || data == 14) && !vm.free_money() && vm.family()) {
            const int money = vm.family()->budget;
            return int16_t(data == 13 ? money % 10000 : money / 10000);
        }
        return vm.global(data);
    case Literal: return data;
    case Temps: return temp(f, data);
    case Parameters: return data >= 0 && size_t(data) < f.args.size() ? f.args[size_t(data)] : 0;
    case StackObjectID: return f.stack_id;
    case TempByTemp: return temp(f, temp(f, data));
    case MyMotives: return motive(f.caller, data);
    case StackObjectMotives: return motive(so, data);
    case StackObjectSlot: {
        if (!so || data < 0 || size_t(data) >= so->slots.size() || !so->slots[size_t(data)])
            return 0;
        return int16_t(so->slots[size_t(data)]->id);
    }
    case StackObjectMotiveByTemp: return motive(so, temp(f, data));
    case MyPersonData: return person_data(f.caller, data);
    case StackObjectPersonData: return person_data(so, data);
    case MySlot: {
        Entity *c = f.caller;
        if (data < 0 || size_t(data) >= c->slots.size() || !c->slots[size_t(data)])
            return 0;
        return int16_t(c->slots[size_t(data)]->id);
    }
    case StackObjectDefinition: return definition(so, data);
    case StackObjectAttributeByParameter:
        return so && data >= 0 && size_t(data) < f.args.size() ? so->attribute(f.args[size_t(data)]) : 0;
    case RoomByTemp0: {   // the room numbered in temp 0
        const int r = temp(f, 0) + (exe_room_numbers() ? 1 : 0);   // (the scripts' number, the exe's: ours is one more)
        if (r <= 0 || size_t(r) >= vm.rooms.size())
            return 0;
        const Vm::Room &room = vm.rooms[size_t(r)];
        switch (data) {
        case 0: return int16_t(room.light);
        case 1: return room.outside ? 1 : 0;
        case 2: return int16_t(room.level);
        case 3: return int16_t(room.area);
        case 4: return room.pool ? 1 : 0;
        default: return room.score;
        }
    }
    case NeighborInStackObject: return neighbor_info(f, data);
    case Local: return data >= 0 && size_t(data) < f.locals.size() ? f.locals[size_t(data)] : 0;
    case Tuning: return vm.tuning(f, uint16_t(data));
    case DynSpriteFlagForTempOfStackObject: {
        const int bit = temp(f, data);
        return so && bit >= 0 && bit < 64 ? int16_t((so->dynamic_sprites >> bit) & 1) : 0;
    }
    case MyPersonDataByTemp: return person_data(f.caller, temp(f, data));
    case StackObjectPersonDataByTemp: return person_data(so, temp(f, data));
    case TreeAdRange:
    case TreeAdPersonalityVar:
    case TreeAdMin: {   // a check tree's view of its interaction's advertisements (FreeSO GetTreeAd)
        const int kind = scope == TreeAdMin ? 0 : scope == TreeAdRange ? 1 : 2;
        const auto it = f.thread->ad_changes.find(kind << 16 | uint16_t(data));
        if (it != f.thread->ad_changes.end())
            return it->second;
        const TtabInteraction *ia = f.thread->ad_source;
        if (!ia || data < 0 || size_t(data) >= ia->motives.size())
            return 0;
        const TtabMotive &m = ia->motives[size_t(data)];
        return kind == 0 ? m.min : kind == 1 ? m.max : int16_t(m.personality);
    }
    case MyTypeAttr: return vm.type_attribute(f.caller, data);
    case StackObjectTypeAttr: return vm.type_attribute(so, data);
    case 38: {
        // The object definition of the neighbour whose id is in the stack object, numbered as the stack
        // object's (two words of version, then the fields). Unnamed in FreeSims; Superstar's studio
        // controller reads word 18 of each neighbour ("Spawn Super Celebs", ControllerStudioLot.iff 4124
        // ip 2; "can visit?" 4114 ip 14), which is 1 in the seven real celebrities' definitions alone
        // (Avril, Christina, Monroe, Freddie Prinze Jr, Jon Bon Jovi, Richie Sambora, Sarah McLachlan):
        // answered 0, none of them ever came to a studio lot nor gave an autograph (the Superstar audit,
        // 2026-11-18; inferred from the scripts' use, the exe's reader not read). TS_SCOPE38_OLD=1: 0.
        static const bool old = TS_ENV("TS_SCOPE38_OLD") != nullptr;
        const Neighbor *n = !old && vm.hood ? vm.hood->by_id(f.stack_id) : nullptr;
        const ObjectInfo *info = n ? vm.content.object(n->guid) : nullptr;
        if (!info || data < 0)
            return 0;
        if (data == 0)
            return int16_t(info->objd.version & 0xffff);
        if (data == 1)
            return int16_t(info->objd.version >> 16);
        return int16_t(info->objd.get(Objd::Field(data - 2)));
    }
    case JobData:   // career in temp 0, level in temp 1
        return vm.hood ? int16_t(vm.hood->job_data(temp(f, 0), temp(f, 1), data)) : 0;
    case NeighborhoodData:
        // Index 2: the house of the family being played: this house at home, their own home while
        // they're downtown or away, 0 on a lot with none. The game's code only reads it (27 uses: every
        // "can this person visit?" refuses a neighbour of that house; the limos and carpools ask it's
        // not 0); the exe sets it. Read as 0, the household left at home could be brought in as
        // guests downtown: Linda shopping (and snoring) there while asleep at home (the user,
        // 2026-11-05). The rest (1, the tutorial's) as 0.
        if (data == 2 && !TS_ENV("TS_HOOD2_ZERO")) {
            const Family *fam = vm.family();
            static int told = 0;
            if (TS_ENV("TS_HOOD2_LOG") && told++ < 3)   // debug: what it reads, and where
                std::printf("neighbourhood 2: %d (lot %d, %s)\n", fam ? fam->house : 0, int(vm.house),
                            f.routine ? f.routine->name.c_str() : "?");
            return fam ? int16_t(fam->house) : 0;
        }
        return 0;
    case NeighborPersonData: {
        const Neighbor *n = vm.hood ? vm.hood->by_id(f.stack_id) : nullptr;
        return n && data >= 0 && size_t(data) < n->person_data.size() ? n->person_data[size_t(data)] : 0;
    }
    case StackObjectFunction:
        return so && data >= 0 && size_t(data) < so->entry_points.size() ? int16_t(so->entry_points[size_t(data)].action) : 0;
    case LocalByTemp: {
        const int i = temp(f, data);
        return i >= 0 && size_t(i) < f.locals.size() ? f.locals[size_t(i)] : 0;
    }
    case StackObjectAttributeByTemp: return so ? so->attribute(temp(f, data)) : 0;
    case TempXL: return int16_t(data >= 0 && data < 2 ? f.thread->temp_xl[size_t(data)] : 0);
    case CityTime:
    case GameTime: {
        // A slow clock for now: a game minute per 30 ticks.
        const uint32_t minutes = 9 * 60 + vm.ticks / 30;
        switch (data) {
        case 0: return int16_t((vm.ticks * 2) % 60);
        case 1: return int16_t(minutes % 60);
        case 2: return int16_t((minutes / 60) % 24);
        case 3: return int16_t((minutes / 60) % 24 >= 6 && (minutes / 60) % 24 < 18 ? 0 : 1);   // day / night
        default: return 0;
        }
    }
    case TSOStandardTime: return 0;
    case MyList:
    case StackObjectList: {
        Entity *e = scope == MyList ? f.caller : so;
        if (!e)
            return 0;
        switch (data) {
        case 0: return e->list.empty() ? 0 : e->list.front();
        case 1: return e->list.empty() ? 0 : e->list.back();
        case 2: return int16_t(e->list.size());
        default: return 0;
        }
    }
    case MoneyOverHead32Bit: return 0;
    case MyLeadTileAttribute: return f.caller->attribute(data);
    case StackObjectLeadTileAttribute: return so ? so->attribute(data) : 0;
    case StackObjectMasterDef: return definition(so, data);
    case FeatureEnableLevel: return 1;
    case MyAvatarID: return 0;
    case StackObjectTemp:   // the stack object's temps (its thread's): three reads in npc-fx.iff (the Makin' Magic audit,
               // 2026-11-18: "reading variable scope 13" on every Magic Town lot)
        return so && so->thread && data >= 0 && size_t(data) < so->thread->temps.size() ? so->thread->temps[size_t(data)] : 0;
    default: break;
    }
    vm.warn(&f, "reading variable scope " + std::to_string(scope));
    return 0;
}

void set(Frame &f, int scope, int16_t data, int16_t value)
{
    Vm &vm = f.vm();
    Entity *so = f.stack_object();
    int16_t dummy = 0;
    switch (scope) {
    case MyObjectAttributes: f.caller->set_attribute(data, value); return;
    case StackObjectAttributes: if (so) so->set_attribute(data, value); return;
    case MyObject:
    case StackObject: {
        Entity *target = scope == MyObject ? f.caller : so;
        if (!target)
            return;
        static const bool watch_hide = TS_ENV("TS_WATCH_HIDE") != nullptr;   // debug: who hides what
        if (watch_hide && data == VarHidden && target->value(VarHidden) != value)
            std::printf("tick %u: #%d %s hidden %d by #%d in %s\n", vm.ticks, int(target->id),
                        target->info ? target->info->label.c_str() : "?", int(value), int(f.caller->id),
                        f.routine ? f.routine->name.c_str() : "?");
        target->set_value(data, value);
        return;
    }
    case Global: vm.set_global(data, value); return;
    case Literal: return;
    case Temps: temp_ref(f, data, dummy) = value; return;
    case Parameters:
        if (data >= 0 && size_t(data) < f.args.size())
            f.args[size_t(data)] = value;
        return;
    case StackObjectID:
        f.stack_id = value;
        if (f.thread)
            f.thread->check_wrote_stack = true;   // (a menu's check naming entries by number: change_action_string)
        return;
    case TempByTemp: temp_ref(f, temp(f, data), dummy) = value; return;
    case MyMotives:
        if (f.caller->person && data >= 0 && data < int(f.caller->motives.size()))
            f.caller->motives[size_t(data)] = clamp_motive(data, value);
        return;
    case StackObjectMotives:
    case StackObjectMotiveByTemp: {
        const int i = scope == StackObjectMotives ? data : temp(f, data);
        if (so && so->person && i >= 0 && i < int(so->motives.size()))
            so->motives[size_t(i)] = clamp_motive(i, value);
        return;
    }
    case MyPersonData:
    case MyPersonDataByTemp: {
        const int i = scope == MyPersonData ? data : temp(f, data);
        if (i >= 0 && i < int(f.caller->person_data.size()))
            f.caller->person_data[size_t(i)] = value;
        if (i >= 46 && i <= 55 && TS_ENV("TS_INTERESTS"))   // debug: who writes a Sim's interests
            std::printf("interest %d of #%d := %d in %s\n", i, int(f.caller->id), int(value), f.routine ? f.routine->name.c_str() : "?");
        return;
    }
    case StackObjectPersonData:
    case StackObjectPersonDataByTemp: {
        const int i = scope == StackObjectPersonData ? data : temp(f, data);
        if (so && i >= 0 && i < int(so->person_data.size()))
            so->person_data[size_t(i)] = value;
        if (so && i >= 46 && i <= 55 && TS_ENV("TS_INTERESTS"))   // debug
            std::printf("interest %d of #%d := %d in %s\n", i, int(so->id), int(value), f.routine ? f.routine->name.c_str() : "?");
        return;
    }
    case StackObjectAttributeByParameter:
        if (so && data >= 0 && size_t(data) < f.args.size())
            so->set_attribute(f.args[size_t(data)], value);
        return;
    case Local:
        if (data >= 0 && size_t(data) < f.locals.size())
            f.locals[size_t(data)] = value;
        return;
    case LocalByTemp: {
        const int i = temp(f, data);
        if (i >= 0 && size_t(i) < f.locals.size())
            f.locals[size_t(i)] = value;
        return;
    }
    case StackObjectAttributeByTemp: if (so) so->set_attribute(temp(f, data), value); return;
    case MyTypeAttr: vm.set_type_attribute(f.caller, data, value); return;
    case StackObjectTypeAttr: vm.set_type_attribute(so, data, value); return;
    case TempXL:
        if (data >= 0 && data < 2)
            f.thread->temp_xl[size_t(data)] = value;
        return;
    case DynSpriteFlagForTempOfStackObject: {
        const int bit = temp(f, data);
        if (so && bit >= 0 && bit < 64)
            so->dynamic_sprites = value > 0 ? so->dynamic_sprites | (uint64_t(1) << bit)
                                            : so->dynamic_sprites & ~(uint64_t(1) << bit);
        return;
    }
    case MoneyOverHead32Bit:
    case Tuning:
    case StackObjectDefinition:
    case StackObjectFunction:
    case CityTime:
    case GameTime:
    case TSOStandardTime:
    case StackObjectMasterDef:
        return;   // read-only, or not shown yet
    case TreeAdRange:
    case TreeAdPersonalityVar:
    case TreeAdMin: {
        const int kind = scope == TreeAdMin ? 0 : scope == TreeAdRange ? 1 : 2;
        f.thread->ad_changes[kind << 16 | uint16_t(data)] = value;
        return;
    }
    case MyLeadTileAttribute: f.caller->set_attribute(data, value); return;
    case StackObjectLeadTileAttribute: if (so) so->set_attribute(data, value); return;
    case StackObjectTemp:   // the stack object's temps
        if (so && so->thread && data >= 0 && size_t(data) < so->thread->temps.size())
            so->thread->temps[size_t(data)] = value;
        return;
    default: break;
    }
    vm.warn(&f, "writing variable scope " + std::to_string(scope));
}

int32_t get_big(Frame &f, int scope, int16_t data)
{
    if (scope == TempXL)
        return data >= 0 && data < 2 ? f.thread->temp_xl[size_t(data)] : 0;
    return get(f, scope, data);
}

void set_big(Frame &f, int scope, int16_t data, int32_t value)
{
    if (scope == TempXL) {
        if (data >= 0 && data < 2)
            f.thread->temp_xl[size_t(data)] = value;
        return;
    }
    set(f, scope, data, int16_t(value));
}

} // namespace ts::vm
