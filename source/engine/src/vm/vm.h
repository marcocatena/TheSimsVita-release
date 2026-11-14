/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * The SimAntics virtual machine, ported from FreeSims (SimsVille/SimsAntics:
 * VM.cs, VMContext.cs, engine/VMThread.cs, engine/VMStackFrame.cs,
 * entities/VMEntity.cs).
 */

// SimAntics runs the game: every object and Sim has a thread that executes
// its BHAV routines from the game's files, one primitive at a time, a few
// per tick. This holds the machine: entities, threads, stack frames, the
// routine cache and the primitive table.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "core/env.h"
#include "files/chunks.h"
#include "files/content.h"
#include "files/lot_chunks.h"
#include "world/lot.h"
#include "world/characters.h"
#include "world/neighborhood.h"
#include "world/rooms.h"

namespace ts::vm {

// What a primitive tells the thread to do next.
enum class Exit : uint8_t {
    GotoTrue, GotoFalse,                   // follow the instruction's true / false pointer now
    GotoTrueNextTick, GotoFalseNextTick,   // the same, but yield until the next tick
    ReturnTrue, ReturnFalse, Error,        // leave the routine
    ContinueNextTick,                      // run this instruction again next tick (waiting)
    Continue,                              // keep going (after pushing a routine)
    Interrupt,
};

// SimAntics variable scopes (FreeSims VMVariableScope).
enum Scope : uint8_t {
    MyObjectAttributes = 0, StackObjectAttributes = 1, TargetObjectAttributes = 2, MyObject = 3, StackObject = 4,
    TargetObject = 5, Global = 6, Literal = 7, Temps = 8, Parameters = 9, StackObjectID = 10, TempByTemp = 11,
    TreeAdRange = 12, StackObjectTemp = 13, MyMotives = 14, StackObjectMotives = 15, StackObjectSlot = 16,
    StackObjectMotiveByTemp = 17, MyPersonData = 18, StackObjectPersonData = 19, MySlot = 20,
    StackObjectDefinition = 21, StackObjectAttributeByParameter = 22, RoomByTemp0 = 23, NeighborInStackObject = 24,
    Local = 25, Tuning = 26, DynSpriteFlagForTempOfStackObject = 27, TreeAdPersonalityVar = 28, TreeAdMin = 29,
    MyPersonDataByTemp = 30, StackObjectPersonDataByTemp = 31, NeighborPersonData = 32, JobData = 33,
    NeighborhoodData = 34, StackObjectFunction = 35, MyTypeAttr = 36, StackObjectTypeAttr = 37, LocalByTemp = 40,
    StackObjectAttributeByTemp = 41, TempXL = 42, CityTime = 43, TSOStandardTime = 44, GameTime = 45, MyList = 46,
    StackObjectList = 47, MoneyOverHead32Bit = 48, MyLeadTileAttribute = 49, StackObjectLeadTileAttribute = 50,
    MyLeadTile = 51, StackObjectLeadTile = 52, StackObjectMasterDef = 53, FeatureEnableLevel = 54, MyAvatarID = 59,
};

// Object data fields (FreeSims VMStackObjectVariable).
enum ObjectVar : int {
    VarGraphic = 0, VarDirection = 1, VarContainerId = 2, VarSlotNumber = 3, VarAllowedHeightFlags = 4,
    VarWallAdjacencyFlags = 5, VarRouteId = 6, VarRoomImpact = 7, VarFlags = 8, VarRoutePreference = 9,
    VarRoutePenalty = 10, VarObjectId = 11, VarTargetId = 12, VarWallPlacementFlags = 13, VarSlotHierNumber = 14,
    VarRepairState = 15, VarLightSource = 16, VarWalkStyle = 17, VarSimAge = 18, VarTreeTableEntry = 20,
    VarSpeed = 22, VarLockoutCount = 25, VarParentId = 26, VarWeight = 27, VarRoom = 29, VarPrepValue = 31,
    VarHideInteraction = 50,   // set by a check tree (the global "hide menu"): pushed only, never on the menu
    VarCookValue = 32, VarSurfaceValue = 33, VarHidden = 34, VarDisposeValue = 36, VarWashDishValue = 37,
    VarEatingSurfaceValue = 38, VarDirtyLevel = 39, VarFlagField2 = 40, VarCurrentValue = 41,
    VarPlacementFlags = 42, VarMovementFlags = 43, VarLightingContribution = 51, VarPrimitiveResult = 52,
    VarPrimitiveResultId = 54, VarCategory = 59, VarServingSurfaceValue = 61, VarUseCount = 62, VarGardeningValue = 64,
    VarWashHandsValue = 65, VarSlotCount = 67,
    kObjectDataSize = 80,
};

// Entity flags in Flags (FreeSims VMEntityFlags).
enum EntityFlag : uint16_t {
    FlagHasZeroExtent = 1 << 2, FlagOccupied = 1 << 5, FlagNotifiedByIdleForInput = 1 << 6,
    FlagInteractionCanceled = 1 << 7, FlagChairFacing = 1 << 8, FlagBurning = 1 << 9, FlagHideForCutaway = 1 << 10,
};

class Vm;
class Thread;
class Entity;

// A BHAV, ready to run.
struct Routine {
    uint16_t id = 0;
    std::string name;
    uint8_t args = 0;
    uint16_t locals = 0;
    std::vector<BhavInstruction> code;
};

// Where a routine's code and resources come from: the object's own file,
// its semi-global file, and the global file (ids 4096+, 8192+, 256+).
struct CodeOwner {
    const ObjectInfo *object = nullptr;
    const LoadedIff *own = nullptr;
    const LoadedIff *semi = nullptr;
};

// A routine's locals or arguments: a few shorts, kept in the frame itself up to 16 (more on the
// heap). Every call made a frame with two heap vectors; autonomy alone makes thousands a choice.
class Shorts {
public:
    Shorts() = default;
    Shorts(const Shorts &o) { assign_from(o.data(), o.size()); }
    Shorts(Shorts &&o) noexcept { take(o); }
    Shorts &operator=(Shorts &&o) noexcept
    {
        if (this != &o)
            take(o);
        return *this;
    }
    Shorts(const std::vector<int16_t> &v) { assign_from(v.data(), v.size()); }
    Shorts &operator=(const Shorts &o)
    {
        if (this != &o)
            assign_from(o.data(), o.size());
        return *this;
    }
    Shorts &operator=(const std::vector<int16_t> &v)
    {
        assign_from(v.data(), v.size());
        return *this;
    }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    int16_t *data() { return heap_.empty() ? inline_ : heap_.data(); }
    const int16_t *data() const { return heap_.empty() ? inline_ : heap_.data(); }
    int16_t &operator[](size_t i) { return data()[i]; }
    const int16_t &operator[](size_t i) const { return data()[i]; }
    int16_t *begin() { return data(); }
    int16_t *end() { return data() + size_; }
    const int16_t *begin() const { return data(); }
    const int16_t *end() const { return data() + size_; }
    void assign(size_t n, int16_t v)
    {
        reserve(n);
        size_ = n;
        std::fill(begin(), end(), v);
    }
    void resize(size_t n, int16_t v = 0)
    {
        const size_t old = size_;
        reserve(n);
        size_ = n;
        for (size_t i = old; i < n; i++)
            data()[i] = v;
    }
    operator std::vector<int16_t>() const { return std::vector<int16_t>(begin(), end()); }

private:
    static constexpr size_t kInline = 16;
    void reserve(size_t n)
    {
        if (n <= kInline || n <= heap_.size())
            return;
        std::vector<int16_t> more(n, 0);
        std::copy(begin(), end(), more.begin());
        heap_ = std::move(more);
    }
    // A move: the heap's block taken as is, the inline ones copied (they're no more than kInline).
    void take(Shorts &o) noexcept
    {
        size_ = o.size_;
        if (!o.heap_.empty()) {
            heap_ = std::move(o.heap_);
        } else {
            heap_.clear();
            std::copy(o.inline_, o.inline_ + size_, inline_);
        }
        o.heap_.clear();
        o.size_ = 0;
    }
    void assign_from(const int16_t *p, size_t n)
    {
        if (n > kInline && heap_.size() < n)
            heap_.assign(n, 0);
        size_ = n;
        std::copy(p, p + n, data());
    }
    int16_t inline_[kInline] = {};
    std::vector<int16_t> heap_;   // in use once more than kInline are needed (its size is the room)
    size_t size_ = 0;
};

struct Frame {
    const Routine *routine = nullptr;
    uint16_t ip = 0;
    Entity *caller = nullptr;   // who runs this (the thread's owner)
    Entity *callee = nullptr;   // whose code it is
    // The stack object: an object id, or in some loops a neighbour id.
    int16_t stack_id = 0;
    CodeOwner owner;
    Shorts locals;
    Shorts args;
    bool action_tree = false;   // part of an interaction rather than main
    bool discard_result = false;
    // A "run immediately" (or nested) interaction: the Sim's running priority (person data 33)
    // before it started, put back when it ends; -1 none (not saved: after a load, as before).
    int16_t saved_running = -1;
    bool resume_caller = false;  // when it returns, the caller's instruction runs again (the router's portals)
    bool pushed = false;   // its Idle For Input started a waiting interaction (the original's frame + 8; not saved)
    Thread *thread = nullptr;

    const BhavInstruction &instruction() const { return routine->code[ip]; }
    Vm &vm() const;
    Entity *stack_object() const;
};

// Interactions (FreeSims VMQueuedAction): what a Sim has been asked to do,
// by the player, by autonomy or by other code, in priority order.
enum class QueueMode : uint8_t { Normal, ParentIdle, ParentExit, Idle };

enum QueuePriority : int16_t {
    PriorityIdle = 0, PriorityAutonomous = 2, PriorityParentExit = 24, PriorityParentIdle = 25,
    PriorityUser = 50, PriorityMaximum = 100,
};

// TTAB interaction flags (FreeSims TTABFlags, the Sims 1 meanings).
enum TtabFlag : uint32_t {
    // (Bit 5, FreeSims' "no demo child": the exe's start test reads it as "visitors may, on a community
    // lot", FUN_0060b110; see Vm::visitor_refused.)
    TtabAllowVisitors = 1u << 0, TtabRunImmediately = 1u << 2, TtabNoChild = 1u << 4, TtabNoDemoChild = 1u << 5,
    // Unleashed's pets: an interaction they may use has their bit (people use it all the same).
    TtabNoAdult = 1u << 6, TtabDebug = 1u << 7, TtabAllowCats = 1u << 9, TtabAllowDogs = 1u << 10,
};

struct QueuedAction {
    const Routine *action = nullptr, *check = nullptr;
    CodeOwner owner;
    int16_t callee = 0, stack_id = 0;
    // Whose picture the queue shows, when not the callee's (Push Interaction's custom icon: the
    // ghost for "Spooked", pushed by a controller with no picture of its own); 0 the callee.
    int16_t icon_owner = 0;
    std::string name;
    std::vector<int16_t> args;
    int interaction = -1;   // the number in the callee's tree table
    bool cancelled = false;
    int16_t priority = PriorityIdle;
    // A "run immediately" one's own priority (the player's 50 for an order), which `priority` gives
    // up to sort it first: the Sim's running priority while it runs, as the original's (not saved).
    int16_t ordered_priority = PriorityIdle;
    QueueMode mode = QueueMode::Normal;
    uint32_t flags = 0;
    bool must_run = false;   // pushed by code: no check tree
    // Chosen by free will: its check passed then (with param 0 = 1, as autonomy asks) and the exe
    // starts it without another (FUN_005f7570); ours re-checked it as the player's (param 0 = 0)
    // and dropped what refused that, Nap and Sit while seated among them (2026-11-04 audit).
    bool found = false;
    bool head = false;       // pushed as a head continuation (Push Interaction flag 4): first, and always input
    uint16_t uid = 0;
};

// One person-or-object's flow of control.
class Thread {
public:
    Thread(Vm &vm, Entity &entity) : vm_(vm), entity_(entity) {}

