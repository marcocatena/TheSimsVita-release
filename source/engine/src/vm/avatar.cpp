/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (SimsVille/SimsAntics: model/VMAnimationState.cs and
 * the animation update in entities/VMAvatar.cs).
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/env.h"
#include "vm/vm.h"

namespace ts::vm {

namespace {

constexpr float kMsPerFrame = 1000.0f / 30.0f;

} // namespace

AnimState::AnimState(const CharAnimation *a, bool back) : anim(a), backwards(back)
{
    if (backwards) {
        frame = frames();
        next_event = anim ? anim->events.size() : 0;
    }
}

float AnimState::frames() const
{
    if (!anim)
        return 1;
    uint32_t most = 0;
    for (const AnimMotion &m : anim->motions)
        most = std::max(most, m.frames);
    if (most == 0)
        most = uint32_t(anim->duration / kMsPerFrame);
    return float(std::max<uint32_t>(most, 1));
}

void Entity::tick_animations(Vm *vm)
{
    for (size_t i = 0; i < animations.size(); i++) {
        AnimState &s = animations[i];
        if (s.end_reached || !s.anim)
            continue;
        s.frame += s.backwards ? -s.speed : s.speed;
        const float now = s.frame * kMsPerFrame;
        const auto &events = s.anim->events;
        auto fire = [&](const AnimEvent &e) {
            if (const std::string *v = e.get("xevt")) {
                if (i == 0) {   // events go to the code waiting on the current animation
                    const int16_t code = int16_t(std::atoi(v->c_str()));
                    if (TS_ENV("TS_ANIM_EVENTS")) {
                        std::printf("[anim] #%d %s at %u ms (frame %.0f):", id, s.anim->name.c_str(), e.time_ms, s.frame);
                        for (const auto &[key, value] : e.properties)
                            std::printf(" %s=%s", key.c_str(), value.c_str());
                        std::printf("\n");
                    }
                    if (s.waiting)
                        s.event_queue.push_back(code);
                    else if (TS_ENV("TS_ANIM_EVENTS"))
                        std::printf("[anim] dropped: %s xevt %d\n", s.anim->name.c_str(), code);
                    if (code < 100)   // 100 and up are cues (sounds): not among the expected events (FreeSO)
                        s.events_run++;
                }
            }
            // A sound with the animation (footsteps, a grunt), as the game plays them (FreeSO
            // VMAvatar.HandleTimePropsEvent): the event by name, with the Sim's variables.
            if (const std::string *v = e.get("sound"); v && vm && !v->empty()) {
                Vm::SoundEvent se;
                se.name = *v;
                se.source = uint16_t(id);
                person_sound_vars(*this, se);
                if (TS_ENV("TS_SOUND_EVENTS"))
                    std::printf("tick %d sound %s from #%d (animation %s)\n", int(vm->ticks), se.name.c_str(), int(id),
                                s.anim->name.c_str());
                if (vm->sounds.size() < 256)
                    vm->sounds.push_back(std::move(se));
            }
            // A foot coming down: the floor's step (FreeSO plays none; the game picks by floor).
            if (const std::string *v = e.get("footstep"); v && vm && person)
                if (const char *step = vm->footstep_sound(*this)) {
                    Vm::SoundEvent se;
                    se.name = step;
                    se.source = uint16_t(id);
                    person_sound_vars(*this, se);
                    if (TS_ENV("TS_SOUND_EVENTS"))
                        std::printf("tick %d sound %s from #%d (footstep at %d,%d level %d)\n", int(vm->ticks), step, int(id),
                                    tile_x(), tile_y(), int(level));
                    if (vm->sounds.size() < 256)
                        vm->sounds.push_back(std::move(se));
                }
            // The censor's parts, set by the animation at its own moments (getting into the bath,
            // undressing): the original puts the value in person data 30, the censorship flags
            // (its animation events, 0x5cf520, beside the hands).
            if (const std::string *v = e.get("censor"); v && person) {
                person_data[30] = int16_t(std::atoi(v->c_str()));
                if (TS_ENV("TS_ANIM_EVENTS"))
                    std::printf("[anim] #%d %s censor %s\n", id, s.anim->name.c_str(), v->c_str());
            }
            if (const std::string *v = e.get("righthand"))
                right_hand = std::atoi(v->c_str());
            if (const std::string *v = e.get("lefthand"))
                left_hand = std::atoi(v->c_str());
        };
        if (!s.backwards) {
            while (s.next_event < events.size() && float(events[s.next_event].time_ms) <= now)
                fire(events[s.next_event++]);
        } else {
            while (s.next_event > 0 && float(events[s.next_event - 1].time_ms) >= now)
                fire(events[--s.next_event]);
        }
        const float length = s.frames();
        const bool done = s.backwards ? s.frame < 0 : s.frame >= length;
        if (done) {
            if (s.loop) {   // round again: its events again, from the start (FreeSO fires them every loop)
                s.frame += s.backwards ? length : -length;
                const float again = s.frame * kMsPerFrame;
                if (!s.backwards) {
                    s.next_event = 0;
                    while (s.next_event < events.size() && float(events[s.next_event].time_ms) <= again)
                        fire(events[s.next_event++]);
                } else {
                    s.next_event = events.size();
                    while (s.next_event > 0 && float(events[s.next_event - 1].time_ms) >= again)
                        fire(events[--s.next_event]);
                }
            } else {
                s.end_reached = true;
            }
        }
    }
}

} // namespace ts::vm

