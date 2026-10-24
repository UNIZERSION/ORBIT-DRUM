// Slicing: equal grid and transient (onset) detection. Loader/UI task only.
//
// Onsets: spectral flux (Dixon 2006 / Bello 2005 / Boeck's log-filtered variant). N = 1024
// Hann frames every 256 samples (own radix-2 real FFT), magnitudes averaged in log-spaced bands
// (6 per octave, 30 Hz..17 kHz) and log compressed, half-wave rectified frame-to-frame increase
// summed over bands, normalized to mean 0 / std 1, then Dixon's three peak-picking rules (local
// max over +-w, above the local mean + delta, above a decaying threshold); delta follows the
// sensitivity. Peaks closer than 40 ms are merged (strongest wins). Every peak is then placed
// in the time domain: the block where plain or high-passed energy jumps most, the first sample
// of the attack, moved back to the nearest preceding zero crossing.
#include <math.h>
#include <string.h>

#include "platform.h"
#include "sampleio.h"

namespace orbit {

namespace {

constexpr int kN = 1024;          // FFT size
constexpr int kM = kN / 2;        // complex FFT size used for the real transform
constexpr int kHop = 256;
constexpr int kBins = kN / 2 + 1;
constexpr int kW = 3;             // peak picking: local max window (frames)
constexpr int kMean = 3;          // peak picking: mean window multiplier (m)
constexpr float kAlpha = 0.9f;    // decaying threshold
constexpr float kLambda = 10.0f;  // log compression log(1 + lambda * |X|)
constexpr float kMinGapS = 0.040f;
constexpr int kBlk = 64;          // refinement block (samples)

constexpr int kBandsPerOct = 6;
constexpr int kMaxBands = 96;
constexpr float kBandLo = 30.0f, kBandHi = 17000.0f;

struct Work {
    float hann[kN];
    float cs[kM + 1], sn[kM + 1];   // e^{-2 pi i k / N} for k = 0..N/2
    uint16_t rev[kM];
    float re[kM], im[kM];
    float cur[kBins];
    // Log-frequency bands (bins [edge[b], edge[b+1])): a bass drum lives in 3-4 FFT bins and
    // would vanish against a hi-hat's 400 bins in a plain per-bin flux sum.
    uint16_t edge[kMaxBands + 1];
    int nb;
    float band[kMaxBands], prevb[kMaxBands];
};

void initBands(Work& w, uint32_t rate)
{
    const float bin_hz = (float)rate / (float)kN;
    const float top = fminf(kBandHi, 0.5f * (float)rate);
    w.nb = 0;
    int last = -1;
    for (int k = 0; w.nb < kMaxBands; ++k) {
        const float f = kBandLo * exp2f((float)k / (float)kBandsPerOct);
        if (f > top) break;
        int b = (int)(f / bin_hz + 0.5f);
        if (b < 1) b = 1;
        if (b > kM) b = kM;
        if (b == last) continue;   // several low bands share one bin: merged
        w.edge[w.nb++] = (uint16_t)b;
        last = b;
    }
    int end = (int)(top / bin_hz + 0.5f) + 1;
    if (end > kBins) end = kBins;
    if (w.nb == 0 || end <= w.edge[w.nb - 1]) end = (w.nb ? w.edge[w.nb - 1] : 1) + 1;
    if (w.nb == 0) w.edge[w.nb++] = 1;
    w.edge[w.nb] = (uint16_t)end;
}

void initWork(Work& w, uint32_t rate)
{
    initBands(w, rate);
    const float pi = 3.14159265358979f;
    for (int i = 0; i < kN; ++i) w.hann[i] = 0.5f - 0.5f * cosf(2.0f * pi * (float)i / (float)kN);
    for (int k = 0; k <= kM; ++k) {
        w.cs[k] = cosf(2.0f * pi * (float)k / (float)kN);
        w.sn[k] = -sinf(2.0f * pi * (float)k / (float)kN);
    }
    int bits = 0;
    while ((1 << bits) < kM) ++bits;
    for (int i = 0; i < kM; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        w.rev[i] = (uint16_t)r;
    }
}

// In-place radix-2 DIT complex FFT of size kM. Twiddle for size kM at k is W_N^(2k).
void fftM(Work& w)
{
    for (int i = 0; i < kM; ++i) {
        const int j = w.rev[i];
        if (j > i) {
            float t = w.re[i]; w.re[i] = w.re[j]; w.re[j] = t;
            t = w.im[i]; w.im[i] = w.im[j]; w.im[j] = t;
        }
    }
    for (int len = 2; len <= kM; len <<= 1) {
        const int half = len >> 1, tstep = (kM / len) * 2;
        for (int i = 0; i < kM; i += len) {
            for (int k = 0; k < half; ++k) {
                const float wr = w.cs[k * tstep], wi = w.sn[k * tstep];
                const int a = i + k, b = a + half;
                const float vr = w.re[b] * wr - w.im[b] * wi;
                const float vi = w.re[b] * wi + w.im[b] * wr;
                w.re[b] = w.re[a] - vr;
                w.im[b] = w.im[a] - vi;
                w.re[a] += vr;
                w.im[a] += vi;
            }
        }
    }
}

// Magnitude spectrum of a real frame x[0..kN) (packed as kM complex values), into w.cur.
void realMagnitudes(Work& w, const float* x)
{
    for (int n = 0; n < kM; ++n) {
        w.re[n] = x[2 * n];
        w.im[n] = x[2 * n + 1];
    }
    fftM(w);
    for (int k = 0; k <= kM; ++k) {
        const int a = k % kM, b = (kM - k) % kM;
        const float zr = w.re[a], zi = w.im[a];
        const float cr = w.re[b], ci = -w.im[b];          // conj(Z[M-k])
        const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);   // even samples
        const float dr = zr - cr, di = zi - ci;
        const float orr = 0.5f * di, oi = -0.5f * dr;     // odd samples: (Zk - Zc) / 2i
        const float xr = er + orr * w.cs[k] - oi * w.sn[k];
        const float xi = ei + orr * w.sn[k] + oi * w.cs[k];
        w.cur[k] = sqrtf(xr * xr + xi * xi);
    }
}

inline float monoAt(const Sample* s, int64_t i)
{
    if (s->channels == 1) return (float)s->data[i] * (1.0f / 32768.0f);
    return ((float)s->data[2 * i] + (float)s->data[2 * i + 1]) * (0.5f / 32768.0f);
}

// |x| (plain) or |x[i] - x[i-1]| (high-passed) at sample i >= 1.
inline float level(const Sample* s, int64_t i, bool hp)
{
    return hp ? fabsf(monoAt(s, i) - monoAt(s, i - 1)) : fabsf(monoAt(s, i));
}

// Precise attack position near a spectral-flux peak whose frame is centred at `center`.
// The block where energy jumps most (ratio of the next 4 blocks to the previous 4) is found
// both in the plain signal (a kick on a noisy tail) and in its first difference (a hat on a
// loud kick tail, whose low frequencies barely show after high-passing).
int64_t refine(const Sample* s, int64_t a, int64_t b, int64_t center)
{
    constexpr int kPre = 4;   // blocks compared before / after a candidate
    int64_t lo = center - kN / 2 - kHop, hi = center + kN / 2 + kHop;
    if (lo < a) lo = a;
    if (hi > b) hi = b;
    // The "before" blocks of the first candidates come from real audio ahead of the window
    // (the sample may continue before the region), never from an empty history.
    int64_t base0 = lo - kPre * kBlk;
    if (base0 < 1) base0 = 1;
    constexpr int kMaxBlk = (kN + 2 * kHop) / kBlk + kPre + 2;
    int n = (int)((hi - base0) / kBlk);
    if (n > kMaxBlk) n = kMaxBlk;
    const int j0 = (int)((lo - base0 + kBlk - 1) / kBlk);
    if (n - j0 < 2) return center < a ? a : (center >= b ? b - 1 : center);
    float ep[kMaxBlk], eh[kMaxBlk];
    float mp = 0.0f, mh = 0.0f;
    for (int j = 0; j < n; ++j) {
        float accp = 0.0f, acch = 0.0f;
        const int64_t base = base0 + (int64_t)j * kBlk;
        float prev = monoAt(s, base - 1);
        for (int k = 0; k < kBlk; ++k) {
            const float x = monoAt(s, base + k);
            const float d = x - prev;
            prev = x;
            accp += x * x;
            acch += d * d;
        }
        ep[j] = accp;
        eh[j] = acch;
        mp = fmaxf(mp, accp);
        mh = fmaxf(mh, acch);
    }
    if (mp <= 0.0f) return center < a ? a : (center >= b ? b - 1 : center);
    int best = j0;
    bool hp = false;
    float best_score = -1.0f;
    for (int pass = 0; pass < 2; ++pass) {
        const float* e = pass ? eh : ep;
        const float eps = (pass ? mh : mp) * 1.0e-3f + 1.0e-12f;
        for (int j = j0; j < n; ++j) {
            float pre = 0.0f, post = 0.0f;
            for (int k = 1; k <= kPre; ++k) pre += j - k >= 0 ? e[j - k] : 0.0f;
            for (int k = 0; k < kPre; ++k) post += j + k < n ? e[j + k] : 0.0f;
            const float score = post / (pre + eps);
            if (score > best_score) {
                best_score = score;
                best = j;
                hp = pass == 1;
            }
        }
    }
    // First sample reaching 25 % of the level peak after the jump.
    const int64_t s0 = base0 + (int64_t)best * kBlk;
    int64_t from = s0 - kBlk, to = s0 + kPre * kBlk;
    if (from < base0) from = base0;
    if (to > hi) to = hi;
    float peak = 0.0f;
    for (int64_t i = s0; i < to; ++i) peak = fmaxf(peak, level(s, i, hp));
    int64_t at = s0;
    for (int64_t i = from; i < to; ++i) {
        if (level(s, i, hp) >= 0.25f * peak) {
            at = hp ? i - 1 : i;
            break;
        }
    }
    // Back to the nearest preceding zero crossing / silent sample (max 2 ms).
    const int64_t lim = at - (int64_t)(s->rate / 500) > a ? at - (int64_t)(s->rate / 500) : a;
    for (int64_t i = at; i > lim; --i) {
        const float x0 = monoAt(s, i - 1), x1 = monoAt(s, i);
        if (x1 == 0.0f) {
            at = i;
            break;
        }
        if (x0 == 0.0f || (x0 < 0.0f) != (x1 < 0.0f)) {
            at = fabsf(x0) <= fabsf(x1) ? i - 1 : i;
            break;
        }
    }
    return at;
}

}  // namespace

