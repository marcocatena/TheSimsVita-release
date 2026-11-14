/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSO (tso.files/HIT: Hot.cs, HITFile.cs, HSM.cs, Hitlist.cs,
 * Patch.cs; tso.sound: HITVM.cs, HITThread.cs, HITInterpreter.cs,
 * Events/HITTVOn.cs; tso.content/TS1/TS1Audio.cs).
 */
#include "core/env.h"
#include "core/memory_kind.h"
#include "audio/hit.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

#include "files/content.h"
#include "files/install.h"

namespace ts::hit {

namespace {

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(uint8_t(c)));
    return s;
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(uint8_t(s[a])))
        a++;
    while (b > a && std::isspace(uint8_t(s[b - 1])))
        b--;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trim(cur));
    return out;
}

// "SoundData\voice\x.xa" -> "x.xa", as the content knows files.
std::string base_name(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return lower(slash == std::string::npos ? path : path.substr(slash + 1));
}

// A station's four-letter id as the game writes it, from its number (FreeSO IIS).
std::string station_id(uint32_t id)
{
    std::string s;
    for (int i = 0; i < 4; i++)
        s += char((id >> (8 * i)) & 0xff);
    return s;
}

} // namespace

// ---------------------------------------------------------------- the tables

struct Track {
    uint32_t subroutine = 0;
    int volume = 1024;
    int duck = 0;    // ducking priority (variable 0x19)
    int group = 1;   // control group: 1 sound effects, 2 music, 3 voices
    uint32_t hitlist = 0, sound = 0;
};

struct Patch {
    std::string name, file;
    bool looped = false;
};

// One .hot table with its .hsm and .hit (FreeSO HITResourceGroup).
struct ResGroup {
    std::map<uint32_t, Track> tracks;
    std::map<uint32_t, Patch> patches;
    std::map<uint32_t, std::vector<uint32_t>> hitlists;
    std::map<uint32_t, uint32_t> track_data;   // id -> subroutine address
    std::map<std::string, int> hsm;            // folded symbol -> value
    std::vector<uint8_t> hit;
};

struct Event {
    int type = 0;
    uint32_t track = 0;
    size_t group = 0;
};

// A hit list, as the game keeps it (FUN_00687960): its entries ready, and those played lately, resting
// until half the list has been played since. One per list for the whole game, between plays: the
// study's cue picks from seven entries, four of them one sample, and comes back round in turn.
struct SmartList {
    std::vector<uint32_t> ready, rested;
    size_t limit = 0;
    bool fixed = false;
    void changed()
    {
        if (!fixed)
            limit = (ready.size() + rested.size()) / 2;
    }
    uint32_t choose(std::mt19937 &random)
    {
        uint32_t id = 0;
        if (!ready.empty()) {
            const size_t i = std::uniform_int_distribution<size_t>(0, ready.size() - 1)(random);
            id = ready[i];
            ready.erase(ready.begin() + std::ptrdiff_t(i));
            rested.push_back(id);
            while (rested.size() > limit) {
                ready.push_back(rested.front());
                rested.erase(rested.begin());
            }
        } else if (!rested.empty()) {
            id = rested.front();
            rested.erase(rested.begin());
            rested.push_back(id);
        }
        return id;
    }
};

constexpr size_t kAddedTables = 64;   // room for the tables of content added while the game runs

struct Engine::Data {
    std::map<std::pair<const ResGroup *, uint32_t>, SmartList> lists;
    std::map<int, uint32_t> seq_groups;   // a group's owner: a thread's serial (the game's table at +0x208)
    std::map<uint32_t, int> duckers;      // threads asking for a ducking level (variable 0x7b; FUN_00689830)
    uint32_t serial = 0;
    std::vector<ResGroup> groups;         // (room kept for added tables: threads and lists point into it)
    std::vector<std::string> tables;      // the .hot files read, by the index's names
    std::map<std::string, Event> events;   // folded name
    std::map<std::string, std::string> stations;   // station id -> folder (relative to the install)
    std::map<int, std::string> music_modes;
    std::string root;
};

