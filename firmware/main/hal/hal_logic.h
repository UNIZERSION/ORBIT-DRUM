#pragma once
// Portable decision logic of the Tab5 HAL (no ESP-IDF includes), shared by hal/*.cpp and the PC test
// host/tests/test_hal_logic.cpp:
//  - FrameBook: bookkeeping of the tear-free presentation (display.cpp): three DPI frame buffers, the
//    areas each one missed ("stale"), which buffer the next refresh may draw into, the repairs that bring
//    it up to date, and the idle-time repairs that take that copy off the next frame's critical path;
//  - outputFor: speaker amplifier + mono mix from the output mode and HP_DET (audio.cpp);
//  - Debounce, OverloadGuard: HP_DET filtering and the audio task's overload yield (audio.cpp).
#include <stdint.h>
#include <string.h>
#include "core/settings.h"

namespace hal::logic {

// ---------------------------------------------------------------- rectangles (inclusive, like lv_area_t)
struct Rect {
    int32_t x1, y1, x2, y2;
};
inline int32_t rectW(const Rect& r) { return r.x2 - r.x1 + 1; }
inline int32_t rectH(const Rect& r) { return r.y2 - r.y1 + 1; }
inline uint32_t rectPx(const Rect& r) { return (uint32_t)rectW(r) * (uint32_t)rectH(r); }

// Logical landscape area -> native portrait area (LV_DISPLAY_ROTATION_90): logical pixel (lx, ly) lies
// at native (ly, native_h - 1 - lx).
inline Rect toNative(const Rect& a, int32_t native_h) { return Rect{a.y1, native_h - 1 - a.x2, a.y2, native_h - 1 - a.x1}; }

// Adds r to the list, merged into the first rectangle it overlaps or touches. A full list collapses into
// `full` (the whole frame).
inline void addDamage(Rect* list, unsigned* count, unsigned max, const Rect& r, const Rect& full)
{
    for (unsigned i = 0; i < *count; i++) {
        Rect& o = list[i];
        const bool adjacent = r.x1 <= o.x2 + 1 && r.x2 >= o.x1 - 1 && r.y1 <= o.y2 + 1 && r.y2 >= o.y1 - 1;
        if (adjacent) {
            if (r.x1 < o.x1) o.x1 = r.x1;
            if (r.y1 < o.y1) o.y1 = r.y1;
            if (r.x2 > o.x2) o.x2 = r.x2;
            if (r.y2 > o.y2) o.y2 = r.y2;
            return;
        }
    }
    if (*count == max) {
        list[0] = full;
        *count = 1;
    } else {
        list[(*count)++] = r;
    }
}

// r minus every rectangle of cov -> out[0..n) (disjoint pieces). tmp is scratch space of `max` rectangles
// (both arrays belong to the caller: nothing big on the stack). -1 when the rest needs more than max pieces.
inline int subtract(const Rect& r, const Rect* cov, int ncov, Rect* out, Rect* tmp, int max)
{
    if (max < 1) return -1;
    Rect* a = out;
    Rect* b = tmp;
    int n = 1;
    a[0] = r;
    for (int c = 0; c < ncov && n; c++) {
        const Rect& k = cov[c];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const Rect& w = a[i];
            if (k.x2 < w.x1 || k.x1 > w.x2 || k.y2 < w.y1 || k.y1 > w.y2) {
                if (m >= max) return -1;
                b[m++] = w;
                continue;
            }
            const int32_t top = k.y1 > w.y1 ? k.y1 : w.y1, bottom = k.y2 < w.y2 ? k.y2 : w.y2;
            const int32_t left = k.x1 > w.x1 ? k.x1 : w.x1, right = k.x2 < w.x2 ? k.x2 : w.x2;
            if (m + 4 > max) return -1;
            if (w.y1 < top) b[m++] = Rect{w.x1, w.y1, w.x2, top - 1};
            if (bottom < w.y2) b[m++] = Rect{w.x1, bottom + 1, w.x2, w.y2};
            if (w.x1 < left) b[m++] = Rect{w.x1, top, left - 1, bottom};
            if (right < w.x2) b[m++] = Rect{right + 1, top, w.x2, bottom};
        }
        Rect* t = a;
        a = b;
        b = t;
        n = m;
    }
    if (a != out) memcpy(out, a, (size_t)n * sizeof(Rect));
    return n;
}

// ---------------------------------------------------------------- presentation bookkeeping
// Three frame buffers. `latest` is the newest published frame (scanned out now or from the next frame
// start); the previous one stays "retired" (held, never written) until two VSYNC edges after the
// switch, because it may have started scanning right before it. Every published frame's damage is added
// to the stale lists of the two other buffers; before a buffer becomes the back buffer, its stale areas
// are copied from `latest`, minus what the coming refresh redraws anyway (`cover`).
// Copy callback: copy(int dst_buffer, const Rect& area) copies `area` from frame `latest` into dst.
// One owner (the presentation task); no locking.
template <int kMaxDamage = 64, int kMaxRest = 24>
struct FrameBook {
    Rect full{};                     // the whole native frame
    unsigned latest = 0;
    unsigned back = 1;               // IDF starts scan-out with buffer 0
    bool pending = false;            // published: the next refresh needs a fresh back buffer
    bool retired[3] = {};
    uint32_t retired_until[3] = {};  // VSYNC count from which the retired buffer may be written
    Rect stale[3][kMaxDamage];
    unsigned stale_count[3] = {};
    Rect damage[kMaxDamage];         // what the current back buffer received since it was chosen
    unsigned damage_count = 0;
    Rect rest[kMaxRest];             // scratch of prepare()
    Rect tmp[kMaxRest];