    void tick();
    void push(Frame frame);
    // Calls routine `bhav` from `frame` with arguments (-1: take the temp register).
    void call(Frame &frame, const Routine *bhav, const CodeOwner &owner, const int16_t args[4]);
    // Runs `frame` to completion now (check trees, entry points run "immediately").
    // With keep_temps the temps it leaves stay (Run Tree By Name, as FreeSO's RunInMyStack: the
    // bar's "CT - Get my Bartender in Temp 0" answers in temp 0); check trees put them back.
    Exit run_now(Frame frame, int max_steps = 20000, bool keep_temps = false);

    // Adds an interaction where its priority puts it.
    void enqueue(QueuedAction action);
    // Cancels an interaction: dropped if waiting, told to stop if running.
    void cancel(uint16_t uid);
    // Starts the first queued interaction whose check tree passes (Idle For Input "allow push").
    bool attempt_push();
    // Inside an interaction: starts the first waiting one on top of it, as Idle For Input does when
    // pushes are allowed (the original's FUN_005f8de0); when it ends, the one below carries on.
    // False when there's none or its check says no.
    // The first waiting interaction started on top of the running one: 1 started, 0 nothing
    // waiting to start, -1 the first couldn't start (dropped, its Queue Skipped run).
    int nest_waiting();
    // The waiting entries (after the running ones) a new one of `priority` would stay behind: those
    // of it or more (the lower ones a new entry drops, enqueue), run-immediately ones not counted.
    int waiting_at_least(int16_t priority) const;
    // Runs an interaction's check tree for this thread's entity. With `ads`,
    // also what the check tree changed in the interaction's advertisements
    // (`ia`'s TTAB entries are what it reads before it changes them).
    bool check(const QueuedAction &action, std::map<int, int16_t> *ads = nullptr, const TtabInteraction *ia = nullptr);
    // Advertisement changes a running check tree makes (the TreeAd scopes,
    // FreeSO MotiveAdChanges), keyed (kind << 16) | motive: kind 0 min, 1 range, 2 personality.
    std::map<int, int16_t> ad_changes;
    const TtabInteraction *ad_source = nullptr;
    bool in_interaction() const { return !stack.empty() && stack.back().action_tree; }
    // Whether something waits that the running interaction should give way to (the original's
    // FUN_005f4eb0): not while person data 71 (non-interruptible) is set; the first waiting
    // action outranks the running one (person data 33) or is a head continuation; or the Sim
    // is starving, bursting or (not a pet) collapsing (hunger, bladder or energy under -99).
    // What Idle For Input, walks and interruptible animations ask; the interaction's own code
    // then ends itself. (Ours set "interaction cancelled" on every important push, FreeSO's way.)
    bool input_waiting() const;

    std::vector<Frame> stack;
    // Stacks kept for run_now (a check tree run beside the thread's own stack), their room
    // kept: autonomy runs some two hundred checks a choice, each had its stack made afresh.
    std::vector<std::vector<Frame>> spare_stacks;
    std::vector<QueuedAction> queue;   // [0] runs when an interaction is on the stack
    bool interrupt = false;            // Notify Out Of Idle
    uint16_t next_uid = 1;
    std::array<int16_t, 20> temps{};
    std::array<int32_t, 2> temp_xl{};
    uint32_t dialog_waiting = 0;   // the dialog this thread waits on
    Exit last_exit = Exit::GotoFalse;
    bool is_check = false;
    // While a pie menu's check tree runs: the entries it names (Change Action String), with
    // the stack object each is about (FreeSO VMThread.ActionStrings).
    std::vector<std::pair<std::string, int16_t>> *action_strings = nullptr;
    int ticks_this_frame = 0;
    int64_t long_run = 0;     // instructions of a run that has gone on over several ticks
    int resting = 0;   // ticks left before a thread that looped for ever runs again
    // Ticks left of a Sleep (primitive 0) the thread sits out without running its code: the count
    // moved here from the sleep's argument, so a sleeping object costs its tick almost nothing
    // (the pet shop's 900 sleeping objects were ~13 ms of every 20 ms tick on the Vita, 2026-10-31).
    // Given back to the sleep's argument if something is pushed on top of it (the sleep then
    // counts on in its own way when reached again); dropped with the frame if the stack goes.
    int sleeping = 0;
    size_t sleeping_depth = 0;   // the stack's size when the sleep set it
    int sleeping_arg = 0;        // the sleep's argument index
    Entity &entity() { return entity_; }
    Vm &vm_ref() { return vm_; }

private:
    void step();
    void execute(Frame &frame);
    void handle(Frame &frame, Exit result);
    void move_to(Frame &frame, uint8_t target, bool keep_going);
    void pop(Exit result);
    // What the engine resets as a Sim's interaction ends (the original's FUN_005f4dc0).
    void interaction_ended();
    void run_immediately();
    void sort_queue();

    Vm &vm_;
    Entity &entity_;
    bool keep_going_ = false;
};

// The walk's animation frames a tick. FreeSO plays it at 1.5; the Legacy Collection's walk,
// timed on its 24 ticks a second, does a leg cycle in 18.2 ticks where 1.5 took 25.8: 2.1.
constexpr float kWalkAnimSpeed = 2.1f;

// The game's ticks a second at its first speed, rounded: a tick every 42 ms (23.8 a second; the
// viewer's loop uses the 42). Here for the sound recorder's ticks.
constexpr int kTicksPerSecond = 24;

// An animation a Sim is playing (FreeSims VMAnimationState); one frame a tick.
struct AnimState {
    const CharAnimation *anim = nullptr;
    float frame = 0;
    float speed = 1;   // frames a tick (the walk: kWalkAnimSpeed)
    bool backwards = false, loop = false, end_reached = false;
    bool background = false;   // started on the game's background channel (Animate's mode 1)
    size_t next_event = 0;      // into anim->events (forwards), or one past (backwards)
    uint8_t events_run = 0;
    std::deque<int16_t> event_queue;   // "xevt" values for the code waiting on it
    // The code is in Animate Sim, waiting on this: events while it is off handling one
    // (a stereo's 25-tick pause) are counted but not delivered.
    bool waiting = true;