namespace {

void read_hot(const std::string &text, ResGroup &g, size_t index, std::map<std::string, Event> &events)
{
    std::map<std::string, int> equates = {   // the event types, whatever a table defines
        { "ksndobplay", kSoundobPlay }, { "ksndobstop", kSoundobStop }, { "ksndobkill", kSoundobKill },
        { "ksndobcancelnote", kSoundobCancelNote }, { "kkillall", kKillAll }, { "kturnontv", kTurnOnTV },
        { "kturnofftv", kTurnOffTV }, { "ksetmusicmode", kSetMusicMode }, { "kplaypiano", kPlayPiano },
    };
    auto eme = [&](const std::string &s) -> int {
        if (s.empty())
            return 0;
        if (std::isdigit(uint8_t(s[0])) || s[0] == '-')
            return std::atoi(s.c_str());
        auto it = equates.find(lower(s));
        return it == equates.end() ? 0 : it->second;
    };
    auto hsm = [&](const std::string &s) -> int {
        auto it = g.hsm.find(lower(s));
        return it == g.hsm.end() ? 0 : it->second;
    };
    std::istringstream in(text);
    std::string line, section;
    std::vector<std::pair<uint32_t, std::vector<std::string>>> track_lines;   // after the track data
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';')
            continue;
        if (line[0] == '[') {
            section = lower(line);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (section == "[eventmappingequate]") {
            equates[lower(key)] = std::atoi(value.c_str());
        } else if (section == "[trackdata]") {
            g.track_data[uint32_t(std::strtoul(key.c_str(), nullptr, 16))] = uint32_t(std::strtoul(value.c_str(), nullptr, 16));
        } else if (section == "[eventmapping]") {
            const auto f = split(value, ',');
            Event e;
            e.type = eme(f[0]);
            e.track = f.size() > 1 ? uint32_t(eme(f[1])) : 0;
            e.group = index;
            events[lower(key)] = e;
        } else if (section == "[track]") {
            track_lines.emplace_back(uint32_t(std::atoi(key.c_str())), split(value, ','));
        } else if (section == "[patch]") {
            const auto f = split(value, ',');
            Patch p;
            if (f.size() > 1)
                p.name = f[1];
            if (f.size() > 2) {
                std::string file = f[2];
                if (file.size() >= 2 && file.front() == '"')
                    file = file.substr(1, file.size() - 2);
                p.file = base_name(file);
            }
            p.looped = f.size() > 3 && f[3] != "0";
            g.patches[uint32_t(std::atoi(key.c_str()))] = p;
        } else if (section == "[globalhitlist]") {
            std::vector<uint32_t> ids;
            bool ok = true;
            for (const std::string &part : split(value, ',')) {
                if (part.empty())
                    continue;
                if (!std::isdigit(uint8_t(part[0]))) {   // a few name hsm labels: skipped, as FreeSO
                    ok = false;
                    break;
                }
                const size_t dash = part.find('-');
                if (dash == std::string::npos) {
                    ids.push_back(uint32_t(std::atoi(part.c_str())));
                } else {
                    const uint32_t a = uint32_t(std::atoi(part.c_str())), b = uint32_t(std::atoi(part.c_str() + dash + 1));
                    for (uint32_t k = a; k <= b && ids.size() < 512; k++)
                        ids.push_back(k);
                }
            }
            if (ok)
                g.hitlists[uint32_t(std::atoi(key.c_str()))] = ids;
        }
    }
    // (track id)=0,(name),(volume),(args),(ducking),(group),(level),@(hitlist label),(sound id)
    for (auto &[id, f] : track_lines) {
        Track t;
        if (f.size() > 2)
            t.volume = eme(f[2]);
        if (f.size() > 4)
            t.duck = eme(f[4]);
        if (f.size() > 5)
            t.group = f[5] == "kGroupMusic" ? 2 : f[5] == "kGroupVox" ? 3 : 1;
        if (f.size() > 7 && f[7].size() > 1)
            t.hitlist = uint32_t(hsm(f[7].substr(1)));   // the label's value (case as the hsm has it, folded)
        if (f.size() > 8)
            t.sound = uint32_t(eme(f[8]));
        if (t.hitlist && g.track_data.count(t.hitlist))
            t.subroutine = g.track_data[t.hitlist];
        if (t.sound && g.track_data.count(t.sound))
            t.subroutine = g.track_data[t.sound];
        g.tracks[id] = t;
    }
}

} // namespace

// ---------------------------------------------------------------- a thread

// A running sound: a HIT program (FreeSO HITThread), a plain track with no
// program (simple mode), or a station (HITTVOn: a TV, a radio, the music).
struct Engine::Thread {
    Engine &e;
    const ResGroup *g = nullptr;
    std::string name;
    uint16_t source = 0;
    bool dead = false;
    // HIT.
    uint32_t pc = 0;
    int loop_pointer = -1, wait_remain = -1;
    bool simple = false, play_simple = false, loop = false, has_set_loop = false, thread_dead = false;
    int regs[16] = {}, local[54] = {}, vars[29] = {};
    std::vector<uint32_t> notes;   // mixer handles
    SmartList own;                // (hit list 0: the thread's own)
    SmartList *hitlist = &own;
    Track track;
    uint32_t track_id = 0, serial = 0;
    int note_off_ms = -1;         // a note with a length (instruction 1): the steps left
    uint32_t target = 0;          // variable 0x35: the track whose variables this one reaches
    const Patch *patch = nullptr;
    bool zero = false, sign = false;
    std::vector<uint32_t> stack;
    float volume = 1;
    int group = kFx;
    // A station.
    bool station = false, music = false;
    std::vector<std::string> files;
    size_t next = 0;
    uint32_t playing_stream = 0;
    int fade = -1;   // ticks left fading out

    Thread(Engine &engine) : e(engine), serial(++engine.data_->serial)
    {
        local[kTrackVolume - 0x10] = local[kVolume - 0x10] = 1024;
    }

