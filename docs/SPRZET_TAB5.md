# ORBIT DRUM — warstwa sprzętowa Tab5 (HAL), budowanie i self-test

Dotyczy: M5Stack Tab5 (ESP32-P4 rev v1.3, 360 MHz, 32 MB PSRAM, 16 MB flash, MIPI-DSI 720×1280
pracujący poziomo jako 1280×720, ES8388, GT911 albo ST7123/ST7121), ESP-IDF 5.5.2, LVGL 9.3,
BSP `espressif/m5stack_tab5` 1.3.2.

## 1. Pliki

| Plik | Zawartość |
|---|---|
| `firmware/main/hal/hal.h` | kontrakt (lead) + dodane: `audioSetTimingHook`, `audioXruns`, `setAudioIdle`, `displayReserveBuffers`, `displayReveal`, `diagnostics` / `diagnosticsReset` |
| `firmware/main/hal/hal_private.h` | wewnętrzne powiązania między plikami HAL (m.in. jedyny uchwyt ekspandera 0x43: `ioe1Open` / `ioe1`) |
| `firmware/main/hal/hal_logic.h` | przenośna logika HAL bez nagłówków IDF: księgowanie buforów prezentacji (`FrameBook`), polityka wyjścia audio (`outputFor`), `Debounce`, `OverloadGuard`, `dmaDescFor`; test PC `host/tests/test_hal_logic.cpp` |
| `firmware/main/hal/board.cpp` | I2C, ekspander 0x44 (ładowarka, USB 5 V, zasilanie C6), bateria INA226, `boardInfo`, NVS |
| `firmware/main/hal/display.cpp` | LVGL + prezentacja bez rozrywania (PPA, 3 bufory DPI, VSYNC), podświetlenie, limit FPS |
| `firmware/main/hal/touch.cpp` | task dotyku (200 Hz, do 5 punktów, id palców), `TouchHook`, wejście LVGL |
| `firmware/main/hal/audio.cpp` | I2S + ES8388, task audio, mono na głośnik, HP_DET/SPK_EN, xruns, task pomocniczy |
| `firmware/main/hal/storage.cpp` | karta SD (FAT32 + exFAT), `settings.bin` + kopia w NVS, `orbit_fs_info` |
| `firmware/main/hal/midi.cpp` | host USB-MIDI (USB-A) |
| `firmware/main/hal/bt_protocol.h` | przenośny klient protokołu mostka Bluetooth Pico (składanie linii, parser, maszyna stanów łącza), bez nagłówków IDF; test PC `host/tests/test_bt_protocol.cpp` |
| `firmware/main/hal/bt.cpp` | Bluetooth przez mostek Pico: task `orbit_bt`, UART2 (G6/G46, własny mini-sterownik), kanał I2S_NUM_0 do Pico, `hal::bt*` (sekcja 11) |
| `firmware/main/hal/platform_idf.cpp` | `core/platform.h` na ESP-IDF (pamięć, czas, log, worker) |
| `firmware/cmake/exfat.cmake` | lokalna kopia komponentu fatfs z `FF_FS_EXFAT 1` |
| `firmware/cmake/orbit_project.cmake` | wspólne poprawki builda (sdkconfig, esp_hosted, ostrzeżenie BSP o LFN) |
| `firmware/cmake/orbit_hal.cmake` | flagi komponentu `main` (`-O2`, `ORBIT_HOT`) |
| `firmware/sdkconfig.defaults`, `firmware/partitions.csv` | konfiguracja i tablica partycji |
| `firmware_selftest/` | osobny projekt: te same `hal/*.cpp` + ekran testowy (`main/selftest_main.cpp`) |

