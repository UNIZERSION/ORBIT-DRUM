#include "fxcheck.h"

#include <math.h>
#include <string.h>
#include <new>

#include "fx.h"
#include "platform.h"

namespace orbit {

namespace {

struct Lcg {
    uint32_t s = 0x1234567u;
    uint32_t next()
    {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    float uni() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }   // 0..1
    float bi() { return uni() * 2.0f - 1.0f; }
};

bool sameBits(float a, float b)
{
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return x == y;
}

}  // namespace

FxCheck fxSelfCheck(int rate, int blocks)
{
    FxCheck r;
    constexpr int32_t kFrames = 65536;   // 1.4 s at 48 kHz: the check's delay times stay below
    float* dm[4] = {};
    bool ok = true;
    for (auto& p : dm) {
        p = static_cast<float*>(bigAlloc((size_t)kFrames * sizeof(float)));
        ok = ok && p;
    }
    // the units and block buffers on the heap too (the test console's task has a 6 KB stack)
    struct Bufs {
        float in_l[kRenderBlock], in_r[kRenderBlock];
        float ol[2][kRenderBlock], orr[2][kRenderBlock];
    };
    StereoDelay* dly = ok ? static_cast<StereoDelay*>(bigAlloc(2 * sizeof(StereoDelay))) : nullptr;
    Bufs* bb = ok ? static_cast<Bufs*>(bigAlloc(sizeof(Bufs))) : nullptr;
    if (ok && dly && bb) {
        for (int u = 0; u < 2; ++u) {
            new (&dly[u]) StereoDelay();
            dly[u].init(dm[2 * u], dm[2 * u + 1], kFrames, rate);
        }
        dly[0].setReference(true);
        r.ran = true;
        static const int kLens[9] = {64, 64, 128, 64, 37, 1, 128, 64, 100};
        Lcg g;
        double e = 0.0;
        // parameters, moved every 120 blocks (delay times cross-fade; some are shorter than a block: fallback path)
        float dsamp = 0.375f * (float)rate, fb = 0.4f, tone = 0.6f;
        bool pp = true;
        int64_t pos = 0;
        for (int b = 0; b < blocks; ++b) {
            const int n = kLens[b % 9];
            if (b % 120 == 0 && b > 0) {
                switch ((b / 120) % 4) {
                case 0: dsamp = 2000.0f + 50000.0f * g.uni(); fb = 0.95f * g.uni(); break;
                case 1: tone = g.uni(); pp = !pp; break;
                case 2: fb = 0.95f * g.uni(); break;
                default: dsamp = (float)(1 + (int)(g.uni() * 90.0f)); break;
                }
            }
            // input: noise bursts (40 ms on, 250 ms off) plus single clicks
            for (int i = 0; i < n; ++i) {
                const int64_t t = pos + i;
                const bool on = (t % (int64_t)(rate * 0.29)) < (int64_t)(rate * 0.04);
                bb->in_l[i] = on ? 0.5f * g.bi() : 0.0f;
                bb->in_r[i] = on ? 0.5f * g.bi() : 0.0f;
                if (t % 7919 == 0) bb->in_l[i] += 0.9f;
            }
            pos += n;
            for (int u = 0; u < 2; ++u) {
                dly[u].setParams(dsamp, fb, tone, pp);
                dly[u].process(bb->in_l, bb->in_r, bb->ol[u], bb->orr[u], n);
            }
            r.delay_block += dly[1].usedBlockPath() ? 1 : 0;
            for (int i = 0; i < n; ++i) {
                if (!sameBits(bb->ol[0][i], bb->ol[1][i])) ++r.delay_diff;
                if (!sameBits(bb->orr[0][i], bb->orr[1][i])) ++r.delay_diff;
                e += (double)bb->ol[0][i] * bb->ol[0][i] + (double)bb->orr[0][i] * bb->orr[0][i];
            }
            r.samples += 2 * n;
            ++r.blocks;
#ifdef ESP_PLATFORM
            if ((b & 15) == 15) sleepMs(1);
#endif
        }
        if (r.samples > 0) r.delay_rms = sqrt(e / (double)r.samples);
        for (int u = 0; u < 2; ++u) dly[u].~StereoDelay();
    }
    bigFree(bb);
    bigFree(dly);
    for (auto* p : dm) bigFree(p);
    return r;
}

}  // namespace orbit
