// fontgen — TTF -> LVGL 9 lv_font_fmt_txt C source (4 bpp, uncompressed, optional kerning).
//
// Parsing and outline rasterisation: stb_truetype (LVGL's tiny_ttf copy). Pipeline per glyph:
//   1. rasterise the outline at S x supersampling (coverage, non-zero winding)
//   2. optional synthetic weight: grey-scale dilation of the supersampled coverage with a disc of
//      radius `weight` (em) — the same as thresholding the outline's distance field at -weight,
//      but cheaper and keeps the anti-aliased edge
//   3. area-downsample S x S -> 1 px, optional gamma, quantise to 4 bpp, trim empty borders
// Options:
//   --ttf F           main font            --size PX   em size in px (CSS font-size)
//   --name SYM        C symbol             --out FILE  output .c
//   --fallback F      font for code points the main font lacks (symbols ▶ ▾ ● ✕ ✎ ...)
//   --weight EM       per-side emboldening in em (0 = regular); advance grows by 2*weight
//   --track EM        extra advance per glyph in em (default 0)
//   --xh-snap         scale vertically so the x-height lands on a whole pixel (light hinting)
//   --gamma G         alpha' = alpha^G  (G < 1 darkens anti-aliased edges)
//   --tnum            tabular digits (0-9 share the widest digit advance, glyphs centred)
//   --kern            emit kerning pairs (|kern| >= 0.3 px)
//   --probe           print coverage + metrics only
// Build: tools/fontgen/build.sh (zig c++).
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype_htcw.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
    bool load(const char* path)
    {
        FILE* f = fopen(path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        data.resize((size_t)n);
        if (fread(data.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return false; }
        fclose(f);
        ok = stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0)) != 0;
        return ok;
    }
};

struct Opts {
    const char* ttf = nullptr;
    const char* fallback = nullptr;
    const char* name = nullptr;
    const char* out = nullptr;
    float size = 14.f;
    float weight = 0.f;
    float track = 0.f;
    float gamma = 1.f;
    bool xh_snap = false;
    bool tnum = false;
    bool kern = false;
    bool probe = false;
};

struct Glyph {
    uint32_t cp = 0;
    const Font* font = nullptr;
    int gi = 0;            // glyph index in its font
    int adv16 = 0;         // advance in 1/16 px
    int box_w = 0, box_h = 0, ofs_x = 0, ofs_y = 0;
    std::vector<uint8_t> a4;   // packed 4 bpp rows (no row padding, LVGL format)
};

std::vector<uint32_t> codepoints()
{
    std::vector<uint32_t> v;
    for (uint32_t c = 0x20; c <= 0x7E; ++c) v.push_back(c);
    for (uint32_t c = 0xA0; c <= 0x17F; ++c) v.push_back(c);
    const uint32_t extra[] = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026,
                              0x2190, 0x2192, 0x2161, 0x25B6, 0x25BE, 0x25CF, 0x2715, 0x270E};
    for (uint32_t c : extra) v.push_back(c);
    std::sort(v.begin(), v.end());
    return v;
}

int glyphTopUnits(const Font& f, uint32_t cp)
{
    int x0, y0, x1, y1;
    int g = stbtt_FindGlyphIndex(&f.info, (int)cp);
    if (!g || !stbtt_GetGlyphBox(&f.info, g, &x0, &y0, &x1, &y1)) return 0;
    return y1;
}

