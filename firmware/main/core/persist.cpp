// Project files: JSON ("orbit-project", version 1) via cJSON. Loader task only.
//
// Compatibility rules: parameters are stored under stable snake_case keys (never the knob
// labels), enums by name. Loading starts from projectDefaults() and applies what it finds, so
// unknown keys are ignored and missing keys keep their defaults; every value is range-checked.
// Saving writes <path>.tmp and then replaces <path>, so a crash never leaves a half file.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "library.h"
#include "platform.h"

namespace orbit {

namespace {

constexpr int kFormatVersion = 1;
constexpr const char* kFormat = "orbit-project";
constexpr long kMaxFileBytes = 4L * 1024 * 1024;

// Stable keys, one per PadParam / GlobalParam, in enum order. Never rename a key: old files
// would silently fall back to the default. New parameters get new keys.
const char* const kPadKeys[] = {
    "start", "end", "fade_in", "fade_out", "gain", "loop",                                     // TRIM
    "attack", "hold", "decay", "sustain", "release", "vel_amp",                                // ENVELOPE
    "cutoff", "resonance", "drive", "filter_type", "filter_env", "filter_decay",               // FILTER
    "tune", "fine", "pitch_env", "pitch_decay", "bits", "downsample",                          // PITCH
    "lfo_rate", "lfo_depth", "lfo_shape", "lfo_target", "lfo_sync", "lfo_fade",                // LFO
    "volume", "pan", "send",                                                                   // MIX
};
static_assert(sizeof(kPadKeys) / sizeof(kPadKeys[0]) == kPadParamCount, "one key per PadParam");

const char* const kGlobalKeys[] = {
    "master", "rev_size", "rev_decay", "rev_damp", "rev_predelay", "rev_level", "dly_div",
    "dly_feedback", "dly_tone", "dly_level", "dly_pingpong", "lofi_bits", "lofi_rate", "lofi_mix",
    "wide", "pump_amount", "pump_release", "space_amount", "metro_level",
    "bass_tune", "bass_cutoff", "bass_reso", "bass_envmod", "bass_decay", "bass_accent", "bass_drive",
    "bass_level", "bass_wave", "bass_send", "bass_bus", "bass_mute",
};
static_assert(sizeof(kGlobalKeys) / sizeof(kGlobalKeys[0]) == kGlobalParamCount, "one key per GlobalParam");

const char* const kPlayModes[] = {"oneshot", "gate", "loop"};
static_assert(sizeof(kPlayModes) / sizeof(kPlayModes[0]) == (size_t)PlayMode::Count, "PlayMode names");
const char* const kLayerModes[] = {"single", "velocity", "round_robin", "random", "stack"};
static_assert(sizeof(kLayerModes) / sizeof(kLayerModes[0]) == (size_t)LayerMode::Count, "LayerMode names");
const char* const kQuants[] = {"bar", "beat", "eighth", "off"};
static_assert(sizeof(kQuants) / sizeof(kQuants[0]) == (size_t)LaunchQuant::Count, "LaunchQuant names");
const char* const kDivs[] = {"1/8", "1/16", "1/32", "1/8T", "1/16T"};
static_assert(sizeof(kDivs) / sizeof(kDivs[0]) == (size_t)Division::Count, "Division names");

// A dense project is tens of thousands of small cJSON nodes. Plain malloc on the Tab5 prefers
// internal RAM for small blocks and could drain it (audio, LVGL, WiFi need it), so cJSON
// allocates from bigAlloc (PSRAM) while a project is saved or loaded. heap_caps_free() frees
// any heap, so a cJSON user elsewhere that overlaps with this stays safe on the device.
void* jsonAlloc(size_t n) { return bigAlloc(n); }
void jsonFree(void* p) { bigFree(p); }
struct JsonHeap {
    JsonHeap()
    {
        cJSON_Hooks h;
        h.malloc_fn = jsonAlloc;
        h.free_fn = jsonFree;
        cJSON_InitHooks(&h);
    }
    ~JsonHeap() { cJSON_InitHooks(nullptr); }   // back to malloc/free
};

void setErr(char* err, int errlen, const char* msg)
{
    if (!err || errlen <= 0) return;
    snprintf(err, (size_t)errlen, "%s", msg);
}

// UTF-8 safe bounded copy, zero-filled (so saved/loaded structs compare equal).
void copyUtf8(char* dst, int cap, const char* src)
{
    memset(dst, 0, (size_t)cap);
    if (!src) return;
    size_t n = strlen(src);
    if (n > (size_t)cap - 1) {
        n = (size_t)cap - 1;
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) --n;
    }
    memcpy(dst, src, n);
}

// Shortest decimal that reads back as the same float (0.7071f -> 0.7071, not 0.70709997...).
double tidy(float v)
{
    if (!(v == v) || v > 3.0e38f || v < -3.0e38f) return 0.0;
    if (v == 0.0f) return 0.0;
    char b[40];
    for (int prec = 6; prec <= 9; ++prec) {
        snprintf(b, sizeof(b), "%.*g", prec, (double)v);
        if ((float)strtod(b, nullptr) == v) break;
    }
    return strtod(b, nullptr);
}

// ------------------------------------------------------------ writer
struct Writer {
    bool ok = true;

