// Build + run (Git Bash, project root; one line):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; J=$HOME/esp/v5.5.2/esp-idf/components/json/cJSON; zig cc -target x86_64-windows-gnu -O2 -c $J/cJSON.c -o host/bin/cJSON.o && zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -g -Wall -Wextra -fsanitize=undefined -Ifirmware/main -Ihost -I$J host/tests/test_sampleio.cpp firmware/main/core/wav.cpp firmware/main/core/sample_ops.cpp firmware/main/core/slice.cpp firmware/main/core/library.cpp firmware/main/core/persist.cpp firmware/main/core/params.cpp firmware/main/core/project.cpp firmware/main/core/sample.cpp host/platform_host.cpp host/bin/cJSON.o host/testdata/utf8/utf8.rc -o host/bin/test_sampleio.exe && host/bin/test_sampleio.exe
//
// WAV reader/writer for every format and chunk layout, a fuzz loop over malformed files
// (input placed against no-access guard pages, so any over-read faults; UBSan catches UB),
// sample ops on known signals, slicing, the SD library and project JSON persistence.
// Run from the project root; generated files go to host/testdata/.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <utime.h>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "cJSON.h"
#include "core/library.h"
#include "core/project.h"
#include "core/sampleio.h"

using namespace orbit;

static int g_fails = 0, g_checks = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(c)) {                                                             \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);             \
            ++g_fails;                                                          \
        }                                                                       \
    } while (0)
#define CHECKF(c, ...)                                                          \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(c)) {                                                             \
            std::printf("FAIL %s:%d %s : ", __FILE__, __LINE__, #c);            \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n");                                                  \
            ++g_fails;                                                          \
        }                                                                       \
    } while (0)

