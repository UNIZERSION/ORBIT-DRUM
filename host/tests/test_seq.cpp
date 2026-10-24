// Build (Git Bash, project root):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost host/tests/test_seq.cpp firmware/main/core/seq.cpp firmware/main/core/params.cpp firmware/main/core/project.cpp firmware/main/core/sample.cpp host/platform_host.cpp -o host/bin/test_seq.exe && host/bin/test_seq.exe
//
// Sequencer tests: exact sample offsets at 48 kHz / 120 BPM (1/16 step = 6000 frames).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "core/project.h"
#include "core/seq.h"

using namespace orbit;

static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);      \
            ++fails;                                                     \
        }                                                                \
    } while (0)
#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const long long _a = (long long)(a), _b = (long long)(b);                                   \
        if (_a != _b) {                                                                             \
            std::printf("FAIL %s:%d %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
            ++fails;                                                                                \
        }                                                                                           \
    } while (0)

constexpr int kRate = 48000;
constexpr uint64_t kStep = 6000;   // 1/16 at 120 BPM
constexpr uint64_t kBar = 16 * kStep;

struct Ev {
    uint64_t frame;
    int pad, vel, pitch;
    bool operator==(const Ev& o) const { return frame == o.frame && pad == o.pad && vel == o.vel && pitch == o.pitch; }
};
struct Click {
    uint64_t frame;
    bool accent;
    bool operator==(const Click& o) const { return frame == o.frame && accent == o.accent; }
};
struct Mark {   // a notice: block [start, start + n)
    uint64_t start;
    int n;
};

static Project* newProject()
{
    Project* p = new Project;
    projectDefaults(*p);
    for (int s = 0; s < kScenes; ++s) {
        Scene& sc = p->scene_data[s];
        for (int t = 0; t < kPads; ++t)
            for (int i = 0; i < kMaxSteps; ++i) sc.track[t].step[i] = Step{};
        sc.length = 16;
        sc.swing = 50;
        sc.div = Division::D1_16;
    }
    p->song.follow = 0;
    p->bpm = 120.0f;
    p->quant = LaunchQuant::Bar;
    p->beats_per_bar = 4;
    return p;
}

static Step& on(Project& p, int scene, int pad, int step, int vel = 100)
{
    Step& s = p.scene_data[scene].track[pad].step[step];
    s.on = 1;
    s.vel = (uint8_t)vel;
    return s;
}

struct Run {
    Project* p;
    Sequencer seq;
    uint64_t now = 0;
    std::vector<Ev> ev;
    std::vector<Click> clicks;
    std::vector<Mark> scene_marks, slot_marks, transport_marks;
    int max_out = 256;

    explicit Run(Project* proj) : p(proj) { seq.init(kRate, p); }

    void block(int n)
    {
        SeqTrigger t[256];
        SeqNotice no;
        const int c = seq.process(n, t, max_out, no);
        CHECK(c >= 0 && c <= max_out);
        for (int i = 0; i < c; ++i) {
            CHECK(t[i].offset < (uint32_t)n);
            if (i) CHECK(t[i].offset >= t[i - 1].offset);
            ev.push_back(Ev{now + t[i].offset, t[i].pad, t[i].vel, t[i].pitch});
        }
        if (no.metronome_click) {
            CHECK(no.click_offset < (uint32_t)n);
            clicks.push_back(Click{now + no.click_offset, no.metronome_accent});
        }
        if (no.scene_changed) scene_marks.push_back(Mark{now, n});
        if (no.song_slot_changed) slot_marks.push_back(Mark{now, n});
        if (no.transport_changed) transport_marks.push_back(Mark{now, n});
        now += (uint64_t)n;
    }
    void runTo(uint64_t frame, int bs)
    {
        while (now < frame) {
            const uint64_t left = frame - now;
            block(left < (uint64_t)bs ? (int)left : bs);
        }
    }
    std::vector<uint64_t> frames(int pad) const
    {
        std::vector<uint64_t> f;
        for (const Ev& e : ev)
            if (e.pad == pad) f.push_back(e.frame);
        return f;
    }
    std::vector<Ev> of(int pad) const
    {
        std::vector<Ev> f;
        for (const Ev& e : ev)
            if (e.pad == pad) f.push_back(e);
        return f;
    }
};

static bool contains(const Mark& m, uint64_t frame) { return frame >= m.start && frame < m.start + (uint64_t)m.n; }

static void expectFrames(const std::vector<uint64_t>& got, const std::vector<uint64_t>& want, const char* what)
{
    bool ok = got == want;
    if (!ok) {
        std::printf("FAIL %s: got", what);
        for (uint64_t f : got) std::printf(" %llu", (unsigned long long)f);
        std::printf(" | want");
        for (uint64_t f : want) std::printf(" %llu", (unsigned long long)f);
        std::printf("\n");
        ++fails;
    }
}

static const int kBlockSizes[] = {1, 64, 128, 256, 97};

// ---------------------------------------------------------------------------------------------