    cJSON* track(cJSON* item)
    {
        if (!item) ok = false;
        return item;
    }
    cJSON* obj(cJSON* parent, const char* key)
    {
        cJSON* o = cJSON_CreateObject();
        attach(parent, key, o);
        return o;
    }
    cJSON* arr(cJSON* parent, const char* key)
    {
        cJSON* a = cJSON_CreateArray();
        attach(parent, key, a);
        return a;
    }
    void attach(cJSON* parent, const char* key, cJSON* item)
    {
        if (!item || !parent) {
            ok = false;
            cJSON_Delete(item);
            return;
        }
        const bool added = key ? cJSON_AddItemToObject(parent, key, item) : cJSON_AddItemToArray(parent, item);
        if (!added) {
            ok = false;
            cJSON_Delete(item);
        }
    }
    void num(cJSON* o, const char* key, double v) { attach(o, key, cJSON_CreateNumber(v)); }
    void f(cJSON* o, const char* key, float v) { num(o, key, tidy(v)); }
    void i(cJSON* o, const char* key, int v) { num(o, key, (double)v); }
    void b(cJSON* o, const char* key, bool v) { attach(o, key, cJSON_CreateBool(v ? 1 : 0)); }
    void s(cJSON* o, const char* key, const char* v) { attach(o, key, cJSON_CreateString(v ? v : "")); }
};

void writeRef(Writer& w, cJSON* layer, const SampleRef& r)
{
    if (!r.path[0]) return;
    if (strncmp(r.path, "builtin:", 8) == 0) w.s(layer, "builtin", r.path + 8);
    else w.s(layer, "path", r.path);
    if (r.name[0]) w.s(layer, "name", r.name);
}

void writePad(Writer& w, cJSON* o, const Pad& p, const SampleRef* refs)
{
    w.s(o, "name", p.name);
    w.s(o, "layer_mode", (size_t)p.layer_mode < (size_t)LayerMode::Count ? kLayerModes[(int)p.layer_mode] : kLayerModes[0]);
    w.s(o, "mode", (size_t)p.mode < (size_t)PlayMode::Count ? kPlayModes[(int)p.mode] : kPlayModes[0]);
    w.b(o, "reverse", p.reverse != 0);
    w.i(o, "bus", p.bus);
    w.i(o, "choke", p.choke);
    w.i(o, "poly", p.poly);
    w.b(o, "mute", p.mute != 0);
    w.b(o, "solo", p.solo != 0);
    w.i(o, "slice_count", p.slice_count);
    w.i(o, "slice_sel", p.slice_sel);
    // Slice markers (+ the stored region end after the last one), trailing zeros dropped.
    int last = kMaxSlices - 1;
    while (last >= 0 && p.slices[last] == 0.0f) --last;
    cJSON* sl = w.arr(o, "slices");
    for (int i = 0; i <= last; ++i) w.attach(sl, nullptr, cJSON_CreateNumber(tidy(p.slices[i])));
    w.f(o, "slice_end", p.slice_end);
    cJSON* prm = w.obj(o, "params");
    for (int k = 0; k < kPadParamCount; ++k) w.f(prm, kPadKeys[k], p.p[k]);
    cJSON* layers = w.arr(o, "layers");
    for (int l = 0; l < kLayers; ++l) {
        cJSON* lo = cJSON_CreateObject();
        w.attach(layers, nullptr, lo);
        if (!lo) continue;
        if (refs) writeRef(w, lo, refs[l]);
        const Layer& L = p.layer[l];
        w.f(lo, "tune", L.tune);
        w.f(lo, "gain_db", L.gain_db);
        w.f(lo, "pan", L.pan);
        w.i(lo, "vel_lo", L.vel_lo);
        w.i(lo, "vel_hi", L.vel_hi);
    }
}

void writeScene(Writer& w, cJSON* o, const Scene& sc)
{
    w.i(o, "length", sc.length);
    w.s(o, "div", (size_t)sc.div < (size_t)Division::Count ? kDivs[(int)sc.div] : kDivs[1]);
    w.i(o, "swing", sc.swing);
    cJSON* tracks = w.arr(o, "tracks");
    const Step def{};
    for (int t = 0; t < kPads; ++t) {
        const Track& tr = sc.track[t];
        cJSON* to = cJSON_CreateObject();
        w.attach(tracks, nullptr, to);
        if (!to) continue;
        w.i(to, "length", tr.length);
        cJSON* steps = w.arr(to, "steps");
        for (int i = 0; i < kMaxSteps; ++i) {
            const Step& st = tr.step[i];
            // Active steps, and OFF steps whose lane values were edited (velocity, probability,
            // ratchet... are kept while a step is off, so they must survive save/open too).
            // Untouched off steps are omitted. Off steps carry "o": 0 (older files: always on).
            if (!st.on && st.vel == def.vel && st.prob == def.prob && st.ratchet == def.ratchet &&
                st.nudge == def.nudge && st.pitch == def.pitch && st.cond == def.cond && st.flam == def.flam)
                continue;
            cJSON* so = cJSON_CreateObject();
            w.attach(steps, nullptr, so);
            if (!so) continue;
            w.i(so, "i", i);
            if (!st.on) w.i(so, "o", 0);
            w.i(so, "v", st.vel);
            if (st.prob != def.prob) w.i(so, "p", st.prob);
            if (st.ratchet != def.ratchet) w.i(so, "r", st.ratchet);
            if (st.nudge != def.nudge) w.i(so, "n", st.nudge);
            if (st.pitch != def.pitch) w.i(so, "t", st.pitch);
            if (st.cond != def.cond) w.i(so, "c", (int)st.cond);
            if (st.flam != def.flam) w.i(so, "f", st.flam);
        }
    }
    // ACID BASS (0.3): steps that play or were edited (a resting step keeps its note, accent and slide).
    cJSON* bo = w.obj(o, "bass");
    cJSON* bsteps = w.arr(bo, "steps");
    for (int i = 0; i < kMaxSteps; ++i) {
        const BassStep& b = sc.bass.step[i];
        if (!b.gate && !b.note && !b.accent && !b.slide) continue;
        cJSON* so = cJSON_CreateObject();
        w.attach(bsteps, nullptr, so);
        if (!so) continue;
        w.i(so, "i", i);
        if (!b.gate) w.i(so, "o", 0);
        if (b.note) w.i(so, "n", b.note);
        if (b.accent) w.i(so, "a", 1);
        if (b.slide) w.i(so, "s", 1);
    }
}

cJSON* buildJson(const Project& p, const SampleRef refs[kPads][kLayers], bool exported, bool& ok)
{
    Writer w;
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        ok = false;
        return nullptr;
    }
    w.s(root, "format", kFormat);
    w.i(root, "version", kFormatVersion);
    if (exported) w.b(root, "exported", true);
    w.s(root, "app", "ORBIT DRUM");
    w.s(root, "name", p.name);
    w.f(root, "bpm", p.bpm);
    w.i(root, "scene", p.scene);
    w.i(root, "beats_per_bar", p.beats_per_bar);
    w.s(root, "quant", (size_t)p.quant < (size_t)LaunchQuant::Count ? kQuants[(int)p.quant] : kQuants[0]);
    w.b(root, "metronome", p.metronome != 0);
    w.i(root, "count_in", p.count_in);
    cJSON* mac = w.obj(root, "macros");
    w.b(mac, "lofi", p.lofi_on != 0);
    w.b(mac, "wide", p.wide_on != 0);
    w.b(mac, "pump", p.pump_on != 0);
    w.b(mac, "space", p.space_on != 0);
    w.b(root, "limiter", p.limiter_on != 0);
    w.i(root, "pump_src", p.pump_src);