static const std::string kDir = "host/testdata";
static const double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- small helpers
using Bytes = std::vector<uint8_t>;
static void put16(Bytes& b, uint32_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
static void put24(Bytes& b, uint32_t v) { for (int i = 0; i < 3; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static void put64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static void putId(Bytes& b, const char* id) { b.insert(b.end(), id, id + 4); }
static void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }

static Bytes chunk(const char* id, const Bytes& body, bool pad = true, int64_t size = -1)
{
    Bytes c;
    putId(c, id);
    put32(c, size >= 0 ? (uint32_t)size : (uint32_t)body.size());
    append(c, body);
    if (pad && (body.size() & 1)) c.push_back(0);
    return c;
}

static Bytes riff(const std::vector<Bytes>& chunks, int64_t size = -1)
{
    Bytes body;
    putId(body, "WAVE");
    for (const Bytes& c : chunks) append(body, c);
    Bytes f;
    putId(f, "RIFF");
    put32(f, size >= 0 ? (uint32_t)size : (uint32_t)body.size());
    append(f, body);
    return f;
}

static bool writeBytes(const std::string& path, const Bytes& b)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = b.empty() || std::fwrite(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    return ok;
}

static bool writeText(const std::string& path, const char* text)
{
    return writeBytes(path, Bytes(text, text + std::strlen(text)));
}

static bool exists(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static bool isDirPath(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdirs(const std::string& p)
{
    for (size_t i = 1; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            const std::string s = p.substr(0, i);
#if defined(_WIN32)
            _mkdir(s.c_str());
#else
            mkdir(s.c_str(), 0775);
#endif
        }
    }
}

static void rmrf(const std::string& p)
{
    if (!isDirPath(p)) {
        std::remove(p.c_str());
        return;
    }
    DIR* d = opendir(p.c_str());
    if (d) {
        std::vector<std::string> names;
        while (struct dirent* e = readdir(d))
            if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) names.push_back(e->d_name);
        closedir(d);
        for (const std::string& n : names) rmrf(p + "/" + n);
    }
#if defined(_WIN32)
    _rmdir(p.c_str());
#else
    rmdir(p.c_str());
#endif
}

static uint32_t g_rng = 0x12345678u;
static uint32_t rnd()
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g_rng = x;
}
static float rnd01() { return (float)(rnd() >> 8) / 16777216.0f; }

static bool guardOk(const Sample* s)
{
    const int ch = s->channels;
    for (int i = 0; i < kGuard * ch; ++i)
        if (s->data[-1 - i] != 0 || s->data[(size_t)s->frames * ch + i] != 0) return false;
    return true;
}

// ---------------------------------------------------------------- WAV generation
enum class Fmt { U8, S16, S24, S32, F32, F64 };
static int containerOf(Fmt f)
{
    switch (f) {
    case Fmt::U8: return 1;
    case Fmt::S16: return 2;
    case Fmt::S24: return 3;
    case Fmt::S32: case Fmt::F32: return 4;
    case Fmt::F64: return 8;
    }
    return 2;
}
static bool isFloatFmt(Fmt f) { return f == Fmt::F32 || f == Fmt::F64; }

static int16_t pattern16(uint32_t n, int c)
{
    if (n == 1) return 32767;
    if (n == 2) return -32768;
    return (int16_t)(uint16_t)((n * 997u + (uint32_t)c * 12345u + (n * n) % 7u) & 0xFFFFu);
}
static int16_t clamp16(int64_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// Interleaved PCM of `ch` channels + the expected int16 of the first min(ch, 2) channels.
static void genData(Fmt f, int ch, uint32_t frames, Bytes& data, std::vector<int16_t>& expect)
{
    const int outc = ch >= 2 ? 2 : 1;
    for (uint32_t n = 0; n < frames; ++n)
        for (int c = 0; c < ch; ++c) {
            const int16_t v = pattern16(n, c);
            int16_t e = v;
            switch (f) {
            case Fmt::U8: {
                const uint8_t b = (uint8_t)((n * 7u + (uint32_t)c * 3u) & 0xFFu);
                data.push_back(b);
                e = (int16_t)(((int)b - 128) * 256);
                break;
            }
            case Fmt::S16: put16(data, (uint16_t)v); break;
            case Fmt::S24: {
                const int32_t v24 = (int32_t)v * 256 + (int32_t)((n * 37u + (uint32_t)c) & 0xFFu);
                put24(data, (uint32_t)v24);
                e = clamp16((int64_t)std::floor((v24 + 128) / 256.0));
                break;
            }
            case Fmt::S32: {
                const int64_t v32 = (int64_t)v * 65536 + (int64_t)((n * 7919u + (uint32_t)c * 31u) & 0xFFFFu);
                put32(data, (uint32_t)(int32_t)v32);
                e = clamp16((int64_t)std::floor((double)(v32 + 32768) / 65536.0));
                break;
            }
            case Fmt::F32:
            case Fmt::F64: {
                double x = v / 32768.0;
                if (n == 3) x = 1.5, e = 32767;           // over full scale -> clipped
                if (n == 4) x = std::nan(""), e = 0;      // NaN -> silence
                if (n == 5) x = -2.0, e = -32768;
                if (n == 6) x = 0.25 + 0.4 / 32768.0, e = 8192;   // rounds to nearest
                if (f == Fmt::F32) {
                    const float xf = (float)x;
                    uint32_t u;
                    std::memcpy(&u, &xf, 4);
                    put32(data, u);
                } else {
                    uint64_t u;
                    std::memcpy(&u, &x, 8);
                    put64(data, u);
                }
                break;
            }
            }
            if (c < outc) expect.push_back(e);
        }
}

static const uint8_t kGuidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

// fmt body: variant 16 / 18 / 40 (WAVE_FORMAT_EXTENSIBLE).
static Bytes fmtBody(Fmt f, int ch, uint32_t rate, int variant, int container = 0, int valid_bits = 0)
{
    if (!container) container = containerOf(f);
    const uint16_t tag = isFloatFmt(f) ? 3 : 1;
    Bytes b;
    put16(b, variant == 40 ? 0xFFFE : tag);
    put16(b, (uint32_t)ch);
    put32(b, rate);
    put32(b, rate * (uint32_t)(ch * container));
    put16(b, (uint32_t)(ch * container));
    put16(b, (uint32_t)(container * 8));
    if (variant == 18) put16(b, 0);
    if (variant == 40) {
        put16(b, 22);
        put16(b, (uint32_t)(valid_bits ? valid_bits : container * 8));
        put32(b, ch == 1 ? 4u : 3u);
        put16(b, tag);
        b.insert(b.end(), kGuidTail, kGuidTail + 14);
    }
    return b;
}

struct WavCase {
    std::string name;
    Bytes file;
    std::vector<int16_t> expect;
    int out_ch;
    uint32_t frames, rate;
    uint16_t channels, bits;
    uint8_t is_float;
    int loop = 0;
    uint32_t loop_start = 0, loop_end = 0;
    std::vector<uint32_t> cues;
};

static WavCase makeCase(const std::string& name, Fmt f, int ch, uint32_t frames, uint32_t rate, int layout)
{
    WavCase c;
    c.name = name;
    c.out_ch = ch >= 2 ? 2 : 1;
    c.rate = rate;
    c.channels = (uint16_t)ch;
    c.bits = (uint16_t)(containerOf(f) * 8);
    c.is_float = isFloatFmt(f) ? 1 : 0;
    Bytes data;
    genData(f, ch, frames, data, c.expect);
    c.frames = frames;
    Bytes list;   // LIST/adtl with an odd label
    putId(list, "adtl");
    {
        Bytes labl;
        put32(labl, 1);
        const char* txt = "Kick";   // + NUL = 5 bytes (odd)
        labl.insert(labl.end(), txt, txt + 5);
        append(list, chunk("labl", labl));
    }
    switch (layout) {
    case 0:   // plain fmt 16 + data
        c.file = riff({chunk("fmt ", fmtBody(f, ch, rate, 16)), chunk("data", data)});
        break;
    case 1:   // fmt 18
        c.file = riff({chunk("fmt ", fmtBody(f, ch, rate, 18)), chunk("data", data)});
        break;
    case 2:   // extensible
        c.file = riff({chunk("fmt ", fmtBody(f, ch, rate, 40)), chunk("data", data)});
        break;
    case 3: {   // LIST before fmt, odd junk chunk with pad byte, fact
        Bytes junk(7, 0xAB), fact;
        put32(fact, frames);
        c.file = riff({chunk("LIST", list), chunk("junk", junk), chunk("fmt ", fmtBody(f, ch, rate, 16)),
                       chunk("fact", fact), chunk("data", data)});
        break;
    }
    case 4: {   // odd chunk WITHOUT the pad byte (buggy writer)
        Bytes junk(3, 0x11);
        c.file = riff({chunk("fmt ", fmtBody(f, ch, rate, 16)), chunk("junk", junk, false), chunk("data", data)});
        break;
    }
    case 5:   // RIFF and data sizes 0xFFFFFFFF (streamed)
        c.file = riff({chunk("fmt ", fmtBody(f, ch, rate, 16)), chunk("data", data, true, 0xFFFFFFFFll)}, 0xFFFFFFFFll);
        break;
    case 6:   // RIFF and data sizes 0
        c.file = riff({chunk("fmt ", fmtBody(f, ch, rate, 16)), chunk("data", data, true, 0)}, 0);
        break;
    case 7: {   // smpl + cue + LIST after data (odd data size possible)
        Bytes smpl;
        for (int i = 0; i < 7; ++i) put32(smpl, i == 2 ? 22675u : (i == 3 ? 60u : 0u));
        put32(smpl, 1);   // loops
        put32(smpl, 0);
        put32(smpl, 0);   // cue id
        put32(smpl, 0);   // type
        put32(smpl, 100); // start
        put32(smpl, frames - 101);   // end (inclusive)
        put32(smpl, 0);
        put32(smpl, 0);
        Bytes cue;
        const uint32_t pts[3] = {frames / 2, 0, frames / 4};   // unsorted on purpose
        put32(cue, 3);
        for (int i = 0; i < 3; ++i) {
            put32(cue, (uint32_t)i + 1);
            put32(cue, pts[i]);
            putId(cue, "data");
            put32(cue, 0);
            put32(cue, 0);
            put32(cue, pts[i]);
        }
        c.file = riff({chunk("fmt ", fmtBody(f, ch, rate, 16)), chunk("data", data), chunk("smpl", smpl),
                       chunk("cue ", cue), chunk("LIST", list)});
        c.loop = 1;
        c.loop_start = 100;
        c.loop_end = frames - 100;
        c.cues = {0, frames / 4, frames / 2};
        break;
    }
    case 8: {   // data chunk claims twice the bytes that exist (truncated download)
        c.file = riff({chunk("fmt ", fmtBody(f, ch, rate, 16)), chunk("data", data, true, (int64_t)data.size() * 2)});
        break;
    }
    }
    return c;
}

static void checkLoaded(const WavCase& c, const Sample* s, const WavInfo& info, const char* how)
{
    CHECKF(s != nullptr, "%s %s: not loaded", c.name.c_str(), how);
    if (!s) return;
    CHECKF(s->frames == c.frames, "%s %s: frames %u != %u", c.name.c_str(), how, s->frames, c.frames);
    CHECKF(s->channels == c.out_ch, "%s %s: channels", c.name.c_str(), how);
    CHECKF(s->rate == c.rate, "%s %s: rate", c.name.c_str(), how);
    CHECKF(info.rate == c.rate && info.channels == c.channels && info.bits == c.bits && info.is_float == c.is_float &&
               info.frames == c.frames,
           "%s %s: info %u/%u/%u/%u/%u", c.name.c_str(), how, info.rate, info.channels, info.bits, info.is_float, info.frames);
    CHECK(guardOk(s));
    if (s->frames == c.frames) {
        int bad = -1;
        for (size_t i = 0; i < c.expect.size(); ++i)
            if (s->data[i] != c.expect[i]) {
                bad = (int)i;
                break;
            }
        CHECKF(bad < 0, "%s %s: sample %d = %d, expected %d", c.name.c_str(), how, bad, bad >= 0 ? s->data[bad] : 0,
               bad >= 0 ? c.expect[(size_t)bad] : 0);
    }
    CHECKF(info.has_loop == c.loop, "%s %s: loop flag", c.name.c_str(), how);
    if (c.loop) CHECKF(info.loop_start == c.loop_start && info.loop_end == c.loop_end, "%s %s: loop %u..%u", c.name.c_str(), how, info.loop_start, info.loop_end);
    CHECKF(info.cue_count == c.cues.size(), "%s %s: cue count %u", c.name.c_str(), how, info.cue_count);
    for (size_t i = 0; i < c.cues.size() && i < info.cue_count; ++i) CHECK(info.cues[i] == c.cues[i]);
}

static std::vector<Bytes> g_seeds;

static void testWavFormats()
{
    struct Spec {
        const char* name;
        Fmt f;
        int ch;
        uint32_t frames, rate;
        int layout;
    };
    const Spec specs[] = {
        {"u8_mono", Fmt::U8, 1, 1000, 22050, 0},       {"u8_stereo", Fmt::U8, 2, 777, 11025, 0},
        {"u8_mono_odd_smpl", Fmt::U8, 1, 1001, 8000, 7}, {"s16_mono", Fmt::S16, 1, 1000, 44100, 0},
        {"s16_stereo", Fmt::S16, 2, 1000, 48000, 0},   {"s24_mono", Fmt::S24, 1, 999, 96000, 0},
        {"s24_stereo", Fmt::S24, 2, 1000, 48000, 1},   {"s32_mono", Fmt::S32, 1, 1000, 44100, 0},
        {"s32_stereo", Fmt::S32, 2, 500, 44100, 3},    {"f32_mono", Fmt::F32, 1, 1000, 48000, 0},
        {"f32_stereo", Fmt::F32, 2, 1000, 44100, 1},   {"f64_mono", Fmt::F64, 1, 1000, 48000, 0},
        {"f64_stereo", Fmt::F64, 2, 333, 32000, 3},    {"s16_4ch", Fmt::S16, 4, 1000, 48000, 0},
        {"f32_6ch_ext", Fmt::F32, 6, 400, 48000, 2},   {"s24_4ch_ext", Fmt::S24, 4, 300, 44100, 2},
        {"s16_stereo_ext", Fmt::S16, 2, 1000, 44100, 2}, {"s16_list_before_fmt", Fmt::S16, 2, 1000, 44100, 3},
        {"s16_odd_nopad", Fmt::S16, 1, 1000, 44100, 4}, {"s16_size_ffffffff", Fmt::S16, 2, 1000, 44100, 5},
        {"s24_size_ffffffff", Fmt::S24, 1, 1001, 44100, 5}, {"s16_size_zero", Fmt::S16, 1, 1000, 44100, 6},
        {"s16_smpl_cue", Fmt::S16, 1, 2000, 44100, 7}, {"f32_smpl_cue", Fmt::F32, 2, 1000, 48000, 7},
        {"s16_truncated", Fmt::S16, 2, 1000, 44100, 8},
    };
    int n = 0;
    for (const Spec& sp : specs) {
        WavCase c = makeCase(sp.name, sp.f, sp.ch, sp.frames, sp.rate, sp.layout);
        const std::string path = kDir + "/" + c.name + ".wav";
        CHECK(writeBytes(path, c.file));
        WavInfo pi;
        const WavError pe = wavProbe(path.c_str(), pi);
        CHECKF(pe == WavError::Ok, "%s probe: %s", c.name.c_str(), wavErrorText(pe));
        CHECKF(pi.frames == c.frames && pi.rate == c.rate, "%s probe info", c.name.c_str());
        WavInfo info;
        WavError err = WavError::Io;
        Sample* s = wavLoad(path.c_str(), &info, &err, 60.0f);
        CHECKF(err == WavError::Ok, "%s load: %s", c.name.c_str(), wavErrorText(err));
        checkLoaded(c, s, info, "file");
        if (s) CHECKF(!std::strcmp(s->name, c.name.substr(0, kNameLen - 1).c_str()), "%s name '%s'", c.name.c_str(), s->name);
        sampleFree(s);
        WavInfo mi;
        Sample* m = wavLoadMem(c.file.data(), (uint32_t)c.file.size(), "mem", &mi, &err);
        checkLoaded(c, m, mi, "mem");
        sampleFree(m);
        g_seeds.push_back(c.file);
        ++n;
    }

    // 24-in-32 extensible (valid bits 24, left-justified): read as 32-bit, reported as 24-bit.
    {
        Bytes data;
        std::vector<int16_t> expect;
        for (uint32_t i = 0; i < 500; ++i) {
            const int32_t v24 = ((int32_t)pattern16(i, 0) * 256) + (int32_t)(i & 0xFF);
            put32(data, (uint32_t)(v24 * 256));
            expect.push_back(clamp16((int64_t)std::floor(((double)v24 * 256.0 + 32768.0) / 65536.0)));
        }
        const Bytes file = riff({chunk("fmt ", fmtBody(Fmt::S32, 1, 48000, 40, 4, 24)), chunk("data", data)});
        WavInfo info;
        WavError err;
        Sample* s = wavLoadMem(file.data(), (uint32_t)file.size(), "x", &info, &err);
        CHECK(s && info.bits == 24 && s->frames == 500);
        if (s) CHECK(std::memcmp(s->data, expect.data(), 500 * 2) == 0);
        sampleFree(s);
        writeBytes(kDir + "/s24in32_ext.wav", file);
        g_seeds.push_back(file);
    }

    // max_seconds cap: 2 s file, 1 s limit -> truncated; info.frames tells the file length.
    {
        Bytes data;
        std::vector<int16_t> ex;
        genData(Fmt::S16, 1, 88200, data, ex);
        const Bytes file = riff({chunk("fmt ", fmtBody(Fmt::S16, 1, 44100, 16)), chunk("data", data)});
        const std::string path = kDir + "/s16_2s.wav";
        writeBytes(path, file);
        WavInfo info;
        WavError err;
        Sample* s = wavLoad(path.c_str(), &info, &err, 1.0f);
        CHECK(s && s->frames == 44100 && info.frames == 88200);
        if (s) CHECK(std::memcmp(s->data, ex.data(), 44100 * 2) == 0);
        sampleFree(s);
    }

    // A big file read through several 64 KB blocks, with an odd data offset (frames split
    // across block boundaries): 24-bit stereo, 3 s.
    {
        Bytes data;
        std::vector<int16_t> ex;
        genData(Fmt::S24, 2, 144000, data, ex);
        Bytes junk(13, 0x55);
        const Bytes file = riff({chunk("fmt ", fmtBody(Fmt::S24, 2, 48000, 16)), chunk("junk", junk), chunk("data", data)});
        const std::string path = kDir + "/s24_stereo_3s.wav";
        writeBytes(path, file);
        WavInfo info;
        WavError err;
        Sample* s = wavLoad(path.c_str(), &info, &err, 60.0f);
        CHECK(s && s->frames == 144000);
        if (s) CHECK(std::memcmp(s->data, ex.data(), ex.size() * 2) == 0);
        sampleFree(s);
    }

    // Errors.
    {
        WavError err = WavError::Ok;
        WavInfo info;
        CHECK(!wavLoad((kDir + "/does_not_exist.wav").c_str(), &info, &err) && err == WavError::Open);
        const Bytes notriff = {'O', 'g', 'g', 'S', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
        CHECK(!wavLoadMem(notriff.data(), (uint32_t)notriff.size(), "x", &info, &err) && err == WavError::NotRiff);
        Bytes notwave = riff({});
        notwave[8] = 'A';
        CHECK(!wavLoadMem(notwave.data(), (uint32_t)notwave.size(), "x", &info, &err) && err == WavError::NotWave);
        const Bytes nofmt = riff({chunk("data", Bytes(100, 0))});
        CHECK(!wavLoadMem(nofmt.data(), (uint32_t)nofmt.size(), "x", &info, &err) && err == WavError::NoFmt);
        const Bytes nodata = riff({chunk("fmt ", fmtBody(Fmt::S16, 1, 44100, 16))});
        CHECK(!wavLoadMem(nodata.data(), (uint32_t)nodata.size(), "x", &info, &err) && err == WavError::NoData);
        Bytes adpcm = fmtBody(Fmt::S16, 1, 44100, 16);
        adpcm[0] = 2;
        const Bytes adp = riff({chunk("fmt ", adpcm), chunk("data", Bytes(100, 0))});
        CHECK(!wavLoadMem(adp.data(), (uint32_t)adp.size(), "x", &info, &err) && err == WavError::Unsupported);
        CHECK(!std::strcmp(wavErrorText(WavError::Unsupported), "Unsupported format"));
        for (int e = 0; e <= (int)WavError::Io; ++e) CHECK(wavErrorText((WavError)e)[0] != 0);
        CHECK(!wavLoadMem(nullptr, 0, "x", &info, &err));
    }

    // wavSave round trip with cue points.
    {
        Sample* s = sampleCreate(10000, 2, 48000);
        for (uint32_t i = 0; i < 20000; ++i) s->data[i] = (int16_t)(std::sin(i * 0.01) * 20000.0);
        std::strcpy(s->name, "save");
        const float sl[3] = {0.0f, 0.25f, 0.5f};
        const std::string path = kDir + "/saved_cue.wav";
        CHECK(wavSave(path.c_str(), s, sl, 3) == WavError::Ok);
        CHECK(wavSave(path.c_str(), s, sl, 3) == WavError::Ok);   // overwrite an existing file
        CHECK(!exists(path + ".tmp"));
        WavInfo info;
        WavError err;
        Sample* r = wavLoad(path.c_str(), &info, &err);
        CHECK(r && r->frames == 10000 && r->channels == 2 && r->rate == 48000);
        if (r) CHECK(std::memcmp(r->data, s->data, 40000) == 0);
        CHECK(info.cue_count == 3 && info.cues[0] == 0 && info.cues[1] == 2500 && info.cues[2] == 5000);
        CHECK(info.bits == 16 && !info.is_float);
        sampleFree(r);
        sampleFree(s);
    }
    std::printf("wav formats: %d cases\n", n);
}

// ---------------------------------------------------------------- fuzzing
// The input is copied flush against a PAGE_NOACCESS page (alternately after its end and
// before its start), so reading even one byte outside it crashes the test.
struct Guarded {
    uint8_t* base = nullptr;
    uint8_t* data = nullptr;
    size_t total = 0;
    void set(const Bytes& b, bool at_end)
    {
        release();
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const size_t pg = si.dwPageSize;
        size_t body = (b.size() + pg - 1) / pg * pg;
        if (!body) body = pg;
        total = body + 2 * pg;
        base = (uint8_t*)VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD old;
        VirtualProtect(base, pg, PAGE_NOACCESS, &old);
        VirtualProtect(base + pg + body, pg, PAGE_NOACCESS, &old);
        data = at_end ? base + pg + body - b.size() : base + pg;
#else
        total = b.size() + 1;
        base = (uint8_t*)std::malloc(total);
        data = base;
        (void)at_end;
#endif
        if (!b.empty()) std::memcpy(data, b.data(), b.size());
    }
    void release()
    {
#if defined(_WIN32)
        if (base) VirtualFree(base, 0, MEM_RELEASE);
#else
        std::free(base);
#endif
        base = data = nullptr;
    }
    ~Guarded() { release(); }
};

static void mutate(Bytes& b)
{
    const int nm = 1 + (int)(rnd() % 6);
    for (int k = 0; k < nm; ++k) {
        if (b.empty()) {
            b.push_back((uint8_t)rnd());
            continue;
        }
        // Half of the edits hit the headers (first 128 bytes), where the parser lives.
        const size_t span = (rnd() & 1) ? (b.size() < 128 ? b.size() : 128) : b.size();
        const size_t at = rnd() % span;
        switch (rnd() % 9) {
        case 0: b[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: b[at] = (uint8_t)rnd(); break;
        case 2: {
            static const uint32_t v[] = {0, 1, 2, 3, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x7FFFFFFFu, 0x80000000u,
                                         0xFFFFu, 0x10000u, 0xFFFFFFF0u, 44, 0x80000001u};
            const uint32_t x = (rnd() & 3) ? v[rnd() % (sizeof(v) / 4)] : rnd();
            for (int i = 0; i < 4 && at + (size_t)i < b.size(); ++i) b[at + (size_t)i] = (uint8_t)(x >> (8 * i));
            break;
        }
        case 3: b.resize(rnd() % (b.size() + 1)); break;
        case 4: {
            const int n = 1 + (int)(rnd() % 16);
            for (int i = 0; i < n; ++i) b.insert(b.begin() + (long)at, (uint8_t)rnd());
            break;
        }
        case 5: {
            const size_t n = 1 + rnd() % 32;
            b.erase(b.begin() + (long)at, b.begin() + (long)(at + n < b.size() ? at + n : b.size()));
            break;
        }
        case 6: {
            static const uint16_t v[] = {0, 1, 2, 3, 6, 7, 8, 12, 16, 24, 32, 64, 65, 0xFFFF, 0xFFFE, 0x8000};
            const uint16_t x = v[rnd() % (sizeof(v) / 2)];
            b[at] = (uint8_t)x;
            if (at + 1 < b.size()) b[at + 1] = (uint8_t)(x >> 8);
            break;
        }
        case 7: {   // copy a block elsewhere (duplicated chunk headers, shifted sizes)
            const size_t from = rnd() % b.size();
            const size_t n = 4 + rnd() % 60;
            for (size_t i = 0; i < n && from + i < b.size() && at + i < b.size(); ++i) b[at + i] = b[from + i];
            break;
        }
        case 8: {   // plant a chunk id
            static const char* ids[] = {"fmt ", "data", "smpl", "cue ", "LIST", "ds64", "RF64", "RIFF"};
            const char* id = ids[rnd() % 8];
            for (int i = 0; i < 4 && at + (size_t)i < b.size(); ++i) b[at + (size_t)i] = (uint8_t)id[i];
            break;
        }
        }
    }
}

static void testFuzz()
{
    const int kIters = 30000;
    int loaded = 0, rejected = 0, file_iters = 0;
    Guarded g;
    const std::string path = kDir + "/fuzz_case.wav";
    for (int it = 0; it < kIters; ++it) {
        Bytes b = g_seeds[rnd() % g_seeds.size()];
        mutate(b);
        g.set(b, (it & 1) == 0);
        WavInfo info;
        WavError err = WavError::Ok;
        Sample* s = wavLoadMem(g.data, (uint32_t)b.size(), "fuzz", &info, &err);
        const bool mem_ok = s != nullptr;
        if (s) {
            ++loaded;
            CHECKF(err == WavError::Ok, "iter %d", it);
            CHECKF(s->frames > 0 && (s->channels == 1 || s->channels == 2), "iter %d", it);
            CHECKF(guardOk(s), "iter %d: guard frames overwritten", it);
            CHECKF(info.frames >= s->frames && info.cue_count <= kMaxSlices, "iter %d", it);
            // Every frame must have come from the input: frames * block <= size.
            CHECKF((uint64_t)s->frames * s->channels <= b.size(), "iter %d: %u frames from %u bytes", it, s->frames, (unsigned)b.size());
            sampleFree(s);
        } else {
            ++rejected;
            CHECKF(err != WavError::Ok, "iter %d: null sample with Ok", it);
        }
        // Every 10th case also goes through the FILE* path (block reader, probe).
        if (it % 10 == 0) {
            ++file_iters;
            writeBytes(path, b);
            WavInfo pi;
            const WavError pe = wavProbe(path.c_str(), pi);
            Sample* f = wavLoad(path.c_str(), &info, &err, 60.0f);
            CHECKF((f != nullptr) == mem_ok, "iter %d: file and memory loaders disagree", it);
            if (f) {
                CHECKF(pe == WavError::Ok, "iter %d", it);
                CHECKF(guardOk(f), "iter %d", it);
                sampleFree(f);
            }
        }
        if (g_fails > 50) break;
    }
    std::printf("fuzz: %d mutated files (%d loaded, %d rejected), %d via file\n", kIters, loaded, rejected, file_iters);

    // Large files (several 64 KB blocks, frames split across blocks) with mutated headers and
    // random truncation, through the FILE* block reader.
    Bytes data;
    std::vector<int16_t> ex;
    genData(Fmt::S24, 2, 40000, data, ex);
    const Bytes big = riff({chunk("fmt ", fmtBody(Fmt::S24, 2, 48000, 16)), chunk("junk", Bytes(5, 1)), chunk("data", data)});
    int big_loaded = 0;
    for (int it = 0; it < 200; ++it) {
        Bytes b = big;
        for (int k = 0; k < 1 + (int)(rnd() % 4); ++k) b[rnd() % 96] = (uint8_t)rnd();
        if (rnd() & 1) b.resize(b.size() - rnd() % (b.size() / 2));
        writeBytes(path, b);
        WavInfo info;
        WavError err;
        Sample* f = wavLoad(path.c_str(), &info, &err, 60.0f);
        if (f) {
            ++big_loaded;
            CHECKF(guardOk(f) && (uint64_t)f->frames * f->channels <= b.size(), "big iter %d", it);
            sampleFree(f);
        }
    }
    std::printf("fuzz: 200 large mutated files via file (%d loaded)\n", big_loaded);
}

// ---------------------------------------------------------------- ops
static Sample* makeSine(uint32_t frames, int ch, uint32_t rate, float freq, float amp)
{
    Sample* s = sampleCreate(frames, ch, rate);
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
            s->data[(size_t)i * ch + c] = (int16_t)std::lround(amp * std::sin(2.0 * kPi * freq * i / rate) * (c ? 0.5 : 1.0));
    std::strcpy(s->name, "sine");
    std::strcpy(s->path, "/sdcard/ORBIT/USER/sine.wav");
    sampleAnalyze(s);
    return s;
}

// Frequency from rising zero crossings (linear interpolation) of channel 0 in [a, b).
static double measureFreq(const Sample* s, uint32_t a, uint32_t b)
{
    const int ch = s->channels;
    double first = -1.0, last = -1.0;
    int count = 0;
    for (uint32_t i = a + 1; i < b && i < s->frames; ++i) {
        const int x0 = s->data[(size_t)(i - 1) * ch], x1 = s->data[(size_t)i * ch];
        if (x0 < 0 && x1 >= 0) {
            const double t = (i - 1) + (double)(-x0) / (double)(x1 - x0);
            if (!count) first = t;
            last = t;
            ++count;
        }
    }
    return count > 1 ? (count - 1) / (last - first) * s->rate : 0.0;
}

static uint64_t checksum(const Sample* s)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < (size_t)s->frames * s->channels; ++i) h = (h ^ (uint16_t)s->data[i]) * 1099511628211ull;
    return h;
}

static void testOps()
{
    Sample* sine = makeSine(48000, 2, 48000, 440.0f, 8000.0f);
    const uint64_t sum0 = checksum(sine);

    Sample* n = opNormalize(sine, -0.3f);
    CHECK(n && std::fabs(n->peak - std::pow(10.0, -0.3 / 20.0)) < 1e-3);
    CHECK(n && n->id != sine->id && n->frames == sine->frames && n->path[0] == 0 && !std::strcmp(n->name, "sine"));
    sampleFree(n);
    n = opNormalize(sine, 0.0f);
    CHECK(n && n->peak > 0.999f);
    sampleFree(n);

    Sample* r = opReverse(sine);
    CHECK(r && r->frames == 48000);
    bool rev_ok = r != nullptr;
    for (uint32_t i = 0; r && i < 48000; ++i)
        if (r->data[2 * i] != sine->data[2 * (47999 - i)] || r->data[2 * i + 1] != sine->data[2 * (47999 - i) + 1]) rev_ok = false;
    CHECK(rev_ok);
    sampleFree(r);

    Sample* c = opCrop(sine, 0.25f, 0.75f);
    CHECK(c && c->frames == 24000 && c->data[0] == sine->data[2 * 12000] && c->data[1] == sine->data[2 * 12000 + 1]);
    CHECK(c && c->data[2 * 23999] == sine->data[2 * 35999]);
    sampleFree(c);
    c = opCrop(sine, 0.75f, 0.25f);   // swapped bounds are accepted
    CHECK(c && c->frames == 24000);
    sampleFree(c);
    CHECK(opCrop(sine, 0.5f, 0.5f) == nullptr);
    c = opCrop(sine, -1.0f, 2.0f);
    CHECK(c && c->frames == 48000);
    sampleFree(c);

    {   // fades on DC: gain curve visible directly
        Sample* dc = sampleCreate(48000, 1, 48000);
        for (int i = 0; i < 48000; ++i) dc->data[i] = 20000;
        Sample* f = opFade(dc, 10.0f, 20.0f);   // 480 / 960 frames
        CHECK(f && f->data[0] == 0);
        CHECK(f && std::abs(f->data[240] - (int)std::lround(20000 * std::sqrt(0.5))) <= 1);
        CHECK(f && f->data[480] == 20000 && f->data[47000] == 20000);
        CHECK(f && f->data[47999] == 0 && std::abs(f->data[48000 - 1 - 480] - (int)std::lround(20000 * std::sqrt(0.5))) <= 1);
        sampleFree(f);
        f = opFade(dc, 2000.0f, 2000.0f);   // longer than the sample: shrunk to fit
        CHECK(f && f->data[0] == 0 && f->data[47999] == 0 && f->data[24000] > 19900);
        sampleFree(f);

        Sample* g = opGain(dc, 6.0206f);
        CHECK(g && g->data[100] == 32767);   // 40000 clipped
        sampleFree(g);
        g = opGain(dc, -6.0206f);
        CHECK(g && std::abs(g->data[100] - 10000) <= 1);
        sampleFree(g);

        Sample* b = opBitcrush(dc, 4, 1);
        CHECK(b && b->data[0] % 4096 == 0);
        sampleFree(b);
        Sample* rs = makeSine(4800, 1, 48000, 100.0f, 16000.0f);
        b = opBitcrush(rs, 16, 4);
        bool held = b != nullptr;
        for (int i = 0; b && i < 4800; ++i)
            if (b->data[i] != rs->data[i / 4 * 4]) held = false;
        CHECK(held);
        sampleFree(b);
        sampleFree(rs);
        sampleFree(dc);
    }

    {   // trim silence: 1000 zeros + 5000-frame tone + 2000 zeros
        Sample* t = sampleCreate(8000, 1, 48000);
        for (int i = 0; i < 5000; ++i) t->data[1000 + i] = (int16_t)std::lround(16000 * std::sin(2 * kPi * 1000.0 * i / 48000.0));
        Sample* tr = opTrimSilence(t, -50.0f);
        CHECK(tr && tr->frames >= 4990 && tr->frames <= 5000 + 48 + 2);
        CHECK(tr && tr->data[0] == 0 && tr->data[1] == t->data[1001]);
        sampleFree(tr);
        Sample* z = sampleCreate(1000, 1, 48000);
        tr = opTrimSilence(z, -50.0f);
        CHECK(tr && tr->frames == 1000);   // all silent: unchanged copy
        sampleFree(tr);
        sampleFree(z);
        sampleFree(t);
    }

    Sample* m = opToMono(sine);
    CHECK(m && m->channels == 1 && m->frames == 48000);
    CHECK(m && m->data[100] == (int16_t)(((int)sine->data[200] + sine->data[201]) / 2));
    sampleFree(m);

    {   // resample 44.1 k -> 48 k: same frequency in Hz, exact length ratio
        Sample* s44 = makeSine(44100, 1, 44100, 1000.0f, 16000.0f);
        Sample* s48 = opResample(s44, 48000);
        CHECK(s48 && s48->rate == 48000 && std::abs((int)s48->frames - 48000) <= 1);
        if (s48) {
            const double f = measureFreq(s48, 4800, 43200);
            CHECKF(std::fabs(f - 1000.0) < 0.5, "resampled freq %.3f", f);
            CHECKF(std::fabs(s48->peak - s44->peak) < 0.01f, "peak %.4f vs %.4f", s48->peak, s44->peak);
        }
        Sample* back = s48 ? opResample(s48, 44100) : nullptr;
        CHECK(back && std::abs((int)back->frames - 44100) <= 1);
        if (back) {   // round trip error well below the 16-bit noise of the first quarter
            double err = 0.0, ref = 0.0;
            for (uint32_t i = 2000; i < 42000; ++i) {
                const double d = back->data[i] - s44->data[i];
                err += d * d;
                ref += (double)s44->data[i] * s44->data[i];
            }
            const double snr = 10.0 * std::log10(ref / (err + 1e-9));
            CHECKF(snr > 60.0, "round trip SNR %.1f dB", snr);
        }
        sampleFree(back);
        sampleFree(s48);
        Sample* st = makeSine(44100, 2, 44100, 1000.0f, 16000.0f);
        Sample* st48 = opResample(st, 48000);
        CHECK(st48 && st48->channels == 2 && std::abs((int)st48->frames - 48000) <= 1);
        if (st48) CHECK(std::fabs(measureFreq(st48, 4800, 43200) - 1000.0) < 0.5);
        sampleFree(st48);
        sampleFree(st);

        Sample* p = opPitch(s44, 12.0f);
        CHECK(p && p->rate == 44100 && std::abs((int)p->frames - 22050) <= 1);
        if (p) {
            const double f = measureFreq(p, 2000, 20000);
            CHECKF(std::fabs(f - 2000.0) < 1.0, "pitch +12 freq %.3f", f);
        }
        sampleFree(p);
        p = opPitch(s44, -12.0f);
        CHECK(p && std::abs((int)p->frames - 88200) <= 1);
        if (p) CHECK(std::fabs(measureFreq(p, 4000, 84000) - 500.0) < 0.5);
        sampleFree(p);
        p = opPitch(s44, 7.0f);
        CHECK(p && std::abs((int)p->frames - (int)std::lround(44100 / std::pow(2.0, 7.0 / 12.0))) <= 1);
        sampleFree(p);
        CHECK(opResample(s44, 10) == nullptr);
        sampleFree(s44);
    }

    Sample* d = opDuplicate(sine);
    CHECK(d && d->id != sine->id && checksum(d) == sum0 && d->frames == sine->frames);
    sampleFree(d);

    {   // zero crossing snap: crossings of a 100 Hz sine at 48 k every 240 frames
        Sample* z = makeSine(48000, 1, 48000, 100.0f, 10000.0f);
        const float p = snapToZeroCrossing(z, 2410.0f / 48000.0f, 5.0f);
        const int fr = (int)std::lround(p * 48000.0f);
        CHECKF(std::abs(fr - 2400) <= 1, "snap -> %d", fr);
        const float q = snapToZeroCrossing(z, 0.5f, 0.01f);   // window too small: unchanged
        CHECK(std::fabs(q - 0.5f) < 1e-6f || std::abs((int)std::lround(q * 48000.0f) - 24000) <= 1);
        sampleFree(z);
    }

    CHECK(checksum(sine) == sum0);   // the input of every op is untouched
    CHECK(opNormalize(nullptr) == nullptr && opReverse(nullptr) == nullptr && opResample(nullptr, 48000) == nullptr);
    sampleFree(sine);
    std::printf("ops: done\n");
}

// ---------------------------------------------------------------- slicing
// Synthetic hits. Each ends with a 5 ms release (a hard cut would itself be a broadband click).
static double release(size_t i, size_t len, uint32_t rate)
{
    const double r = 0.005 * rate;
    return (double)(len - i) < r ? (double)(len - i) / r : 1.0;
}
static void addKick(std::vector<float>& b, uint32_t rate, double at, float amp)
{
    double ph = 0.0;
    const size_t s0 = (size_t)std::lround(at * rate), len = (size_t)(0.35 * rate);
    for (size_t i = 0; i < len && s0 + i < b.size(); ++i) {   // 808 style: sine, pitch drop 170 -> 50 Hz
        const double t = (double)i / rate;
        ph += 2.0 * kPi * (50.0 + 120.0 * std::exp(-t * 30.0)) / rate;
        b[s0 + i] += (float)(amp * std::exp(-t * 8.0) * std::sin(ph) * release(i, len, rate));
    }
}
static void addSnare(std::vector<float>& b, uint32_t rate, double at, float amp)
{
    const size_t s0 = (size_t)std::lround(at * rate), len = (size_t)(0.2 * rate);
    for (size_t i = 0; i < len && s0 + i < b.size(); ++i) {
        const double t = (double)i / rate;
        const double noise = rnd01() * 2.0 - 1.0;
        b[s0 + i] += (float)(amp * release(i, len, rate) *
                             (0.5 * std::exp(-t * 25.0) * std::sin(2 * kPi * 190.0 * t) + 0.6 * std::exp(-t * 18.0) * noise));
    }
}
static void addHat(std::vector<float>& b, uint32_t rate, double at, float amp)
{
    const size_t s0 = (size_t)std::lround(at * rate), len = (size_t)(0.06 * rate);
    float prev = 0.0f;
    for (size_t i = 0; i < len && s0 + i < b.size(); ++i) {   // high-passed noise
        const double t = (double)i / rate;
        const float n = rnd01() * 2.0f - 1.0f;
        b[s0 + i] += (float)(amp * std::exp(-t * 60.0) * (n - prev) * 0.5 * release(i, len, rate));
        prev = n;
    }
}

static Sample* drumLoop(uint32_t rate, int ch, std::vector<double>& onsets, float noise = 0.0f)
{
    std::vector<float> buf((size_t)(2.0 * rate), 0.0f);
    struct Ev {
        char k;
        double t;
    };
    const Ev evs[] = {{'K', 0.0}, {'H', 0.25}, {'S', 0.5}, {'H', 0.75}, {'K', 1.0},
                      {'H', 1.125}, {'S', 1.5}, {'K', 1.625}, {'H', 1.75}, {'S', 1.875}};
    onsets.clear();
    for (const Ev& e : evs) {
        if (e.k == 'K') addKick(buf, rate, e.t, 0.8f);
        if (e.k == 'S') addSnare(buf, rate, e.t, 0.6f);
        if (e.k == 'H') addHat(buf, rate, e.t, 0.35f);
        onsets.push_back(e.t);
    }
    for (float& v : buf) v += noise * (rnd01() * 2.0f - 1.0f);   // optional noise floor
    Sample* s = sampleCreate((uint32_t)buf.size(), ch, rate);
    for (size_t i = 0; i < buf.size(); ++i)
        for (int c = 0; c < ch; ++c) {
            const float v = buf[i] * (c ? 0.8f : 1.0f) * 32767.0f;
            s->data[i * ch + c] = (int16_t)(v > 32767.f ? 32767 : (v < -32768.f ? -32768 : std::lround(v)));
        }
    sampleAnalyze(s);
    return s;
}

static void checkOnsets(const Sample* s, const std::vector<double>& expect, double t0, double t1, float sens, const char* what)
{
    float pos[kMaxSlices];
    const double dur = (double)s->frames / s->rate;
    const int n = sliceTransients(s, (float)(t0 / dur), (float)(t1 / dur), sens, kMaxSlices, pos);
    std::vector<double> inside;
    for (double t : expect)
        if (t > t0 + 0.02 && t < t1) inside.push_back(t);
    CHECKF(n == (int)inside.size() + 1, "%s: %d slices, expected %d", what, n, (int)inside.size() + 1);
    CHECK(n >= 1 && std::fabs(pos[0] - t0 / dur) < 1e-6);
    for (int i = 1; i < n; ++i) CHECK(pos[i] > pos[i - 1]);
    const int before = g_fails;
    double worst = 0.0;
    for (double t : inside) {
        double best = 1e9;
        for (int i = 1; i < n; ++i) best = std::fmin(best, std::fabs(pos[i] * dur - t));
        CHECKF(best <= 0.010, "%s: onset %.3f s missed (nearest %.1f ms)", what, t, best * 1000.0);
        worst = std::fmax(worst, best);
    }
    std::printf("  onsets %s: %d slices, worst error %.2f ms\n", what, n, worst * 1000.0);
    if (g_fails != before || n != (int)inside.size() + 1) {
        std::printf("  detected:");
        for (int i = 0; i < n; ++i) std::printf(" %.4f", pos[i] * dur);
        std::printf("\n");
    }
}

static void testSlicing()
{
    float pos[kMaxSlices];
    CHECK(sliceGrid(0.1f, 0.9f, 4, pos) == 4);
    CHECK(std::fabs(pos[0] - 0.1f) < 1e-6f && std::fabs(pos[1] - 0.3f) < 1e-6f && std::fabs(pos[3] - 0.7f) < 1e-6f);
    CHECK(sliceGrid(0.0f, 1.0f, 16, pos) == 16 && std::fabs(pos[15] - 0.9375f) < 1e-6f);
    CHECK(sliceGrid(0.0f, 1.0f, 40, pos) == kMaxSlices);
    CHECK(sliceGrid(0.5f, 0.5f, 4, pos) == 0 && sliceGrid(0.0f, 1.0f, 0, pos) == 0);

    std::vector<double> on;
    Sample* loop = drumLoop(48000, 1, on);
    checkOnsets(loop, on, 0.0, 2.0, 0.5f, "48k mono");
    checkOnsets(loop, on, 0.8, 2.0, 0.5f, "48k region");
    {
        const int lo = sliceTransients(loop, 0.0f, 1.0f, 0.0f, kMaxSlices, pos);
        const int mid = sliceTransients(loop, 0.0f, 1.0f, 0.5f, kMaxSlices, pos);
        const int hi = sliceTransients(loop, 0.0f, 1.0f, 1.0f, kMaxSlices, pos);
        CHECKF(lo <= mid && mid <= hi, "sensitivity %d %d %d", lo, mid, hi);
        for (int i = 1; i < hi; ++i) CHECK((pos[i] - pos[i - 1]) * 2.0f >= 0.040f - 1e-4f);   // 40 ms gap (2 s loop)
        const int four = sliceTransients(loop, 0.0f, 1.0f, 0.5f, 4, pos);
        CHECK(four == 4 && pos[0] == 0.0f && pos[1] < pos[2] && pos[2] < pos[3]);
    }
    sampleFree(loop);
    Sample* loop44 = drumLoop(44100, 2, on);
    checkOnsets(loop44, on, 0.0, 2.0, 0.5f, "44.1k stereo");
    sampleFree(loop44);
    for (int seed = 0; seed < 3; ++seed) {   // other noise realisations + a -54 dB noise floor
        Sample* nl = drumLoop(48000, 1, on, 0.002f);
        checkOnsets(nl, on, 0.0, 2.0, 0.5f, "48k noisy");
        sampleFree(nl);
    }

    Sample* silent = sampleCreate(48000, 1, 48000);
    CHECK(sliceTransients(silent, 0.0f, 1.0f, 1.0f, kMaxSlices, pos) == 1 && pos[0] == 0.0f);
    sampleFree(silent);
    Sample* tiny = sampleCreate(100, 1, 48000);
    CHECK(sliceTransients(tiny, 0.2f, 1.0f, 0.5f, kMaxSlices, pos) == 1 && std::fabs(pos[0] - 0.2f) < 1e-6f);
    CHECK(sliceTransients(tiny, 0.5f, 0.2f, 0.5f, kMaxSlices, pos) == 0);
    sampleFree(tiny);
    std::printf("slicing: done\n");
}

// ---------------------------------------------------------------- library
static void testLibrary()
{
    const std::string root = kDir + "/lib/ORBIT";
    rmrf(kDir + "/lib");
    mkdirs(kDir + "/lib");
    Library lib;
    CHECK(lib.open(root.c_str()));
    for (const char* sub : {"KITS", "SAMPLES/Drums", "SAMPLES/Bass", "SAMPLES/Synth", "SAMPLES/Vocals", "SAMPLES/FX", "USER", "PROJECTS"})
        CHECKF(isDirPath(root + "/" + sub), "missing %s", sub);
    CHECK(!std::strcmp(lib.root(), root.c_str()));

    Bytes data;
    std::vector<int16_t> ex;
    genData(Fmt::S16, 1, 100, data, ex);
    const Bytes wav = riff({chunk("fmt ", fmtBody(Fmt::S16, 1, 44100, 16)), chunk("data", data)});
    auto w = [&](const std::string& rel) {
        const std::string p = root + "/" + rel;
        mkdirs(p.substr(0, p.rfind('/')));
        CHECKF(writeBytes(p, wav), "write %s", rel.c_str());
    };
    w("KITS/TR-808/SD.wav");
    w("KITS/TR-808/BD.wav");
    w("KITS/TR-808/CH.wav");
    w("KITS/Mapped/kick.wav");
    w("KITS/Mapped/snare.wav");
    w("KITS/Mapped/hat.wav");
    writeText(root + "/KITS/Mapped/kit.txt", "\xEF\xBB\xBF# pad map\r\n1=kick.wav\r\n03 = snare.wav\nB1=hat.wav\n17=bad.wav\nX=bad.wav\n\n");
    mkdirs(root + "/KITS/Empty");
    writeText(root + "/KITS/Empty/notes.txt", "nothing");
    for (int i = 1; i <= 20; ++i) w("KITS/Big/s" + std::to_string(i) + ".wav");
    w("SAMPLES/Drums/Kick 2.wav");
    w("SAMPLES/Drums/Kick 10.wav");
    w("SAMPLES/Drums/Kick 1.WAV");
    w("SAMPLES/Drums/sub/deeper/Clap.wav");
    w("SAMPLES/Drums/._Kick.wav");
    writeText(root + "/SAMPLES/Drums/readme.txt", "x");
    w("SAMPLES/bass/\xC5\xBB\xC3\xB3\xC5\x82\xC4\x87 bas.wav");       // "Żółć bas" (folder in lower case)
    w("SAMPLES/Synth/\xC5\x81\xC3\xB3\x64\xC5\xBA pad.wav");          // "Łódź pad"
    w("SAMPLES/Vocals/G\xC5\x82os.wav");                             // "Głos"
    w("SAMPLES/FX/a/b/c/ok.wav");                                    // depth 4: kept
    w("SAMPLES/FX/a/b/c/d/deep.wav");                                // depth 5: skipped
    w("SAMPLES/Loops/loop.wav");                                     // unknown category -> All
    w("USER/rec1.wav");
    w("USER/edits/rec2.wav");

    const int n = lib.scan();
    CHECKF(n == 3 + 4 + 1 + 1 + 1 + 1 + 1 + 2, "scan count %d", n);
    CHECK(lib.count() == n && lib.scan() == n);   // rescan is stable
    for (int i = 0; i < n; ++i) CHECK(lib.at(i).path[0] != 0);
    CHECK(lib.at(-1).path[0] == 0 && lib.at(n).path[0] == 0);

    int idx[kMaxLibraryFiles];
    auto names = [&](Category c, const char* q) {
        const int k = lib.filter(c, q, idx, kMaxLibraryFiles);
        std::string s;
        for (int i = 0; i < k; ++i) s += std::string(i ? "|" : "") + lib.at(idx[i]).name;
        return s;
    };
    CHECKF(names(Category::Kits, "") == "Big|Mapped|TR-808", "kits: %s", names(Category::Kits, "").c_str());
    CHECKF(names(Category::Drums, "") == "Kick 1|Kick 2|Kick 10|Clap", "drums: %s", names(Category::Drums, "").c_str());
    CHECKF(names(Category::Bass, "") == "\xC5\xBB\xC3\xB3\xC5\x82\xC4\x87 bas", "bass: %s", names(Category::Bass, "").c_str());
    CHECK(names(Category::Fx, "") == "ok");
    CHECK(names(Category::User, "") == "rec2|rec1" || names(Category::User, "") == "rec1|rec2");
    CHECKF(lib.filter(Category::All, "", idx, kMaxLibraryFiles) == n - 3, "all");
    CHECK(names(Category::All, "loop") == "loop");
    CHECK(names(Category::Drums, "KICK") == "Kick 1|Kick 2|Kick 10");
    CHECK(names(Category::All, "zolc") == "\xC5\xBB\xC3\xB3\xC5\x82\xC4\x87 bas");
    CHECK(names(Category::All, "\xC5\xBB\xC3\x93\xC5\x81\xC4\x86") == "\xC5\xBB\xC3\xB3\xC5\x82\xC4\x87 bas");   // "ŻÓŁĆ"
    CHECK(names(Category::All, "lodz") == "\xC5\x81\xC3\xB3\x64\xC5\xBA pad");
    CHECK(names(Category::All, " g\xC5\x81OS ") == "G\xC5\x82os");   // " gŁOS "
    CHECK(names(Category::Kits, "808") == "TR-808");
    CHECK(names(Category::Synth, "kick").empty());
    CHECK(lib.filter(Category::All, "", idx, 2) == 2);
    CHECK(!std::strcmp(categoryName(Category::All), "All Samples") && !std::strcmp(categoryName(Category::Fx), "FX"));

    // probe
    lib.filter(Category::Drums, "Kick 1", idx, 1);
    lib.probe(idx[0]);
    const LibEntry& e = lib.at(idx[0]);
    CHECK(e.probed && e.rate == 44100 && e.channels == 1 && e.bits == 16 && std::fabs(e.seconds - 100.0f / 44100.0f) < 1e-6f);
    CHECK(e.bytes == wav.size() && e.cat == Category::Drums && !e.is_kit);

    // kits
    char paths[16][kPathLen];
    auto kitIndex = [&](const char* name) {
        const int k = lib.filter(Category::Kits, name, idx, kMaxLibraryFiles);
        return k ? idx[0] : -1;
    };
    int k = lib.kitFiles(kitIndex("TR-808"), paths, 16);
    CHECK(k == 3 && std::strstr(paths[0], "/BD.wav") && std::strstr(paths[1], "/CH.wav") && std::strstr(paths[2], "/SD.wav"));
    k = lib.kitFiles(kitIndex("Mapped"), paths, 16);
    CHECKF(k == 5, "mapped count %d", k);
    CHECK(std::strstr(paths[0], "Mapped/kick.wav") && !paths[1][0] && std::strstr(paths[2], "Mapped/snare.wav") && !paths[3][0] &&
          std::strstr(paths[4], "Mapped/hat.wav"));
    k = lib.kitFiles(kitIndex("Big"), paths, 16);
    CHECK(k == 16 && std::strstr(paths[0], "/s1.wav") && std::strstr(paths[1], "/s2.wav") && std::strstr(paths[9], "/s10.wav") &&
          std::strstr(paths[15], "/s16.wav"));
    CHECK(lib.kitFiles(kitIndex("Big"), paths, 4) == 4);
    lib.filter(Category::Drums, "", idx, 1);
    CHECK(lib.kitFiles(idx[0], paths, 16) == 0);   // not a kit

    // projects
    char path[kPathLen];
    CHECK(lib.projectPath("Beat", path, sizeof path) && std::string(path) == root + "/PROJECTS/Beat.orb");
    CHECK(lib.projectDir("Beat", path, sizeof path) && std::string(path) == root + "/PROJECTS/Beat/");
    CHECK(lib.userPath("rec 3.wav", path, sizeof path) && std::string(path) == root + "/USER/rec 3.wav");
    CHECK(!lib.projectPath("a/b", path, sizeof path) && !lib.projectPath("", path, sizeof path) && !lib.projectPath("..", path, sizeof path));
    CHECK(!lib.projectPath("Beat", path, 10));
    const char* pn[3] = {"old", "new", "mid"};
    const long times[3] = {1000000000L, 1700000000L, 1500000000L};
    for (int i = 0; i < 3; ++i) {
        const std::string p = root + "/PROJECTS/" + pn[i] + ".orb";
        writeText(p, "{}");
        struct utimbuf ut;
        ut.actime = ut.modtime = (time_t)times[i];
        utime(p.c_str(), &ut);
    }
    writeText(root + "/PROJECTS/notes.txt", "x");
    mkdirs(root + "/PROJECTS/new");
    char pnames[8][32];
    const int np = lib.listProjects(pnames, 8);
    CHECKF(np == 3 && !std::strcmp(pnames[0], "new") && !std::strcmp(pnames[1], "mid") && !std::strcmp(pnames[2], "old"), "projects %d", np);
    CHECK(lib.listProjects(pnames, 1) == 1 && !std::strcmp(pnames[0], "new"));
    CHECK(lib.freeBytes() == 0 && lib.totalBytes() == 0);   // weak orbit_fs_info: unknown on the PC

    // a second instance over the same card sees the same entries (the app scans in the back one)
    Library other;
    CHECK(other.open((root + "/").c_str()) && other.scan() == n && !std::strcmp(other.root(), root.c_str()));
    Library closed;
    CHECK(closed.count() == 0 && closed.scan() == 0 && closed.filter(Category::All, "", idx, 10) == 0 && closed.root()[0] == 0);
    CHECK(!lib.truncated() && !closed.truncated());
    std::printf("library: %d entries\n", n);
}

// More SAMPLES files than kMaxLibraryFiles: the list is truncated (and says so), but the user's
// own files (USER: SAVE WAV output, recordings) are never the ones dropped. Own card per process:
// thousands of files must not leak into other runs of this test.
static void testLibraryTruncation()
{
#if defined(_WIN32)
    const unsigned pid = (unsigned)GetCurrentProcessId();
#else
    const unsigned pid = (unsigned)getpid();
#endif
    const std::string base = kDir + "/libbulk_" + std::to_string(pid);
    const std::string root = base + "/ORBIT";
    rmrf(base);
    mkdirs(base);
    Library lib;
    CHECK(lib.open(root.c_str()));
    const auto touch = [](const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f) std::fclose(f);
        return f != nullptr;
    };
    mkdirs(root + "/USER/edits");
    mkdirs(root + "/KITS/Kit");
    CHECK(touch(root + "/USER/rec1.wav") && touch(root + "/USER/edits/rec2.wav") && touch(root + "/KITS/Kit/BD.wav"));
    const std::string bulk = root + "/SAMPLES/Bulk";
    mkdirs(bulk);
    bool made = true;
    for (int i = 0; i < kMaxLibraryFiles + 8; ++i) {
        char nm[32];
        std::snprintf(nm, sizeof nm, "/b%05d.wav", i);
        made = touch(bulk + nm) && made;
    }
    CHECK(made);
    const int nt = lib.scan();
    CHECKF(nt == kMaxLibraryFiles && lib.truncated(), "bulk scan %d truncated %d", nt, (int)lib.truncated());
    static int idx[kMaxLibraryFiles];
    auto names = [&](Category c) {
        const int k = lib.filter(c, "", idx, kMaxLibraryFiles);
        std::string s;
        for (int i = 0; i < k; ++i) s += std::string(i ? "|" : "") + lib.at(idx[i]).name;
        return s;
    };
    CHECKF(names(Category::User) == "rec2|rec1" || names(Category::User) == "rec1|rec2", "user after truncation: %s",
           names(Category::User).c_str());
    CHECK(names(Category::Kits) == "Kit");
    CHECK(lib.filter(Category::Drums, "", idx, kMaxLibraryFiles) == 0);
    rmrf(bulk);
    CHECK(lib.scan() == 3 && !lib.truncated());
    rmrf(base);
    std::printf("library truncation: %d of %d entries kept, USER first\n", nt, kMaxLibraryFiles + 11);
}

// ---------------------------------------------------------------- persistence
static void randomProject(Project& p, SampleRef refs[kPads][kLayers])
{
    projectDefaults(p);
    std::strcpy(p.name, "M\xC3\xB3j projekt \xC4\x85\xC4\x99");   // "Mój projekt ąę"
    p.bpm = 87.35f;
    p.scene = 2;
    p.beats_per_bar = 3;
    p.quant = LaunchQuant::Eighth;
    p.metronome = 1;
    p.count_in = 2;
    p.lofi_on = 1;
    p.wide_on = 0;
    p.pump_on = 1;
    p.space_on = 1;
    p.limiter_on = 0;
    p.pump_src = 5;
    for (int k = 0; k < kGlobalParamCount; ++k) p.g[k] = fromNorm(globalSpec((GlobalParam)k), rnd01());
    for (int b = 0; b < kBuses; ++b) {
        std::snprintf(p.bus[b].name, sizeof p.bus[b].name, "BUS %d", b);
        p.bus[b].level = rnd01();
        p.bus[b].pan = rnd01() * 2.0f - 1.0f;
        p.bus[b].mute = (uint8_t)(rnd() & 1);
        p.bus[b].solo = (uint8_t)(rnd() & 1);
    }
    for (int i = 0; i < kPads; ++i) {
        Pad& pad = p.pad[i];
        std::memset(pad.name, 0, sizeof pad.name);
        std::snprintf(pad.name, sizeof pad.name, "Pad \xC5\x9B\xC4\x87 %d", i);   // "Pad ść N"
        for (int l = 0; l < kLayers; ++l) {
            Layer& L = pad.layer[l];
            L.slot = -1;
            L.tune = (float)((int)(rnd() % 97) - 48) * 0.5f + rnd01() * 0.4f;
            L.gain_db = -24.0f + rnd01() * 30.0f;
            L.pan = rnd01() * 2.0f - 1.0f;
            L.vel_lo = (uint8_t)(rnd() % 64);
            L.vel_hi = (uint8_t)(64 + rnd() % 64);
            SampleRef& r = refs[i][l];
            r = SampleRef{};
            const int kind = (i + l) % 3;
            if (kind == 0) {
                std::snprintf(r.path, sizeof r.path, "/sdcard/ORBIT/SAMPLES/Drums/Kick \xC5\xBC %d-%d.wav", i, l);
                std::snprintf(r.name, sizeof r.name, "Kick \xC5\xBC %d", i);
            } else if (kind == 1) {
                std::strcpy(r.path, "builtin:TR-808/BD");
                std::strcpy(r.name, "BD");
            }
        }
        pad.layer_mode = (LayerMode)(rnd() % (int)LayerMode::Count);
        pad.mode = (PlayMode)(rnd() % (int)PlayMode::Count);
        pad.reverse = (uint8_t)(rnd() & 1);
        pad.bus = (uint8_t)(rnd() % kBuses);
        pad.choke = (uint8_t)(rnd() % 9);
        pad.poly = (uint8_t)(1 + rnd() % kMaxPadPoly);
        pad.mute = (uint8_t)(rnd() & 1);
        pad.solo = (uint8_t)(rnd() & 1);
        pad.slice_count = (uint8_t)(rnd() % (kMaxSlices + 1));
        pad.slice_sel = pad.slice_count ? (uint8_t)(rnd() % pad.slice_count) : 0;
        for (int s = 0; s < kMaxSlices; ++s) pad.slices[s] = 0.0f;
        for (int s = 0; s < pad.slice_count; ++s) pad.slices[s] = (float)s / (float)(pad.slice_count + 1) + rnd01() * 0.01f;
        if (pad.slice_count && pad.slice_count < kMaxSlices) pad.slices[pad.slice_count] = 0.97f;   // stored region end
        pad.slice_end = pad.slice_count ? 0.97f - rnd01() * 0.01f : 1.0f;   // Pad::slice_end (GRID 16 too)
        for (int k = 0; k < kPadParamCount; ++k) pad.p[k] = fromNorm(padSpec((PadParam)k), rnd01());
    }
    for (int s = 0; s < kScenes; ++s) {
        Scene& sc = p.scene_data[s];
        sc.length = (uint8_t)(1 + rnd() % kMaxSteps);
        sc.div = (Division)(rnd() % (int)Division::Count);
        sc.swing = (uint8_t)(50 + rnd() % 26);
        for (int t = 0; t < kPads; ++t) {
            Track& tr = sc.track[t];
            tr.length = (uint8_t)(rnd() % (kMaxSteps + 1));
            for (int i = 0; i < kMaxSteps; ++i) {
                Step& st = tr.step[i];
                st = Step{};
                if (rnd() % 4) {
                    if (rnd() % 8 == 0) {   // an OFF step whose lanes were edited: kept as they are
                        st.vel = (uint8_t)(1 + rnd() % 127);
                        st.prob = (uint8_t)(rnd() % 101);
                        st.ratchet = (uint8_t)(1 + rnd() % 4);
                        st.nudge = (int8_t)((int)(rnd() % 101) - 50);
                    }
                    continue;
                }
                st.on = 1;
                st.vel = (uint8_t)(1 + rnd() % 127);
                st.prob = (uint8_t)(rnd() % 101);
                st.ratchet = (uint8_t)(1 + rnd() % 4);
                st.nudge = (int8_t)((int)(rnd() % 101) - 50);
                st.pitch = (int8_t)((int)(rnd() % 49) - 24);
                st.cond = (TrigCond)(rnd() % (int)TrigCond::Count);
                st.flam = (uint8_t)(rnd() % 9);
            }
        }
    }
    for (int i = 0; i < kSongSlots; ++i) {
        p.song.slot[i] = (int8_t)((int)(rnd() % 5) - 1);
        p.song.fx[i] = (uint8_t)(rnd() & 1);
    }
    p.song.follow = 1;
}

static void reportDiff(const Project& a, const Project& b)
{
    const uint8_t* x = (const uint8_t*)&a;
    const uint8_t* y = (const uint8_t*)&b;
    for (size_t i = 0; i < sizeof(Project); ++i)
        if (x[i] != y[i]) {
            const size_t pad0 = offsetof(Project, pad), sc0 = offsetof(Project, scene_data);
            std::printf("  first difference at byte %u", (unsigned)i);
            if (i >= pad0 && i < sc0) {
                const size_t o = (i - pad0) % sizeof(Pad);
                std::printf(" (pad %u, offset %u; p[] at %u)", (unsigned)((i - pad0) / sizeof(Pad)), (unsigned)o, (unsigned)offsetof(Pad, p));
            }
            std::printf("\n");
            return;
        }
}

static std::string readAll(const std::string& path)
{
    std::string s;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

static void testPersist()
{
    static Project a, b, c, def;
    static SampleRef ra[kPads][kLayers], rb[kPads][kLayers];
    char err[96] = {};
    projectDefaults(def);
    randomProject(a, ra);
    const std::string path = kDir + "/roundtrip.orb";
    std::remove(path.c_str());
    CHECKF(projectSave(path.c_str(), a, ra, err, sizeof err), "save: %s", err);
    CHECKF(projectSave(path.c_str(), a, ra, err, sizeof err), "overwrite: %s", err);
    CHECK(!exists(path + ".tmp") && !exists(path + ".bak"));
    std::memset(&b, 0x5A, sizeof b);
    CHECKF(projectLoad(path.c_str(), b, rb, err, sizeof err), "load: %s", err);
    const bool same = std::memcmp(&a, &b, sizeof(Project)) == 0;
    CHECK(same);
    if (!same) reportDiff(a, b);
    bool refs_same = true;
    for (int i = 0; i < kPads; ++i)
        for (int l = 0; l < kLayers; ++l)
            if (std::strcmp(ra[i][l].path, rb[i][l].path) || std::strcmp(ra[i][l].name, rb[i][l].name)) refs_same = false;
    CHECK(refs_same);

    const std::string text = readAll(path);
    CHECK(text.find("\"orbit-project\"") != std::string::npos && text.find("\"cutoff\"") != std::string::npos);

    // A file from before Pad::slice_end (no "slice_end" key): the last slice ends at the stored marker
    // after the slices when there is one (count < 16 and above the last start), else at 1.0.
    {
        cJSON* j = cJSON_Parse(text.c_str());
        cJSON* pads = j ? cJSON_GetObjectItem(j, "pads") : nullptr;
        int removed = 0;
        for (cJSON* po = pads ? pads->child : nullptr; po; po = po->next)
            if (cJSON_GetObjectItem(po, "slice_end")) {
                cJSON_DeleteItemFromObject(po, "slice_end");
                ++removed;
            }
        CHECK(removed == kPads);
        char* old_text = j ? cJSON_Print(j) : nullptr;
        const std::string legacy = kDir + "/legacy_slices.orb";
        writeText(legacy, old_text ? old_text : "");
        if (old_text) cJSON_free(old_text);
        cJSON_Delete(j);
        CHECKF(projectLoad(legacy.c_str(), c, nullptr, err, sizeof err), "legacy: %s", err);
        int bad = 0, stored = 0;
        for (int i = 0; i < kPads; ++i) {
            const Pad& p = a.pad[i];
            const int n = p.slice_count;
            const bool has = n > 0 && n < kMaxSlices && p.slices[n] > p.slices[n - 1];
            stored += has;
            if (c.pad[i].slice_end != (has ? p.slices[n] : 1.0f)) ++bad;
        }
        CHECKF(bad == 0, "legacy slice_end: %d pads wrong (%d with a stored end)", bad, stored);
    }
    CHECK(text.find("CUTOFF") == std::string::npos && text.find("\"builtin\"") != std::string::npos);
    CHECK(text.find("\"exported\"") == std::string::npos);

    // export: same content + "exported": true, loadable
    const std::string ex = kDir + "/export.json";
    CHECK(projectExportJson(ex.c_str(), a, ra));
    {
        const std::string et = readAll(ex);
        cJSON* j = cJSON_Parse(et.c_str());
        CHECK(j && cJSON_IsTrue(cJSON_GetObjectItem(j, "exported")));
        cJSON_Delete(j);
        CHECK(projectLoad(ex.c_str(), c, nullptr, err, sizeof err) && std::memcmp(&a, &c, sizeof(Project)) == 0);
    }

    // minimal file: everything at defaults
    const std::string mini = kDir + "/minimal.orb";
    writeText(mini, "{\"format\":\"orbit-project\",\"version\":1}");
    CHECK(projectLoad(mini.c_str(), c, rb, err, sizeof err) && std::memcmp(&c, &def, sizeof(Project)) == 0);
    CHECK(rb[0][0].path[0] == 0);

    // unknown keys ignored, missing keys keep defaults, values range-checked
    const std::string partial = kDir + "/partial.orb";
    writeText(partial,
              "\xEF\xBB\xBF{\"format\":\"orbit-project\",\"version\":1,\"bpm\":99.5,\"future\":{\"x\":[1,2,{\"y\":null}]},"
              "\"quant\":\"beat\",\"metronome\":true,\"count_in\":7,"
              "\"pads\":[{\"name\":\"Custom\",\"mode\":\"loop\",\"poly\":99,\"params\":{\"cutoff\":1000,\"tune\":-7,\"bogus\":3,\"volume\":\"loud\"},"
              "\"layers\":[{\"path\":\"/sd/a.wav\",\"name\":\"A\",\"weird\":1,\"tune\":100},{\"builtin\":\"TR-909/SD\"}]},"
              "{\"mode\":\"nonsense\",\"layer_mode\":2}],"
              "\"scenes\":[{\"swing\":10,\"tracks\":[{\"steps\":[{\"i\":3,\"v\":500,\"zz\":1,\"n\":-100,\"c\":2},{\"i\":99},{\"v\":5}]}]}],"
              "\"globals\":{\"master\":-6,\"rev_size\":7},\"song\":{\"slots\":[1,9,\"x\"]}}");
    CHECKF(projectLoad(partial.c_str(), c, rb, err, sizeof err), "partial: %s", err);
    CHECK(c.bpm == 99.5f && c.quant == LaunchQuant::Beat && c.metronome == 1 && c.count_in == 2);
    CHECK(!std::strcmp(c.pad[0].name, "Custom") && c.pad[0].mode == PlayMode::Loop && c.pad[0].poly == kMaxPadPoly);
    CHECK(c.pad[0].p[PP_CUTOFF] == 1000.0f && c.pad[0].p[PP_TUNE] == -7.0f && c.pad[0].p[PP_VOLUME] == def.pad[0].p[PP_VOLUME]);
    CHECK(c.pad[0].p[PP_ATTACK] == def.pad[0].p[PP_ATTACK] && c.pad[0].layer[0].tune == 48.0f);
    CHECK(!std::strcmp(rb[0][0].path, "/sd/a.wav") && !std::strcmp(rb[0][0].name, "A") && !std::strcmp(rb[0][1].path, "builtin:TR-909/SD"));
    CHECK(c.pad[1].mode == def.pad[1].mode && c.pad[1].layer_mode == LayerMode::RoundRobin && !std::strcmp(c.pad[1].name, def.pad[1].name));
    CHECK(std::memcmp(&c.pad[2], &def.pad[2], sizeof(Pad)) == 0);
    const Track& t0 = c.scene_data[0].track[0];
    int on = 0;
    for (int i = 0; i < kMaxSteps; ++i) on += t0.step[i].on;
    CHECK(on == 1 && t0.step[3].on && t0.step[3].vel == 127 && t0.step[3].nudge == -50 && t0.step[3].cond == TrigCond::P2of2);
    CHECK(c.scene_data[0].swing == 50 && c.scene_data[0].length == def.scene_data[0].length);
    CHECK(std::memcmp(&c.scene_data[0].track[4], &def.scene_data[0].track[4], sizeof(Track)) == 0);   // not listed: default
    CHECK(std::memcmp(&c.scene_data[1], &def.scene_data[1], sizeof(Scene)) == 0);
    CHECK(c.g[GP_MASTER] == -6.0f && c.g[GP_REV_SIZE] == globalSpec(GP_REV_SIZE).max && c.g[GP_DLY_TONE] == def.g[GP_DLY_TONE]);
    CHECK(c.song.slot[0] == 1 && c.song.slot[1] == -1 && c.song.slot[2] == -1 && c.song.slot[3] == -1);
    CHECK(std::memcmp(c.bus, def.bus, sizeof(c.bus)) == 0);

    // newer version: loads the known fields, says so
    const std::string newer = kDir + "/newer.orb";
    writeText(newer, "{\"format\":\"orbit-project\",\"version\":7,\"bpm\":140,\"new_thing\":[]}");
    err[0] = 0;
    CHECK(projectLoad(newer.c_str(), c, nullptr, err, sizeof err) && c.bpm == 140.0f && err[0] != 0);

    // rejected files leave the project untouched
    std::memcpy(&c, &a, sizeof(Project));
    const char* bad[] = {"{\"format\":\"orbit-project\"}", "{\"format\":\"other\",\"version\":1}",
                         "{\"format\":\"orbit-project\",\"version\":0}", "{\"format\":\"orbit-project\",\"version\":1",
                         "[1,2,3]", "", "\x00\x01\x02"};
    for (const char* t : bad) {
        const std::string bp = kDir + "/bad.orb";
        writeText(bp, t);
        err[0] = 0;
        CHECKF(!projectLoad(bp.c_str(), c, rb, err, sizeof err) && err[0], "accepted '%s'", t);
    }
    CHECK(!projectLoad((kDir + "/nope.orb").c_str(), c, rb, err, sizeof err) && err[0]);
    CHECK(std::memcmp(&c, &a, sizeof(Project)) == 0);

    // crash between the renames: only <path>.bak exists
    const std::string lost = kDir + "/lost.orb";
    std::remove(lost.c_str());
    CHECK(projectSave((lost + ".bak").c_str(), a, ra, err, sizeof err));
    CHECK(projectLoad(lost.c_str(), b, nullptr, err, sizeof err) && std::memcmp(&a, &b, sizeof(Project)) == 0);
    std::remove((lost + ".bak").c_str());

    // defaults round trip as well (demo patterns, demo song); untouched off steps are not written
    CHECK(projectSave(path.c_str(), def, nullptr, err, sizeof err) && projectLoad(path.c_str(), b, rb, err, sizeof err));
    CHECK(std::memcmp(&b, &def, sizeof(Project)) == 0);
    CHECK(readAll(path).find("\"o\"") == std::string::npos);

    // the ghost-note case: a step prepared in the VELOCITY / PROBABILITY / RATCHET lanes but left
    // off keeps its values (it used to come back as 100 / 100 / 1)
    {
        std::memcpy(&c, &def, sizeof(Project));
        Step& g = c.scene_data[1].track[3].step[5];
        g = Step{};
        g.vel = 30;
        g.prob = 40;
        g.ratchet = 3;
        g.cond = TrigCond::Fill;
        Step& h = c.scene_data[1].track[3].step[6];   // only the pitch lane edited
        h = Step{};
        h.pitch = -7;
        CHECK(projectSave(path.c_str(), c, nullptr, err, sizeof err) && projectLoad(path.c_str(), b, rb, err, sizeof err));
        const Step& g2 = b.scene_data[1].track[3].step[5];
        const Step& h2 = b.scene_data[1].track[3].step[6];
        CHECKF(g2.on == 0 && g2.vel == 30 && g2.prob == 40 && g2.ratchet == 3 && g2.cond == TrigCond::Fill,
               "off step: on %d vel %d prob %d ratchet %d", g2.on, g2.vel, g2.prob, g2.ratchet);
        CHECK(h2.on == 0 && h2.pitch == -7 && h2.vel == 100);
        CHECK(std::memcmp(&b, &c, sizeof(Project)) == 0);
    }
    CHECK(!projectSave("", a, ra, err, sizeof err) && err[0]);
    CHECK(!projectSave((kDir + "/no_such_dir/x.orb").c_str(), a, ra, err, sizeof err) && err[0]);
    std::printf("persist: project %u bytes, file %u bytes\n", (unsigned)sizeof(Project), (unsigned)text.size());
}

// Optional argument: run only the sections whose names it contains ("slice", "lib,persist"...).
int main(int argc, char** argv)
{
    mkdirs(kDir);
    if (!isDirPath(kDir)) {
        std::printf("run from the project root (no %s)\nsampleio: FAILED\n", kDir.c_str());
        return 1;
    }
    const std::string only = argc > 1 ? argv[1] : "";
    auto want = [&](const char* name) { return only.empty() || only.find(name) != std::string::npos; };
    if (want("wav") || want("fuzz")) testWavFormats();
    if (want("fuzz")) testFuzz();
    if (want("ops")) testOps();
    if (want("slice")) testSlicing();
    if (want("lib")) testLibrary();
    if (want("lib")) testLibraryTruncation();
    if (want("persist")) testPersist();
    std::remove((kDir + "/fuzz_case.wav").c_str());
    if (g_fails) std::printf("sampleio: %d of %d checks FAILED\n", g_fails, g_checks);
    else std::printf("sampleio: all %d checks passed\n", g_checks);
    return g_fails ? 1 : 0;
}
