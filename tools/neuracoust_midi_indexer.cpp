// Walks folders of Standard MIDI Files, parses each with the engine's SMF reader, and CLASSIFIES it —
// drum vs. melodic, instrument family, genre, and mood — into a TSV index the DAW's MIDI library loads.
// Content-based (channel 10 = drums, program changes = instrument, tempo + major/minor = mood), so it
// separates a drum groove from a piano part where a filename never could. Run as a subprocess by the DAW.
//
// Usage: neuracoust_midi_indexer <outputTSV> <root1> [root2 ...]
// stdout: PROGRESS <done> <total>   (periodic)   then   DONE <written>   |   ERROR <message>
// TSV columns: path \t name \t pack \t genre \t mood \t instrument \t isDrum(0|1) \t bpm \t isFill(0|1)
#include "project/TimelineExport.h"

#include <algorithm>
#include <array>
#include <map>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// GM program number (0-127) → a concise instrument family key. Localised in the Swift UI.
std::string programFamily(int program) {
    if (program < 0 || program > 127) return "Other";
    static const char* fam[16] = {
        "Piano", "Mallet", "Organ", "Guitar", "Bass", "Strings", "Strings", "Brass",
        "Reed", "Pipe", "Synth", "Synth", "Ethnic", "Percussion", "SFX", "SFX"};
    return fam[program / 8];
}

// Instrument family from a path/pack name — these single-instrument packs rarely set a GM program, so
// the folder ("Piano Collection", "Rock and Guitar Midis", "Bass Midis") is the reliable signal.
std::string pathInstrument(const std::string& upPath) {
    if (upPath.find("PIANO") != std::string::npos) return "Piano";
    if (upPath.find("GUITAR") != std::string::npos) return "Guitar";
    if (upPath.find("BASS") != std::string::npos) return "Bass";
    if (upPath.find("STRING") != std::string::npos) return "Strings";
    if (upPath.find("BRASS") != std::string::npos || upPath.find("TRUMPET") != std::string::npos ||
        upPath.find("TROMBONE") != std::string::npos) return "Brass";
    if (upPath.find("SAX") != std::string::npos || upPath.find("OBOE") != std::string::npos ||
        upPath.find("CLARINET") != std::string::npos || upPath.find("REED") != std::string::npos) return "Reed";
    if (upPath.find("FLUTE") != std::string::npos || upPath.find("PIPE") != std::string::npos) return "Pipe";
    if (upPath.find("ORGAN") != std::string::npos) return "Organ";
    if (upPath.find("SYNTH") != std::string::npos || upPath.find("PAD") != std::string::npos ||
        upPath.find("PLUCK") != std::string::npos || upPath.find("ARP") != std::string::npos) return "Synth";
    if (upPath.find("CHORD") != std::string::npos || upPath.find("KEYS") != std::string::npos ||
        upPath.find("KEYBOARD") != std::string::npos) return "Keys";
    if (upPath.find("LEAD") != std::string::npos || upPath.find("MELOD") != std::string::npos) return "Lead";
    return "";
}

// First standalone integer in a musical BPM range (60–220) — used only as a fallback for the display
// name; the authoritative tempo comes from the file's tempo meta event.
int filenameBpm(const std::string& name) {
    std::string d; int best = 0;
    auto flush = [&]() {
        if (best == 0 && !d.empty()) { int v = std::atoi(d.c_str()); if (v >= 60 && v <= 220) best = v; }
        d.clear();
    };
    for (char c : name) { if (std::isdigit(static_cast<unsigned char>(c))) d += c; else flush(); }
    flush();
    return best;
}

const std::array<const char*, 40> kGenreKeywords = {
    "JAZZ", "ROCK", "FUNK", "SOUL", "POP", "RNB", "R&B", "HOUSE", "TECHNO", "TRAP", "DNB",
    "DUBSTEP", "PSYTRANCE", "PSY", "TRANCE", "AMBIENT", "CINEMATIC", "BLUES", "LATIN", "REGGAE",
    "METAL", "HIPHOP", "HIP HOP", "SWING", "SHUFFLE", "COUNTRY", "GOSPEL", "AFRO", "DISCO",
    "ELECTRONIC", "EDM", "DANCE", "GARAGE", "BREAKBEAT", "ORCHESTRAL", "WORLD", "PUNK", "FUSION",
    "BOSSA", "SAMBA"};