    cJSON* g = w.obj(root, "globals");
    for (int k = 0; k < kGlobalParamCount; ++k) w.f(g, kGlobalKeys[k], p.g[k]);

    cJSON* buses = w.arr(root, "buses");
    for (int b = 0; b < kBuses; ++b) {
        cJSON* bo = cJSON_CreateObject();
        w.attach(buses, nullptr, bo);
        if (!bo) continue;
        char nm[sizeof(p.bus[b].name) + 1];
        memcpy(nm, p.bus[b].name, sizeof(p.bus[b].name));
        nm[sizeof(p.bus[b].name)] = 0;
        w.s(bo, "name", nm);
        w.f(bo, "level", p.bus[b].level);
        w.f(bo, "pan", p.bus[b].pan);
        w.b(bo, "mute", p.bus[b].mute != 0);
        w.b(bo, "solo", p.bus[b].solo != 0);
    }

    cJSON* pads = w.arr(root, "pads");
    for (int i = 0; i < kPads; ++i) {
        cJSON* po = cJSON_CreateObject();
        w.attach(pads, nullptr, po);
        if (po) writePad(w, po, p.pad[i], refs ? refs[i] : nullptr);
    }

    cJSON* scenes = w.arr(root, "scenes");
    for (int s = 0; s < kScenes; ++s) {
        cJSON* so = cJSON_CreateObject();
        w.attach(scenes, nullptr, so);
        if (so) writeScene(w, so, p.scene_data[s]);
    }

