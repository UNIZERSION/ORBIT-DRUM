// ORBIT DRUM audio engine (see engine.h for the threading contract).
//
// Per block: drain producer queues -> Sequencer::process -> render voices in segments split at
// trigger offsets (sample-accurate starts, chokes and steals) -> buses -> FX bus -> master
// (PUMP, LO-FI, WIDE, preview, MASTER gain, limiter, metronome) -> telemetry and events.
#include "engine.h"

#include <math.h>
#include <new>
#include <string.h>

#include "dsp/fastmath.h"
#include "dsp/interp.h"
#include "dsp/limiter.h"
#include "dsp/smooth.h"
#include "acid.h"
#include "fx.h"
#include "params.h"
#include "platform.h"
#include "seq.h"
#include "spsc.h"
#include "voice.h"

namespace orbit {

using namespace dsp;

namespace {

constexpr uint32_t kCmdQueue = 128;
constexpr uint32_t kEvtQueue = 256;
constexpr int kMaxSeqTrig = 128;
constexpr int kMaxLive = 64;
constexpr int kRetireMax = 2 * kMaxSamples;
constexpr int kOutboxMax = 64;
constexpr int kMaxPumpHits = 32;
constexpr float kStealMs = 4.0f;
constexpr float kChokeMs = 5.0f;
constexpr float kRetireMs = 4.0f;
constexpr float kPanicMs = 5.0f;
constexpr float kPreviewFadeMs = 5.0f;
// CPU governor (Telemetry::cpu is a smoothed render/block ratio, ~10-block time constant).
constexpr float kGovHigh = 0.80f;     // above: shed voices
constexpr float kGovPanic = 0.95f;    // above: shed faster
constexpr float kGovLow = 0.60f;      // below: give voices back
constexpr float kGovDownS = 0.030f;   // min time between two reductions (lets the load settle)
constexpr float kGovUpS = 0.100f;     // one voice back per 100 ms
constexpr int kGovMin = Engine::kMinVoiceLimit;

// A layer's slot index is UI-written at any time: read it exactly once (no re-load by the
// compiler) and validate that copy before it indexes the slot table.
inline int loadSlot(const Layer& L) { return __atomic_load_n(&L.slot, __ATOMIC_RELAXED); }

enum LiveType : uint8_t { LV_TRIGGER = 0, LV_RELEASE };

struct LiveEvt {
    uint8_t type;
    uint8_t pad;
    uint8_t vel;
    float pitch;
};

struct PreviewVoice {
    const Sample* smp = nullptr;
    int32_t pos = 0;
    float frac = 0.0f;
    float inc = 1.0f;
    float gain = 1.0f;
    float fade = 1.0f;
    float fade_step = 0.0f;
    bool fading = false;
    bool finished = false;   // done, PreviewDone not delivered yet
};

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Bit-exact equality like memcmp() == 0, inline: libc memcmp runs from flash, and the pad cache
// check below runs for every pad in every block (the UI keeps evicting flash lines from the cache).
inline bool sameBits(const float* a, const float* b, int n)
{
    for (int i = 0; i < n; ++i) {
        uint32_t x, y;
        memcpy(&x, a + i, sizeof x);
        memcpy(&y, b + i, sizeof y);
        if (x != y) return false;
    }
    return true;
}
inline bool sameBytes(const uint8_t* a, const uint8_t* b, int n)
{
    for (int i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

}  // namespace

class EngineImpl {
public:
    // ---- setup ----
    bool init(int rate_hz, const Project* project);
    void* raw = nullptr;    // allocation start (alignment padding)
    bool raw_fast = false;

    // ---- shared with other tasks ----
    Spsc<Command, kCmdQueue> cmdq[(int)Producer::Count];
    Spsc<Event, kEvtQueue> evq;
    Telemetry tel;
    std::atomic<const Sample*> slot_pub[kMaxSamples];

    // ---- audio task ----
    void process(float* l, float* r, int n);
    void processI16(int16_t* lr, int n);

    int rate = 0;
    float fs = 48000.0f;
    VoiceCtx vctx;
    const Project* prj = nullptr;
    bool inited = false;
    float cpu_s = 0.0f;
    std::atomic<uint8_t> user_cap{(uint8_t)kVoices};   // Engine::setVoiceLimit (any task)

private:
    void processBlock(float* l, float* r, int n);
    void governor(int n);
    Voice* stealVictim();
    void updatePadCache();
    void drainCommands();
    bool handleCommand(const Command& c);
    bool setSample(int slot, const Sample* s);
    void startPreview(const Sample* s, int vel);
    void stopPreview();
    void renderPreview(int n);
    bool hit(int pad, int vel, float pitch, uint32_t offset);
    // Picks the layers of a hit. out_smp[i] is the slot content validated for out_layer[i]:
    // hit() must use it and never index slots_ again with a slot re-read from the Project.
    int selectLayers(int pad, int vel, int* out_layer, const Sample** out_smp);
    Voice* allocVoice();
    void renderVoices(int from, int to, int n);
    void scheduleBass(const AcidEvent& e);
    void renderBass(int n);
    void master(float* l, float* r, int n, const SeqNotice& notice);
    void retireCheck();
    void updateTelemetry(const SeqNotice& notice, int n);
    bool reliable(const Event& e);
    void emit(const Event& e) { evq.push(e); }
    void flushOutbox();
    bool referenced(const Sample* s) const;

    Command deferred_[(int)Producer::Count];
    bool has_deferred_[(int)Producer::Count] = {};
    LiveEvt live_[kMaxLive];
    int nlive_ = 0;
    SeqTrigger strig_[kMaxSeqTrig];

    Sequencer seq_;
    const Sample* slots_[kMaxSamples] = {};
    Voice voices_[kVoicePool];
    uint32_t hit_serial_ = 0, age_serial_ = 0;
    int gov_cap_ = kVoices;        // CPU governor limit
    int cap_ = kVoices;            // effective: min(governor, user)
    uint32_t gov_frames_ = 0;      // frames since the governor last moved
    int coef_n_ = 0;               // block length the per-block coefficients below are for
    float k40_ = 0.0f, k20_ = 0.0f;

    PadCache pc_[kPads];
    float praw_[kPads][kPadParamCount];
    uint8_t pmisc_[kPads][6];
    bool pvalid_[kPads] = {};
    float pbpm_ = 0.0f;
    uint8_t rr_next_[kPads] = {};
    int8_t last_rand_[kPads] = {};
    Rng rng_;
    Rng dither_;

    // block buffers
    float bus_l_[kBuses][kRenderBlock], bus_r_[kBuses][kRenderBlock];
    float snd_l_[kBuses][kRenderBlock], snd_r_[kBuses][kRenderBlock];
    bool bus_used_[kBuses] = {};
    float mix_l_[kRenderBlock], mix_r_[kRenderBlock];
    float ex_l_[kRenderBlock], ex_r_[kRenderBlock];      // pump-exempt (bus of the PUMP source)
    float fx_l_[kRenderBlock], fx_r_[kRenderBlock];      // FX send sum
    float all_l_[kRenderBlock], all_r_[kRenderBlock];    // all bus inputs (SPACE / throw)
    float space_[kRenderBlock];
    float ret_l_[kRenderBlock], ret_r_[kRenderBlock];
    float pump_g_[kRenderBlock];
    float prev_l_[kRenderBlock], prev_r_[kRenderBlock];
    float tmp_l_[kRenderBlock], tmp_r_[kRenderBlock];
    bool prev_used_ = false;

    Ramp bus_gl_[kBuses], bus_gr_[kBuses], bus_send_[kBuses];
    Ramp master_g_;
    float throw_ = 0.0f, space_amt_ = 0.0f, lofi_mix_ = 0.0f, wide_amt_ = 0.0f, pump_depth_ = 0.0f;

    FxBus fx_;
    FxParams fxp_;
    Pump pump_;
    LoFi lofi_;
    Wide wide_;
    LookaheadLimiter lim_;
    Click click_;
    uint32_t pump_hits_[kMaxPumpHits];
    int npump_ = 0;

    int8_t metro_override_ = -1;
    bool rec_armed_ = false;
    bool last_playing_ = false;

    const Sample* retired_[kRetireMax];
    int nretired_ = 0;
    Event outbox_[kOutboxMax];
    int nout_ = 0;
    PreviewVoice pv_[2];   // 0 = current, 1 = previous one fading out

    // ACID BASS (0.3): one 303-style voice, mixed into bus GP_BASS_BUS like a pad
    AcidVoice bass_;
    bool bass_queued_ = false;     // events wait for the next render (the voice may be idle)
    bool bass_live_ = false;       // a held live note (BASS keys) owns the voice: the bass line waits
    float bass_buf_[kRenderBlock];
};

// ============================================================================ setup

bool EngineImpl::init(int rate_hz, const Project* project)
{
    prj = project;
    if (rate_hz != rate || !fx_.ready()) {
        if (!fx_.init(rate_hz)) logf("engine: FX memory allocation failed, FX bus disabled");
    } else {
        fx_.clear();
    }
    rate = rate_hz;
    fs = (float)rate_hz;
    vctx.fs = fs;
    vctx.inv_fs = 1.0f / fs;
    seq_.init(rate_hz, project);
    bass_.init(rate_hz);
    bass_queued_ = false;
#ifdef ESP_PLATFORM
    // Probability rolls differ on every power-on (the PC build stays deterministic for tests).
    seq_.seed((uint32_t)nowUs() ^ 0x9E3779B9u);
#endif
    for (auto& v : voices_) v = Voice();
    for (auto& b : pvalid_) b = false;
    for (int b = 0; b < kBuses; ++b) {
        bus_gl_[b].reset(0.0f);
        bus_gr_[b].reset(0.0f);
        bus_send_[b].reset(0.0f);
        bus_used_[b] = false;
    }
    master_g_.reset(dbToLin(project->g[GP_MASTER]));
    throw_ = space_amt_ = lofi_mix_ = wide_amt_ = pump_depth_ = 0.0f;
    lim_.init(fs);
    pump_.init(fs);
    pump_.env = 0.0f;
    pump_.attacking = 0;
    wide_.init(fs);
    lofi_.rr.reset();
    click_ = Click();
    nlive_ = 0;
    rec_armed_ = false;     // a re-init (rate change) starts unarmed, like the UI shows it
    metro_override_ = -1;   // Project::metronome until the UI sends Metronome again
    for (int i = 0; i < 2; ++i)
        if (pv_[i].smp && !pv_[i].finished) pv_[i].finished = true;   // reported on the next block
    rng_.s = 0x2545F491u;
    memset(last_rand_, 0xff, sizeof(last_rand_));
    dither_.s = 0x6C8E9CF5u;
    last_playing_ = false;
    gov_cap_ = kVoices;
    cap_ = clampi(user_cap.load(std::memory_order_relaxed), kGovMin, kVoices);
    gov_frames_ = 0;
    coef_n_ = 0;
    tel.voice_cap.store((uint8_t)cap_, std::memory_order_relaxed);
    inited = true;
    return true;
}

// ============================================================================ commands

ORBIT_HOT void EngineImpl::drainCommands()
{
    nlive_ = 0;
    for (int p = 0; p < (int)Producer::Count; ++p) {
        if (has_deferred_[p]) {
            if (!handleCommand(deferred_[p])) continue;   // still blocked: keep order, try next block
            has_deferred_[p] = false;
        }
        Command c;
        while (cmdq[p].pop(c)) {
            if (!handleCommand(c)) {
                deferred_[p] = c;
                has_deferred_[p] = true;
                break;
            }
        }
    }
}

ORBIT_HOT bool EngineImpl::handleCommand(const Command& c)
{
    switch (c.type) {
    case CmdType::Trigger:
    case CmdType::Release:
        if (c.pad >= kPads) return true;
        if (nlive_ >= kMaxLive) return false;
        live_[nlive_].type = c.type == CmdType::Trigger ? LV_TRIGGER : LV_RELEASE;
        live_[nlive_].pad = c.pad;
        live_[nlive_].vel = (uint8_t)clampi(c.vel, 1, 127);
        live_[nlive_].pitch = (float)clampi((int)c.value, -48, 48);
        ++nlive_;
        return true;
    case CmdType::Play: seq_.play(); return true;
    case CmdType::Stop: seq_.stop(); return true;
    case CmdType::Continue: seq_.resume(); return true;
    case CmdType::Locate: seq_.locate((int)c.value); return true;
    case CmdType::QueueScene:
        if (c.pad < kScenes) seq_.queueScene(c.pad);
        return true;
    case CmdType::SetSample: return setSample((int)c.value, c.sample);
    case CmdType::Preview:
        if (nout_ >= kOutboxMax - 2) return false;
        startPreview(c.sample, c.vel);
        return true;
    case CmdType::StopPreview: stopPreview(); return true;
    case CmdType::Panic: {
        for (auto& v : voices_)
            if (v.active()) voiceFadeOut(v, kPanicMs * 0.001f * fs);
        stopPreview();
        AcidEvent e;
        e.type = AcidEvtType::Panic;
        bass_live_ = false;
        scheduleBass(e);
        return true;
    }
    case CmdType::BassNote: {
        AcidEvent e;
        e.type = AcidEvtType::NoteOn;
        e.note = (uint8_t)clampi((int)c.value, 0, 127);
        e.accent = (c.vel & 1) ? 1 : 0;
        e.legato = (c.vel & 2) ? 1 : 0;
        e.gate_frames = c.pad ? (uint32_t)c.pad * (uint32_t)rate / 100u : 0u;
        if (!c.pad) bass_live_ = true;   // held from the keyboard until BassOff
        scheduleBass(e);
        return true;
    }
    case CmdType::BassOff: {
        AcidEvent e;
        e.type = AcidEvtType::NoteOff;
        e.note = kAcidAnyNote;
        bass_live_ = false;
        scheduleBass(e);
        return true;
    }
    case CmdType::SetFill: seq_.setFill(c.value != 0); return true;
    case CmdType::SetRecord: rec_armed_ = c.value != 0; return true;
    case CmdType::Metronome: metro_override_ = c.value ? 1 : 0; return true;
    default: return true;
    }
}

ORBIT_HOT bool EngineImpl::referenced(const Sample* s) const
{
    for (const auto& v : voices_)
        if (v.active() && v.smp == s) return true;
    for (const auto& p : pv_)
        if (p.smp == s) return true;
    return false;
}

ORBIT_HOT bool EngineImpl::setSample(int slot, const Sample* s)
{
    if (slot < 0 || slot >= kMaxSamples) {
        // invalid slot: hand the sample straight back
        if (!s) return true;
        if (nretired_ >= kRetireMax) return false;
        retired_[nretired_++] = s;
        return true;
    }
    const Sample* old = slots_[slot];
    if (old == s) return true;
    if (old && nretired_ >= kRetireMax) return false;   // defer until retirements drain
    // Every install is answered by exactly one SampleRetired. Installing a pointer that already sits in
    // another slot, or whose retirement is still pending, answers that earlier install at once (the
    // owner keeps the sample: it is installed again); the new install is answered when the pointer
    // leaves its last slot, as usual.
    int pending = -1;
    bool elsewhere = false;
    if (s) {
        for (int i = 0; i < nretired_ && pending < 0; ++i)
            if (retired_[i] == s) pending = i;
        for (int i = 0; i < kMaxSamples && !elsewhere; ++i)
            if (i != slot && slots_[i] == s) elsewhere = true;
        if ((pending >= 0 || elsewhere) && nout_ >= kOutboxMax) return false;   // no room for the answer yet
    }
    slots_[slot] = s;
    slot_pub[slot].store(s, std::memory_order_release);
    if (pending >= 0 || elsewhere) {
        if (pending >= 0) retired_[pending] = retired_[--nretired_];   // re-used before it was retired: keep it
        Event e;
        e.type = EvtType::SampleRetired;
        e.sample = s;
        reliable(e);   // room checked above
    }
    if (old) {
        for (int i = 0; i < kMaxSamples; ++i)
            if (slots_[i] == old) return true;   // still in another slot
        retired_[nretired_++] = old;
        for (auto& v : voices_)
            if (v.active() && v.smp == old) voiceFadeOut(v, kRetireMs * 0.001f * fs);
    }
    return true;
}

// ============================================================================ events

ORBIT_HOT bool EngineImpl::reliable(const Event& e)
{
    if (nout_ >= kOutboxMax) return false;
    outbox_[nout_++] = e;
    return true;
}

ORBIT_HOT void EngineImpl::flushOutbox()
{
    int k = 0;
    while (k < nout_ && evq.push(outbox_[k])) ++k;
    if (k == 0) return;
    for (int i = k; i < nout_; ++i) outbox_[i - k] = outbox_[i];
    nout_ -= k;
}

ORBIT_HOT void EngineImpl::retireCheck()
{
    for (int i = nretired_ - 1; i >= 0; --i) {
        const Sample* s = retired_[i];
        if (referenced(s)) continue;
        Event e;
        e.type = EvtType::SampleRetired;
        e.sample = s;
        if (!reliable(e)) break;
        retired_[i] = retired_[--nretired_];
    }
}

// ============================================================================ preview

ORBIT_HOT void EngineImpl::startPreview(const Sample* s, int vel)
{
    // the caller made sure the outbox has room for two events
    if (pv_[0].smp) {
        if (pv_[1].smp) {   // a third preview while one still fades: report the oldest now
            Event e;
            e.type = EvtType::PreviewDone;
            e.sample = pv_[1].smp;
            reliable(e);
        }
        pv_[1] = pv_[0];
        if (!pv_[1].finished && !pv_[1].fading) {
            pv_[1].fading = true;
            pv_[1].fade_step = 1.0f / (kPreviewFadeMs * 0.001f * fs);
        }
    }
    pv_[0] = PreviewVoice();
    if (!s || !s->data || s->frames == 0 || s->rate == 0) {
        if (s) {
            Event e;
            e.type = EvtType::PreviewDone;
            e.sample = s;
            reliable(e);
        }
        return;
    }
    pv_[0].smp = s;
    pv_[0].inc = (float)s->rate / fs;
    pv_[0].gain = (float)clampi(vel, 1, 127) * (1.0f / 127.0f);
}

ORBIT_HOT void EngineImpl::stopPreview()
{
    for (auto& p : pv_) {
        if (p.smp && !p.finished && !p.fading) {
            p.fading = true;
            p.fade_step = 1.0f / (kPreviewFadeMs * 0.001f * fs);
        }
    }
}

ORBIT_HOT void EngineImpl::renderPreview(int n)
{
    prev_used_ = false;
    for (auto& p : pv_) {
        if (!p.smp) continue;
        if (p.finished) {
            Event e;
            e.type = EvtType::PreviewDone;
            e.sample = p.smp;
            if (reliable(e)) p = PreviewVoice();
            continue;
        }
        if (!prev_used_) {
            memset(prev_l_, 0, sizeof(float) * (size_t)n);
            memset(prev_r_, 0, sizeof(float) * (size_t)n);
            prev_used_ = true;
        }
        const Sample* s = p.smp;
        const int ch = s->channels == 2 ? 2 : 1;
        const int32_t frames = (int32_t)s->frames;
        const float g = p.gain * kInv32768 * (ch == 2 ? 1.0f : 0.70710678f);
        for (int i = 0; i < n; ++i) {
            if (p.pos >= frames || p.fade <= 0.0f) {
                p.finished = true;
                break;
            }
            const int16_t* q = s->data + (size_t)p.pos * ch;
            const float gg = g * p.fade;
            const float yl = hermiteI16(q, ch, p.frac) * gg;
            const float yr = ch == 2 ? hermiteI16(q + 1, ch, p.frac) * gg : yl;
            prev_l_[i] += yl;
            prev_r_[i] += yr;
            if (p.fading) p.fade -= p.fade_step;
            p.frac += p.inc;
            const int32_t a = (int32_t)p.frac;
            p.pos += a;
            p.frac -= (float)a;
        }
        if (p.finished) {
            Event e;
            e.type = EvtType::PreviewDone;
            e.sample = p.smp;
            if (reliable(e)) p = PreviewVoice();
        }
    }
}

// ============================================================================ pads and voices

ORBIT_HOT void EngineImpl::updatePadCache()
{
    const Project& P = *prj;
    const float bpm = clampf(P.bpm, 20.0f, 400.0f);
    const bool bpm_changed = bpm != pbpm_;
    pbpm_ = bpm;
    for (int p = 0; p < kPads; ++p) {
        const Pad& pd = P.pad[p];
        const uint8_t misc[6] = {(uint8_t)pd.mode, pd.reverse, pd.bus, pd.choke, pd.poly, (uint8_t)pd.layer_mode};
        const bool sync = pd.p[PP_LFO_SYNC] >= 0.5f;
        if (pvalid_[p] && !(sync && bpm_changed) && sameBits(praw_[p], pd.p, kPadParamCount) &&
            sameBytes(pmisc_[p], misc, (int)sizeof(misc)))
            continue;
        memcpy(praw_[p], pd.p, sizeof(praw_[p]));
        memcpy(pmisc_[p], misc, sizeof(misc));
        pvalid_[p] = true;

        float v[kPadParamCount];
        for (int i = 0; i < kPadParamCount; ++i) v[i] = clampParam(padSpec((PadParam)i), praw_[p][i]);
        PadCache& c = pc_[p];
        float st = minf(v[PP_START], v[PP_END]), en = maxf(v[PP_START], v[PP_END]);
        if (en - st < 1.0e-6f) en = minf(1.0f, st + 1.0e-6f);
        c.start = st;
        c.end = en;
        c.loop = minf(v[PP_LOOP], 0.999f);
        c.fade_in_s = v[PP_FADE_IN] * 0.001f;
        c.fade_out_s = v[PP_FADE_OUT] * 0.001f;
        c.gain = dbToLin(v[PP_GAIN]) * (v[PP_VOLUME] <= -59.9f ? 0.0f : dbToLin(v[PP_VOLUME]));
        c.env.att_c = attackCoef(v[PP_ATTACK], fs);
        c.env.dec_c = segmentCoef(v[PP_DECAY], fs);
        c.env.rel_c = segmentCoef(v[PP_RELEASE], fs);
        c.env.sus = v[PP_SUSTAIN];
        c.env.hold = (int32_t)(v[PP_HOLD] * 0.001f * fs);
        c.vel_amp = v[PP_VEL_AMP];
        c.cutoff_norm = v[PP_CUTOFF] / fs;
        c.cutoff_l2 = log2f(c.cutoff_norm);
        c.k = svfK(v[PP_RESO]);
        const float d = v[PP_DRIVE];
        if (d > 0.001f) {
            c.drive_pre = (1.0f + 7.0f * d) * kInv32768;
            c.drive_post = 32768.0f / (1.0f + 1.5f * d);
        } else {
            c.drive_pre = 0.0f;
            c.drive_post = 1.0f;
        }
        c.filter_on = 1;
        c.mix_lp = c.mix_bp = c.mix_hp = 0.0f;
        switch ((FilterType)(int)v[PP_FTYPE]) {
        case FilterType::LowPass: c.mix_lp = 1.0f; break;
        case FilterType::BandPass: c.mix_bp = c.k; break;   // normalized: unity gain at fc
        case FilterType::HighPass: c.mix_hp = 1.0f; break;
        default: c.filter_on = 0; break;
        }
        c.fenv_oct = v[PP_FENV] * 6.0f;
        c.fdec_a = expf(segmentLnPerSample(v[PP_FDECAY], fs) * (float)kCtlBlock);
        c.tune = v[PP_TUNE] + v[PP_FINE] * 0.01f;
        c.penv = v[PP_PENV];
        c.pdec_a = expf(segmentLnPerSample(v[PP_PDECAY], fs) * (float)kCtlBlock);
        if (v[PP_BITS] < 15.5f) {
            const float step = exp2f(16.0f - v[PP_BITS]);
            c.crush_step = step;
            c.crush_q = 1.0f / step;
        } else {
            c.crush_q = 0.0f;
            c.crush_step = 1.0f;
        }
        c.downsample = clampi((int)v[PP_DOWNSAMPLE], 1, 32);
        float hz = v[PP_LFO_RATE];
        if (v[PP_LFO_SYNC] >= 0.5f) {
            const int idx = clampi((int)(toNorm(padSpec(PP_LFO_RATE), hz) * 11.0f + 0.5f), 0, (int)SyncDiv::Count - 1);
            hz = (bpm / 60.0f) / syncDivBeats((SyncDiv)idx);
        }
        c.lfo_inc = hz / fs;
        c.lfo_depth = v[PP_LFO_DEPTH];
        c.lfo_fade_inv = v[PP_LFO_FADE] > 0.5f ? 1.0f / (v[PP_LFO_FADE] * 0.001f * fs) : 0.0f;
        c.lfo_shape = (uint8_t)clampi((int)v[PP_LFO_SHAPE], 0, (int)LfoShape::Count - 1);
        c.lfo_target = (uint8_t)clampi((int)v[PP_LFO_TARGET], 0, (int)LfoTarget::Count - 1);
        c.pan = v[PP_PAN];
        c.send = v[PP_SEND];
        c.mode = (uint8_t)clampi((int)pd.mode, 0, (int)PlayMode::Count - 1);
        c.reverse = pd.reverse ? 1 : 0;
        c.bus = (uint8_t)clampi(pd.bus, 0, kBuses - 1);
        c.choke = pd.choke;
        c.poly = (uint8_t)clampi(pd.poly, 1, kMaxPadPoly);
    }
}

ORBIT_HOT int EngineImpl::selectLayers(int pad, int vel, int* out, const Sample** out_smp)
{
    const Pad& pd = prj->pad[pad];
    int usable[kLayers];
    const Sample* usmp[kLayers];   // the slot content validated together with usable[]
    int nu = 0;
    for (int l = 0; l < kLayers; ++l) {
        const int s = loadSlot(pd.layer[l]);
        if ((unsigned)s >= (unsigned)kMaxSamples) continue;
        const Sample* smp = slots_[s];
        if (!smp || smp->frames == 0) continue;
        usable[nu] = l;
        usmp[nu] = smp;
        ++nu;
    }
    if (nu == 0) return 0;
    int n = 0;
    auto take = [&](int i) {
        out[n] = usable[i];
        out_smp[n] = usmp[i];
        ++n;
    };
    switch (pd.layer_mode) {
    case LayerMode::Velocity: {
        // Ranges set per layer (vel_lo/vel_hi, saved in the project) win; with every range at the
        // default 0..127 the usable layers split the velocity range evenly, in layer order
        // (LAYER 1 = softest), so VELOCITY never sounds like STACK.
        bool custom = false;
        for (int i = 0; i < nu; ++i) {
            const Layer& L = pd.layer[usable[i]];
            custom |= L.vel_lo != 0 || L.vel_hi != 127;
        }
        if (!custom) {
            take(clampi(vel * nu / 128, 0, nu - 1));
            break;
        }
        for (int i = 0; i < nu; ++i) {
            const Layer& L = pd.layer[usable[i]];
            if (vel >= L.vel_lo && vel <= L.vel_hi) take(i);
        }
        break;
    }
    case LayerMode::RoundRobin: {
        const int start = rr_next_[pad] % kLayers;
        for (int k = 0; k < kLayers && n == 0; ++k) {
            const int l = (start + k) % kLayers;
            for (int i = 0; i < nu; ++i) {
                if (usable[i] != l) continue;
                take(i);
                rr_next_[pad] = (uint8_t)((l + 1) % kLayers);
                break;
            }
        }
        break;
    }
    case LayerMode::Random: {
        int pick = (int)(rng_.next() % (uint32_t)nu);
        if (nu > 1 && usable[pick] == last_rand_[pad]) pick = (pick + 1 + (int)(rng_.next() % (uint32_t)(nu - 1))) % nu;
        take(pick);
        last_rand_[pad] = (int8_t)usable[pick];
        break;
    }
    case LayerMode::Stack:
        for (int i = 0; i < nu; ++i) take(i);
        break;
    case LayerMode::Single:
    default: take(0); break;
    }
    return n;
}

// Quietest voice in release, else the oldest live voice.
ORBIT_HOT Voice* EngineImpl::stealVictim()
{
    Voice* victim = nullptr;
    float best = 1.0e9f;
    for (auto& v : voices_) {
        if (!v.live() || v.amp.stage != ES_RELEASE) continue;
        if (v.level() < best) {
            best = v.level();
            victim = &v;
        }
    }
    if (victim) return victim;
    uint32_t oldest = 0xffffffffu;
    for (auto& v : voices_)
        if (v.live() && v.age < oldest) {
            oldest = v.age;
            victim = &v;
        }
    return victim;
}

// CPU governor: Telemetry::cpu above kGovHigh sheds voices (2, or 4 above kGovPanic, every
// kGovDownS) below the live count, so a dense pattern loses a few tails instead of underrunning
// the output on every block; below kGovLow one voice comes back every kGovUpS. The user limit
// (Engine::setVoiceLimit) caps the result. Without timing reports (PC tests) cpu_s stays 0.
ORBIT_HOT void EngineImpl::governor(int n)
{
    if (gov_frames_ < 0x40000000u) gov_frames_ += (uint32_t)n;
    int live = 0;
    for (const auto& v : voices_) live += v.live() ? 1 : 0;
    const float load = cpu_s;
    if (load > kGovHigh && (float)gov_frames_ >= kGovDownS * fs) {
        const int base = live < gov_cap_ ? live : gov_cap_;
        const int cut = load > kGovPanic ? 4 : 2;
        const int cap = base - cut < kGovMin ? kGovMin : base - cut;
        if (cap < gov_cap_) {
            gov_cap_ = cap;
            gov_frames_ = 0;
        }
    } else if (load < kGovLow && gov_cap_ < kVoices && (float)gov_frames_ >= kGovUpS * fs) {
        ++gov_cap_;
        gov_frames_ = 0;
    }
    const int user = clampi(user_cap.load(std::memory_order_relaxed), kGovMin, kVoices);
    cap_ = gov_cap_ < user ? gov_cap_ : user;
    tel.voice_cap.store((uint8_t)cap_, std::memory_order_relaxed);
    while (live > cap_) {
        Voice* v = stealVictim();
        if (!v) break;
        voiceFadeOut(*v, kStealMs * 0.001f * fs);
        --live;
    }
}

ORBIT_HOT Voice* EngineImpl::allocVoice()
{
    int live = 0;
    for (const auto& v : voices_) live += v.live() ? 1 : 0;
    if (live >= cap_) {
        Voice* victim = stealVictim();
        if (victim) voiceFadeOut(*victim, kStealMs * 0.001f * fs);
    }
    for (auto& v : voices_)
        if (!v.active()) return &v;
    // every ghost slot busy: drop the ghost that is closest to silence
    Voice* g = nullptr;
    float best = 2.0f;
    for (auto& v : voices_)
        if (v.state == VS_FADE && v.fade < best) {
            best = v.fade;
            g = &v;
        }
    if (g) g->state = VS_FREE;
    return g;
}

ORBIT_HOT bool EngineImpl::hit(int pad, int vel, float pitch, uint32_t offset)
{
    if (pad < 0 || pad >= kPads) return false;
    int layers[kLayers];
    const Sample* smps[kLayers];
    const int nl = selectLayers(pad, vel, layers, smps);
    if (pad == prj->pump_src && npump_ < kMaxPumpHits) pump_hits_[npump_++] = offset;
    if (nl == 0) return false;
    const PadCache& pc = pc_[pad];
    const uint32_t hit_id = ++hit_serial_;

    // choke: other pads of the same group fade out
    if (pc.choke) {
        for (auto& v : voices_)
            if (v.live() && v.pad != pad && pc_[v.pad].choke == pc.choke) voiceFadeOut(v, kChokeMs * 0.001f * fs);
    }
    // per-pad polyphony counts hits (a stacked hit is one)
    for (;;) {
        uint32_t seen[kVoicePool];
        int ns = 0;
        uint32_t oldest = 0xffffffffu;
        for (const auto& v : voices_) {
            if (!v.live() || v.pad != pad) continue;
            bool dup = false;
            for (int i = 0; i < ns; ++i) dup |= seen[i] == v.hit;
            if (!dup) seen[ns++] = v.hit;
            if (v.hit < oldest) oldest = v.hit;
        }
        if (ns < pc.poly) break;
        for (auto& v : voices_)
            if (v.live() && v.pad == pad && v.hit == oldest) voiceFadeOut(v, kStealMs * 0.001f * fs);
    }

    const Pad& pd = prj->pad[pad];
    int started = 0;
    for (int i = 0; i < nl; ++i) {
        const Layer& L = pd.layer[layers[i]];
        Voice* v = allocVoice();
        if (!v) break;
        VoiceStart s;
        s.smp = smps[i];   // validated by selectLayers (L.slot may have changed since)
        s.pad = (uint8_t)pad;
        s.layer = (uint8_t)layers[i];
        s.vel = (uint8_t)vel;
        s.pitch = clampf(L.tune, -48.0f, 48.0f) + pitch;
        s.layer_gain = dbToLin(clampf(L.gain_db, -60.0f, 24.0f));
        s.layer_pan = clampf(L.pan, -1.0f, 1.0f);
        s.hit = hit_id;
        s.age = ++age_serial_;
        s.seed = rng_.next();
        if (voiceStart(*v, s, pc, vctx)) ++started;
        else v->state = VS_FREE;
    }
    if (started) {
        Event e;
        e.type = EvtType::PadHit;
        e.pad = (uint8_t)pad;
        e.vel = (uint8_t)vel;
        emit(e);
        tel.pad_hits.fetch_or(1u << pad, std::memory_order_relaxed);
    }
    return started > 0;
}

ORBIT_HOT void EngineImpl::renderVoices(int from, int to, int n)
{
    if (to <= from) return;
    for (auto& v : voices_) {
        if (!v.active()) continue;
        const PadCache& pc = pc_[v.pad];
        const int b = pc.bus;
        if (!bus_used_[b]) {
            memset(bus_l_[b], 0, sizeof(float) * (size_t)n);
            memset(bus_r_[b], 0, sizeof(float) * (size_t)n);
            memset(snd_l_[b], 0, sizeof(float) * (size_t)n);
            memset(snd_r_[b], 0, sizeof(float) * (size_t)n);
            bus_used_[b] = true;
        }
        const VoiceOut o = {bus_l_[b] + from, bus_r_[b] + from, snd_l_[b] + from, snd_r_[b] + from};
        voiceRender(v, pc, o, to - from, vctx);
    }
}

// ============================================================================ ACID BASS

ORBIT_HOT void EngineImpl::scheduleBass(const AcidEvent& e)
{
    if (!bass_.schedule(e)) return;   // queue full (16 events per block): dropped
    bass_queued_ = true;
    if (e.type == AcidEvtType::NoteOn) tel.bass_notes.fetch_add(1, std::memory_order_relaxed);
}

// The voice into bus GP_BASS_BUS (pre-fader, like a pad's voices) and that bus's FX send buffer.
ORBIT_HOT void EngineImpl::renderBass(int n)
{
    if (!bass_queued_ && !bass_.active()) return;
    bass_queued_ = false;
    const Project& P = *prj;
    AcidParams ap;
    ap.tune = clampf(P.g[GP_BASS_TUNE], -12.0f, 12.0f);
    ap.cutoff = clampf(P.g[GP_BASS_CUTOFF], 0.0f, 1.0f);
    ap.reso = clampf(P.g[GP_BASS_RESO], 0.0f, 1.0f);
    ap.envmod = clampf(P.g[GP_BASS_ENVMOD], 0.0f, 1.0f);
    ap.decay = clampf(P.g[GP_BASS_DECAY], 0.0f, 1.0f);
    ap.accent = clampf(P.g[GP_BASS_ACCENT], 0.0f, 1.0f);
    ap.drive = clampf(P.g[GP_BASS_DRIVE], 0.0f, 1.0f);
    const float ldb = clampf(P.g[GP_BASS_LEVEL], -60.0f, 6.0f);
    ap.level = ldb <= -59.9f ? 0.0f : dbToLin(ldb);
    ap.wave = P.g[GP_BASS_WAVE] >= 0.5f ? 1 : 0;
    memset(bass_buf_, 0, sizeof(float) * (size_t)n);
    bass_.render(bass_buf_, n, ap);
    const int b = clampi((int)(P.g[GP_BASS_BUS] + 0.5f), 0, kBuses - 1);
    if (!bus_used_[b]) {
        memset(bus_l_[b], 0, sizeof(float) * (size_t)n);
        memset(bus_r_[b], 0, sizeof(float) * (size_t)n);
        memset(snd_l_[b], 0, sizeof(float) * (size_t)n);
        memset(snd_r_[b], 0, sizeof(float) * (size_t)n);
        bus_used_[b] = true;
    }
    const float send = clampf(P.g[GP_BASS_SEND], 0.0f, 1.0f);
    float* bl = bus_l_[b];
    float* br = bus_r_[b];
    float* sl = snd_l_[b];
    float* sr = snd_r_[b];
    for (int i = 0; i < n; ++i) {
        const float x = bass_buf_[i];
        bl[i] += x;
        br[i] += x;
        sl[i] += x * send;
        sr[i] += x * send;
    }
}

// ============================================================================ master

ORBIT_HOT void EngineImpl::master(float* l, float* r, int n, const SeqNotice& notice)
{
    const Project& P = *prj;
    const float block_s = (float)n / fs;
    const size_t bytes = sizeof(float) * (size_t)n;
    memset(mix_l_, 0, bytes);
    memset(mix_r_, 0, bytes);
    memset(ex_l_, 0, bytes);
    memset(ex_r_, 0, bytes);
    memset(fx_l_, 0, bytes);
    memset(fx_r_, 0, bytes);
    memset(all_l_, 0, bytes);
    memset(all_r_, 0, bytes);

    bool any_solo = false;
    for (int b = 0; b < kBuses; ++b) any_solo |= P.bus[b].solo != 0;
    const int pump_bus = P.pump_src < kPads ? pc_[P.pump_src].bus : -1;

    // buses: fader, pan, mute/solo, peak; FX sends gated by mute/solo (post pad fader)
    for (int b = 0; b < kBuses; ++b) {
        const Bus& B = P.bus[b];
        const float gate = (!B.mute && (!any_solo || B.solo)) ? 1.0f : 0.0f;
        float pl, pr;
        panBalance(B.pan, pl, pr);
        const float g = busGain(B.level) * gate;
        bus_gl_[b].begin(g * pl, n);
        bus_gr_[b].begin(g * pr, n);
        bus_send_[b].begin(gate, n);
        float peak = 0.0f;
        if (bus_used_[b]) {
            float* dl = (b == pump_bus) ? ex_l_ : mix_l_;
            float* dr = (b == pump_bus) ? ex_r_ : mix_r_;
            const float* bl = bus_l_[b];
            const float* br = bus_r_[b];
            const float* sl = snd_l_[b];
            const float* sr = snd_r_[b];
            for (int i = 0; i < n; ++i) {
                const float gl = bus_gl_[b].next(), gr = bus_gr_[b].next(), gs = bus_send_[b].next();
                const float ol = bl[i] * gl, orr = br[i] * gr;
                dl[i] += ol;
                dr[i] += orr;
                peak = maxf(peak, maxf(absf(ol), absf(orr)));
                fx_l_[i] += sl[i] * gs;
                fx_r_[i] += sr[i] * gs;
                all_l_[i] += bl[i] * gs;
                all_r_[i] += br[i] * gs;
            }
        }
        bus_gl_[b].end();
        bus_gr_[b].end();
        bus_send_[b].end();
        tel.bus_peak[b].store(peak, std::memory_order_relaxed);
    }

    // FX throw (song FX lane while FOLLOW plays) and SPACE macro
    const int slot = seq_.songSlot();
    const bool throw_on = P.song.follow && slot >= 0 && slot < kSongSlots && P.song.fx[slot];
    const float thr0 = throw_, sp0 = space_amt_;
    if (n != coef_n_) {   // per-block smoothing coefficients: libm only when the block length changes
        coef_n_ = n;
        k40_ = 1.0f - expf(-block_s / 0.04f);
        k20_ = 1.0f - expf(-block_s / 0.02f);
    }
    const float k40 = k40_;
    throw_ += k40 * ((throw_on ? 1.0f : 0.0f) - throw_);
    if (throw_ < 1.0e-4f && !throw_on) throw_ = 0.0f;
    const float sp_target = P.space_on ? clampf(P.g[GP_SPACE_AMT], 0.0f, 1.0f) : 0.0f;
    space_amt_ += k40 * (sp_target - space_amt_);
    if (absf(space_amt_ - sp_target) < 1.0e-4f) space_amt_ = sp_target;
    {
        const float inv = 1.0f / (float)n;
        const float dthr = (throw_ - thr0) * inv, dsp = (space_amt_ - sp0) * inv;
        float t = thr0, s = sp0;
        for (int i = 0; i < n; ++i) {
            t += dthr;
            s += dsp;
            fx_l_[i] = fx_l_[i] * (1.0f + t) + 0.35f * t * all_l_[i];
            fx_r_[i] = fx_r_[i] * (1.0f + t) + 0.35f * t * all_r_[i];
            space_[i] = s * 0.5f * (all_l_[i] + all_r_[i]);
        }
    }

    // FX bus
    const float bpm = clampf(P.bpm, 20.0f, 400.0f);
    const int div = clampi((int)(P.g[GP_DLY_DIV] + 0.5f), 0, (int)SyncDiv::Count - 1);
    fxp_.delay_samples = syncDivBeats((SyncDiv)div) * (60.0f / bpm) * fs;
    fxp_.dly_feedback = clampf(P.g[GP_DLY_FEEDBACK], 0.0f, 0.95f);
    fxp_.dly_tone = clampf(P.g[GP_DLY_TONE], 0.0f, 1.0f);
    fxp_.dly_level = clampf(P.g[GP_DLY_LEVEL], 0.0f, 1.0f);
    fxp_.dly_pingpong = P.g[GP_DLY_PINGPONG] >= 0.5f;
    fxp_.rev_size = clampf(P.g[GP_REV_SIZE], 0.0f, 1.0f);
    fxp_.rev_decay = clampf(P.g[GP_REV_DECAY], 0.2f, 12.0f);
    fxp_.rev_damp = clampf(P.g[GP_REV_DAMP], 0.0f, 1.0f);
    fxp_.rev_predelay_ms = clampf(P.g[GP_REV_PREDELAY], 0.0f, 200.0f);
    fxp_.rev_level = minf(1.0f, clampf(P.g[GP_REV_LEVEL], 0.0f, 1.0f) * (1.0f + 0.5f * throw_));
    fx_.process(fx_l_, fx_r_, space_, n, fxp_, ret_l_, ret_r_);

    // PUMP: duck everything except the source pad's bus
    const float pd_target = P.pump_on ? clampf(P.g[GP_PUMP_AMT], 0.0f, 1.0f) : 0.0f;
    pump_depth_ += k20_ * (pd_target - pump_depth_);
    if (absf(pump_depth_ - pd_target) < 1.0e-4f) pump_depth_ = pd_target;
    pump_.process(pump_g_, n, pump_hits_, npump_, pump_depth_, clampf(P.g[GP_PUMP_RELEASE], 20.0f, 800.0f), fs);
    for (int i = 0; i < n; ++i) {
        const float g = pump_g_[i];
        l[i] = (mix_l_[i] + ret_l_[i]) * g + ex_l_[i];
        r[i] = (mix_r_[i] + ret_r_[i]) * g + ex_r_[i];
    }

    // LO-FI
    const float lm0 = lofi_mix_;
    lofi_mix_ = P.lofi_on ? clampf(P.g[GP_LOFI_MIX], 0.0f, 1.0f) : 0.0f;
    lofi_.process(l, r, n, clampf(P.g[GP_LOFI_BITS], 4.0f, 16.0f), clampf(P.g[GP_LOFI_RATE], 500.0f, 96000.0f), lm0,
                  lofi_mix_, fs);

    // WIDE
    const float wa0 = wide_amt_;
    wide_amt_ += k20_ * ((P.wide_on ? 1.0f : 0.0f) - wide_amt_);
    if (wide_amt_ < 1.0e-4f && !P.wide_on) wide_amt_ = 0.0f;
    if (wide_amt_ > 0.9999f && P.wide_on) wide_amt_ = 1.0f;
    wide_.process(l, r, n, wa0, wide_amt_, P.g[GP_WIDE]);

    // preview, MASTER gain, limiter
    if (prev_used_) {
        for (int i = 0; i < n; ++i) {
            l[i] += prev_l_[i];
            r[i] += prev_r_[i];
        }
    }
    const float mdb = clampf(P.g[GP_MASTER], -60.0f, 6.0f);
    master_g_.begin(mdb <= -59.9f ? 0.0f : dbToLin(mdb), n);
    for (int i = 0; i < n; ++i) {
        const float g = master_g_.next();
        l[i] *= g;
        r[i] *= g;
    }
    master_g_.end();
    const float gmin = lim_.process(l, r, n, P.limiter_on != 0);
    // -20 log10(g) = -6.0206 log2(g); no libm (flash) on every limiting block
    const float gr = gmin >= 0.99999f ? 0.0f : (gmin > 1.0e-6f ? -6.02059991f * fastLog2(gmin) : 120.0f);
    tel.gain_reduction.store(gr, std::memory_order_relaxed);

    // metronome after the limiter, delayed by its latency so it lines up with the audio
    const bool metro = metro_override_ >= 0 ? metro_override_ != 0 : P.metronome != 0;
    if (notice.metronome_click && metro)
        click_.trigger((int32_t)notice.click_offset + lim_.latency(), notice.metronome_accent,
                       clampf(P.g[GP_METRO_LEVEL], 0.0f, 1.0f), fs);
    click_.process(l, r, n);

    float pkl = 0.0f, pkr = 0.0f;
    for (int i = 0; i < n; ++i) {
        pkl = maxf(pkl, absf(l[i]));
        pkr = maxf(pkr, absf(r[i]));
    }
    tel.master_peak[0].store(pkl, std::memory_order_relaxed);
    tel.master_peak[1].store(pkr, std::memory_order_relaxed);
}

// ============================================================================ telemetry

ORBIT_HOT void EngineImpl::updateTelemetry(const SeqNotice& notice, int n)
{
    (void)n;
    const bool playing = seq_.playing();
    tel.playing.store(playing ? 1 : 0, std::memory_order_relaxed);
    tel.recording.store(rec_armed_ ? 1 : 0, std::memory_order_relaxed);
    tel.step.store((int16_t)seq_.step(), std::memory_order_relaxed);
    tel.scene.store((int8_t)seq_.scene(), std::memory_order_relaxed);
    tel.queued_scene.store((int8_t)seq_.queuedScene(), std::memory_order_relaxed);
    tel.song_slot.store((int8_t)seq_.songSlot(), std::memory_order_relaxed);
    tel.loop_count.store(seq_.loopCount(), std::memory_order_relaxed);
    tel.beat.store(seq_.beatsF(), std::memory_order_relaxed);   // float only: no soft double here

    float env[kPads] = {};
    int count = 0;
    const int watch = tel.watch_pad.load(std::memory_order_relaxed);
    const Voice* newest = nullptr;
    for (auto& v : voices_) {
        if (!v.active()) continue;
        ++count;
        if (v.level() > env[v.pad]) env[v.pad] = v.level();
        if (v.pad == watch && (!newest || v.age > newest->age)) newest = &v;
        voiceFlush(v);
    }
    for (int p = 0; p < kPads; ++p) tel.pad_env[p].store(minf(env[p], 1.0f), std::memory_order_relaxed);
    tel.voices.store((uint8_t)(count > 255 ? 255 : count), std::memory_order_relaxed);
    tel.play_pos.store(newest ? voicePosition(*newest) : -1.0f, std::memory_order_relaxed);

    if (notice.scene_changed) {
        Event e;
        e.type = EvtType::SceneChanged;
        e.scene = (int8_t)seq_.scene();
        emit(e);
    }
    if (notice.song_slot_changed) {
        Event e;
        e.type = EvtType::SongSlotChanged;
        e.step = (int16_t)seq_.songSlot();
        e.scene = (int8_t)seq_.scene();
        emit(e);
    }
    if (notice.transport_changed || playing != last_playing_) {
        Event e;
        e.type = EvtType::TransportChanged;
        e.pad = playing ? 1 : 0;
        e.step = (int16_t)seq_.step();
        e.scene = (int8_t)seq_.scene();
        emit(e);
    }
    last_playing_ = playing;
}

// ============================================================================ block

ORBIT_HOT void EngineImpl::process(float* l, float* r, int n)
{
    if (n <= 0) return;
    if (!inited || !prj) {
        memset(l, 0, sizeof(float) * (size_t)n);
        memset(r, 0, sizeof(float) * (size_t)n);
        return;
    }
    while (n > 0) {
        const int m = n > kRenderBlock ? kRenderBlock : n;
        processBlock(l, r, m);
        l += m;
        r += m;
        n -= m;
    }
}

ORBIT_HOT void EngineImpl::processBlock(float* l, float* r, int n)
{
    updatePadCache();
    drainCommands();
    governor(n);

    SeqNotice notice;
    int ns = seq_.process(n, strig_, kMaxSeqTrig, notice);
    if (ns < 0) ns = 0;
    if (ns > kMaxSeqTrig) ns = kMaxSeqTrig;
    // ACID BASS: the bass line's notes of this block; a stop closes a held (slid) note. A note held on the BASS
    // keys owns the voice: the line's notes (and the stop's note-off) wait until it is released.
    if (notice.transport_changed && !seq_.playing() && !bass_live_) {
        AcidEvent off;
        off.type = AcidEvtType::NoteOff;
        off.note = kAcidAnyNote;
        scheduleBass(off);
    }
    for (int i = 0; i < notice.bass_count && !bass_live_; ++i) {
        AcidEvent e = notice.bass[i];
        if (e.offset >= (uint32_t)n) e.offset = (uint32_t)n - 1;
        scheduleBass(e);
    }

    // live hits while recording: quantized into the playing scene by the UI
    if (rec_armed_ && seq_.playing()) {
        for (int i = 0; i < nlive_; ++i) {
            if (live_[i].type != LV_TRIGGER) continue;
            int sc = 0, st = 0, nu = 0;
            if (!seq_.quantize(0, live_[i].pad, sc, st, nu)) continue;   // st: the pad's track step
            Event e;
            e.type = EvtType::RecordHit;
            e.pad = live_[i].pad;
            e.vel = live_[i].vel;
            e.scene = (int8_t)sc;
            e.step = (int16_t)st;
            e.nudge = (int8_t)nu;
            if (!reliable(e)) emit(e);
        }
    }

    for (auto& u : bus_used_) u = false;
    npump_ = 0;

    // live commands act at the block start, in arrival order
    for (int i = 0; i < nlive_; ++i) {
        const LiveEvt& e = live_[i];
        if (e.type == LV_TRIGGER) {
            hit(e.pad, e.vel, e.pitch, 0);
        } else {
            for (auto& v : voices_)
                if (v.live() && v.pad == e.pad) voiceRelease(v, pc_[e.pad]);
        }
    }
    nlive_ = 0;

    // sequencer hits at their sample offsets
    int pos = 0;
    for (int k = 0; k < ns; ++k) {
        int off = (int)strig_[k].offset;
        if (off >= n) off = n - 1;
        if (off < pos) off = pos;
        if (off > pos) {
            renderVoices(pos, off, n);
            pos = off;
        }
        hit(strig_[k].pad, clampi(strig_[k].vel, 1, 127), (float)strig_[k].pitch, (uint32_t)off);
    }
    renderVoices(pos, n, n);
    renderBass(n);
    renderPreview(n);
    master(l, r, n, notice);
    updateTelemetry(notice, n);
    retireCheck();
    flushOutbox();
}

ORBIT_HOT void EngineImpl::processI16(int16_t* lr, int n)
{
    while (n > 0) {
        const int m = n > kRenderBlock ? kRenderBlock : n;
        process(tmp_l_, tmp_r_, m);
        for (int i = 0; i < m; ++i) {
            // TPDF dither (+-1 LSB), round, clip
            const float d0 = dither_.uni() - dither_.uni();
            const float d1 = dither_.uni() - dither_.uni();
            int a = floorToInt(tmp_l_[i] * 32767.0f + d0 + 0.5f);
            int b = floorToInt(tmp_r_[i] * 32767.0f + d1 + 0.5f);
            a = clampi(a, -32768, 32767);
            b = clampi(b, -32768, 32767);
            lr[2 * i] = (int16_t)a;
            lr[2 * i + 1] = (int16_t)b;
        }
        lr += 2 * m;
        n -= m;
    }
}

// ============================================================================ Engine (forwarding)

namespace {
Telemetry& dummyTelemetry()
{
    static Telemetry t;
    return t;
}
}  // namespace

Engine::Engine() : impl_(nullptr)
{
    const size_t bytes = sizeof(EngineImpl) + alignof(EngineImpl);
    bool fast = true;
    void* raw = fastAlloc(bytes);
    if (!raw) {
        fast = false;
        raw = bigAlloc(bytes);
        // the heap logs of app_main read against this: the audio state is not in internal RAM
        logf("engine: %u B state in PSRAM (no internal RAM)%s", (unsigned)bytes, raw ? "" : " - FAILED");
    }
    if (!raw) return;
    uintptr_t p = (uintptr_t)raw;
    p = (p + alignof(EngineImpl) - 1) & ~(uintptr_t)(alignof(EngineImpl) - 1);
    impl_ = new ((void*)p) EngineImpl();
    impl_->raw = raw;
    impl_->raw_fast = fast;
    for (auto& s : impl_->slot_pub) s.store(nullptr, std::memory_order_relaxed);
}

Engine::~Engine()
{
    if (!impl_) return;
    void* raw = impl_->raw;
    const bool fast = impl_->raw_fast;
    impl_->~EngineImpl();
    if (fast) fastFree(raw);
    else bigFree(raw);
    impl_ = nullptr;
}

bool Engine::init(int sample_rate, const Project* project)
{
    if (!impl_ || !project || sample_rate < 8000 || sample_rate > 192000) return false;
    return impl_->init(sample_rate, project);
}

int Engine::sampleRate() const { return impl_ ? impl_->rate : 0; }

ORBIT_HOT void Engine::process(float* left, float* right, int n)
{
    if (!impl_) {
        for (int i = 0; i < n; ++i) left[i] = right[i] = 0.0f;
        return;
    }
    impl_->process(left, right, n);
}

ORBIT_HOT void Engine::processI16(int16_t* lr, int n)
{
    if (!impl_) {
        for (int i = 0; i < 2 * n; ++i) lr[i] = 0;
        return;
    }
    impl_->processI16(lr, n);
}

ORBIT_HOT void Engine::reportTiming(uint32_t render_us, uint32_t block_us)
{
    if (!impl_ || block_us == 0) return;
    const float ratio = (float)render_us / (float)block_us;
    impl_->cpu_s += 0.1f * (ratio - impl_->cpu_s);
    impl_->tel.cpu.store(impl_->cpu_s, std::memory_order_relaxed);
}

ORBIT_HOT void Engine::reportXrun()
{
    if (impl_) impl_->tel.xruns.fetch_add(1, std::memory_order_relaxed);
}

bool Engine::post(Producer from, const Command& c)
{
    const int q = (int)from;
    if (!impl_ || q < 0 || q >= (int)Producer::Count) return false;
    return impl_->cmdq[q].push(c);
}

bool Engine::poll(Event& e) { return impl_ ? impl_->evq.pop(e) : false; }

Telemetry& Engine::telemetry() { return impl_ ? impl_->tel : dummyTelemetry(); }

const Sample* Engine::slot(int i) const
{
    if (!impl_ || i < 0 || i >= kMaxSamples) return nullptr;
    return impl_->slot_pub[i].load(std::memory_order_acquire);
}

void Engine::setVoiceLimit(int n)
{
    if (!impl_) return;
    impl_->user_cap.store((uint8_t)clampi(n, kMinVoiceLimit, kVoices), std::memory_order_relaxed);
}

int Engine::voiceLimit() const { return impl_ ? impl_->user_cap.load(std::memory_order_relaxed) : kVoices; }

}  // namespace orbit
