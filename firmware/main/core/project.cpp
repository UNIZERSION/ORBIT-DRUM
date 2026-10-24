#include "project.h"

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
const char* const kBusNames[kBuses] = {"KICK", "SNARE", "HATS", "PERC", "TOMS", "CYMB", "FX", "BASS"};

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

void demoScenes(Project& p)
{
    // A: house (909) — four on the floor, clap on 2 and 4, offbeat open hat, 16th closed hats
    Scene& a = p.scene_data[0];
    set(a, 0, {0, 4, 8, 12}, 120);
    set(a, 4, {4, 12}, 110);
    set(a, 3, {2, 6, 10, 14}, 95);
    set(a, 2, {0, 1, 3, 4, 5, 7, 8, 9, 11, 12, 13, 15}, 70);
    set(a, 2, {0, 4, 8, 12}, 96);
    set(a, 5, {7, 15}, 80);
    a.swing = 54;
    // an acid line on top of it (C minor): accents and slides
    bass(a, {{0, 0, 1, 0}, {2, 12, 0, 0}, {3, 0, 0, 1}, {4, 3, 0, 0}, {6, 0, 1, 0}, {7, 10, 0, 1},
             {8, 12, 0, 0}, {10, 0, 0, 0}, {11, 7, 1, 1}, {12, 5, 0, 0}, {14, 3, 1, 0}, {15, 15, 0, 0}});

    // B: techno — rumbling kick, rim stabs, ride, ratcheted hat at the end
    Scene& b = p.scene_data[1];
    set(b, 0, {0, 4, 8, 12}, 127);
    set(b, 5, {3, 11, 14}, 90);
    set(b, 3, {2, 6, 10, 14}, 100);
    set(b, 10, {0, 2, 4, 6, 8, 10, 12, 14}, 70);
    set(b, 2, {15}, 90);
    b.track[2].step[15].ratchet = 3;
    set(b, 4, {12}, 100);
    b.track[4].step[12].prob = 60;
    bass(b, {{0, -12, 1, 0}, {1, 0, 0, 0}, {3, -12, 0, 1}, {4, -10, 0, 0}, {6, 0, 1, 0}, {8, -12, 0, 0},
             {9, 2, 0, 1}, {10, 3, 1, 0}, {12, -12, 0, 0}, {13, 0, 0, 0}, {14, -5, 1, 1}, {15, -2, 0, 0}});

    // C: hip-hop — syncopated kick, snare on 2 and 4, swung hats, ghost snare
    Scene& c = p.scene_data[2];
    set(c, 0, {0, 7, 10}, 120);
    set(c, 1, {4, 12}, 115);
    set(c, 1, {15}, 55);
    set(c, 2, {0, 2, 4, 6, 8, 10, 12, 14}, 85);
    set(c, 3, {6}, 80);
    c.swing = 62;

    // D: breakbeat fill — ghost notes, snare roll with ratchets, crash at the start
    Scene& d = p.scene_data[3];
    set(d, 0, {0, 10}, 125);
    set(d, 1, {4, 12}, 118);
    set(d, 1, {7, 9}, 50);
    set(d, 1, {13, 14, 15}, 100);
    d.track[1].step[14].ratchet = 2;
    d.track[1].step[15].ratchet = 4;
    set(d, 2, {0, 2, 4, 6, 8, 10}, 80);
    set(d, 9, {0}, 110);
    set(d, 6, {11}, 100);
    set(d, 7, {12}, 100);
}

}  // namespace

// Zeroes the bytes [from, to) of an object (padding included, so zeroed structs compare equal).
static inline void zeroRange(void* from, const void* to)
{
    char* a = static_cast<char*>(from);
    const char* b = static_cast<const char*>(to);
    if (b > a) memset(a, 0, (size_t)(b - a));
}

void projectDefaults(Project& p)
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
    }
    demoScenes(p);

    // Demo song: A A B B C C D D, then end. FOLLOW stays off until the user turns it on.
    const int8_t chain[8] = {0, 0, 1, 1, 2, 2, 3, 3};
    for (int i = 0; i < kSongSlots; ++i) {
        p.song.slot[i] = i < 8 ? chain[i] : -1;
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

void projectEmpty(Project& p)
{
    projectDefaults(p);
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
    }
    for (int i = 0; i < kSongSlots; ++i) {
        p.song.slot[i] = -1;
        p.song.fx[i] = 0;
    }
    p.song.follow = 0;
}

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
