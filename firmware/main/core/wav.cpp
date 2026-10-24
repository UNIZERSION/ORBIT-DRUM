// WAV reader / writer (RIFF/RF64 WAVE). Loader/UI task only.
//
// Robustness rules (the input is an untrusted file on a removable card):
//  * every chunk size is checked against the real file size in 64-bit arithmetic;
//  * RIFF size is ignored (chunks are bounded by the file), data size 0 / 0xFFFFFFFF or larger
//    than the file means "until EOF";
//  * odd chunks are padded as the spec says, with a fallback for writers that forget the pad;
//  * nothing is read outside [0, size) and no output buffer is written outside the frames that
//    were allocated, whatever the header claims.
#include "sampleio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

namespace orbit {

namespace {

constexpr uint32_t kBlockBytes = 64 * 1024;   // SD read size (multiple of 512)
constexpr uint32_t kAlign = 128;              // DMA/cache friendly buffer alignment
constexpr uint32_t kMaxChannels = 64;
constexpr uint32_t kMaxRate = 1000000;
constexpr int kMaxCueRead = 256;              // cue points examined (more are ignored)

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
inline void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline bool isId(const uint8_t* p, const char* id) { return memcmp(p, id, 4) == 0; }

bool seek64(FILE* f, uint64_t off)
{
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET) == 0;
#else
    if (off > 0x7FFFFFFFull) return false;
    return fseek(f, (long)off, SEEK_SET) == 0;
#endif
}

bool fileSize64(FILE* f, uint64_t& size)
{
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) return false;
    const long long n = _ftelli64(f);
#else
    if (fseek(f, 0, SEEK_END) != 0) return false;
    const long n = ftell(f);
#endif
    if (n < 0) return false;
    size = (uint64_t)n;
    return true;
}

// Bounded random-access byte source over a FILE* or a memory block.
struct Src {
    FILE* f = nullptr;
    const uint8_t* mem = nullptr;
    uint64_t size = 0;
    uint64_t pos = ~0ull;   // current FILE position (avoids redundant seeks)

    bool read(uint64_t off, void* dst, uint32_t n)
    {
        if (off > size || (uint64_t)n > size - off) return false;
        if (n == 0) return true;
        if (mem) {
            memcpy(dst, mem + off, n);
            return true;
        }
        if (pos != off && !seek64(f, off)) {
            pos = ~0ull;
            return false;
        }
        const size_t got = fread(dst, 1, n, f);
        if (got != n) {
            pos = ~0ull;
            return false;
        }
        pos = off + n;
        return true;
    }
    // Reads up to n bytes (short read allowed at EOF). Returns bytes read.
    uint32_t readSome(uint64_t off, void* dst, uint32_t n)
    {
        if (off >= size) return 0;
        if ((uint64_t)n > size - off) n = (uint32_t)(size - off);
        return read(off, dst, n) ? n : 0;
    }
};

enum class Kind : uint8_t { U8, S16, S24, S32, F32, F64 };

struct Parsed {
    WavInfo info;
    Kind kind = Kind::S16;
    uint32_t src_channels = 0;
    uint32_t container = 0;      // bytes per sample
    uint32_t block = 0;          // bytes per frame
    uint64_t frames = 0;         // whole frames in the data chunk (may exceed 32 bits)
    uint64_t data_off = 0, data_bytes = 0;
};

const uint8_t kGuidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

