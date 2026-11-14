/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * The XA decoder is ported from FreeSims (sims.files/XA/XAFile.cs).
 */
#include "audio/sound.h"
#include "core/memory_kind.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sstream>

#include "core/strings.h"
#include "files/content.h"
#include "files/install.h"

namespace ts {

namespace {

uint16_t rd16(const uint8_t *p)
{
    return uint16_t(p[0] | p[1] << 8);
}

uint32_t rd32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// EA XA ADPCM: blocks of a predictor/shift byte, then nibbles.
struct XaChannel {
    int cur = 0, prev = 0;
    int16_t next(int nibble, int c1, int c2, int shift)
    {
        int v = int(int32_t(uint32_t(nibble) << 28) >> shift);
        v = (v + cur * c1 + prev * c2 + 0x80) >> 8;
        v = std::clamp(v, -32768, 32767);
        prev = cur;
        cur = v;
        return int16_t(v);
    }
};

constexpr int kXaTable[8] = { 0, 0xF0, 0x1CC, 0x188, 0, 0, -0xD0, -0xDC };

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(uint8_t(s[a])))
        a++;
    while (b > a && std::isspace(uint8_t(s[b - 1])))
        b--;
    return s.substr(a, b - a);
}

std::vector<std::string> split_fields(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : s) {
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (c == ',' && !quoted) {
            out.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    out.push_back(trim(cur));
    return out;
}

std::string base_file(std::string path)
{
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos)
        path = path.substr(slash + 1);
    return fold_path(path);
}

} // namespace

bool decode_xa(const uint8_t *data, size_t size, Pcm &out)
{
    if (size < 24 || std::memcmp(data, "XA", 2) != 0)
        return false;
    out.channels = rd16(data + 10);
    out.rate = int(rd32(data + 12));
    const uint32_t decoded = rd32(data + 4);
    if (out.channels < 1 || out.channels > 2 || out.rate <= 0)
        return false;
    out.samples.clear();
    out.samples.reserve(decoded / 2);
    size_t p = 24;
    XaChannel left, right;
    if (out.channels == 1) {
        while (p + 15 <= size) {
            const uint8_t h = data[p];
            const int c1 = kXaTable[h >> 4 & 3], c2 = kXaTable[(h >> 4 & 3) + 4], shift = (h & 15) + 8;
            for (int i = 1; i < 15; i++) {
                out.samples.push_back(left.next(data[p + i] >> 4, c1, c2, shift));
                out.samples.push_back(left.next(data[p + i] & 15, c1, c2, shift));
            }
            p += 15;
        }
    } else {
        while (p + 30 <= size) {
            const uint8_t hl = data[p], hr = data[p + 1];
            const int c1l = kXaTable[hl >> 4 & 3], c2l = kXaTable[(hl >> 4 & 3) + 4], sl = (hl & 15) + 8;
            const int c1r = kXaTable[hr >> 4 & 3], c2r = kXaTable[(hr >> 4 & 3) + 4], sr = (hr & 15) + 8;
            for (int i = 2; i < 29; i += 2) {
                out.samples.push_back(left.next(data[p + i] >> 4, c1l, c2l, sl));
                out.samples.push_back(right.next(data[p + i + 1] >> 4, c1r, c2r, sr));
                out.samples.push_back(left.next(data[p + i] & 15, c1l, c2l, sl));
                out.samples.push_back(right.next(data[p + i + 1] & 15, c1r, c2r, sr));
            }
            p += 30;
        }
    }
    return !out.samples.empty();
}