// Renders one glyph. scale_x/scale_y: font units -> px. shift_px: extra x shift (tnum centring).
void renderGlyph(Glyph& G, float scale_x, float scale_y, float weight_px, float shift_px, float gamma)
{
    const stbtt_fontinfo* info = &G.font->info;
    stbtt_vertex* verts = nullptr;
    int nv = stbtt_GetGlyphShape(info, G.gi, &verts);
    if (nv <= 0) { G.box_w = G.box_h = 0; return; }
    int ux0, uy0, ux1, uy1;
    stbtt_GetGlyphBox(info, G.gi, &ux0, &uy0, &ux1, &uy1);
    const float size_px = scale_y * 1000.f;   // only used to pick S
    (void)size_px;
    // Supersampling factor: denser for small glyphs so fractional weights are resolved finely.
    const float glyph_h = (float)(uy1 - uy0) * scale_y;
    int S = (int)std::lround(160.f / std::max(glyph_h, 4.f));
    S = std::clamp(S, 6, 16);
    const float shift = shift_px + weight_px;   // emboldening grows to the right, left edge stays
    // Pixel-space box (y down), padded for the dilation.
    const int pad = (int)std::ceil(weight_px) + 1;
    const int X0 = (int)std::floor(ux0 * scale_x + shift) - pad;
    const int X1 = (int)std::ceil(ux1 * scale_x + shift) + pad;
    const int Y0 = (int)std::floor(-uy1 * scale_y) - pad;
    const int Y1 = (int)std::ceil(-uy0 * scale_y) + pad;
    const int W = X1 - X0, H = Y1 - Y0;
    const int HW = W * S, HH = H * S;
    std::vector<unsigned char> hi((size_t)HW * HH, 0);
    stbtt__bitmap bm;
    bm.w = HW;
    bm.h = HH;
    bm.stride = HW;
    bm.pixels = hi.data();
    stbtt_Rasterize(&bm, 0.35f, verts, nv, scale_x * S, scale_y * S, shift * S, 0.f, X0 * S, Y0 * S, 1,
                    nullptr);
    stbtt_FreeShape(info, verts);

    if (weight_px > 0.f) {
        const float r = weight_px * S;
        const int ri = (int)std::floor(r);
        std::vector<int> half(2 * ri + 1);
        for (int dy = -ri; dy <= ri; ++dy) half[dy + ri] = (int)std::floor(std::sqrt(r * r - (float)(dy * dy)) + 1e-4f);
        // Row max for every needed half width, then max over the disc rows.
        std::vector<unsigned char> out((size_t)HW * HH, 0);
        std::vector<std::vector<unsigned char>> rowmax(ri + 1);
        for (int w = 0; w <= ri; ++w) {
            bool used = false;
            for (int h : half) used |= (h == w);
            if (!used) continue;
            rowmax[w].assign((size_t)HW * HH, 0);
            for (int y = 0; y < HH; ++y) {
                const unsigned char* src = &hi[(size_t)y * HW];
                unsigned char* dst = &rowmax[w][(size_t)y * HW];
                for (int x = 0; x < HW; ++x) {
                    unsigned char m = 0;
                    const int a = std::max(0, x - w), b = std::min(HW - 1, x + w);
                    for (int k = a; k <= b; ++k) m = std::max(m, src[k]);
                    dst[x] = m;
                }
            }
        }
        for (int y = 0; y < HH; ++y) {
            for (int dy = -ri; dy <= ri; ++dy) {
                const int sy = y + dy;
                if (sy < 0 || sy >= HH) continue;
                const unsigned char* src = &rowmax[half[dy + ri]][(size_t)sy * HW];
                unsigned char* dst = &out[(size_t)y * HW];
                for (int x = 0; x < HW; ++x) dst[x] = std::max(dst[x], src[x]);
            }
        }
        hi.swap(out);
    }

    // Downsample + gamma + quantise.
    std::vector<uint8_t> a((size_t)W * H, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned sum = 0;
            for (int sy = 0; sy < S; ++sy) {
                const unsigned char* row = &hi[(size_t)(y * S + sy) * HW + x * S];
                for (int sx = 0; sx < S; ++sx) sum += row[sx];
            }
            float v = (float)sum / (255.f * S * S);
            if (gamma != 1.f && v > 0.f) v = std::pow(v, gamma);
            int q = (int)std::lround(v * 15.f);
            a[(size_t)y * W + x] = (uint8_t)std::clamp(q, 0, 15);
        }
    // Trim empty borders.
    int tx0 = W, ty0 = H, tx1 = -1, ty1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (a[(size_t)y * W + x]) { tx0 = std::min(tx0, x); tx1 = std::max(tx1, x); ty0 = std::min(ty0, y); ty1 = std::max(ty1, y); }
    if (tx1 < 0) { G.box_w = G.box_h = 0; return; }
    G.box_w = tx1 - tx0 + 1;
    G.box_h = ty1 - ty0 + 1;
    G.ofs_x = X0 + tx0;
    G.ofs_y = -(Y0 + ty1 + 1);   // LVGL: bottom of the box above the baseline, y up
    // Pack 4 bpp continuously (LVGL fmt_txt packs the whole glyph bit stream, no row padding).
    const int total = G.box_w * G.box_h;
    G.a4.assign((size_t)(total + 1) / 2, 0);
    int k = 0;
    for (int y = ty0; y <= ty1; ++y)
        for (int x = tx0; x <= tx1; ++x, ++k) {
            const uint8_t v = a[(size_t)y * W + x];
            if (k & 1) G.a4[(size_t)k / 2] |= v;
            else G.a4[(size_t)k / 2] |= (uint8_t)(v << 4);
        }
}

