// Build + run (Git Bash, project root):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost host/tests/test_hal_logic.cpp -o host/bin/test_hal_logic.exe && host/bin/test_hal_logic.exe
// Tests the portable decision logic of the Tab5 HAL (firmware/main/hal/hal_logic.h), the code display.cpp
// and audio.cpp run on the device:
//  - rectangle helpers (landscape -> native mapping, damage merging, rectangle subtraction);
//  - FrameBook, the tear-free presentation bookkeeping, simulated with three pixel buffers: every published
//    frame equals what LVGL drew, the scanned-out and the held (just retired) buffers are never written, idle
//    repairs take the copy off the next refresh's critical path, the back buffer has the least to copy;
//  - outputFor (speaker / mono policy with an unverified HP_DET), Debounce, OverloadGuard, dmaDescFor.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "hal/hal_logic.h"

using namespace hal::logic;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint32_t g_rng = 12345;
static uint32_t rnd()
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}
static int rndIn(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static Rect rndRect(int w, int h, int maxw, int maxh)
{
    const int rw = rndIn(1, maxw), rh = rndIn(1, maxh);
    const int x = rndIn(0, w - rw), y = rndIn(0, h - rh);
    return Rect{x, y, x + rw - 1, y + rh - 1};
}
static bool inside(const Rect& r, int x, int y) { return x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2; }

// ---------------------------------------------------------------- rectangles
static void testRects()
{
    // logical (lx, ly) -> native (ly, 1279 - lx); the touch inverse is lx = 1279 - raw_y, ly = raw_x
    const Rect n = toNative(Rect{0, 0, 1279, 719}, 1280);
    CHECK(n.x1 == 0 && n.y1 == 0 && n.x2 == 719 && n.y2 == 1279);
    const Rect p = toNative(Rect{10, 20, 10, 20}, 1280);
    CHECK(p.x1 == 20 && p.y1 == 1269 && p.x2 == 20 && p.y2 == 1269);
    CHECK(1279 - p.y1 == 10 && p.x1 == 20);
    const Rect q = toNative(Rect{100, 5, 199, 54}, 1280);
    CHECK(rectW(q) == 50 && rectH(q) == 100 && rectPx(q) == 5000u);

    // damage: overlapping / touching rectangles merge, a full list collapses to the whole frame
    const Rect full{0, 0, 99, 99};
    Rect list[4];
    unsigned cnt = 0;
    addDamage(list, &cnt, 4, Rect{0, 0, 9, 9}, full);
    addDamage(list, &cnt, 4, Rect{10, 0, 19, 9}, full);   // touches: merged
    CHECK(cnt == 1 && list[0].x2 == 19);
    addDamage(list, &cnt, 4, Rect{50, 50, 59, 59}, full);
    addDamage(list, &cnt, 4, Rect{30, 70, 31, 71}, full);
    addDamage(list, &cnt, 4, Rect{80, 10, 81, 11}, full);
    CHECK(cnt == 4);
    addDamage(list, &cnt, 4, Rect{90, 90, 91, 91}, full);
    CHECK(cnt == 1 && list[0].x1 == 0 && list[0].y2 == 99);

    // subtraction against a pixel grid
    constexpr int W = 40, H = 30, kMax = 24;
    Rect out[kMax], tmp[kMax];
    int overflow = 0;
    for (int it = 0; it < 4000; ++it) {
        const Rect r = rndRect(W, H, W, H);
        Rect cov[8];
        const int nc = rndIn(0, 8);
        for (int c = 0; c < nc; ++c) cov[c] = rndRect(W, H, 20, 20);
        const int nout = subtract(r, cov, nc, out, tmp, kMax);
        if (nout < 0) {
            ++overflow;
            continue;
        }
        bool ok = true;
        for (int y = 0; y < H && ok; ++y) {
            for (int x = 0; x < W && ok; ++x) {
                bool covered = false;
                for (int c = 0; c < nc; ++c) covered |= inside(cov[c], x, y);
                const bool want = inside(r, x, y) && !covered;
                int hits = 0;
                for (int k = 0; k < nout; ++k) hits += inside(out[k], x, y) ? 1 : 0;
                if (hits != (want ? 1 : 0)) ok = false;   // exact cover, pieces disjoint
            }
        }
        CHECK(ok);
        for (int k = 0; k < nout; ++k) CHECK(out[k].x1 <= out[k].x2 && out[k].y1 <= out[k].y2);
    }
    CHECK(overflow < 4000 / 4);   // the -1 path is the exception, not the rule
    // no cover: the rectangle itself; full cover: nothing
    CHECK(subtract(Rect{1, 2, 3, 4}, nullptr, 0, out, tmp, kMax) == 1 && out[0].x1 == 1 && out[0].y2 == 4);
    const Rect all{0, 0, 50, 50};
    CHECK(subtract(Rect{1, 2, 3, 4}, &all, 1, out, tmp, kMax) == 0);
}

// ---------------------------------------------------------------- presentation simulation
namespace {
constexpr int SW = 24, SH = 40;   // a small native frame
using Book = FrameBook<16, 24>;

struct Sim {
    Book book;
    std::vector<uint16_t> buf[3];
    std::vector<uint16_t> truth;
    uint32_t vsync = 0;
    uint64_t prepare_px = 0;      // copied on the refreshes' critical path
    uint64_t idle_px = 0;         // copied between refreshes
    int bad_writes = 0;           // copies into the latest or a held buffer
    uint16_t next_value = 1;

    Sim()
    {
        book.reset(SW, SH);
        for (auto& b : buf) b.assign(SW * SH, 0);
        truth.assign(SW * SH, 0);
    }
    // independent of FrameBook: the scanned-out buffer, and the one replaced at VSYNC count v until v + 2
    unsigned shown = 0;
    uint32_t free_from[3] = {0, 0, 0};
    bool mayWrite(unsigned i) const { return i != shown && (int32_t)(vsync - free_from[i]) >= 0; }
    void copyRect(int dst, const Rect& r)
    {
        if (!mayWrite((unsigned)dst)) ++bad_writes;
        for (int y = r.y1; y <= r.y2; ++y)
            for (int x = r.x1; x <= r.x2; ++x) buf[dst][y * SW + x] = buf[book.latest][y * SW + x];
    }
    // one LVGL refresh redrawing `cover` (each area in full), presented like display.cpp does it
    void refresh(const Rect* cover, int ncover, bool cover_known)
    {
        for (int c = 0; c < ncover; ++c) {
            const uint16_t v = next_value++;
            for (int y = cover[c].y1; y <= cover[c].y2; ++y)
                for (int x = cover[c].x1; x <= cover[c].x2; ++x) truth[y * SW + x] = (uint16_t)(v * 7 + x + y * 3);
        }
        if (book.pending) {   // beginFrame: wait for VSYNC while both other buffers are held
            int cand = book.candidate(vsync);
            for (int guard = 0; cand < 0 && guard < 10; ++guard) cand = book.candidate(++vsync);
            CHECK(cand >= 0);
            // the least stale writable buffer is chosen
            for (unsigned i = 0; i < 3; ++i) {
                if (book.writable(i, vsync)) CHECK(book.stalePx((unsigned)cand) <= book.stalePx(i));
            }
            const Rect* cv = cover_known ? cover : &cover[0];   // too many invalid areas: the first piece only
            prepare_px += book.prepare((unsigned)cand, cv, cover_known ? ncover : 1,
                                       [this](int d, const Rect& r) { copyRect(d, r); });
        }
        // pieces: every cover area, rendered and rotated into the back buffer
        for (int c = 0; c < ncover; ++c) {
            if (!mayWrite(book.back)) ++bad_writes;
            for (int y = cover[c].y1; y <= cover[c].y2; ++y)
                for (int x = cover[c].x1; x <= cover[c].x2; ++x) buf[book.back][y * SW + x] = truth[y * SW + x];
            book.damaged(cover[c]);
        }
        free_from[shown] = vsync + 2;
        shown = book.back;
        book.published(vsync);
        CHECK(book.latest == shown);
        CHECK(buf[book.latest] == truth);   // the published frame is complete: no tearing, nothing stale
    }
    uint32_t irng = 777;          // idle timing: its own generator, so both runs see the same frames
    uint32_t irnd()
    {
        irng = irng * 1103515245u + 12345u;
        return irng >> 8;
    }
    void idle(uint32_t budget)
    {
        const int ticks = (int)(irnd() % 7);
        for (int t = 0; t < ticks; ++t) {
            idle_px += book.idleStep(vsync, budget, [this](int d, const Rect& r) { copyRect(d, r); });
            if ((irnd() & 1) != 0) ++vsync;
        }
    }
    void idleLong(int steps, uint32_t budget)
    {
        for (int t = 0; t < steps; ++t)
            idle_px += book.idleStep(vsync, budget, [this](int d, const Rect& r) { copyRect(d, r); });
    }
};

void runSim(bool with_idle, uint32_t seed, uint64_t* prepare_px, uint64_t* idle_px)
{
    g_rng = seed;
    Sim s;
    const Rect full{0, 0, SW - 1, SH - 1};
    for (int frame = 0; frame < 600; ++frame) {
        Rect cover[8];
        int n;
        const uint32_t kind = rnd() % 10;
        if (kind == 0 || frame == 0) {
            cover[0] = full;   // view switch / modal: the whole screen
            n = 1;
        } else {
            n = rndIn(1, 8);
            for (int c = 0; c < n; ++c) cover[c] = rndRect(SW, SH, kind < 4 ? 4 : 12, kind < 4 ? 4 : 12);
        }
        s.refresh(cover, n, (rnd() % 8) != 0);
        if (with_idle) s.idle(64);
        s.vsync += (uint32_t)rndIn(0, 2);
        CHECK(s.bad_writes == 0);
        if (s.bad_writes) break;
    }
    if (with_idle) {
        // long idle: everything repairable gets repaired
        s.idleLong(400, 64);
        s.vsync += 3;
        s.idleLong(400, 64);
        CHECK(s.book.idleTarget(s.vsync) < 0);
        for (unsigned i = 0; i < 3; ++i) {
            if (i != s.book.latest) CHECK(s.book.stale_count[i] == 0 && s.buf[i] == s.truth);
        }
    }
    CHECK(s.bad_writes == 0);
    *prepare_px = s.prepare_px;
    *idle_px = s.idle_px;
}

void testFrameBook()
{
    uint64_t base_prep = 0, base_idle = 0, idle_prep = 0, idle_idle = 0;
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        uint64_t p, i;
        runSim(false, seed * 7919u, &p, &i);
        base_prep += p;
        base_idle += i;
        runSim(true, seed * 7919u, &p, &i);
        idle_prep += p;
        idle_idle += i;
    }
    CHECK(base_idle == 0);
    CHECK(idle_idle > 0);
    // idle repairs take copying off the refreshes' critical path (how much depends on the idle time per
    // frame; the simulation gives ~3 steps of 64 px between frames)
    std::printf("presentation: critical-path copy %llu px without idle repair, %llu px with (+%llu px idle)\n",
                (unsigned long long)base_prep, (unsigned long long)idle_prep, (unsigned long long)idle_idle);
    CHECK(idle_prep < base_prep);

    // The hitch after a big frame: a full-screen refresh, then a small one (playhead, pad flash). Without
    // idle time between them the small refresh first copies nearly the whole frame into its back buffer;
    // with idle repairs it copies nothing.
    for (int with_idle = 0; with_idle < 2; ++with_idle) {
        Sim s;
        const Rect full{0, 0, SW - 1, SH - 1};
        const Rect small{3, 3, 5, 6};
        s.refresh(&full, 1, true);   // boot: the first frame needs no back-buffer preparation
        s.vsync += 3;
        s.refresh(&full, 1, true);   // view switch: both other buffers miss the whole screen
        s.vsync += 3;                // both are released (two VSYNCs after their switch)
        if (with_idle) s.idleLong(100, 96);
        const uint64_t before = s.prepare_px;
        s.refresh(&small, 1, true);
        const uint64_t cost = s.prepare_px - before;
        if (with_idle) CHECK(cost == 0);
        else CHECK(cost == (uint64_t)(SW * SH) - rectPx(small));
        CHECK(s.bad_writes == 0);
    }

    // idle steps stay within the budget (a bigger rectangle gives its top rows)
    Book b;
    b.reset(SW, SH);
    b.back = 1;
    b.damaged(Rect{0, 0, SW - 1, SH - 1});
    b.published(10);   // latest = 1, buffer 0 held until VSYNC 12, buffer 2 writable
    CHECK(b.latest == 1 && b.pending);
    CHECK(!b.writable(1, 10) && !b.writable(0, 11) && b.writable(0, 12) && b.writable(2, 10));
    CHECK(b.idleTarget(10) == 2);
    int calls = 0;
    uint32_t px = b.idleStep(10, 100, [&](int d, const Rect& r) {
        ++calls;
        CHECK(d == 2);
        CHECK(rectPx(r) <= 100u);
    });
    CHECK(calls == 1 && px == (uint32_t)(SW * 4));   // 100 px budget / 24 px rows = 4 rows
    CHECK(b.stalePx(2) == (uint64_t)(SW * SH - SW * 4));
    // only the held buffer has stale areas left -> no idle work until it is released
    while (b.idleStep(10, 1000, [](int, const Rect&) {})) {
    }
    CHECK(b.stale_count[2] == 0);
    CHECK(b.idleTarget(11) < 0 && b.idleTarget(12) == 0);
    CHECK(b.candidate(11) == 2);              // the clean one
    CHECK(b.oldestRetired() == 0 || b.oldestRetired() == 2);
    // not pending (a refresh is being drawn): no idle work at all
    b.prepare(2, nullptr, 0, [](int, const Rect&) {});
    CHECK(!b.pending && b.back == 2 && b.idleTarget(20) < 0);
}
}  // namespace