    AnimState() = default;
    AnimState(const CharAnimation *a, bool back);
    float frames() const;   // length in frames (33.3 ms each)
};

// An object or person on the lot.
class Entity {
public:
    uint16_t id = 0;
    // Its number for life: ids are the game's, and a removed object's goes to a new one once nothing
    // holds it (Vm::create, as the original reuses the lowest free); what caches an object by number
    // across time (the viewer's looks) keys by this instead.
    uint32_t serial = 0;
    const ObjectInfo *info = nullptr;
    CodeOwner code;
    bool person = false;
    std::array<int16_t, kObjectDataSize> data{};
    std::vector<int16_t> attributes;
    std::vector<ObjFunction> entry_points;   // OBJf or generated from OBJD
    // Position in sixteenths of a tile (tile x * 16 + 8 is the middle); level 1 = ground.
    int16_t x16 = -1, y16 = -1;
    int8_t level = 1;
    // What shows over it (Set Balloon/Headline, FreeSO VMRuntimeHeadline): a picture of
    // GameData/Sprites.iff by group and index (a thought, the skill meter, "Can't Route",
    // ...), in a balloon of `balloon` (255 or 3: none). `duration` ticks, -1 until replaced.
    struct Headline {
        bool on = false;
        uint8_t group = 0, balloon = 255, flags = 0;
        int16_t index = 0;
        int duration = 0, anim = 0;
        int16_t icon = 0;   // group 7 (an object's own picture): that object
        bool crossed() const { return flags & 2; }
    } headline;
    int direction = 0;   // 0-7, north = 0
    // Where a walking Sim's body faces, in eighths (0-8): it turns towards the way it walks
    // at most 0.25 radians a tick (FreeSO VMRoutingFrame), not at once. Below 0: `direction`.
    float heading = -1;
    float shown_direction() const { return heading >= 0 ? heading : float(direction); }
    bool carry_off = false;   // the game's code stopped the carry pose: no default one till the hand's next filled
    // The animation a mode-3 Animate ("stop the carry, play and wait") started: while it plays, no
    // default carry over it (the exe's per-frame update, 0x5d34a4, holds the standard carry off
    // while that flag is set; it clears when the animation ends).
    const CharAnimation *carry_held_off = nullptr;
    // The exe's lists (FUN_0052bd20): the object list, new objects at its head and loaded ones at
    // its tail (list_key ascending is that order); the person and per-type lists, appended
    // (seq, creation order). Set To Next walks them (FUN_0061c000), not the ids.
    int64_t list_key = 0;
    uint64_t seq = 0;
    uint32_t snapped_at = 0;   // the tick a Snap last moved this Sim (into a seat, out of one, aside)
    Entity *container = nullptr;
    int16_t container_slot = -1;
    std::vector<Entity *> slots;   // what this holds, by slot
    std::deque<int16_t> list;      // MyList
    uint64_t dynamic_sprites = 0;  // which of its DGRP's dynamic sprites show (a lit stove, a TV picture)
    std::unique_ptr<Thread> thread;
    bool dead = false;
    bool room_known = false;       // its "room changed" code has run for its room (Vm::refresh_rooms)
    // People.
    std::vector<AnimState> animations;     // [0] is the current one
    // The last frame of the animation that last ended, shown while none plays: the original
    // holds the pose (a Sim sat on the toilet or soaking in the bath between its animations,
    // the tick between two sitting idles). Not saved: a loaded Sim idles in its posture.
    AnimState held;
    std::unique_ptr<AnimState> carry;      // arm pose while carrying
    // Suits put on over the outfit by objects' code (Change Suit, object scope: the book being
    // read, "suit-book101" from the bookcase's STR# 304), drawn with the Sim.
    std::vector<std::string> accessories;
    std::string custom_name;   // given by the player (a baby's, a pet's: the text-entry dialog), used for $Object
    // The inventory of tokens the code keeps on a Sim (Token Operations, opcode 51): a kind
    // (0 gift, 1/3 date, 2 sim data, 4 purchase, 5 souvenir, 6 skill, 7 magic, 8 ingredient)
    // and an object's GUID. A pet shop's receipt, Makin' Magic's ingredients and charms.
    using Token = ts::Token;
    std::vector<Token> tokens;
    std::vector<std::string> walk_animations;   // by posture (global STR# 150, the Sim's own STR# 150)
    std::vector<std::string> swim_animations;   // the same in a pool (global STR# 158, children 160; FreeSO)
    // Motives change at a rate per game hour up to a limit (Set Motive Change): a list, as the
    // original keeps it (its handler appends an entry {motive, rate a tick, max}; two changes to
    // one motive add up; "clear all" empties the list).
    struct MotiveChange {
        int8_t motive = 0;
        int16_t per_hour = 0, max = 32767;
        double fraction = 0;
    };
    std::vector<MotiveChange> motive_changes;
    std::array<int16_t, 7> decay_fractions{};   // the game's own motive decay, in 1/1000ths
    // The seven decaying needs as floats, as the exe keeps them (tick_motives), and the whole value
    // each was last shown as: a need changed since by anything else is taken up afresh.
    std::array<float, 7> exact_need{};
    std::array<int16_t, 7> exact_shown{};
    bool exact_ready = false;   // (from the save's thousandths, the first decay after a load)
    void sync_exact_needs();
    void show_exact_need(int i);
    void decay_motives(Vm &vm);   // the steady decay and the mood, when their time comes (tick_motives)
    int16_t decay_hunger = -32768;   // Hunger as the last decay left it (what's eaten since fills the bladder)
    int decay_minute = -1;
    // Walking somewhere (Go To Routing Slot, Go To Relative Position).
    struct Route {
        enum State : uint8_t { Idle, Walking, Arrived, Failed } state = Idle;
        const Routine *routine = nullptr;   // the instruction that asked, to tell a stale route
        uint16_t ip = 0;
        size_t depth = 0;
        std::vector<std::pair<int16_t, int16_t>> path;   // waypoints, in sixteenths of a tile
        size_t next = 0;
        // Where the walker is to a fraction of a sixteenth (x16, y16 are it rounded): a step
        // is under one, so rounding each tick would lose it (or, on a diagonal, gain it).
        float fx = 0, fy = 0;
        int face = -1;   // direction to turn to on arrival, -1 keep
        // A walk onto an object's own spot (a seat): on arrival, which side of
        // it the Sim came in from goes in person data 37 (1 front, 4 right, 16 back, 64 left).
        const Entity *entry_object = nullptr;
        int16_t from_x = 0, from_y = 0;
        // What the walk goes to use (its parts don't block a way planned again).
        int16_t target_id = 0;
        // Stairs taken for one instruction (a guard against going up and down for ever).
        int portals = 0;
        // A turn on the spot under way (the walk table's standing turns, FreeSO VMRoutingFrame
        // BEGIN_TURN / END_TURN): 1 before walking off, 2 on arriving; to face `turn_to` after.
        int turning = 0, turn_to = -1;
        // The walk's pace (FreeSO VMRoutingFrame): 1 to 8 tenths of a sixteenth of a tile a
        // tick, up by one a tick when setting off, down again over the last stretch.
        int velocity = 0;
        // The walker's pace against an adult's: its walk loop's stride a second over the adult
        // loop's (a2o-walking-loop, 4.07 units in 1266 ms). The original moves each walker by its
        // own animation (measured 2026-11-04: a stray dog crossed the Goths' lawn at 0.42 of
        // Mortimer's pace, its d2o-walking-loop's stride a second 0.44 of his); ours moved every
        // walker at the adult's, the skunk's and children's feet sliding.
        float stride_scale = 1.0f;
        float walk_frames = 0;   // animation frames walked since setting off (kWalkAnimSpeed a tick), for walk-half
        // The exe's walker (FUN_005d33b0): the start of the segment walked (the last point passed),
        // and the look-ahead radius, a quarter of the walk loop's stride in sixteenths (an adult's
        // 5.4: the point the Sim heads for, a third of a tile ahead on the path). Not saved: set
        // again from where the Sim stands.
        float seg_x = 0, seg_y = 0;
        bool seg_set = false;
        bool to_corner = false;   // walking back to the corner passed (seg), a cut corner blocked by a Sim (walk_step)
        bool corner_used = false;   // ... done once this plan (not again: the usual blocked walk then)
        float look_radius = 0;
        // A door's portal routine is under way from the walk at this stack depth: walks deeper
        // than it are the door's own (to it, through it) and open no doors themselves.
        size_t door_depth = 0;
        const Routine *portal_routine = nullptr;
        uint16_t portal_ip = 0;
        size_t portal_depth = 0;
        int16_t portal_door = 0;   // the door those tries were at (each door on a walk gets its own)
        // Doors whose portal code failed this walk instruction: left out of its plans from then on, as
        // the exe's room route marks a failed portal shut (FUN_006321d0: 1101 in the Sim's route
        // map) and takes another; with none left the walk fails (no room route, 2).
        const Routine *shut_routine = nullptr;
        uint16_t shut_ip = 0;
        size_t shut_depth = 0;
        std::vector<int16_t> shut_doors;
        // Asked to step aside because they'd open a way when there was none (walk_to), and when.
        int16_t opening_asked = 0;
        uint32_t opening_start = 0;
        // The seat's stand function was run for this walk instruction (once a walk).
        const Routine *stand_routine = nullptr;
        uint16_t stand_ip = 0;
        size_t stand_depth = 0;
        // A slot's goal was a seat: its sit function was run for this walk instruction.
        const Routine *sit_routine = nullptr;
        uint16_t sit_ip = 0;
        size_t sit_depth = 0;
        const Entity *sit_seat = nullptr;
        // A walk failed and the game's route failure feedback (global 398) runs for this instruction.
        const Routine *fail_routine = nullptr;
        uint16_t fail_ip = 0;
        size_t fail_depth = 0;
        // How the walk was planned (TS_OVERLAP reports it): 0 plainly, 1 across the object it goes to,
        // 2 boxed in, through objects; and from which tile.
        mutable uint8_t plan_kind = 0;
        mutable int16_t plan_x = 0, plan_y = 0;
        int last_failure = 0;   // why the last walk ended without arriving (FreeSO VMRouteFailCode; 0 none)
        bool no_spot_waited = false;   // a route with no spot to go to: its tick taken, it fails now
        // Another Sim in the way of the walk's next step (the original's walk update, FUN_005d33b0,
        // tests each step against other Sims' 6x6 sixteenths): who, and what's been done about it
        // (FUN_005f7950 states 6 and 0xe): 0 nothing, 1 planned again here, 2 asked them to move
        // and waiting, 3 planned again after asking (blocked again here: the walk fails).
        int16_t blocked_by = 0;
        int block_stage = 0;
        int rewaits = 0;   // waits again for the one asked, still stepping aside, before failing (walk_blocked)
        int16_t block_x = 0, block_y = 0;   // where the last block was (a new place starts afresh)
        uint32_t ask_start = 0;             // when the asking began (up to 100 ticks of waiting)
        uint32_t yield_start = 0;           // two walkers blocking each other: when this one began to give way
        int16_t asked = 0;                  // the Sim asked to step aside
        // A Sim on the spot a routing slot walks to, asked to step off it and waited for.
        int16_t spot_asked = 0;
        uint32_t spot_ask_start = 0;
        uint32_t waited_at = UINT32_MAX;    // the tick the route waiting animation last ran (once a tick)
        bool paused = false;                // standing still while asking and waiting
        // A routing slot's spots the walk failed to reach for this instruction (a door that won't
        // open, no path): the next try picks another, as the original's router takes its goals in
        // turn (FreeSO VMRoutingFrame's Choices). Up to four, then the failure is the Sim's.
        // Kept per instruction (a door's own code runs a goto of its own inside the walk).
        struct SpotTries {
            const Routine *routine = nullptr;
            uint16_t ip = 0;
            size_t depth = 0;
            int16_t goal_x = 0, goal_y = 0;   // the spot its walk under way goes to
            std::vector<std::pair<int16_t, int16_t>> failed;
        };
        std::vector<SpotTries> spot_tries;
        SpotTries &spot_tries_of(const Routine *routine, uint16_t ip, size_t depth)
        {
            for (SpotTries &t : spot_tries)
                if (t.routine == routine && t.ip == ip && t.depth == depth)
                    return t;
            if (spot_tries.size() >= 8)
                spot_tries.erase(spot_tries.begin());
            spot_tries.push_back({ routine, ip, depth, 0, 0, {} });
            return spot_tries.back();
        }
        void spot_tries_done(const Routine *routine, uint16_t ip, size_t depth)
        {
            for (size_t i = 0; i < spot_tries.size(); i++)
                if (spot_tries[i].routine == routine && spot_tries[i].ip == ip && spot_tries[i].depth == depth) {
                    spot_tries.erase(spot_tries.begin() + long(i));
                    return;
                }
        }
    } route;
    // A Destination made to send a Sim out of another's way (the original's FUN_005f3830): the
    // spot to step to, used instead of its routing slot 0 by its "Move out of way".
    std::shared_ptr<SlotItem> slot_override;
    // Routing slots (type 3) and container slots of this object's SLOT chunk.
    const std::vector<SlotItem> &slot_items(Vm &vm);
    std::vector<SlotItem> slot_items_;
    bool slots_loaded_ = false;
    // Relationships, from this to another object (by id): [0] daily, [1] lifetime, ...
    std::map<uint16_t, std::vector<int16_t>> relationships;
    int left_hand = 0, right_hand = 0;     // gestures from animation events
    std::array<int16_t, 16> motives{};
    std::vector<int16_t> person_data = std::vector<int16_t>(256, 0);

    bool in_world() const { return x16 >= 0; }
    int tile_x() const { return x16 / 16; }
    int tile_y() const { return y16 / 16; }
    int16_t value(int var) const;
    void values(int16_t *out, int count) const;   // value(0 .. count - 1) at once
    void set_value(int var, int16_t v);
    int16_t attribute(int i) const { return i >= 0 && size_t(i) < attributes.size() ? attributes[size_t(i)] : 0; }
    void set_attribute(int i, int16_t v);
    bool flag(uint16_t f) const { return (uint16_t(data[VarFlags]) & f) != 0; }
    void set_flag(uint16_t f, bool on);
    // The interactions this object offers (its TTAB, or its semi-global's).
    const Ttab *tree_table = nullptr;
    const LoadedIff *tree_file = nullptr;   // where tree_table and its TTAs strings are
    uint16_t tree_table_id = 0;
    const ObjectInfo *master = nullptr;     // a multi-tile object's shared definition (its name, its table)
    std::vector<Entity *> group;            // all the parts of a multi-tile object (empty for one tile)