    cJSON* song = w.obj(root, "song");
    cJSON* slots = w.arr(song, "slots");
    cJSON* fx = w.arr(song, "fx");
    for (int i = 0; i < kSongSlots; ++i) {
        w.attach(slots, nullptr, cJSON_CreateNumber((double)p.song.slot[i]));
        w.attach(fx, nullptr, cJSON_CreateBool(p.song.fx[i] ? 1 : 0));
    }
    w.b(song, "follow", p.song.follow != 0);

    ok = w.ok;
    return root;
}

// <path>.tmp -> <path>. FAT (and Windows) refuse to rename over an existing file, so the old
// file is moved to <path>.bak first and removed only after the new one is in place.
bool replaceFile(const char* tmp, const char* path)
{
    if (rename(tmp, path) == 0) return true;
    char bak[kPathLen + 8];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    remove(bak);
    const bool had_old = rename(path, bak) == 0;
    if (rename(tmp, path) == 0) {
        if (had_old) remove(bak);
        return true;
    }
    if (had_old) rename(bak, path);
    remove(tmp);
    return false;
}

bool writeJson(const char* path, const Project& p, const SampleRef refs[kPads][kLayers], bool exported,
               char* err, int errlen)
{
    if (!path || !path[0]) {
        setErr(err, errlen, "Błędna ścieżka projektu");
        return false;
    }
    JsonHeap heap;
    bool ok = false;
    cJSON* root = buildJson(p, refs, exported, ok);
    if (!root || !ok) {
        cJSON_Delete(root);
        setErr(err, errlen, "Brak pamięci");
        return false;
    }
    char* text = cJSON_PrintBuffered(root, 64 * 1024, 1);
    cJSON_Delete(root);
    if (!text) {
        setErr(err, errlen, "Brak pamięci");
        return false;
    }
    const size_t len = strlen(text);
    char tmp[kPathLen + 8];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        cJSON_free(text);
        setErr(err, errlen, "Błędna ścieżka projektu");
        return false;
    }
    FILE* f = fopen(tmp, "wb");
    if (!f) {
        cJSON_free(text);
        setErr(err, errlen, "Nie można zapisać pliku");
        return false;
    }
    bool wr = fwrite(text, 1, len, f) == len && fputc('\n', f) != EOF;
    cJSON_free(text);
    if (fflush(f) != 0) wr = false;
    if (fclose(f) != 0) wr = false;
    if (!wr) {
        remove(tmp);
        setErr(err, errlen, "Błąd zapisu na karcie");
        return false;
    }
    if (!replaceFile(tmp, path)) {
        setErr(err, errlen, "Nie można zastąpić pliku projektu");
        return false;
    }
    if (err && errlen > 0) err[0] = 0;
    return true;
}

