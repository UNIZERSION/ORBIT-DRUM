#include "sample.h"

#include <atomic>
#include <math.h>
#include <new>
#include <string.h>

#include "platform.h"

namespace orbit {

namespace {
std::atomic<uint32_t> g_next_id{1};
}

uint32_t sampleNextId() { return g_next_id.fetch_add(1, std::memory_order_relaxed); }

Sample* sampleCreate(uint32_t frames, int channels, uint32_t rate)
{
    if (channels < 1 || channels > 2 || frames == 0) return nullptr;
    Sample* s = static_cast<Sample*>(bigAlloc(sizeof(Sample)));
    if (!s) return nullptr;
    new (s) Sample();
    const size_t total = (size_t)(frames + 2 * kGuard) * (size_t)channels;
    // 0.4.9: not zeroed (a multi-MB memset at full speed costs dropouts while the audio plays): every caller writes
    // all frames; only the guard frames on both sides are cleared here
    int16_t* block = static_cast<int16_t*>(bigAllocRaw(total * sizeof(int16_t)));
    if (!block) {
        bigFree(s);
        return nullptr;
    }
    const size_t guard = (size_t)kGuard * (size_t)channels;
    memset(block, 0, guard * sizeof(int16_t));
    memset(block + total - guard, 0, guard * sizeof(int16_t));
    s->alloc = block;
    s->data = block + kGuard * channels;
    s->frames = frames;
    s->channels = (uint8_t)channels;
    s->rate = rate;
    s->id = sampleNextId();
    return s;
}

Sample* sampleWrapEmbedded(const int16_t* frame0, uint32_t frames, int channels, uint32_t rate, const char* name)
{
    Sample* s = static_cast<Sample*>(bigAlloc(sizeof(Sample)));
    if (!s) return nullptr;
    new (s) Sample();
    s->data = const_cast<int16_t*>(frame0);
    s->frames = frames;
    s->channels = (uint8_t)channels;
    s->rate = rate;
    s->embedded = 1;
    s->id = sampleNextId();
    if (name) strncpy(s->name, name, kNameLen - 1);
    sampleAnalyze(s);
    return s;
}

void sampleFree(Sample* s)
{
    if (!s) return;
    if (!s->embedded && s->alloc) bigFree(s->alloc);
    s->~Sample();
    bigFree(s);
}

Sample* sampleClone(const Sample* src)
{
    if (!src) return nullptr;
    Sample* s = sampleCreate(src->frames, src->channels, src->rate);
    if (!s) return nullptr;
    bulkCopy(s->data, src->data, (size_t)src->frames * src->channels * sizeof(int16_t));
    memcpy(s->pk_min, src->pk_min, sizeof(s->pk_min));
    memcpy(s->pk_max, src->pk_max, sizeof(s->pk_max));
    s->peak = src->peak;
    s->rms = src->rms;
    memcpy(s->name, src->name, sizeof(s->name));
    memcpy(s->path, src->path, sizeof(s->path));
    return s;
}

namespace {

// One bin of the waveform overview (mono mix min/max) and, with ACC, the peak (any channel) and
// sum of squares of the same frames. Integer only: the P4 has no double FPU, and this runs over
// every frame of every loaded WAV and built-in sound.
template <int CH, bool ACC>
inline void scanBin(const int16_t* d, uint32_t a, uint32_t e, int& lo, int& hi, int& peak, uint64_t& sum)
{
    int l = lo, h = hi, pk = peak;
    uint64_t sq = sum;
    for (uint32_t i = a; i < e; ++i) {
        const int x = d[(size_t)i * CH];
        const int y = CH == 2 ? d[(size_t)i * CH + 1] : x;
        const int v = CH == 2 ? (x + y) / 2 : x;
        if (v < l) l = v;
        if (v > h) h = v;
        if (ACC) {
            const int mx = x < 0 ? -x : x;
            if (mx > pk) pk = mx;
            sq += (uint32_t)(x * x);   // <= 2^30
            if (CH == 2) {
                const int my = y < 0 ? -y : y;
                if (my > pk) pk = my;
                sq += (uint32_t)(y * y);
            }
        }
    }
    lo = l;
    hi = h;
    peak = pk;
    sum = sq;
}

template <int CH>
void analyze(Sample* s)
{
    const uint32_t n = s->frames;
    const int16_t* d = s->data;
    int peak = 0;
    uint64_t sum = 0;
    // With n >= kPeakBins the bins partition [0, n): the overview, peak and RMS come from one pass
    // over the data (it used to be read twice, the second time with soft-double math).
    const bool partition = n >= (uint32_t)kPeakBins;
    BulkPacer pace;   // one pass over the whole sample: paced while the audio plays (platform.h)
    for (int b = 0; b < kPeakBins; ++b) {
        const uint32_t a = (uint32_t)((uint64_t)n * b / kPeakBins);
        uint32_t e = (uint32_t)((uint64_t)n * (b + 1) / kPeakBins);
        if (e <= a) e = a + 1;
        if (e > n) e = n;
        int lo = 32767, hi = -32768;
        if (partition) scanBin<CH, true>(d, a, e, lo, hi, peak, sum);
        else scanBin<CH, false>(d, a, e, lo, hi, peak, sum);
        if (lo > hi) lo = hi = 0;
        s->pk_min[b] = (int8_t)(lo / 258);
        s->pk_max[b] = (int8_t)(hi / 258);
        bulkPace(pace, (size_t)(e - a) * CH * sizeof(int16_t));
    }
    if (!partition) {   // fewer frames than bins (bins overlap): one extra pass over < 1024 frames
        int lo = 0, hi = 0;
        scanBin<CH, true>(d, 0, n, lo, hi, peak, sum);
    }
    s->peak = (float)peak / 32768.0f;
    s->rms = sqrtf((float)sum / (float)(n * (uint32_t)CH)) / 32768.0f;
}

}  // namespace

void sampleAnalyze(Sample* s)
{
    if (!s || !s->data || !s->frames) return;
    if (s->channels == 2) analyze<2>(s);
    else analyze<1>(s);
}

float sampleSeconds(const Sample* s) { return (s && s->rate) ? (float)s->frames / (float)s->rate : 0.0f; }

}  // namespace orbit