## 2. Budowanie (offline)

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf_build.ps1 -Project firmware_selftest -Log firmware_selftest\build.log
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf_build.ps1 -Project firmware -Log firmware\build.log
```

- `firmware/managed_components` + `firmware/dependencies.lock` to skopiowane komponenty. Lista zależności
  w `firmware/main/idf_component.yml` musi zostać **identyczna**, inaczej zmieni się `manifest_hash`
  i menedżer komponentów zechce sieci.
- `firmware_selftest/main/idf_component.yml` ma te same zależności, `dependencies.lock` jest kopiowany
  z firmware przy każdej konfiguracji, a `firmware_selftest/managed_components` to junction
  (`mklink /J`, na Linuksie symlink) do `firmware/managed_components`, tworzony automatycznie przez
  `firmware_selftest/CMakeLists.txt`. Nie usuwać go poleceniem kasującym zawartość katalogu
  (skasowałoby to komponenty firmware); usuwać sam link (`rmdir`).
- **Bez sieci**: menedżer komponentów przy każdej konfiguracji pyta rejestr o nowsze wersje i sprawdza
  wersje z locka. `cmake/orbit_project.cmake` ustawia `IDF_COMPONENT_CHECK_NEW_VERSION=0` i proxy
  `127.0.0.1:9` (odmowa połączenia) dla procesów menedżera — w logu pojawia się „Cannot establish a
  connection to the component registry. Skipping checks of dependency changes.” i build idzie dalej
  z lokalnych komponentów. Aktualizacja zależności: `-DORBIT_ALLOW_NETWORK=ON` albo zmienna
  środowiskowa `ORBIT_ALLOW_NETWORK=1`.
- **sdkconfig**: źródłem konfiguracji jest `sdkconfig.defaults`. Gdy plik się zmieni (albo jest nowszy od
  `sdkconfig`, którego dany katalog build jeszcze nie widział), `orbit_refresh_sdkconfig` zmienia nazwę
  starego `sdkconfig` na `sdkconfig.old` i IDF generuje nowy. Zmiany z `menuconfig` trzeba więc
  przenieść do `sdkconfig.defaults`. Self-test używa `firmware/sdkconfig.defaults` + własnego
  `firmware_selftest/sdkconfig.defaults` (większe fonty Montserrat).
- **esp_hosted**: jest w manifeście (wymóg identycznej listy), więc jest budowany i linkowany jako
  WHOLE_ARCHIVE. Jego konstruktor `port_esp_hosted_host_init.c` uruchamiałby SDIO do ESP32-C6 przed
  `app_main` i kończył się assertem — `orbit_project_fixups()` usuwa ten plik z builda. Wi-Fi nie jest
  używane, C6 pozostaje wyłączony (ekspander 0x44, P0 = 0).
- **exFAT**: IDF 5.5.2 ma `FF_FS_EXFAT 0` na sztywno. `cmake/exfat.cmake` kopiuje
  `$IDF_PATH/components/fatfs` do `build/orbit_components/fatfs`, ustawia `FF_FS_EXFAT 1` i dodaje kopię
  do `EXTRA_COMPONENT_DIRS` (komponent projektu wygrywa z komponentem IDF). IDF nie jest modyfikowany.
  Wymaga `CONFIG_FATFS_USE_LABEL=y` i LFN.
- `firmware/main/CMakeLists.txt` zbiera rekurencyjnie `core/`, `hal/`, `ui/` (`*.c`, `*.cpp`) oraz
  `app_main.cpp` (albo `app_main.c`, jeśli `.cpp` jeszcze nie ma). Po dodaniu pliku: `idf.py reconfigure`.
- Ostrzeżenia: IDF kompiluje z `-Wall -Werror=all` (np. `-Wclass-memaccess`, `-Wstringop-truncation`,
  `-Wmaybe-uninitialized` są błędami w GCC, a nie występują w zig/clang na PC; `memset` na typie
  z inicjalizatorami pól wymaga rzutowania na `void*`). C++ jest kompilowany jako gnu++2b, więc
  `orbit_hal.cmake` wyłącza `-Wdeprecated-enum-enum-conversion` (idiom LVGL `LV_PART_x | LV_STATE_y`).
- Log `idf_build.ps1` jest w UTF-16 (przekierowanie PowerShell) z dopiskiem `EXIT=n` w UTF-8.
- Wynik (22.10.2026): `firmware_selftest/build/orbit_selftest.bin` 1 103 536 B, bez ostrzeżeń w plikach
  HAL; `firmware` (jeszcze z zaślepką `app_main.c` leada) buduje się bez błędów i ostrzeżeń.

### ORBIT_HOT

`cmake/orbit_hal.cmake` definiuje `ORBIT_HOT=IRAM_ATTR` i dołącza `esp_attr.h` (`-include`) do każdego
pliku komponentu `main`. Kod w `core/` musi więc definiować pusty `ORBIT_HOT` tylko warunkowo:

```cpp
#ifndef ORBIT_HOT
#define ORBIT_HOT
#endif
```

Funkcja oznaczona `ORBIT_HOT` trafia do sekcji `.iram1.N` (RAM wewnętrzny), jak w IDF. Self-test oznacza
tak swoją funkcję `render` — sprawdzone w pliku `.map` (patrz sekcja 9).

## 3. Partycje i instalacja

`partitions.csv`: `nvs` 0x9000 (24 KB), `factory` 0x10000, **7 MB**. Obraz musi mieć < 7 MB (sprawdza
to build). Dwie drogi:

1. **Z launcherem Tab5** (normalnie): skopiować `build/orbit_drum.bin` (lub `orbit_selftest.bin`) na kartę
   SD i zainstalować z launchera do jego slotu 8 MB. Aplikacja widzi wtedy tablicę partycji launchera;
   NVS jest wspólny — używamy przestrzeni `orbit` (launcher: `l5`, `nvs.net80211`). Obraz ma
   `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` (zakres rewizji 1..199), więc launcher go przyjmie.
2. **Bez launchera**: `idf.py -p COM3 flash` zapisuje bootloader, tę tablicę i aplikację pod 0x10000 —
   **na urządzeniu z launcherem nadpisuje launcher**. Nie używać na Tab5 z launcherem.

## 4. Wyświetlacz (display.cpp)

- Panel natywnie 720×1280 (pionowo); LVGL widzi 1280×720 z `LV_DISPLAY_ROTATION_90`.
- Kolejność startu: zasilanie LCD (ekspander 0x43, do 3 prób, reset szyny I2C), `lvgl_port_init`
  (task LVGL: rdzeń 0, priorytet 4, stos 16 KB, tick 5 ms), sonda dotyku ST712x (FW 1 → ST7121, 965 Mb/s;
  inaczej 1000 Mb/s), priorytet QoS odczytu DW-GDMA (bez niego niebieskie mignięcia),
  `bsp_display_new` (oba warianty paneli wykrywa BSP), 3 bufory DPI, klient PPA SRM, callback VSYNC
  (`on_refresh_done`, IRAM).
- Bufory rysowania LVGL: 2 × 1280×32 RGB565 (2 × 80 KB) w wewnętrznym SRAM (DMA, wyrównanie 64 B),
  rezerwowane **jako pierwsza rzecz w `app_main`** (`displayReserveBuffers()`), zanim cokolwiek innego
  pofragmentuje stertę — rozmiar nie zależy więc od stanu sterty przy starcie (wcześniej 48/40 wierszy
  „jeśli się zmieści”, a 48 nie mieściło się nigdy). Gdy rezerwacja się nie uda: 1280×96 w PSRAM (log i
  pole `draw_sram` diagnostyki). Tryb PARTIAL. `app_main` loguje wolny SRAM (i największy blok DMA) po
  każdym etapie startu (`heap after board / App::init / display / UI / audio`).
- `flush_cb` kolejkuje kawałek do taska `orbit_present` (rdzeń 0, priorytet 5, stos 4 KB; tablice
  robocze są statyczne w `FrameBook`, nie na stosie), `flush_wait_cb` czeka na niego, więc PPA obraca
  kawałek N, gdy LVGL rysuje N+1. Ostatni kawałek odświeżenia publikuje ukryty bufor
  `esp_lcd_panel_draw_bitmap(panel, 0, 0, 720, 1, fb)` na własnym buforze panelu (bez kopii). Sterownik
  DPI wybiera bufor po adresie; zakres wierszy steruje tylko jego zapisem cache, a klatka już jest w
  pamięci (PPA pisze przez DMA, ścieżki CPU robią `esp_cache_msync` swoich wierszy). Pełny zakres
  oznaczałby zapis do 1,8 MB pod globalnym spinlockiem `esp_cache_msync`, na którym `i2s_channel_write`
  rdzenia audio kręci się z zamaskowanymi przerwaniami. **Nie** włączać
  `CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS` jako lekarstwa: wtedy `msync` z taska bierze mutex i
  `i2s_channel_write` spałby przez cały zapis (a unieważnień PPA to i tak nie obejmuje).
- 3 bufory (`FrameBook` w `hal_logic.h`): poprzednia klatka jest „wycofana” przez 2 zbocza VSYNC
  (wtedy nigdy nie jest pisana); obszary, które bufor przegapił, są kopiowane z najnowszej klatki.
  Między odświeżeniami task prezentacji naprawia je w tle (krok ≤ 96 Ki pikseli, kolejka sprawdzana po
  każdym kroku; bez pracy odpytuje co 10 ms), więc pierwsze małe odświeżenie po dużym (zamknięcie okna,
  zmiana widoku) zwykle nie czeka na kopię ~0,9 Mpx. Co zostanie, jest kopiowane przy wyborze bufora, z
  pominięciem tego, co bieżące odświeżenie i tak przerysuje (`inv_areas` LVGL 9.3 — prywatny nagłówek,
  wiąże kod z LVGL 9.3). Wybór bufora: zapisywalny z najmniejszą liczbą nieaktualnych pikseli.
- Brak VSYNC przez 150 ms: zamiast zawieszenia bierze się najstarszy bufor (najwyżej jedno rozerwanie).
- Błąd PPA: przejście na obrót i kopiowanie przez CPU (z `esp_cache_msync`), obraz dalej bez rozrywania.
- Podświetlenie zapala się dopiero z pierwszą klatką gotowego UI: `app_main` po zbudowaniu UI (pod
  blokadą LVGL) woła `displayReveal()`, a pierwsza klatka odświeżenia rozpoczętego po tym wywołaniu
  zapala podświetlenie, gdy jest na ekranie (2 VSYNC po publikacji), jasnością z `setBrightness()`
  (domyślnie 80 %). Bez `displayReveal()` podświetlenie zapala się 3 s po `displayStart` (awaria budowy
  UI nie zostawia ciemnego ekranu). Wcześniej zapalało się przy pierwszej opublikowanej klatce, czyli na
  pustym jasnym ekranie domyślnym LVGL, zanim UI powstało. Wygaszanie: tylko podświetlenie
  (`setBrightness(0)`).
- Budzenie taska LVGL (`lvgl_port_task_wake`) tylko dla unieważnień z innych tasków; własne unieważnienia
  taska LVGL (timery, wejście, odświeżanie) nie wywołują jądra — jego następny sen wynika z timera
  odświeżania. Początek odświeżania nie budzi go wcale.
- Zasilanie LCD i dotyku (ekspander 0x43: LCD_RST, potem TP_RST — kolejność BSP) włącza już `boardInit`,
  więc 500 ms stabilizacji dotyku przed sondą ST712x mija w czasie montowania SD i `App::init`;
  `probeLaneRate` czeka tylko resztę. Drugie 500 ms jest w prywatnym `bsp_get_board_version()` BSP
  (`bsp_display_new`) i bez nadpisania BSP nie da się go pominąć.
- `setFrameCap(30|60)`: okres timera odświeżania LVGL 33 / 16 ms.
- LVGL bez warstwy OS (`CONFIG_LV_OS_NONE=y`): zadania rysowania wykonują się w przypiętym tasku LVGL
  (rdzeń 0). Z `LV_OS_FREERTOS` LVGL 9.3 tworzy na każdą jednostkę rysującą wątek „swdraw” zwykłym
  `xTaskCreate` (bez rdzenia, 16 KB SRAM), który rysował na rdzeniu audio i dodawał dwa przełączenia
  wątków na każde zadanie rysowania. Blokada to mutex esp_lvgl_port (`lvLock`).
- Diagnostyka (`hal::diagnostics`): najdłuższy obrót kawałka, naprawa przed odświeżeniem i publikacja.
- `restart()`: handler zamknięcia wygasza podświetlenie i wysyła DISPOFF (bez szumu na panelu).
- `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y`: skanowanie nie staje podczas zapisu flash.

## 5. Dotyk (touch.cpp)

- Jeden task `orbit_touch` (rdzeń 0, priorytet 6) co 5 ms (200 Hz) czyta kontroler (jedyny kod, który
  z nim rozmawia). INT (GPIO23) nie jest używany — odpytywanie jest pewniejsze (doświadczenie launchera).
- Do 5 punktów. Współrzędne surowe są natywne (x 0..719, y 0..1279). Poziomo: `X = 1279 − y_raw`,
  `Y = x_raw` — dokładnie odwrotność mapowania obrazu (`nativeArea`: piksel (lx, ly) leży w (ly, 1279−lx))
  i to samo, co LVGL robi z punktem, który dostaje w `read_cb`.
- Id palców: GT911 podaje track id; sterownik ST712x go gubi, więc id są nadawane przez śledzenie
  najbliższego punktu (promień 160 px) i są stałe dla palca.
- `TouchHook` jest wołany z taska dotyku przy każdej próbce z palcem i raz z `count = 0` po puszczeniu
  wszystkich (puszczenie = brak punktu przez 25 ms; GT911 odświeża ~100 Hz i między próbkami zwraca 0).
- LVGL: `read_cb` zwraca tylko zapamiętany pierwszy palec (bez I2C w tasku LVGL); przy naciśnięciu /
  puszczeniu task dotyku budzi task LVGL (`lvgl_port_task_wake`). Gdy pierwszy palec się podniesie,
  LVGL dostaje „puszczone” do czasu podniesienia wszystkich.
- `touchRate()`: liczba próbek z palcem na sekundę (0, gdy nic nie dotyka).

## 6. Audio (audio.cpp)

- Nie używamy `bsp_audio_codec_speaker_init()` (mono, 6×240 ramek ≈ 30 ms). Własny start:
  I2S1 master, tylko TX, `dma_desc_num` = 4 dla bloku 64, 3 dla 128/256 (`dmaDescFor`: jeden bufor
  zapasu ≈ 1,33 ms przy bloku 64, dopóki czasy z urządzenia nie są zmierzone), `dma_frame_num = blok`,
  `auto_clear = true`, Philips 16 bit **stereo**, MCLK = 256·fs **z APLL** (APLL = 2·MCLK = 24,576 /
  22,5792 MHz, dzielnik całkowity), piny MCLK 30, BCLK 27, WS 29, DOUT 26; `i2s_channel_preload_data`
  zerami; ES8388 przez `esp_codec_dev` (dane: nasz uchwyt TX i `clk_src = APLL`, sterowanie: I2C BSP
  0x10), `esp_codec_dev_open(fs, 16 bit, 2 kanały)`.
- Dlaczego APLL: poniżej rewizji v3 domyślnym źródłem I2S jest XTAL 40 MHz (`I2S_LL_DEFAULT_CLK_SRC`), a
  40 / 12,288 = 3,255 daje dzielnik ułamkowy — okres MCLK skacze między 75 i 100 ns (~25 % jittera,
  ES8388 ma wyłączone DLL). `esp_codec_dev_open` przestawia zegar kanału (`set_fmt` → `set_drv_fs`) i bez
  `clk_src` w `audio_codec_i2s_cfg_t` wróciłby do XTAL. Self-test: w logu `STD: TX, sample_rate_hz: 48000,
  mclk_multiple: 256, clk_src: 5` (APLL) i brak ostrzeżenia „APLL is occupied”.
- Rejestry ES8388 0x30/0x31 (LOUT2/ROUT2) = 0x1E (0 dB) — sterownik zostawia −30 dB.
- Task `orbit_audio`: rdzeń 1, priorytet `configMAX_PRIORITIES − 2`. Najpierw sam tworzy, konfiguruje i
  włącza kanał I2S (`i2s_new_channel` … `i2s_channel_enable`), więc przerwanie GDMA kanału jest
  przydzielone na rdzeniu 1, z dala od wyświetlacza, USB, SD i dotyku na rdzeniu 0 (IPC odpada: stos
  taska IPC ma 1 KB). `audioStart` czeka na ten krok, otwiera kodek (I2C, rdzeń 0; disable / reconfig /
  enable kanału nie przenosi przerwania) i daje taskowi sygnał startu. Pętla: render bloku → (mono L=R
  dla głośnika) → `i2s_channel_write` (timeout 20 ms) → hook czasu (`render_us`, `block_us`).
- Przeciążenie: gdy render zajmuje ≥ 95 % okresu bloku, zapis nigdy nie blokuje, a task o najwyższym
  priorytecie nie oddaje rdzenia. Po 32 takich blokach z rzędu task śpi jeden tick (`OverloadGuard`).
  Sprawdzanie IDLE1 przez task watchdog jest wyłączone (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n`);
  zamiast tego pętla audio sama jest zapisana do watchdoga (`esp_task_wdt_add/reset`), więc zawieszona
  pętla nadal jest zgłaszana, a przeciążenie nie sypie zrzutami na konsolę.
