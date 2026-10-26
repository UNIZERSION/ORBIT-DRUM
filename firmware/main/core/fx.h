#pragma once
// FX bus (tempo-synced stereo delay -> Dattorro plate) and the master-section processors
// (PUMP ducking, LO-FI, WIDE, metronome click). Audio task only after init().
#ifndef ORBIT_HOT
#define ORBIT_HOT
#endif

#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "dsp/fastmath.h"
#include "dsp/lofi.h"
#include "dsp/reverb.h"
#include "dsp/smooth.h"
#include "dsp/svf.h"

namespace orbit {

// Internal render chunk: Engine::process splits blocks (<= kMaxBlock) into chunks of this size,
// which keeps every block-sized buffer of the engine and the FX bus small (internal RAM).
constexpr int kRenderBlock = 128;
static_assert(kRenderBlock <= kMaxBlock, "render chunk larger than the max block");

// Stereo delay with a tone lowpass + DC blocker and a soft clip in the feedback loop.
// Delay-time changes crossfade two read heads (30 ms) instead of jumping.
class StereoDelay {
public:
    static constexpr float kMaxSeconds = 2.5f;
    static int32_t framesFor(int rate) { return (int32_t)(kMaxSeconds * (float)rate) + 8; }

    void init(float* mem_l, float* mem_r, int32_t frames, int rate);
    void clear();
    // Block rate. delay_samples is halved until it fits the buffer (stays tempo-synced).
    void setParams(float delay_samples, float feedback, float tone, bool pingpong);
    // Wet output only (overwrites out). 0.4.9: block-wise (all reads of the block first, then the feedback
    // filters and writes) whenever both read heads are >= n samples back; bit-exact with processRef().
    void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int n);
    // The per-sample code up to 0.4.9 (tests, A/B measurements on the device).
    void processRef(const float* in_l, const float* in_r, float* out_l, float* out_r, int n);
    void setReference(bool on) { ref_ = on; }
    bool usedBlockPath() const { return block_used_; }   // the last process() (fxcheck)
    int32_t delay() const { return cur_; }

private:
    float* bl_ = nullptr;
    float* br_ = nullptr;
    int32_t len_ = 0, w_ = 0;
    int32_t cur_ = 1, next_ = 1;
    float xf_ = 0.0f, xf_step_ = 0.0f;
    bool xfading_ = false;
    float fb_ = 0.0f, tone_a_ = 0.5f, dc_a_ = 0.005f;
    float lpl_ = 0.0f, lpr_ = 0.0f, dcl_ = 0.0f, dcr_ = 0.0f;
    bool pp_ = true;
    float fs_ = 48000.0f;
    float tone_in_ = -1.0f;   // tone the coefficient tone_a_ was computed for (libm only on change)
    bool ref_ = false;
    bool block_used_ = false;
};

struct FxParams {
    float delay_samples = 12000.0f;
    float dly_feedback = 0.38f;
    float dly_tone = 0.6f;
    float dly_level = 0.5f;
    bool dly_pingpong = true;
    float rev_size = 0.55f;
    float rev_decay = 2.2f;   // s (RT60)
    float rev_damp = 0.45f;
    float rev_predelay_ms = 12.0f;
    float rev_level = 0.7f;
    bool reference = false;   // 0.4.9: the delay's per-sample reference code (A/B measurements)
    bool bypass = false;      // 0.4.9 diagnostics only: no delay / reverb at all (memory-contention tests)
};

