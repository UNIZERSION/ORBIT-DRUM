// BROWSER view (work area, prototype .l-browser-view): search field + virtualized file list on
// the left (.browser-samples), preview card on the right (.l-preview: name, waveform, file info,
// ▶ PREVIEW / LOAD TO <pad> / LOAD & EDIT <pad> (OPEN IN SAMPLER when the pad already holds the
// file) / SAMPLE DETAILS; kits: ▶ PREVIEW / LOAD KIT). LOAD & EDIT replaces a different sample
// on the target pad layer only after a second tap within 3 s.
//
// The list holds thousands of entries with a fixed set of kRows row objects: a transparent
// spacer gives the scroll range, and on every scroll event the rows that left the viewport are
// re-bound (position = index * 43 px, texts set only when they differ). Durations are probed
// lazily (Library::probe, a WAV header read) for the visible rows, at most two per refresh.
// Kits category: SD kit folders followed by the two built-in kits; without an SD card the list
// shows a notice and only the built-in kits.
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <type_traits>
#include <utility>

#include "core/kits.h"
#include "core/library.h"
#include "core/sample.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/wave_view.h"
#include "ui/widgets.h"

namespace orbit::ui {

namespace view_browser {
namespace {

// ---------------------------------------------------------------- geometry (prototype px)
// View origin = work panel content (312,74). .browser-samples 325,87 460x458; .l-preview
// 798,87 245x458 (padding 15); search 336,98 438x35; rows 43 px from y 143.
constexpr int kPanelY = 13;
constexpr int kListX = 13, kListW = 460, kPanelH = 458;
constexpr int kPrevX = 486, kPrevW = 245;
constexpr int kInnerW = kListW - 2;           // 458
constexpr int kSearchTop = 55;                // list starts below the search strip
constexpr int kListH = kPanelH - 2 - kSearchTop - 3;
constexpr int kRowH = 43;
constexpr int kRows = kListH / kRowH + 3;     // recycled row objects (12)
constexpr int kPrevInner = kPrevW - 2 - 30;   // 213
constexpr int kWaveY = 77, kWaveH = 130;

const uint32_t &kSearchBg = col::search_bg, &kSearchBorder = col::search_line, &kSearchText = col::search_text;
// .browser-samples #dfe1e2: its nearest RGB565 colour is visibly green (222,227,222); LIGHT's list_bg
// #dedfdf renders neutral (222,223,222).
const uint32_t& kListBg = col::list_bg;
const uint32_t &kDurColor = col::dur_text, &kRowPressed = col::row_press;
const uint32_t &kFrameBg = col::wave_frame, &kFrameBorder = col::wave_border, &kGridText = col::wave_text;

constexpr int kNone = -1;
// Entry ids: >= 0 library index, kBuiltin0 - k = built-in kit k.
constexpr int kBuiltin0 = -2;
inline bool isBuiltin(int id) { return id <= kBuiltin0; }
inline int builtinOf(int id) { return kBuiltin0 - id; }
constexpr int kKitCountCache = 64;   // kit sound counts remembered (each one is a folder read)
constexpr uint32_t kConfirmMs = 3000;   // second tap of a replacing LOAD KIT
constexpr uint32_t kUndoMs = 8000;      // LOAD KIT reads UNDO LOAD KIT this long after a kit load

// ---------------------------------------------------------------- optional App calls
// App calls that may not exist in every App revision (requested from the App owner). Detected at
// compile time so this file builds either way; the fallback explains it with a toast.
template <class A, class = void> struct HasPreviewKit : std::false_type {};
template <class A>
struct HasPreviewKit<A, std::void_t<decltype(std::declval<A&>().previewKit(0))>> : std::true_type {};
template <class A, class = void> struct HasPreviewBuiltin : std::false_type {};
template <class A>
struct HasPreviewBuiltin<A, std::void_t<decltype(std::declval<A&>().previewBuiltinKit(0))>> : std::true_type {};

template <class A> bool optPreviewKit(A& a, int lib)
{
    if constexpr (HasPreviewKit<A>::value) {
        a.previewKit(lib);
        return true;
    } else {
        (void)a;
        (void)lib;
        return false;
    }
}
template <class A> bool optPreviewBuiltin(A& a, int kit)
{
    if constexpr (HasPreviewBuiltin<A>::value) {
        a.previewBuiltinKit(kit);
        return true;
    } else {
        (void)a;
        (void)kit;
        return false;
    }
}

// ---------------------------------------------------------------- state
struct Row {
    lv_obj_t* obj = nullptr;
    lv_obj_t* icon = nullptr;
    lv_obj_t* name = nullptr;
    lv_obj_t* dur = nullptr;
    int pos = INT_MIN;     // virtual list position bound to this row
    int id = kNone - 100;  // entry id shown
    int icon_kind = -1;
    bool pending = false;  // duration not known yet (probe later)
};

enum class Sel : uint8_t { None, File, Kit, Builtin };

struct State {
    lv_obj_t* root = nullptr;
    lv_obj_t* samples = nullptr;
    lv_obj_t* search = nullptr;
    lv_obj_t* clear = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* spacer = nullptr;
    lv_obj_t* notice = nullptr;     // "Brak karty SD ..." strip (no card)
    lv_obj_t* empty = nullptr;      // "no files" message
    Row rows[kRows];

    lv_obj_t* prev = nullptr;
    lv_obj_t* over = nullptr;       // SELECTED SAMPLE / SELECTED KIT
    lv_obj_t* name = nullptr;
    lv_obj_t* wave = nullptr;
    lv_obj_t* grid = nullptr;       // kit contents (replaces the waveform for kits)
    lv_obj_t* grid_l = nullptr;
    lv_obj_t* grid_r = nullptr;
    lv_obj_t* info = nullptr;
    lv_obj_t* busy = nullptr;       // WCZYTYWANIE… tag
    lv_obj_t* btn[4] = {};