bool decode_wav(const uint8_t *data, size_t size, Pcm &out)
{
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
        return false;
    int bits = 16;
    size_t p = 12;
    bool have_format = false;
    while (p + 8 <= size) {
        const uint32_t len = rd32(data + p + 4);
        const uint8_t *body = data + p + 8;
        if (p + 8 + len > size)
            break;
        if (std::memcmp(data + p, "fmt ", 4) == 0 && len >= 16) {
            if (rd16(body) != 1)
                return false;   // PCM only
            out.channels = rd16(body + 2);
            out.rate = int(rd32(body + 4));
            bits = rd16(body + 14);
            have_format = true;
        } else if (std::memcmp(data + p, "data", 4) == 0 && have_format) {
            out.samples.clear();
            if (bits == 16) {
                out.samples.resize(len / 2);
                for (size_t i = 0; i < out.samples.size(); i++)
                    out.samples[i] = int16_t(rd16(body + i * 2));
            } else if (bits == 8) {
                out.samples.resize(len);
                for (size_t i = 0; i < len; i++)
                    out.samples[i] = int16_t((int(body[i]) - 128) << 8);
            } else {
                return false;
            }
            return out.channels >= 1 && out.channels <= 2 && !out.samples.empty();
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

SoundLibrary::SoundLibrary(Content &content) : content_(content)
{
    std::map<std::string, std::pair<size_t, int>> events;   // (a later file's mapping wins, as before)
    for (const std::string &name : content.names_ending(".hot")) {   // SoundData's and each expansion's
        std::vector<uint8_t> bytes;
        if (const FileRef *ref = content.file(name))
            if (read_ref(*ref, bytes))
                load_hot(std::string(bytes.begin(), bytes.end()), events);
        hots_.push_back(name);
    }
    events_.reserve(events.size());
    for (const auto &[folded, where] : events) {   // (in the map's order: sorted by name)
        events_.push_back({ uint32_t(event_text_.size()), uint32_t(where.first), where.second });
        event_text_ += folded;
        event_text_ += '\0';
    }
}

void SoundLibrary::add_hot(const std::string &name)
{
    if (std::find(hots_.begin(), hots_.end(), name) != hots_.end())
        return;
    hots_.push_back(name);
    std::vector<uint8_t> bytes;
    const FileRef *ref = content_.file(name);
    if (!ref || !read_ref(*ref, bytes))
        return;
    // The events as they are, back into the map the tables are read into, the new table's over them.
    std::map<std::string, std::pair<size_t, int>> events;
    for (const Event &e : events_)
        events[event_text_.c_str() + e.name] = { size_t(e.table), e.track };
    load_hot(std::string(bytes.begin(), bytes.end()), events);
    events_.clear();
    event_text_.clear();
    events_.reserve(events.size());
    for (const auto &[folded, where] : events) {
        events_.push_back({ uint32_t(event_text_.size()), uint32_t(where.first), where.second });
        event_text_ += folded;
        event_text_ += '\0';
    }
}

// Parsed into maps first, as before (a later line for the same id wins; the names' lists keep
// every line in order), then made into the table's arrays and the maps let go.
void SoundLibrary::load_hot(const std::string &text, std::map<std::string, std::pair<size_t, int>> &events)
{
    struct TrackLine {
        std::string name, routine;
        int patch = 0;
    };
    struct PatchLine {
        std::string file;
        bool loop = false;
    };
    std::map<int, TrackLine> tracks;
    std::map<int, PatchLine> patches;
    std::map<std::string, std::vector<int>> by_name;   // folded name without its last digits -> ids
    const size_t index = tables_.size();
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';')
            continue;
        if (line[0] == '[') {
            section = line;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq));
        const std::vector<std::string> f = split_fields(line.substr(eq + 1));
        if (section == "[EventMapping]") {
            if (f.size() >= 2 && f[0] == "kSndobPlay")
                events[fold_path(key)] = { index, std::atoi(f[1].c_str()) };
        } else if (section == "[Track]") {
            TrackLine tr;
            if (f.size() >= 2)
                tr.name = f[1];
            if (f.size() >= 8)
                tr.routine = f[7];
            if (f.size() >= 9)
                tr.patch = std::atoi(f[8].c_str());
            tracks[std::atoi(key.c_str())] = tr;
        } else if (section == "[Patch]") {
            if (f.size() < 3)
                continue;
            PatchLine pt;
            pt.file = base_file(f[2]);
            pt.loop = f.size() >= 4 && f[3] == "1";
            const int id = std::atoi(key.c_str());
            patches[id] = pt;
            // Without the trailing digits, so a track finds its variations.
            std::string stem = fold_path(f[1]);
            while (!stem.empty() && std::isdigit(uint8_t(stem.back())))
                stem.pop_back();
            by_name[stem].push_back(id);
        }
        // (The hit lists, [GlobalHitList], are their programs' to pick: not used here.)
    }
    tables_.emplace_back();
    Table &t = tables_.back();
    std::map<std::string, uint32_t> words;   // each word kept once (many patches share a file)
    auto word = [&](const std::string &w) {
        auto it = words.find(w);
        if (it != words.end())
            return it->second;
        const uint32_t at = uint32_t(t.text.size());
        t.text += w;
        t.text += '\0';
        words.emplace(w, at);
        return at;
    };
    t.tracks.reserve(tracks.size());
    for (const auto &[id, tr] : tracks)
        t.tracks.push_back({ id, word(tr.name), tr.patch, tr.routine.find("Looped") != std::string::npos });
    t.patches.reserve(patches.size());
    for (const auto &[id, pt] : patches)
        t.patches.push_back({ id, word(pt.file), pt.loop });
    t.named.reserve(by_name.size());
    for (const auto &[stem, ids] : by_name) {
        t.named.push_back({ word(stem), uint32_t(t.ids.size()), uint32_t(ids.size()) });
        t.ids.insert(t.ids.end(), ids.begin(), ids.end());
    }
    t.text.shrink_to_fit();
    t.ids.shrink_to_fit();
}