    AnimState *current_animation() { return animations.empty() ? nullptr : &animations[0]; }
    // Interaction `number` of this object (by TTA index), for a queue.
    bool action(Vm &vm, int number, int16_t stack_id, QueuedAction &out);
    // A routine of this object's own or semi-global file by its label (Run Tree By Name).
    const Routine *tree_by_name(Vm &vm, const std::string &name, CodeOwner &owner);
    void tick_motives(Vm &vm);
    void tick_route(Vm &vm);
    // The exe's walk step (tick_route's, unless TS_WALK_OLD): towards the look-ahead point.
    void walk_step(Vm &vm);
    // The point a walker heads for (walk_aim): on the path, look_radius ahead of where it stands.
    bool walk_aim(float px, float py, float &lx, float &ly);
    // The head turned towards what the game's code looks at (person data 41 the object, 42 the
    // state: 1 looking, 4 turning back, 8 done; 45 a timeout in ticks): radians off the body's
    // facing, + to the Sim's left as the renderer turns it.
    float head_yaw = 0;
    uint8_t anim_waits = 0;   // ticks waited for an animation's files (Vm::animation_waits)
    bool guest = false;       // brought to a community lot for atmosphere (the Sim density setting counts these)
    void tick_head(Vm &vm);
    // Plays the walk table's standing turn from this person's facing to `to` (0-7), if it's 45
    // degrees or more away; `why` is Route::turning's 1 or 2. False if no turn is needed.
    bool turn_towards(Vm &vm, int to, int why);
    // The walk's animations: the loop (walk table 20) at kWalkAnimSpeed.
    void start_walk(Vm &vm);
    // Puts `obj` in this object's slot (taking it from wherever it was).
    void place_in_slot(Entity *obj, int slot);
    // Takes this object out of its container, if any.
    void leave_container();
    // Places this object on a tile (sixteenths) and level, out of any container.
    void set_position(int16_t x16, int16_t y16, int8_t level);
    // The same for a multi-tile object: every part moves, each at its offset from this one.
    void place_whole(int16_t x16, int16_t y16, int8_t level, int direction);
    // Advances the animations one tick, queueing their events.
    // Steps the animations; their "sound" events (footsteps, ...) go to `vm`'s sounds.
    void tick_animations(Vm *vm = nullptr);
    // Runs entry point `entry` (0 init, 1 main, ...): now (to completion) or queued on the thread.
    bool run_entry(Vm &vm, int entry, bool immediately, int16_t stack_id = 0, const int16_t *args = nullptr);
};

// A part's offset (tiles) in its multi-tile object, turned to `direction`.
void part_offset(const Entity &e, int direction, int &dx, int &dy);

// Buy mode's catalogue (buy.cpp): what the family can buy, as the game's
// own catalogue has it (FreeSO TS1ObjectProvider): objects with a buy
// category, not disabled, with art, one entry per multi-tile object.
struct CatalogItem {
    const ObjectInfo *info = nullptr;
    std::string name;
    int price = 0;
    int category = 0;        // the bit of OBJD FunctionFlags: 0 seating, 1 surfaces, ...
    uint16_t subsort = 0;    // OBJD FunctionSubsort: its sub-category on home lots
    uint16_t room_flags = 0; // OBJD RoomFlags: 0, not sold on home lots (the newsstand, the clothes racks)
    // Its sub-category on the other lots, 0 not sold there: downtown, vacation, Old Town's
    // community lots, Studio Town, Magic Town (OBJD DTSubsort ... MTSubsort).
    uint16_t area_sort[5] = {};
};
// Which catalogue a lot's buy mode shows, as the original picks it (0x475960's modes 1-6):
// -1 home (RoomFlags set), else the index into CatalogItem::area_sort.
int catalog_area(const Vm &vm);
std::vector<CatalogItem> build_catalog(const Content &content);
const char *catalog_category_name(int category);
constexpr int kCatalogCategories = 8;
// The categories in the order the game's catalogue shows them (by FunctionFlags bit).
constexpr int kCatalogOrder[kCatalogCategories] = { 0, 1, 5, 3, 2, 4, 7, 6 };
// The game's own names (UIText.iff, so in its language): the category's, its sub-categories
// by FunctionSubsort bit (0-3, empty where it has none), and Other / All / Pets / Magic.
struct CatalogNames {
    std::string category;
    std::string subsort[4];
    std::string other = "Other", all = "All", pets = "Pets", magic = "Magic";
};
CatalogNames catalog_names(Content &content, int category);
// The sub-category an item goes in: its FunctionSubsort bit 0-3, 5 pets, 6 magic, else -1 (other).
int catalog_subsort(uint16_t subsort);

// Build mode's catalogue (build.cpp): wall tools (walls and fences),
// floors, wall coverings (the game's own and .flr/.wll files), and the
// objects with a build category (doors, windows, stairs, ...).
struct BuildItem {
    // Terrain: the game's four landscaping tools, `pattern` which (kTerrain... below).
    // Roof: the roof tool's choices, applied to the house as they're picked (as the original's):
    // a pitch (category 0, `pattern` 0 flat .. 3 steep, kRoofPitches) or a pattern (category 1,
    // `file` the texture's name in GameData/Roofs, without ".bmp").
    enum Kind { WallStyle, Floor, Wallpaper, Object, Terrain, Roof } kind = Floor;
    static constexpr float kRoofPitches[4] = { 0.0f, 0.4f, 0.6f, 1.0f };   // as the original saves them
    // kTerrainSand: ours, the grass tool's Control-click (grass taken away, the sand under it) as a
    // tool of its own (the user, 2026-11-13: the Vita has no Control key to find it by).
    enum { kTerrainRaise = 0, kTerrainLower = 1, kTerrainLevel = 2, kTerrainGrass = 3, kTerrainSand = 4 };
    static bool paints_ground(uint16_t pattern) { return pattern == kTerrainGrass || pattern == kTerrainSand; }
    std::string name, description;
    int price = 0;
    uint16_t pattern = 0;               // wall tool: style id; floor/wall covering: global id
    std::string file;                   // a covering in its own .flr/.wll file
    const ObjectInfo *info = nullptr;   // an object
    int category = 0;                   // an object: OBJD BuildModeType
};
std::vector<BuildItem> build_mode_catalog(const Content &content);
// The pattern a wall tool's new walls get (the wall tool's 0; a fence's its own: 248-251).
uint16_t wall_tool_pattern(uint16_t style);
const std::vector<std::string> &game_floor_names(const Content &content);   // Build.iff's floors, by id - 1 (read once)
const char *build_object_category_name(int type);
// Build mode's sub-groups (ours; the original has none): floors, wall coverings, doors and windows by
// material or style, from words in their names and descriptions. The names by id, and an item's id
// (-1: "Other", or a kind with none).
std::vector<std::string> build_subgroup_names(BuildItem::Kind kind, int category);
int build_subgroup(const BuildItem &item);

using Primitive = Exit (*)(Frame &frame, const uint8_t *operand);

class Vm {
public:
    // `hood` may be null (no neighbourhood: Sims start with blank person data);
    // `characters` is shared with the renderer when given.
    Vm(Content &content, Lot &lot, Neighborhood *hood = nullptr, int house = 0,
       CharacterLibrary *characters = nullptr);
    ~Vm();

    // Creates every object on the lot and runs its init and main.
    void load_lot();
    // A lot that came from somewhere else (the exe: one whose file has an EXPi chunk; ours: a lot
    // that arrived by Ad hoc Share, at its first load here): each object's "import lot" tree, by
    // name, as the exe's lot loader broadcasts it (FUN_0054eb20 -> FUN_0052c290). The game's
    // five: the urns forget their ghost's neighbour id (attribute 1), the Visit Generator its
    // attribute 7, the phone stops what anyone is doing with it. Returns how many ran.
    int run_import_lot();
    // A lot that arrived by Ad hoc Share, at its first load here: the "import lot" trees, and the
    // neighbour ids its tombstones hold (ghost_attribute) as ours for the same Sim (the arrival's
    // `neighbors`, its family's `members`), 0 for one who isn't here (as "import lot" leaves them).
    void import_arrival(const ShareArrival &arrival);
    // Called between one object and the next while a lot is made (load_lot, load_state), where
    // nothing holds on to a file's bytes: the viewer lets the file cache down to its budget
    // there (Downtown's objects took it to 95 MB on the Vita, frames being what trims it).
    // Set only while loading, on the loading thread.
    std::function<void()> between_objects;
    // Set by the viewer: a file to read ahead on its reader thread into the read-ahead store. An
    // animation not in yet is asked for this way and the Sim waits a tick (animate), instead of the
    // game's thread reading the card mid-tick (35-100 ms a tick at Solana, the stutter's half).
    std::function<void(const FileRef &)> request_file;
    // The viewer's Sim density setting: 0 the game's own, 1 medium, 2 low. On a community lot the
    // pedestrian markers bring guests (shoppers, strollers) for atmosphere; past the setting's cap
    // a guest isn't brought (the marker asks again later). The staff the lot's objects make for
    // themselves (chef, waitress, clerks, janitor) and the family visiting aren't guests.
    int sim_density = 0;
    int guest_cap() const { return sim_density == 1 ? 3 : sim_density == 2 ? 1 : 1 << 30; }
    bool animation_pending = false;   // animation_for found one waiting for its files
    // animation_for's answers, once found (the library never lets one go): by the object, its files,
    // the table and the entry. Every animating Sim, every tick, looked its animation up by name: the
    // name copied and lower-cased, the library's lock taken, a search of its map.
    struct AnimationKey {
        const void *object, *own, *semi;
        uint32_t table;
        int id;
        bool operator==(const AnimationKey &o) const
        {
            return object == o.object && own == o.own && semi == o.semi && table == o.table && id == o.id;
        }
    };
    struct AnimationKeyHash {
        size_t operator()(const AnimationKey &k) const
        {
            size_t h = std::hash<const void *>()(k.object);
            h = h * 31u + std::hash<const void *>()(k.own);
            h = h * 31u + std::hash<const void *>()(k.semi);
            return h * 31u + (size_t(k.table) << 12 ^ size_t(uint32_t(k.id)));
        }
    };
    std::unordered_map<AnimationKey, const CharAnimation *, AnimationKeyHash> animations_found;
    // Debug: routines that yielded at the 20,000-instruction mark, by "routine of object", and how often.
    std::unordered_map<std::string, int> long_runs;
    bool animation_sync = false;      // (waited long enough: read now after all)
    std::map<std::string, uint32_t> requested_files;   // the files asked of the reader, and the tick each was asked
    std::map<std::string, uint32_t> create_waits;   // Create Object waiting for a file: since when (create_object)
    bool animation_waits(const std::string &name);
    // Animations read ahead (read_ahead_animations) not all in yet: followed up each tick, a bundle
    // taken in as it arrives and its .cfp asked for, so both are in before the code plays it.
    std::vector<std::string> animations_ahead;
    void loading_pause()
    {
        if (between_objects)
            between_objects();
    }
    // An object's saved state from the lot file (attributes, object data, needs).
    void restore_saved(Entity &e, const LotObject &o);
    // One simulation tick: every thread runs until it yields.
    void tick();
    // A step in buy (mode 1) or build mode (2), as the exe's with its clock stopped (FUN_00557a00:
    // global 19 the mode): only the objects whose data 60 is set run (fences, paintings, queue ropes,
    // the help systems and controllers set it in their init); the clock and the tick count stand still.
    void tick_stopped(int mode);

    Entity *entity(int16_t id)   // (inline: asked on nearly every instruction)
    {
        if (id <= 0 || size_t(id) >= by_id_.size())
            return nullptr;
        Entity *e = by_id_[size_t(id)];
        return e && !e->dead ? e : nullptr;
    }
    // One past the highest id handed out (ids index by_id_).
    size_t id_limit() const { return by_id_.size(); }
    // The people's ids, ascending (kept for Set To Next's "next person", run hundreds of times a
    // choice by the check trees): made again each tick, or when an entity is added.
    const std::vector<int16_t> &person_ids();
    // Groups the parts of multi-tile objects on the lot: parts of the same
    // object (file and master id) on neighbouring tiles, a storey apart at most.
    void group_parts();
    // A new object (tile_x -1: not on the lot); `id` 0 picks the next free one.
    Entity *create(const ObjectInfo &info, int tile_x, int tile_y, int level, int direction, uint16_t id = 0);
    // A routine by id as seen from `owner`'s code; null if missing.
    const Routine *routine(const CodeOwner &owner, uint16_t id, CodeOwner *resolved = nullptr);
    // The category (object data 59) an object's Init gives it ("me 59 := ..." in Init or what
    // it calls, two deep), read from the code without running it; 0 if none.
    int16_t init_category(Entity &e);
    CodeOwner owner_of(const ObjectInfo &info);

