/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/SimsAntics: engine/VMThread.cs queue handling,
 * engine/VMQueuedAction.cs, VMEntity.GetAction/TreeByName,
 * engine/VMTS1MotiveDecay.cs, model/VMMotiveChange.cs).
 */
#include <algorithm>
#include <climits>
#include <cstring>

#include <cstdio>
#include <cstdlib>

#include "core/env.h"
#include "vm/vm.h"
#include "vm/footprint.h"
#include "vm/primitives.h"

namespace ts::vm {

// ---------------------------------------------------------------- the queue

namespace {

// How many queue items are running (protected from reordering), minus one:
// one per interaction started on the stack.
int active_index(const Thread &t)
{
    int running = 0;
    for (size_t i = 0; i < t.stack.size(); i++) {
        const Frame &f = t.stack[i];
        if (f.action_tree && (i == 0 || !t.stack[i - 1].action_tree || f.discard_result))
            running++;
    }
    return running - 1;
}

} // namespace

int Thread::waiting_at_least(int16_t priority) const
{
    int n = 0;
    for (size_t i = size_t(active_index(*this) + 1); i < queue.size(); i++)
        if (queue[i].mode == QueueMode::Normal && !(queue[i].flags & TtabRunImmediately) && queue[i].priority >= priority)
            n++;
    return n;
}

namespace {

Frame frame_for(Vm &vm, Entity &caller, const QueuedAction &a, const Routine *routine)
{
    Frame f;
    f.routine = routine;
    f.caller = &caller;
    f.callee = vm.entity(a.callee);
    if (!f.callee)
        f.callee = &caller;
    f.owner = a.owner;
    f.stack_id = a.stack_id;
    f.args = a.args;
    if (f.args.size() < 4)
        f.args.resize(4, 0);
    return f;
}

} // namespace

void Thread::enqueue(QueuedAction a)
{
    a.uid = next_uid++;
    if (!is_check && (a.flags & TtabRunImmediately)) {
        // Run on the next tick, ahead of everything (see run_immediately).
        a.ordered_priority = a.priority;
        a.mode = QueueMode::Idle;
        a.priority = INT16_MIN;
        queue.push_back(std::move(a));
        return;
    }
    const int active = active_index(*this);
    // A head continuation (Push Interaction flag 4) goes first among the waiting, whatever its
    // priority (the original's FUN_005f1f90).
    if (a.head && !is_check) {
        queue.insert(queue.begin() + (active + 1), std::move(a));
        return;
    }
    // As the original's (FUN_005f01f0): waiting actions at the back of lower priority than the
    // new one are dropped, each object's Queue Skipped (entry 4) run with the Sim as its stack
    // object (the object lets go of what it kept for them). A player's order clears what
    // autonomy had queued; a fire's panic (priority 100) clears the player's orders.
    if (entity_.person && !is_check && a.mode == QueueMode::Normal)
        while (int(queue.size()) > active + 1 && queue.back().mode == QueueMode::Normal &&
               queue.back().priority < a.priority) {
            const int16_t object = queue.back().callee;
            if (TS_ENV("TS_QUEUE_WHY"))   // debug
                std::printf("queue: #%d drops interaction %d on #%d (pushed out by priority %d)\n", int(entity_.id),
                            queue.back().interaction, int(object), int(a.priority));
            queue.pop_back();
            if (Entity *obj = vm_.entity(object); obj && !obj->person)
                obj->run_entry(vm_, 4, true, int16_t(entity_.id));
        }
    if (queue.empty()) {
        queue.push_back(std::move(a));
    } else {
        // After everything of the same or higher priority, but never past a parent's exit.
        bool past_parent = a.mode != QueueMode::ParentIdle;
        for (int i = int(queue.size()) - 1; i > active; i--) {
            if (past_parent && (a.priority <= queue[size_t(i)].priority || queue[size_t(i)].mode == QueueMode::ParentExit)) {
                queue.insert(queue.begin() + (i + 1), std::move(a));
                return;
            }
            if (queue[size_t(i)].mode == QueueMode::ParentExit)
                past_parent = true;
        }
        queue.insert(queue.begin() + (active + 1), std::move(a));
    }
}

bool Thread::input_waiting() const
{
    if (!entity_.person || entity_.person_data.size() <= 71 || entity_.person_data[71] != 0)
        return false;
    const int active = active_index(*this);
    for (size_t i = size_t(active + 1); i < queue.size(); i++) {
        const QueuedAction &q = queue[i];
        if (q.mode == QueueMode::Idle || q.cancelled)
            continue;
        if (q.head || q.priority > entity_.person_data[33])
            return true;
        break;   // only the first waiting one counts
    }
    if (active >= 0 && queue[size_t(active)].cancelled)
        return true;   // the player cancelled what's running
    // Emergencies (residential lots, not visitors: person data 32 under 2).
    if (entity_.person_data[32] < 2 && vm_.residential()) {
        if (entity_.motives[7] < -99 || entity_.motives[9] < -99)
            return true;
        if (entity_.motives[5] < -99 && entity_.person_data.size() > 65 && entity_.person_data[65] < 2)
            return true;
    }
    return false;
}

void Thread::cancel(uint16_t uid)
{
    auto it = std::find_if(queue.begin(), queue.end(), [&](const QueuedAction &q) { return q.uid == uid; });
    if (it == queue.end())
        return;
    it->cancelled = true;
    const int index = int(it - queue.begin());
    if (it->mode == QueueMode::ParentIdle) {
        for (size_t i = size_t(index) + 1; i < queue.size(); i++) {
            if (queue[i].mode == QueueMode::ParentIdle) {
                queue.erase(queue.begin() + long(i--));
            } else if (queue[i].mode == QueueMode::ParentExit) {
                queue[i].cancelled = true;
                queue[i].priority = 0;
            }
        }
    }
    QueuedAction &q = queue[size_t(index)];
    if (index > active_index(*this) && q.mode == QueueMode::Normal) {
        // Dropped before it starts: its object's Queue Skipped (entry 4) runs with the Sim as its
        // stack object, as when a higher priority pushes it out (enqueue; FreeSO CancelAction).
        // A social's object says the other Sim is off ("Cancel": its attributes 13 and 22), and the
        // one who started it stops waiting: a ghost whose "Spooked" the player cancelled waited for
        // the rest of the night where it stood (in the doorway Cassandra had to use, 2026-10-30).
        const int16_t object = q.callee;
        queue.erase(queue.begin() + index);
        if (Entity *obj = vm_.entity(object); obj && !obj->person)
            obj->run_entry(vm_, 4, true, int16_t(entity_.id));
    } else {
        entity_.set_flag(FlagInteractionCanceled, true);
        q.priority = 0;
        // The running one: what it's worth is 0 now (the exe's cancel zeroes person data 33, which
        // the game's code reads: Wake Up's "33 is 0: stop", the ghost's "33 < 25"). Ours kept it.
        if (entity_.person && index == active_index(*this) && entity_.person_data.size() > 33)
            entity_.person_data[33] = 0;
    }
}

bool Thread::check(const QueuedAction &a, std::map<int, int16_t> *ads, const TtabInteraction *ia)
{
    if (ads)
        ads->clear();
    // A burning callee offers nothing, menus, free will and starts alike (FUN_0060b110 refuses
    // every entry of an object whose flags have bit 9, Burning, before its check tree; ours had
    // the test in Find Best Object only).
    if (const Entity *callee = vm_.entity(a.callee); callee && callee != &entity_ && callee->flag(FlagBurning))
        return false;
    // A pet takes only what's marked for its kind, pushed or chosen: the exe tests every queued
    // action as it starts (FUN_005f8de0 -> FUN_0060b110: the interaction's cat and dog flags against
    // person data 0x612's). Pushed ones skipped it here, so the fire's "Fire!" (flags 0x109, no pet
    // bits), pushed onto everyone in the room, had the Kats' cats screaming and panicking like Sims
    // (the user's report, 2026-11-03).
    // The visitor rule at the start too, pushed ones included (the same FUN_0060b110).
    // TS_VISITOR_PUSH_OLD=1: not at the start, as before.
    static const bool visitor_push_old = TS_ENV("TS_VISITOR_PUSH_OLD") != nullptr || TS_ENV("TS_VISITOR_OLD");
    if (!visitor_push_old && entity_.person && a.interaction >= 0 && vm_.visitor_refused(entity_, a.flags, vm_.entity(a.callee)))
        return false;
    if (entity_.person && a.interaction >= 0 && !TS_ENV("TS_PETS_TAKE_PUSHES"))
        if (const char pet = vm_.pet_kind(entity_)) {
            const uint32_t want = pet == 'c' ? TtabAllowCats : TtabAllowDogs;
            if (!(a.flags & want))
                return false;
        }
    // Pushed ones run their check too, as the exe's start test does for every queued action
    // (FUN_0060b110 -> the TTAB check tree, after the visitor and pet tests). TS_PUSH_NO_CHECK
    // (debug): pushed ones skip it, as before.
    static const bool push_no_check = TS_ENV("TS_PUSH_NO_CHECK") != nullptr;
    if ((a.must_run && push_no_check) || !a.check)
        return true;
    const auto saved_changes = std::move(ad_changes);
    const TtabInteraction *saved_source = ad_source;
    ad_changes.clear();
    ad_source = ia;
    const bool ok = run_now(frame_for(vm_, entity_, a, a.check)) == Exit::ReturnTrue;
    if (ads)
        ads->swap(ad_changes);   // (ads was emptied above: handed over, not copied)
    ad_changes = std::move(saved_changes);
    ad_source = saved_source;
    return ok;
}

bool Thread::attempt_push()
{
    while (!queue.empty()) {
        QueuedAction &a = queue[0];
        if (a.cancelled)
            entity_.set_flag(FlagInteractionCanceled, true);
        if ((a.mode != QueueMode::ParentIdle || !entity_.flag(FlagInteractionCanceled)) && a.action && ((a.found && !a.cancelled) || check(a))) {
            // The engine tells the code how important what it is doing is
            // (person data 33): sleep keeps going while it isn't 0. A "run immediately" one waits
            // here at a placeholder priority (enqueue), its own kept aside: started from here (free
            // will's Gosub Found Action starts what it chose at once), it ran at 1, and so did what
            // it pushed "at the pusher's priority": a free-will social's other half came in at 1 and
            // the other Sim's next free-will choice (2) threw it out. John's talks with Linda ended
            // as they began (the user's "they don't interact much", 2026-11-04); the exe runs them at
            // the order's priority (FUN_005f01f0, FUN_005f7570: 2 for free will).
            const int16_t running = (a.flags & TtabRunImmediately) && a.priority == INT16_MIN ? a.ordered_priority : a.priority;
            if (entity_.person)
                entity_.person_data[33] = std::max<int16_t>(running, 1);
            if (TS_ENV("TS_QUEUE_WHY"))   // debug (and the interaction sweep's "started")
                std::printf("queue: tick %u #%d starts %s (interaction %d on #%d%s%s)%s\n", unsigned(vm_.ticks), int(entity_.id),
                            a.name.c_str(), a.interaction, int(a.callee),
                            TS_ENV("TS_QUEUE_FLAGS") ? (" " + std::string(vm_.entity(a.callee) && vm_.entity(a.callee)->info ? vm_.entity(a.callee)->info->label : "?")).c_str() : "",
                            TS_ENV("TS_QUEUE_FLAGS") ? (", flags " + std::to_string(a.flags) + ", type " + std::to_string(entity_.person_data.size() > 32 ? entity_.person_data[32] : -1)).c_str() : "",
                            TS_ENV("TS_QUEUE_PRIORITY") ? (" p" + std::to_string(a.priority) + (a.found ? " free will" : "") +
                                                                (a.must_run ? " pushed" : "")).c_str()
                                                             : "");
            Frame f = frame_for(vm_, entity_, a, a.action);
            f.action_tree = true;
            read_ahead_animations(vm_, entity_, f.owner, a.action);   // (what it will play, read in meanwhile)
            push(std::move(f));
            return true;
        }
        if (vm_.interaction_ends) {
            Vm::InteractionEnd end;
            end.sim = int16_t(entity_.id);
            end.interaction = a.interaction;
            end.callee = a.callee;
            end.routine = a.cancelled ? "(cancelled)" : !a.action ? "(no action)" : "(its check says no)";
            end.result = -2;
            vm_.interaction_ends->push_back(std::move(end));
        }
        if (TS_ENV("TS_QUEUE_WHY"))   // debug: what's dropped as it comes up
            std::printf("queue: #%d drops interaction %d \"%s\" on #%d (%s%s%s%s)\n", int(entity_.id), a.interaction, a.name.c_str(),
                        int(a.callee), a.cancelled ? "cancelled" : !a.action ? "no action" : "its check says no",
                        a.must_run ? ", pushed" : "", a.check ? ", check " : "", a.check ? a.check->name.c_str() : "");
        // One that can't start runs its object's Queue Skipped (entry 4) as it goes, as the exe's
        // start (FUN_005f8de0 -> FUN_005f01a0) does: a social's other half let go, the object's
        // own bookkeeping undone. Ours ran it only on a priority drop or a cancel. (A cancelled
        // one had it from cancel().)
        const QueuedAction gone = std::move(queue[0]);
        queue.erase(queue.begin());
        if (!gone.cancelled)
            if (Entity *obj = vm_.entity(gone.callee); obj && !obj->person)
                obj->run_entry(vm_, 4, true, int16_t(entity_.id));
        continue;
    }
    return false;
}

int Thread::nest_waiting()
{
    const int active = active_index(*this);
    size_t i = size_t(active + 1);
    while (i < queue.size() && (queue[i].mode == QueueMode::Idle || queue[i].cancelled))
        i++;
    if (i >= queue.size() || queue[i].mode != QueueMode::Normal || queue[i].priority == 0)
        return 0;
    if (!queue[i].action || !((queue[i].found && !queue[i].cancelled) || check(queue[i]))) {
        // Can't start: out of the queue with its Queue Skipped, as the exe's Idle For Input pops
        // the head and drops a failing one (FUN_005f8de0). Ours left it there, and the waiting
        // interaction tried it again every tick for good.
        if (TS_ENV("TS_QUEUE_WHY"))   // debug
            std::printf("queue: #%d drops waiting interaction %d \"%s\" (it can't start)\n", int(entity_.id),
                        queue[i].interaction, queue[i].name.c_str());
        const QueuedAction gone = std::move(queue[i]);
        queue.erase(queue.begin() + long(i));
        if (Entity *obj = vm_.entity(gone.callee); obj && !obj->person)
            obj->run_entry(vm_, 4, true, int16_t(entity_.id));
        return -1;
    }
    QueuedAction a = std::move(queue[i]);
    queue.erase(queue.begin() + long(i));
    Frame f = frame_for(vm_, entity_, a, a.action);
    f.action_tree = true;
    f.discard_result = true;
    if (entity_.person) {
        f.saved_running = entity_.person_data[33];
        entity_.person_data[33] = std::max<int16_t>(a.priority, 1);
    }   // (as a "run immediately" one: when it ends, the waiting frame goes on)
    queue.insert(queue.begin(), std::move(a));
    push(std::move(f));
    return 1;
}

void Thread::run_immediately()
{
    if (queue.empty())   // (every object's, nearly: nothing to do, without the walk of its stack)
        return;
    // Drop interactions with objects that are gone (not the running ones).
    const int active = active_index(*this);
    for (size_t i = size_t(active + 1); i < queue.size(); i++)
        if (!vm_.entity(queue[i].callee))
            queue.erase(queue.begin() + long(i--));
    auto it = std::find_if(queue.begin(), queue.end(),
                           [](const QueuedAction &q) { return (q.flags & TtabRunImmediately) != 0; });
    if (it == queue.end())
        return;
    const int index = int(it - queue.begin());
    if (index > active || (!in_interaction() && index >= 0)) {
        QueuedAction a = std::move(*it);
        queue.erase(it);
        if (!a.action)
            return;
        Frame f = frame_for(vm_, entity_, a, a.action);
        f.action_tree = true;
        f.discard_result = true;
        // As the original's (FUN_005f01f0 runs it with person data 33 set to the order's priority,
        // then puts it back): what it pushes "at the pusher's priority" has the order's. Ours left
        // 33 as it was, 0 for an idle Sim, so the conversation a player's "Talk" queued went at
        // priority 0 and the Sim's next free-will choice threw it out (the user: orders to
        // socialise felt overridden).
        if (entity_.person) {
            f.saved_running = entity_.person_data[33];
            entity_.person_data[33] = std::max<int16_t>(a.ordered_priority, 1);
        }
        queue.insert(queue.begin(), std::move(a));
        push(std::move(f));
    }
}

// ---------------------------------------------------------------- tree tables

const Ttab *Vm::ttab(const LoadedIff *file, uint16_t id)
{
    if (!file)
        return nullptr;
    auto key = std::make_pair(file, id);
    auto it = tables_.find(key);
    if (it != tables_.end())
        return it->second.get();
    std::unique_ptr<Ttab> t;
    if (const IffChunk *c = file->iff.find("TTAB", id)) {
        t = std::make_unique<Ttab>();
        std::string err;
        if (!parse_ttab(file->body(*c), c->size, *t, err))
            t.reset();
    }
    return (tables_[key] = std::move(t)).get();
}

bool Entity::action(Vm &vm, int number, int16_t stack, QueuedAction &out)
{
    if (!tree_table)
        return false;
    const TtabInteraction *found = nullptr;
    for (const TtabInteraction &ia : tree_table->interactions)
        if (int(ia.tta_index) == number) {
            found = &ia;
            break;
        }
    if (!found)
        return false;
    // (A fresh one, but its name's and arguments' room kept: free will fills one for every
    // interaction it looks at, two allocations each, 1 us each on the Vita.)
    std::string name = std::move(out.name);
    std::vector<int16_t> args = std::move(out.args);
    out = QueuedAction();
    name.clear();
    out.name = std::move(name);
    out.args = std::move(args);
    out.action = vm.routine(code, found->action);
    out.check = found->test ? vm.routine(code, found->test) : nullptr;
    out.owner = code;
    out.callee = int16_t(id);
    out.stack_id = stack;
    out.interaction = number;
    out.flags = found->flags;
    out.args.assign(4, 0);
    // The name, from the TTAs strings beside the table.
    if (const StringTable *names = vm.string_table(tree_file, "TTAs", tree_table_id))
        if (const std::string *s = names->get(found->tta_index))
            out.name.assign(*s);
    return out.action != nullptr;
}

const Routine *Entity::tree_by_name(Vm &vm, const std::string &name, CodeOwner &owner)
{
    for (const LoadedIff *f : { code.own, code.semi }) {
        if (!f)
            continue;
        for (const IffChunk &c : f->iff.chunks)
            if (std::strcmp(c.type, "BHAV") == 0 && c.label == name) {
                owner = code;
                return vm.routine(code, c.id);
            }
    }
    return nullptr;
}

// ---------------------------------------------------------------- the player

Vm::MenuOwners Vm::menu_owners()
{
    MenuOwners out;
    for (auto &e : entities)
        if (!e->dead && e->tree_table && e->info)
            if (const uint16_t master = e->info->objd.get(Objd::MasterID))
                out[master].push_back(e.get());
    return out;
}

Entity *Vm::menu_owner(Entity *obj, const MenuOwners *owners)
{
    if (!obj || !obj->info)
        return obj;
    // Parts sharing one set of interactions (FreeSO GetInteractionGroupLeader):
    // a part in interaction group n gets its menu from the part marked -n.
    const int16_t interaction_group = int16_t(obj->info->objd.get(Objd::InteractionGroupID));
    if (interaction_group > 0)
        for (Entity *part : obj->group)
            if (part && !part->dead && part->info &&
                int16_t(part->info->objd.get(Objd::InteractionGroupID)) == -interaction_group)
                return part;
    if (obj->tree_table)
        return obj;
    // A part of a multi-tile object: the nearest part of the same object that has the table.
    const uint16_t master = obj->info->objd.get(Objd::MasterID);
    if (master == 0)
        return obj;
    Entity *best = nullptr;
    int best_d = INT32_MAX;
    auto consider = [&](Entity *e) {
        if (!e->dead && e->tree_table && e->info && e->info->objd.get(Objd::MasterID) == master &&
            e->info->ref.path == obj->info->ref.path) {
            const int d = std::abs(e->x16 - obj->x16) + std::abs(e->y16 - obj->y16);
            if (d < best_d) {
                best_d = d;
                best = e;
            }
        }
    };
    if (owners) {
        if (auto it = owners->find(master); it != owners->end())
            for (Entity *e : it->second)
                consider(e);
    } else {
        for (auto &e : entities)
            consider(e.get());
    }
    return best ? best : obj;
}

bool Vm::visitor_refused(const Entity &sim, uint32_t flags, const Entity *object) const
{
    if (!sim.person || (flags & TtabAllowVisitors) || sim.person_data.size() <= 32 || sim.person_data[32] == 0)
        return false;
    // (A lot a family lives on is residential whatever residential() says: Magic Town's homes.)
    if (!residential() && !(hood && hood->family_in_house(house)) && (flags & TtabNoDemoChild))
        return false;
    // The exe's exception (the same FUN_0060b110): an object of category 4 or 7 (object data 59;
    // the exe's +0xc0) for a visitor whose neighbour (person data 31, FUN_0054b880) holds a token of
    // kind 0 of the object type 13's GUID (FUN_00573520: the only one is Vacation's Hotel Key,
    // 3AC022D6) with a count above 0 (FUN_00424fb0): a hotel's guests in the hotel's rooms.
    if (object && hood && sim.person_data.size() > 31) {
        const int16_t category = object->value(VarCategory);
        if (category == 4 || category == 7)
            if (const Neighbor *n = hood->by_id(sim.person_data[31]))
                for (const Token &t : n->tokens)
                    if (t.kind == 0 && t.guid == 0x3AC022D6u && t.count > 0)
                        return false;
    }
    return true;
}

bool Vm::resident(const Entity &e) const
{
    if (!e.person || !e.info)
        return false;
    const Family *fam = family();   // the family living here, or the one visiting downtown
    return fam && std::find(fam->members.begin(), fam->members.end(), e.info->guid) != fam->members.end();
}

char Vm::pet_kind(const Entity &person) const
{
    if (!person.person || !person.info)
        return 0;
    auto it = pet_kinds_.find(person.info->guid);
    if (it != pet_kinds_.end())
        return it->second;
    std::string skeleton;
    if (!person.info->body_strings.empty()) {
        skeleton = person.info->body_strings[0];
    } else if (const LoadedIff *f = content.load(person.info->ref)) {
        if (const IffChunk *c = f->iff.find("STR#", 200)) {
            StringTable t;
            std::string err;
            if (parse_str(f->body(*c), c->size, t, err))
                if (const std::string *s = t.get(0))
                    skeleton = *s;
        }
    }
    for (char &ch : skeleton)
        ch = char(std::tolower(uint8_t(ch)));
    const char kind = skeleton == "kat" ? 'c' : skeleton == "dog" ? 'd' : 0;
    return pet_kinds_[person.info->guid] = kind;
}

bool Vm::allowed(const Entity &sim, const TtabInteraction &ia, bool autonomy, const Entity *object) const
{
    if ((ia.flags & TtabDebug) && !(debug_menu && !autonomy))
        return false;
    // Pets only what's marked for their kind; people whatever the pet bits (FreeSO VMThread).
    const char pet = pet_kind(sim);
    if (pet) {
        if (!(ia.flags & (TtabAllowCats | TtabAllowDogs)))
            return false;
        if ((pet == 'c' && !(ia.flags & TtabAllowCats)) || (pet == 'd' && !(ia.flags & TtabAllowDogs)))
            return false;
    }
    const bool child = sim.person_data[58] > 0 && sim.person_data[58] < 18;   // PersonsAge, as "Am I a Child?" reads it
    // No Adult isn't asked of a pet (FUN_0060a8e0): the pets' own things carry it, and with it no
    // pet could choose anything at all, their bowls and beds included, for as long as there were pets.
    if ((child && (ia.flags & TtabNoChild)) || (!child && !pet && (ia.flags & TtabNoAdult)))
        return false;
    // The household's things are for the household; on a community lot, only what's marked so
    // (the exe's visitor rule). TS_VISITOR_OLD=1: ours before 2026-11-04 (residential lots with a
    // family only: a downtown townie bought a light meal at a restaurant's fridge, whose dish then
    // wouldn't let him eat it, and everyone tried to eat it all day, the "Eat" loop on lots 21-27).
    static const bool visitor_old = TS_ENV("TS_VISITOR_OLD") != nullptr;
    if (visitor_old) {
        if (!(ia.flags & TtabAllowVisitors) && !resident(sim) && hood && hood->family_in_house(house))
            return false;
    } else if (visitor_refused(sim, ia.flags, object)) {
        return false;
    }
    // The interaction's autonomy threshold against the Sim's autonomy level (person data 36, 50
    // for people by the game's own init; the original's candidate filter FUN_0060a8e0 skips an
    // interaction whose threshold is above it, as it skips Debug ones): "Clog" and the like at
    // 100 are for the game's code, never a Sim's choice.
    if (autonomy && sim.person_data.size() > 36 && int(ia.autonomy_threshold) > sim.person_data[36])
        return false;   // (the player's menu: not checked here; the original's menu rule unread)
    return true;
}

std::vector<Vm::MenuEntry> Vm::pie_menu(Entity &sim, Entity &obj)
{
    std::vector<MenuEntry> out;
    if (!obj.tree_table || !sim.thread)
        return out;
    static const bool why = TS_ENV("TS_MENU_WHY") != nullptr;   // debug: why each is left out
    for (const TtabInteraction &ia : obj.tree_table->interactions) {
        if (!allowed(sim, ia, false, &obj)) {
            if (why) {
                QueuedAction named_a;
                obj.action(*this, int(ia.tta_index), int16_t(obj.id), named_a);
                std::printf("menu: #%u \"%s\" not allowed (flags %08x)\n", unsigned(ia.tta_index), named_a.name.c_str(),
                            unsigned(ia.flags));
            }
            continue;
        }
        QueuedAction a;
        if (!obj.action(*this, int(ia.tta_index), int16_t(obj.id), a) || a.name.empty() || a.name[0] == '*') {
            if (why)
                std::printf("menu: #%u no action or a hidden name (%s)\n", unsigned(ia.tta_index), a.name.c_str());
            continue;
        }
        std::vector<std::pair<std::string, int16_t>> named;   // what the check tree calls its entries
        sim.thread->action_strings = &named;
        sim.set_value(VarHideInteraction, 0);
        const bool ok = (debug_menu && (ia.flags & TtabDebug)) || sim.thread->check(a);   // (tests: a debug one shown whatever its check says)
        sim.thread->action_strings = nullptr;
        const bool hidden = sim.value(VarHideInteraction) == 1;   // the check's "hide menu": pushed only (FreeSO)
        sim.set_value(VarHideInteraction, 0);
        if (!ok || hidden) {
            if (why) {
                std::printf("menu: %s: %s (check %s)\n", a.name.c_str(), hidden ? "hidden from the menu" : "its check says no",
                            a.check ? a.check->name.c_str() : "-");
                for (const auto &p : entities)   // debug: the object's parts: flags and what their slots hold
                    if (p && p->info && obj.info && p->info->guid == obj.info->guid ||
                        (p && p->info && obj.info && p->info->label.substr(0, 12) == obj.info->label.substr(0, 12))) {
                        std::printf("  part #%d %s: attr0 %d data8 %04x slots", int(p->id), p->info->label.c_str(),
                                    p->attributes.empty() ? -999 : int(p->attributes[0]), unsigned(uint16_t(p->data[8])));
                        for (const Entity *s : p->slots)
                            std::printf(" %d", s ? int(s->id) : 0);
                        std::printf("\n");
                    }
            }
            continue;
        }
        if (named.empty()) {
            out.push_back({ a.name, int(ia.tta_index) });
            continue;
        }
        // One entry each ("Call..." on a phone: Go Downtown, Vacation, ... one per plugin),
        // under the interaction's own name when it opens a submenu.
        const bool submenu = a.name.size() > 3 && a.name.compare(a.name.size() - 3, 3, "...") == 0;
        for (const auto &[name, param] : named)
            out.push_back({ submenu ? a.name + "/" + name : name, int(ia.tta_index), param });
    }
    return out;
}

bool Vm::queue_interaction(Entity &sim, Entity &obj, int number, int16_t param)
{
    QueuedAction a;
    if (!sim.thread || !obj.action(*this, number, int16_t(obj.id), a))
        return false;
    if (param >= 0) {   // a named entry: what it is about, in parameter 0 (a phone plugin)
        if (a.args.size() < 4)
            a.args.resize(4, 0);
        a.args[0] = param;
    }
    a.priority = PriorityUser;
    sim.thread->enqueue(std::move(a));
    return true;
}

Entity *Vm::selected()
{
    if (Entity *e = entity(selected_id)) {
        if (e->person && on_lot(*e)) {
            chosen_away = -1;   // home again: as any other
            return e;
        }
        if (e->person && !e->dead && e->id == chosen_away)
            return e;
    }
    // The family living here, in the order the family lists them.
    if (const Family *fam = family())
        for (uint32_t guid : fam->members)
            for (auto &e : entities)
                if (e->person && on_lot(*e) && e->info && e->info->guid == guid && !pet_kind(*e)) {
                    selected_id = int16_t(e->id);
                    return e.get();
                }
    // At home with everyone away (at work, at school): the one selected stays selected, away (as
    // choosing an away Sim keeps them, and Next Sim goes round those away too), else the first of
    // the family; never a visitor. The "whoever is" below picked the neighbour visiting while Lisa
    // was at work: the plumbob over Bella, her needs in the panel, Lisa's face gone (the user,
    // 2026-11-10). TS_SELECT_AWAY_OLD=1: as before.
    static const bool away_old = TS_ENV("TS_SELECT_AWAY_OLD") != nullptr;
    if (!away_old && hood)
        if (const Family *home = hood->family_in_house(house)) {
            auto member = [&](const Entity &e) {
                return e.person && !e.dead && e.info && !pet_kind(e) &&
                       std::find(home->members.begin(), home->members.end(), e.info->guid) != home->members.end();
            };
            if (Entity *e = entity(selected_id); e && member(*e))
                return e;
            for (uint32_t guid : home->members)
                for (auto &e : entities)
                    if (member(*e) && e->info->guid == guid) {
                        selected_id = int16_t(e->id);
                        return e.get();
                    }
        }
    // Nobody of theirs here yet (a trip's travellers still walking in from the street): whoever
    // is, but not kept, or the lot's own staff stayed selected once the family had arrived
    // (the survey's trips went on with a barmaid, a maintenance man, a raccoon; 2026-10-31).
    for (auto &e : entities)
        if (e->person && on_lot(*e))
            return e.get();
    return nullptr;
}

bool Vm::cancel_last(Entity &sim)
{
    if (!sim.thread || sim.thread->queue.empty())
        return false;
    for (auto it = sim.thread->queue.rbegin(); it != sim.thread->queue.rend(); ++it)
        if (it->mode != QueueMode::Idle && !it->cancelled) {
            sim.thread->cancel(it->uid);
            return true;
        }
    return false;
}

void Vm::select_next()
{
    Entity *now = selected();
    std::vector<Entity *> family_sims;
    const Family *fam = family();
    for (auto &e : entities)   // the family, those away (at work, at school) too, as in the game
        if (e->person && !e->dead && e->info && !pet_kind(*e) &&   // (a family's pets aren't played)
            (fam ? std::find(fam->members.begin(), fam->members.end(), e->info->guid) != fam->members.end()
                 : on_lot(*e)))
            family_sims.push_back(e.get());
    if (family_sims.empty())
        return;
    auto it = std::find(family_sims.begin(), family_sims.end(), now);
    Entity *next = it == family_sims.end() || it + 1 == family_sims.end() ? family_sims.front() : *(it + 1);
    choose(int16_t(next->id));
}

// ---------------------------------------------------------------- motives

namespace {

// The game's own decay tuning for people (FreeSims VMTS1MotiveDecay; Global.iff FCNS 1 has the
// same): what's used if the FCNS can't be read.
constexpr float kDecay[] = {
    180, 16, 7, 0.01f, 0.3f, 0.0021f, 0.055f, 0.000125f, 0.25f, 1, 0.17f, 0.08f, 0.3f, 0.15f, 0.4f, 0.6f, 0.5f,
};
constexpr int kDecayMotives[] = { 7, 6, 8, 9, 5, 15, 14 };   // hunger comfort hygiene bladder energy fun social
constexpr int kMood = 3, kSleepState = 11, kRoom = 13, kEnergy = 5;

} // namespace

// The old whole-point way of objects' rates (TS_DECAY_THOUSANDTHS): a fraction each, whole points
// added to the shown need.
static void old_object_rates(Entity &e)
{
    for (Entity::MotiveChange &c : e.motive_changes) {   // (each entry on its own: two to one motive add up)
        const size_t i = size_t(c.motive);
        if (c.per_hour == 0 || i >= e.motives.size())
            continue;
        const double rate = c.per_hour / 60.0 / 30.0;
        c.fraction += rate;
        if (std::abs(c.fraction) >= 1) {
            int m = e.motives[i];
            if ((rate > 0 && m > c.max) || (rate < 0 && m < c.max))
                continue;   // already past it: leave it (don't clamp)
            m += int(c.fraction);
            c.fraction -= int(c.fraction);
            if ((rate > 0 && m > c.max) || (rate < 0 && m < c.max))
                m = c.max;
            e.motives[i] = int16_t(std::clamp(m, -100, 100));
        }
    }
}

// The seven decaying needs' floats take up what anything else did to the shown needs since (the
// game's code, an object's whole-point change): a shown need that isn't what was last shown is
// taken as it now is. The first time after a load, from the save's thousandths.
void Entity::sync_exact_needs()
{
    for (int i = 0; i < 7; i++) {
        const int which = kDecayMotives[i];
        if (!exact_ready || motives[size_t(which)] != exact_shown[size_t(i)])
            exact_need[size_t(i)] = float(motives[size_t(which)]) - (exact_ready ? 0.0f : decay_fractions[size_t(i)] / 1000.0f);
        exact_shown[size_t(i)] = motives[size_t(which)];
    }
    exact_ready = true;
}

// A need's float shown as the game's code reads it: rounded half away from zero (FUN_0061d920),
// within +-100; what it is below that kept for the save.
void Entity::show_exact_need(int i)
{
    const int which = kDecayMotives[i];
    const float need = exact_need[size_t(i)];
    const int shown = int(need < 0 ? need - 0.5f : need + 0.5f);
    motives[size_t(which)] = int16_t(std::clamp(shown, -100, 100));
    exact_shown[size_t(i)] = motives[size_t(which)];
    decay_fractions[size_t(i)] = int16_t(std::lround((float(motives[size_t(which)]) - need) * 1000.0f));
}

void Entity::tick_motives(Vm &vm)
{
    static const bool old_way = TS_ENV("TS_DECAY_THOUSANDTHS") != nullptr;
    if (old_way) {
        old_object_rates(*this);
        decay_motives(vm);
        return;
    }
    // As the exe's tick for a Sim (0x5f5046 on): the decay when its time comes, then the objects'
    // rates, every tick, added to the same floats (0x5f5184: movss), each entry towards its max in
    // list order: a rise only while below the max and not past it, a fall likewise. Ours added
    // whole points to the shown need before the decay, and the decay then took the changed need up
    // afresh and dropped its fraction: a bed's comfort never rose, the TV's fun went too fast (the
    // second audit, 2026-11-04). The rates run whether the decay is skipped or not (hidden, person
    // data 29), as the exe's.
    sync_exact_needs();
    decay_motives(vm);
    for (MotiveChange &c : motive_changes) {
        const size_t i = size_t(c.motive);
        if (c.per_hour == 0 || i >= motives.size())
            continue;
        const float rate = float(c.per_hour) / 60.0f / 30.0f;
        int slot = -1;
        for (int k = 0; k < 7; k++)
            if (kDecayMotives[k] == int(i))
                slot = k;
        if (slot < 0) {   // (not one of the seven: Set Motive Change allows only those, but in case)
            float m = float(motives[i]) + rate;
            motives[i] = int16_t(std::clamp(int(std::lround(m)), -100, 100));
            continue;
        }
        float &need = exact_need[size_t(slot)];
        const float max = float(c.max);
        if (rate >= 0) {
            if (need < max)
                need = std::min(need + rate, max);
        } else if (need > max) {
            need = std::max(need + rate, max);
        }
        show_exact_need(slot);
    }
}

void Entity::decay_motives(Vm &vm)
{
    // The steady decay, every two game minutes, as the original's (its code, 0x6833f0, with the
    // tuning of Global.iff FCNS 1, 5 or 6: people, dogs, cats).
    const int half_minutes = vm.clock.minutes / 2;
    if (half_minutes == decay_minute)
        return;
    decay_minute = half_minutes;   // (the exe's per-Sim timer goes on whether it decays or not)
    // Not while hidden, nor while person data 29 ("Cheats") is set: the exe decays a Sim only with
    // both 0 (0x5f5053). The game's code sets 29 for ghosts ("person main" also puts their needs at
    // 75), Sims away on a trip or abducted, Create-A-Sim's, and some NPCs; ours never read it, so
    // their needs fell (a ghost went for food and the toilet). The mood is the decay's, so it stays.
    if (value(VarHidden) > 0 || (person && person_data.size() > 29 && person_data[29] != 0))
        return;
    const char pet = vm.pet_kind(*this);
    const std::array<float, 17> &k = vm.decay_tuning(pet);
    // Asleep: the sleep state below 0, as the exe tests it (0x6833f0; the tent's "Play" sets 1 for
    // two Sims woken in it, awake to the decay; ours took any value but 0 as asleep).
    const bool sleeping = motives[kSleepState] < 0;
    // What was eaten since the last decay fills the bladder ("hunger to bladder multiplier").
    const int ate = decay_hunger == -32768 ? 0 : std::max(0, motives[7] - decay_hunger);
    // The needs as the exe keeps them: floats, falling by its formulas in floats and read by the
    // game's code rounded half away from zero (FUN_0061d920). Ours held whole numbers and a store of
    // thousandths cut down at each step: hunger fell from the whole number (0.38 lower a day), social
    // 0.6% slow, and scripts saw a falling need up to a point too high (the 2026-11-04 decay check).
    // Here each of the seven has its exact value; a need something else changed (an object, the
    // game's code) is taken up as it now is. decay_fractions keeps what the exact value is below the
    // shown one, in thousandths, for the save. TS_DECAY_THOUSANDTHS=1: the old whole-number way.
    static const bool old_way = TS_ENV("TS_DECAY_THOUSANDTHS") != nullptr;
    if (!old_way)
        sync_exact_needs();
    for (int i = 0; i < 7; i++) {
        const int which = kDecayMotives[i];
        float &need = exact_need[size_t(i)];
        float fall = 0;   // in points, this step (a negative one a rise: energy asleep)
        switch (i) {
        case 0:   // hunger: (hunger + 100) x the ratio, from the need itself
            fall = k[5] * (100.0f + (old_way ? float(motives[7]) : need));
            break;
        case 1:   // comfort: from lazy's to active's by the Active personality (0-1000)
            fall = k[15] + (k[14] - k[15]) * float(person_data[3]) / 1000.0f;
            break;
        case 2: fall = k[sleeping ? 11 : 10]; break;
        case 3: fall = k[sleeping ? 13 : 12] + float(ate) * k[4]; break;
        case 4: {   // energy, as the original's (0x6833f0, its tuning set at 0x6829f0-ish):
            // awake it falls by the span over the waking hours (180 / 16 an hour); asleep it rises
            // by the span over the sleeping hours (180 / 8: 22.5 an hour, whatever the bed: the
            // bed's own rate comes on top), less the drift in the daytime. Pets: the span, and
            // the waking hours.
            const int wake = int(k[2]), until = int(k[2]) + int(k[1]);
            if (sleeping) {
                fall = -(pet ? k[1] / 30.0f : k[0] / (24.0f - k[1]) / 30.0f);
                if (vm.clock.hours > wake && vm.clock.hours < until)
                    fall += k[3];
            } else {
                fall = pet ? k[0] / 30.0f : k[0] / (30.0f * float(int(k[1])));
            }
            break;
        }
        case 5: fall = sleeping ? 0.0f : k[8]; break;   // fun (asleep: x the asleep multiplier, 1)
        case 6: fall = sleeping ? 0.0f : k[6] + k[7] * float(person_data[6]); break;   // social, awake
        }
        if (fall > 0 && vm.motive_decay > 0)   // the player's "Mood degradation" option: slower falls
            fall *= vm.motive_decay == 1 ? 0.5f : 0.25f;
        if (old_way) {   // (as before: thousandths, cut down, whole points off as they come)
            int m = motives[size_t(which)];
            decay_fractions[size_t(i)] = int16_t(decay_fractions[size_t(i)] + int(fall * 1000.0f));
            if (std::abs(decay_fractions[size_t(i)]) >= 1000) {
                m -= decay_fractions[size_t(i)] / 1000;
                decay_fractions[size_t(i)] = int16_t(decay_fractions[size_t(i)] % 1000);
                motives[size_t(which)] = int16_t(std::clamp(m, -100, 100));
            }
            need = float(motives[size_t(which)]);
        } else {
            need = std::clamp(need - fall, -100.0f, 100.0f);   // (the exe clamps at each decay)
            show_exact_need(i);
        }
        exact_shown[size_t(i)] = motives[size_t(which)];
    }
    decay_hunger = motives[7];
    // The mood, as the original's (0x5f0d10, after its decay): the needs' weighted mean, each
    // weighed by its own curve at its level (Global STR# 502, children 504): a need near the
    // bottom counts for many (comfort at -100 ten times one above -40), a full one for a little
    // more. Energy isn't among them (pets: see below). (Ours was the plain mean of eight,
    // energy in: it slid all day with the energy.)
    static constexpr int kMoodMotives[7] = { 7, 6, 8, 9, 15, kRoom, 14 };   // hunger comfort hygiene bladder fun room social
    const int age = person_data[58];
    const std::vector<Vm::Curve> &weights = vm.mood_curves(age > 0 && age < 18);
    float sum = 0, total = 0;
    for (int i = 0; i < 7; i++) {
        // Pets: the exe leaves out a need whose *level* is 13 (0x5f0de6, its constant 13.0 compared
        // with the level, not the room's index: the original's own slip), so a pet's mood does have
        // the room in it. Ours left the room out. TS_PET_MOOD_OLD=1: as before.
        static const bool pet_old = TS_ENV("TS_PET_MOOD_OLD") != nullptr;
        if (pet && (pet_old ? kMoodMotives[i] == kRoom : motives[size_t(kMoodMotives[i])] == 13))
            continue;
        const float level = float(motives[size_t(kMoodMotives[i])]);
        const float w = weights[size_t(i)].at(level);
        sum += w * level;
        total += w;
    }
    motives[kMood] = int16_t(total > 0 ? std::lround(sum / total) : 0);
}

const std::array<float, 17> &Vm::decay_tuning(char pet_kind)
{
    auto it = decay_tuning_.find(pet_kind);
    if (it != decay_tuning_.end())
        return it->second;
    std::array<float, 17> &out = decay_tuning_[pet_kind];
    std::copy(std::begin(kDecay), std::end(kDecay), out.begin());
    // FCNS: 0, a version, "SNCF", the count; then each value: its name (zero-ended, padded to an
    // even length), the float, a description (the same).
    const uint16_t id = pet_kind == 'd' ? 5 : pet_kind == 'c' ? 6 : 1;
    const LoadedIff *g = global_code.own;
    const IffChunk *c = g ? g->iff.find("FCNS", id) : nullptr;
    const uint8_t *b = c ? g->body(*c) : nullptr;
    if (!b || c->size < 16)
        return out;
    size_t at = 16;
    auto skip_string = [&]() {
        const size_t from = at;
        while (at < c->size && b[at] != 0)
            at++;
        at++;
        if ((at - from) % 2)
            at++;
    };
    uint32_t count;
    std::memcpy(&count, b + 12, 4);
    for (uint32_t n = 0; n < count && n < out.size(); n++) {
        skip_string();
        if (at + 4 > c->size)
            break;
        std::memcpy(&out[n], b + at, 4);
        at += 4;
        skip_string();
    }
    if (TS_ENV("TS_DECAY")) {   // debug: the table read
        std::printf("decay tuning FCNS %d:", id);
        for (float v : out)
            std::printf(" %g", v);
        std::printf("\n");
    }
    return out;
}

const Entity *Vm::person_in_the_way(const Entity &walker, float x, float y) const
{
    // Two Sims' boxes (person_box: 3/16 either way, less the walker's routing footprint, person
    // data 73; the original's DAT_00a50fac) at the step's end against where the other stands.
    if (is_ghost(walker))
        return nullptr;   // a ghost walks through
    const Footprint mine = person_box(walker, int(std::lround(x)), int(std::lround(y)));
    const Footprint now = person_box(walker, walker.x16, walker.y16);
    static const bool rect_planner = TS_ENV("TS_ROUTE_RECT") == nullptr || TS_ENV("TS_ROUTE_RECT")[0] != '0';
    // A dog called to the Pet Bath (Unleashed, PetBath.iff 4134 "Dog - Move to Tub") walks to the tile
    // beside the Sim who called it, who waits there taking no pushes: blocked by her, the dog's walk
    // failed and its script cancelled the bath for both (attribute 9), the Sim draining the tub and
    // leaving (the community's "Pet Bath fix": "abandon it before the dog arrives"). The tub's A side
    // keeps the Sim (attribute 10) and the dog (11): the dog passes her. TS_PET_BATH_OLD=1: as before.
    int16_t bath_caller = 0;
    static const bool bath_old = TS_ENV("TS_PET_BATH_OLD") != nullptr;
    if (!bath_old && community_fixes && walker.route.target_id != 0)
        if (const Entity *tub = const_cast<Vm *>(this)->entity(walker.route.target_id);
            tub && tub->info && tub->info->where.find("PetBath.iff") != std::string::npos)
            for (const auto &t : entities)
                if (t->info && t->info->ref.path == tub->info->ref.path && t->attributes.size() > 11 &&
                    t->attributes[11] == walker.id && t->attributes[10] != 0)
                    bath_caller = t->attributes[10];
    for (const auto &up : entities) {
        const Entity &e = *up;
        if (&e == &walker || !e.person || e.dead || !e.in_world() || e.container || e.level != walker.level ||
            e.id == walker.route.target_id || is_ghost(e) || (bath_caller != 0 && e.id == bath_caller))
            continue;
        const Footprint theirs = person_box(e, e.x16, e.y16);
        if (!mine.overlaps(theirs))
            continue;
        // Already overlapping (they'd never part): let them pass. With the tile planner, a Sim on the
        // walker's own tile too (it can't steer round one there as the original's does in sixteenths).
        if (now.overlaps(theirs) || (!rect_planner && e.tile_x() == walker.tile_x() && e.tile_y() == walker.tile_y()))
            continue;
        // Only a step that brings them closer is blocked: two Sims stopped side by side can always part.
        const float before = std::hypot(float(e.x16 - walker.x16), float(e.y16 - walker.y16));
        if (std::hypot(float(e.x16) - x, float(e.y16) - y) >= before)
            continue;
        if (TS_ENV("TS_BLOCK_WHY"))   // debug: each blocked step, the two boxes
            std::printf("block: #%d at (%d, %d) step (%.2f, %.2f) box %d..%d, %d..%d; by #%d at (%d, %d) box %d..%d, %d..%d\n",
                        int(walker.id), int(walker.x16), int(walker.y16), double(x), double(y), mine.x0, mine.x1, mine.y0,
                        mine.y1, int(e.id), int(e.x16), int(e.y16), theirs.x0, theirs.x1, theirs.y0, theirs.y1);
        if (TS_ENV("TS_BLOCK_WHY")) {
            std::printf("  path from %zu:", walker.route.next);
            for (size_t k = walker.route.next; k < walker.route.path.size(); k++)
                std::printf(" (%d, %d)", int(walker.route.path[k].first), int(walker.route.path[k].second));
            std::printf("; at (%.2f, %.2f)\n", double(walker.route.fx), double(walker.route.fy));
        }
        return &e;
    }
    return nullptr;
}

float Vm::global_tuning(uint16_t id, const std::string &name, float fallback)
{
    const auto key = std::make_pair(id, name);
    auto it = global_tuning_.find(key);
    if (it != global_tuning_.end())
        return it->second;
    float value = fallback;
    const LoadedIff *g = global_code.own;
    const IffChunk *c = g ? g->iff.find("FCNS", id) : nullptr;
    const uint8_t *b = c ? g->body(*c) : nullptr;
    if (b && c->size >= 16) {
        size_t at = 16;
        auto read_string = [&]() {
            const size_t from = at;
            while (at < c->size && b[at] != 0)
                at++;
            std::string s(reinterpret_cast<const char *>(b + from), at - from);
            at++;
            if ((at - from) % 2)
                at++;
            return s;
        };
        uint32_t count;
        std::memcpy(&count, b + 12, 4);
        for (uint32_t n = 0; n < count && at < c->size; n++) {
            const std::string here = read_string();
            if (at + 4 > c->size)
                break;
            float v;
            std::memcpy(&v, b + at, 4);
            at += 4;
            read_string();
            if (here == name) {
                value = v;
                break;
            }
        }
    }
    if (TS_ENV("TS_TUNING"))   // debug
        std::printf("tuning FCNS %d \"%s\": %g%s\n", int(id), name.c_str(), double(value), value == fallback ? " (the default)" : "");
    global_tuning_[key] = value;
    return value;
}

} // namespace ts::vm