    void reset(int32_t native_w, int32_t native_h)
    {
        full = Rect{0, 0, native_w - 1, native_h - 1};
        latest = 0;
        back = 1;
        pending = false;
        for (int i = 0; i < 3; i++) {
            retired[i] = false;
            retired_until[i] = 0;
            stale_count[i] = 0;
        }
        damage_count = 0;
    }

    uint64_t stalePx(unsigned i) const
    {
        uint64_t px = 0;
        for (unsigned k = 0; k < stale_count[i]; k++) px += rectPx(stale[i][k]);
        return px;
    }

    // may buffer i be written now (not the published one, not held)?
    bool writable(unsigned i, uint32_t vsync_now) const
    {
        if (i == latest) return false;
        return !retired[i] || (int32_t)(vsync_now - retired_until[i]) >= 0;
    }

    // back buffer for the next refresh: a writable one with the fewest stale pixels (the least copying);
    // -1 while both are held
    int candidate(uint32_t vsync_now) const
    {
        int best = -1;
        uint64_t best_px = 0;
        for (unsigned i = 0; i < 3; i++) {
            if (!writable(i, vsync_now)) continue;
            const uint64_t px = stalePx(i);
            if (best < 0 || px < best_px) {
                best = (int)i;
                best_px = px;
            }
        }
        return best;
    }

    // no VSYNC for too long: the buffer retired first (may tear once, never hangs)
    int oldestRetired() const
    {
        int c = -1;
        for (unsigned i = 0; i < 3; i++) {
            if (i == latest) continue;
            if (c < 0 || (int32_t)(retired_until[i] - retired_until[c]) < 0) c = (int)i;
        }
        return c;
    }

    // Make buffer `cand` the back buffer: copy what it missed except the cover areas (redrawn by this
    // refresh, each rendered in full). Returns the pixels copied.
    template <class Copy>
    uint64_t prepare(unsigned cand, const Rect* cover, int ncover, Copy&& copy)
    {
        uint64_t px = 0;
        for (unsigned i = 0; i < stale_count[cand]; i++) {
            const int n = subtract(stale[cand][i], cover, ncover, rest, tmp, kMaxRest);
            if (n < 0) {   // too fragmented: repair it whole (nothing of this refresh is written yet)
                copy((int)cand, stale[cand][i]);
                px += rectPx(stale[cand][i]);
                continue;
            }
            for (int k = 0; k < n; k++) {
                copy((int)cand, rest[k]);
                px += rectPx(rest[k]);
            }
        }
        stale_count[cand] = 0;
        retired[cand] = false;
        back = cand;
        damage_count = 0;
        pending = false;
        return px;
    }

    // a native area of the back buffer was written
    void damaged(const Rect& native) { addDamage(damage, &damage_count, (unsigned)kMaxDamage, native, full); }

    // the back buffer was published at VSYNC count `vsync_now`
    void published(uint32_t vsync_now)
    {
        retired[latest] = true;
        retired_until[latest] = vsync_now + 2;
        latest = back;
        for (unsigned i = 0; i < 3; i++) {
            if (i == latest) continue;
            for (unsigned j = 0; j < damage_count; j++) addDamage(stale[i], &stale_count[i], (unsigned)kMaxDamage, damage[j], full);
        }
        pending = true;
    }

    // Between refreshes (published, the next refresh not started): a writable buffer still has stale
    // areas, so idleStep() has work.
    int idleTarget(uint32_t vsync_now) const
    {
        if (!pending) return -1;
        int best = -1;
        uint64_t best_px = 0;
        for (unsigned i = 0; i < 3; i++) {
            if (!stale_count[i] || !writable(i, vsync_now)) continue;
            const uint64_t px = stalePx(i);
            if (best < 0 || px < best_px) {   // finish the cheaper one first: it becomes a clean candidate
                best = (int)i;
                best_px = px;
            }
        }
        return best;
    }