    int16_t global(int i) const { return i >= 0 && i < int(globals.size()) ? globals[size_t(i)] : 0; }
    void set_global(int i, int16_t v)
    {
        if (i >= 0 && i < int(globals.size())) {
            static const int watch = TS_ENV("TS_WATCH_GLOBAL") ? std::atoi(TS_ENV("TS_WATCH_GLOBAL")) : -1;
            if (i == watch && globals[size_t(i)] != v)   // debug: who changes it
                watch_global_changed(i, v);
            globals[size_t(i)] = v;
        }
    }
    void watch_global_changed(int i, int16_t v);
    // A tuning constant (BCON) as SimAntics addresses them: table and key in one value.
    int16_t tuning(const Frame &frame, uint16_t which);
    // A string from a STR# table of `owner`'s files (or global.iff).
    std::string string(const CodeOwner &owner, uint16_t table, int index);
    // A whole STR# table of one file (English), empty if absent.
    std::vector<std::string> strings(const LoadedIff *file, uint16_t table);
    const Ttab *ttab(const LoadedIff *file, uint16_t id);

    // What the player can do. The interactions `sim` may choose on `obj` now
    // (their check trees pass), by name with their TTA numbers.
    struct MenuEntry {
        std::string name;
        int number = 0;
        // An entry the check tree named itself (Change Action String: a phone plugin's "Go
        // Downtown"): the stack object it was about, passed as the action's first parameter.
        int16_t param = -1;
    };
    std::vector<MenuEntry> pie_menu(Entity &sim, Entity &obj);
    // Queues interaction `number` of `obj` for `sim`, as the player asks.
    bool queue_interaction(Entity &sim, Entity &obj, int number, int16_t param = -1);
    // The object whose interactions a click on `obj` shows (a multi-tile object's lead).
    // (owners: the parts with a table, by master id and file, in the object list's order: made once
    // for a whole pass over the lot, as free will's, instead of a scan of every object per part.)
    using MenuOwners = std::unordered_map<uint32_t, std::vector<Entity *>>;
    Entity *menu_owner(Entity *obj, const MenuOwners *owners = nullptr);
    MenuOwners menu_owners();
    // The Sim the player controls: the chosen one, else the first of the family here.
    Entity *selected();
    void select_next();
    // A footstep's sound event where `walker` stands: footstep_soft (carpet), _medium (wood),
    // _hard (tile, stone), _terrain (outside); null for none (in the water). The game keeps
    // which floor sounds how in its program; this goes by the floor's name.
    const char *footstep_sound(const Entity &walker);
    std::map<uint16_t, const char *> footstep_kinds_;   // by floor id, worked out once
    std::vector<std::string> floor_names_;              // the game's own floors (Build.iff), by id - 1
    // A Sim the player chose while they were away (at work, at school): they stay selected.
    // Otherwise the selected Sim leaving hands control to one at home, as the game does.
    int16_t chosen_away = -1;
    void choose(int16_t id)
    {
        selected_id = id;
        const Entity *e = entity(id);
        chosen_away = e && !on_lot(*e) ? id : -1;
    }
    // Cancels the last thing `sim` has queued (or what they are doing, if nothing is).
    bool cancel_last(Entity &sim);
    int16_t selected_id = 0;
    // A Sim of the family living here (everyone else is a visitor).
    bool resident(const Entity &e) const;
    // The exe's visitor rule (FUN_0060b110, every menu, choice and start): someone not of the
    // household (person data 32, the person type, not 0) can't take an interaction without Allow
    // Visitors, on a residential lot (FUN_00456590: the house's zoning 1), nor on a community lot
    // unless the interaction has bit 5. Except an object of category 4 or 7 (data 59) for a visitor
    // holding a Hotel Key (a vacation hotel's room, its guests).
    bool visitor_refused(const Entity &sim, uint32_t flags, const Entity *object) const;
    // Whether `sim` may do interaction `ia` at all (visitors, pets, children, debug ones).
    bool allowed(const Entity &sim, const TtabInteraction &ia, bool autonomy = false, const Entity *object = nullptr) const;
    // A person's kind by its skeleton (bodystring 0): 'c' a cat, 'd' a dog, 0 a Sim.
    char pet_kind(const Entity &person) const;
    mutable std::map<uint32_t, char> pet_kinds_;   // by GUID, read once
    // The motive decay's tuning (Global.iff FCNS 1 people, 5 dogs, 6 cats: the 17 values the
    // original's decay reads, in its order), read once; the people's are FreeSims' if missing.
    const std::array<float, 17> &decay_tuning(char pet_kind);
    std::map<char, std::array<float, 17>> decay_tuning_;
    // A value of Global.iff's FCNS `id` by its name, as the original's tuning lookup (FUN_00649fa0)
    // reads it; `fallback` (the program's own default) when the name isn't there.
    float global_tuning(uint16_t id, const std::string &name, float fallback);
    // Another Sim whose box (6 sixteenths across, as the original's) `walker` would walk into at
    // (x, y): not one already overlapping it, nor seated or held, nor a ghost, nor the walk's target.
    const Entity *person_in_the_way(const Entity &walker, float x, float y) const;
    std::map<std::pair<uint16_t, std::string>, float> global_tuning_;
    // Rooms as the game's code sees them: numbered across both storeys
    // (room 0 is none), with what is in them.
    struct Room {
        int level = 1;
        bool outside = false, pool = false;
        int area = 0;
        int light = 0;        // 0-100: lamps, plus windows and doors by day
        int lamps = 0;        // 0-100: the lamps alone (what the game's drawing lights a room with)
        int drawn = 0;        // 0-100: the light it's drawn with indoors (the light, or 80: see refresh_rooms)
        int16_t score = 0;    // what being here does to a Sim's Room need
    };
    std::vector<Room> rooms;
    bool rooms_ready_ = false;   // objects set up: their "room changed" code may run
    // The room at a tile of a storey (1 = ground); 0 off the lot.
    uint16_t room_at(int level, int tile_x, int tile_y) const;
    // A storey's room (RoomMap's local number, storey 0 = ground) as `rooms` numbers it; 0 none.
    uint16_t room_of(int level0, uint16_t local) const
    {
        return level0 < 0 || level0 >= kLotLevels || local == 0 ? 0 : uint16_t(room_base_[level0] + local);
    }
    // A family's home lot (not downtown, a vacation spot or a community lot).
    bool residential() const;
    // The time of day, as SimAntics global 4 has it: 0 day, 1 dawn, 2 night, 3 dusk.
    int time_of_day() const;
    // A routine the engine answers "true" for without running it (see vm.cpp: few, each explained).
    bool answered_by_engine(const Routine &r, const CodeOwner &owner) const;
    // The community bug fixes (community_fixes, see vm.cpp "Community bug fixes"): a pest controller's
    // "generating object?" answered "no" for beds, showers, tubs, sinks; other routines answered or
    // prepared (fixed_answer: -1 none, 0 false, 1 true).
    bool no_pests_from(const Routine &r, int16_t stack_id);
    int fixed_answer(const Routine &r, const CodeOwner &owner, const Frame &caller);
    // In a pool's room (swimming: routes through water only, swims instead of walking; FreeSO InPool).
    bool in_pool(const Entity &e) const;
    // The walking/standing animations for where a Sim is: the swim table in a pool.
    const std::vector<std::string> &moves(const Entity &e) const;
    // Works the rooms out from the walls, once.
    void build_room_map();
    // Each object's room, the rooms' light and scores, the Sims' Room need (every second).
    void refresh_rooms();
    // Lets go of removed objects nothing refers to any more (every 30 s).
    void forget_removed();
    // The ids free for new objects (the gaps below the highest handed out), after a lot or a save
    // is loaded; forget_removed adds a forgotten object's.
    void collect_free_ids();
    // The game's global objects (OBJD Global 1) the lot lacks, made out of the world and returned
    // without their Init run (load_lot and load_state run it).
    // only_type >= 0: only those of that OBJD type (a save: the gift tokens, 12; the area controllers a
    // lot removed at its first load stay gone).
    std::vector<Entity *> add_missing_globals(int only_type = -1);
    // A new object with all its parts (a multi-tile object's master creates the
    // parts at their offsets); returns the lead part.
    Entity *create_with_parts(const ObjectInfo &info, int tile_x, int tile_y, int level, int direction);
    // On the lot itself (Sims away are kept in a corner outside it).
    bool on_lot(const Entity &e) const
    {
        return e.in_world() && !e.dead && e.tile_x() < lot.size && e.tile_y() < lot.size;
    }
    // The family playing here: the one living here, else the one that travelled here
    // (downtown, on vacation). Null on lots with neither.
    // Debug (TS_PROFILE): time spent per primitive opcode and per object's thread.
    struct Profile {
        bool on = false;
        double primitive_ms[256] = {};
        uint64_t primitive_calls[256] = {};
        std::map<std::string, double> object_ms;
        double motives_ms = 0, routes_ms = 0, animations_ms = 0, rooms_ms = 0;
        double score_action_ms = 0, score_check_ms = 0, score_allowed_ms = 0;
        uint64_t score_checks = 0, check_instructions = 0;
        uint64_t instructions = 0;
    } profile;
    // A tick over this many ms is logged with what took its time (0: never). The viewer sets
    // it on the Vita, so the log names the stutter's slow ticks.
    double slow_tick_ms = 0;
    // This tick's autonomous choices (find_best_action): how many, their check trees, their time;
    // counted whenever slow ticks are logged (the slow-tick line names them: a choice at Solana
    // runs ~40 objects' check trees in one tick).
    uint32_t tick_choices = 0, tick_checks = 0;
    double tick_choice_ms = 0;
    // A Sim the planner leaves out of the people in the way (asking who'd open a way: walk_to).
    const Entity *route_ignore_person = nullptr;
    int route_plans = 0;   // and this tick's route planning: its grids and its searches (routing.cpp)
    double route_grid_ms = 0, route_search_ms = 0;
    double route_rect_ms = 0;      // the sixteenths planner (TS_ROUTE_RECT), this tick
    long route_total_plans = 0;    // debug: every tick's plans and their time (grids, searches, rect), the run's
    double route_total_ms = 0;
    int route_rect_failed = 0;    // its plans that found no path this tick (the tile planner then tried)
    // The object whose code is running now (0 between), for the viewer's watchdog.
    int running_id = 0;
    const Family *family() const
    {
        if (!hood)
            return nullptr;
        if (const Family *f = hood->family_in_house(house))
            return f;
        return hood->travel.family && hood->travel.to_house == house ? hood->family_by_id(hood->travel.family) : nullptr;
    }
    Family *family() { return const_cast<Family *>(static_cast<const Vm *>(this)->family()); }
    // Writes the Sims' lasting person data (skills, job, personality ...) back to their
    // neighbours, as the game does leaving a lot, so the next lot builds them as they are.
    void store_people();
    // A Sim's interests on the game's 0-1000 scale (the exe's FUN_005f4050, run as a Sim comes onto
    // the lot: FUN_005f2e90, 005f4220, 005f4ba0): when interests 46-53 are all 10 or less (the
    // neighbourhood file's 0-10), a new set is dealt, ten (46-55) of four low 0-3, three middling 4-6
    // and three high 7-10, and Hot Date's five (13, 14, 16, 20, 26) of one low, two middling and two
    // high, each x100. The talk code reads them so (HDSI_Talk "talk - function": under 100 the
    // listener ends the conversation). TS_INTERESTS_OLD=1: left as they are.
    void deal_interests(Entity &e);
    // A lot opened from its save (not reached on a trip) that a trip left: the trip is
    // over (the game was quit away), so the travellers still off the lot walk in from the
    // street and the trip is forgotten. The interaction that would have brought them
    // home isn't in the save.
    void end_trip_here();
    // Family members missing from their own lot altogether, built again at the door (see vm.cpp).
    int restore_missing_family();
    // Other families' members left on this lot by a trip's save, taken off it (see vm.cpp).
    int remove_visiting_families();
    // A family just moved in (from the neighbourhood, the lot fresh from its file): its Sims
    // made at the way in from the street, with their neighbourhood data, and started.
    void move_in(int16_t family_id);
    int64_t iteration_limit() const;   // the instructions a run may take before it counts as stuck
    // The other way, for a lot taken up again after a trip: its Sims as the neighbourhood
    // now has them (what they learnt away), keeping what is about this lot.
    void refresh_people();
    // Generic call 17 asks to go to another lot: the host saves this one and opens that
    // (house number), with hood->travel saying who goes. -1: no request.
    int switch_to_house = -1;
    // That call's work: `traveller` (and the family, on a vacation or when global 34 says) sets
    // off for lot `house` (-1: the family's home), their needs and the hour taken along. False if
    // there's no such lot or neighbourhood. (Also the debug "trip" command's: there without the cab.)
    bool start_trip(Entity &traveller, int house);
    // Money when no family owns the lot.
    int loose_budget = 20000;
    // Whether the game lets the player buy and build now (generic calls 10 and 11).
    bool build_buy_enabled = true;
    // Which of the lot's objects an autonomous choice looks at (the original's FUN_005f6cd0, by
    // (Sim id + a count of choices) & 3): 0 all, 1 within 7 tiles, 2 even ids, 3 odd ids; -1 all
    // (the debug listing).
    int autonomy_subset = -1;
    uint32_t autonomy_choices = 0;
    std::vector<int16_t> build_buy_locks;   // the objects that turned it off (generic call 10), until each turns it on
    // The move_objects cheat: buy mode lifts and sells what the game normally keeps put.
    bool move_objects = false;
    // Tests only (the remote's `debugmenu`): the player's menu shows the game's debug interactions too.
    bool debug_menu = false;
    // The original's game options: Free Will (global 30, 1 on, set each tick) and Quick Tips
    // (off: the help systems' boxes aren't shown, their questions answered no; dialog_with).
    bool free_will = true, quick_tips = false;
    // Stray pets may wander onto the lot (Unleashed): the pedestrian portal's "create neighbor"
    // brings a pet only with global 20's bit 6 set, which the "Stray-Away" sign clears in the
    // original; the user's option clears it instead (2026-10-31).
    bool stray_pets = true;   // off: the pedestrian portal brings no pets (memory.cpp, global 20)
    bool critters = true;   // off ("Critters", the user's 2026-11-14): no raccoon or skunk is made (Vm::fixed_answer)
    // The community bug fixes (the user, 2026-11-13: one switch, "Community bug fixes"): the flies/roaches
    // fix, the Pet Bath, the women's walk, the Obsessed Fan, the burglar, the butler's
    // fridge, the burger plate, Waylon's fame. Off: as the original.
    bool community_fixes = true;
    // The player's option "Friendships fade" (2026-10-30): points a day the family's daily
    // relationship scores move toward 0, at the day's turn; 0 leaves them (the engine has no
    // decay of its own; the original's daily rule and rate are not read yet).
    int relationship_decay = 0;
    // The player's option "Mood degradation" (the user's, 2026-11-04): 0 as the
    // original, 1 half as fast, 2 a quarter as fast. Only the steady decay's falls are slowed (the
    // energy a sleeping Sim gets back and what objects give or take are as the game's).
    int motive_decay = 0;
    // The player's option "Bills" (the user's, 2026-11-13): 0 as the original (every 3 days), 4-7 every
    // that many days, -1 none (Vm::tuning: the mailbox's own constants answered).
    int bill_days = 0;
    // The player's option "Plant life span" (the user's, 2026-11-14): 0 as the original, 2 or 5 times as
    // long, -1 immortal (plants and flowers never dry out; gift bouquets never wilt). Vm::fixed_answer
    // skips the plants' "process" (their drying, a day's worth at a time) but every 2nd or 5th; Vm::tuning
    // stretches a bouquet's days. plant_runs: each plant's calls of it so far (not saved: a fraction lost).
    int plant_life = 0;
    std::unordered_map<uint16_t, uint16_t> plant_runs;
    // The player's gameplay modifier "Friendship gains" (the user's, 2026-11-10; not the original's):
    // 1 as the original, 2 or 3 times what a rise in a daily relationship score brings (a social's
    // step, up to 20 points at a time; set values and falls as the game's), up to 100.
    int relationship_gain = 1;
    // The view's camera angle (0-3), given by the viewer while the game's thread is idle: what
    // "look towards the camera" turns a Sim to (the refusals, "I'm not in the mood").
    int camera_rotation = 0;
    // A lot opened with no family (global 32, the original's 0x456911): money doesn't count. Funds
    // read as 999,999 and nothing changes them (0x556e20, 0x5572f0), on every kind of lot.
    bool free_money() const { return globals[32] == 1; }
    void set_family_global();   // global 9: the family on this lot (lot load, save restore, move-in)
    int budget() { return free_money() ? 999999 : family() ? family()->budget : loose_budget; }
    void add_money(int amount)
    {
        if (free_money())
            return;
        if (Family *f = family())
            f->budget += amount;
        else
            loose_budget += amount;
    }

