// kitgen — renders the built-in "808 STYLE" / "909 STYLE" kits (own synthesis) and writes:
//   sdcard/ORBIT/KITS/<kit>/NN_<Name>.wav + kit.txt     48 kHz mono 16-bit
//   firmware/main/core/kits_808.c, kits_909.c            embedded int16 arrays + sound tables
//   host/out/kit_demo_808.wav, kit_demo_909.wav           2-bar grooves for listening
// Usage: kitgen <project root>      (build + run: bash tools/kitgen/build.sh)
// Deterministic: fixed noise seeds, no time/date in the output.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "voices.h"

namespace kitgen {

float rmsWindow(const Buf& b, float t0, float t1)
{
    size_t i0 = (size_t)osFrames(t0), i1 = (size_t)osFrames(t1);
    if (i1 > b.size()) i1 = b.size();
    if (i1 <= i0) return 0.0f;
    float s = 0.0f;
    for (size_t i = i0; i < i1; ++i) s += b[i] * b[i];
    return sqrtf(s / (float)(i1 - i0));
}

Buf mixNorm(const Buf& a, float ga_db, const Buf& b, float gb_db, float win)
{
    const float ra = rmsWindow(a, 0.0f, win), rb = rmsWindow(b, 0.0f, win);
    const float ka = ra > 0.0f ? dbToLin(ga_db) / ra : 0.0f;
    const float kb = rb > 0.0f ? dbToLin(gb_db) / rb : 0.0f;
    Buf o(a.size() > b.size() ? a.size() : b.size(), 0.0f);
    for (size_t i = 0; i < a.size(); ++i) o[i] += ka * a[i];
    for (size_t i = 0; i < b.size(); ++i) o[i] += kb * b[i];
    return o;
}

Buf decimate(const Buf& in)
{
    // 383 taps, cutoff 20 kHz @ 192 kHz, Blackman-Harris: passband to ~18 kHz, > 90 dB rejection
    // from ~22 kHz (what folds below 20 kHz is attenuated by ~90 dB).
    constexpr int kTaps = 383, kMid = (kTaps - 1) / 2;
    static std::vector<float> h;
    if (h.empty()) {
        h.resize(kTaps);
        const float fc = 20000.0f / kFs;
        float sum = 0.0f;
        for (int k = 0; k < kTaps; ++k) {
            const float x = (float)(k - kMid);
            const float s = k == kMid ? 2.0f * fc : sinf(kTwoPi * fc * x) / (kPi * x);
            const float a = kTwoPi * (float)k / (float)(kTaps - 1);
            const float w = 0.35875f - 0.48829f * cosf(a) + 0.14128f * cosf(2.0f * a) - 0.01168f * cosf(3.0f * a);
            h[(size_t)k] = s * w;
            sum += h[(size_t)k];
        }
        for (float& v : h) v /= sum;
    }
    const int n = (int)in.size();
    Buf out((size_t)((n + kOs - 1) / kOs), 0.0f);
    for (size_t j = 0; j < out.size(); ++j) {
        const int c = (int)j * kOs + kMid;
        float acc = 0.0f;
        for (int k = 0; k < kTaps; ++k) {
            const int i = c - k;
            if (i >= 0 && i < n) acc += h[(size_t)k] * in[(size_t)i];
        }
        out[j] = acc;
    }
    return out;
}

}  // namespace kitgen

using namespace kitgen;

