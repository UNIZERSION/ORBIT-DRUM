#include "project.h"

#include "synthmap.h"

#include <initializer_list>
#include <math.h>
#include <string.h>

#ifndef ORBIT_HOT
#define ORBIT_HOT
#endif

namespace orbit {

namespace {

// Canonical pad layout shared by the built-in kits (tools/kitgen writes them in this order):
//  0 Kick  1 Snare  2 Cl Hat  3 Op Hat  4 Clap  5 Rim  6 Lo Tom  7 Mid Tom  8 Hi Tom
//  9 Crash/Cymbal  10 Ride/Cowbell  11..15 kit specific
const char* const kPadNames[kPads] = {"Kick", "Snare", "Cl Hat", "Op Hat", "Clap", "Rim", "Lo Tom", "Mid Tom",
                                      "Hi Tom", "Crash", "Ride", "Kick 2", "Snare 2", "Op Hat 2", "Clap 2", "Kick 3"};
const uint8_t kPadBus[kPads] = {0, 1, 2, 2, 1, 1, 4, 4, 4, 5, 5, 0, 1, 2, 1, 0};
const uint8_t kPadChoke[kPads] = {0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
// Bus 8 takes the ACID BASS by default (GP_BASS_BUS); no pad is routed there.
// Bus 7 takes the SYNTH by default (GP_SYN_BUS, 0.4.9; it was the unused "FX" channel).
const char* const kBusNames[kBuses] = {"KICK", "SNARE", "HATS", "PERC", "TOMS", "CYMB", "SYNTH", "BASS"};

void set(Scene& s, int pad, std::initializer_list<int> steps, int vel = 100)
{
    for (int st : steps) {
        Step& x = s.track[pad].step[st];
        x.on = 1;
        x.vel = (uint8_t)vel;
    }
}

// ACID BASS demo line: {step, note (semitones from C2), accent, slide}
struct BassNote {
    int8_t step, note, accent, slide;
};
void bass(Scene& s, std::initializer_list<BassNote> notes)
{
    for (const BassNote& n : notes) {
        BassStep& b = s.bass.step[n.step];
        b.gate = 1;
        b.note = n.note;
        b.accent = (uint8_t)n.accent;
        b.slide = (uint8_t)n.slide;
    }
}

// The pads of the built-in kits (kPadNames order)
enum : int {
    kKick = 0, kSnare = 1, kClHat = 2, kOpHat = 3, kClap = 4, kRim = 5, kLoTom = 6, kMidTom = 7, kHiTom = 8,
    kCrash = 9, kRide = 10, kKickLong = 11
};

// Every quarter note of both bars of a 2-bar scene: the step inside the beat (0..3) plus 4 * beat.
void beats(Scene& s, int pad, int offset, int vel)
{
    for (int b = 0; b < 8; ++b) set(s, pad, {b * 4 + offset}, vel);
}

// SYNTH line helper: a chord (or one note) of up to 4 notes on a step.
void chord(Scene& s, int step, int len, int vel, std::initializer_list<int> notes)
{
    SynthStep& st = s.synth.step[step];
    int i = 0;
    for (int n : notes) {
        if (i >= kSynthStepNotes) break;
        st.n[i].note = (uint8_t)n;
        st.n[i].vel = (uint8_t)vel;
        st.n[i].len = (uint8_t)len;
        ++i;
    }
}

// The demo's drums, both bars: four on the floor (kick), a clap on 2 and 4, closed hats (16ths with the offbeats
// loud, or only the offbeat 8ths), open hats on the "and" of 2 and 4 (they choke the closed ones).
void demoKick(Scene& s, int vel) { beats(s, kKick, 0, vel); }
void demoClap(Scene& s, int vel)
{
    for (int st : {4, 12, 20, 28}) set(s, kClap, {st}, vel);
}
void demoHats16(Scene& s)
{
    beats(s, kClHat, 0, 46);
    beats(s, kClHat, 1, 58);
    beats(s, kClHat, 2, 92);
    beats(s, kClHat, 3, 58);
}
void demoOpenHats(Scene& s, int vel)
{
    for (int st : {6, 14, 22, 30}) {
        s.track[kClHat].step[st] = Step{};
        set(s, kOpHat, {st}, vel);
    }
}

// The acid line (A minor, semitones from C2) over the chords Am F C G, half a bar each: a rolling 16th line with
// octave jumps, slides into the next note and accents on the chord roots.
void demoRiff(Scene& s)
{
    bass(s, {{0, -3, 1, 0},  {2, 9, 0, 1},   {3, -3, 0, 0},  {5, 4, 0, 0},   {6, -3, 1, 0},  {7, 7, 0, 1},
             {8, -7, 1, 0},  {10, 5, 0, 1},  {11, 0, 0, 0},  {13, -7, 0, 0}, {14, -3, 1, 0}, {15, 0, 0, 1},
             {16, 0, 1, 0},  {18, 12, 0, 1}, {19, -5, 0, 0}, {21, 0, 0, 0},  {22, 4, 1, 0},  {23, 7, 0, 1},
             {24, -5, 1, 0}, {26, 7, 0, 0},  {27, 2, 0, 1},  {28, -5, 0, 0}, {29, -1, 0, 0}, {30, 2, 1, 1},
             {31, 4, 0, 0}});
}

// The break's line: the chord roots held with slides up the octave, climbing into the drop at the end.
void demoBreakRiff(Scene& s)
{
    bass(s, {{0, -3, 1, 0},  {3, 9, 0, 1},   {4, 7, 0, 0},   {8, -7, 1, 0},  {11, 5, 0, 1},  {12, 4, 0, 0},
             {16, 0, 1, 0},  {19, 12, 0, 1}, {20, 11, 0, 0}, {24, -5, 1, 0}, {27, 7, 0, 1},  {28, 9, 0, 0},
             {30, 11, 1, 1}, {31, 12, 0, 0}});
}

// The only demo (0.4.9, our own track): "ORBIT DRIVE", 124 BPM, A minor, the chords Am F C G (half a bar each).
// The three instruments - the drums, the 303 and the SYNTH - build it up; the song plays the parts in order:
//   A intro   kick, offbeat hats, a rim; the SYNTH holds the chords
//   B groove  + clap, 16th hats, open hats; the acid line; offbeat SYNTH stabs
//   C break   no kick: clap, hats, a tom and snare fill; the acid line held; a SYNTH arpeggio
//   D drop    everything: crash, ride, the acid line, the SYNTH hook over short chord dyads
void orbitDemo(Project& p)
{
    p.bpm = 124.0f;
    for (int i = 0; i < kScenes; ++i) {
        p.scene_data[i].length = 32;
        p.scene_data[i].swing = 54;
    }
    // the voicings (MIDI notes, around C4..C6 where the SYNTH view shows them): Am F C G
    const int am[3] = {64, 69, 72}, f[3] = {65, 69, 72}, c[3] = {64, 67, 72}, g[3] = {62, 67, 71};
    const int* const chords[4] = {am, f, c, g};

    Scene& a = p.scene_data[0];   // A: intro
    demoKick(a, 108);
    for (int b = 0; b < 8; ++b) set(a, kClHat, {b * 4 + 2}, 84);
    set(a, kRim, {14, 30}, 64);
    for (int h = 0; h < 4; ++h) chord(a, h * 8, h == 3 ? 8 : 7, 78, {chords[h][0], chords[h][1], chords[h][2]});

    Scene& b = p.scene_data[1];   // B: groove
    demoKick(b, 116);
    demoClap(b, 104);
    demoHats16(b);
    demoOpenHats(b, 72);
    set(b, kRim, {3, 11, 19, 27}, 52);
    demoRiff(b);
    for (int h = 0; h < 4; ++h)
        for (int st : {2, 6}) chord(b, h * 8 + st, 1, st == 2 ? 78 : 70, {chords[h][0], chords[h][1], chords[h][2]});

    Scene& c2 = p.scene_data[2];   // C: break
    demoClap(c2, 88);
    for (int b2 = 0; b2 < 8; ++b2) set(c2, kClHat, {b2 * 4 + 2}, 76);
    set(c2, kKickLong, {0}, 100);
    set(c2, kLoTom, {24}, 92);
    set(c2, kMidTom, {26}, 96);
    set(c2, kHiTom, {27}, 100);
    set(c2, kSnare, {28}, 64);
    set(c2, kSnare, {29}, 80);
    set(c2, kSnare, {30}, 96);
    set(c2, kSnare, {31}, 112);
    demoBreakRiff(c2);
    {
        const int arp[16] = {69, 72, 76, 72, 69, 72, 77, 72, 67, 72, 76, 72, 67, 71, 74, 79};
        for (int i = 0; i < 16; ++i) chord(c2, i * 2, 1, i % 4 == 0 ? 92 : 70, {arp[i]});
    }

    Scene& d = p.scene_data[3];   // D: drop
    demoKick(d, 118);
    demoClap(d, 108);
    demoHats16(d);
    demoOpenHats(d, 80);
    beats(d, kRide, 0, 54);
    set(d, kCrash, {0}, 104);
    demoRiff(d);
    {
        struct Note {
            int step, len, note;
        };
        const Note hook[] = {{0, 2, 76},  {3, 1, 74},  {4, 2, 72},  {6, 2, 69},  {8, 2, 72},  {11, 1, 69},
                             {12, 1, 72}, {14, 2, 77}, {16, 3, 76}, {19, 1, 74}, {20, 2, 72}, {22, 2, 67},
                             {24, 2, 71}, {26, 2, 74}, {28, 3, 79}, {31, 1, 81}};
        for (const Note& n : hook) chord(d, n.step, n.len, n.step % 8 == 0 ? 100 : 86, {n.note});
        // short dyads on the offbeats under the hook (the root's third and fifth, below it)
        const int dy[4][2] = {{60, 64}, {60, 65}, {60, 64}, {59, 62}};
        for (int h = 0; h < 4; ++h)
            for (int st : {2, 6}) {
                SynthStep& sst = d.synth.step[h * 8 + st];
                if (sst.n[0].note) continue;   // (the hook does not land on these steps)
                chord(d, h * 8 + st, 1, 64, {dy[h][0], dy[h][1]});
            }
    }

    // Song: A A B B C C D D B A, then end. FOLLOW stays off until the user turns it on.
    const int8_t chain[10] = {0, 0, 1, 1, 2, 2, 3, 3, 1, 0};
    for (int i = 0; i < kSongSlots; ++i) p.song.slot[i] = i < 10 ? chain[i] : -1;

    // The 303: a round, squelchy line (moderate cutoff and drive), a little delay
    p.g[GP_BASS_CUTOFF] = 0.36f;
    p.g[GP_BASS_RESO] = 0.72f;
    p.g[GP_BASS_ENVMOD] = 0.56f;
    p.g[GP_BASS_DECAY] = 0.42f;
    p.g[GP_BASS_ACCENT] = 0.70f;
    p.g[GP_BASS_DRIVE] = 0.45f;
    p.g[GP_BASS_LEVEL] = -3.0f;
    p.g[GP_BASS_SEND] = 0.12f;
    // The SYNTH: the BRASS patch with less vibrato, under the drums, into the delay and the reverb
    synthApplyPreset(0, p.g);   // BRASS
    p.g[GP_SYN_LFO_PITCH] = 0.02f;
    p.g[GP_SYN_LEVEL] = -10.0f;
    p.g[GP_SYN_SEND] = 0.24f;
    p.g[GP_DLY_FEEDBACK] = 0.32f;
    p.g[GP_DLY_LEVEL] = 0.36f;
    // hats a little right, the open hat a little left
    p.pad[kClHat].p[PP_PAN] = 0.12f;
    p.pad[kOpHat].p[PP_PAN] = -0.12f;
}

}  // namespace

// Zeroes the bytes [from, to) of an object (padding included, so zeroed structs compare equal).
static inline void zeroRange(void* from, const void* to)
{
    char* a = static_cast<char*>(from);
    const char* b = static_cast<const char*>(to);
    if (b > a) memset(a, 0, (size_t)(b - a));
}

// Everything but the demo: parameter defaults, pad and bus names, empty scenes and song.
static void baseDefaults(Project& p)
{
    // p may be the LIVE project the audio task reads every block (App::newProject resets it in
    // place while a queued Stop is not processed yet). A whole-object memset would point every
    // layer at engine slot 0 (the old kit's first sample) until the pad loop below rewrote it, so
    // a hit in that window played the wrong sample. Layer slots go straight to -1 (one store
    // each, never through 0) and everything else is zeroed around them.
    for (auto& pad : p.pad)
        for (auto& L : pad.layer) __atomic_store_n(&L.slot, (int16_t)-1, __ATOMIC_RELAXED);
    zeroRange(&p, &p.pad[0]);
    for (auto& pad : p.pad) {
        zeroRange(&pad, &pad.layer[0]);
        for (auto& L : pad.layer) zeroRange(&L._pad0, &L + 1);   // all of Layer but slot
        zeroRange(&pad.layer[kLayers - 1] + 1, &pad + 1);
    }
    zeroRange(&p.pad[kPads - 1] + 1, &p + 1);
    strncpy(p.name, "ORBIT_01", sizeof(p.name) - 1);
    p.bpm = 120.0f;
    p.scene = 0;
    p.beats_per_bar = 4;
    p.quant = LaunchQuant::Bar;
    p.metronome = 0;
    p.count_in = 0;
    p.lofi_on = p.wide_on = p.pump_on = p.space_on = 0;
    p.limiter_on = 1;
    p.pump_src = 0;

    for (int i = 0; i < kPads; ++i) {
        Pad& pad = p.pad[i];
        strncpy(pad.name, kPadNames[i], kNameLen - 1);
        for (int l = 0; l < kLayers; ++l) {   // Layer{} field by field: slot is already -1 (see above)
            Layer& L = pad.layer[l];
            L.tune = 0.0f;
            L.gain_db = 0.0f;
            L.pan = 0.0f;
            L.vel_lo = 0;
            L.vel_hi = 127;
        }
        pad.layer_mode = LayerMode::Single;
        pad.mode = PlayMode::OneShot;
        pad.reverse = 0;
        pad.bus = kPadBus[i];
        pad.choke = kPadChoke[i];
        pad.poly = 4;
        pad.mute = pad.solo = 0;
        pad.slice_count = 0;
        pad.slice_sel = 0;
        pad.slice_end = 1.0f;
        for (int k = 0; k < kPadParamCount; ++k) pad.p[k] = padSpec((PadParam)k).def;
    }

    for (int s = 0; s < kScenes; ++s) {
        Scene& sc = p.scene_data[s];
        for (int t = 0; t < kPads; ++t) {
            sc.track[t].length = 0;
            for (int st = 0; st < kMaxSteps; ++st) sc.track[t].step[st] = Step{};
        }
        sc.length = 16;
        sc.div = Division::D1_16;
        sc.swing = 50;
        sc.bass = BassLine{};
        sc.synth = SynthLine{};
    }
    for (int i = 0; i < kSongSlots; ++i) {
        p.song.slot[i] = -1;
        p.song.fx[i] = 0;
    }
    p.song.follow = 0;

    for (int b = 0; b < kBuses; ++b) {
        memset(p.bus[b].name, 0, sizeof(p.bus[b].name));
        strncpy(p.bus[b].name, kBusNames[b], sizeof(p.bus[b].name) - 1);
        p.bus[b].level = 0.7071f;   // 0 dB
        p.bus[b].pan = 0.0f;
        p.bus[b].mute = p.bus[b].solo = 0;
    }
    for (int g = 0; g < kGlobalParamCount; ++g) p.g[g] = globalSpec((GlobalParam)g).def;
}

void projectDefaults(Project& p)
{
    baseDefaults(p);
    orbitDemo(p);
}

// NEW PROJECT: no demo, and none of its tempo or sound either (120 BPM, default knobs).
void projectEmpty(Project& p) { baseDefaults(p); }

// busGain() (every block per bus) and trackLength() (every step per track) are called by the audio
// task's IRAM code: ORBIT_HOT keeps them off the flash cache too.
ORBIT_HOT float busGain(float level)
{
    if (level <= 0.0f) return 0.0f;
    if (level > 1.0f) level = 1.0f;
    return level * level * 2.0f;   // 0.7071 -> 1.0 (0 dB), 1.0 -> 2.0 (+6 dB)
}

float dbToGain(float db) { return db <= -96.0f ? 0.0f : powf(10.0f, db * 0.05f); }

float gainToDb(float g) { return g <= 1.0e-5f ? -100.0f : 20.0f * log10f(g); }

ORBIT_HOT int trackLength(const Scene& s, int t)
{
    int len = s.track[t].length ? s.track[t].length : s.length;
    if (len < 1) len = 1;
    if (len > kMaxSteps) len = kMaxSteps;
    return len;
}

float divisionBeats(Division d)
{
    switch (d) {
    case Division::D1_8: return 0.5f;
    case Division::D1_16: return 0.25f;
    case Division::D1_32: return 0.125f;
    case Division::D1_8T: return 1.0f / 3.0f;
    case Division::D1_16T: return 1.0f / 6.0f;
    default: return 0.25f;
    }
}

const char* divisionName(Division d)
{
    switch (d) {
    case Division::D1_8: return "1/8";
    case Division::D1_16: return "1/16";
    case Division::D1_32: return "1/32";
    case Division::D1_8T: return "1/8T";
    case Division::D1_16T: return "1/16T";
    default: return "1/16";
    }
}

const char* trigCondName(TrigCond c)
{
    switch (c) {
    case TrigCond::Always: return "ALWAYS";
    case TrigCond::P1of2: return "1:2";
    case TrigCond::P2of2: return "2:2";
    case TrigCond::P1of4: return "1:4";
    case TrigCond::P2of4: return "2:4";
    case TrigCond::P3of4: return "3:4";
    case TrigCond::P4of4: return "4:4";
    case TrigCond::Fill: return "FILL";
    case TrigCond::NotFill: return "!FILL";
    case TrigCond::First: return "FIRST";
    default: return "?";
    }
}

}  // namespace orbit
