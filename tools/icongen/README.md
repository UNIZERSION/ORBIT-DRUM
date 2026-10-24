# icongen — stroke icons for ORBIT DRUM

```
python -I tools/icongen/icongen.py [--preview tools/icongen/preview.png]
```

Python 3 stdlib only. Rasterises the prototype's `ico()` icons (`reference/TAB5_DAW_ORBIT_LIGHT.html`:
viewBox 24, `fill:none`, stroke 1.7, round caps and joins) into LVGL 9 **A8** image descriptors at
14, 16, 18, 20, 22 and 26 px → `firmware/main/ui/icons/orbit_icons.c` + `icons.h`.

* Supported SVG: `<path>` with M L H V C S A Z (absolute and relative, implicit repeats),
  `<rect rx>`, `<circle>`. Curves and arcs are flattened finely; a round-capped/joined stroke is the
  union of capsules around the segments, so coverage = distance to the nearest segment vs. half the
  stroke width, sampled 4x4 per pixel with a one-sub-sample linear edge ramp.
* Order of `ICONS` == `enum class orbit::ui::Icon` (checked by a `static_assert` in widgets.cpp).
  Pencil, Envelope (decay curve), Slice (slash), Rewind and Link are not in `ico()`; they are drawn
  in the same stroke language after `reference/ORBIT_LIGHT_GRAFIKA_ZRODLOWA.png`. Entries after the
  enum ones (`chev_down`) are kit-internal (`ORBIT_ICON_CHEV_DOWN`).
* A8 images are drawn with the `image_recolor` colour of the image object, so one bitmap serves every
  colour: `iconCreate(parent, Icon::Play, 18, col::ink)`.