    // Buy mode (buy.cpp), as FreeSO's buy, move and delete commands do it.
    // The object in the player's hand: a new one (made off the lot and set up,
    // not paid for until placed) or one lifted off the lot to move it.
    struct Held {
        Entity *obj = nullptr;
        bool bought = false;   // new from the catalogue (else being moved)
        int price = 0;
        int16_t from_x16 = -1, from_y16 = -1;   // where a moved one came from
        int8_t from_level = 1;
        int from_direction = 0;
        Entity *from_container = nullptr;
        int from_slot = -1;
    } held;
    // Takes a new object from the catalogue into the hand.
    bool hold_new(const CatalogItem &item, std::string &why);
    // Lifts an object off the lot to move it (its lead part, the whole object).
    bool hold_existing(Entity *obj, std::string &why);
    // What stops the held object going at tile (tx, ty) of storey `level`
    // (1 ground) facing `direction` (0-7, even): empty if nothing. `onto`
    // gets the surface it would stand on, if any.
    std::string placement_problem(int tx, int ty, int level, int direction, Entity **onto = nullptr);
    // The same test for any object (its lead part) at a tile: why it can't go there, or "".
    // `in_play`: as the exe's test for an object the game's code creates (FUN_0053e1d0's rule for a
    // Sim nearby: in the way only if the object's flags don't let people through, neither's "allow
    // intersection" lets the other in, and the Sim's box meets its footprint), not buy mode's.
    std::string placement_problem_of(Entity *lead, int tx, int ty, int level, int direction, Entity **onto = nullptr,
                                     bool in_play = false);
    // Puts the held object down there (paying for a new one).
    bool place_held(int tx, int ty, int level, int direction, std::string &why);
    // Puts it back: a new one goes, a moved one returns where it was.
    void drop_held();
    // Whether the player may move or sell an object now; why not otherwise.
    std::string movable_problem(Entity *obj, bool selling);
    // What selling an object gives back (the exe's FUN_0053adc0): its current value (object data 41),
    // halved while broken. The price paid till midnight, then less its depreciation (new_day).
    int sell_value(const Entity *obj) const;
    // Midnight, as the exe's clock has it: objects' ages and depreciation (FUN_0052cef0).
    void new_day();
    // The lot as the game values it when it's saved (the original's 0x56ad40): objects, and the
    // architecture (build mode's objects, walls, coverings, floors, pool). `report` splits the
    // architecture for checking against the values lots ship with.
    struct LotValues {
        int objects = 0, architecture = 0, arch_objects = 0, walls = 0, coverings = 0, floors = 0, pool_tiles = 0;
    };
    LotValues lot_values() const;
    // The live panel's House tab, as the original's (FUN_004dc390, its stats FUN_0056b460, read
    // 2026-11-10): square feet (9 a tile of every inside room), bedrooms (inside rooms with a bed,
    // category 4) and bathrooms (5, 6, 7: toilet, tub, shower), the lot's size word (0 small under 40
    // tiles, 1 medium under 50, 2 large) and the five bars 0-100: Size (STR# 505 curve 1 of square feet a
    // family member), Furnishings (curve 2 of architecture / objects value), Yard (outdoors' good objects
    // and pool tiles, twice their bad impact, x 0.33), Upkeep (objects not dirty or broken), Layout (time
    // spent not walking, layout_t / layout_b).
    struct HouseStats {
        int sqft = 0, bedrooms = 0, bathrooms = 0, lot_word = 0;
        int bars[5] = {};   // size, furnishings, yard, upkeep, layout
    };
    HouseStats house_stats() const;
    // The Layout bar's history (the exe's FUN_005f4f90, FUN_0056aef0): six days (5 back and today) of
    // family-member ticks (every 10 ticks, awake and shown) and of those walking; a fresh lot all 200
    // (the bar 0 until the days fill). At midnight slot 0 is copied into 1-4 (the original's) and today
    // into 0. Saved with the lot (save version 19).
    int layout_t[6] = { 200, 200, 200, 200, 200, 200 }, layout_b[6] = { 200, 200, 200, 200, 200, 200 };
    bool sell(Entity *obj, std::string &why);
    // The house bulldozed to an empty lot, as the original's (0x54f6e0): what players may delete
    // off (things on the street kept), every wall, every floor but the street's; the terrain stays.
    void bulldoze();
    // A catalogue entry's description (CTSS string 1).
    std::string catalog_description(const ObjectInfo &info);
    // Sims made while playing (a baby growing up: Make New Character), kept in saves.
    using MadeCharacter = MadeSim;
    std::vector<MadeCharacter> made_characters;
    // Makes one (age in years, gender 0 male 1 female, skin as the game numbers it in person data
    // 60: 1 light, 2 dark, 3 medium): the new neighbour's id, 0 if it can't.
    int16_t make_character(int age, int gender, int skin_number, const std::string &name = {});
    // A pet bought at a pen (Unleashed): a copy of the template dog or cat with the coat numbered
    // `coat` among pet_coats(); its neighbour id, 0 if it can't be made.
    int16_t make_pet(bool dog, int coat, bool female, const std::string &name);
    // Registers a made character with the content and the neighbourhood (also when loading).
    const ObjectInfo *register_made(const MadeCharacter &m);
    // Makin' Magic's Transform (generic calls 39 and 40, the exe's FUN_00549ac0 / FUN_005483a0 with
    // FUN_00548770): a child made an adult, the same Sim with an adult body and head of their sex
    // and tone at random (the exe rewrites their character file); a dog or cat made a person, a new
    // adult of the pet's sex and a random tone in the pet's place (its family place, relationships,
    // tokens and spot on the lot; a neighbour record of its own, kept with the neighbourhood's
    // made Sims), the pet gone. The grown one's id, 0 if it can't.
    int16_t grow_up(Entity &child);
    int16_t pet_to_person(Entity &pet);
    // A person's walking, standing and swimming animations by age and body (create, grow_up).
    void set_body_animations(Entity &e);
    // Removes an object as Remove Object Instance does, from outside any code (debug tools).
    void remove_now(Entity &e);
    uint32_t choices_tick = 0;   // free will's choices this tick (find_best_action's limit)
    int choices_made = 0;
    // Remove Object Instance of another object, as the exe's (FUN_0052bfc0): queued (once each) and
    // done when the tick's objects have all run (FUN_00530a90 -> FUN_0052ecc0), so the rest of the
    // tick still sees it (out of the world at once: remove_object). TS_REMOVE_NOW=1: at once, as before.
    std::vector<Entity *> pending_removals;
    void finish_removals();
    // Clothes bought at Hot Date's racks (generic calls 24, 29-31; FreeSO
    // VMTS1PurchasableOutfitHelper): the outfits of a clothing type (0 everyday, 1 formal, 2
    // swimwear, 3 pyjamas, 4 winter, 5 high fashion) the install has for this Sim, which of them
    // they wear (-1 none), and a new one put in their bodystring for good (the neighbour keeps it
    // and the saves), answering the one before. `looks_changed` counts the changes for the
    // drawing's look caches.
    std::vector<std::string> suits_of(const Entity &sim, int type);
    int16_t suit_index(const Entity &sim, int type);
    int16_t set_suit(Entity &sim, int type, int index);
    unsigned looks_changed = 0;
    // Build mode (build.cpp). Each returns what it cost (>= 1 when something
    // changed), 0 when there was nothing to do, -1 when it can't (`why`).
    // Floors: tiles (x0, y0)-(x1, y1) of storey `level` (1 ground); null erases.
    int build_floor(int x0, int y0, int x1, int y1, int level, const BuildItem *item, std::string &why);
    // The roof tool: a pitch or a pattern (a BuildItem::Roof) for the whole house, free (the
    // original's: "There is no cost for roofs"). Whether it changed anything.
    bool set_roof(const BuildItem &item);
    int build_pool(int x0, int y0, int x1, int y1, int level, const BuildItem &item, bool erase, std::string &why);
    // Walls: a straight run between grid corners (x0, y0) and (x1, y1), or
    // taking one down.
    int build_walls(int x0, int y0, int x1, int y1, int level, const BuildItem &tool, bool erase, std::string &why);
    // A wall covering on one face: the wall on `edge` (0 north, 1 east, 2 south,
    // 3 west) of tile (x, y), the face seen from that tile.
    int build_wallpaper(int x, int y, int level, int edge, const BuildItem &item, std::string &why);
    // A diagonal run (corner to corner at 45 degrees; build_walls hands these on).
    int build_diagonal(int x0, int y0, int x1, int y1, int level, const BuildItem &tool, bool erase, std::string &why);
    // The room tool: walls round the rectangle from corner (x0, y0) to (x1, y1).
    int build_room(int x0, int y0, int x1, int y1, int level, const BuildItem &tool, std::string &why);
    // The whole room the tile is in: its floor (null: taken up), or every wall face inside it.
    int fill_floor(int x, int y, int level, const BuildItem *item, std::string &why);
    // (`edge`: the face pointed at, its tile (x, y); outdoors only the walls joined to that one.)
    int fill_wallpaper(int x, int y, int level, int edge, const BuildItem &item, std::string &why);
    // The faces the whole-room covering would cover, as (tile x, tile y, edge): inside, every face
    // of the room; outdoors (room 1's), those of the walls joined to the face pointed at, one
    // building's outside, not every building's (the user's shed, 2026-11-12).
    std::vector<std::array<int, 3>> fill_wallpaper_faces(int x, int y, int level, int edge) const;
    // The terrain (the game's landscaping tools, a simoleon a corner): the tile corners from
    // (x0, y0) to (x1, y1) (corner c is the lot point c - 0.5) raised (how +1) or lowered (-1) a
    // step, or (how 0) brought to the height of corner (x0, y0). Corners at floors, walls or
    // objects stay as they are.
    int shape_terrain(int x0, int y0, int x1, int y1, int how, std::string &why);
    // Grass on the tiles from (x0, y0) to (x1, y1): grown (lush) or taken away; a simoleon a tile.
    int paint_grass(int x0, int y0, int x1, int y1, bool grow, std::string &why);
    // The lot's pattern number for a covering (adding a .flr/.wll file to the lot's map).
    uint16_t style_for(const BuildItem &item);
    // Whether floor may be laid on tile (x, y) of storey `level` (2 and up), the exe's "floorable" tile
    // flag (FUN_005826b0, its final pass; checked by the floor tool, FUN_0058aa90, and marked in build
    // mode by the floor painter): see build.cpp.
    bool floorable(int x, int y, int level) const;
    // After a change: rooms again, objects beside walls told.
    void lot_edited();
    void walls_changed();
    // An object the game's code put on the lot, or took off it: its "dynamic multi-tile update" entry (8)
    // and its like-kind neighbours' run, as buy mode's placing does (the floods' joined pictures).
    void dynamic_placed(Entity &lead);
    void dynamic_left(int x, int y, int level, uint32_t guid);
    bool lot_changed = false;   // the house differs from its lot file (saves keep it)
    uint32_t lot_version = 0;   // counts changes (the viewer redraws the house)
    uint64_t edits = 0;         // counts changes to objects made between ticks (buying, moving, selling, building)
    // Objects bought by the player, by id: price and the day (sold back at full price that day).
    std::map<uint16_t, std::pair<int, int>> bought;
    int day_number() const { return clock.year * 360 + (clock.month - 1) * 30 + clock.day; }