- Xruns: callback `on_send_q_ovf` (IRAM) zwiększa licznik (`audioXruns()`) dopiero po pierwszym
  `i2s_channel_write` strumienia: wcześniej kolejka jest pełna zaległości startu (DMA kręci się, gdy
  kodek jest konfigurowany przez I2C) — to nie są niedobory. `audioStop` wyłącza liczenie przed
  zatrzymaniem. Licznik nigdy nie jest zerowany (App liczy przyrosty). `CONFIG_I2S_ISR_IRAM_SAFE=y`.
- Diagnostyka (`hal::diagnostics`): najdłuższy render, `i2s_channel_write` (normalnie ≈ okres bloku
  minus render: czekanie na wolny bufor), odstęp bloków (więcej niż (bufory − 1) bloków = niedobór),
  liczba przeciążonych bloków, liczba buforów DMA.
- Opóźnienie `audioLatencyMs()` = (bufory DMA + 1 blok renderu) / fs:

  | blok | bufory DMA | 48 kHz | 44,1 kHz |
  |---|---|---|---|
  | 64 | 4 | 6,7 ms | 7,3 ms |
  | 128 | 3 | 10,7 ms | 11,6 ms |
  | 256 | 3 | 21,3 ms | 23,2 ms |

- Wyjście: HP_DET = ekspander 0x43 P7, SPK_EN = 0x43 P1. Reset ekspandera przez sterownik BSP zostawia
  P7 jako „wyjście” Hi-Z z włączonym pull-down (walczy z obwodem detekcji gniazda), więc `ioe1Open`
  (board.cpp) ustawia P7 jako wejście bez podciągania i loguje surowy `IN_STA`. Odczyt 5 Hz w tasku
  `orbit_aux` (rdzeń 0, priorytet 2), filtr: 2 zgodne odczyty z rzędu; 1 = słuchawki
  (`kHpDetActiveHigh`). **Potwierdzone na Tab5 24.10.2026:** `IN_STA 0x90` z wtykiem, `0x10` bez (bit 4 to
  LCD_RST), tak samo przy każdym starcie, więc `kHpDetTrusted = true`: w `AudioOut::Auto` słuchawki →
  wzmacniacz wyłączony, stereo; brak → wzmacniacz włączony, mono (głośnik gra tylko LOUT1). Z tego samego
  odczytu (`headphonesInserted()`) korzysta pamięć głośności (głośnik / słuchawki).