namespace {

constexpr int kRate = 48000;
constexpr int kGuard = 4;                       // must match core/sample.h
const float kKickPeak = dbToLin(-1.0f);         // kicks peak at -1 dBFS
const float kTrimThr = dbToLin(-70.0f);         // trailing silence threshold (re full scale)

struct Out {
    const SoundDef* def;
    std::vector<int16_t> pcm;    // SD card WAV
    std::vector<int16_t> embed;  // flash copy (== pcm unless embed_cap_s)
    float gain_db = 0.0f;
    bool peak_limited = false;
    float peak_db = 0.0f, rms_db = 0.0f;
};

// Max RMS over 30 ms windows (1 ms hop) at 48 kHz.
float shortRms(const Buf& x)
{
    const size_t w = 1440;
    float best = 0.0f;
    for (size_t s = 0; s < x.size(); s += 48) {
        size_t e = s + w < x.size() ? s + w : x.size();
        float acc = 0.0f;
        for (size_t i = s; i < e; ++i) acc += x[i] * x[i];
        best = fmaxf(best, sqrtf(acc / (float)w));
        if (e == x.size()) break;
    }
    return best;
}

void fadeOut(Buf& x, size_t end, size_t len)
{
    if (end > x.size()) end = x.size();
    if (len > end) len = end;
    const size_t s = end - len;
    for (size_t i = s; i < end; ++i) {
        const float u = (float)(i - s + 1) / (float)len;  // last sample -> exactly 0
        x[i] *= 0.5f + 0.5f * cosf(kPi * u);
    }
    x.resize(end);
}

std::vector<int16_t> toI16(const Buf& x)
{
    std::vector<int16_t> o(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        float v = roundf(x[i] * 32767.0f);
        o[i] = (int16_t)clampf(v, -32767.0f, 32767.0f);
    }
    return o;
}

// Renders one kit: decimate, DC block, ROM cap, level, trim, quantise.
std::vector<Out> renderKit(const SoundDef* defs)
{
    std::vector<Out> outs((size_t)kSoundsPerKit);
    std::vector<Buf> x((size_t)kSoundsPerKit);
    for (int p = 0; p < kSoundsPerKit; ++p) {
        const SoundDef& d = defs[p];
        Buf b = decimate(d.render());
        // DC blocker (1-pole HP ~5 Hz): the coupling capacitor of an analogue output.
        float prev_x = 0.0f, prev_y = 0.0f;
        const float r = 1.0f - kTwoPi * 5.0f / (float)kRate;
        for (float& v : b) {
            const float y = v - prev_x + r * prev_y;
            prev_x = v;
            prev_y = y;
            v = y;
        }
        if (d.cap_s > 0.0f) fadeOut(b, (size_t)(d.cap_s * (float)kRate), (size_t)(d.cap_fade_ms * 0.001f * (float)kRate));
        x[(size_t)p] = std::move(b);
    }
    // Kick (pad 0) is the level reference.
    const float ref = shortRms(x[0]) * kKickPeak / peakOf(x[0]);
    for (int p = 0; p < kSoundsPerKit; ++p) {
        const SoundDef& d = defs[p];
        Out& o = outs[(size_t)p];
        o.def = &d;
        Buf& b = x[(size_t)p];
        const float pk = peakOf(b);
        float g = kKickPeak / pk;
        if (!d.kick) {
            const float gr = ref * dbToLin(d.level_db) / shortRms(b);
            o.peak_limited = gr > g;
            if (gr < g) g = gr;
        }
        scale(b, g);
        o.gain_db = linToDb(g);
        // Trim trailing silence (< -70 dBFS) with a 5 ms fade.
        size_t last = 0;
        for (size_t i = 0; i < b.size(); ++i)
            if (fabsf(b[i]) >= kTrimThr) last = i;
        fadeOut(b, last + 1, (size_t)(0.005f * (float)kRate));
        o.peak_db = linToDb(peakOf(b));
        o.rms_db = linToDb(shortRms(b));
        o.pcm = toI16(b);
        if (d.embed_cap_s > 0.0f && b.size() > (size_t)(d.embed_cap_s * (float)kRate)) {
            Buf e = b;
            fadeOut(e, (size_t)(d.embed_cap_s * (float)kRate), (size_t)(0.25f * (float)kRate));
            o.embed = toI16(e);
        } else {
            o.embed = o.pcm;
        }
    }
    return outs;
}

void put16(FILE* f, uint16_t v)
{
    fputc(v & 0xff, f);
    fputc(v >> 8, f);
}
void put32(FILE* f, uint32_t v)
{
    put16(f, (uint16_t)(v & 0xffff));
    put16(f, (uint16_t)(v >> 16));
}

bool writeWav(const std::string& path, const std::vector<int16_t>& s)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "kitgen: nie mozna zapisac %s\n", path.c_str());
        return false;
    }
    const uint32_t data = (uint32_t)(s.size() * 2);
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);  // PCM
    put16(f, 1);  // mono
    put32(f, kRate);
    put32(f, kRate * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, data);
    for (int16_t v : s) put16(f, (uint16_t)v);
    fclose(f);
    return true;
}