    uint8_t byte() { return pc < g->hit.size() ? g->hit[pc++] : 0; }
    uint32_t u32()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++)
            v |= uint32_t(byte()) << (8 * i);
        return v;
    }
    uint32_t trans(uint32_t id) const   // a call id to its address (TrackData), TS1
    {
        auto it = g->track_data.find(id);
        return it == g->track_data.end() ? id : it->second;
    }
    void flags(int v)
    {
        zero = v == 0;
        sign = v < 0;
    }
    // The variables with a meaning of their own (FUN_0068c020, FUN_0068d7b0, FUN_00689cd0).
    enum {
        kTrackVolume = 0x12, kVolume = 0x13, kDuck = 0x19, kFadeTo = 0x28, kFadeVar = 0x29, kFadeStep = 0x2a,
        kFading = 0x2d, kPatch = 0x32, kTarget = 0x35, kDucking = 0x7b,
    };
    int &var(int at) { return local[at - 0x10]; }
    // Variable 0x35 set: everything but that goes to the track it names (FUN_0068bc50, FUN_0068ca80).
    Thread *aimed(int at) const
    {
        if (!target || at == kTarget || at < 0x10 || at >= 0x46)
            return nullptr;
        Thread *t = by_serial(target);
        return t && t != this ? t : nullptr;
    }
    // The fade a tick, the game's: towards a value by a step; done, the flag clears.
    void fade_step()
    {
        if (!var(kFading))
            return;
        const int at = var(kFadeVar);
        if (at < 0x10 || at >= 0x46) {
            var(kFading) = 0;
            return;
        }
        int step = var(kFadeStep);
        const int to = var(kFadeTo);
        int now = var(at);
        if (step == 0)
            var(kFading) = 0;
        if ((now < to) != (step > 0))
            step = -step;
        now += step;
        if ((step > 0 && to < now) || (step < 0 && now < to)) {
            var(kFading) = 0;
            now = to;
        }
        var(at) = now;
    }
    // Volume: the track's by what it was asked at, out of 1024; halved under a higher ducking level.
    void mix()
    {
        int v = var(kVolume) * var(kTrackVolume) / 1024;
        const int ducking = e.globals_[kDucking - 0x64];
        if (ducking && var(kDuck) < ducking)
            v /= 2;
        const float wanted = std::clamp(float(v) / 1024.0f, 0.0f, 1.0f);
        if (wanted == volume)
            return;
        volume = wanted;
        for (uint32_t h : notes)
            e.out_.set_volume(h, volume);
    }
    void set_ducking(int v)
    {
        auto &all = e.data_->duckers;
        if (v)
            all[serial] = v;
        else
            all.erase(serial);
        int top = 0;
        for (const auto &[who, level] : all)
            top = std::max(top, level);
        e.globals_[kDucking - 0x64] = top;
    }
    int read(int at) const
    {
        if (Thread *t = aimed(at))
            return t->local[at - 0x10];
        if (at < 0x10)
            return regs[at];
        if (at < 0x46)
            return local[at - 0x10];
        if (at >= 0x64 && at < 0x88)
            return e.globals_[at - 0x64];
        if (at >= 0x271a && at < 0x2737)
            return vars[at - 0x271a];
        return 0;
    }
    void write_own(int at, int v)
    {
        if (at == kPatch) {   // switches what plays
            auto it = g->patches.find(uint32_t(v));
            if (it != g->patches.end())
                patch = &it->second;
        } else if (at == kTarget) {
            target = 0;
            for (auto &t : e.threads_)
                if (v && t->g == g && t->track_id == uint32_t(v) && t->source == source && !t->dead)
                    target = t->serial;
        } else if (at == kFading && v && !note_active(int(notes.size()) - 1)) {
            v = 0;   // nothing sounding: nothing to fade
        }
        local[at - 0x10] = v;
    }
    void write(int at, int v)
    {
        if (Thread *t = aimed(at)) {
            t->write_own(at, v);
        } else if (at < 0x10) {
            regs[at] = v;
        } else if (at < 0x46) {
            write_own(at, v);
        } else if (at == kDucking) {
            set_ducking(v);
        } else if (at >= 0x64 && at < 0x88) {
            e.globals_[at - 0x64] = v;
        } else if (at >= 0x271a && at < 0x2737) {
            vars[at - 0x271a] = v;
        }
    }
    void load_hitlist(uint32_t id)
    {
        hitlist = &own;
        auto it = g->hitlists.find(id);
        if (it != g->hitlists.end()) {
            auto made = e.data_->lists.try_emplace({ g, id });
            if (made.second) {
                made.first->second.ready = it->second;
                made.first->second.changed();
            }
            hitlist = &made.first->second;
        }
        if (TS_ENV("HIT_TRACE")) {   // debug
            std::printf("  hit %s hitlist %u:", name.c_str(), unsigned(id));
            for (uint32_t t : hitlist->ready)
                std::printf(" %u", unsigned(t));
            std::printf(" (resting");
            for (uint32_t t : hitlist->rested)
                std::printf(" %u", unsigned(t));
            std::printf(")\n");
        }
    }
    uint32_t choose() { return hitlist->choose(e.random_); }
    void set_track(uint32_t id)
    {
        track_id = id;
        auto it = g->tracks.find(id);
        if (it != g->tracks.end()) {
            track = it->second;
            var(kTrackVolume) = track.volume;
            var(kVolume) = 1024;
            var(kDuck) = track.duck;
            if (track.hitlist) {
                load_hitlist(track.hitlist);
                if (!track.sound)
                    track.sound = choose();
            }
        } else {
            track = Track();
            track.sound = id;
        }
        auto p = g->patches.find(track.sound);
        patch = p == g->patches.end() ? nullptr : &p->second;
        if (TS_ENV("HIT_TRACE"))   // debug: how each event's track resolved
            std::printf("  hit %s track %u: hitlist %u, sound %u, subroutine %s, patch %s\n", name.c_str(), unsigned(id),
                        unsigned(track.hitlist), unsigned(track.sound), track.subroutine ? "yes" : "no",
                        patch ? patch->file.c_str() : "-");
        mix();
        group = track.group == 2 ? kMusic : track.group == 3 ? kVox : kFx;
    }
    int note_on(bool looped)
    {
        if (!patch || patch->file.empty())
            return -1;
        const uint32_t h = e.out_.note(patch->file, source, looped || patch->looped, volume, group);
        if (!h)
            return -1;
        notes.push_back(h);
        return int(notes.size()) - 1;
    }
    bool note_active(int i) const { return i >= 0 && size_t(i) < notes.size() && e.out_.playing(notes[size_t(i)]); }
    void note_off()
    {
        if (!notes.empty())
            e.out_.stop(notes.back());
    }
    // A variable written: the flags follow every write (FUN_0068c020).
    void set(int at, int v)
    {
        write(at, v);
        flags(v);
    }
    Thread *by_serial(uint32_t wanted) const
    {
        for (auto &t : e.threads_)
            if (t->serial == wanted && !t->dead)
                return t.get();
        return nullptr;
    }
    // Instruction 0x32: the track starts, with this one's object, volume and pan.
    void play_track(uint32_t id)
    {
        if (!id || !g->tracks.count(id) || e.threads_.size() >= 64)
            return;
        for (auto &t : e.threads_)
            if (t.get() != this && t->g == g && t->track_id == id && t->source == source)
                t->stop();
        auto t = std::make_unique<Thread>(e);
        t->g = g;
        t->name = name + ">" + std::to_string(id);
        t->source = source;
        t->regs[1] = regs[1], t->regs[2] = regs[2], t->regs[3] = regs[3];
        std::copy(std::begin(vars), std::end(vars), t->vars);
        const uint32_t at = g->tracks.at(id).subroutine;
        t->set_track(id);
        if (at) {
            t->pc = at;
            t->loop_pointer = int(at);
        } else {
            t->simple = t->play_simple = true;
        }
        e.threads_.push_back(std::move(t));
    }
    void kill_notes()
    {
        for (uint32_t h : notes)
            e.out_.stop(h);
        notes.clear();
    }

    // One instruction, as the game's (FUN_006880c0, ending at 0x53): 0 continue, 1 halt (until the next tick), 2 kill
    int step()
    {
        if (TS_ENV("HIT_TRACE"))
            std::printf("  hit %s @%u op %d regs %d %d %d %d %d %d %d %d %d %d %d %d\n", name.c_str(), pc, g->hit[pc], regs[0], regs[1],
                        regs[2], regs[3], regs[4], regs[5], regs[6], regs[7], regs[8], regs[9], regs[10], regs[11]);
        const uint8_t op = byte();
        if (e.op_counts)
            (*e.op_counts)[op]++;
        auto two = [&](auto &&with) {   // dest op= src
            const uint8_t dest = byte(), src = byte();
            set(dest, with(read(dest), read(src)));
            return 0;
        };
        switch (op) {
        case 1: {   // a note for a time
            byte();
            note_off_ms = read(byte());
            note_on(false);
            return 0;
        }
        case 2:   // note on
            byte();
            note_on(false);
            return 0;
        case 3:   // note off
            byte();
            note_off();
            return 0;
        case 4: {   // load byte (unsigned)
            const uint8_t dest = byte();
            set(dest, byte());
            return 0;
        }
        case 5: {   // load long
            const uint8_t dest = byte();
            set(dest, int(u32()));
            return 0;
        }
        case 6: {   // set
            const uint8_t dest = byte();
            set(dest, read(byte()));
            return 0;
        }
        case 7: {   // call
            const uint32_t target = u32();
            stack.push_back(pc);
            pc = trans(target);
            return 0;
        }
        case 8: case 12:   // return, end: the same to the game
            if (!stack.empty()) {
                pc = stack.back();
                stack.pop_back();
                return 0;
            }
            if (loop && has_set_loop) {
                pc = uint32_t(loop_pointer);
                return 0;
            }
            return 2;
        case 9: {   // wait: so many of HIT's steps (the value in a variable)
            // The exe's (FUN_006880c0 case 9) runs the thread again at the HIT clock plus the value,
            // the clock going up one each update (FUN_0068d2d0): steps, not milliseconds. Taken
            // for milliseconds (FreeSO's), every wait was a sixteenth as long: Sims studying or
            // reading mumbled without a break, snores and washing voices ran together, the fire
            // alarm beeped twenty times a second (the user's John "HMMHMM" constantly, 2026-11-03).
            // Ours steps 60 times a second (Viewer::run_hit).
            const uint8_t src = byte();
            if (wait_remain == -1) {
                wait_remain = read(src);
#if !defined(__vita__)
                if (TS_ENV("HIT_WAITS"))   // debug: each wait asked for
                    std::printf("hit wait %s %d\n", name.c_str(), int(wait_remain));
#endif
            }
            wait_remain -= 1;
            if (wait_remain > 0) {
                pc -= 2;
                return 1;
            }
            wait_remain = -1;
            return 0;
        }
        case 11:   // wait for the sample
            if (!note_active(int(notes.size()) - 1))
                return 0;
            pc--;
            return 1;
        case 13:   // jump
            pc = trans(u32());
            return 0;
        case 14:   // test
            flags(read(byte()));
            return 0;
        case 15: case 29: case 30: case 70: return 0;   // nothing
        case 16: return two([](int a, int b) { return a + b; });
        case 17: return two([](int a, int b) { return a - b; });
        case 18: return two([](int a, int b) { return b ? a / b : 0; });
        case 19: return two([](int a, int b) { return a * b; });
        case 20: {   // compare
            const uint8_t a = byte(), b = byte();
            flags(read(a) - read(b));
            return 0;
        }
        case 21: return two([](int a, int b) { return a < b ? 1 : 0; });
        case 22: case 40: return two([](int a, int b) { return b < a ? 1 : 0; });
        case 23: {   // not
            const uint8_t dest = byte();
            set(dest, read(dest) == 0 ? 1 : 0);
            return 0;
        }
        case 24: {   // random, low to high less one (FUN_008248c0); both are variables
            const uint8_t dest = byte();
            const int lo = read(byte()), hi = read(byte());
            const int r = hi > lo ? std::uniform_int_distribution<int>(lo, hi - 1)(e.random_) : lo;
            if (TS_ENV("HIT_TRACE"))   // debug: the random picks (the user heard the same cue over and over, 2026-11-02)
                std::printf("  hit %s random %d..%d -> %d\n", name.c_str(), int(lo), int(hi), r);
            set(dest, r);
            return 0;
        }
        case 25: {   // absolute
            const uint8_t dest = byte();
            set(dest, std::abs(read(dest)));
            return 0;
        }
        case 27: return 2;   // kill
        case 28: byte(); return 2;
        case 31: {   // load from the variable a variable names
            const uint8_t dest = byte();
            set(dest, read(read(byte())));
            return 0;
        }
        case 32:   // loop
            pc = uint32_t(loop_pointer);
            return 0;
        case 33:   // set loop
            loop_pointer = int(pc);
            has_set_loop = true;
            return 0;
        case 34: byte(); byte(); return 0;   // callback
        case 35:   // smart add
            hitlist->ready.push_back(uint32_t(read(byte())));
            hitlist->changed();
            return 0;
        case 36: {   // smart remove
            const uint32_t id = uint32_t(read(byte()));
            for (std::vector<uint32_t> *l : { &hitlist->ready, &hitlist->rested })
                l->erase(std::remove(l->begin(), l->end(), id), l->end());
            hitlist->changed();
            return 0;
        }
        case 37:   // smart reset
            hitlist->ready.clear();
            hitlist->rested.clear();
            return 0;
        case 38:   // smart index: how many rest
            hitlist->limit = size_t(std::max(0, read(byte())));
            hitlist->fixed = true;
            return 0;
        case 39: {   // smart choose
            const uint8_t dest = byte();
            if (!hitlist->ready.empty() || !hitlist->rested.empty())
                set(dest, int(choose()));
            return 0;
        }
        case 41: return two([](int a, int b) { return a && b ? 1 : 0; });
        case 42: return two([](int a, int b) { return a | b; });
        case 43: return two([](int a, int b) { return !a && !b ? 1 : 0; });
        case 44: return two([](int a, int b) { return a ^ b; });
        case 45: case 46: {   // min, max (the other way round in FreeSO)
            const uint8_t dest = byte();
            const int v = int(u32()), cur = read(dest);
            set(dest, op == 45 ? std::min(cur, v) : std::max(cur, v));
            return 0;
        }
        case 47: case 48: {   // inc, dec
            const uint8_t dest = byte();
            set(dest, read(dest) + (op == 47 ? 1 : -1));
            return 0;
        }
        case 49: byte(); return 0;   // print register
        case 50:   // play track
            play_track(uint32_t(read(byte())));
            return 0;
        case 51: {   // kill track
            const uint32_t id = uint32_t(read(byte()));
            for (auto &t : e.threads_)
                if (t.get() != this && t->g == g && t->track_id == id && t->source == source)
                    t->stop();
            return 0;
        }
        case 58: case 59: case 60: case 61: return 0;   // test sounds
        case 62: case 63: case 64: case 65: case 66: case 67: {   // conditional jumps
            const uint32_t target = u32();
            bool go = false;
            switch (op) {
            case 62: go = zero; break;
            case 63: go = !zero; break;
            case 64: go = !sign && !zero; break;
            case 65: go = sign; break;
            case 66: go = !sign; break;
            default: go = sign || zero; break;
            }
            if (go)
                pc = trans(target);
            return 0;
        }
        case 68: load_hitlist(uint32_t(read(byte()))); return 0;   // smart set list
        case 69: {   // sequence group: this thread takes it, ending who had it (the game fades them
            // down, step 200 of 1024 a tick, if they're audible). The group is the object: a Sim's new line
            // ends their last.
            uint32_t &owner = e.data_->seq_groups[read(byte())];
            Thread *other = by_serial(owner);
            owner = serial;
            if (!other || other == this)
                return 0;
            if (other->var(kVolume) == 0) {
                other->stop();
                return 0;
            }
            if (!other->var(kFading) || other->var(kFadeVar) != kVolume) {
                other->var(kFadeStep) = 200;
                other->var(kFadeVar) = kVolume;
                other->write_own(kFading, 1);
            }
            pc -= 2;   // this again next tick, the old line on its way down
            return 1;
        }
        case 71: {   // sequence group, or give way to who has it
            uint32_t &owner = e.data_->seq_groups[read(byte())];
            Thread *other = by_serial(owner);
            owner = serial;
            if (other && other != this) {
                kill_notes();
                return 2;
            }
            return 0;
        }
        case 72: {   // source data field: the speaker's variables
            const uint8_t dest = byte();
            byte();
            const int field = read(byte());
            set(dest, field >= 0 && field < 29 ? vars[field] : 0);
            return 0;
        }
        case 73: {   // a sequence group's track
            const int group = read(byte());
            set(byte(), int(e.data_->seq_groups[group]));
            return 0;
        }
        case 74: case 76: {   // set LL, TL: no flags
            const uint8_t dest = byte();
            write(dest, read(byte()));
            return 0;
        }
        case 75: {   // set LT
            const uint8_t dest = byte();
            set(dest, read(byte()));
            return 0;
        }
        case 77: case 78: case 79: case 80: case 81: case 82: {   // wait until
            const int a = read(byte()), b = read(byte());
            const bool done = op == 77   ? a == b
                              : op == 78 ? a != b
                              : op == 79 ? b < a
                              : op == 80 ? a < b
                              : op == 81 ? b <= a
                                         : a <= b;
            if (done)
                return 0;
            pc -= 3;
            return 1;
        }
        case 83: byte(); return 0;   // duck
        default:
            return 0;
        }
    }

    // A station: its next file, or (none left) the start again.
    bool play_next()
    {
        if (files.empty())
            return false;
        if (next >= files.size()) {
            std::shuffle(files.begin(), files.end(), e.random_);
            next = 0;
        }
        playing_stream = e.out_.stream(files[next++], source, volume, group);
        return playing_stream != 0;
    }

    // A tick: false when the thread is done.
    bool tick()
    {
        if (dead)
            return false;
        if (!station) {
            fade_step();
            mix();
        }
        if (station) {
            if (fade > 0) {
                if (--fade == 0) {
                    e.out_.stop(playing_stream);
                    return false;
                }
                e.out_.set_volume(playing_stream, volume * float(fade) / 180.0f);
            }
            if (!playing_stream || !e.out_.playing(playing_stream)) {
                if (!play_next())
                    return false;
            }
            return true;
        }
        if (simple) {
            if (play_simple) {
                note_on(loop);
                play_simple = false;
            }
            return note_active(int(notes.size()) - 1);
        }
        if (note_off_ms >= 0 && (note_off_ms -= 1) <= 0) {   // (steps, as op 9: the exe's case 1 uses the same clock)
            note_off();
            note_off_ms = -1;
        }
        if (thread_dead)
            return note_active(int(notes.size()) - 1);
        for (int guard = 0; guard < 10000; guard++) {   // a program runs until it waits
            if (pc >= g->hit.size())
                return false;
            e.steps_++;   // (the log's count: how much the programs do a frame)
            const int r = step();
            if (r == 1)
                return true;
            if (r == 2) {
                thread_dead = true;
                return true;
            }
        }
        return false;   // a program that never waits
    }

    void stop()
    {
        if (e.data_->duckers.count(serial))
            set_ducking(0);
        kill_notes();
        if (playing_stream)
            e.out_.stop(playing_stream);
        playing_stream = 0;
        dead = true;
    }
};