// A path component that is just an index ("01", "02@BASIC", "03 - SWING") is a sort key, not a genre.
bool looksLikeIndexFolder(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '@' || s[i] == ' ' || s[i] == '-' || s[i] == '.')) ++i;
    return i >= 2 && i > s.size() / 2;   // mostly leading digits/separators
}

std::string genreFromRel(const std::string& rel, const std::string& pack) {
    const std::string up = toUpper(rel);
    for (const char* kw : kGenreKeywords)
        if (up.find(kw) != std::string::npos) return kw;
    // Fall back to the first meaningful sub-folder (skip index-only folders and the filename).
    std::vector<std::string> comps;
    std::string cur;
    for (char c : rel) { if (c == '/') { if (!cur.empty()) comps.push_back(cur); cur.clear(); } else cur += c; }
    if (!comps.empty()) comps.pop_back();   // drop the filename
    for (auto& c : comps) {
        if (looksLikeIndexFolder(c)) continue;
        std::string g = c; for (char& ch : g) if (ch == '@' || ch == '_') ch = ' ';
        return g;
    }
    return pack;
}

// Krumhansl–Kessler major/minor decision on a pitch-class histogram: returns true if the best-correlating
// key is major. Root is irrelevant here — mood only cares about mode.
bool isMajorMode(const std::array<double, 12>& pc) {
    static const std::array<double, 12> maj {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const std::array<double, 12> min {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    double total = 0.0; for (double v : pc) total += v;
    if (total <= 1e-9) return true;
    const double cMean = total / 12.0;
    auto mean = [](const std::array<double, 12>& p) { double s = 0; for (double v : p) s += v; return s / 12.0; };
    const double mjM = mean(maj), mnM = mean(min);
    auto corr = [&](const std::array<double, 12>& prof, double pM, int root) {
        double n = 0, dc = 0, dp = 0;
        for (int i = 0; i < 12; ++i) {
            const double c = pc[static_cast<size_t>(i)] - cMean;
            const double p = prof[static_cast<size_t>((i - root + 12) % 12)] - pM;
            n += c * p; dc += c * c; dp += p * p;
        }
        const double d = std::sqrt(dc * dp); return d > 1e-12 ? n / d : 0.0;
    };
    double bestMaj = -2, bestMin = -2;
    for (int r = 0; r < 12; ++r) { bestMaj = std::max(bestMaj, corr(maj, mjM, r)); bestMin = std::max(bestMin, corr(min, mnM, r)); }
    return bestMaj >= bestMin;
}

// Mood from mode (reliable, from the pitch histogram) refined by tempo ONLY when the tempo is trustworthy
// (a filename BPM). The SMF reader defaults an absent tempo to 120, so keying mood off that would tag
// everything "energetic" — mode is the honest primary signal.
std::string moodFor(bool major, int bpm, bool tempoReliable) {
    if (!tempoReliable || bpm <= 0) return major ? "bright" : "dark";
    if (major) {
        if (bpm >= 120) return "energetic";
        if (bpm <= 85) return "calm";
        return "bright";
    } else {
        if (bpm >= 120) return "tense";
        if (bpm <= 85) return "dark";
        return "melancholic";
    }
}

struct Row {
    std::string path, name, pack, genre, mood, instrument;
    bool isDrum = false; int bpm = 0; bool isFill = false;
};

// Parse + classify one MIDI file. Returns false only if it can't be read at all.
bool classify(const fs::path& file, const std::string& pack, const std::string& rootPath, Row& out) {
    out.path = file.string();
    out.name = file.stem().string();
    out.pack = pack;
    std::string rel = out.path.rfind(rootPath, 0) == 0 ? out.path.substr(rootPath.size()) : out.path;
    out.genre = genreFromRel(rel, pack);
    out.isFill = toUpper(out.name).find("FILL") != std::string::npos;

    neuracoust::daw::MidiImportResult r = neuracoust::daw::readProjectMidiFile(file);
    if (!r.ok) {
        // Unreadable — keep it in the index from filename alone (drum guess from the path).
        out.bpm = filenameBpm(out.name);
        out.isDrum = toUpper(rel).find("DRUM") != std::string::npos;
        out.instrument = out.isDrum ? "Drums" : "Unknown";
        out.mood = "";
        return true;
    }
    long long totalNotes = 0, ch10Notes = 0, kitNotes = 0, coreNotes = 0;
    // Simultaneity, for telling a chord from a kit. Notes that begin on the same beat form an onset;
    // an onset counts as harmonic when two of its pitches sit a third-to-fifth apart.
    long long chordOnsets = 0, harmonicOnsets = 0;
    int lowestPitch = 127;
    std::array<double, 12> pc {};                 // pitch-class histogram of MELODIC notes only
    std::array<long long, 128> programCount {};
    programCount.fill(0);
    std::array<bool, 128> pitchSeen {};
    pitchSeen.fill(false);
    std::array<long long, 128> pitchCount {};
    pitchCount.fill(0);
    long long melodicNotes = 0;
    int minPitch = 127, maxPitch = 0;
    // GM percussion key numbers (kick/snare/hats/toms/cymbals/etc.) — a note here is a "kit" hit whatever
    // channel it is on, because this collection puts drums on ch 1, not the GM ch 10.
    auto isKitKey = [](int p) { return p >= 35 && p <= 81; };   // full GM percussion map
    bool hasKick = false, hasSnare = false, hasHat = false;
    std::vector<std::string> regionFamilies;   // one instrument family per source track (for "Multi")
    for (const auto& region : r.project.midiRegions) {
        if (!region.notes.empty()) {
            // This source track's family: its dominant program, else its register.
            int rProg = -1; long long rBest = 0;
            std::array<long long, 128> rProgCount {}; rProgCount.fill(0);
            for (const auto& pe : region.programChangeEvents)
                if (pe.channel != 10 && pe.program >= 0 && pe.program < 128) ++rProgCount[static_cast<size_t>(pe.program)];
            for (int p = 0; p < 128; ++p) if (rProgCount[static_cast<size_t>(p)] > rBest) { rBest = rProgCount[static_cast<size_t>(p)]; rProg = p; }
            long long rCh10 = 0, rNotes = 0, rKit = 0; int rMin = 127, rMax = 0;
            for (const auto& n : region.notes) {
                ++rNotes;
                if (n.channel == 10) ++rCh10;
                if (n.pitch >= 35 && n.pitch <= 52) ++rKit;
                rMin = std::min(rMin, n.pitch); rMax = std::max(rMax, n.pitch);
            }
            std::string fam;
            if (rCh10 >= rNotes / 2 || (rKit >= (rNotes * 80) / 100 && rProg < 0)) fam = "Drums";
            else if (rProg >= 0) fam = programFamily(rProg);
            else { const int med = (rMin + rMax) / 2; fam = med < 48 ? "Bass" : (med < 60 ? "Keys" : "Lead"); }
            regionFamilies.push_back(fam);
        }
        // Group this region's notes by start time and look for CHORDS — the one thing a drum kit
        // never plays. Two traps, both hit while measuring this collection:
        //
        //  • Group coarsely (a 1/64 note) and a flam merges with the hit next to it, inventing a
        //    simultaneity that was not played. Round to a 1/512 note instead: fine enough to keep a
        //    flam separate, coarse enough that a quantised chord still lands on one key.
        //  • "Two notes a third to a fifth apart" is not a chord when the notes are drums. Kick 36 +
        //    hat 42 is a sixth; snare 38 + crash 49 an eleventh. Every groove in the library scored
        //    as harmonic. A chord needs THREE pitches stacked in thirds, which a kit does not do.
        {
            std::map<long long, std::vector<int>> onsets;
            for (const auto& note : region.notes) {
                onsets[std::llround(note.startBeats * 128.0)].push_back(note.pitch);
            }
            for (auto& [beat, pitches] : onsets) {
                (void)beat;
                ++chordOnsets;
                if (pitches.size() < 3) continue;
                std::sort(pitches.begin(), pitches.end());
                pitches.erase(std::unique(pitches.begin(), pitches.end()), pitches.end());
                for (size_t k = 0; k + 2 < pitches.size(); ++k) {
                    const int lower = pitches[k + 1] - pitches[k];
                    const int upper = pitches[k + 2] - pitches[k + 1];
                    if (lower >= 3 && lower <= 5 && upper >= 3 && upper <= 5) { ++harmonicOnsets; break; }
                }
            }
        }
        for (const auto& note : region.notes) {
            ++totalNotes;
            lowestPitch = std::min(lowestPitch, note.pitch);
            pitchSeen[static_cast<size_t>(std::max(0, std::min(127, note.pitch)))] = true;
            ++pitchCount[static_cast<size_t>(std::max(0, std::min(127, note.pitch)))];
            if (note.channel == 10) ++ch10Notes;
            if (isKitKey(note.pitch)) {
                ++kitNotes;
                if (note.pitch >= 35 && note.pitch <= 52) ++coreNotes;   // kick/snare/hats/low toms
                if (note.pitch == 35 || note.pitch == 36) hasKick = true;
                if (note.pitch == 38 || note.pitch == 40) hasSnare = true;
                if (note.pitch == 42 || note.pitch == 44 || note.pitch == 46) hasHat = true;
            }
            if (note.channel != 10) {
                ++melodicNotes;
                pc[static_cast<size_t>(((note.pitch % 12) + 12) % 12)] += 1.0;
                minPitch = std::min(minPitch, note.pitch);
                maxPitch = std::max(maxPitch, note.pitch);
            }
        }
        for (const auto& pcEvt : region.programChangeEvents)
            if (pcEvt.channel != 10 && pcEvt.program >= 0 && pcEvt.program < 128)
                ++programCount[static_cast<size_t>(pcEvt.program)];
    }
    // Filenames in these libraries carry the intended groove tempo far more reliably than the embedded
    // tempo meta, so prefer it; fall back to the file's tempo, then nothing.
    const int fnBpm = filenameBpm(out.name);
    out.bpm = fnBpm > 0 ? fnBpm : (r.project.tempoBpm > 0 ? r.project.tempoBpm : 0);

    int distinctPitches = 0;
    for (bool s : pitchSeen) if (s) ++distinctPitches;
    const std::string upPath = toUpper(rel) + "\n" + toUpper(pack);
    const bool pathDrum = upPath.find("DRUM") != std::string::npos ||
                          toUpper(out.name).rfind("MIDIDRUM", 0) == 0 ||
                          upPath.find("EZDRUMMER") != std::string::npos;
    bool hasMelodicProgram = false;   // a pitched-instrument program (drums are 0 program / channel 10)
    for (int p = 0; p < 112; ++p) if (programCount[static_cast<size_t>(p)] > 0) hasMelodicProgram = true;

    // Drum if: it lives in a drum pack with no pitched program (these files put the kit on ch 1, so path
    // is the reliable signal), OR GM ch10 dominates, OR a kit signature (kick+snare/hat packed into the
    // GM percussion keys with few distinct pitches) catches a stray drum file in a non-drum folder.
    // Chord detection first: a stacked triad vetoes every drum rule below it, including the kit
    // signature — a wide "B Maj" voicing with its root on key 35 and a note on 42 otherwise reads
    // as kick-plus-hat, which is how 57 chord files landed in the drum library.
    const bool harmonic = harmonicOnsets * 10 >= chordOnsets;                    // > 10 % of onsets
    const bool ch10Dominant = totalNotes > 0 && ch10Notes >= totalNotes / 2;
    // Strict, so it doesn't fire on a sparse piano intro: the rhythmic core [35,52] must dominate (piano
    // melody never packs 90% of its notes that low), with the kick+snare/hat pattern and few pitches.
    const bool kitSignature = totalNotes > 0 && coreNotes >= (totalNotes * 90) / 100 &&
                              hasKick && (hasSnare || hasHat) && distinctPitches <= 12 &&
                              !hasMelodicProgram && !harmonic;
    // A folder name is a hint, never a verdict. "Drum Kits Midis" and friends are producer packs:
    // they ship the chords, pads and leads that came with the kit alongside the grooves, and taking
    // the path at its word filed all of it under Drums — measured on this collection, "01. Chords"
    // (a 4-note chord stab) and "02. Saw" (a lead at C5–A5) both came back as drum grooves.
    //
    // Three content vetoes, each measured against real EZdrummer/Groove-Monkee grooves, which score
    // harmonic 0.00 every time and always put something in the percussion register:
    //   • a kit does not play thirds and fifths — any harmony at all disqualifies it
    //   • a kit always has a low note; a lead sitting entirely above middle C is not one
    //   • a groove REPEATS its few pitches; four sustained notes is a pad, not a beat
    const bool tooHigh = totalNotes > 0 && lowestPitch >= 60;
    const bool repeats = distinctPitches > 0 && totalNotes >= 8 &&
                         totalNotes >= 3 * distinctPitches;
    // A kit's pitches are SCATTERED — a handful of articulation keys spread over two or three
    // octaves. A bass line or a riff uses about as many pitches inside one octave, which is what
    // tells the two apart once neither plays chords. (A one- or two-pitch loop is a hit pattern by
    // definition; there is no melody in it to lose.)
    const int pitchSpan = (totalNotes > 0 && maxPitch >= minPitch) ? (maxPitch - minPitch) : 0;
    const bool scattered = pitchSpan * 2 >= distinctPitches * 5;
    // One pitch carries a groove — the hat or the kick is half the notes in the bar. A riff spreads
    // its notes around instead. Without this a palm-muted two-note power chord looked exactly like a
    // kick-and-snare pattern: few pitches, heavy repetition, no triad.
    long long topPitchCount = 0;
    for (long long c : pitchCount) topPitchCount = std::max(topPitchCount, c);
    const bool oneVoiceDominates = totalNotes > 0 && topPitchCount * 3 >= totalNotes;       // ≥ 33 %
    // A busy fill has no dominant voice, so dominance alone would lose it. But a file that carries
    // BOTH a kick key and a snare/hat key is a kit whatever else it does — a guitar riff reaching
    // key 40 (Electric Snare, and also E2) never also reaches 35/36, two octaves below open E.
    const bool kitKeysPresent = hasKick && (hasSnare || hasHat);
    // The real drum test, and it never asks what the folder is called. Modern drum libraries put
    // articulations anywhere from key 15 to key 90 — the old [35,52] window called a Groove Monkee
    // paradiddle a Keys part because its hat-pedal keys sit below 35 and its ride bell above 52.
    const bool contentDrum = totalNotes > 0 && distinctPitches <= 12 && repeats && scattered &&
                             (oneVoiceDominates || kitKeysPresent) &&
                             !harmonic && !tooHigh && !hasMelodicProgram;
    // The path is left as a weaker corroborator: it lets a sparser groove through inside a folder
    // that says drums, but it can no longer overrule the content.
    const bool packDrum = pathDrum && !hasMelodicProgram && totalNotes > 0 &&
                          !harmonic && !tooHigh && distinctPitches <= 16;
    out.isDrum = ch10Dominant || kitSignature || contentDrum || packDrum;
    if (std::getenv("NC_MIDI_INDEX_DEBUG") != nullptr) {
        std::fprintf(stderr, "DBG %s notes=%lld distinct=%d low=%d harm=%lld/%lld melodicProg=%d "
                             "pathDrum=%d ch10=%d kitSig=%d content=%d packDrum=%d -> drum=%d\n",
                     out.name.c_str(), totalNotes, distinctPitches, lowestPitch,
                     harmonicOnsets, chordOnsets, static_cast<int>(hasMelodicProgram),
                     static_cast<int>(pathDrum), static_cast<int>(ch10Dominant),
                     static_cast<int>(kitSignature), static_cast<int>(contentDrum),
                     static_cast<int>(packDrum),
                     static_cast<int>(out.isDrum));
    }
    if (out.isDrum) {
        out.instrument = "Drums";
        out.mood = "";   // grooves are rhythm, not harmony
        return true;
    }
    // Multi-track song: two or more source tracks with DISTINCT instrument families → a full arrangement,
    // not a single-instrument loop. Labelled "Multi" so it's told apart (and dragging it splits the parts).
    std::vector<std::string> distinctFam;
    for (const auto& f : regionFamilies)
        if (std::find(distinctFam.begin(), distinctFam.end(), f) == distinctFam.end()) distinctFam.push_back(f);
    if (distinctFam.size() >= 2) {
        out.instrument = "Multi";
        out.mood = melodicNotes > 0 ? moodFor(isMajorMode(pc), out.bpm, fnBpm > 0) : "";
        return true;
    }
    // Instrument family: the most-used program; else infer from register.
    int bestProg = -1; long long bestCount = 0;
    for (int p = 0; p < 128; ++p) if (programCount[static_cast<size_t>(p)] > bestCount) { bestCount = programCount[static_cast<size_t>(p)]; bestProg = p; }
    const std::string pathInst = pathInstrument(upPath);
    if (bestProg >= 0) {
        out.instrument = programFamily(bestProg);
    } else if (!pathInst.empty()) {
        out.instrument = pathInst;              // "Piano Collection" / "Guitar Midis" etc.
    } else if (melodicNotes > 0) {
        const int median = (minPitch + maxPitch) / 2;
        out.instrument = median < 48 ? "Bass" : (median < 60 ? "Keys" : "Lead");
    } else {
        out.instrument = "Unknown";
    }
    out.mood = melodicNotes > 0 ? moodFor(isMajorMode(pc), out.bpm, fnBpm > 0) : "";
    return true;
}

std::string tsvEscape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) { if (c == '\t' || c == '\n' || c == '\r') o += ' '; else o += c; }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("ERROR usage: neuracoust_midi_indexer <outputTSV> <root1> [root2 ...]\n");
        return 2;
    }
    const std::string outPath = argv[1];
    std::vector<std::pair<fs::path, std::string>> roots;   // (root, pack label)
    for (int i = 2; i < argc; ++i) {
        fs::path r = argv[i];
        roots.emplace_back(r, r.filename().string());
    }

    // 1) Enumerate every .mid up front so work parallelises cleanly and progress is a real fraction.
    std::vector<std::pair<fs::path, size_t>> files;   // (file, root index)
    for (size_t ri = 0; ri < roots.size(); ++ri) {
        std::error_code ec;
        if (!fs::exists(roots[ri].first, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(roots[ri].first,
                 fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = toUpper(it->path().extension().string());
            if (ext == ".MID" || ext == ".MIDI") files.emplace_back(it->path(), ri);
        }
    }
    const size_t total = files.size();
    if (total == 0) { std::printf("ERROR no MIDI files found under the given roots\n"); return 1; }

    // 2) Classify in parallel. Each worker fills its own row bucket; merge at the end (order irrelevant).
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const unsigned workers = std::min(hw > 1 ? hw - 1 : 1, 12u);
    std::vector<std::vector<Row>> buckets(workers);
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    auto worker = [&](unsigned w) {
        std::vector<Row>& out = buckets[w];
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= total) break;
            const auto& [file, ri] = files[i];
            Row row;
            if (classify(file, roots[ri].second, roots[ri].first.string(), row)) out.push_back(std::move(row));
            const size_t d = done.fetch_add(1) + 1;
            if ((d % 4000) == 0) { std::printf("PROGRESS %zu %zu\n", d, total); std::fflush(stdout); }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w) pool.emplace_back(worker, w);
    for (auto& t : pool) t.join();

    // 3) Write the TSV.
    std::FILE* f = std::fopen(outPath.c_str(), "wb");
    if (f == nullptr) { std::printf("ERROR cannot open output %s\n", outPath.c_str()); return 1; }
    size_t written = 0;
    std::string line;
    for (const auto& bucket : buckets) {
        for (const auto& r : bucket) {
            line.clear();
            line += tsvEscape(r.path); line += '\t';
            line += tsvEscape(r.name); line += '\t';
            line += tsvEscape(r.pack); line += '\t';
            line += tsvEscape(r.genre); line += '\t';
            line += r.mood; line += '\t';
            line += r.instrument; line += '\t';
            line += r.isDrum ? "1" : "0"; line += '\t';
            line += std::to_string(r.bpm); line += '\t';
            line += r.isFill ? "1" : "0"; line += '\n';
            std::fwrite(line.data(), 1, line.size(), f);
            ++written;
        }
    }
    std::fclose(f);
    std::printf("DONE %zu\n", written);
    std::fflush(stdout);
    return 0;
}