- Błąd odczytu 0x43 (24.10.2026, ORBIT 0.3.0 z modułem Pico): kilka sekund po tym, jak Pico odpowiedział
  (faza łącza „awake”), każdy odczyt 0x43 kończył się błędem aż do resetu; dotyk działał dalej. Przyczyna
  nieznana. Od 0.3.1 `readHeadphones` przy błędzie: jedna linia z kodem błędu (linie sterownika
  `pi4ioe5v6408` / `io_expander` wyciszone na ten czas), odczyt co 1 s, po 2 s i potem co 10 s linia
  `I2C …: 0x43 …, 0x44 …, 0x55 touch …, 0x41 INA226 …` (ok / NACK / timeout) i `i2c_master_bus_reset`
  (najwyżej 6 razy). Ostatni stan HP_DET zostaje. Po powrocie odczytu: P7 znów wejściem, SPK_EN zapisany
  od nowa. `Speaker` / `Headphones` wymuszają tryb zawsze. SPK_EN jest zmieniany tylko
  przy działającym strumieniu (sterownik ekspandera 0x43 jest współdzielony ze startem wyświetlacza —
  `audioStart` po `displayStart`). Task `orbit_aux`, `applyOutputLocked` i `audioStop` używają tylko
  zapamiętanego uchwytu 0x43 (`detail::ioe1()`); nigdy nie wołają `bsp_io_expander_init()`, które po
  nieudanej próbie tworzyłoby i RESETOWAŁO układ od nowa (zgaszone LCD_EN / TP_RST).
- `audioSetVolume` = głośność kodeka (krzywa esp_codec_dev). `audioStop()/audioStart()` przy zmianie
  częstotliwości / bloku (kanał I2S i kodek są tworzone od nowa; wzmacniacz gaszony przed zatrzymaniem).

## 7. Karta SD i ustawienia (storage.cpp)

- SDMMC slot 0, 4 bity, 40 MHz (konfiguracja BSP), `/sdcard`, FAT32 + exFAT, LFN 255 UTF-8,
  `max_files = 12`, **nigdy nie formatuje**. 3 próby; po każdej nieudanej `bsp_sdcard_unmount()`
  (zwolnienie LDO4). `ESP_FAIL` = karta odpowiada, ale nie ma FAT32/exFAT (bez dalszych prób).
- `orbit_fs_info(path, total, free)` (extern "C", silny symbol) → `esp_vfs_fat_info` punktu montowania.
- Ustawienia: `/sdcard/ORBIT/settings.bin` = nagłówek {magia "ORBS", wersja 1, rozmiar, CRC-32} +
  bajty `orbit::Settings`. Zapis przez `settings.tmp` + rename. Odczyt: SD (`.bin`, potem `.tmp`) →
  NVS → wartości domyślne (`false`). Wartości spoza zakresów są poprawiane.