static void testBasicOffsets()
{
    Project* p = newProject();
    on(*p, 0, 0, 0);
    on(*p, 0, 0, 4);
    on(*p, 0, 0, 8);
    on(*p, 0, 0, 12);
    std::vector<uint64_t> want;
    for (uint64_t i = 0; i < 8; ++i) want.push_back(i * 4 * kStep);
    for (int bs : kBlockSizes) {
        Run r(p);
        r.seq.play();
        CHECK(r.seq.playing());
        CHECK_EQ(r.seq.step(), 0);
        r.runTo(2 * kBar, bs);
        char what[64];
        std::snprintf(what, sizeof what, "basic offsets bs=%d", bs);
        expectFrames(r.frames(0), want, what);
        CHECK_EQ(r.ev.size(), 8u);
        for (const Ev& e : r.ev) CHECK(e.vel == 100 && e.pitch == 0);
        CHECK_EQ(r.seq.loopCount(), 1u);   // the boundary at the block end belongs to the next block
        CHECK_EQ(r.seq.step(), 15);
        CHECK(r.transport_marks.size() == 1 && r.transport_marks[0].start == 0);
    }
    delete p;
    // 44.1 kHz: 5512.5 frames per step, rounded up to whole frames, no drift
    p = newProject();
    for (int s = 0; s < 16; ++s) on(*p, 0, 0, s);
    for (int bs : kBlockSizes) {
        Run r(p);
        r.seq.init(44100, p);
        r.seq.play();
        r.runTo(441000, bs);
        const std::vector<uint64_t> f = r.frames(0);
        CHECK_EQ(f.size(), 80u);
        bool ok = true;
        for (size_t i = 0; i < f.size(); ++i) ok = ok && f[i] == (i * 11025 + 1) / 2;
        CHECK(ok);
    }
    delete p;
}

static void testIdenticalTimelines()
{
    Project* p = newProject();
    Scene& sc = p->scene_data[0];
    sc.swing = 58;
    for (int i = 0; i < 16; i += 4) on(*p, 0, 0, i, 120);
    on(*p, 0, 1, 2).nudge = -30;
    on(*p, 0, 1, 5).nudge = 20;
    on(*p, 0, 1, 6).ratchet = 3;
    on(*p, 0, 1, 7).flam = 3;
    on(*p, 0, 1, 9).prob = 50;
    on(*p, 0, 1, 10).cond = TrigCond::P1of2;
    on(*p, 0, 1, 11).cond = TrigCond::First;
    {
        Step& s = on(*p, 0, 1, 13);
        s.ratchet = 4;
        s.flam = 8;
        s.nudge = -50;
        s.pitch = 7;
    }
    on(*p, 0, 1, 15).nudge = -50;
    sc.track[2].length = 3;
    on(*p, 0, 2, 0, 90);
    on(*p, 0, 2, 1, 80).prob = 70;
    sc.track[3].length = 5;
    on(*p, 0, 3, 2).ratchet = 2;
    on(*p, 0, 4, 3).cond = TrigCond::P3of4;
    on(*p, 0, 5, 1).nudge = -45;

    std::vector<Ev> ref;
    std::vector<Click> ref_clicks;
    for (int bs : kBlockSizes) {
        Run r(p);
        r.seq.seed(1234);
        r.seq.play();
        r.runTo(8 * kBar, bs);
        for (size_t i = 1; i < r.ev.size(); ++i) CHECK(r.ev[i].frame >= r.ev[i - 1].frame);
        if (ref.empty()) {
            ref = r.ev;
            ref_clicks = r.clicks;
            CHECK(ref.size() > 150);
            CHECK_EQ(ref_clicks.size(), 32u);
        } else {
            const bool same = r.ev == ref;
            if (!same) std::printf("FAIL timeline differs for block size %d (%zu vs %zu events)\n", bs, r.ev.size(), ref.size());
            CHECK(same);
            CHECK(r.clicks == ref_clicks);
        }
    }
    // spot checks of the reference (swing 58 = +960 frames on odd steps)
    auto has = [&](uint64_t f, int pad, int vel) {
        for (const Ev& e : ref)
            if (e.frame == f && e.pad == pad && e.vel == vel) return true;
        return false;
    };
    CHECK(has(2 * kStep - 1800, 1, 100));          // nudge -30
    CHECK(has(5 * kStep + 960 + 1200, 1, 100));    // swing + nudge 20
    CHECK(has(6 * kStep + 4000, 1, 100));          // ratchet 3, third hit
    CHECK(has(7 * kStep + 960 - 1344, 1, 60));     // flam 3 = 28 ms grace
    CHECK(has(13 * kStep + 960 - 3000 - 2304, 1, 60));  // flam 8 before a nudged ratchet
    CHECK(has(kBar + 15 * kStep + 960 - 3000, 1, 100)); // 2nd loop, step 15 nudge -50
    CHECK(has(kStep + 960 - 2700, 5, 100));        // swing + nudge -45
    delete p;
}

static void testSwing()
{
    const int sw[] = {54, 62, 75};
    const uint64_t delay[] = {480, 1440, 3000};
    for (int i = 0; i < 3; ++i) {
        Project* p = newProject();
        p->scene_data[0].swing = (uint8_t)sw[i];
        for (int s = 0; s < 4; ++s) on(*p, 0, 0, s);
        for (int bs : {1, 97, 256}) {
            Run r(p);
            r.seq.play();
            r.runTo(kBar, bs);
            expectFrames(r.frames(0), {0, kStep + delay[i], 2 * kStep, 3 * kStep + delay[i]}, "swing");
        }
        delete p;
    }
}

static void testNudge()
{
    Project* p = newProject();
    on(*p, 0, 0, 0).nudge = -10;
    on(*p, 0, 0, 4).nudge = -25;
    on(*p, 0, 0, 8).nudge = 40;
    on(*p, 0, 0, 12).nudge = 50;
    for (int bs : kBlockSizes) {
        Run r(p);
        r.seq.play();
        r.runTo(2 * kBar, bs);
        // step 0 of the first pass is "late" at play start and fires at 0; later passes are early
        expectFrames(r.frames(0),
                     {0, 22500, 50400, 75000, kBar - 600, kBar + 22500, kBar + 50400, kBar + 75000, 2 * kBar - 600},
                     "nudge");
    }
    delete p;
}