bool printableId(Src& src, uint64_t off)
{
    uint8_t id[4];
    if (!src.read(off, id, 4)) return false;
    for (uint8_t c : id)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

void sortUnique(uint32_t* v, int& n)
{
    for (int i = 1; i < n; ++i) {   // insertion sort (n <= kMaxCueRead)
        const uint32_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = x;
    }
    int m = 0;
    for (int i = 0; i < n; ++i)
        if (m == 0 || v[i] != v[m - 1]) v[m++] = v[i];
    n = m;
}

WavError parse(Src& src, Parsed& P)
{
    uint8_t hdr[12];
    if (src.size < 12 || !src.read(0, hdr, 12)) return WavError::NotRiff;
    const bool rf64 = isId(hdr, "RF64") || isId(hdr, "BW64");
    if (!rf64 && !isId(hdr, "RIFF")) return WavError::NotRiff;
    if (!isId(hdr + 8, "WAVE")) return WavError::NotWave;

    const uint64_t end = src.size;   // RIFF size is only a hint; the file size is the truth
    bool have_fmt = false, have_data = false, fmt_bad = false;
    uint16_t tag = 0, channels = 0, align = 0, bits = 0, valid_bits = 0;
    uint32_t rate = 0;
    uint64_t ds64_data = 0;
    bool have_ds64 = false;
    uint32_t cues[kMaxCueRead];
    int ncue = 0;

    uint64_t off = 12;
    for (int guard = 0; guard < 10000 && off + 8 <= end; ++guard) {
        uint8_t ch[8];
        if (!src.read(off, ch, 8)) return WavError::Io;
        const uint32_t sz = rd32(ch + 4);
        const uint64_t body = off + 8;
        const uint64_t avail = end - body;
        const uint32_t have = (uint64_t)sz <= avail ? sz : (uint32_t)avail;   // bytes really present
        uint64_t len = sz;   // chunk body length used to find the next chunk

        if (isId(ch, "fmt ") && !have_fmt) {
            uint8_t f[40] = {};
            const uint32_t n = have < 40 ? have : 40;
            if (n < 16 || !src.read(body, f, n)) {
                fmt_bad = true;
            } else {
                tag = rd16(f);
                channels = rd16(f + 2);
                rate = rd32(f + 4);
                align = rd16(f + 12);
                bits = rd16(f + 14);
                valid_bits = 0;
                if (tag == 0xFFFE) {   // WAVE_FORMAT_EXTENSIBLE: real format in the subformat GUID
                    if (n >= 40 && rd16(f + 16) >= 22 && memcmp(f + 26, kGuidTail, 14) == 0) {
                        valid_bits = rd16(f + 18);
                        tag = rd16(f + 24);
                    } else {
                        tag = 0;   // unknown subformat
                    }
                }
                have_fmt = true;
            }
        } else if (isId(ch, "data") && !have_data) {
            P.data_off = body;
            bool open_end = sz == 0 || sz == 0xFFFFFFFFu;
            if (rf64 && have_ds64 && sz == 0xFFFFFFFFu) {
                len = ds64_data;   // 64-bit data size from ds64
                open_end = false;
            }
            const bool to_eof = open_end || len > avail;
            P.data_bytes = to_eof ? avail : len;
            have_data = true;
            if (to_eof) break;   // data runs to EOF: nothing reliable after it
        } else if (isId(ch, "ds64") && have >= 24) {
            uint8_t d[24];
            if (src.read(body, d, 24)) {
                ds64_data = rd64(d + 8);
                have_ds64 = true;
            }
        } else if (isId(ch, "smpl") && have >= 60) {
            uint8_t s[60];
            if (src.read(body, s, 60) && rd32(s + 28) >= 1) {
                const uint32_t ls = rd32(s + 36 + 8), le = rd32(s + 36 + 12);
                if (le >= ls && le != 0xFFFFFFFFu) {
                    P.info.has_loop = 1;
                    P.info.loop_start = ls;
                    P.info.loop_end = le + 1;   // smpl end is inclusive, ours is exclusive
                }
            }
        } else if (isId(ch, "cue ") && have >= 4 && ncue == 0) {
            uint8_t c4[4];
            if (src.read(body, c4, 4)) {
                uint32_t count = rd32(c4);
                const uint32_t fit = (have - 4) / 24;
                if (count > fit) count = fit;
                if (count > (uint32_t)kMaxCueRead) count = kMaxCueRead;
                for (uint32_t i = 0; i < count; ++i) {
                    uint8_t cp[24];
                    if (!src.read(body + 4 + (uint64_t)i * 24, cp, 24)) break;
                    uint32_t pos = rd32(cp + 20);   // dwSampleOffset
                    if (pos == 0) pos = rd32(cp + 4);   // some writers only fill dwPosition
                    cues[ncue++] = pos;
                }
            }
        }
        // LIST/adtl, bext, id3, junk, fact, acid...: skipped

        if (len > avail) break;
        uint64_t next = body + len;
        if (len & 1) {
            // Spec: odd chunks carry a pad byte. Some writers omit it: use the unpadded position
            // only if it looks like a chunk id and the padded one does not.
            const bool padded_ok = next + 1 + 8 <= end && printableId(src, next + 1);
            const bool unpadded_ok = next + 8 <= end && printableId(src, next);
            if (padded_ok || !unpadded_ok) next += 1;
        }
        off = next;
    }

    if (!have_fmt) return fmt_bad ? WavError::Unsupported : WavError::NoFmt;
    if (!have_data) return WavError::NoData;
    if (channels == 0 || channels > kMaxChannels || rate == 0 || rate > kMaxRate) return WavError::Unsupported;

    // Container size: blockAlign / channels when consistent, else from bits.
    const uint32_t from_bits = ((uint32_t)bits + 7) / 8;
    uint32_t c = (align && align % channels == 0) ? align / channels : 0;
    if (c == 0 || c < from_bits) c = from_bits;
    if (tag == 1) {
        if (c == 1) P.kind = Kind::U8;
        else if (c == 2) P.kind = Kind::S16;
        else if (c == 3) P.kind = Kind::S24;
        else if (c == 4) P.kind = Kind::S32;
        else return WavError::Unsupported;
    } else if (tag == 3) {
        if (c == 4) P.kind = Kind::F32;
        else if (c == 8) P.kind = Kind::F64;
        else return WavError::Unsupported;
    } else {
        return WavError::Unsupported;
    }

    P.src_channels = channels;
    P.container = c;
    P.block = c * channels;
    P.frames = P.data_bytes / P.block;
    if (P.frames == 0) return WavError::NoData;

    WavInfo& I = P.info;
    I.rate = rate;
    I.channels = channels;
    I.bits = (uint16_t)(c * 8);
    if (valid_bits && valid_bits < I.bits && tag == 1) I.bits = valid_bits;   // 24-in-32 shows 24
    I.is_float = tag == 3;
    I.frames = P.frames > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)P.frames;
    if (I.has_loop) {
        if (I.loop_start >= I.frames) I.has_loop = 0, I.loop_start = I.loop_end = 0;
        else if (I.loop_end > I.frames) I.loop_end = I.frames;
    }
    sortUnique(cues, ncue);
    I.cue_count = 0;
    for (int i = 0; i < ncue && I.cue_count < kMaxSlices; ++i)
        if (cues[i] < I.frames) I.cues[I.cue_count++] = cues[i];
    return WavError::Ok;
}

inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

inline int16_t f2s(float x)
{
    const float v = x * 32768.0f;
    if (!(v == v)) return 0;   // NaN
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)(v >= 0.0f ? (int32_t)(v + 0.5f) : -(int32_t)(-v + 0.5f));
}

inline int16_t conv(Kind k, const uint8_t* p)
{
    switch (k) {
    case Kind::U8: return (int16_t)(((int32_t)p[0] - 128) * 256);
    case Kind::S16: return (int16_t)rd16(p);
    case Kind::S24: {
        const int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8;
        return sat16((v + 128) >> 8);
    }
    case Kind::S32: {
        const int64_t v = (int32_t)rd32(p);
        const int64_t r = (v + 32768) >> 16;
        return (int16_t)(r > 32767 ? 32767 : r);
    }
    case Kind::F32: {
        const uint32_t u = rd32(p);
        float x;
        memcpy(&x, &u, 4);
        return f2s(x);
    }
    case Kind::F64: {
        const uint64_t u = rd64(p);
        double d;
        memcpy(&d, &u, 8);
        if (!(d == d)) return 0;
        if (d > 2.0) d = 2.0;   // keep the double->float conversion in range
        if (d < -2.0) d = -2.0;
        return f2s((float)d);
    }
    }
    return 0;
}

// Converts n whole frames from raw bytes into dst (out_ch interleaved channels).
void convertFrames(const Parsed& P, const uint8_t* src, uint32_t n, int16_t* dst, int out_ch)
{
    const uint32_t blk = P.block, c = P.container;
    const Kind k = P.kind;
    if (out_ch == 1) {
        for (uint32_t i = 0; i < n; ++i) dst[i] = conv(k, src + (size_t)i * blk);
    } else {
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* f = src + (size_t)i * blk;
            dst[2 * i] = conv(k, f);
            dst[2 * i + 1] = conv(k, f + c);
        }
    }
}

void setName(char* dst, int cap, const char* src)
{
    // UTF-8 safe truncation to cap-1 bytes.
    memset(dst, 0, (size_t)cap);
    if (!src) return;
    size_t n = strlen(src);
    if (n > (size_t)cap - 1) {
        n = (size_t)cap - 1;
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) --n;
    }
    memcpy(dst, src, n);
}

void stemOf(const char* path, char* out, int cap)
{
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') b = p + 1;
    char tmp[kPathLen];
    setName(tmp, sizeof(tmp), b);
    char* dot = strrchr(tmp, '.');
    if (dot && dot != tmp) *dot = 0;
    setName(out, cap, tmp);
}