    // model
    int count = 0;                  // virtual positions
    int hdr = 0;                    // notice height (no card)
    int sel_builtin = -1;           // built-in kit selected in the list (not an App entry)
    bool sd = false;
    bool need_probe = false;
    int list_sd = -1;               // list geometry laid out for (search strip shown: 1, no card: 0)
    // LOAD & EDIT over a different sample: armed by the first tap (entry, pad, layer, time)
    int edit_armed_id = kNone;
    int edit_armed_pad = -1, edit_armed_layer = -1;
    uint32_t edit_armed_ms = 0;
    // LOAD KIT over pads that hold samples: armed by the first tap (entry, time). After a load started
    // here, LOAD KIT reads UNDO LOAD KIT for kUndoMs while that kit stays selected (App::undoPads).
    int kit_armed_id = kNone;
    uint32_t kit_armed_ms = 0;
    bool kit_waiting = false;        // a kit load started here: its result arms the UNDO
    int kit_wait_id = kNone;
    uint32_t kit_wait_pads = 0;      // revs.pads when it started
    int kit_undo_id = kNone;         // entry whose load UNDO undoes
    uint32_t kit_undo_until = 0;     // lv_tick while LOAD KIT reads UNDO (0: not)

    // last drawn
    uint32_t rev_lib = 0, rev_sel = 0, rev_samples = 0;
    int last_cat = -1;
    int last_libcount = -1;
    char last_search[48] = {};
    int shown_sel = INT_MIN;
    Sel shown_kind = Sel::None;
    uint32_t shown_bp = 0xFFFFFFFFu;
    bool shown_probed = false;
    bool shown_busy = false;
    uint32_t kit_counts_key[kKitCountCache] = {}; // kit folder path hash -> sound count (a list page and more)
    int kit_counts_n[kKitCountCache] = {};
    int kit_counts_used = 0;
    int kit_counts_next = 0;         // round-robin replacement once full
};
State g;

lv_style_t s_row, s_row_sel, s_row_pr, s_btn_dis;
bool s_styles = false;

void initStyles()
{
    if (s_styles) return;
    s_styles = true;
    // .file-item: 43 px, bottom border #cdd0d1, padding 0 15; .selected #ffddd0; :hover #f9e1d7.
    lv_style_init(&s_row);
    lv_style_set_border_side(&s_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s_row, 1);
    lv_style_set_border_color(&s_row, c(col::file_line));
    lv_style_set_bg_opa(&s_row, LV_OPA_TRANSP);
    lv_style_set_bg_color(&s_row, c(col::file_sel));
    lv_style_set_pad_hor(&s_row, 15);
    lv_style_init(&s_row_sel);
    lv_style_set_bg_opa(&s_row_sel, LV_OPA_COVER);
    lv_style_set_bg_color(&s_row_sel, c(col::file_sel));
    lv_style_init(&s_row_pr);
    lv_style_set_bg_opa(&s_row_pr, LV_OPA_COVER);
    lv_style_set_bg_color(&s_row_pr, c(kRowPressed));
    // Disabled action buttons (nothing selected): flat tab colour, muted text.
    lv_style_init(&s_btn_dis);
    lv_style_set_bg_color(&s_btn_dis, c(col::tab));
    lv_style_set_border_color(&s_btn_dis, c(col::line));
    lv_style_set_text_color(&s_btn_dis, c(col::muted));
}

// ---------------------------------------------------------------- small helpers
void setText(lv_obj_t* l, const char* t)
{
    if (!l || !t) return;
    const char* cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

void setHidden(lv_obj_t* o, bool hidden)
{
    if (!o) return;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void setEnabled(lv_obj_t* o, bool en)
{
    if (!o) return;
    if (lv_obj_has_state(o, LV_STATE_DISABLED) == !en) return;
    lv_obj_set_state(o, LV_STATE_DISABLED, !en);
}

// Bounded UTF-8 safe copy (no "%s" formatting: GCC's truncation check knows the source sizes).
void copyText(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0) return;
    size_t n = 0;
    if (src) {
        while (n + 1 < cap && src[n]) ++n;
        if (src[n])
            while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) --n;   // do not split a character
        memcpy(dst, src, n);
    }
    dst[n] = 0;
}

const char* baseName(const char* path)
{
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

void padLabel(int pad, char* out, size_t n)
{
    if (pad < 0 || pad >= kPads) pad = 0;
    snprintf(out, n, "%c%d", 'A' + pad / 4, pad % 4 + 1);
}

void fmtDuration(float sec, char* out, size_t n)
{
    if (sec <= 0.0f) {
        snprintf(out, n, "—");
    } else if (sec < 10.0f) {
        snprintf(out, n, "%.3fs", (double)sec);
    } else if (sec < 60.0f) {
        snprintf(out, n, "%.1fs", (double)sec);
    } else {
        const int t = (int)(sec + 0.5f);
        snprintf(out, n, "%d:%02d", t / 60, t % 60);
    }
}

void fmtRate(uint32_t rate, char* out, size_t n)
{
    if (rate % 1000u == 0) snprintf(out, n, "%u KHZ", (unsigned)(rate / 1000u));
    else snprintf(out, n, "%.1f KHZ", (double)rate / 1000.0);
}

// "03_Closed_Hat" -> "Closed Hat" (kit file names carry the pad order as a prefix).
void soundName(const char* path, char* out, size_t n)
{
    char stem[kNameLen + 8];
    copyText(stem, sizeof stem, baseName(path));
    char* dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    const char* s = stem;
    while (*s >= '0' && *s <= '9') ++s;
    while (*s == '_' || *s == '-' || *s == ' ') ++s;
    if (!*s) s = stem;
    copyText(out, n, s);
    for (char* p = out; *p; ++p)
        if (*p == '_') *p = ' ';
}

uint32_t pathHash(const char* s)
{
    uint32_t h = 2166136261u;   // FNV-1a
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

// Sounds in a kit folder (a directory read): cached by folder path, so a rescan that renumbers
// the library never shows a stale count.
int kitSoundCount(int lib)
{
    const uint32_t key = pathHash(app().library().at(lib).path);
    for (int i = 0; i < g.kit_counts_used; ++i)
        if (g.kit_counts_key[i] == key) return g.kit_counts_n[i];
    static char paths[kPads][kPathLen];
    const int n = app().library().kitFiles(lib, paths, kPads);
    int used = 0;
    for (int i = 0; i < n; ++i)
        if (paths[i][0]) ++used;
    int slot;
    if (g.kit_counts_used < kKitCountCache) {
        slot = g.kit_counts_used++;
    } else {   // full: round robin (a long Kits list never pins one slot that every new row overwrites)
        slot = g.kit_counts_next;
        g.kit_counts_next = (g.kit_counts_next + 1) % kKitCountCache;
    }
    g.kit_counts_key[slot] = key;
    g.kit_counts_n[slot] = used;
    return used;
}

// ---------------------------------------------------------------- model
int builtinCount() { return builtinKitCount() < 2 ? builtinKitCount() : 2; }

bool kitsMode() { return app().ui().category == Category::Kits; }

int computeCount()
{
    if (!g.sd) return builtinCount();
    const int n = app().resultCount();
    return kitsMode() ? n + builtinCount() : n;
}

int idAt(int pos)
{
    if (pos < 0 || pos >= g.count) return kNone;
    if (!g.sd) return kBuiltin0 - pos;
    const int n = app().resultCount();
    if (pos < n) return app().result(pos);
    return kBuiltin0 - (pos - n);
}

// The selection shown: a built-in kit picked here, else the App's library selection when it
// belongs to the current mode (a kit only in Kits, a file only outside it).
int selectedId()
{
    if (g.sel_builtin >= 0) return kBuiltin0 - g.sel_builtin;
    if (!g.sd) return kNone;
    const int sel = app().ui().lib_sel;
    if (sel < 0 || sel >= app().library().count()) return kNone;
    return (app().library().at(sel).is_kit != 0) == kitsMode() ? sel : kNone;
}

Sel kindOf(int id)
{
    if (id == kNone) return Sel::None;
    if (isBuiltin(id)) return Sel::Builtin;
    return app().library().at(id).is_kit ? Sel::Kit : Sel::File;
}

// ---------------------------------------------------------------- rows
void bindRow(Row& r, int pos, bool force)
{
    if (pos < 0 || pos >= g.count) {
        setHidden(r.obj, true);
        r.pos = pos;
        r.id = kNone - 100;
        r.pending = false;
        return;
    }
    const int id = idAt(pos);
    if (r.pos != pos) lv_obj_set_y(r.obj, uiSy(r.obj, g.hdr + pos * kRowH));   // maximized: larger rows
    setHidden(r.obj, false);
    const bool changed = force || r.pos != pos || r.id != id;
    r.pos = pos;
    r.id = id;
    lv_obj_set_state(r.obj, LV_STATE_CHECKED, id == selectedId());
    if (!changed && !r.pending) return;
    char dur[24];
    int kind;
    if (isBuiltin(id)) {
        const int k = builtinOf(id);
        char nm[48];
        snprintf(nm, sizeof nm, "%s \xC2\xB7 wbudowany", builtinKit(k).name ? builtinKit(k).name : "KIT");
        setText(r.name, nm);
        snprintf(dur, sizeof dur, "%d sounds", builtinKit(k).count);
        kind = 2;
        r.pending = false;
    } else {
        const LibEntry& e = app().library().at(id);
        if (e.is_kit) {
            setText(r.name, e.name);
            snprintf(dur, sizeof dur, "%d sounds", kitSoundCount(id));
            kind = 1;
            r.pending = false;
        } else {
            setText(r.name, baseName(e.path));
            if (e.probed) fmtDuration(e.seconds, dur, sizeof dur);
            else dur[0] = 0;   // probed lazily (refresh)
            r.pending = !e.probed;
            if (r.pending) g.need_probe = true;
            kind = 0;
        }
    }
    setText(r.dur, dur);
    if (kind != r.icon_kind) {
        r.icon_kind = kind;
        iconSet(r.icon, kind == 0 ? Icon::Wave : (kind == 1 ? Icon::Folder : Icon::Grid), 18);
    }
}

void layoutRows(bool force)
{
    if (!g.list) return;
    const float ys = uiScaleOf(g.list).sy;   // the scroll position is in drawn px
    int sy = (int)lroundf((float)lv_obj_get_scroll_y(g.list) / ys) - g.hdr;
    if (sy < 0) sy = 0;
    const int first = sy / kRowH;
    for (int pos = first; pos < first + kRows; ++pos) {
        Row& r = g.rows[pos % kRows];
        if (force || r.pos != pos) bindRow(r, pos, force);
    }
}

void updateSelectionMarks()
{
    const int sel = selectedId();
    for (Row& r : g.rows)
        if (r.obj && r.pos >= 0 && r.pos < g.count) lv_obj_set_state(r.obj, LV_STATE_CHECKED, r.id == sel);
}

// Header reads of the visible rows (lazy, a few per refresh: each is an SD file open).
void probeVisible()
{
    if (!g.need_probe || !g.sd) return;
    int budget = 2;
    bool more = false;
    const uint32_t t0 = lv_tick_get();
    for (Row& r : g.rows) {
        if (!r.pending || r.pos < 0 || r.pos >= g.count || isBuiltin(r.id) || r.id < 0) {
            r.pending = false;
            continue;
        }
        if (budget <= 0 || lv_tick_elaps(t0) >= 4) {   // keep the frame: the rest next refresh
            more = true;
            continue;
        }
        app().library().probe(r.id);
        --budget;
        const LibEntry& e = app().library().at(r.id);
        char dur[24];
        if (e.probed) fmtDuration(e.seconds, dur, sizeof dur);
        else dur[0] = 0;
        setText(r.dur, dur);
        r.pending = false;
    }
    g.need_probe = more;
}

// ---------------------------------------------------------------- preview card
// LOAD KIT reads UNDO LOAD KIT: a kit load started here replaced pads less than kUndoMs ago, the App
// still holds that undo, and the same kit is selected.
bool kitUndoShown(int id)
{
    return g.kit_undo_until && id != kNone && id == g.kit_undo_id && app().canUndoPads() &&
           (int32_t)(lv_tick_get() - g.kit_undo_until) < 0;
}

// The selected pad layer already plays this library file (unedited: an edited sample has no path).
bool padHoldsFile(int id)
{
    if (id < 0 || kindOf(id) != Sel::File) return false;
    const Sample* s = app().padSample(app().ui().pad, app().ui().layer);
    return s && s->path[0] && strcmp(s->path, app().library().at(id).path) == 0;
}

void updateLoadLabel()
{
    if (!g.btn[1]) return;
    const int id = selectedId();
    const Sel k = kindOf(id);
    char t[40];
    char lbl[8], dst[24];
    padLabel(app().ui().pad, lbl, sizeof lbl);
    if (app().ui().layer > 0) snprintf(dst, sizeof dst, "%s \xC2\xB7 L%d", lbl, app().ui().layer + 1);
    else snprintf(dst, sizeof dst, "%s", lbl);
    if (k == Sel::Kit || k == Sel::Builtin) snprintf(t, sizeof t, kitUndoShown(id) ? "UNDO LOAD KIT" : "LOAD KIT");
    else snprintf(t, sizeof t, "LOAD TO %s", dst);
    btnSetText(g.btn[1], t);
    // The third button loads too unless the pad already holds the file: say so on the button.
    if (g.btn[2]) {
        if (k != Sel::File || padHoldsFile(id)) snprintf(t, sizeof t, "OPEN IN SAMPLER");
        else snprintf(t, sizeof t, "LOAD & EDIT %s", dst);
        btnSetText(g.btn[2], t);
    }
}

void fillGrid(const char* const* names, int n)
{
    // Two columns of 8 lines: "A1 Kick" (pad label in the accent colour).
    char left[400], right[400];
    size_t lu = 0, ru = 0;
    left[0] = right[0] = 0;
    for (int i = 0; i < kPads; ++i) {
        char lbl[8];
        padLabel(i, lbl, sizeof lbl);
        char line[64];
        snprintf(line, sizeof line, "%s#FF6531 %s# %s", (i % 8) ? "\n" : "", lbl, (i < n && names[i]) ? names[i] : "—");
        char* dst = i < 8 ? left : right;
        size_t& used = i < 8 ? lu : ru;
        const int w = snprintf(dst + used, sizeof left - used, "%s", line);
        if (w > 0) used += (size_t)w < sizeof left - used ? (size_t)w : sizeof left - used - 1;
    }
    setText(g.grid_l, left);
    setText(g.grid_r, right);
}

void showKitContents(int id)
{
    static char names_buf[kPads][32];
    const char* names[kPads] = {};
    int n = 0;
    if (isBuiltin(id)) {
        const BuiltinKit& k = builtinKit(builtinOf(id));
        n = k.count < kPads ? k.count : kPads;
        for (int i = 0; i < n; ++i) {
            const BuiltinSound& s = k.sounds[i];
            copyText(names_buf[i], sizeof names_buf[i], s.long_name ? s.long_name : s.name);
            names[i] = names_buf[i];
        }
    } else {
        static char paths[kPads][kPathLen];
        n = app().library().kitFiles(id, paths, kPads);
        for (int i = 0; i < n; ++i) {
            if (paths[i][0]) soundName(paths[i], names_buf[i], sizeof names_buf[i]);
            else copyText(names_buf[i], sizeof names_buf[i], "\xE2\x80\x94");
            names[i] = names_buf[i];
        }
    }
    fillGrid(names, n);
}

void updatePreview(bool force)
{
    if (!g.prev) return;
    const int id = selectedId();
    const Sel kind = kindOf(id);
    const Sample* bp = app().previewSample();
    bool probed = false;
    if (kind == Sel::File) probed = app().library().at(id).probed != 0;
    // Waveform: only when the decoded preview belongs to the selected file.
    const Sample* show = nullptr;
    if (kind == Sel::File && bp && strcmp(bp->path, app().library().at(id).path) == 0) show = bp;
    const uint32_t bpid = show ? show->id : 0;
    if (!force && id == g.shown_sel && kind == g.shown_kind && bpid == g.shown_bp && probed == g.shown_probed)
        return;
    const bool sel_changed = force || id != g.shown_sel || kind != g.shown_kind;
    g.shown_sel = id;
    g.shown_kind = kind;
    g.shown_bp = bpid;
    g.shown_probed = probed;

    const bool kit = kind == Sel::Kit || kind == Sel::Builtin;
    setText(g.over, kit ? "SELECTED KIT" : "SELECTED SAMPLE");
    setHidden(g.wave, kit);
    setHidden(g.grid, !kit);
    char info[96] = "";
    switch (kind) {
    case Sel::None:
        setText(g.name, g.sd ? "Wybierz plik z listy" : "Wybierz zestaw");
        lv_obj_set_style_text_color(g.name, c(col::muted), 0);
        break;
    case Sel::File: {
        const LibEntry& e = app().library().at(id);
        setText(g.name, e.name);
        lv_obj_set_style_text_color(g.name, c(col::ink), 0);
        if (e.probed && e.rate) {
            char rate[16], dur[24];
            fmtRate(e.rate, rate, sizeof rate);
            fmtDuration(e.seconds, dur, sizeof dur);
            for (char* p = dur; *p; ++p)
                if (*p == 's') *p = 'S';
            snprintf(info, sizeof info, "%s \xE2\x80\xA2 %u-BIT %s \xE2\x80\xA2 %s", rate, (unsigned)e.bits,
                     e.channels == 2 ? "STEREO" : "WAV", dur);
        } else {
            snprintf(info, sizeof info, "WAV");
        }
        break;
    }
    case Sel::Kit: {
        const LibEntry& e = app().library().at(id);
        setText(g.name, e.name);
        lv_obj_set_style_text_color(g.name, c(col::ink), 0);
        snprintf(info, sizeof info, "%d SOUNDS \xE2\x80\xA2 SD KIT", kitSoundCount(id));
        if (sel_changed) showKitContents(id);
        break;
    }
    case Sel::Builtin: {
        const BuiltinKit& k = builtinKit(builtinOf(id));
        setText(g.name, k.name ? k.name : "KIT");
        lv_obj_set_style_text_color(g.name, c(col::ink), 0);
        char rate[16];
        fmtRate(k.rate, rate, sizeof rate);
        snprintf(info, sizeof info, "%d SOUNDS \xE2\x80\xA2 %s \xE2\x80\xA2 BUILT-IN", k.count, rate);
        if (sel_changed) showKitContents(id);
        break;
    }
    }
    setText(g.info, info);
    if (!kit) wave::setSample(g.wave, show);

    // Buttons: files 4, kits 2.
    const bool any = kind != Sel::None;
    setEnabled(g.btn[0], any);
    setEnabled(g.btn[1], any);
    setHidden(g.btn[2], kit);
    setHidden(g.btn[3], kit);
    setEnabled(g.btn[2], kind == Sel::File);
    setEnabled(g.btn[3], true);
    if (sel_changed) updateLoadLabel();
}

// ---------------------------------------------------------------- whole model refresh
void modelUpdate()
{
    const UiState& u = app().ui();
    g.sd = app().sdReady();
    g.hdr = g.sd ? 0 : kRowH;
    g.count = computeCount();
    if (g.sd && g.sel_builtin >= 0 && !kitsMode()) g.sel_builtin = -1;   // built-ins live in Kits only
    const int libcount = g.sd ? app().library().count() : -1;
    const bool reset = (int)u.category != g.last_cat || strcmp(u.search, g.last_search) != 0 || libcount != g.last_libcount;
    if (reset) {
        g.last_cat = (int)u.category;
        g.last_libcount = libcount;
        copyText(g.last_search, sizeof g.last_search, u.search);
        g.kit_counts_used = 0;
        g.kit_counts_next = 0;
        textFieldSet(g.search, u.search);
    }
    setHidden(g.search, !g.sd);   // search filters the SD library only
    setHidden(g.clear, !g.sd || u.search[0] == 0);
    if ((int)g.sd != g.list_sd) {   // no card: the list starts at the top instead of under the search strip
        g.list_sd = g.sd ? 1 : 0;
        const int top = g.sd ? kSearchTop : 3;
        lv_obj_set_pos(g.list, 0, uiSy(g.list, top));
        lv_obj_set_height(g.list, uiSy(g.list, kPanelH - 2 - top - 3));
    }
    lv_obj_set_height(g.spacer, uiSy(g.spacer, g.hdr + (g.count > 0 ? g.count : 1) * kRowH));
    if (reset) lv_obj_scroll_to_y(g.list, 0, LV_ANIM_OFF);
    setHidden(g.notice, g.sd);
    // Empty list message.
    if (g.count == 0) {
        char msg[96];
        if (u.search[0]) snprintf(msg, sizeof msg, "Brak wyników dla \xE2\x80\x9E%s\xE2\x80\x9D", u.search);
        else if (app().loadingCount() > 0 && app().library().count() == 0) snprintf(msg, sizeof msg, "Skanowanie karty SD\xE2\x80\xA6");
        else snprintf(msg, sizeof msg, "Brak plików WAV w kategorii %s", categoryName(u.category));
        setText(g.empty, msg);
    }
    setHidden(g.empty, g.count != 0);
    layoutRows(true);
    updatePreview(false);
}

// ---------------------------------------------------------------- events
void onRowClicked(lv_event_t* e)
{
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_current_target(e);
    const int slot = (int)(intptr_t)lv_obj_get_user_data(obj);
    if (slot < 0 || slot >= kRows) return;
    const int id = g.rows[slot].id;
    if (id == kNone || g.rows[slot].pos < 0 || g.rows[slot].pos >= g.count) return;
    const bool again = id == selectedId();
    if (isBuiltin(id)) {
        if (again) {
            if (!optPreviewBuiltin(app(), builtinOf(id))) app().toast("Odsłuch zestawu: użyj LOAD KIT");
            return;
        }
        g.sel_builtin = builtinOf(id);
        updateSelectionMarks();
        updatePreview(true);
        return;
    }
    if (again) {   // second tap on the selected row plays it
        if (app().library().at(id).is_kit) {
            if (!optPreviewKit(app(), id)) app().toast("Odsłuch zestawu: użyj LOAD KIT");
        } else {
            app().previewEntry(id);
        }
        return;
    }
    g.sel_builtin = -1;
    app().selectEntry(id);   // bumps revs.library -> refresh redraws marks + preview
    updateSelectionMarks();
    updatePreview(false);
}

void onListScroll(lv_event_t*) { layoutRows(false); }

void onSearchDone(const char* text, void*)
{
    app().setSearch(text ? text : "");
    refresh();
}

void onClear(lv_event_t*)
{
    textFieldSet(g.search, "");
    app().setSearch("");
    refresh();
}

void onPreview(lv_event_t*)
{
    const int id = selectedId();
    switch (kindOf(id)) {
    case Sel::File: app().previewEntry(id); break;
    case Sel::Kit:
        if (!optPreviewKit(app(), id)) app().toast("Odsłuch zestawu: użyj LOAD KIT");
        break;
    case Sel::Builtin:
        if (!optPreviewBuiltin(app(), builtinOf(id))) app().toast("Odsłuch zestawu: użyj LOAD KIT");
        break;
    case Sel::None: break;
    }
}

// LOAD KIT. Replacing pads that hold samples needs a second tap within kConfirmMs (the first names
// how many pads it replaces); afterwards the button reads UNDO LOAD KIT for kUndoMs (App::undoPads
// brings the pads back, edited samples too).
void onLoadKit(int id)
{
    if (kitUndoShown(id)) {
        g.kit_undo_until = 0;
        app().undoPads();   // the App says what came back
        updateLoadLabel();
        return;
    }
    int n = 0;
    if (isBuiltin(id)) {
        n = builtinKit(builtinOf(id)).count;
    } else {
        n = kitSoundCount(id);   // cached folder read (the list rows show it too)
    }
    if (n > kPads) n = kPads;
    int busy = 0, first = -1, last = -1;
    for (int i = 0; i < n; ++i)
        if (app().padSample(i, -1)) {
            ++busy;
            if (first < 0) first = i;
            last = i;
        }
    const bool confirmed = g.kit_armed_id == id && lv_tick_elaps(g.kit_armed_ms) < kConfirmMs;
    if (busy > 0 && !confirmed) {
        g.kit_armed_id = id;
        g.kit_armed_ms = lv_tick_get();
        char a[8], b[8];
        padLabel(first, a, sizeof a);
        padLabel(last, b, sizeof b);
        const int m10 = busy % 10, m100 = busy % 100;
        const char* word = busy == 1 ? "pad" : (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? "pady" : "padów";
        if (busy == 1)
            app().toast("LOAD KIT zastąpi pad %s \xE2\x80\x94 dotknij ponownie", a);
        else
            app().toast("LOAD KIT zastąpi %d %s (%s\xE2\x80\x93%s) \xE2\x80\x94 dotknij ponownie", busy, word, a, b);
        return;
    }
    g.kit_armed_id = kNone;
    g.kit_waiting = true;
    g.kit_wait_id = id;
    g.kit_wait_pads = app().revs().pads;
    if (isBuiltin(id)) app().loadBuiltinKit(builtinOf(id));
    else if (!app().loadKit(id)) g.kit_waiting = false;
}

void onLoad(lv_event_t*)
{
    const int id = selectedId();
    switch (kindOf(id)) {
    case Sel::File:
        if (!app().loadFileToPad(id, app().ui().pad, app().ui().layer)) app().toast("Nie można wczytać pliku");
        break;
    case Sel::Kit:
    case Sel::Builtin: onLoadKit(id); break;
    case Sel::None: break;
    }
}

// LOAD & EDIT <pad> / OPEN IN SAMPLER. The pad already holds the file: only the view changes.
// An empty layer: load + SAMPLE view. Another sample on that layer: the first tap names what
// would be replaced, a second tap within 3 s (same file, pad and layer) replaces it.
void onOpenSampler(lv_event_t*)
{
    const int id = selectedId();
    if (kindOf(id) != Sel::File) return;
    const int pad = app().ui().pad, layer = app().ui().layer;
    if (padHoldsFile(id)) {
        g.edit_armed_id = kNone;
        app().setView(View::Sample);
        return;
    }
    const Sample* cur = app().padSample(pad, layer);
    const bool confirmed = g.edit_armed_id == id && g.edit_armed_pad == pad && g.edit_armed_layer == layer &&
                           lv_tick_elaps(g.edit_armed_ms) < 3000;
    if (cur && !confirmed) {
        g.edit_armed_id = id;
        g.edit_armed_pad = pad;
        g.edit_armed_layer = layer;
        g.edit_armed_ms = lv_tick_get();
        char lbl[8];
        padLabel(pad, lbl, sizeof lbl);
        if (layer > 0)
            app().toast("%s \xC2\xB7 L%d gra %s \xE2\x80\x94 dotknij ponownie, aby zastąpić", lbl, layer + 1,
                        cur->name[0] ? cur->name : "sampel");
        else
            app().toast("%s gra %s \xE2\x80\x94 dotknij ponownie, aby zastąpić", lbl, cur->name[0] ? cur->name : "sampel");
        return;
    }
    g.edit_armed_id = kNone;
    if (app().loadFileToPad(id, pad, layer)) app().setView(View::Sample);
    else app().toast("Nie można wczytać pliku");
}

void onDetails(lv_event_t*) { dlg_sample::open(); }

void onRootDelete(lv_event_t*) { g = State{}; }

// ---------------------------------------------------------------- construction
lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, c(bg), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, c(col::line), 0);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void buildList()
{
    g.samples = box(g.root, kListX, kPanelY, kListW, kPanelH, kListBg);

    // .l-input-search: 438x35 at 10,10, #e9ebec, border #bac0c3, radius 5, padding 0 12.
    g.search = textFieldCreate(g.samples, kInnerW - 20, app().ui().search, (int)sizeof(app().ui().search) - 1,
                               onSearchDone, nullptr, "Search .wav samples...");
    lv_obj_set_pos(g.search, 10, 10);
    lv_obj_set_height(g.search, 35);
    lv_obj_set_style_bg_color(g.search, c(kSearchBg), 0);
    lv_obj_set_style_border_color(g.search, c(kSearchBorder), 0);
    lv_obj_set_style_radius(g.search, 5, 0);
    lv_obj_set_style_pad_left(g.search, 12, 0);
    lv_obj_set_style_pad_right(g.search, 36, 0);
    lv_obj_set_style_text_font(g.search, f_r14, 0);
    lv_obj_set_style_text_color(g.search, c(kSearchText), 0);
    lv_obj_set_ext_click_area(g.search, 8);

    g.clear = iconBtn(g.samples, Icon::Close, 16, 30);
    lv_obj_set_pos(g.clear, 10 + kInnerW - 20 - 33, 12);
    lv_obj_add_event_cb(g.clear, onClear, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(g.clear, LV_OBJ_FLAG_HIDDEN);

    g.list = lv_obj_create(g.samples);
    lv_obj_remove_style_all(g.list);
    lv_obj_set_pos(g.list, 0, kSearchTop);
    lv_obj_set_size(g.list, kInnerW, kListH);
    lv_obj_set_scroll_dir(g.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g.list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_remove_flag(g.list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g.list, onListScroll, LV_EVENT_SCROLL, nullptr);

    g.spacer = lv_obj_create(g.list);
    lv_obj_remove_style_all(g.spacer);
    lv_obj_set_pos(g.spacer, 0, 0);
    lv_obj_set_size(g.spacer, 1, kRowH);
    lv_obj_remove_flag(g.spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g.spacer, LV_OBJ_FLAG_SCROLLABLE);

    // No card: notice strip above the built-in kits.
    g.notice = lv_obj_create(g.list);
    lv_obj_remove_style_all(g.notice);
    lv_obj_add_style(g.notice, &s_row, 0);
    lv_obj_set_pos(g.notice, 0, 0);
    lv_obj_set_size(g.notice, kInnerW, kRowH);
    lv_obj_remove_flag(g.notice, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g.notice, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* ni = iconCreate(g.notice, Icon::Info, 18, col::accent_dark);
    lv_obj_align(ni, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* nl = label(g.notice, "Brak karty SD \xE2\x80\x94 dostępne są wbudowane zestawy", f_r13, col::accent_dark);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 29, 0);
    lv_obj_add_flag(g.notice, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < kRows; ++i) {
        Row& r = g.rows[i];
        r.obj = lv_obj_create(g.list);
        lv_obj_remove_style_all(r.obj);
        lv_obj_add_style(r.obj, &s_row, 0);
        lv_obj_add_style(r.obj, &s_row_sel, LV_STATE_CHECKED);
        lv_obj_add_style(r.obj, &s_row_pr, LV_STATE_PRESSED);
        lv_obj_set_size(r.obj, kInnerW, kRowH);
        lv_obj_set_pos(r.obj, 0, i * kRowH);
        lv_obj_remove_flag(r.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(r.obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(r.obj, (void*)(intptr_t)i);
        lv_obj_add_event_cb(r.obj, onRowClicked, LV_EVENT_CLICKED, nullptr);
        r.icon = iconCreate(r.obj, Icon::Wave, 18, col::ink);
        lv_obj_align(r.icon, LV_ALIGN_LEFT_MID, 0, 0);
        r.name = label(r.obj, "", f_r13, col::ink);
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(r.name, kInnerW - 30 - 29 - 90);
        lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 29, 0);
        r.dur = label(r.obj, "", f_r12, kDurColor);
        lv_obj_set_width(r.dur, 86);
        lv_obj_set_style_text_align(r.dur, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r.dur, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
    }

    g.empty = label(g.list, "", f_r14, col::muted);
    lv_obj_set_width(g.empty, kInnerW - 40);
    lv_label_set_long_mode(g.empty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(g.empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.empty, 20, 60);
    lv_obj_add_flag(g.empty, LV_OBJ_FLAG_HIDDEN);
}

void buildPreview()
{
    g.prev = box(g.root, kPrevX, kPanelY, kPrevW, kPanelH, col::preview_bg);
    lv_obj_set_style_pad_all(g.prev, 15, 0);

    g.over = overline(g.prev, "SELECTED SAMPLE");
    lv_obj_set_style_text_font(g.over, f_s13, 0);   // 13 px caption (1.2x rule): the box has room
    lv_obj_set_style_text_letter_space(g.over, ls(13, .13f), 0);
    lv_obj_set_pos(g.over, 0, 3);

    g.name = label(g.prev, "", f_m20, col::ink);
    lv_label_set_long_mode(g.name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g.name, kPrevInner);
    lv_obj_set_pos(g.name, 0, 31);

    g.wave = wave::create(g.prev, kPrevInner, kWaveH);
    lv_obj_set_pos(g.wave, 0, kWaveY);
    wave::setShowTime(g.wave, false);
    wave::setHandles(g.wave, false);
    wave::setPlayhead(g.wave, -1.0f);

    g.grid = lv_obj_create(g.prev);
    lv_obj_remove_style_all(g.grid);
    lv_obj_set_pos(g.grid, 0, kWaveY);
    lv_obj_set_size(g.grid, kPrevInner, kWaveH);
    lv_obj_set_style_bg_opa(g.grid, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.grid, c(kFrameBg), 0);
    lv_obj_set_style_border_width(g.grid, 1, 0);
    lv_obj_set_style_border_color(g.grid, c(kFrameBorder), 0);
    lv_obj_set_style_pad_hor(g.grid, 5, 0);
    lv_obj_set_style_pad_ver(g.grid, 3, 0);
    lv_obj_remove_flag(g.grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g.grid, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* l = label(g.grid, "", f_r12, kGridText);
        lv_label_set_recolor(l, true);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(l, (kPrevInner - 12) / 2, kWaveH - 8);
        lv_obj_set_pos(l, i * ((kPrevInner - 12) / 2 + 2), 0);
        (i ? g.grid_r : g.grid_l) = l;
    }
    lv_obj_add_flag(g.grid, LV_OBJ_FLAG_HIDDEN);

    // Loader activity tag, top-right corner of the waveform frame.
    g.busy = label(g.prev, "WCZYTYWANIE\xE2\x80\xA6", f_m12, col::on_accent);
    lv_obj_set_style_bg_opa(g.busy, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.busy, c(col::accent), 0);
    lv_obj_set_style_radius(g.busy, 3, 0);
    lv_obj_set_style_pad_hor(g.busy, 6, 0);
    lv_obj_set_style_pad_ver(g.busy, 2, 0);
    lv_obj_align(g.busy, LV_ALIGN_TOP_RIGHT, -6, kWaveY + 6);
    lv_obj_add_flag(g.busy, LV_OBJ_FLAG_HIDDEN);

    g.info = overline(g.prev, "");
    lv_obj_set_style_text_letter_space(g.info, 1, 0);
    lv_label_set_long_mode(g.info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g.info, kPrevInner);
    lv_obj_set_pos(g.info, 0, 225);

    static const char* const kText[4] = {"\xE2\x96\xB6 PREVIEW", "LOAD TO A1", "OPEN IN SAMPLER", "SAMPLE DETAILS"};
    static const lv_event_cb_t kCb[4] = {onPreview, onLoad, onOpenSampler, onDetails};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* b = actionBtn(g.prev, kText[i], i == 0, kPrevInner, 34);
        lv_obj_set_pos(b, 0, 258 + i * 44);
        lv_obj_add_style(b, &s_btn_dis, LV_STATE_DISABLED);
        lv_obj_add_event_cb(b, kCb[i], LV_EVENT_CLICKED, nullptr);
        g.btn[i] = b;
    }
}

}  // namespace

// ---------------------------------------------------------------- public
lv_obj_t* create(lv_obj_t* work)
{
    initStyles();
    g = State{};
    g.root = lv_obj_create(work);
    lv_obj_remove_style_all(g.root);
    lv_obj_set_size(g.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g.root, 0, 0);
    lv_obj_remove_flag(g.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g.root, onRootDelete, LV_EVENT_DELETE, nullptr);
    buildList();
    buildPreview();
    refresh();
    return g.root;
}

void refresh()
{
    if (!g.root) return;
    const Revs& r = app().revs();
    if (r.library != g.rev_lib || app().sdReady() != g.sd) {
        g.rev_lib = r.library;
        modelUpdate();
    }
    if (r.selection != g.rev_sel || r.samples != g.rev_samples) {   // pad / layer / its sample changed
        g.rev_sel = r.selection;
        g.rev_samples = r.samples;
        updateLoadLabel();
    }
    updatePreview(false);   // cheap compare; catches the decoded preview arriving
    // A kit load started here finished (loader idle, pads changed): LOAD KIT turns into UNDO LOAD KIT.
    if (g.kit_waiting && app().loadingCount() == 0 && r.pads != g.kit_wait_pads) {
        g.kit_waiting = false;
        const char* l = app().undoPadsLabel();
        if (app().canUndoPads() && l && (strcmp(l, "LOAD KIT") == 0 || strcmp(l, "ZESTAW") == 0)) {
            g.kit_undo_id = g.kit_wait_id;
            g.kit_undo_until = lv_tick_get() + kUndoMs;
            if (!g.kit_undo_until) g.kit_undo_until = 1;
            updateLoadLabel();
        }
    }
    if (g.kit_undo_until && !kitUndoShown(g.kit_undo_id)) {   // expired, undone elsewhere or replaced
        g.kit_undo_until = 0;
        updateLoadLabel();
    }
    const bool busy = app().loadingCount() > 0;
    if (busy != g.shown_busy) {
        g.shown_busy = busy;
        setHidden(g.busy, !busy);
    }
    probeVisible();
}

// ui.cpp runs the screen-enter animation; here only a full re-bind (the library may have changed
// while the view was hidden).
void onShow()
{
    if (!g.root) return;
    g.rev_lib = 0;
    refresh();
}

void focusSearch()
{
    if (!g.search) return;
    if (!app().sdReady()) {
        app().toast("Brak karty SD \xE2\x80\x94 dostępne są wbudowane zestawy");
        return;
    }
    lv_obj_send_event(g.search, LV_EVENT_CLICKED, nullptr);   // the text field opens the keyboard
}

}  // namespace view_browser
}  // namespace orbit::ui
