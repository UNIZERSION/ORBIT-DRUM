// Built-in kits: wraps the generated tables in kits_808.c / kits_909.c (tools/kitgen) as
// orbit::BuiltinKit. The generated files are plain C, so their sound table uses a C struct with
// the same layout as orbit::BuiltinSound; we copy it field by field once (no type punning).
#include "kits.h"

#include <stddef.h>
#include <string.h>

#include "platform.h"
#include "sample.h"

extern "C" {
// Mirror of the struct emitted by tools/kitgen (see the head of kits_808.c).
typedef struct orbit_builtin_sound_c {
    const char* name;
    const char* long_name;
    const uint8_t* packed;
    uint32_t packed_bytes;
    uint32_t frames;
    uint8_t choke;
    uint8_t bus;
} orbit_builtin_sound_c;

extern const orbit_builtin_sound_c orbit_kit808_sounds[16];
extern const orbit_builtin_sound_c orbit_kit909_sounds[16];
}

namespace orbit {

namespace {

static_assert(offsetof(orbit_builtin_sound_c, packed) == offsetof(BuiltinSound, packed), "kits layout");
static_assert(offsetof(orbit_builtin_sound_c, packed_bytes) == offsetof(BuiltinSound, packed_bytes), "kits layout");
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
        dst[i].packed = src[i].packed;
        dst[i].packed_bytes = src[i].packed_bytes;
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

namespace {
// Unpacked PCM (frame 0 of a guarded PSRAM buffer), filled by builtinPcm().
const int16_t* s_pcm[kKitCount][kKitSounds] = {};
}  // namespace

// KPK1 (see tools/kitgen/kitpack.py): blocks of 256 samples, a header byte (order << 5 | k), fixed predictor of order
// 0..2, zigzag residuals in Rice codes (q < 24: q ones, a zero, k bits; else 24 ones and 20 bits).
bool kitUnpack(const uint8_t* src, uint32_t bytes, int16_t* dst, uint32_t frames)
{
    uint32_t pos = 0;   // byte
    uint32_t acc = 0;   // bit buffer, the next bit at bit 31
    int have = 0;       // valid bits in acc
    auto fill = [&]() {
        while (have <= 24) {
            const uint32_t b = pos < bytes ? src[pos] : 0u;
            ++pos;
            acc |= b << (24 - have);
            have += 8;
        }
    };
    auto take = [&](int n) -> uint32_t {   // n <= 24
        fill();
        const uint32_t v = n ? acc >> (32 - n) : 0u;
        acc <<= n;
        have -= n;
        return v;
    };
    int32_t h1 = 0, h2 = 0;
    uint32_t n = 0;
    BulkPacer pace;   // 0.4.9: a kit switch while the audio plays unpacks into PSRAM paced (platform.h)
    while (n < frames) {
        if ((n & 8191u) == 0 && n) bulkPace(pace, 8192u * 2u);
        const uint32_t hdr = take(8);
        const int order = (int)(hdr >> 5), k = (int)(hdr & 31u);
        if (order > 2 || k > 20) return false;
        const uint32_t end = frames - n < 256u ? frames : n + 256u;
        for (; n < end; ++n) {
            int q = 0;
            for (;;) {   // count up to 24 one bits
                fill();
                if (!(acc & 0x80000000u) || q == 24) break;
                acc <<= 1;
                --have;
                ++q;
            }
            uint32_t u;
            if (q == 24) {
                u = take(20);
            } else {
                take(1);   // the terminating zero
                u = ((uint32_t)q << k) | take(k);
            }
            const int32_t e = (int32_t)(u >> 1) ^ -(int32_t)(u & 1u);
            const int32_t pred = order == 0 ? 0 : (order == 1 ? h1 : 2 * h1 - h2);
            const int32_t x = e + pred;
            if (x < -32768 || x > 32767) return false;
            dst[n] = (int16_t)x;
            h2 = h1;
            h1 = x;
        }
        if (pos > bytes + 4) return false;   // read past the end (a few bytes of look-ahead are normal)
    }
    return true;
}

const int16_t* builtinPcm(int kit, int sound)
{
    if (kit < 0 || kit >= kKitCount || sound < 0 || sound >= kKitSounds) return nullptr;
    if (s_pcm[kit][sound]) return s_pcm[kit][sound];
    const BuiltinSound& snd = kitsTable().sounds[kit][sound];
    if (!snd.packed || !snd.frames) return nullptr;
    // kitUnpack writes every frame; the guard frames on both sides are cleared here (a full zeroing of the block
    // would cost dropouts when a kit is loaded while the audio plays)
    int16_t* buf = static_cast<int16_t*>(bigAllocRaw(sizeof(int16_t) * ((size_t)snd.frames + 2u * (size_t)kGuard)));
    if (!buf) return nullptr;
    memset(buf, 0, sizeof(int16_t) * (size_t)kGuard);
    memset(buf + kGuard + snd.frames, 0, sizeof(int16_t) * (size_t)kGuard);
    if (!kitUnpack(snd.packed, snd.packed_bytes, buf + kGuard, snd.frames)) {
        bigFree(buf);
        return nullptr;
    }
    s_pcm[kit][sound] = buf + kGuard;
    return s_pcm[kit][sound];
}

const int16_t* builtinPcmIfUnpacked(int kit, int sound)
{
    if (kit < 0 || kit >= kKitCount || sound < 0 || sound >= kKitSounds) return nullptr;
    return s_pcm[kit][sound];
}

int builtinKitCount() { return kKitCount; }

const BuiltinKit& builtinKit(int i)
{
    const Kits& k = kitsTable();
    if (i < 0 || i >= kKitCount) i = 0;  // out of range -> first kit (never a dangling ref)
    return k.kits[i];
}

}  // namespace orbit
