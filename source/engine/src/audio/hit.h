/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSO (tso.files/HIT: Hot.cs, HITFile.cs, HSM.cs, Hitlist.cs,
 * Patch.cs; tso.sound: HITVM.cs, HITThread.cs, HITInterpreter.cs,
 * Events/HITTVOn.cs; tso.content/TS1/TS1Audio.cs).
 */

// Maxis' HIT sound system. The game's objects ask for sound events by name
// ("fridge_get", "bed_getin_vox", "stereo_rock"); the .hot tables (with the
// .hsm symbols and .hit bytecode beside them) say what each event does:
// play a track (a sample, or a little HIT program that picks among samples,
// by who is speaking for voices, and loops or waits), switch a TV or radio
// on (a station: a folder of songs or TV sounds played one after another),
// or change the background music. This runs those programs; the sounds
// themselves go to an Output (the mixer) as notes.
#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace ts {

class Content;

namespace hit {

// HIT event types (FreeSO HITEvents).
enum EventType {
    kSoundobPlay = 1, kSoundobStop = 2, kSoundobKill = 3, kSoundobCancelNote = 19, kKillAll = 20,
    kTurnOnTV = 30, kTurnOffTV = 31, kSetMusicMode = 36, kPlayPiano = 43,
};

// Where sounds go: the mixer. A note is one sample playing for one source
// (the object that asked: it follows it on screen); a stream is a station's
// next file (an .mp3 or .xa on disk), played once.
class Output {
public:
    virtual ~Output() = default;
    // Starts a sample (a file the content knows by name); its handle, 0 if it can't.
    virtual uint32_t note(const std::string &file, uint16_t source, bool loop, float volume, int group) = 0;
    virtual uint32_t stream(const std::string &path, uint16_t source, float volume, int group) = 0;
    virtual bool playing(uint32_t handle) const = 0;
    virtual void stop(uint32_t handle) = 0;
    virtual void set_volume(uint32_t handle, float volume) = 0;
};

// Volume groups (HITVolumeGroup): sound effects, music, voices, ambience.
enum Group { kFx = 0, kMusic = 1, kVox = 2, kAmbience = 3 };

class Engine {
public:
    // Reads every .hot table in the install, with its .hsm and .hit.
    Engine(Content &content, Output &out);
    ~Engine();
    // One more table (added content that arrived while the game runs): its .hot by the index's
    // name, with its .hsm and .hit. One already read is left alone. On the thread that ticks it.
    void add_table(Content &content, const std::string &hot);

    // A sound event for an object (Play Sound): `vars` are the speaker's
    // object variables (a Sim's sex, age, skills and personality: FreeSO
    // VMAvatar.SubmitHITVars), `loop` the primitive's loop flag. False if
    // the event is unknown or plays nothing.
    bool play(const std::string &event, uint16_t source, const int vars[29], bool loop);
    // An object's sounds end (Stop All Sounds, the object gone).
    void stop(uint16_t source);
    void stop_all();
    // Debug (the log's sound line): the sound threads running, and the program steps run since
    // the count was last reset (every tick of every thread adds its steps).
    size_t threads() const { return threads_.size(); }
    size_t steps_ = 0;
    // The background music (buy and build mode): a music mode (FreeSO MusicModes:
    // 0 none, 1 buy, 2 build, ...); fades out what plays.
    void music(int mode);
    // The mode changed with music playing (buy to build and back): the track plays on and the next
    // one comes from the new mode's (the original's, as the user heard it, 2026-11-10). With
    // nothing playing, as music().
    void music_next(int mode);
    // The mode whose music plays now, 0 none.
    int music_mode() const { return music_ ? music_mode_ : 0; }
    // Runs the programs a tick (60 a second, as HIT's waits count milliseconds by 16).
    void tick();

    size_t events() const;
    std::vector<std::string> event_names() const;
    // Instructions run, by number (hit_sweep): null, and nothing counted, in play.
    std::array<uint64_t, 256> *op_counts = nullptr;
    // The event whose thread is running (inside play and tick: who asked for a note).
    const std::string &running() const { return running_; }
    // The events still sounding (for reports: what never stopped).
    std::vector<std::string> live() const;
    // Whether an event is known, and what it does (for reports).
    bool knows(const std::string &event, int *type = nullptr) const;

    struct Data;
    struct Thread;

private:
    std::unique_ptr<Data> data_;
    Output &out_;
    std::vector<std::unique_ptr<Thread>> threads_;
    std::map<std::string, Thread *> active_;   // event -> its live thread (reused, as FreeSO's ActiveEvents)
    Thread *music_ = nullptr;
    int music_mode_ = 0;   // the mode whose tracks music_ picks from
    std::string running_;
    // The music's shuffles and the scripts' random picks: from the clock, a new order each time the
    // game starts (a fixed 1234 played the same tracks in the same order every time: the user,
    // 2026-11-12, buy and build mode's songs). TS_HIT_SEED=<n>: that seed (tests).
    static uint32_t first_seed()
    {
        if (const char *s = std::getenv("TS_HIT_SEED"))
            return uint32_t(std::strtoul(s, nullptr, 10));
        return uint32_t(std::chrono::system_clock::now().time_since_epoch().count() ^
                        std::chrono::steady_clock::now().time_since_epoch().count());
    }
    std::mt19937 random_{ first_seed() };
    int globals_[36] = {};
    friend struct Thread;
    Thread *start(const std::string &event, uint16_t source, bool loop);
};

} // namespace hit
} // namespace ts