// ---------------------------------------------------------------- the engine

Engine::Engine(Content &content, Output &out) : data_(std::make_unique<Data>()), out_(out)
{
    MemScope scope(MemKind::Index);   // (the sound programs' banks, kept for good: counted with the index on the Vita)
    data_->root = content.root();
    const std::vector<std::string> hots = content.names_ending(".hot");   // SoundData's and each expansion's
    data_->groups.reserve(hots.size() + kAddedTables);
    for (const std::string &hot : hots)
        add_table(content, hot);
    // Radio and TV stations, and the background music (FreeSO TS1Audio).
    const std::pair<uint32_t, const char *> stations[] = {
        { 6685, "Music/Stations/Beach" }, { 269, "Music/Stations/Classica" }, { 260, "Music/Stations/Country" },
        { 6681, "Music/Stations/CountryD" }, { 6684, "Music/Stations/Disco" }, { 65536, "Music/Stations/EZ" },
        { 65537, "Music/Stations/EZExp" }, { 270, "Music/Stations/Latin" }, { 6682, "Music/Stations/Rap" },
        { 6683, "Music/Stations/Rave" }, { 280, "Music/Stations/Rock" }, { 22900, "Music/Stations/UnlCheap" },
        { 22901, "Music/Stations/UnlSpeak" }, { 22902, "Music/Stations/UnlSpeak" }, { 100100, "Music/Stations/Superstar" },
        { 28, "SoundData/TVStations/TV_Action" }, { 29, "SoundData/TVStations/TV_Comedy_cartoon" },
        { 27, "SoundData/TVStations/TV_Mystery" }, { 26, "SoundData/TVStations/TV_Romance" },
        { 103000, "ExpansionPack6/Sound/TVStations/Animal" }, { 103001, "ExpansionPack6/Sound/TVStations/Entertainment" },
        { 103002, "ExpansionPack6/Sound/TVStations/News" }, { 103003, "ExpansionPack6/Sound/TVStations/Sports" },
    };
    for (const auto &[id, path] : stations)
        data_->stations[station_id(id)] = path;
    const std::pair<const char *, const char *> modes[] = {
        { "KBUI", "Music/Modes/Build" }, { "KBUY", "Music/Modes/Buy" }, { "KDTN", "Music/Modes/dt_nhood" },
        { "KLOD", "Music/Modes/Load" }, { "K_MM", "Music/Modes/MakinMagic" }, { "KMMB", "Music/Modes/MM_Build" },
        { "KMM2", "Music/Modes/MM_Buy" }, { "KNHD", "Music/Modes/Nhood" }, { "KNUS", "Music/Modes/NhoodUS" },
        { "KSST", "Music/Modes/Superstar" }, { "KVAC", "Music/Modes/Vacation" },
    };
    for (const auto &[id, path] : modes)
        data_->stations[id] = path;
    // The rest of the exe's kSetMusicMode modes (FUN_006014b0: 12 Old Town's station 0x51a6, UnlSpeak; 13
    // Studio Town's mode music 0x18706; 14 its station 0x18705; 16 Makin' Magic's credits 0x192bd).
    // (Ours, not the game's names: they key the folders.)
    data_->stations["KUNS"] = "Music/Stations/UnlSpeak";
    data_->stations["KSSS"] = "Music/Stations/Superstar";
    data_->stations["KMMC"] = "Music/Modes/MM_credits";
    data_->music_modes = { { 1, "KBUY" }, { 2, "KBUI" }, { 3, "KNUS" }, { 5, "KLOD" }, { 8, "KBUI" },
                           { 10, "KDTN" }, { 11, "KVAC" }, { 12, "KUNS" }, { 13, "KSST" }, { 14, "KSSS" },
                           { 15, "K_MM" }, { 16, "KMMC" }, { 17, "KMMB" }, { 18, "KMM2" },
                           { 20, "KSST" } };   // 20: the viewer's own for Studio Town's (Superstar) music
}

