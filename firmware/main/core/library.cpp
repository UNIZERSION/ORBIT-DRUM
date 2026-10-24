// SD card sample library: folder layout, scan, filter, kits and project names.
// Loader/UI task only (stdio + dirent). Works on /sdcard (FATFS VFS) and on PC folders.
#include "library.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "platform.h"
#include "sampleio.h"

// Filesystem size query, provided by the HAL on the Tab5 (FATFS f_getfree).
extern "C" __attribute__((weak)) bool orbit_fs_info(const char* path, uint64_t* total, uint64_t* free_bytes)
{
    (void)path;
    (void)total;
    (void)free_bytes;
    return false;
}

namespace orbit {

namespace {

constexpr int kMaxDepth = 4;
constexpr int kMaxKitPads = 16;
const char* const kCatFolders[] = {"Drums", "Bass", "Synth", "Vocals", "FX"};
const Category kCatValues[] = {Category::Drums, Category::Bass, Category::Synth, Category::Vocals, Category::Fx};

int makeDir(const char* path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0775);
#endif
}

bool isDir(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Creates every missing folder of path (like mkdir -p).
bool makeDirs(const char* path)
{
    char tmp[kPathLen];
    const size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; ++i) {
        if (tmp[i] != '/' && tmp[i] != '\\') continue;
        if (tmp[i - 1] == ':' || tmp[i - 1] == '/' || tmp[i - 1] == '\\') continue;   // "C:/", "//"
        const char c = tmp[i];
        tmp[i] = 0;
        if (!isDir(tmp)) makeDir(tmp);
        tmp[i] = c;
    }
    if (!isDir(tmp)) makeDir(tmp);
    return isDir(tmp);
}

bool join(char* out, int len, const char* a, const char* b)
{
    const int n = snprintf(out, (size_t)len, "%s/%s", a, b);
    return n > 0 && n < len;
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool eqCi(const char* a, const char* b)
{
    while (*a && *b) {
        if (lowerAscii(*a) != lowerAscii(*b)) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

bool endsWithCi(const char* s, const char* suffix)
{
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && eqCi(s + n - m, suffix);
}

bool isWavName(const char* name)
{
    // Hidden files and macOS "._x.wav" resource forks are skipped.
    return name[0] != '.' && endsWithCi(name, ".wav") && strlen(name) > 4;
}

// Natural, case-insensitive order: "Kick 2" < "Kick 10".
int naturalCmp(const char* a, const char* b)
{
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') ++a;
            while (*b == '0') ++b;
            int na = 0, nb = 0;
            while (a[na] >= '0' && a[na] <= '9') ++na;
            while (b[nb] >= '0' && b[nb] <= '9') ++nb;
            if (na != nb) return na < nb ? -1 : 1;
            const int c = strncmp(a, b, (size_t)na);
            if (c) return c;
            a += na;
            b += nb;
            continue;
        }
        const char ca = lowerAscii(*a), cb = lowerAscii(*b);
        if (ca != cb) return (unsigned char)ca < (unsigned char)cb ? -1 : 1;
        ++a;
        ++b;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

// UTF-8 safe copy with truncation ("..." marks a shortened name).
void copyName(char* dst, int cap, const char* src, int src_len, bool mark)
{
    if (cap <= 0) return;
    int n = src_len;
    bool cut = false;
    if (n > cap - 1) {
        n = cap - 1 - (mark ? 2 : 0);
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) --n;
        cut = mark;
    }
    memcpy(dst, src, (size_t)n);
    if (cut) {
        dst[n++] = '.';
        dst[n++] = '.';
    }
    dst[n] = 0;
}

void stemName(const char* file, char* out, int cap)
{
    const char* dot = strrchr(file, '.');
    const int n = dot && dot != file ? (int)(dot - file) : (int)strlen(file);
    copyName(out, cap, file, n, true);
}

// Folds a UTF-8 string for searching: ASCII lowercase, Polish and Latin-1 letters -> base
// letter. Other multi-byte sequences are copied unchanged. Returns the folded length.
int fold(const char* s, char* out, int cap)
{
    // Latin-1 supplement U+00C0..U+00FF -> base letter (0 = keep).
    static const char kLatin1[64] = {
        'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
        'd', 'n', 'o', 'o', 'o', 'o', 'o', 0,   'o', 'u', 'u', 'u', 'u', 'y', 0,   's',
        'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
        'd', 'n', 'o', 'o', 'o', 'o', 'o', 0,   'o', 'u', 'u', 'u', 'u', 'y', 0,   'y'};
    int n = 0;
    const uint8_t* p = (const uint8_t*)s;
    while (*p && n < cap - 1) {
        const uint8_t c = *p;
        if (c < 0x80) {
            out[n++] = lowerAscii((char)c);
            ++p;
            continue;
        }
        if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            const uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
            char base = 0;
            if (cp >= 0xC0 && cp <= 0xFF) {
                base = kLatin1[cp - 0xC0];
            } else {
                switch (cp) {
                case 0x104: case 0x105: base = 'a'; break;   // Ą ą
                case 0x106: case 0x107: base = 'c'; break;   // Ć ć
                case 0x118: case 0x119: base = 'e'; break;   // Ę ę
                case 0x141: case 0x142: base = 'l'; break;   // Ł ł
                case 0x143: case 0x144: base = 'n'; break;   // Ń ń
                case 0x15A: case 0x15B: base = 's'; break;   // Ś ś
                case 0x179: case 0x17A:                      // Ź ź
                case 0x17B: case 0x17C: base = 'z'; break;   // Ż ż
                default: break;
                }
            }
            if (base) {
                out[n++] = base;
            } else if (n + 2 < cap) {
                out[n++] = (char)p[0];
                out[n++] = (char)p[1];
            }
            p += 2;
            continue;
        }
        out[n++] = (char)c;   // other bytes (3/4-byte sequences, stray bytes) as they are
        ++p;
    }
    out[n] = 0;
    return n;
}

int parsePad(const char* key)
{
    // "1".."16", "01", or pad names "A1".."D4" (row A..D, column 1..4).
    while (*key == ' ' || *key == '\t') ++key;
    if ((key[0] >= 'A' && key[0] <= 'D') || (key[0] >= 'a' && key[0] <= 'd')) {
        const int row = lowerAscii(key[0]) - 'a';
        if (key[1] >= '1' && key[1] <= '4') return row * 4 + (key[1] - '1');
        return -1;
    }
    int v = 0, digits = 0;
    while (*key >= '0' && *key <= '9' && digits < 4) {
        v = v * 10 + (*key - '0');
        ++key;
        ++digits;
    }
    if (!digits || v < 1 || v > kMaxKitPads) return -1;
    return v - 1;
}

void trim(char* s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') ++i;
    if (i) memmove(s, s + i, n - i + 1);
}

LibEntry g_empty;

}  // namespace

struct Library::Impl {
    char root[kPathLen];
    LibEntry* e;        // kMaxLibraryFiles entries (PSRAM)
    int n;
    int truncated;

