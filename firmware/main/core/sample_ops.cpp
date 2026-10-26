// Destructive sample operations (copy-on-write). Loader task only: every op allocates a new
// Sample and leaves the input untouched, so the engine can keep playing the old buffer.
#include <math.h>
#include <string.h>

#include "platform.h"
#include "sampleio.h"

namespace orbit {

namespace {

inline int16_t sat16f(float v)
{
    if (!(v == v)) return 0;
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)(v >= 0.0f ? (int32_t)(v + 0.5f) : -(int32_t)(-v + 0.5f));
}

inline int16_t sat16i(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

inline float clamp01(float x) { return !(x >= 0.0f) ? 0.0f : (x > 1.0f ? 1.0f : x); }

bool valid(const Sample* s) { return s && s->data && s->frames > 0 && (s->channels == 1 || s->channels == 2); }

// 0.4.9: a loop over a whole sample paces its PSRAM traffic while the audio plays (platform.h bulkPace):
// at(i) inside the loop counts kPaceItems items at a time.
struct Pace {
    static constexpr size_t kItems = 4096;
    BulkPacer p;
    size_t bytes_per_item, next = kItems;
    explicit Pace(size_t bpi) : bytes_per_item(bpi) {}
    void at(size_t i)
    {
        if (i < next) return;
        bulkPace(p, kItems * bytes_per_item);
        next += kItems;
    }
};

// New sample that inherits the display name. path is cleared: an edited sample is unsaved.
Sample* derive(const Sample* s, uint32_t frames, int channels, uint32_t rate)
{
    Sample* r = sampleCreate(frames, channels, rate);
    if (!r) return nullptr;
    memcpy(r->name, s->name, sizeof(r->name));
    r->path[0] = 0;
    return r;
}

Sample* finish(Sample* r)
{
    if (r) sampleAnalyze(r);
    return r;
}

uint32_t fracToFrame(float x, uint32_t frames)
{
    const float f = clamp01(x) * (float)frames + 0.5f;
    const uint32_t i = f >= (float)frames ? frames : (uint32_t)f;
    return i > frames ? frames : i;
}

// Frames [a, b) as a new sample.
Sample* cropFrames(const Sample* s, uint32_t a, uint32_t b)
{
    if (b > s->frames) b = s->frames;
    if (b <= a) return nullptr;
    Sample* r = derive(s, b - a, s->channels, s->rate);
    if (!r) return nullptr;
    bulkCopy(r->data, s->data + (size_t)a * s->channels, (size_t)(b - a) * s->channels * sizeof(int16_t));
    return finish(r);
}

// ------------------------------------------------------------ windowed-sinc resampler
// Kaiser-windowed sinc, kZc zero crossings per side, table with kRes points per zero crossing
// and linear interpolation between them (~-90 dB images, offline quality).
constexpr int kZc = 16;
constexpr int kRes = 512;
constexpr int kTable = kZc * kRes + 2;
constexpr float kBeta = 8.6f;      // ~86 dB stopband
constexpr float kPassband = 0.95f; // cutoff relative to the lower Nyquist

float besselI0(float x)
{
    // Power series; converges quickly for the beta used here.
    float sum = 1.0f, term = 1.0f;
    const float q = x * x * 0.25f;
    for (int k = 1; k < 64; ++k) {
        term *= q / (float)(k * k);
        sum += term;
        if (term < sum * 1.0e-9f) break;
    }
    return sum;
}

float* buildSincTable()
{
    float* t = static_cast<float*>(bigAlloc(sizeof(float) * kTable));
    if (!t) return nullptr;
    const float inv_i0 = 1.0f / besselI0(kBeta);
    const float pi = 3.14159265358979f;
    for (int i = 0; i <= kZc * kRes; ++i) {
        const float x = (float)i / (float)kRes;   // in zero crossings
        const float r = x / (float)kZc;
        const float w = besselI0(kBeta * sqrtf(fmaxf(0.0f, 1.0f - r * r))) * inv_i0;
        const float sinc = i == 0 ? 1.0f : sinf(pi * x) / (pi * x);
        t[i] = sinc * w;
    }
    t[kZc * kRes + 1] = 0.0f;
    return t;
}

// Resamples s into out_frames frames. step = input frames per output frame (32.32 fixed point).
Sample* resampleCore(const Sample* s, uint64_t step_fx, uint32_t out_frames, uint32_t out_rate)
{
    if (out_frames == 0 || step_fx == 0) return nullptr;
    float* tab = buildSincTable();
    if (!tab) return nullptr;
    Sample* r = derive(s, out_frames, s->channels, out_rate);
    if (!r) {
        bigFree(tab);
        return nullptr;
    }
    const float step = (float)step_fx * (1.0f / 4294967296.0f);
    // Anti-aliasing: when reading faster than 1 input frame per output frame, lower the cutoff.
    const float fc = (step > 1.0f ? 1.0f / step : 1.0f) * kPassband;
    const float half = (float)kZc / fc;   // kernel half width in input frames
    const float scale = fc * (float)kRes;
    const int64_t n_in = s->frames;
    const int ch = s->channels;
    const int16_t* in = s->data;
    const int kLast = kZc * kRes;   // table index limit (exclusive)
    const int ihalf = (int)half + 1;
    uint64_t pos = 0;
    Pace pace((size_t)ch * 4u);
    for (uint32_t o = 0; o < out_frames; ++o, pos += step_fx) {
        pace.at(o);
        const int64_t ci = (int64_t)(pos >> 32);
        const float cf = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
        int64_t j0 = ci - ihalf, j1 = ci + ihalf;
        if (j0 < 0) j0 = 0;
        if (j1 > n_in - 1) j1 = n_in - 1;
        // Kernel position of tap j in table units, stepped by `scale` per tap.
        float x = ((float)(j0 - ci) - cf) * scale;
        float acc0 = 0.0f, acc1 = 0.0f;
        if (ch == 1) {
            for (int64_t j = j0; j <= j1; ++j, x += scale) {
                const float ax = fabsf(x);
                const int k = (int)ax;
                if (k >= kLast) continue;
                const float w = tab[k] + (ax - (float)k) * (tab[k + 1] - tab[k]);
                acc0 += w * (float)in[j];
            }
            r->data[o] = sat16f(acc0 * fc);
        } else {
            for (int64_t j = j0; j <= j1; ++j, x += scale) {
                const float ax = fabsf(x);
                const int k = (int)ax;
                if (k >= kLast) continue;
                const float w = tab[k] + (ax - (float)k) * (tab[k + 1] - tab[k]);
                acc0 += w * (float)in[2 * j];
                acc1 += w * (float)in[2 * j + 1];
            }
            r->data[2 * o] = sat16f(acc0 * fc);
            r->data[2 * o + 1] = sat16f(acc1 * fc);
        }
    }
    bigFree(tab);
    return finish(r);
}

}  // namespace

Sample* opCrop(const Sample* s, float start, float end)
{
    if (!valid(s)) return nullptr;
    uint32_t a = fracToFrame(start, s->frames), b = fracToFrame(end, s->frames);
    if (b < a) {
        const uint32_t t = a;
        a = b;
        b = t;
    }
    return cropFrames(s, a, b);
}

Sample* opNormalize(const Sample* s, float target_db)
{
    if (!valid(s)) return nullptr;
    const size_t n = (size_t)s->frames * s->channels;
    int peak = 0;
    Pace scan(2);
    for (size_t i = 0; i < n; ++i) {
        scan.at(i);
        const int v = s->data[i] < 0 ? -(int)s->data[i] : (int)s->data[i];
        if (v > peak) peak = v;
    }
    Sample* r = derive(s, s->frames, s->channels, s->rate);
    if (!r) return nullptr;
    if (peak == 0) {   // silence: nothing to scale
        bulkCopy(r->data, s->data, n * sizeof(int16_t));
        return finish(r);
    }
    if (target_db > 0.0f) target_db = 0.0f;
    const float target = powf(10.0f, target_db * 0.05f) * 32768.0f;
    const float g = target / (float)peak;
    Pace pace(4);
    for (size_t i = 0; i < n; ++i) {
        pace.at(i);
        r->data[i] = sat16f((float)s->data[i] * g);
    }
    return finish(r);
}

Sample* opReverse(const Sample* s)
{
    if (!valid(s)) return nullptr;
    Sample* r = derive(s, s->frames, s->channels, s->rate);
    if (!r) return nullptr;
    const uint32_t n = s->frames;
    Pace pace((size_t)s->channels * 4u);
    if (s->channels == 1) {
        for (uint32_t i = 0; i < n; ++i) {
            pace.at(i);
            r->data[i] = s->data[n - 1 - i];
        }
    } else {
        for (uint32_t i = 0; i < n; ++i) {
            pace.at(i);
            r->data[2 * i] = s->data[2 * (n - 1 - i)];
            r->data[2 * i + 1] = s->data[2 * (n - 1 - i) + 1];
        }
    }
    return finish(r);
}

Sample* opFade(const Sample* s, float fade_in_ms, float fade_out_ms)
{
    if (!valid(s)) return nullptr;
    const uint32_t n = s->frames;
    const int ch = s->channels;
    if (!(fade_in_ms > 0.0f)) fade_in_ms = 0.0f;
    if (!(fade_out_ms > 0.0f)) fade_out_ms = 0.0f;
    float fi = fade_in_ms * 0.001f * (float)s->rate;
    float fo = fade_out_ms * 0.001f * (float)s->rate;
    if (fi + fo > (float)n) {   // fades longer than the sample: shrink both proportionally
        const float k = (float)n / (fi + fo);
        fi *= k;
        fo *= k;
    }
    const uint32_t nin = (uint32_t)fi, nout = (uint32_t)fo;
    Sample* r = derive(s, n, ch, s->rate);
    if (!r) return nullptr;
    bulkCopy(r->data, s->data, (size_t)n * ch * sizeof(int16_t));
    // Linear in power: gain = sqrt(t), so the energy ramps linearly.
    for (uint32_t i = 0; i < nin; ++i) {
        const float g = sqrtf((float)i / (float)nin);
        for (int c = 0; c < ch; ++c) r->data[(size_t)i * ch + c] = sat16f((float)r->data[(size_t)i * ch + c] * g);
    }
    for (uint32_t k = 0; k < nout; ++k) {   // k = frames before the end (last frame -> 0)
        const uint32_t i = n - 1 - k;
        const float g = sqrtf((float)k / (float)nout);
        for (int c = 0; c < ch; ++c) r->data[(size_t)i * ch + c] = sat16f((float)r->data[(size_t)i * ch + c] * g);
    }
    return finish(r);
}

Sample* opGain(const Sample* s, float db)
{
    if (!valid(s)) return nullptr;
    if (!(db == db)) db = 0.0f;
    if (db > 48.0f) db = 48.0f;
    const float g = db <= -96.0f ? 0.0f : powf(10.0f, db * 0.05f);
    Sample* r = derive(s, s->frames, s->channels, s->rate);
    if (!r) return nullptr;
    const size_t n = (size_t)s->frames * s->channels;
    Pace pace(4);
    for (size_t i = 0; i < n; ++i) {
        pace.at(i);
        r->data[i] = sat16f((float)s->data[i] * g);
    }
    return finish(r);
}

Sample* opTrimSilence(const Sample* s, float threshold_db)
{
    if (!valid(s)) return nullptr;
    if (!(threshold_db < 0.0f)) threshold_db = -50.0f;
    const int thr = (int)(powf(10.0f, threshold_db * 0.05f) * 32768.0f);
    const uint32_t n = s->frames;
    const int ch = s->channels;
    auto loud = [&](uint32_t i) {
        for (int c = 0; c < ch; ++c) {
            const int v = s->data[(size_t)i * ch + c];
            if (v > thr || -v > thr) return true;
        }
        return false;
    };
    uint32_t a = 0;
    while (a < n && !loud(a)) ++a;
    if (a >= n) return opDuplicate(s);   // all silent: nothing to trim
    uint32_t b = n;
    while (b > a + 1 && !loud(b - 1)) --b;
    // Start on the preceding zero crossing (max 2 ms back) so the attack is kept click-free.
    const uint32_t back = s->rate / 500 + 1;
    const uint32_t lim = a > back ? a - back : 0;
    while (a > lim) {
        const int v0 = s->data[(size_t)(a - 1) * ch], v1 = s->data[(size_t)a * ch];
        if (v0 == 0 || (v0 < 0) != (v1 < 0)) {
            if (v0 == 0) --a;
            break;
        }
        --a;
    }
    // Keep a short tail (1 ms) after the last loud frame: the decay below the threshold.
    const uint32_t tail = s->rate / 1000;
    b = (n - b > tail) ? b + tail : n;
    return cropFrames(s, a, b);
}

Sample* opToMono(const Sample* s)
{
    if (!valid(s)) return nullptr;
    Sample* r = derive(s, s->frames, 1, s->rate);
    if (!r) return nullptr;
    if (s->channels == 1) {
        bulkCopy(r->data, s->data, (size_t)s->frames * sizeof(int16_t));
    } else {
        Pace pace(6);
        for (uint32_t i = 0; i < s->frames; ++i) {
            pace.at(i);
            const int32_t v = (int32_t)s->data[2 * i] + (int32_t)s->data[2 * i + 1];
            r->data[i] = (int16_t)(v >= 0 ? v / 2 : -((-v) / 2));
        }
    }
    return finish(r);
}

Sample* opResample(const Sample* s, uint32_t new_rate)
{
    if (!valid(s) || new_rate < 1000 || new_rate > 384000 || s->rate == 0) return nullptr;
    if (new_rate == s->rate) {
        Sample* r = opDuplicate(s);
        return r;
    }
    const uint64_t out = ((uint64_t)s->frames * new_rate + s->rate / 2) / s->rate;
    if (out == 0 || out > 0x7FFFFFFFull) return nullptr;
    const uint64_t step = ((uint64_t)s->rate << 32) / new_rate;
    return resampleCore(s, step, (uint32_t)out, new_rate);
}

Sample* opPitch(const Sample* s, float semitones)
{
    if (!valid(s) || !(semitones == semitones)) return nullptr;
    if (semitones > 48.0f) semitones = 48.0f;
    if (semitones < -48.0f) semitones = -48.0f;
    if (semitones == 0.0f) return opDuplicate(s);
    const float ratio = exp2f(semitones * (1.0f / 12.0f));   // input frames per output frame
    const float outf = (float)s->frames / ratio + 0.5f;
    if (!(outf >= 1.0f) || outf > 2.0e9f) return nullptr;
    const uint64_t step = (uint64_t)(ratio * 4294967296.0f);
    return resampleCore(s, step, (uint32_t)outf, s->rate);
}

Sample* opBitcrush(const Sample* s, int bits, int downsample)
{
    if (!valid(s)) return nullptr;
    if (bits < 1) bits = 1;
    if (bits > 16) bits = 16;
    if (downsample < 1) downsample = 1;
    if (downsample > 256) downsample = 256;
    Sample* r = derive(s, s->frames, s->channels, s->rate);
    if (!r) return nullptr;
    const int shift = 16 - bits;
    const int32_t round = shift > 0 ? (1 << (shift - 1)) : 0;
    const int ch = s->channels;
    int16_t held[2] = {0, 0};
    Pace pace((size_t)ch * 4u);
    for (uint32_t i = 0; i < s->frames; ++i) {
        pace.at(i);
        if (i % (uint32_t)downsample == 0) {   // sample-and-hold
            for (int c = 0; c < ch; ++c) {
                const int32_t v = s->data[(size_t)i * ch + c];
                held[c] = shift ? sat16i(((v + round) >> shift) * (1 << shift)) : (int16_t)v;
            }
        }
        for (int c = 0; c < ch; ++c) r->data[(size_t)i * ch + c] = held[c];
    }
    return finish(r);
}

Sample* opDuplicate(const Sample* s)
{
    if (!valid(s)) return nullptr;
    Sample* r = sampleClone(s);   // fresh id, same name and source path
    return finish(r);
}

float snapToZeroCrossing(const Sample* s, float pos, float window_ms)
{
    if (!valid(s)) return clamp01(pos);
    pos = clamp01(pos);
    const uint32_t n = s->frames;
    const int ch = s->channels;
    auto mono = [&](uint32_t i) {
        return ch == 1 ? (int32_t)s->data[i] : (int32_t)s->data[2 * i] + (int32_t)s->data[2 * i + 1];
    };
    uint32_t c = fracToFrame(pos, n);
    if (c >= n) c = n - 1;
    if (!(window_ms > 0.0f)) window_ms = 5.0f;
    uint32_t w = (uint32_t)(window_ms * 0.001f * (float)s->rate);
    if (w < 1) w = 1;
    // A crossing at i: mono(i-1) and mono(i) differ in sign (or one is zero). Pick the frame
    // of the pair with the smaller magnitude.
    auto crossing = [&](uint32_t i, uint32_t& at) {
        if (i == 0 || i >= n) return false;
        const int32_t v0 = mono(i - 1), v1 = mono(i);
        if (v1 == 0) {
            at = i;
            return true;
        }
        if (v0 == 0 || (v0 < 0) != (v1 < 0)) {
            const int32_t a0 = v0 < 0 ? -v0 : v0, a1 = v1 < 0 ? -v1 : v1;
            at = a0 <= a1 ? i - 1 : i;
            return true;
        }
        return false;
    };
    for (uint32_t d = 0; d <= w; ++d) {
        uint32_t at = 0;
        if (crossing(c + d, at)) return (float)at / (float)n;
        if (d <= c && crossing(c - d, at)) return (float)at / (float)n;
    }
    return pos;
}

}  // namespace orbit