Engine::~Engine() = default;

void Engine::add_table(Content &content, const std::string &hot)
{
    if (std::find(data_->tables.begin(), data_->tables.end(), hot) != data_->tables.end())
        return;
    MemScope scope(MemKind::Index);
    const std::string base = hot.substr(0, hot.size() - 4);
    std::vector<uint8_t> bytes;
    const FileRef *ref = content.file(hot);
    if (!ref || !read_ref(*ref, bytes))
        return;
    ResGroup g;
    std::vector<uint8_t> sym;
    if (const FileRef *h = content.file(base + ".hsm"); h && read_ref(*h, sym)) {
        std::istringstream in(std::string(sym.begin(), sym.end()));
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string name;
            int value = 0;
            if (ls >> name >> value && !g.hsm.count(lower(name)))   // repeats are local labels
                g.hsm[lower(name)] = value;
        }
    }
    if (const FileRef *h = content.file(base + ".hit"))
        read_ref(*h, g.hit);
    if (data_->groups.size() == data_->groups.capacity())   // (no room left: the threads and lists that point into it end first)
        stop_all();
    data_->groups.push_back(std::move(g));
    data_->tables.push_back(hot);
    const size_t before = data_->events.size();
    read_hot(std::string(bytes.begin(), bytes.end()), data_->groups.back(), data_->groups.size() - 1, data_->events);
    if (TS_ENV("HIT_TRACE"))   // debug: each table read
        std::printf("  hit table %s: %zu events (hsm %zu, hit %zu bytes)\n", hot.c_str(), data_->events.size() - before,
                    data_->groups.back().hsm.size(), data_->groups.back().hit.size());
}