static void testRatchetFlam()
{
    Project* p = newProject();
    on(*p, 0, 0, 0).ratchet = 2;
    on(*p, 0, 1, 4).ratchet = 3;
    on(*p, 0, 2, 8).ratchet = 4;
    on(*p, 0, 3, 4).flam = 1;
    on(*p, 0, 4, 8, 127).flam = 8;
    {
        Step& s = on(*p, 0, 5, 12);
        s.flam = 2;
        s.ratchet = 2;
    }
    for (int bs : kBlockSizes) {
        Run r(p);
        r.seq.play();
        r.runTo(kBar, bs);
        expectFrames(r.frames(0), {0, 3000}, "ratchet 2");
        expectFrames(r.frames(1), {24000, 26000, 28000}, "ratchet 3");
        expectFrames(r.frames(2), {48000, 49500, 51000, 52500}, "ratchet 4");
        const std::vector<Ev> f1 = r.of(3), f8 = r.of(4), fr = r.of(5);
        CHECK(f1.size() == 2 && f1[0].frame == 24000 - 960 && f1[0].vel == 60 && f1[1].frame == 24000 && f1[1].vel == 100);
        CHECK(f8.size() == 2 && f8[0].frame == 48000 - 2304 && f8[0].vel == 76 && f8[1].frame == 48000 && f8[1].vel == 127);
        CHECK(fr.size() == 3 && fr[0].frame == 72000 - 1152 && fr[0].vel == 60 && fr[1].frame == 72000 && fr[2].frame == 75000);
    }
    delete p;
}

static void testProbability()
{
    Project* p = newProject();
    p->scene_data[0].length = 1;
    on(*p, 0, 0, 0).prob = 50;
    on(*p, 0, 1, 0).prob = 0;
    on(*p, 0, 2, 0).prob = 100;
    int counts[2] = {0, 0};
    for (int rep = 0; rep < 2; ++rep) {
        Run r(p);
        r.seq.seed(777);
        r.seq.play();
        r.runTo(10000 * kStep, 256);
        counts[rep] = (int)r.frames(0).size();
        CHECK_EQ(r.frames(1).size(), 0u);
        CHECK_EQ(r.frames(2).size(), 10000u);
        CHECK_EQ(r.seq.loopCount(), 9999u);
    }
    std::printf("probability 50%%: %d / 10000\n", counts[0]);
    CHECK(counts[0] > 4750 && counts[0] < 5250);
    CHECK_EQ(counts[0], counts[1]);   // deterministic per seed
    delete p;
}

static void testTrigConditions()
{
    Project* p = newProject();
    p->scene_data[0].length = 2;
    const TrigCond conds[] = {TrigCond::Always, TrigCond::P1of2, TrigCond::P2of2, TrigCond::P1of4, TrigCond::P2of4,
                              TrigCond::P3of4, TrigCond::P4of4, TrigCond::Fill, TrigCond::NotFill, TrigCond::First};
    for (int i = 0; i < 10; ++i) on(*p, 0, i, 0).cond = conds[i];
    // A:B counts loops of the TRACK: a 1-step track loops every step
    p->scene_data[0].track[10].length = 1;
    on(*p, 0, 10, 0).cond = TrigCond::P1of2;

    const uint64_t loop = 2 * kStep;
    auto loops = [&](const Run& r, int pad) {
        std::vector<uint64_t> l;
        for (uint64_t f : r.frames(pad)) l.push_back(f / loop);
        return l;
    };
    {
        Run r(p);
        r.seq.play();
        r.runTo(8 * loop, 64);
        expectFrames(loops(r, 0), {0, 1, 2, 3, 4, 5, 6, 7}, "always");
        expectFrames(loops(r, 1), {0, 2, 4, 6}, "1:2");
        expectFrames(loops(r, 2), {1, 3, 5, 7}, "2:2");
        expectFrames(loops(r, 3), {0, 4}, "1:4");
        expectFrames(loops(r, 4), {1, 5}, "2:4");
        expectFrames(loops(r, 5), {2, 6}, "3:4");
        expectFrames(loops(r, 6), {3, 7}, "4:4");
        expectFrames(loops(r, 7), {}, "fill (off)");
        expectFrames(loops(r, 8), {0, 1, 2, 3, 4, 5, 6, 7}, "!fill (off)");
        expectFrames(loops(r, 9), {0}, "first");
        expectFrames(r.frames(10), {0, 2 * kStep, 4 * kStep, 6 * kStep, 8 * kStep, 10 * kStep, 12 * kStep, 14 * kStep},
                     "1:2 on a 1-step track");
    }
    {
        Run r(p);
        r.seq.play();
        r.runTo(40000, 100);   // FILL is read at expansion (lookahead): the step at 48000 is next
        r.seq.setFill(true);
        r.runTo(8 * loop, 100);
        expectFrames(loops(r, 7), {4, 5, 6, 7}, "fill held from loop 4");
        expectFrames(loops(r, 8), {0, 1, 2, 3}, "!fill released at loop 4");
    }
    delete p;
}

