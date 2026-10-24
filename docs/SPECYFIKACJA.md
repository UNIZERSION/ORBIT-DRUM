# ORBIT DRUM — specyfikacja projektu (Tab5, ESP-IDF 5.5.2 + LVGL 9.3)

Drum machine / sampler na M5Stack Tab5 zbudowany od zera na podstawie prototypu
`reference/TAB5_DAW_ORBIT_LIGHT.html` (wariant biały). Wygląd, układ, kolory, czasy animacji i
zachowania kontrolek pochodzą z prototypu; to, co w prototypie było symulacją, działa naprawdę.

Materiały referencyjne:
- `reference/TAB5_DAW_ORBIT_LIGHT.html` — prototyp (CSS = źródło kolorów i geometrii).
- `reference/geometry.json` — zmierzone w Chrome pozycje, rozmiary, czcionki i kolory **każdego**
  widocznego elementu w 20 stanach (4 widoki, 5 zakładek edytora, 7 zakładek ustawień, 3 okna,
  odtwarzanie). Format: `[{state, elements:[{tag,x,y,w,h,cls,text,data,css:{...}}]}]`.
- `reference/shots/<stan>.png` — zrzuty 1280×720 tych stanów (porównanie wizualne).
- `reference/ORBIT_LIGHT_GRAFIKA_ZRODLOWA.png` — oryginalna grafika (moodboard).
- `docs/BADANIA_DAW_SYNTEZATORY.md` — rozpoznanie technologii, DSP, funkcji, 808/909.

## 1. Architektura

```
firmware/                      projekt ESP-IDF (idf.py build)
  main/app_main.cpp            start: hal::boardInit -> sd -> settings -> App::init -> displayStart -> ui::build -> audioStart
  main/core/                   przenośny C++17 (bez IDF i LVGL) — kompiluje się też na PC
    config.h params.h/.cpp project.h/.cpp sample.h/.cpp spsc.h
    engine.h/.cpp (+ voice.*, dsp/*.h, fx.*)  seq.h/.cpp
    sampleio.h (wav.cpp, sample_ops.cpp, slice.cpp)  library.h (library.cpp, persist.cpp)
    kits.h (+ kits_808.c, kits_909.c generowane)  settings.h/.cpp  platform.h
  main/hal/                    tylko Tab5: hal.h, board, display (bez rozrywania), touch (multi-touch),
                               audio (ES8388), storage (SD, exFAT), settings (NVS), midi (USB host), platform_idf.cpp
  main/ui/                     LVGL 9.3 (kompiluje się też na PC): theme, widgets, app, ui, scr_*, view_*, dlg_*, fonts/, icons/
host/                          PC: hal_host.cpp, platform_host.cpp, render (zrzuty PNG), testy
tools/                         kitgen (syntezа 808/909 -> WAV + C), fontgen, icongen, ref_capture.py, make_release.py
sdcard/ORBIT/                  zawartość karty SD (KITS, SAMPLES, USER, PROJECTS)
```

### Wątki (ESP32-P4, 2 rdzenie HP)
| Zadanie | Rdzeń | Priorytet | Rola |
|---|---|---|---|
| audio | 1 | najwyższy aplikacyjny (configMAX_PRIORITIES-2) | `Engine::processI16` blokami (domyślnie 128 ramek) -> I2S/ES8388 przez DMA |
| touch | 0 | 6 | odczyt GT911/ST7123 (do 5 punktów) ~100-200 Hz, natychmiastowe uderzenia padów (`Producer::Touch`), punkt 0 dla LVGL |
| LVGL | 0 | 4 | UI, App, odświeżanie widoków (16 ms) |
| LVGL draw units | 0/1 | wg esp_lvgl_port | rysowanie programowe |
| loader | 0 | 2 | SD: WAV, skan biblioteki, zapis projektu, operacje na samplach |
| USB MIDI | 0 | 5 | wejście nut/zegara (`Producer::Midi`) |