size_t Engine::events() const
{
    return data_->events.size();
}

std::vector<std::string> Engine::live() const
{
    std::vector<std::string> out;
    for (const auto &t : threads_)
        if (!t->dead)
            out.push_back(t->name + "#" + std::to_string(t->source));
    return out;
}

std::vector<std::string> Engine::event_names() const
{
    std::vector<std::string> out;
    for (const auto &[name, ev] : data_->events)
        out.push_back(name);
    return out;
}

bool Engine::knows(const std::string &event, int *type) const
{
    auto it = data_->events.find(lower(event));
    if (it == data_->events.end())
        return false;
    if (type)
        *type = it->second.type;
    return true;
}

namespace {

// A station's files: TV sounds (.xa, with the commercials in the folder
// above mixed in) or songs (.mp3), shuffled.
std::vector<std::string> station_files(const std::string &root, const std::string &folder, std::mt19937 &random)
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = fs::path(root) / folder;
    std::vector<std::string> xa, mp3;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string ext = lower(it->path().extension().string());
        if (ext == ".xa")
            xa.push_back(it->path().string());
        else if (ext == ".mp3")
            mp3.push_back(it->path().string());
    }
    if (!xa.empty()) {
        out = xa;
        for (fs::directory_iterator it(dir.parent_path(), ec), end; !ec && it != end; it.increment(ec))
            if (lower(it->path().extension().string()) == ".xa")
                out.push_back(it->path().string());
    } else {
        out = mp3;
    }
    std::shuffle(out.begin(), out.end(), random);
    return out;
}

} // namespace

