# ORBIT DRUM in M5Burner

The file: `release/ORBIT_DRUM_<version>_M5Burner.bin` (made by `python -I tools/make_release.py`). It is the whole
flash image from address **0x0**: the bootloader (0x2000), the partition table (0x8000) and ORBIT (0x10000). Flashing
it replaces whatever was on the Tab5 (a launcher too): ORBIT then runs on its own. Chip: ESP32-P4, 16 MB flash.
Erase the flash first (M5Burner: Erase, or `esptool erase_flash`): settings of the app that was there before (its
NVS) would otherwise stay in the way.

## Description for M5Burner (English)

**ORBIT DRUM 0.4.9 — drum machine, sampler, acid bass and synth for the M5Stack Tab5**

Firmware written from scratch (C++, ESP-IDF, LVGL) for the Tab5's 5" 1280×720 touch screen. A demo track, "ORBIT
DRIVE" (drums, acid bass and synth), opens at start: press PLAY, switch scenes A–D, or turn on FOLLOW to hear the
whole song. Erase the flash before burning; ORBIT runs on its own (no launcher).

Features
- 16 multi-touch pads, built-in 808 / 909 style kits (no card needed), your own WAV samples from microSD
- Sample editor: start / end, slices, envelope, filter, pitch, LFO, 4 layers per pad
- Step sequencer up to 64 steps (velocity, probability, ratchet, swing), scenes A–D and a 16-slot song
- ACID BASS: a 303-style mono voice with accent and slide, a step editor and a live two-octave keyboard
- SYNTH: a 4-voice virtual-analog synth, chords in every scene, 48 knobs, 8 patches, a live keyboard (chords with
  several fingers)
- Mixer with 8 buses, reverb, delay, LO-FI / WIDE / PUMP / SPACE macros, limiter
- Projects on microSD with autosave; USB-MIDI host (notes, clock, MIDI learn)
- Bluetooth audio (A2DP headphones and speakers) through the Aurora Player Bluetooth module (M5Stamp Pico)
- 6 color palettes and a custom accent, a larger interface option

Tab5 Keyboard (the M5Stack keyboard for the Tab5; plug it in any time)
- Space play / stop · Shift+Space continue · F9 record · Tab / Shift+Tab next / previous view
- Up / Down scene · Enter play the scene · Left / Right pad · F1–F8 mute pads 1–8 · 0 mute the pad
- Ctrl+Z undo · Ctrl+S save · Ctrl+Shift+S save as · Ctrl+O open · Ctrl+N new · Ctrl+, settings
- F11 full screen · Esc close / back · Shift+F1 guide · Ctrl+Alt+B browser · Ctrl+Alt+M mixer
- A W S E D F T G Y H U J K O L P ; ' play notes: the pads, or the bass / the synth in their views (keys held
  together play a chord in SYNTH) · Z / X octave · C / V velocity · M notes on / off

GitHub: (link coming soon)

## Opis po polsku (dla autora)

**ORBIT DRUM 0.4.9 — automat perkusyjny, sampler, bas acid i syntezator na M5Stack Tab5**

Firmware napisane od zera (C++, ESP-IDF, LVGL) na dotykowy ekran Tab5 5" 1280×720. Przy starcie otwiera się utwór
demo „ORBIT DRIVE” (perkusja, bas acid i syntezator): PLAY gra, sceny A–D przełączają części, FOLLOW gra całą
piosenkę. Przed wgraniem wyczyść pamięć (Erase); ORBIT działa sam, bez launchera.

Funkcje
- 16 padów wielodotykowych, wbudowane zestawy w stylu 808 / 909 (bez karty), własne sample WAV z karty microSD
- Edytor sampli: początek / koniec, cięcie na plasterki, obwiednia, filtr, wysokość, LFO, 4 warstwy na pad
- Sekwencer do 64 kroków (głośność, prawdopodobieństwo, ratchet, swing), sceny A–D i piosenka z 16 pól
- ACID BASS: jednogłosowy bas w stylu 303 z akcentem i slide, edytor kroków i klawiatura na żywo (dwie oktawy)
- SYNTH: 4-głosowy syntezator analogowy (wirtualny), akordy w każdej scenie, 48 pokręteł, 8 brzmień, klawiatura na
  żywo (akordy kilkoma palcami)
- Mikser z 8 kanałami, pogłos, echo, makra LO-FI / WIDE / PUMP / SPACE, limiter
- Projekty na karcie microSD z autozapisem; host USB-MIDI (nuty, zegar, MIDI learn)
- Dźwięk przez Bluetooth (słuchawki i głośniki A2DP) przez moduł Bluetooth z Aurora Player (M5Stamp Pico)
- 6 palet kolorów i własny kolor akcentu, opcja większego interfejsu

Klawiatura Tab5 (klawiatura M5Stack do Tab5; można podłączyć w każdej chwili)
- Spacja start / stop · Shift+Spacja wznów · F9 nagrywanie · Tab / Shift+Tab następny / poprzedni widok
- Góra / Dół scena · Enter graj scenę · Lewo / Prawo pad · F1–F8 wycisz pady 1–8 · 0 wycisz pad
- Ctrl+Z cofnij · Ctrl+S zapisz · Ctrl+Shift+S zapisz jako · Ctrl+O otwórz · Ctrl+N nowy · Ctrl+, ustawienia
- F11 pełny ekran · Esc zamknij / wróć · Shift+F1 przewodnik · Ctrl+Alt+B przeglądarka · Ctrl+Alt+M mikser
- A W S E D F T G Y H U J K O L P ; ' grają nuty: pady albo bas / syntezator w ich widokach (w SYNTH klawisze
  trzymane razem grają akord) · Z / X oktawa · C / V głośność · M nuty wł. / wył.

GitHub: (odnośnik wkrótce)