Sample* load(Src& src, const char* name, const char* path, WavInfo* info, WavError* err, float max_seconds)
{
    WavError dummy;
    if (!err) err = &dummy;
    Parsed P;
    WavError e = parse(src, P);
    if (info) *info = P.info;
    if (e != WavError::Ok) {
        *err = e;
        return nullptr;
    }
    uint64_t frames = P.frames;
    if (max_seconds > 0.0f) {
        const float lim = max_seconds * (float)P.info.rate;
        const uint64_t cap = lim >= 4.0e9f ? 0xFFFFFFFFull : (lim < 1.0f ? 1ull : (uint64_t)lim);
        if (frames > cap) frames = cap;
    }
    const int out_ch = P.src_channels >= 2 ? 2 : 1;
    if ((frames + 2 * kGuard) * (uint64_t)out_ch * 2u > 0x7FFFFFFFull) {
        *err = WavError::TooLarge;
        return nullptr;
    }
    Sample* s = sampleCreate((uint32_t)frames, out_ch, P.info.rate);
    if (!s) {
        *err = WavError::NoMemory;
        return nullptr;
    }

    if (src.mem) {
        convertFrames(P, src.mem + P.data_off, (uint32_t)frames, s->data, out_ch);
    } else {
        // Reads start at 512-byte aligned file offsets into an aligned buffer, so FATFS can DMA
        // whole sectors straight into it. A frame split between two blocks goes through carry[].
        void* raw = bigAlloc(kBlockBytes + kAlign);
        if (!raw) {
            sampleFree(s);
            *err = WavError::NoMemory;
            return nullptr;
        }
        uint8_t* buf = (uint8_t*)(((uintptr_t)raw + kAlign - 1) & ~(uintptr_t)(kAlign - 1));
        uint8_t carry[kMaxChannels * 8];
        uint32_t carry_len = 0;
        const uint32_t blk = P.block;
        uint64_t fpos = P.data_off & ~511ull;
        uint32_t skip = (uint32_t)(P.data_off - fpos);
        uint64_t left = frames;   // frames still to convert
        int16_t* out = s->data;
        bool ok = true;
        while (left > 0) {
            const uint32_t n = src.readSome(fpos, buf, kBlockBytes);
            if (n <= skip) {
                ok = false;
                break;
            }
            const uint8_t* p = buf + skip;
            uint32_t avail = n - skip;
            skip = 0;
            fpos += n;
            if (carry_len) {
                uint32_t take = blk - carry_len;
                if (take > avail) take = avail;
                memcpy(carry + carry_len, p, take);
                carry_len += take;
                p += take;
                avail -= take;
                if (carry_len == blk) {
                    convertFrames(P, carry, 1, out, out_ch);
                    out += out_ch;
                    --left;
                    carry_len = 0;
                }
            }
            uint64_t whole = avail / blk;
            if (whole > left) whole = left;
            convertFrames(P, p, (uint32_t)whole, out, out_ch);
            out += whole * out_ch;
            left -= whole;
            p += whole * blk;
            avail -= (uint32_t)(whole * blk);
            if (left > 0 && avail > 0) {   // partial frame at the end of the block (avail < blk)
                memcpy(carry, p, avail);
                carry_len = avail;
            }
        }
        bigFree(raw);
        if (!ok) {
            sampleFree(s);
            *err = WavError::Io;
            return nullptr;
        }
    }

    if (name) setName(s->name, kNameLen, name);
    if (path && strlen(path) < (size_t)kPathLen) setName(s->path, kPathLen, path);
    sampleAnalyze(s);
    *err = WavError::Ok;
    return s;
}

FILE* openRead(const char* path, Src& src)
{
    if (!path || !path[0]) return nullptr;
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    setvbuf(f, nullptr, _IONBF, 0);   // large freads go straight to the FS (no 128 B stdio copies)
    uint64_t size = 0;
    if (!fileSize64(f, size)) {
        fclose(f);
        return nullptr;
    }
    src.f = f;
    src.size = size;
    return f;
}

// Write <path>.tmp, then replace path (FAT and Windows refuse rename over an existing file).
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

}  // namespace

const char* wavErrorText(WavError e)
{
    switch (e) {
    case WavError::Ok: return "OK";
    case WavError::Open: return "Nie można otworzyć pliku";
    case WavError::NotRiff: return "To nie jest plik WAV";
    case WavError::NotWave: return "Plik RIFF bez danych WAVE";
    case WavError::NoFmt: return "Brak nagłówka formatu";
    case WavError::NoData: return "Brak danych audio";
    case WavError::Unsupported: return "Nieobsługiwany format";
    case WavError::TooLarge: return "Plik jest za duży";
    case WavError::NoMemory: return "Brak pamięci";
    case WavError::Io: return "Błąd odczytu/zapisu karty";
    }
    return "Nieznany błąd";
}