- Kopia w NVS (przestrzeń `orbit`, klucz `settings`) tylko gdy audio milczy: App woła
  `setAudioIdle(true/false)` (transport zatrzymany / gra). Gdy nie jest bezczynne, zapis NVS czeka
  i wykonuje go task `orbit_aux` przy najbliższej ciszy. NVS nigdy nie jest kasowany (wspólny z launcherem).

## 8. USB-MIDI (midi.cpp) i płyta (board.cpp)

- USB-A (HS OTG). Zasilanie 5 V portu: ekspander 0x44 P3 ustawiany rejestrami — **nie**
  `bsp_usb_host_start()`, bo jego `bsp_feature_enable(BSP_FEATURE_USB)` otwiera sterownik 0x44,
  który resetuje układ (gaśnie ładowanie i zasilanie C6). Biblioteka USB Host instalowana bezpośrednio,
  task biblioteki + task klienta (rdzeń 0, priorytet 5).
- Pierwsze urządzenie z interfejsem Audio/MIDIStreaming (klasa 1, podklasa 3) z endpointem bulk (lub
  interrupt) IN/OUT: claim, jeden transfer IN w kolejce, pakiety 4-bajtowe → `MidiHook` (z taska USB;
  kanałowe, system common, real-time; SysEx pomijany). `midiSend` kolejkuje do 64 pakietów na OUT.
  Odłączenie: anulowanie transferów, zwolnienie interfejsu, zamknięcie urządzenia.
  Bez urządzenia nic się nie dzieje. Huby i kilka urządzeń naraz: nieobsługiwane.
- `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=2048`: domyślne 256 B odrzuca w enumeracji (esp-usb 1.4.1
  `enum.c`, „Configuration descriptor larger than control transfer max length”) urządzenia z większym
  deskryptorem konfiguracji — wieloportowe interfejsy MIDI i instrumenty USB audio + MIDI (300–1000+ B).
  Koszt: ~2 KB wewnętrznego RAM DMA na urządzenie + bufor sterownika enumeracji.
- Ekspander 0x43 (LCD / dotyk, SPK_EN, HP_DET): otwierany raz w `boardInit` przez `ioe1Open` — do 3 prób
  `bsp_io_expander_init()` z `i2c_master_bus_reset` między nimi (szyna potrafi wisieć po włączeniu
  zasilania), w razie porażki jeszcze raz w `lcdPower`. Uchwyt jest zapamiętany w `detail::ioe1()`.
- Płyta: ładowanie dozwolone (0x44 P7 = 1, CHG_STAT wejście), C6 wyłączony (0x44 P0 = 0), bateria
  z INA226 0x41 (2S Li-ion 6,4–8,3 V liniowo; poza 5–9 V = brak pakietu, −1), `charging()` = prąd do
  pakietu > 20 mA, `boardInfo()` = "ESP32-P4 v1.3, ST7123, 32 MB PSRAM" (panel znany po `displayStart`).

## 9. Procedura self-testu

1. Zbudować `firmware_selftest` (polecenie w sekcji 2). Obraz: `firmware_selftest/build/orbit_selftest.bin`.
2. Zainstalować: plik `.bin` na kartę SD → launcher (zalecane), albo za zgodą użytkownika esptool do
   slotu aplikacji launchera. Nie używać `idf.py flash` na urządzeniu z launcherem.
3. Monitor szeregowy (USB-C, USB-Serial/JTAG) pokazuje start: board info, `IO expander 0x43: IN_STA …`,
   panel, tryb PPA/CPU i bufory rysowania (`1280x32 SRAM`), SD, audio (`… x 4 DMA buffers … MCLK from
   APLL` i linia esp_codec_dev `clk_src: 5`), co 5 s linię `fps … touch … xruns …`, linię maksimów
   (`max: audio render / write / period …, rotate / repair / publish …, HP_DET IN_STA …`, zerowane co
   5 s) i wolny stos tasków HAL (`stack free (B): orbit_present … orbit_touch … orbit_aux …`).
4. Na ekranie sprawdzić:
   - **Obraz**: tytuł „ORBIT SELFTEST” w lewym górnym rogu (orientacja), paski kolorów w kolejności
     biały, żółty, cyjan, zielony, magenta, czerwony, niebieski, czarny; gradient bez pasów i rozerwań.
     Podświetlenie zapala się dopiero z gotowym obrazem (bez śmieci).
   - **ANIM**: przesuwający się kwadrat — płynnie, bez rozrywania; `DISPLAY … fps` ok. 55–60 (panel
     ST7123 ~57,8 Hz, ILI9881C ~48 Hz). **60 FPS → 30 FPS** zmienia limit.
   - **Dotyk**: kółka z numerami pod każdym palcem (do 5), id stałe podczas ruchu, kółko dokładnie pod
     palcem we wszystkich rogach (mapowanie). `TOUCH … Hz` ok. 200 przy przytrzymanym palcu
     (GT911 może pokazać mniej).
   - **PAD**: dotknięcie pomarańczowego kwadratu od razu daje klik (ścieżka dotyk → audio bez LVGL).
   - **Audio**: TONE 440 (sinus), CLICK (metronom 120 BPM, akcent co 4), `render … us`, `block … us`
     (≈ 2667 µs przy 128/48k), `xruns` = 0 zaraz po starcie i po każdej zmianie BUF / kHz (zaległości
     startu nie są liczone) i nie rośnie podczas animacji i dotyku. `write max` ≈ okres bloku,
     `period max` poniżej (bufory − 1) × okres bloku (przy 64/48k: < 4 ms). Gdy `write max` wyraźnie
     przekracza okres bloku przy pełnych przerysowaniach (ANIM, przełączanie ekranów),
     `i2s_channel_write` czeka na spinlock cache — wtedy porównać z `rotate max` i rozważyć większe
     kawałki (bufory rysowania w PSRAM). BUF 64/128/256 i 48/44,1 kHz restartują strumień; latencja
     zgodna z tabelą w sekcji 6. Ton 440 Hz bez syczenia i zniekształceń (MCLK z APLL).
   - **Start**: podświetlenie zapala się od razu z gotowym ekranem testu (bez jasnego pustego ekranu).
   - **Słuchawki**: linia OUTPUT pokazuje `HP_DET raw 0x.., bit 7 = …`. Zanotować bit 7 bez wtyku i z
     wtykiem (kilka razy; wartość ma być stabilna). Z wtykiem 1 — polaryzacja zgodna z
     `kHpDetActiveHigh = true`; z wtykiem 0 — ustawić `false`. Gdy odczyt jest stabilny i poprawny,
     ustawić `kHpDetTrusted = true` w `audio.cpp`: wtedy OUT AUTO wycisza głośnik przy słuchawkach i gra
     stereo (do tego czasu AUTO = głośnik + mono, a słuchawki grają razem z głośnikiem). Jeśli bit 7
     „pływa” bez wtyku, obwód detekcji wymaga podciągania (P7 jest wejściem bez pull — zmienić w
     `ioe1Open`). OUT AUTO / SPEAKER / PHONES wymusza wyjście. VOL −/+ zmienia głośność kodeka.
   - **SD**: rozmiar, wolne miejsce i lista katalogu głównego (karta FAT32 i exFAT); przycisk SD
     ponawia montowanie po włożeniu karty.
   - **Bateria / pamięć**: procent i „charging” przy USB; wolny RAM wewnętrzny i PSRAM.
   - **MIDI**: podłączyć klawiaturę USB-MIDI do USB-A — nazwa urządzenia, licznik komunikatów,
     ostatni komunikat; Note On daje klik.
   - Log: `settings save ok, reload ok` (zapis `ORBIT/settings.bin` i NVS).