void usage()
{
    fprintf(stderr, "usage: fontgen --ttf F --size PX --name SYM --out FILE [--fallback F] [--weight EM] "
                    "[--track EM] [--xh-snap] [--gamma G] [--tnum] [--kern] [--probe]\n");
    exit(2);
}

std::string cpComment(uint32_t cp)
{
    char buf[16];
    if (cp >= 0x20 && cp < 0x7F && cp != '\\' && cp != '"' && cp != '*' && cp != '/') {
        snprintf(buf, sizeof buf, "\"%c\"", (char)cp);
        return buf;
    }
    return "";
}

}  // namespace

int main(int argc, char** argv)
{
    Opts o;
    std::string cmdline;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (a == "--ttf") o.ttf = next();
        else if (a == "--fallback") o.fallback = next();
        else if (a == "--name") o.name = next();
        else if (a == "--out") o.out = next();
        else if (a == "--size") o.size = (float)atof(next());
        else if (a == "--weight") o.weight = (float)atof(next());
        else if (a == "--track") o.track = (float)atof(next());
        else if (a == "--gamma") o.gamma = (float)atof(next());
        else if (a == "--xh-snap") o.xh_snap = true;
        else if (a == "--tnum") o.tnum = true;
        else if (a == "--kern") o.kern = true;
        else if (a == "--probe") o.probe = true;
        else usage();
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        const char* slash = strrchr(argv[i], '/');
        if (a.size() > 4 && (a.substr(a.size() - 4) == ".ttf" || a.substr(a.size() - 2) == ".c") && slash) a = slash + 1;
        cmdline += (i > 1 ? " " : "") + a;
    }
    if (!o.ttf || (!o.probe && (!o.name || !o.out))) usage();
    Font main_font, fb_font;
    if (!main_font.load(o.ttf)) { fprintf(stderr, "cannot load %s\n", o.ttf); return 1; }
    if (o.fallback && !fb_font.load(o.fallback)) { fprintf(stderr, "cannot load %s\n", o.fallback); return 1; }

    const float scale = stbtt_ScaleForMappingEmToPixels(&main_font.info, o.size);
    float scale_y = scale;
    const int xh_units = glyphTopUnits(main_font, 'x');
    if (o.xh_snap && xh_units > 0) {
        const float xh = xh_units * scale;
        scale_y = std::max(1.f, std::round(xh)) / (float)xh_units;
    }
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&main_font.info, &asc, &desc, &gap);

    if (o.probe) {
        int x0, y0, x1, y1;
        int gl = stbtt_FindGlyphIndex(&main_font.info, 'l');
        stbtt_GetGlyphBox(&main_font.info, gl, &x0, &y0, &x1, &y1);
        int upm = (int)std::lround(o.size / scale);
        printf("upm %d asc %d desc %d gap %d xheight %d cap %d stem(l) %d (%.4f em)\n", upm, asc, desc, gap, xh_units,
               glyphTopUnits(main_font, 'H'), x1 - x0, (x1 - x0) / (double)upm);
        int missing = 0;
        for (uint32_t cp : codepoints()) {
            if (!stbtt_FindGlyphIndex(&main_font.info, (int)cp)) {
                bool in_fb = o.fallback && stbtt_FindGlyphIndex(&fb_font.info, (int)cp);
                printf("missing U+%04X%s\n", cp, in_fb ? " (fallback has it)" : "");
                ++missing;
            }
        }
        int aw, lsb;
        printf("digit advances:");
        for (int d = '0'; d <= '9'; ++d) { stbtt_GetCodepointHMetrics(&main_font.info, d, &aw, &lsb); printf(" %d", aw); }
        printf("\n%d missing\n", missing);
        return 0;
    }

    const float weight_px = o.weight * o.size;
    const float track_px = o.track * o.size + 2.f * weight_px;
    std::vector<Glyph> glyphs;
    int tab_adv_units = 0;
    if (o.tnum)
        for (int d = '0'; d <= '9'; ++d) {
            int aw, lsb;
            stbtt_GetCodepointHMetrics(&main_font.info, d, &aw, &lsb);
            tab_adv_units = std::max(tab_adv_units, aw);
        }
    for (uint32_t cp : codepoints()) {
        Glyph G;
        G.cp = cp;
        G.font = &main_font;
        G.gi = stbtt_FindGlyphIndex(&main_font.info, (int)cp);
        if (!G.gi && o.fallback) {
            G.font = &fb_font;
            G.gi = stbtt_FindGlyphIndex(&fb_font.info, (int)cp);
        }
        if (!G.gi) { fprintf(stderr, "%s: no glyph for U+%04X, skipped\n", o.name, cp); continue; }
        const float gscale = stbtt_ScaleForMappingEmToPixels(&G.font->info, o.size);
        const float gscale_y = G.font == &main_font ? scale_y : gscale;
        int aw, lsb;
        stbtt_GetGlyphHMetrics(&G.font->info, G.gi, &aw, &lsb);
        float adv = aw * gscale;
        float shift = 0.f;
        if (o.tnum && cp >= '0' && cp <= '9') {
            const float tab = tab_adv_units * gscale;
            shift = (tab - adv) * 0.5f;
            adv = tab;
        }
        adv += track_px;
        G.adv16 = (int)std::lround(adv * 16.f);
        if (cp != 0x20 && cp != 0xA0) renderGlyph(G, gscale, gscale_y, weight_px, shift + o.track * o.size * 0.5f, o.gamma);
        glyphs.push_back(std::move(G));
    }

    // Line metrics: hhea ascent/descent, extended to every rendered glyph.
    int top = (int)std::ceil(asc * scale_y), bottom = (int)std::ceil(-desc * scale_y);
    for (const Glyph& G : glyphs) {
        if (!G.box_h) continue;
        top = std::max(top, G.ofs_y + G.box_h);
        bottom = std::max(bottom, -G.ofs_y);
    }

    // Kerning pairs (glyph id = index + 1 in glyph_dsc).
    struct Pair { uint16_t l, r; int8_t v; };
    std::vector<Pair> pairs;
    if (o.kern) {
        for (size_t i = 0; i < glyphs.size(); ++i) {
            if (glyphs[i].font != &main_font) continue;
            for (size_t j = 0; j < glyphs.size(); ++j) {
                if (glyphs[j].font != &main_font) continue;
                int k = stbtt_GetGlyphKernAdvance(&main_font.info, glyphs[i].gi, glyphs[j].gi);
                if (!k) continue;
                float px = k * scale;
                if (std::fabs(px) < 0.3f) continue;
                int v = (int)std::lround(px * 16.f);
                v = std::clamp(v, -128, 127);
                pairs.push_back({(uint16_t)(i + 1), (uint16_t)(j + 1), (int8_t)v});
            }
        }
    }

    FILE* f = fopen(o.out, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", o.out); return 1; }
    auto P = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
    };
    P("/*******************************************************************************\n");
    P(" * Generated by tools/fontgen (do not edit). Size: %g px, 4 bpp\n", (double)o.size);
    P(" * fontgen %s\n", cmdline.c_str());
    P(" ******************************************************************************/\n\n");
    P("#include \"lvgl.h\"\n\n");
    P("/*-----------------\n *    BITMAPS\n *----------------*/\n\n");
    P("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n");
    std::vector<uint32_t> bidx(glyphs.size());
    uint32_t off = 0;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        const Glyph& G = glyphs[i];
        bidx[i] = off;
        P("    /* U+%04X %s */\n", G.cp, cpComment(G.cp).c_str());
        for (size_t k = 0; k < G.a4.size(); ++k) {
            if (k % 16 == 0) P("    ");
            P("0x%02x,%s", G.a4[k], (k % 16 == 15 || k + 1 == G.a4.size()) ? "\n" : " ");
        }
        off += (uint32_t)G.a4.size();
    }
    if (off == 0) P("    0\n");
    P("};\n\n");
    P("/*---------------------\n *  GLYPH DESCRIPTION\n *--------------------*/\n\n");
    P("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n");
    P("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,\n");
    for (size_t i = 0; i < glyphs.size(); ++i) {
        const Glyph& G = glyphs[i];
        P("    {.bitmap_index = %u, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d},\n", bidx[i],
          G.adv16, G.box_w, G.box_h, G.ofs_x, G.ofs_y);
    }
    P("};\n\n");
    // cmaps: contiguous runs -> FORMAT0_TINY, the rest -> one SPARSE_TINY list.
    P("/*---------------------\n *  CHARACTER MAPPING\n *--------------------*/\n\n");
    struct Run { uint32_t start, len, gid; };
    std::vector<Run> runs;
    std::vector<uint32_t> sparse;
    uint32_t sparse_gid = 0;
    // Latin ranges -> runs (any length); symbols above U+017F -> one sparse list at the end.
    for (size_t i = 0; i < glyphs.size();) {
        if (glyphs[i].cp > 0x17F) {
            if (sparse.empty()) sparse_gid = (uint32_t)(i + 1);
            sparse.push_back(glyphs[i].cp);
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < glyphs.size() && glyphs[j].cp <= 0x17F && glyphs[j].cp == glyphs[j - 1].cp + 1) ++j;
        runs.push_back({glyphs[i].cp, (uint32_t)(j - i), (uint32_t)(i + 1)});
        i = j;
    }
    bool sparse_ok = true;
    for (size_t k = 0; k < sparse.size(); ++k) {
        size_t gi = sparse_gid - 1 + k;
        if (gi >= glyphs.size() || glyphs[gi].cp != sparse[k]) sparse_ok = false;
    }
    if (!sparse_ok) { fprintf(stderr, "internal: sparse glyphs are not contiguous\n"); return 1; }
    if (!sparse.empty()) {
        P("static const uint16_t unicode_list_sparse[] = {\n");
        for (size_t k = 0; k < sparse.size(); ++k)
            P("%s0x%x,%s", k % 8 == 0 ? "    " : "", sparse[k] - sparse[0], (k % 8 == 7 || k + 1 == sparse.size()) ? "\n" : " ");
        P("};\n\n");
    }
    P("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n");
    for (const Run& r : runs)
        P("    {.range_start = %u, .range_length = %u, .glyph_id_start = %u, .unicode_list = NULL, .glyph_id_ofs_list = NULL, "
          ".list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},\n",
          r.start, r.len, r.gid);
    if (!sparse.empty())
        P("    {.range_start = %u, .range_length = %u, .glyph_id_start = %u, .unicode_list = unicode_list_sparse, "
          ".glyph_id_ofs_list = NULL, .list_length = %u, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY},\n",
          sparse[0], sparse.back() - sparse[0] + 1, sparse_gid, (unsigned)sparse.size());
    P("};\n\n");
    const unsigned cmap_num = (unsigned)runs.size() + (sparse.empty() ? 0u : 1u);
    if (!pairs.empty()) {
        P("/*-----------------\n *    KERNING\n *----------------*/\n\n");
        P("static const uint16_t kern_pair_glyph_ids[] = {\n");
        for (size_t k = 0; k < pairs.size(); ++k)
            P("%s%u, %u,%s", k % 6 == 0 ? "    " : "", pairs[k].l, pairs[k].r, (k % 6 == 5 || k + 1 == pairs.size()) ? "\n" : " ");
        P("};\n\nstatic const int8_t kern_pair_values[] = {\n");
        for (size_t k = 0; k < pairs.size(); ++k)
            P("%s%d,%s", k % 12 == 0 ? "    " : "", pairs[k].v, (k % 12 == 11 || k + 1 == pairs.size()) ? "\n" : " ");
        P("};\n\nstatic const lv_font_fmt_txt_kern_pair_t kern_pairs = {\n");
        P("    .glyph_ids = kern_pair_glyph_ids,\n    .values = kern_pair_values,\n    .pair_cnt = %u,\n    .glyph_ids_size = 1\n};\n\n",
          (unsigned)pairs.size());
    }
    P("static const lv_font_fmt_txt_dsc_t font_dsc = {\n");
    P("    .glyph_bitmap = glyph_bitmap,\n    .glyph_dsc = glyph_dsc,\n    .cmaps = cmaps,\n");
    P("    .kern_dsc = %s,\n    .kern_scale = 16,\n    .cmap_num = %u,\n    .bpp = 4,\n    .kern_classes = 0,\n",
      pairs.empty() ? "NULL" : "&kern_pairs", cmap_num);
    P("    .bitmap_format = 0,\n};\n\n");
    P("const lv_font_t %s = {\n", o.name);
    P("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n");
    P("    .line_height = %d,\n    .base_line = %d,\n    .subpx = LV_FONT_SUBPX_NONE,\n", top + bottom, bottom);
    P("    .underline_position = %d,\n    .underline_thickness = %d,\n",
      -(int)std::lround(o.size * 0.1f), std::max(1, (int)std::lround(o.size * 0.06f)));
    P("    .dsc = &font_dsc,\n    .fallback = NULL,\n    .user_data = NULL,\n};\n");
    fclose(f);
    fprintf(stderr, "%s: %zu glyphs, %u bitmap bytes, %zu kern pairs, line %d base %d\n", o.name, glyphs.size(), off,
            pairs.size(), top + bottom, bottom);
    return 0;
}
