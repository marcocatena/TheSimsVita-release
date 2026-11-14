/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/SimsAntics/primitives: VMSleep, VMExpression,
 * VMRandomNumber, VMTestObjectType, VMSetToNext,
 * VMGetDistanceTo, VMGetDirectionTo, VMLookTowards and the simple ones).
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/env.h"
#include "vm/primitives.h"

namespace ts::vm {

namespace {

Exit sleep(Frame &f, const uint8_t *op)
{
    const int i = s16(op, 0);
    if (i < 0 || size_t(i) >= f.args.size())
        return Exit::GotoTrue;
    // Ticks left, unsigned: idle(-29536) is 36000 ticks (a controller with nothing to do
    // on this lot sleeping a game day), not "done" (it looped without yielding). -1 ends it, as in FreeSO.
    const uint16_t left = uint16_t(f.args[size_t(i)]);
    if (left == 0 || left == 0xffff)
        return Exit::GotoTrue;
    // This tick's yield here; the rest sat out by the thread itself (Thread::sleeping), the
    // argument left at 0 so the next run of this instruction ends the sleep: the same number of
    // ticks as counting one down each tick, without running the thread for each.
    f.args[size_t(i)] = 0;
    f.thread->sleeping = int(left) - 1;
    f.thread->sleeping_depth = f.thread->stack.size();
    f.thread->sleeping_arg = i;
    return Exit::ContinueNextTick;
}

Exit expression(Frame &f, const uint8_t *op)
{
    const int16_t lhs_data = s16(op, 0), rhs_data = s16(op, 2);
    const int oper = op[5], lhs_owner = op[6], rhs_owner = op[7];
    auto lhs = [&] { return get_big(f, lhs_owner, lhs_data); };
    auto rhs = [&] { return get_big(f, rhs_owner, rhs_data); };
    auto store = [&](int32_t v) { set_big(f, lhs_owner, lhs_data, v); };
    auto test = [](bool b) { return b ? Exit::GotoTrue : Exit::GotoFalse; };
    switch (oper) {
    case 0: return test(lhs() > rhs());
    case 1: return test(lhs() < rhs());
    case 2: return test(lhs() == rhs());
    case 3: store(lhs() + rhs()); return Exit::GotoTrue;
    case 4: store(lhs() - rhs()); return Exit::GotoTrue;
    case 5: store(rhs()); return Exit::GotoTrue;
    case 6: store(lhs() * rhs()); return Exit::GotoTrue;
    case 7: {
        const int32_t r = rhs();
        store(r == 0 ? -1 : lhs() / r);
        return Exit::GotoTrue;
    }
    case 8: {
        const int32_t r = rhs();
        return test(r >= 1 && r <= 32 && (lhs() & (1 << (r - 1))) != 0);
    }
    case 9: {
        const int32_t r = rhs();
        if (r >= 1 && r <= 32)
            store(lhs() | (1 << (r - 1)));
        return Exit::GotoTrue;
    }
    case 10: {
        const int32_t r = rhs();
        if (r >= 1 && r <= 32)
            store(lhs() & int16_t(~(1 << (r - 1))));
        return Exit::GotoTrue;
    }
    case 11: {
        const int32_t v = lhs() + 1;
        store(v);
        return test(v < rhs());
    }
    case 12: {
        const int32_t r = rhs();
        if (r != 0)   // of the sizes, as the original (-3 % 5 is 3; FreeSO's gave 2)
            store(std::abs(int16_t(lhs())) % std::abs(r));
        return Exit::GotoTrue;
    }
    case 13: store(lhs() & rhs()); return Exit::GotoTrue;
    case 14: return test(lhs() >= rhs());
    case 15: return test(lhs() <= rhs());
    case 16: return test(lhs() != rhs());
    case 17: {
        const int32_t v = lhs() - 1;
        store(v);
        return test(v > rhs());
    }
    // 18 and 19 are |= and ^= in the Sims 1 (its FUN_00616e30); FreeSO's list push and pop
    // there are The Sims Online's, and made every |= and ^= in the game's code do nothing.
    case 18: store(lhs() | rhs()); return Exit::GotoTrue;
    case 19: store(lhs() ^ rhs()); return Exit::GotoTrue;
    case 20: store(int32_t(std::sqrt(double(std::max<int32_t>(0, rhs()))))); return Exit::GotoTrue;
    default: break;
    }
    f.vm().warn(&f, "unknown expression operator " + std::to_string(oper));
    return Exit::Error;
}

Exit random_number(Frame &f, const uint8_t *op)
{
    const uint16_t range = uint16_t(get(f, u16(op, 6), s16(op, 4)));
    Vm &vm = f.vm();
    vm.random_state = vm.random_state * 1103515245u + 12345u;
    const int16_t r = range ? int16_t((vm.random_state >> 8) % range) : 0;
    if (const char *log = TS_ENV("TS_RANDOM_LOG"); log && f.routine && f.routine->name.find(log) != std::string::npos)   // debug: a routine's draws
        std::printf("random: day %d %02d:%02d %s of #%d (stack object #%d): %d of %u\n", vm.clock.day, vm.clock.hours, vm.clock.minutes,
                    f.routine->name.c_str(), f.caller ? int(f.caller->id) : 0, int(f.stack_id), int(r), unsigned(range));
    set(f, u16(op, 2), s16(op, 0), r);
    return Exit::GotoTrue;
}

Exit test_object_type(Frame &f, const uint8_t *op)
{
    const uint32_t guid = u32(op, 0);
    Entity *e = f.vm().entity(get(f, op[6], s16(op, 4)));
    if (!e)
        return Exit::Error;
    if (e->info && e->info->guid == guid)
        return Exit::GotoTrue;
    if (e->master && e->master->guid == guid)   // a part of a multi-tile object is its master too
        return Exit::GotoTrue;
    return Exit::GotoFalse;
}

Exit set_to_next_one(Frame &f, const uint8_t *op)
{
    const uint32_t guid = u32(op, 0);
    uint8_t flags = op[4];
    int owner = op[5], data = op[7];
    if (!(flags & 0x80)) {   // old operands: always the stack object
        owner = StackObjectID;
        data = 0;
    }
    const int type = flags & 0x7f;
    Vm &vm = f.vm();
    const int16_t current = get(f, owner, int16_t(data));
    Entity *pointer = vm.entity(current);
    auto choose = [&](Entity *e) {
        set(f, owner, int16_t(data), int16_t(e->id));
        return Exit::GotoTrue;
    };
    switch (type) {
    case 5:     // the next neighbour (by id)
    case 7: {   // the next neighbour of a type (person object GUID)
        if (!vm.hood)
            return Exit::GotoFalse;
        for (const Neighbor &n : vm.hood->neighbors)
            if (n.id > current && (type == 5 || n.guid == guid)) {
                set(f, owner, int16_t(data), n.id);
                return Exit::GotoTrue;
            }
        return Exit::GotoFalse;
    }
    case 10:   // the next career
        if (vm.hood)
            for (uint16_t c : vm.hood->careers)
                if (int(c) > current && c != 0xffff) {
                    set(f, owner, int16_t(data), int16_t(c));
                    return Exit::GotoTrue;
                }
        return Exit::GotoFalse;
    case 12: {   // the next person on the lot of the current one's family
        // As the exe (FUN_0061c000 case 0xc): from the current person (an object id, never 0), the
        // next in the person list whose family number (person data 61) is theirs, round to the
        // start; back at the current one, none. The object's id, not a neighbour's: ours went up the
        // family's neighbour ids, and Unleashed's "Go To Community", back home, woke the family out
        // of the shuttle by them ("notify out of idle" on a neighbour id: an error), so Linda stayed
        // in it for good, her "Get In" never over (the user's report, 2026-11-06). TS_NEXT_FAMILY_OLD=1: as before.
        static const bool family_old = TS_ENV("TS_NEXT_FAMILY_OLD") != nullptr;
        if (!family_old) {
            if (current == 0 || !pointer || !pointer->person || pointer->person_data.size() <= 61)
                return Exit::GotoFalse;
            const int16_t number = pointer->person_data[61];
            const std::vector<int16_t> &people = vm.person_ids();
            size_t at = 0;
            while (at < people.size() && people[at] != current)
                at++;
            if (at == people.size())
                return Exit::GotoFalse;
            for (size_t k = 1; k < people.size(); k++) {
                Entity *e = vm.entity(people[(at + k) % people.size()]);
                if (e && e->person && !e->dead && e->person_data.size() > 61 && e->person_data[61] == number)
                    return choose(e);
            }
            return Exit::GotoFalse;
        }
        const Family *fam = vm.hood ? vm.hood->family_in_house(vm.house) : nullptr;
        if (!fam)
            return Exit::GotoFalse;
        int16_t best = 0;
        for (uint32_t g : fam->members)
            if (const Neighbor *n = vm.hood->by_guid(g))
                if (n->id > current && (best == 0 || n->id < best))
                    best = n->id;
        if (best == 0)
            return Exit::GotoFalse;
        set(f, owner, int16_t(data), best);
        return Exit::GotoTrue;
    }
    case 11:   // the closest house: no neighbourhood map
        return Exit::GotoFalse;
    case 3: {   // the next part of a multi-tile object, round to the first
        if (!pointer || pointer->group.empty())
            return Exit::GotoFalse;
        Entity *next = nullptr, *first = nullptr;
        for (Entity *e : pointer->group) {
            if (e->dead)
                continue;
            if (!first || e->id < first->id)
                first = e;
            if (e->id > uint16_t(current) && (!next || e->id < next->id))
                next = e;
        }
        if (!first)
            return Exit::GotoFalse;
        return choose(next ? next : first);
    }
    case 9: {   // object adjacent to the object in local
        const uint8_t local = op[6];
        // As the exe's (FUN_0061c000 case 9): the sides in the order +x, +y, -x, -y from the one
        // after the current object's; each tile's object from the storey's object grid (no
        // people, nothing held; storeys 1 and 2), taken only in the anchor's room (data 29). A
        // local index out of range, or no anchor, is an error (8, 10). Ours went N, E, S, W and
        // took anything on the tile, any room. TS_ADJACENT_OLD=1: as before.
        static const bool adjacent_old = TS_ENV("TS_ADJACENT_OLD") != nullptr;
        if (!adjacent_old) {
            if (local >= f.locals.size())
                return Exit::Error;
            Entity *anchor = vm.entity(f.locals[local]);
            if (!anchor)
                return Exit::Error;
            static const int kSide[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
            int from = -1;
            if (pointer) {
                const int dx = pointer->tile_x() - anchor->tile_x(), dy = pointer->tile_y() - anchor->tile_y();
                for (int d = 0; d < 4; d++)
                    if (kSide[d][0] == dx && kSide[d][1] == dy)
                        from = d;
                if (from < 0)
                    return Exit::GotoFalse;
            }
            if (anchor->level < 1 || anchor->level > 2)
                return Exit::GotoFalse;
            // (The rooms where the objects are now, as the game's code reads them (object_value): the
            // kept data 29 is refreshed every 30 ticks, and an object made this tick read room 0. The
            // genie's flood (global 292) makes its first "Flood - master" in front of the lamp and then
            // looks for it here to grow it 34 times over: never found, the flood stayed one tile, which
            // the user didn't see at all, 2026-11-07.)
            const int16_t room = object_value_now(vm, *anchor, VarRoom);
            for (int d = from + 1; d < 4; d++) {
                const int tx = anchor->tile_x() + kSide[d][0], ty = anchor->tile_y() + kSide[d][1];
                for (auto &e : vm.entities)
                    if (!e->dead && !e->person && !e->container && e->in_world() && e->level == anchor->level &&
                        e->tile_x() == tx && e->tile_y() == ty) {
                        if (object_value_now(vm, *e, VarRoom) == room)
                            return choose(e.get());
                        break;   // (the grid holds one object a tile)
                    }
            }
            return Exit::GotoFalse;
        }
        Entity *anchor = local < f.locals.size() ? vm.entity(f.locals[local]) : nullptr;
        if (!anchor)
            return Exit::GotoFalse;
        static const int kStep[4][2] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };
        int from = -1;
        if (pointer) {
            const int dx = pointer->tile_x() - anchor->tile_x(), dy = pointer->tile_y() - anchor->tile_y();
            from = dx == 0 ? (dy < 0 ? 0 : 2) : (dx < 0 ? 3 : 1);
            if (from == 3)
                return Exit::GotoFalse;
        }
        for (int d = from + 1; d < 4; d++)
            for (auto &e : vm.entities)
                if (!e->dead && e->in_world() && e->level == anchor->level &&
                    e->tile_x() == anchor->tile_x() + kStep[d][0] && e->tile_y() == anchor->tile_y() + kStep[d][1])
                    return choose(e.get());
        return Exit::GotoFalse;
    }
    default: break;
    }
    // A search through the objects, by id, for the next after `current`;
    // "on the same tile" wraps around to the first.
    const bool loop = type == 8;
    if (loop && !pointer)
        return Exit::GotoFalse;
    // The next one up by id: the first match found going up the ids (by_id_), from the people's
    // own list for "next person". (The whole list scanned for every step of every check tree's
    // loop was a third of a choice's time.)
    auto matches = [&](const Entity *e) {
        switch (type) {
        case 1: return e->person;
        case 2: return !e->person;
        case 4: return e->info && e->info->guid == guid;
        case 6: return e->value(VarCategory) == (f.args.empty() ? 0 : f.args[0]);   // the object's own category (object data 59), as FreeSO
        case 8: return e->in_world() && e->tile_x() == pointer->tile_x() && e->tile_y() == pointer->tile_y() &&
                       e->level == pointer->level;
        default: return true;
        }
    };
    // The exe's order (FUN_0061c000): "next person" the person list, "next of a type" that type's
    // list (both as the objects came: Entity::seq); every object, objects not people and of a
    // category the object list (new ones at its head: Entity::list_key). From the current one's
    // place, which must be in the list (else none). Ours went up the ids, which differs once ids
    // are reused or objects are made in play. TS_NEXT_BY_ID=1: by id, as before.
    static const bool by_id = TS_ENV("TS_NEXT_BY_ID") != nullptr;
    if (!by_id && (type == 1 || type == 4)) {
        const std::vector<int16_t> &people = vm.person_ids();
        if (type == 1) {
            size_t k = 0;
            if (current != 0) {
                while (k < people.size() && people[k] != current)
                    k++;
                if (k == people.size())
                    return Exit::GotoFalse;
                k++;
            }
            for (; k < people.size(); k++)
                if (Entity *e = vm.entity(people[k]); e && e->person)
                    return choose(e);
            return Exit::GotoFalse;
        }
        // Of a type: the next one made after the current one (by seq), of that GUID.
        uint64_t after = 0;
        if (current != 0) {
            const Entity *cur = vm.entity(current);
            if (!cur || !matches(cur))
                return Exit::GotoFalse;
            after = cur->seq;
        }
        // (vm.entities is in creation order: appended, and erased in place.)
        auto from = std::upper_bound(vm.entities.begin(), vm.entities.end(), after,
                                     [](uint64_t s, const std::unique_ptr<Entity> &e) { return s < e->seq; });
        for (auto it = from; it != vm.entities.end(); ++it)
            if (!(*it)->dead && matches(it->get()))
                return choose(it->get());
        return Exit::GotoFalse;
    }
    if (!by_id && (type == 0 || type == 2 || type == 6)) {
        auto it = vm.object_order.begin();
        const Entity *cur = nullptr;
        if (current != 0) {
            cur = vm.entity(current);
            if (!cur)
                return Exit::GotoFalse;
            it = vm.object_order.upper_bound(cur->list_key);
        }
        for (; it != vm.object_order.end(); ++it)
            if (Entity *e = it->second; e && !e->dead && matches(e)) {
                // Debug: the walk must only go forward; an object listed under a place that isn't its
                // own (or two live objects with one id) turns it into a loop (the downtown loops of
                // 2026-11-05: 20,000 instructions a tick in Hot Date's "update person count").
                static int backwards = 0;
                if ((e->list_key != it->first || (cur && e->list_key <= cur->list_key)) && backwards < 10) {
                    backwards++;
                    std::printf("set to next: the walk went wrong at tick %u: from #%d (key %lld) to #%d listed at %lld, its key %lld%s\n",
                                unsigned(vm.ticks), int(current), cur ? (long long)cur->list_key : 0LL, int(e->id),
                                (long long)it->first, (long long)e->list_key,
                                vm.entity(e->id) != e ? " (its id names another object)" : "");
                }
                return choose(e);
            }
        return Exit::GotoFalse;
    }
    if (type == 1) {
        for (const int16_t id : vm.person_ids())
            if (id > current)
                if (Entity *e = vm.entity(id); e && e->person)
                    return choose(e);
        return Exit::GotoFalse;
    }
    for (size_t id = size_t(std::max<int>(0, uint16_t(current))) + 1; id < vm.id_limit(); id++)
        if (Entity *e = vm.entity(int16_t(id)); e && matches(e))
            return choose(e);
    if (loop)   // round to the first
        for (size_t id = 1; id <= size_t(uint16_t(current)) && id < vm.id_limit(); id++)
            if (Entity *e = vm.entity(int16_t(id)); e && matches(e))
                return choose(e);
    return Exit::GotoFalse;
}

// Set To Next: with none left, the next person, part, object of a type or neighbour (of a
// type) sets the variable to 0 as it fails, as the original's (FUN_0061c000); ours left it.
Exit set_to_next(Frame &f, const uint8_t *op)
{
    // A scan of every object for one type, written in the game's code as "set to next object"
    // followed by "test object type" whose false branch comes back here: the two instructions
    // ran for each of the lot's objects (the pet shop's 900: the pet gym's tiles asked "Is It
    // Festival Time?" every 20 ticks, 1,800 instructions and 1.6 ms a tile on the Vita, 2026-10-31).
    // Seen together, the next object of that type is found here in one step; what the code sees
    // is the same (the objects between would have failed the test and come straight back).
    if ((op[4] & 0x7f) == 0 && f.routine && f.ip < f.routine->code.size()) {
        const BhavInstruction &me = f.routine->code[f.ip];
        if (me.true_pointer < f.routine->code.size()) {
            const BhavInstruction &test = f.routine->code[me.true_pointer];
            int owner = op[5], data = op[7];
            if (!(op[4] & 0x80)) {
                owner = StackObjectID;
                data = 0;
            }
            if (test.opcode == 32 && test.false_pointer == f.ip && test.operand[6] == owner && s16(test.operand, 4) == data) {
                const uint32_t guid = u32(test.operand, 0);
                Vm &vm = f.vm();
                const int16_t current = get(f, owner, int16_t(data));
                static const bool by_id = TS_ENV("TS_NEXT_BY_ID") != nullptr;
                if (!by_id) {   // the object list's order, as set_to_next_one
                    auto it = vm.object_order.begin();
                    if (current != 0) {
                        const Entity *cur = vm.entity(current);
                        if (!cur)
                            return Exit::GotoFalse;
                        it = vm.object_order.upper_bound(cur->list_key);
                    }
                    for (; it != vm.object_order.end(); ++it)
                        if (Entity *e = it->second;
                            e && !e->dead && ((e->info && e->info->guid == guid) || (e->master && e->master->guid == guid))) {
                            set(f, owner, int16_t(data), int16_t(e->id));
                            return Exit::GotoTrue;
                        }
                    return Exit::GotoFalse;
                }
                for (size_t id = size_t(std::max<int>(0, uint16_t(current))) + 1; id < vm.id_limit(); id++)
                    if (Entity *e = vm.entity(int16_t(id));
                        e && ((e->info && e->info->guid == guid) || (e->master && e->master->guid == guid))) {
                        set(f, owner, int16_t(data), int16_t(e->id));
                        return Exit::GotoTrue;
                    }
                return Exit::GotoFalse;
            }
        }
    }
    const Exit e = set_to_next_one(f, op);
    const int type = op[4] & 0x7f;
    if (e == Exit::GotoFalse && (type == 1 || type == 3 || type == 4 || type == 5 || type == 7)) {
        const bool old = !(op[4] & 0x80);
        set(f, old ? StackObjectID : op[5], int16_t(old ? 0 : op[7]), 0);
    }
    return e;
}

// Distance in tiles from the caller (or another object) to the stack object.
Exit get_distance_to(Frame &f, const uint8_t *op)
{
    // As the original (its primitive switch, case 0xb, and FUN_00539fb0): from me, or with
    // flag bit 0 from the object in the variable the operand names, to the stack object; the
    // straight-line distance in tiles between their exact spots, 20 tiles a storey, truncated.
    // (Ours took the larger of the two tile differences, 5 a storey, and temp 0's object.)
    Entity *a = f.stack_object();
    Entity *b = (op[2] & 1) ? f.vm().entity(get(f, op[3], s16(op, 4))) : f.caller;
    // The exe's errors: a temp past 7 (2), a missing object (0x17); and an object of OBJD type 7
    // (a controller) is at distance 0 (FUN_00539fb0). TS_SOFT_MISSING=1: ours before (10000).
    static const bool soft = TS_ENV("TS_SOFT_MISSING") != nullptr;
    if (!soft && (s16(op, 0) < 0 || s16(op, 0) > 7 || !a || !b))
        return Exit::Error;
    if (!soft && ((a->info && a->info->objd.get(Objd::ObjectType) == 7) || (b->info && b->info->objd.get(Objd::ObjectType) == 7))) {
        f.thread->temps[size_t(s16(op, 0))] = 0;
        return Exit::GotoTrue;
    }
    const size_t temp = size_t(std::clamp<int>(s16(op, 0), 0, 19));
    if (!a || !b || !a->in_world() || !b->in_world()) {
        f.thread->temps[temp] = 10000;
        return Exit::GotoTrue;
    }
    const double dx = a->x16 - b->x16, dy = a->y16 - b->y16, dl = (a->level - b->level) * 20.0;
    f.thread->temps[temp] = int16_t(std::sqrt((dx * dx + dy * dy) / 256.0 + dl * dl));
    return Exit::GotoTrue;
}

// Direction (0-7, north 0 clockwise) from me (or, flag bit 0, the object a variable names) to
// the stack object, as the original's (FUN_005f2070): a diagonal when the two distances are
// within 2:1 of each other, ties diagonal; else the nearer straight one. (Ours rounded the
// angle to the nearest eighth, which makes diagonals a narrower wedge.)
Exit get_direction_to(Frame &f, const uint8_t *op)
{
    Entity *target = f.stack_object();
    Entity *from = (op[4] & 1) ? f.vm().entity(get(f, op[5], s16(op, 6))) : f.caller;
    static const bool soft = TS_ENV("TS_SOFT_MISSING") != nullptr;
    if (!soft && (!target || !from))
        return Exit::Error;   // (the exe's 0x17 for "from", 0x15 for the stack object)
    if (!target || !from || !target->in_world() || !from->in_world())
        return Exit::GotoFalse;
    const int dx = target->x16 - from->x16, dy = target->y16 - from->y16;
    const int ax = std::abs(dx), ay = std::abs(dy);
    int dir;
    if (ax * 2 < ay)
        dir = 4;              // along y
    else
        dir = ax <= ay * 2 ? 3 : 2;   // diagonal, or along x
    if (dx < 0)
        dir = dir == 2 ? 6 : dir == 3 ? 5 : dir;
    if (dy < 0)
        dir = dir == 3 ? 1 : dir == 4 ? 0 : dir == 5 ? 7 : dir;
    set(f, u16(op, 2), s16(op, 0), int16_t(dir));
    return Exit::GotoTrue;
}

// A breakpoint: the game's code uses them as asserts that should never be
// reached, with a condition (variable != 0); one that fires is an error
// (the shipped game resets the object). Without a condition, nothing.
Exit breakpoint(Frame &f, const uint8_t *op)
{
    const int scope = u16(op, 2);
    if (scope == 0xffff)
        return Exit::GotoTrue;
    if (get(f, scope, s16(op, 0)) == 0)
        return Exit::GotoTrue;
    // The release game carries on past one (FreeSO VMBreakPoint: true); ending the interaction
    // with an error left theatre seats, recliners and the butler call broken (the sweep).
    f.vm().warn(&f, "breakpoint reached");
    return Exit::GotoTrue;
}

Exit always_true(Frame &, const uint8_t *)
{
    return Exit::GotoTrue;
}

} // namespace

// Set Balloon/Headline (FreeSO VMSetBalloonHeadline): the caller's (or the stack object's)
// headline, until its duration runs out; index -1 or duration 0 takes it away.
Exit balloon_headline(Frame &f, const uint8_t *op)
{
    const uint16_t flags2 = u16(op, 0);
    const int index = int8_t(op[2]);
    const uint8_t group = op[3];
    const int16_t duration = s16(op, 4);
    const uint8_t balloon = op[6], flags = op[7];
    Entity *obj = (flags2 & 1) ? f.stack_object() : f.caller;
    if (!obj)
        return Exit::GotoTrue;
    Entity::Headline &h = obj->headline;
    if (index == -1 || duration == 0) {
        if (TS_ENV("TS_HEADLINES") && h.on)
            std::printf("tick %u headline off #%d (%s)\n", unsigned(f.vm().ticks), int(obj->id),
                        f.routine ? f.routine->name.c_str() : "?");
        h.on = false;
        return Exit::GotoTrue;
    }
    h = {};
    h.on = true;
    h.group = group;
    h.balloon = balloon;
    h.flags = flags;
    h.index = int16_t(index);
    if (group == 7) {   // an object's own picture: the stack object's, or one in a local
        const size_t local = flags2 >> 1;
        Entity *icon = index < 2 ? f.stack_object()
                                 : (local < f.locals.size() ? f.vm().entity(f.locals[local]) : nullptr);
        h.icon = icon ? int16_t(icon->id) : 0;
    } else if (flags & 0x10) {   // indexed: plus temp 0
        h.index = int16_t(index + f.thread->temps[0]);
    }
    h.duration = (flags & 8) && duration != -1 ? duration * 15 : duration;   // in loops of 15 ticks
    static const bool log = TS_ENV("TS_HEADLINES") != nullptr;   // debug: each one as it's set
    if (log)
        std::printf("tick %u headline on #%d: group %d index %d balloon %d flags %d for %d (%s)\n", unsigned(f.vm().ticks),
                    int(obj->id), int(group), int(h.index), int(balloon), int(flags), h.duration,
                    f.routine ? f.routine->name.c_str() : "?");
    return Exit::GotoTrue;
}

void register_primitives(Vm &vm)
{
    auto &p = vm.primitives;
    p[0] = sleep;
    p[2] = expression;
    p[7] = always_true;    // refresh (graphics)
    p[8] = random_number;
    p[11] = get_distance_to;
    p[12] = get_direction_to;
    p[15] = breakpoint;
    p[31] = set_to_next;
    p[32] = test_object_type;
    p[21] = always_true;   // (the animation tester's string: a debug object)
    p[34] = always_true;   // UI effect (the tutorial's panel highlights): no tutorial here
    p[41] = balloon_headline;
    register_more_primitives(vm);
}

} // namespace ts::vm