### Zawieszenie PPA (znalezione na urządzeniu 22.10.2026)

Pierwsze uruchomienie self-testu: obraz zamarzał po 2 klatkach (dotyk i audio działały). Diagnostyka
(`hal::detail::displayDebug`, puls w self-teście) pokazała zadanie prezentacji zablokowane na zawsze w
`ppa_do_scale_rotate_mirror` (obrót 90°, RGB565). To błąd ESP32-P4 / ESP-IDF
[#19096](https://github.com/espressif/esp-idf/issues/19096) (pokrewny #19023): SRM nigdy nie kończy,
gdy docelowy `out.block_offset_x % 4 == 2`, a bufor DPI jest skanowany; poprawka jest tylko na master.
Obejście w `display.cpp`:
- `roundArea()` na `LV_EVENT_INVALIDATE_AREA` zaokrągla każdy obszar logiczny do 4 wierszy (logiczne y =
  docelowe x obrotu); LVGL używa tego zdarzenia także do wysokości kawałków PARTIAL,
- kopie naprawcze (obrót 0°) są wyrównane do 4 kolumn,
- PPA działa nieblokująco z limitem 80 ms (`ppaRun`); po przekroczeniu obrót przejmuje CPU na stałe
  (licznik `timeouts` w `displayDebug`), więc UI nigdy już nie zamarza.
Wynik na Tab5 (ST7121): 0 przekroczeń, obrót kawałka 0,7–1,1 ms, audio 0 xrun.

### Czas startu (zmierzony na Tab5, 23.10.2026)

Profil startu jest w logu (`boot …`, `init …`, `ui …`, `panel …`). Od uruchomienia aplikacji przez
launcher do podświetlenia z gotowym interfejsem: **~1,95 s** (wcześniej 3,3 s; pierwsze uruchomienie z
pustą kartą dodatkowo tworzy foldery ORBIT, ~2,5 s jednorazowo).
- `hal::displayStart()` wraca od razu; panel (BSP: 500 ms czekania na dotyk + ~1,2 s komend
  inicjalizacji ST7121) startuje w zadaniu `orbit_panel`, a w tym czasie powstają App i UI. Odświeżanie
  LVGL jest wstrzymane do `displayReveal()`, więc pierwsza klatka nie blokuje budowy UI.
- Typ panelu (ST7121 / ST7123 / ILI9881C) jest zapamiętany w NVS (`orbit_hw/panel`): kolejne starty
  pomijają ~0,5 s sondowania; po inicjalizacji typ jest sprawdzany bez czekania.
- Przy starcie tworzony jest tylko widok widoczny; pozostałe 4 widoki powstają po pierwszej klatce, co
  ~100 ms (albo od razu przy pierwszym przełączeniu).
- Ścieżka krytyczna to teraz BSP panelu (1,72 s). Pozostałe 500 ms czekania w `bsp_get_board_version`
  da się usunąć tylko łatką komponentu BSP.
Launcher dokłada własne ~4 s startu i odliczanie `bootTime` (u Ciebie 5 s w `/sdcard/config.conf`,
zmiana w zakładce CFG launchera).

## 10. Do sprawdzenia na sprzęcie / ryzyka

- HP_DET (0x43 P7) potwierdzony 24.10.2026 (1 = słuchawki, wejście bez podciągania daje stały odczyt). Do
  sprawdzenia: które wyjście ES8388 idzie do gniazda (LOUT1/ROUT1 czy LOUT2/ROUT2) i skąd biorą się błędy
  odczytu 0x43 przy pracującym module Pico (sekcja 8; linie `I2C …` w logu 0.3.1).
- Przy bloku 128 opóźnienie wynosi ~10,7 ms (3 bufory DMA po jednym bloku, zgodnie z zadaniem); dla
  < 10 ms wybrać blok 64 (4 bufory, 6,7 ms). Czwarty bufor przy 64 to zapas do czasu pomiaru na
  urządzeniu (`write max`, `period max` w self-teście); jeśli najgorszy przypadek jest daleko od 1 ms,
  można wrócić do 3 (`dmaDescFor`).
- USB-MIDI zbudowane, ale niesprawdzone z urządzeniem; urządzenia Full-Speed za hubem nie zadziałają.
- Palety Base color sprawdzone tylko na zrzutach z PC: na panelu ocenić ciemne palety (SLATE, DARK,
  BLACK) przy niskiej jasności i czas startu z paletą inną niż LIGHT (tło piecze się na rdzeniu 1 od
  początku startu; gdy ustawienia z karty wybiorą inną paletę, wypiekanie zaczyna się od nowa — log
  `ui: background baked in … ms`).
- LVGL działa bez warstwy OS (brak wątku „swdraw”); gdyby kiedyś była potrzebna, tylko `LV_OS_CUSTOM`
  z wątkiem przypiętym do rdzenia 0.
- Kawałki 1280×32: każdy obrót PPA unieważnia w cache `720 × szerokość kawałka × 2` B okna wyjścia (do
  1,84 MB dla kawałka pełnej szerokości, ok. 23 razy na pełną klatkę) pod globalnym spinlockiem cache.
  Czas nieznany — mierzyć (`rotate max`); przy dużym koszcie porównać z większymi buforami w PSRAM.
- Prezentacja korzysta z prywatnych pól LVGL 9.3 (`inv_areas`, `inv_p`, `inv_area_joined`) — przy
  zmianie wersji LVGL sprawdzić `display.cpp`.
- Bluetooth (sekcja 11): cały tor jest niesprawdzony na sprzęcie — własny sterownik UART2 na warstwie LL
  (odbiór, impuls budzenia na G6), wspólny APLL dwóch kanałów I2S przy 44,1 kHz, zapas DMA mostka
  (`dropped_blocks` liczy bloki, które się nie zmieściły, i niedobory), wejście G46 (poza M5-Bus). Jeśli UART nie działa: `ORBIT_BT_IDF_UART=1` (sterownik IDF,
  ~18 KB flash więcej).

## 11. Bluetooth (mostek Pico z Aurory)

ESP32-C6 w Tab5 nie ma klasycznego Bluetooth (A2DP), więc zwykłe słuchawki i głośniki BT obsługuje
płytka pomocnicza odtwarzacza Aurora Player: **M5Stamp Pico** (ESP32-PICO-D4) z firmware „Pico Bluetooth
bridge” 0.9.x (formaty linii jak w 0.9.5). Pico wgrywa się według `aurora-player/pico-bridge/README.md`
(ORBIT go nie programuje). ORBIT ma własną implementację udokumentowanego protokołu tekstowego
(`bt_protocol.h`) — kod Aurory (GPL-3.0) nie jest kopiowany.

### Połączenie (7 przewodów, bez konwertera poziomów)

| # | Tab5 | złącze Tab5 | kierunek | Pico | sygnał |
|---|---|---|---|---|---|
| 1 | 5V | M5-Bus pin 28 | → | pad 5V | zasilanie Pico (nigdy do 3V3 ani do pinu sygnałowego) |
| 2 | GND | M5-Bus pin 1/3/5 | — | GND | wspólna masa |
| 3 | G5 | M5-Bus pin 11 | → | G26 | I2S BCK (zegar bitowy) |
| 4 | G18 | M5-Bus pin 7 | → | G25 | I2S WS / LRCK |
| 5 | G19 | M5-Bus pin 9 | → | G22 | I2S dane audio |
| 6 | G6 | M5-Bus pin 16 | → | G32 | UART Tab5 → Pico; impuls niski 120 ms budzi uśpione Pico |
| 7 | G46 | pady STAMP w środku Tab5 (nie M5-Bus) | ← | G33 | UART Pico → Tab5 |

Tab5 jest masterem I2S (16 bit stereo, Philips, 44,1 kHz, bez MCLK). Nie zamieniać przewodów UART
(G6 → G32, G33 → G46). Podczas programowania Pico odłączyć je od Tab5 (zasilanie tylko z jednego źródła).
BSP opisuje G19/G20 jako `BSP_USB_NEG/POS`, ale host USB ORBIT używa PHY High-Speed (osobne piny), więc
G19 jest wolny — tak jak w Aurorze.

### Protokół (UART2, 115200 8N1, linie ASCII zakończone LF)

| ORBIT wysyła | Pico odpowiada | kiedy |
|---|---|---|
| `USE` | — | co 3 s, gdy Pico czuwa (bez niego Pico zasypia po 15 s) |
| `STATUS` | `STATUS connection=… audio=… volume=… scan=… version=… cap_airpods=…` (+ `AIRPODS …`, `CODEC …`) | co 15 s; dodatkowo jako sonda, gdy Pico milczy ≥ 5 s |
| `LIST` | `DEVICE <n> <MAC> <rssi> <saved> <selected> <nazwa>` … `END_LIST` | po `READY`, po połączeniu, po `SCAN_DONE`, co 4 s w trakcie skanowania |
| `SCAN` | `RESTART scanning…`, `READY`, po 20 s `SCAN_DONE` | `btScan()` |
| `SELECTMAC <MAC>` | `SELECTED <nazwa>; restarting to connect`, `READY` | `btSelect()` |
| `RECONNECT` | `RESTART reconnecting…`, `READY` | `btReconnect()` |
| `TEST` | `TEST START: LEFT / silence / RIGHT` | `btTest()` (3 s: lewy, cisza, prawy) |
| `VOLUME <0-100>` | `VOLUME <n>` | `btSetVolume()`; ponownie po każdym `READY` |
| `AIRPODS MODE <1-4>` | `AIRPODS_PENDING …` / `AIRPODS_ERROR …` | `btAirpodsMode()` (tylko gdy `BtStatus::airpods`) |
| `STREAM <token>` | `STREAM_ACK <token>` | trasa włączona + słuchawki połączone; nowy token przy każdym starcie |
| `STOP` | `SILENT` | trasa wyłączona, rozłączenie, wyłączenie BT |
| `SLEEP` | `STANDBY` | `btEnable(false)`, także przed powrotem do launchera |

Bez pytania Pico wysyła: `READY version=0.9.5 cap_airpods=1` (każdy start), `STATE connection=<n>
audio=<2|0>` (2 = połączone / strumień gra), `REMOTE PLAY|PAUSE|NEXT|PREVIOUS` (przyciski słuchawek →
`btPopRemote()`), `STREAM_FAULT <token> INPUT_STALLED` (1,5 s bez dźwięku z Tab5), `RECOVERY <token>
TRANSPORT_STALLED`, `STREAM_FAULT <token> TRANSPORT_FAILED`, `ERROR <tekst>` (trafia do
`BtStatus::message`).

Zachowanie łącza (`btp::Link`, krok co 20 ms):
- **Włączenie**: impuls G6 = 0 przez 120 ms → oczekiwanie na start; 650 ms po impulsie `USE` + `STATUS`
  (Pico, które już czuwało, nie wysyła `READY`). Każda poprawna linia = moduł jest. Bez odpowiedzi przez
  3 s — kolejny impuls; po 3 próbach komunikat **„Nie wykryto modułu Bluetooth (Pico) — sprawdź
  podłączenie”** i dalej jedna próba co 15 s.
- **Czuwanie**: `USE` co 3 s, `STATUS` jak wyżej, 12 s bez żadnej odpowiedzi → ponowne budzenie.
  Polecenia z API czekają w kolejce (8 linii, każda ≤ 80 znaków ASCII), wysyłane po jednym na krok; po
  `RESTART …` / `SELECTED …; restarting` wstrzymane do `READY` (Pico się restartuje).
- **Strumień**: gdy `btSetAudioRoute(true, …)`, słuchawki połączone i kodek pracuje z 44,1 kHz (albo audio
  stoi) → `STREAM <token>`; bez `STREAM_ACK` w 1 s jeszcze raz ten sam token, potem nowa próba po 5 s.
  `INPUT_STALLED` → od razu nowy `STREAM`; `TRANSPORT_FAILED` → komunikat i nowy `STREAM` po 5 s;
  `RECOVERY` → komunikat (Pico samo odnawia strumień). Przy 48 kHz nie ma strumienia, komunikat
  **„Bluetooth wymaga 44,1 kHz”**.
- **Wyłączenie**: `STOP` (jeśli strumień), `SLEEP`, czekanie na `STANDBY` maks. 2,5 s, potem UART jest
  zwalniany. Po `SLEEP` nic już nie wysyłamy: każdy bit startu (stan niski na G32) obudziłby Pico. G6
  zostaje wejściem z pull-up (stan wysoki).
- Linie od Pico: maks. 120 bajtów, znaki sterujące odrzucane (CR, LF, CRLF dowolnie). Wyjątki: `STATUS …`
  do 720 bajtów (Pico 0.9.5 wypisuje tam ~450–650 znaków) i `DEVICE …` do 300 (wynik skanowania niesie całą
  wykrytą nazwę, do 248 bajtów). Nazwy urządzeń mogą mieć UTF-8 (polskie
  litery); niepoprawne bajty zamieniane na `?`, nazwa przycinana do 47 bajtów bez dzielenia znaku.

### Audio i opóźnienie

- Kanał mostka `I2S_NUM_0` (BCK G5, WS G18, DOUT G19, auto-clear) ma to samo
  źródło zegara co kodek (APLL, MCLK = 256·fs), więc przy 44,1 kHz oba kanały chodzą z jednego zegara: bez
  dryfu i bez resamplingu. Częstotliwości APLL nie da się zmienić, gdy trzymają go dwa kanały, dlatego
  `audioStart` najpierw zwalnia kanał mostka (`detail::btAudioSuspend`), a po starcie kodeka task BT
  tworzy go od nowa (`btAudioResume`). Przy tworzeniu kanału mostka sterownik I2S loguje ostrzeżenie
  „APLL is occupied already … working at X Hz while the expected frequency is X Hz” — przy równych
  liczbach to normalne.
- Bufory DMA mostka: jeden bufor = jeden blok audio (task zapisuje całe bufory, więc reguła sterownika IDF
  „kolejka prawie pełna” nigdy nie porzuca bufora zapełnionego do połowy) i o 4 więcej niż w kodeku
  (64 ramki: 8 × 64, 128: 7 × 128, 256: 7 × 256), z czego kodek + 2 wypełnione ciszą przed startem. Task
  audio pisze wtedy, gdy pozwala mu kodek — w najgorszym razie o całą kolejkę kodeka później — i nadal
  zdąży przed DMA mostka. Model zdarzeniowy reguł IDF 5.5.2 (wszystkie fazy, skoki czasu renderowania,
  1 % bloków po 1–2,5 okresu): bez przerw; poprzednie 4 × 128 gubiło dźwięk przy blokach 64 i 256.
- Task audio (rdzeń 1) po każdym bloku, jeszcze przed miksem mono głośnika, kopiuje blok stereo do kanału
  mostka (`i2s_channel_write`, timeout 0, tylko atomiki); blok, który się nie zmieścił, i każdy niedobór DMA
  mostka w czasie, gdy task go zasila (`on_send_q_ovf`), zwiększają `BtStatus::dropped_blocks` (powinno
  zostać 0). Z `local = false` kodek dostaje ciszę (dopiero po
  `STREAM_ACK`), więc nadal taktuje pętlę.
- Opóźnienie: mostek dokłada 8,7 / 14,5 / 29 ms (bloki 64 / 128 / 256: kodek + 2 bufory przed
  odtwarzaniem), ale A2DP/SBC w Pico i słuchawkach to zwykle **150–300 ms** (zależnie od słuchawek). Do
  grania na padach na żywo zostaje głośnik / jack; Bluetooth nadaje się do odsłuchu sekwencji. Z
  `local = true` głośnik gra ok. 0,2 s przed słuchawkami, dlatego „Also speaker / jack” jest domyślnie
  wyłączone.
- Rozmiar: `bt.cpp` z protokołem ~13,8 KB flash (`-Os` dla tego pliku) + ~0,2 KB IRAM (przerwanie UART),
  `audio.cpp` +0,2 KB; 75 B BSS, reszta (~7,5 KB: kontekst, lista 32 urządzeń) w PSRAM dopiero po
  pierwszym włączeniu, 1 KB bufora odbioru i bufory DMA mostka (2 / 3,5 / 7 KB przy blokach 64 / 128 / 256)
  w SRAM. Własny sterownik UART na warstwie LL zamiast
  `uart_driver_install` oszczędza ~18 KB (`ORBIT_BT_IDF_UART=1` przełącza na sterownik IDF).
- Start ORBIT się nie wydłuża: nic nie powstaje, dopóki Bluetooth nie zostanie włączony.

### Procedura testu

1. PC: `bash tools/run_host_tests.sh bt_protocol` — parser każdej linii, fuzz > 5000 linii, pełna oś
   czasu (włączenie → `READY` → `USE` co 3 s → `LIST` → połączenie → `STREAM`/`ACK` → `INPUT_STALLED` →
   `RECOVERY` → `TRANSPORT_FAILED` → `STOP` → `SLEEP` → `STANDBY`), brak modułu, 48 kHz, kolejka
   poleceń, `hal::bt*` na PC (atrapa Pico: `halHostBtFeed` / `halHostBtTakeSent` / `halHostBtAdvance`).
2. Sprzęt bez Pico: włączyć Bluetooth w ORBIT → log `orbit_bt: link: wake pulse`, `boot wait`; po ~9 s
   `link: no module` i komunikat „Nie wykryto modułu…”; potem impuls co 15 s. Wyłączenie → `link: off`.
3. Z Pico (wgrane 0.9.5, 7 przewodów): włączenie → `link: awake`, „Moduł Bluetooth (Pico) gotowy, wersja
   0.9.5”. Słuchawki w trybie parowania → skan (20 s) → lista (nowe z RSSI < 0, zapisane z `saved`) →
   wybór → „Wybrano: …”, potem „Połączono z urządzeniem Bluetooth”.
4. Częstotliwość 44,1 kHz, trasa do mostka → w słuchawkach dźwięk ORBIT; `BtStatus::streaming = 1`,
   `dropped_blocks = 0` (sprawdzić też przy bloku 256), `sbc_rate = 44100`. `TEST` → ton lewy / prawy.
   Przyciski słuchawek → `btPopRemote()`. Przy 48 kHz komunikat „Bluetooth wymaga 44,1 kHz”.
5. Odłączyć przewód danych I2S (G19) przy grającym strumieniu → `STREAM_FAULT … INPUT_STALLED` i nowy
   `STREAM`.
6. Wyłączenie Bluetooth → `STOP`, `SLEEP`, „Bluetooth wyłączony”; Pico śpi (pobór prądu spada) i nie budzi
   się samo (G6 zostaje w stanie wysokim). Ponowne włączenie budzi je impulsem. BACK TO LAUNCHER przy
   włączonym Bluetooth też najpierw wysyła `STOP` + `SLEEP`.