    LibEntry* add()
    {
        if (n >= kMaxLibraryFiles) {
            truncated = 1;
            return nullptr;
        }
        LibEntry* x = &e[n++];
        *x = LibEntry{};
        return x;
    }

    void walk(const char* dir, int depth, Category cat, bool top_samples)
    {
        DIR* d = opendir(dir);
        if (!d) return;
        struct dirent* de;
        while ((de = readdir(d)) != nullptr) {
            const char* nm = de->d_name;
            if (nm[0] == '.') continue;
            char path[kPathLen];
            if (!join(path, kPathLen, dir, nm)) continue;   // path too long: skipped
#if defined(DT_DIR) && defined(DT_REG) && !defined(_WIN32)
            bool sub = de->d_type == DT_DIR;
            if (de->d_type == DT_UNKNOWN) sub = isDir(path);
#else
            const bool sub = isDir(path);
#endif
            if (sub) {
                if (depth >= kMaxDepth) continue;
                Category c = cat;
                if (top_samples) {   // first folder below SAMPLES names the category
                    c = Category::All;
                    for (int i = 0; i < 5; ++i)
                        if (eqCi(nm, kCatFolders[i])) c = kCatValues[i];
                }
                walk(path, depth + 1, c, false);
            } else if (isWavName(nm)) {
                LibEntry* x = add();
                if (!x) break;
                memcpy(x->path, path, strlen(path) + 1);
                stemName(nm, x->name, (int)sizeof(x->name));
                x->cat = cat;
            }
            if (truncated) break;
        }
        closedir(d);
    }