// ------------------------------------------------------------ reader
const cJSON* item(const cJSON* o, const char* key) { return o ? cJSON_GetObjectItemCaseSensitive(o, key) : nullptr; }

bool getNum(const cJSON* o, const char* key, double& v)
{
    const cJSON* x = item(o, key);
    if (cJSON_IsNumber(x) && x->valuedouble == x->valuedouble) {
        v = x->valuedouble;
        return true;
    }
    if (cJSON_IsBool(x)) {
        v = cJSON_IsTrue(x) ? 1.0 : 0.0;
        return true;
    }
    return false;
}

// Range-limited float (the double is clamped before the conversion, so it is always defined).
void getF(const cJSON* o, const char* key, float& dst, float lo, float hi)
{
    double v;
    if (!getNum(o, key, v)) return;
    if (v < (double)lo) v = (double)lo;
    if (v > (double)hi) v = (double)hi;
    dst = (float)v;
}

template <typename T>
void getI(const cJSON* o, const char* key, T& dst, int lo, int hi)
{
    double v;
    if (!getNum(o, key, v)) return;
    if (v < (double)lo) v = (double)lo;
    if (v > (double)hi) v = (double)hi;
    dst = (T)(int)floor(v + 0.5);
}

void getB(const cJSON* o, const char* key, uint8_t& dst)
{
    double v;
    if (getNum(o, key, v)) dst = v != 0.0 ? 1 : 0;
}

void getS(const cJSON* o, const char* key, char* dst, int cap)
{
    const cJSON* x = item(o, key);
    if (cJSON_IsString(x) && x->valuestring) copyUtf8(dst, cap, x->valuestring);
}

// Enum stored by name (numbers accepted too).
template <typename E>
void getE(const cJSON* o, const char* key, E& dst, const char* const* names, int count)
{
    const cJSON* x = item(o, key);
    if (cJSON_IsString(x) && x->valuestring) {
        for (int i = 0; i < count; ++i)
            if (strcmp(x->valuestring, names[i]) == 0) {
                dst = (E)i;
                return;
            }
        return;
    }
    if (cJSON_IsNumber(x) && x->valuedouble >= 0.0 && x->valuedouble < (double)count) dst = (E)(int)x->valuedouble;
}

const cJSON* at(const cJSON* arr, int i) { return cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, i) : nullptr; }

void readRef(const cJSON* lo, SampleRef& r)
{
    r = SampleRef{};
    const cJSON* b = item(lo, "builtin");
    const cJSON* p = item(lo, "path");
    if (cJSON_IsString(b) && b->valuestring && b->valuestring[0]) {
        char tmp[kPathLen + 8];
        snprintf(tmp, sizeof(tmp), "builtin:%s", b->valuestring);
        copyUtf8(r.path, kPathLen, tmp);
    } else if (cJSON_IsString(p) && p->valuestring) {
        copyUtf8(r.path, kPathLen, p->valuestring);
    }
    if (r.path[0]) getS(lo, "name", r.name, kNameLen);
}