// Send bus: send -> delay; reverb input = send + delay return + SPACE; output = both returns.
class FxBus {
public:
    // 0.4.9: the reverb's lines in internal RAM. With ~45 read / write streams per sample in PSRAM they crowded the
    // cache and the PSRAM bus that the UI and the audio share (on the Tab5 the UI drew 2x faster with the FX bus
    // off). reserveLines() takes the block once at boot, before the heap fragments, for the highest rate the engine
    // uses (48 kHz; 44.1 kHz needs less); init() then puts the lines there, else (no reservation, or no internal
    // RAM) in PSRAM. Same arithmetic either way: only the place of the memory differs.
    FxBus() = default;
    ~FxBus() { release(); }
    FxBus(const FxBus&) = delete;
    FxBus& operator=(const FxBus&) = delete;
    static bool reserveLines(int max_rate);
    static bool linesReserved();
    bool init(int rate);   // allocates (not on the audio task); false when out of memory
    void release();
    bool linesInternal() const { return lines_resv_; }   // this init uses the reserved internal block
    void clear();
    // send_l/r: FX send; space: mono reverb-only send. Overwrites out_l/out_r with the FX return.
    // n <= kRenderBlock.
    void process(const float* send_l, const float* send_r, const float* space, int n, const FxParams& p,
                 float* out_l, float* out_r);
    bool ready() const { return rev_lines_ != nullptr && rev_pre_ != nullptr && dly_l_ != nullptr; }
    bool idle() const { return idle_; }
    // CPU cycles of the last process() (0 on the PC or while idle): the delay, the reverb
    uint32_t delayCycles() const { return cyc_dly_; }
    uint32_t reverbCycles() const { return cyc_rev_; }
    size_t memoryBytes() const { return mem_bytes_; }

private:
    StereoDelay delay_;
    dsp::PlateReverb reverb_;
    float* rev_lines_ = nullptr;   // the reserved block (not owned) or a PSRAM block (owned)
    bool lines_resv_ = false;
    float* rev_pre_ = nullptr;     // the pre-delay (PSRAM)
    float* dly_l_ = nullptr;
    float* dly_r_ = nullptr;
    size_t mem_bytes_ = 0;
    int rate_ = 48000;
    dsp::Ramp dly_level_, rev_level_;
    uint32_t silent_ = 0;   // samples since the last non-silent input
    bool idle_ = true;
    float echo_fb_ = -1.0f;   // tail estimate cache (logf only when feedback / delay change)
    int32_t echo_dly_ = -1;
    float echo_s_ = 0.0f;
    uint32_t cyc_dly_ = 0, cyc_rev_ = 0;
    float rin_[kRenderBlock];
    float dl_[kRenderBlock], dr_[kRenderBlock], rl_[kRenderBlock], rr_[kRenderBlock];
};

// PUMP: ducking envelope triggered by hits (attack ~2 ms linear, exponential release).
struct Pump {
    float env = 0.0f;
    float att_step = 0.01f;
    int attacking = 0;
    float rel_ms = -1.0f, rel_fs = 0.0f, rel_c = 1.0f;   // release coefficient cache
    void init(float fs) { att_step = 1.0f / (0.002f * fs); }
    // Fills gain[0..n) with 1 - depth * env. hits = sorted offsets inside the block.
    void process(float* gain, int n, const uint32_t* hits, int nhits, float depth, float release_ms, float fs);
};

// LO-FI macro: bit crush + sample-rate reduction with dry/wet mix (mix 0 = bypass).
struct LoFi {
    dsp::RateReducer rr;
    float crush_bits = -1.0f, crush_q = 1.0f, crush_iq = 1.0f;   // crushQ cache (exp2f only when BITS moves)
    void process(float* l, float* r, int n, float bits, float rate_hz, float mix_from, float mix_to, float fs);
};

// WIDE macro: M/S, side * (1 + width) through a ~150 Hz highpass (lows stay mono).
struct Wide {
    dsp::Svf hp;
    dsp::SvfCoefs c;
    void init(float fs) { c = dsp::svfCoefs(150.0f / fs, 1.41421356f); hp.reset(); }
    // amt = on/off crossfade 0..1, width = GP_WIDE (ramped per block from/to)
    void process(float* l, float* r, int n, float amt_from, float amt_to, float width);
};

// Metronome blip (sine with fast decay), scheduled with a sample delay.
struct Click {
    int32_t wait = -1;      // samples until start, -1 = idle
    int32_t left = 0;       // samples remaining
    float phase = 0.0f, inc = 0.0f, amp = 0.0f, decay = 0.999f, env = 0.0f;
    float pend_inc = 0.0f, pend_amp = 0.0f;
    int32_t pend_len = 0;
    float dec_fs = 0.0f;    // rate `decay` was computed for
    void trigger(int32_t delay, bool accent, float level, float fs);
    void process(float* l, float* r, int n);
};

}  // namespace orbit