    void scanKits()
    {
        char kits[kPathLen];
        if (!join(kits, kPathLen, root, "KITS")) return;
        DIR* d = opendir(kits);
        if (!d) return;
        struct dirent* de;
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            char path[kPathLen];
            if (!join(path, kPathLen, kits, de->d_name) || !isDir(path)) continue;
            if (!kitHasFiles(path)) continue;
            LibEntry* x = add();
            if (!x) break;
            memcpy(x->path, path, strlen(path) + 1);
            copyName(x->name, (int)sizeof(x->name), de->d_name, (int)strlen(de->d_name), true);
            x->cat = Category::Kits;
            x->is_kit = 1;
            x->probed = 1;   // nothing to read from a folder
        }
        closedir(d);
    }

    static bool kitHasFiles(const char* dir)
    {
        DIR* d = opendir(dir);
        if (!d) return false;
        bool any = false;
        struct dirent* de;
        while (!any && (de = readdir(d)) != nullptr)
            any = isWavName(de->d_name) || eqCi(de->d_name, "kit.txt");
        closedir(d);
        return any;
    }
};

const char* categoryName(Category c)
{
    switch (c) {
    case Category::Kits: return "Kits";
    case Category::All: return "All Samples";
    case Category::Drums: return "Drums";
    case Category::Bass: return "Bass";
    case Category::Synth: return "Synth";
    case Category::Vocals: return "Vocals";
    case Category::Fx: return "FX";
    case Category::User: return "User";
    default: return "?";
    }
}

bool Library::open(const char* root)
{
    if (!root || !root[0]) return false;
    if (!impl_) {
        impl_ = static_cast<Impl*>(bigAlloc(sizeof(Impl)));
        if (!impl_) return false;
        impl_->e = static_cast<LibEntry*>(bigAlloc(sizeof(LibEntry) * kMaxLibraryFiles));
        if (!impl_->e) {
            bigFree(impl_);
            impl_ = nullptr;
            return false;
        }
    }
    Impl& m = *impl_;
    m.n = 0;
    m.truncated = 0;
    size_t len = strlen(root);
    while (len > 1 && (root[len - 1] == '/' || root[len - 1] == '\\')) --len;
    if (len >= sizeof(m.root) - 24) return false;   // room for "/PROJECTS/<name>.orb"
    memcpy(m.root, root, len);
    m.root[len] = 0;
    if (!makeDirs(m.root)) return false;
    static const char* const kSub[] = {"KITS", "SAMPLES", "SAMPLES/Drums", "SAMPLES/Bass", "SAMPLES/Synth",
                                       "SAMPLES/Vocals", "SAMPLES/FX", "USER", "PROJECTS"};
    char p[kPathLen];
    for (const char* sub : kSub)
        if (join(p, kPathLen, m.root, sub) && !isDir(p)) makeDir(p);
    return true;
}

const char* Library::root() const { return impl_ ? impl_->root : ""; }

int Library::scan()
{
    if (!impl_) return 0;
    Impl& m = *impl_;
    m.n = 0;
    m.truncated = 0;
    m.scanKits();
    const int kits_end = m.n;
    char p[kPathLen];
    // USER first: when a big SAMPLES pack hits kMaxLibraryFiles, the user's own files (SAVE WAV,
    // recordings) must not be the ones dropped. Display order is set by the sort below.
    if (join(p, kPathLen, m.root, "USER")) m.walk(p, 0, Category::User, false);
    if (join(p, kPathLen, m.root, "SAMPLES")) m.walk(p, 0, Category::All, true);
    // Kits first (by name), then files by path; both in natural order.
    qsort(m.e, (size_t)kits_end, sizeof(LibEntry), [](const void* x, const void* y) {
        return naturalCmp(static_cast<const LibEntry*>(x)->name, static_cast<const LibEntry*>(y)->name);
    });
    qsort(m.e + kits_end, (size_t)(m.n - kits_end), sizeof(LibEntry), [](const void* x, const void* y) {
        const LibEntry* a = static_cast<const LibEntry*>(x);
        const LibEntry* b = static_cast<const LibEntry*>(y);
        if (a->cat != b->cat && (a->cat == Category::User || b->cat == Category::User))
            return a->cat == Category::User ? 1 : -1;   // USER after SAMPLES
        return naturalCmp(a->path, b->path);
    });
    if (m.truncated) logf("library: more than %d files, list truncated", kMaxLibraryFiles);
    return m.n;
}