std::string wavName(int pad, const SoundDef& d)
{
    char b[64];
    snprintf(b, sizeof b, "%02d_%s.wav", pad + 1, d.file);
    return b;
}

bool writeKitFolder(const std::string& dir, const std::vector<Out>& outs)
{
    std::string txt;
    for (int p = 0; p < kSoundsPerKit; ++p) {
        const std::string n = wavName(p, *outs[(size_t)p].def);
        if (!writeWav(dir + "/" + n, outs[(size_t)p].pcm)) return false;
        char line[96];
        snprintf(line, sizeof line, "%02d=%s\n", p + 1, n.c_str());
        txt += line;
    }
    FILE* f = fopen((dir + "/kit.txt").c_str(), "wb");
    if (!f) return false;
    fwrite(txt.data(), 1, txt.size(), f);
    fclose(f);
    return true;
}

bool writeC(const std::string& path, const char* tag, const char* kit_name, const std::vector<Out>& outs)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "kitgen: nie mozna zapisac %s\n", path.c_str());
        return false;
    }
    size_t bytes = 0;
    for (const Out& o : outs) bytes += (o.embed.size() + 2 * kGuard) * 2;
    fprintf(f,
            "/* Generated by tools/kitgen (bash tools/kitgen/build.sh). Do not edit by hand.\n"
            " * Built-in kit \"%s\": own synthesis from published circuit descriptions, no samples\n"
            " * of the original machines. 48000 Hz, mono, int16; %u bytes of PCM.\n"
            " * Each array: %d zero guard frames, the sound, %d zero guard frames (core/sample.h). */\n"
            "#include <stddef.h>\n#include <stdint.h>\n\n"
            "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
            "/* Same layout as orbit::BuiltinSound (core/kits.h); both sides assert the offsets. */\n"
            "typedef struct orbit_builtin_sound_c {\n"
            "    const char* name;\n    const char* long_name;\n    const int16_t* pcm;\n"
            "    uint32_t frames;\n    uint8_t choke;\n    uint8_t bus;\n} orbit_builtin_sound_c;\n\n"
            "#ifdef __cplusplus\n#define ORBIT_KIT_SA static_assert\n#else\n#define ORBIT_KIT_SA _Static_assert\n#endif\n"
            "ORBIT_KIT_SA(offsetof(orbit_builtin_sound_c, pcm) == 2 * sizeof(void*), \"layout\");\n"
            "ORBIT_KIT_SA(offsetof(orbit_builtin_sound_c, frames) == 3 * sizeof(void*), \"layout\");\n"
            "ORBIT_KIT_SA(offsetof(orbit_builtin_sound_c, choke) == 3 * sizeof(void*) + 4, \"layout\");\n"
            "ORBIT_KIT_SA(offsetof(orbit_builtin_sound_c, bus) == 3 * sizeof(void*) + 5, \"layout\");\n\n",
            kit_name, (unsigned)bytes, kGuard, kGuard);
    for (int p = 0; p < kSoundsPerKit; ++p) {
        const Out& o = outs[(size_t)p];
        fprintf(f, "/* %02d %s: %u frames (%.3f s) */\nstatic const int16_t %s_%02d[%u] = {\n", p + 1, o.def->long_name,
                (unsigned)o.embed.size(), (double)o.embed.size() / kRate, tag, p, (unsigned)(o.embed.size() + 2 * kGuard));
        std::vector<int16_t> a((size_t)kGuard, 0);
        a.insert(a.end(), o.embed.begin(), o.embed.end());
        a.insert(a.end(), (size_t)kGuard, 0);
        std::string line;
        char num[16];
        for (size_t i = 0; i < a.size(); ++i) {
            snprintf(num, sizeof num, "%d,", (int)a[i]);
            line += num;
            if ((i + 1) % 24 == 0 || i + 1 == a.size()) {
                fprintf(f, "%s\n", line.c_str());
                line.clear();
            }
        }
        fprintf(f, "};\n\n");
    }
    fprintf(f, "extern const orbit_builtin_sound_c orbit_%s_sounds[%d];\nconst orbit_builtin_sound_c orbit_%s_sounds[%d] = {\n",
            tag, kSoundsPerKit, tag, kSoundsPerKit);
    for (int p = 0; p < kSoundsPerKit; ++p) {
        const Out& o = outs[(size_t)p];
        fprintf(f, "    {\"%s\", \"%s\", %s_%02d + %d, %uu, %u, %u},\n", o.def->name, o.def->long_name, tag, p, kGuard,
                (unsigned)o.embed.size(), (unsigned)o.def->choke, (unsigned)o.def->bus);
    }
    fprintf(f, "};\n\n#ifdef __cplusplus\n}\n#endif\n");
    fclose(f);
    return true;
}