WavError wavProbe(const char* path, WavInfo& info)
{
    info = WavInfo{};
    Src src;
    FILE* f = openRead(path, src);
    if (!f) return WavError::Open;
    Parsed P;
    const WavError e = parse(src, P);
    fclose(f);
    if (e == WavError::Ok) info = P.info;
    return e;
}

Sample* wavLoad(const char* path, WavInfo* info, WavError* err, float max_seconds)
{
    if (info) *info = WavInfo{};
    Src src;
    FILE* f = openRead(path, src);
    if (!f) {
        if (err) *err = WavError::Open;
        return nullptr;
    }
    char stem[kNameLen];
    stemOf(path, stem, sizeof(stem));
    Sample* s = load(src, stem, path, info, err, max_seconds);
    fclose(f);
    return s;
}

Sample* wavLoadMem(const uint8_t* data, uint32_t size, const char* name, WavInfo* info, WavError* err)
{
    if (info) *info = WavInfo{};
    if (!data) {
        if (err) *err = WavError::Open;
        return nullptr;
    }
    Src src;
    src.mem = data;
    src.size = size;
    return load(src, name, nullptr, info, err, 0.0f);
}

WavError wavSave(const char* path, const Sample* s, const float* slices, int slice_count)
{
    if (!path || !path[0]) return WavError::Open;
    if (!s || !s->data || !s->frames || s->channels < 1 || s->channels > 2) return WavError::NoData;
    if (!slices || slice_count < 0) slice_count = 0;
    if (slice_count > kMaxSlices) slice_count = kMaxSlices;
    const uint32_t ch = s->channels;
    const uint64_t data_bytes = (uint64_t)s->frames * ch * 2u;
    const uint32_t cue_bytes = slice_count ? 4u + 24u * (uint32_t)slice_count : 0u;
    const uint64_t riff = 4 + (8 + 16) + (8 + data_bytes) + (slice_count ? 8 + cue_bytes : 0);
    if (riff > 0xFFFFFFF0ull) return WavError::TooLarge;

    char tmp[kPathLen + 8];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return WavError::Open;
    FILE* f = fopen(tmp, "wb");
    if (!f) return WavError::Open;
    setvbuf(f, nullptr, _IONBF, 0);

    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    wr32(h + 4, (uint32_t)riff);
    memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, 16);
    wr16(h + 20, 1);
    wr16(h + 22, ch);
    wr32(h + 24, s->rate);
    wr32(h + 28, s->rate * ch * 2u);
    wr16(h + 32, ch * 2u);
    wr16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    wr32(h + 40, (uint32_t)data_bytes);
    bool ok = fwrite(h, 1, 44, f) == 44;

    const uint32_t chunk_samples = 16 * 1024;
    uint8_t* buf = (uint8_t*)bigAlloc(chunk_samples * 2);
    if (!buf) ok = false;
    const uint64_t total = (uint64_t)s->frames * ch;
    for (uint64_t i = 0; ok && i < total; i += chunk_samples) {
        const uint32_t n = (uint32_t)(total - i < chunk_samples ? total - i : chunk_samples);
        for (uint32_t k = 0; k < n; ++k) wr16(buf + 2 * k, (uint16_t)s->data[i + k]);
        ok = fwrite(buf, 1, (size_t)n * 2, f) == (size_t)n * 2;
    }
    bigFree(buf);

    if (ok && slice_count) {
        uint8_t c[12];
        memcpy(c, "cue ", 4);
        wr32(c + 4, cue_bytes);
        wr32(c + 8, (uint32_t)slice_count);
        ok = fwrite(c, 1, 12, f) == 12;
        for (int i = 0; ok && i < slice_count; ++i) {
            float x = slices[i];
            if (!(x >= 0.0f)) x = 0.0f;
            if (x > 1.0f) x = 1.0f;
            uint32_t pos = (uint32_t)(x * (float)s->frames + 0.5f);
            if (pos >= s->frames) pos = s->frames - 1;
            uint8_t p[24];
            wr32(p, (uint32_t)i + 1);   // id
            wr32(p + 4, pos);           // play order position
            memcpy(p + 8, "data", 4);
            wr32(p + 12, 0);
            wr32(p + 16, 0);
            wr32(p + 20, pos);          // sample offset
            ok = fwrite(p, 1, 24, f) == 24;
        }
    }
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(tmp);
        return WavError::Io;
    }
    return replaceFile(tmp, path) ? WavError::Ok : WavError::Io;
}

}  // namespace orbit