static void testPolymeter()
{
    Project* p = newProject();
    p->scene_data[0].track[0].length = 3;
    on(*p, 0, 0, 0);
    on(*p, 0, 1, 0);
    p->scene_data[0].track[2].length = 3;
    on(*p, 0, 2, 2);
    for (int bs : {1, 97, 256}) {
        Run r(p);
        r.seq.play();
        r.runTo(2 * kBar, bs);
        std::vector<uint64_t> w0, w2;
        for (uint64_t s = 0; s < 32; ++s) {
            if (s % 3 == 0) w0.push_back(s * kStep);
            if (s % 3 == 2) w2.push_back(s * kStep);
        }
        expectFrames(r.frames(0), w0, "polymeter 3 (step 0)");
        expectFrames(r.frames(2), w2, "polymeter 3 (step 2)");
        expectFrames(r.frames(1), {0, kBar}, "scene length 16");
        CHECK_EQ(r.seq.loopCount(), 1u);
    }
    delete p;
}

static void testTempoChange()
{
    Project* p = newProject();
    for (int s = 0; s < 16; ++s) on(*p, 0, 0, s, s + 1);   // velocity = step + 1
    on(*p, 0, 1, 9).nudge = -50;
    // exact: change at a block boundary inside step 8 (phase 1/3)
    for (int bs : {50, 100}) {
        p->bpm = 120.0f;
        Run r(p);
        r.seq.play();
        r.runTo(50000, bs);
        p->bpm = 150.0f;   // 4800 frames per step
        r.runTo(kBar, bs);
        std::vector<uint64_t> f = r.frames(0);
        CHECK(f.size() >= 16);
        for (int s = 0; s <= 8; ++s) CHECK_EQ(f[s], s * kStep);
        CHECK_EQ(f[9], 53200);
        CHECK_EQ(f[10], 58000);
        CHECK_EQ(f[15], 53200 + 6 * 4800);
        expectFrames(r.frames(1), {53200 - 2400}, "early hit after a tempo change");
    }
    // random tempo every block: every step exactly once, in order, no gaps
    for (int bs : {64, 97, 256}) {
        p->bpm = 120.0f;
        Run r(p);
        r.seq.play();
        uint32_t x = 12345;
        for (int b = 0; b < 30000; ++b) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            if ((x & 7) == 0) p->bpm = 60.0f + (float)(x % 14000) / 100.0f;
            r.block(bs);
        }
        const std::vector<Ev> e = r.of(0);
        std::printf("random tempo bs=%d: %zu steps\n", bs, e.size());
        CHECK(e.size() > 250);
        bool ok = true;
        for (size_t i = 0; i < e.size(); ++i) {
            ok = ok && e[i].vel == (int)(i % 16) + 1;
            if (i) ok = ok && e[i].frame > e[i - 1].frame;
        }
        CHECK(ok);
        // the early hit (step 9, nudge -50) always lands inside step 8, once per pass
        int last_vel = 0, early = 0, step9 = 0;
        bool inside = true;
        for (const Ev& x : r.ev) {
            if (x.pad == 0) {
                last_vel = x.vel;
                step9 += x.vel == 10;
            } else if (x.pad == 1) {
                inside = inside && last_vel == 9;
                ++early;
            }
        }
        CHECK(inside);
        CHECK(early == step9 || early == step9 + 1);
        // steps heard so far == floor(beats * 4) + 1
        CHECK_EQ(e.size(), (size_t)std::floor(r.seq.beats() * 4.0 - 1e-9) + 1);
    }
    p->bpm = 120.0f;
    delete p;
}