Zasady czasu rzeczywistego w `Engine::process`: brak alokacji, blokad, I/O, logów; komunikacja tylko
przez kolejki SPSC (osobna na producenta), kolejkę zdarzeń do UI i atomową `Telemetry`.
Projekt (`Project`) ma jednego pisarza (UI); silnik czyta go bezpośrednio (szczegóły w `project.h`).
Sample zmieniają się tylko komendą `SetSample`; stary sampel jest „emerytowany” po wygaśnięciu głosów.

### Audio
- 48 kHz (lub 44,1 kHz w ustawieniach), stereo, int16 do kodeka, wewnętrznie float32.
- Blok 128 ramek (2,7 ms @48k); opcje 64/128/256. Docelowe opóźnienie wyjścia < 10 ms.
- 32 głosy, interpolacja 4-punktowa Hermite, obwiednia AHDSR, filtr SVF (TPT, Cytomic) LP/BP/HP z
  rezonansem i drive, obwiednia filtra i wysokości, LFO (5 kształtów, 4 cele, sync), crush/downsample
  na pad, warstwy (Single/Velocity/RoundRobin/Random/Stack), choke, poly na pad, wygaszanie kradzionych
  głosów (bez trzasków), wygładzanie parametrów.
- 8 szyn miksera (poziom, pan, mute, solo, pomiar) + szyna FX (delay synchronizowany + pogłos) +
  master: PUMP (ducking od wybranego padu), LO-FI, WIDE (M/S), poziom master, limiter z look-ahead,
  metronom. Pomiary szczytowe do UI.

### Sekwencer (w wątku audio, dokładny co do próbki)
Sceny A–D (= patterny) po 16 torów (pady) × do 64 kroków; krok: on, velocity, probability,
ratchet 1–4, nudge ±50 %, pitch ±24, warunek (1:2, 2:2, 1:4…4:4, FILL, !FILL, FIRST). Długość sceny
16/32/48/64, długość toru (polimetria), podział 1/8, 1/16, 1/32, 1/8T, 1/16T, swing 50–75 %.
Przełączanie scen kwantowane (takt / ćwierćnuta / ósemka / natychmiast). Tryb piosenki (FOLLOW):
16 slotów, każdy = jedno przejście sceny. Nagrywanie na żywo z kwantyzacją, metronom, tap tempo.

## 2. Ekran (1280×720) — funkcje elementów szablonu

