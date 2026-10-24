// Sample-accurate step sequencer (see seq.h).
//
// Time model
//  * Audible position = (cur, phase): `cur` counts steps since the timeline was (re)started and
//    `phase` is the 32.32 fixed-point frame distance from the grid start of step `cur` to the
//    start of the next block. A block of n frames adds n << 32, so with a constant tempo the
//    timeline does not depend on the block size. A tempo/division change rescales `phase`
//    (keeps the fractional position inside the step): nothing jumps, drops or doubles.
//  * Steps are expanded ahead of time (lookahead E = half a step for negative nudge + the flam
//    grace lead, at most one step) into pending instances. Hit times are kept in step units
//    (step index + q/1200 of a step), so pending hits follow tempo changes. Every hit has a bit
//    that is cleared when it is emitted: never twice; a hit whose time already passed (tempo
//    jump, start, output full) fires late at offset 0 instead of being dropped.
//  * The scheduler state of each expanded step is kept in a small ring. It feeds step()/scene()
//    when the audible position enters the step and quantize(), and it lets queueScene() roll the
//    scheduler back when the target boundary was already expanded (exact launch quantization).
//  * Musical counters use 24 ticks per quarter: every division is a whole number of ticks
//    (1/8 = 12, 1/16 = 6, 1/32 = 3, 1/8T = 8, 1/16T = 4), so beats, bars and eighths are exact.
#include "seq.h"

#include <new>
#include <string.h>

// Everything below runs per block on the audio task (or as a control call from it): on the Tab5
// it is placed in IRAM (ORBIT_HOT = IRAM_ATTR, see cmake/orbit_hal.cmake), off the flash cache.
#ifndef ORBIT_HOT
#define ORBIT_HOT
#endif