static void testSceneSwitch()
{
    struct Case {
        LaunchQuant q;
        uint64_t queue_at, want;
    };
    const Case cases[] = {
        {LaunchQuant::Bar, 25000, 96000},    {LaunchQuant::Beat, 25000, 48000}, {LaunchQuant::Eighth, 25000, 36000},
        {LaunchQuant::Off, 25000, 30000},    // boundary already expanded: rollback
        {LaunchQuant::Eighth, 31000, 36000}, {LaunchQuant::Off, 31000, 36000},  {LaunchQuant::Beat, 47000, 48000},
        {LaunchQuant::Bar, 95000, 96000},
    };
    Project* p = newProject();
    p->scene_data[0].length = 12;   // bar boundaries do not coincide with the pass end
    for (int s = 0; s < 12; ++s) on(*p, 0, 0, s);
    for (int s = 0; s < 16; ++s) on(*p, 1, 1, s);
    on(*p, 1, 2, 0).nudge = -20;    // early hit of the new scene's first step
    for (const Case& c : cases) {
        for (int bs : {100, 64, 1}) {
            p->quant = c.q;
            Run r(p);
            r.seq.play();
            r.runTo(c.queue_at, bs);
            r.seq.queueScene(1);
            CHECK_EQ(r.seq.queuedScene(), 1);
            CHECK_EQ(r.seq.scene(), 0);
            r.runTo(c.want + 3 * kStep, bs);
            const std::vector<uint64_t> a = r.frames(0), b = r.frames(1), e = r.frames(2);
            CHECK(!b.empty() && b[0] == c.want);
            CHECK(!a.empty() && a.back() == c.want - kStep);
            CHECK_EQ(a.size(), c.want / kStep);
            CHECK_EQ(b.size(), 3u);
            if (c.want - 1200 > c.queue_at) {
                CHECK(e.size() == 1 && e[0] == c.want - 1200);
            } else {
                CHECK(e.empty());   // that early hit was already in the past when queued
            }
            CHECK(r.scene_marks.size() == 1 && contains(r.scene_marks[0], c.want));
            CHECK_EQ(r.seq.scene(), 1);
            CHECK_EQ(r.seq.queuedScene(), -1);
            CHECK_EQ(r.seq.step(), 2);
            CHECK_EQ(r.seq.loopCount(), 0u);
            if (b.empty() || b[0] != c.want)
                std::printf("  case quant=%d queue_at=%llu bs=%d\n", (int)c.q, (unsigned long long)c.queue_at, bs);
        }
    }
    // re-queueing the same scene after its early hit fired must not repeat that hit
    {
        p->quant = LaunchQuant::Beat;
        Run r(p);
        r.seq.play();
        r.runTo(25000, 100);
        r.seq.queueScene(1);
        r.runTo(47000, 100);   // the early hit (46800) has fired
        r.seq.queueScene(1);   // rollback + re-expansion of the same boundary
        r.runTo(60000, 100);
        expectFrames(r.frames(2), {46800}, "no duplicate after re-queue");
        CHECK_EQ(r.frames(1).front(), 48000);
    }
    // queue B then A (the playing scene) cancels
    {
        p->quant = LaunchQuant::Beat;
        Run r(p);
        r.seq.play();
        r.runTo(25000, 100);
        r.seq.queueScene(1);
        r.runTo(45000, 100);   // switch at 48000 already expanded
        CHECK_EQ(r.seq.queuedScene(), 1);
        r.seq.queueScene(0);
        CHECK_EQ(r.seq.queuedScene(), -1);
        r.runTo(2 * kBar, 100);
        CHECK(r.frames(1).empty());
        CHECK_EQ(r.seq.scene(), 0);
        CHECK(r.scene_marks.empty());
    }
    // switching to a scene with another division: exact from its first step, for any block size
    {
        Project* q = newProject();
        for (int s = 0; s < 16; ++s) on(*q, 0, 0, s);
        q->scene_data[1].div = Division::D1_8;   // 12000 frames per step
        q->scene_data[1].length = 4;
        for (int s = 0; s < 4; ++s) on(*q, 1, 1, s);
        on(*q, 1, 2, 1).nudge = -50;
        q->quant = LaunchQuant::Beat;
        for (int bs : kBlockSizes) {
            Run r(q);
            r.seq.play();
            r.runTo(25000, bs);
            r.seq.queueScene(1);
            r.runTo(100000, bs);
            r.seq.queueScene(0);   // back to 1/16 at the next beat: 120000 (108000 is an off-beat)
            r.runTo(130000, bs);
            expectFrames(r.frames(1), {48000, 60000, 72000, 84000, 96000, 108000}, "1/8 scene after 1/16");
            expectFrames(r.frames(2), {54000, 102000}, "1/8 scene early hit");
            const std::vector<uint64_t> a = r.frames(0);
            CHECK(a.size() == 8 + 2 && a[7] == 42000 && a[8] == 120000 && a[9] == 126000);
            CHECK(r.seq.scene() == 0 && r.seq.step() == 1);
        }
        delete q;
    }
    // stopped: immediate
    {
        Run r(p);
        r.seq.queueScene(1);
        CHECK_EQ(r.seq.scene(), 1);
        CHECK_EQ(r.seq.queuedScene(), -1);
        r.block(64);
        CHECK_EQ(r.scene_marks.size(), 1u);
        r.seq.play();
        r.runTo(kBar, 64);
        CHECK_EQ(r.frames(1).size(), 16u);
        CHECK(r.frames(0).empty());
    }
    delete p;
}

static void testSong()
{
    Project* p = newProject();
    on(*p, 0, 0, 0);
    on(*p, 1, 1, 0);
    on(*p, 2, 2, 0);
    p->scene_data[2].length = 8;
    for (int i = 0; i < kSongSlots; ++i) p->song.slot[i] = -1;
    p->song.slot[0] = 1;
    p->song.slot[1] = 2;
    p->song.follow = 1;
    {
        Run r(p);
        r.seq.play();
        CHECK_EQ(r.seq.songSlot(), 0);
        CHECK_EQ(r.seq.scene(), 1);
        r.runTo(50000, 128);
        r.seq.queueScene(3);   // ignored while following
        CHECK_EQ(r.seq.queuedScene(), -1);
        r.runTo(100000, 128);
        CHECK_EQ(r.seq.songSlot(), 1);
        CHECK_EQ(r.seq.scene(), 2);
        r.runTo(150000, 128);
        CHECK_EQ(r.seq.songSlot(), 0);   // empty slot 2: back to slot 0
        CHECK_EQ(r.seq.scene(), 1);
        r.runTo(400000, 128);
        expectFrames(r.frames(1), {0, 144000, 288000}, "song slot 0 (scene B)");
        expectFrames(r.frames(2), {96000, 240000, 384000}, "song slot 1 (scene C, 8 steps)");
        CHECK(r.frames(0).empty());
        CHECK(r.slot_marks.size() >= 6);
        CHECK(contains(r.slot_marks[0], 0) && contains(r.slot_marks[1], 96000) && contains(r.slot_marks[2], 144000));
        CHECK(contains(r.scene_marks[1], 96000) && contains(r.scene_marks[2], 144000));
    }
    // all 16 slots used: wraps after the last one
    {
        p->scene_data[1].length = 1;
        p->scene_data[2].length = 1;
        for (int i = 0; i < kSongSlots; ++i) p->song.slot[i] = (int8_t)(i % 2 ? 2 : 1);
        Run r(p);
        r.seq.play();
        std::vector<int> slots;
        for (int s = 0; s < 40; ++s) {
            r.runTo((uint64_t)s * kStep + 100, 100);
            slots.push_back(r.seq.songSlot());
        }
        bool ok = true;
        for (int s = 0; s < 40; ++s) ok = ok && slots[s] == s % 16;
        CHECK(ok);
        CHECK_EQ(r.frames(1).size(), 20u);
        CHECK_EQ(r.frames(2).size(), 20u);
        p->scene_data[1].length = 16;
        p->scene_data[2].length = 8;
    }
    // FOLLOW toggled while playing applies at the next scene boundary
    {
        for (int i = 0; i < kSongSlots; ++i) p->song.slot[i] = -1;
        p->song.slot[0] = 1;
        p->song.follow = 0;
        Run r(p);
        r.seq.play();
        CHECK_EQ(r.seq.songSlot(), -1);
        r.runTo(30000, 64);
        p->song.follow = 1;
        r.runTo(95000, 64);
        CHECK_EQ(r.seq.songSlot(), -1);
        CHECK_EQ(r.seq.scene(), 0);
        r.runTo(100000, 64);
        CHECK_EQ(r.seq.songSlot(), 0);
        CHECK_EQ(r.seq.scene(), 1);
        p->song.follow = 0;
        r.runTo(190000, 64);
        CHECK_EQ(r.seq.songSlot(), 0);
        r.runTo(200000, 64);
        CHECK_EQ(r.seq.songSlot(), -1);
        CHECK_EQ(r.seq.scene(), 1);   // keeps looping the current scene
        r.runTo(300000, 64);
        expectFrames(r.frames(0), {0}, "scene A before FOLLOW");
        expectFrames(r.frames(1), {96000, 192000, 288000}, "scene B after FOLLOW");
        r.seq.queueScene(0);          // not following any more: queue works again
        CHECK_EQ(r.seq.queuedScene(), 0);
    }
    delete p;
}