Engine::Thread *Engine::start(const std::string &event, uint16_t source, bool loop)
{
    const std::string key = lower(event);
    auto it = data_->events.find(key);
    if (it == data_->events.end())
        return nullptr;
    const Event &ev = it->second;
    const ResGroup &g = data_->groups[ev.group];
    auto thread = std::make_unique<Thread>(*this);
    thread->g = &g;
    thread->name = key;
    thread->source = source;
    thread->regs[1] = source;   // the object: the Sims' sequence groups go by it (0x45 reads variable 1)
    if (ev.type == kTurnOnTV) {   // a TV or radio station
        auto st = data_->stations.find(station_id(ev.track));
        if (st == data_->stations.end())
            return nullptr;
        thread->station = true;
        thread->files = station_files(data_->root, st->second, random_);
        thread->volume = 0.8f;
        thread->group = kMusic;
        if (thread->files.empty())
            return nullptr;
    } else if (ev.type == kSoundobPlay || ev.type == kPlayPiano) {
        uint32_t track_id = ev.track;
        auto tr = g.tracks.find(track_id);
        uint32_t subroutine = tr != g.tracks.end() ? tr->second.subroutine : 0;
        if (!subroutine && ev.type == kPlayPiano) {   // piano_play names no track: the PlayPiano program (FreeSO HITVM)
            if (auto h = g.hsm.find("playpiano"); h != g.hsm.end())
                subroutine = uint32_t(h->second);
            for (const auto &[id, t] : g.tracks)
                if (subroutine && t.subroutine == subroutine)
                    track_id = id;
        }
        if (subroutine) {
            thread->pc = subroutine;
            thread->loop_pointer = int(subroutine);
            thread->set_track(track_id);
        } else if (ev.track && tr != g.tracks.end()) {
            thread->set_track(ev.track);
            thread->simple = true;
            thread->play_simple = true;
        } else if (ev.track && g.patches.count(ev.track)) {   // a patch for a track
            thread->set_track(ev.track);
            thread->simple = true;
            thread->play_simple = true;
        } else {
            return nullptr;
        }
        thread->loop = loop || key == "piano_play";
        thread->has_set_loop = key == "piano_play";
    } else {
        return nullptr;
    }
    threads_.push_back(std::move(thread));
    return threads_.back().get();
}