// ---------------------------------------------------------------- audio policy
static void testAudioPolicy()
{
    using orbit::AudioOut;
    // forced modes
    CHECK(outputFor(AudioOut::Speaker, true, true).speaker && outputFor(AudioOut::Speaker, true, true).mono);
    CHECK(!outputFor(AudioOut::Headphones, false, false).speaker && !outputFor(AudioOut::Headphones, false, false).mono);
    // Auto with a verified HP_DET: headphones -> speaker off, stereo
    CHECK(!outputFor(AudioOut::Auto, true, true).speaker && !outputFor(AudioOut::Auto, true, true).mono);
    CHECK(outputFor(AudioOut::Auto, false, true).speaker && outputFor(AudioOut::Auto, false, true).mono);
    // Auto with an unverified HP_DET: a misread pin must never silence the speaker or drop its right channel
    for (int hp = 0; hp < 2; ++hp) {
        const OutputState o = outputFor(AudioOut::Auto, hp != 0, false);
        CHECK(o.speaker && o.mono);
    }

    Debounce d;
    CHECK(d.update(true, 2));          // the first reading counts at once
    CHECK(d.update(false, 2));         // one deviating reading: unchanged
    CHECK(!d.update(false, 2));        // two in a row: accepted
    CHECK(!d.update(true, 2));
    CHECK(!d.update(false, 2));        // glitch interrupted: the count starts again
    CHECK(!d.update(true, 2));
    CHECK(d.update(true, 2));

    OverloadGuard g;
    int yields = 0;
    for (int i = 0; i < 100; ++i) yields += g.block(1300, 1333, 32) ? 1 : 0;   // 97.5 %: overloaded
    CHECK(yields == 3 && g.total == 100);
    CHECK(!g.block(1000, 1333, 32) && g.run == 0);                               // 75 %: fine
    for (int i = 0; i < 31; ++i) CHECK(!g.block(1333, 1333, 32));
    CHECK(g.block(1333, 1333, 32));
    CHECK(!g.block(1266, 1333, 1));                                              // 94.97 %: not counted

    CHECK(dmaDescFor(64) == 4 && dmaDescFor(48) == 4 && dmaDescFor(128) == 3 && dmaDescFor(256) == 3);
}

