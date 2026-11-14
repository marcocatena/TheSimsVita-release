/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/SimsAntics/primitives): the primitives that
 * act on the world, objects and people. Routing is simplified for now: Sims
 * walk straight to the spot a slot gives.
 */
#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <set>

#include "core/env.h"
#include "core/fast_clock.h"
#include "vm/primitives.h"
#include "vm/footprint.h"
#include "world/create_sim.h"

namespace ts::vm {

namespace {

Exit truth(bool b)
{
    return b ? Exit::GotoTrue : Exit::GotoFalse;
}

// The 8 directions as steps of one tile (north = -y, clockwise).
constexpr int kStep[8][2] = { { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 } };

// ---------------------------------------------------------------- animation

// An animation from one of the tables Animate Sim addresses: the code
// owner's (its OBJD's animation table, else STR# 129), or global ones.
const CharAnimation *animation_for(Frame &f, int source, int id)
{
    Vm &vm = f.vm();
    // The animation-name table (parsed once by the VM; read here, not copied).
    const StringTable *table = nullptr;
    uint16_t used = 0;   // (the warning below: which table answered)
    auto has_strings = [](const StringTable *t) { return t && t->get(0); };
    // Source 4 (FreeSO's own): the stack object's tables, for an animation id taken
    // from a parameter (the code passes another object's animation).
    CodeOwner so_owner;
    if (source == 4) {
        const Entity *so = f.stack_object();
        if (!so || !so->info)
            return nullptr;
        so_owner = vm.owner_of(*so->info);
    }
    const CodeOwner &code = source == 4 ? so_owner : f.owner;
    // The table asked first (the object's, by the ages of me and the other), for the key of the
    // answers kept (Vm::animations_found). TS_ANIM_LOOKUP_OLD=1: looked up afresh every time.
    uint16_t base = code.object ? code.object->objd.get(Objd::AnimationTableID) : 0;
    if (base == 0)
        base = 129;
    uint16_t t = 0;
    if (source == 0 || source == 4) {
        auto is_child = [](const Entity *e) { return e && e->person && e->person_data[58] > 0 && e->person_data[58] < 18; };
        const Entity *other = f.stack_object();
        const bool me_child = is_child(f.caller);
        t = uint16_t(base + (me_child ? 1 : 0));
        if (other && other->person && other != f.caller && is_child(other) != me_child)
            t = uint16_t(base + (me_child ? 3 : 2));
    }
    static const bool lookup_old = TS_ENV("TS_ANIM_LOOKUP_OLD") != nullptr;
    const Vm::AnimationKey key{ code.object, code.own, code.semi, uint32_t(source) << 16 | t, id };
    if (!lookup_old)
        if (auto it = vm.animations_found.find(key); it != vm.animations_found.end())
            return it->second;
    switch (source) {
    case 0: case 4: {   // the object's: adult (129), child (+1), adult with a child (+2), child with an adult (+3)
        // Then the standard 129 set: Unleashed's pets name table 128 in their OBJD, but their
        // character files hold 129-132 ("Animations"), so every pet animation by number was
        // missing: a cat collapsed asleep stood in its standing loop and dreamed every tick, its
        // thought bubble flashing through pictures (the user's Kats, 2026-11-03).
        const uint16_t standard = uint16_t(129 + (t - base));
        for (uint16_t which : { t, base, standard, uint16_t(129) }) {
            for (const LoadedIff *file : { code.own, code.semi }) {
                table = vm.string_table(file, "STR#", which);
                used = which;
                if (has_strings(table))
                    break;
            }
            if (has_strings(table))
                break;
        }
        break;
    }
    case 1: table = vm.string_table(vm.global_code.own, "STR#", 128); break;
    case 2: table = vm.string_table(vm.global_code.own, "STR#", 130); break;
    case 3: table = vm.string_table(vm.global_code.own, "STR#", 156); break;
    default: break;
    }
    const std::string *entry = id >= 0 && table ? table->get(size_t(id)) : nullptr;
    if (!entry || entry->empty()) {
        if (TS_ENV("TS_ANIM_START"))   // debug: which table had nothing
            std::printf("animation %d (source %d): table %u (%lu items) of %s has %s\n", id, source, unsigned(used),
                        table ? (unsigned long)table->items.size() : 0ul,
                        code.object ? code.object->label.c_str() : "?", entry ? "an empty entry" : "no entry");
        return nullptr;
    }
    std::string name = *entry;
    if ((source == 0 || source == 4) && name.find('-') == std::string::npos) {
        // A placeholder where this pairing of ages has no animation of its own ("Tickle A Loop",
        // or the entry's own number: the Grim Reaper's child table is "0" to "23"): the same entry
        // of the other age tables. (Every real animation name has a dash: a2o-..., c2o-....) The
        // Reaper's ceremony over a child read "17" as a name and missed its ritual loops (2026-10-30).
        for (int k = 0; k < 4; k++)   // (base: the object's table, as above)
            for (const LoadedIff *file : { code.own, code.semi }) {
                const StringTable *other = vm.string_table(file, "STR#", uint16_t(base + k));
                const std::string *s = other ? other->get(size_t(id)) : nullptr;
                if (s && s->find('-') != std::string::npos) {
                    if (vm.animation_waits(*s)) {   // its files asked of the reader: next tick
                        vm.animation_pending = true;
                        return nullptr;
                    }
                    if (!vm.animation(*s))
                        continue;
                    name = *s;
                    k = 4;
                    break;
                }
            }
    }
    if (vm.animation_waits(name)) {   // its files asked of the reader: next tick
        vm.animation_pending = true;
        return nullptr;
    }
    const CharAnimation *a = vm.animation(name);
    if (a && !lookup_old)
        vm.animations_found.emplace(key, a);
    if (!a)
        vm.warn(&f, "missing animation " + name + " (entry " + std::to_string(id) + " of the " +
                        (source == 1 ? "global 128" : source == 2 ? "global 130" : source == 3 ? "global 156" : "object's") +
                        " table" + (used ? " " + std::to_string(used) + ", " + std::to_string(table->items.size()) + " items" : "") +
                        ")");
    return a;
}

} // namespace (reopened below: this one is exported, primitives.h)

int read_ahead_animations(Vm &vm, const Entity &caller, const CodeOwner &code, const Routine *root)
{
    // The animations a tree of the object's own code (and its semi-global's: not the globals', whose
    // trees reach hundreds) can play, asked of the reader now. Where files are read in the background
    // (the Vita), Animate otherwise waits for each as it comes, up to 12 ticks: the genie's cast is six
    // animations he hadn't played, his zap came 60 ticks late on the chant (204 ticks after it where
    // the original's is about 144) and after Linda's outcome box (the user, 2026-11-07). The exe reads
    // each when asked, on the spot. TS_ANIM_AHEAD_OFF=1: none; TS_ANIM_AHEAD=1: each tree's count.
    static const bool off = TS_ENV("TS_ANIM_AHEAD_OFF") != nullptr;
    if (off || !root || !vm.request_file || !vm.characters)
        return 0;
    std::vector<const Routine *> todo{ root };
    std::set<const Routine *> seen{ root };
    std::set<std::pair<int, int>> ids;   // (source, id)
    while (!todo.empty()) {
        const Routine *r = todo.back();
        todo.pop_back();
        for (const BhavInstruction &in : r->code) {
            if (in.opcode == 44) {   // Animate Sim
                const int source = in.operand[4], flags = in.operand[5];
                const int id = in.operand[0] | in.operand[1] << 8;
                if (!(flags & 4) && id > 0 && source <= 3)   // (not one by a parameter's number)
                    ids.insert({ source, id });
            } else if (in.opcode >= 4096 && seen.size() < 96) {   // the object's own (4096+) or semi-global (8192+)
                if (const Routine *next = vm.routine(code, in.opcode); next && seen.insert(next).second)
                    todo.push_back(next);
            }
        }
    }
    auto has_strings = [](const StringTable *t) { return t && t->get(0); };
    uint16_t base = code.object ? code.object->objd.get(Objd::AnimationTableID) : 0;
    if (base == 0)
        base = 129;
    const bool child = caller.person && caller.person_data[58] > 0 && caller.person_data[58] < 18;
    const uint16_t t = uint16_t(base + (child ? 1 : 0));
    const StringTable *own = nullptr;
    for (uint16_t which : { t, base, uint16_t(129 + (t - base)), uint16_t(129) }) {
        for (const LoadedIff *file : { code.own, code.semi })
            if (const StringTable *s = vm.string_table(file, "STR#", which); has_strings(s)) {
                own = s;
                break;
            }
        if (own)
            break;
    }
    int asked = 0;
    for (const auto &[source, id] : ids) {
        const StringTable *table = source == 0 ? own
                                   : source == 1 ? vm.string_table(vm.global_code.own, "STR#", 128)
                                   : source == 2 ? vm.string_table(vm.global_code.own, "STR#", 130)
                                                 : vm.string_table(vm.global_code.own, "STR#", 156);
        const std::string *name = table ? table->get(size_t(id)) : nullptr;
        if (name && name->find('-') != std::string::npos && vm.animation_waits(*name)) {
            asked++;
            if (vm.animations_ahead.size() < 256 &&
                std::find(vm.animations_ahead.begin(), vm.animations_ahead.end(), *name) == vm.animations_ahead.end())
                vm.animations_ahead.push_back(*name);   // (its .cfp asked for once its bundle is in: Vm::tick)
        }
    }
    if (TS_ENV("TS_ANIM_AHEAD"))   // debug
        std::printf("tick %u animations read ahead for #%d's %s: %d of %d (%d routines)\n", unsigned(vm.ticks), int(caller.id),
                    root->name.c_str(), asked, int(ids.size()), int(seen.size()));
    return asked;
}

namespace {

void play_standing(Vm &vm, Entity &sim)
{
    sim.animations.clear();
    sim.held = AnimState();
    int posture = sim.person_data[0];
    if (posture != 1 && posture != 2)
        posture = 3;   // 1 sit, 2 kneel; standing is 3 in the walk table
    const std::vector<std::string> &moves = vm.moves(sim);   // treading water in a pool
    if (size_t(posture) < moves.size())
        if (const CharAnimation *a = vm.animation(moves[size_t(posture)])) {
            sim.animations.emplace_back(a, false);
            sim.animations.back().loop = true;
        }
}

Exit animate(Frame &f, const uint8_t *op)
{
    Entity &sim = *f.caller;
    if (!sim.person)
        return Exit::GotoTrue;
    const uint8_t local_event = op[2], source = op[4], flags = op[5], expected = op[6];
    const bool backwards = flags & 2, store_in_local = flags & 32, id_from_param = flags & 4;
    const int id = id_from_param ? (u16(op, 0) < f.args.size() ? f.args[u16(op, 0)] : 0) : u16(op, 0);
    const int mode = (flags & 1) | ((flags >> 3) & 2);
    Vm &vm = f.vm();
    // How fast what this starts plays, in frames a tick, as the exe's animation clock
    // (FUN_0057c360: ticks x 50 ms, the tick's 42 x 1.2; x 42 with flag 0x80) over 33 ms frames:
    // 1.5, or 1.26. Hurryable ones (flag 0x40) go at the walker's rate (FUN_005cf460: 1.4, 2.8 when
    // running, WalkStyle 1): 2.1 or 4.2. Ours played every one at 1 a tick: sitting down on the
    // Bobs' loveseat took 122 ticks where the original's takes about 59 (the second audit,
    // 2026-11-04). TS_ANIM_RATE_OLD=1: one a tick, as before.
    static const bool rate_old = TS_ENV("TS_ANIM_RATE_OLD") != nullptr;
    const float rate = rate_old ? 1.0f
                       : (flags & 0x40) ? 1.5f * (sim.value(VarWalkStyle) == 1 ? 2.8f : 1.4f)
                       : (flags & 0x80) ? 42.0f / (1000.0f / 30.0f)
                                        : 1.5f;
    if (id == 0 && TS_ENV("TS_ANIM_START"))   // debug
        std::printf("tick %u #%d animate 0 mode %d (carry %s, hand %s)\n", unsigned(vm.ticks), int(sim.id), mode,
                    sim.carry && sim.carry->anim ? sim.carry->anim->name.c_str() : "-",
                    !sim.slots.empty() && sim.slots[0] ? "full" : "empty");
    if (id == 0 && mode == 2) {
        // Animation 0 on the background channel (the carry pose's): that channel stops, the main
        // animation plays on. The SimCity drinks machine's "Buy Soda - Drink" does this between
        // the drink-stop animation's event and its end; ours took it as "back to standing" and
        // restarted the drink-stop for ever (the interaction sweep, 2026-10-30).
        sim.carry.reset();
        sim.carry_off = true;
        return Exit::GotoTrue;
    }
    if (id == 0 && mode == 1) {
        // The game's background channel (FUN_005d6380: stopped, and nothing else): the pose stays.
        // Ours took it as back to standing, a pop in the middle of washing a dish.
        if (AnimState *cur = sim.current_animation(); cur && cur->background) {
            sim.held = *cur;
            sim.held.loop = sim.held.background = false;
            sim.held.event_queue.clear();
            sim.animations.clear();
        }
        return Exit::GotoTrue;
    }
    if (id == 0) {   // back to the posture's standing loop
        if (sim.person_data.size() > 42 && sim.person_data[42] == 1)
            sim.person_data[42] = 4;   // the head turns back (FreeSO VMAnimateSim)
        play_standing(vm, sim);
        sim.carry.reset();
        if (!sim.slots.empty() && sim.slots[0])
            if (const CharAnimation *carry = vm.animation("a2o-rarm-carry-loop"))
                sim.carry = std::make_unique<AnimState>(carry, false);
        return Exit::GotoTrue;
    }
    // An id from a parameter names one of the stack object's animations (FreeSO: "fixes MM rollercoaster").
    vm.animation_pending = false;
    const CharAnimation *anim = animation_for(f, id_from_param && source == 0 ? 4 : source, id);
    if (!anim && vm.animation_pending) {
        // Its files are being read ahead: this Sim waits a tick (in its pose), up to half a second;
        // past that, read now as before. Not just after a Snap: the Sim already stands (or sits)
        // where the animation begins, and waiting there showed as a jump into the seat and back
        // (the user's report; the second audit, 2026-11-04): read now, as the exe always does.
        vm.animation_pending = false;
        const bool just_snapped = sim.snapped_at && vm.ticks - sim.snapped_at <= 1;
        if (sim.anim_waits < 12 && !just_snapped) {
            sim.anim_waits++;
            if (TS_ENV("TS_ANIM_WAITS"))   // debug: each tick a Sim waits for an animation's files
                std::printf("tick %u #%d waits for animation %d (source %d), %d\n", unsigned(vm.ticks), int(sim.id), id, source,
                            int(sim.anim_waits));
            return Exit::ContinueNextTick;
        }
        vm.animation_sync = true;
        anim = animation_for(f, id_from_param && source == 0 ? 4 : source, id);
        vm.animation_sync = false;
    }
    sim.anim_waits = 0;
    if (TS_ENV("TS_ANIM_START") && (sim.animations.empty() || sim.animations.front().anim != anim))   // debug
        std::printf("tick %u #%d animate %d (source %d) %s%s mode %d (carry %s%s, hand %s)\n", unsigned(vm.ticks), int(sim.id), id,
                    int(source), anim ? anim->name.c_str() : "(missing)", backwards ? " backwards" : "", mode,
                    sim.carry && sim.carry->anim ? sim.carry->anim->name.c_str() : "-", sim.carry_off ? ", off" : "",
                    !sim.slots.empty() && sim.slots[0] ? "full" : "empty");
    if (anim && TS_ENV("TS_ANIM_BONES") && (sim.animations.empty() || sim.animations.front().anim != anim)) {   // debug
        std::printf("  %s bones:", anim->name.c_str());
        for (const AnimMotion &m : anim->motions)
            if (m.frames > 0)
                std::printf(" %s%s", m.bone.c_str(), m.frames > 1 ? "*" : "");
        std::printf("\n");
    }
    if (anim && TS_ENV("TS_ANIM_MOTION") && (sim.animations.empty() || sim.animations.front().anim != anim))   // debug
        for (const AnimMotion &m : anim->motions)
            if (m.has_translation && m.frames > 0 && m.first_translation + m.frames <= anim->translations.size()) {
                const Vec3 &a = anim->translations[m.first_translation], &b = anim->translations[m.first_translation + m.frames - 1];
                std::printf("  %s %s moves (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)\n", anim->name.c_str(), m.bone.c_str(), a.x, a.y,
                            a.z, b.x, b.y, b.z);
            }
    if (!anim)
        return Exit::GotoTrueNextTick;   // as FreeSO: a missing one waits a tick, so a loop round it can't spin
    if (mode == 1) {   // play looping and carry on (FreeSO)
        sim.animations.clear();
        sim.animations.emplace_back(anim, backwards);
        sim.animations.back().speed = rate;
        sim.animations.back().loop = true;
        sim.animations.back().background = true;
        sim.left_hand = sim.right_hand = 0;
        return Exit::GotoTrue;
    }
    if (mode == 3) {
        sim.carry.reset();
        sim.carry_held_off = anim;   // (no default carry while this plays: the viewer's default_carry)
    }
    if (mode == 0 || mode == 3) {   // play and wait
        AnimState *cur = sim.current_animation();
        // (Running as a loop, it's the standing loop or a background one, not this request's: a pet
        // asking for its stand-still got the loop already playing and waited for its end for ever,
        // Tag's two sides hung, and the Goths' bathroom with them: Bella on the toilet five hours.)
        if (!cur || cur->anim != anim || cur->loop || (cur->end_reached && cur->event_queue.empty() && cur->frame == 0)) {
            sim.animations.clear();
            sim.animations.emplace_back(anim, backwards);
            sim.animations.back().speed = rate;
            sim.left_hand = sim.right_hand = 0;
            return Exit::ContinueNextTick;
        }
        cur->waiting = true;
        // Interruptible (flag bit 3) and something waits that outranks what runs: it ends here,
        // its events still to come handed over first (the original's FUN_005d6380).
        const bool cut_short = (flags & 8) && sim.thread && sim.thread->input_waiting();
        if (cur->end_reached || cut_short)   // events the animation should have had, by number
            while (cur->events_run < expected)
                cur->event_queue.push_back(cur->events_run++);
        if (!cur->event_queue.empty()) {   // events before the end, so none is missed
            const int16_t code = cur->event_queue.front();
            cur->event_queue.pop_front();
            if (store_in_local)
                set(f, Local, local_event, code);
            else
                set(f, Parameters, 0, code);
            if (TS_ENV("TS_ANIMATE"))   // debug: each event handed to the code
                std::printf("tick %u animate #%d %s: event %d (expected %d, run %d, end %d)\n", unsigned(vm.ticks), int(sim.id),
                            anim->name.c_str(), int(code), int(expected), int(cur->events_run), int(cur->end_reached));
            cur->waiting = false;   // off handling it: what fires meanwhile isn't delivered
            return Exit::GotoFalse;
        }
        if (cur->end_reached || cut_short) {
            // Its pose held until the next animation (the original keeps the skeleton as it is).
            sim.held = *cur;
            sim.held.frame = std::clamp(sim.held.frame, 0.0f, std::max(0.0f, cur->frames() - 1));
            sim.held.loop = false;
            sim.held.event_queue.clear();
            sim.animations.clear();
            return Exit::GotoTrue;
        }
        return Exit::ContinueNextTick;
    }
    if (mode == 2) {   // a custom carry pose
        sim.carry = std::make_unique<AnimState>(anim, false);
        sim.carry->speed = rate;
        return Exit::GotoTrue;
    }
    return Exit::GotoTrue;
}

// Change Suit: a person's outfits (scope 1) are numbered slots kept in person
// data 8, which the renderer dresses them by. Scope 2 puts on (flag 1: takes off) a suit named
// in the code's own object's STR# 304 ("Suit primitive strings": the bookcase's book in the
// hand, "suit-book101"), worn over the outfit (FreeSO VMChangeSuitOrAccessory).
Exit change_suit(Frame &f, const uint8_t *op)
{
    const int data = op[0], scope = op[1], flags = u16(op, 2);
    if (scope == 1 && f.caller->person && !(flags & 5))
        f.caller->person_data[8] = int16_t(data);
    if (TS_ENV("TS_SUITS"))   // debug
        std::printf("change suit #%d: scope %d data %d flags %d\n", int(f.caller->id), scope, data, flags);
    if ((scope == 2 || scope == 0) && f.caller->person) {   // 0: global.iff's STR# 304
        const std::vector<std::string> names = f.vm().strings(scope == 2 ? f.owner.own : f.vm().global_code.own, 304);
        if (TS_ENV("TS_SUITS"))   // debug
            std::printf("change suit #%d: %s %d of %lu names (%s)\n", int(f.caller->id), (flags & 1) ? "off" : "on", data,
                        (unsigned long)names.size(), size_t(data) < names.size() ? names[size_t(data)].c_str() : "-");
        if (size_t(data) < names.size() && !names[size_t(data)].empty()) {
            std::vector<std::string> &on = f.caller->accessories;
            const std::string &name = names[size_t(data)];
            auto it = std::find(on.begin(), on.end(), name);
            if (flags & 1) {
                if (it != on.end())
                    on.erase(it);
            } else if (it == on.end()) {
                on.push_back(name);
            }
        }
    }
    return Exit::GotoTrue;
}

Exit look_towards(Frame &f, const uint8_t *op)
{
    Entity *so = f.stack_object();
    const int mode = op[0];
    if (mode == 0) {   // the head towards the stack object (the original's FUN_005d0090: person data 41-45)
        if (f.thread->is_check || !f.caller->person || f.caller->person_data.size() <= 45)
            return Exit::GotoTrue;
        auto &pd = f.caller->person_data;
        pd[41] = int16_t(f.stack_id);
        pd[42] = f.stack_id != 0 ? 1 : 0;
        pd[43] = 0;
        pd[44] = 1;
        pd[45] = 0;
        return Exit::GotoTrue;
    }
    // The body's modes turn over several ticks, with the turn-on-the-spot animation, as the
    // original's (the 2026-10-28 reading: modes 1-3 turn over several ticks); ours snapped.
    // The turn runs under this instruction (route.turning 3), not the walk's.
    auto turn_body = [&](int dir) -> Exit {
        Entity &sim = *f.caller;
        if (f.thread->is_check)
            return Exit::GotoFalse;
        if (sim.route.turning == 3) {   // our turn under way: done when its animation is
            const AnimState *turn = sim.current_animation();
            if (turn && turn->anim && !turn->end_reached)
                return Exit::ContinueNextTick;
            sim.direction = sim.route.turn_to;
            sim.route.turning = 0;
            return Exit::GotoTrue;
        }
        if (sim.direction == dir)
            return Exit::GotoTrue;
        if (!sim.person || sim.route.state == Entity::Route::Walking || !sim.turn_towards(f.vm(), dir, 3)) {
            sim.direction = dir;   // no animation for it (or mid-walk): at once
            return Exit::GotoTrue;
        }
        return Exit::ContinueNextTick;
    };
    // A person not standing (sitting, kneeling, lying) isn't turned: the exe's handler
    // (FUN_005f8fc0) returns false at once when person data 0 isn't 0, before anything else and
    // before a turn starts. Ours turned them on the spot: John at the wrong angle on the sofa
    // after a nap, the seated "jumps" (2026-11-04 audit). (Modes above 3 are the exe's error 0x27;
    // ours still turns for 4 and 5, not checked that no game code asks for them.)
    if (f.caller->person && f.caller->route.turning != 3 && !f.thread->is_check && f.caller->person_data.size() > 0 &&
        f.caller->person_data[0] != 0)
        return Exit::GotoFalse;
    if (mode == 1)   // the body towards the camera (FreeSO BodyTowardsCamera, a no-op in TSO): facing the
                     // screen, which is facing 3 at the first camera angle, one turn less at each next
        return turn_body(((3 - 2 * f.vm().camera_rotation) % 8 + 8) % 8);
    if (mode < 2 || !so || !so->in_world() || !f.caller->in_world())
        return Exit::GotoTrue;
    const float dx = float(so->x16 - f.caller->x16), dy = float(so->y16 - f.caller->y16);
    if (dx == 0 && dy == 0)
        return Exit::GotoTrue;
    int dir = (int(std::lround(std::atan2(dx, -dy) / (3.14159265f / 4))) + 8) % 8;
    if (mode == 3 || mode == 5)
        dir = (dir + 4) % 8;
    return turn_body(dir);
}

// ---------------------------------------------------------------- running other code

// Functions (Find Best Object For Function, Run Functional Tree) to entry points.
constexpr int kFunctionEntry[] = { 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 14, 28, 16, 17, 29 };
// What each function is scored by (the object's own value for it; -1: a fixed 50).
constexpr int kFunctionScore[] = { VarPrepValue, VarCookValue, VarSurfaceValue, VarDisposeValue, -1, -1,
                                   VarWashDishValue, VarEatingSurfaceValue, -1, -1, VarServingSurfaceValue, VarDirtyLevel,
                                   VarGardeningValue, VarWashHandsValue, VarRepairState };

bool passes_entry_check(Frame &f, Entity &obj, int entry)
{
    const uint16_t cond = obj.entry_points[size_t(entry)].condition;
    if (cond == 0)
        return true;
    Vm &vm = f.vm();
    CodeOwner owner;
    const Routine *r = vm.routine(obj.code, cond, &owner);
    if (!r)
        return true;
    Frame check;
    check.routine = r;
    check.caller = f.caller;
    check.callee = &obj;
    check.owner = owner;
    check.stack_id = int16_t(obj.id);
    check.args.assign(4, 0);
    return f.thread->run_now(std::move(check)) == Exit::ReturnTrue;
}

// Whether anyone is using the object (FreeSO VMEntity.IsInUse, multitile): it or one of its parts
// is marked occupied, or a Sim has a frame on their stack whose callee it is (an interaction of it,
// or its sit function on their way to it).
bool object_in_use(const Vm &vm, const Entity &obj)
{
    auto one = [&](const Entity &e) {
        if (e.flag(FlagOccupied))
            return true;
        for (const auto &p : vm.entities)
            if (p->person && !p->dead && p->thread)
                for (const Frame &fr : p->thread->stack)
                    if (fr.callee == &e)
                        return true;
        return false;
    };
    if (one(obj))
        return true;
    for (const Entity *part : obj.group)
        if (part && part != &obj && one(*part))
            return true;
    return false;
}

Exit run_functional_tree(Frame &f, const uint8_t *op)
{
    const int function = u16(op, 0);
    Entity *obj = f.stack_object();
    if (!obj || function < 0 || function >= int(std::size(kFunctionEntry)))
        return Exit::GotoFalse;
    const int entry = kFunctionEntry[function];
    if (size_t(entry) >= obj->entry_points.size() || obj->entry_points[size_t(entry)].action == 0)
        return Exit::GotoFalse;
    if (!passes_entry_check(f, *obj, entry))
        return Exit::GotoFalse;
    Vm &vm = f.vm();
    CodeOwner owner;
    const Routine *r = vm.routine(obj->code, obj->entry_points[size_t(entry)].action, &owner);
    if (!r)
        return Exit::GotoFalse;
    Frame child;
    child.routine = r;
    child.caller = f.caller;
    child.callee = obj;
    child.owner = owner;
    child.stack_id = int16_t(obj->id);
    child.action_tree = f.action_tree;
    child.args.assign(std::max<size_t>(r->args, 4), 0);
    f.thread->push(std::move(child));   // `f` may move: don't touch it after this
    return Exit::Continue;
}

// The best object for a function (prepare food, cook, put down, dispose, ...), as the original's
// (FUN_006175a0): each object with that entry point, not locked out nor burning, is worth its own
// value for the function (sitting and category 4: 50), which its check tree may change (the
// caller's FunctionScore, object data 66, set before and read after), less three per tile away
// (half the distance for cleaning), never under 1. Flag 4: only in my room (or, with flag 8,
// that of the object a variable names). Functions 4, 5 and 9 find nothing. (Ours had FreeSO's
// thresholds, a third of a tile's sixteenths for distance, and a free-slot rule of its own.)
Exit find_best_object_for_function(Frame &f, const uint8_t *op)
{
    // (The two filters of ours stay for now: without them a restaurant guest on lot 81 tried one
    // table 181 times in 30,000 ticks, the survey of 2026-11-04. TS_FBO_EXE=1: the exe's search.)
    static const bool old_rules = TS_ENV("TS_FBO_EXE") == nullptr;
    const int function = u16(op, 0);
    const int flags = op[6];
    if (function < 0 || function >= int(std::size(kFunctionEntry)) || function == 4 || function == 5 || function == 9)
        return Exit::GotoFalse;
    const int entry = kFunctionEntry[function];
    Vm &vm = f.vm();
    Entity &me = *f.caller;
    if (function == 8 && me.person && me.person_data.size() > 65 && me.person_data[65] > 1)
        return Exit::GotoFalse;   // (pets don't sit)
    const Entity *room_of = &me;
    if (flags & 8) {
        room_of = vm.entity(get(f, u16(op, 4), s16(op, 2)));
        if (!room_of)
            return Exit::GotoFalse;
    }
    const uint16_t room = vm.room_at(room_of->level, room_of->tile_x(), room_of->tile_y());
    constexpr int kFunctionScoreVar = 66;
    Entity *best = nullptr;
    float best_score = 0;
    for (size_t i = 0; i < vm.entities.size(); i++) {
        Entity &e = *vm.entities[i];
        if (e.dead || !e.in_world() || e.value(VarLockoutCount) > 0 || e.flag(FlagBurning))
            continue;
        // Flag 1 keeps what's outside, flag 2 what's inside (FUN_006280a0, the game's "IsOutside": a
        // vacation Janitor's beach, its Maid's rooms); else flag 4, the same room as asked.
        if (flags & 3) {
            const uint16_t r = vm.room_at(e.level, e.tile_x(), e.tile_y());
            const bool outside = r < vm.rooms.size() && vm.rooms[r].outside;
            if ((flags & 1) ? (r != 0 && !outside) : (r == 0 || outside))
                continue;
        } else if ((flags & 4) && vm.room_at(e.level, e.tile_x(), e.tile_y()) != room) {
            continue;
        }
        int score;
        if (function == 15) {
            if (e.value(VarCategory) != 4)
                continue;
            score = 50;
        } else {
            if (size_t(entry) >= e.entry_points.size() || e.entry_points[size_t(entry)].action == 0)
                continue;
            score = kFunctionScore[function] >= 0 ? e.value(kFunctionScore[function]) : 50;
            if (score == 0)
                continue;
            if (e.entry_points[size_t(entry)].condition != 0) {
                me.data[kFunctionScoreVar] = int16_t(score);
                if (!passes_entry_check(f, e, entry))
                    continue;
                score = me.data[kFunctionScoreVar];
                if (score == 0)
                    continue;
            } else if (old_rules && (entry == 20 || entry == 25 || entry == 26)) {
                // (FreeSO's SurfaceFunctions: a surface or seat without a check tree free only
                // with nothing in its slot. Not the exe's.)
                if (!e.slots.empty() && e.slots[0])
                    continue;
            }
            // (FreeSO's IsInUse, added 2026-11-02 for the Bobs' seat fight: not the exe's either.)
            if (old_rules && object_in_use(vm, e))
                continue;
            // The exe's search (FUN_006175a0) has neither a free-slot nor an in-use test: its only
            // filters are the lockout, burning, the room, the category, a score and the check tree.
            // The game's own code does the rest: "prepare food" and "do read" test the slot
            // themselves, lock out a taken counter or seat (object data 25) and search again, and a
            // sit that fails comes back as a route failure they retry on. That retry gave up only
            // because our walks left person data 62 at 4 (route_result, 2026-11-04 audit); with it
            // right, these two filters of ours hide objects the original tries; kept for now (see
            // old_rules above), to remove once the restaurant table loop is understood.
        }
        const double dx = (e.x16 - me.x16) / 16.0, dy = (e.y16 - me.y16) / 16.0, dl = (e.level - me.level) * 20.0;
        double distance = std::sqrt(dx * dx + dy * dy + dl * dl);
        if (distance > 1 && function == 11)
            distance *= 0.5;
        const float value = std::max(1.0f, float(score - distance * 3.0));
        if (value > best_score) {
            best_score = value;
            best = &e;
        }
    }
    if (!best)
        return Exit::GotoFalse;
    f.stack_id = int16_t(best->id);
    return Exit::GotoTrue;
}

Exit run_tree_by_name(Frame &f, const uint8_t *op)
{
    Entity *obj = f.stack_object();
    if (!obj)
        return Exit::GotoFalse;
    const uint16_t table = u16(op, 0);
    const int scope = op[2], index = op[4], destination = op[5];
    Vm &vm = f.vm();
    std::vector<std::string> names;
    if (scope == 1)
        names = vm.strings(vm.global_code.own, table);
    else {
        if (f.routine->id >= 8192)
            names = vm.strings(f.owner.semi, table);
        if (names.empty())
            names = vm.strings(f.owner.own, table);
    }
    if (index < 1 || size_t(index) > names.size()) {
        vm.warn(&f, "run tree by name: no name " + std::to_string(index) + " in table " + std::to_string(table));
        return Exit::GotoFalse;
    }
    CodeOwner owner;
    const Routine *tree = obj->tree_by_name(vm, names[size_t(index - 1)], owner);
    if (TS_ENV("TS_TREE_NAMES"))   // debug: each tree asked for by name
        std::printf("tick %u #%d %s: run tree by name \"%s\" on #%d (%s)%s\n", unsigned(vm.ticks), int(f.caller->id),
                    f.routine ? f.routine->name.c_str() : "?", names[size_t(index - 1)].c_str(), int(obj->id),
                    obj->info ? obj->info->label.c_str() : "?", tree ? "" : ": none");
    if (!tree)   // not a problem: the code probes for a tree by name and takes the false branch
        return Exit::GotoFalse;   // (the person's idle asks a chair for "CT - Relax": only sofas have it)
    Frame child;
    child.routine = tree;
    child.callee = obj;
    child.owner = owner;
    child.stack_id = int16_t(obj->id);
    child.args.assign(std::max<size_t>(tree->args, 4), 0);
    for (size_t i = 0; i < 4; i++)
        child.args[i] = f.thread->temps[i];
    if (destination == 2) {   // as a subroutine on my stack
        child.caller = f.caller;
        child.action_tree = f.action_tree;
        f.thread->push(std::move(child));
        return Exit::Continue;
    }
    // Run to completion now on my thread, the temps it leaves kept (FreeSO RunInMyStack): trees
    // answer in them. Ours put them back, so a customer at a bar asking the bar for its bartender
    // ("CT - Get my Bartender in Temp 0") got back what temp 0 held before, a counter, and pushed
    // the order onto the counter: no bartender ever served (2026-10-30). And ours ran a tree whose
    // destination is the stack object on *its* thread, its temps kept changed: a tree run on a Sim
    // mid-interaction (a ghost's, the tank's) zeroed the temp her Feed Fish counted its loops in,
    // and she fed the fish for thirteen hours (2026-10-30). FreeSO runs it on the caller's thread
    // with the stack object as "me".
    Thread *on = f.thread;
    child.caller = destination == 0 ? f.caller : obj;
    return truth(on->run_now(std::move(child), 20000, true) == Exit::ReturnTrue);
}

// Notify Stack Object Out Of Idle: if the stack object is waiting right now in a Sleep or an
// Idle For Input, its count is cut to nothing (the original's case 0x31). Ours kept a latch
// that woke whatever it waited in next, however much later.
Exit notify_out_of_idle(Frame &f, const uint8_t *)
{
    Entity *so = f.stack_object();
    static const bool soft = TS_ENV("TS_SOFT_MISSING") != nullptr;
    if (!so && !soft)
        return Exit::Error;   // (the exe's 0x15: no stack object)
    if (!so || !so->thread || so->thread->stack.empty())
        return Exit::GotoTrue;
    Frame &top = so->thread->stack.back();
    if (top.routine && top.ip < top.routine->code.size()) {
        const BhavInstruction &ins = top.routine->code[top.ip];
        if (ins.opcode == 0 || ins.opcode == 0x11) {
            const int16_t i = int16_t(ins.operand[0] | (ins.operand[1] << 8));
            if (i >= 0 && size_t(i) < top.args.size())
                top.args[size_t(i)] = 0;
            so->thread->sleeping = 0;   // (a sleep sat out by the thread itself ends too)
        }
    }
    return Exit::GotoTrue;
}

// Whether the stack object's code is running for me (or, byte 0 = 1, for the Sim a variable
// names): any frame on the Sim's stack, as the original's list of the objects it is busy with
// (its Sim dispatcher, case 0x25). Ours looked at the first queued action only.
Exit test_sim_interacting_with(Frame &f, const uint8_t *op)
{
    Entity *sim = op[0] == 1 ? f.vm().entity(get(f, op[1], s16(op, 2))) : f.caller;
    if (!sim || !sim->thread)
        return Exit::GotoFalse;
    for (const Frame &fr : sim->thread->stack)
        if (fr.action_tree && fr.callee && fr.callee != sim && fr.callee->id == uint16_t(f.stack_id))   // (not a check tree)
            return Exit::GotoTrue;
    return Exit::GotoFalse;
}

Exit push_interaction(Frame &f, const uint8_t *op)
{
    const int number = op[0], where = op[1], priority_kind = op[2], flags = op[3];
    Vm &vm = f.vm();
    const int16_t source_id = (flags & 2) ? (size_t(where) < f.locals.size() ? f.locals[size_t(where)] : 0)
                                          : (size_t(where) < f.args.size() ? f.args[size_t(where)] : 0);
    Entity *source = vm.entity(source_id);
    Entity *target = f.stack_object();
    if (!source)
        return Exit::GotoFalse;
    QueuedAction a;
    if (!source->action(vm, number, int16_t(source->id), a))   // its stack object is the object itself
        return Exit::GotoFalse;
    int16_t priority = 0;
    switch (priority_kind) {
    case 0:   // the pusher's own: a Sim's running priority (person data 33), else 1
        priority = f.caller->person ? f.caller->person_data[33] : int16_t(1);
        break;
    case 1: priority = PriorityMaximum; break;
    case 2: priority = PriorityAutonomous; break;
    case 3: priority = PriorityUser; break;
    case 4: priority = PriorityParentIdle; a.mode = QueueMode::ParentIdle; break;
    case 5: priority = PriorityParentExit; a.mode = QueueMode::ParentExit; break;
    case 6: priority = PriorityIdle; a.mode = QueueMode::Idle; break;
    default: break;
    }
    a.priority = priority;
    a.must_run = true;
    a.head = (flags & 4) != 0;   // a head continuation: first in the queue, and always input
    if ((flags & 1) && size_t(op[4]) < f.locals.size())   // a custom icon: the object in that local (FreeSims UseCustomIcon)
        a.icon_owner = f.locals[size_t(op[4])];
    if (target == source && target && target->person)
        vm.warn(&f, "pushed " + a.name + " of a Sim onto that Sim (source " + std::to_string(where) + ", flags " +
                        std::to_string(flags) + ")");
    if (TS_ENV("TS_PUSH_LOG") && target)   // who pushed what onto whom
        std::printf("[tick %u] push \"%s\" onto #%d by #%d in %s, priority %d (kind %d, running %d)\n", vm.ticks, a.name.c_str(),
                    target->id, f.caller->id, f.routine->name.c_str(), int(a.priority), priority_kind,
                    f.caller->person ? int(f.caller->person_data[33]) : -1);
    // The exe's queue holds eight waiting entries (a ring, FUN_005f01f0): a push into a full one is
    // false, which the game's code sees; a head continuation still goes in, the last waiting one
    // dropped for it; a run-immediately one doesn't go into the ring. Ours always took it.
    if (target && target->thread && !(a.flags & TtabRunImmediately) && !TS_ENV("TS_QUEUE_NO_LIMIT") &&
        target->thread->waiting_at_least(a.priority) >= 8) {
        if (!a.head)
            return Exit::GotoFalse;
        Thread &t = *target->thread;
        for (size_t i = t.queue.size(); i-- > 0;)
            if (t.queue[i].mode == QueueMode::Normal && !(t.queue[i].flags & TtabRunImmediately)) {
                t.queue.erase(t.queue.begin() + long(i));
                break;
            }
    }
    if (target && target->thread)
        target->thread->enqueue(std::move(a));
    return Exit::GotoTrue;
}

// Idle For Input with the queue: main lets queued interactions start.
Exit idle_for_input(Frame &f, const uint8_t *op)
{
    Thread &t = *f.thread;
    const int allow_push = u16(op, 2);
    if (allow_push == 1 && !f.action_tree && t.attempt_push())
        return Exit::Continue;   // the interaction is on the stack now
    // As the original's (FUN_005f8de0): something waits that outranks what runs (see
    // input_waiting): "notified". Inside an interaction with pushes allowed, a wait that hasn't
    // started anything yet starts the waiting interaction on top of its own (the one below
    // carries on after it), or, if that can't start, waits a tick; only once it has started one,
    // or with pushes not allowed, it's true at once and the interaction's own code (Wait For
    // Notify, "watch finished?") ends itself. Ours was true at once in every interaction: a
    // visitor's pedestrian walk (PedMarkers "Interaction Walk", which waits again until greeted)
    // went round without a tick passing until the engine cut its code off, again and again,
    // leaving the talks it had started hanging (Solana, the stall finder, 2026-10-30).
    // Else the count runs down, and at its end the flag goes and it's true.
    if (f.caller->person ? t.input_waiting() : false) {
        f.caller->set_flag(FlagNotifiedByIdleForInput, true);
        if (allow_push == 1 && f.action_tree && !f.pushed) {
            // (`f` may move once a frame is pushed: set first, undone if nothing starts)
            const int16_t ticks_left = get(f, Parameters, s16(op, 0));
            f.pushed = true;
            set(f, Parameters, s16(op, 0), 0);
            const int nested = t.nest_waiting();
            if (nested > 0)
                return Exit::Continue;   // the waiting interaction is on the stack now
            f.pushed = false;
            set(f, Parameters, s16(op, 0), ticks_left);
            // Nothing that can start waits (only a cancel, or an emergency): true, so the
            // interaction's own code ends itself, as the exe's; ours waited here for good (a
            // cancelled interaction in Wait For Notify froze until something new was queued).
            // One that couldn't start was dropped: the wait goes on next tick.
            return nested == 0 ? Exit::GotoTrue : Exit::ContinueNextTick;
        }
        return Exit::GotoTrue;
    }
    if (!f.caller->person && !t.queue.empty() && !f.action_tree) {   // (objects' own queues)
        f.caller->set_flag(FlagNotifiedByIdleForInput, true);
        return Exit::GotoTrue;
    }
    const int16_t i = s16(op, 0);
    int16_t ticks = get(f, Parameters, i);
    if (ticks <= 0) {
        f.caller->set_flag(FlagNotifiedByIdleForInput, false);
        return Exit::GotoTrue;
    }
    set(f, Parameters, i, int16_t(ticks - 1));
    return Exit::ContinueNextTick;
}

// ---------------------------------------------------------------- objects

void remove_entity(Vm &vm, Entity *e, Frame &f);   // (below)

Exit create_object(Frame &f, const uint8_t *op)
{
    bool bring_guest = false;
    uint32_t guid = u32(op, 0);
    const int where = op[4], local = op[6];
    Vm &vm = f.vm();
    if (op[5] & 4) {   // the Sim of the neighbour whose id is in the stack object (a visitor)
        const Neighbor *n = vm.hood ? vm.hood->by_id(f.stack_id) : nullptr;
        if (!n)
            return Exit::GotoFalse;
        guid = n->guid;
        // A guest for atmosphere (the markers' "Bring a guest not here already"): the Sim density
        // setting's cap on a community lot (Vm::sim_density).
        if (f.routine && !vm.residential()) {
            std::string name = f.routine->name;
            for (char &ch : name)
                ch = char(std::tolower(uint8_t(ch)));
            if (name.find("guest") != std::string::npos) {
                bring_guest = true;
                int guests = 0;
                for (const auto &e : vm.entities)
                    if (e->person && !e->dead && e->guest && vm.on_lot(*e))
                        guests++;
                if (guests >= vm.guest_cap()) {
                    static std::set<std::string> told;   // (once a routine: which routines bring guests)
                    if (told.insert(f.routine->name).second)
                        std::printf("Sim density: no more guests (%d here), %s not brought by \"%s\"\n", guests,
                                    n->name.c_str(), f.routine->name.c_str());
                    return Exit::GotoFalse;
                }
            }
        }
    }
    const ObjectInfo *info = vm.content.object(guid);
    if (!info)
        return Exit::GotoFalse;
    // Its file not read yet (a visitor's character file, a car's): read in the background while the
    // creator waits here, up to a second (24 ticks), then read now as before. Reading it on the spot
    // was most of the Vita's downtown hitches (a 54-114 KB character file, 25 ms off the card, when a
    // pedestrian marker brings a guest). Only where something reads ahead (the viewer);
    // TS_CREATE_READ_NOW=1: on the spot, as before.
    static const bool read_now = TS_ENV("TS_CREATE_READ_NOW") != nullptr;
    if (!read_now && vm.request_file && !f.thread->is_check && !vm.content.loaded(info->ref) && !read_ahead_has(info->ref)) {
        const std::string key = info->ref.path + ":" + std::to_string(info->ref.offset);
        auto [it, fresh] = vm.create_waits.emplace(key, vm.ticks);
        if (fresh)
            vm.request_file(info->ref);
        if (vm.ticks - it->second < 24)
            return Exit::ContinueNextTick;
        vm.create_waits.erase(it);   // (waited long enough: read now)
    } else if (!vm.create_waits.empty()) {
        vm.create_waits.erase(info->ref.path + ":" + std::to_string(info->ref.offset));
    }
    Entity *me = f.caller;
    Entity *so = f.stack_object();
    int tx = -1, ty = -1, level = me->level - 1, dir = 0;
    auto at = [&](const Entity *e) {
        if (e && e->in_world()) {
            tx = e->tile_x();
            ty = e->tile_y();
            level = e->level - 1;
        }
    };
    switch (where) {
    case 0:   // in front of me
    case 3: {  // in front of the stack object
        const Entity *e = where == 0 ? me : so;
        at(e);
        if (e && tx >= 0) {
            dir = e->direction & 6;
            tx += kStep[dir][0];
            ty += kStep[dir][1];
        }
        break;
    }
    case 5:   // underneath me: facing my way (a child's stool, whose picture sits forward
              // of its spot, is then under the feet, as in the original; facing north it was a tile off)
        at(me);
        dir = me->direction & 6;
        break;
    case 7: at(vm.entity(f.args.empty() ? 0 : f.args[0])); break;
    case 8: at(vm.entity(size_t(local) < f.locals.size() ? f.locals[size_t(local)] : 0)); break;
    case 9: {
        at(me);
        const int d = (size_t(local) < f.locals.size() ? f.locals[size_t(local)] : 0) & 6;
        if (tx >= 0) {
            tx += kStep[d][0];
            ty += kStep[d][1];
            dir = d;
        }
        break;
    }
    default: break;   // 1 on top of me and 2 in my hand (both my slot 0, in the original's
                      // FUN_00614700), 4 in the stack object's slot 0, 6 out of the world;
                      // 10 and 11 my slots 2 and 3, 12 to 14 the stack object's slots 2 to 4
    }
    const bool into_slot = where == 1 || where == 2 || where == 4 || (where >= 10 && where <= 14);
    // Beside or on an object that is itself out of the world: nothing is made. The exe (FUN_00614700)
    // takes that object's position, (-16, -16) off the lot, and its bounds test (tiles 1 to size - 2,
    // for every position but 6, "out of the world") fails. Ours made the object out of the world with
    // no test at all: the roaches' controller, finding a dirty dish stranded off the lot, made a swarm
    // "in front of" it, off the lot too, alive and counted (the Granges' save of 2026-11-10 had one,
    // "Roaches #62", beside its three stranded plates), and each such swarm, dirty itself, was the next
    // day's roach-making object. TS_CREATE_OFFWORLD_OLD=1: as before.
    static const bool offworld_old = TS_ENV("TS_CREATE_OFFWORLD_OLD") != nullptr;
    if (!offworld_old && !into_slot && where != 6 && tx < 0 && (where == 0 || where == 3 || where == 5 || where == 7 || where == 8 || where == 9))
        return Exit::GotoFalse;
    if (!into_slot && where != 6 && tx >= 0) {
        // In the world: inside the lot's edge (the original's bounds), in front of the stack
        // object only in its room (unless flag bit 6), and with flag bit 3 only on an empty tile.
        if (tx < 1 || ty < 1 || tx >= vm.lot.size - 1 || ty >= vm.lot.size - 1)
            return Exit::GotoFalse;
        if (where == 3 && so && !(op[5] & 0x40) &&
            vm.room_at(level + 1, tx, ty) != vm.room_at(so->level, so->tile_x(), so->tile_y()))
            return Exit::GotoFalse;
        if (op[5] & 8)
            for (auto &e : vm.entities)
                if (!e->dead && e->in_world() && !e->container && e->tile_x() == tx && e->tile_y() == ty &&
                    e->level == level + 1)
                    return Exit::GotoFalse;
    }
    // No duplicate (flag 1): not if one is already on that tile (ash on ash). The exe compares the
    // objects' definitions (object +0x120 against the created GUID's): a multi-tile object's parts
    // carry their master's, so a flood's "Flood - master" finds the "Flood - tile" already there.
    // Ours compared each part's own GUID, and floods piled up four to a tile (lot 89, 2026-11-06).
    static const bool create_place_old = TS_ENV("TS_CREATE_PLACE_OLD") != nullptr;
    if ((op[5] & 1) && !into_slot && tx >= 0)
        for (auto &e : vm.entities)
            if (!e->dead && e->info && (e->info->guid == guid || (!create_place_old && e->master && e->master->guid == guid)) &&
                e->in_world() && !e->container && e->tile_x() == tx && e->tile_y() == ty && e->level == level + 1)
                return Exit::GotoFalse;
    // Next to me in a direction (9): not through a wall (the exe's FUN_0053b6a0 on the new tile's side
    // facing back, unless flag bit 6). Floods spread through walls.
    if (where == 9 && !create_place_old && !(op[5] & 0x40) && tx >= 0 && level >= 0 && level < kLotLevels &&
        !vm.lot.walls[level].empty()) {
        const int fx = me->tile_x(), fy = me->tile_y();
        bool wall = false;
        if (tx == fx + 1)
            wall = (vm.lot.wall(tx, ty, level).segments & WallTopLeft) || (vm.lot.wall(fx, fy, level).segments & WallBottomRight);
        else if (tx == fx - 1)
            wall = (vm.lot.wall(fx, fy, level).segments & WallTopLeft) || (vm.lot.wall(tx, ty, level).segments & WallBottomRight);
        else if (ty == fy + 1)
            wall = (vm.lot.wall(tx, ty, level).segments & WallTopRight) || (vm.lot.wall(fx, fy, level).segments & WallBottomLeft);
        else if (ty == fy - 1)
            wall = (vm.lot.wall(fx, fy, level).segments & WallTopRight) || (vm.lot.wall(tx, ty, level).segments & WallBottomLeft);
        if (wall)
            return Exit::GotoFalse;
    }
    // Flag 0x20: facing the stack object's way (the exe: a person's whole direction, anything else's
    // in quarter turns, direction & 6); ours never read the flag (248 instructions have it). With the
    // main arguments below (the creator's ids, temp 0), all on by default: the exe's (and FreeSO's).
    // TS_CREATE_EXE: 1 (or "all") every part; else a sum of 1 the facing (0x20), 2 the ids (0x02),
    // 4 temp 0 (0x10), to tell them apart (8 + a sum: exactly those; 8 alone: none).
    static const int create_exe = [] {
        const char *v = TS_ENV("TS_CREATE_EXE");
        if (!v)
            return 7;   // (every part, as the exe, since 2026-11-06. Temp 0 off, the vacation vans' schedulers got
                        // job 0 and each van's Pick Up (person data 61 against its attribute 2, set from that job)
                        // took everyone with 61 = 0, so the island's director, maid and janitor left with the first
                        // tourists' van ~30 game minutes in. With it: the staff stay; survey 81 lots seed 1, failed
                        // walks 5246 -> 5474 (lot 89's flood clean-up, noisy across seeds; lot 45's director boxing
                        // herself in behind the shop's register on some seeds, the game's own wander), starts the
                        // same, false ends -1%. TS_CREATE_EXE=11: the facing and ids only, as before.)
        const int n = std::atoi(v);
        return n >= 8 ? (n & 7) : n <= 1 ? 7 : n;   // (8 + parts: exactly those, 9 the facing alone)
    }();
    if ((op[5] & 0x20) && (create_exe & 1))
        if (const Entity *facing = f.stack_object(); facing && f.stack_id > 0)
            dir = info->objd.get(Objd::ObjectType) == 2 ? int(facing->direction) : int(facing->direction & 6);
    if ((op[5] & 2) && (op[5] & 0x10))
        return Exit::Error;   // (the exe's error 0x36: ids and temp 0 both asked to be passed)
    Entity *obj = vm.create_with_parts(*info, tx, ty, level, dir);
    if (obj && bring_guest)
        obj->guest = true;
    vm.warn(&f, "created " + (info->name.empty() ? info->label : info->name));
    // On top of / underneath me, below an object in a parameter or local: FreeSO takes that
    // one's exact position, not its tile's middle (a child's step stool goes under the feet,
    // not up to half a tile behind them).
    if (obj->group.empty() && (where == 5 || where == 7 || where == 8)) {
        const Entity *at_e = where == 7   ? vm.entity(f.args.empty() ? 0 : f.args[0])
                             : where == 8 ? vm.entity(size_t(local) < f.locals.size() ? f.locals[size_t(local)] : 0)
                                          : me;
        if (at_e && at_e->in_world() && tx >= 0) {
            obj->x16 = at_e->x16;
            obj->y16 = at_e->y16;
        }
    }
    if (TS_ENV("TS_CREATE"))   // debug: where a created object goes
        std::printf("create %s at position %d: tile (%d, %d) level %d dir %d; me #%d at (%.2f, %.2f) facing %d; it at (%.2f, %.2f)\n",
                    info->label.c_str(), where, tx, ty, level + 1, dir, int(me->id), me->x16 / 16.0, me->y16 / 16.0,
                    int(me->direction), obj->x16 / 16.0, obj->y16 / 16.0);
    if (where == 1 || where == 2)
        me->place_in_slot(obj, 0);
    else if (where == 4 && so)
        so->place_in_slot(obj, 0);
    else if (where == 10 || where == 11)
        me->place_in_slot(obj, where - 8);
    else if (where >= 12 && where <= 14 && so)
        so->place_in_slot(obj, where - 10);
    if (obj->group.empty()) {
        obj->run_entry(vm, 0, true);   // init
    } else {
        for (Entity *part : obj->group)
            part->run_entry(vm, 0, true);
    }
    // (After its init, which sets its placement flags: "Init Object", data 42.) Placed only where it may go: the exe tests the new object's placement (its vtable +0x54, the
    // placement test buy mode uses too: walls, floor and water, other objects' footprints and their
    // "allow intersection") for every position but out of the world, and on failure deletes it and
    // the create fails. Ours put it down wherever: floods spread under a scuba tank's parts where
    // nobody could reach them to mop (lot 89, 2026-11-06). In the world on a tile only (positions
    // into a slot keep the slot's own rules); not a person (visitors arrive by their own code).
    // TS_CREATE_PLACE_OLD=1: as before.
    if (obj && !create_place_old && !into_slot && where != 6 && tx >= 0 && !obj->person && obj->in_world()) {
        const std::string why = vm.placement_problem_of(obj, tx, ty, level + 1, dir, nullptr, true);
        if (!why.empty()) {
            if (TS_ENV("TS_CREATE"))   // debug
                std::printf("create %s at position %d: tile (%d, %d) level %d refused: %s (placement flags %04x, floor %d)\n",
                            info->label.c_str(), where, tx, ty, level + 1, why.c_str(), unsigned(uint16_t(obj->value(VarPlacementFlags))),
                            vm.lot.floors[level].empty() ? -1 : int(vm.lot.floor(tx, ty, level)));
            remove_entity(vm, obj, f);
            return Exit::GotoFalse;
        }
    }
    // A neighbour arriving as a visitor comes in their everyday clothes: their record keeps the
    // outfit they were last wearing at home (person data 8, Change Suit's numbered outfit), and
    // Mortimer, asleep in pyjamas when his lot was left, turned up at the Bobs' in them at night
    // (2026-11-02). The original's visitors are dressed for the day.
    if ((op[5] & 4) && obj->person && obj->person_data.size() > 8)
        obj->person_data[8] = 0;
    // A person made in play (the genie, a visitor): what its own main can play, read in meanwhile.
    if (obj->person && obj->entry_points.size() > 1)
        read_ahead_animations(vm, *obj, obj->code, vm.routine(obj->code, obj->entry_points[1].action));
    // What the new object's main is given, as the exe's (flag 0x02: the creator's id as param 0 and
    // the stack object as its stack object; 0x10: temp 0 as param 0). Ours always started main
    // later with nothing, so code that reads who made it or the temp it was handed got 0 (the second
    // audit, 2026-11-04). Its main pushed now, on the lead part; the other parts start theirs as before.
    // (Only when the new object's main has parameters, as the exe's FUN_00614700: FUN_0062dcf0() > 0.
    // A main without any (the vacation director's) got the creator's stack object and walked to
    // itself 900 times on the survey.)
    const Routine *new_main = nullptr;
    if (obj->entry_points.size() > 1 && obj->entry_points[1].action >= 256)
        new_main = vm.routine(obj->code, obj->entry_points[1].action);
    if ((((op[5] & 0x02) && (create_exe & 2)) || ((op[5] & 0x10) && (create_exe & 4))) && obj->thread &&
        obj->thread->stack.empty() && new_main &&
        new_main->args > 0) {
        int16_t args[4] = { 0, 0, 0, 0 };
        int16_t stack = 0;
        if (op[5] & 2) {
            args[0] = int16_t(f.caller->id);
            stack = f.stack_id;
        } else {
            args[0] = f.thread->temps[0];
        }
        if (TS_ENV("TS_CREATE_ARGS"))   // debug
            std::printf("create args: %s main %s param 0 = %d, stack %d (flags %02x, by #%d %s)\n",
                        info->label.c_str(), new_main->name.c_str(), int(args[0]), int(stack), unsigned(op[5]), int(f.caller->id),
                        f.routine->name.c_str());
        obj->run_entry(vm, 1, false, stack, args);
    }
    if (!obj->person)
        vm.dynamic_placed(*obj);   // (a flood beside floods: their pictures join)
    f.stack_id = int16_t(obj->id);   // always, in the original (flag bit 6 keeping it is The Sims Online's)
    return Exit::GotoTrue;
}

void remove_entity(Vm &vm, Entity *e, Frame &f)
{
    if (!e || e->dead)
        return;
    if (!e->group.empty()) {   // a multi-tile object goes whole (FreeSims VMMultitileGroup.Delete)
        const std::vector<Entity *> parts = e->group;
        for (Entity *part : parts)
            if (part != e && !part->dead) {
                part->group.clear();
                remove_entity(vm, part, f);
            }
        e->group.clear();
    }
    // (Its like neighbours' pictures change when it has gone: Vm::dynamic_left below.)
    const bool had_dynamic = !e->person && e->info && e->in_world() && !e->container && e->entry_points.size() > 8 &&
                             e->entry_points[8].action != 0;
    const int left_x = e->tile_x(), left_y = e->tile_y(), left_level = e->level;
    e->leave_container();
    // What it held: out of the world and shown again, then put down where it was only if it was in
    // the world itself (FreeSO VMEntity.Delete: "hidden held objects should unhide on forced drop").
    // Ours left them where they were, still hidden: a plate put into the group meal's pot, the pot
    // washed (the sink's "hide food" takes the pot out of the world and hides both) and removed,
    // came back as an invisible dirty plate on the counter nobody could clean (the Bobs' flies,
    // 2026-11-03; the Goths, seed 1, tick 62784).
    //
    // The objects it held go with it, as the exe's: its removal (FUN_0052bfc0) doesn't queue an object
    // whose holder, or the holder's holder, is already queued, because the holder's removal takes them.
    // The food's code counts on it: washing a stack of dishes hides every dish of the stack, sends the
    // bottom one out of the world ("hide food", Sinks.iff 4106) and removes that one alone. With FreeSO's
    // rule the dishes stacked in it were left out of the world, alive: they spoiled there, their flies
    // buzzed with no way to reach them, and a platter stacked on such a plate stayed drawn on the table
    // with every Clean Up walking to nothing (the Granges' flies, the user's report, 2026-11-10; the
    // Bobs' "flies heard, none seen", 2026-11-05). A person held (sitting, in a car) is never removed
    // this way: out of the slot and put down, as before. TS_REMOVE_KEEPS=1: FreeSO's rule for all.
    static const bool keeps = TS_ENV("TS_REMOVE_KEEPS") != nullptr;
    const bool was_in_world = e->in_world();
    for (Entity *s : std::vector<Entity *>(e->slots)) {
        if (!s)
            continue;
        if (!keeps && !s->person && !s->dead) {
            if (TS_ENV("TS_REMOVALS"))   // debug
                std::printf("tick %u: %s #%d removed with its holder %s #%d\n", unsigned(vm.ticks), s->info ? s->info->label.c_str() : "?",
                            int(s->id), e->info ? e->info->label.c_str() : "?", int(e->id));
            remove_entity(vm, s, f);   // (the caller, if it is one of them, stops after its primitive as ever)
            continue;
        }
        s->leave_container();
        if (s->value(VarHidden) != 0)
            s->set_value(VarHidden, 0);
        if (was_in_world && !TS_ENV("TS_REMOVE_OLD"))
            s->place_whole(e->x16, e->y16, e->level, s->direction);
        else if (!TS_ENV("TS_REMOVE_OLD"))
            for (Entity *part : s->group.empty() ? std::vector<Entity *>{ s } : s->group)
                part->x16 = part->y16 = -1;
    }
    e->dead = true;
    if (had_dynamic)
        vm.dynamic_left(left_x, left_y, left_level, e->info->guid);
    Vm::SoundEvent quiet;   // its sounds end with it
    quiet.source = e->id;
    quiet.stop = true;
    vm.sounds.push_back(quiet);
    if (e != f.caller && e->thread)
        e->thread->stack.clear();   // its own thread stops (the caller's stops after this primitive)
}

Exit remove_object(Frame &f, const uint8_t *op)
{
    // 0 me, 1 the stack object, 2 nothing (false), as the original's case 0x12 (ours removed the stack object for 2).
    if (u16(op, 0) >= 2)
        return Exit::GotoFalse;
    Entity *e = u16(op, 0) == 0 ? f.caller : f.stack_object();
    if (e && TS_ENV("TS_REMOVALS"))   // debug: what removes what
        std::printf("tick %u: %s #%d removed by %s (%s)\n", unsigned(f.vm().ticks), e->info ? e->info->label.c_str() : "?",
                    e->id, f.caller && f.caller->info ? f.caller->info->label.c_str() : "?",
                    f.routine ? f.routine->name.c_str() : "?");
    // Another object's removal waits for the tick's end, as the exe's; the caller's own stays at
    // once (whether the exe's caller goes on with its code this tick hangs on a flag not read yet).
    // Out of the world at once, as the exe's FUN_0052bfc0 calls the object's vtable slot 0x50
    // (FUN_0053bf80): a single-tile object's Pickup entry (10) run, its slots and container
    // emptied, moved off the lot (-16, -16), its room updated; deleted at the tick's end. (Deferred
    // alone, still standing where it was, downtown Sims failed to sit at the cafe's chairs and to
    // pick up at shop counters: the 2026-11-04 survey.) TS_REMOVE_NOW=1: at once, as before.
    static const bool now = TS_ENV("TS_REMOVE_NOW") != nullptr;
    Vm &vm = f.vm();
    if (!now && e && e != f.caller && !e->dead) {
        if (std::find(vm.pending_removals.begin(), vm.pending_removals.end(), e) == vm.pending_removals.end()) {
            vm.pending_removals.push_back(e);
            if (e->group.empty())
                e->run_entry(vm, 10, true);   // Pickup
            const std::vector<Entity *> parts = e->group.empty() ? std::vector<Entity *>{ e } : e->group;
            const bool was_in_world = e->in_world();
            static const bool keeps = TS_ENV("TS_REMOVE_KEEPS") != nullptr;
            for (Entity *part : parts) {
                part->leave_container();
                for (Entity *s : std::vector<Entity *>(part->slots)) {   // (remove_entity's rule)
                    if (!s)
                        continue;
                    if (!keeps && !s->person)   // an object held goes with it, at the tick's end; it stays in the slot till then
                        continue;
                    s->leave_container();
                    if (s->value(VarHidden) != 0)
                        s->set_value(VarHidden, 0);
                    if (was_in_world)
                        s->place_whole(part->x16, part->y16, part->level, s->direction);
                    else
                        for (Entity *sp : s->group.empty() ? std::vector<Entity *>{ s } : s->group)
                            sp->x16 = sp->y16 = -1;
                }
            }
            for (Entity *part : parts) {
                const bool had_dynamic = !part->person && part->info && part->in_world() && part->entry_points.size() > 8 &&
                                         part->entry_points[8].action != 0;
                const int left_x = part->tile_x(), left_y = part->tile_y();
                part->x16 = part->y16 = -1;
                if (had_dynamic)
                    vm.dynamic_left(left_x, left_y, part->level, part->info->guid);
            }
        }
        return Exit::GotoTrue;
    }
    remove_entity(vm, e, f);
    return Exit::GotoTrue;
}

Exit grab(Frame &f, const uint8_t *)
{
    Entity *so = f.stack_object();
    if (!so)
        return Exit::GotoFalse;
    f.caller->place_in_slot(so, 0);
    return Exit::GotoTrue;
}

// Puts what the caller holds down on a free tile around it, in front first.
bool wall_in_way(const Vm &vm, int ax, int ay, int bx, int by, int level);

// Drop: what I hold goes down one step in front of me, from where I stand exactly, keeping its
// own facing, or not at all (the original's primitive switch, case 5: the object's own "can
// place" there, else false, which the caller's code deals with). Ours tried all eight tiles round.
Exit drop(Frame &f, const uint8_t *)
{
    Entity *me = f.caller;
    if (me->slots.empty() || !me->slots[0])
        return Exit::GotoFalse;
    Entity *held = me->slots[0];
    Vm &vm = f.vm();
    const int d = me->direction & 7;
    const int x = me->x16 + kStep[d][0] * 16, y = me->y16 + kStep[d][1] * 16;
    const int tx = x / 16, ty = y / 16;
    if (x < 0 || y < 0 || tx < 1 || ty < 1 || tx >= vm.lot.size - 1 || ty >= vm.lot.size - 1)
        return Exit::GotoFalse;
    if (wall_in_way(vm, me->tile_x(), me->tile_y(), tx, ty, me->level))
        return Exit::GotoFalse;
    for (auto &e : vm.entities)
        if (!e->dead && !e->person && e.get() != held && e->in_world() && !e->container && e->tile_x() == tx &&
            e->tile_y() == ty && e->level == me->level && !(uint16_t(e->value(VarFlags)) & FlagHasZeroExtent))
            return Exit::GotoFalse;
    held->set_position(int16_t(x), int16_t(y), me->level);
    return Exit::GotoTrue;
}

Exit drop_onto(Frame &f, const uint8_t *op)
{
    const int src = u16(op, 0) == 1 ? f.args[size_t(std::min<int>(u16(op, 2), 3))] : u16(op, 2);
    const int dest = u16(op, 4) == 1 ? f.args[size_t(std::min<int>(u16(op, 6), 3))] : u16(op, 6);
    Entity *so = f.stack_object();
    if (!so || src < 0 || size_t(src) >= f.caller->slots.size() || !f.caller->slots[size_t(src)])
        return Exit::GotoFalse;
    if (dest >= 0 && size_t(dest) < so->slots.size() && so->slots[size_t(dest)])
        return Exit::GotoFalse;
    so->place_in_slot(f.caller->slots[size_t(src)], dest);
    return Exit::GotoTrue;
}

// Reach: a reach animation in the game; here the pick-up / put-down happens at once.
Exit reach(Frame &f, const uint8_t *op)
{
    const int mode = u16(op, 0);
    Entity *so = f.stack_object();
    if (!so)
        return Exit::GotoFalse;
    if (mode == 0) {
        if (so->container != f.caller)
            f.caller->place_in_slot(so, 0);
        return Exit::GotoTrue;
    }
    if (mode == 1) {
        const int param = u16(op, 2);
        const int slot = size_t(param) < f.args.size() ? f.args[size_t(param)] : 0;
        if (!f.caller->slots.empty() && f.caller->slots[0])
            so->place_in_slot(f.caller->slots[0], slot);
        return Exit::GotoTrue;
    }
    return Exit::GotoFalse;
}

Exit find_location_for(Frame &f, const uint8_t *op)
{
    const int mode = op[0], local = op[1], flags = op[2];
    Vm &vm = f.vm();
    Entity *obj = f.stack_object();
    if (obj && mode == 1) {   // out of the world, every part (no reference needed)
        for (Entity *part : obj->group.empty() ? std::vector<Entity *>{ obj } : obj->group) {
            part->leave_container();
            part->x16 = part->y16 = -1;
        }
        return Exit::GotoTrue;
    }
    Entity *ref = (flags & 1) ? vm.entity(size_t(local) < f.locals.size() ? f.locals[size_t(local)] : 0) : f.caller;
    if (mode == 2 && !ref)   // the smoke cloud's local isn't set yet when it asks
        ref = f.caller;
    if (!obj || !ref)
        return Exit::GotoFalse;
    if (TS_ENV("TS_FIND"))
        std::printf("find location for #%d (%s, facing %d) mode %d near #%d (%s at %d,%d facing %d) in %s of #%d\n", int(obj->id),
                    obj->info ? obj->info->label.c_str() : "?", int(obj->direction), mode, int(ref->id),
                    ref->info ? ref->info->label.c_str() : "?", ref->tile_x(), ref->tile_y(), int(ref->direction),
                    f.routine ? f.routine->name.c_str() : "?", int(f.caller->id));
    // Where the object fits: every part (a 3x8 bus, not just its lead tile) on the lot
    // and clear of other objects' footprints (things with none, like portals and
    // markers, don't count; FreeSO tests each spot with SetPosition).
    const std::vector<Entity *> parts = obj->group.empty() ? std::vector<Entity *>{ obj } : obj->group;
    auto own = [&](const Entity *e) { return std::find(parts.begin(), parts.end(), e) != parts.end(); };
    // Vehicles (modes 3 and 4, along a car portal) go on the street, which lies past the lot's
    // own size in the 64x64 arrays (House05: size 52, its car portal at y 53).
    const int limit = mode == 3 || mode == 4 ? kLotArraySize : vm.lot.size;
    auto fits = [&]() {
        for (const Entity *p : parts) {
            const int px = p->tile_x(), py = p->tile_y();
            if (px < 1 || py < 1 || px >= limit - 1 || py >= limit - 1) {
                if (TS_ENV("TS_FIND"))
                    std::printf("  find location: part at (%d, %d) off the lot\n", px, py);
                return false;
            }
            // Upstairs, not in the air: a tile with no floor takes only what may go there (placement
            // flag 9) or sits a storey up by its own (level offset), as the exe's placement test that
            // tries each spot (FloorChangeValid; ours, buy mode's "There's no floor there"). Ours
            // didn't look: lot 75's cleaner, at the top of the spiral stair, had her "Wander" marker
            // put beyond the railing in mid-air and failed to reach it, and every walk after it
            // (the survey, 2026-11-06). TS_FIND_AIR_OLD=1: as before.
            static const bool air_old = TS_ENV("TS_FIND_AIR_OLD") != nullptr;
            if (!air_old && p->level >= 2 && p->level <= kLotLevels && !vm.lot.floors[p->level - 1].empty() &&
                vm.lot.floor(px, py, p->level - 1) == 0 && !(p->value(VarPlacementFlags) & (1 << 9)) &&
                (p->info ? int16_t(p->info->objd.get(Objd::LevelOffset)) : 0) == 0) {
                if (TS_ENV("TS_FIND"))
                    std::printf("  find location: (%d, %d) level %d has no floor\n", px, py, int(p->level));
                return false;
            }
            // No wall (a fence) between this tile and a neighbouring part, nor across it (FreeSO: walls in the way).
            const int l = p->level - 1;
            if (l >= 0 && l < kLotLevels && !vm.lot.walls[l].empty()) {
                const WallTile &w = vm.lot.wall(px, py, l);
                if (w.segments & (WallHorizontalDiag | WallVerticalDiag))
                    return false;
                for (const Entity *q : parts) {
                    if (q->level != p->level)
                        continue;
                    const int qx = q->tile_x(), qy = q->tile_y();
                    if ((qx == px - 1 && qy == py && (w.segments & WallTopLeft)) ||
                        (qx == px && qy == py - 1 && (w.segments & WallTopRight)) ||
                        (qx == px + 1 && qy == py && (w.segments & WallBottomRight)) ||
                        (qx == px && qy == py + 1 && (w.segments & WallBottomLeft)))
                        return false;
                }
            }
            for (auto &e : vm.entities) {
                // A Sim takes the tile too, unless the object goes right on its reference (mode 2:
                // the ghost, the fight's cloud) or may overlap (flag 2): the genie's pot of gold, its
                // search starting on the wisher's own tile, came out inside them (2026-11-03).
                const bool sim_blocks = e->person && mode != 2 && !(flags & 2);
                if (e->dead || own(e.get()) || (e->person && !sim_blocks) || !e->in_world() || e->container ||
                    e->tile_x() != px || e->tile_y() != py || e->level != p->level)
                    continue;
                // Only things with no extent at all (markers, portals) leave the tile free: a door
                // can be walked through but nothing may be put on its tile (a parcel left on the
                // front door's tile shut the family out of their house).
                if (uint16_t(e->value(VarFlags)) & FlagHasZeroExtent)
                    continue;
                if (TS_ENV("TS_FIND"))
                    std::printf("  find location: (%d, %d) taken by %s #%d\n", px, py,
                                e->info ? e->info->label.c_str() : "?", e->id);
                return false;
            }
        }
        return true;
    };
    const bool was_placed = obj->in_world();
    const int16_t was_x = obj->x16, was_y = obj->y16;
    const int was_level = obj->level, was_dir = obj->direction;
    // Flag 4 (FreeSO's UserEditableTilesOnly): not on a tile marked as not the lot's own ground,
    // the road and pavement (the tile flags' bit 0x20, the exe's lot +0x34; FUN_0053a800 reads it
    // unless the move-objects cheat is on). Ours never tested it, and on a lot whose road runs
    // inside its size (the Bobs', House06, 43 tiles, the car portal at (36, 28)) a family coming
    // home got out of the car in the middle of the road: "Go To Community" puts each by the car
    // with flag 4 ("find good location for me"), the user's report 2026-11-06. TS_FIND_EDITABLE_OLD=1:
    // as before.
    static const bool editable_old = TS_ENV("TS_FIND_EDITABLE_OLD") != nullptr;
    const bool own_ground_only = (flags & 4) && !editable_old;
    auto try_at = [&](int tx, int ty, int dir) {
        if (tx < 1 || ty < 1 || tx >= limit - 1 || ty >= limit - 1)
            return false;
        if (own_ground_only) {
            const int l = ref->level - 1;
            if (l >= 0 && l < kLotLevels && !vm.lot.tile_flags[l].empty() &&
                (vm.lot.tile_flags[l][size_t(tx) + size_t(ty) * kLotArraySize] & 0x20))
                return false;
        }
        obj->place_whole(int16_t(tx * 16 + 8), int16_t(ty * 16 + 8), ref->level, dir);
        return fits();
    };
    const int rx = ref->tile_x(), ry = ref->tile_y();
    if (mode == 2) {   // right on the reference, turned by its facing (the ghost, the fight's smoke: FUN_00617980)
        const int quarters = ((((ref->direction + 5) & 7) - obj->direction + 8) & 7) / 2;
        obj->place_whole(ref->x16, ref->y16, ref->level, (obj->direction + quarters * 2) & 7);
        if (fits())
            return Exit::GotoTrue;
    } else if (mode == 3 || mode == 4) {   // along the reference's vector (cars onto the street), facing its way
        // FreeSO steps (0, +1) for north in its own tile axes, whose y runs the other way
        // to ours: here that is the way the reference faces (a car portal faces the road).
        const int dir = mode == 4 ? (ref->direction + 2) % 8 : ref->direction % 8;
        for (int i = 0; i < 32; i++) {
            const int k = i / 2;
            if (try_at(rx + kStep[dir][0] * k, ry + kStep[dir][1] * k, dir))
                return Exit::GotoTrue;
            if ((i % 2) && try_at(rx - kStep[dir][0] * k, ry - kStep[dir][1] * k, dir))
                return Exit::GotoTrue;
        }
    } else {
        // Rings round the reference, the object keeping its own facing, first only in the
        // reference's room, then anywhere (the original's FUN_006179e0 / FUN_0053a800; FreeSO's
        // turned it to each facing, north first, so most things put down faced north).
        const int facing = obj->direction;
        // Mode 5 (Wander, the ghost): from a random tile within five of the reference (FUN_0053a800,
        // flag at +0x17). Ours took any tile on the lot: Sims crossed the lot to stand about.
        int cx = rx, cy = ry;
        if (mode == 5)
            for (int i = 0; i < 64; i++) {
                vm.random_state = vm.random_state * 1103515245u + 12345u;
                const int nx = rx + int((vm.random_state >> 8) % 10u) - 5;
                vm.random_state = vm.random_state * 1103515245u + 12345u;
                const int ny = ry + int((vm.random_state >> 8) % 10u) - 5;
                if (nx >= 1 && ny >= 1 && nx < limit - 1 && ny < limit - 1) {
                    cx = nx, cy = ny;
                    break;
                }
            }
        const uint16_t ref_room = vm.room_at(ref->level, cx, cy);
        // The exe's order (FUN_0053a800 with no direction, as FUN_00610540 sets it up): one spiral
        // from the middle, west first, then round (-1, 0) (-1, 1) (0, 1) (1, 1) (1, 0) (1, -1)
        // (0, -1) (-1, -1) (-2, -1) (-2, 0) ..., over the whole lot (4096 steps, or until it has
        // been off the lot eight times the lot's size in a row). Ours went ring by ring, each from
        // its north-west corner, ten rings then twenty: a register clerk came out at the end of
        // the counter instead of behind it. TS_FIND_RINGS_OLD=1: as before.
        static const bool rings_old = TS_ENV("TS_FIND_RINGS_OLD") != nullptr;
        for (int pass = 0; pass < 2 && !rings_old; pass++) {
            int sx = 0, sy = 0, off = 0;
            for (int step = 0; step < 4096 && off < vm.lot.size * 8; step++) {
                const int tx = cx + sx, ty = cy + sy;
                if (tx < 1 || ty < 1 || tx >= limit - 1 || ty >= limit - 1) {
                    off++;
                } else {
                    off = 0;
                    if ((pass == 1 || vm.room_at(ref->level, tx, ty) == ref_room) && try_at(tx, ty, facing))
                        return Exit::GotoTrue;
                }
                if (-sx < sy) {
                    if (sx < sy)
                        sx += 1;   // (sy < -sx can't hold here)
                    else
                        sy -= 1;
                } else if (sx < sy) {
                    if (sy < -sx)
                        sy += 1;
                    else
                        sx += 1;
                } else {
                    sx -= 1;
                }
            }
        }
        for (int pass = 0; pass < 2 && rings_old; pass++) {
            auto try_here = [&](int tx, int ty) {
                if (pass == 0 && vm.room_at(ref->level, tx, ty) != ref_room)
                    return false;
                return try_at(tx, ty, facing);
            };
            for (int i = 0; i < (pass == 0 ? 10 : 20); i++) {
                if (i == 0) {
                    if (try_here(cx, cy))
                        return Exit::GotoTrue;
                    continue;
                }
                for (int x = -i; x <= i; x++)
                    for (int side = -1; side <= 1; side += 2)
                        if (try_here(cx + x, cy + side * i))
                            return Exit::GotoTrue;
                for (int y = 1 - i; y < i; y++)
                    for (int side = -1; side <= 1; side += 2)
                        if (try_here(cx + side * i, cy + y))
                            return Exit::GotoTrue;
            }
        }
    }
    // Nowhere: it stays where it was (the exe tests each spot before moving anything); new, that's
    // out of the world, and the caller's false branch deals with it.
    if (was_placed)
        obj->place_whole(was_x, was_y, was_level, was_dir);
    else
        for (Entity *p : parts)
            p->x16 = p->y16 = -1;
    return Exit::GotoFalse;
}

// ---------------------------------------------------------------- walking

// A wall (or fence) on the edge between two neighbouring tiles, or across either.
bool wall_between(const Vm &vm, int ax, int ay, int bx, int by, int level)
{
    const int l = level - 1;
    if (l < 0 || l >= kLotLevels || vm.lot.walls[l].empty())
        return false;
    auto at = [&](int x, int y) -> uint8_t {
        return x >= 0 && y >= 0 && x < kLotArraySize && y < kLotArraySize ? vm.lot.wall(x, y, l).segments : uint8_t(0);
    };
    if ((at(ax, ay) | at(bx, by)) & (WallHorizontalDiag | WallVerticalDiag))
        return true;
    if (bx == ax + 1 && by == ay)
        return (at(ax, ay) & WallBottomRight) || (at(bx, by) & WallTopLeft);
    if (bx == ax - 1 && by == ay)
        return (at(ax, ay) & WallTopLeft) || (at(bx, by) & WallBottomRight);
    if (by == ay + 1 && bx == ax)
        return (at(ax, ay) & WallBottomLeft) || (at(bx, by) & WallTopRight);
    if (by == ay - 1 && bx == ax)
        return (at(ax, ay) & WallTopRight) || (at(bx, by) & WallBottomLeft);
    return false;
}

// Whether an object stands on a tile, so that nobody can stand there (FreeSO's slot parser
// drops such spots): one the router keeps people off (not a rug, a door, a marker).
bool tile_taken(const Vm &vm, int tx, int ty, int level, const Entity *walker, const Entity *target = nullptr)
{
    for (const auto &e : vm.entities)
        if (!e->dead && !e->person && e.get() != walker && e->in_world() && !e->container && e->level == level &&
            e->tile_x() == tx && e->tile_y() == ty && blocks_walking(*e)) {
            // The object's own other tiles aren't in the way of its own slots (a bubble blower's
            // middle is stood in from its own ring; the router ignores the target's parts too).
            if (target && target != e.get() &&
                std::find(target->group.begin(), target->group.end(), e.get()) != target->group.end())
                continue;
            return true;
        }
    return false;
}

// Whether a wall stands between tile (ax, ay) and tile (bx, by) on the way from one to the
// other (FreeSO's slot parser rejects spots a wall separates from the object): a diagonal
// step is blocked when both ways round its corner are.
bool wall_in_way(const Vm &vm, int ax, int ay, int bx, int by, int level)
{
    int x = ax, y = ay;
    for (int guard = 0; guard < 8 && (x != bx || y != by); guard++) {
        const int sx = (bx > x) - (bx < x), sy = (by > y) - (by < y);
        if (sx && sy) {
            const bool via_x = wall_between(vm, x, y, x + sx, y, level) || wall_between(vm, x + sx, y, x + sx, y + sy, level);
            const bool via_y = wall_between(vm, x, y, x, y + sy, level) || wall_between(vm, x, y + sy, x + sx, y + sy, level);
            if (via_x && via_y)
                return true;
        } else if (wall_between(vm, x, y, x + sx, y + sy, level)) {
            return true;
        }
        x += sx;
        y += sy;
    }
    return false;
}

// Another Sim standing on the spot (x, y) of `level`, within half a tile's third: the original's
// routing fails there ("destination occupied", its spot check tests Sims' footprints) and the
// object's code gives up. Only where they stand: ours also counted Sims merely heading there,
// so the second of two Sims using a door failed the door's own walk and went through it shut
// (the user saw Bella walk through a closed door; 96 of 126 door uses in a half day). Two Sims
// heading for one spot (three Goths at one easel) now meet on arrival: the later one is
// blocked, asks, waits and gives up, as in the original.
Entity *spot_taken(const Vm &vm, const Entity &walker, int16_t x, int16_t y, int level)
{
    const Footprint mine = person_box(walker, x, y);   // (less the walker's routing footprint, person data 73)
    for (auto &up : vm.entities) {
        Entity &e = *up;
        if (&e == &walker || !e.person || e.dead || !e.in_world() || e.container || e.level != level)
            continue;
        if (mine.overlaps(person_box(e, e.x16, e.y16)))
            return &e;
    }
    return nullptr;
}

// The spots round `obj` a routing slot offers, scored, and the one walked to, as the original
// does it (FUN_0060d440 makes the goals, FUN_0060e2b0 checks each, FUN_0060e690 plans to the
// best of them): every spot on the rings of the slot's band (min to max proximity, a ring every
// `resolution` sixteenths) in each of its directions is a goal, scored 100 + the stand (or sit)
// weight - its distance off the optimal x the gradient, +25 if no wall is beside the object that
// way, +20 for where the Sim already stands, a little at random with RandomScoring; then the one
// with the least walking plus 16 a point below the best score (a point is worth a tile's walk).
// Seats (a slot that allows sitting): a chair beside a spot, facing the object square on, taken
// instead with the sitting weight. (Ours took the nearest free spot at the optimal distance.)
// Whether a person standing at (px, py) (sixteenths) would have their box across a wall: the exe's
// placement test for a Sim (FUN_00641430, its error 7), its box (person_box: 3/16 either way, less
// the routing footprint's insets), each tile it covers
// read for its own top-left (the line x = tx*16), top-right (y = ty*16) and diagonals (x + y and
// x - y), styles 3, 5, 6, 15 and 23 not counting; strictly across, so a box touching the line
// passes. Ours never tested it: slot spots on a wall line (John standing in the wall, the user's
// 2026-11-04 report).
bool box_hits_wall(const Vm &vm, const Footprint &box, int level)
{
    const int l = level - 1;   // (storeys from 1, the walls' array from 0)
    if (l < 0 || l >= kLotLevels || vm.lot.walls[l].empty())
        return false;
    const int x0 = box.x0, x1 = box.x1, y0 = box.y0, y1 = box.y1;
    auto counts = [](uint16_t style) { return style != 3 && style != 5 && style != 6 && style != 15 && style != 23; };
    for (int ty = y0 >> 4; ty <= (y1 - 1) >> 4; ty++)
        for (int tx = x0 >> 4; tx <= (x1 - 1) >> 4; tx++) {
            if (tx < 0 || ty < 0 || tx >= kLotArraySize || ty >= kLotArraySize)
                continue;
            const WallTile &w = vm.lot.wall(tx, ty, l);
            if (!w.segments)
                continue;
            if ((w.segments & WallTopLeft) && counts(w.top_left_style) && x0 < tx * 16 && tx * 16 < x1)
                return true;
            if ((w.segments & WallTopRight) && counts(w.top_right_style) && y0 < ty * 16 && ty * 16 < y1)
                return true;
            if ((w.segments & WallHorizontalDiag) && counts(w.top_right_style)) {
                const int c = (tx + ty + 1) * 16;
                if (x0 + y0 < c && c < x1 + y1)
                    return true;
            }
            if ((w.segments & WallVerticalDiag) && counts(w.top_right_style)) {
                const int c = (tx - ty) * 16;
                if (x0 - y1 < c && c < x1 - y0)
                    return true;
            }
        }
    return false;
}

bool slot_spot_scored(const Entity &obj, Entity &walker, const SlotItem &slot, int16_t &x, int16_t &y, int &face,
                      int *entry, Vm &vm, Entity **seat, Entity **in_way = nullptr,
                      const std::vector<std::pair<int16_t, int16_t>> *avoid = nullptr, int *fail_code = nullptr,
                      Entity **fail_blocker = nullptr)
{
    constexpr int kAbsolute = 0x200, kIgnoreRooms = 0x800, kRandomScoring = 0x2000, kAverage = 0x10000;
    const int dirs = slot.flags & 255;
    // The origin: the object, or the middle of all its parts.
    double ox = obj.x16, oy = obj.y16;
    if ((slot.flags & kAverage) && !obj.group.empty()) {
        ox = oy = 0;
        for (const Entity *p : obj.group) {
            ox += p->x16;
            oy += p->y16;
        }
        ox /= double(obj.group.size());
        oy /= double(obj.group.size());
    }
    const int facing = (slot.flags & kAbsolute) ? 0 : (obj.direction & 7);
    const int stand_w = std::max(slot.standing, slot.ground) * 10;
    const int sit_w = (walker.person && walker.person_data.size() > 65 && walker.person_data[65] >= 2) ? 0 : slot.sitting * 10;
    const bool in_wall = (uint16_t(obj.value(VarFlagField2)) & 0x8000) != 0;   // a door: its own spots aren't past a wall
    const int otx = obj.tile_x(), oty = obj.tile_y();
    const uint16_t obj_room = vm.room_at(obj.level, otx, oty);
    auto rotate = [&](double lx, double ly, int sector, int16_t &wx, int16_t &wy) {
        const double t = sector * 3.14159265358979 / 4;
        const double rx = lx * std::cos(t) - ly * std::sin(t), ry = lx * std::sin(t) + ly * std::cos(t);
        wx = int16_t(std::lround(ox + rx));
        wy = int16_t(std::lround(oy + ry));
    };
    auto face_for = [&](int16_t px, int16_t py) {
        if (slot.facing >= 0)
            return (slot.facing + facing) % 8;
        if (slot.facing == -3)
            return -1;
        const double dx = obj.x16 - px, dy = obj.y16 - py;
        if (dx == 0 && dy == 0)
            return int(obj.direction);
        const int to = (int(std::lround(std::atan2(dx, -dy) / (3.14159265358979 / 4))) + 8) % 8;
        return slot.facing == -1 ? (to + 4) % 8 : to;
    };
    auto wall_side = [&](int sector) {   // a wall on the object's own tile that way
        return wall_in_way(vm, otx, oty, otx + kStep[sector & 7][0], oty + kStep[sector & 7][1], obj.level);
    };
    struct Goal {
        int16_t x, y;
        int score, entry;
        Entity *chair;
    };
    std::vector<Goal> goals;
    int why[6] = {};   // (debug) spots turned down: off the lot, another room, a wall, an object, a Sim, a wall on the object's side
    const Entity *seat_taker = nullptr;   // a seat that would have done, taken by someone else (failure 9)
    // The storey's things by tile, once a call (in the entities' order): each spot checked against
    // every entity on the lot, and a chair looked for among them on nine tiles, was 25-35 ms a
    // plan on the Vita at Solana (650 objects; the chef's every few seconds).
    struct ByTile {
        std::vector<const Entity *> objects, people;
    };
    std::vector<ByTile> by_tile(size_t(kLotArraySize) * size_t(kLotArraySize));
    for (const auto &e : vm.entities) {
        if (e->dead || !e->in_world() || e->container || e->level != obj.level)
            continue;
        const int ex = e->tile_x(), ey = e->tile_y();
        if (ex < 0 || ey < 0 || ex >= kLotArraySize || ey >= kLotArraySize)
            continue;
        ByTile &t = by_tile[size_t(ex) + size_t(ey) * kLotArraySize];
        (e->person ? t.people : t.objects).push_back(e.get());
    }
    auto on_tile = [&](int tx, int ty) -> const ByTile * {
        return tx < 0 || ty < 0 || tx >= kLotArraySize || ty >= kLotArraySize ? nullptr
                                                                             : &by_tile[size_t(tx) + size_t(ty) * kLotArraySize];
    };
    // The object's own other parts: in the way only where the Sim's box at the spot overlaps the part's
    // footprint (FreeSO's GetAvatarPlace tests every object's footprint, the slot's own object's parts
    // too). Ours let them all be stood in (whole tiles counted, the bubble blower's ring and the porch
    // swing had failed, 2026-10-27): a booth table's diagonal serving spot inside the booth's own
    // "dead piece", the waitress walked through the seat to it (lot 26, the user's report 2026-11-06).
    // TS_OWN_PARTS_OLD=1: as before.
    // (A part whose "allow intersection", entry 5, says yes for the Sim, or the Sim's own for it, isn't in
    // the way, as FreeSO's test runs them both: the bubble blower's ring parts, stood in to blow.)
    static const bool own_parts_old = TS_ENV("TS_OWN_PARTS_OLD") != nullptr;
    auto lets_stand = [&](const Entity &part) {
        Entity &p = const_cast<Entity &>(part);
        const int16_t to_part[4] = { int16_t(walker.id), 0, 0, 0 }, to_sim[4] = { int16_t(part.id), 0, 0, 0 };
        // (The other one is the stack object as well as parameter 0, as FreeSO's ExecuteEntryPoint(5, .., obj, ..):
        // the blower's asks "so def 9 == 2", whether the stack object is a person.)
        return (p.entry_points.size() > 5 && p.entry_points[5].action && p.run_entry(vm, 5, true, int16_t(walker.id), to_part)) ||
               (walker.entry_points.size() > 5 && walker.entry_points[5].action &&
                walker.run_entry(vm, 5, true, int16_t(part.id), to_sim));
    };
    auto taken_here = [&](int tx, int ty, int16_t px = -1, int16_t py = -1) {   // as tile_taken, from the table
        const ByTile *t = on_tile(tx, ty);
        if (!t)
            return false;
        for (const Entity *e : t->objects)
            if (e != &walker && blocks_walking(*e)) {
                if (&obj != e && std::find(obj.group.begin(), obj.group.end(), e) != obj.group.end() &&
                    (own_parts_old || px < 0 || !walker.person ||
                     !person_box(walker, px, py).overlaps(footprint(*e, e->x16, e->y16, e->direction)) || lets_stand(*e)))
                    continue;   // the object's own other tiles aren't in the way of its own slots (but where it stands)
                if (TS_ENV("TS_SLOT") && px >= 0)   // debug
                    std::printf("  spot (%.2f, %.2f) of #%d: #%d (%s) in the way\n", px / 16.0, py / 16.0, int(obj.id), int(e->id),
                                e->info ? e->info->label.c_str() : "?");
                if (fail_blocker)
                    *fail_blocker = const_cast<Entity *>(e);
                return true;
            }
        return false;
    };
    // The wall test for a spot, as the exe's (FUN_0060e2b0 / FUN_0053b6a0): the spot's own tile, its
    // edge back towards the object only; the route plans round anything between. Ours cast a line
    // from the object to the spot (FreeSims), so a far spot past any wall or fence was refused: a
    // vacation director's slot (4-15 tiles out) found none of 221 (the second audit's survey).
    // TS_WALL_RAYCAST=1: the line, as before.
    static const bool raycast = TS_ENV("TS_WALL_RAYCAST") != nullptr;
    auto wall_back = [&](int tx, int ty) {
        if (raycast)
            return wall_in_way(vm, otx, oty, tx, ty, obj.level);
        const int ux = (otx > tx) - (otx < tx), uy = (oty > ty) - (oty < ty);
        return (ux || uy) && wall_between(vm, tx, ty, tx + ux, ty + uy, obj.level);
    };
    auto spot_ok = [&](int16_t px, int16_t py) {
        const int tx = px / 16, ty = py / 16;
        if (px < 0 || py < 0 || tx < 1 || ty < 1 || tx >= vm.lot.size - 1 || ty >= vm.lot.size - 1)
            return why[0]++, false;
        if (!(slot.flags & kIgnoreRooms) && vm.room_at(obj.level, tx, ty) != obj_room)
            return (!in_wall && wall_back(tx, ty) ? why[2]++ : why[1]++), false;
        // (A slot that ignores rooms, IgnoreRooms 0x800, skips the wall test too, as the exe's
        // FUN_0060e2b0; ours still raycast it.)
        if (!(slot.flags & kIgnoreRooms) && !in_wall && wall_back(tx, ty))
            return why[2]++, false;
        // A person's box across a wall there (the exe's placement test, any slot).
        // A door's slots too (in_wall spares them the tests above, not this one): its "clear door"
        // slot 8 put a Sim one tile beside the door, flush against the wall, at the edge of the tile
        // (Linda standing in the bathroom's wall, the user's 2026-11-05 screenshot; a survey of all lots
        // found Tiffany on lot 63 the same way). TS_BOX_DOORS_OLD=1: doors' slots spared, as before.
        static const bool no_box = TS_ENV("TS_NO_BOX") != nullptr;   // debug: without the box test
        static const bool box_doors_old = TS_ENV("TS_BOX_DOORS_OLD") != nullptr;
        if (walker.person && (!in_wall || !box_doors_old) && !no_box && box_hits_wall(vm, person_box(walker, px, py), obj.level)) {
            if (TS_ENV("TS_BOX_LOG"))   // debug
                std::printf("box: spot (%.2f, %.2f) of #%d for #%d across a wall\n", px / 16.0, py / 16.0, int(obj.id),
                            int(walker.id));
            return why[2]++, false;
        }
        if (taken_here(tx, ty, px, py))
            return why[3]++, false;
        // A Sim standing there now (the original tests Sims' footprints, not where they're going):
        // on this tile or one beside it (the first in the entities' order, as before).
        const Entity *standing = nullptr;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (const ByTile *t = on_tile(tx + dx, ty + dy))
                    for (const Entity *e : t->people)
                        if (e != &walker && person_box(walker, px, py).overlaps(person_box(*e, e->x16, e->y16)) &&
                            (!standing || e->id < standing->id))
                            standing = e;
        if (standing) {
            if (in_way)
                *in_way = const_cast<Entity *>(standing);
            if (TS_ENV("TS_SLOT"))   // debug
                std::printf("  spot (%.2f, %.2f) for #%d: #%d stands there\n", px / 16.0, py / 16.0, int(walker.id),
                            int(standing->id));
            return why[4]++, false;
        }
        // Another Sim standing on the spot's tile, further off than its footprint: the original walks
        // round them in sixteenths; our planner keeps whole tiles and leaves the goal's tile open, so the
        // way went straight through them and the walk asked them to move. After every Talk the other Sim's
        // "walk away" (its slot 6, ahead allowed) took the spot just past the talker, and the talker
        // stepped back for it (the user's report, 2026-11-05). Such a spot isn't taken. TS_SPOT_TILE_OLD=1:
        // as before.
        // Only for a Sim's own slots (the walk away, a social's places): round a table or a counter it
        // left the waitresses downtown no spot to serve from whenever a Sim stood by (lots 22, 25, 26).
        static const bool spot_tile_old = TS_ENV("TS_SPOT_TILE_OLD") != nullptr;
        if (!spot_tile_old && walker.person && obj.person)
            if (const ByTile *t = on_tile(tx, ty))
                for (const Entity *e : t->people)
                    if (e != &walker && e != &obj && !e->container) {
                        if (TS_ENV("TS_SLOT"))   // debug
                            std::printf("  spot (%.2f, %.2f) for #%d: #%d on its tile\n", px / 16.0, py / 16.0, int(walker.id),
                                        int(e->id));
                        return why[4]++, false;
                    }
        return true;
    };
    auto random_bit = [&]() {
        if (!(slot.flags & kRandomScoring))
            return 0;
        vm.random_state = vm.random_state * 1103515245u + 12345u;
        return int((vm.random_state >> 8) % 20u) - 10;
    };
    // A chair beside the spot, facing the object square on, free, its sit entry there.
    auto chair_at = [&](int16_t px, int16_t py, int sector) -> Entity * {
        if (sit_w <= 0)
            return nullptr;
        for (int sdir = 0; sdir < 9; sdir++) {   // the eight beside the spot, and the spot's own tile
            const int cx = px / 16 + (sdir < 8 ? kStep[sdir][0] : 0), cy = py / 16 + (sdir < 8 ? kStep[sdir][1] : 0);
            const ByTile *t = px < 0 || py < 0 ? nullptr : on_tile(cx, cy);
            if (!t)
                continue;
            for (const Entity *ce : t->objects) {
                Entity *e = const_cast<Entity *>(ce);   // (as the entities' list gives them)
                if (e->entry_points.size() <= 26 || e->entry_points[26].action == 0)
                    continue;
                if ((e->direction & 7) != ((sector + 4) & 7))
                    continue;
                // And sat on from the spot's side: its ChairEntryFlags, side i the tile in direction
                // (its facing + i), as getting up reads them (FUN_0060d440's seat loop: 85 for chairs,
                // front and sides and back; 1 for most sofas, the front only). Ours took any seat
                // beside a spot, a sofa from its end.
                if (sdir < 8 && e->info) {
                    const int side = ((sdir + 4) - e->direction + 16) & 7;
                    if (!(e->info->objd.get(Objd::ChairEntryFlags) & (1 << side)) && !TS_ENV("TS_SEAT_ANY_SIDE"))
                        continue;
                }
                // Taken: something other than the walker in its slot (the exe reads the seat's one
                // occupant, slot 0, and compares it with the walker, FUN_005f2ba0). Ours counted the
                // walker too, so a Sim's own seat was never a place to sit: seated Sims told to
                // watch TV got up and went for another seat (the Bobs, 2026-11-04 audit).
                const Entity *occupant = e->slots.empty() ? nullptr : e->slots[0];
                const bool taken = occupant && occupant != &walker;
                if (taken && !seat_taker)
                    seat_taker = occupant;
                const double ddx = e->x16 - ox, ddy = e->y16 - oy, dd = std::sqrt(ddx * ddx + ddy * ddy);
                if (!taken && dd >= slot.min_proximity - 0.01 && dd <= std::max(slot.max_proximity, slot.min_proximity) + 0.01)
                    return e;
            }
        }
        return nullptr;
    };
    if (dirs == 0) {   // one spot: the slot's offset, turned with the object
        int16_t px, py;
        rotate(slot.x, slot.y, facing, px, py);
        // The walls' tests too (the back edge towards the object, a Sim's box across a wall), not
        // the others (a Sim standing there is waited for, as before): a pet bowl outside with its
        // front to the house had its spot on the wall line, and pets ate from inside, standing in
        // the wall (the all-lots wall census, 2026-11-05: lot 56's pets, lot 69's). TS_ONE_SPOT_OLD=1:
        // as before.
        static const bool one_spot_old = TS_ENV("TS_ONE_SPOT_OLD") != nullptr;
        bool walls_ok = true;
        if (!one_spot_old && walker.person) {
            const int tx = px / 16, ty = py / 16;
            if (!(slot.flags & kIgnoreRooms) && !in_wall && tx >= 0 && ty >= 0 && wall_back(tx, ty))
                walls_ok = false;
            else if (box_hits_wall(vm, person_box(walker, px, py), obj.level))
                walls_ok = false;
        }
        // A solid object on the spot (not the object itself or its other tiles, nor one the walker
        // stands in now): no spot, as the original's. Its planner keeps every solid footprint but
        // those the walker starts in as an obstacle (FUN_0060e690), so a goal inside one is never
        // reached; FreeSO's slot search refuses it outright (VerifyAndAddLocation, DestTileOccupied).
        // Ours left the one spot unchecked and the planner left out whatever held the goal: the
        // register clerks' "on top of" the register (their Main, goto relative -2) took them into
        // the counter or table the register stands on (Store Clerk Ian inside the counter, the
        // user's report 2026-11-06); the original's walk fails and the clerk stays beside it where
        // he was put. TS_ONE_SPOT_SOLID_OLD=1: as before.
        static const bool solid_old = TS_ENV("TS_ONE_SPOT_SOLID_OLD") != nullptr;
        const Entity *solid = nullptr;
        if (walls_ok && !solid_old && walker.person) {
            const Footprint mine = person_box(walker, px, py), now = person_box(walker, walker.x16, walker.y16);
            for (int dy = -1; dy <= 1 && !solid; dy++)
                for (int dx = -1; dx <= 1 && !solid; dx++)
                    if (const ByTile *t = px < 0 || py < 0 ? nullptr : on_tile(px / 16 + dx, py / 16 + dy))
                        for (const Entity *e : t->objects) {
                            if (e == &walker || e == &obj || !blocks_walking(*e) ||
                                std::find(obj.group.begin(), obj.group.end(), e) != obj.group.end())
                                continue;
                            const Footprint fp = footprint(*e, e->x16, e->y16, e->direction);
                            if (fp.overlaps(mine) && !fp.overlaps(now)) {
                                solid = e;
                                break;
                            }
                        }
            if (solid && TS_ENV("TS_SLOT"))   // debug
                std::printf("  one spot (%.2f, %.2f) of #%d for #%d: #%d is there\n", px / 16.0, py / 16.0, int(obj.id),
                            int(walker.id), int(solid->id));
        }
        if (solid) {
            if (fail_blocker)
                *fail_blocker = const_cast<Entity *>(solid);
            why[3]++;
        } else if (walls_ok) {
            goals.push_back({ px, py, 100, 0, nullptr });
        } else {
            why[2]++;
        }
    } else {
        const int lo = slot.min_proximity, hi = std::max(slot.max_proximity, lo), opt = slot.optimal_proximity;
        if (lo < 1 || hi > 512 || opt < lo || opt > hi)
            return false;   // no goals (the original's check of the proximities)
        int res = std::max(1, int(slot.resolution));
        int k0 = lo / res, k1 = hi / res;
        if (sit_w > 0) {   // seats looked for tile by tile, one ring further each way
            res = 16;
            k0 = std::max(1, lo / res - 1);
            k1 = hi / res + 1;
        }
        for (int d = 0; d < 8; d++) {
            if (!(dirs & (1 << d)))
                continue;
            const int sector = (d + facing) & 7;
            if (!(slot.flags & kIgnoreRooms) && !in_wall && wall_side(sector)) {
                why[5]++;
                continue;   // a wall on the object's own tile that way: none of it (code 11)
            }
            const bool open = (slot.flags & kIgnoreRooms) || in_wall ||
                              (!wall_side(sector) && !wall_side(sector + 1) && !wall_side(sector + 7));
            const bool next_on = dirs & (1 << ((d + 2) & 7));
            for (int k = k0; k <= k1; k++)
                for (int c = -k; c <= (next_on ? k - 1 : k); c++) {
                    const double lx = c * res + slot.x, ly = -k * res + slot.y;
                    const double dist = std::sqrt(lx * lx + ly * ly);
                    int16_t px, py;
                    rotate(lx, ly, sector, px, py);
                    const bool in_band = dist >= lo - 0.01 && dist <= hi + 0.01;
                    const int base = 100 - int(std::fabs(double(int(dist)) - opt) * slot.gradient) + (open ? 25 : 0) +
                                     ((px == walker.x16 && py == walker.y16) ? 20 : 0);
                    auto add_standing = [&]() {
                        if (stand_w > 0 && in_band && spot_ok(px, py)) {
                            goals.push_back({ px, py, base + stand_w + random_bit(), 1 << d, nullptr });
                            return true;
                        }
                        return false;
                    };
                    auto add_seat = [&]() {
                        if (Entity *chair = chair_at(px, py, sector)) {
                            if (std::find_if(goals.begin(), goals.end(), [&](const Goal &g) { return g.chair == chair; }) == goals.end())
                                goals.push_back({ chair->x16, chair->y16, base + sit_w + random_bit(), 1 << d, chair });
                            return true;
                        }
                        return false;
                    };
                    if (stand_w >= sit_w) {
                        if (!add_standing())
                            add_seat();
                    } else if (!add_seat()) {
                        add_standing();
                    }
                }
            // Where the walker stands, when that is in this direction's band: a goal too, with the
            // +20 the exe gives the walker's own spot. Only the rings' grid points were goals, and
            // the +20 went to one only if the walker stood exactly on it: after a social the other
            // Sim had turned to face this one, the grid turned with it, and every Talk began with a
            // step back or round to the best grid point ("why can't he just stand next to her", the
            // user's report 2026-11-05).
            // Off for now (TS_SLOT_OWN_SPOT=1: on): the 81-lot survey had failed walks +7%, false ends
            // +6.5% with it alone; the step back it was for is the "walk away" through the talker,
            // which the spot-tile rule in spot_ok answers.
            static const bool own_spot_off = TS_ENV("TS_SLOT_OWN_SPOT") == nullptr;
            if (!own_spot_off && stand_w > 0 && walker.person && walker.level == obj.level && !walker.container) {
                const double t = sector * 3.14159265358979 / 4;
                const double rx = walker.x16 - ox, ry = walker.y16 - oy;
                const double lx = rx * std::cos(t) + ry * std::sin(t), ly = -rx * std::sin(t) + ry * std::cos(t);
                const double ax = lx - slot.x, ay = -(ly - slot.y);   // across, and out along the direction
                const double dist = std::sqrt(lx * lx + ly * ly);
                const bool in_wedge = ay > 0 && (next_on ? (ax >= -ay && ax < ay) : std::fabs(ax) <= ay);
                if (TS_ENV("TS_SLOT_OWN_LOG"))   // debug
                    std::printf("own spot: #%d (%d, %d) at slot of #%d (%d, %d; origin %.1f, %.1f) dir %d sector %d: across %.1f out %.1f dist %.1f band %d-%d wedge %d ok %d\n",
                                int(walker.id), int(walker.x16), int(walker.y16), int(obj.id), int(obj.x16), int(obj.y16), double(ox),
                                double(oy), d, sector, ax, ay, dist, lo, hi, int(in_wedge), int(spot_ok(walker.x16, walker.y16)));
                // (A sixteenth either way: the goals themselves are rounded to sixteenths, a diagonal
                // one tile off at 11, 11 is 15.6 away, and a Sim sent there stands outside the exact band.)
                if (in_wedge && dist >= lo - 1.0 && dist <= hi + 1.0 && spot_ok(walker.x16, walker.y16)) {
                    const int base = 100 - int(std::fabs(double(int(dist)) - opt) * slot.gradient) + (open ? 25 : 0) + 20;
                    goals.push_back({ walker.x16, walker.y16, base + stand_w + random_bit(), 1 << d, nullptr });
                }
            }
        }
    }
    goals.erase(std::remove_if(goals.begin(), goals.end(), [](const Goal &g) { return g.score <= 0; }), goals.end());
    // At most 48 goals (8 when the walker's room has 251 tiles or more: outdoors), as the exe's goal
    // building (FUN_0060d440 after FUN_0060bd40 set the cap at +0xac): over it, the seats are kept and
    // the rest drawn with the game's random number (FUN_0061ee40), each draw taking the first goal not
    // yet taken from a random place on (no wrap: a draw past the last free one takes nothing). Ours
    // kept every spot, so a Sim outdoors weighed hundreds and always took the same best one; the
    // original's spread over a few. TS_NO_GOAL_CAP=1: as before.
    {
        static const bool no_cap = TS_ENV("TS_NO_GOAL_CAP") != nullptr;
        const uint16_t walker_room = vm.room_at(walker.level, walker.tile_x(), walker.tile_y());
        const size_t cap = walker_room < vm.rooms.size() && vm.rooms[walker_room].area >= 251 ? 8 : 48;
        if (!no_cap && goals.size() > cap) {
            std::vector<Goal> all = std::move(goals);
            std::vector<bool> taken(all.size(), false);
            goals.clear();
            for (size_t i = 0; i < all.size(); i++)
                if (all[i].chair) {
                    goals.push_back(all[i]);
                    taken[i] = true;
                }
            for (size_t count = goals.size(); count < cap; count++) {
                const uint64_t p = uint64_t(vm.random_state) * 0x41c64e6dull + 0x3039u;
                vm.random_state = uint32_t(p);
                const uint32_t r = uint32_t(p >> 16);
                for (size_t i = r % all.size(); i < all.size(); i++)
                    if (!taken[i]) {
                        goals.push_back(all[i]);
                        taken[i] = true;
                        break;
                    }
            }
        }
    }
    // The spots this instruction's walks failed to reach already (Entity::Route::SpotTries).
    if (avoid)
        goals.erase(std::remove_if(goals.begin(), goals.end(),
                                   [&](const Goal &g) {
                                       for (const auto &p : *avoid)
                                           if (p.first == g.x && p.second == g.y)
                                               return true;
                                       return false;
                                   }),
                    goals.end());
    if (fail_code)
        *fail_code = why[3] ? 8 : why[1] || why[2] || why[5] ? 11 : 1;
    if (fail_code && goals.empty() && seat_taker) {
        // No goal left and a seat skipped as someone else's: "seat taken" (9), its occupant the
        // balloon's, ahead of the other codes (the exe's local_43, FUN_005f2ba0).
        *fail_code = 9;
        if (fail_blocker)
            *fail_blocker = const_cast<Entity *>(seat_taker);
    }
    if (goals.empty()) {
        if (TS_ENV("TS_SLOT"))
            std::printf("  slot spots on #%d (stand %d sit %d, flags %x, prox %d/%d/%d): none; off lot %d, other room %d, wall %d, object %d, "
                        "Sim %d, wall beside the object %d\n", int(obj.id), stand_w, sit_w, unsigned(slot.flags), int(slot.min_proximity),
                        int(slot.optimal_proximity), int(slot.max_proximity), why[0], why[1], why[2], why[3], why[4], why[5]);
        return false;
    }
    int best = INT32_MIN;
    for (const Goal &g : goals)
        best = std::max(best, g.score);
    std::vector<std::pair<int, int>> tiles;
    for (const Goal &g : goals)
        tiles.push_back({ g.x / 16, g.y / 16 });
    const std::vector<float> cost = walk_costs(vm, walker, tiles, &obj);
    int chosen = -1;
    double least = 1e29;
    for (size_t i = 0; i < goals.size(); i++) {
        if (cost[i] >= 1e29f)
            continue;
        const double total = cost[i] + double(best - goals[i].score);   // (both in tiles: a point is a tile's walk)
        if (total < least) {
            least = total;
            chosen = int(i);
        }
    }
    if (chosen < 0)   // none reachable: the best scored, and the walk will say why
        chosen = int(std::max_element(goals.begin(), goals.end(), [](const Goal &a, const Goal &b) { return a.score < b.score; }) -
                     goals.begin());
    const Goal &g = goals[size_t(chosen)];
    x = g.x;
    y = g.y;
    face = g.chair ? -1 : face_for(g.x, g.y);
    if (entry)
        *entry = g.entry ? g.entry : dirs;
    if (seat)
        *seat = g.chair;
    if (const char *dbg = TS_ENV("TS_SLOT"); dbg && dbg[0] == '2') {   // debug: every candidate
        std::printf("  slot of #%d (flags %x, dirs %x facing %d, prox %d-%d, obj at (%d, %d) walker at (%.1f, %.1f) footprint %x): why",
                    int(obj.id), unsigned(slot.flags), unsigned(dirs), facing, int(slot.min_proximity), int(slot.max_proximity), obj.tile_x(), obj.tile_y(),
                    walker.x16 / 16.0, walker.y16 / 16.0,
                    walker.person && walker.person_data.size() > 73 ? unsigned(uint16_t(walker.person_data[73])) : 0u);
        for (int i = 0; i < 6; i++)
            std::printf(" %d", why[i]);
        std::printf(";");
        for (const Goal &g : goals)
            std::printf(" (%d,%d)%d", g.x / 16, g.y / 16, g.score);
        std::printf("\n");
    }
    if (TS_ENV("TS_SLOT"))
        std::printf("  slot spot: (%d, %d) of %lu, score %d (best %d), walk %.1f%s\n", x / 16, y / 16,
                    (unsigned long)goals.size(), g.score, best, double(cost[size_t(chosen)]), g.chair ? ", a seat" : "");
    return true;
}

// A spot near `obj` as a routing slot describes it: in one of the slot's
// directions (relative to the object unless absolute), at its proximity, not
// with a wall between it and the object (with `vm`).
// `entry`: the slot's direction flags the spot fulfils, as the game's code
// reads them after the walk (person data 37, RouteEntryFlags).
bool slot_spot(const Entity &obj, const Entity &walker, const SlotItem &slot, int16_t &x, int16_t &y, int &face,
               int *entry = nullptr, int lot_size = 0, const Vm *vm = nullptr, Entity **seat = nullptr)
{
    if (seat)
        *seat = nullptr;
    // FreeSims VMSlotParser. The slot's centre: the object's, plus the slot's
    // offset turned with the object.
    constexpr uint32_t kAbsolute = 512, kSnapToDirection = 4096;
    const double rot = obj.direction * 3.14159265358979 / 4;
    const double ox = slot.x * std::cos(rot) - slot.y * std::sin(rot);
    const double oy = slot.x * std::sin(rot) + slot.y * std::cos(rot);
    const int16_t cx = int16_t(std::lround(obj.x16 + ox)), cy = int16_t(std::lround(obj.y16 + oy));
    const int dirs = slot.flags & 255;
    const bool absolute = slot.flags & kAbsolute;
    if (TS_ENV("TS_SLOT"))   // debug: the slot asked for
        std::printf("slot on #%d (dir %d): x %.1f y %.1f flags %x facing %d prox %d/%d/%d snap %d standing %d sitting %d\n",
                    obj.id, obj.direction, double(slot.x), double(slot.y), unsigned(slot.flags), int(slot.facing),
                    int(slot.min_proximity), int(slot.optimal_proximity), int(slot.max_proximity), int(slot.snap_target),
                    int(slot.standing), int(slot.sitting));
    // Which way to face at a spot: towards the object (-2), away (-1), anywhere (-3), or a direction.
    auto facing_at = [&](int16_t px, int16_t py) {
        if (slot.facing >= 0)
            return absolute ? slot.facing % 8 : (slot.facing + obj.direction) % 8;
        if (slot.facing == -3)
            return -1;
        if (px == obj.x16 && py == obj.y16)
            return obj.direction;   // on the object itself (a seat): the way it faces
        const double dx = obj.x16 - px, dy = obj.y16 - py;
        int to = (int(std::lround(std::atan2(dx, -dy) / (3.14159265358979 / 4))) + 8) % 8;
        return slot.facing == -1 ? (to + 4) % 8 : to;
    };
    // A seat on that tile the slot can take instead of a spot to stand (FreeSims VMSlotParser:
    // a slot that allows sitting takes a chair facing the right way within 45 degrees, its sit
    // entry point 26; the TV's viewing slot weighs sitting 1000 to standing 1).
    auto seat_on = [&](int16_t px, int16_t py, int want_face) -> Entity * {
        if (!seat || !vm || slot.sitting <= 0)
            return nullptr;
        for (const auto &e : vm->entities)
            if (!e->dead && !e->person && e.get() != &walker && e->in_world() && !e->container && e->level == obj.level &&
                e->tile_x() == px / 16 && e->tile_y() == py / 16 && e->entry_points.size() > 26 && e->entry_points[26].action != 0) {
                bool taken = false;
                for (const Entity *held : e->slots)
                    taken = taken || (held && held->person);
                const int turn = want_face < 0 ? 0 : std::abs(((e->direction - want_face) % 8 + 12) % 8 - 4);   // steps apart, 0-4
                if (!taken && turn <= 1)   // within 45 degrees
                    return e.get();
            }
        return nullptr;
    };
    const bool prefer_seats = slot.sitting > slot.standing;
    // A door stands in its wall and opens it: its own spots either side aren't "past a wall"
    // (the front door's slot had no spot, so every walk through it counted as failed).
    const bool in_wall = (uint16_t(obj.value(VarFlagField2)) & 0x8000) != 0;   // architectural door
    if (slot.flags & kSnapToDirection) {   // exactly there, facing the slot's way
        x = cx;
        y = cy;
        int rel = slot.facing >= 0 ? slot.facing : 0;
        if (slot.facing < 0)
            for (int d = 0; d < 8; d++)
                if (dirs & (1 << d)) {
                    rel = d;
                    break;
                }
        face = (rel + obj.direction) % 8;
        if (entry)
            *entry = dirs;   // the direction bits only
        return true;
    }
    if (dirs == 0 || slot.x != 0 || slot.y != 0) {   // exactly there
        x = cx;
        y = cy;
        face = facing_at(x, y);
        if (entry)
            *entry = dirs;   // the direction bits only
        return true;
    }
    // Around it, in the slot's directions, at its preferred distance: the spot nearest the walker.
    const int distance = slot.optimal_proximity > 0 ? slot.optimal_proximity : slot.min_proximity;
    double best = 1e30;
    bool found = false;
    for (int d = 0; d < 8; d++) {
        if (!(dirs & (1 << d)))
            continue;
        const int world_dir = absolute ? d : (d + obj.direction) % 8;
        const int16_t px = int16_t(cx + kStep[world_dir][0] * distance);
        const int16_t py = int16_t(cy + kStep[world_dir][1] * distance);
        if (lot_size > 0 && (px < 0 || py < 0 || px >= lot_size * 16 || py >= lot_size * 16))
            continue;   // not off the lot (a Sim running from a ghost)
        if (vm && !in_wall && wall_in_way(*vm, obj.tile_x(), obj.tile_y(), px / 16, py / 16, obj.level))
            continue;   // the other side of a wall (a bath against it is got into from the room)
        Entity *chair = nullptr;
        if (vm && tile_taken(*vm, px / 16, py / 16, obj.level, &walker, &obj)) {
            chair = seat_on(px, py, facing_at(px, py));
            if (TS_ENV("TS_SLOT"))
                for (const auto &e : vm->entities)
                    if (!e->dead && !e->person && e.get() != &walker && e->in_world() && !e->container && e->level == obj.level &&
                        e->tile_x() == px / 16 && e->tile_y() == py / 16 && blocks_walking(*e))
                        std::printf("  slot spot (%d, %d): taken by #%d %s%s\n", px / 16, py / 16, int(e->id),
                                    e->info ? e->info->label.c_str() : "?", chair ? " (a seat to take)" : "");
            if (!chair)
                continue;   // in an object (a sink beside the Sim talked to): nowhere to stand
        }
        // Another Sim there or headed there: the next spot (FreeSO's router tries the slot's goals
        // in turn; Bella's panic at a fire took Mortimer's spot and gave up, again and again).
        if (vm && walker.person && !chair && spot_taken(*vm, walker, px, py, obj.level))
            continue;
        const double dx = px - walker.x16, dy = py - walker.y16;
        const double score = dx * dx + dy * dy - (chair && prefer_seats ? 1e12 : 0);
        if (score < best) {
            best = score;
            x = px;
            y = py;
            face = facing_at(px, py);
            found = true;
            if (seat)
                *seat = chair;
            if (entry) {
                // The direction from the object, relative to it (FreeSims GetSearchDirection: its
                // sectors are cut 0.1 degree short, so a spot straight out one way is that way
                // alone; seats' code tests person data 37 for exactly 1, 4, 16 or 64).
                const int rel = absolute ? (d - obj.direction + 8) % 8 : d;
                *entry = dirs & (1 << rel);
            }
        }
    }
    const int lo = slot.min_proximity, hi = std::max(slot.max_proximity, lo);
    if ((found && !(prefer_seats && seat && !*seat)) || hi <= 0 || (lo == distance && hi == distance))
        return found;
    // (A slot that would rather sit, with no seat at its preferred distance: a seat anywhere in its band first.)
    if (found)
        best = 1e30;
    const bool had_spot = found;
    const int16_t spot_x = x, spot_y = y;
    const int spot_face = face;
    const int spot_entry = entry ? *entry : 0;
    // Nothing free at the preferred distance (the TV's 3 tiles in front, a sofa there): anywhere
    // in the slot's band (min to max) in its directions, as FreeSims' VMSlotParser searches it:
    // the spot nearest the preferred distance, then the walker (a third as much).
    const int step = std::max(16, int(slot.resolution));
    const int bound = (hi / step) * step;
    for (int gy = -bound; gy <= bound; gy += step)
        for (int gx = -bound; gx <= bound; gx += step) {
            const double dist = std::sqrt(double(gx * gx + gy * gy));
            if (dist < lo - 0.01 || dist > hi + 0.01 || (gx == 0 && gy == 0))
                continue;
            // Its direction from the object, relative to it, in the game's overlapping sectors (FreeSims GetSearchDirection).
            double deg = std::atan2(double(gx), double(-gy)) * 180.0 / 3.14159265358979 - (absolute ? 0 : obj.direction * 45.0);
            while (deg > 180.0)
                deg -= 360.0;
            while (deg <= -180.0)
                deg += 360.0;
            int sectors = 0;
            for (int d = 0; d < 8; d++) {
                double off = deg - d * 45.0;
                while (off > 180.0)
                    off -= 360.0;
                while (off <= -180.0)
                    off += 360.0;
                if (std::fabs(off) <= 44.9)   // (FreeSims ANGLE_ERROR -0.1: no overlap)
                    sectors |= 1 << d;
            }
            if (!(sectors & dirs))
                continue;
            const int16_t px = int16_t(cx + gx), py = int16_t(cy + gy);
            if (lot_size > 0 && (px < 0 || py < 0 || px >= lot_size * 16 || py >= lot_size * 16))
                continue;
            if (vm && !in_wall && wall_in_way(*vm, obj.tile_x(), obj.tile_y(), px / 16, py / 16, obj.level))
                continue;
            // In the object's own room, unless the slot says rooms don't matter (FreeSims: IgnoreRooms,
            // 2048): a spot two tiles off could be past a wall (Bella into the bath from the next room).
            constexpr int kIgnoreRooms = 2048;
            if (vm && !(slot.flags & kIgnoreRooms) &&
                vm->room_at(obj.level, px / 16, py / 16) != vm->room_at(obj.level, obj.tile_x(), obj.tile_y()))
                continue;
            Entity *chair = nullptr;
            if (vm && tile_taken(*vm, px / 16, py / 16, obj.level, &walker, &obj)) {
                chair = seat_on(px, py, facing_at(px, py));
                if (!chair)
                    continue;
            }
            if (had_spot && !chair)
                continue;   // looking for seats only: the spot at the preferred distance stands
            if (vm && walker.person && !chair && spot_taken(*vm, walker, px, py, obj.level))
                continue;
            const double wx = px - walker.x16, wy = py - walker.y16;
            const double score = std::fabs(dist - distance) + std::sqrt(wx * wx + wy * wy) / 3.0 - (chair && prefer_seats ? 1e12 : 0);
            if (score < best) {
                best = score;
                x = px;
                y = py;
                face = facing_at(px, py);
                found = true;
                if (entry)
                    *entry = dirs & sectors;
                if (seat)
                    *seat = chair;
            }
        }
    if (had_spot && seat && !*seat) {   // no seat in the band either: the standing spot as found
        x = spot_x;
        y = spot_y;
        face = spot_face;
        if (entry)
            *entry = spot_entry;
    }
    if (found && TS_ENV("TS_SLOT"))
        std::printf("  slot spot: none at %d, so (%d, %d) in the band %d-%d\n", distance, x / 16, y / 16, lo, hi);
    return found;
}

// The side a Sim gets out of a seat, for a walk to (x, y): of the sides its ChairEntryFlags allow,
// the free one nearest the goal; another Sim there doesn't count as taken (FUN_0060d130, FUN_0060e2b0).
// As a flag, 1 << the side; 0 for none. The stand function starts from it (person data 37: a
// dining chair's goes that way first, then round the others).
int seat_exit_side(const Vm &vm, const Entity &sim, const Entity &seat, int x, int y)
{
    const int flags = seat.info ? seat.info->objd.get(Objd::ChairEntryFlags) : 0;
    int best = 0;
    long best_d = 0;
    for (int i = 0; i < 8; i++) {
        if (!(flags & (1 << i)))
            continue;
        const int dir = (seat.direction + i) & 7;
        const int tx = seat.tile_x() + kStep[dir][0], ty = seat.tile_y() + kStep[dir][1];
        if (tx < 1 || ty < 1 || tx >= vm.lot.size - 1 || ty >= vm.lot.size - 1 ||
            wall_between(vm, seat.tile_x(), seat.tile_y(), tx, ty, seat.level) ||
            tile_taken(vm, tx, ty, seat.level, &sim, &seat))
            continue;
        const long dx = tx * 16 + 8 - x, dy = ty * 16 + 8 - y, d = dx * dx + dy * dy;
        if (!best || d < best_d) {
            best = 1 << i;
            best_d = d;
        }
    }
    return best;
}

bool take_ladder(Frame &f, Exit &result);

Exit push_person_routine(Frame &f, uint16_t id);
bool ask_to_move(Vm &vm, Entity &sim, Entity &blocker, bool *asked_already = nullptr, bool all_round = false);

// No way to (x, y): the Sim near the walker whose leaving opens one, as the exe's routing frame
// looks for before it fails a walk (FUN_005f07e0 with FUN_0060fa70's candidates: people within 3
// tiles counted in the way, not the target, not held; the plan made again without each). Null if
// none would.
Entity *person_opening_way(Vm &vm, Entity &sim, int16_t x, int16_t y, const Entity *target)
{
    std::vector<std::pair<float, Entity *>> near;
    const uint16_t room = vm.room_at(sim.level, sim.tile_x(), sim.tile_y());
    for (auto &up : vm.entities) {
        Entity &e = *up;
        if (e.dead || !e.person || !e.in_world() || e.container || e.level != sim.level || &e == &sim || &e == target ||
            is_ghost(e))
            continue;
        const float dx = float(e.x16 - sim.x16) / 16, dy = float(e.y16 - sim.y16) / 16, d = dx * dx + dy * dy;
        if (d > 9.0f || vm.room_at(e.level, e.tile_x(), e.tile_y()) != room)
            continue;
        near.push_back({ d, &e });
    }
    std::sort(near.begin(), near.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::pair<int16_t, int16_t>> way;
    for (const auto &[d, e] : near) {
        vm.route_ignore_person = e;
        const bool opens = find_path(vm, sim, x, y, way, target);
        vm.route_ignore_person = nullptr;
        if (opens)
            return e;
    }
    return nullptr;
}

// Starts or continues a walk for the instruction in `f`.
Exit walk_to(Frame &f, int16_t x, int16_t y, int face, const Entity *target)
{
    Entity &sim = *f.caller;
    auto &route = sim.route;
    // Sitting, and going somewhere else than this seat: the seat's own stand function first
    // (entry point 27, as FreeSO's VMRoutingFrame pushes it): it plays getting up and leaves the
    // Sim beside the seat; the walk instruction runs again when it returns. Once a walk: if it
    // didn't get them up, they're stood up below as before. (Ours stood them up on the spot, no
    // animation: the 2026-10-27 Vita test's jumps from sitting to standing.)
    // The "tried" mark is for that one walk: once they're up it goes. Kept, the same instruction's
    // next walk from a seat counted as tried and the Sim was stood up on the spot without the
    // seat's stand function, its clears not run (person data 59 and 79 left set: every social
    // with Sims on sofas hidden; the sofa's "group talk - exit person" skipped; 2026-11-04 audit).
    if (!(sim.person && sim.person_data[0] == 1 && sim.container && !sim.container->person))
        route.stand_routine = nullptr;
    if (sim.person && sim.person_data[0] == 1 && sim.container && !sim.container->person && sim.container != target) {
        Entity &seat = *sim.container;
        const size_t depth = f.thread->stack.size();
        const bool tried = route.stand_routine == f.routine && route.stand_ip == f.ip && route.stand_depth == depth;
        if (!tried && seat.entry_points.size() > 27 && seat.entry_points[27].action != 0) {
            Vm &vm = f.vm();
            CodeOwner owner;
            if (const Routine *r = vm.routine(seat.code, seat.entry_points[27].action, &owner)) {
                route.stand_routine = f.routine;
                route.stand_ip = f.ip;
                route.stand_depth = depth;
                route.state = Entity::Route::Idle;
                route.path.clear();
                Frame stand;
                stand.routine = r;
                stand.caller = &sim;
                stand.callee = &seat;
                stand.owner = owner;
                stand.stack_id = int16_t(seat.id);
                stand.action_tree = f.action_tree;
                stand.resume_caller = true;
                stand.args.assign(std::max<size_t>(r->args, 4), 0);
                const int side = seat_exit_side(vm, sim, seat, x, y);
                stand.args[0] = int16_t(side);
                sim.person_data[37] = int16_t(side);
                f.thread->push(std::move(stand));   // `f` may move: don't touch it after this
                return Exit::Continue;
            }
        }
    }
    if (route.door_depth && f.thread->stack.size() <= route.door_depth)
        route.door_depth = 0;   // back from a door's portal function: doors open again
    if (sim.person && f.vm().in_pool(sim)) {
        const uint16_t to = f.vm().room_at(sim.level, x / 16, y / 16);
        if (to == 0 || to >= f.vm().rooms.size() || !f.vm().rooms[to].pool) {
            Exit out = Exit::GotoFalse;
            if (take_ladder(f, out))
                return out;
        }
    }
    // Doors shut for another walk instruction's sake don't stay shut for this one.
    if (route.shut_routine != f.routine || route.shut_ip != f.ip || route.shut_depth != f.thread->stack.size())
        route.shut_doors.clear();
    route.routine = f.routine;
    route.ip = f.ip;
    route.depth = f.thread->stack.size();
    route.next = 0;
    route.face = face;
    route.entry_object = nullptr;
    route.from_x = sim.x16;
    route.from_y = sim.y16;
    if (sim.person && (sim.person_data[0] != 0 || (sim.container && !sim.container->person))) {
        // A Sim walks standing: out of a seat it was left in.
        if (sim.container && !sim.container->person)
            sim.leave_container();
        sim.person_data[0] = 0;
    }
    Vm &vm = f.vm();
    route.target_id = target ? int16_t(target->id) : int16_t(0);
    route.blocked_by = 0;
    route.block_stage = 0;
    route.rewaits = 0;
    route.paused = false;
    const bool way = find_path(vm, sim, x, y, route.path, target);
    if (const char *who = TS_ENV("TS_WALK_GOAL"); who && std::atoi(who) == sim.id) {   // debug: a Sim's walks, as they start
        std::printf("walk goal: tick %u #%d from (%.2f, %.2f) room %u to (%.2f, %.2f) room %u (%s, in %s), %s, %u steps\n",
                    unsigned(vm.ticks), int(sim.id), sim.x16 / 16.0, sim.y16 / 16.0,
                    unsigned(vm.room_at(sim.level, sim.tile_x(), sim.tile_y())), x / 16.0, y / 16.0,
                    unsigned(vm.room_at(sim.level, x / 16, y / 16)), target && target->info ? target->info->label.c_str() : "-",
                    f.routine ? f.routine->name.c_str() : "?", way ? "a way" : "no way", unsigned(route.path.size()));
    }
    if (way)
        route.opening_asked = 0;
    // No way, but one Sim near would open it by stepping aside: asked first, and waited for up to a
    // hundred ticks, the walk planned again each time (the exe's routing frame, FUN_005f07e0, before
    // the failure). Ours failed at once with the balloon (the routing audit, 2026-11-04).
    if (!way && sim.person && sim.person_data[0] == 0)
        if (Entity *opener = person_opening_way(vm, sim, x, y, target)) {
            constexpr uint16_t kRouteWaiting = 8276, kAskToMove = 8302;
            if (route.opening_asked != int16_t(opener->id)) {
                bool asked_already = false;
                if (ask_to_move(vm, sim, *opener, &asked_already, false) || asked_already) {
                    if (TS_ENV("TS_QUEUE_WHY"))   // debug
                        std::printf("walk: #%d has no way; asks #%d, who'd open one, to move\n", int(sim.id),
                                    int(opener->id));
                    route.opening_asked = int16_t(opener->id);
                    route.opening_start = vm.ticks;
                    route.state = Entity::Route::Idle;
                    play_standing(vm, sim);
                    if (!asked_already)
                        return push_person_routine(f, kAskToMove);
                }
            }
            if (route.opening_asked == int16_t(opener->id) && vm.ticks - route.opening_start < 100) {
                route.state = Entity::Route::Idle;   // (the instruction plans afresh when it runs again)
                if (route.waited_at == vm.ticks)
                    return Exit::ContinueNextTick;
                route.waited_at = vm.ticks;
                return push_person_routine(f, kRouteWaiting);
            }
        }
    if (!way) {
        vm.warn(&f, "no way from (" + std::to_string(sim.tile_x()) + ", " + std::to_string(sim.tile_y()) + ") to (" +
                        std::to_string(x / 16) + ", " + std::to_string(y / 16) + ") on level " + std::to_string(sim.level));
        if (const char *map = TS_ENV("TS_ROUTE_MAP")) {   // debug: the grid between the two (or "x0 y0 x1 y1")
            int b[4] = { std::min(sim.tile_x(), x / 16) - 3, std::min(sim.tile_y(), y / 16) - 3, std::max(sim.tile_x(), x / 16) + 3,
                         std::max(sim.tile_y(), y / 16) + 3 };
            std::sscanf(map, "%d %d %d %d", &b[0], &b[1], &b[2], &b[3]);
            std::printf("route map, #%d (%d, %d) to (%d, %d), level %d:\n%s", int(sim.id), sim.tile_x(), sim.tile_y(), x / 16, y / 16, int(sim.level), route_map(vm, sim, sim.level, b[0], b[1], b[2], b[3]).c_str());
        }
        route.state = Entity::Route::Failed;   // reported on the next tick, as the game does
        return Exit::ContinueNextTick;
    }
    route.state = Entity::Route::Walking;
    route.velocity = 0;
    route.walk_frames = 0;
    route.turning = 0;
    route.seg_set = false;
    route.to_corner = false;
    route.corner_used = false;
    route.fx = float(sim.x16);
    route.fy = float(sim.y16);
    // Facing away from the way out: a turn on the spot first only when the point it heads for is a
    // quarter turn or more off (the exe's walker, FUN_005d33b0; pets 0.8 of an eighth); less is turned
    // while walking. Ours turned for an eighth or more (FreeSO BeginWalk), every set-off a stop.
    // TS_WALK_OLD=1: as before. Not for the walks a door's own code makes through it.
    static const bool walk_old = TS_ENV("TS_WALK_OLD") != nullptr;
    if (!walk_old && sim.person) {
        float lx, ly;
        // (Not for a destination a few sixteenths off: walk_step puts the Sim there without a turn.)
        const bool near = route.path.size() == 1 && std::hypot(float(route.path[0].first) - route.fx,
                                                               float(route.path[0].second) - route.fy) < 3.1f;
        if (!near && sim.walk_aim(route.fx, route.fy, lx, ly) && (lx != route.fx || ly != route.fy)) {
            const float want = std::fmod(std::atan2(lx - route.fx, route.fy - ly) / (3.14159265f / 4) + 8.0f, 8.0f);
            const bool pet = sim.person_data.size() > 65 && sim.person_data[65] >= 2;
            if (std::fabs(std::remainder(want - float(sim.direction), 8.0f)) >= (pet ? 0.8f : 2.0f) &&
                sim.turn_towards(vm, int(std::lround(want)) % 8, 1))
                return Exit::ContinueNextTick;
        }
        sim.start_walk(vm);
        return Exit::ContinueNextTick;
    }
    if (sim.person && route.door_depth == 0)
        for (const auto &[px, py] : route.path)
            if (px != sim.x16 || py != sim.y16) {
                const int way = (int(std::lround(std::atan2(float(px - sim.x16), float(sim.y16 - py)) / (3.14159265f / 4))) + 8) % 8;
                if (sim.turn_towards(vm, way, 1))
                    return Exit::ContinueNextTick;
                break;
            }
    sim.start_walk(vm);
    return Exit::ContinueNextTick;
}

// Which stairs to take, as the original's router goes room to room through portals, stairs among
// them (FUN_0060e690's door trees; FreeSO's room route): of the stairs between the walker's storey
// and `to_level`, the one the walker can walk to whose other end leads to `goal` (its tile or one
// beside it), the shortest way in all. With `via` the goal is on the walker's own storey but cut off
// there: down (or up) one stair and back by another. Ours took the nearest stair as the crow flies:
// lot 75's spiral stairs stand behind railings, and a Sim upstairs sent to the toilets in the other
// half of the floor, reachable only by going down one spiral stair and up another, failed every
// walk ("goto nearby" on the wrong pad, then the balloon; the survey, 2026-11-06). Null: none found
// (the caller keeps the nearest). TS_STAIRS_NEAREST=1: the nearest always, as before;
// TS_STAIRS=1 prints the choice.
Entity *choose_stairs(Vm &vm, const Entity &sim, int to_level, const Entity *goal, bool via)
{
    static const bool nearest_only = TS_ENV("TS_STAIRS_NEAREST") != nullptr;
    if (nearest_only || !goal || !sim.person || to_level < 1 || to_level > kLotLevels)
        return nullptr;
    // The stairs: objects with a portal function (entry 15) with parts on two storeys. Their ends on a
    // storey: the parts there one stands on (the pads), else all of them there.
    struct Stair {
        std::vector<Entity *> parts;
    };
    std::vector<Stair> stairs;
    for (auto &up : vm.entities) {
        Entity &e = *up;
        if (e.dead || e.person || !e.in_world() || e.container || !e.info || e.entry_points.size() <= 15 ||
            e.entry_points[15].action == 0 || e.group.size() < 2)
            continue;
        bool seen = false;
        for (const Stair &s : stairs)
            seen |= std::find(s.parts.begin(), s.parts.end(), &e) != s.parts.end();
        if (seen)
            continue;
        int lo = 99, hi = -1;
        for (const Entity *p : e.group)
            if (p && !p->dead && p->in_world()) {
                lo = std::min(lo, int(p->level));
                hi = std::max(hi, int(p->level));
            }
        if (hi > lo)
            stairs.push_back({ std::vector<Entity *>(e.group.begin(), e.group.end()) });
    }
    auto ends = [](const Stair &s, int level) {
        std::vector<Entity *> out, all;
        for (Entity *p : s.parts)
            if (p && !p->dead && p->in_world() && p->level == level) {
                all.push_back(p);
                if (!blocks_walking(*p))
                    out.push_back(p);
            }
        return out.empty() ? all : out;
    };
    auto touches = [&](const Stair &s, int a, int b) { return !ends(s, a).empty() && !ends(s, b).empty(); };
    std::vector<std::pair<int, int>> goal_tiles;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            goal_tiles.push_back({ goal->tile_x() + dx, goal->tile_y() + dy });
    auto min_of = [](const std::vector<float> &v) {
        float m = 1e30f;
        for (float c : v)
            m = std::min(m, c);
        return m;
    };
    // From an end on `level` to the goal (on `level`): the least walk.
    auto to_goal = [&](const Entity &end, int level) {
        return min_of(walk_costs_from(vm, sim, level, end.tile_x(), end.tile_y(), goal_tiles));
    };
    const int here = sim.level;
    float best = 1e29f;
    const Stair *pick = nullptr;
    // The walker to every stair end on its storey, in one search.
    std::vector<std::pair<int, int>> mine;
    std::vector<std::pair<const Stair *, Entity *>> mine_of;
    for (const Stair &s : stairs)
        for (int other = here - 1; other <= here + 1; other += 2)
            if ((via ? (other >= 1 && other <= kLotLevels) : other == to_level) && touches(s, here, other)) {
                for (Entity *p : ends(s, here)) {
                    mine.push_back({ p->tile_x(), p->tile_y() });
                    mine_of.push_back({ &s, p });
                }
                break;
            }
    const std::vector<float> walk = walk_costs(vm, sim, mine);
    for (size_t i = 0; i < mine_of.size(); i++) {
        if (walk[i] >= 1e29f)
            continue;
        const Stair &s = *mine_of[i].first;
        for (int other = here - 1; other <= here + 1; other += 2) {
            if (other < 1 || other > kLotLevels || (!via && other != to_level) || !touches(s, here, other))
                continue;
            for (Entity *far : ends(s, other)) {
                float rest = 1e30f;
                if (!via) {
                    rest = to_goal(*far, other);
                } else {   // across `other` to another stair back to this storey, then to the goal
                    std::vector<std::pair<int, int>> backs;
                    std::vector<Entity *> back_of;
                    for (const Stair &t : stairs)
                        if (&t != &s && touches(t, other, here))
                            for (Entity *p : ends(t, other)) {
                                backs.push_back({ p->tile_x(), p->tile_y() });
                                back_of.push_back(p);
                            }
                    const std::vector<float> across = walk_costs_from(vm, sim, other, far->tile_x(), far->tile_y(), backs);
                    for (size_t k = 0; k < back_of.size(); k++) {
                        if (across[k] >= 1e29f)
                            continue;
                        for (const Stair &t : stairs)
                            if (std::find(t.parts.begin(), t.parts.end(), back_of[k]) != t.parts.end())
                                for (Entity *home : ends(t, here))
                                    rest = std::min(rest, across[k] + 2 + to_goal(*home, here));
                    }
                }
                if (walk[i] + rest < best) {
                    best = walk[i] + rest;
                    pick = &s;
                }
            }
        }
    }
    if (!pick)
        return nullptr;
    // The part to go by: an end on this storey with the portal function, nearest by walk.
    Entity *part = nullptr;
    float part_cost = 1e30f;
    for (size_t i = 0; i < mine_of.size(); i++)
        if (mine_of[i].first == pick && walk[i] < part_cost && mine_of[i].second->entry_points.size() > 15 &&
            mine_of[i].second->entry_points[15].action != 0) {
            part_cost = walk[i];
            part = mine_of[i].second;
        }
    if (!part)
        for (Entity *p : pick->parts)
            if (p && p->level == here && p->in_world() && p->entry_points.size() > 15 && p->entry_points[15].action != 0) {
                part = p;
                break;
            }
    if (TS_ENV("TS_STAIRS"))   // debug
        std::printf("stairs: #%d at (%.1f, %.1f) level %d to #%d (level %d%s): by #%s at (%d, %d), %.1f tiles\n", int(sim.id),
                    sim.x16 / 16.0, sim.y16 / 16.0, int(sim.level), int(goal->id), int(goal->level), via ? ", round" : "",
                    part ? std::to_string(part->id).c_str() : "-", part ? part->tile_x() : -1, part ? part->tile_y() : -1,
                    double(best));
    return part;
}

// Whether the walker can't get to `goal` (its tile or one beside it) on its own storey.
bool cut_off_from(Vm &vm, const Entity &sim, const Entity &goal)
{
    std::vector<std::pair<int, int>> tiles;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            tiles.push_back({ goal.tile_x() + dx, goal.tile_y() + dy });
    for (float c : walk_costs(vm, sim, tiles))
        if (c < 1e29f)
            return false;
    return true;
}

// A target on another storey: the stairs' portal function (entry point 15,
// as FreeSims' router pushes it) takes the Sim there; the instruction runs
// again when it returns. The stairs that lead to `goal` (choose_stairs), else
// a stair part on this storey whose object has a part on the target storey,
// the nearest one. `via`: the goal is on this storey but cut off (choose_stairs).
Exit take_stairs(Frame &f, int target_level, const Entity *goal = nullptr, bool via = false)
{
    Entity &sim = *f.caller;
    Vm &vm = f.vm();
    auto &route = sim.route;
    const size_t depth = f.thread->stack.size();
    if (route.portal_routine != f.routine || route.portal_ip != f.ip || route.portal_depth != depth) {
        route.portal_routine = f.routine;
        route.portal_ip = f.ip;
        route.portal_depth = depth;
        route.portals = 0;
    }
    if (route.portals++ > 2)
        return Exit::GotoFalse;
    Entity *best = choose_stairs(vm, sim, target_level, goal, via);
    if (via && !best)
        return Exit::GotoFalse;
    int best_d = INT32_MAX;
    const bool chosen = best != nullptr;
    for (auto &up : vm.entities) {
        if (chosen)
            break;
        Entity &e = *up;
        if (e.dead || e.person || !e.in_world() || e.container || e.level != sim.level || !e.info ||
            e.entry_points.size() <= 15 || e.entry_points[15].action == 0)
            continue;
        // A part of this same object on the target storey. (Ours took any object of the same kind
        // there: a door model used on both storeys counted as stairs, and Johnny of House63, going
        // downstairs, went back and forth through the bathroom door for six game hours.)
        bool reaches = false;
        for (const Entity *part : e.group)
            if (part && !part->dead && part->level == target_level && part->in_world()) {
                reaches = true;
                break;
            }
        if (!reaches)
            continue;
        const int d = std::abs(e.x16 - sim.x16) + std::abs(e.y16 - sim.y16);
        if (d < best_d) {
            best_d = d;
            best = &e;
        }
    }
    if (!best)
        return Exit::GotoFalse;
    CodeOwner owner;
    const Routine *r = vm.routine(best->code, best->entry_points[15].action, &owner);
    if (!r)
        return Exit::GotoFalse;
    Frame portal;
    portal.routine = r;
    portal.caller = &sim;
    portal.callee = best;
    portal.owner = owner;
    portal.stack_id = int16_t(best->id);
    portal.action_tree = f.action_tree;
    portal.resume_caller = true;
    portal.args.assign(std::max<size_t>(r->args, 4), 0);
    f.thread->push(std::move(portal));   // `f` may move: don't touch it after this
    return Exit::Continue;
}

// A swimmer going somewhere dry: out by a ladder. The exe's router goes from room to room by their
// portals (objects of type 8 with a portal function: FUN_00632290, then entry 15, as for a door);
// ours had only doors and stairs, so nobody who dived in could get out, and their every walk failed.
// The ladder's code tells its parts by attribute 1: 0 the part in the water, for coming out; 1 the
// part on the side. The nearest; false if there's none.
bool take_ladder(Frame &f, Exit &result)
{
    Entity &sim = *f.caller;
    Vm &vm = f.vm();
    auto &route = sim.route;
    const size_t depth = f.thread->stack.size();
    if (route.portal_routine != f.routine || route.portal_ip != f.ip || route.portal_depth != depth) {
        route.portal_routine = f.routine;
        route.portal_ip = f.ip;
        route.portal_depth = depth;
        route.portals = 0;
    }
    if (route.portals++ > 2)
        return false;
    Entity *best = nullptr;
    int best_d = INT32_MAX;
    for (auto &up : vm.entities) {
        Entity &e = *up;
        if (e.dead || e.person || !e.in_world() || e.container || e.level != sim.level || !e.info ||
            e.info->objd.get(Objd::ObjectType) != 8 || e.entry_points.size() <= 15 || e.entry_points[15].action == 0 ||
            e.attribute(1) != 0 || e.group.empty())
            continue;
        // (A diving board has the same two, but its portal only goes in: a ladder has its approach too.)
        bool reaches_land = false, is_ladder = false;
        for (const Entity *part : e.group) {
            reaches_land = reaches_land || (part->attribute(1) == 1 && !vm.in_pool(*part));
            is_ladder = is_ladder || part->attribute(1) == -1;
        }
        if (!reaches_land || !is_ladder)
            continue;
        const int d = std::abs(e.x16 - sim.x16) + std::abs(e.y16 - sim.y16);
        if (d < best_d) {
            best_d = d;
            best = &e;
        }
    }
    if (!best)
        return false;
    CodeOwner owner;
    const Routine *r = vm.routine(best->code, best->entry_points[15].action, &owner);
    if (!r)
        return false;
    if (TS_ENV("TS_QUEUE_WHY"))   // debug
        std::printf("walk: #%d out of the water by the ladder at (%d, %d)\n", int(sim.id), best->tile_x(), best->tile_y());
    Frame portal;
    portal.routine = r;
    portal.caller = &sim;
    portal.callee = best;
    portal.owner = owner;
    portal.stack_id = int16_t(best->id);
    portal.action_tree = f.action_tree;
    portal.resume_caller = true;
    portal.args.assign(std::max<size_t>(r->args, 4), 0);
    f.thread->push(std::move(portal));   // `f` may move: don't touch it after this
    result = Exit::Continue;
    return true;
}

// The door the walk is about to go through: the next step crosses a wall where a door stands
// (an object with a portal function, entry 15, on either side of that wall), else null.
Entity *door_ahead(Vm &vm, const Entity &sim)
{
    const auto &route = sim.route;
    if (route.next >= route.path.size())
        return nullptr;
    const int cx = sim.x16 / 16, cy = sim.y16 / 16;
    const int tx = route.path[route.next].first / 16, ty = route.path[route.next].second / 16;
    const int sx = (tx > cx) - (tx < cx), sy = (ty > cy) - (ty < cy);
    if ((sx == 0) == (sy == 0))
        return nullptr;   // staying in the tile, or a diagonal step (never through a door)
    const int nx = cx + sx, ny = cy + sy;
    if (!wall_between(vm, cx, cy, nx, ny, sim.level))
        return nullptr;
    // A door is two parts, one each side of its wall, and its code tells the sides apart by the
    // part it runs on (part 1's attribute 1, "test reverse orientation"): the part on the Sim's
    // side, in the room they're in, as FreeSO's router picks its room portals. (Ours took the
    // first found: from one side, the door sent the Sim back where they came from.)
    Entity *far_part = nullptr;
    for (auto &up : vm.entities) {
        Entity &e = *up;
        if (e.dead || e.person || !e.in_world() || e.container || e.level != sim.level ||
            e.entry_points.size() <= 15 || e.entry_points[15].action == 0)
            continue;
        if (e.tile_x() == cx && e.tile_y() == cy)
            return &e;
        if (e.tile_x() == nx && e.tile_y() == ny && !far_part)
            far_part = &e;
    }
    return far_part;
}

// Through a door: the walk stops and the door's portal function (entry 15, as FreeSO's
// VMRoutingFrame pushes it) takes the Sim to the door, opens it (its picture and sound),
// walks them through and closes it; the walk instruction runs again when it returns, from
// the other side. Once a walk: if its code didn't get the Sim through (someone in the
// doorway, a door that won't open), the walk goes on through, so nobody is ever stuck there.
Exit use_door(Frame &f, Entity &door)
{
    Entity &sim = *f.caller;
    auto &route = sim.route;
    const size_t depth = f.thread->stack.size();
    if (route.portal_routine != f.routine || route.portal_ip != f.ip || route.portal_depth != depth ||
        route.portal_door != int16_t(door.id)) {
        route.portal_routine = f.routine;
        route.portal_ip = f.ip;
        route.portal_depth = depth;
        route.portal_door = int16_t(door.id);
        route.portals = 0;
    }
    if (route.portals++ > 0) {
        // The door's code didn't get them through (someone on its far side, a door that won't open
        // for them, a Sim standing in the doorway itself): that door is shut for the rest of this
        // walk and the walk is planned again without it, by another door, as the exe's room route
        // does (FUN_005f7950 case 2: FUN_006321d0 marks the failed portal, the next is taken; none:
        // no room route). A Sim in the doorway walks off it on the new plan. Ours stepped them a
        // tile on through at once ("jumps"; the walls audit, 2026-11-04). TS_DOOR_STEP=1: as before.
        static const bool old_step = TS_ENV("TS_DOOR_STEP") != nullptr;
        if (old_step && sim.tile_x() == door.tile_x() && sim.tile_y() == door.tile_y() && route.next < route.path.size()) {
            sim.set_position(route.path[route.next].first, route.path[route.next].second, sim.level);
            route.next++;
            route.portal_routine = nullptr;
            route.portals = 0;
            return Exit::ContinueNextTick;
        }
        if (!old_step) {
            if (route.shut_routine != f.routine || route.shut_ip != f.ip || route.shut_depth != depth) {
                route.shut_routine = f.routine;
                route.shut_ip = f.ip;
                route.shut_depth = depth;
                route.shut_doors.clear();
            }
            if (std::find(route.shut_doors.begin(), route.shut_doors.end(), int16_t(door.id)) == route.shut_doors.end()) {
                route.shut_doors.push_back(int16_t(door.id));   // (and its other parts: the whole door)
                for (const Entity *part : door.group)
                    if (part && part != &door)
                        route.shut_doors.push_back(int16_t(part->id));
                if (TS_ENV("TS_DOORS"))   // debug
                    std::printf("door #%d: #%d can't get through: shut for this walk, planned again\n", int(door.id),
                                int(sim.id));
                            route.state = Entity::Route::Idle;
                route.path.clear();
                route.blocked_by = 0;
                route.paused = false;
                route.portal_routine = nullptr;
                play_standing(f.vm(), sim);
                return Exit::Continue;   // the walk instruction again: a fresh plan without that door
            }
        }
        // The door's own code didn't get the Sim through (someone on its far side, a door that
        // won't open for them): the walk fails there, as the original's (Doors.iff "portal
        // function" calls "fail portal call" and returns false). Ours walked them on through the
        // shut door "so nobody is stuck" (the user saw Bella do it); a Sim standing on the far
        // side is now asked to step aside first (wait_for_spot).
        if (TS_ENV("TS_DOORS"))   // debug
            std::printf("door #%d: #%d can't get through (its code ended %s; #%d at (%.2f, %.2f))\n", int(door.id),
                        int(sim.id),
                        f.thread->last_exit == Exit::ReturnTrue    ? "true"
                        : f.thread->last_exit == Exit::ReturnFalse ? "false"
                                                                   : "in an error",
                        int(sim.id), sim.x16 / 16.0, sim.y16 / 16.0);
        route.state = Entity::Route::Idle;
        route.path.clear();
        route.blocked_by = 0;
        route.paused = false;
        route.portal_routine = nullptr;   // the next walk tries the door afresh
        play_standing(f.vm(), sim);
        route.last_failure = 13;   // a person in the way (the commonest reason)
        return Exit::GotoFalse;
    }
    Vm &vm = f.vm();
    CodeOwner owner;
    const Routine *r = vm.routine(door.code, door.entry_points[15].action, &owner);
    // Its condition first, as FreeSO's PushEntryPoint does: the door's own check works out
    // which side the Sim comes from (its attribute 1, which "test reverse orientation" reads);
    // without it the door sent everyone to the same side, back where they came from, and they
    // walked through it with no door opening.
    bool may = r != nullptr;
    if (may && door.entry_points[15].condition != 0) {
        CodeOwner test_owner;
        if (const Routine *test = vm.routine(door.code, door.entry_points[15].condition, &test_owner)) {
            Frame check;
            check.routine = test;
            check.caller = &sim;
            check.callee = &door;
            check.owner = test_owner;
            check.stack_id = int16_t(door.id);
            check.args.assign(std::max<size_t>(test->args, 4), 0);
            check.locals.assign(test->locals, 0);
            may = f.thread->run_now(std::move(check)) == Exit::ReturnTrue;
        }
    }
    if (TS_ENV("TS_DOORS"))   // debug
        std::printf("door #%d: #%d runs its portal code (%s)%s, side %d (depth %lu, door depth %lu)\n", int(door.id), int(sim.id),
                    r ? r->name.c_str() : "none", may ? "" : ", its check says no",
                    door.attributes.size() > 1 ? int(door.attributes[1]) : -1, (unsigned long)depth,
                    (unsigned long)route.door_depth);
    if (!may)
        return Exit::ContinueNextTick;   // walk on through (never stuck)
    if (!r)
        return Exit::ContinueNextTick;   // no portal code: walk on through
    route.state = Entity::Route::Idle;
    route.path.clear();
    route.door_depth = depth;
    Frame portal;
    portal.routine = r;
    portal.caller = &sim;
    portal.callee = &door;
    portal.owner = owner;
    portal.stack_id = int16_t(door.id);
    portal.action_tree = f.action_tree;
    portal.resume_caller = true;
    portal.args.assign(std::max<size_t>(r->args, 4), 0);
    f.thread->push(std::move(portal));   // `f` may move: don't touch it after this
    return Exit::Continue;
}

// The route state for this instruction: walking, done, or not started.
// The walk planned again from here to where it was going (another Sim in the way).
void plan_again(Vm &vm, Entity &sim)
{
    auto &route = sim.route;
    std::vector<std::pair<int16_t, int16_t>> way;
    const auto [dx, dy] = route.path.back();
    if (find_path(vm, sim, dx, dy, way, route.target_id ? vm.entity(route.target_id) : nullptr) && !way.empty()) {
        route.path = std::move(way);
        route.next = 0;
        route.seg_set = false;
        route.to_corner = false;
        route.corner_used = false;
    }
    route.blocked_by = 0;
    route.paused = false;
    route.velocity = 0;
}

// Asking `blocker` to step out of the way (the original's FUN_005f3830): a Destination made
// where they stand, its spot one to five tiles off to either side of the walker's way, and its
// "Move out of way" (interaction 3) queued on them at the walker's priority. Not if they're
// asking someone themselves, or already have one of these.
// `asked_already`: someone's request that they move is still with them (the walk waits for
// it as if it were its own).
// `all_round` (the snap's: FUN_005f3640): any of the four sides instead, the walker having no way.
bool ask_to_move(Vm &vm, Entity &sim, Entity &blocker, bool *asked_already, bool all_round)
{
    constexpr uint32_t kDestinationGuid = 0x7C4;
    if (asked_already)
        *asked_already = false;
    if (!blocker.thread || blocker.route.block_stage == 2)
        return false;
    for (const auto &e : vm.entities)
        if (!e->dead && e->slot_override && e->data[VarTargetId] == int16_t(blocker.id)) {
            if (asked_already)
                *asked_already = true;
            return false;
        }
    const ObjectInfo *info = vm.content.object(kDestinationGuid);
    if (!info)
        return false;
    Entity *dest = vm.create_with_parts(*info, blocker.tile_x(), blocker.tile_y(), blocker.level - 1, 0);
    if (!dest)
        return false;
    dest->x16 = blocker.x16;
    dest->y16 = blocker.y16;
    dest->run_entry(vm, 0, true);   // init
    dest->data[VarTargetId] = int16_t(blocker.id);   // (ours: whose it is)
    // The way the walker goes through them, and the two sides of it.
    const double angle = std::atan2(double(blocker.x16 - sim.x16), double(sim.y16 - blocker.y16));
    const int d = (int(std::lround(angle / (3.14159265358979 / 4))) + 8) % 8;
    auto slot = std::make_shared<SlotItem>();
    slot->type = 3;
    slot->flags = (all_round ? 1u | 4u | 16u | 64u : (1u << ((d + 2) & 7)) | (1u << ((d + 6) & 7))) | 0x100 | 0x2000;
    slot->min_proximity = 16;
    slot->optimal_proximity = 16;
    slot->max_proximity = 80;
    slot->resolution = 8;
    slot->facing = -3;
    slot->standing = 1;
    dest->slot_override = slot;
    QueuedAction a;
    if (!dest->action(vm, 3, int16_t(dest->id), a)) {
        Frame none;
        none.caller = &sim;
        remove_entity(vm, dest, none);
        return false;
    }
    a.priority = std::max<int16_t>(1, sim.person_data[33]);
    a.must_run = true;
    if (TS_ENV("TS_QUEUE_WHY"))   // debug
        std::printf("walk: #%d asks #%d to move out of the way (#%d)\n", int(sim.id), int(blocker.id), int(dest->id));
    blocker.thread->enqueue(std::move(a));
    return true;
}

// A global (the Sim's own semi-global) routine pushed for the walking instruction, which runs
// again when it returns.
Exit push_person_routine(Frame &f, uint16_t id)
{
    Entity &sim = *f.caller;
    Vm &vm = f.vm();
    CodeOwner owner;
    const Routine *r = vm.routine(sim.code, id, &owner);
    // Cats and dogs have no such routines: their semi-globals number other things the same (8276
    // is "Interaction - Pet" there). A pet held up just waits; run, every pet blocked on a walk made
    // a social with nobody and queued it, hundreds an hour, the Kats' lot's ticks nearly doubled.
    if (!r || r->name.rfind("route ", 0) != 0)
        return Exit::ContinueNextTick;
    Frame frame;
    frame.routine = r;
    frame.caller = &sim;
    frame.callee = &sim;
    frame.owner = owner;
    frame.stack_id = sim.route.blocked_by;
    frame.action_tree = f.action_tree;
    frame.resume_caller = true;
    frame.args.assign(std::max<size_t>(r->args, 4), 0);
    frame.locals.assign(r->locals, 0);
    f.thread->push(std::move(frame));   // `f` may move: don't touch it after this
    return Exit::Continue;
}

// Another Sim in the walk's way, as the original's routing frame deals with it (FUN_005f7950,
// states 6 and 0xe): the first time at a place, the way is planned again (other Sims within
// three tiles count as obstacles); blocked again there, the walker asks them to move (its
// "route ask to move animation") and waits up to 100 ticks ("route waiting animation"), then
// plans again; blocked once more, the walk fails. Moving on more than half a tile starts afresh.
Exit walk_blocked(Frame &f)
{
    Entity &sim = *f.caller;
    Vm &vm = f.vm();
    auto &route = sim.route;
    constexpr uint16_t kRouteWaiting = 8276, kAskToMove = 8302;
    if (route.block_stage == 2) {
        Entity *asked = vm.entity(route.asked);
        const bool still = asked && !asked->dead && route.blocked_by &&
                           vm.person_in_the_way(sim, float(route.path[route.next].first), float(route.path[route.next].second));
        if (vm.ticks - route.ask_start < 100 && (still || vm.ticks - route.ask_start < 8)) {
            if (route.waited_at == vm.ticks)   // (once a tick: see wait_for_spot)
                return Exit::ContinueNextTick;
            route.waited_at = vm.ticks;
            return push_person_routine(f, kRouteWaiting);
        }
        route.block_stage = 3;
        plan_again(vm, sim);
        return Exit::ContinueNextTick;
    }
    // Two walkers each blocked by the other: the one with the higher id stands still a while and the
    // other plans round it (a standing Sim is an obstacle to its plan). Both planning at once
    // stepped the same way together, every time: Mortimer and the Mystery Man side-stepped into
    // one row and back again for 2,300 ticks, each step a "fresh" block that never got to asking
    // (the Goths' dinner, 2026-11-03). Past 60 ticks it plans again itself.
    // (Not "blocked by each other" as such: the lower one, handled first, has planned again by then.)
    // (And two blocking each other in a doorway, neither walking for long: each retried every few
    // ticks for ever, 374 failed walks on lot 27; the same rule settles it.)
    if (Entity *other = vm.entity(route.blocked_by);
        other && other->person && sim.id > other->id &&
        (other->route.state == Entity::Route::Walking || other->route.blocked_by == int16_t(sim.id))) {
        if (route.yield_start == 0) {
            route.yield_start = vm.ticks;
            play_standing(vm, sim);
        }
        if (vm.ticks - route.yield_start < 60)
            return Exit::ContinueNextTick;
    }
    route.yield_start = 0;
    const int mx = sim.x16 - route.block_x, my = sim.y16 - route.block_y;
    if (route.block_stage == 0 || mx * mx + my * my > 64) {
        route.block_x = sim.x16;
        route.block_y = sim.y16;
        route.block_stage = 1;
        if (TS_ENV("TS_QUEUE_WHY"))   // debug
            std::printf("walk: #%d blocked by #%d, plans again (tick %u, at %.2f, %.2f)\n", int(sim.id),
                        int(route.blocked_by), vm.ticks, sim.x16 / 16.0, sim.y16 / 16.0);
        plan_again(vm, sim);
        return Exit::ContinueNextTick;
    }
    Entity *blocker = vm.entity(route.blocked_by);
    bool asked_already = false;
    if (route.block_stage == 1 && blocker && sim.person_data[0] == 0 && ask_to_move(vm, sim, *blocker, &asked_already)) {
        route.block_stage = 2;
        route.asked = int16_t(blocker->id);
        route.ask_start = vm.ticks;
        route.paused = true;
        play_standing(vm, sim);
        return push_person_routine(f, kAskToMove);
    }
    // Already asked to move (by someone else, or by us a moment ago), or waiting on a request of
    // their own: waited for as if asked, up to the usual hundred ticks, not given up at once. Ours
    // gave up in two ticks and the game's code walked again at once, round and round (a guest at
    // lot 26's fridge 319 times, blocked by one who waited on another; the second audit's survey).
    // A blocker still walking (passing by; a pet, which can't be asked): waited for as well. With each
    // walker at its own stride's pace (Route::stride_scale) a dog or a child takes longer to pass, and
    // a walk blocked by one gave up at once (route failures +30% on the survey, pets' "move from
    // filled tile" 171 -> 1017). TS_WAIT_WALKER_OLD=1: as before.
    static const bool wait_walker_old = TS_ENV("TS_WAIT_WALKER_OLD") != nullptr;
    const bool passing = !wait_walker_old && blocker && blocker->route.state == Entity::Route::Walking;
    if (route.block_stage == 1 && blocker && blocker->person && (sim.person_data[0] == 0 || passing) &&
        (asked_already || passing || blocker->route.block_stage == 2 || blocker->route.opening_asked != 0)) {
        route.block_stage = 2;
        route.asked = int16_t(blocker->id);
        route.ask_start = vm.ticks;
        route.paused = true;
        play_standing(vm, sim);
        return Exit::ContinueNextTick;
    }
    // The one asked to move still on their way aside, across the new plan (their Move out of way walk):
    // waited for again, up to three times, not failed. A guest stepping in at Downtown's edge met the
    // one who was making room for her twice and her meal was dropped by the game's own code (Lisa,
    // the user's main Vita, 2026-11-14: "any way to prevent this"). TS_REWAIT_OLD=1: failed, as before.
    static const bool rewait_old = TS_ENV("TS_REWAIT_OLD") != nullptr;
    if (!rewait_old && route.block_stage == 3 && blocker && blocker->person && blocker->id == uint16_t(route.asked) &&
        blocker->route.state == Entity::Route::Walking && route.rewaits < 3) {
        route.rewaits++;
        route.block_stage = 2;
        route.ask_start = vm.ticks;
        route.paused = true;
        play_standing(vm, sim);
        if (TS_ENV("TS_QUEUE_WHY"))   // debug
            std::printf("walk: #%d waits again for #%d, still stepping aside\n", int(sim.id), int(blocker->id));
        return Exit::ContinueNextTick;
    }
    // Nothing more to try: the walk fails.
    if (TS_ENV("TS_QUEUE_WHY"))   // debug
        std::printf("walk: #%d stays blocked by #%d, gives up\n", int(sim.id), int(route.blocked_by));
    route.state = Entity::Route::Idle;
    route.path.clear();
    route.blocked_by = 0;
    route.paused = false;
    route.block_stage = 0;
    play_standing(vm, sim);
    route.last_failure = 1;
    return Exit::GotoFalse;
}

Exit route_progress(Frame &f, bool &started)
{
    auto &route = f.caller->route;
    started = route.state != Entity::Route::Idle && route.routine == f.routine && route.ip == f.ip &&
              route.depth == f.thread->stack.size();
    if (!started)
        return Exit::Continue;
    if (route.state == Entity::Route::Walking) {
        // Something more important was queued (the interaction is cancelled): the walk
        // stops, a failed route the interaction ends on (FreeSO VMRoutingFrame, unless
        // person data 71, NonInterruptable, says not to).
        Entity &sim = *f.caller;
        if (sim.person && sim.thread && sim.thread->input_waiting()) {
            route.state = Entity::Route::Idle;
            route.path.clear();
            play_standing(f.vm(), sim);
            // A Sim running its own Main's code (no interaction active: an NPC's main calling its work
            // trees straight, the maid's "clean up") has nothing that would yield to what waits, and
            // its queue is served only when its code idles: cut, its walk failed, its code tried again
            // at once, the Move still queued (Tidy Heidi cleaning a toilet, asked to move: 13,000
            // failed walks on lot 71, 2026-11-06). The waiting interaction starts here instead, on top
            // (as Idle For Input lets it: attempt_push), and this walk is planned afresh from wherever
            // it leaves the Sim. TS_CUT_MAIN_WALKS=1: cut as any other, as before.
            static const bool cut_main = TS_ENV("TS_CUT_MAIN_WALKS") != nullptr;
            if (!cut_main && !f.action_tree && sim.thread->attempt_push()) {
                if (TS_ENV("TS_QUEUE_WHY"))   // debug
                    std::printf("walk: #%d's walk gives way to what it was asked (from its main)\n", int(sim.id));
                return Exit::Continue;   // (`f` may have moved: nothing of it touched)
            }
            route.last_failure = 4;   // interrupted
            return Exit::GotoFalse;
        }
        // Another Sim in the way of the next step.
        if (sim.person && (route.blocked_by || route.block_stage == 2) && route.next < route.path.size() &&
            !TS_ENV("TS_PEOPLE_PASS"))
            return walk_blocked(f);
        // A door in the way: through it by its own code (not from inside a door's own walks), unless
        // its code already failed this walk (shut: the plan goes round it; standing in its doorway,
        // the Sim walks off it).
        if (sim.person && (route.door_depth == 0 || f.thread->stack.size() <= route.door_depth))
            if (Entity *door = door_ahead(f.vm(), sim);
                door && std::find(route.shut_doors.begin(), route.shut_doors.end(), int16_t(door->id)) == route.shut_doors.end())
                return use_door(f, *door);
        return Exit::ContinueNextTick;
    }
    const bool ok = route.state == Entity::Route::Arrived;
    route.state = Entity::Route::Idle;
    route.last_failure = ok ? 0 : 3;   // no path
    return truth(ok);
}

// Routing slots (SLOT type 3) of an object, or the global ones (global.iff SLOT 100).
std::vector<const SlotItem *> routing_slots(Vm &vm, Entity &obj, bool global)
{
    static std::vector<SlotItem> globals;
    static bool globals_read = false;
    std::vector<const SlotItem *> out;
    if (global) {
        if (!globals_read) {
            globals_read = true;
            if (const LoadedIff *g = vm.global_code.own)
                if (const IffChunk *c = g->iff.find("SLOT", 100)) {
                    std::string err;
                    parse_slot(g->body(*c), c->size, globals, err);
                }
        }
        for (const SlotItem &s : globals)
            if (s.type == 3)
                out.push_back(&s);
        return out;
    }
    for (const SlotItem &s : obj.slot_items(vm))
        if (s.type == 3)
            out.push_back(&s);
    return out;
}

// A walk that can't be made: the game's own feedback (global 398 "route failure feedback": the
// can't-get-there balloon over the Sim, a shrug or a look at what's in the way), with why
// (FreeSO VMRouteFailCode) and what blocks it, as FreeSO's router calls it unless the
// instruction says no failure trees; then the instruction fails. Person data 62 keeps why.
Exit route_failed(Frame &f, int code, const Entity *blocker, bool failure_trees)
{
    Entity &sim = *f.caller;
    if (sim.person && sim.person_data.size() > 62)
        sim.person_data[62] = int16_t(code);
    if (TS_ENV("TS_QUEUE_WHY"))   // debug
        std::printf("route failed: #%d, why %d%s%s%s [%s, to #%d %s]\n", int(sim.id), code,
                    failure_trees && sim.person ? " (the balloon)" : "", blocker ? ", blocked by #" : "",
                    blocker ? std::to_string(blocker->id).c_str() : "", f.routine ? f.routine->name.c_str() : "?",
                    int(f.stack_id), f.stack_object() && f.stack_object()->info ? f.stack_object()->info->label.c_str() : "");
    if (!failure_trees || !sim.person || code == 4)
        return Exit::GotoFalse;
    Vm &vm = f.vm();
    CodeOwner owner;
    const Routine *r = vm.routine(f.owner, 398, &owner);
    if (!r)
        return Exit::GotoFalse;
    auto &route = sim.route;
    route.fail_routine = f.routine;
    route.fail_ip = f.ip;
    route.fail_depth = f.thread->stack.size();
    Frame feedback;
    feedback.routine = r;
    feedback.caller = &sim;
    feedback.callee = &sim;
    feedback.owner = owner;
    feedback.stack_id = f.stack_id;
    feedback.action_tree = f.action_tree;
    feedback.resume_caller = true;
    feedback.args.assign(std::max<size_t>(r->args, 4), 0);
    feedback.args[0] = int16_t(code);
    feedback.args[1] = blocker ? int16_t(blocker->id) : 0;
    feedback.locals.assign(r->locals, 0);
    f.thread->push(std::move(feedback));   // `f` may move: don't touch it after this
    return Exit::Continue;
}

// A Sim standing on the spot a walk goes to (two Sims meeting at a door, each on the other's
// side): asked to step aside and waited for, up to 100 ticks, as a Sim in the way of a walk is
// (FUN_005f7950, its ask and waiting animations), then the walk fails. Ours failed at once, and
// two Sims at one door tried again and again for the rest of the day (the original's door code
// fails its walk there too). When they wait on each other the later one gives way: its walk
// fails now, and the "move out of way" the other asked for runs.
Exit wait_for_spot(Frame &f, Entity &in_way, bool failure_trees)
{
    constexpr uint16_t kRouteWaiting = 8276, kAskToMove = 8302;
    Entity &sim = *f.caller;
    Vm &vm = f.vm();
    auto &route = sim.route;
    bool fresh = route.spot_asked != int16_t(in_way.id);
    const bool mutual = in_way.route.spot_asked == int16_t(sim.id);
    if (fresh && !mutual && sim.person_data[0] == 0) {
        bool asked_already = false;
        if (ask_to_move(vm, sim, in_way, &asked_already)) {
            if (TS_ENV("TS_QUEUE_WHY"))   // debug
                std::printf("walk: #%d waits for #%d to leave its spot\n", int(sim.id), int(in_way.id));
            route.spot_asked = int16_t(in_way.id);
            route.spot_ask_start = vm.ticks;
            play_standing(vm, sim);
            return push_person_routine(f, kAskToMove);
        }
        // Asked already (by this walk a moment ago: the asking animation's own goto cleared the
        // mark, and the next look failed at once instead of waiting; Bella at the bathroom door
        // with the maid passing through it, "confused", 2026-10-30), or by another Sim: the wait
        // goes on from that request, the usual hundred ticks from the last ask of ours.
        if (asked_already) {
            if (route.spot_asked == 0 || vm.ticks - route.spot_ask_start >= 100)
                route.spot_ask_start = vm.ticks;
            route.spot_asked = int16_t(in_way.id);
            fresh = false;
            if (TS_ENV("TS_QUEUE_WHY") && vm.ticks % 50 == 0)   // debug
                std::printf("walk: #%d waits on an earlier request to #%d (at %.1f, %.1f)\n", int(sim.id), int(in_way.id),
                            in_way.x16 / 16.0, in_way.y16 / 16.0);
        }
    }
    if (!fresh && vm.ticks - route.spot_ask_start < 100) {
        if (TS_ENV("TS_QUEUE_WHY") && vm.ticks % 50 == 0)   // debug
            std::printf("walk: #%d still waiting for #%d (at %.1f, %.1f) to leave its spot\n", int(sim.id), int(in_way.id),
                        in_way.x16 / 16.0, in_way.y16 / 16.0);
        // The waiting animation once a tick, the walk looking again the next (as the original's
        // routing frame waits, a tick at a time). The animation ends at once when something waits
        // ("notified"), and ours ran it again in the same tick without end; the engine cut the
        // Sim's code off, here in the middle of getting into a bath: the tub's exit never ran, it
        // stayed "occupied" and nobody in the house could bathe again (the Goths left alone ten
        // days: hygiene -100 from day 3, Cassandra taken away, 2026-10-30).
        if (route.waited_at == vm.ticks)
            return Exit::ContinueNextTick;
        route.waited_at = vm.ticks;
        return push_person_routine(f, kRouteWaiting);
    }
    route.spot_asked = 0;
    return route_failed(f, 13, &in_way, failure_trees);   // a person in the way
}

// Back from the route failure feedback for this instruction: it fails.
bool back_from_failure(Frame &f)
{
    auto &route = f.caller->route;
    if (route.fail_routine != f.routine || route.fail_ip != f.ip || route.fail_depth != f.thread->stack.size())
        return false;
    route.fail_routine = nullptr;
    return true;
}

Exit goto_routing_slot_walk(Frame &f, const uint8_t *op)
{
    if (f.thread->is_check)
        return Exit::GotoFalse;
    if (back_from_failure(f))
        return Exit::GotoFalse;
    const bool failure_trees = !(op[4] & 1);   // FreeSO NoFailureTrees
    bool started;
    // (Before the call: it can push a routine and move the stack `f` is in. A lot 21 crash.)
    const size_t my_depth = f.thread->stack.size();
    const Exit progress = route_progress(f, started);
    if (started) {
        if (progress == Exit::GotoFalse && f.caller->route.last_failure) {
            // The spot couldn't be reached (a door that wouldn't open for them, a Sim in the way
            // who wouldn't move, no path): another spot of the slot before the walk is given up,
            // as the original's router tries its goals in turn. Cassandra on the bathroom doorway
            // tile with a puddle on it: "Clean Up" chose a spot in the hall, through the door the
            // puddle held shut, and failed there over and over, with spots inside the bathroom
            // untried (2026-10-30).
            auto &rt = f.caller->route;
            const int why = rt.last_failure;
            // The object cut off on this storey altogether (no path to it nor to any tile beside it):
            // round by the stairs, down one and up another (choose_stairs), as the original's room
            // route goes through any portal. TS_STAIRS_NEAREST=1: not, as before.
            if (why == 3 && f.caller->person) {
                Entity *goal = f.stack_object();
                if (goal && goal->in_world() && goal->level == f.caller->level && !TS_ENV("TS_STAIRS_NEAREST") &&
                    cut_off_from(f.vm(), *f.caller, *goal)) {
                    rt.last_failure = 0;
                    const Exit e = take_stairs(f, f.caller->level, goal, true);   // (`f` may move: returned at once)
                    if (e != Exit::GotoFalse)
                        return e;
                    rt.last_failure = why;
                }
            }
            auto &tries = rt.spot_tries_of(f.routine, f.ip, my_depth);
            if ((why == 13 || why == 3) && tries.failed.size() < 4) {
                tries.failed.push_back({ tries.goal_x, tries.goal_y });
                if (TS_ENV("TS_QUEUE_WHY"))   // debug
                    std::printf("goto routing slot: #%d couldn't reach (%d, %d) (why %d): another spot\n", int(f.caller->id),
                                tries.goal_x / 16, tries.goal_y / 16, why);
                rt.last_failure = 0;
                return Exit::Continue;   // this instruction again: a fresh search
            }
            rt.spot_tries_done(f.routine, f.ip, my_depth);
            return route_failed(f, why, nullptr, failure_trees);
        }
        if (progress == Exit::GotoTrue)
            f.caller->route.spot_tries_done(f.routine, f.ip, my_depth);
        return progress;
    }
    Entity *obj = f.stack_object();
    if (!obj || !obj->in_world() || !f.caller->in_world())
        return Exit::GotoFalse;
    const int data = u16(op, 0), scope = u16(op, 2);
    Vm &vm = f.vm();
    // The slot (FreeSO VMSlotScope): 0 the one a parameter names, 1 this one,
    // 2 a global one (global.iff SLOT 100); the object's routing slots are
    // its type 3 items, numbered among themselves.
    std::vector<const SlotItem *> routing = routing_slots(vm, *obj, scope == 2);
    const int index = scope == 0 ? (size_t(data) < f.args.size() ? f.args[size_t(data)] : -1) : data;
    // A Destination sending a Sim out of another's way: its slot 0 is the spot to step to.
    if (obj->slot_override && scope != 2 && index == 0) {
        if (routing.empty())
            routing.push_back(nullptr);
        routing[0] = obj->slot_override.get();
    }
    if (index < 0 || size_t(index) >= routing.size())
        return Exit::GotoFalse;
    if (obj->level != f.caller->level)
        return take_stairs(f, obj->level, obj);
    Entity &sim = *f.caller;
    auto &route = sim.route;
    const size_t depth = f.thread->stack.size();
    if (route.sit_routine == f.routine && route.sit_ip == f.ip && route.sit_depth == depth) {
        // Back from the seat's sit function: sat down in it, or not. Not: the route failure
        // feedback, as the exe's routing frame goes on to it (FUN_005f7950 case 4: the code the
        // sit's own walk left in person data 62, or 5 "can't sit"); ours ended silently.
        const Entity *seat = route.sit_seat;
        route.sit_routine = nullptr;
        route.sit_seat = nullptr;
        if (sim.person && seat && sim.container == seat && sim.person_data[0] == 1)
            return Exit::GotoTrue;
        const int left = sim.person_data.size() > 62 ? sim.person_data[62] : 0;
        return route_failed(f, left > 0 ? left : 5, seat, failure_trees);
    }
    int16_t x, y;
    int face;
    int entry = 0;
    Entity *seat = nullptr;
    Entity *in_way = nullptr;
    int fail_code = 1;
    Entity *fail_blocker = nullptr;
    const bool found = slot_spot_scored(*obj, *f.caller, *routing[size_t(index)], x, y, face, &entry, vm,
                                        sim.person ? &seat : nullptr, &in_way,
                                        &sim.route.spot_tries_of(f.routine, f.ip, my_depth).failed, &fail_code,
                                        &fail_blocker);
    if (!found) {
        if (sim.person && in_way && in_way->person)
            return wait_for_spot(f, *in_way, failure_trees);
        if (TS_ENV("TS_QUEUE_WHY"))   // debug
            std::printf("goto routing slot: no spot for slot %d of #%d at (%d, %d) facing %d\n", index, int(obj->id),
                        obj->tile_x(), obj->tile_y(), int(obj->direction));
        if (TS_ENV("TS_QUEUE_WHY"))   // debug: where the Sims are
            for (auto &up : vm.entities)
                if (up->person && up->in_world())
                    std::printf("   #%d at (%.2f, %.2f) level %d\n", int(up->id), up->x16 / 16.0, up->y16 / 16.0,
                                int(up->level));
        // A route that can't start still takes a tick to fail (FreeSO's routing frame fails on its
        // own turn): at once, a musician's Wander went round its loop 20,000 times a tick.
        if (!sim.route.no_spot_waited) {
            sim.route.no_spot_waited = true;
            return Exit::ContinueNextTick;
        }
        sim.route.no_spot_waited = false;
        // Why, as FUN_0060d440 ranks it over the spots it tried: an object 8 (the balloon shows it),
        // a wall or another room 11 (12 is uneven ground, which ours doesn't test); nothing to say 1,
        // no balloon.
        return route_failed(f, fail_code, fail_code == 8 || fail_code == 9 ? fail_blocker : nullptr, failure_trees);
    }
    if (seat) {
        // The seat takes over (FreeSims VMRoutingFrame): its sit function walks the Sim to it and
        // sits; this instruction runs again when it returns and says whether they sat.
        if (sim.container == seat && sim.person_data[0] == 1)
            return Exit::GotoTrue;   // already sitting in it
        CodeOwner owner;
        const ObjFunction &sit = seat->entry_points[26];
        const Routine *r = vm.routine(seat->code, sit.action, &owner);
        bool may = r != nullptr;
        if (may && sit.condition != 0) {
            CodeOwner test_owner;
            if (const Routine *test = vm.routine(seat->code, sit.condition, &test_owner)) {
                Frame check;
                check.routine = test;
                check.caller = &sim;
                check.callee = seat;
                check.owner = test_owner;
                check.stack_id = int16_t(seat->id);
                check.args.assign(std::max<size_t>(test->args, 4), 0);
                check.locals.assign(test->locals, 0);
                may = f.thread->run_now(std::move(check)) == Exit::ReturnTrue;
            }
        }
        if (TS_ENV("TS_QUEUE_WHY"))   // debug
            std::printf("goto routing slot: #%d sits in #%d %s for #%d%s\n", int(sim.id), int(seat->id),
                        seat->info ? seat->info->label.c_str() : "?", int(obj->id), may ? "" : " (its sit check says no)");
        if (!may)
            return route_failed(f, 5, seat, failure_trees);   // can't sit
        route.sit_routine = f.routine;
        route.sit_ip = f.ip;
        route.sit_depth = depth;
        route.sit_seat = seat;
        Frame frame;
        frame.routine = r;
        frame.caller = &sim;
        frame.callee = seat;
        frame.owner = owner;
        frame.stack_id = int16_t(seat->id);
        frame.action_tree = f.action_tree;
        frame.resume_caller = true;
        frame.args.assign(std::max<size_t>(r->args, 4), 0);
        frame.locals.assign(r->locals, 0);
        f.thread->push(std::move(frame));   // `f` may move: don't touch it after this
        return Exit::Continue;
    }
    if (Entity *in_way = f.caller->person ? spot_taken(vm, *f.caller, x, y, f.caller->level) : nullptr) {
        if (TS_ENV("TS_QUEUE_WHY"))   // debug
            std::printf("goto routing slot: #%d's spot by #%d is taken by #%d\n", int(f.caller->id), int(obj->id),
                        int(in_way->id));
        return wait_for_spot(f, *in_way, failure_trees);   // a person there
    }
    f.caller->route.spot_asked = 0;
    if (f.caller->person)
        f.caller->person_data[37] = int16_t(entry);   // which side it came to
    {
        auto &tries = f.caller->route.spot_tries_of(f.routine, f.ip, my_depth);
        tries.goal_x = x;
        tries.goal_y = y;
    }
    const Exit walking = walk_to(f, x, y, face, obj);
    if ((routing[size_t(index)]->flags & 255) == 0 && f.caller->route.state == Entity::Route::Walking)
        f.caller->route.entry_object = obj;   // onto the object's own spot: the side it comes in from, on arrival
    return walking;
}

Exit goto_relative_walk(Frame &f, const uint8_t *op)
{
    if (f.thread->is_check)
        return Exit::GotoFalse;
    if (back_from_failure(f))
        return Exit::GotoFalse;
    const bool failure_trees = !(op[6] & 2);   // FreeSO VMGotoRelativeFlags.NoFailureTrees
    bool started;
    const Exit progress = route_progress(f, started);
    if (started) {
        if (progress == Exit::GotoFalse && f.caller->route.last_failure) {
            // Cut off on this storey: round by the stairs (as goto routing slot).
            auto &rt = f.caller->route;
            const int why = rt.last_failure;
            if (why == 3 && f.caller->person) {
                Entity *goal = f.stack_object();
                if (goal && goal->in_world() && goal->level == f.caller->level && !TS_ENV("TS_STAIRS_NEAREST") &&
                    cut_off_from(f.vm(), *f.caller, *goal)) {
                    rt.last_failure = 0;
                    const Exit e = take_stairs(f, f.caller->level, goal, true);   // (`f` may move: returned at once)
                    if (e != Exit::GotoFalse)
                        return e;
                    rt.last_failure = why;
                }
            }
            return route_failed(f, f.caller->route.last_failure, nullptr, failure_trees);
        }
        return progress;
    }
    Entity *obj = f.stack_object();
    if (!obj || !obj->in_world() || !f.caller->in_world())
        return Exit::GotoFalse;
    if (obj->level != f.caller->level)
        return take_stairs(f, obj->level, obj);
    const int location = int8_t(op[2]), direction = int8_t(op[3]);
    SlotItem slot;
    slot.type = 3;
    if (location == -2) {   // on top of
        slot.min_proximity = slot.optimal_proximity = 0;
    } else {
        // As the original builds it (FUN_005f76e0): one tile off on that side; an odd location
        // (a corner) is the two sides beside it; anywhere near (-1, or anything else) is the
        // four sides, up to two tiles off. Never the diagonal tiles (FreeSO's had them).
        slot.min_proximity = slot.optimal_proximity = slot.max_proximity = 16;
        const int l = location;
        if (l >= 0 && l <= 7)
            slot.flags = (l % 2 == 0) ? (1 << l) : ((1 << (l - 1)) | (1 << ((l + 1) % 8)));
        else {
            slot.flags = 1 | 4 | 16 | 64;
            slot.max_proximity = 32;
        }
    }
    slot.facing = direction == -2 ? -2 : direction == -1 ? -3 : direction;   // facing it, anywhere, or relative
    int16_t x, y;
    int face;
    Vm &vm = f.vm();
    int entry = 0;
    Entity *in_way = nullptr;
    int fail_code = 1;
    Entity *fail_blocker = nullptr;
    if (!slot_spot_scored(*obj, *f.caller, slot, x, y, face, &entry, vm, nullptr, &in_way, nullptr, &fail_code,
                          &fail_blocker)) {
        // A Sim standing on the one spot there is (a shopper on the register's customer side):
        // waited for, as the routing slots do, not given up on. The Vita's pet sale failed at the
        // register's "no spot" once, with someone on it (2026-10-31).
        if (f.caller->person && in_way && in_way->person) {
            if (TS_ENV("TS_QUEUE_WHY"))   // debug
                std::printf("goto relative: #%d's spot by #%d (location %d) has #%d on it\n", int(f.caller->id), int(obj->id),
                            location, int(in_way->id));
            return wait_for_spot(f, *in_way, failure_trees);
        }
        if (TS_ENV("TS_QUEUE_WHY"))   // debug
            std::printf("goto relative: no spot by #%d at (%d, %d) facing %d (location %d)\n", int(obj->id), obj->tile_x(),
                        obj->tile_y(), int(obj->direction), location);
        // The same search as a routing slot's, so the same reasons (FUN_005f76e0 builds a slot for it).
        return route_failed(f, fail_code, fail_code == 8 || fail_code == 9 ? fail_blocker : nullptr, failure_trees);
    }
    if (Entity *in_way = f.caller->person ? spot_taken(vm, *f.caller, x, y, f.caller->level) : nullptr) {
        if (TS_ENV("TS_QUEUE_WHY"))   // debug
            std::printf("goto relative: #%d's spot by #%d is taken by #%d\n", int(f.caller->id), int(obj->id),
                        int(in_way->id));
        return wait_for_spot(f, *in_way, failure_trees);
    }
    f.caller->route.spot_asked = 0;
    if (f.caller->person)
        f.caller->person_data[37] = int16_t(entry);   // which side it came to (FreeSims sets it for every route)
    if (TS_ENV("TS_QUEUE_WHY"))   // debug: where each goto relative sends its Sim
        std::printf("goto relative: #%d to (%d, %d) facing %d, location %d by #%d at (%d, %d) facing %d\n", int(f.caller->id), x / 16,
                    y / 16, face, location, int(obj->id), obj->tile_x(), obj->tile_y(), int(obj->direction));
    return walk_to(f, x, y, face, obj);
}

// Snap: straight to a slot's spot without walking.
Exit snap(Frame &f, const uint8_t *op)
{
    Entity *obj = f.stack_object();
    if (!obj)
        return Exit::GotoFalse;
    const int index = u16(op, 0), mode = u16(op, 2);
    Vm &vm = f.vm();
    // A slot already holding something else can't take the caller: the exe's snap places through
    // its placement test (FUN_0061d080, the object's vtable +0x54 with the container and slot),
    // which fails there. Ours put the caller in and pushed the one there out of it: Linda, finding
    // her side of the bed blocked, tried John's side ("try other side") and snapped in on top of
    // him, John left asleep in no bed and the side marked hers (the user's 2026-11-03 report).
    auto slot_taken = [&](int s) {
        return s >= 0 && size_t(s) < obj->slots.size() && obj->slots[size_t(s)] && obj->slots[size_t(s)] != f.caller;
    };
    if (mode == 1) {   // be contained
        if (slot_taken(0))
            return Exit::GotoFalse;
        obj->place_in_slot(f.caller, 0);
        f.caller->snapped_at = vm.ticks;
        return Exit::GotoTrue;
    }
    SlotItem slot;
    if (mode == 2) {   // in front
        slot.type = 3;
        slot.min_proximity = slot.optimal_proximity = 16;
        slot.flags = 1;
    } else {
        // 0 the slot a parameter names, 3 this one, 4 a global one (FreeSO VMSnapSlotScope).
        const std::vector<const SlotItem *> routing = routing_slots(vm, *obj, mode == 4);
        const int i = mode == 0 ? (size_t(index) < f.args.size() ? f.args[size_t(index)] : -1) : index;
        if (i < 0 || size_t(i) >= routing.size())
            return Exit::GotoFalse;
        slot = *routing[size_t(i)];
    }
    if (TS_ENV("TS_SNAP_LOG"))   // debug
        std::printf("snap #%d into #%d (%s) mode %d index %d: snap target %d, taken %d, in it #%d\n", int(f.caller->id),
                    int(obj->id), obj->info ? obj->info->label.c_str() : "?", mode, index, slot.snap_target,
                    int(slot_taken(slot.snap_target)),
                    slot.snap_target >= 0 && size_t(slot.snap_target) < obj->slots.size() && obj->slots[size_t(slot.snap_target)]
                        ? int(obj->slots[size_t(slot.snap_target)]->id)
                        : 0);
    if (slot.snap_target >= 0) {
        if (slot_taken(slot.snap_target) && !TS_ENV("TS_SNAP_EVICT"))   // (debug: the old way)
            return Exit::GotoFalse;
        obj->place_in_slot(f.caller, slot.snap_target);
        f.caller->snapped_at = vm.ticks;
        // Snap-to-direction: turned the slot's way too (FreeSO VMSnap), as the animations played
        // there expect (a bath's get-in); else the Sim keeps its facing (a sofa's seat).
        int16_t sx, sy;
        int face;
        if ((slot.flags & 4096) && f.caller->person && slot_spot(*obj, *f.caller, slot, sx, sy, face) && face >= 0)
            f.caller->direction = int8_t(face);
        return Exit::GotoTrue;
    }
    int16_t x, y;
    int face;
    if (!slot_spot(*obj, *f.caller, slot, x, y, face))
        return Exit::GotoFalse;
    Entity &mover = *f.caller;
    if (!mover.person) {
        // An object moves whole, within the 64x64 arrays (the street runs past the lot's own
        // size); off them is "no valid goals" (primitive result 2, FreeSO VMSnap): that's how
        // a car driving away along the street knows it has gone.
        const int dx = x - mover.x16, dy = y - mover.y16;
        for (Entity *part : mover.group.empty() ? std::vector<Entity *>{ &mover } : mover.group) {
            if (!part->in_world())
                continue;
            const int tx = (part->x16 + dx) / 16, ty = (part->y16 + dy) / 16;
            if (part->x16 + dx < 0 || part->y16 + dy < 0 || tx >= kLotArraySize || ty >= kLotArraySize) {
                mover.data[VarPrimitiveResult] = 2;
                mover.data[VarPrimitiveResultId] = 0;
                return Exit::GotoFalse;
            }
        }
        // A Sim in the way stops it: the snap's placement fails on a person unless the mover allows
        // person intersection (flag 4; FreeSO VMSnap's SetPosition, the exe's placement test). A
        // car's "drive away" (CarGlobals 8205) moves it by snaps along the street and, when one
        // fails, honks and tries again: the original's cars stop for a Sim on the road (the user's
        // report 2026-11-06). Ours drove through them. Sims already in its way where it stands
        // (getting in or out) don't count. TS_SNAP_PEOPLE_OLD=1: as before.
        static const bool people_old = TS_ENV("TS_SNAP_PEOPLE_OLD") != nullptr;
        if (!people_old && !(uint16_t(mover.value(VarFlags)) & (1 << 4)))
            for (Entity *part : mover.group.empty() ? std::vector<Entity *>{ &mover } : mover.group) {
                if (!part->in_world())
                    continue;
                const Footprint to = footprint(*part, int16_t(part->x16 + dx), int16_t(part->y16 + dy), part->direction),
                                at = footprint(*part, part->x16, part->y16, part->direction);
                for (const auto &other : vm.entities) {
                    const Entity &p = *other;
                    if (!p.person || p.dead || !p.in_world() || p.container || p.level != part->level || &p == obj ||
                        mover.container == &p)
                        continue;   // (nor whoever it's snapped to or carried by)
                    const Footprint box = person_box(p, p.x16, p.y16);
                    if (box.overlaps(to) && !box.overlaps(at)) {
                        if (TS_ENV("TS_SNAP_PEOPLE"))   // debug
                            std::printf("snap: #%d (%s) waits for #%d at (%.2f, %.2f)\n", int(mover.id),
                                        mover.info ? mover.info->label.c_str() : "?", int(p.id), p.x16 / 16.0, p.y16 / 16.0);
                        // (Not result 2, "no valid goals", which the car reads as having left the street.)
                        mover.data[VarPrimitiveResult] = 0;
                        mover.data[VarPrimitiveResultId] = p.id;
                        return Exit::GotoFalse;
                    }
                }
            }
        // Cockroaches don't run through furniture: their code tries another way when a snap fails
        // (Roaches.iff "move", nodes 3 and 10), and a snap fails on a taken spot (FreeSO VMSnap's
        // SetPosition). Ours never did: they went under the beds (the 2026-11-02 report). Only
        // them: the other things that move like this are cars on the street and carried pieces.
        // Nor off the lot's own tiles: the exe counts an object as placed only on tiles 1 to size - 2
        // (FUN_0053b210's test), so a snap past them fails and the roach turns. Ours let it on to the
        // 64 x 64 arrays' rim, kept for the cars driving away: past the last tile drawn a roach wasn't
        // seen, and a Sim walked to the map's edge to stomp nothing ("Betty walked off the map to mess
        // with invisible cockroaches", the user's report, 2026-11-10). TS_ROACH_EDGE_OLD=1: as before.
        static const bool roach_edge_old = TS_ENV("TS_ROACH_EDGE_OLD") != nullptr;
        const bool roach = mover.info && mover.info->where.find("Roaches.iff") != std::string::npos;
        if (roach && !roach_edge_old && (x / 16 < 1 || y / 16 < 1 || x / 16 >= vm.lot.size - 1 || y / 16 >= vm.lot.size - 1)) {
            if (TS_ENV("TS_ROACH"))
                std::printf("roach kept on the lot at %d %d; ", x / 16, y / 16);
            mover.data[VarPrimitiveResult] = 0;
            mover.data[VarPrimitiveResultId] = 0;
            return Exit::GotoFalse;
        }
        // What stops a roach's step, as the exe's placement test for a snap (FUN_0061d080 -> the object's
        // vtable + 0x54, FUN_0053a1b0 -> FUN_0053d910 -> FUN_0053e1d0). A roach has "zero extent" (its
        // init sets flag bit 2), so no object is in its way unless both carry exclusive placement bit 0
        // (object data 63; a roach's is 0), and Sims never are (FUN_0053e1d0: a zero-extent mover is
        // stopped by a person only with its flag bit 1). What does stop it: a tile that isn't level (its
        // placement flags, 3, lack "on slope" 0x100: error 0x11), a pool or water (no 0x800 / 4), and
        // a wall its box would lie across (FUN_00641430, error 7; no "in wall" 0x400): its box is a tile
        // wide, so a step of 2 from a tile's middle towards a wall fails at once, and its "move" then
        // jumps the whole tile (slot + 8), box clear of the line: through the wall, a tile at a time.
        // Ours stopped it at anything that blocks walking (2026-11-02, from FreeSO's snap), and each
        // such stop came on the fourth of a tile's eight steps, as the roach's middle crossed into the
        // furniture's tile: it stayed 6/16 short of its tile's middle from then on, "inbetween tiles
        // when they stop moving" (the user, 2026-11-11), next to every counter, chair and bed. The
        // exe's roach goes under them, and rests off a tile's middle only against a slope, water or
        // the lot's rim. TS_ROACH_BLOCK_OLD=1: as before.
        static const bool roach_block_old = TS_ENV("TS_ROACH_BLOCK_OLD") != nullptr;
        if (roach && !roach_block_old) {
            const int tx = x / 16, ty = y / 16, l = obj->level - 1;
            const uint16_t placement = uint16_t(mover.value(VarPlacementFlags));
            auto refuse = [&](const char *why, int id) {
                if (TS_ENV("TS_ROACH"))
                    std::printf("roach #%d stopped at %d.%02d %d.%02d (%s); ", int(mover.id), x / 16, (x % 16) * 100 / 16, y / 16,
                                (y % 16) * 100 / 16, why);
                mover.data[VarPrimitiveResult] = 0;
                mover.data[VarPrimitiveResultId] = int16_t(id);
                return Exit::GotoFalse;
            };
            const int n = kLotArraySize + 1;
            if (!(placement & 0x100) && vm.lot.altitude.size() >= size_t(n) * size_t(n)) {
                const uint8_t a = vm.lot.altitude[size_t(tx + ty * n)];
                if (vm.lot.altitude[size_t(tx + 1 + ty * n)] != a || vm.lot.altitude[size_t(tx + (ty + 1) * n)] != a ||
                    vm.lot.altitude[size_t(tx + 1 + (ty + 1) * n)] != a)
                    return refuse("a slope", 0);
            }
            if (l >= 0 && l < kLotLevels && !vm.lot.floors[l].empty() && !(placement & (0x800 | 4))) {
                const uint16_t floor = vm.lot.floor(tx, ty, l);
                if (floor == kFloorPool || floor == kFloorWater)
                    return refuse("water", 0);
            }
            if (!(placement & 0x400) && box_hits_wall(vm, footprint(mover, int16_t(x), int16_t(y), mover.direction), obj->level))
                return refuse("a wall across its box", 0);
            const uint16_t exclusive = uint16_t(mover.data[63]);
            if (exclusive & 1)
                for (const auto &other : vm.entities)
                    if (other.get() != &mover && !other->dead && !other->person && other->in_world() && !other->container &&
                        other->level == obj->level && other->tile_x() == tx && other->tile_y() == ty &&
                        (uint16_t(other->data[63]) & 1))
                        return refuse("an object with the same exclusive placement", other->id);
        } else if (roach) {
            for (const auto &other : vm.entities)
                if (other.get() != &mover && !other->person && other->in_world() && other->level == obj->level &&
                    other->tile_x() == x / 16 && other->tile_y() == y / 16 && blocks_walking(*other)) {
                    if (TS_ENV("TS_ROACH"))
                        std::printf("roach blocked at %d %d by %s; ", x / 16, y / 16, other->info ? other->info->label.c_str() : "?");
                    return Exit::GotoFalse;
                }
        }
        mover.place_whole(int16_t(x), int16_t(y), obj->level,
                          face >= 0 && (slot.flags & 4096) ? face : mover.direction);
        return Exit::GotoTrue;
    }
    // A Sim doesn't snap onto something solid: the snap fails (FreeSO VMSnap's SetPosition) and the
    // game's code takes another way. Up from a dining chair, "in front" is the table: placed there,
    // they stood on it and walked off across it (the 2026-11-02 walk-through reports). Stairs and
    // the object snapped to don't count.
    static const bool old_snap = TS_ENV("TS_SNAP_OLD") != nullptr;   // debug: the old way, to compare
    // Nor into another room, nor through a wall: the exe's placement for a snap (FUN_0061d080), for
    // "in front" always and else for slots without IgnoreRooms (0x800): the spot in the room the
    // Sim is in, and, unless the Sim is on the lot's edge or the spot is where it stands, its tile's
    // edge towards the spot and the spot's edge back both open (FUN_005f2070's direction,
    // FUN_0053b6a0's edge test). Ours placed them anywhere the slot said. (The exe does this for
    // every mover; ours only for people: stairs and roaches have IgnoreRooms, cars none checked.)
    // TS_SNAP_ROOMS_OLD=1: as before.
    static const bool rooms_old = TS_ENV("TS_SNAP_ROOMS_OLD") != nullptr;
    if (!old_snap && !rooms_old && (mode == 2 || !(slot.flags & 0x800))) {
        const int sx = mover.tile_x(), sy = mover.tile_y(), tx = x / 16, ty = y / 16;
        bool refused = vm.room_at(obj->level, tx, ty) != vm.room_at(mover.level, sx, sy);
        const bool on_edge = sx < 1 || sy < 1 || sx >= vm.lot.size - 1 || sy >= vm.lot.size - 1;
        if (!refused && !on_edge && (tx != sx || ty != sy)) {
            const int ux = (tx > sx) - (tx < sx), uy = (ty > sy) - (ty < sy);
            refused = wall_between(vm, sx, sy, sx + ux, sy + uy, obj->level) || wall_between(vm, tx, ty, tx - ux, ty - uy, obj->level);
        }
        if (refused) {
            if (TS_ENV("TS_QUEUE_WHY"))   // debug
                std::printf("snap: #%d can't go to (%d, %d): another room or a wall;", int(mover.id), tx, ty);
            mover.data[VarPrimitiveResult] = 0;
            mover.data[VarPrimitiveResultId] = 0;
            return Exit::GotoFalse;
        }
    }
    if (!old_snap)
    for (const auto &other : vm.entities)
        if (other.get() != &mover && other.get() != obj && !other->person && !other->dead && other->in_world() &&
            !other->container && other->level == obj->level && other->tile_x() == x / 16 && other->tile_y() == y / 16 &&
            blocks_walking(*other) && std::find(obj->group.begin(), obj->group.end(), other.get()) == obj->group.end() &&
            !(other->entry_points.size() > 15 && other->entry_points[15].action != 0)) {
            if (TS_ENV("TS_QUEUE_WHY"))   // debug
                std::printf("snap: #%d can't stand at (%d, %d): %s;", int(mover.id), x / 16, y / 16,
                            other->info ? other->info->label.c_str() : "?");
            // FUN_0061c910: 2 is only for off the lot; anything in the way leaves 0 and its id.
            mover.data[VarPrimitiveResult] = 0;
            mover.data[VarPrimitiveResultId] = other->id;
            return Exit::GotoFalse;
        }
    // Nor onto another Sim (FUN_0053e1d0, flags as read there: bit 2 clear, bit 4 lets people through;
    // bit 2 set, bit 1 is what stops them). With the instruction's flag 2 they're asked to move, and
    // the result is 1 (FUN_005f3640). The box is ours: about two thirds of a tile.
    if (!old_snap)
        for (const auto &other : vm.entities) {
            if (other.get() == &mover || other.get() == obj || !other->person || other->dead || !other->in_world() ||
                other->container || other->level != obj->level || std::abs(other->x16 - x) >= 10 ||
                std::abs(other->y16 - y) >= 10)
                continue;
            const int16_t flags = other->data[VarFlags];
            if (!((flags & 4) ? (flags & 2) != 0 : (flags & 0x10) == 0))
                continue;
            if (TS_ENV("TS_QUEUE_WHY"))   // debug
                std::printf("snap: #%d can't stand on #%d: spot (%.2f, %.2f), it is at (%.2f, %.2f) flags %d stage %d thread %d, op4 %d;", int(mover.id), int(other->id), x / 16.0, y / 16.0, other->x16 / 16.0, other->y16 / 16.0, int(flags), int(other->route.block_stage), other->thread ? 1 : 0, int(op[4]));
            // (The Sim itself, not the list's slot: asking it to move can make an object, and the
            // list reallocated under `other`: a read of freed memory, lot 21's crash, 2026-11-05.)
            Entity *const in_way = other.get();
            const int16_t in_way_id = int16_t(in_way->id);
            mover.data[VarPrimitiveResult] = (op[4] & 2) && ask_to_move(vm, mover, *in_way, nullptr, true) ? 1 : 0;
            mover.data[VarPrimitiveResultId] = in_way_id;
            return Exit::GotoFalse;
        }
    mover.set_position(x, y, obj->level);
    if (face >= 0 && (slot.flags & 4096))
        mover.direction = face;
    mover.snapped_at = vm.ticks;
    return Exit::GotoTrue;
}

// ---------------------------------------------------------------- people

Exit set_motive_change(Frame &f, const uint8_t *op)
{
    Entity &sim = *f.caller;
    const int delta_owner = op[0], max_owner = op[1], motive = op[2], flags = op[3];
    // As the original's handler (the Sims' dispatcher, case 0x1d): flag bit 0 empties the list;
    // else the motive must be 5-9, 14 or 15 (anything else is an error there) and an entry is
    // appended: {motive, rate / 60 / 30 a tick, max}. No other flag is read: FreeSO's "once" is
    // The Sims Online's (an operand with that bit set is a rate here, as in the game).
    if (flags & 1) {   // clear all
        sim.motive_changes.clear();
        return Exit::GotoTrue;
    }
    if (!((motive >= 5 && motive <= 9) || motive == 14 || motive == 15)) {
        f.vm().warn(&f, "set motive change: motive " + std::to_string(motive) + " (the original's error 0x1a)");
        return Exit::GotoTrue;
    }
    if (flags & 2)   // (counted: how much of the game's code carries the bit the original ignores)
        f.vm().warn(&f, "set motive change with the 'once' bit, taken as a rate (as the original)");
    const int16_t rate = get(f, delta_owner, s16(op, 4));
    const int16_t max = get(f, max_owner, s16(op, 6));
    // The rate as it is, an hour's worth (the original adds rate / 60 / 30 a tick). FreeSims scaled
    // energy x22 here for the sleeping rise the original gets from its decay (see tick_motives).
    sim.motive_changes.push_back({ int8_t(motive), rate, max, 0.0 });
    return Exit::GotoTrue;
}

// Relationships (FreeSO VMRelationship, the Sims 1 way): between two people
// with neighbour ids, or in "neighbour" mode (where the stack object and the
// local hold neighbour ids), they are the neighbourhood's; otherwise the object's own.
Exit relationship_op(Frame &f, int rel, int mode, bool fail_if_small, bool neighbors, int local, int scope,
                     int16_t data, int set_mode, bool old)
{
    Vm &vm = f.vm();
    // The exe's errors (FUN_0061b350, the old form FUN_0061b820): a local (parameter) index out of
    // range for modes 2 and 3 (0x33 / 8), a caller not a person in neighbour mode 0 or 1 (0x1c), a
    // missing object, or a missing neighbour record on either side (0x15). Ours read 0, went on or
    // was false. TS_REL_SOFT=1: as before.
    static const bool soft = TS_ENV("TS_REL_SOFT") != nullptr;
    if (!soft && (mode == 2 || mode == 3) && (old ? f.args.empty() : size_t(local) >= f.locals.size()))
        return Exit::Error;
    if (!soft && neighbors && (mode == 0 || mode == 1) && !f.caller->person)
        return Exit::Error;
    Entity *a = nullptr, *b = nullptr;
    const int16_t other = old ? (f.args.empty() ? 0 : f.args[0])
                              : (size_t(local) < f.locals.size() ? f.locals[size_t(local)] : 0);
    int16_t a_raw = 0, b_raw = 0;   // the ids the operand names (neighbour ids in neighbour mode)
    switch (mode) {
    case 0: a = f.caller; b = f.stack_object(); a_raw = int16_t(f.caller->id); b_raw = f.stack_id; break;
    case 1: a = f.stack_object(); b = f.caller; a_raw = f.stack_id; b_raw = int16_t(f.caller->id); break;
    case 2: a = f.stack_object(); b = vm.entity(other); a_raw = f.stack_id; b_raw = other; break;
    case 3: a = vm.entity(other); b = f.stack_object(); a_raw = other; b_raw = f.stack_id; break;
    default: return Exit::Error;
    }
    if ((!a || !b) && !neighbors)
        return soft ? Exit::GotoTrue : Exit::Error;
    auto nid = [](const Entity *e) { return e && e->person ? e->person_data[31] : int16_t(0); };
    std::vector<int16_t> *values = nullptr;
    int16_t from_n = nid(a), to_n = nid(b);
    if (neighbors) {
        // The caller is a person; the other side is whatever id the operand gives.
        from_n = (mode == 0) ? nid(f.caller) : a_raw;
        to_n = (mode == 1) ? nid(f.caller) : b_raw;
    }
    if (vm.hood && (neighbors || (from_n > 0 && to_n > 0))) {
        Neighbor *n = vm.hood->by_id(from_n);
        if (!n)
            return soft ? Exit::GotoFalse : Exit::Error;
        if (!soft && neighbors && !vm.hood->by_id(to_n))
            return Exit::Error;   // (the target's record must exist too: FUN_0054b240)
        auto it = n->relationships.find(to_n);
        if (it == n->relationships.end()) {
            if (fail_if_small)
                return Exit::GotoFalse;
            it = n->relationships.emplace(to_n, std::vector<int16_t>()).first;
        }
        values = &it->second;
    } else {
        if (!a || !b)
            return Exit::GotoTrue;
        auto it = a->relationships.find(b->id);
        if (it == a->relationships.end()) {
            if (fail_if_small)
                return Exit::GotoFalse;
            it = a->relationships.emplace(b->id, std::vector<int16_t>()).first;
        }
        values = &it->second;
    }
    if (int(values->size()) <= rel) {
        if (fail_if_small)
            return Exit::GotoFalse;
        values->resize(size_t(rel) + 1, 0);
    }
    int16_t &v = (*values)[size_t(rel)];
    // Read, or written as it is: the original (FUN_0061b350) neither clamps to -100..100 nor has
    // an "add" (both The Sims Online's, in FreeSO); the game's own code keeps values in range.
    if (set_mode == 0)
        set(f, scope, data, v);
    else {
        const int16_t was = v;
        v = get(f, scope, data);
        // The player's "Friendship gains" modifier (not the original's): a daily score's rise of a
        // social's size (1-20 points) multiplied, up to 100. Falls, the fade and values set from
        // afar (a move-in's, a marriage's) are the game's. Only between people: objects keep their
        // own counters and links in relationship 0 (seen: a controller's to each Sim, 0 -> 1).
        // TS_REL_GAIN traces it.
        const bool people = neighbors ? (from_n > 0 && to_n > 0) : (a && b && a->person && b->person);
        if (people && v != was && TS_ENV("TS_REL_LOG"))   // debug: every change of a relationship between people
            std::printf("tick %u (%d:%02d) rel %d: neighbours %d -> %d: %d -> %d (in %s of %s)\n", unsigned(vm.ticks),
                        int(vm.clock.hours), int(vm.clock.minutes), rel, int(from_n), int(to_n), int(was), int(v),
                        f.routine ? f.routine->name.c_str() : "?",
                        f.caller && f.caller->info ? f.caller->info->label.c_str() : "?");
        if (rel == 0 && people && vm.relationship_gain > 1 && v > was && v - was <= 20) {
            const int boosted = std::max(int(v), std::min(100, int(was) + (int(v) - int(was)) * vm.relationship_gain));
            if (TS_ENV("TS_REL_GAIN"))
                std::printf("relationship gain: #%d -> #%d (neighbours %d -> %d): %d -> %d, boosted to %d\n", a ? int(a->id) : 0,
                            b ? int(b->id) : 0, int(from_n), int(to_n), int(was), int(v), boosted);
            v = int16_t(boosted);
        }
    }
    return Exit::GotoTrue;
}

Exit relationship_new(Frame &f, const uint8_t *op)
{
    const int rel = op[0], mode = op[1], flags = op[2], local = op[3];
    const int scope = u16(op, 4);
    const int set_mode = (flags >> 2) & 1;   // get, set
    return relationship_op(f, rel, mode, flags & 1, flags & 2, local, scope, s16(op, 6), set_mode, false);
}

Exit relationship_old(Frame &f, const uint8_t *op)
{
    return relationship_op(f, op[1], op[3], op[4] & 1, op[4] & 2, 0, Parameters, op[2], op[0] != 0 ? 1 : 0, true);
}

Exit transfer_funds(Frame &f, const uint8_t *op)
{
    // As the original (its primitive switch, case 0x19): the amount (flag 2: coming in) is paid
    // out of the family's money, the expense type in byte 6 (what it's spent on); flag 1 only
    // tests. Paying fails if there isn't enough. (Ours branched on byte 7 with The Sims
    // Online's money types, which the Sims 1 never sets.)
    static const int kOldOwner[] = { Literal, Parameters, Local };
    const int old_owner = op[0];
    const int owner = old_owner < 3 ? kOldOwner[old_owner] : op[1];
    const int32_t value = get_big(f, owner, int16_t(u16(op, 2)));
    const int flags = op[4];
    const int32_t amount = (flags & 2) ? -value : value;
    Vm &vm = f.vm();
    Family *family = vm.family();
    int free_funds = 999999;   // (a lot with no family: nothing changes them, Vm::free_money)
    int &budget = vm.free_money() ? free_funds : family ? family->budget : vm.loose_budget;
    if (amount >= 0 && budget < amount)
        return Exit::GotoFalse;
    if (!(flags & 1)) {
        budget -= amount;
        // The amount over the payer's head, as the original shows it (the user, 2026-11-06: the till
        // rang with nothing shown). Only on a lot with a family's money at stake.
        if (amount != 0 && !vm.free_money() && f.caller && f.caller->in_world() && vm.money_shown.size() < 32)
            vm.money_shown.push_back({ int16_t(f.caller->id), -amount });
    }
    return Exit::GotoTrue;
}

std::string entity_name(const Entity *e)
{
    if (!e || !e->info)
        return "someone";
    if (!e->custom_name.empty())   // the player's name for it (a baby)
        return e->custom_name;
    if (e->person && e->master == nullptr) {
        // Character files are labelled "user00010 - Bella": the name is after the dash.
        const std::string &l = e->info->label;
        const size_t dash = l.find(" - ");
        if (e->info->name.empty() && dash != std::string::npos)
            return l.substr(dash + 3);
    }
    if (e->master && !e->master->name.empty())
        return e->master->name;
    return e->info->name.empty() ? e->info->label : e->info->name;
}

// A neighbour's name: their character's catalogue name, else what follows
// the dash in "user00010 - Bella", else the neighbourhood's own.
std::string neighbor_name(Vm &vm, const Neighbor &n)
{
    if (const ObjectInfo *info = vm.content.object(n.guid)) {
        if (!info->name.empty())
            return info->name;
        const size_t dash = info->label.find(" - ");
        if (dash != std::string::npos)
            return info->label.substr(dash + 3);
    }
    return n.name.empty() ? std::string("someone") : n.name;
}

// The phone book: the Sims `sim` knows (a relationship either way), not
// counting the family, by name.
std::vector<std::pair<int16_t, std::string>> known_sims(Vm &vm, const Entity &sim)
{
    std::vector<std::pair<int16_t, std::string>> out;
    if (!vm.hood)
        return out;
    const int me = sim.person_data[31];
    const Family *family = vm.family();
    for (const Neighbor &n : vm.hood->neighbors) {
        if (n.id == me || n.guid == 0)
            continue;
        if (family && std::find(family->members.begin(), family->members.end(), n.guid) != family->members.end())
            continue;
        const Neighbor *mine = vm.hood->by_id(me);
        const bool known = (mine && mine->relationships.count(n.id)) || n.relationships.count(me);
        const ObjectInfo *info = vm.content.object(n.guid);
        if (!known || !info || info->objd.get(Objd::ObjectType) != 2)
            continue;
        // Not the pets (you can't phone a dog: the user's 2026-11-04 report; the exe tells a pet by
        // person data 65, 2 and up; a record without person data by its skeleton).
        const bool pet = n.person_data.size() > 65 ? n.person_data[65] >= 2
                         : (!info->body_strings.empty() && (info->body_strings[0].find("kat") == 0 || info->body_strings[0].find("dog") == 0 ||
                                                            info->body_strings[0].find("Kat") == 0 || info->body_strings[0].find("Dog") == 0));
        if (pet)
            continue;
        out.emplace_back(n.id, neighbor_name(vm, n));
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    return out;
}

// The pet pens' chooser (dialog type 9): the coats of the pen's kind (the dog pen 3d3e7889, the
// cat pen 6350e96f; by name otherwise), numbered as pet_coats() lists them (make_pet's index).
std::vector<std::pair<int16_t, std::string>> pet_choices(Vm &vm, const Entity *pen, bool *dog_out = nullptr,
                                                         std::vector<std::string> *suits = nullptr)
{
    constexpr uint32_t kDogPen = 0x3d3e7889, kCatPen = 0x6350e96f;
    bool dog = true;
    if (pen && pen->info) {
        if (pen->info->guid == kCatPen)
            dog = false;
        else if (pen->info->guid != kDogPen)
            dog = pen->info->name.find("Cat") == std::string::npos;
    }
    if (dog_out)
        *dog_out = dog;
    std::vector<std::pair<int16_t, std::string>> out;
    const auto coats = pet_coats(vm.content, dog);
    for (size_t i = 0; i < coats.size(); i++) {
        out.emplace_back(int16_t(i), coats[i].second);
        if (suits)
            suits->push_back(coats[i].first);
    }
    return out;
}

// Hot Date's clothing racks' chooser (dialog type 6): the outfits of the clothing type in temp 0
// the shopper could buy, numbered as purchasable_outfits() lists them (generic call 24's index);
// the one they wear now marked.
std::vector<std::pair<int16_t, std::string>> clothes_choices(Vm &vm, const Entity &shopper, int type)
{
    std::vector<std::pair<int16_t, std::string>> out;
    const std::vector<std::string> suits = vm.suits_of(shopper, type);
    const int worn = vm.suit_index(shopper, type);
    for (size_t i = 0; i < suits.size(); i++)
        out.emplace_back(int16_t(i), "Outfit " + std::to_string(i + 1) + (int(i) == worn ? " (wearing)" : ""));
    return out;
}

// Makin' Magic's "Transform Me" charm (dialog type 14, the exe's FUN_004ecda0): eight new looks for
// an adult in this order, two for a child, each an outfit number (person data 8) the charm's
// transform spell puts on; the original pages through them showing the Sim in each ("Choose the
// New You", UIText STR# 273). The names are ours (the original names none).
std::vector<std::pair<int16_t, std::string>> transform_choices(const Entity &sim)
{
    const int age = sim.person_data.size() > 58 ? sim.person_data[58] : 27;
    const bool female = sim.person_data.size() > 65 && sim.person_data[65] == 1;
    if (age > 0 && age < 18)
        return female ? std::vector<std::pair<int16_t, std::string>>{ { 54, "Wizard Girl" }, { 60, "Skeleton" } }
                      : std::vector<std::pair<int16_t, std::string>>{ { 53, "Goblin" }, { 60, "Skeleton" } };
    if (female)
        return { { 48, "Sorceress" }, { 43, "Vampire" }, { 44, "Werewolf" }, { 49, "Witch" },
                 { 39, "Blue Genie" }, { 50, "Ogress" }, { 51, "Crone" }, { 52, "Nymph" } };
    return { { 48, "Sorcerer" }, { 58, "Vampire" }, { 44, "Werewolf" }, { 45, "Headless" },
             { 39, "Blue Genie" }, { 46, "Troll" }, { 47, "Hunchback" }, { 40, "Minotaur" } };
}

// The lots a travel chooser offers (dialog types 5 downtown, 7 vacation, 8 the
// neighbourhood's community lots, 11 Studio Town, 13 Magic Town): those with a
// lot file, by house number, named from the *Desc.iff files.
std::vector<std::pair<int16_t, std::string>> travel_lots(Vm &vm, int type)
{
    std::vector<std::pair<int16_t, std::string>> out;
    if (!vm.hood)
        return out;
    auto range = [&](int lo, int hi) {
        for (int h : vm.hood->houses) {
            auto zone = vm.hood->zoning.find(h);
            if (zone != vm.hood->zoning.end() && zone->second == 0)
                continue;   // somebody's home (Magic Town's 90-92), not a place to visit
            if (h >= lo && h <= hi && h != vm.house) {
                auto name = vm.hood->lot_names.find(h);
                out.emplace_back(int16_t(h), name != vm.hood->lot_names.end() ? name->second : "Lot " + std::to_string(h));
            }
        }
    };
    switch (type) {
    case 5: range(21, 30); break;
    case 7: range(40, 49); break;
    case 11: range(81, 89); break;
    case 13: range(90, 99); break;
    default:   // the neighbourhood's community lots
        for (const auto &[h, zone] : vm.hood->zoning)
            if (zone == 1 && vm.hood->houses.count(h) && h != vm.house)
                out.emplace_back(int16_t(h), vm.hood->lot_names.count(h) ? vm.hood->lot_names[h] : "Lot " + std::to_string(h));
        break;
    }
    return out;
}

// The game's dialog text with its $ substitutions (FreeSims VMDialogHandler.ParseDialogString).
// The person the last name token named (the exe's FUN_006123b0 hands the Dialog primitive the
// neighbour a name token resolved to, the last one, and a box of picture type 0 shows them: the
// goodbye's "$Me" title, Liz's face). Armed by the Dialog primitive only.
struct DialogNamed {
    bool armed = false;
    int16_t entity = 0, neighbor = -1;
} g_dialog_named;

std::string dialog_text(Frame &f, const std::string &in)
{
    Vm &vm = f.vm();
    auto named = [&](const Entity *e) {
        if (g_dialog_named.armed && e && e->person)
            g_dialog_named.entity = int16_t(e->id), g_dialog_named.neighbor = -1;
    };
    auto named_neighbor = [&](int16_t id) {
        if (g_dialog_named.armed && id > 0)
            g_dialog_named.neighbor = id, g_dialog_named.entity = 0;
    };
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != '$') {
            out += in[i];
            continue;
        }
        auto starts = [&](const char *word) {
            const size_t n = std::strlen(word);
            if (in.compare(i + 1, n, word) != 0)
                return false;
            i += n;
            return true;
        };
        auto number = [&]() {
            int v = 0;
            while (i + 1 < in.size() && std::isdigit(uint8_t(in[i + 1])))
                v = v * 10 + (in[++i] - '0');
            return v;
        };
        // The game's other words (FUN_006123b0).
        auto local_value = [&](int l) { return size_t(l) < f.locals.size() ? int(f.locals[size_t(l)]) : 0; };
        auto attribute_of_stack = [&](int a) {
            const Entity *o = f.stack_object();
            return o ? int(o->attribute(a)) : 0;
        };
        if (starts("NeighborLocal:")) {
            const Neighbor *n = vm.hood ? vm.hood->by_id(int16_t(local_value(number()))) : nullptr;
            if (n)
                named_neighbor(n->id);
            out += n ? neighbor_name(vm, *n) : std::string();
        } else if (starts("NameAttrib:")) {
            const Entity *e = vm.entity(int16_t(attribute_of_stack(number())));
            named(e);
            out += entity_name(e);
        } else if (starts("Attrib:")) {
            out += std::to_string(attribute_of_stack(number()));
        } else if (starts("TokenNameLocal:")) {   // a token of this Sim's: the name of what it's for
            const int t = local_value(number());
            if (t >= 0 && size_t(t) < f.caller->tokens.size())
                if (const ObjectInfo *info = vm.content.object(f.caller->tokens[size_t(t)].guid))
                    out += !info->name.empty() ? info->name : info->label;
        } else if (starts("FameTitleLocal:")) {   // the fame level in the local: its title (FUN_00592640:
            const int t = local_value(number());   // fame.iff STR# 1, 1..11; STR# 4 is star power's numbers)
            const FameTables &fame = fame_tables(vm.content);
            if (t >= 0 && size_t(t) < fame.titles.size())
                out += fame.titles[size_t(t)];
        } else if (starts("CatalogLocal:")) {   // the object in the local: its catalogue text (CTSS 1)
            const Entity *o = vm.entity(int16_t(local_value(number())));
            const ObjectInfo *info = o ? (o->master ? o->master : o->info) : nullptr;
            const LoadedIff *file = info ? vm.content.load(info->ref) : nullptr;
            if (const IffChunk *c = file ? file->iff.find("CTSS", info->objd.get(Objd::CatalogStringsID)) : nullptr) {
                StringTable t;
                std::string err;
                if (parse_str(file->body(*c), c->size, t, err))
                    if (const std::string *text = t.get(1))
                        out += *text;
            }
        } else if (starts("FamilyAssets:")) {   // the family numbered in the local: its money
            const int number_is = local_value(number());
            if (vm.hood)
                for (const Family &fam : vm.hood->families)
                    if (fam.number == number_is) {
                        out += std::to_string(fam.budget);
                        break;
                    }
        } else if (starts("Family")) {   // the family of the neighbour in the stack object
            const Neighbor *n = vm.hood ? vm.hood->by_id(f.stack_id) : nullptr;
            if (n)
                for (const Family &fam : vm.hood->families)
                    if (std::find(fam.members.begin(), fam.members.end(), n->guid) != fam.members.end()) {
                        out += fam.name;
                        break;
                    }
        } else if (starts("Neighbor")) {   // the neighbour whose id is in the stack object (FreeSO)
            const Neighbor *n = vm.hood ? vm.hood->by_id(f.stack_id) : nullptr;
            if (n)
                named_neighbor(n->id);
            out += n ? neighbor_name(vm, *n) : std::string("someone");
        } else if (starts("DynamicObjectName") || starts("Object")) {
            named(f.stack_object());
            out += entity_name(f.stack_object());
        } else if (starts("Me")) {
            named(f.caller);
            out += entity_name(f.caller);
        }
        else if (starts("MoneyXL:"))
            out += "$" + std::to_string(f.thread->temp_xl[size_t(std::min(number(), 1))]);
        else if (starts("TempXL:"))
            out += std::to_string(f.thread->temp_xl[size_t(std::min(number(), 1))]);
        else if (starts("Temp:"))
            out += std::to_string(f.thread->temps[size_t(std::min(number(), 19))]);
        else if (starts("Attribute:"))
            out += std::to_string(f.caller->attribute(number()));
        else if (starts("NameLocal:")) {
            const int l = number();
            const Entity *e = size_t(l) < f.locals.size() ? vm.entity(f.locals[size_t(l)]) : nullptr;
            named(e);
            out += entity_name(e);
        } else if (starts("Local:")) {
            const int l = number();
            out += std::to_string(size_t(l) < f.locals.size() ? f.locals[size_t(l)] : 0);
        } else if (starts("Param:")) {
            const int a = number();
            out += std::to_string(size_t(a) < f.args.size() ? f.args[size_t(a)] : 0);
        } else if (const bool desc = starts("JobDesc:"); desc || starts("Job:")) {
            // $Job:<career local>:<level local>: the job title; $JobDesc: its pay and hours
            const int lc = number();
            int ll = -1;
            if (i + 1 < in.size() && in[i + 1] == ':') {
                i++;
                ll = number();
            }
            auto local = [&](int l) { return l >= 0 && size_t(l) < f.locals.size() ? f.locals[size_t(l)] : 0; };
            const int career = local(lc), level = local(ll);
            std::string title;
            if (vm.hood)
                for (const Career &c : vm.hood->career_data)
                    if (c.id == career && level >= 0 && size_t(level) < c.levels.size()) {
                        const Career::Level &l = c.levels[size_t(level)];
                        title = desc ? "Salary $" + std::to_string(l.salary) + ", hours " + std::to_string(l.start) +
                                           ":00 to " + std::to_string(l.end) + ":00"
                                     : l.name;
                    }
            if (title.empty() && !desc)
                title = "a new job";
            out += title;
        } else if (starts("JobOffer:")) {
            // $JobOffer:<career local>:<level local> (the newspaper's and computer's offers): the
            // career and the job in it. (Printed as it stood before: the 2026-10-27 shots.)
            const int lc = number();
            int ll = -1;
            if (i + 1 < in.size() && in[i + 1] == ':') {
                i++;
                ll = number();
            }
            auto local = [&](int l) { return l >= 0 && size_t(l) < f.locals.size() ? f.locals[size_t(l)] : 0; };
            const int career = local(lc), level = local(ll);
            std::string text = "a new career";
            if (vm.hood)
                for (const Career &c : vm.hood->career_data)
                    if (c.id == career) {
                        text = c.name + " career";
                        if (level >= 0 && size_t(level) < c.levels.size())
                            text += " as " + c.levels[size_t(level)].name;
                    }
            out += text;
        } else if (starts("TimeLocal:")) {   // an hour in a local: as the clock shows it
            const int l = number();
            const int hour = ((size_t(l) < f.locals.size() ? f.locals[size_t(l)] : 0) % 24 + 24) % 24;
            out += std::to_string(hour % 12 == 0 ? 12 : hour % 12) + ":00 " + (hour < 12 ? "AM" : "PM");
        } else if (starts("DynamicStringLocal:")) {
            while (i + 1 < in.size() && (std::isdigit(uint8_t(in[i + 1])) || in[i + 1] == ':'))
                i++;   // strings picked by the code: not shown
        } else if (i + 1 < in.size() && in[i + 1] == '$') {   // "$$Local:0": a dollar sign, then the token
            out += '$';
        } else {
            out += '$';
        }
    }
    return out;
}

// Dialogs (private, global and semi-global strings, STR# 301): shown to the
// player; a blocking one waits for the answer, which picks the way on.
Exit dialog_with(Frame &f, const uint8_t *op, const LoadedIff *file)
{
    Vm &vm = f.vm();
    Thread &t = *f.thread;
    const int type = op[5], flags = op[7];
    if (t.dialog_waiting) {
        auto it = std::find_if(vm.dialogs.begin(), vm.dialogs.end(), [&](const Vm::Dialog &d) { return d.id == t.dialog_waiting; });
        if (it == vm.dialogs.end()) {   // gone: as if OK
            t.dialog_waiting = 0;
            return Exit::GotoTrue;
        }
        if (it->response < 0)
            return Exit::ContinueNextTick;
        const int answer = it->response;
        const int16_t number = it->number;
        const std::string text = it->text;
        vm.dialogs.erase(it);
        t.dialog_waiting = 0;
        switch (type) {
        case 1: return answer == 0 ? Exit::GotoTrue : Exit::GotoFalse;
        case 3:   // a text entry: the stack object's name (the baby's; FreeSO: at most 32 letters, nothing typed keeps the old)
            if (!text.empty() && f.stack_object())
                f.stack_object()->custom_name = text.substr(0, 64);
            return Exit::GotoTrue;
        case 2:
            if (answer > 0) {
                t.temps[(flags & 32) ? 1 : 0] = int16_t((answer - 1) % 2);
                return Exit::GotoFalse;
            }
            return Exit::GotoTrue;
        case 5: case 7: case 8: case 11: case 13:   // a lot chooser: the house number
        case 9:    // the pet chooser: the coat's number
        case 6:    // Hot Date's clothes chooser: the outfit's number (missing, it fell to "OK" and left temp 0
                   // the dialog's type, 1: the booth put on outfit 1 whatever was chosen, the user's report
                   // 2026-11-05)
        case 10:   // the phone book: the neighbour picked, in temp 0 or 1 (FreeSO VMDialogPrivateStrings)
        case 14:   // Transform Me: the new look's outfit number in temp 0 (the exe: as type 6's)
            if (answer != 0)
                return Exit::GotoFalse;
            if (flags & 16)
                t.temp_xl[(flags & 32) ? 1 : 0] = number;
            else
                t.temps[(flags & 32) ? 1 : 0] = number;
            return Exit::GotoTrue;
        default: return Exit::GotoTrue;
        }
    }
    if (f.thread->is_check)
        return Exit::GotoTrue;
    std::vector<std::string> table = vm.strings(file, 301);
    auto str = [&](int id) { return id > 0 && size_t(id - 1) < table.size() ? dialog_text(f, table[size_t(id - 1)]) : std::string(); };
    Vm::Dialog d;
    g_dialog_named = DialogNamed();
    g_dialog_named.armed = true;
    d.id = vm.next_dialog++;
    d.type = type;
    d.message = str(std::max<int>(1, op[2]));
    d.title = str(op[6]);
    d.yes = str(op[3]);
    d.no = str(op[4]);
    d.cancel = str(op[0]);
    if (type == 0)   // a message: only its OK (the other string ids are left over)
        d.no.clear();
    if (type <= 1)
        d.cancel.clear();
    d.caller = f.caller->id;
    d.icon_type = (flags >> 1) & 7;
    d.icon_object = f.stack_object() ? f.stack_object()->id : f.caller->id;
    // Picture type 0, as the exe's (FUN_00634b70, case 0): the person a name token in its strings named
    // (the last one), else the stack object. Ours took the stack object alone: the goodbye's "$Me" title
    // named Liz, but the stack object was the street's portal, so the box had no face (the user,
    // 2026-11-13). TS_DIALOG_NAMED_OLD=1: as before. (The strings were all filled above.)
    g_dialog_named.armed = false;
    if (d.icon_type == 0 && !TS_ENV("TS_DIALOG_NAMED_OLD")) {
        if (g_dialog_named.entity > 0)
            d.icon_object = uint16_t(g_dialog_named.entity);
        else if (g_dialog_named.neighbor > 0)
            d.named_neighbor = g_dialog_named.neighbor;
    }
    if (d.icon_type == 2)   // the neighbour numbered by the stack object's id (the original, 0x635000: as case 0 reads it)
        d.icon_neighbor = f.stack_id;
    d.icon_operand = op[1];
    d.icon_name = op[1] ? str(op[1]) : std::string();
    d.icon_file = file;
    if (TS_ENV("TS_DIALOG_ICON"))   // debug
        std::printf("dialog icon: type %d, stack id %d, temps %d %d, flags %d\n", d.icon_type, int(f.stack_id), t.temps[0],
                    t.temps[1], flags);
    d.source = f.routine->name + " #" + std::to_string(f.routine->id) + " temp0=" + std::to_string(t.temps[0]);
    d.blocking = !(flags & 1);
    const bool lot_chooser = type == 5 || type == 7 || type == 8 || type == 11 || type == 13;
    const bool pet_chooser = type == 9;   // the pet pens' coats (Unleashed): the pick's number in temp 0
    const bool clothes_chooser = type == 6;   // Hot Date's racks: the outfits of the type in temp 0, the pick's number in temp 0
    const bool transform_chooser = type == 14;   // Makin' Magic's Transform Me: the new look's outfit in temp 0
    if (type == 10 || lot_chooser || pet_chooser || clothes_chooser || transform_chooser) {   // the phone book, or where to go (FreeSO UINeighborhoodSelectionPanel)
        bool dog = true;
        d.choices = clothes_chooser     ? clothes_choices(vm, *f.caller, t.temps[0])
                    : pet_chooser       ? pet_choices(vm, f.stack_object(), &dog, &d.previews)
                    : lot_chooser       ? travel_lots(vm, type)
                    : transform_chooser ? transform_choices(*f.caller)
                                        : known_sims(vm, *f.caller);
        if (d.choices.empty())
            return Exit::GotoFalse;
        // The clothes and pet choosers are pickers (the original's UIText 220 "Clothing Dialog Text"
        // and 221 "Pet Dialog Strings": one outfit or pet at a time with Previous, Next, Buy and
        // Cancel); the viewer draws the shopper wearing each outfit, or the coat on a dog or cat.
        static const char *const kClothes[6] = { "Please Select Outfit", "Select Formalwear", "Select Swimsuit", "Select PJs",
                                                 "Select Winter Wear", "Select High Fashion" };
        d.title = clothes_chooser ? std::string(t.temps[0] >= 0 && t.temps[0] < 6 ? kClothes[t.temps[0]] : "Please Select Outfit")
                  : pet_chooser ? std::string("Please Select Your Pet")
                  : transform_chooser ? std::string("Choose the New You")   // (UIText STR# 273, 4)
                  : lot_chooser ? std::string(type == 7 ? "Vacation" : type == 5 ? "Downtown" : type == 11 ? "Studio Town"
                                                                      : type == 13 ? "Magic Town" : "Old Town")
                                : entity_name(f.caller);   // its string ids aren't the chooser's text
        if (clothes_chooser) {
            // The price: the rack's tuning "Cost" (BCON 4098) as its "Buy TEST" charges by rack type
            // (formal 256, swimwear 258, pyjamas 257, Superstar's 260: the racks' "Rack Clothing Type
            // To Dialog Type" numbers the dialog types 1-4 in that order). The dresser's own
            // "Change Normal Outfit" (type 0) has no cost table: nothing to pay.
            const int t0 = t.temps[0];
            const int cost_index = t0 == 2 ? 258 : t0 == 3 ? 257 : t0 == 4 ? 260 : 256;
            d.price = t0 >= 1 && t0 <= 4 ? std::max<int>(0, vm.tuning(f, uint16_t(cost_index))) : 0;
            d.previews = vm.suits_of(*f.caller, t0);
            d.preview_kind = 1;
            d.preview_start = std::max<int>(0, vm.suit_index(*f.caller, t0));
            d.message.clear();
        } else if (pet_chooser) {
            d.preview_kind = dog ? 2 : 3;
            d.message.clear();
        } else if (transform_chooser) {   // the Sim drawn in each look (an outfit number)
            for (const auto &[outfit, name] : d.choices)
                d.previews.push_back(std::to_string(outfit));
            d.preview_kind = 4;
            d.message = "Beauty or Beast?";   // (UIText STR# 273, 2)
        } else
            d.message = lot_chooser ? "Where do you want to go?" : "Who do you want to call?";
        d.type = 10;   // shown as a list either way
        d.no_cancel = lot_chooser;
        d.yes.clear();
        d.no.clear();
        d.cancel.clear();
        if (vm.auto_answer) {   // nobody to ask: the first one (TS_DIALOG_CHOICE=<n> another, for tests)
            static const char *const choice_env = TS_ENV("TS_DIALOG_CHOICE");
            const size_t pick = choice_env ? std::min<size_t>(size_t(std::max(0, std::atoi(choice_env))), d.choices.size() - 1) : 0;
            if (flags & 16)
                t.temp_xl[(flags & 32) ? 1 : 0] = d.choices[pick].first;
            else
                t.temps[(flags & 32) ? 1 : 0] = d.choices[pick].first;
            return Exit::GotoTrue;
        }
        d.blocking = true;
        vm.dialogs.push_back(d);
        t.dialog_waiting = d.id;
        return Exit::ContinueNextTick;
    }
    if (d.message.empty() && d.title.empty())
        return Exit::GotoTrue;
    if (vm.auto_answer || type == 5 || type >= 6) {   // nothing to ask with, or input we can't take yet
        if (!d.blocking || type == 0 || type == 3)
            return Exit::GotoTrue;
        if (vm.auto_answer_no && (type == 1 || type == 2)) {   // the second button (run_vm `answer no`)
            if (type == 2)
                t.temps[(flags & 32) ? 1 : 0] = 0;
            return Exit::GotoFalse;
        }
        return Exit::GotoTrue;   // the first button
    }
    // Quick Tips off: the help systems' boxes aren't shown, their questions answered no (it then
    // turns itself off, as when the player says no). Not global 26 (we had it as the option): the
    // original's option is the program's own (its tooltips' delay), and global 26 = 1 also hides
    // every trip on the phone (the plugins' "global 26 == 0"), so the cab never offered Downtown.
    if (!vm.quick_tips && f.caller && f.caller->info) {
        std::string where = f.caller->info->where;
        for (char &c : where)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (where.find("helpsystem") != std::string::npos)
            return type == 1 ? Exit::GotoFalse : Exit::GotoTrue;
    }
    vm.dialogs.push_back(d);
    if (!d.blocking)
        return Exit::GotoTrue;
    t.dialog_waiting = d.id;
    return Exit::ContinueNextTick;
}

Exit dialog_private(Frame &f, const uint8_t *op)
{
    return dialog_with(f, op, f.owner.own);
}

Exit dialog_global(Frame &f, const uint8_t *op)
{
    return dialog_with(f, op, f.vm().global_code.own);
}

Exit dialog_semiglobal(Frame &f, const uint8_t *op)
{
    return dialog_with(f, op, f.owner.semi ? f.owner.semi : f.owner.own);
}

// Make New Character (Sims 1 opcode 19, FreeSO VMTS1MakeNewCharacter): a new
// Sim (a baby growing into a child), its skin, age and sex from three locals;
// its neighbour id goes in the stack object. Or a pet (operand 3: 1 a dog, 2 a
// cat; the pens' "Buy a Pet"): its coat is the stack object's attribute 8 (the
// pen's, from the chooser dialog), its name the pen's (the name dialog's), as
// FreeSO reads them.
Exit make_new_character(Frame &f, const uint8_t *op)
{
    auto local = [&](int i) { return size_t(i) < f.locals.size() ? f.locals[size_t(i)] : int16_t(0); };
    if (op[3] == 1 || op[3] == 2) {
        const Entity *pen = f.stack_object();
        const int16_t id = f.vm().make_pet(op[3] == 1, pen ? pen->attribute(8) : 0, local(op[2]) > 0,
                                            pen ? pen->custom_name : std::string());
        if (!id)
            return Exit::GotoFalse;
        f.stack_id = id;
        return Exit::GotoTrue;
    }
    if (op[3] != 0)
        return Exit::GotoFalse;
    const int16_t id = f.vm().make_character(local(op[1]), local(op[2]), local(op[0]));
    if (!id)
        return Exit::GotoFalse;
    f.stack_id = id;
    return Exit::GotoTrue;
}

// Gosub Found Action (Sims 1 opcode 30, FreeSO VMGosubFoundAction): main
// starts the interaction autonomy just queued; when it ends this runs again
// and goes on (false) once nothing more can start.
Exit gosub_found_action(Frame &f, const uint8_t *)
{
    Thread &t = *f.thread;
    if (!t.queue.empty() && t.in_interaction())
        f.stack_id = t.queue[0].callee;
    if (!f.action_tree && t.attempt_push())
        return Exit::Continue;
    return Exit::GotoFalse;
}

// Burn: sets fire to the stack object's tile (or the one in front of it), as
// the original's primitive does (FUN_00613a20; FreeSO's VMBurn differs). The
// tile must be inside the lot's edge; a fire can't go in front through a wall
// (only some fences let it), onto a diagonal wall's tile, onto a fireproof
// object or where a fire already burns. A burnable object there burns whole,
// one fire per tile of it, unless any part of it is busy (and the operand
// doesn't say to burn busy objects). How fast fires spread is the fire
// object's own code: the primitive itself has no chance to fail.
Exit burn(Frame &f, const uint8_t *op)
{
    Vm &vm = f.vm();
    constexpr uint32_t kFireGuid = 0x24C95F99;
    constexpr uint16_t kBurns = 1 << 5, kFireProof = 1 << 11;
    const int type = op[0];   // 0 the stack object, 1 in front of it, 2 the floor under it
    const bool burn_busy = op[1] & 1;
    Entity *so = f.stack_object();
    if (!so || !so->in_world())
        return Exit::GotoFalse;
    int x = so->tile_x(), y = so->tile_y();
    const int level = so->level;
    if (type == 1) {
        x += kStep[so->direction & 7][0];
        y += kStep[so->direction & 7][1];
    }
    if (x < 1 || y < 1 || x >= vm.lot.size - 1 || y >= vm.lot.size - 1)
        return Exit::GotoFalse;
    if (type == 1) {
        const int l = level - 1;
        if (l >= 0 && l < kLotLevels && !vm.lot.walls[l].empty()) {
            const int sx = so->tile_x(), sy = so->tile_y();
            // The edge between the two tiles: the top-left edge of the one further along x, or the
            // top-right edge of the one further along y, which keeps its style.
            const WallTile *owner = nullptr;
            bool left = false;
            if (x == sx + 1 && y == sy)
                owner = &vm.lot.wall(x, y, l), left = true;
            else if (x == sx - 1 && y == sy)
                owner = &vm.lot.wall(sx, sy, l), left = true;
            else if (y == sy + 1 && x == sx)
                owner = &vm.lot.wall(x, y, l);
            else if (y == sy - 1 && x == sx)
                owner = &vm.lot.wall(sx, sy, l);
            if (owner && (owner->segments & (left ? WallTopLeft : WallTopRight))) {
                const uint16_t style = left ? owner->top_left_style : owner->top_right_style;
                if (style != 3 && style != 5 && style != 6 && style != 15 && style != 23)
                    return Exit::GotoFalse;   // a wall in the way (fences of those styles let it through)
            }
            if (vm.lot.wall(x, y, l).segments & (WallHorizontalDiag | WallVerticalDiag))
                return Exit::GotoFalse;
        }
    }
    const ObjectInfo *fire_info = vm.content.object(kFireGuid);
    if (!fire_info)
        return Exit::GotoFalse;
    auto on_tile = [&](int tx, int ty) {
        std::vector<Entity *> here;
        for (auto &e : vm.entities)
            if (!e->dead && e->in_world() && !e->container && e->level == level && e->tile_x() == tx && e->tile_y() == ty)
                here.push_back(e.get());
        return here;
    };
    auto fire_at = [&](int tx, int ty) {
        for (const Entity *e : on_tile(tx, ty))
            if (e->info && e->info->guid == kFireGuid)
                return true;
        return false;
    };
    auto burns = [&](const Entity &e) { return (uint16_t(e.value(VarFlagField2)) & kBurns) != 0; };
    Entity *burnable = nullptr;
    for (Entity *e : on_tile(x, y)) {
        if (burns(*e))
            burnable = e;
        if (e->flag(kFireProof) || (e->info && e->info->guid == kFireGuid))
            return Exit::GotoFalse;
    }
    auto make_fire = [&](int tx, int ty) {
        Entity *fire = vm.create_with_parts(*fire_info, tx, ty, level - 1, 0);
        if (fire)
            fire->run_entry(vm, 0, true);   // init; main runs on its thread
        return fire != nullptr;
    };
    if (!burnable)
        return truth(make_fire(x, y));
    // What burns with it: all its parts, and what burns on their tiles.
    std::vector<Entity *> burning{ burnable };
    for (size_t i = 0; i < burning.size(); i++) {
        Entity *e = burning[i];
        std::vector<Entity *> more = e->group;
        if (burns(*e))
            for (Entity *other : on_tile(e->tile_x(), e->tile_y()))
                if (burns(*other))
                    more.push_back(other);
        for (Entity *m : more)
            if (m->level == level && std::find(burning.begin(), burning.end(), m) == burning.end())
                burning.push_back(m);
    }
    auto in_use = [&](const Entity &e) {
        if (e.flag(FlagOccupied) || (e.person && e.person_data[68] > 0))
            return true;
        for (auto &p : vm.entities)
            if (p->person && !p->dead && p->thread && p.get() != &e)
                for (const Frame &fr : p->thread->stack)
                    if (fr.callee == &e)
                        return true;
        return false;
    };
    for (const Entity *e : burning)
        if (e->flag(kFireProof) || (!burn_busy && in_use(*e)))
            return Exit::GotoFalse;
    bool made = false;
    for (const Entity *e : burning) {
        const int tx = e->tile_x(), ty = e->tile_y();
        if (tx < 0 || ty < 0 || tx >= vm.lot.size || ty >= vm.lot.size || fire_at(tx, ty))
            continue;
        made = make_fire(tx, ty) || made;
    }
    return truth(made);
}

// The needs a traveller takes along, in the order of the game's sim data (0 bladder,
// 1 comfort, 2 energy, 3 fun, 4 hunger, 5 hygiene, 6 social): their motive numbers.
constexpr int kTravelMotives[7] = { 9, 6, 5, 15, 7, 8, 14 };

// A Sim arriving on this lot: their person object made out of the world and
// started (the lot's code brings them in, by cab or the like).
Entity *spawn_traveller(Vm &vm, uint32_t guid)
{
    const ObjectInfo *info = vm.content.object(guid);
    if (!info)
        return nullptr;
    Entity *sim = vm.create_with_parts(*info, -1, -1, 0, 0);
    if (sim) {
        sim->run_entry(vm, 0, true);   // init
        // Their family number (person data 61): the vacation spawner finds the arrivals by it.
        if (vm.hood && vm.hood->travel.family && sim->person_data.size() > 61 && sim->person_data[61] == 0)
            sim->person_data[61] = vm.hood->travel.family;
    }
    return sim;
}

bool start_trip(Vm &vm, Entity &traveller, int target, bool whole_family, int add_hours = 0, int add_minutes = 0)
{
    constexpr int kFamilyNumber = 61;   // person data TS1FamilyNumber
    Neighborhood *hood = vm.hood;
    auto &entities = vm.entities;
    const auto &clock = vm.clock;
    const int house = vm.house;
    auto family = [&vm] { return vm.family(); };
    auto global = [&vm](int i) { return vm.global(i); };
    if (!hood)
        return false;
    Travel &tr = hood->travel;
    const Family *fam = family();
    // Only to a lot there is (or home, -1): a chooser answered with nothing (Makin' Magic's "Ask
    // for Lot" goes on either way, temp 0 left as the Sim's own id) sent the Bobs to "house 200":
    // they were stored for a lot that never opened and vanished from their home (2026-11-02).
    if (target != -1 && (target <= 0 || !hood->houses.count(target))) {
        std::printf("trip: no lot %d to go to; nobody goes\n", target);
        return false;
    }
    const bool vacation = target >= 40 && target < 50;
    tr.family = fam ? fam->id : 0;
    tr.sim_guid = traveller.info ? traveller.info->guid : 0;
    // Who goes (1: the whole family): a vacation always; Makin' Magic's plugin sets global 34
    // (FreeSO reads that alone); Unleashed's phone plugin passes it to the call in temp 3
    // instead (its "Interaction - Go To Community": temp 3 := the plugin's attribute 8, set for
    // Old Town's shuttle, whose box says to bring the whole family — the original takes all
    // three Goths, 2026-10-31). The debug trip takes the one Sim.
    tr.transit = vacation ? 1 : global(34) ? global(34) : whole_family ? 1 : 0;
    tr.from_house = house;
    tr.to_house = target == -1 ? (fam ? fam->house : 0) : target;
    // Each traveller's needs and the time they left, for the lot they arrive on.
    std::vector<Entity *> people{ &traveller };
    if (tr.transit >= 1 && fam)
        for (auto &e : entities)
            if (e->person && !e->dead && e.get() != &traveller && e->person_data[kFamilyNumber] == fam->id)
                people.push_back(e.get());
    // Going alone, their date (a kind 1 token; going home, kind 3) goes with them, and their needs
    // are kept too (the exe's FUN_005f5fb0); the lot they arrive on brings them in (generic call 19/20).
    if (tr.transit == 0 && !TS_ENV("TS_TRIP_TOKENS_OLD")) {
        const uint8_t k = target == -1 ? 3 : 1;
        for (const Entity::Token &x : traveller.tokens)
            if (x.kind == k) {
                for (auto &e : entities)
                    if (e->person && !e->dead && e.get() != &traveller && e->info && e->info->guid == x.guid)
                        people.push_back(e.get());
                break;
            }
    }
    for (size_t i = 0; i < people.size(); i++) {
        auto &data = tr.sim_data[people[i]->person_data[31]];
        for (int k = 0; k < 7; k++)
            data[k] = people[i]->motives[size_t(kTravelMotives[k])];
        if (tr.transit > 1 && fam)
            data[9] = int16_t(fam->members.size());
        if (i == 0) {
            // The time they get there (FUN_005f5fb0): temps 1 and 2 on from now, or just those, a
            // vacation from a home. The lot they arrive on takes it up.
            int hour = add_hours, minute = add_minutes;
            const bool from_home = hood->zoning.count(house) && hood->zoning.at(house) == 0;
            if (!(from_home && vacation)) {
                hour = clock.hours;
                minute = clock.minutes + add_minutes;
                if (minute > 59) {
                    hour++;
                    minute %= 60;
                }
                hour += add_hours;
                if (hour > 23)
                    hour %= 24;
            }
            data[7] = int16_t(hour);
            data[8] = int16_t(minute);
            tr.arrive_hour = hour;
            tr.arrive_minute = minute;
        }
    }
    if (tr.to_house <= 0)
        return false;
    vm.store_people();   // the lot they arrive on builds them as they are now
    vm.switch_to_house = tr.to_house;
    return true;
}

// Generic Sims call (TS1): what the engine does for the code (FreeSO VMGenericTS1Call).
Exit find_five_worst_motives(Frame &f, const uint8_t *)
{
    Entity &sim = *f.caller;
    if (!sim.person)
        return Exit::GotoTrue;
    std::vector<std::pair<int16_t, int>> needs;
    for (int m : { 5, 6, 7, 8, 9, 13, 14, 15 })
        needs.push_back({ sim.motives[size_t(m)], m });
    std::stable_sort(needs.begin(), needs.end());
    for (size_t i = 0; i < 5 && i < needs.size(); i++)
        f.thread->temps[i] = int16_t(needs[i].second);
    return Exit::GotoTrue;
}

// Special Effect (35). The original (FUN_0061d5d0, then FUN_0040af10) points the player at the
// stack object: flag (op 4) bit 3 set moves the view there (the fire's 0x09, the baby's 0x19);
// clear posts a notice for it (FUN_004c6e10: the burglar's 0x07, the car pool's 0x43), shown only
// while the object is off screen unless bit 2; bit 0 clear ends the notice. Bit 5 takes the
// seconds from local <op 0-1> instead of op 0-1 itself; op 5 names a line of the caller's STR#
// 305; op 2-3 (the notice's shape) and bits 1, 4, 6 are left. Vm::Attention carries it to the
// interface.
Exit special_effect(Frame &f, const uint8_t *op)
{
    Vm &vm = f.vm();
    Entity *so = f.stack_object();
    if (!so || f.thread->is_check)
        return Exit::GotoTrue;
    const int flags = op[4], value = int16_t(op[0] | op[1] << 8);
    Vm::Attention a;
    a.object = so->id;
    a.start = (flags & 1) != 0;
    a.centre = (flags & 8) != 0;
    a.always = (flags & 4) != 0;
    a.seconds = (flags & 0x20) ? (value >= 0 && size_t(value) < f.locals.size() ? f.locals[size_t(value)] : 0) : value;
    if (op[5]) {
        const std::vector<std::string> table = vm.strings(f.owner.own, 305);
        if (size_t(op[5] - 1) < table.size())
            a.text = dialog_text(f, table[size_t(op[5] - 1)]);
    }
    a.source = (f.caller->info ? f.caller->info->label : std::string("?")) + ": " + f.routine->name;
    if (vm.attention.size() >= 32)   // nobody taking them (run_vm without the trace): the oldest go
        vm.attention.erase(vm.attention.begin());
    vm.attention.push_back(std::move(a));
    return Exit::GotoTrue;
}

Exit generic_call(Frame &f, const uint8_t *op)
{
    Vm &vm = f.vm();
    auto &temps = f.thread->temps;
    constexpr int kFamilyNumber = 61;   // person data TS1FamilyNumber
    switch (op[0]) {
    case 0:   // house tutorial complete: no tutorial here
        return Exit::GotoTrue;
    case 2: {   // the running action's picture is the stack object's (the plate, not the fridge: FreeSO SetActionIconToStackObject)
        if (f.thread && !f.thread->queue.empty())
            f.thread->queue[0].icon_owner = f.stack_id;
        return Exit::GotoTrue;
    }
    case 15:   // my routing footprint equals temp 0 (person data 72), its shrink (73) cleared: the exe's case 0xf
        if (f.caller->person && f.caller->person_data.size() > 73) {
            f.caller->person_data[72] = temps[0];
            f.caller->person_data[73] = 0;
        }
        return Exit::GotoTrue;
    case 1: {   // swap my and the stack object's slots
        Entity *me = f.caller, *so = f.stack_object();
        if (me && so && me->container && so->container) {
            Entity *c1 = me->container, *c2 = so->container;
            const int s1 = me->container_slot, s2 = so->container_slot;
            me->leave_container();
            so->leave_container();
            c1->place_in_slot(so, s1);
            c2->place_in_slot(me, s2);
        }
        break;
    }
    case 4: {   // add the neighbour in the stack object to the family living here
        Family *fam = vm.family();
        const Neighbor *n = vm.hood ? vm.hood->by_id(f.stack_id) : nullptr;
        if (!fam || !n || fam->members.size() >= 8)
            return Exit::GotoFalse;
        for (Family &other : vm.hood->families)   // out of the family they were in
            if (&other != fam)
                other.members.erase(std::remove(other.members.begin(), other.members.end(), n->guid), other.members.end());
        if (std::find(fam->members.begin(), fam->members.end(), n->guid) == fam->members.end())
            fam->members.push_back(n->guid);
        if (TS_ENV("TS_QUEUE_WHY"))   // debug
            std::printf("family: %s (neighbour %d) joins the %s family, now %lu members\n", n->name.c_str(), int(n->id),
                        fam->name.c_str(), (unsigned long)fam->members.size());
        int16_t resident_type = 0;   // what the family's Sims have in person data 32 (global tuning 260 entry 0)
        for (auto &e : vm.entities)
            if (e->person && !e->dead && e->person_data[31] != n->id && vm.resident(*e))
                resident_type = e->person_data[32];
        for (auto &e : vm.entities)
            if (e->person && !e->dead && e->person_data[31] == n->id) {
                e->person_data[kFamilyNumber] = fam->id;
                e->person_data[32] = resident_type;   // lives here from now on
            }
        if (Neighbor *nb = vm.hood->by_id(n->id); nb && nb->person_data.size() > kFamilyNumber)
            nb->person_data[kFamilyNumber] = fam->id;
        break;
    }
    case 5: {   // a family moving in brings its money (the family in temp 0)
        Family *fam = vm.family();
        if (fam && vm.hood)
            for (Family &other : vm.hood->families)
                if (other.id == temps[0] && &other != fam)
                    fam->budget += other.budget + other.net_worth;
        break;
    }
    case 6: {   // the neighbour in the stack object leaves the family (moving out, dying)
        Family *fam = vm.family();
        const Neighbor *n = vm.hood ? vm.hood->by_id(f.stack_id) : nullptr;
        if (!fam || !n)
            return Exit::GotoFalse;
        fam->members.erase(std::remove(fam->members.begin(), fam->members.end(), n->guid), fam->members.end());
        for (auto &e : vm.entities)
            if (e->person && !e->dead && e->person_data[31] == n->id)
                e->person_data[kFamilyNumber] = 0;
        if (Neighbor *nb = vm.hood->by_id(n->id); nb && nb->person_data.size() > kFamilyNumber)
            nb->person_data[kFamilyNumber] = 0;
        break;
    }
    // No buying or building while any object asks it (a fire, a party ...): a list of who asked,
    // as the original's (FUN_0052d220 / FUN_0052dd70); one fire going out doesn't open it while
    // another burns. (Ours was one switch.)
    case 10:
        if (std::find(vm.build_buy_locks.begin(), vm.build_buy_locks.end(), int16_t(f.caller->id)) == vm.build_buy_locks.end())
            vm.build_buy_locks.push_back(int16_t(f.caller->id));
        vm.build_buy_enabled = false;
        break;
    case 11:
        vm.build_buy_locks.erase(std::remove(vm.build_buy_locks.begin(), vm.build_buy_locks.end(), int16_t(f.caller->id)),
                                 vm.build_buy_locks.end());
        vm.build_buy_enabled = vm.build_buy_locks.empty();
        break;
    case 13:   // everyone else stops what they're doing with the stack object (FUN_0052c9b0)
        if (Entity *so = f.stack_object())
            for (auto &e : vm.entities)
                if (e->person && !e->dead && e.get() != so && e->thread) {
                    std::vector<uint16_t> uids;
                    for (const QueuedAction &q : e->thread->queue)
                        if (q.callee == int16_t(so->id))
                            uids.push_back(q.uid);
                    for (uint16_t uid : uids)
                        e->thread->cancel(uid);
                }
        break;
    case 12: temps[0] = 0; break;                   // distance to the camera
    case 14: vm.set_global(31, temps[0]); break;    // the house's radio station
    case 17:   // go to the lot in temp 0 (-1: the family's home), the whole family when temp 3: FreeSO ChangeToLotInTemp0
        if (!f.caller->person || !ts::vm::start_trip(vm, *f.caller, temps[0], temps[3] != 0, temps[1], temps[2]))
            return Exit::GotoFalse;
        return Exit::GotoTrueNextTick;
    case 18: {   // build the Sim who travelled here, out of the world, selected: temp 0 its id
        const Travel *tr = vm.hood ? &vm.hood->travel : nullptr;
        Entity *sim = tr && tr->to_house == vm.house && tr->sim_guid ? spawn_traveller(vm, tr->sim_guid) : nullptr;
        if (!sim) {   // nobody coming (the lot opened from the house picker)
            temps[0] = -1;
            break;
        }
        vm.set_global(3, int16_t(sim->id));
        vm.selected_id = int16_t(sim->id);
        temps[0] = int16_t(sim->id);
        break;
    }
    case 19: case 20: {   // the date (19) or the Sim to take home (20) of the person in temp 0: temp 0 them, or -1
        Entity *who = vm.entity(temps[0]);
        temps[0] = -1;
        if (!who || !who->person || !vm.hood)
            return Exit::GotoFalse;
        // As the exe's (FUN_00617d70 cases 0x13/0x14): their first token of kind 1 (19) or 3 (20)
        // names the Sim; made here if not on the lot (FUN_004391f0); the token used up.
        static const bool trip_old = TS_ENV("TS_TRIP_TOKENS_OLD") != nullptr;
        if (!trip_old) {
            const uint8_t k = op[0] == 19 ? 1 : 3;
            auto it = std::find_if(who->tokens.begin(), who->tokens.end(), [&](const Entity::Token &x) { return x.kind == k; });
            if (it == who->tokens.end())
                break;
            const uint32_t guid = it->guid;
            who->tokens.erase(it);
            Entity *other = nullptr;
            for (auto &e : vm.entities)
                if (e->person && !e->dead && e->info && e->info->guid == guid)
                    other = e.get();
            if (!other)
                other = spawn_traveller(vm, guid);
            if (other)
                temps[0] = int16_t(other->id);
            break;
        }
        auto it = vm.hood->travel.sim_data.find(who->person_data[31]);
        const int key = op[0] == 19 ? 10 : 11;
        if (it == vm.hood->travel.sim_data.end() || !it->second.count(key))
            break;
        const Neighbor *n = vm.hood->by_id(it->second[key]);
        it->second.erase(key);
        if (Entity *other = n ? spawn_traveller(vm, n->guid) : nullptr)
            temps[0] = int16_t(other->id);
        break;
    }
    case 21: {   // the needs the person in temp 0 travelled with
        Entity *who = vm.entity(temps[0]);
        if (!who || !who->person || !vm.hood)
            break;
        auto it = vm.hood->travel.sim_data.find(who->person_data[31]);
        if (it == vm.hood->travel.sim_data.end())
            break;
        for (int k = 0; k < 7; k++)
            if (auto v = it->second.find(k); v != it->second.end()) {
                who->motives[size_t(kTravelMotives[k])] = v->second;
                it->second.erase(v);
            }
        break;
    }
    case 22: break;   // choose a downtown lot: the chooser dialog does it (dialog type 5)
    case 23: {   // the time the stack object left the lot it was on: temp 0 hours, temp 1 minutes
        // (going home, they keep away until the house's clock gets there; now if not known)
        temps[0] = int16_t(vm.clock.hours);
        temps[1] = int16_t(vm.clock.minutes);
        const Entity *so = f.stack_object();
        if (so && so->person && vm.hood)
            if (auto it = vm.hood->travel.sim_data.find(so->person_data[31]); it != vm.hood->travel.sim_data.end()) {
                if (auto h = it->second.find(7); h != it->second.end())
                    temps[0] = h->second;
                if (auto m = it->second.find(8); m != it->second.end())
                    temps[1] = m->second;
            }
        break;
    }
    case 24:   // Hot Date: change suits permanently (temp 0 the clothing type, temp 1 the outfit's number); the one before in temp 1
        if (TS_ENV("TS_SUITS"))   // debug: who asks, from where
            std::printf("generic 24: #%d type %d outfit %d in %s\n", int(f.caller->id), int(temps[0]), int(temps[1]),
                        f.routine ? f.routine->name.c_str() : "?");
        temps[1] = f.caller->person ? vm.set_suit(*f.caller, temps[0], temps[1]) : int16_t(-1);
        break;
    case 29: {   // set the stack object's suit (temps as 24)
        Entity *so = f.stack_object();
        temps[1] = so && so->person ? vm.set_suit(*so, temps[0], temps[1]) : int16_t(-1);
        break;
    }
    case 30: {   // get the stack object's suit: temp 1 the number of the type-in-temp-0 outfit they wear, -1 none
        Entity *so = f.stack_object();
        temps[1] = so && so->person ? vm.suit_index(*so, temps[0]) : int16_t(-1);
        break;
    }
    case 31: {   // count the stack object's suits of the type in temp 0, in temp 0
        Entity *so = f.stack_object();
        temps[0] = so && so->person ? int16_t(vm.suits_of(*so, temps[0]).size()) : int16_t(0);
        break;
    }
    case 25: break;   // save the Sim's persistent data: the neighbourhood save keeps it
    case 26: {   // build the vacation family: temp 0 a Sim of it, temp 1 whether all came
        const Travel *tr = vm.hood ? &vm.hood->travel : nullptr;
        if (!tr || tr->to_house != vm.house || !tr->sim_guid)   // nobody arriving (the lot opened from the house
            return Exit::ContinueNextTick;                       // picker): the spawner waits, else it sends cabs for no one
        Entity *first = nullptr;
        const Family *fam = vm.hood->family_by_id(tr->family);
        if (tr->transit >= 1 && fam) {
            for (uint32_t guid : fam->members) {
                bool here = false;
                for (auto &e : vm.entities)
                    here |= e->person && !e->dead && e->info && e->info->guid == guid;
                Entity *sim = here ? nullptr : spawn_traveller(vm, guid);
                if (sim && (!first || guid == tr->sim_guid))
                    first = sim;
            }
        } else {
            first = spawn_traveller(vm, tr->sim_guid);
        }
        if (first) {
            vm.set_global(3, int16_t(first->id));
            vm.selected_id = int16_t(first->id);
        }
        vm.set_global(9, fam ? fam->id : 0);
        // Temp 0: the family's number when the whole family came (FreeSO's name for the call:
        // BuildVacationFamilyPutFamilyNumInTemp0; the spawner's tree walks the people whose person
        // data 61 equals it, places each at the marker and pushes its arrival on them), else the
        // one Sim's id. A Sim's id here meant nobody matched and the Goths stayed out of the
        // world at every vacation lot (2026-10-31).
        temps[1] = int16_t(tr->transit >= 1 ? 1 : 0);
        temps[0] = tr->transit >= 1 && fam ? int16_t(fam->id) : first ? int16_t(first->id) : -1;
        break;
    }
    case 27: temps[0] = 9; break;   // vacation lots free (FreeSO: other families' saves don't block them yet)
    case 28: {                                      // zoning of the lot numbered in temp 0
        // Downtown (21-30) and vacation (40-49) lots, which LotZoning doesn't list, are neither
        // residential (0: a Sim downtown sent themselves home as a visitor) nor community (1:
        // Unleashed's Old Town phone adder looks for its marker and stops at a breakpoint): 2,
        // as Studio Town's lots are in FreeSO.
        const int h = temps[0];
        int zone = ((h >= 81 && h <= 89) || (h >= 21 && h <= 30) || (h >= 40 && h <= 49)) ? 2 : 0;
        if (vm.hood) {
            auto it = vm.hood->zoning.find(temps[0]);
            if (it != vm.hood->zoning.end())
                zone = it->second;
        }
        if (const char *z = TS_ENV("TS_ZONING"); z && h == vm.global(10))   // debug: this lot's zoning forced (a pet shop at home)
            zone = std::atoi(z);
        temps[0] = int16_t(zone);
        break;
    }
    case 32: break;   // create purchased pets near owner (coming home from a pet shop): a pet joined the family at the
                      // shop (call 4) and the home's load puts every member of the family on the lot, the pet with them
    case 33: {        // add the neighbour in the stack object to the family in temp 0 (a pet running away to, or joining, a family)
        if (!vm.hood)
            return Exit::GotoFalse;
        Family *fam = nullptr;
        for (Family &o : vm.hood->families)
            if (o.id == temps[0])
                fam = &o;
        const Neighbor *n = vm.hood->by_id(f.stack_id);
        if (!fam || !n || fam->members.size() >= 8)
            return Exit::GotoFalse;
        for (Family &other : vm.hood->families)
            if (&other != fam)
                other.members.erase(std::remove(other.members.begin(), other.members.end(), n->guid), other.members.end());
        if (std::find(fam->members.begin(), fam->members.end(), n->guid) == fam->members.end())
            fam->members.push_back(n->guid);
        if (Neighbor *nb = vm.hood->by_id(n->id); nb && nb->person_data.size() > kFamilyNumber)
            nb->person_data[kFamilyNumber] = fam->id;
        break;
    }
    case 38: {   // the numbers in a string: temp 0 a STR# of the running routine's file, temp 1 the string
        // (0-based); up to 7 numbers into temps 1.., their count in temp 0. No table or string: temp 0 -1 and
        // false. Temp 0 of 0: 0, true. (The exe's case 0x26; Makin' Magic's spell book reads its recipes.)
        const int table = temps[0], index = temps[1];
        temps[0] = 0;
        if (table == 0)
            break;
        const uint16_t rid = f.routine ? f.routine->id : 0;
        const LoadedIff *file = rid >= 8192 ? f.owner.semi : rid >= 4096 ? f.owner.own : vm.global_code.own;
        const StringTable *t = vm.string_table(file, "STR#", uint16_t(table));
        const std::string *s = t && index >= 0 ? t->get(size_t(index)) : nullptr;
        if (!s) {
            temps[0] = -1;
            return Exit::GotoFalse;
        }
        int count = 0;
        for (size_t i = 0; i < s->size() && count < 7;) {
            if (std::isdigit(uint8_t((*s)[i]))) {   // (digits only: the exe's reader takes no sign)
                int v = 0;
                while (i < s->size() && std::isdigit(uint8_t((*s)[i])))
                    v = v * 10 + ((*s)[i++] - '0');
                temps[size_t(1 + count++)] = int16_t(v);
            } else {
                i++;
            }
        }
        temps[0] = int16_t(count);
        break;
    }
    case 39:   // child to adult, pet to adult (Makin' Magic's "Grow Me"): the exe queues the person in temp 0
    case 40:   // to be remade grown up and answers with the new one in temp 1 (FUN_005570a0/FUN_00557190).
        // Ours at once (the exe answers the tick after, the request done between ticks). A failed
        // one: temp 1 0, true, as the exe's (the charm places nothing). Done: temp 1 the grown one;
        // temp 0 0 when that's a new object (the pet's gone, and with it this thread).
    {
        Entity *who = vm.entity(temps[0]);
        int16_t grown = 0;
        if (who && who->person)
            grown = op[0] == 39 ? vm.grow_up(*who) : vm.pet_to_person(*who);
        temps[1] = grown;
        if (grown && grown != temps[0]) {
            Entity *old = who;
            temps[0] = 0;
            remove_entity(vm, old, f);
        }
        break;
    }
    case 34:   // promote fame if needed (Superstar: FUN_00593b70), the stack object; false: no change
    case 36: { // demote fame if needed (FUN_00591ee0)
        Entity *so = f.stack_object();
        if (!so || !so->person)
            return Exit::Error;   // (the exe's error 0x1c)
        const bool changed = op[0] == 34 ? fame_promote(vm, *so) : fame_demote(vm, *so);
        if (TS_ENV("TS_FAME"))   // debug
            std::printf("fame: %s #%d %s: level %d, points %d, star power %d\n", so->info ? so->info->label.c_str() : "?",
                        int(so->id), op[0] == 34 ? "promote" : "demote", int(so->person_data[81]), int(so->person_data[80]),
                        friends_star_power(vm, so->person_data[31]));
        return changed ? Exit::GotoTrue : Exit::GotoFalse;
    }
    case 42: {                                      // make the Sim in temp 0 the selected one
        Entity *e = vm.entity(temps[0]);
        if (!e || !e->person)
            return Exit::GotoFalse;
        vm.set_global(3, temps[0]);
        vm.selected_id = temps[0];
        break;
    }
    default: break;
    }
    return Exit::GotoTrue;
}

// Play Sound: the event named by an FWAV chunk of the code's files.
Exit play_sound(Frame &f, const uint8_t *op);

} // namespace (reopened below)

bool Vm::start_trip(Entity &traveller, int house)
{
    return ts::vm::start_trip(*this, traveller, house, false);
}

const FameTables &fame_tables(Content &content)
{
    static FameTables t;
    static bool read = false;
    if (read)
        return t;
    read = true;
    const FileRef *ref = content.file("fame.iff");
    const LoadedIff *file = ref ? content.load(*ref) : nullptr;
    auto table = [&](uint16_t id) {
        std::vector<std::string> out;
        StringTable st;
        std::string err;
        if (const IffChunk *c = file ? file->iff.find("STR#", id) : nullptr; c && parse_str(file->body(*c), c->size, st, err))
            for (size_t i = 0; const std::string *s = st.get(i); i++)
                out.push_back(*s);
        return out;
    };
    for (const std::string &s : table(5))
        t.points.push_back(std::atoi(s.c_str()));
    for (const std::string &s : table(4))
        t.star_power.push_back(std::atoi(s.c_str()));
    for (const std::string &s : table(6)) {   // "%d,%d,%d,%d,%d,%d"
        std::array<int, 6> row{};
        size_t at = 0;
        for (int k = 0; k < 6 && at <= s.size(); k++) {
            const size_t comma = std::min(s.find(',', at), s.size());
            row[size_t(k)] = std::atoi(s.substr(at, comma - at).c_str());
            at = comma + 1;
        }
        t.skills.push_back(row);
    }
    const std::vector<std::string> panel = table(1);
    for (size_t i = 1; i < panel.size() && i <= 11; i++)   // (entry 0 is the panel's heading)
        t.titles.push_back(panel[i]);
    return t;
}

int friends_star_power(const Vm &vm, int16_t id)
{
    constexpr int kFamilyNumber = 61, kFameLevel = 81, kFriendshipThreshold = 50;   // (Global.far's tuning)
    const Neighbor *me = vm.hood ? vm.hood->by_id(id) : nullptr;
    if (!me)
        return 0;
    const int my_family = me->person_data.size() > kFamilyNumber ? me->person_data[kFamilyNumber] : 0;
    int sum = 0;
    for (const auto &[other, values] : me->relationships) {
        const Neighbor *o = vm.hood->by_id(int16_t(other));
        if (!o || o == me || values.empty() || values[0] < kFriendshipThreshold || o->person_data.size() <= kFameLevel)
            continue;
        const int family = o->person_data[kFamilyNumber];
        if (family == 0 || family == my_family)
            continue;
        const auto back = o->relationships.find(me->id);
        if (back == o->relationships.end() || back->second.empty() || back->second[0] < kFriendshipThreshold)
            continue;
        sum += o->person_data[kFameLevel];
    }
    return sum;
}

std::array<int, 6> fame_skills_needed(Content &content, const std::vector<int16_t> &pd)
{
    std::array<int, 6> need{};
    const FameTables &t = fame_tables(content);
    const int level = pd.size() > 81 ? pd[81] : 0;
    if (level < 0 || size_t(level) >= t.skills.size())
        return need;
    for (int k = 0; k < 6; k++) {
        const int points = t.skills[size_t(level)][size_t(k)];
        need[size_t(k)] = points * 100 <= pd[size_t(kFameSkills[k])] ? 0 : points;
    }
    return need;
}

// The level written to the person and their neighbour record (the exe's is the record itself).
static void set_fame_level(Vm &vm, Entity &person, int level, bool best)
{
    person.person_data[81] = int16_t(level);
    if (best && person.person_data[82] < level)
        person.person_data[82] = int16_t(level);
    if (Neighbor *n = vm.hood ? vm.hood->by_id(person.person_data[31]) : nullptr; n && n->person_data.size() > 82) {
        n->person_data[81] = person.person_data[81];
        n->person_data[82] = person.person_data[82];
    }
}

bool fame_promote(Vm &vm, Entity &person)
{
    const FameTables &t = fame_tables(vm.content);
    const auto &pd = person.person_data;
    const int level = pd[81];
    if (!person.person || level < 0 || level >= 10 || size_t(level) >= t.points.size() || size_t(level) >= t.star_power.size())
        return false;
    for (int need : fame_skills_needed(vm.content, pd))
        if (need > 0)
            return false;
    if (float(t.star_power[size_t(level)]) > float(friends_star_power(vm, pd[31])) * 0.5f || pd[80] < t.points[size_t(level)])
        return false;
    set_fame_level(vm, person, level + 1, true);
    return true;
}

bool fame_demote(Vm &vm, Entity &person)
{
    const FameTables &t = fame_tables(vm.content);
    const int level = person.person_data[81];
    if (!person.person || level <= 0 || size_t(level) > t.points.size() || person.person_data[80] >= t.points[size_t(level - 1)])
        return false;
    set_fame_level(vm, person, level - 1, false);
    return true;
}

// A person's variables for their sounds' HIT programs (FreeSO VMAvatar.SubmitHITVars):
// gender (2 for a child), skills, age and personality, and their voice.
void person_sound_vars(const Entity &source, Vm::SoundEvent &e)
{
    const auto &pd = source.person_data;
    e.voice = pd[65] == 0 ? 'm' : 'f';   // Gender: 0 male
    const int age = pd[58];
    e.vars[0] = (age < 18 && age != 0) ? 2 : pd[65];
    e.vars[2] = std::min(100, pd[10] / 10);    // cooking
    e.vars[5] = std::min(100, pd[15] / 10);    // creativity
    e.vars[7] = std::min(100, pd[12] / 10);    // mechanical
    e.vars[11] = std::min(100, pd[17] / 10);   // body
    e.vars[14] = age;
    for (int i = 0; i < 6; i++)                // nice, active, generous, playful, outgoing, neat
        e.vars[size_t(15 + i)] = pd[size_t(2 + i)] / 10;
}

namespace {

Exit play_sound(Frame &f, const uint8_t *op)
{
    Vm &vm = f.vm();
    const uint16_t id = u16(op, 0);
    const int flags = op[4], volume = op[5];
    std::string name;
    for (const LoadedIff *file : { f.owner.own, f.owner.semi, vm.global_code.own })
        if (file)
            if (const IffChunk *c = file->iff.find("FWAV", id)) {
                const uint8_t *b = file->body(*c);
                for (uint32_t i = 0; i < c->size && b[i]; i++)
                    name += char(b[i]);
                break;
            }
    if (name.empty())
        return Exit::GotoTrue;
    Entity *source = (flags & 2) ? f.stack_object() : f.caller;
    Vm::SoundEvent e;
    e.name = name;
    e.source = source ? source->id : f.caller->id;
    e.loop = flags & 1;
    e.no_pan = (flags & 8) != 0;   // (the skill-point ding, a visitor's knock: 49 uses, all one-shots)
    if (const Entity *at = source ? source : f.caller) {
        while (at->container && !at->in_world())   // (held: where the holder is)
            at = at->container;
        if (at->in_world()) {
            e.x16 = at->x16;
            e.y16 = at->y16;
            e.level = at->level;
        }
    }
    e.volume = volume ? volume : 100;
    if (source && source->person)
        person_sound_vars(*source, e);
    static const bool log = TS_ENV("TS_SOUND_EVENTS") != nullptr;   // debug: each event as it's played
    if (log) {   // (with the real time: the game's ticks against the sounds' own clock)
        static const auto t0 = std::chrono::steady_clock::now();
        std::printf("tick %d (%.0f ms) sound %s from #%d (%s)\n", int(vm.ticks),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), e.name.c_str(),
                    int(e.source), f.routine ? f.routine->name.c_str() : "?");
    }
    if (vm.sounds.size() < 256)
        vm.sounds.push_back(std::move(e));
    return Exit::GotoTrue;
}

// Stop All Sounds: the caller's, or (flags 1) the stack object's (FreeSO VMStopAllSounds).
// Change Action String (FreeSO VMChangeActionString): in a pie menu's check tree, a menu
// entry of its own, about the stack object; otherwise the running interaction's new name.
Exit change_action_string(Frame &f, const uint8_t *op)
{
    Vm &vm = f.vm();
    const uint16_t table = u16(op, 0), scope = u16(op, 2);
    const int id = op[4];
    const LoadedIff *file = scope == 0 ? f.owner.own : scope == 1 ? (f.callee ? f.callee->code.semi : nullptr)
                                                                 : vm.global_code.own;
    const std::vector<std::string> strings = vm.strings(file, table);
    if (id < 1 || size_t(id) > strings.size())
        return Exit::GotoTrue;
    const std::string name = dialog_text(f, strings[size_t(id - 1)]);
    if (f.thread->is_check) {   // building a menu: an entry; any other check: nothing to rename
        if (f.thread->action_strings)
            f.thread->action_strings->emplace_back(name, f.stack_id);
    } else if (!f.thread->queue.empty()) {
        f.thread->queue[0].name = name;
    }
    return Exit::GotoTrue;
}

Exit stop_all_sounds(Frame &f, const uint8_t *op)
{
    const Entity *owner = op[0] == 1 ? f.stack_object() : f.caller;
    if (!owner)
        return Exit::GotoTrue;
    Vm::SoundEvent e;
    e.source = owner->id;
    e.stop = true;
    if (f.vm().sounds.size() < 256)
        f.vm().sounds.push_back(e);
    return Exit::GotoTrue;
}

Exit get_terrain_info(Frame &f, const uint8_t *)
{
    f.thread->temps[0] = 0;
    return Exit::GotoTrue;
}

// Find Best Action (FreeSims VMFindBestAction): the Sim choosing something to
// do by itself. Each interaction advertises what it does for each motive;
// the lower a motive, the more that counts, less with distance; the best
// one whose check tree passes is queued.
// Autonomy (FreeSO VMFindBestAction, the Sims 1 way): every interaction with
// advertisements whose check tree passes is scored by how much happier it
// would make the Sim, read off the game's curves for each need it serves
// (as the check tree adjusted its ads), less for distance.
std::vector<AdScore> score_all(Vm &vm, Entity &sim, bool keep_failed, float min_score, int top)
{
    // How much distance counts, by the interaction's code (the original's FUN_00661ac0): 1 none,
    // 2-4 low, moderate, high, over 4 0.02, 0 the interaction's own. The values are the game's
    // tuning (Global.iff FCNS 2: 0.1 / 0.3 / 0.6, visitors 0.01 / 0.02 / 0.03), not the program's
    // defaults (0.002 / 0.02 / 0.1, which it never uses).
    const bool visitor = sim.person_data[32] != 0;   // as the original picks the visitor table
    const char *const prefix = visitor ? "visitor " : "";
    const float attenuation[5] = { 0, 0, vm.global_tuning(2, std::string(prefix) + "low attenuation", 0.002f),
                                   vm.global_tuning(2, std::string(prefix) + "moderate attenuation", 0.02f),
                                   vm.global_tuning(2, std::string(prefix) + "high attenuation", 0.1f) };
    // The needs the curves are for, in their order.
    static const int kWeightMotives[9] = { 5, 6, 7, 8, 9, 3, 13, 14, 15 };
    // Personality or skill (person data) an ad can vary by (the original's table at 0x997698):
    // odd personalities as is, even ones inverted; then skills and the rest, as is.
    static const int kVaryBy[23] = { -1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 9, 10, 11, 12, 55, 55, 15, 55, 17, 18 };
    constexpr int kRoomMotive = 13;
    const uint16_t sim_room = vm.room_at(sim.level, sim.tile_x(), sim.tile_y());
    const bool child = sim.person_data[58] > 0 && sim.person_data[58] < 18;
    const std::vector<Vm::Curve> &curves = vm.interaction_curves(child);
    const float weight = 1.0f / 9;
    float happy[9];
    float base = 0;
    for (int i = 0; i < 9; i++) {
        happy[i] = curves[size_t(i)].at(sim.motives[size_t(kWeightMotives[i])]) * weight;
        base += happy[i];
    }
    std::vector<AdScore> out;
    std::unordered_set<const Entity *> seen;
    std::map<int, int16_t> changes;
    static const bool owners_old = TS_ENV("TS_MENU_OWNER_SCAN") != nullptr;   // debug: each part scans the lot, as before
    const Vm::MenuOwners owners = owners_old ? Vm::MenuOwners() : vm.menu_owners();
    // (Timed only when profiling: the clock read four times an interaction, at some forty
    // interactions a choice, was part of a choice's 20-50 ms on the Vita.)
    using PClock = ts::FastClock;
    const bool timing = vm.profile.on;
    auto now = [timing] { return timing ? PClock::now() : PClock::time_point(); };
    auto since = [](PClock::time_point t) { return std::chrono::duration<double, std::milli>(PClock::now() - t).count(); };
    // An interaction's score from its ads (the check tree's changes to them in `changed_ads`, none for
    // the raw score) by the Sim's needs, less for distance; `ads`, when asked, gets the debugging
    // listing of what each ad came to.
    auto score_of = [&](const TtabInteraction &ia, const std::map<int, int16_t> &changed_ads, float distance, bool other_room,
                        std::string *ads) {
        float score = base;
        for (size_t m = 0; m < ia.motives.size() && m < sim.motives.size(); m++) {
            const TtabMotive &ad = ia.motives[m];
            if (ad.min == 0 && ad.max == 0)
                continue;
            auto changed = [&](int kind, int16_t v) {
                const auto it = changed_ads.find(kind << 16 | int(m));
                return it == changed_ads.end() ? v : it->second;
            };
            const int min = changed(0, ad.min), personality = changed(2, int16_t(ad.personality));
            int delta = changed(1, ad.max);
            // The room's ad counts only in the object's own room (the original sets its delta to
            // minus its minimum for an object elsewhere).
            if (int(m) == kRoomMotive && other_room)
                delta = -min;
            const int mine = sim.motives[m];
            int wi = -1;
            for (int i = 0; i < 9; i++)
                if (kWeightMotives[i] == int(m))
                    wi = i;
            if (wi < 0)
                continue;
            score -= happy[wi];
            // What it offers (the original's FUN_006837e0): the minimum plus the delta, or with a
            // personality or skill, the minimum plus the delta scaled by it (out of 1000). Every
            // Sim sees the minimum; a neat one sees more of a cleaning ad's delta, a sloppy one
            // less. (Ours scaled the minimum too, offered nothing at all unless the need was
            // below the minimum, and doubled skills: most cleaning, fun and exercise ads were off.)
            float by = 1;
            float offer = float(min + delta);
            if (personality > 0 && personality < int(std::size(kVaryBy)) &&
                size_t(kVaryBy[personality]) < sim.person_data.size()) {
                float trait = sim.person_data[size_t(kVaryBy[personality])];
                if (personality < 13 && (personality & 1) == 0)
                    trait = 1000 - trait;
                by = trait / 1000.0f;
                offer = float(min) + float(delta) * by;
            }
            score += curves[size_t(wi)].at(float(mine) + offer / 1000.0f) * weight;
            if (ads) {   // (the debugging listing's: what each ad came to)
                *ads += " m" + std::to_string(m) + ":" + std::to_string(int(offer));
                if (by != 1) {
                    char s[32];
                    std::snprintf(s, sizeof s, "(p%d x%.2f)", personality, by);
                    *ads += s;
                }
            }
        }
        score -= base;
        const float atten = ia.attenuation_code == 0 ? ia.attenuation_value
                            : ia.attenuation_code >= 5 ? 0.02f
                                                       : attenuation[ia.attenuation_code];
        return score / (1 + atten * distance);
    };
    // Every advertised interaction the Sim may take, with its raw score (the ads as the table has
    // them): the check trees come after, and for the choice only as many as its pick needs.
    struct Candidate {
        Entity *obj;
        const TtabInteraction *ia;
        float distance;
        bool other_room;
        float raw;
    };
    static const std::map<int, int16_t> no_changes;
    std::vector<Candidate> candidates;
    for (size_t k = 0; k < vm.entities.size(); k++) {
        Entity *obj = vm.menu_owner(vm.entities[k].get(), owners_old ? nullptr : &owners);
        if (!obj || obj == &sim || obj->dead || !obj->tree_table || !vm.on_lot(*obj) || !seen.insert(obj).second)
            continue;
        if (obj->value(VarLockoutCount) > 0)
            continue;   // locked out for a while (the fire after a failed panic), as the original skips it
        if (vm.autonomy_subset == 2 && (obj->id & 1))
            continue;
        if (vm.autonomy_subset == 3 && !(obj->id & 1))
            continue;
        if (vm.autonomy_subset == 1) {
            const float sx = float(obj->x16 - sim.x16) / 16, sy = float(obj->y16 - sim.y16) / 16,
                        sl = float(obj->level - sim.level) * 20;
            if (sx * sx + sy * sy + sl * sl > 49.0f)
                continue;
        }
        const bool other_room = vm.room_at(obj->level, obj->tile_x(), obj->tile_y()) != sim_room;
        const float dx = float(obj->x16 - sim.x16), dy = float(obj->y16 - sim.y16),
                    dl = float(obj->level - sim.level) * 320;
        const float distance = std::sqrt(dx * dx + dy * dy + dl * dl) / 16.0f;
        for (const TtabInteraction &ia : obj->tree_table->interactions) {
            bool has_ads = false;
            for (const TtabMotive &m : ia.motives)
                has_ads |= m.min != 0 || m.max != 0;
            auto t0 = now();
            if (!has_ads || !vm.allowed(sim, ia, true, obj))
                continue;
            if (timing)
                vm.profile.score_allowed_ms += since(t0);
            candidates.push_back({ obj, &ia, distance, other_room, score_of(ia, no_changes, distance, other_room, nullptr) });
        }
    }
    // The check trees. All of them for the debugging listing (keep_failed, TS_AUTONOMY_FULL=1); for
    // the choice, in the order of the raw scores, until the best `top` passing ones can't be
    // bettered by what's left and nothing left reaches the minimum score. A check tree changes
    // the ads it's scored by in 1% of its runs and passes in 17% (House28, 15,000 checks), so the
    // choice is the full run's but for that 1%, at a few dozen trees instead of a thousand (1,083
    // trees, 87 ms, for one choice on the Vita at downtown, 2026-11-05).
    static const bool full = TS_ENV("TS_AUTONOMY_FULL") != nullptr;
    const bool lazy = !keep_failed && !full && top > 0;
    if (lazy)
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &x, const Candidate &y) { return x.raw > y.raw; });
    // (One kept across the loop, emptied each time, so its action's name and arguments keep
    // their room: see Entity::action.)
    AdScore result;
    std::vector<float> passing;   // the passing ones' scores, highest first (the stopping rule)
    for (const Candidate &c : candidates) {
        if (lazy) {
            if (c.raw < min_score)
                break;
            // (Or equal: among equal scores the full run's top `count` are the first in object order,
            // and that is the order they come in here too; checking a whole tie group, a downtown
            // shop's hundreds of identical racks, ran 400-666 trees a choice for nothing.)
            if (passing.size() >= size_t(top) && c.raw <= passing[size_t(top) - 1])
                break;
        }
        result.score = 0;
        result.passed = false;
        result.ads.clear();
        result.obj = c.obj;
        auto t0 = now();
        if (!c.obj->action(vm, int(c.ia->tta_index), int16_t(c.obj->id), result.action) || result.action.name.empty() ||
            result.action.name[0] == '*')
            continue;
        if (timing)
            vm.profile.score_action_ms += since(t0);
        // The check tree is asked "on its own?" with param 0 = 1: on the action itself, put
        // back after (the original's FUN_005f6cd0 passes 1 to the check of each candidate).
        if (result.action.args.size() < 4)
            result.action.args.resize(4, 0);
        const int16_t arg0 = result.action.args[0];
        result.action.args[0] = 1;
        t0 = now();
        const uint64_t i0 = vm.profile.instructions;
        result.passed = sim.thread->check(result.action, &changes, c.ia);
        result.action.args[0] = arg0;
        vm.tick_checks++;
        {   // Debug (TS_AD_STATS=1): how often a check tree changes the ads it's scored by, and passes.
            static const bool ad_stats = TS_ENV("TS_AD_STATS") != nullptr;
            static uint64_t s_checks = 0, s_changed = 0, s_passed = 0;
            if (ad_stats) {
                s_checks++;
                s_changed += changes.empty() ? 0 : 1;
                s_passed += result.passed ? 1 : 0;
                if (s_checks % 5000 == 0)
                    std::printf("ad stats: %llu checks, %llu changed the ads (%.1f%%), %llu passed (%.1f%%)\n",
                                (unsigned long long)s_checks, (unsigned long long)s_changed, 100.0 * double(s_changed) / double(s_checks),
                                (unsigned long long)s_passed, 100.0 * double(s_passed) / double(s_checks));
            }
        }
        if (timing) {
            vm.profile.score_check_ms += since(t0);
            vm.profile.score_checks++;
            vm.profile.check_instructions += vm.profile.instructions - i0;
        }
        if (!result.passed && !keep_failed)
            continue;
        result.score = changes.empty() && !keep_failed ? c.raw
                                                       : score_of(*c.ia, changes, c.distance, c.other_room, keep_failed ? &result.ads : nullptr);
        if (lazy && result.passed)
            passing.insert(std::upper_bound(passing.begin(), passing.end(), result.score, std::greater<float>()), result.score);
        out.push_back(std::move(result));
    }
    std::stable_sort(out.begin(), out.end(), [](const AdScore &a, const AdScore &b) { return a.score > b.score; });
    return out;
}

// Then one of the best few at random, as the original's (FUN_005f6cd0): the tuning's "random
// selection count" (4) less the Sim's logic skill / 2000, at least one, each as likely.
Exit find_best_action(Frame &f, const uint8_t *)
{
    Vm &vm = f.vm();
    Entity &sim = *f.caller;
    // Free will off (the options' Free Will): the first thing FUN_005f6cd0 does. The household
    // (person data 32 0) chooses nothing, its pets aside; a visitor goes on unless it's of the lot's
    // own family (person data 61) and not a pet. Ours never read the option on a home lot: only
    // global 30, which the game's code reads off residential lots alone (2026-11-04 audit).
    if (!vm.free_will && sim.person_data.size() > 65) {
        const bool pet = (sim.person_data[65] & 6) != 0;
        bool goes = pet;
        if (sim.person_data[32] != 0) {
            const Family *fam = vm.hood ? vm.hood->family_in_house(vm.house) : nullptr;
            goes = sim.person_data[65] > 1 || !fam || sim.person_data[61] != fam->id;
        }
        if (!goes)
            return Exit::GotoFalse;
    }
    // Something better than what it's doing is already asked for: that.
    for (const QueuedAction &q : sim.thread->queue)
        if (q.priority > sim.person_data[33]) {
            f.stack_id = q.callee;
            return Exit::GotoTrue;
        }
    // At most two choices a tick (the Vita: five Sims choosing at once on a downtown lot took one
    // tick 220 ms, a visible hitch); the rest choose on the next tick, as this instruction runs
    // again. A count, not a time, so every platform behaves alike. TS_CHOICES_UNCAPPED=1: no limit.
    // (Off unless TS_CHOICES_CAP=n: on the Vita it didn't lessen the hitches once a lot had settled,
    // worst ticks ~120 ms either way; the arrival's big ones are file reads and markers starting.)
    static const bool uncapped = TS_ENV("TS_CHOICES_CAP") == nullptr;
    static const int kChoicesPerTick = TS_ENV("TS_CHOICES_CAP") ? std::max(1, std::atoi(TS_ENV("TS_CHOICES_CAP"))) : 3;
    if (!uncapped) {
        if (vm.choices_tick != vm.ticks) {
            vm.choices_tick = vm.ticks;
            vm.choices_made = 0;
        }
        if (vm.choices_made >= kChoicesPerTick)
            return Exit::ContinueNextTick;
        vm.choices_made++;
    }
    const bool visitor = sim.person_data[32] != 0;
    const float min_score = visitor ? vm.global_tuning(2, "min autonomy score for visitors", 0.05f)
                                    : vm.global_tuning(2, "min autonomy score for family", 0.2f);
    // Which quarter of the objects this choice looks at: (the Sim's id + the lot's step count) & 3,
    // as FUN_005f6cd0 (its lot+0x98, the tick); ours counted choices made. TS_SUBSET_CHOICES=1: as before.
    vm.autonomy_subset = TS_ENV("TS_SUBSET_CHOICES") ? int((sim.id + vm.autonomy_choices++) & 3) : int((sim.id + vm.ticks) & 3);
    const int count = std::max(1, int(vm.global_tuning(2, "random selection count", 10)) - sim.person_data[18] / 2000);
    const auto t_choice = ts::FastClock::now();
    const uint32_t random_before = vm.random_state;   // (the verify below runs the full way from the same state)
    std::vector<AdScore> valid = score_all(vm, sim, false, min_score, count);
    vm.tick_choice_ms += std::chrono::duration<double, std::milli>(ts::FastClock::now() - t_choice).count();
    {   // Debug (TS_AUTONOMY_VERIFY=1): the lazy run's pick set against the full run's, choice by choice.
        static const bool verify = TS_ENV("TS_AUTONOMY_VERIFY") != nullptr;
        if (verify) {
            const uint32_t random_after = vm.random_state;
            vm.random_state = random_before;   // (the check trees draw random numbers: the same stream for both)
            std::vector<AdScore> full = score_all(vm, sim, false, -1e9f, 0);
            vm.random_state = random_after;
            auto pick_set = [&](std::vector<AdScore> &v) {
                std::set<std::pair<int, int>> set;
                while (!v.empty() && v.back().score < min_score)
                    v.pop_back();
                for (size_t k = 0; k < v.size() && int(k) < count; k++)
                    set.emplace(int(v[k].obj ? v[k].obj->id : 0), int(v[k].action.interaction));
                return set;
            };
            std::vector<AdScore> lazy_copy = valid;
            const auto a = pick_set(lazy_copy), b = pick_set(full);
            static int choices = 0, mismatches = 0;
            choices++;
            if (a != b)
                mismatches++;
            if (choices % 50 == 0 || (a != b && mismatches <= 5))
                std::printf("autonomy verify: %d choices, %d pick sets differ (%zu lazy, %zu full)\n", choices, mismatches, a.size(), b.size());
        }
    }
    vm.tick_choices++;
    vm.autonomy_subset = -1;
    if (const char *who = TS_ENV("TS_CHOICE_LOG"); who && std::atoi(who) == sim.id) {   // debug: a Sim's best ten
        std::printf("choice: tick %u #%d (subset %d, energy %d):", vm.ticks, int(sim.id), int((sim.id + vm.ticks) & 3),
                    int(sim.motives[5]));
        for (size_t k = 0; k < valid.size(); k++)
            if (k < 6 || (valid[k].obj && valid[k].obj->info && valid[k].obj->info->label.find("Bed") != std::string::npos))
            std::printf(" %zu:[%s on %s %.4f]", k, valid[k].action.name.c_str(),
                        valid[k].obj && valid[k].obj->info ? valid[k].obj->info->label.c_str() : "?", valid[k].score);
        std::printf("\n");
    }
    while (!valid.empty() && valid.back().score < min_score)
        valid.pop_back();
    if (valid.empty())
        return Exit::GotoFalse;
    if (valid.size() > size_t(count))
        valid.resize(size_t(count));
    AdScore *chosen = nullptr;
    // On a residential lot a visitor takes the best of them, not one at random (FUN_005f6cd0: the
    // random pick only for a non-visitor whose best entry lacks flag bit 0, or off residential lots;
    // the zoning there is FUN_0054bf70's 1, residential, ours 0).
    // The flag bit is the best interaction's TTAB flag 0x100 (FreeSO's AutoFirstSelect), copied into
    // the action by FUN_005edcf0: a family Sim takes such a best choice outright too.
    if (visitor || (valid[0].action.flags & 0x100u)) {
        const int h = vm.house;
        int zone = ((h >= 81 && h <= 89) || (h >= 21 && h <= 30) || (h >= 40 && h <= 49)) ? 2 : 0;
        if (vm.hood)
            if (auto it = vm.hood->zoning.find(h); it != vm.hood->zoning.end())
                zone = it->second;
        if (zone == 0)
            chosen = &valid[0];
        if (TS_ENV("TS_AUTOFIRST") && chosen && !visitor)   // debug: family Sims taking their best outright
            std::printf("autofirst: %s takes %s on %s\n", sim.info ? sim.info->label.c_str() : "?",
                        chosen->action.name.c_str(), chosen->obj->info ? chosen->obj->info->label.c_str() : "?");
    }
    if (!chosen) {   // (a random number drawn only for the random pick, as the exe's)
        vm.random_state = vm.random_state * 1103515245u + 12345u;
        chosen = &valid[(vm.random_state >> 8) % valid.size()];
    }
    // Sitting, the choice has to be worth getting up for (the original fails the whole call).
    if (sim.person_data[0] != 0 && chosen->score < vm.global_tuning(2, "min autonomy score for sitting", 0.2f))
        return Exit::GotoFalse;
    chosen->action.priority = PriorityAutonomous;
    chosen->action.found = true;
    f.stack_id = int16_t(chosen->obj->id);
    sim.thread->enqueue(std::move(chosen->action));
    return Exit::GotoTrue;
}

// Token Operations (Sims 1 opcode 51): a Sim's (or, with bit 13 of the flags, the object in
// temp 4's) inventory of tokens, each a kind (operand 1), an object's GUID (operand 4-7; 0: the
// stack object's) and a count, as the original keeps them (its handler FUN_00619d30 and the
// inventory's routines: add FUN_00424d20, remove FUN_004252d0, find FUN_00424fb0, remove at an
// index FUN_00425360, next FUN_00425540). Modes: 0 add a count (bit 1: from the temp in bits
// 2-3, else 1) to the token of that kind and object, making it when there is none; 1 remove:
// one off that token's count (gone at 0; a count of -1 takes the whole token); 2 remove the
// count from the token at the index in the temp in bits 8-9 (the token goes at 0; a negative
// count takes it whole); 3 find the token of that kind and object: its count to the temp in
// bits 5-6, its index to the temp in bits 11-12, false when there is none; 4 the next token of
// the kind (and object, only when bit 0 says so) after the index in the temp in bits 8-9: its
// count and index to those temps, false (index -1) at the end; 5/6 a Hot Date token
// (downtown, back home) for the neighbour in temp 0. Makin' Magic's starter kit adds its
// 35 MagiCoins as one kind-7 token with that count and each ingredient as a kind-8 token;
// a pet bought at a pen: its "CT - Buy" (run in the register's sale) adds a kind-6 token with
// the pen's GUID to the buyer, and "Buy a Pet" finds and removes it before making the pet.
Exit token_operations(Frame &f, const uint8_t *op)
{
    Vm &vm = f.vm();
    Thread &t = *f.thread;
    const int mode = op[0];
    const uint8_t kind = op[1];
    const uint16_t flags = uint16_t(op[2] | op[3] << 8);
    // Every kind kept alike, as the exe's (FUN_00619d30: no case for any kind). Ours answered
    // false for the date and sim-data kinds (1-3) and modes 5/6, so the trip scripts took their
    // false branches and a date never came along. TS_TRIP_TOKENS_OLD=1: as before.
    static const bool trip_old = TS_ENV("TS_TRIP_TOKENS_OLD") != nullptr;
    if (trip_old && (kind == 1 || kind == 2 || kind == 3 || mode == 5 || mode == 6))
        return Exit::GotoFalse;
    Entity *owner = (flags & 0x2000) ? vm.entity(t.temps[4]) : f.caller;
    if (!owner || !owner->person)
        return Exit::Error;   // (the exe's 0x1c: the owner must be a person)
    if (mode == 5 || mode == 6) {
        // The Sim's date (5: kind 1, going out) or who they take home (6: kind 3): the neighbour
        // in temp 0, the only one of that kind (FUN_00425100 clears the kind, FUN_00424d20 adds).
        const uint8_t k = mode == 5 ? 1 : 3;
        const Neighbor *n = vm.hood ? vm.hood->by_id(t.temps[0]) : nullptr;
        if (!n)
            return Exit::Error;
        auto &held = owner->tokens;
        held.erase(std::remove_if(held.begin(), held.end(), [&](const Entity::Token &x) { return x.kind == k; }), held.end());
        held.push_back({ k, n->guid, 1 });
        if (TS_ENV("TS_TOKENS"))   // debug
            std::printf("tokens: #%d's %s is neighbour %d (%08x)\n", int(owner->id), mode == 5 ? "date" : "guest home",
                        int(t.temps[0]), unsigned(n->guid));
        return Exit::GotoTrue;
    }
    uint32_t guid = uint32_t(op[4] | op[5] << 8 | op[6] << 16 | uint32_t(op[7]) << 24);
    if (!guid && f.stack_object() && f.stack_object()->info)
        guid = f.stack_object()->info->guid;
    auto &tokens = owner->tokens;
    auto same = [&](const Entity::Token &k) { return k.kind == kind && k.guid == guid; };
    const int count = (flags & 2) ? t.temps[(flags >> 2) & 3] : 1;   // add, remove: how many
    const int count_temp = (flags >> 5) & 3, start_temp = (flags >> 8) & 3, index_temp = (flags >> 11) & 3;
    if (TS_ENV("TS_TOKENS"))   // debug
        std::printf("tokens: #%d mode %d kind %d guid %08x count %d (%lu held)\n", int(owner->id), mode, int(kind),
                    unsigned(guid), count, (unsigned long)tokens.size());
    switch (mode) {
    case 0: {   // add
        if (count == 0)
            return Exit::GotoTrue;
        auto it = std::find_if(tokens.begin(), tokens.end(), same);
        if (it != tokens.end()) {
            it->count = int16_t(std::min(int(it->count) + count, 0x7fff));
            return Exit::GotoTrue;
        }
        if (tokens.size() >= 0x7ffe)
            return Exit::GotoFalse;
        tokens.push_back({ kind, guid, int16_t(count) });
        return Exit::GotoTrue;
    }
    case 1: {   // remove one (a count of -1: the token whole)
        if (count == 0)
            return Exit::GotoTrue;
        auto it = std::find_if(tokens.begin(), tokens.end(), same);
        if (it == tokens.end())
            return Exit::GotoFalse;
        if (it->count >= 2 && count != -1) {
            it->count--;
            return Exit::GotoTrue;
        }
        tokens.erase(it);
        return Exit::GotoTrue;
    }
    case 2: {   // remove the count from the token at an index (negative: the token whole)
        if (count == 0)
            return Exit::GotoTrue;
        const int index = t.temps[start_temp];
        if (index < 0 || size_t(index) >= tokens.size())
            return Exit::GotoFalse;
        Entity::Token &k = tokens[size_t(index)];
        if (count >= 0 && k.count > count) {
            k.count = int16_t(k.count - count);
            return Exit::GotoTrue;
        }
        tokens.erase(tokens.begin() + index);
        return Exit::GotoTrue;
    }
    case 3: {   // find
        for (size_t i = 0; i < tokens.size(); i++)
            if (same(tokens[i])) {
                t.temps[count_temp] = tokens[i].count;
                t.temps[index_temp] = int16_t(i);
                return Exit::GotoTrue;
            }
        return Exit::GotoFalse;
    }
    case 4: {   // the next of the kind after the index in the start temp
        const bool with_object = flags & 1;
        for (size_t i = size_t(std::max(0, int(t.temps[start_temp]) + 1)); i < tokens.size(); i++)
            if (tokens[i].kind == kind && (!with_object || tokens[i].guid == guid)) {
                t.temps[count_temp] = tokens[i].count;
                t.temps[index_temp] = int16_t(i);
                return Exit::GotoTrue;
            }
        t.temps[count_temp] = 0;
        t.temps[index_temp] = -1;
        return Exit::GotoFalse;
    }
    default:
        return Exit::GotoFalse;
    }
}

Exit fail(Frame &, const uint8_t *)
{
    return Exit::GotoFalse;
}

} // namespace

std::vector<AdScore> score_interactions(Vm &vm, Entity &sim, bool keep_failed)
{
    return score_all(vm, sim, keep_failed, -1e9f, 0);
}


// The route result (person data 62) as the exe's routing frame writes it where every walk ends
// (FUN_005f7950's end block, byte 0x60c = person data 62): 0 when it got there, the failure code
// otherwise (route_failed). Ours wrote only the failures, so one interrupted walk (4) stayed for
// good, and the game's code that reads it ("prepare food", "do read", "cook", "serve meal", 57
// lines in all: `my person 62 == 4` gives up) gave up where the original tries the next counter or
// seat: John refusing the Bobs' free counter, Linda's study ending at once (2026-11-04 audit).
static Exit route_result(Frame &f, Exit e)
{
    if (e == Exit::GotoTrue && f.caller && f.caller->person && f.caller->person_data.size() > 62)
        f.caller->person_data[62] = 0;
    // The walk that shut them is over: its shut doors open again (only that one's: a door's own
    // walks inside its portal code end too, and cleared them under it).
    if ((e == Exit::GotoTrue || e == Exit::GotoFalse) && f.caller) {
        auto &route = f.caller->route;
        if (route.shut_routine == f.routine && route.shut_ip == f.ip && route.shut_depth == f.thread->stack.size())
            route.shut_doors.clear();
        if (f.caller->person && f.caller->person_data.size() > 73)
            f.caller->person_data[73] = 0;   // the walk's footprint shrink, over with it (the exe: 0x622 cleared)
    }
    return e;
}
// The routing footprint the game's code asked for (person data 72, generic call 15) is the one in
// force (73) for the walk and its spot search (the exe's walk state machine copies 0x620 to 0x622;
// FUN_0060bd40 for the scoring): the Sim's box shrinks by its nibbles (person_box).
static void walk_footprint(Frame &f)
{
    if (f.caller && f.caller->person && f.caller->person_data.size() > 73)
        f.caller->person_data[73] = f.caller->person_data[72];
}
Exit goto_routing_slot(Frame &f, const uint8_t *op) { walk_footprint(f); return route_result(f, goto_routing_slot_walk(f, op)); }
Exit goto_relative(Frame &f, const uint8_t *op) { walk_footprint(f); return route_result(f, goto_relative_walk(f, op)); }

void register_more_primitives(Vm &vm)
{
    auto &p = vm.primitives;
    p[1] = generic_call;
    p[33] = find_five_worst_motives;
    p[35] = special_effect;
    p[3] = find_best_action;   // "find best interaction" in the Sims 1 numbering
    p[23] = play_sound;
    p[48] = stop_all_sounds;
    p[50] = change_action_string;
    p[4] = grab;
    p[5] = drop;
    p[6] = change_suit;
    p[13] = push_interaction;
    p[14] = find_best_object_for_function;
    p[16] = find_location_for;
    p[17] = idle_for_input;
    p[18] = remove_object;
    p[20] = run_functional_tree;
    p[22] = look_towards;
    p[24] = relationship_old;
    p[25] = transfer_funds;
    p[26] = relationship_new;
    p[27] = goto_relative;
    p[28] = run_tree_by_name;
    p[29] = set_motive_change;
    p[36] = dialog_private;
    p[37] = test_sim_interacting_with;
    p[38] = dialog_global;
    p[39] = dialog_semiglobal;
    p[9] = burn;
    p[19] = make_new_character;
    p[30] = gosub_found_action;
    p[42] = create_object;
    p[43] = drop_onto;
    p[44] = animate;
    p[45] = goto_routing_slot;
    p[46] = snap;
    p[47] = reach;
    p[49] = notify_out_of_idle;
    p[10] = fail;        // the tutorial / help system: not running
    p[51] = token_operations;
    p[62] = fail;        // plugins (online)
    p[63] = get_terrain_info;
    p[65] = find_best_action;
    p[67] = fail;        // inventory (online)
}

std::vector<std::pair<int16_t, std::string>> phone_book(Vm &vm, const Entity &sim)
{
    return known_sims(vm, sim);
}

// Removes an object from outside its code (debug tools): never one whose primitive is running.
void Vm::remove_now(Entity &e)
{
    Frame none;
    remove_entity(*this, &e, none);
}

void Vm::finish_removals()
{
    // (Removals can queue more: a removed object's parts, or code run as it goes.)
    for (size_t i = 0; i < pending_removals.size(); i++) {
        Entity *e = pending_removals[i];
        Frame none;
        if (e && !e->dead)
            remove_entity(*this, e, none);
    }
    pending_removals.clear();
}

bool person_box_hits_wall(const Vm &vm, const Entity &sim, int px, int py, int level)
{
    return box_hits_wall(vm, person_box(sim, px, py), level);
}

} // namespace ts::vm
