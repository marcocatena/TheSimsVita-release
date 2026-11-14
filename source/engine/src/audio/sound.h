/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * The XA decoder is ported from FreeSims (sims.files/XA/XAFile.cs).
 */

// The game's sounds. Objects name sound events ("fridge_get"); Maxis' HIT
// tables (.hot files: SoundData and each expansion's Sound.far) map an event
// to a track and a track to one of its samples (patches). Tracks run little
// HIT programs; most are the generic "play this patch" or "loop this patch",
// and the others pick among patches named after the track (fridge_get1,
// fridge_get2, ...), which is what is done here rather than running HIT.
// Samples are EA XA ADPCM or plain WAV.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {

class Content;
struct FileRef;

// Decoded audio: 16-bit samples, interleaved when stereo.
struct Pcm {
    int channels = 1;
    int rate = 22050;
    std::vector<int16_t> samples;
};

// Audio decoded as it plays (music): interleaved 16-bit frames.
class Stream {
public:
    virtual ~Stream() = default;
    virtual int rate() const = 0;
    virtual int channels() const = 0;
    // Up to `frames` frames; fewer (0) at the end.
    virtual size_t read(int16_t *out, size_t frames) = 0;
};
// An MP3 file's bytes as a stream (mp3.cpp); null if it isn't one.
std::unique_ptr<Stream> open_mp3(std::vector<uint8_t> bytes);
// The same, read from the file a little at a time as it plays (64 KB held, not the whole song:
// a song of megabytes in one block is what a long session's heap no longer has).
std::unique_ptr<Stream> open_mp3_file(const std::string &path);

bool decode_xa(const uint8_t *data, size_t size, Pcm &out);
bool decode_wav(const uint8_t *data, size_t size, Pcm &out);

class SoundLibrary {
public:
    explicit SoundLibrary(Content &content);

    // A sample for an event: its file (base name) and whether it loops; false if none.
    // `pick` chooses among alternatives (pass a random number); voices
    // depend on who speaks ('f' or 'm').
    bool resolve(const std::string &event, uint32_t pick, std::string &file, bool &loop, char voice = 'f') const;
    // The decoded sample, loaded on first use and kept while there's room (null if it can't
    // be read): past kSampleBudget, those used longest ago and not playing are let go.
    std::shared_ptr<const Pcm> sample(const std::string &file);
    // The sample if it's in (no reading or decoding: the caller reads it elsewhere), else null;
    // `known` false when the install has no such file (nothing to read).
    std::shared_ptr<const Pcm> cached(const std::string &file, bool &known);
    const FileRef *file_ref(const std::string &file) const;
    // A sample decoded elsewhere kept, as sample() keeps what it decodes (the budget's evictions).
    void keep(const std::string &file, std::shared_ptr<Pcm> pcm);

    size_t events() const { return events_.size(); }
    // One more .hot (added content that arrived while the game runs), by the index's name: its
    // events among the others (a name both have: the new one's, as a later file's at the start).
    // One already read is left alone.
    void add_hot(const std::string &name);
    // Debug (TS_SOUND_DUMP): every event's sample for both voices and a range of picks, one
    // line each, to check the tables give the same as before a change.
    std::string dump() const;
    // Every kept sample not playing now let go (the heap running out); returns the bytes.
    size_t let_go_unused();
    size_t samples_kept() const { return samples_.size(); }
    size_t sample_bytes() const { return sample_bytes_; }
    // Debug (the log's sound line): samples read and decoded since the count was last reset, and
    // the time it took (on the caller's thread: the main one, for HIT's notes).
    size_t decodes_ = 0;
    double decode_ms_ = 0;

private:
    // Each .hot file numbers its own tracks and patches. Kept as arrays sorted by id with their
    // words in one block of text: as maps of strings (17,515 patches, 6,344 hit lists, ~4,400
    // tracks and events) they were 4.5 MB of the Vita's heap, from 2.4 MB of .hot text.
    struct Track {
        int id = 0;
        uint32_t name = 0;      // in the table's text
        int patch = 0;
        bool looped = false;    // its routine a looped one
    };
    struct Patch {
        int id = 0;
        uint32_t file = 0;      // in the table's text
        bool loop = false;
    };
    struct Named {              // the patches by folded name without its last digits
        uint32_t stem = 0;      // in the table's text
        uint32_t first = 0, count = 0;   // in `ids`
    };
    struct Table {
        std::string text;       // the words, each ended by a NUL
        std::vector<Track> tracks;    // by id
        std::vector<Patch> patches;   // by id
        std::vector<Named> named;     // by stem
        std::vector<int> ids;
        const char *word(uint32_t at) const { return text.c_str() + at; }
        const Track *track(int id) const;
        const Patch *patch(int id) const;
        const Named *by_name(const std::string &stem) const;
    };
    struct Event {
        uint32_t name = 0;      // in event_text_: the folded event name
        uint32_t table = 0;
        int track = 0;
    };
    void load_hot(const std::string &text, std::map<std::string, std::pair<size_t, int>> &events);

    Content &content_;
    std::vector<Table> tables_;
    std::string event_text_;
    std::vector<Event> events_;   // by name
    std::vector<std::string> hots_;   // the .hot files read, by the index's names
    struct Kept {
        std::shared_ptr<const Pcm> pcm;
        uint64_t used = 0;   // when last asked for (use_clock_)
        size_t bytes = 0;
    };
#if defined(__vita__)
    static constexpr size_t kSampleBudget = size_t(8) << 20;   // the console's heap is tight (12 to 2026-11-05: 16.7 MB held downtown)
#else
    static constexpr size_t kSampleBudget = size_t(24) << 20;
#endif
    std::map<std::string, Kept> samples_;
    size_t sample_bytes_ = 0;
    uint64_t use_clock_ = 0;
};

} // namespace ts