// FOLLOW switched off: scene selection must work at once, also while the song slot it was
// following is still playing or after STOP STOP rewound to song slot 0.
static void testFollowOff()
{
    Project* p = newProject();
    for (int s = 0; s < 16; ++s) on(*p, 0, 0, s);   // A: pad 0
    for (int s = 0; s < 16; ++s) on(*p, 1, 1, s);   // B: pad 1
    for (int s = 0; s < 16; ++s) on(*p, 2, 2, s);   // C: pad 2
    for (int i = 0; i < kSongSlots; ++i) p->song.slot[i] = -1;
    p->song.slot[0] = 0;
    p->song.slot[1] = 1;
    // stopped: FOLLOW on, STOP STOP (rewind to slot 0), FOLLOW off, select C, PLAY -> C
    {
        p->song.follow = 1;
        Run r(p);
        r.seq.play();
        r.runTo(30000, 64);
        r.seq.stop();
        r.seq.stop();
        CHECK_EQ(r.seq.songSlot(), 0);
        CHECK_EQ(r.seq.scene(), 0);
        p->song.follow = 0;
        r.seq.queueScene(2);
        CHECK_EQ(r.seq.scene(), 2);
        CHECK_EQ(r.seq.songSlot(), -1);
        CHECK_EQ(r.seq.queuedScene(), -1);
        r.runTo(30000 + 64, 64);
        CHECK(!r.scene_marks.empty() && !r.slot_marks.empty());
        const size_t a0 = r.frames(0).size();
        r.seq.play();
        const uint64_t t0 = r.now;
        r.runTo(t0 + kBar, 64);
        CHECK_EQ(r.frames(2).size(), 16u);
        CHECK(!r.frames(2).empty() && r.frames(2)[0] == t0);
        CHECK_EQ(r.frames(0).size(), a0);
        CHECK_EQ(r.seq.scene(), 2);
        r.seq.stop();
        r.seq.stop();   // STOP STOP with FOLLOW off keeps the selected scene
        CHECK_EQ(r.seq.scene(), 2);
        CHECK_EQ(r.seq.songSlot(), -1);
        // FOLLOW back on: selection ignored again (the song decides), PLAY starts slot 0
        p->song.follow = 1;
        r.seq.queueScene(1);
        CHECK_EQ(r.seq.scene(), 2);
        r.seq.play();
        CHECK_EQ(r.seq.songSlot(), 0);
        CHECK_EQ(r.seq.scene(), 0);
    }
    // playing: FOLLOW off during song slot 0 (scene A), select C -> launches at the quantization
    // point (or at the pass end of the song slot if that comes first), slot -1 from there on
    struct Case {
        LaunchQuant q;
        int len;
        uint64_t queue_at, want;
    };
    const Case cases[] = {
        {LaunchQuant::Bar, 16, 25000, 96000},   // bar = pass end
        {LaunchQuant::Beat, 16, 25000, 48000},  // beat, long before the pass end
        {LaunchQuant::Bar, 12, 25000, 72000},   // pass end (12 steps) before the next bar
        {LaunchQuant::Bar, 12, 71500, 72000},   // pass end already expanded (lookahead): rollback
        {LaunchQuant::Bar, 16, 95500, 96000},   // bar already expanded: rollback
    };
    for (const Case& c : cases) {
        for (int bs : {100, 64, 1}) {
            p->quant = c.q;
            p->scene_data[0].length = (uint8_t)c.len;
            p->song.follow = 1;
            Run r(p);
            r.seq.play();
            CHECK_EQ(r.seq.songSlot(), 0);
            r.runTo(c.queue_at, bs);
            p->song.follow = 0;
            r.seq.queueScene(2);
            CHECK_EQ(r.seq.queuedScene(), 2);
            r.runTo(c.want + 2 * kStep + 10, bs);
            const std::vector<uint64_t> a = r.frames(0), cc = r.frames(2);
            CHECK(!cc.empty() && cc[0] == c.want);
            CHECK(!a.empty() && a.back() == c.want - kStep);
            CHECK_EQ(cc.size(), 3u);
            CHECK(r.frames(1).empty());   // the song never reached slot 1
            CHECK_EQ(r.seq.scene(), 2);
            CHECK_EQ(r.seq.songSlot(), -1);
            CHECK_EQ(r.seq.queuedScene(), -1);
            if (cc.empty() || cc[0] != c.want)
                std::printf("  follow-off case quant=%d len=%d queue_at=%llu bs=%d\n", (int)c.q, c.len,
                            (unsigned long long)c.queue_at, bs);
        }
    }
    // playing: FOLLOW off, select C, then A again (the playing scene) cancels: A keeps looping
    {
        p->quant = LaunchQuant::Bar;
        p->scene_data[0].length = 16;
        p->song.follow = 1;
        Run r(p);
        r.seq.play();
        r.runTo(25000, 64);
        p->song.follow = 0;
        r.seq.queueScene(2);
        r.runTo(94000, 64);
        r.seq.queueScene(0);
        CHECK_EQ(r.seq.queuedScene(), -1);
        r.runTo(2 * kBar + 10, 64);
        CHECK(r.frames(2).empty() && r.frames(1).empty());
        CHECK_EQ(r.frames(0).size(), 33u);
        CHECK_EQ(r.seq.scene(), 0);
        CHECK_EQ(r.seq.songSlot(), -1);
    }
    // FOLLOW on with an empty song: nothing to follow, so selection works
    {
        for (int i = 0; i < kSongSlots; ++i) p->song.slot[i] = -1;
        p->song.follow = 1;
        p->quant = LaunchQuant::Beat;
        Run r(p);
        r.seq.play();
        r.runTo(25000, 64);
        r.seq.queueScene(2);
        CHECK_EQ(r.seq.queuedScene(), 2);
        r.runTo(60000, 64);
        CHECK(!r.frames(2).empty() && r.frames(2)[0] == 48000);
    }
    p->quant = LaunchQuant::Bar;
    delete p;
}

