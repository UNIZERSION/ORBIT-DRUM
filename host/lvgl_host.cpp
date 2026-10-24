// Headless LVGL 9.3 harness for the PC (see lvgl_host.h): virtual clock, one full-frame RGB565
// buffer in DIRECT mode (always holds the whole frame), a pointer indev driven by the API and a
// small PNG writer (filtered rows + fixed-Huffman deflate with LZ77, no external libraries).
#include "lvgl_host.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

uint32_t g_ms = 0;
lv_display_t* g_disp = nullptr;
lv_indev_t* g_indev = nullptr;
uint8_t* g_fb = nullptr;
int g_w = 0, g_h = 0;
int g_px = 0, g_py = 0;
bool g_pressed = false;

uint32_t tick_cb() { return g_ms; }

void flush_cb(lv_display_t* disp, const lv_area_t*, uint8_t*)
{
    // DIRECT mode: LVGL already rendered into g_fb at screen positions.
    lv_display_flush_ready(disp);
}

void read_cb(lv_indev_t*, lv_indev_data_t* data)
{
    data->point.x = g_px;
    data->point.y = g_py;
    data->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ------------------------------------------------------------------ PNG writer
uint32_t crc_table[256];
void crc_init()
{
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n)
{
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int nbits = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t v, int n)   // LSB first
    {
        acc |= v << nbits;
        nbits += n;
        while (nbits >= 8) { out.push_back((uint8_t)acc); acc >>= 8; nbits -= 8; }
    }
    void putRev(uint32_t code, int n)   // Huffman codes are MSB first
    {
        uint32_t r = 0;
        for (int i = 0; i < n; ++i) r |= ((code >> i) & 1u) << (n - 1 - i);
        put(r, n);
    }
    void flush() { if (nbits) { out.push_back((uint8_t)acc); acc = 0; nbits = 0; } }
};

void putLit(BitWriter& bw, int s)
{
    if (s < 144) bw.putRev(0x30 + s, 8);
    else if (s < 256) bw.putRev(0x190 + (s - 144), 9);
    else if (s < 280) bw.putRev(s - 256, 7);
    else bw.putRev(0xC0 + (s - 280), 8);
}