// ---- demo grooves ---------------------------------------------------------------------------

struct Hit {
    int step;  // 0..31 (2 bars of 1/16)
    int pad;
    int vel;   // 1..127
};

bool writeDemo(const std::string& path, const std::vector<Out>& outs, float bpm, float swing, const std::vector<Hit>& hits)
{
    const float step_s = 60.0f / bpm / 4.0f;
    auto hitTime = [&](int step) {
        float t = (float)step * step_s;
        if (step & 1) t += (swing - 0.5f) * 2.0f * step_s;
        return (size_t)(t * (float)kRate);
    };
    const size_t len = (size_t)((32.0f * step_s + 2.0f) * (float)kRate);
    Buf mix(len, 0.0f);
    for (size_t h = 0; h < hits.size(); ++h) {
        const Hit& x = hits[h];
        const Out& o = outs[(size_t)x.pad];
        const size_t t0 = hitTime(x.step);
        size_t n = o.pcm.size();
        // Choke group: the next hit of the same group cuts this voice (4 ms fade).
        if (o.def->choke) {
            size_t cut = (size_t)-1;
            for (const Hit& y : hits) {
                const size_t ty = hitTime(y.step);
                if (outs[(size_t)y.pad].def->choke == o.def->choke && ty > t0 && ty < cut) cut = ty;
            }
            if (cut != (size_t)-1 && cut - t0 < n) n = cut - t0;
        }
        const size_t fade = 192;
        const float g = (float)x.vel / 127.0f;
        for (size_t i = 0; i < n && t0 + i < len; ++i) {
            float v = (float)o.pcm[i] / 32767.0f * g;
            if (n < o.pcm.size() && i + fade > n) v *= (float)(n - i) / (float)fade;
            mix[t0 + i] += v;
        }
    }
    const float pk = peakOf(mix);
    if (pk > 0.0f) scale(mix, kKickPeak / pk);
    size_t last = 0;
    for (size_t i = 0; i < mix.size(); ++i)
        if (fabsf(mix[i]) >= kTrimThr) last = i;
    fadeOut(mix, last + 1, 240);
    return writeWav(path, toI16(mix));
}

void addSteps(std::vector<Hit>& v, int pad, std::initializer_list<int> steps, int vel)
{
    for (int s : steps) v.push_back({s, pad, vel});
}

std::vector<Hit> groove808()
{
    std::vector<Hit> h;
    addSteps(h, 9, {0}, 90);                                              // cymbal
    addSteps(h, 0, {0, 6, 10, 16, 22, 25, 27}, 120);                      // kick
    addSteps(h, 15, {30}, 110);                                           // long kick
    addSteps(h, 1, {4, 12, 20}, 112);                                     // snare
    addSteps(h, 4, {28}, 115);                                            // clap
    addSteps(h, 2, {0, 2, 4, 6, 8, 10, 12, 16, 18, 20, 22, 24, 26}, 80);  // closed hat
    addSteps(h, 2, {13, 15, 29, 31}, 60);
    addSteps(h, 3, {14, 30}, 85);                                         // open hat
    addSteps(h, 10, {3, 11, 19}, 75);                                     // cowbell
    addSteps(h, 11, {2, 10, 18, 26}, 70);                                 // claves
    addSteps(h, 12, {1, 5, 9, 13, 17, 21, 25, 29}, 55);                   // maracas
    addSteps(h, 13, {7, 15, 23}, 85);                                     // low conga
    addSteps(h, 14, {5, 13, 21}, 80);                                     // high conga
    addSteps(h, 5, {23}, 90);                                             // rim
    addSteps(h, 8, {28}, 95);                                             // toms fill
    addSteps(h, 7, {29}, 95);
    addSteps(h, 6, {31}, 100);
    return h;
}