namespace orbit {

namespace {

constexpr int64_t kOne = int64_t(1) << 32;   // one frame in 32.32
constexpr int kQ = 1200;                     // sub-step unit: LCM of %-of-step and ratchet 1..4
constexpr int kTicksPerBeat = 24;
constexpr int kRing = 8;                     // expanded steps kept (power of two)
constexpr int kMaxLead = kRing - 2;          // max steps the scheduler runs ahead
constexpr int kPool = 96;                    // pending step instances (bound: ~4 per track)
constexpr int kClicks = 8;
constexpr int kDue = 128;                    // hits emitted per chunk (rest stays pending)
constexpr int kFlamMaxMs = 48;               // 16 + 4 * 8

constexpr uint8_t kFlScene = 1, kFlSlot = 2, kFlQueued = 4;          // Sched::flags
constexpr uint8_t kNoteScene = 1, kNoteSlot = 2, kNoteTransport = 4;  // State::notes

struct Sched {               // scheduler state of one step (also the rollback snapshot)
    uint32_t ticks;          // ticks since the scene started, at the step's grid start
    uint32_t ticks_play;     // ticks since play()
    uint32_t loop;           // completed passes of the scene
    uint32_t rng;            // generator state before the step was expanded
    uint8_t track_step[kPads];
    uint8_t track_loop[kPads];  // wraps at 256; only % 2 and % 4 are used
    uint8_t scene, scene_step, d, flags;
    int8_t slot;             // song slot, -1 = not following
    uint8_t _pad[3];
};

struct Inst {                // pending hits of one step of one track
    uint32_t step;           // absolute step index
    int16_t q0;              // first hit: swing + nudge, in 1/1200 step from the grid start
    uint16_t grace;          // flam lead in frames, 0 = none
    uint8_t pad, vel;
    int8_t pitch;
    uint8_t rat;             // ratchet 1..4
    uint8_t pend;            // bit 0 = grace, bit 1 + j = ratchet hit j
    uint8_t _pad[3];
};

struct Click {
    uint32_t step;
    int16_t q;
    uint8_t accent, _pad;
};

struct Due {
    uint16_t off;
    uint8_t inst, hit;
};

struct BassInst {            // a pending ACID BASS note (one per expanded step at most)
    uint32_t step;           // absolute step index
    int16_t q0;              // swing, in 1/1200 step from the grid start
    uint8_t note, accent, legato, slide;
};
constexpr int kBassPool = 8;   // >= kMaxLead

struct Audible {             // what the listener hears now (subset of Sched)
    uint8_t scene, step, d;
    int8_t slot;
    uint32_t loop, ticks_play;
};

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Frame index (relative to the block) at which an event at 32.32 time t happens: ceil(t).
inline int64_t frameOf(int64_t t) { return t > 0 ? (t + kOne - 1) >> 32 : -((-t) >> 32); }

inline uint32_t xorshift(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

ORBIT_HOT int ticksPerStep(Division d)
{
    switch (d) {
    case Division::D1_8: return 12;
    case Division::D1_16: return 6;
    case Division::D1_32: return 3;
    case Division::D1_8T: return 8;
    case Division::D1_16T: return 4;
    default: return 6;
    }
}

inline int beatsPerBar(const Project& p) { return clampi(p.beats_per_bar, 1, 16); }

inline int sceneLen(const Scene& s) { return clampi(s.length, 1, kMaxSteps); }

inline bool songValid(const Project& p, int i)
{
    return i >= 0 && i < kSongSlots && p.song.slot[i] >= 0 && p.song.slot[i] < kScenes;
}

// FOLLOW is on and there is a song to follow (play() starts it at slot 0).
inline bool following(const Project& p) { return p.song.follow && songValid(p, 0); }

// Does the boundary after a step starting at tick a (d ticks long) cross a launch point?
ORBIT_HOT bool crossing(uint32_t a, int d, LaunchQuant q, int bpb)
{
    uint32_t period;
    switch (q) {
    case LaunchQuant::Bar: period = uint32_t(kTicksPerBeat * bpb); break;
    case LaunchQuant::Beat: period = kTicksPerBeat; break;
    case LaunchQuant::Eighth: period = kTicksPerBeat / 2; break;
    default: return true;   // Off: next step
    }
    return (a + uint32_t(d)) / period != a / period;
}

ORBIT_HOT bool condOk(TrigCond c, uint8_t track_loop, uint32_t scene_loop, bool fill)
{
    switch (c) {
    case TrigCond::P1of2: return (track_loop & 1u) == 0;
    case TrigCond::P2of2: return (track_loop & 1u) == 1;
    case TrigCond::P1of4: return (track_loop & 3u) == 0;
    case TrigCond::P2of4: return (track_loop & 3u) == 1;
    case TrigCond::P3of4: return (track_loop & 3u) == 2;
    case TrigCond::P4of4: return (track_loop & 3u) == 3;
    case TrigCond::Fill: return fill;
    case TrigCond::NotFill: return !fill;
    case TrigCond::First: return scene_loop == 0;
    default: return true;
    }
}

// Time of hit h of an instance whose step starts at G (block-relative 32.32 frames).
inline int64_t hitTime(const Inst& in, int h, int64_t G, int64_t L)
{
    const int j = h ? h - 1 : 0;
    const int64_t q = int64_t(in.q0) + int64_t(j * (kQ / in.rat));
    int64_t t = G + q * L / kQ;
    if (h == 0) {
        int64_t g = int64_t(in.grace) << 32;
        if (g > L) g = L;    // keeps the lookahead bounded at extreme tempos
        t -= g;
    }
    return t;
}

inline uint8_t graceVel(uint8_t v)
{
    const int g = (int(v) * 60 + 50) / 100;
    return uint8_t(g < 1 ? 1 : g);
}

struct SeqData {
    const Project* proj;
    int rate;
    uint32_t rng;
    bool playing, fill, last_valid, fresh;
    int8_t queued;           // requested scene, -1 none (consumed by the scheduler)
    uint8_t notes;           // notices raised by control calls, delivered by process()
    uint8_t key_div;
    uint8_t _pad0;
    uint32_t key_bpm;        // tempo cache key (bpm * 1000)
    int key_rate;
    int64_t L;               // step length, 32.32 frames
    // audible position
    uint32_t cur;
    uint32_t last_cur;       // block start of the last processed block (quantize)
    int64_t phase;
    int64_t last_phase, last_L;
    Audible aud;
    // scheduler
    uint32_t sched;          // next step to expand
    uint32_t redo_until;     // steps below this are re-expansions after a rollback
    Sched sch;               // state of step sched-1 (or of sched when fresh)
    Sched ring[kRing];
    int pool_n, click_n;
    Inst pool[kPool];
    Click clicks[kClicks];
    Due due[kDue];
    int bass_n;
    BassInst bass[kBassPool];
};

using State = SeqData;

inline const Project& proj(const State& S) { return *S.proj; }

ORBIT_HOT void audibleFrom(State& S, const Sched& s)
{
    S.aud.scene = s.scene;
    S.aud.step = s.scene_step;
    S.aud.d = s.d;
    S.aud.slot = s.slot;
    S.aud.loop = s.loop;
    S.aud.ticks_play = s.ticks_play;
}

// Restarts the timeline at `step` of `scene` (pending hits are discarded).
ORBIT_HOT void startAt(State& S, int scene, int step, int slot, uint32_t loop, uint32_t ticks_play)
{
    S.cur = S.sched = 0;
    S.redo_until = 0;
    S.phase = 0;
    S.fresh = true;
    S.last_valid = false;
    S.pool_n = S.click_n = 0;
    S.bass_n = 0;
    scene = clampi(scene, 0, kScenes - 1);
    const Scene& sc = proj(S).scene_data[scene];
    const int len = sceneLen(sc);
    step = ((step % len) + len) % len;
    Sched& s = S.sch;
    memset(&s, 0, sizeof s);
    s.scene = uint8_t(scene);
    s.scene_step = uint8_t(step);
    s.d = uint8_t(ticksPerStep(sc.div));
    s.slot = int8_t(slot);
    s.loop = loop;
    s.ticks = uint32_t(step) * s.d;
    s.ticks_play = ticks_play;
    for (int t = 0; t < kPads; ++t) {
        const int tl = trackLength(sc, t);
        s.track_step[t] = uint8_t(step % tl);
        s.track_loop[t] = uint8_t(step / tl);
    }
    audibleFrom(S, s);
}

ORBIT_HOT void enterScene(Sched& s, int scene)
{
    s.scene = uint8_t(scene);
    s.scene_step = 0;
    s.loop = 0;
    s.ticks = 0;
    memset(s.track_step, 0, sizeof s.track_step);
    memset(s.track_loop, 0, sizeof s.track_loop);
}

// Moves the scheduler state one step forward and takes the boundary decisions
// (queued scene, song progression, FOLLOW toggles).
ORBIT_HOT void advance(State& S)
{
    const Project& p = proj(S);
    Sched& s = S.sch;
    const Scene& sc = p.scene_data[s.scene];
    const uint32_t a = s.ticks;
    const int d = s.d;
    s.ticks += uint32_t(d);
    s.ticks_play += uint32_t(d);
    s.flags = 0;
    for (int t = 0; t < kPads; ++t) {
        if (++s.track_step[t] >= trackLength(sc, t)) {
            s.track_step[t] = 0;
            ++s.track_loop[t];
        }
    }
    bool pass_end = false;
    if (++s.scene_step >= sceneLen(sc)) {
        s.scene_step = 0;
        ++s.loop;
        pass_end = true;
    }

    int next = -1, slot = s.slot;
    uint8_t why = 0;
    const bool song = following(p);
    const bool cross = crossing(a, d, p.quant, beatsPerBar(p));
    if (pass_end && song) {
        int i = s.slot >= 0 ? s.slot + 1 : 0;   // FOLLOW just switched on: start at slot 0
        if (!songValid(p, i)) i = 0;            // empty slot or end: back to slot 0
        slot = i;
        next = p.song.slot[i];
        S.queued = -1;
    } else if (S.queued >= 0 && (s.slot < 0 ? cross : (!song && (cross || pass_end)))) {
        // A queued scene launches at the quantization point. While a song slot still plays after
        // FOLLOW was switched off, that also ends the song (or its pass end does, if earlier).
        slot = -1;
        next = S.queued;
        S.queued = -1;
        why = kFlQueued;
    } else if (pass_end && s.slot >= 0) {
        slot = -1;                              // FOLLOW switched off / no song: loop the current scene
    }
    if (slot != s.slot) {
        s.slot = int8_t(slot);
        s.flags |= kFlSlot;
    }
    if (next >= 0 && next != s.scene) {
        enterScene(s, next);
        s.flags |= uint8_t(kFlScene | why);
    }
    s.d = uint8_t(ticksPerStep(p.scene_data[s.scene].div));
}

// Expands step S.sched whose grid starts at G (block-relative 32.32 frames).
ORBIT_HOT void expand(State& S, int64_t G, int64_t L)
{
    if (!S.fresh) advance(S);
    S.fresh = false;
    Sched& s = S.sch;
    s.rng = S.rng;
    const uint32_t k = S.sched;
    S.ring[k & (kRing - 1)] = s;
    S.sched = k + 1;
    // After a rollback, hits of re-expanded steps that are already in the past were (or would
    // have been) played by the replaced timeline: drop them instead of firing them late.
    const bool redo = int32_t(k - S.redo_until) < 0;

    const Project& p = proj(S);
    const Scene& sc = p.scene_data[s.scene];
    const int swing = clampi(sc.swing, 50, 75);
    const int qs = (s.scene_step & 1) ? (swing - 50) * (kQ / 50) : 0;   // MPC: delay the 2nd of each pair
    for (int t = 0; t < kPads; ++t) {
        const Step& st = sc.track[t].step[s.track_step[t]];
        if (!st.on) continue;
        if (!condOk(st.cond, s.track_loop[t], s.loop, S.fill)) continue;
        if (st.prob < 100 && int(xorshift(S.rng) % 100u) >= int(st.prob)) continue;
        if (S.pool_n >= kPool) continue;   // unreachable: the lookahead bounds the pool
        Inst& in = S.pool[S.pool_n];
        in.step = k;
        in.q0 = int16_t(qs + clampi(st.nudge, -50, 50) * (kQ / 100));
        in.rat = uint8_t(clampi(st.ratchet, 1, 4));
        in.grace = st.flam ? uint16_t((16 + 4 * clampi(st.flam, 1, 8)) * S.rate / 1000) : 0;
        in.pad = uint8_t(t);
        in.vel = uint8_t(clampi(st.vel, 1, 127));
        in.pitch = int8_t(clampi(st.pitch, -24, 24));
        in.pend = uint8_t((((1u << in.rat) - 1u) << 1) | (in.grace ? 1u : 0u));
        if (redo) {
            for (int h = 0; h <= in.rat; ++h)
                if ((in.pend & (1u << h)) && frameOf(hitTime(in, h, G, L)) < 0) in.pend &= uint8_t(~(1u << h));
        }
        if (in.pend) ++S.pool_n;
    }

    // ACID BASS: the scene's bass step, on the same grid and swing as the drums (not while muted). A note after a
    // SLIDE step slides into it (legato); the bass line follows the scene length.
    if (p.g[GP_BASS_MUTE] < 0.5f && S.bass_n < kBassPool) {
        const BassStep& b = sc.bass.step[s.scene_step];
        if (b.gate) {
            const int len = sceneLen(sc);
            const BassStep& prev = sc.bass.step[(s.scene_step + len - 1) % len];
            BassInst& bi = S.bass[S.bass_n];
            bi.step = k;
            bi.q0 = int16_t(qs);
            bi.note = uint8_t(clampi(kBassRootNote + clampi(b.note, kBassNoteMin, kBassNoteMax), 0, 127));
            bi.accent = b.accent ? 1 : 0;
            bi.slide = b.slide ? 1 : 0;
            bi.legato = (prev.gate && prev.slide) ? 1 : 0;
            if (!(redo && frameOf(G + int64_t(qs) * L / kQ) < 0)) ++S.bass_n;
        }
    }

    // Metronome: a quarter boundary inside [ticks, ticks + d) (usually at the step start).
    const uint32_t a = s.ticks;
    const uint32_t m = (a + kTicksPerBeat - 1) / kTicksPerBeat * kTicksPerBeat;
    if (m < a + s.d && S.click_n < kClicks) {
        Click& c = S.clicks[S.click_n];
        c.step = k;
        c.q = int16_t((m - a) * uint32_t(kQ) / s.d);
        c.accent = (m / kTicksPerBeat) % uint32_t(beatsPerBar(p)) == 0;
        if (!(redo && frameOf(G + int64_t(c.q) * L / kQ) < 0)) ++S.click_n;
    }
}

// Throws away pending hits of steps >= k and rewinds the scheduler to re-decide boundary k.
ORBIT_HOT void rollback(State& S, uint32_t k)
{
    if (int32_t(S.sched - S.redo_until) > 0) S.redo_until = S.sched;
    S.sch = S.ring[(k - 1) & (kRing - 1)];
    S.rng = S.ring[k & (kRing - 1)].rng;
    S.fresh = false;
    S.sched = k;
    int w = 0;
    for (int i = 0; i < S.pool_n; ++i)
        if (int32_t(S.pool[i].step - k) < 0) S.pool[w++] = S.pool[i];
    S.pool_n = w;
    w = 0;
    for (int i = 0; i < S.click_n; ++i)
        if (int32_t(S.clicks[i].step - k) < 0) S.clicks[w++] = S.clicks[i];
    S.click_n = w;
    w = 0;
    for (int i = 0; i < S.bass_n; ++i)
        if (int32_t(S.bass[i].step - k) < 0) S.bass[w++] = S.bass[i];
    S.bass_n = w;
}

ORBIT_HOT int64_t stepLength(State& S)
{
    const Project& p = proj(S);
    float bpm = p.bpm;
    if (!(bpm >= 30.0f)) bpm = 30.0f;   // also catches NaN
    if (bpm > 300.0f) bpm = 300.0f;
    const uint32_t milli = uint32_t(bpm * 1000.0f + 0.5f);
    const uint8_t div = uint8_t(p.scene_data[S.aud.scene].div);
    if (S.L > 0 && milli == S.key_bpm && div == S.key_div && S.rate == S.key_rate) return S.L;
    S.key_bpm = milli;
    S.key_div = div;
    S.key_rate = S.rate;
    // L = d/24 beat * 60/bpm s * rate = d * 2500 * rate / (bpm * 1000)   (exact integers)
    const uint64_t num = uint64_t(ticksPerStep(Division(div))) * 2500u * uint64_t(S.rate);
    return int64_t((num << 32) / milli);
}

// round(a * b / c) with a 128-bit intermediate (portable: RV32 has no __int128). Requires the
// quotient to fit in 64 bits. ~64 iterations: only used when the tempo changes.
ORBIT_HOT uint64_t mulDivRound(uint64_t a, uint64_t b, uint64_t c)
{
    const uint64_t m32 = 0xffffffffu;
    const uint64_t p00 = (a & m32) * (b & m32), p01 = (a & m32) * (b >> 32);
    const uint64_t p10 = (a >> 32) * (b & m32), p11 = (a >> 32) * (b >> 32);
    const uint64_t mid = (p00 >> 32) + (p01 & m32) + (p10 & m32);
    uint64_t lo = (p00 & m32) | (mid << 32);
    uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    const uint64_t half = c / 2;
    lo += half;
    if (lo < half) ++hi;
    uint64_t q = 0, r = hi;
    for (int i = 63; i >= 0; --i) {
        const bool carry = (r >> 63) != 0;
        r = (r << 1) | ((lo >> i) & 1u);
        q <<= 1;
        if (carry || r >= c) {
            r -= c;
            q |= 1u;
        }
    }
    return q;
}

// Keeps the fractional position inside the step when the step length changes.
ORBIT_HOT int64_t rescale(int64_t phase, int64_t from, int64_t to)
{
    if (from <= 0 || phase <= 0) return 0;
    if (phase > from + kOne) phase = from + kOne;
    return int64_t(mulDivRound(uint64_t(phase), uint64_t(to), uint64_t(from)));
}

ORBIT_HOT void insertDue(State& S, int& dn, uint16_t off, uint8_t inst, uint8_t hit)
{
    int i = dn;
    if (dn == kDue) {
        if (S.due[kDue - 1].off <= off) return;   // full: this one stays pending
        i = kDue - 1;                             // evict the latest (stays pending)
    } else {
        ++dn;
    }
    while (i > 0 && S.due[i - 1].off > off) {
        S.due[i] = S.due[i - 1];
        --i;
    }
    S.due[i] = Due{off, inst, hit};
}

// One chunk of at most kMaxBlock frames.
ORBIT_HOT int chunk(State& S, int n, SeqTrigger* out, int max, SeqNotice& no, uint32_t base, bool first)
{
    const Project& p = proj(S);
    const int64_t L = stepLength(S);
    if (L != S.L) {
        S.phase = rescale(S.phase, S.L, L);
        S.L = L;
    }
    if (first) {
        S.last_cur = S.cur;
        S.last_phase = S.phase;
        S.last_L = L;
        S.last_valid = true;
    }
    const int64_t end = int64_t(n) << 32;

    // 1. expand every step whose earliest possible hit may fall in this chunk
    int64_t flam = int64_t(kFlamMaxMs * S.rate / 1000) << 32;
    if (flam > L) flam = L;
    const int64_t E = L / 2 + flam;
    while (int32_t(S.sched - S.cur) < kMaxLead) {
        const int64_t G = int64_t(int32_t(S.sched - S.cur)) * L - S.phase;
        if (G - E >= end) break;
        expand(S, G, L);
    }

    // 2. collect due hits (sorted by offset, stable in creation order)
    bool solo = false;
    for (int t = 0; t < kPads; ++t) solo = solo || p.pad[t].solo;
    int dn = 0;
    for (int i = 0; i < S.pool_n; ++i) {
        Inst& in = S.pool[i];
        const int64_t G = int64_t(int32_t(in.step - S.cur)) * L - S.phase;
        for (int h = 0; h <= in.rat; ++h) {
            if (!(in.pend & (1u << h))) continue;
            const int64_t f = frameOf(hitTime(in, h, G, L));
            if (f >= n) break;   // hits of an instance are in time order
            const Pad& pd = p.pad[in.pad];
            if (pd.mute || (solo && !pd.solo)) {
                in.pend &= uint8_t(~(1u << h));   // silenced: consumed
                continue;
            }
            insertDue(S, dn, uint16_t(f < 0 ? 0 : f), uint8_t(i), uint8_t(h));
        }
    }
    const int count = dn < max ? dn : (max > 0 ? max : 0);
    for (int i = 0; i < count; ++i) {
        Inst& in = S.pool[S.due[i].inst];
        const int h = S.due[i].hit;
        SeqTrigger& o = out[i];
        o.pad = in.pad;
        o.vel = h == 0 ? graceVel(in.vel) : in.vel;
        o.pitch = in.pitch;
        o._pad = 0;
        o.offset = base + S.due[i].off;
        in.pend &= uint8_t(~(1u << h));
    }
    int w = 0;
    for (int i = 0; i < S.pool_n; ++i)
        if (S.pool[i].pend) S.pool[w++] = S.pool[i];
    S.pool_n = w;

    // 3. ACID BASS notes (in step order, so offsets come sorted)
    w = 0;
    for (int i = 0; i < S.bass_n; ++i) {
        const BassInst& bi = S.bass[i];
        const int64_t G = int64_t(int32_t(bi.step - S.cur)) * L - S.phase;
        const int64_t f = frameOf(G + int64_t(bi.q0) * L / kQ);
        if (f >= n) {
            S.bass[w++] = bi;
            continue;
        }
        if (no.bass_count >= SeqNotice::kBassMax) continue;   // unreachable: one note per step
        AcidEvent& e = no.bass[no.bass_count++];
        e.type = AcidEvtType::NoteOn;
        e.note = bi.note;
        e.accent = bi.accent;
        e.legato = bi.legato;
        e.offset = base + uint32_t(f < 0 ? 0 : f);
        const uint32_t step_frames = uint32_t(L >> 32);
        e.gate_frames = bi.slide ? step_frames + step_frames / 2 : step_frames / 2;
    }
    S.bass_n = w;

    // 4. metronome
    w = 0;
    for (int i = 0; i < S.click_n; ++i) {
        const Click& c = S.clicks[i];
        const int64_t G = int64_t(int32_t(c.step - S.cur)) * L - S.phase;
        const int64_t f = frameOf(G + int64_t(c.q) * L / kQ);
        if (f < n) {
            if (!no.metronome_click) {
                no.metronome_click = true;
                no.metronome_accent = c.accent != 0;
                no.click_offset = base + uint32_t(f < 0 ? 0 : f);
            }
        } else {
            S.clicks[w++] = c;
        }
    }
    S.click_n = w;

    // 5. move the audible position across the step boundaries inside this chunk
    uint32_t crossed = 0;
    int64_t Lc = L;
    int64_t Gc = -S.phase;   // grid start of the audible step (block-relative)
    for (;;) {
        const int64_t G = Gc + Lc;
        if (frameOf(G) >= n) break;
        ++crossed;
        Gc = G;
        const uint32_t k = S.cur + crossed;
        while (int32_t(k - S.sched) >= 0)   // defensive: the lookahead was capped
            expand(S, Gc + int64_t(int32_t(S.sched - k)) * Lc, Lc);
        const Sched& e = S.ring[k & (kRing - 1)];
        audibleFrom(S, e);
        if (e.flags & kFlSlot) no.song_slot_changed = true;
        if (e.flags & kFlScene) {
            no.scene_changed = true;
            Lc = stepLength(S);   // new scene, maybe another division: exact from its first step
        }
    }
    S.cur += crossed;
    S.phase = end - Gc;
    S.L = Lc;
    return count;
}

}  // namespace

struct Sequencer::State : SeqData {};

ORBIT_HOT Sequencer::State& Sequencer::st()
{
    static_assert(sizeof(State) <= sizeof(state_), "enlarge Sequencer::state_ in seq.h");
    static_assert(alignof(State) <= 8, "state_ is 8-byte aligned");
    return *reinterpret_cast<State*>(state_);
}
ORBIT_HOT const Sequencer::State& Sequencer::st() const { return *reinterpret_cast<const State*>(state_); }

void Sequencer::init(int sample_rate, const Project* project)
{
    State* S = new (state_) State();
    S->proj = project;
    S->rate = clampi(sample_rate, 8000, 96000);
    S->rng = 0x6D2B79F5u;
    S->queued = -1;
    S->aud.slot = -1;
    if (project) startAt(*S, project->scene, 0, -1, 0, 0);
}

void Sequencer::setSampleRate(int sample_rate) { st().rate = clampi(sample_rate, 8000, 96000); }

ORBIT_HOT void Sequencer::play()
{
    State& S = st();
    if (!S.proj) return;
    const Project& p = *S.proj;
    const int prev_scene = S.aud.scene, prev_slot = S.aud.slot;
    int scene = S.aud.scene, slot = -1;
    if (p.song.follow && songValid(p, 0)) {
        slot = 0;
        scene = p.song.slot[0];
    }
    S.queued = -1;
    startAt(S, scene, 0, slot, 0, 0);
    S.playing = true;
    S.notes |= kNoteTransport;
    if (scene != prev_scene) S.notes |= kNoteScene;
    if (slot != prev_slot || slot >= 0) S.notes |= kNoteSlot;
}

ORBIT_HOT void Sequencer::stop()
{
    State& S = st();
    S.notes |= kNoteTransport;
    if (S.playing) {
        S.playing = false;   // position, pending hits and lookahead stay for resume()
        return;
    }
    if (!S.proj) return;
    const Project& p = *S.proj;
    const int prev_scene = S.aud.scene, prev_slot = S.aud.slot;
    int scene = S.aud.scene, slot = -1;
    if (p.song.follow && songValid(p, 0)) {
        slot = 0;
        scene = p.song.slot[0];
    }
    S.queued = -1;
    startAt(S, scene, 0, slot, 0, 0);
    if (scene != prev_scene) S.notes |= kNoteScene;
    if (slot != prev_slot) S.notes |= kNoteSlot;
}

ORBIT_HOT void Sequencer::resume()
{
    State& S = st();
    if (!S.proj || S.playing) return;
    S.playing = true;
    S.notes |= kNoteTransport;
}

ORBIT_HOT void Sequencer::locate(int step)
{
    State& S = st();
    if (!S.proj) return;
    startAt(S, S.aud.scene, step, S.aud.slot, S.aud.loop, S.aud.ticks_play);
    S.notes |= kNoteTransport;
}

ORBIT_HOT void Sequencer::queueScene(int scene)
{
    State& S = st();
    if (!S.proj || scene < 0 || scene >= kScenes) return;
    const Project& p = *S.proj;
    // Only the FOLLOW flag decides: a song slot that is still playing (or was rewound to) after
    // FOLLOW went off must not swallow the selection.
    if (following(p)) return;
    if (!S.playing) {
        const bool was_song = S.aud.slot >= 0;
        if (scene != S.aud.scene || was_song) {
            if (scene != S.aud.scene) S.notes |= kNoteScene;
            if (was_song) S.notes |= kNoteSlot;
            startAt(S, scene, 0, -1, 0, 0);
        }
        S.queued = -1;
        return;
    }
    if (scene == S.aud.scene) {   // already playing it: cancel, also an expanded switch / song step
        S.queued = -1;
        for (uint32_t k = S.cur + 1; int32_t(k - S.sched) < 0; ++k) {
            if (S.ring[k & (kRing - 1)].flags & (kFlQueued | kFlSlot)) {
                rollback(S, k);
                break;
            }
        }
        return;
    }
    S.queued = int8_t(scene);
    // The target boundary may already be expanded (lookahead): re-decide it. A song pass end
    // (kFlSlot) is a launch point too while a song slot still plays.
    for (uint32_t k = S.cur + 1; int32_t(k - S.sched) < 0; ++k) {
        const Sched& prev = S.ring[(k - 1) & (kRing - 1)];
        if ((S.ring[k & (kRing - 1)].flags & (kFlQueued | kFlSlot)) ||
            crossing(prev.ticks, prev.d, p.quant, beatsPerBar(p))) {
            rollback(S, k);
            break;
        }
    }
}

ORBIT_HOT void Sequencer::setFill(bool on) { st().fill = on; }

ORBIT_HOT int Sequencer::process(int n, SeqTrigger* out, int max, SeqNotice& notice)
{
    State& S = st();
    notice = SeqNotice{};
    if (S.notes & kNoteScene) notice.scene_changed = true;
    if (S.notes & kNoteSlot) notice.song_slot_changed = true;
    if (S.notes & kNoteTransport) notice.transport_changed = true;
    S.notes = 0;
    if (!S.proj || !S.playing || n <= 0) return 0;
    if (!out) max = 0;
    int count = 0;
    for (int done = 0; done < n;) {
        const int c = n - done < kMaxBlock ? n - done : kMaxBlock;
        count += chunk(S, c, out + count, max - count, notice, uint32_t(done), done == 0);
        done += c;
    }
    return count;
}

ORBIT_HOT bool Sequencer::playing() const { return st().playing; }

ORBIT_HOT int Sequencer::step() const
{
    const State& S = st();
    return S.playing ? S.aud.step : -1;
}

ORBIT_HOT int Sequencer::scene() const { return st().aud.scene; }

ORBIT_HOT int Sequencer::queuedScene() const
{
    const State& S = st();
    if (S.queued >= 0) return S.queued;
    for (uint32_t k = S.cur + 1; int32_t(k - S.sched) < 0; ++k) {
        const Sched& e = S.ring[k & (kRing - 1)];
        if (e.flags & kFlQueued) return e.scene;
    }
    return -1;
}

ORBIT_HOT int Sequencer::songSlot() const { return st().aud.slot; }

ORBIT_HOT uint32_t Sequencer::loopCount() const { return st().aud.loop; }

double Sequencer::beats() const
{
    const State& S = st();
    double b = double(S.aud.ticks_play);
    if (S.L > 0) b += double(S.phase) / double(S.L) * double(S.aud.d);
    return b / double(kTicksPerBeat);
}

// beats() for the audio task: only fcvt.s.w / fdiv.s, no soft-float double or int64->float.
// L <= 12 ticks at 30 BPM and 96 kHz = 96000 frames (32.32), so L >> 20 < 4e8 and phase >> 20
// (0..~L) both fit in int32.
ORBIT_HOT float Sequencer::beatsF() const
{
    const State& S = st();
    float b = (float)S.aud.ticks_play;
    const int32_t l20 = int32_t(S.L >> 20);
    if (l20 > 0) {
        int64_t ph = S.phase;
        if (ph < 0) ph = 0;
        b += (float)int32_t(ph >> 20) / (float)l20 * (float)S.aud.d;
    }
    return b * (1.0f / float(kTicksPerBeat));
}

ORBIT_HOT bool Sequencer::quantize(uint32_t offset, int pad, int& scene, int& step, int& nudge) const
{
    const State& S = st();
    if (!S.playing || !S.last_valid || S.last_L <= 0) return false;
    const int64_t L = S.last_L;
    const int64_t pos = S.last_phase + (int64_t(offset) << 32);
    int64_t j = pos / L;
    int64_t r = pos - j * L;
    if (2 * r >= L) {   // closer to the next step
        ++j;
        r -= L;
    }
    const uint32_t k = S.last_cur + uint32_t(j);
    if (int32_t(k - S.sched) >= 0 || int32_t(S.sched - k) > kRing) return false;
    const Sched& e = S.ring[k & (kRing - 1)];
    scene = e.scene;
    step = (pad >= 0 && pad < kPads) ? e.track_step[pad] : e.scene_step;
    const int64_t num = r * 200 + (r >= 0 ? L : -L);   // round(r * 100 / L)
    nudge = clampi(int(num / (2 * L)), -50, 50);
    return true;
}

void Sequencer::seed(uint32_t s) { st().rng = s ? s : 0x6D2B79F5u; }

}  // namespace orbit
