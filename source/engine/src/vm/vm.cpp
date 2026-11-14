/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/SimsAntics: VM.cs, VMContext.cs,
 * engine/VMThread.cs, entities/VMEntity.cs, utils/VMWorldActivator.cs).
 */
#include "core/env.h"
#include "core/memory_kind.h"
#include "core/fast_clock.h"
#include "vm/vm.h"
#include "files/install.h"
#include "core/strings.h"

#include <chrono>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <unordered_set>
#include <cstdio>
#include <tuple>

namespace ts::vm {

Vm &Frame::vm() const
{
    return thread->vm_ref();
}

Entity *Frame::stack_object() const
{
    return stack_id > 0 ? thread->vm_ref().entity(stack_id) : nullptr;
}

// ---------------------------------------------------------------- Entity

int16_t Entity::value(int var) const
{
    switch (var) {   // fields the VM keeps elsewhere
    case VarObjectId: return int16_t(id);
    case VarDirection: return int16_t(direction & 7);
    case VarContainerId:
    case VarParentId: return container ? int16_t(container->id) : int16_t(0);
    case VarSlotNumber: return container ? container_slot : int16_t(-1);
    case VarSlotCount: return int16_t(slots.size());
    case VarUseCount: {   // who is running an interaction on this object, as FreeSO's GetUsers: the
        // queue's active part only (the running interaction and what it ran at once), not what
        // waits behind it — a two-Sim interaction expects 0 until both are in it. The cars' shared
        // main waits for it to reach 0 before removing the car (2026-10-31: with nothing counted
        // the vacation wagon removed itself with the Goths still inside).
        if (!thread)
            return 0;
        // Each running interaction on it, not each Sim: the exe adds one for every tree it pushes
        // with this object (FUN_005f2af0, object data 62 at +0xc6) and takes it off when that
        // frame pops (FUN_005f5320), with no limit of one a Sim. TS_USE_COUNT_PER_SIM=1: ours before.
        static const bool per_sim = TS_ENV("TS_USE_COUNT_PER_SIM") != nullptr;
        int users = 0;
        for (const auto &e : thread->vm_ref().entities) {
            if (!e->thread || e.get() == this)
                continue;
            const Thread &t = *e->thread;
            int running = 0;   // (as queue.cpp's active_index)
            for (size_t i = 0; i < t.stack.size(); i++)
                if (t.stack[i].action_tree && (i == 0 || !t.stack[i - 1].action_tree || t.stack[i].discard_result))
                    running++;
            for (int i = 0; i < running && size_t(i) < t.queue.size(); i++)
                if (t.queue[size_t(i)].callee == int16_t(id)) {
                    users++;
                    if (per_sim)
                        break;
                }
        }
        return int16_t(users);
    }
    case VarRoom:   // out of the world (at work, in a cab): -5, as FreeSO ("is the whole family here?" tests >= 0)
        if (!in_world() && !container)
            return -5;
        break;
    default: break;
    }
    return var >= 0 && var < kObjectDataSize ? data[size_t(var)] : 0;
}

void Entity::values(int16_t *out, int count) const
{
    const int n = std::clamp(count, 0, int(kObjectDataSize));
    std::copy(data.begin(), data.begin() + n, out);
    std::fill(out + n, out + std::max(count, n), int16_t(0));
    for (int var : { int(VarObjectId), int(VarDirection), int(VarContainerId), int(VarParentId), int(VarSlotNumber),
                     int(VarSlotCount), int(VarRoom) })
        if (var < count)
            out[var] = value(var);
}

void Entity::set_value(int var, int16_t v)
{
    if (var == VarDirection) {
        direction = ((int(v) % 8) + 8) % 8;
        return;
    }
    if (var >= 0 && var < kObjectDataSize)
        data[size_t(var)] = v;
}

void Entity::set_attribute(int i, int16_t v)
{
    if (i < 0 || i > 4096)
        return;
    if (size_t(i) >= attributes.size())
        attributes.resize(size_t(i) + 1, 0);
    attributes[size_t(i)] = v;
}

void Entity::set_flag(uint16_t f, bool on)
{
    uint16_t v = uint16_t(data[VarFlags]);
    v = on ? uint16_t(v | f) : uint16_t(v & ~f);
    data[VarFlags] = int16_t(v);
}

bool Entity::run_entry(Vm &vm, int entry, bool immediately, int16_t stack_id, const int16_t *args)
{
    if (entry < 0 || size_t(entry) >= entry_points.size() || entry_points[size_t(entry)].action < 256 || !thread)
        return false;
    CodeOwner resolved;
    const Routine *r = vm.routine(code, entry_points[size_t(entry)].action, &resolved);
    if (!r) {
        if (entry != 2)   // many objects name a Load routine they don't have
            vm.warn(nullptr, "missing entry point routine " + std::to_string(entry_points[size_t(entry)].action) +
                             " of " + (info ? info->label : std::string("?")));
        return false;
    }
    Frame f;
    f.routine = r;
    f.caller = f.callee = this;
    f.stack_id = stack_id;
    f.owner = resolved;
    f.args.assign(4, 0);
    if (args)
        std::copy(args, args + 4, f.args.begin());
    if (immediately) {
        const bool ok = thread->run_now(std::move(f)) == Exit::ReturnTrue;
        // After a Sim's init: the Legacy's premade Sims set their own 0-10 interests there ("init
        // traits", the Goths'), which the exe deals on 0-1000 as they come onto the lot.
        if (entry == 0 && person)
            vm.deal_interests(*this);
        // Bartender Waylon (Superstar, NPC_RaveWaylon.iff "init tree" ip 6-7) is made a five-star
        // celebrity (person data 80 := 1000, 81 := 10): fans and stars went after him, off his post into
        // Studio Town's streets, with a star over his head (the community's "Bartender Waylon Fix").
        // No fame. TS_WAYLON_FAME_OLD=1.
        static const bool waylon_old = TS_ENV("TS_WAYLON_FAME_OLD") != nullptr;
        if ((entry == 0 || entry == 2) && person && vm.community_fixes && !waylon_old && info && info->guid == 0x997116B5u &&
            person_data.size() > 81) {
            person_data[80] = 0;
            person_data[81] = 0;
        }
        return ok;
    }
    thread->push(std::move(f));
    return true;
}

// ---------------------------------------------------------------- Thread

void Thread::push(Frame frame)
{
    if (!frame.routine || frame.routine->code.empty())
        return;   // some BHAVs are empty
    if (vm_.count_calls)
        vm_.calls[frame.routine->name]++;
    frame.thread = this;
    frame.ip = 0;
    frame.locals.assign(std::max<size_t>(frame.routine->locals, frame.routine->args), 0);
    stack.push_back(std::move(frame));
}

void Thread::call(Frame &frame, const Routine *bhav, const CodeOwner &owner, const int16_t args[4])
{
    if (!bhav) {
        vm_.warn(&frame, "call to a missing routine " + std::to_string(frame.instruction().opcode));
        pop(Exit::Error);
        return;
    }
    Frame child;
    child.routine = bhav;
    child.caller = frame.caller;
    child.callee = frame.callee;
    child.owner = owner;
    child.stack_id = frame.stack_id;
    child.action_tree = frame.action_tree;
    child.args.assign(std::max<size_t>(bhav->args, 4), 0);
    // "-1" passes the temp register through, unless arguments 1-3 are all 0: then (-1, 0, 0, 0)
    // passes -1 itself (FreeSO's VMSubRoutineOperand.UseTemp0). Ours always took the temp: the
    // chemistry set's Make Potion calls with (-1, 0, 0, 0), -1 meaning "a potion at random", and
    // got whatever temp 0 held: the same potion every time, or a type with no flask to show and no
    // outcome, the user's potion that "did absolutely nothing" (2026-11-07). TS_CALL_ARGS_OLD=1: as before.
    static const bool args_old = TS_ENV("TS_CALL_ARGS_OLD") != nullptr;
    const bool use_temps = args_old || !(args[1] == 0 && args[2] == 0 && args[3] == 0);
    for (size_t i = 0; i < child.args.size(); i++) {
        int16_t a = i < 4 ? args[i] : int16_t(-1);
        if (a == -1 && use_temps)
            a = i < temps.size() ? temps[i] : 0;
        child.args[i] = a;
    }
    push(std::move(child));
}

Exit Thread::run_now(Frame frame, int max_steps, bool keep_temps)
{
    // A separate run on the same temps: check trees and immediate entry points.
    // Its own stack and temps (FreeSims runs check trees in a thread of
    // their own, and saves the temps around RunInMyStack).
    std::vector<Frame> saved;
    if (!spare_stacks.empty()) {
        saved = std::move(spare_stacks.back());
        spare_stacks.pop_back();
    }
    saved.swap(stack);   // (the spare, empty, becomes the stack; the thread's own is kept aside)
    const auto saved_temps = temps;
    const auto saved_xl = temp_xl;
    const Exit saved_exit = last_exit;
    const bool was_check = is_check;
    is_check = true;
    push(std::move(frame));
    int steps = 0;
    while (!stack.empty() && steps++ < max_steps)
        step();
    if (!stack.empty()) {
        vm_.warn(&stack.back(), "check tree ran too long");
        last_exit = Exit::Error;
    }
    stack.swap(saved);
    saved.clear();
    spare_stacks.push_back(std::move(saved));   // (room kept for the next)
    is_check = was_check;
    const Exit result = last_exit;
    if (!keep_temps) {
        temps = saved_temps;
        temp_xl = saved_xl;
    }
    last_exit = saved_exit;
    return result;
}

void Thread::tick()
{
    if (entity_.dead)
        return;
    if (resting > 0) {
        resting--;
        return;
    }
    ticks_this_frame = 0;
    run_immediately();
    // What we were doing is gone (its object removed) while asleep in it: woken, so the check below
    // drops it now, not when the sleep runs out. The Magic Starter Kit's Banish removes the kit and
    // then idles 20,000 ticks in a loop, for the kit's removal to end the action: Lisa stood idling
    // for good with "Toad Sweat" in her queue (the user, 2026-11-10). TS_SLEEP_GONE_OLD=1: as before.
    static const bool sleep_gone_old = TS_ENV("TS_SLEEP_GONE_OLD") != nullptr;
    if (sleeping > 0 && !sleep_gone_old && in_interaction() && (queue.empty() || !vm_.entity(queue[0].callee)))
        sleeping = 0;
    if (sleeping > 0) {
        if (stack.size() == sleeping_depth && !stack.empty() && stack.back().ip < stack.back().routine->code.size() &&
            stack.back().instruction().opcode == 0) {
            sleeping--;
            return;
        }
        if (stack.size() > sleeping_depth && sleeping_depth > 0) {   // something pushed on top: the count goes back to the sleep
            Frame &fr = stack[sleeping_depth - 1];
            if (fr.ip < fr.routine->code.size() && fr.routine->code[fr.ip].opcode == 0 && size_t(sleeping_arg) < fr.args.size())
                fr.args[size_t(sleeping_arg)] = int16_t(sleeping);
        }
        sleeping = 0;
    }
    if (stack.empty()) {
        if (is_check)
            return;
        entity_.run_entry(vm_, 1, false);   // main, again
        if (stack.empty())
            return;
    }
    if (in_interaction() && (queue.empty() || !vm_.entity(queue[0].callee))) {
        // What we were doing is gone: drop the interaction, back to main.
        while (!stack.empty() && stack.back().action_tree)
            stack.pop_back();
        if (!queue.empty())
            queue.erase(queue.begin());
        entity_.set_flag(FlagInteractionCanceled, false);
        interaction_ended();
        if (stack.empty())
            return;
    }
    keep_going_ = true;
    while (keep_going_ && !stack.empty() && !entity_.dead) {
        if (ticks_this_frame++ > 20000) {
            // Long work isn't a fault: the game's limit (its "Too many iterations", FUN_0062d6e0) is
            // (neighbours squared + the lot's objects) x 15 + 10,000 for one run, about 3.2 million
            // with 462 neighbours, because the Visit Generator's walk-by count does ask every
            // neighbour about every neighbour. The game does it in a tick; here it's spread over ticks
            // of this size, so the frame holds. Stopped at 20,000 as this used to be, the script died
            // at 10:00, 14:00, 16:00 and 20:00 every day, with the walk-bys and the friendships' fade.
            if ((long_run += 20000) < vm_.iteration_limit()) {
                // Debug: which code runs this long (the first time a routine does), with the count of
                // its yields so far this run: the Hot Date socials' "update person count" yielded every
                // tick at downtown (20,002 instructions a tick, 977 slow ticks in a session, 2026-11-05).
                if (long_run == 20000 && !stack.empty() && stack.back().routine) {
                    const std::string key = stack.back().routine->name + " of " + (entity_.info ? entity_.info->label : std::string("?"));
                    const int seen = ++vm_.long_runs[key];
                    if (seen == 1 || seen == 100 || seen == 1000)
                        std::printf("long run: %s, %d time(s), at %d of %s\n", key.c_str(), seen, int(stack.back().ip),
                                    stack.size() > 1 ? stack[stack.size() - 2].routine->name.c_str() : "-");
                }
                return;
            }
            long_run = 0;
            std::string path;
            for (const Frame &fr : stack)
                path += " > " + fr.routine->name + " [so #" + std::to_string(fr.stack_id) + "]";
            vm_.warn(&stack.back(), "thread looped without yielding:" + path);
            stack.clear();
            resting = 300;
            return;
        }
        keep_going_ = false;
        step();
    }
    long_run = 0;
}

void Thread::step()
{
    if (stack.empty())
        return;
    execute(stack.back());
}

void Thread::execute(Frame &frame)
{
    if (frame.ip >= frame.routine->code.size()) {
        vm_.warn(&frame, "ran off the end of a routine");
        pop(Exit::Error);
        return;
    }
    const BhavInstruction &ins = frame.instruction();
    if (vm_.watch_lines > 0 && (vm_.watch_entity == entity_.id ||
                                (!vm_.watch.empty() &&
                                 std::find(vm_.watch.begin(), vm_.watch.end(), frame.routine->name) != vm_.watch.end()))) {
        vm_.watch_lines--;
        std::printf("    [tick %u] %s #%d: %s ip %d op %d [%02x %02x %02x %02x %02x %02x %02x %02x] temps %d %d %d  so #%d\n",
                    vm_.ticks, entity_.info ? entity_.info->label.c_str() : "?", entity_.id, frame.routine->name.c_str(),
                    frame.ip, ins.opcode, ins.operand[0], ins.operand[1], ins.operand[2], ins.operand[3],
                    ins.operand[4], ins.operand[5], ins.operand[6], ins.operand[7], temps[0], temps[1], temps[2],
                    frame.stack_id);
    }
    if (ins.opcode >= 256) {   // a subroutine call
        CodeOwner resolved;
        const Routine *r = vm_.routine(frame.owner, ins.opcode, &resolved);
        if (r && vm_.answered_by_engine(*r, resolved)) {   // a few the engine answers "true" itself
            handle(frame, Exit::GotoTrue);
            return;
        }
        if (r && vm_.no_pests_from(*r, frame.stack_id)) {
            handle(frame, Exit::GotoFalse);
            return;
        }
        if (const int answer = r ? vm_.fixed_answer(*r, resolved, frame) : -1; answer >= 0) {
            handle(frame, answer ? Exit::GotoTrue : Exit::GotoFalse);
            return;
        }
        int16_t args[4];
        for (int i = 0; i < 4; i++)
            args[i] = int16_t(ins.operand[i * 2] | ins.operand[i * 2 + 1] << 8);
        call(frame, r, resolved, args);
        keep_going_ = true;
        return;
    }
    const Primitive p = vm_.primitives[ins.opcode];
    if (!p) {
        vm_.warn(&frame, "primitive " + std::to_string(ins.opcode) + " not ported yet");
        handle(frame, Exit::GotoTrue);
        return;
    }
    if (vm_.profile.on) {
        const auto t0 = ts::FastClock::now();
        const uint16_t op = ins.opcode;
        const Exit e = p(frame, ins.operand);
        vm_.profile.primitive_ms[op] += std::chrono::duration<double, std::milli>(ts::FastClock::now() - t0).count();
        vm_.profile.primitive_calls[op]++;
        vm_.profile.instructions++;
        handle(frame, e);
        return;
    }
    handle(frame, p(frame, ins.operand));
}

void Thread::handle(Frame &frame, Exit result)
{
    switch (result) {
    case Exit::ContinueNextTick: keep_going_ = false; break;
    case Exit::Error:
    case Exit::ReturnTrue:
    case Exit::ReturnFalse: pop(result); break;
    case Exit::GotoTrue: move_to(frame, frame.instruction().true_pointer, true); break;
    case Exit::GotoFalse: move_to(frame, frame.instruction().false_pointer, true); break;
    case Exit::GotoTrueNextTick: move_to(frame, frame.instruction().true_pointer, false); break;
    case Exit::GotoFalseNextTick: move_to(frame, frame.instruction().false_pointer, false); break;
    case Exit::Continue: keep_going_ = true; break;
    case Exit::Interrupt:
        stack.clear();
        if (!queue.empty())
            queue.erase(queue.begin());
        last_exit = result;
        break;
    }
}

void Thread::move_to(Frame &frame, uint8_t target, bool keep_going)
{
    switch (target) {
    case 255: pop(Exit::ReturnFalse); break;
    case 254: pop(Exit::ReturnTrue); break;
    case 253: pop(Exit::Error); break;
    default:
        frame.ip = target;
        keep_going_ = keep_going;
        break;
    }
}

void Thread::interaction_ended()
{
    if (!entity_.person)
        return;
    entity_.person_data[33] = 0;   // doing nothing in particular
    // Non-interruptible and the route footprint mask go whatever the interaction left them at
    // (the original writes both as one word). Ours kept them: a Sim whose interaction ended
    // inside the route waiting animation (which sets 71 and puts back what it found) stayed
    // non-interruptible for good, deaf to cancels and orders, and the next route waiting
    // animation kept it so.
    entity_.person_data[71] = 0;
    entity_.person_data[72] = 0;
    // And person data 73, the footprint's shrink (FUN_005f4dc0: 0x622; the exe then rebuilds the
    // Sim's footprint box, FUN_0053a470, which our router doesn't keep). Person data start at the
    // exe's person + 0x590, so its 0x61e and 0x622 are 71-72 and 73.
    entity_.person_data[73] = 0;
    // The motive changes the interaction set go with it (FUN_005f4dc0 empties the same list Set
    // Motive Change appends to): a chair's comfort doesn't go on rising once the Sim is up.
    entity_.motive_changes.clear();
    // The carrying arm with nothing in the hand goes too: a carry pose the code set (Animate's
    // mode 2: the book, the doll) and never took off, left up after the interaction (the sweep's
    // "carry" leads, 2026-10-30; the game never shows one).
    if (entity_.slots.empty() || !entity_.slots[0])
        entity_.carry.reset();
}

void Thread::pop(Exit result)
{
    if (stack.empty())
        return;
    const bool discard = stack.back().discard_result;
    const int16_t saved_running = stack.back().saved_running;
    if (stack.back().resume_caller) {   // back to the instruction that pushed it, to run again
        stack.pop_back();
        last_exit = result;
        if (stack.empty()) {
            keep_going_ = false;
            return;
        }
        keep_going_ = true;
        return;
    }
    if (const char *trace = TS_ENV("TS_TRACE_RETURN"); trace && stack.back().routine && stack.back().routine->name == trace)   // debug
        std::printf("tick %u #%d returns from \"%s\" %s: stack object #%d, temp 0 = %d\n", unsigned(vm_.ticks), int(entity_.id),
                    trace, result == Exit::ReturnTrue ? "true" : result == Exit::ReturnFalse ? "false" : "error",
                    int(stack.back().stack_id), int(temps[0]));
    // Leaving an interaction for main (or what ran before a "run immediately" one).
    const bool leaving = stack.back().action_tree && (stack.size() == 1 || !stack[stack.size() - 2].action_tree || discard);
    if (leaving && TS_ENV("TS_QUEUE_WHY"))   // debug: how each interaction ends
        std::printf("queue: tick %u #%d ends %s at instruction %d: %s\n", unsigned(vm_.ticks), int(entity_.id),
                    stack.back().routine ? stack.back().routine->name.c_str() : "?", int(stack.back().ip),
                    result == Exit::ReturnTrue ? "true" : result == Exit::ReturnFalse ? "false" : "error");
    if (leaving && vm_.interaction_ends) {
        Vm::InteractionEnd end;
        end.sim = int16_t(entity_.id);
        end.interaction = queue.empty() ? -1 : queue[0].interaction;
        end.callee = queue.empty() ? 0 : queue[0].callee;
        end.routine = stack.back().routine ? stack.back().routine->name : "?";
        end.ip = int(stack.back().ip);
        end.result = result == Exit::ReturnTrue ? 1 : result == Exit::ReturnFalse ? 0 : -1;
        vm_.interaction_ends->push_back(std::move(end));
    }
    stack.pop_back();
    last_exit = result;
    if (leaving && !queue.empty()) {
        if (queue[0].mode != QueueMode::ParentIdle)
            entity_.set_flag(FlagInteractionCanceled, false);
        queue.erase(queue.begin());
        interaction_ended();
        if (discard && saved_running >= 0 && entity_.person)
            entity_.person_data[33] = saved_running;   // back to what it was doing
    }
    if (leaving && result == Exit::Error && entity_.person) {
        // An interaction that went wrong leaves the Sim standing where it is,
        // as the game's reset does (not stuck "sitting" with no seat); its headline goes with
        // the reset (FreeSO VMAvatar.Reset).
        if (entity_.container && !entity_.container->person)
            entity_.leave_container();
        entity_.person_data[0] = 0;
        entity_.headline.on = false;
    }
    if (stack.empty()) {
        keep_going_ = false;
        return;
    }
    if (discard)
        result = Exit::Continue;
    else if (leaving && stack.back().instruction().opcode == 30)
        // Back in Gosub Found Action (try autonomy): the action it ran is done, so on the true way.
        result = Exit::GotoTrue;
    else if (leaving)
        // Back in the Allow Push idle that started it, which runs again next
        // tick (FreeSO VMThread.Pop): what the interaction pushed comes next,
        // before main goes on to idle (and drops the plate it was handed).
        result = Exit::ContinueNextTick;
    else if (result == Exit::ReturnTrue)
        result = Exit::GotoTrue;
    else if (result == Exit::ReturnFalse || result == Exit::Error)
        result = result == Exit::Error ? Exit::Error : Exit::GotoFalse;
    handle(stack.back(), result);
}

// ---------------------------------------------------------------- Vm

Vm::Vm(Content &c, Lot &l, Neighborhood *h, int house_number, CharacterLibrary *chars)
    : content(c), lot(l), hood(h), house(house_number), characters(chars)
{
    if (const char *slow = TS_ENV("TS_SLOW_TICKS"))   // debug: log ticks over this many ms
        slow_tick_ms = std::atof(slow);
#if defined(__vita__)
    else
        slow_tick_ms = 20;   // the stutter's slow ticks, named in the log
#endif
    if (!characters) {
        own_characters_ = std::make_unique<CharacterLibrary>(content);
        characters = own_characters_.get();
    }
    if (hood)   // the clothes bought (the neighbourhood save keeps each neighbour's bodystring as changed)
        for (const Neighbor &n : hood->neighbors)
            if (!n.body.empty())
                content.set_body_strings(n.guid, n.body);
    // The lot's saved globals (FreeSims VMWorldActivator.LoadFromIff), with
    // what the game sets itself: every expansion, the runtime version, idle
    // interactions on, this house.
    for (size_t i = 0; i < lot.simi.globals.size() && i < globals.size(); i++)
        globals[i] = lot.simi.globals[i];
    globals[20] = 255;
    globals[25] = 4;
    // 17 and 19 as the exe has them in live play (its step, FUN_00557a00, stops the clock while 17 is
    // set, or 19 on a lot): 19 is the interface's mode (FUN_00409610: 0 live, 1 buy, 2 build, 3
    // options; the help system's "check build mode" tests 2). The lots' files hold 17 = 4 and 19 = 3,
    // saved from the options panel; ours kept them for the whole game (FreeSims set 17 to 4). Ours
    // runs no ticks outside live mode, so live's values are the ones the game's code can see.
    globals[17] = 0;
    globals[19] = 0;
    globals[10] = int16_t(house);
    set_family_global();
    globals[30] = 1;   // free will (Vm::free_will sets it each tick): the selected Sim acts on its own too
    globals[32] = 0;
    clock.hours = globals[0];
    clock.day = globals[1];
    clock.minutes = globals[5];
    clock.fraction = globals[6] * Clock::kTicksPerMinute / 60;
    clock.month = globals[7];
    clock.year = globals[8];
    if (const FileRef *g = content.file("global.iff"))
        global_code.own = content.load(*g);
    register_primitives(*this);
}

Vm::~Vm() = default;

void Vm::warn(const Frame *frame, const std::string &what)
{
    std::string where;
    if (frame && frame->routine) {
        where = " (in " + frame->routine->name + " #" + std::to_string(frame->routine->id);
        if (frame->owner.object)
            where += " of " + (frame->owner.object->name.empty() ? frame->owner.object->label : frame->owner.object->name);
        if (frame->stack_id)
            where += ", stack object #" + std::to_string(frame->stack_id);
        where += ")";
    }
    warnings[what + where]++;
}

CodeOwner Vm::owner_of(const ObjectInfo &info)
{
    CodeOwner o;
    o.object = &info;
    o.own = content.load(info.ref);
    if (!info.semi_global.empty())
        if (const FileRef *s = content.file(info.semi_global + ".iff"))
            o.semi = content.load(*s);
    return o;
}

const Routine *Vm::routine(const CodeOwner &owner, uint16_t id, CodeOwner *resolved)
{
    // 256-4095 global.iff, 4096-8191 the object's own file, 8192+ its semi-global.
    const LoadedIff *file = id >= 8192 ? owner.semi : id >= 4096 ? owner.own : global_code.own;
    if (resolved)
        *resolved = owner;   // the caller's resources stay in scope, as in FreeSims
    if (!file)
        return nullptr;
    auto key = std::make_pair(file, id);
    auto it = routines_.find(key);
    if (it != routines_.end())
        return it->second.get();
    std::unique_ptr<Routine> r;
    if (const IffChunk *c = file->iff.find("BHAV", id)) {
        Bhav b;
        std::string err;
        if (parse_bhav(file->body(*c), c->size, b, err)) {
            r = std::make_unique<Routine>();
            r->id = id;
            r->name = c->label;
            r->args = b.args;
            r->locals = b.locals;
            r->code = std::move(b.instructions);
        }
    }
    return (routines_[key] = std::move(r)).get();
}

const StringTable *Vm::string_table(const LoadedIff *file, const char *type, uint16_t id)
{
    if (!file)
        return nullptr;
    uint32_t tag = 0;
    std::memcpy(&tag, type, 4);
    const auto key = std::make_tuple(file, tag, id);
    if (auto it = string_tables_.find(key); it != string_tables_.end())
        return it->second.get();
    std::unique_ptr<StringTable> t;
    if (const IffChunk *c = file->iff.find(type, id)) {
        t = std::make_unique<StringTable>();
        std::string err;
        if (!parse_str(file->body(*c), c->size, *t, err))
            t.reset();
    }
    return (string_tables_[key] = std::move(t)).get();
}

const Bcon *Vm::constants(const LoadedIff *file, uint16_t id)
{
    if (!file)
        return nullptr;
    const auto key = std::make_pair(file, id);
    if (auto it = constants_.find(key); it != constants_.end())
        return it->second.get();
    std::unique_ptr<Bcon> b;
    if (const IffChunk *c = file->iff.find("BCON", id)) {
        b = std::make_unique<Bcon>();
        std::string err;
        if (!parse_bcon(file->body(*c), c->size, *b, err))
            b.reset();
    }
    return (constants_[key] = std::move(b)).get();
}

void Vm::watch_global_changed(int i, int16_t v)
{
    const Entity *e = entity(int16_t(running_id));
    std::printf("tick %u: global %d %d -> %d by #%d %s\n", unsigned(ticks), i, int(globals[size_t(i)]), int(v), running_id,
                e && e->info ? e->info->label.c_str() : "?");
}

// Whether an object comes from that file (its ObjectInfo::where, "Objects.far:Plants.iff" or a bare
// file name), the file's name given in lower case.
static bool object_file_is(const ObjectInfo &info, const char *file)
{
    std::string w = info.where;
    const size_t cut = w.find_last_of(":/\\");
    if (cut != std::string::npos)
        w = w.substr(cut + 1);
    for (char &c : w)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return w == file;
}

int16_t Vm::tuning(const Frame &frame, uint16_t which)
{
    // "Friendships fade": the step of the game's own daily decrease (Visit Generator, 16:00: each
    // family Sim on the lot and whoever they know, 2 points toward 0), halved or none.
    if (TS_ENV("TS_DECAY_LOG") && frame.routine && frame.routine->name.find("relationships dec") != std::string::npos)   // debug
        std::printf("decay tuning %u in \"%s\": relationship_decay %d\n", unsigned(which), frame.routine->name.c_str(), relationship_decay);
    if (which == 4 && relationship_decay != 0 && frame.routine && frame.routine->name == "relationships dec and search loop")
        return relationship_decay == 1 ? 1 : 0;
    // "Bills" (Gameplay modifiers, the user's 2026-11-13): the mailbox's "process" (MailBox.iff) counts
    // its tuning 1 days down at the hour of its tuning 2 and leaves bills at 0: every 3 days, the
    // original's. bill_days 4-7 answers the days; -1 (off) an hour that never comes, so no bills and
    // no mail carrier for them. The mailbox is known by its object's file (ObjectInfo::where, e.g.
    // "Objects.far:MailBox.iff", any case): the owner's file path is the archive's (".../Objects.far"),
    // which the first check looked in, so the option did nothing (the user, 2026-11-14: "every 6 days
    // ... doesn't feel that way"; 5 bills in 16 days either way). Not MailboxVacation.iff.
    auto is_mailbox = [](const Frame &fr) { return fr.owner.object && object_file_is(*fr.owner.object, "mailbox.iff"); };
    if (TS_ENV("TS_BILLS_LOG") && frame.routine && frame.routine->name == "process")   // debug
        std::printf("bills: tuning %u in \"process\" of %s, mailbox %d, bill_days %d\n", unsigned(which),
                    frame.owner.object ? frame.owner.object->where.c_str() : "(none)", int(is_mailbox(frame)), bill_days);
    if (bill_days != 0 && (which == 1 || which == 2) && frame.routine && frame.routine->name == "process" && is_mailbox(frame)) {
        if (which == 1 && bill_days > 0)
            return int16_t(bill_days);
        if (which == 2 && bill_days < 0)
            return -1;
    }
    // "Plant life span" (the user's, 2026-11-14): a gift bouquet (Hot Date's GiftFlowers.iff) wilts on the
    // day its "init tree" sets, today + its tuning 0, kept within the game's 30-day count (%= 30): the
    // days twice or five times as many, at most 29. (Immortal: Vm::fixed_answer skips the wilting.)
    static thread_local bool plant_tuning = false;
    if (plant_life > 1 && which == 0 && !plant_tuning && frame.routine && frame.routine->name == "init tree" &&
        frame.owner.object && object_file_is(*frame.owner.object, "giftflowers.iff")) {
        plant_tuning = true;
        const int days = tuning(frame, which);
        plant_tuning = false;
        return int16_t(std::clamp(days * plant_life, int(days), 29));
    }
    // FreeSims VMMemory.GetTuningVariable: table in the top bits, key in the low 7.
    uint16_t table = which >> 7;
    const uint16_t key = which & 0x7f;
    auto from = [&](const LoadedIff *file, uint16_t id, int16_t &out) {
        const Bcon *b = constants(file, id);
        if (!b || key >= b->constants.size())
            return false;
        out = int16_t(b->constants[key]);
        if (TS_ENV("TS_TUNING"))   // debug: each constant read, and from where
            std::printf("tuning %u: BCON %u #%u of %s = %d\n", unsigned(which), unsigned(id), unsigned(key),
                        file ? file->ref.path.c_str() : "?", int(out));
        return true;
    };
    int16_t v = 0;
    if (table < 64) {
        if (from(frame.owner.own, uint16_t(table + 4096), v))
            return v;
    } else if (table < 128) {
        table = uint16_t(table - 64);
        if (from(frame.owner.own, uint16_t(table + 8192), v) || from(frame.owner.semi, uint16_t(table + 8192), v))
            return v;
    } else if (table < 192) {
        table = uint16_t(table - 128);
        if (from(global_code.own, uint16_t(table + 256), v))
            return v;
    }
    warn(&frame, "missing tuning constant " + std::to_string(which));
    return 0;
}

float Vm::Curve::at(float x) const
{
    // FreeSO TS1Curve.GetPoint: flat before the first point and after the last.
    if (points.empty())
        return 0;
    if (x < points.front().first)
        return points.front().second;
    size_t i = points.size() - 1;
    while (i > 0 && x < points[i].first)
        i--;
    if (i == points.size() - 1)
        return points[i].second;
    const auto &a = points[i], &b = points[i + 1];
    const float t = (x - a.first) / (b.first - a.first);
    return (1 - t) * a.second + t * b.second;
}

const std::vector<Vm::Curve> &Vm::interaction_curves(bool child)
{
    std::vector<Curve> &out = curves_[child ? 1 : 0];
    if (!out.empty())
        return out;
    // "(-100;-100) (-72;90) (100;90)", one need a line (one entry has ");(").
    for (std::string line : strings(global_code.own, child ? 503 : 501)) {
        for (size_t at; (at = line.find(";(")) != std::string::npos;)
            line.replace(at, 1, " ");
        Curve c;
        for (size_t at = 0; (at = line.find('(', at)) != std::string::npos;) {
            float x = 0, y = 0;
            if (std::sscanf(line.c_str() + at, "(%f;%f)", &x, &y) == 2)
                c.points.emplace_back(x, y);
            at++;
        }
        std::sort(c.points.begin(), c.points.end());
        if (TS_ENV("TS_CURVES"))   // debug: each need's curve as read
            std::printf("interaction curve %lu: %s\n", (unsigned long)out.size(), line.c_str());
        out.push_back(std::move(c));
    }
    while (out.size() < 9)   // no table: every level worth the same
        out.push_back(Curve{ { { -100.0f, 1.0f }, { 100.0f, 1.0f } } });
    return out;
}

const std::vector<Vm::Curve> &Vm::mood_curves(bool child)
{
    std::vector<Curve> &out = mood_curves_[child ? 1 : 0];
    if (!out.empty())
        return out;
    for (std::string line : strings(global_code.own, child ? 504 : 502)) {
        Curve c;
        for (size_t at = 0; (at = line.find('(', at)) != std::string::npos; at++) {
            float x = 0, y = 0;
            if (std::sscanf(line.c_str() + at, "(%f;%f)", &x, &y) == 2)
                c.points.emplace_back(x, y);
        }
        std::sort(c.points.begin(), c.points.end());
        out.push_back(std::move(c));
    }
    while (out.size() < 7)   // no table: each need counts the same
        out.push_back(Curve{ { { -100.0f, 1.0f }, { 100.0f, 1.0f } } });
    return out;
}

const ObjectInfo *Vm::master_of(const ObjectInfo &info)
{
    const uint16_t master = info.objd.get(Objd::MasterID);
    if (master == 0)
        return nullptr;
    auto key = std::make_pair(info.ref.path + "#" + std::to_string(info.ref.offset), master);
    auto it = masters_.find(key);
    if (it != masters_.end())
        return it->second;
    const ObjectInfo *best = nullptr;
    for (const auto &entry : content.objects().by_guid) {
        const ObjectInfo &o = entry.second;
        if (o.ref.path == info.ref.path && o.ref.offset == info.ref.offset && o.objd.get(Objd::MasterID) == master &&
            o.objd.get(Objd::SubIndex) == 0xffff)
            best = &o;
    }
    return masters_[key] = best;
}

std::vector<std::string> Vm::strings(const LoadedIff *file, uint16_t table)
{
    std::vector<std::string> out;
    if (!file)
        return out;
    if (const StringTable *t = string_table(file, "STR#", table))
        for (size_t i = 0;; i++) {
            const std::string *s = t->get(i);
            if (!s)
                break;
            out.push_back(*s);
        }
    return out;
}

std::string Vm::string(const CodeOwner &owner, uint16_t table, int index)
{
    for (const LoadedIff *f : { owner.own, owner.semi, global_code.own }) {
        if (!f)
            continue;
        if (const StringTable *t = string_table(f, "STR#", table))
            if (const std::string *s = t->get(size_t(index)))
                return *s;
    }
    return std::string();
}

const std::vector<int16_t> &Vm::person_ids()
{
    if (person_ids_tick_ != ticks || person_ids_count_ != entities.size()) {
        person_ids_.clear();
        for (size_t id = 1; id < by_id_.size(); id++)
            if (by_id_[id] && by_id_[id]->person)
                person_ids_.push_back(int16_t(id));
        // In the exe's person list order (appended as they come: Entity::seq), which Set To
        // Next's "next person" walks; by id with TS_NEXT_BY_ID=1, as before.
        static const bool by_id = TS_ENV("TS_NEXT_BY_ID") != nullptr;
        if (!by_id)
            std::stable_sort(person_ids_.begin(), person_ids_.end(),
                             [&](int16_t a, int16_t b) { return by_id_[size_t(a)]->seq < by_id_[size_t(b)]->seq; });
        person_ids_tick_ = ticks;
        person_ids_count_ = entities.size();
    }
    return person_ids_;
}

bool Vm::check_object_order(const char *where)
{
    static int reported = 0;
    bool ok = true;
    {   // and every entry of the list an object that exists (a freed one there is the loops' cause)
        std::unordered_set<const Entity *> have;
        for (const auto &u : entities)
            have.insert(u.get());
        for (const auto &[key, e] : object_order)
            if (!have.count(e)) {
                ok = false;
                if (reported < 20) {
                    reported++;
                    std::printf("object order (%s, tick %u): the list's place %lld points at an object that doesn't exist\n", where,
                                unsigned(ticks), (long long)key);
                }
            }
    }
    for (const auto &u : entities) {
        const Entity *e = u.get();
        if (e->dead)
            continue;
        const auto it = object_order.find(e->list_key);
        const bool listed = it != object_order.end() && it->second == e;
        const bool by_own_id = entity(e->id) == e;
        if (listed && by_own_id)
            continue;
        ok = false;
        if (reported < 20) {
            reported++;
            const Entity *other = it != object_order.end() ? it->second : nullptr;
            const Entity *named = entity(e->id);
            std::printf("object order (%s, tick %u): #%d %s key %lld%s%s\n", where, unsigned(ticks), int(e->id),
                        e->info ? e->info->label.c_str() : "?", (long long)e->list_key,
                        listed ? "" : (other ? (" not listed: the key holds #" + std::to_string(other->id) + " " +
                                                (other->info ? other->info->label : std::string("?"))).c_str()
                                             : " not listed: the key is empty"),
                        by_own_id ? "" : (named ? (", its id names #" + std::to_string(named->id) + " " +
                                                   (named->info ? named->info->label : std::string("?"))).c_str()
                                                : ", its id names nothing"));
        }
    }
    return ok;
}

void Vm::set_list_place(Entity &e, int64_t key, uint64_t seq)
{
    auto it = object_order.find(e.list_key);
    if (it != object_order.end() && it->second == &e)
        object_order.erase(it);
    e.list_key = key;
    e.seq = seq;
    object_order[key] = &e;
    head_key = std::min(head_key, key);
    tail_key = std::max(tail_key, key);
    next_seq = std::max(next_seq, seq);
}

Entity *Vm::create(const ObjectInfo &info, int tile_x, int tile_y, int level, int direction, uint16_t id)
{
    auto e = std::make_unique<Entity>();
    // Its place in the exe's lists (FUN_0052bd20): one with its id given (the lot's, a save's:
    // FUN_0052d5f0) goes at the object list's tail, a new one (Create Object and the rest, id 0)
    // at its head; every one at the end of the person and per-type lists.
    e->list_key = id != 0 ? ++tail_key : --head_key;
    e->seq = ++next_seq;
    // (Entered in the lists at the end, all at once: see there.)
    if (id == 0) {
        // The lowest free id, as the original (FUN_0052bd20 scans from 1): a removed object's once
        // nothing holds it any more (forget_removed), else the next. Ours never reused one, so a lot
        // played long enough (190 game days on the Goths') would have run past the game's 32,767
        // and made objects its code couldn't see.
        static const bool no_reuse = TS_ENV("TS_NO_ID_REUSE") != nullptr;   // debug: the old way
        while (!no_reuse && !free_ids_.empty()) {
            std::pop_heap(free_ids_.begin(), free_ids_.end(), std::greater<uint16_t>());
            const uint16_t low = free_ids_.back();
            free_ids_.pop_back();
            if (low > 0 && low < next_id_ && (low >= by_id_.size() || by_id_[low] == nullptr)) {
                id = low;
                break;
            }
        }
        if (id == 0) {
            if (next_id_ >= 32767) {   // (none free and the last one gone: nothing more can be made)
                warn(nullptr, "no object ids left");
                return nullptr;
            }
            id = next_id_;
        }
    }
    e->id = id;
    e->serial = next_serial_++;
    next_id_ = std::max<uint16_t>(next_id_, uint16_t(id + 1));
    e->info = &info;
    e->code = owner_of(info);
    e->person = info.objd.get(Objd::ObjectType) == 2;
    e->direction = direction & 7;
    if (tile_x >= 0) {
        e->x16 = int16_t(tile_x * 16 + 8);
        e->y16 = int16_t(tile_y * 16 + 8);
        e->level = int8_t(level + 1);
    }
    e->attributes.assign(info.objd.get(Objd::NumAttributes), 0);
    // Entry points: the OBJf table, or the routines named in the OBJD.
    if (info.objd.get(Objd::UsesFnTable) && e->code.own) {
        if (const IffChunk *c = e->code.own->iff.find("OBJf", info.objd_id)) {
            std::string err;
            parse_objf(e->code.own->body(*c), c->size, e->entry_points, err);
        }
    } else {
        static const Objd::Field kEntries[] = {
            Objd::BhavInit, Objd::BhavMain, Objd::BhavLoad, Objd::BhavCleanup, Objd::BhavQueueSkipped,
            Objd::BhavAllowIntersection, Objd::BhavWallAdjacencyChanged, Objd::BhavRoomChange,
            Objd::BhavDynamicMultiTileUpdate, Objd::BhavPlace, Objd::BhavPickup, Objd::BhavUserPlace,
            Objd::BhavUserPickup, Objd::BhavLevelInfo, Objd::BhavServingSurface, Objd::BhavPortal,
            Objd::BhavGardening, Objd::BhavWashHands, Objd::BhavPrepareFood, Objd::BhavCookFood,
            Objd::BhavPlaceSurface, Objd::BhavDispose, Objd::BhavEat, Objd::BhavPickupFromSlot, Objd::BhavWashDish,
            Objd::BhavEatSurface, Objd::BhavSit, Objd::BhavStand, Objd::BhavClean, Objd::BhavRepair,
        };
        for (Objd::Field f : kEntries)
            e->entry_points.push_back({ 0, info.objd.get(f) });
        e->entry_points.resize(33);
    }
    e->set_flag(FlagChairFacing, true);
    // Interactions: the tree table in the object's file, else in its semi-global;
    // a part of a multi-tile object uses the one its master definition names.
    e->master = master_of(info);
    uint16_t table = info.objd.get(Objd::TreeTableID);
    if ((table == 0 || table == 0xffff) && e->master)
        table = e->master->objd.get(Objd::TreeTableID);
    e->tree_table_id = table;
    if (table != 0 && table != 0xffff) {
        for (const LoadedIff *f : { e->code.own, e->code.semi })
            if (const Ttab *t = ttab(f, table)) {
                e->tree_table = t;
                e->tree_file = f;
                break;
            }
    }
    if (e->person) {
        // A Sim's lasting person data comes from the neighbourhood.
        if (hood)
            if (const Neighbor *n = hood->by_guid(info.guid)) {
                std::copy(n->person_data.begin(), n->person_data.end(), e->person_data.begin());
                e->person_data[31] = n->id;   // NeighborId
            }
        deal_interests(*e);
        set_body_animations(*e);
        // Resident or visitor (person data 32), as the game's code compares it:
        // global tuning table 260, entries 0 and 1.
        if (const LoadedIff *g = global_code.own)
            if (const IffChunk *c = g->iff.find("BCON", 260)) {
                Bcon b;
                std::string err;
                if (parse_bcon(g->body(*c), c->size, b, err) && b.constants.size() > 1)
                    e->person_data[32] = int16_t(b.constants[resident(*e) ? 0 : 1]);
            }
        // Motives aren't in the lot file we read: start everyone comfortable.
        for (int m : { 5, 6, 7, 8, 9, 14, 15 })
            e->motives[size_t(m)] = 50;
    }
    e->thread = std::make_unique<Thread>(*this, *e);
    {   // Debug (TS_FAIL_CREATE=<n>): the n-th object made runs out of memory here, as the heap did
        // (TS_CREATE_LIST_FIRST=1 enters it in the object list first, as before the fix, to compare).
        static const int fail_at = TS_ENV("TS_FAIL_CREATE") ? std::atoi(TS_ENV("TS_FAIL_CREATE")) : 0;
        static int made = 0;
        if (TS_ENV("TS_CREATE_LIST_FIRST"))
            object_order[e->list_key] = e.get();
        if (fail_at > 0 && ++made == fail_at) {
            std::printf("create: #%d made to run out of memory (TS_FAIL_CREATE)\n", int(made));
            throw std::bad_alloc();
        }
    }
    // All or nothing. Everything that can run out of memory first (the id table grown, the entity
    // list's room), then the object entered in the lists, the one step that can still fail first.
    // It used to go into the object list at the start: an allocation failing after that (a tick cut
    // short by the heap running out) or "no object ids left" left the list pointing at a freed
    // object, and every Set To Next walk that reached it jumped about the list for ever (the
    // waitress's and the Hot Date socials' 20,000-instruction loops downtown, 2026-11-05).
    if (by_id_.size() <= id)
        by_id_.resize(size_t(id) + 1, nullptr);
    // (Grown by half again when full, as push_back would: reserve(size + 1) reallocated the list at
    // nearly every creation, quadratic, and pulled the rug from under any loop over it that makes
    // something on the way: snap's ask_to_move, a crash on lot 21, 2026-11-05.)
    if (entities.size() == entities.capacity())
        entities.reserve(entities.capacity() + entities.capacity() / 2 + 16);
    object_order[e->list_key] = e.get();   // (a map's insert: all or nothing)
    by_id_[id] = e.get();
    entities.push_back(std::move(e));      // (the room is there: no allocation, nothing thrown)
    return entities.back().get();
}

void Vm::set_body_animations(Entity &entity)
{
    Entity *e = &entity;
    e->walk_animations.clear();
    // Walking and standing animations by posture: the global table (StdAdult, or StdChild for
    // a child: c2o-, a child's own walk), the Sim's own over it (FreeSO InitBodyData).
    const bool child = e->person_data[58] > 0 && e->person_data[58] < 18;
    e->swim_animations = strings(global_code.own, child ? 160 : 158);
    e->walk_animations = strings(global_code.own, child ? 151 : 150);
    const std::vector<std::string> own = strings(e->code.own, 150);
    // A table for another body (a cat's "k2o-", a dog's "d2o-"): what it doesn't list stays
    // empty, not a person's animation. A cat's walk had a2o-walking-half-loop (entry 25, the
    // slow-walk third of the mix) on its skeleton, which stood it up on its hind legs.
    auto body = [](const std::string &name) { return name.substr(0, name.find("2o-")); };
    std::string own_body, people_body;
    for (const std::string &s : own)
        if (own_body.empty() && s.find("2o-") != std::string::npos)
            own_body = body(s);
    for (const std::string &s : e->walk_animations)
        if (people_body.empty() && s.find("2o-") != std::string::npos)
            people_body = body(s);
    if (!own_body.empty() && own_body != people_body)
        for (std::string &s : e->walk_animations)
            s.clear();
    // Women walk as women: the game has the women's walk (a2o-walking-female-start, -loop, -stop;
    // Animation.far, ExpansionShared's -ul copies the same), but since Unleashed nothing names it (Global
    // STR# 153 "StdFemale" is empty): every woman walked with the men's loop, the "gorilla walk" (the
    // community's "Gorilla Walk Fix"). An adult woman of the people's body: StdFemale where it names one,
    // else the women's walk (19 start, 20 loop, 27 and 28 stop). TS_FEMALE_WALK_OLD=1.
    static const bool female_walk_old = TS_ENV("TS_FEMALE_WALK_OLD") != nullptr;
    if (community_fixes && !female_walk_old && !child && e->person_data.size() > 65 && e->person_data[65] == 1 &&
        people_body == "a" && (own_body.empty() || own_body == "a")) {
        const std::vector<std::string> female = strings(global_code.own, 153);
        const std::pair<size_t, const char *> steps[] = { { 19, "a2o-walking-female-start" }, { 20, "a2o-walking-female-loop" },
                                                          { 27, "a2o-walking-female-stop" }, { 28, "a2o-walking-female-stop" } };
        for (const auto &[i, name] : steps) {
            if (i >= e->walk_animations.size())
                e->walk_animations.resize(i + 1);
            e->walk_animations[i] = i < female.size() && !female[i].empty() ? female[i] : std::string(name);
        }
    }
    for (size_t i = 0; i < own.size(); i++) {
        if (i >= e->walk_animations.size())
            e->walk_animations.resize(i + 1);
        if (!own[i].empty())
            e->walk_animations[i] = own[i];
    }
#if !defined(__vita__)
    if (TS_ENV("TS_WALK_TABLE")) {   // debug: each Sim's walk table as made
        std::printf("walk table of %s:", e->info->label.c_str());
        for (size_t i = 0; i < e->walk_animations.size(); i++)
            std::printf(" %lu=%s", (unsigned long)i, e->walk_animations[i].c_str());
        std::printf("\n");
    }
#endif
}

int64_t Vm::iteration_limit() const
{
    const int64_t n = hood ? int64_t(hood->neighbors.size()) : 0;
    return (n * n + int64_t(entities.size())) * 15 + 10000;
}

void Vm::load_lot()
{
    // As FreeSims does for Sims 1 lots: each saved object is created fresh
    // where the lot has it, keeping its saved id (saved data refers to
    // objects by id), with its saved object data; then every object runs
    // init and user-place now, and main from its thread.
    // The Sims of the families the game adds to a neighbourhood when it first opens (Unleashed's
    // Kats, Burbs, ...): their lots keep Sims of the same names under GUIDs no character has, the game
    // having given each its own character file. Those are the family, as saved on the lot.
    std::vector<uint32_t> residents;
    if (hood)
        for (const Family &f : hood->families)
            if (f.house == house && house != 0)
                for (uint32_t guid : f.members)
                    if (std::none_of(lot.objects.begin(), lot.objects.end(), [&](const LotObject &o) { return o.guid == guid; }))
                        residents.push_back(guid);
    // A family the engine brought in itself from its family file (world/hood_import.h) is known by
    // the GUIDs that file's lot has its Sims under (Neighborhood::guid_alias): each Sim is that one,
    // whatever its name (the pets' names aren't their objects'). TS_ALIAS_OFF=1: by name, as before.
    // One of them who doesn't live here any more (the family moved out, another moved into the lot
    // as its file has it) is left out, not taken for one of the new family.
    static const bool alias_off = TS_ENV("TS_ALIAS_OFF") != nullptr;
    auto resident_named = [&](const std::string &name) {
        auto it = std::find_if(residents.begin(), residents.end(), [&](uint32_t guid) {
            const ObjectInfo *who = content.object(guid);   // (the neighbour's own name is the file's: user00035)
            return who && (who->name == name || who->label == name);
        });
        if (it == residents.end())
            it = residents.begin();
        const uint32_t guid = *it;
        residents.erase(it);
        return guid;
    };
    // A lot that arrived from another console with no save (never played there: Ad hoc Share,
    // world/share.h): its file has the sender's family under their GUIDs there. Each is the Sim
    // made for them here; any other person in the file isn't here.
    const ShareArrival *arrival = nullptr;
    if (hood && !TS_ENV("TS_SHARE_SWAP_OFF"))
        for (const ShareArrival &a : hood->arrivals)
            if (a.house == house && a.save_hash == 0)
                arrival = &a;
    std::vector<Entity *> made;
    for (const LotObject &o : lot.objects) {
        const ObjectInfo *info = content.object(o.guid);
        uint32_t resident = 0;
        if (arrival && o.person) {
            const ShareArrival::Member *member = nullptr;
            for (const ShareArrival::Member &m : arrival->members)
                if (m.old_guid == o.guid)
                    member = &m;
            if (!member)
                continue;
            if (auto it = std::find(residents.begin(), residents.end(), member->guid); it != residents.end())
                residents.erase(it);
            resident = member->guid;
            info = content.object(resident);
        }
        if (!info && o.person && hood && !alias_off)
            if (auto alias = hood->guid_alias.find(o.guid); alias != hood->guid_alias.end()) {
                auto it = std::find(residents.begin(), residents.end(), alias->second);
                if (it == residents.end())
                    continue;   // moved out
                resident = *it;
                residents.erase(it);
                info = content.object(resident);
            }
        if (!info && o.person && !residents.empty()) {
            resident = resident_named(o.type_name);
            info = content.object(resident);
        }
        if (!info) {
            char text[64];
            std::snprintf(text, sizeof text, "lot object with an unknown GUID %08x", unsigned(o.guid));
            warn(nullptr, text);
            continue;
        }
        Entity *e = create(*info, o.x, o.y, o.level, o.direction(), o.id);
        made.push_back(e);
        if (o.restored)
            restore_saved(*e, o);
        if (resident)
            if (const Neighbor *n = hood->by_guid(resident)) {
                if (e->person_data.size() > 31)
                    e->person_data[31] = n->id;
                e->tokens = n->tokens;
                if (arrival && e->person_data.size() > 61)
                    e->person_data[61] = arrival->family;   // (the file's is the sender's family number)
            }
        loading_pause();
    }
    // What the save has held in slots goes back into its container; what
    // shares a tile with another object (a window over a counter, the top of
    // a tall window) goes where that object is, at its own level offset.
    for (const LotObject &o : lot.objects) {
        Entity *e = entity(int16_t(o.id));
        if (!e || o.x >= 0)
            continue;
        if (o.container() != 0) {
            if (Entity *holder = entity(int16_t(o.container())))
                holder->place_in_slot(e, o.data[ObjSlotNumber]);
            continue;
        }
        const LotObject *root = &o;
        for (int hops = 0; root->x < 0 && root->data[ObjParentId] != 0 && hops < 8; hops++) {
            auto it = std::find_if(lot.objects.begin(), lot.objects.end(),
                                   [&](const LotObject &c) { return c.id == uint16_t(root->data[ObjParentId]); });
            if (it == lot.objects.end())
                break;
            root = &*it;
        }
        if (root->x < 0)
            continue;
        // Its level: the tile's (the grid the other object is in), raised by its own level
        // offset; against another part of the same object (a tall window's top over its
        // bottom) by the difference, as both count from their lead. Another object's offset is
        // its own business: a window sharing a tile with a stair's top part (offset 1 from the
        // stair) went a storey down, into the wall between the Goths' hall and bathroom.
        const ObjectInfo *theirs = content.object(root->guid);
        const int16_t own_offset = int16_t(e->info->objd.get(Objd::LevelOffset));
        const bool same_object = theirs && theirs->where == e->info->where &&
                                 theirs->objd.get(Objd::MasterID) != 0 &&
                                 theirs->objd.get(Objd::MasterID) == e->info->objd.get(Objd::MasterID);
        const int level = root->level + own_offset - (same_object ? int16_t(theirs->objd.get(Objd::LevelOffset)) : 0);
        if (TS_ENV("TS_PLACEDBG"))   // debug: objects placed by the one whose tile they share
            std::printf("placed #%d %s by #%d %s at (%d, %d) level %d: own offset %d, theirs %d -> level %d\n", int(o.id),
                        e->info->label.c_str(), int(root->id), theirs ? theirs->label.c_str() : "?", root->x, root->y,
                        root->level, int(int16_t(e->info->objd.get(Objd::LevelOffset))),
                        theirs ? int(int16_t(theirs->objd.get(Objd::LevelOffset))) : 0, level);
        if (level >= 0 && level < kLotLevels)
            e->set_position(int16_t(root->x * 16 + 8), int16_t(root->y * 16 + 8), int8_t(level + 1));
    }
    // The game's global objects (OBJD Global 1: controllers, the phone's plugins for going
    // downtown or on vacation, ...) are on every lot, out of the world (FreeSO
    // VMTS1Activator); lots saved before an expansion lack theirs. The Hot Date gift tokens (type
    // 12, 76 of them global) too: FreeSO leaves them out ("mysteriously set as global"), but the
    // social menu's "Give Gift..." check (PersonGlobals 8457) looks for the objects of type 12 on
    // the lot and asks each one (its "CT - Offer on Menu") whether the Sim holds its token; with
    // none there, John's roses were never offered to Linda (the user, 2026-11-06 and -07).
    for (Entity *e : add_missing_globals())
        made.push_back(e);
    group_parts();
    // A part the lot file lists without a place whose object's lead is on the lot goes where the
    // lead puts it: by its SubIndex from the lead's tile, turned with it (the original and FreeSO
    // place a multi-tile object from its lead). A plate glass window's part 1 on a downtown lot
    // had no cell, its part 2 (the lead) did: the window was half made, its room code at a
    // breakpoint. Which lead: one of the same object missing that part, the nearest by id (an
    // object's parts are numbered together).
    {
        bool moved = false;
        for (Entity *e : made) {
            if (e->person || !e->info || e->in_world() || e->container)
                continue;
            const Objd &d = e->info->objd;
            const uint16_t master = d.get(Objd::MasterID), sub = d.get(Objd::SubIndex);
            if (master == 0 || sub == 0 || sub == 0xffff)
                continue;
            Entity *lead = nullptr;
            for (Entity *c : made) {
                if (c == e || !c->info || !c->in_world() || c->container || c->info->where != e->info->where ||
                    c->info->objd.get(Objd::MasterID) != master || c->info->objd.get(Objd::SubIndex) != 0 ||
                    c->tile_x() >= lot.size || c->tile_y() >= lot.size)   // (not a lead kept past the lot)
                    continue;
                bool has_it = false;
                for (Entity *part : c->group)
                    has_it = has_it || (part->info && part->info->objd.get(Objd::SubIndex) == sub);
                if (!has_it && (!lead || std::abs(int(c->id) - int(e->id)) < std::abs(int(lead->id) - int(e->id))))
                    lead = c;
            }
            if (!lead)
                continue;
            const int ox = int8_t(sub >> 8), oy = int8_t(sub & 0xff);
            int rx = ox, ry = oy;
            switch ((lead->direction / 2) % 4) {
            case 1: rx = -oy; ry = ox; break;
            case 2: rx = -ox; ry = -oy; break;
            case 3: rx = oy; ry = -ox; break;
            default: break;
            }
            e->direction = lead->direction;
            e->set_position(int16_t((lead->tile_x() + rx) * 16 + 8), int16_t((lead->tile_y() + ry) * 16 + 8),
                            int8_t(lead->level + int16_t(d.get(Objd::LevelOffset)) -
                                   int16_t(lead->info->objd.get(Objd::LevelOffset))));
            if (TS_ENV("TS_PLACEDBG"))   // debug
                std::printf("placed #%d %s by its lead #%d at (%d, %d)\n", int(e->id), e->info->label.c_str(), int(lead->id),
                            e->tile_x(), e->tile_y());
            moved = true;
            group_parts();   // (so the next part of the same object finds this lead has it)
        }
        if (moved)
            group_parts();
    }
    // A part of a multi-tile object the lot file lists without a place, a container or the rest
    // of its object (two or three parts of a dartboard on a Studio Town lot, its lead missing)
    // isn't made: FreeSO's lot loader (VMTS1Activator) makes a multi-tile object from its lead
    // only, whole. Made, they ran their code out of the world looking for the part that wasn't
    // there, round and round without a tick passing ("Group Talk - Create", the stall finder).
    for (Entity *e : made)
        if (!e->person && e->info && e->info->objd.get(Objd::MasterID) != 0 &&
            e->info->objd.get(Objd::SubIndex) != 0xffff && !e->in_world() && !e->container && e->group.empty()) {
            if (TS_ENV("TS_PLACEDBG"))   // debug
                std::printf("not made: #%d %s, a part of a multi-tile object on its own and nowhere\n", int(e->id),
                            e->info->label.c_str());
            e->dead = true;
        }
    made.erase(std::remove_if(made.begin(), made.end(), [](const Entity *e) { return e->dead; }), made.end());
    build_room_map();
    refresh_rooms();
    rooms_ready_ = true;   // from here on, objects' "room changed" code runs (refresh_rooms)
    // Objects whose saved state came back carry on from it (Load); the rest start new.
    std::vector<bool> restored;
    for (Entity *e : made) {
        const auto it = std::find_if(lot.objects.begin(), lot.objects.end(),
                                     [&](const LotObject &o) { return o.id == e->id; });
        restored.push_back(it != lot.objects.end() && it->restored);
    }
    // A category the save left at 0 (the Goths' counters, fridge and stove: their counter
    // code then finds no counter beside it and turns the corner piece straight, as its Main
    // re-picks the piece at once here): the one its Init gives it. (The game resumes each
    // object's code where it was saved; this engine starts Main again.)
    for (size_t i = 0; i < made.size(); i++)
        if (restored[i] && !made[i]->person && made[i]->data[VarCategory] == 0)
            if (const int16_t c = init_category(*made[i]))
                made[i]->data[VarCategory] = c;
    // Lights start new as well: their Init keeps their brightness in attribute 2 (50 or 75), which
    // "set object lighting" gives the room when they're on; the Goths' saved table lamps have it
    // at 0 (saved before their Init set it?), so lit they gave the room no light at night. Their
    // Main works out on or off again within its first pass.
    for (size_t i = 0; i < made.size(); i++)
        if (restored[i] && made[i]->info && !made[i]->person &&
            ts::fold_path(made[i]->info->semi_global) == "lightglobals")
            restored[i] = false;
    for (size_t i = 0; i < made.size(); i++) {
        made[i]->run_entry(*this, restored[i] ? 2 : 0, true);   // load or init
        loading_pause();
    }
    for (size_t i = 0; i < made.size(); i++)
        if (!restored[i] && made[i]->in_world())   // (the global objects out of the world aren't placed)
            made[i]->run_entry(*this, 11, true);   // user place
    if (arrival)
        import_arrival(*arrival);
    else if (lot.exported)   // a family file's lot (brought in from the PC, or Unleashed's): as the exe (FUN_0054eb20)
        run_import_lot();
    refresh_rooms();   // each object's "room changed" code, now that it's set up
    for (Entity *e : made)
        e->run_entry(*this, 1, false);   // main, on its thread
    collect_free_ids();
}

int Vm::run_import_lot()
{
    if (TS_ENV("TS_IMPORT_LOT_OFF"))   // tests: as before (the trees not run)
        return 0;
    int ran = 0;
    std::vector<Entity *> all;
    for (auto &e : entities)
        if (!e->dead && e->thread)
            all.push_back(e.get());
    for (Entity *e : all) {
        CodeOwner owner;
        const Routine *tree = e->tree_by_name(*this, "import lot", owner);
        if (!tree || e->dead)
            continue;
        Frame f;
        f.routine = tree;
        f.caller = f.callee = e;
        f.owner = owner;
        f.stack_id = int16_t(e->id);
        f.args.assign(std::max<size_t>(tree->args, 4), 0);
        e->thread->run_now(std::move(f));
        std::printf("import lot: #%d %s\n", int(e->id), e->info ? e->info->label.c_str() : "?");
        ran++;
    }
    return ran;
}

void Vm::import_arrival(const ShareArrival &arrival)
{
    std::vector<std::pair<Entity *, int16_t>> ghosts;   // each tombstone and the neighbour id it held
    std::map<uint32_t, int> ghost_of;
    for (auto &e : entities) {
        if (e->dead || e->person || !e->info)
            continue;
        auto it = ghost_of.find(e->info->guid);
        if (it == ghost_of.end())
            it = ghost_of.emplace(e->info->guid, ghost_attribute(content, *e->info)).first;
        if (it->second >= 0 && size_t(it->second) < e->attributes.size() && e->attributes[size_t(it->second)] != 0)
            ghosts.push_back({ e.get(), e->attributes[size_t(it->second)] });
    }
    run_import_lot();
    if (TS_ENV("TS_GHOSTS_OFF"))   // tests: as "import lot" leaves them (none)
        return;
    for (const auto &[e, theirs] : ghosts) {
        int16_t ours = 0;
        for (const ShareArrival::Member &m : arrival.members)
            if (m.old_neighbor == theirs)
                ours = m.neighbor;
        for (const auto &[from, to] : arrival.neighbors)
            if (!ours && from == theirs)
                ours = to;
        e->attributes[size_t(ghost_of[e->info->guid])] = ours;
        const Neighbor *n = ours && hood ? hood->by_id(ours) : nullptr;
        const ObjectInfo *who = n ? content.object(n->guid) : nullptr;
        std::printf("share: #%d %s holds neighbour %d there: %d here (%s)\n", int(e->id), e->info->label.c_str(), int(theirs),
                    int(ours), who ? who->name.c_str() : "not here");
    }
}

// The global objects the lot lacks, made out of the world (see load_lot); returned uninitialised.
// TS_GIFT_GLOBALS_OFF=1: the gift tokens left out, as before.
std::vector<Entity *> Vm::add_missing_globals(int only_type)
{
    std::vector<Entity *> made;
    static const bool gifts_off = TS_ENV("TS_GIFT_GLOBALS_OFF") != nullptr;
    std::set<uint32_t> present;
    for (const auto &e : entities)
        if (e->info && !e->dead)
            present.insert(e->info->guid);
    for (const auto &[guid, info] : content.objects().by_guid)
        if (info.objd.get(Objd::Global) == 1 && (!gifts_off || info.objd.get(Objd::ObjectType) != 12) && !present.count(guid) &&
            !info.objd.get(Objd::MasterID) && (only_type < 0 || info.objd.get(Objd::ObjectType) == only_type))
            if (Entity *e = create(info, -1, -1, 0, 0)) {
                made.push_back(e);
                loading_pause();
                if (TS_ENV("TS_GLOBALS"))
                    std::printf("global object %08x lot categories %04x %s (%s)\n", guid,
                                unsigned(info.objd.get(Objd::LotCategories)), info.label.c_str(), info.where.c_str());
            }
    return made;
}

void Vm::collect_free_ids()
{
    free_ids_.clear();
    for (size_t id = 1; id < by_id_.size() && id < next_id_; id++)
        if (!by_id_[id])
            free_ids_.push_back(uint16_t(id));
    std::make_heap(free_ids_.begin(), free_ids_.end(), std::greater<uint16_t>());
}

void Vm::store_people()
{
    if (!hood)
        return;
    for (const auto &e : entities) {
        if (!e->person || e->dead || e->person_data[31] == 0)
            continue;
        Neighbor *n = hood->by_id(e->person_data[31]);
        if (!n || (e->info && n->guid != e->info->guid))
            continue;
        const size_t size = std::max<size_t>(n->person_data.size(), 88);
        n->person_data.assign(e->person_data.begin(), e->person_data.begin() + std::ptrdiff_t(std::min(size, e->person_data.size())));
        n->tokens = e->tokens;
        // What only means something on this lot (FreeSO InheritNeighbor resets these):
        // posture, tick counter, resident/visitor, priority, greeting, visit schedule, route entry.
        for (int k : { 0, 27, 32, 33, 34, 35, 37 })
            if (size_t(k) < n->person_data.size())
                n->person_data[size_t(k)] = 0;
    }
}

void Vm::deal_interests(Entity &e)
{
    static const bool old = TS_ENV("TS_INTERESTS_OLD") != nullptr;
    if (old || !e.person || e.person_data.size() < 56)
        return;
    for (int i = 46; i <= 53; i++)
        if (e.person_data[size_t(i)] >= 11)
            return;   // dealt already (or set by the game's code)
    auto rnd = [&]() {
        random_state = random_state * 1103515245u + 12345u;
        return int((random_state >> 8) & 0x7fffff);
    };
    auto deal = [&](int low, int mid, int high, const int *fields, int count) {
        int left[3] = { low, mid, high };
        for (int k = 0; k < count; k++) {
            int band;
            do
                band = rnd() % 3;
            while (left[band] < 1);
            left[band]--;
            const int v = band == 0 ? (rnd() & 3) : band == 1 ? rnd() % 3 + 4 : (rnd() & 3) + 7;
            e.person_data[size_t(fields[k])] = int16_t(v * 100);
        }
    };
    static const int kTen[10] = { 46, 47, 48, 49, 50, 51, 52, 53, 54, 55 };
    static const int kHotDate[5] = { 13, 14, 16, 20, 26 };
    deal(4, 3, 3, kTen, 10);
    deal(1, 2, 2, kHotDate, 5);
    if (TS_ENV("TS_INTERESTS"))   // debug
        std::printf("interests dealt for #%d (neighbour %d): 46 %d, 47 %d, 13 %d\n", int(e.id), int(e.person_data[31]),
                    int(e.person_data[46]), int(e.person_data[47]), int(e.person_data[13]));
}

void Vm::end_trip_here()
{
    if (!hood || hood->travel.from_house != house || house == 0)
        return;
    // Where people come in from the street: the pedestrian portal nearest the lot's middle.
    const Entity *door = nullptr;
    int best = 1 << 30;
    for (auto &e : entities)
        if (e->info && e->info->label == "Portal - Pedestrian" && on_lot(*e)) {
            const int d = std::abs(e->tile_x() - lot.size / 2) + std::abs(e->tile_y() - lot.size / 2);
            if (d < best) {
                best = d;
                door = e.get();
            }
        }
    for (auto &e : entities) {
        if (!e->person || e->dead || on_lot(*e) || !hood->travel.sim_data.count(e->person_data[31]))
            continue;
        const int16_t x = door ? door->x16 : int16_t(lot.size * 8), y = door ? door->y16 : int16_t(lot.size * 8);
        e->leave_container();
        e->set_position(x, y, 1);
    }
    hood->travel = Travel();
}

// Members of the neighbourhood's families who don't live here, left on this lot by a save made
// while they visited it on a trip (saves of visited lots aren't written any more; older ones
// are): taken off the lot, as the trip's end took them home. Townies, NPCs and the lot's own
// family stay. Returns how many went.
int Vm::remove_visiting_families()
{
    if (!hood)
        return 0;
    const Family *here = hood->family_in_house(house);
    int gone = 0;
    std::vector<int16_t> gone_ids;
    for (auto &e : entities) {
        if (!e->person || e->dead || !e->info)
            continue;
        const Family *theirs = nullptr;
        for (const Family &f : hood->families)
            if (std::find(f.members.begin(), f.members.end(), e->info->guid) != f.members.end()) {
                theirs = &f;
                break;
            }
        if (!theirs || theirs == here || theirs->house == 0)
            continue;   // a townie or NPC, our own family, or a family of the bin (no home to be away from)
        // A visitor (person data 32 set: the game runs them as one, Vm::visitor_refused) stays: a
        // neighbour who walked by or was invited is on the lot when it's saved and is there when it's
        // opened again, as in the original. Only a family that travelled here (playing it as theirs,
        // 32 clear) is sent home. Ours sent the visitors home too: Brad Burb and Liz Carper gone from
        // the Granges' (the user, 2026-11-13). TS_VISITORS_GO_HOME=1: as before.
        static const bool visitors_go = TS_ENV("TS_VISITORS_GO_HOME") != nullptr;
        if (!visitors_go && e->person_data.size() > 32 && e->person_data[32] != 0) {
            std::printf("family: %s of the %s family is visiting here: stays\n", e->info->label.c_str(), theirs->name.c_str());
            continue;
        }
        std::printf("family: %s of the %s family was left here by a trip's save: gone home\n", e->info->label.c_str(),
                    theirs->name.c_str());
        e->leave_container();
        for (Entity *s : e->slots)   // (as remove_entity: what they held is let go, their code stops)
            if (s)
                s->leave_container();
        e->set_position(-1, -1, e->level);
        e->dead = true;
        if (e->thread)
            e->thread->stack.clear();
        gone_ids.push_back(int16_t(e->id));
        gone++;
    }
    // What they were doing with the people left here goes with them. A social runs through a
    // controller of its own (Social Talk 1.4, Social Entertain 1.4, ...; out of the world) that names
    // the two Sims in its attributes (0 the other, 6 ours): left behind, it kept Lisa talking to
    // nobody, her balloon over her and her queue full of socials with someone gone (the Granges, the
    // user's report, 2026-11-13: Liz Carper's visit saved, sent home on the load). So: those
    // controllers removed, and the queued actions on them, or on the people gone, dropped (the one
    // running ends on its own next tick: its object is gone, Thread::run). TS_VISITOR_SOCIALS_OLD=1:
    // as before.
    if (gone > 0 && !TS_ENV("TS_VISITOR_SOCIALS_OLD")) {
        const std::vector<int16_t> people = gone_ids;   // (the controllers named by the people alone)
        auto names_gone = [&](const Entity &o) {
            for (int16_t a : o.attributes)
                if (a > 0 && std::find(people.begin(), people.end(), a) != people.end())
                    return true;
            return false;
        };
        for (auto &e : entities) {
            if (e->dead || e->person || !e->info || e->in_world() || e->info->label.rfind("Social ", 0) != 0 ||
                e->info->label.rfind("Social Interactions", 0) == 0 || !names_gone(*e))
                continue;   // (Social Interactions: the lasting ones, not a social's own)
            std::printf("family: the %s #%d of someone gone home removed\n", e->info->label.c_str(), int(e->id));
            gone_ids.push_back(int16_t(e->id));
            remove_now(*e);
        }
        auto went = [&](int16_t id) { return id > 0 && std::find(gone_ids.begin(), gone_ids.end(), id) != gone_ids.end(); };
        for (auto &e : entities) {
            if (e->dead || !e->person || !e->thread)
                continue;
            auto &queue = e->thread->queue;
            const size_t keep_first = e->thread->in_interaction() ? 1 : 0;   // (the running one: see above)
            for (size_t i = queue.size(); i-- > keep_first;)
                if (went(queue[i].callee) || went(queue[i].stack_id))
                    queue.erase(queue.begin() + std::ptrdiff_t(i));
        }
    }
    return gone;
}

// A family's home opened with members of theirs missing altogether (no Sim of that guid on the
// lot, in the world or out of it): they're built again from the neighbourhood's records and set
// down by the pedestrian portal nearest the lot's middle, as a trip's arrivals are. The Bobs
// (2026-11-02): a cancelled travel chooser sent them to a lot that doesn't exist, so they were
// taken off their lot and never put down anywhere; the home save had the house without them.
// Not while they're away on a trip to a real lot (they come back with it).
int Vm::restore_missing_family()
{
    const Family *fam = hood ? hood->family_in_house(house) : nullptr;
    if (!fam || house == 0)
        return 0;
    if (hood->travel.family == fam->id && hood->travel.to_house != house && hood->travel.to_house > 0 &&
        hood->houses.count(hood->travel.to_house))
        return 0;   // away for real
    std::vector<uint32_t> missing;
    for (uint32_t guid : fam->members) {
        bool here = false;
        for (const auto &e : entities)
            if (e->person && !e->dead && e->info && e->info->guid == guid) {
                here = true;
                break;
            }
        if (!here)
            missing.push_back(guid);
    }
    if (missing.empty())
        return 0;
    const Entity *door = nullptr;
    int best = 1 << 30;
    for (auto &e : entities)
        if (e->info && e->info->label == "Portal - Pedestrian" && on_lot(*e)) {
            const int d = std::abs(e->tile_x() - lot.size / 2) + std::abs(e->tile_y() - lot.size / 2);
            if (d < best) {
                best = d;
                door = e.get();
            }
        }
    int made = 0;
    for (uint32_t guid : missing) {
        const ObjectInfo *info = content.object(guid);
        if (!info)   // a Sim made in Create-A-Sim: their definition from the neighbourhood's records
            for (const MadeSim &m : hood->made)
                if (m.guid == guid) {
                    info = register_made(m);
                    break;
                }
        if (!info) {
            std::printf("family: no definition of member %08x to build\n", unsigned(guid));
            continue;
        }
        Entity *sim = create_with_parts(*info, -1, -1, 0, 0);
        if (!sim)
            continue;
        sim->run_entry(*this, 0, true);   // init: their person data from the neighbourhood's record
        if (sim->person_data.size() > 61 && sim->person_data[61] == 0)
            sim->person_data[61] = fam->id;
        const int16_t x = door ? door->x16 : int16_t(lot.size * 8), y = door ? door->y16 : int16_t(lot.size * 8);
        sim->set_position(x, y, 1);
        std::printf("family: %s was missing from their home and is back at the door\n", info->label.c_str());
        made++;
    }
    if (made && hood->travel.family == fam->id)
        hood->travel = Travel();   // (the trip that lost them is over)
    return made;
}

// Global 9, the number of the family on the lot, as the exe sets it at every lot load (sims.c
// 258581-258609: the family whose house this is, else the travelling family on its away lot, else
// 0). Ours never set it: on a lot empty in its file (the Bobs', House06) it stayed 0 after the move
// in, and the Scheduler's "final check" (global 9 against the visitor's family) ended every
// scheduled NPC and visit there: no paper, no mail, no visitors (the second audit, 2026-11-04).
void Vm::set_family_global()
{
    int16_t number = 0;
    if (hood) {
        if (const Family *fam = hood->family_in_house(house))
            number = fam->id;
        else if (hood->travel.family && hood->travel.to_house == house)
            number = hood->travel.family;
    }
    if (globals.size() > 9)
        globals[9] = number;
}

void Vm::move_in(int16_t family_id)
{
    const Family *fam = hood ? hood->family_by_id(family_id) : nullptr;
    if (!fam)
        return;
    if (globals.size() > 9)
        globals[9] = fam->id;   // (the exe's next lot load would; see set_family_global)
    // By the mailbox, a tile towards the road (FreeSims VMNetSimJoinCmd: the mailbox's position
    // + 1, + 1), as the game's: the pedestrian portals sit at the lot's far corners (the Goths':
    // (41, 50) and (41, 1)), and a family placed at one stood a long way down the pavement (the
    // user, 2026-10-30). Without a mailbox, the portal nearest the lot's middle, else the middle.
    const Entity *mailbox = nullptr;
    const Entity *door = nullptr;
    int best = 1 << 30;
    for (auto &e : entities) {
        if (!e->info || !on_lot(*e))
            continue;
        if (!mailbox && e->info->label.rfind("MailBox", 0) == 0)
            mailbox = e.get();
        if (e->info->label == "Portal - Pedestrian") {
            const int d = std::abs(e->tile_x() - lot.size / 2) + std::abs(e->tile_y() - lot.size / 2);
            if (d < best) {
                best = d;
                door = e.get();
            }
        }
    }
    const int tx = mailbox ? std::clamp(mailbox->tile_x() + 1, 0, lot.size - 1) : door ? door->tile_x() : lot.size / 2;
    const int ty = mailbox ? std::clamp(mailbox->tile_y() + 1, 0, lot.size - 1) : door ? door->tile_y() : lot.size / 2;
    std::vector<Entity *> made;
    // Side by side, stepping in towards the lot's middle: not all on one tile.
    const int sx = tx < lot.size / 2 ? 1 : -1, sy = ty < lot.size / 2 ? 1 : -1;
    for (uint32_t guid : fam->members) {
        const ObjectInfo *info = content.object(guid);
        const Neighbor *n = hood->by_guid(guid);
        if (!info || !n)
            continue;
        const int k = int(made.size());
        const int x = std::clamp(tx + sx * (k % 2), 0, lot.size - 1), y = std::clamp(ty + sy * (k / 2), 0, lot.size - 1);
        Entity *e = create(*info, x, y, 0, 4);
        if (!e)
            continue;
        for (size_t k = 0; k < n->person_data.size() && k < e->person_data.size(); k++)
            e->person_data[k] = n->person_data[k];
        e->person_data[31] = n->id;
        e->tokens = n->tokens;
        deal_interests(*e);
        made.push_back(e);
    }
    for (Entity *e : made)
        e->run_entry(*this, 0, true);   // init
    refresh_people();                   // what init reset of theirs (personality, skills, ...)
    for (Entity *e : made)
        e->run_entry(*this, 1, false);  // main, on its thread
    for (Entity *e : made)
        if (!pet_kind(*e)) {
            selected_id = e->id;
            break;
        }
}

void Vm::refresh_people()
{
    if (!hood)
        return;
    for (auto &e : entities) {
        if (!e->person || e->dead || e->person_data[31] == 0)
            continue;
        const Neighbor *n = hood->by_id(e->person_data[31]);
        if (!n || (e->info && n->guid != e->info->guid))
            continue;
        for (size_t k = 0; k < n->person_data.size() && k < e->person_data.size(); k++)
            if (k != 0 && k != 27 && k != 31 && k != 32 && k != 33 && k != 34 && k != 35 && k != 37)
                e->person_data[k] = n->person_data[k];
        if (e->tokens.empty())
            e->tokens = n->tokens;
        deal_interests(*e);
    }
}

int16_t Vm::init_category(Entity &e)
{
    if (e.entry_points.empty() || e.entry_points[0].action == 0)
        return 0;
    // Each routine with the code owner it resolves in, two calls deep.
    std::vector<std::pair<const Routine *, CodeOwner>> todo, next;
    CodeOwner owner;
    if (const Routine *r = routine(e.code, e.entry_points[0].action, &owner))
        todo.push_back({ r, owner });
    for (int depth = 0; depth < 3 && !todo.empty(); depth++) {
        next.clear();
        for (const auto &[r, own] : todo)
            for (const BhavInstruction &in : r->code) {
                const uint8_t *op = in.operand;
                if (in.opcode == 2 && op[5] == 5 && op[6] == MyObject && int16_t(op[0] | op[1] << 8) == VarCategory) {
                    const int16_t data = int16_t(op[2] | op[3] << 8);
                    if (op[7] == Literal)
                        return data;
                    if (op[7] == Tuning) {
                        Frame f;
                        f.owner = own;
                        f.caller = f.callee = &e;
                        return tuning(f, uint16_t(data));
                    }
                }
                if (in.opcode >= 256 && depth < 2) {
                    CodeOwner sub;
                    if (const Routine *c = routine(own, in.opcode, &sub))
                        next.push_back({ c, sub });
                }
            }
        todo.swap(next);
    }
    return 0;
}

void Vm::restore_saved(Entity &e, const LotObject &o)
{
    if (!o.attributes.empty()) {
        e.attributes = o.attributes;
        if (e.info && e.attributes.size() < e.info->objd.get(Objd::NumAttributes))
            e.attributes.resize(e.info->objd.get(Objd::NumAttributes), 0);
    }
    for (int i = 0; i < 68; i++) {
        switch (i) {   // kept by the VM itself, or worked out again
        case VarObjectId: case VarDirection: case VarContainerId: case VarParentId: case VarSlotNumber:
        case VarSlotCount: case VarRoom:
            continue;
        default:
            e.data[size_t(i)] = o.data[i];
        }
    }
    e.dynamic_sprites = o.dynamic_sprites;
    for (const auto &[target, values] : o.relationships)   // a Sim's to other Sims come from the neighbourhood
        if (!e.person)
            e.relationships[target] = values;
    if (e.person && o.person) {
        for (size_t m = 0; m < e.motives.size(); m++)
            e.motives[m] = int16_t(std::clamp(std::lround(o.motives[m]), -100L, 100L));
        const int16_t neighbor = e.person_data[31];
        std::copy(o.person_data.begin(), o.person_data.begin() + std::ptrdiff_t(std::min(o.person_data.size(), e.person_data.size())),
                  e.person_data.begin());
        if (e.person_data[31] == 0)
            e.person_data[31] = neighbor;
        deal_interests(e);
    }
}

void Vm::Clock::tick()
{
    if (++fraction >= kTicksPerMinute) {
        fraction = 0;
        if (++minutes >= 60) {
            minutes = 0;
            if (++hours >= 24) {
                hours = 0;
                if (++day > 30) {   // the game's months are 30 days
                    day = 1;
                    if (++month > 12) {
                        month = 1;
                        year++;
                    }
                }
            }
        }
    }
}

const CharAnimation *Vm::animation(const std::string &name)
{
    return name.empty() ? nullptr : characters->animation(name);
}

void Vm::animations_in_use(std::unordered_set<const CharAnimation *> &out) const
{
    for (const auto &e : entities) {
        if (!e)
            continue;
        for (const AnimState &a : e->animations)
            if (a.anim)
                out.insert(a.anim);
        if (e->held.anim)
            out.insert(e->held.anim);
        if (e->carry && e->carry->anim)
            out.insert(e->carry->anim);
        if (e->carry_held_off)
            out.insert(e->carry_held_off);
    }
}

void Vm::forget_animations(const std::vector<const CharAnimation *> &gone)
{
    if (gone.empty())
        return;
    const std::unordered_set<const CharAnimation *> went(gone.begin(), gone.end());
    for (auto it = animations_found.begin(); it != animations_found.end();)
        it = went.count(it->second) ? animations_found.erase(it) : std::next(it);
}

void Vm::files_in_use(std::unordered_set<const LoadedIff *> &out) const
{
    auto add = [&](const LoadedIff *f) {
        if (f)
            out.insert(f);
    };
    add(global_code.own);
    add(global_code.semi);
    for (const auto &e : entities) {
        if (!e)
            continue;
        add(e->code.own);
        add(e->code.semi);
        add(e->tree_file);
    }
    for (const auto &[key, table] : string_tables_)
        add(std::get<0>(key));
    for (const auto &[key, bcon] : constants_)
        add(key.first);
    for (const auto &[key, routine] : routines_)
        add(key.first);
    for (const auto &[key, ttab] : tables_)
        add(key.first);
}

bool Vm::animation_waits(const std::string &name)
{
    if (animation_sync || name.empty() || !request_file || !characters || characters->animation_loaded(name))
        return false;
    const std::vector<FileRef> files = characters->animation_files(name);
    if (files.empty())
        return false;
    if (requested_files.size() > 512)
        requested_files.clear();
    // Waiting only while one of its files is still to come: once all are in the reader's store,
    // animation() takes them from memory. Ours waited until the animation was taken in, which
    // nothing did before the 12 ticks ran out: every animation not played before started half a
    // second late (2026-11-07). TS_ANIM_BUNDLE_WAIT=1: as before.
    static const bool wait_old = std::getenv("TS_ANIM_BUNDLE_WAIT") != nullptr;
    bool all_in = !wait_old;
    for (const FileRef &r : files) {
        if (!wait_old && read_ahead_has(r))
            continue;
        all_in = false;
        // Asked again if asked long ago and still not in: read and then let go from the store
        // (its room, the lot's let-go) before it was played, it would otherwise wait out the 12 ticks.
        auto [it, fresh] = requested_files.try_emplace(r.path + ":" + std::to_string(r.offset), ticks);
        if (fresh || (!wait_old && ticks - it->second > 48)) {
            it->second = ticks;
            request_file(r);
        }
    }
    return !all_in;
}

Entity *Vm::create_with_parts(const ObjectInfo &info, int tile_x, int tile_y, int level, int direction)
{
    const uint16_t master = info.objd.get(Objd::MasterID);
    if (master == 0 || info.objd.get(Objd::SubIndex) != 0xffff)
        return create(info, tile_x, tile_y, level, direction);
    // The master definition names the object; its parts are the other
    // definitions in the file with its master id, each at its offset.
    std::vector<Entity *> parts;
    Entity *lead = nullptr;
    for (const auto &entry : content.objects().by_guid) {
        const ObjectInfo &o = entry.second;
        if (o.ref.path != info.ref.path || o.ref.offset != info.ref.offset || o.objd.get(Objd::MasterID) != master ||
            o.objd.get(Objd::SubIndex) == 0xffff)
            continue;
        const uint16_t sub = o.objd.get(Objd::SubIndex);
        const int ox = int8_t(sub >> 8), oy = int8_t(sub & 0xff);
        int rx = ox, ry = oy;
        switch ((direction / 2) % 4) {
        case 1: rx = -oy; ry = ox; break;
        case 2: rx = -ox; ry = -oy; break;
        case 3: rx = oy; ry = -ox; break;
        default: break;
        }
        Entity *e = create(o, tile_x < 0 ? -1 : tile_x + rx, tile_x < 0 ? -1 : tile_y + ry,
                           level + int16_t(o.objd.get(Objd::LevelOffset)), direction);
        parts.push_back(e);
        if (sub == 0 || !lead)
            lead = e;
    }
    if (parts.empty())
        return create(info, tile_x, tile_y, level, direction);
    if (parts.size() > 1)
        for (Entity *e : parts)
            e->group = parts;
    return lead;
}

void Vm::group_parts()
{
    // Each part knows its place in the object (FreeSims VMMultitileGroup): x in
    // the high byte of its SubIndex, y in the low byte, the storey in its
    // LevelOffset, all turned with the object. Parts of the same object that
    // agree on where the object's origin is are one object.
    std::map<std::tuple<std::string, uint64_t, uint16_t, int, int, int, int>, std::vector<Entity *>> groups;
    for (auto &e : entities) {
        if (!e->info || !e->in_world() || e->container)
            continue;
        const Objd &d = e->info->objd;
        const uint16_t master = d.get(Objd::MasterID);
        const uint16_t sub = d.get(Objd::SubIndex);
        if (master == 0 || sub == 0xffff)
            continue;
        const int ox = int8_t(sub >> 8), oy = int8_t(sub & 0xff);
        int rx = ox, ry = oy;
        switch ((e->direction / 2) % 4) {
        case 1: rx = -oy; ry = ox; break;
        case 2: rx = -ox; ry = -oy; break;
        case 3: rx = oy; ry = -ox; break;
        default: break;
        }
        // (Storeys aren't compared: saves put the parts of some tall windows a storey apart.)
        groups[{ e->info->ref.path, e->info->ref.offset, master, e->direction, e->tile_x() - rx, e->tile_y() - ry, 0 }]
            .push_back(e.get());
    }
    for (auto &[key, parts] : groups)
        if (parts.size() > 1)
            for (Entity *e : parts)
                e->group = parts;
}

void Vm::tick_stopped(int mode)
{
    globals[19] = int16_t(mode);
    for (size_t i = 0; i < entities.size(); i++) {   // (threads may create entities)
        Entity *e = entities[i].get();
        if (e->dead || !e->thread || e->data[60] == 0)
            continue;
        running_id = e->id;
        e->thread->tick();
    }
    running_id = 0;
    finish_removals();
}

void Vm::tick()
{
    MemScope scope(MemKind::Game);   // (the game's own allocations counted as such on the Vita: core/memory_kind.h)
    if (ticks % 300 == 0)   // (cheap: one lookup an object; logs only what's wrong)
        check_object_order("tick");
    if (!animations_ahead.empty())   // (the read-ahead's next stage asked for as the first arrives)
        animations_ahead.erase(std::remove_if(animations_ahead.begin(), animations_ahead.end(),
                                              [this](const std::string &name) { return !animation_waits(name); }),
                               animations_ahead.end());
    globals[19] = 0;   // live mode (tick_stopped sets buy's or build's)
    // Interests on 0-1000 (deal_interests) for every Sim, whichever way it came onto the lot and
    // whatever its init set (the Legacy's premades set 0-10 ones on their thread's first tick).
    for (auto &e : entities)
        if (e->person && !e->dead)
            deal_interests(*e);
    // The House tab's Layout bar (FUN_005f4f90): every 10 ticks each family member on the lot, shown and
    // awake (motive 11 not below 0), counts towards today; walking, towards today's walking too.
    if (ticks % 10 == 0)
        if (const Family *fam = family())
            for (auto &e : entities)
                if (e->person && !e->dead && on_lot(*e) && e->info && e->value(VarHidden) == 0 &&
                    std::find(fam->members.begin(), fam->members.end(), e->info->guid) != fam->members.end() &&
                    (e->motives.size() <= 11 || e->motives[11] >= 0)) {
                    layout_t[5]++;
                    if (e->route.state == Entity::Route::Walking)
                        layout_b[5]++;
                }
    if (const char *w = TS_ENV("TS_WATCH_OBJ")) {   // debug: an object's place and holder as they change
        static int16_t lx = -2, ly = -2;
        static int lc = -2, ld = -2, lf = -2;
        static std::vector<int16_t> la;
        static uint32_t lg = 0;
        if (const Entity *o = entity(int16_t(std::atoi(w)))) {
            const int c = o->container ? o->container->id : 0;
            const int fl = uint16_t(o->data[VarFlags]);
            const uint32_t g = o->info ? o->info->guid : 0;
            if (o->x16 != lx || o->y16 != ly || c != lc || int(o->dead) != ld || fl != lf || o->attributes != la || g != lg) {
                std::printf("watch tick %u: #%d %s at (%d, %d) in #%d dead %d hidden %d flags %04x attrs", ticks, int(o->id),
                            o->info ? o->info->label.c_str() : "?", o->x16, o->y16, c, int(o->dead), int(o->value(VarHidden)),
                            unsigned(fl));
                for (size_t k = 0; k < o->attributes.size() && k < 16; k++)
                    std::printf(" %d", int(o->attributes[k]));
                std::printf("\n");
            }
            lx = o->x16, ly = o->y16, lc = c, ld = int(o->dead), lf = fl, la = o->attributes, lg = g;
        }
    }
    // A balloon showing an object (group 7) goes when that object does: nothing left to draw, it
    // stayed as an empty bubble for good (Linda's, the user's 2026-11-03 report; FreeSO's headline
    // renderer ends it too).
    for (auto &e : entities)
        if (e->headline.on && e->headline.group == 7 && e->headline.icon != 0) {
            const Entity *shown = entity(e->headline.icon);
            if (!shown || shown->dead) {
                e->headline.on = false;
                if (TS_ENV("TS_HEADLINES"))   // debug
                    std::printf("tick %u headline on #%d gone with #%d\n", ticks, int(e->id), int(e->headline.icon));
            }
        }
    globals[30] = free_will ? 1 : 0;
    ticks++;
    const int day_before = clock.day;
    clock.tick();
    if (clock.day != day_before)
        new_day();
    // The selected Sim, as the game's code knows it: only a real selection (the family's, or one kept
    // while away), not selected()'s stand-in when none of the family is on the lot (whoever is there:
    // a clerk made "the player's Sim" refused every "Move out of way", its test "global 3 != me", and
    // a vacation director stayed shut in behind the shop's register for good, lot 45). Kept as it
    // was meanwhile (0 on a lot no family has been on). TS_GLOBAL3_ANY=1: as before.
    static const bool global3_any = TS_ENV("TS_GLOBAL3_ANY") != nullptr;
    if (Entity *sim = selected(); sim && (global3_any || sim->id == selected_id))
        globals[3] = int16_t(sim->id);
    globals[6] = int16_t(clock.seconds());
    globals[5] = int16_t(clock.minutes);
    globals[0] = int16_t(clock.hours);
    globals[1] = int16_t(clock.day);
    globals[7] = int16_t(clock.month);
    globals[8] = int16_t(clock.year);
    // Global 4, the time of day, as the exe's clock sets it on the hour (FUN_005576f0): 2 night (till
    // 6), 3 the morning's hour (6), 0 day (7-17), 1 the evening's (18), 2 night (19 on). Ours was the
    // lighting's turns (time_of_day: dawn 5:45-6:30 as 1, dusk 19:00-19:45 as 3), 1 and 3 the wrong way
    // round and off by up to an hour, for the game's code that reads it. (The original forces it to 0
    // while drawing a lot's thumbnail, 0x5c1a00 area; FreeSims kept it 0 always.)
    static const bool old_tod = TS_ENV("TS_OLD_TOD") != nullptr;   // debug: the lighting's
    globals[4] = int16_t(old_tod             ? time_of_day()
                         : clock.hours < 6   ? 2
                         : clock.hours < 7   ? 3
                         : clock.hours < 18  ? 0
                         : clock.hours < 19  ? 1
                                             : 2);
    if (TS_ENV("TS_TOD")) {   // debug: when it changes
        static int16_t was = -1;
        if (globals[4] != was)
            std::printf("time of day %d at %02d:%02d\n", globals[4], clock.hours, clock.minutes);
        was = globals[4];
    }
    using PClock = ts::FastClock;
    auto since = [](PClock::time_point t) { return std::chrono::duration<double, std::milli>(PClock::now() - t).count(); };
    static const bool arrivals = TS_ENV("TS_ARRIVE") != nullptr;   // debug: people coming and going
    if (arrivals) {
        static std::map<int, bool> was_in;
        for (auto &e : entities) {
            if (!e->person)
                continue;
            const bool in = e->in_world();
            auto it = was_in.find(e->id);
            if (it == was_in.end() || it->second != in) {
                const std::string where = e->thread && !e->thread->stack.empty() && e->thread->stack.back().routine
                                              ? e->thread->stack.back().routine->name
                                              : std::string("?");
                std::printf("tick %u %s #%d %s at (%.1f, %.1f) level %d (%s)\n", unsigned(ticks),
                            e->info ? e->info->name.c_str() : "?", int(e->id), in ? "appears" : "goes",
                            in ? e->x16 / 16.0 : (it == was_in.end() ? -1.0 : 0.0), in ? e->y16 / 16.0 : 0.0, int(e->level),
                            where.c_str());
            }
            was_in[e->id] = in;
        }
        // People gone from the lot altogether: where they were last seen.
        static std::map<int, std::pair<float, float>> last_at;
        std::set<int> here;
        for (auto &e : entities)
            if (e->person) {
                here.insert(e->id);
                if (e->in_world())
                    last_at[e->id] = { e->x16 / 16.0f, e->y16 / 16.0f };
            }
        for (auto it = last_at.begin(); it != last_at.end();)
            if (!here.count(it->first)) {
                std::printf("tick %u #%d removed, last at (%.1f, %.1f)\n", unsigned(ticks), it->first, it->second.first,
                            it->second.second);
                it = last_at.erase(it);
            } else {
                ++it;
            }
    }
    const bool timed = slow_tick_ms > 0;
    const PClock::time_point tick_began = timed ? PClock::now() : PClock::time_point();
    tick_choices = tick_checks = 0;
    tick_choice_ms = 0;
    route_total_plans += route_plans;   // (the last tick's, into the run's totals)
    route_total_ms += route_grid_ms + route_search_ms + route_rect_ms;
    route_plans = 0;
    route_grid_ms = route_search_ms = 0;
    route_rect_ms = 0;
    route_rect_failed = 0;
    double people_ms = 0, rooms_ms = 0;
    const IoStats io_began = timed ? io_stats() : IoStats();
    const size_t noted_began = timed ? noted_reads_count() : 0;
    std::vector<std::pair<double, int>> slowest;   // threads over a millisecond: (ms, entity)
    for (auto &e : entities) {
        if (e->data[VarLockoutCount] > 0)   // counts down a tick at a time (FreeSO TimestampLockoutCount)
            e->data[VarLockoutCount]--;
        if (e->headline.on) {   // FreeSO VMHeadlineRenderer.Update
            e->headline.anim++;
            if (e->headline.duration >= 0 && --e->headline.duration <= 0)
                e->headline.on = false;
        }
        if (e->person && !e->dead) {
            e->person_data[27]++;   // TickCounter: the main loop turns it into its autonomy countdown
            if (profile.on) {
                auto t = PClock::now();
                e->tick_animations(this);
                profile.animations_ms += since(t);
                t = PClock::now();
                e->tick_motives(*this);
                profile.motives_ms += since(t);
                t = PClock::now();
                e->tick_route(*this);
                e->tick_head(*this);
                profile.routes_ms += since(t);
            } else {
                const PClock::time_point t = timed ? PClock::now() : PClock::time_point();
                e->tick_animations(this);
                e->tick_motives(*this);
                e->tick_route(*this);
                e->tick_head(*this);
                if (timed)
                    people_ms += since(t);
            }
            static const int traced = TS_ENV("TS_SIM_TRACE") ? std::atoi(TS_ENV("TS_SIM_TRACE")) : 0;   // debug
            if (traced && traced == int(e->id)) {
                const AnimState *a = e->current_animation();
                std::printf("trace tick %u #%d at (%d, %d) level %d dir %d anim %s frame %.1f%s route %d%s\n", unsigned(ticks),
                            int(e->id), int(e->x16), int(e->y16), int(e->level), int(e->direction),
                            a && a->anim ? a->anim->name.c_str() : e->held.anim ? e->held.anim->name.c_str() : "-",
                            a ? a->frame : e->held.anim ? e->held.frame : 0.0f, a ? "" : e->held.anim ? " (held)" : " (none)",
                            int(e->route.state), e->route.turning ? " turning" : "");
            }
        }
    }
    for (size_t i = 0; i < entities.size(); i++)   // threads may create entities
        if (entities[i]->thread) {
            running_id = entities[i]->id;
            // A thread sitting out a sleep (or resting) runs no code: not clocked. The clock is a
            // system call on the Vita, and two of them for each of the pet shop's 845 objects
            // were most of a 22 ms tick that ran 500 instructions (2026-10-31; the slow-tick
            // line could attribute nothing, its own reads being the cost).
            const Thread &th = *entities[i]->thread;
            if (th.sleeping > 0 || th.resting > 0) {
                entities[i]->thread->tick();
                continue;
            }
            if (profile.on) {
                const auto t = PClock::now();
                const std::string name = entities[i]->info ? (entities[i]->info->name.empty() ? entities[i]->info->label : entities[i]->info->name) : "?";
                entities[i]->thread->tick();
                profile.object_ms[name] += since(t);
            } else if (timed) {
                const auto t = PClock::now();
                entities[i]->thread->tick();
                if (const double ms = since(t); ms >= 1.0)
                    slowest.push_back({ ms, int(entities[i]->id) });
            } else {
                entities[i]->thread->tick();
            }
        }
    running_id = 0;
    finish_removals();   // (the tick's Remove Object Instances, as the exe's)
    if (ticks % 900 == 0)
        forget_removed();
    if (ticks % 30 == 0) {
        const auto t = PClock::now();
        refresh_rooms();
        rooms_ms = since(t);
        profile.rooms_ms += rooms_ms;
    }
    if (timed) {
        const double total = since(tick_began);
        if (total >= slow_tick_ms) {
            std::sort(slowest.rbegin(), slowest.rend());
            const IoStats io = io_stats();
            std::printf("slow tick %u: %.1f ms (%llu reads, %llu KB, %llu read ahead; people's animations, motives, routes %.1f, "
                        "rooms %.1f)",
                        unsigned(ticks), total, (unsigned long long)(io.reads - io_began.reads),
                        (unsigned long long)((io.bytes - io_began.bytes) / 1024), (unsigned long long)(io.ahead - io_began.ahead),
                        people_ms, rooms_ms);
            if (tick_choices)
                std::printf("; autonomy: %u choice(s), %u check trees, %.1f ms", unsigned(tick_choices), unsigned(tick_checks),
                            tick_choice_ms);
            if (route_plans)
                std::printf("; routing: %d plan(s), grids %.1f ms, searches %.1f ms, rect %.1f ms (%d failed)", route_plans, route_grid_ms,
                            route_search_ms, route_rect_ms, route_rect_failed);
            for (const FileRef &r : noted_reads_from(noted_began)) {   // what it read from the card
                const size_t slash = r.path.find_last_of("/\\");
                std::printf("; read %s+%llu", r.path.c_str() + (slash == std::string::npos ? 0 : slash + 1),
                            (unsigned long long)r.offset);
            }
            {   // the instructions run this tick, all threads: a routine looping over the lot's objects shows here
                unsigned long long ran = 0;
                for (const auto &e : entities)
                    if (e->thread)
                        ran += (unsigned long long)std::max(0, e->thread->ticks_this_frame);
                std::printf("; %llu instructions", ran);
            }
            for (size_t k = 0; k < slowest.size() && k < 4; k++) {
                const Entity *e = entity(int16_t(slowest[k].second));
                std::printf("; #%d %s %.1f ms in", slowest[k].second,
                            e && e->info ? (e->info->name.empty() ? e->info->label.c_str() : e->info->name.c_str()) : "?",
                            slowest[k].first);
                int shown = 0;   // the routines it's in, innermost first (an empty stack: between them)
                if (e && e->thread)
                    for (size_t d = e->thread->stack.size(); d-- > 0 && shown < 3;)
                        if (const Routine *r = e->thread->stack[d].routine) {
                            std::printf("%s %s", shown ? " <" : "", r->name.c_str());
                            shown++;
                        }
                if (!shown)
                    std::printf(" no routine (main again, or choosing)");
                if (e && e->thread) {
                    std::printf(" (%d instructions", e->thread->ticks_this_frame);
                    if (!e->thread->stack.empty() && e->thread->stack.back().routine) {   // where it stands: the opcode there
                        const Frame &top = e->thread->stack.back();
                        if (top.ip < top.routine->code.size())
                            std::printf(", at %u: opcode %u", unsigned(top.ip), unsigned(top.routine->code[top.ip].opcode));
                    }
                    std::printf(")");
                }
            }
            std::printf("\n");
        }
    }
}

void Vm::build_room_map()
{
    rooms.assign(1, Room());   // room 0: none
    for (int level = 0; level < kLotLevels; level++) {
        room_maps_[level] = build_rooms(lot, level);
        const RoomMap &m = room_maps_[level];
        // Numbered as the original's (FUN_005826b0: (storey - 1) * 0x400 | its count): an upper
        // storey's rooms from 1024 on, which the game's code tells storeys by (the stairs' "Go
        // Upstairs" / "Go Downstairs", stair.iff "Use TEST": the Sim's room >= 1024). Ours ran on
        // from the ground floor's, so the stairs offered "Go Upstairs" upstairs, and a Sim sent
        // up from the top waited in the stairs' own code for the stairs it held (2026-10-30).
        room_base_[level] = uint16_t(level * 0x400);
        if (rooms.size() - 1 > room_base_[level])   // (a storey of over 1023 rooms: on from there)
            room_base_[level] = uint16_t(rooms.size() - 1);
        Room none;
        none.level = 0;
        rooms.resize(size_t(room_base_[level]) + 1, none);   // (the numbers in between: no room)
        for (size_t r = 1; r < m.outside.size(); r++) {
            Room room;
            room.level = level + 1;
            room.outside = m.outside[r];
            rooms.push_back(room);
        }
        for (int y = 0; y < lot.size; y++)
            for (int x = 0; x < lot.size; x++) {
                const uint16_t local = m.room(x, y);
                if (local == 0 || local >= m.outside.size())
                    continue;
                Room &room = rooms[room_base_[level] + local];
                room.area++;
                const uint16_t floor = lot.floor(x, y, level);
                if (floor == kFloorPool || floor == kFloorWater)
                    room.pool = true;
            }
    }
}

uint16_t Vm::room_at(int level, int x, int y) const
{
    const int l = level - 1;
    if (l < 0 || l >= kLotLevels || x < 0 || y < 0 || x >= lot.size || y >= lot.size || room_maps_[l].map.empty())
        return 0;
    const uint16_t local = room_maps_[l].room(x, y);
    return local == 0 || local >= room_maps_[l].outside.size() ? 0 : uint16_t(room_base_[l] + local);
}

bool Vm::residential() const
{
    // The exe's (FUN_00456590 -> FUN_0054bf70): the house's zone in the list read from
    // LotZoning.iff (1 residential, 2 community; house 0 residential; a house not listed is
    // neither). Downtown (21-30), vacation (40-49) and Studio Town (81-89) aren't listed; Magic
    // Town's homes, 90-92, are residential, its 93-98 community (the exe's defaults without the
    // file say the same). Ours counted every lot from 81 up as community, Magic Town's homes too.
    // Without a zoning list (no neighbourhood): the old ranges. TS_RESIDENTIAL_OLD=1: as before.
    static const bool old = TS_ENV("TS_RESIDENTIAL_OLD") != nullptr;
    if (house == 0)
        return true;
    if (!old && hood && !hood->zoning.empty()) {
        auto it = hood->zoning.find(house);
        return it != hood->zoning.end() && it->second == 0;   // (ours: 0 residential, 1 community)
    }
    if ((house >= 21 && house <= 30) || (house >= 40 && house <= 49) || house >= 81)
        return false;   // downtown, vacation, Studio Town, Magic Town
    return !hood || !hood->zoning.count(house) || hood->zoning.at(house) == 0;   // else LotZoning (Old Town lots are community)
}

bool Vm::answered_by_engine(const Routine &r, const CodeOwner &owner) const
{
    // Unleashed's Controller - Village brings the Old Town trolley with visitors whenever
    // five or fewer people are about. On a home lot the trolley's own code only waits, for
    // ever, and Bring Trolley's "one already here?" test names a GUID no installed object
    // has: trolleys pile up across the street and the school bus and car pools can't come.
    // The original can't do that, so on residential lots Bring Trolley does nothing (the
    // shuttle a Sim calls from home comes another way, Get Trolley, and still works).
    return r.id == 4135 && r.name == "Bring Trolley" && owner.object &&
           owner.object->where.find("VillageControl.iff") != std::string::npos && residential();
}

// ---------------------------------------------------------------- Community bug fixes
// The game's own bugs the community fixed with replacement .iff files (which we can't ship), done here
// in the engine, all behind one switch (community_fixes; Options > Game > Gameplay modifiers, "Community
// bug fixes"; the user, 2026-11-13), each also with its own TS_..._OLD to put the original's back.
// Read from the Legacy files and checked in ours (docs/TEST_NOTES.md, 2026-11-13 afternoon).

int Vm::fixed_answer(const Routine &r, const CodeOwner &owner, const Frame &caller)
{
    // "Plant life span" (Gameplay modifiers, the user's 2026-11-14; not the original's). A plant's main
    // waits a long while (Plants.iff: 22,000 ticks) and calls its "process", which takes 15 off its water
    // (attribute 1; at 0 dead, under 50 wilted). Twice or five times as long: only every 2nd or 5th call
    // runs, the others answered true at once; immortal: none (watering still fills it). A gift bouquet's
    // main calls "Force Wilted" on its day (GiftFlowers.iff; the days stretched in Vm::tuning): immortal,
    // never. Only from the object's own main (the cheats' and Sims' "Force ..." untouched).
    if (plant_life != 0 && caller.routine && caller.routine->name.rfind("main", 0) == 0 && owner.object) {
        static const char *const kPlants[] = { "plants.iff", "plants2.iff", "plantssuperstar.iff", "plantsspellbound.iff",
                                               "flowersoutdoor.iff", "flowerscoped.iff", "flowersunleashed.iff",
                                               "flowersspellbound.iff" };
        if (r.name == "process")
            for (const char *file : kPlants)
                if (object_file_is(*owner.object, file)) {
                    if (plant_life < 0)
                        return 1;
                    const uint16_t id = caller.caller ? caller.caller->id : 0;
                    const uint16_t n = ++plant_runs[id];
                    if (TS_ENV("TS_PLANT_LOG"))   // debug
                        std::printf("plant: #%d \"process\" call %u, %s\n", int(id), unsigned(n),
                                    n % unsigned(plant_life) == 0 ? "runs" : "skipped");
                    return n % unsigned(plant_life) == 0 ? -1 : 1;
                }
        if (plant_life < 0 && r.name == "Force Wilted" && object_file_is(*owner.object, "giftflowers.iff"))
            return 1;
    }
    // "Critters" off (Gameplay modifiers, the user's 2026-11-14; not the original's): Unleashed's
    // controller (NPC_Unleashed_Controller.iff main) makes El Bandito the raccoon (when the family's
    // trash is out) and the skunk (by the family's hygiene) by "create raccoon" (4107) and "create
    // skunk" (4101); both its branches go the same way after either, so "none made" (false) is safe.
    // One already on the lot leaves by its own code.
    if (!critters && owner.object && (r.name == "create raccoon" || r.name == "create skunk") &&
        object_file_is(*owner.object, "npc_unleashed_controller.iff"))
        return 0;
    if (!community_fixes)
        return -1;
    auto in = [](const CodeOwner &o, const char *file) {
        return o.object && o.object->where.find(file) != std::string::npos;
    };
    // The Obsessed Fan (Superstar) "Is Anyone Awake?" (4099): "Check for cop" (4102) leaves the stack
    // object 0 and, with no cop, its false branch goes to 13, "verify person in stack obj" (global 355),
    // which then always fails: on to 17 and "Leave". On a home lot he left at once, never stealing an
    // award or going through the trash (the community's "Obsessed Fan Fix"). That call answered "true"
    // carries on to 1 (the awake count), as with a cop checked. TS_FAN_AWAKE_OLD=1.
    static const bool fan_old = TS_ENV("TS_FAN_AWAKE_OLD") != nullptr;
    if (!fan_old && r.id == 355 && caller.routine && caller.ip == 13 && caller.routine->id == 4099 &&
        caller.routine->name == "Is Anyone Awake?" && in(caller.owner, "NPC_Superstar_ObsessedFan.iff"))
        return 1;
    // The burglar's "can item be stolen ( Thief )" (NPCController.iff 4115) checks flags, value and slots,
    // not that the thing is in the world: a controller with a value (the exe's midnight depreciation
    // raises a never-bought one to its limit) was the most valuable, he couldn't walk to it, "stole" it
    // where it was and left just after the warning music (the "Disappearing Thief" fix). Not in the
    // world: not stealable. TS_THIEF_STEAL_OLD=1.
    static const bool thief_old = TS_ENV("TS_THIEF_STEAL_OLD") != nullptr;
    if (!thief_old && r.id == 4115 && r.name == "can item be stolen ( Thief )" && in(owner, "NPCController.iff")) {
        const Entity *e = entity(caller.stack_id);
        if (!e || !e->in_world())
            return 0;
    }
    // Superstar's butler, "Query - Is Object a Fridge?" (NPC_Superstar_Butler.iff 4120), knows four
    // fridges by GUID; Makin' Magic's La Frigorifero by Belconi (849207AF, the Tuscan one), with every
    // tree the butler uses, came later: with only it he never cooked. TS_BUTLER_FRIDGE_OLD=1.
    static const bool butler_old = TS_ENV("TS_BUTLER_FRIDGE_OLD") != nullptr;
    if (!butler_old && r.id == 4120 && r.name == "Query - Is Object a Fridge?" && in(owner, "NPC_Superstar_Butler.iff")) {
        const Entity *e = entity(caller.stack_id);
        const ObjectInfo *info = e ? (e->master ? e->master : e->info) : nullptr;
        if (info && info->guid == 0x849207AFu)
            return 1;
    }
    // A burger's plate (Food.iff / FoodHD.iff "graphic state - single meal" 4192) steps full, half, empty
    // by the food left (attribute 0) against the starting amount (8), which nothing sets for a burger
    // (the BBQ's, Cheap Eats' counter): it stayed full to the last bite. As "Eat Regular Meal" (4196 ip
    // 8) does on its cook path, the start is taken when eating begins ("eat meal", 4186). TS_BURGER_PLATE_OLD=1.
    static const bool burger_old = TS_ENV("TS_BURGER_PLATE_OLD") != nullptr;
    if (!burger_old && r.id == 4186 && r.name == "eat meal" && (in(owner, ":Food.iff") || in(owner, ":FoodHD.iff")))
        if (Entity *e = entity(caller.stack_id); e && e->attributes.size() > 8 && e->attributes[8] == 0 && e->attributes[0] > 0)
            e->attributes[8] = e->attributes[0];
    return -1;
}

bool Vm::no_pests_from(const Routine &r, int16_t stack_id)
{
    // Flies.iff and Roaches.iff ("a fly/roach generating object?") breed pests by any object with dirt
    // (object data 39) at 600 or more, but for a list of GUIDs (floods, the base game's and House Party's
    // beds, showers and tubs) Maxis stopped adding to after House Party: the later packs' beds, showers
    // and tubs breed them, and sinks too, whose dirt only Wash Hands raises and nothing in the game
    // lowers (no Clean entry nor interaction): a sink washed at a dozen times bred them for good (the
    // user's roaches, 2026-11-13). The community's fix (Flies.iff, r/thesims1) leaves out all beds,
    // showers, tubs and sinks; here by the catalogue's own groups (OBJD function flags and subsort:
    // seating/beds 0x1/0x8, plumbing/showers and tubs 0x10/0x2, plumbing/sinks 0x10/0x4), for both.
    if (!community_fixes || (r.name != "a roach generating object?" && r.name != "a fly generating object?"))
        return false;
    const Entity *e = entity(stack_id);
    const ObjectInfo *info = e ? (e->master ? e->master : e->info) : nullptr;
    if (!info)
        return false;
    const uint16_t flags = info->objd.get(Objd::FunctionFlags), sub = info->objd.get(Objd::FunctionSubsort);
    return ((flags & 0x01) && (sub & 0x08)) || ((flags & 0x10) && (sub & 0x06));
}

bool Vm::in_pool(const Entity &e) const
{
    if (!e.in_world())
        return false;
    const uint16_t r = room_at(e.level, e.tile_x(), e.tile_y());
    if (r == 0 || r >= rooms.size() || !rooms[r].pool)
        return false;
    // In the pool's own water, not on the Water Tool's (a lake nobody swims in: a Sim there stands on
    // a pier; routing.cpp water_blocks). TS_LAKE_OLD=1: any water room.
    static const bool lake_old = TS_ENV("TS_LAKE_OLD") != nullptr;
    const int l = e.level - 1;
    if (lake_old || l < 0 || l >= kLotLevels || lot.floors[l].empty())
        return true;
    return lot.floor(e.tile_x(), e.tile_y(), l) != kFloorWater;
}

const std::vector<std::string> &Vm::moves(const Entity &e) const
{
    return in_pool(e) && e.swim_animations.size() > 3 ? e.swim_animations : e.walk_animations;
}

int Vm::time_of_day() const
{
    // 0 day, 1 dawn, 2 night, 3 dusk. Where the original sets it isn't found yet; its lighting
    // turns over dawn 5:45-6:30 and dusk 19:00-19:45 (measured on its screen, 2026-10-26).
    const int m = clock.hours * 60 + clock.minutes;
    if (m >= 5 * 60 + 45 && m < 6 * 60 + 30)
        return 1;
    if (m >= 6 * 60 + 30 && m < 19 * 60)
        return 0;
    if (m >= 19 * 60 && m < 19 * 60 + 45)
        return 3;
    return 2;
}

void Vm::refresh_rooms()
{
    if (rooms.size() <= 1)
        return;
    // The original's (its code, 0x6271e0 and 0x627420, with Global.iff FCNS 3's tuning): a room's
    // light from its windows by daylight and its lamps for its area; its score from the objects'
    // own Room Impact (as their code sets it: plants, paintings, lit lamps up; trash, puddles
    // down) for its area, its light, its size, how much of its floor is floored and how much of
    // its walls covered. Outdoors one score for all, from the objects out there and the time of day.
    constexpr uint16_t kWindow = 1 << 14, kDoor = 1 << 15;
    std::vector<int> ambient(rooms.size(), 0), daylight(rooms.size(), 0), impact(rooms.size(), 0),
        good(rooms.size(), 0), floored(rooms.size(), 0), faces(rooms.size(), 0), covered(rooms.size(), 0);
    // For the drawing (0x626590): the room's light-giving things, on or off (flag field 2 bit 11,
    // not windows and doors), and whether a human Sim is in it (0x5f9850's flag, not ghosts).
    std::vector<int> light_sources(rooms.size(), 0), humans(rooms.size(), 0);
    for (auto &up : entities) {
        Entity &e = *up;
        if (e.dead || !e.in_world())
            continue;
        const Entity *where = &e;
        while (where->container)
            where = where->container;
        const uint16_t room = room_at(where->level, where->tile_x(), where->tile_y());
        // An object whose room changed runs its "room changed" code (OBJD BHAV_RoomChange, entry
        // point 7), as the original's does: a window sets its light by it, 65 on its outside part
        // and 0 on its inside one (Windows.iff, global "is object's room outside"), so windows
        // light only the outdoors and a room by day is as its lamps make it. Ours never ran it:
        // the Goths' saved windows kept 65 inside, every room was fully lit by day (the original's
        // at 0.5 at 9:54 AM, measured 2026-10-28). TS_NO_ROOM_CHANGED: debug, as it was.
        static const bool no_room_changed = TS_ENV("TS_NO_ROOM_CHANGED") != nullptr;
        const bool changed = e.data[VarRoom] != int16_t(room) || !e.room_known;
        e.data[VarRoom] = int16_t(room);
        if (changed && !no_room_changed && !e.person && room != 0 && rooms_ready_) {
            e.room_known = true;
            e.run_entry(*this, 7, true);
        }
        if (room != 0 && e.value(VarHidden) == 0) {
            if (e.person && e.person_data.size() > 68 && e.person_data[65] < 2 && !(e.person_data[68] & 1))
                humans[room]++;
            if (!e.person && (uint16_t(e.value(VarFlagField2)) & 0x400) && e.info && e.info->objd.get(Objd::ObjectType) != 8)
                light_sources[room]++;
        }
        if (room == 0)
            continue;
        // Its room impact (0x626590, people too): indoors added to the room's sum; outdoors only a
        // bad one (below 0) is, and one of 0 or more counts as a good object. Ours added every one
        // outdoors and counted only those above 0, and left people out.
        static const bool old_outdoors = TS_ENV("TS_OLD_OUTDOORS") != nullptr;   // debug: as it was
        if (old_outdoors && e.person)
            continue;
        if (e.value(VarHidden) == 0) {
            const int room_impact = e.value(VarRoomImpact);
            if (old_outdoors || !rooms[room].outside || room_impact < 0)
                impact[room] += room_impact;
            good[room] += old_outdoors ? room_impact > 0 : room_impact >= 0;
        }
        if (e.person)
            continue;
        // Its light (0x626590): only a light source's (flag field 2 bit 11), by daylight for windows
        // and doors (their definition's type 8), else as a lamp. Ours added every window's and door's
        // light, flagged a light source or not. TS_OLD_ROOM_LIGHT: debug, as it was.
        static const bool old_light = TS_ENV("TS_OLD_ROOM_LIGHT") != nullptr;
        const int light = e.value(VarLightingContribution);
        if (old_light && light > 0) {
            if (uint16_t(e.value(VarFlagField2)) & (kWindow | kDoor))
                daylight[room] += light;
            else
                ambient[room] += light;
        } else if (!old_light && light != 0 && (uint16_t(e.value(VarFlagField2)) & 0x400)) {
            if (e.info && e.info->objd.get(Objd::ObjectType) == 8) {
                daylight[room] += light;
                if (TS_ENV("TS_ROOM_LIGHTS"))   // debug: what lights a room
                    std::printf("daylight in room %u: #%d %s at (%d, %d): %d (flags2 %x)\n", unsigned(room), int(e.id),
                                e.info->label.c_str(), e.tile_x(), e.tile_y(), light, unsigned(uint16_t(e.value(VarFlagField2))));
            }
            else
                ambient[room] += light;
        }
    }
    // Each room's floored tiles, and the faces of solid walls that look into it and how many of
    // them are covered (the faces build mode's whole-room covering would paint).
    for (int l = 0; l < kLotLevels; l++)
        for (int y = 0; y < lot.size; y++)
            for (int x = 0; x < lot.size; x++) {
                const uint16_t room = room_at(l + 1, x, y);
                if (room == 0 || room >= rooms.size())
                    continue;
                if (l == 0 && !lot.floors[0].empty() && lot.floor(x, y, 0) == kFloorPool && !TS_ENV("TS_OLD_OUTDOORS"))
                    good[room]++;   // a pool's tile counts as a good thing (0x626870)
                if (rooms[room].outside)
                    continue;
                if (!lot.floors[l].empty() && lot.floor(x, y, l) != 0)
                    floored[room]++;
                if (lot.walls[l].empty())
                    continue;
                const WallTile &w = lot.wall(x, y, l);
                auto face = [&](bool wall, uint16_t style, uint16_t pattern) {
                    if (!wall || !solid_wall_style(style ? style : 1))
                        return;
                    faces[room]++;
                    covered[room] += pattern != 0;
                };
                face(w.segments & WallTopRight, w.top_right_style, w.top_right_pattern);
                face(w.segments & WallTopLeft, w.top_left_style, w.top_left_pattern);
                if (x + 1 < kLotArraySize) {
                    const WallTile &e = lot.wall(x + 1, y, l);
                    face(e.segments & WallTopLeft, e.top_left_style, w.bottom_right_pattern);
                }
                if (y + 1 < kLotArraySize) {
                    const WallTile &s = lot.wall(x, y + 1, l);
                    face(s.segments & WallTopRight, s.top_right_style, w.bottom_left_pattern);
                }
            }
    const int tod = time_of_day();
    const float sun = tod == 0 ? 1.0f : tod == 2 ? 0.0f : 0.5f;   // 0x627b00
    float outdoors = 0;
    for (size_t r = 1; r < rooms.size(); r++) {
        Room &room = rooms[r];
        if (room.outside) {
            room.light = int(sun * 100 + 0.5f);
            room.lamps = std::min(100, ambient[r] / 30);   // (the viewer's own: outdoor lamps light the yard a little)
            outdoors += float(good[r]) * 1.0f + float(impact[r]) * 2.0f;   // good object count factor 1, impact factor 2
            continue;
        }
        const float area = float(std::max(1, room.area));
        float light = std::min(float(daylight[r]) / 100.0f / area * sun * 30.0f, sun) +   // object light contribution factor 30
                      float(ambient[r]) / 100.0f / area * 30.0f;
        light = light < 0.01f ? 0.0f : light > 0.99f ? 1.0f : light;
        room.light = int(light * 100 + 0.5f);
        // Drawn (0x6271e0's end, 0x628a90): a pool's room at 0.8, and a room with nothing that gives
        // light, on or off, at no less than 0.8 while a human Sim is in it (the Goths' bathroom at
        // night). Only the drawing: the room's light and score are as worked out. (The original
        // keeps a flag set as a human comes in and cleared as the last leaves; ours looks each time.)
        room.drawn = room.pool ? 80 : humans[r] > 0 && light_sources[r] == 0 ? std::max(room.light, 80) : room.light;
        // The viewer's drawing goes by the lamps alone, as it was fitted to the original's screen.
        const float area_scale = std::max(1.0f, float(room.area) / 100.0f);
        room.lamps = std::min(100, int(float(ambient[r]) / area_scale));
        const float clip = std::min(area, 60.0f), effective = std::clamp(area, 10.0f, 45.0f);
        const float floor_share = float(floored[r]) / area, wall_share = faces[r] ? float(covered[r]) / float(faces[r]) : 1.0f;
        const float score = float(impact[r]) * 10.0f / effective   // room impact multiplier 10, area 10-45
                            + light * 40.0f - 20.0f                 // light score -20 to 20
                            + clip / 60.0f * 60.0f - 30.0f          // area score -30 (none) to 30 (60 tiles)
                            + floor_share * 40.0f - 40.0f           // floor score -40 (bare) to 0
                            + wall_share * 40.0f - 40.0f;           // wall score -40 (bare) to 0
        room.score = int16_t(std::clamp(int(std::lround(score)), -100, 100));
    }
    // Outdoors: the outdoor rooms' sum over the outdoor object divisor 3, and the time of day's
    // bonus (daylight 0, dawn and dusk -20, night -50).
    const float bonus = tod == 0 ? 0.0f : tod == 2 ? -50.0f : -20.0f;
    const int16_t outdoor_score = int16_t(std::clamp(int(std::lround(outdoors / 3.0f + bonus)), -100, 100));
    for (size_t r = 1; r < rooms.size(); r++)
        if (rooms[r].outside)
            rooms[r].score = outdoor_score;
    if (TS_ENV("TS_ROOMS")) {   // debug: each room's light, score and where it is
        std::printf("rooms at %02d:%02d (time of day %d)\n", clock.hours, clock.minutes, tod);
        for (size_t r = 1; r < rooms.size(); r++) {
            int fx = -1, fy = -1;
            for (int y = 0; y < lot.size && fx < 0; y++)
                for (int x = 0; x < lot.size; x++)
                    if (room_at(rooms[r].level, x, y) == r) {
                        fx = x, fy = y;
                        break;
                    }
            std::printf("  room %lu level %d %s area %d light %d drawn %d score %d (impact %d, lamps %d, windows %d, floored %d, walls %d/%d, light sources %d, humans %d) first tile %d,%d\n",
                        (unsigned long)r, rooms[r].level, rooms[r].outside ? "outside" : "inside", rooms[r].area, rooms[r].light,
                        rooms[r].drawn, rooms[r].score, impact[r], ambient[r], daylight[r], floored[r], covered[r], faces[r],
                        light_sources[r], humans[r], fx, fy);
        }
    }
    // The Sims' Room need: their room's score, or outdoors what is near them.
    for (auto &up : entities) {
        Entity &sim = *up;
        if (!sim.person || sim.dead || !sim.in_world())
            continue;
        const uint16_t room = uint16_t(sim.data[VarRoom]);
        if (room == 0 || room >= rooms.size())
            continue;
        sim.motives[13] = rooms[room].score;
    }
}

void Vm::forget_removed()
{
    // Objects gone without opening build and buy again (generic call 11) no longer keep it shut.
    if (!build_buy_locks.empty()) {
        build_buy_locks.erase(std::remove_if(build_buy_locks.begin(), build_buy_locks.end(),
                                             [&](int16_t id) { const Entity *e = entity(id); return !e || e->dead; }),
                              build_buy_locks.end());
        if (build_buy_locks.empty())
            build_buy_enabled = true;
    }
    // Objects the game removed (social helpers, meals, ...) go once nothing points at them: no
    // frame runs their code or for them (or has them as its stack object), no queued action is on
    // them, no group or slot holds them, no walk goes to them. Their id is then free for a new object.
    std::vector<const Entity *> used;
    auto by_number = [&](int16_t id) -> const Entity * { return id > 0 && size_t(id) < by_id_.size() ? by_id_[size_t(id)] : nullptr; };
    for (auto &e : entities) {
        if (e->dead)
            continue;
        if (e->thread) {
            for (const Frame &f : e->thread->stack) {
                used.push_back(f.caller);
                used.push_back(f.callee);
                used.push_back(by_number(f.stack_id));
            }
            for (const QueuedAction &q : e->thread->queue) {
                used.push_back(by_number(q.callee));
                used.push_back(by_number(q.stack_id));
                used.push_back(by_number(q.icon_owner));
            }
        }
        for (const Entity *p : e->group)
            used.push_back(p);
        for (const Entity *p : e->slots)
            used.push_back(p);
        used.push_back(e->container);
        used.push_back(e->route.entry_object);
        used.push_back(by_number(e->route.target_id));
        used.push_back(by_number(e->headline.icon));
    }
    std::sort(used.begin(), used.end());
    std::vector<uint16_t> freed;
    size_t kept = 0;
    for (size_t i = 0; i < entities.size(); i++) {
        Entity *e = entities[i].get();
        if (e->dead && !std::binary_search(used.begin(), used.end(), e)) {
            if (e->id < by_id_.size() && by_id_[e->id] == e) {
                by_id_[e->id] = nullptr;
                freed.push_back(e->id);
            }
            if (auto it = object_order.find(e->list_key); it != object_order.end() && it->second == e)
                object_order.erase(it);
            continue;   // destroyed when the unique_ptr goes with the erase below
        }
        if (kept != i)
            entities[kept] = std::move(entities[i]);
        kept++;
    }
    entities.resize(kept);
    // What the engine keeps by number goes with the forgotten object, so a new one with its id
    // doesn't inherit it: the price paid (sell_value's "bought today"), other objects'
    // relationships to it (the game's own code never clears those either way).
    for (uint16_t id : freed) {
        bought.erase(id);
        for (auto &e : entities)
            e->relationships.erase(id);
        free_ids_.push_back(id);
        std::push_heap(free_ids_.begin(), free_ids_.end(), std::greater<uint16_t>());
    }
}

} // namespace ts::vm
