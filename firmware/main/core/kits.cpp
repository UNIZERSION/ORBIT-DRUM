// Built-in kits: wraps the generated tables in kits_808.c / kits_909.c (tools/kitgen) as
// orbit::BuiltinKit. The generated files are plain C, so their sound table uses a C struct with
// the same layout as orbit::BuiltinSound; we copy it field by field once (no type punning).
#include "kits.h"

#include <stddef.h>

extern "C" {
// Mirror of the struct emitted by tools/kitgen (see the head of kits_808.c).
typedef struct orbit_builtin_sound_c {
    const char* name;
    const char* long_name;
    const int16_t* pcm;
    uint32_t frames;
    uint8_t choke;
    uint8_t bus;
} orbit_builtin_sound_c;

extern const orbit_builtin_sound_c orbit_kit808_sounds[16];
extern const orbit_builtin_sound_c orbit_kit909_sounds[16];
}

namespace orbit {

namespace {

static_assert(offsetof(orbit_builtin_sound_c, pcm) == offsetof(BuiltinSound, pcm), "kits layout");
static_assert(offsetof(orbit_builtin_sound_c, frames) == offsetof(BuiltinSound, frames), "kits layout");
static_assert(offsetof(orbit_builtin_sound_c, choke) == offsetof(BuiltinSound, choke), "kits layout");
static_assert(offsetof(orbit_builtin_sound_c, bus) == offsetof(BuiltinSound, bus), "kits layout");
static_assert(sizeof(orbit_builtin_sound_c) == sizeof(BuiltinSound), "kits layout");

constexpr int kKitCount = 2;
constexpr int kKitSounds = 16;
constexpr uint32_t kKitRate = 48000;

void copyTable(const orbit_builtin_sound_c* src, BuiltinSound* dst)
{
    for (int i = 0; i < kKitSounds; ++i) {
        dst[i].name = src[i].name;
        dst[i].long_name = src[i].long_name;
        dst[i].pcm = src[i].pcm;
        dst[i].frames = src[i].frames;
        dst[i].choke = src[i].choke;
        dst[i].bus = src[i].bus;
    }
}

// Built in place (kits[] points into sounds[] of the same object).
struct Kits {
    BuiltinSound sounds[kKitCount][kKitSounds];
    BuiltinKit kits[kKitCount];
    Kits()
    {
        copyTable(orbit_kit808_sounds, sounds[0]);
        copyTable(orbit_kit909_sounds, sounds[1]);
        kits[0] = BuiltinKit{"808 STYLE", kKitRate, sounds[0], kKitSounds};
        kits[1] = BuiltinKit{"909 STYLE", kKitRate, sounds[1], kKitSounds};
    }
};

const Kits& kitsTable()
{
    static const Kits k;  // built on first use: safe against static-init order across TUs
    return k;
}

}  // namespace

int builtinKitCount() { return kKitCount; }

const BuiltinKit& builtinKit(int i)
{
    const Kits& k = kitsTable();
    if (i < 0 || i >= kKitCount) i = 0;  // out of range -> first kit (never a dangling ref)
    return k.kits[i];
}

}  // namespace orbit