int Library::count() const { return impl_ ? impl_->n : 0; }

bool Library::truncated() const { return impl_ && impl_->truncated; }

const LibEntry& Library::at(int i) const
{
    if (!impl_ || i < 0 || i >= impl_->n) return g_empty;
    return impl_->e[i];
}

void Library::probe(int i)
{
    if (!impl_ || i < 0 || i >= impl_->n) return;
    LibEntry& x = impl_->e[i];
    if (x.probed || x.is_kit) {
        x.probed = 1;
        return;
    }
    WavInfo info;
    if (wavProbe(x.path, info) == WavError::Ok) {
        x.rate = info.rate;
        x.channels = info.channels;
        x.bits = info.bits;
        x.seconds = info.rate ? (float)info.frames / (float)info.rate : 0.0f;
    }
    struct stat st;
    if (stat(x.path, &st) == 0) {
        const uint64_t sz = st.st_size > 0 ? (uint64_t)st.st_size : 0;   // off_t is 32 or 64 bit
        x.bytes = sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sz;
    }
    x.probed = 1;
}

int Library::filter(Category c, const char* query, int* out, int max) const
{
    if (!impl_ || !out || max <= 0) return 0;
    char q[96];
    const int qn = fold(query ? query : "", q, (int)sizeof(q));
    // Trim spaces around the query.
    int qs = 0, qe = qn;
    while (qs < qe && q[qs] == ' ') ++qs;
    while (qe > qs && q[qe - 1] == ' ') --qe;
    q[qe] = 0;
    const char* needle = q + qs;
    int found = 0;
    char name[96];
    for (int i = 0; i < impl_->n && found < max; ++i) {
        const LibEntry& x = impl_->e[i];
        if (c == Category::Kits) {
            if (!x.is_kit) continue;
        } else if (x.is_kit) {
            continue;
        } else if (c != Category::All && x.cat != c) {
            continue;
        }
        if (*needle) {
            fold(x.name, name, (int)sizeof(name));
            if (!strstr(name, needle)) continue;
        }
        out[found++] = i;
    }
    return found;
}

int Library::kitFiles(int entry, char paths[][kPathLen], int max) const
{
    if (!impl_ || !paths || max <= 0 || entry < 0 || entry >= impl_->n) return 0;
    const LibEntry& kit = impl_->e[entry];
    if (!kit.is_kit) return 0;
    if (max > kMaxKitPads) max = kMaxKitPads;
    for (int i = 0; i < max; ++i) paths[i][0] = 0;

    char txt[kPathLen];
    FILE* f = join(txt, kPathLen, kit.path, "kit.txt") ? fopen(txt, "rb") : nullptr;
    if (f) {
        int count = 0;
        char line[256];
        bool first = true;
        while (fgets(line, sizeof(line), f)) {
            char* l = line;
            if (first && (uint8_t)l[0] == 0xEF && (uint8_t)l[1] == 0xBB && (uint8_t)l[2] == 0xBF) l += 3;   // BOM
            first = false;
            trim(l);
            if (!l[0] || l[0] == '#' || l[0] == ';') continue;
            char* eq = strchr(l, '=');
            if (!eq) continue;
            *eq = 0;
            char* file = eq + 1;
            trim(file);
            const int pad = parsePad(l);
            if (pad < 0 || pad >= max || !file[0] || strstr(file, "..")) continue;
            if (!join(paths[pad], kPathLen, kit.path, file)) paths[pad][0] = 0;   // too long
            else if (pad + 1 > count) count = pad + 1;
        }
        fclose(f);
        if (count > 0) return count;   // an empty / invalid kit.txt falls back to the WAV list
    }

    // No mapping: WAVs in natural name order -> pads 1..16.
    // Called from the UI task: the name table lives on the heap, not on its stack.
    char (*names)[kPathLen] = static_cast<char (*)[kPathLen]>(bigAlloc(sizeof(char) * kPathLen * (kMaxKitPads + 1)));
    if (!names) return 0;
    int n = 0;
    DIR* d = opendir(kit.path);
    if (!d) {
        bigFree(names);
        return 0;
    }
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        const char* nm = de->d_name;
        if (!isWavName(nm) || strlen(nm) >= (size_t)kPathLen) continue;
        // Keep the max smallest names (insertion into a sorted list).
        int pos = n;
        while (pos > 0 && naturalCmp(nm, names[pos - 1]) < 0) --pos;
        if (pos >= max) continue;
        const int last = n < max ? n : max - 1;
        for (int k = last; k > pos; --k) memcpy(names[k], names[k - 1], kPathLen);
        memcpy(names[pos], nm, strlen(nm) + 1);
        if (n < max) ++n;
    }
    closedir(d);
    int count = 0;
    for (int i = 0; i < n; ++i)
        if (join(paths[count], kPathLen, kit.path, names[i])) ++count;
    bigFree(names);
    return count;
}