const SoundLibrary::Track *SoundLibrary::Table::track(int id) const
{
    auto it = std::lower_bound(tracks.begin(), tracks.end(), id, [](const Track &k, int v) { return k.id < v; });
    return it != tracks.end() && it->id == id ? &*it : nullptr;
}

const SoundLibrary::Patch *SoundLibrary::Table::patch(int id) const
{
    auto it = std::lower_bound(patches.begin(), patches.end(), id, [](const Patch &p, int v) { return p.id < v; });
    return it != patches.end() && it->id == id ? &*it : nullptr;
}

const SoundLibrary::Named *SoundLibrary::Table::by_name(const std::string &stem) const
{
    auto it = std::lower_bound(named.begin(), named.end(), stem,
                               [this](const Named &n, const std::string &s) { return std::strcmp(word(n.stem), s.c_str()) < 0; });
    return it != named.end() && stem == word(it->stem) ? &*it : nullptr;
}

bool SoundLibrary::resolve(const std::string &event, uint32_t pick, std::string &file, bool &loop, char voice) const
{
    const std::string key = fold_path(event);
    auto ev = std::lower_bound(events_.begin(), events_.end(), key, [this](const Event &e, const std::string &s) {
        return std::strcmp(event_text_.c_str() + e.name, s.c_str()) < 0;
    });
    if (ev == events_.end() || key != event_text_.c_str() + ev->name)
        return false;
    const Table &t = tables_[ev->table];
    const int track = ev->track;
    std::vector<int> choices;
    bool looped_track = false;
    auto by_name = [&](const std::string &stem) {
        if (const Named *n = t.by_name(fold_path(stem)))
            choices.assign(t.ids.begin() + std::ptrdiff_t(n->first), t.ids.begin() + std::ptrdiff_t(n->first + n->count));
    };
    if (const Track *k = t.track(track)) {
        const std::string name = t.word(k->name);
        looped_track = k->looped;
        if (k->patch != 0 && t.patch(k->patch))
            choices.push_back(k->patch);
        // (Hit lists aren't keyed by track: their programs pick them, so they aren't used here.)
        if (choices.empty())
            by_name(name);
        if (choices.empty())
            by_name(event);   // some tracks are misspelt ("tub_ssplash" plays tub_splash1-4)
        // Voices: "hungry_sated_vox" is spoken as vox_hungry_sated + f/m + a number.
        const std::string folded = fold_path(name);
        if (choices.empty() && folded.size() > 4 && folded.compare(folded.size() - 4, 4, "_vox") == 0) {
            const std::string base = "vox_" + folded.substr(0, folded.size() - 4);
            by_name(base + voice);
            if (choices.empty())
                by_name(base + (voice == 'f' ? 'm' : 'f'));
            if (choices.empty())
                by_name(base);
        }
    }
    if (choices.empty() && t.patch(track))
        choices.push_back(track);
    choices.erase(std::remove_if(choices.begin(), choices.end(), [&](int id) { return !t.patch(id); }), choices.end());
    if (choices.empty())
        return false;
    const Patch &p = *t.patch(choices[pick % choices.size()]);
    file = t.word(p.file);
    loop = p.loop || looped_track;
    return true;
}