void readPad(const cJSON* o, Pad& p, SampleRef* refs)
{
    getS(o, "name", p.name, kNameLen);
    getE(o, "layer_mode", p.layer_mode, kLayerModes, (int)LayerMode::Count);
    getE(o, "mode", p.mode, kPlayModes, (int)PlayMode::Count);
    getB(o, "reverse", p.reverse);
    getI(o, "bus", p.bus, 0, kBuses - 1);
    getI(o, "choke", p.choke, 0, 8);
    getI(o, "poly", p.poly, 1, kMaxPadPoly);
    getB(o, "mute", p.mute);
    getB(o, "solo", p.solo);
    const cJSON* sl = item(o, "slices");
    if (cJSON_IsArray(sl)) {
        for (int i = 0; i < kMaxSlices; ++i) p.slices[i] = 0.0f;
        for (int i = 0; i < kMaxSlices; ++i) {
            const cJSON* x = at(sl, i);
            if (!x) break;
            if (cJSON_IsNumber(x)) {
                double v = x->valuedouble;
                if (!(v >= 0.0)) v = 0.0;
                if (v > 1.0) v = 1.0;
                p.slices[i] = (float)v;
            }
        }
    }
    getI(o, "slice_count", p.slice_count, 0, kMaxSlices);
    getI(o, "slice_sel", p.slice_sel, 0, kMaxSlices - 1);
    if (p.slice_sel >= p.slice_count) p.slice_sel = 0;
    // End of the last slice. Files written before the field kept it after the markers when there was
    // room (slices[slice_count]); otherwise the last slice ran to the end of the sample.
    double se;
    if (getNum(o, "slice_end", se)) {
        if (!(se >= 0.0)) se = 0.0;
        if (se > 1.0) se = 1.0;
        p.slice_end = (float)se;
    } else {
        const int n = p.slice_count;
        p.slice_end = (n > 0 && n < kMaxSlices && p.slices[n] > p.slices[n - 1]) ? p.slices[n] : 1.0f;
    }
    const cJSON* prm = item(o, "params");
    if (cJSON_IsObject(prm)) {
        for (int k = 0; k < kPadParamCount; ++k) {
            double v;
            if (!getNum(prm, kPadKeys[k], v)) continue;
            const ParamSpec& s = padSpec((PadParam)k);
            if (v < (double)s.min) v = (double)s.min;
            if (v > (double)s.max) v = (double)s.max;
            p.p[k] = clampParam(s, (float)v);
        }
    }
    const cJSON* layers = item(o, "layers");
    for (int l = 0; l < kLayers; ++l) {
        const cJSON* lo = at(layers, l);
        if (!cJSON_IsObject(lo)) continue;
        Layer& L = p.layer[l];
        getF(lo, "tune", L.tune, -48.0f, 48.0f);
        getF(lo, "gain_db", L.gain_db, -96.0f, 24.0f);
        getF(lo, "pan", L.pan, -1.0f, 1.0f);
        getI(lo, "vel_lo", L.vel_lo, 0, 127);
        getI(lo, "vel_hi", L.vel_hi, 0, 127);
        if (refs) readRef(lo, refs[l]);
    }
}

void readScene(const cJSON* o, Scene& sc)
{
    getI(o, "length", sc.length, 1, kMaxSteps);
    getE(o, "div", sc.div, kDivs, (int)Division::Count);
    getI(o, "swing", sc.swing, 50, 75);
    // ACID BASS (0.3): files without "bass" keep the default (empty) line.
    const cJSON* bass = item(o, "bass");
    const cJSON* bsteps = cJSON_IsObject(bass) ? item(bass, "steps") : nullptr;
    if (cJSON_IsArray(bsteps)) {
        sc.bass = BassLine{};
        const cJSON* bso = nullptr;
        cJSON_ArrayForEach(bso, bsteps)
        {
            double idx;
            if (!cJSON_IsObject(bso) || !getNum(bso, "i", idx) || !(idx >= 0.0) || idx >= (double)kMaxSteps) continue;
            BassStep& b = sc.bass.step[(int)idx];
            int on = 1, acc = 0, sl = 0;
            getI(bso, "o", on, 0, 1);
            getI(bso, "n", b.note, kBassNoteMin, kBassNoteMax);
            getI(bso, "a", acc, 0, 1);
            getI(bso, "s", sl, 0, 1);
            b.gate = (uint8_t)on;
            b.accent = (uint8_t)acc;
            b.slide = (uint8_t)sl;
        }
    }
    const cJSON* tracks = item(o, "tracks");
    if (!cJSON_IsArray(tracks)) return;
    for (int t = 0; t < kPads; ++t) {
        const cJSON* to = at(tracks, t);
        if (!cJSON_IsObject(to)) continue;
        Track& tr = sc.track[t];
        getI(to, "length", tr.length, 0, kMaxSteps);
        const cJSON* steps = item(to, "steps");
        if (!cJSON_IsArray(steps)) continue;
        // The file lists every active step (and every edited off step, "o": 0) of this track:
        // the default pattern is replaced.
        for (int i = 0; i < kMaxSteps; ++i) tr.step[i] = Step{};
        const cJSON* so = nullptr;
        cJSON_ArrayForEach(so, steps)
        {
            double idx;
            if (!cJSON_IsObject(so) || !getNum(so, "i", idx) || !(idx >= 0.0) || idx >= (double)kMaxSteps) continue;
            Step& st = tr.step[(int)idx];
            st = Step{};
            int on = 1;   // files without "o" list active steps only
            getI(so, "o", on, 0, 1);
            st.on = (uint8_t)on;
            getI(so, "v", st.vel, 1, 127);
            getI(so, "p", st.prob, 0, 100);
            getI(so, "r", st.ratchet, 1, 4);
            getI(so, "n", st.nudge, -50, 50);
            getI(so, "t", st.pitch, -24, 24);
            int c = (int)st.cond;
            getI(so, "c", c, 0, (int)TrigCond::Count - 1);
            st.cond = (TrigCond)c;
            getI(so, "f", st.flam, 0, 8);
        }
    }
}