    // Copies up to about `budget_px` stale pixels of idleTarget() from `latest` (whole rectangles, or the top
    // rows of a bigger one). Returns the pixels copied (0: nothing to do).
    template <class Copy>
    uint32_t idleStep(uint32_t vsync_now, uint32_t budget_px, Copy&& copy)
    {
        const int t = idleTarget(vsync_now);
        if (t < 0) return 0;
        uint32_t done = 0;
        while (stale_count[t] && done < budget_px) {
            Rect& r = stale[t][stale_count[t] - 1];
            const uint32_t left = budget_px - done;
            if (rectPx(r) <= left) {
                const Rect whole = r;
                --stale_count[t];
                copy(t, whole);
                done += rectPx(whole);
            } else {
                int32_t rows = (int32_t)(left / (uint32_t)rectW(r));
                if (rows < 1) rows = 1;
                const Rect top = Rect{r.x1, r.y1, r.x2, r.y1 + rows - 1};
                r.y1 += rows;
                copy(t, top);
                done += rectPx(top);
                break;   // what is left of the budget is less than one row of this rectangle
            }
        }
        return done;
    }
};

// ---------------------------------------------------------------- audio output policy
struct OutputState {
    bool speaker;   // SPK_EN (NS4150B amplifier, fed from LOUT1 only)
    bool mono;      // L = R = (L + R) / 2 after render
};

// hp: debounced HP_DET says headphones. hp_trusted: the HP_DET level and polarity were confirmed on the
// hardware. Until then Auto never switches the speaker off (a misread pin would leave a stock unit
// silent) and stays mono (a misread pin would drop the right channel from the speaker).
inline OutputState outputFor(orbit::AudioOut mode, bool hp, bool hp_trusted)
{
    switch (mode) {
    case orbit::AudioOut::Speaker: return OutputState{true, true};
    case orbit::AudioOut::Headphones: return OutputState{false, false};
    default: break;
    }
    if (!hp_trusted) return OutputState{true, true};
    return hp ? OutputState{false, false} : OutputState{true, true};
}

// A level that must be read `need` times in a row before it is accepted; the first reading is taken as is.
struct Debounce {
    bool state = false;
    bool seeded = false;
    bool cand = false;
    int count = 0;
    bool update(bool level, int need)
    {
        if (!seeded) {
            seeded = true;
            state = cand = level;
            count = 0;
            return state;
        }
        if (level == state) {
            count = 0;
            return state;
        }
        if (level != cand) {
            cand = level;
            count = 0;
        }
        if (++count >= need) {
            state = level;
            count = 0;
        }
        return state;
    }
};

// Audio task overload: a render that takes (nearly) the whole block period means i2s_channel_write finds a
// free DMA buffer every time and never blocks, so the top-priority task would never let anything else run
// on its core. After `limit` such blocks in a row the task should sleep one tick once.
struct OverloadGuard {
    uint32_t run = 0;
    uint32_t total = 0;   // overloaded blocks since start
    // true: yield now
    bool block(uint32_t render_us, uint32_t period_us, uint32_t limit)
    {
        if ((uint64_t)render_us * 100u >= (uint64_t)period_us * 95u) {
            ++total;
            if (++run >= limit) {
                run = 0;
                return true;
            }
            return false;
        }
        run = 0;
        return false;
    }
};

// DMA descriptors (one block each) for a block size: 64-frame blocks get one more buffer of slack
// (1.33 ms at 48 kHz) against core-0 critical sections until the on-device timing is measured.
inline int dmaDescFor(int frames) { return frames <= 64 ? 4 : 3; }

// ---------------------------------------------------------------- touch: the finger LVGL follows (touch.cpp)
// LVGL has one pointer. It follows the first finger down until that finger lifts, then nobody until the other
// fingers it could follow are up too (no jump onto a finger resting somewhere). A finger that came down in the
// exclusive zone (setTouchExclusive: the BASS KEYS keyboard, played through the TouchHook) is never followed and
// never blocks: a knob can be turned while keys are held, and tapped again after the knob finger lifted.
struct PointerFollow {
    int primary = -1;         // id followed, -1 nobody, -2 wait until the followable fingers are up
    uint32_t prev_ids = 0;    // finger ids of the previous sample
    uint32_t excl_ids = 0;    // fingers that came down in the exclusive zone (until they lift)

    // ids[i] / in_zone[i]: the n fingers of this sample and whether each lies in the exclusive zone now (only
    // read for fingers that just came down). Returns the index of the followed finger, -1 nobody.
    int update(const uint8_t* ids, const bool* in_zone, int n)
    {
        uint32_t now = 0;
        for (int i = 0; i < n; i++) {
            const uint32_t b = 1u << (ids[i] & 31);
            if (!(prev_ids & b) && in_zone[i]) excl_ids |= b;
            now |= b;
        }
        excl_ids &= now;
        prev_ids = now;
        bool other = false;   // a followable finger is down
        for (int i = 0; i < n; i++) other = other || !(excl_ids & (1u << (ids[i] & 31)));
        if (primary == -2 && !other) primary = -1;
        for (int i = 0; i < n && primary == -1; i++) {
            if (!(excl_ids & (1u << (ids[i] & 31)))) primary = ids[i];
        }
        for (int i = 0; i < n; i++) {
            if (ids[i] == primary) return i;
        }
        if (primary >= 0) primary = other ? -2 : -1;   // the followed finger lifted
        return -1;
    }
    void reset()   // all fingers up
    {
        primary = -1;
        prev_ids = 0;
        excl_ids = 0;
    }
};

}  // namespace hal::logic