// beatsF() (audio task, float only) tracks beats() at any tempo, division and rate.
static void testBeatsF()
{
    Project* p = newProject();
    for (int s = 0; s < 16; ++s) on(*p, 0, 0, s);
    double worst = 0.0;
    for (int rate : {44100, 48000, 96000})
        for (float bpm : {30.0f, 87.3f, 120.0f, 300.0f})
            for (Division d : {Division::D1_8, Division::D1_16T, Division::D1_32}) {
                p->bpm = bpm;
                p->scene_data[0].div = d;
                Sequencer q;
                q.init(rate, p);
                CHECK(q.beatsF() == 0.0f);
                q.play();
                SeqTrigger t[256];
                SeqNotice no;
                for (int k = 0; k < 3000; ++k) {
                    q.process(97, t, 256, no);
                    const double e = std::fabs((double)q.beatsF() - q.beats());
                    if (e > worst) worst = e;
                }
            }
    CHECK(worst < 2e-3);
    std::printf("  beatsF vs beats: max error %.2e beats\n", worst);
    p->bpm = 120.0f;
    delete p;
}

static void testMuteSolo()
{
    Project* p = newProject();
    for (int t = 0; t < 3; ++t) on(*p, 0, t, 0);
    auto pads = [&]() {
        Run r(p);
        r.seq.play();
        r.runTo(kStep, 64);
        std::vector<uint64_t> v;
        for (const Ev& e : r.ev) v.push_back((uint64_t)e.pad);
        return v;
    };
    expectFrames(pads(), {0, 1, 2}, "all");
    p->pad[0].mute = 1;
    expectFrames(pads(), {1, 2}, "mute 0");
    p->pad[2].solo = 1;
    expectFrames(pads(), {2}, "solo 2");
    p->pad[0].solo = 1;   // muted and soloed: mute wins
    expectFrames(pads(), {2}, "solo 0+2, 0 muted");
    p->pad[0].mute = 0;
    expectFrames(pads(), {0, 2}, "solo 0+2");
    delete p;
}

static void testQuantize()
{
    Project* p = newProject();
    p->scene_data[0].swing = 66;   // quantize ignores swing
    p->scene_data[0].track[2].length = 5;   // polymeter track: loops every 5 steps, free across passes
    Run r(p);
    int sc = -1, st = -1, nu = 99;
    CHECK(!r.seq.quantize(0, sc, st, nu));
    CHECK(!r.seq.quantize(0, 2, sc, st, nu));
    r.seq.play();
    r.runTo(25000, 1000);
    r.block(1000);   // [25000, 26000)
    CHECK(r.seq.quantize(500, sc, st, nu));
    CHECK(sc == 0 && st == 4 && nu == 25);
    r.runTo(29000, 1000);
    r.block(1000);   // [29000, 30000)
    CHECK(r.seq.quantize(400, sc, st, nu));
    CHECK(sc == 0 && st == 5 && nu == -10);
    CHECK(r.seq.quantize(0, sc, st, nu));
    CHECK(sc == 0 && st == 5 && nu == -17);
    CHECK(r.seq.quantize(999, sc, st, nu));
    CHECK(sc == 0 && st == 5 && nu == 0);
    // per pad: the track step that track plays there (logic-5); a scene-length track = the scene step
    CHECK(r.seq.quantize(400, 2, sc, st, nu));
    CHECK(sc == 0 && st == 0 && nu == -10);   // scene step 5 = track step 0 of the 5-step track
    CHECK(r.seq.quantize(400, 3, sc, st, nu));
    CHECK(sc == 0 && st == 5 && nu == -10);
    r.runTo(95000, 1000);
    r.block(1000);   // [95000, 96000): late hits go to step 0 of the next pass
    CHECK(r.seq.quantize(0, sc, st, nu));
    CHECK(sc == 0 && st == 0 && nu == -17);
    CHECK(r.seq.quantize(990, sc, st, nu));   // 95990
    CHECK(sc == 0 && st == 0 && nu == 0);
    CHECK(r.seq.quantize(990, 2, sc, st, nu));   // scene step 16 = track step 16 % 5 = 1 (not 0 % 5)
    CHECK(sc == 0 && st == 1 && nu == 0);
    CHECK(r.seq.quantize(990, 0, sc, st, nu));
    CHECK(sc == 0 && st == 0 && nu == 0);
    r.seq.stop();
    CHECK(!r.seq.quantize(0, sc, st, nu));
    delete p;
}

