# fontgen — UI fonts for ORBIT DRUM

`fontgen` turns a TTF into an LVGL 9 `lv_font_fmt_txt` C file (4 bpp, uncompressed bitmaps,
optional kerning pairs). It is a small C++ program built with zig on top of LVGL's own copy of
stb_truetype (`firmware/managed_components/lvgl__lvgl/src/libs/tiny_ttf/stb_truetype_htcw.h`).
No Node.js, no network.

```
bash tools/fontgen/build.sh              # build the tool + regenerate firmware/main/ui/fonts/*.c
bash tools/fontgen/build.sh --tool-only  # only the tool
bash tools/fontgen/specimen.sh           # weight/face comparison against reference/shots
```

## Character set

`0x20-0x7E`, `0xA0-0x17F` (Latin-1 + Latin Extended-A: every Polish letter), `– — ‘ ’ “ ” „ • …`,
`← →`, `Ⅱ` (pause in the top bar) and the prototype's text symbols `▶ ▾ ● ✕ ✎`. Code points the
main face lacks come from the fallback face (DejaVu Sans): the symbols and the non-Polish Latin
Extended-A letters that Lato 1.x does not have (Polish ones are native). Digits are tabular
(`--tnum`) so changing numbers never shift.

## Faces and weights (decision)

The prototype is set in Segoe UI (Windows, not redistributable). Local OFL candidates were rendered
with the headless LVGL host next to crops of `reference/shots/view-sample.png` at the prototype
sizes (`specimen.sh` → `specimen_out/compare_*.png`; the reference crop is the top row, rows named
`*_pick` are what `build.sh` uses):

| group | sample | candidates |
|---|---|---|
| a | sample name 21 px / 500 | Lato +0, +0.012, +0.020 em; Montserrat Medium |
| b | knob labels 10 px / 600 | Lato +0.012, +0.022, +0.030; Montserrat Medium / Bold |
| c | panel titles 12 px / 600 | Lato +0.022, +0.030; Montserrat Medium |
| d | body 11 px / 400 | Lato +0, +0.004, +0.008; Montserrat Medium |
| e | logo 30 px / 650 | Lato +0.030, +0.045, +0.060; Montserrat Bold |
| f | BPM 14 px / 500 | Lato +0.004, +0.012; Montserrat Medium |
| g | wave readout 11 px Consolas | Share Tech Mono +0, +0.025; Lato tabular |

* **Lato** (humanist, x-height 0.507 em / cap 0.717 em, almost identical to Segoe UI's 0.50 / 0.70)
  is clearly closer than Montserrat, which is geometric, ~12 % wider and reads as a different
  typeface at every size. Lato 1.104 "Western+Polish" contains all Polish letters.
* Only Lato **Regular** exists locally, so Medium/SemiBold/Bold are synthesised: the outline is
  rasterised 6-16x supersampled, the coverage is dilated by a disc of radius `weight` (equivalent to
  thresholding the outline's distance field at `-weight`, keeps the anti-aliased edge), then
  area-downsampled to 4 bpp; the advance grows by `2*weight` like a real bold. Chosen per-side
  offsets: regular +0.004 em (Lato is a touch lighter than Segoe UI at 11-14 px), medium +0.012,
  semibold +0.022, bold +0.045. Note: Chrome renders the prototype's weight 500 with Segoe UI
  *Regular* (Segoe has no 500), so "medium" here is only slightly heavier than regular.
* **Mono** (`f_mono12`, wave time readout): Share Tech Mono +0.025 em with `--track -0.05` to keep
  its 0.54 em pitch (Consolas is heavier than the thin Share Tech).

## Sizes

theme.h names every font `f<weight><px>`; the sizes are the prototype's px x ~1.2 where the boxes
allow (1280x720 on a 5" panel is ~294 ppi). `build.sh` lists them; changing a size means adding a
`gen` line and a pointer in `ui/theme.h`/`theme.cpp`.

## Swapping in Inter (or any other face) later

```
TEXT_TTF=tools/fonts/Inter-Regular.ttf MEDIUM_TTF=tools/fonts/Inter-Medium.ttf \
SEMI_TTF=tools/fonts/Inter-SemiBold.ttf BOLD_TTF=tools/fonts/Inter-Bold.ttf \
W_REGULAR=0 W_MEDIUM=0 W_SEMI=0 W_BOLD=0 bash tools/fontgen/build.sh
```

With only `TEXT_TTF` changed, the synthetic weights are applied to the new face (re-check the
offsets with `specimen.sh`). Nothing else in the firmware changes: symbols stay `orbit_font_*`.

## fontgen options

```
--ttf F --size PX --name SYM --out FILE   required
--fallback F    face for missing code points     --weight EM   synthetic emboldening per side
--track EM      extra advance per glyph          --gamma G     alpha^G (G < 1 darkens AA edges)
--xh-snap       x-height on a whole pixel        --tnum        tabular digits
--kern          kerning pairs (|kern| >= 0.3 px) --probe       coverage + metrics only
```

## Licences

`tools/fonts/`: Lato (OFL 1.1, `OFL-Lato.txt`), Montserrat (OFL 1.1, `OFL-Montserrat.txt`, only used
for the comparison), Share Tech Mono (OFL 1.1, `OFL-ShareTechMono.txt`), DejaVu Sans (Bitstream Vera
licence, `LICENSE-DejaVu.txt`). The generated bitmap fonts are derived works distributed under the
same licences; they are named `orbit_font_*`, not with the reserved font names.