std::vector<Hit> groove909()
{
    std::vector<Hit> h;
    addSteps(h, 9, {0}, 95);                                       // crash
    addSteps(h, 0, {0, 4, 8, 12, 16, 20, 24}, 120);                // kick
    addSteps(h, 15, {28}, 120);                                    // hard kick
    addSteps(h, 4, {4, 12, 20}, 110);                              // clap
    addSteps(h, 14, {28}, 110);                                    // room clap
    addSteps(h, 1, {12, 28}, 95);                                  // snare
    addSteps(h, 12, {29, 30, 31}, 70);                             // snap roll
    addSteps(h, 2, {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23}, 70);  // closed hat
    addSteps(h, 2, {0, 4, 8, 12, 16, 20, 24}, 95);
    addSteps(h, 3, {2, 6, 10, 14, 18, 22}, 95);                    // open hat
    addSteps(h, 13, {26}, 95);                                     // long open hat
    addSteps(h, 10, {16, 18, 20, 22, 24, 26, 28, 30}, 80);         // ride
    addSteps(h, 5, {7, 15, 23}, 85);                               // rim
    addSteps(h, 8, {25}, 95);                                      // toms
    addSteps(h, 7, {26}, 95);
    addSteps(h, 6, {27}, 100);
    addSteps(h, 11, {31}, 90);                                     // long kick pickup
    return h;
}

void report(const char* kit, const std::vector<Out>& outs)
{
    size_t bytes = 0, sd = 0;
    printf("%s\n pad name       frames   sec    peak dBFS  rms30 dBFS  gain dB  limit\n", kit);
    for (int p = 0; p < kSoundsPerKit; ++p) {
        const Out& o = outs[(size_t)p];
        bytes += (o.embed.size() + 2 * kGuard) * 2;
        sd += o.pcm.size() * 2 + 44;
        printf(" %2d  %-9s %7u  %5.3f  %7.2f    %7.2f    %+6.1f   %s%s\n", p + 1, o.def->name, (unsigned)o.pcm.size(),
               (double)o.pcm.size() / kRate, (double)o.peak_db, (double)o.rms_db, (double)o.gain_db,
               o.def->kick ? "kick" : (o.peak_limited ? "peak" : "rms"),
               o.embed.size() != o.pcm.size() ? " (embed capped)" : "");
    }
    printf(" embedded PCM: %u bytes, SD WAVs: %u bytes\n", (unsigned)bytes, (unsigned)sd);
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string root = argc > 1 ? argv[1] : ".";
    struct Job {
        const char* kit;
        const char* tag;
        const char* dir;
        const char* demo;
        const SoundDef* defs;
        float bpm, swing;
        std::vector<Hit> hits;
    };
    Job jobs[2] = {
        {"808 STYLE", "kit808", "808_STYLE", "kit_demo_808.wav", kit808(), 100.0f, 0.5f, groove808()},
        {"909 STYLE", "kit909", "909_STYLE", "kit_demo_909.wav", kit909(), 126.0f, 0.54f, groove909()},
    };
    size_t total = 0;
    for (Job& j : jobs) {
        std::vector<Out> outs = renderKit(j.defs);
        report(j.kit, outs);
        for (const Out& o : outs) total += (o.embed.size() + 2 * kGuard) * 2;
        const std::string tagfile = std::string(j.tag).substr(3);  // "808"
        if (!writeKitFolder(root + "/sdcard/ORBIT/KITS/" + j.dir, outs)) return 1;
        if (!writeC(root + "/firmware/main/core/kits_" + tagfile + ".c", j.tag, j.kit, outs)) return 1;
        if (!writeDemo(root + "/host/out/" + j.demo, outs, j.bpm, j.swing, j.hits)) return 1;
    }
    printf("total embedded PCM: %u bytes (%.2f MB)\n", (unsigned)total, (double)total / 1048576.0);
    printf("kitgen: gotowe\n");
    return 0;
}
