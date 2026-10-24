// Minimal App for widget tests (host only): parameter storage + Settings. Implements just the
// App methods the widget kit uses; the real controller is firmware/main/ui/app.cpp.
// Needs core/params.cpp and core/settings.cpp at link time.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui/app.h"

namespace orbit::ui {

namespace {

struct FakeState {
    bool init = false;
    Settings settings;
    float pad[kPads][kPadParamCount];
    float global[kGlobalParamCount];
    float bus_level[kBuses + 1];   // fader position 0..1 (index kBuses = master)
    float bus_pan[kBuses + 1];     // -1..1
    float layer_tune[kLayers], layer_gain[kLayers], layer_pan[kLayers];
    char last_toast[160];
} g;

FakeState& st()
{
    if (!g.init) {
        g.init = true;
        settingsDefaults(g.settings);
        for (int p = 0; p < kPads; ++p)
            for (int i = 0; i < kPadParamCount; ++i) g.pad[p][i] = padSpec((PadParam)i).def;
        for (int i = 0; i < kGlobalParamCount; ++i) g.global[i] = globalSpec((GlobalParam)i).def;
        for (int b = 0; b <= kBuses; ++b) { g.bus_level[b] = 0.8f; g.bus_pan[b] = 0.f; }
        for (int l = 0; l < kLayers; ++l) { g.layer_tune[l] = 0.f; g.layer_gain[l] = 0.f; g.layer_pan[l] = 0.f; }
    }
    return g;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

const ParamSpec kLayerTune = {"TUNE", -24.f, 24.f, 0.f, Curve::Lin, Unit::Semis, 0, nullptr};
const ParamSpec kLayerGain = {"GAIN", -24.f, 24.f, 0.f, Curve::Lin, Unit::Db, 0, nullptr};
const ParamSpec kLayerPan = {"PAN", -1.f, 1.f, 0.f, Curve::Lin, Unit::Pan, 0, nullptr};

float* slot(const ParamRef& r)
{
    FakeState& s = st();
    const int i = r.index;
    switch (r.kind) {
    case ParamRef::Kind::Pad: return &s.pad[i < 0 ? 0 : i][r.id];
    case ParamRef::Kind::Global: return &s.global[r.id];
    case ParamRef::Kind::BusLevel: return &s.bus_level[i < 0 ? 0 : i];
    case ParamRef::Kind::BusPan: return &s.bus_pan[i < 0 ? 0 : i];
    case ParamRef::Kind::LayerTune: return &s.layer_tune[i < 0 ? 0 : i];
    case ParamRef::Kind::LayerGain: return &s.layer_gain[i < 0 ? 0 : i];
    case ParamRef::Kind::LayerPan: return &s.layer_pan[i < 0 ? 0 : i];
    case ParamRef::Kind::OutVolume: return &s.bus_level[0];   // the widget tests do not use it
    }
    return &s.pad[0][0];
}

}  // namespace

App& app()
{
    static App a;
    return a;
}

const Settings& App::settings() const { return st().settings; }
Settings& App::settingsEdit() { return st().settings; }

const ParamSpec* App::paramSpec(const ParamRef& r) const
{
    switch (r.kind) {
    case ParamRef::Kind::Pad: return &padSpec((PadParam)r.id);
    case ParamRef::Kind::Global: return &globalSpec((GlobalParam)r.id);
    case ParamRef::Kind::LayerTune: return &kLayerTune;
    case ParamRef::Kind::LayerGain: return &kLayerGain;
    case ParamRef::Kind::LayerPan: return &kLayerPan;
    default: return nullptr;   // bus kinds: fader law
    }
}

float App::paramGet(const ParamRef& r) const { return *slot(r); }

void App::paramSet(const ParamRef& r, float value)
{
    const ParamSpec* s = paramSpec(r);
    if (s) *slot(r) = clampParam(*s, value);
    else if (r.kind == ParamRef::Kind::BusPan) *slot(r) = clampf(value, -1.f, 1.f);
    else *slot(r) = clampf(value, 0.f, 1.f);
}

float App::paramNorm(const ParamRef& r) const
{
    const ParamSpec* s = paramSpec(r);
    if (s) return toNorm(*s, *slot(r));
    if (r.kind == ParamRef::Kind::BusPan) return (*slot(r) + 1.f) * 0.5f;
    return *slot(r);
}

void App::paramSetNorm(const ParamRef& r, float norm)
{
    norm = clampf(norm, 0.f, 1.f);
    const ParamSpec* s = paramSpec(r);
    if (s) *slot(r) = fromNorm(*s, norm);
    else if (r.kind == ParamRef::Kind::BusPan) *slot(r) = norm * 2.f - 1.f;
    else *slot(r) = norm;
}

void App::paramReset(const ParamRef& r)
{
    const ParamSpec* s = paramSpec(r);
    if (s) *slot(r) = s->def;
    else if (r.kind == ParamRef::Kind::BusPan) *slot(r) = 0.f;
    else *slot(r) = 0.8f;
}

int App::paramText(const ParamRef& r, char* buf, int len) const
{
    const ParamSpec* s = paramSpec(r);
    if (s) return formatParam(*s, *slot(r), buf, len);
    if (r.kind == ParamRef::Kind::BusPan) return snprintf(buf, (size_t)len, "%+.2f", (double)*slot(r));
    const float v = *slot(r);
    if (v <= 0.0001f) return snprintf(buf, (size_t)len, "-inf dB");
    return snprintf(buf, (size_t)len, "%.1f dB", (double)(40.f * log10f(v / 0.8f)));
}

const char* App::paramCaption(const ParamRef& r) const
{
    const ParamSpec* s = paramSpec(r);
    if (s) return s->label;
    return r.kind == ParamRef::Kind::BusPan ? "PAN" : "LEVEL";
}

void App::toast(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st().last_toast, sizeof st().last_toast, fmt, ap);
    va_end(ap);
}

}  // namespace orbit::ui