std::string SoundLibrary::dump() const
{
    std::string out;
    for (const Event &e : events_) {
        const std::string event = event_text_.c_str() + e.name;
        for (char voice : { 'f', 'm' })
            for (uint32_t pick : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 1000003u }) {
                std::string file;
                bool loop = false;
                const bool ok = resolve(event, pick, file, loop, voice);
                out += event + ' ' + voice + ' ' + std::to_string(pick) + ": " + (ok ? file + (loop ? " loop" : "") : "-") + '\n';
            }
    }
    return out;
}

std::shared_ptr<const Pcm> SoundLibrary::sample(const std::string &file)
{
    auto it = samples_.find(file);
    if (it != samples_.end()) {
        it->second.used = ++use_clock_;
        return it->second.pcm;
    }
    std::shared_ptr<Pcm> pcm;
    const auto t_load = std::chrono::steady_clock::now();
    struct Count {   // (counted however load() ends)
        SoundLibrary &lib;
        std::chrono::steady_clock::time_point from;
        ~Count()
        {
            lib.decodes_++;
            lib.decode_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
        }
    } count{ *this, t_load };
    auto load = [&] {
        MemScope scope(MemKind::Sounds);   // (placed and counted as such on the Vita: core/memory_kind.h)
        std::vector<uint8_t> bytes;
        if (const FileRef *ref = content_.file(file))
            if (read_ref(*ref, bytes)) {
                auto p = std::make_shared<Pcm>();
                if (decode_xa(bytes.data(), bytes.size(), *p) || decode_wav(bytes.data(), bytes.size(), *p))
                    pcm = p;
            }
    };
    // The heap with no piece big enough (a long sample is a megabyte in one block): the samples
    // not playing let go and tried once more, else no sound this time; not kept as missing, as
    // one that can't be read is (it was then silent for good), nor thrown out of the frame (asked
    // for again every frame, it was the frame dropped every frame: the 2026-10-29 freeze).
    try {
        load();
    } catch (const std::bad_alloc &) {
        let_go_unused();
        try {
            load();
        } catch (const std::bad_alloc &) {
            return nullptr;
        }
    }
    keep(file, pcm);
    return pcm;
}

std::shared_ptr<const Pcm> SoundLibrary::cached(const std::string &file, bool &known)
{
    auto it = samples_.find(file);
    if (it != samples_.end()) {
        it->second.used = ++use_clock_;
        known = true;
        return it->second.pcm;
    }
    known = content_.file(file) != nullptr;
    return nullptr;
}

const FileRef *SoundLibrary::file_ref(const std::string &file) const
{
    return content_.file(file);
}

void SoundLibrary::keep(const std::string &file, std::shared_ptr<Pcm> pcm)
{
    const size_t size = pcm ? pcm->samples.size() * sizeof(int16_t) : 0;
    // Over the budget: the longest unused go first, never one still playing (held elsewhere).
    while (sample_bytes_ + size > kSampleBudget) {
        auto oldest = samples_.end();
        for (auto k = samples_.begin(); k != samples_.end(); ++k)
            if (k->second.pcm.use_count() <= 1 && (oldest == samples_.end() || k->second.used < oldest->second.used))
                oldest = k;
        if (oldest == samples_.end())
            break;   // all playing: kept over the budget for now
        sample_bytes_ -= oldest->second.bytes;
        samples_.erase(oldest);
    }
    if (auto old = samples_.find(file); old != samples_.end())   // (decoded twice: the newer kept)
        sample_bytes_ -= old->second.bytes;
    Kept &kept = samples_[file];
    kept.pcm = pcm;
    kept.used = ++use_clock_;
    kept.bytes = size;
    sample_bytes_ += size;
}

size_t SoundLibrary::let_go_unused()
{
    size_t gone = 0;
    for (auto k = samples_.begin(); k != samples_.end();)
        if (k->second.pcm.use_count() <= 1) {   // (playing: held by a voice too)
            gone += k->second.bytes;
            sample_bytes_ -= k->second.bytes;
            k = samples_.erase(k);
        } else {
            ++k;
        }
    return gone;
}

} // namespace ts
