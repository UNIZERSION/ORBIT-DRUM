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

// The pads of the built-in kits (kPadNames order)
enum : int { kKick = 0, kClHat = 2, kOpHat = 3, kClap = 4, kRim = 5, kCrash = 9, kRide = 10, kKickLong = 11, kClapRoom = 14 };

// Every quarter note of both bars of a 2-bar scene: the step inside the beat (0..3) plus 4 * beat.
void beats(Scene& s, int pad, int offset, int vel)
{
    for (int b = 0; b < 8; ++b) set(s, pad, {b * 4 + offset}, vel);
}

// 909 groove of the demo (both bars): four on the floor, clap on 2 and 4 with a quieter room clap under it
// (the slapback), 16th closed hats with the offbeat loudest.
void funkDrums(Scene& s, bool kick)
{
    if (kick) beats(s, kKick, 0, 118);
    for (int st : {4, 12, 20, 28}) {
        set(s, kClap, {st}, 108);
        set(s, kClapRoom, {st}, 50);
    }
    beats(s, kClHat, 0, 72);
    beats(s, kClHat, 1, 44);
    beats(s, kClHat, 2, 96);
    beats(s, kClHat, 3, 52);
}

// The acid line (G minor, semitones from C2): an original 2-bar riff in the manner of late-90s French house -
// the first 16th left out, octave jumps, slides and accents on a few scale tones.
void funkRiff(Scene& s)
{
    bass(s, {{1, -5, 1, 0},  {2, 7, 0, 1},   {3, 5, 0, 0},   {5, -5, 0, 0},  {6, -2, 0, 1},  {7, 0, 1, 0},
             {9, 7, 1, 1},   {10, 5, 0, 1},  {11, 2, 0, 0},  {12, -5, 0, 0}, {14, -2, 1, 0}, {15, 0, 0, 1},
             {16, 2, 0, 0},  {17, -5, 1, 0}, {18, 7, 0, 0},  {20, 5, 0, 1},  {21, 7, 1, 0},  {23, 10, 0, 1},
             {24, 12, 1, 0}, {25, 10, 0, 0}, {26, 7, 0, 1},  {27, 5, 0, 0},  {28, 2, 1, 0},  {30, 0, 0, 1},
             {31, -2, 0, 0}});
}

// The break's line: bar 1 of the riff twice, the second time climbing to the high G.
void funkBreakRiff(Scene& s)
{
    bass(s, {{1, -5, 1, 0},  {2, 7, 0, 1},   {3, 5, 0, 0},   {5, -5, 0, 0},  {6, -2, 0, 1},  {7, 0, 1, 0},
             {9, 7, 1, 1},   {10, 5, 0, 1},  {11, 2, 0, 0},  {12, -5, 0, 0}, {14, -2, 1, 0}, {15, 0, 0, 1},
             {17, -5, 1, 0}, {18, 7, 0, 1},  {19, 5, 0, 0},  {21, -5, 0, 0}, {22, -2, 0, 1}, {23, 0, 1, 0},
             {25, 7, 1, 1},  {26, 10, 0, 1}, {27, 12, 0, 0}, {28, 14, 1, 1}, {29, 17, 0, 1}, {30, 19, 1, 0}});
}

// The only demo (0.4.6): one track in the manner of the TB-303 + TR-909 sound of Daft Punk's "Da Funk" (111 BPM,
// G minor, a distorted acid line over a plain 909 groove). Notes and patterns are our own. The four scenes are
// its parts and the song plays them in order: A intro (drums), B groove + 303, C break (no kick), D full.
void funkDemo(Project& p)
{
    p.bpm = 111.0f;
    for (int i = 0; i < kScenes; ++i) {
        p.scene_data[i].length = 32;
        p.scene_data[i].swing = 56;
    }
    Scene& a = p.scene_data[0];   // A: intro, drums only, a rim and a pickup kick in bar 2
    funkDrums(a, true);
    set(a, kRim, {7, 23}, 60);
    set(a, kKick, {30}, 84);

    Scene& b = p.scene_data[1];   // B: groove with the acid line
    funkDrums(b, true);
    set(b, kRim, {7, 23}, 60);
    funkRiff(b);

    Scene& c = p.scene_data[2];   // C: break - no kick, a long kick at the start, rim syncopation, the riff climbs
    funkDrums(c, false);
    set(c, kKickLong, {0}, 104);
    set(c, kRim, {3, 6, 11, 19, 22, 27}, 66);
    funkBreakRiff(c);

    Scene& d = p.scene_data[3];   // D: full - crash, ride, open hats on the offbeats (they choke the closed ones)
    funkDrums(d, true);
    for (int st = 2; st < 32; st += 4) d.track[kClHat].step[st] = Step{};
    beats(d, kOpHat, 2, 84);
    beats(d, kRide, 0, 58);
    set(d, kCrash, {0}, 104);
    funkRiff(d);

    // Song: A A B B C C D D B B, then end. FOLLOW stays off until the user turns it on.
    const int8_t chain[10] = {0, 0, 1, 1, 2, 2, 3, 3, 1, 1};
    for (int i = 0; i < kSongSlots; ++i) p.song.slot[i] = i < 10 ? chain[i] : -1;

    // The 303: low cutoff, high resonance, strong envelope and accent, heavy drive; a little delay
    p.g[GP_BASS_CUTOFF] = 0.30f;
    p.g[GP_BASS_RESO] = 0.80f;
    p.g[GP_BASS_ENVMOD] = 0.62f;
    p.g[GP_BASS_DECAY] = 0.50f;
    p.g[GP_BASS_ACCENT] = 0.78f;
    p.g[GP_BASS_DRIVE] = 0.80f;
    p.g[GP_BASS_LEVEL] = -2.0f;
    p.g[GP_BASS_SEND] = 0.18f;
    p.g[GP_DLY_FEEDBACK] = 0.30f;
    p.g[GP_DLY_LEVEL] = 0.40f;
    // the room clap a little right, under the dry one
    p.pad[kClapRoom].p[PP_PAN] = 0.35f;
    p.pad[kClapRoom].p[PP_SEND] = 0.30f;
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
    funkDemo(p);
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