// ---------------------------------------------------------------- touch: the finger LVGL follows (0.4.3)
static void testPointerFollow()
{
    // one finger: followed until it lifts
    {
        PointerFollow f;
        const uint8_t a[1] = {3};
        const bool out[1] = {false};
        CHECK(f.update(a, out, 1) == 0);
        CHECK(f.update(a, out, 1) == 0);
        f.reset();
        CHECK(f.primary == -1 && f.excl_ids == 0);
    }
    // a key held in the zone, then a knob finger: LVGL follows the knob; the knob lifts and a new tap is followed
    // at once (the key finger does not block)
    {
        PointerFollow f;
        const uint8_t key[1] = {0};
        const bool in[1] = {true};
        CHECK(f.update(key, in, 1) == -1);
        const uint8_t both[2] = {0, 1};
        const bool z2[2] = {true, false};
        CHECK(f.update(both, z2, 2) == 1);
        const bool z2moved[2] = {false, true};   // the zone is asked only on the way down: roles stay
        CHECK(f.update(both, z2moved, 2) == 1);
        CHECK(f.update(key, in, 1) == -1);        // the knob finger lifted
        CHECK(f.primary == -1);
        const uint8_t tap[2] = {0, 2};
        CHECK(f.update(tap, z2, 2) == 1);         // a new finger is followed while the key is still held
        CHECK(f.primary == 2);
    }
    // two ordinary fingers: the first is followed; when it lifts, nobody until the second lifts too
    {
        PointerFollow f;
        const uint8_t a[1] = {0}, ab[2] = {0, 1}, b[1] = {1}, c[1] = {2};
        const bool no[2] = {false, false};
        CHECK(f.update(a, no, 1) == 0);
        CHECK(f.update(ab, no, 2) == 0);
        CHECK(f.update(b, no, 1) == -1 && f.primary == -2);   // no jump onto the resting finger
        CHECK(f.update(b, no, 1) == -1);
        CHECK(f.update(c, no, 1) == -1);   // (b lifted and c came down in the same sample: still waiting)
        f.reset();
        CHECK(f.update(c, no, 1) == 0);
    }
    // a finger that came down outside the zone stays followable inside it; only zone fingers are skipped
    {
        PointerFollow f;
        const uint8_t a[1] = {4};
        const bool out[1] = {false}, in[1] = {true};
        CHECK(f.update(a, out, 1) == 0);
        CHECK(f.update(a, in, 1) == 0);
        const uint8_t ab[2] = {4, 5};
        const bool z[2] = {true, true};
        CHECK(f.update(ab, z, 2) == 0);   // 5 came down in the zone: never followed
        CHECK(f.excl_ids == (1u << 5));
    }
}

int main()
{
    testRects();
    testFrameBook();
    testAudioPolicy();
    testPointerFollow();
    std::printf(fails ? "hal logic: %d FAILED\n" : "hal logic: all passed\n", fails);
    return fails ? 1 : 0;
}