int Library::listProjects(char names[][32], int max) const
{
    if (!impl_ || !names || max <= 0) return 0;
    char dir[kPathLen];
    if (!join(dir, kPathLen, impl_->root, "PROJECTS")) return 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct Item {
        char name[32];
        int64_t mtime;
    };
    constexpr int kMaxItems = 256;
    Item* items = static_cast<Item*>(bigAlloc(sizeof(Item) * kMaxItems));
    if (!items) {
        closedir(d);
        return 0;
    }
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr && n < kMaxItems) {
        const char* nm = de->d_name;
        if (nm[0] == '.' || !endsWithCi(nm, ".orb")) continue;
        const size_t stem = strlen(nm) - 4;
        if (stem == 0 || stem >= sizeof(items[0].name)) continue;   // would not fit / not reopenable
        char path[kPathLen];
        if (!join(path, kPathLen, dir, nm)) continue;
        struct stat st;
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        memcpy(items[n].name, nm, stem);
        items[n].name[stem] = 0;
        items[n].mtime = (int64_t)st.st_mtime;
        ++n;
    }
    closedir(d);
    // Newest first; equal times by name.
    qsort(items, (size_t)n, sizeof(Item), [](const void* x, const void* y) {
        const Item* a = static_cast<const Item*>(x);
        const Item* b = static_cast<const Item*>(y);
        if (a->mtime != b->mtime) return a->mtime > b->mtime ? -1 : 1;
        return naturalCmp(a->name, b->name);
    });
    const int count = n < max ? n : max;
    for (int i = 0; i < count; ++i) memcpy(names[i], items[i].name, sizeof(items[i].name));
    bigFree(items);
    return count;
}

namespace {
bool validName(const char* name)
{
    if (!name || !name[0] || strlen(name) >= 32) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const char* p = name; *p; ++p)
        if (strchr("/\\:*?\"<>|", *p) || (uint8_t)*p < 0x20) return false;
    return true;
}
}  // namespace

bool Library::projectPath(const char* name, char* out, int len) const
{
    if (!impl_ || !out || len <= 0 || !validName(name)) return false;
    const int n = snprintf(out, (size_t)len, "%s/PROJECTS/%s.orb", impl_->root, name);
    return n > 0 && n < len;
}

bool Library::projectDir(const char* name, char* out, int len) const
{
    if (!impl_ || !out || len <= 0 || !validName(name)) return false;
    const int n = snprintf(out, (size_t)len, "%s/PROJECTS/%s/", impl_->root, name);
    return n > 0 && n < len;
}

bool Library::userPath(const char* file, char* out, int len) const
{
    if (!impl_ || !out || len <= 0 || !file || !file[0] || strstr(file, "..")) return false;
    const int n = snprintf(out, (size_t)len, "%s/USER/%s", impl_->root, file);
    return n > 0 && n < len;
}

uint64_t Library::freeBytes() const
{
    uint64_t total = 0, free_b = 0;
    if (!impl_ || !orbit_fs_info(impl_->root, &total, &free_b)) return 0;
    return free_b;
}

uint64_t Library::totalBytes() const
{
    uint64_t total = 0, free_b = 0;
    if (!impl_ || !orbit_fs_info(impl_->root, &total, &free_b)) return 0;
    return total;
}

}  // namespace orbit