    void warn(const Frame *frame, const std::string &what);

    Content &content;
    Lot &lot;   // build mode changes its walls and floors
    Neighborhood *hood;
    int house;
    CharacterLibrary *characters;
    // An animation by name (without extension); null if there is none.
    const CharAnimation *animation(const std::string &name);
    // The animations the lot's Sims play or hold now (the character library lets go of others).
    void animations_in_use(std::unordered_set<const CharAnimation *> &out) const;
    // Lookups of animations let go are forgotten (looked up afresh when next asked).
    void forget_animations(const std::vector<const CharAnimation *> &gone);
    // The files this lot's code runs from, or has run from (the caches' keys): what Content must
    // keep while the lot lives.
    void files_in_use(std::unordered_set<const LoadedIff *> &out) const;

    // The game clock (FreeSims VMClock): 30 ticks a game minute (kTicksPerSecond a second).
    struct Clock {
        int fraction = 0, minutes = 0, hours = 8, day = 1, month = 1, year = 0;
        static constexpr int kTicksPerMinute = 30;
        int seconds() const { return fraction * 60 / kTicksPerMinute; }
        void tick();
    } clock;
    std::vector<std::unique_ptr<Entity>> entities;
    std::array<int16_t, 64> globals{};
    // Attributes shared by every object of a type (by GUID): the neighbourhood's, which
    // every lot shares (only a lot without one keeps its own).
    std::map<uint32_t, std::vector<int16_t>> own_type_attributes;
    std::map<uint32_t, std::vector<int16_t>> &type_attributes() { return hood ? hood->type_attributes : own_type_attributes; }
    const std::map<uint32_t, std::vector<int16_t>> &type_attributes() const
    {
        return hood ? hood->type_attributes : own_type_attributes;
    }
    int16_t type_attribute(const Entity *e, int i) const
    {
        if (!e || !e->info || i < 0)
            return 0;
        auto it = type_attributes().find(e->info->guid);
        return it != type_attributes().end() && size_t(i) < it->second.size() ? it->second[size_t(i)] : 0;
    }
    void set_type_attribute(const Entity *e, int i, int16_t v)
    {
        if (!e || !e->info || i < 0 || i > 1024)
            return;
        auto &a = type_attributes()[e->info->guid];
        if (a.size() <= size_t(i))
            a.resize(size_t(i) + 1, 0);
        a[size_t(i)] = v;
    }
    std::array<Primitive, 256> primitives{};
    uint32_t ticks = 0;
    uint32_t random_state = 12345;
    // Sounds the game asked for since the last look (the player plays them):
    // an event name for an object, or (stop) the end of that object's sounds.
    struct SoundEvent {
        std::string name;
        uint16_t source = 0;
        bool loop = false, stop = false;
        bool no_pan = false;   // Play Sound's flag 8 (FreeSims NoPan): heard as it is, wherever the source is
        // Where the source was when it played (-1 off the lot): heard from there if it leaves the
        // lot in the same tick (Mortimer shutting the carpool's door as he gets in).
        int x16 = -1, y16 = -1, level = 1;
        int volume = 100;
        char voice = 'f';   // who speaks, for voices: f or m
        // A Sim's object variables for the sound's HIT program (FreeSO
        // VMAvatar.SubmitHITVars): 0 sex (2 a child), 2/5/7/11 skills, 14 age, 15-20 personality.
        std::array<int, 29> vars{};
    };
    std::vector<SoundEvent> sounds;
    // A dialog the game's code is showing (FreeSims VMDialogInfo). A blocking
    // one stops the simulation until answered (the player's UI sets `response`:
    // 0 yes/ok, 1 no, 2 cancel). With no one to ask (auto_answer), the first button.
    struct Dialog {
        uint32_t id = 0;
        int type = 0;   // 0 message, 1 yes/no, 2 yes/no/cancel, 3 text, 5 number, ...
        std::string title, message, yes, no, cancel;
        uint16_t caller = 0;
        // Its picture (operand flags bits 1-3, FreeSims VMDialogFlags): 0 auto (the stack
        // object, else the caller), 1 none, 2 a neighbour (by id, temp 0), 3 indexed, 4 named.
        int icon_type = 0;
        uint16_t icon_object = 0;
        int16_t icon_neighbor = -1;
        int16_t named_neighbor = -1;   // picture type 0: a neighbour the text named, not on the lot (their face)
        // The second operand: a string of the dialog's table naming its picture (icon type 4), or
        // a number (type 3); and the code file it's in (where the picture is looked for).
        int icon_operand = 0;
        std::string icon_name;
        const void *icon_file = nullptr;   // the LoadedIff whose strings the dialog uses
        std::string source;   // the routine that shows it, for debugging
        bool blocking = true;
        int response = -1;
        std::string text;   // a text entry's (type 3) answer: the name typed
        // A chooser (the phone book: type 10): what to pick from, by number
        // (a neighbour id), and the number picked (with response 0; 1 cancels).
        std::vector<std::pair<int16_t, std::string>> choices;
        int16_t number = 0;
        bool no_cancel = false;   // a lot chooser: one of the choices must be taken (the original's map has no way back)
        // A picker (the clothing racks' and the dresser's outfits, the pet pens' coats: dialog
        // types 6 and 9; the original's "Please Select Outfit" / "Please Select Your Pet" with
        // Previous and Next): one choice shown at a time with its picture. `previews` matches
        // `choices`: a body suit "suit,BODY=texture" the caller is drawn wearing (preview_kind
        // 1), or a dog's (2) or cat's (3) coat. `price` is written under the picture when above
        // 0; `preview_start` is the choice shown first (the outfit worn). 4: Makin' Magic's Transform Me,
        // each preview an outfit number the caller is drawn in.
        std::vector<std::string> previews;
        int preview_kind = 0;
        int price = 0;
        int preview_start = 0;
    };
    std::vector<Dialog> dialogs;   // waiting to be shown/answered, oldest first
    // Where the game's code points the player (primitive 35 "Special Effect": the original's
    // FUN_0061d5d0 / FUN_0040af10, the "Auto-Centering" option). With `centre` the view moves to
    // the object (a fire, a birth, a visitor leaving with a word); without, a notice names it
    // for `seconds` (the burglar's 10000 last until the call with `start` false; the Grim Reaper,
    // the car pool, a phone call), only while it's off screen unless `always`. `text` is a line
    // of the caller's STR# 305 ("Burglar!", "A baby is born!"). The player's interface takes
    // them each frame (run_vm prints them).
    struct Attention {
        uint16_t object = 0;
        bool start = true, centre = false, always = false;
        int seconds = 0;
        std::string text;
        std::string source;   // the object and routine that asked, for debugging
    };
    std::vector<Attention> attention;
    // Money paid or received by Transfer Funds, for the amount shown rising over the one it's for
    // (the viewer takes them as it takes `attention`).
    struct MoneyShown {
        int16_t who = 0;      // the object the code ran for (the Sim paying)
        int32_t amount = 0;   // + received, - paid
    };
    std::vector<MoneyShown> money_shown;
    // Autonomy's scoring curves (FreeSO TS1Curve; global STR# 501 adults, 503
    // children): how much each need's level is worth, by need.
    struct Curve {
        std::vector<std::pair<float, float>> points;   // by x
        float at(float x) const;
    };
    const std::vector<Curve> &interaction_curves(bool child);
    // The mood's weights (Global STR# 502 adults, 504 children: "HappyWeightCurves"), one a need
    // in the original's order: hunger, comfort, hygiene, bladder, fun, room, social.
    const std::vector<Curve> &mood_curves(bool child);
    bool auto_answer = true;
    bool auto_answer_no = false;   // (headless tests: the second button instead of the first)
    uint32_t next_dialog = 1;
    // Whether the game is waiting on the player.
    bool waiting_on_player() const
    {
        for (const Dialog &d : dialogs)
            if (d.blocking && d.response < 0)
                return true;
        return false;
    }
    // Problems met while running (missing routines, unknown scopes, ...), with how often.
    std::map<std::string, int> warnings;
    // Debug (the interaction sweep): how each Sim's interactions end, when set.
    struct InteractionEnd {
        int16_t sim = 0;
        int interaction = -1;   // the queued action's number (-1 when not known)
        int16_t callee = 0;
        std::string routine;    // the routine it ended in
        int ip = 0;
        int result = 0;         // 1 true, 0 false, -1 error, -2 dropped before starting (its check said no)
    };
    std::vector<InteractionEnd> *interaction_ends = nullptr;
    // With count_calls: how often each routine was entered (to see what runs).
    bool count_calls = false;
    std::map<std::string, int> calls;
    // Routines whose every instruction is printed as it runs (debugging), and how many lines are left.
    std::vector<std::string> watch;
    int watch_entity = 0;   // and everything this object runs
    int watch_lines = 400;
    CodeOwner global_code;