namespace ts::vm {

// ---------------------------------------------------------------- slots and positions

const std::vector<SlotItem> &Entity::slot_items(Vm &vm)
{
    (void)vm;
    if (!slots_loaded_) {
        slots_loaded_ = true;
        if (info && code.own)
            if (const IffChunk *c = code.own->iff.find("SLOT", info->objd.get(Objd::SlotID))) {
                std::string err;
                parse_slot(code.own->body(*c), c->size, slot_items_, err);
            }
    }
    return slot_items_;
}

void Entity::leave_container()
{
    if (!container)
        return;
    for (Entity *&s : container->slots)
        if (s == this)
            s = nullptr;
    // Out of a Sim's hands: the carrying arm let go with it (FreeSO VMAvatar.ClearSlot; the pot
    // put on the stove left Bella's arm up, the user saw, 2026-10-30).
    if (container->person && container_slot == 0)
        container->carry.reset();
    container = nullptr;
    container_slot = -1;
}

void Entity::place_in_slot(Entity *obj, int slot)
{
    if (!obj || obj == this || slot < 0 || slot > 64)
        return;
    obj->leave_container();
    if (slots.size() <= size_t(slot))
        slots.resize(size_t(slot) + 1, nullptr);
    if (slots[size_t(slot)] && slots[size_t(slot)] != obj)
        slots[size_t(slot)]->leave_container();
    slots[size_t(slot)] = obj;
    obj->container = this;
    obj->container_slot = int16_t(slot);
    obj->x16 = x16;
    obj->y16 = y16;
    obj->level = level;
    // Into a Sim's hands: the carrying arm (FreeSO VMAvatar.PlaceInSlot), unless a carry pose of
    // the code's own is on.
    if (person && slot == 0)
        carry_off = false;
    if (person && slot == 0 && !carry && thread)
        if (const CharAnimation *a = thread->vm_ref().animation("a2o-rarm-carry-loop"))
            carry = std::make_unique<AnimState>(a, false);
}

void Entity::set_position(int16_t nx, int16_t ny, int8_t nlevel)
{
    if (const char *watch = TS_ENV("TS_POS_WATCH"); watch && std::atoi(watch) == id && (nx != x16 || ny != y16))   // debug
        std::printf("pos: #%d set (%d, %d) -> (%d, %d)\n", int(id), int(x16), int(y16), int(nx), int(ny));
    leave_container();
    x16 = nx;
    y16 = ny;
    level = nlevel;
    for (Entity *s : slots)   // what it holds goes with it
        if (s) {
            s->x16 = nx;
            s->y16 = ny;
            s->level = nlevel;
        }
}

// A part's offset in its object, turned with it (FreeSims VMMultitileGroup).
void part_offset(const Entity &e, int direction, int &dx, int &dy)
{
    const uint16_t sub = e.info ? e.info->objd.get(Objd::SubIndex) : 0;
    const int ox = sub == 0xffff ? 0 : int8_t(sub >> 8), oy = sub == 0xffff ? 0 : int8_t(sub & 0xff);
    dx = ox;
    dy = oy;
    switch ((direction / 2) % 4) {
    case 1: dx = -oy; dy = ox; break;
    case 2: dx = -ox; dy = -oy; break;
    case 3: dx = oy; dy = -ox; break;
    default: break;
    }
}

void Entity::place_whole(int16_t nx, int16_t ny, int8_t nlevel, int dir)
{
    direction = dir & 7;
    if (group.empty()) {
        set_position(nx, ny, nlevel);
        return;
    }
    int mx, my;
    part_offset(*this, direction, mx, my);
    const int my_level_offset = info ? int16_t(info->objd.get(Objd::LevelOffset)) : 0;
    for (Entity *part : group) {
        int px, py;
        part_offset(*part, direction, px, py);
        const int level_offset = part->info ? int16_t(part->info->objd.get(Objd::LevelOffset)) : 0;
        part->direction = direction;
        part->set_position(int16_t(nx + (px - mx) * 16), int16_t(ny + (py - my) * 16),
                           int8_t(nlevel + level_offset - my_level_offset));
    }
}

// ---------------------------------------------------------------- walking

namespace {

// The walk as the original paces it (FreeSO VMRoutingFrame / VMPathLineSegment): a segment
// takes "frames" per sixteenth of a tile, the Sim gets through `velocity` of them a tick,
// velocity going up by one a tick from 1 to 8 when setting off and back down to 1 over the
// last 28 frames of the walk. FreeSO's ten frames a sixteenth are for its 30 ticks a second;
// at the original's 24, eight keep the Legacy Collection's pace (a sixteenth of a tile a tick
// at full stride, 1.5 tiles a second; its walk timed on the Goths' lawn, 2026-10-26).
constexpr float kFramesPerSixteenth = 8.0f;
constexpr int kTopVelocity = 8, kTailOffFrames = 28;
constexpr float kUnitsPerTile = 3;

int direction_of(float dx, float dy)
{
    const float angle = std::atan2(dx, -dy);   // 0 = north (-y), clockwise
    return (int(std::lround(angle / (3.14159265f / 4))) + 8) % 8;
}

} // namespace

void Entity::tick_head(Vm &vm)
{
    // FreeSO's VMAvatar.UpdateHeadSeek, on the person data the original's head seek sets
    // (FUN_005d0090: 41 the object, 42 state 1, 43 0, 44 1, 45 0).
    constexpr float kMost = 1.15f, kStep = 0.12f;   // as far as the head turns either way, and how fast (a tick)
    if (person_data.size() <= 45)
        return;
    int16_t &state = person_data[42];
    float want = head_yaw;
    if (state == 1) {
        const Entity *target = vm.entity(person_data[41]);
        if (!target || target->dead || !target->in_world() || !in_world() || target->level != level) {
            state = 4;
        } else {
            const float dx = float(target->x16 - x16), dy = float(target->y16 - y16);
            if (dx != 0 || dy != 0) {
                const float to = std::atan2(dx, -dy);   // 0 = north (-y), clockwise
                const float body = (heading >= 0 ? heading : float(direction)) * 3.14159265f / 4;
                const float off = std::remainder(to - body, 2 * 3.14159265f);   // + clockwise
                want = std::clamp(-off, -kMost, kMost);   // the renderer's + is to the left
            }
            if (person_data[45] > 0 && --person_data[45] == 0)
                state = 4;
        }
    }
    if (state == 4 || state == 0 || state == 8)
        want = 0;
    head_yaw += std::clamp(want - head_yaw, -kStep, kStep);
    if (state == 4 && head_yaw == 0) {
        state = 8;
        person_data[43] = 0;
    }
}

void Entity::tick_route(Vm &vm)
{
    if (route.state != Route::Walking || route.turning || route.next >= route.path.size())
        heading = -1;   // not walking along: the body faces `direction` itself
    if (route.state != Route::Walking)
        return;
    if (route.turning) {   // turning on the spot: its animation turns the body; it faces the new way after
        const AnimState *turn = current_animation();
        if (turn && turn->anim && !turn->end_reached)
            return;
        direction = route.turn_to;
        const int was = route.turning;
        route.turning = 0;
        if (was == 1) {   // set off now
            start_walk(vm);
            return;
        }
    }
    if (route.next >= route.path.size()) {
        // To face the way asked, a turn on the spot first (FreeSO EndWalk / END_TURN).
        if (person && route.face >= 0 && route.face != direction && turn_towards(vm, route.face, 2))
            return;
        route.state = Route::Arrived;
        if (route.entry_object && person) {
            const Entity &obj = *route.entry_object;
            const auto [px, py] = route.path.size() >= 2 ? route.path[route.path.size() - 2]
                                                         : std::make_pair(route.from_x, route.from_y);
            const float ax = float(px - obj.x16), ay = float(py - obj.y16);
            if (ax != 0 || ay != 0) {
                // The side, turned with the object, to the nearest quarter.
                const float angle = std::atan2(ax, -ay) / (3.14159265f / 2);   // quarters from north, clockwise
                const int quarter = (int(std::lround(angle)) + 4) % 4;
                const int rel = (quarter * 2 - obj.direction + 16) % 8;
                person_data[37] = int16_t(1 << (((rel + 1) / 2 * 2) % 8));
            }
            route.entry_object = nullptr;
        }
        if (route.face >= 0)
            direction = route.face;
        // Back to standing.
        animations.clear();
        const std::vector<std::string> &moves = vm.moves(*this);   // treading water in a pool
        if (moves.size() > 3)
            if (const CharAnimation *a = vm.animation(moves[3])) {
                animations.emplace_back(a, false);
                animations.back().loop = true;
            }
        return;
    }
    if (std::lround(route.fx) != x16 || std::lround(route.fy) != y16) {   // set (or moved) elsewhere
        route.fx = float(x16);
        route.fy = float(y16);
        route.seg_set = false;
        route.to_corner = false;
        route.corner_used = false;
    }
    // The exe's walker (walk_step) unless TS_WALK_OLD=1: FreeSO's, below, stopped and turned at
    // every corner (the user, 2026-11-10: "they continue walking and turning" in the original).
    static const bool walk_old = TS_ENV("TS_WALK_OLD") != nullptr;
    if (!walk_old) {
        walk_step(vm);
        return;
    }
    // How far is left of the whole walk, in frames: setting off speeds up, the end slows down.
    float left = 0, px = route.fx, py = route.fy;
    for (size_t k = route.next; k < route.path.size(); k++) {
        left += std::hypot(float(route.path[k].first) - px, float(route.path[k].second) - py);
        px = float(route.path[k].first);
        py = float(route.path[k].second);
    }
    if (left * kFramesPerSixteenth <= float(kTailOffFrames)) {
        if (route.velocity <= 0)
            route.velocity = 1;
        if (route.velocity > 1)
            route.velocity--;
    } else if (route.velocity < kTopVelocity) {
        route.velocity++;
    }
    const float speed = float(route.velocity) / kFramesPerSixteenth * route.stride_scale;   // sixteenths this tick
    auto [tx, ty] = route.path[route.next];
    const float dx = float(tx) - route.fx, dy = float(ty) - route.fy;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (route.paused)
        return;   // asking someone to move, waiting
    // Another Sim where this step would go (the original's walk update tests the next spot
    // against every Sim's box, 6 sixteenths across): the Sim stays put, and the walking
    // instruction deals with it (plans again, asks them to move, waits, or gives up).
    if (person) {
        const float nx = dist <= speed ? float(tx) : route.fx + dx / dist * speed;
        const float ny = dist <= speed ? float(ty) : route.fy + dy / dist * speed;
        if (const Entity *other = vm.person_in_the_way(*this, nx, ny)) {
            route.blocked_by = int16_t(other->id);
            route.velocity = 0;
            return;
        }
    }
    route.blocked_by = 0;
    route.yield_start = 0;   // (the way is clear: giving way is over)
    route.walk_frames += kWalkAnimSpeed;
    if (dist <= speed) {
        route.fx = float(tx);
        route.fy = float(ty);
        route.next++;
    } else {
        route.fx += dx / dist * speed;
        route.fy += dy / dist * speed;
        // The body turns towards the way it goes by at most 0.25 radians a tick, the walk slowing
        // through a sharp corner and stopping for one past a right angle (FreeSO VMRoutingFrame).
        constexpr float kEighth = 3.14159265f / 4, kMostTurn = 0.25f;
        const float want = std::atan2(dx, -dy);   // 0 = north (-y), clockwise
        const float was = (heading >= 0 ? heading : float(direction)) * kEighth;
        float diff = std::remainder(want - was, 2 * 3.14159265f);
        const float size = std::fabs(diff);
        if (size > kMostTurn) {
            diff = diff > 0 ? kMostTurn : -kMostTurn;
            if (size > 0.5f && route.velocity > 4)
                route.velocity -= 2;
            if (size > 1.0f && route.velocity > 2)
                route.velocity -= 2;
            if (size > 3.14159265f / 2 + 0.01f)
                route.velocity = 1;
        }
        heading = std::fmod((was + diff) / kEighth + 8.0f, 8.0f);
        direction = int(std::lround(heading)) % 8;
    }
    if (const char *w = TS_ENV("TS_WALK_TRACE"); w && std::atoi(w) == id)   // debug: the walk tick by tick
        std::printf("walk #%d tick %u: (%.2f, %.2f) heading %.2f velocity %d, to point %zu/%zu (%.2f, %.2f)\n", int(id),
                    unsigned(vm.ticks), route.fx / 16, route.fy / 16, heading, route.velocity, route.next, route.path.size(),
                    route.next < route.path.size() ? route.path[route.next].first / 16.0 : -1.0,
                    route.next < route.path.size() ? route.path[route.next].second / 16.0 : -1.0);
    if (const char *watch = TS_ENV("TS_POS_WATCH"); watch && std::atoi(watch) == id &&
        (x16 != int16_t(std::lround(route.fx)) || y16 != int16_t(std::lround(route.fy))))   // debug: who moves this Sim
        std::printf("pos: #%d walks (%d, %d) -> (%ld, %ld), route state %d\n", int(id), int(x16), int(y16), std::lround(route.fx),
                    std::lround(route.fy), int(route.state));
    x16 = int16_t(std::lround(route.fx));
    y16 = int16_t(std::lround(route.fy));
    for (Entity *s : slots)
        if (s) {
            s->x16 = x16;
            s->y16 = y16;
        }
}

// The exe's walker (FUN_005d33b0, read 2026-11-10; its states 9 walk / 10 run): the Sim heads for a
// point on its path look_radius ahead (the path sampled a sixteenth at a time from where the Sim
// stands on its segment, each sample rounded to whole sixteenths), moves towards that point at its
// walk's own pace, turns its body towards it by at most 0.2 of an eighth (9 degrees) a tick, and only
// stops to turn on the spot when the point is a quarter turn (2 eighths) or more off its heading
// (pets: 0.8 of an eighth, 0.057 a tick). No easing in or out and no slowing at corners: the corner is
// a small arc where the body catches up. Path points closer than the radius are passed at once; on
// the last segment the point is the destination itself.
bool Entity::walk_aim(float px, float py, float &lx, float &ly)
{
    const float r = route.look_radius > 0 ? route.look_radius : 5.43f;
    if (route.next >= route.path.size())
        return false;
    if (!route.seg_set) {
        route.seg_x = px;
        route.seg_y = py;
        route.seg_set = true;
    }
    auto pt = [&](size_t k) { return std::make_pair(float(route.path[k].first), float(route.path[k].second)); };
    while (route.next + 1 < route.path.size()) {   // points within the radius: passed
        const auto [nx, ny] = pt(route.next);
        if (std::hypot(nx - px, ny - py) >= r)
            break;
        route.seg_x = nx;
        route.seg_y = ny;
        route.next++;
    }
    if (route.next + 1 >= route.path.size()) {   // the last segment: the destination
        std::tie(lx, ly) = pt(route.next);
        return true;
    }
    // Where the Sim is on its segment, then along the path a sixteenth at a time.
    auto [ex, ey] = pt(route.next);
    float sx = ex - route.seg_x, sy = ey - route.seg_y;
    const float len2 = sx * sx + sy * sy;
    float t = len2 > 0 ? ((px - route.seg_x) * sx + (py - route.seg_y) * sy) / len2 : 1.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    float cx = route.seg_x + sx * t, cy = route.seg_y + sy * t;
    size_t k = route.next;
    for (int guard = 0; guard < 4096; guard++) {
        const float rx = std::round(cx), ry = std::round(cy);
        if (std::hypot(rx - px, ry - py) >= r) {
            lx = rx;
            ly = ry;
            return true;
        }
        auto [tx, ty] = pt(k);
        const float d = std::hypot(tx - cx, ty - cy);
        if (d <= 1.0f) {
            cx = tx;
            cy = ty;
            if (++k >= route.path.size()) {
                lx = tx;
                ly = ty;
                return true;
            }
        } else {
            cx += (tx - cx) / d;
            cy += (ty - cy) / d;
        }
    }
    std::tie(lx, ly) = pt(route.path.size() - 1);
    return true;
}

void Entity::walk_step(Vm &vm)
{
    constexpr float kPi = 3.14159265f;
    float lx = 0, ly = 0;
    if (!walk_aim(route.fx, route.fy, lx, ly))
        return;
    // Going back to the corner passed (a cut corner was blocked by a Sim, below): there first, then on
    // along the path itself. (Half a step back and on again each tick, two guests at a booth rocked in
    // place for hours.)
    bool corner = false;
    if (route.to_corner) {
        if (std::hypot(route.seg_x - route.fx, route.seg_y - route.fy) <= 0.5f) {
            route.to_corner = false;
        } else {
            lx = route.seg_x;
            ly = route.seg_y;
            corner = true;
        }
    }
    const bool pet = person && person_data.size() > 65 && person_data[65] >= 2;
    const float stop_turn = pet ? 0.8f : 2.0f, most_turn = pet ? 0.057f : 0.2f;   // eighths (0xa51440, 0xa51444)
    float dx = lx - route.fx, dy = ly - route.fy;
    float dist = std::sqrt(dx * dx + dy * dy);
    const bool last = !corner && route.next + 1 >= route.path.size();
    // A destination a few sixteenths off (under 3.1): no walk and no turn for it. The exe side-steps
    // there (the walk table's standing adjusts, 11-18) or, under half a sixteenth, snaps; ours snaps
    // (it turned right round to take one sixteenth's step: Mortimer's 180-degree turns, 2026-11-10).
    if (last && dist < 3.1f && route.walk_frames == 0) {   // (the whole walk that short, not a walk's end)
        route.fx = lx;
        route.fy = ly;
        route.next = route.path.size();
        route.velocity = 0;
        x16 = int16_t(std::lround(route.fx));
        y16 = int16_t(std::lround(route.fy));
        for (Entity *s : slots)
            if (s) {
                s->x16 = x16;
                s->y16 = y16;
            }
        return;
    }
    if (dist > 0.01f) {
        const float was = heading >= 0 ? heading : float(direction);
        const float want = std::fmod(std::atan2(dx, -dy) / (kPi / 4) + 8.0f, 8.0f);   // 0 north, clockwise, eighths
        float diff = std::remainder(want - was, 8.0f);
        if (person && std::fabs(diff) >= stop_turn && !(last && dist <= route.stride_scale)) {
            // A quarter turn or more: stop and turn on the spot (the walk table's standing turns).
            if (turn_towards(vm, int(std::lround(want)) % 8, 1)) {
                route.velocity = 0;
                return;
            }
        }
        diff = std::clamp(diff, -most_turn, most_turn);
        heading = std::fmod(was + diff + 8.0f, 8.0f);
        direction = int(std::lround(heading)) % 8;
    }
    if (route.paused)
        return;   // asking someone to move, waiting
    // The step: the walk loop's stride at its rate (an adult's a sixteenth a tick), the walker's own.
    const float step = route.stride_scale;
    float nx, ny;
    bool arrived = false;
    if (dist <= step) {
        nx = lx;
        ny = ly;
        arrived = last;
    } else {
        nx = route.fx + dx / dist * step;
        ny = route.fy + dy / dist * step;
    }
    // Another Sim where this step goes (the original tests every Sim's box): the Sim stays put, and
    // the walking instruction deals with it.
    // Unless the step is blocked only because the look-ahead cut a corner: the plan's corner just
    // passed (within the radius) is where the way goes round that Sim, and the cut walked into them,
    // planned again the same way, for ever (lot 26's diner, 2026-11-14: Lisa an inch from the waiter
    // waiting in the aisle to seat her, her path round him (183,210) -> (178,210) -> (178,222) cut
    // straight at (178,222); she never sat, he never stopped waiting, the other staff jammed behind
    // them). Then the step goes to that corner instead, if that's clear, and on to it (to_corner);
    // once a plan (corner_used): every time, a Sim boxed in turned round on the spot for hours, never
    // counted as blocked. Survey of 81 lots, 30,000 ticks each (with the planner's people margin,
    // routing.cpp): failed walks 13,638 -> 2,222, "gives up" 5,635 -> 331. TS_CORNER_OLD=1: as before.
    if (person)
        if (const Entity *other = vm.person_in_the_way(*this, nx, ny)) {
            static const bool corner_old = TS_ENV("TS_CORNER_OLD") != nullptr;
            bool to_corner = false;
            if (!corner_old && !corner && !route.corner_used && route.seg_set && route.next > 0) {
                const float cx = route.seg_x - route.fx, cy = route.seg_y - route.fy;
                const float cd = std::hypot(cx, cy);
                if (cd > 0.5f) {
                    const float sx = cd <= step ? route.seg_x : route.fx + cx / cd * step;
                    const float sy = cd <= step ? route.seg_y : route.fy + cy / cd * step;
                    if (!vm.person_in_the_way(*this, sx, sy)) {
                        nx = sx;
                        ny = sy;
                        arrived = false;
                        to_corner = true;
                        route.to_corner = cd > step;   // (the rest of the way back on the next ticks)
                        route.corner_used = true;
                    }
                }
            }
            if (!to_corner) {
                route.blocked_by = int16_t(other->id);
                route.velocity = 0;
                return;
            }
        }
    route.blocked_by = 0;
    route.yield_start = 0;
    route.walk_frames += kWalkAnimSpeed;
    route.fx = nx;
    route.fy = ny;
    if (arrived)
        route.next = route.path.size();
    // The legs (the viewer's walk/stand mix, 0-8): full while walking; over the last half stride the
    // walk weighted out towards max(0.2, what's left over the half stride), as the exe's foot-plant stop.
    const float half = 2.0f * (route.look_radius > 0 ? route.look_radius : 5.43f);
    const float left = last ? std::hypot(lx - nx, ly - ny) : half;
    route.velocity = left >= half ? 8 : std::clamp(int(std::lround(8.0f * std::max(0.2f, left / half))), 1, 8);
    if (const char *w = TS_ENV("TS_WALK_TRACE"); w && std::atoi(w) == id)   // debug: the walk tick by tick
        std::printf("walk #%d tick %u: (%.2f, %.2f) heading %.2f, aim (%.2f, %.2f), point %zu/%zu\n", int(id), unsigned(vm.ticks),
                    route.fx / 16, route.fy / 16, heading, lx / 16, ly / 16, route.next, route.path.size());
    x16 = int16_t(std::lround(route.fx));
    y16 = int16_t(std::lround(route.fy));
    for (Entity *s : slots)
        if (s) {
            s->x16 = x16;
            s->y16 = y16;
        }
}

bool Entity::turn_towards(Vm &vm, int to, int why)
{
    // FreeSO VMRoutingFrame.Turn: the walk table's standing turns, 180 (4, 5), 90 (6, 7) and 45
    // degrees (8, 9), clockwise first; a turn of 135 plays the 90 and the rest is snapped at its end.
    const int diff = ((to - direction) % 8 + 12) % 8 - 4;   // -4..3 eighths, + clockwise
    const int steps = std::abs(diff);
    if (steps == 0 || to < 0)
        return false;
    const int off = diff > 0 || steps == 4 ? 0 : 1;
    const int index = (steps == 4 ? 4 : steps >= 2 ? 6 : 8) + off;
    const std::vector<std::string> &moves = vm.moves(*this);
    const CharAnimation *a = size_t(index) < moves.size() && !moves[size_t(index)].empty() ? vm.animation(moves[size_t(index)]) : nullptr;
    if (!a)
        return false;
    animations.clear();
    animations.emplace_back(a, false);
    // At the walker's rate, as the walk loop (the exe sets every walker animation's rate from
    // FUN_005cf460 each tick, the standing turns' too: 0x5d56a1). Ours played turns at 1 frame a
    // tick, twice as long: a 90-degree turn at a door 47 ticks where the exe's takes ~22, so Sims
    // seemed to stop and face each door (the user, 2026-11-10; the turn itself is the door's own
    // "goto door and face thru"). TS_TURN_SPEED_OLD=1: as before.
    static const bool turn_old = TS_ENV("TS_TURN_SPEED_OLD") != nullptr;
    if (!turn_old)
        animations.back().speed = kWalkAnimSpeed;
    route.turning = why;
    route.turn_to = to;
    if (TS_ENV("TS_TURNS"))   // debug: each turn on the spot
        std::printf("tick %u turn %s %s: %d -> %d (%s)\n", unsigned(vm.ticks), info ? info->label.c_str() : "?",
                    a->name.c_str(), direction, to,
                    why == 1 ? "setting off" : "arriving");
    return true;
}

void Entity::start_walk(Vm &vm)
{
    animations.clear();
    const std::vector<std::string> &moves = vm.moves(*this);   // swimming in a pool
    // Running (walk style, object data 17, 1: set by the game's code, 532 places: to the toilet at
    // bladder -80, pets chasing, children playing, the wagon): the walk table's run loop (21, as
    // FreeSO), not in a pool. The exe plays it at the walk's rate (FUN_005cf460: 1.2 + 0.2 a step,
    // 1.4 in the walking state 3; the x8 is outside routing), its stride giving the pace: an adult
    // runs at 4.1 times their walk, a child 3.1. Ours always walked. TS_RUN_OLD=1: as before.
    static const bool run_old = TS_ENV("TS_RUN_OLD") != nullptr;
    size_t loop = 20;
    if (!run_old && person && value(VarWalkStyle) == 1 && !vm.in_pool(*this) && moves.size() > 21 && !moves[21].empty())
        loop = 21;
    // (The run loop not read yet, where things are read in the background: this walk walks, and the
    // run comes next time. Its 670 KB read on the spot was a 90 ms tick on the Vita.)
    if (loop == 21 && vm.animation_waits(moves[21]))
        loop = 20;
    if (moves.size() > loop)
        if (const CharAnimation *a = vm.animation(moves[loop])) {
            if (TS_ENV("TS_WALK"))   // debug: the walk loop's own speed
                std::printf("walk %s: %.3f over %.0f ms, moving %d, %u translations\n", a->name.c_str(), a->distance,
                            a->duration, int(a->moving), unsigned(a->translations.size()));
            animations.emplace_back(a, false);
            animations.back().loop = true;
            animations.back().speed = kWalkAnimSpeed;
            // The pace by this walk loop's stride (Route::stride_scale). TS_STRIDE_OLD=1: every
            // walker at the adult's, as before.
            static const bool stride_old = TS_ENV("TS_STRIDE_OLD") != nullptr;
            constexpr float kAdultStridePerMs = 4.070f / 1266.0f;
            route.stride_scale = 1.0f;
            // Pets (person data 65 >= 2) too, unless TS_STRIDE_NO_PETS=1. (First tried: at their own pace (a dog
            // 0.44 of an adult's, as measured) the pet shop's and visitors' pets clogged tiles and
            // doorways ("move from filled tile" failures x6 on the survey); what keeps the exe's from
            // it was the adults' scale rounding to 0.9999999, fixed below.)
            static const bool stride_pets = TS_ENV("TS_STRIDE_NO_PETS") == nullptr;   // (on: the rounding below was the clogging; survey +1.5% route failures with pets at their pace)
            const bool pet = person && person_data.size() > 65 && person_data[65] >= 2;
            if (!stride_old && (!pet || stride_pets) && a->moving && a->distance > 0 && a->duration > 0)
                route.stride_scale = std::clamp(a->distance / a->duration / kAdultStridePerMs, 0.15f, 2.0f);
            if (std::fabs(route.stride_scale - 1.0f) < 0.01f)
                route.stride_scale = 1.0f;   // (the adult's own: exactly, its rounding moved every walk a hair)
            // The look-ahead radius (walk_aim): half of half the loop's stride, in sixteenths (the exe's
            // [0x83] x 0.5, FUN_005d2890: an adult's 4.07 units x 16/3 / 4 = 5.4).
            route.look_radius = a->moving && a->distance > 0 ? a->distance * 16.0f / 3.0f / 4.0f : 5.43f;
        }
}

} // namespace ts::vm