int sliceGrid(float start, float end, int count, float* positions)
{
    if (!positions || count < 1) return 0;
    if (count > kMaxSlices) count = kMaxSlices;
    if (!(start >= 0.0f)) start = 0.0f;
    if (start > 1.0f) start = 1.0f;
    if (!(end <= 1.0f)) end = 1.0f;
    if (end < 0.0f) end = 0.0f;
    if (end <= start) return 0;
    const float len = end - start;
    for (int i = 0; i < count; ++i) positions[i] = start + len * (float)i / (float)count;
    return count;
}

int sliceTransients(const Sample* s, float start, float end, float sensitivity, int max, float* positions)
{
    if (!positions || max < 1) return 0;
    if (!(start >= 0.0f)) start = 0.0f;
    if (start > 1.0f) start = 1.0f;
    if (!(end <= 1.0f)) end = 1.0f;
    if (end <= start) return 0;
    positions[0] = start;
    if (!s || !s->data || s->frames < 2 || s->rate == 0 || max == 1) return 1;
    if (!(sensitivity >= 0.0f)) sensitivity = 0.0f;
    if (sensitivity > 1.0f) sensitivity = 1.0f;

    const int64_t frames = s->frames;
    int64_t a = (int64_t)(start * (float)frames), b = (int64_t)(end * (float)frames + 0.5f);
    if (b > frames) b = frames;
    if (a < 0) a = 0;
    if (b - a < kN) return 1;
    const int nf = (int)((b - a) / kHop) + 1;

    Work* w = static_cast<Work*>(fastAlloc(sizeof(Work)));
    bool w_big = false;
    if (!w) {
        w = static_cast<Work*>(bigAlloc(sizeof(Work)));
        w_big = true;
    }
    float* odf = static_cast<float*>(bigAlloc(sizeof(float) * (size_t)nf * 2 + sizeof(float) * kN));
    if (!w || !odf) {
        if (w) (w_big ? bigFree(w) : fastFree(w));
        bigFree(odf);
        return 1;
    }
    float* gate = odf + nf;      // decaying threshold per frame (scratch)
    float* frame = gate + nf;    // windowed frame, kN floats
    initWork(*w, s->rate);

    // 1. Spectral flux. Frame f is centred at a + f*hop; samples outside [a, b) count as 0.
    // This runs on the loader task (core 0, priority 2) and takes seconds on long samples: a 1 ms
    // sleep every ~20 ms of work lets IDLE0 run, so the task watchdog stays quiet.
    uint64_t yield_us = nowUs();
    for (int f = 0; f < nf; ++f) {
        if ((f & 15) == 15 && nowUs() - yield_us > 20000) {
            sleepMs(1);
            yield_us = nowUs();
        }
        const int64_t c = a + (int64_t)f * kHop;
        for (int i = 0; i < kN; ++i) {
            const int64_t t = c - kN / 2 + i;
            frame[i] = (t >= a && t < b) ? monoAt(s, t) * w->hann[i] : 0.0f;
        }
        realMagnitudes(*w, frame);
        for (int bd = 0; bd < w->nb; ++bd) {   // mean magnitude per band, log compressed
            float sum = 0.0f;
            const int k0 = w->edge[bd], k1 = w->edge[bd + 1];
            for (int k = k0; k < k1; ++k) sum += w->cur[k];
            w->band[bd] = log1pf(kLambda * sum / (float)(k1 - k0));
        }
        float flux = 0.0f;
        if (f > 0)
            for (int bd = 0; bd < w->nb; ++bd) {
                const float d = w->band[bd] - w->prevb[bd];
                if (d > 0.0f) flux += d;   // half-wave rectified
            }
        odf[f] = flux;
        memcpy(w->prevb, w->band, sizeof(float) * (size_t)w->nb);
    }

    // 2. Normalize to mean 0, std 1.
    float mean = 0.0f;
    for (int f = 0; f < nf; ++f) mean += odf[f];
    mean /= (float)nf;
    float var = 0.0f;
    for (int f = 0; f < nf; ++f) var += (odf[f] - mean) * (odf[f] - mean);
    const float sd = sqrtf(var / (float)nf);
    int count = 1;
    if (sd > 1.0e-6f) {
        for (int f = 0; f < nf; ++f) odf[f] = (odf[f] - mean) / sd;

        // 3. Dixon peak picking. delta: LOW sensitivity 1.5 .. HIGH 0.1.
        const float delta = 0.1f + (1.0f - sensitivity) * 1.4f;
        float g = odf[0];
        for (int f = 0; f < nf; ++f) {
            gate[f] = g;   // g_alpha(f - 1)
            g = fmaxf(odf[f], kAlpha * g + (1.0f - kAlpha) * odf[f]);
        }
        // Candidates: (position, strength), strongest kept first.
        constexpr int kMaxCand = 256;
        int64_t cpos[kMaxCand];
        float cstr[kMaxCand];
        int nc = 0;
        for (int f = 1; f < nf && nc < kMaxCand; ++f) {
            const float v = odf[f];
            bool peak = true;
            for (int k = f - kW; k <= f + kW && peak; ++k)
                if (k >= 0 && k < nf && k != f && (odf[k] > v || (odf[k] == v && k < f))) peak = false;
            if (!peak) continue;
            float m = 0.0f;
            int mc = 0;
            for (int k = f - kMean * kW; k <= f + kW; ++k)
                if (k >= 0 && k < nf) {
                    m += odf[k];
                    ++mc;
                }
            if (v < m / (float)mc + delta) continue;
            if (v < gate[f]) continue;
            cpos[nc] = refine(s, a, b, a + (int64_t)f * kHop);
            cstr[nc] = v;
            ++nc;
        }
        // 4. Strongest first, minimum gap to everything accepted (the region start included).
        const int64_t gap = (int64_t)(kMinGapS * (float)s->rate);
        int64_t acc[kMaxSlices];
        int na = 0;
        acc[na++] = a;
        const int want = max < kMaxSlices ? max : kMaxSlices;
        while (na < want) {
            int bi = -1;
            for (int i = 0; i < nc; ++i)
                if (cstr[i] > -1.0e30f && (bi < 0 || cstr[i] > cstr[bi])) bi = i;
            if (bi < 0) break;
            const int64_t p = cpos[bi];
            cstr[bi] = -1.0e31f;
            bool ok = p > a && p < b;
            for (int i = 0; i < na && ok; ++i) {
                const int64_t d = p > acc[i] ? p - acc[i] : acc[i] - p;
                if (d < gap) ok = false;
            }
            if (ok) acc[na++] = p;
        }
        // Ascending order (insertion sort, n <= 16).
        for (int i = 1; i < na; ++i) {
            const int64_t x = acc[i];
            int j = i - 1;
            while (j >= 0 && acc[j] > x) {
                acc[j + 1] = acc[j];
                --j;
            }
            acc[j + 1] = x;
        }
        for (int i = 1; i < na; ++i) positions[i] = (float)acc[i] / (float)frames;
        count = na;
    }
    if (w_big) bigFree(w);
    else fastFree(w);
    bigFree(odf);
    return count;
}

}  // namespace orbit