static void testTransport()
{
    Project* p = newProject();
    for (int s = 0; s < 16; s += 4) on(*p, 0, 0, s);
    Run r(p);
    CHECK(!r.seq.playing());
    CHECK_EQ(r.seq.step(), -1);
    r.seq.play();
    r.runTo(30100, 100);
    CHECK_EQ(r.seq.step(), 5);
    r.seq.stop();
    CHECK(!r.seq.playing());
    CHECK_EQ(r.seq.step(), -1);
    r.runTo(40100, 100);
    CHECK_EQ(r.transport_marks.size(), 2u);
    CHECK_EQ(r.ev.size(), 2u);   // 0, 24000
    r.seq.resume();              // continues at frame 30100 of the pattern
    r.runTo(70000, 100);
    expectFrames(r.frames(0), {0, 24000, 58000}, "resume");
    r.seq.stop();
    r.seq.stop();                // already stopped: rewind
    r.runTo(71000, 100);
    CHECK_EQ(r.seq.step(), -1);
    r.seq.resume();
    r.runTo(72000, 100);
    CHECK_EQ(r.frames(0).back(), 71000u);   // step 0 again
    CHECK_EQ(r.seq.step(), 0);
    r.seq.locate(8);
    r.runTo(73000, 100);
    CHECK_EQ(r.frames(0).back(), 72000u);
    CHECK_EQ(r.seq.step(), 8);
    r.seq.play();                // play while playing restarts
    r.runTo(74000, 100);
    CHECK_EQ(r.frames(0).back(), 73000u);
    CHECK_EQ(r.seq.step(), 0);
    CHECK(r.transport_marks.size() >= 6);
    // beats(): quarter notes since play()
    r.runTo(73000 + 48000, 100);
    CHECK(r.seq.beats() > 1.999 && r.seq.beats() < 2.001);
    CHECK(r.seq.beatsF() > 1.999f && r.seq.beatsF() < 2.001f);
    delete p;
}

static void testMetronome()
{
    Project* p = newProject();
    for (int bs : kBlockSizes) {
        Run r(p);
        r.seq.play();
        r.runTo(4 * kBar, bs);
        CHECK_EQ(r.clicks.size(), 16u);
        bool ok = r.clicks.size() == 16;
        for (size_t i = 0; ok && i < r.clicks.size(); ++i)
            ok = r.clicks[i].frame == i * 24000 && r.clicks[i].accent == (i % 4 == 0);
        CHECK(ok);
    }
    // triplet grid (1/8T = 8000 frames): still one click per quarter
    p->scene_data[0].div = Division::D1_8T;
    {
        Run r(p);
        r.seq.play();
        r.runTo(96000, 97);
        CHECK_EQ(r.clicks.size(), 4u);
        for (size_t i = 0; i < r.clicks.size(); ++i) CHECK_EQ(r.clicks[i].frame, i * 24000);
    }
    // 3/4: accent every third click
    p->scene_data[0].div = Division::D1_16;
    p->beats_per_bar = 3;
    {
        Run r(p);
        r.seq.play();
        r.runTo(6 * 24000, 128);
        CHECK_EQ(r.clicks.size(), 6u);
        for (size_t i = 0; i < r.clicks.size(); ++i) CHECK(r.clicks[i].accent == (i % 3 == 0));
    }
    delete p;
}

static void testOutputLimit()
{
    // more due hits than the caller accepts: the rest fires in the next block, nothing is lost
    Project* p = newProject();
    for (int t = 0; t < kPads; ++t) on(*p, 0, t, 0);
    Run r(p);
    r.max_out = 5;
    r.seq.play();
    r.runTo(kStep, 64);
    CHECK_EQ(r.ev.size(), 16u);
    bool ok = true;
    for (int i = 0; i < 16; ++i) ok = ok && r.ev[i].pad == i;
    CHECK(ok);
    delete p;
}

int main()
{
    testBasicOffsets();
    testIdenticalTimelines();
    testSwing();
    testNudge();
    testRatchetFlam();
    testProbability();
    testTrigConditions();
    testPolymeter();
    testTempoChange();
    testSceneSwitch();
    testSong();
    testFollowOff();
    testBeatsF();
    testMuteSolo();
    testQuantize();
    testTransport();
    testMetronome();
    testOutputLimit();
    if (fails) {
        std::printf("test_seq: %d checks FAILED\n", fails);
        return 1;
    }
    std::printf("test_seq: all checks passed\n");
    return 0;
}
