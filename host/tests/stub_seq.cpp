// Minimal Sequencer used ONLY by host/tests/test_engine.cpp while the real core/seq.cpp is
// being written. Straight 16ths from the playing scene (no swing, probability, ratchets or
// conditions), bar-quantized scene queue, metronome notices, quantize() for live recording.
#include <math.h>
#include <string.h>
#include "core/seq.h"

namespace orbit {

struct Sequencer::State {
    const Project* prj;
    int rate;
    int playing;
    int scene;
    int queued;
    int step;
    int fill;
    int pending_notice;    // transport changed since the last process()
    int pending_scene;
    uint32_t loops;
    uint32_t seed;
    double frame;          // frames since play()
    double block_start;    // frame where the last processed block started
    double next_step;      // frame of the next step
    double next_beat;      // frame of the next quarter note
    int beat_index;
    int step_count;        // steps since play()
};

Sequencer::State& Sequencer::st()
{
    static_assert(sizeof(State) <= sizeof(state_), "state too big");
    return *reinterpret_cast<State*>(state_);
}
const Sequencer::State& Sequencer::st() const { return *reinterpret_cast<const State*>(state_); }

static double stepFrames(const Project* p, int scene, int rate)
{
    const Scene& s = p->scene_data[scene];
    return (double)divisionBeats(s.div) * 60.0 / (double)p->bpm * (double)rate;
}
static double beatFrames(const Project* p, int rate) { return 60.0 / (double)p->bpm * (double)rate; }

void Sequencer::init(int sample_rate, const Project* project)
{
    memset(state_, 0, sizeof(state_));
    State& s = st();
    s.prj = project;
    s.rate = sample_rate;
    s.scene = project ? project->scene : 0;
    s.queued = -1;
    s.step = -1;
    s.seed = 1;
}

void Sequencer::setSampleRate(int sample_rate) { st().rate = sample_rate; }

void Sequencer::play()
{
    State& s = st();
    s.playing = 1;
    s.frame = 0.0;
    s.next_step = 0.0;
    s.next_beat = 0.0;
    s.beat_index = 0;
    s.step_count = 0;
    s.step = -1;
    s.loops = 0;
    if (s.queued >= 0) {
        s.scene = s.queued;
        s.queued = -1;
        s.pending_scene = 1;
    }
    s.pending_notice = 1;
}

void Sequencer::stop()
{
    State& s = st();
    if (!s.playing) {
        s.frame = 0.0;
        s.step = -1;
        s.step_count = 0;
    }
    s.playing = 0;
    s.pending_notice = 1;
}

void Sequencer::resume()
{
    State& s = st();
    if (!s.playing) {
        s.playing = 1;
        s.pending_notice = 1;
    }
}

void Sequencer::locate(int step)
{
    State& s = st();
    s.step_count = step;
    s.next_step = s.frame;
}

void Sequencer::queueScene(int scene)
{
    State& s = st();
    if (scene < 0 || scene >= kScenes) return;
    if (!s.playing) {
        if (s.scene != scene) s.pending_scene = 1;
        s.scene = scene;
        s.queued = -1;
    } else {
        s.queued = scene;
    }
}

void Sequencer::setFill(bool on) { st().fill = on ? 1 : 0; }

int Sequencer::process(int n, SeqTrigger* out, int max, SeqNotice& notice)
{
    State& s = st();
    notice = SeqNotice();
    if (s.pending_notice) notice.transport_changed = true;
    if (s.pending_scene) notice.scene_changed = true;
    s.pending_notice = 0;
    s.pending_scene = 0;
    if (!s.playing || !s.prj) return 0;
    const Project* p = s.prj;
    int count = 0;
    s.block_start = s.frame;
    const double end = s.frame + n;
    while (s.next_beat < end) {
        if (!notice.metronome_click) {
            notice.metronome_click = true;
            notice.metronome_accent = (s.beat_index % (p->beats_per_bar ? p->beats_per_bar : 4)) == 0;
            notice.click_offset = (uint32_t)(s.next_beat - s.frame);
        }
        ++s.beat_index;
        s.next_beat = s.beat_index * beatFrames(p, s.rate);
    }
    while (s.next_step < end) {
        const Scene& sc = p->scene_data[s.scene];
        const int len = sc.length ? sc.length : 16;
        int st_idx = s.step_count % len;
        if (st_idx == 0 && s.step_count > 0) {
            ++s.loops;
            if (s.queued >= 0) {   // bar-quantized switch (stub: at the scene loop point)
                s.scene = s.queued;
                s.queued = -1;
                notice.scene_changed = true;
            }
        }
        const Scene& cur = p->scene_data[s.scene];
        const uint32_t off = (uint32_t)(s.next_step - s.frame);
        bool any_solo = false;
        for (int t = 0; t < kPads; ++t) any_solo |= p->pad[t].solo != 0;
        for (int t = 0; t < kPads && count < max; ++t) {
            const Pad& pad = p->pad[t];
            if (pad.mute || (any_solo && !pad.solo)) continue;
            const int tl = trackLength(cur, t);
            const Step& stp = cur.track[t].step[s.step_count % tl];
            if (!stp.on) continue;
            out[count].pad = (uint8_t)t;
            out[count].vel = stp.vel;
            out[count].pitch = stp.pitch;
            out[count].offset = off < (uint32_t)n ? off : (uint32_t)(n - 1);
            ++count;
        }
        s.step = st_idx;
        ++s.step_count;
        s.next_step = s.step_count * stepFrames(p, s.scene, s.rate);
    }
    s.frame = end;
    return count;
}

bool Sequencer::playing() const { return st().playing != 0; }
int Sequencer::step() const { return st().playing ? st().step : -1; }
int Sequencer::scene() const { return st().scene; }
int Sequencer::queuedScene() const { return st().queued; }
int Sequencer::songSlot() const { return -1; }
uint32_t Sequencer::loopCount() const { return st().loops; }
double Sequencer::beats() const
{
    const State& s = st();
    return s.prj ? s.frame / beatFrames(s.prj, s.rate) : 0.0;
}

float Sequencer::beatsF() const { return (float)beats(); }

bool Sequencer::quantize(uint32_t offset, int pad, int& scene, int& step, int& nudge) const
{
    const State& s = st();
    if (!s.playing || !s.prj) return false;
    const double sf = stepFrames(s.prj, s.scene, s.rate);
    const double pos = (s.block_start + offset) / sf;
    const double r = floor(pos + 0.5);
    const int len = s.prj->scene_data[s.scene].length ? s.prj->scene_data[s.scene].length : 16;
    int k = (int)r % len;
    if (k < 0) k += len;
    scene = s.scene;
    if (pad >= 0 && pad < kPads) {   // the pad's track step (polymeter)
        const int tl = trackLength(s.prj->scene_data[s.scene], pad);
        step = (int)r % tl;
        if (step < 0) step += tl;
    } else {
        step = k;
    }
    nudge = (int)lround((pos - r) * 100.0);
    return true;
}

void Sequencer::seed(uint32_t v) { st().seed = v; }

}  // namespace orbit