bool Engine::play(const std::string &event, uint16_t source, const int vars[29], bool loop)
{
    const std::string key = lower(event);
    int type = 0;
    if (!knows(key, &type))
        return false;
    if (type == kSoundobStop || type == kSoundobKill || type == kTurnOffTV || type == kSoundobCancelNote) {
        stop(source);   // the object's sounds end (a TV switched off)
        return true;
    }
    if (type == kSetMusicMode) {
        music(int(data_->events[key].track));
        return true;
    }
    // One of these playing for this object already: it carries on (FreeSO ActiveEvents).
    const std::string active = key + "#" + std::to_string(source);
    if (auto a = active_.find(active); a != active_.end()) {
        for (auto &t : threads_)
            if (t.get() == a->second && !t->dead)
                return true;
        active_.erase(a);
    }
    if (type == kTurnOnTV)   // one station an object
        for (auto &t : threads_)
            if (t->station && !t->music && t->source == source)
                t->stop();
    Thread *t = start(key, source, loop);
    if (!t)
        return false;
    if (vars)
        std::copy(vars, vars + 29, t->vars);
    active_[active] = t;
    return true;
}

void Engine::stop(uint16_t source)
{
    for (auto &t : threads_)
        if (t->source == source && !t->music) {
            if (TS_ENV("HIT_TRACE"))   // debug
                std::printf("  hit stop %s#%u\n", t->name.c_str(), unsigned(source));
            t->stop();
        }
}

void Engine::stop_all()
{
    for (auto &t : threads_)
        t->stop();
    music_ = nullptr;
}

void Engine::music(int mode)
{
    if (music_)
        music_->fade = 180;   // three seconds
    music_ = nullptr;
    auto m = data_->music_modes.find(mode);
    if (m == data_->music_modes.end())
        return;
    auto st = data_->stations.find(m->second);
    if (st == data_->stations.end())
        return;
    auto thread = std::make_unique<Thread>(*this);
    thread->station = thread->music = true;
    thread->files = station_files(data_->root, st->second, random_);
    thread->volume = 0.6f;
    thread->group = kMusic;
    thread->g = data_->groups.empty() ? nullptr : &data_->groups[0];
    if (thread->files.empty())
        return;
    threads_.push_back(std::move(thread));
    music_ = threads_.back().get();
    music_mode_ = mode;
    std::printf("music: mode %d's tracks, a new one now\n", mode);
}

void Engine::music_next(int mode)
{
    // TS_MUSIC_SWITCH_OLD=1: the switch starts the new mode's music at once, as before.
    static const bool old = TS_ENV("TS_MUSIC_SWITCH_OLD") != nullptr;
    if (old || !music_ || music_->dead) {
        music(mode);
        return;
    }
    if (mode == music_mode_)
        return;
    auto m = data_->music_modes.find(mode);
    if (m == data_->music_modes.end())
        return;
    auto st = data_->stations.find(m->second);
    if (st == data_->stations.end())
        return;
    std::vector<std::string> files = station_files(data_->root, st->second, random_);
    if (files.empty())
        return;
    music_->files = std::move(files);   // (station_files shuffles them)
    music_->next = 0;
    std::printf("music: mode %d -> %d, the track plays on, the next from mode %d\n", music_mode_, mode, mode);
    music_mode_ = mode;
}

void Engine::tick()
{
    for (size_t i = 0; i < threads_.size(); i++) {
        running_ = threads_[i]->name;
        if (!threads_[i]->tick()) {
            threads_[i]->stop();
            if (threads_[i].get() == music_)
                music_ = nullptr;
            for (auto a = active_.begin(); a != active_.end();)
                a = a->second == threads_[i].get() ? active_.erase(a) : std::next(a);
            threads_.erase(threads_.begin() + std::ptrdiff_t(i--));
        }
    }
    running_.clear();
}

} // namespace ts::hit