Geometria paneli (px): top 0–62; PADS 14,73 288×331; BROWSER 14,413 288×294; praca 311,73 746×486;
makra 1066,73 200×486; SCENES 311,568 955×139. Rozmiary tekstu ~1,2× prototypu tam, gdzie mieści się
pudełko (ekran 5" ma ~294 ppi). Każdy mały element ma niewidzialny powiększony obszar dotyku.

### Pasek górny
- Logo **TAB5** + „SAMPLER / SEQUENCER”.
- Nawigacja: SAMPLE, SEQUENCE, MIXER, BROWSER oraz **FX** (nowy, ten sam styl).
- Folder → PROJECT MANAGER. Zębatka → SETTINGS. „120 BPM / 4 / 4” — tempo na żywo.
- ▶/Ⅱ start/pauza (też spacja na PC), ● REC (uzbrojenie nagrywania), ? → przewodnik.
- Chip **BT** (za podpisem logo, tylko przy włączonym Bluetooth): akcent = połączono, szary = czeka na
  urządzenie; dotknięcie → SETTINGS / Bluetooth.

### PADS (lewy górny)
- 16 padów A1–D4: indeks, nazwa (z sampla), stan wybrany (pomarańczowy gradient + biały znacznik),
  błysk przy uderzeniu (sekwencer i dotyk, `pad-hit` 280 ms), przyciemnienie + „M” gdy wyciszony, „S” solo.
- **Multi-touch**: dotknięcie gra od razu (na wciśnięciu, nie po puszczeniu), kilka palców naraz.
  Velocity z pozycji pionowej w padzie (góra = głośniej) albo stała (Ustawienia > Touch).
  Dotknięcie wybiera też pad do edycji.
- MUTE / SOLO: przełączniki trybu — dotknięcie padu przełącza jego wyciszenie / solo.
- Podczas REC + PLAY uderzenia zapisują się do sceny (kwantyzacja do kroku).

### BROWSER (lewy dolny)
Kategorie: Kits, All Samples, Drums, Bass, Synth, Vocals, FX, User. Dotknięcie wybiera kategorię i
otwiera widok BROWSER; lupa otwiera BROWSER z polem wyszukiwania.

### Widok SAMPLE (edytor)
- Nagłówek: nazwa sampla wybranej warstwy + ✎ (SAMPLE DETAILS); LAYER 1–4 (wybór warstwy; długie
  przytrzymanie: wyczyść warstwę, tryb warstw).
- Fala: prawdziwy przebieg (min/max), obszar START–END (cieniowanie poza nim, pomarańczowe znaczniki
  z uchwytami do przeciągania, podwójne dotknięcie = przyciągnięcie do przejścia przez zero), znaczniki
  plastrów (SLICE), krzywa obwiedni (ENVELOPE), playhead z rzeczywistej pozycji głosu, odczyt czasu
  „0.000s — 0.420s  44 100 Hz”.
- Zakładki i pokrętła (4 | 2):
  - TRIM: ATTACK, DECAY, SUSTAIN, RELEASE | CUTOFF, RESONANCE (jak w szablonie; START/END na fali)
  - SLICE: TUNE, START, END, GAIN | przyciski AUTO ▾ (czułość LOW/MED/HIGH), GRID ▾ (2/4/8/16), TO PADS;
    plaster wybiera się dotknięciem fali
  - ENVELOPE: ATTACK, HOLD, DECAY, SUSTAIN | RELEASE, VELOCITY
  - FILTER: CUTOFF, RESONANCE, DRIVE, TYPE | ENV AMT, ENV DECAY
  - PITCH: TUNE, FINE, P.ENV, P.DECAY | BITS, DOWNSAMPLE
  - LFO: RATE, DEPTH, SHAPE, TARGET | SYNC, FADE
- Lewa kolumna: trzy narzędzia (fala → TRIM, krzywa → ENVELOPE, ukośnik → SLICE), „ONE SHOT ▾”
  (ONE SHOT / GATE / LOOP + REVERSE), napis „TRIM · EDIT MODE”.

### Widok SEQUENCE
- „PATTERN EDITOR / SCENE A”, „DRUM MACHINE · 16 STEPS”, RANDOM, CLEAR, PLAY/PAUSE oraz FILL
  (przytrzymanie = warunek FILL).
- Dodane w tym samym stylu: wybór taktu 1–4 (gdy scena > 16 kroków) i banku torów (A1–B4 / C1–D4).
- Siatka 8 torów × 16 kroków: dotknięcie przełącza, przeciąganie maluje, ramka bieżącego kroku,
  kroki poza długością toru przygaszone. Dotknięcie nazwy toru wybiera pad.
- Tryby pasa (SELECTED TRACK): VELOCITY, PROBABILITY oraz (menu) RATCHET, NUDGE, PITCH, COND —
  siatka pokazuje wtedy słupki wartości wybranego toru, przeciąganie w pionie ustawia wartość.
- PATTERN SETTINGS: „1/16 ▾” (podział), „SWING 54%” (przeciąganie / lista), „16 STEPS” (długość).

### Widok MIXER
8 kanałów (szyn) + MASTER: prawdziwe mierniki, suwaki (dB), M/S; MASTER: L (limiter) i C (metronom). Pady są przypisane do szyn (SAMPLE DETAILS > OUTPUT).

### Widok BROWSER
Wyszukiwarka (klawiatura ekranowa z polskimi literami), wirtualizowana lista plików z SD
(nazwa, czas), podgląd: nazwa, fala, „44.1 kHz • 16-bit WAV • 0.42 s”, ▶ PREVIEW, LOAD TO PAD,
OPEN IN SAMPLER; dla zestawów: LOAD KIT (16 sampli na pady), wbudowane TR-808 / TR-909.

### Widok FX (nowy)
Karty w stylu `.l-sec-card`: REVERB (SIZE, DECAY, DAMP, PRE-DLY, LEVEL), DELAY (TIME synchro,
FEEDBACK, TONE, LEVEL, PING-PONG), MACROS: LO-FI / WIDE / PUMP / SPACE (przełączniki jak `.tab`
+ pokrętła ilości), LIMITER (przełącznik + wskaźnik redukcji), MASTER (pokrętło).

### Panel makr (prawy)
VOLUME, FX SEND, PAN (duże białe pokrętła) dla wybranego padu; transport ◀◀ (stop/powrót), ▶/Ⅱ, ●;
TAP, –, BPM, + (przytrzymanie przyspiesza).

### SCENES (dół) — scena i aranżacja
A B C D: wybór sceny (podczas grania kolejkowana — migająca do zmiany), „+”: kopiuj scenę / wyczyść.
Oś 16 slotów piosenki: wiersze A, B, C, D (komórka = slot gra tę scenę; ponowne dotknięcie czyści)
i FX (rzut FX w tym slocie). Grający slot pulsuje, znacznik pozycji nad osią. FOLLOW = tryb piosenki.
GRID = podział sceny, LENGTH = długość sceny (1/2/3/4 takty).

### Okna
- SETTINGS (General, Audio, MIDI, Bluetooth, Display, Touch, Storage, About) — wszystko działa, APPLY zapisuje
  do NVS; zakładka Bluetooth działa od razu (bez APPLY). Display → Base color (6 palet: LIGHT = prototyp,
  WARM, SILVER, SLATE, DARK, BLACK; pozostałe wyprowadzone z LIGHT w `ui/theme.cpp`, kontrast tekstu
  sprawdza `test_widgets`) i Accent color (8 gotowych + CUSTOM: odcień 0–359°, nasycenie 20–100 %);
  nowe kolory po szybkim restarcie (projekt zapisuje się i otwiera ponownie).
- PROJECT MANAGER — projekty z SD, otwieranie, NEW PROJECT, SAVE, EXPORT JSON.
- UI GUIDE — przewodnik po prawdziwych funkcjach.
- SAMPLE DETAILS — informacje o pliku, fala, pokrętła FADE IN/OUT, GAIN, LOOP; CHOKE, OUTPUT, POLY,
  LAYER MODE; akcje TRIM, SLICE, NORMALIZE, REVERSE, DUPLICATE + FADE, TRIM SILENCE, MONO, PRINT CRUSH,
  UNDO, SAVE WAV.

### Animacje (z prototypu)
ripple 560 ms; `:active` skala .963 120 ms; wejście widoku 280 ms; okno 250 ms + scrim 200 ms;
toast 240 ms / 2,6 s; pad-hit 280 ms; pulsowanie grającego klipu 1,1 s; playhead fali.

### MIDI (USB-A, host)
Nuty na kanale z ustawień (domyślnie 10, mapa GM/909: BD 36, RS 37, SD 38, CP 39, LT 41/43, CH 42,
MT 45/47, OH 46, HT 48/50, CR 49, RD 51; pozostałe pady od 52 w górę) grają pady; Start/Stop/Continue
sterują transportem; zegar 24 PPQN (wejście: śledzenie tempa, wyjście: opcjonalnie); MIDI Learn:
następny CC przypisuje się do ostatnio ruszonego pokrętła.

### Bluetooth (moduł Pico z Aurory)
ESP32-C6 w Tab5 nie ma klasycznego Bluetooth, więc słuchawki i głośniki A2DP obsługuje M5Stamp Pico z
firmware „Pico Bluetooth bridge” 0.9.x z Aurora Player (I2S: G5/G18/G19 → Pico G26/G25/G22, Tab5 taktuje;
UART 115200: G6 → G32, także budzenie impulsem 120 ms, G46 ← G33). Własna implementacja
udokumentowanego protokołu tekstowego (READY / STATE / LIST / SELECTMAC / SCAN / VOLUME / STREAM / STOP /
REMOTE …), kod Aurory (GPL-3.0) nie jest kopiowany.
- SETTINGS / Bluetooth: włącznik, stan modułu (wersja, połączenie, komunikat), „Also speaker / jack”,
  głośność modułu, AirPods (bateria L / R / etui, tryb OFF / ANC / TRANSPARENCY / ADAPTIVE), a pod nimi
  SCAN (20 s, „SCANNING…”), RECONNECT, TEST i lista do 32 urządzeń (8 zapamiętanych w Pico + wyniki
  skanowania; nazwa, adres, sygnał z RSSI, SAVED / SELECTED / CONNECTED; dotknięcie = połącz; przy
  wyłączonym Bluetooth pusta). Sekcja dłuższa niż okno pokazuje pasek przewijania. Ustawienia `bt_enabled`,
  `bt_local` (domyślnie 0: głośnik grałby ok. 0,2 s przed słuchawkami), `bt_volume` zapisują się od razu
  (głośność ~1 s po ostatniej zmianie); leżą za 36 bajtami plików ustawień 0.1.0, więc te dostają wartości
  domyślne.
- Włączenie przestawia silnik na 44,1 kHz (stop audio → nowy silnik → start, jak zmiana częstotliwości
  w Audio); wyłączenie przywraca poprzednią. Wybór częstotliwości w Audio przy włączonym Bluetooth jest
  zapamiętywany i obowiązuje po wyłączeniu.
- Przy starcie z zapisanym `bt_enabled` most rusza w `App::tick()`, gdy działa już interfejs, a start audio
  w `app_main` się zakończył (strumień działa albo `App::audioBootDone()` po nieudanym starcie) — start
  aplikacji się nie wydłuża i zmiana częstotliwości nigdy nie ściga się z tym startem.
- BACK TO LAUNCHER przy włączonym Bluetooth najpierw wysyła do Pico `STOP` + `SLEEP`.
- Przyciski słuchawek (AVRCP): PLAY → wznowienie od miejsca pauzy (jak przycisk play na ekranie),
  PAUSE → stop, NEXT / PREVIOUS → następna / poprzednia scena. Toasty „Bluetooth: połączono z …” /
  „Bluetooth: rozłączono” (to drugie nie przez 5 s po SCAN / wyborze urządzenia / RECONNECT, które celowo
  restartują Pico — ich podpowiedź zostaje na ekranie).
- Bluetooth dodaje ok. 0,15–0,25 s opóźnienia (SBC) — do gry na żywo głośnik lub słuchawki przewodowe.

## 3. Karta SD
`/sdcard/ORBIT/KITS/TR-808/*.wav`, `/sdcard/ORBIT/KITS/TR-909/*.wav` (nasza synteza),
`/sdcard/ORBIT/SAMPLES/<kategoria>/`, `/sdcard/ORBIT/USER/`, `/sdcard/ORBIT/PROJECTS/`.
Obsługa FAT32 i exFAT. Bez karty działają wbudowane kity.

## 4. Testy na PC
- `host/` buduje `core/` i `ui/` z LVGL zig c++: testy silnika (renderowanie offline do WAV,
  dokładność taktowania, brak NaN/clipu, wydajność), WAV/operacje, zapis/odczyt projektu, oraz zrzuty
  PNG wszystkich widoków do porównania z `reference/shots/`.