bool applyJson(const cJSON* root, Project& p, SampleRef refs[kPads][kLayers], char* err, int errlen)
{
    const cJSON* fmt = item(root, "format");
    if (!cJSON_IsObject(root) || !cJSON_IsString(fmt) || strcmp(fmt->valuestring, kFormat) != 0) {
        setErr(err, errlen, "To nie jest projekt ORBIT");
        return false;
    }
    double ver = 0.0;
    if (!getNum(root, "version", ver) || ver < 1.0) {
        setErr(err, errlen, "Nieobsługiwana wersja projektu");
        return false;
    }
    projectDefaults(p);
    getS(root, "name", p.name, (int)sizeof(p.name));
    getF(root, "bpm", p.bpm, 30.0f, 300.0f);
    getI(root, "scene", p.scene, 0, kScenes - 1);
    getI(root, "beats_per_bar", p.beats_per_bar, 1, 16);
    getE(root, "quant", p.quant, kQuants, (int)LaunchQuant::Count);
    getB(root, "metronome", p.metronome);
    getI(root, "count_in", p.count_in, 0, 2);
    const cJSON* mac = item(root, "macros");
    getB(mac, "lofi", p.lofi_on);
    getB(mac, "wide", p.wide_on);
    getB(mac, "pump", p.pump_on);
    getB(mac, "space", p.space_on);
    getB(root, "limiter", p.limiter_on);
    getI(root, "pump_src", p.pump_src, 0, kPads - 1);

    const cJSON* g = item(root, "globals");
    if (cJSON_IsObject(g))
        for (int k = 0; k < kGlobalParamCount; ++k) {
            double v;
            if (!getNum(g, kGlobalKeys[k], v)) continue;
            const ParamSpec& s = globalSpec((GlobalParam)k);
            if (v < (double)s.min) v = (double)s.min;
            if (v > (double)s.max) v = (double)s.max;
            p.g[k] = clampParam(s, (float)v);
        }

    const cJSON* buses = item(root, "buses");
    for (int b = 0; b < kBuses; ++b) {
        const cJSON* bo = at(buses, b);
        if (!cJSON_IsObject(bo)) continue;
        const cJSON* nm = item(bo, "name");
        if (cJSON_IsString(nm) && nm->valuestring) {
            char tmp[sizeof(p.bus[b].name)];
            copyUtf8(tmp, (int)sizeof(tmp), nm->valuestring);
            memcpy(p.bus[b].name, tmp, sizeof(tmp));
        }
        getF(bo, "level", p.bus[b].level, 0.0f, 1.0f);
        getF(bo, "pan", p.bus[b].pan, -1.0f, 1.0f);
        getB(bo, "mute", p.bus[b].mute);
        getB(bo, "solo", p.bus[b].solo);
    }

    const cJSON* pads = item(root, "pads");
    for (int i = 0; i < kPads; ++i) {
        const cJSON* po = at(pads, i);
        if (cJSON_IsObject(po)) readPad(po, p.pad[i], refs ? refs[i] : nullptr);
    }

    const cJSON* scenes = item(root, "scenes");
    for (int s = 0; s < kScenes; ++s) {
        const cJSON* so = at(scenes, s);
        if (cJSON_IsObject(so)) readScene(so, p.scene_data[s]);
    }

    const cJSON* song = item(root, "song");
    if (cJSON_IsObject(song)) {
        const cJSON* slots = item(song, "slots");
        const cJSON* fx = item(song, "fx");
        if (cJSON_IsArray(slots))
            for (int i = 0; i < kSongSlots; ++i) {
                const cJSON* x = at(slots, i);
                p.song.slot[i] = -1;   // a listed chain replaces the demo chain
                if (cJSON_IsNumber(x) && x->valuedouble >= 0.0 && x->valuedouble < (double)kScenes)
                    p.song.slot[i] = (int8_t)(int)x->valuedouble;
            }
        if (cJSON_IsArray(fx))
            for (int i = 0; i < kSongSlots; ++i) {
                const cJSON* x = at(fx, i);
                p.song.fx[i] = (cJSON_IsTrue(x) || (cJSON_IsNumber(x) && x->valuedouble != 0.0)) ? 1 : 0;
            }
        getB(song, "follow", p.song.follow);
    }

    if (ver > (double)kFormatVersion) setErr(err, errlen, "Projekt z nowszej wersji: wczytano znane pola");
    else if (err && errlen > 0) err[0] = 0;
    return true;
}