const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void putMatch(BitWriter& bw, int len, int dist)
{
    int li = 28;
    while (kLenBase[li] > len) --li;
    putLit(bw, 257 + li);
    if (kLenExtra[li]) bw.put((uint32_t)(len - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    while (kDistBase[di] > dist) --di;
    bw.putRev((uint32_t)di, 5);
    if (kDistExtra[di]) bw.put((uint32_t)(dist - kDistBase[di]), kDistExtra[di]);
}

// zlib stream: one final fixed-Huffman block, greedy LZ77 with hash chains (32 KB window).
void zlibCompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    out.push_back(0x78);
    out.push_back(0x01);
    BitWriter bw(out);
    bw.put(1, 1);   // BFINAL
    bw.put(1, 2);   // fixed Huffman
    const int kWin = 32768, kHashBits = 15, kMaxChain = 48;
    std::vector<int> head(1 << kHashBits, -1), prev(in.size(), -1);
    const size_t n = in.size();
    auto hash = [&](size_t i) {
        return (uint32_t)(((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & ((1 << kHashBits) - 1));
    };
    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 2 < n) {
            uint32_t h = hash(i);
            int cand = head[h];
            int chain = kMaxChain;
            const size_t maxlen = (n - i) < 258 ? (n - i) : 258;
            while (cand >= 0 && (int)(i - cand) <= kWin && chain--) {
                size_t l = 0;
                while (l < maxlen && in[cand + l] == in[i + l]) ++l;
                if ((int)l > best_len) { best_len = (int)l; best_dist = (int)(i - cand); if (l == maxlen) break; }
                cand = prev[cand];
            }
            prev[i] = head[h];
            head[h] = (int)i;
        }
        if (best_len >= 3) {
            putMatch(bw, best_len, best_dist);
            for (size_t k = 1; k < (size_t)best_len; ++k) {
                size_t j = i + k;
                if (j + 2 < n) { uint32_t h = hash(j); prev[j] = head[h]; head[h] = (int)j; }
            }
            i += best_len;
        } else {
            putLit(bw, in[i]);
            ++i;
        }
    }
    putLit(bw, 256);
    bw.flush();
    uint32_t a = 1, b = 0;
    for (uint8_t v : in) { a = (a + v) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(adler >> s));
}

void putChunk(FILE* f, const char* type, const std::vector<uint8_t>& data)
{
    uint8_t len[4] = {(uint8_t)(data.size() >> 24), (uint8_t)(data.size() >> 16), (uint8_t)(data.size() >> 8), (uint8_t)data.size()};
    fwrite(len, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
    uint32_t crc = crc32(0, (const uint8_t*)type, 4);
    crc = crc32(crc, data.data(), data.size());
    uint8_t c[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(c, 1, 4, f);
}

int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

bool writePng(const char* path, const uint8_t* rgb, int w, int h)
{
    static bool crc_ready = false;
    if (!crc_ready) { crc_init(); crc_ready = true; }
    const int stride = w * 3;
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(stride + 1) * h);
    std::vector<uint8_t> cand[5];
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = rgb + (size_t)y * stride;
        const uint8_t* up = y ? row - stride : nullptr;
        long best_sum = -1;
        int best = 0;
        for (int ft = 0; ft < 5; ++ft) {
            cand[ft].resize(stride);
            long sum = 0;
            for (int x = 0; x < stride; ++x) {
                int a = x >= 3 ? row[x - 3] : 0, b = up ? up[x] : 0, c = (x >= 3 && up) ? up[x - 3] : 0;
                int v = row[x];
                int p = ft == 0 ? v : ft == 1 ? v - a : ft == 2 ? v - b : ft == 3 ? v - ((a + b) >> 1) : v - paeth(a, b, c);
                cand[ft][x] = (uint8_t)p;
                sum += abs((int8_t)(uint8_t)p);
            }
            if (best_sum < 0 || sum < best_sum) { best_sum = sum; best = ft; }
        }
        raw.push_back((uint8_t)best);
        raw.insert(raw.end(), cand[best].begin(), cand[best].end());
    }
    std::vector<uint8_t> z;
    zlibCompress(raw, z);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                 (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                                 8, 2, 0, 0, 0};
    putChunk(f, "IHDR", ihdr);
    putChunk(f, "IDAT", z);
    putChunk(f, "IEND", {});
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

}  // namespace

lv_display_t* lvhost_init(int w, int h)
{
    if (g_disp) return g_disp;
    lv_init();
    lv_tick_set_cb(tick_cb);
    g_w = w;
    g_h = h;
    g_disp = lv_display_create(w, h);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    const uint32_t size = (uint32_t)w * (uint32_t)h * 2u;
    g_fb = (uint8_t*)calloc(1, size + 64);
    lv_display_set_buffers(g_disp, g_fb, nullptr, size, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(g_disp, flush_cb);
    g_indev = lv_indev_create();
    lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_indev, read_cb);
    lv_indev_set_display(g_indev, g_disp);
    // Read the pointer every 10 ms and refresh every 16 ms so simulated gestures are sampled densely.
    lv_timer_set_period(lv_indev_get_read_timer(g_indev), 10);
    lv_timer_set_period(lv_display_get_refr_timer(g_disp), 16);
    lv_timer_handler();
    return g_disp;
}

void lvhost_run(int ms)
{
    while (ms > 0) {
        const int step = ms < 5 ? ms : 5;
        g_ms += (uint32_t)step;
        ms -= step;
        lv_timer_handler();
    }
}

bool lvhost_screenshot(const char* png_path)
{
    if (!g_disp) return false;
    lv_refr_now(g_disp);
    std::vector<uint8_t> rgb((size_t)g_w * g_h * 3);
    const uint16_t* src = (const uint16_t*)g_fb;
    for (int i = 0; i < g_w * g_h; ++i) {
        const uint16_t p = src[i];
        const int r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        rgb[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
        rgb[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
    const bool ok = writePng(png_path, rgb.data(), g_w, g_h);
    if (!ok) fprintf(stderr, "lvhost: cannot write %s\n", png_path);
    return ok;
}

void lvhost_pointer(int x, int y, bool pressed)
{
    g_px = x;
    g_py = y;
    g_pressed = pressed;
}

void lvhost_tap(int x, int y)
{
    lvhost_pointer(x, y, true);
    lvhost_run(60);
    lvhost_pointer(x, y, false);
    lvhost_run(100);
}

void lvhost_double_tap(int x, int y)
{
    lvhost_pointer(x, y, true);
    lvhost_run(50);
    lvhost_pointer(x, y, false);
    lvhost_run(80);
    lvhost_pointer(x, y, true);
    lvhost_run(50);
    lvhost_pointer(x, y, false);
    lvhost_run(100);
}

void lvhost_drag(int x0, int y0, int x1, int y1, int ms)
{
    lvhost_pointer(x0, y0, true);
    lvhost_run(40);
    const int steps = ms / 10 > 1 ? ms / 10 : 1;
    for (int i = 1; i <= steps; ++i) {
        lvhost_pointer(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, true);
        lvhost_run(10);
    }
    lvhost_run(30);
    lvhost_pointer(x1, y1, false);
    lvhost_run(100);
}

uint32_t lvhost_ms() { return g_ms; }