    // A file's string table (STR#, TTAs ...) or constants (BCON), parsed once and kept: the
    // game's code reads them all the time (autonomy names every interaction of every object).
    const StringTable *string_table(const LoadedIff *file, const char *type, uint16_t id);
    const Bcon *constants(const LoadedIff *file, uint16_t id);

private:
    std::map<std::tuple<const LoadedIff *, uint32_t, uint16_t>, std::unique_ptr<StringTable>> string_tables_;
    // (Hashed: a routine and a constant are looked up on every call and every tuning read, several
    // hundred thousand times a second in free will's check trees; the ordered map's search was
    // part of a choice's cost.)
    struct FileIdHash {
        size_t operator()(const std::pair<const LoadedIff *, uint16_t> &k) const
        {
            return std::hash<const void *>()(k.first) * 31u + k.second;
        }
    };
    std::unordered_map<std::pair<const LoadedIff *, uint16_t>, std::unique_ptr<Bcon>, FileIdHash> constants_;
    std::unordered_map<std::pair<const LoadedIff *, uint16_t>, std::unique_ptr<Routine>, FileIdHash> routines_;
    std::map<std::pair<const LoadedIff *, uint16_t>, std::unique_ptr<Ttab>> tables_;
    std::vector<Entity *> by_id_;
    std::vector<int16_t> person_ids_;
    uint32_t person_ids_tick_ = ~0u;
    size_t person_ids_count_ = 0;
    RoomMap room_maps_[kLotLevels];
    uint16_t room_base_[kLotLevels] = {};
    std::map<std::pair<std::string, uint16_t>, const ObjectInfo *> masters_;
    const ObjectInfo *master_of(const ObjectInfo &info);
    std::unique_ptr<CharacterLibrary> own_characters_;
    uint16_t next_id_ = 1;
    std::vector<uint16_t> free_ids_;   // a heap, the lowest first (the original's FUN_0052bd20 takes the lowest free)
public:
    // The exe's object list order (Entity::list_key), dead ones included until they're freed.
    std::map<int64_t, Entity *> object_order;
    int64_t head_key = 0, tail_key = 0;
    uint64_t next_seq = 0;
    void set_list_place(Entity &e, int64_t key, uint64_t seq);   // (a save's, restored)
    // Every live object in the object list at its own place and found by its own id; the first
    // few that aren't, logged with `where` (an object the list misses is one Set To Next never
    // reaches: the game's "verify stack obj id" then retries for ever). True when all hold.
    bool check_object_order(const char *where);
private:
    uint32_t next_serial_ = 1;
    std::vector<Curve> curves_[2];   // adults, children
    std::vector<Curve> mood_curves_[2];
};

// Variables (FreeSims VMMemory).
int16_t get(Frame &frame, int scope, int16_t data);
void set(Frame &frame, int scope, int16_t data, int16_t value);
int32_t get_big(Frame &frame, int scope, int16_t data);   // TempXL aware
void set_big(Frame &frame, int scope, int16_t data, int32_t value);

// The primitive table.
void register_primitives(Vm &vm);

// Autonomy's view of what a Sim could do: every interaction with
// advertisements, scored, best first (primitives_world.cpp). Without
// `keep_failed`, only those whose check tree passes.
struct AdScore {
    float score = 0;
    Entity *obj = nullptr;
    QueuedAction action;
    bool passed = false;
    std::string ads;   // the ads it was scored on, for debugging
};
std::vector<AdScore> score_interactions(Vm &vm, Entity &sim, bool keep_failed);

// The game state of a lot in the engine's own format (save.cpp). Loading
// replaces the VM's objects (use it instead of load_lot()). `apply_hood`
// false: the neighbourhood's people, families and money in the lot save are
// skipped (a neighbourhood save, newer, has them).
bool save_state(const Vm &vm, std::vector<uint8_t> &out);
// `fresh`: the lot as saved (what's built, what stands where) but starting as a lot opened from
// its file does: nobody on it, every object's init and main run from the top. A lot edited from
// the neighbourhood, reached on a trip: the original writes the edit into the lot's own file.
bool load_state(Vm &vm, const uint8_t *data, size_t size, std::string &error, bool apply_hood = true, bool fresh = false);
// The neighbourhood's own save (people, families, money, a trip under way).
// A person's variables for their sounds (gender, skills, age, personality; their voice).
void person_sound_vars(const Entity &source, Vm::SoundEvent &e);
bool save_hood(const Neighborhood &hood, std::vector<uint8_t> &out);
// Ad hoc Share (world/share.h): the hash an arrived lot's save is known by, and the characters a
// lot's save carries of its own (Sims made on the lot: a baby grown up, a pet from the pen), read
// without loading the lot.
uint32_t share_hash(const uint8_t *data, size_t size);
bool read_save_characters(const uint8_t *data, size_t size, std::vector<MadeSim> &out);
// And the GUID of each object a lot's save has that isn't a person (the kinds of thing on the lot).
// `styles`: and the floor and wall files the house names, when build mode changed it (else they
// are the lot file's own).
bool read_save_kinds(const uint8_t *data, size_t size, std::vector<uint32_t> &out, std::vector<std::string> *styles = nullptr,
                     std::vector<std::pair<uint32_t, std::vector<int16_t>>> *objects = nullptr);   // (`objects`: each object's GUID and attributes)
// A tombstone or urn (an object whose code has the game's "set ghost neighbor id": the three urns,
// whose attribute 1 their ghost and "show death dialog" read as a neighbour id): that attribute,
// else -1. What the exe's "import lot" sets to 0 on a lot from elsewhere (Ad hoc Share translates it).
int ghost_attribute(Content &content, const ObjectInfo &info);
bool load_hood(Neighborhood &hood, const uint8_t *data, size_t size, std::string &error);

// Superstar's fame (the exe's fame manager, FUN_00593740: fame.iff read once). Each table has an entry
// a level (0..10, person data 81, half stars): the fame points (person data 80) a level needs (STR# 5),
// the friends' star power (STR# 4), the six skills' points (STR# 6; cooking, mechanical, charisma,
// body, logic, creativity: person data kFameSkills), and the level's title (STR# 1, 1..11).
struct FameTables {
    std::vector<int> points, star_power;
    std::vector<std::array<int, 6>> skills;
    std::vector<std::string> titles;
};
inline constexpr int kFameSkills[6] = { 10, 12, 11, 17, 18, 15 };   // the exe's table at 0x990f04
const FameTables &fame_tables(Content &content);
// Friends' star power of the neighbour `id`, in half stars (the exe's FUN_0054b460; shown halved):
// the fame levels of the neighbours in a family (person data 61, not 0) other than theirs, whose
// daily relationship each way is at least the tuning's "friendship threshold" (50).
int friends_star_power(const Vm &vm, int16_t id);
// The skill points the next fame level still asks of a person (0 where had; FUN_005925a0).
std::array<int, 6> fame_skills_needed(Content &content, const std::vector<int16_t> &person_data);
// Generic calls 34 and 36 (FUN_00593b70, FUN_00591ee0): up a level when the points, the skills and
// the friends' star power are there (person data 82, the best reached, follows), down one when the
// points fall under the level below's. Whether the level changed.
bool fame_promote(Vm &vm, Entity &person);
bool fame_demote(Vm &vm, Entity &person);

// The phone book: the Sims `sim` knows, not counting the family (neighbour id, name).
std::vector<std::pair<int16_t, std::string>> phone_book(Vm &vm, const Entity &sim);

// The router's view of tiles (x0, y0)-(x1, y1) of a storey, as text (for debugging).
std::string route_map(Vm &vm, const Entity &walker, int level, int x0, int y0, int x1, int y1);

// A way for `walker` to the spot (sixteenths of a tile) on its storey,
// as waypoints ending at the spot; false if there is none. The tiles of
// `target` (and its other parts) don't block: Sims step onto what they use.
// Whether an object keeps people off its tile, as the router has it (not doors, windows, rugs
// and the like that may be walked on, or markers with no extent).
bool blocks_walking(const Entity &e);
// A Sim standing at (px, py) sixteenths would have the exe's box (3/16 either way) across a wall.
bool person_box_hits_wall(const Vm &vm, const Entity &sim, int px, int py, int level);
// A ghost, as the exe tests it everywhere (person data 68 bit 1, or 84 bit 1 for a living ghost,
// FUN_0053a470 and the router): it plans its walks through everything on its storey.
inline bool is_ghost(const Entity &e)
{
    return e.person && ((e.person_data.size() > 68 && (e.person_data[68] & 1)) ||
                        (e.person_data.size() > 84 && (e.person_data[84] & 1)));
}
// How far the walker has to walk (in tiles, by the router's own steps) to each of `tiles` on its
// storey, in one search from where it stands: 1e30 where it can't get to. For choosing among a
// slot's spots by walking cost and score, as the original's planner does (FUN_0060e690).
std::vector<float> walk_costs(Vm &vm, const Entity &walker, const std::vector<std::pair<int, int>> &tiles,
                              const Entity *target = nullptr);
// The same from tile (sx, sy) on `level` (where the walker isn't): how far to each of `tiles` there.
std::vector<float> walk_costs_from(Vm &vm, const Entity &walker, int level, int sx, int sy,
                                   const std::vector<std::pair<int, int>> &tiles, const Entity *target = nullptr);
bool find_path(Vm &vm, const Entity &walker, int16_t to_x16, int16_t to_y16,
               std::vector<std::pair<int16_t, int16_t>> &out, const Entity *target = nullptr);

} // namespace ts::vm