char* readFile(const char* path, long& len, char* err, int errlen)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        setErr(err, errlen, "Nie można otworzyć projektu");
        return nullptr;
    }
    len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        setErr(err, errlen, "Błąd odczytu karty");
        return nullptr;
    }
    if (len == 0 || len > kMaxFileBytes) {
        fclose(f);
        setErr(err, errlen, len == 0 ? "Pusty plik projektu" : "Plik projektu jest za duży");
        return nullptr;
    }
    char* buf = static_cast<char*>(bigAlloc((size_t)len + 1));
    if (!buf) {
        fclose(f);
        setErr(err, errlen, "Brak pamięci");
        return nullptr;
    }
    const size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        bigFree(buf);
        setErr(err, errlen, "Błąd odczytu karty");
        return nullptr;
    }
    buf[len] = 0;
    return buf;
}

}  // namespace

bool projectSave(const char* path, const Project& p, const SampleRef refs[kPads][kLayers], char* err, int errlen)
{
    return writeJson(path, p, refs, false, err, errlen);
}

bool projectExportJson(const char* path, const Project& p, const SampleRef refs[kPads][kLayers])
{
    char err[64];
    return writeJson(path, p, refs, true, err, (int)sizeof(err));
}

bool projectLoad(const char* path, Project& p, SampleRef refs[kPads][kLayers], char* err, int errlen)
{
    if (err && errlen > 0) err[0] = 0;
    if (!path || !path[0]) {
        setErr(err, errlen, "Błędna ścieżka projektu");
        return false;
    }
    long len = 0;
    char* text = readFile(path, len, err, errlen);
    if (!text) {
        // A crash between the two renames of a save leaves only <path>.bak: use it.
        char bak[kPathLen + 8];
        if (snprintf(bak, sizeof(bak), "%s.bak", path) < (int)sizeof(bak)) {
            FILE* f = fopen(bak, "rb");
            if (f) {
                fclose(f);
                text = readFile(bak, len, err, errlen);
            }
        }
        if (!text) return false;
    }
    const char* body = text;
    size_t blen = (size_t)len;
    if (blen >= 3 && (uint8_t)body[0] == 0xEF && (uint8_t)body[1] == 0xBB && (uint8_t)body[2] == 0xBF) {
        body += 3;   // UTF-8 BOM from desktop editors
        blen -= 3;
    }
    JsonHeap heap;
    cJSON* root = cJSON_ParseWithLength(body, blen);
    bigFree(text);
    if (!root) {
        setErr(err, errlen, "Uszkodzony plik projektu (JSON)");
        return false;
    }
    if (refs)
        for (int i = 0; i < kPads; ++i)
            for (int l = 0; l < kLayers; ++l) refs[i][l] = SampleRef{};
    // Parse into a scratch copy first: a rejected file leaves the caller's project untouched.
    Project* tmp = static_cast<Project*>(bigAlloc(sizeof(Project)));
    if (!tmp) {
        cJSON_Delete(root);
        setErr(err, errlen, "Brak pamięci");
        return false;
    }
    const bool ok = applyJson(root, *tmp, refs, err, errlen);
    cJSON_Delete(root);
    if (ok) memcpy(&p, tmp, sizeof(Project));
    bigFree(tmp);
    return ok;
}

}  // namespace orbit
