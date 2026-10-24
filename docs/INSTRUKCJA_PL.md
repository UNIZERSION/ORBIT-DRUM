# ORBIT DRUM — instrukcja (PL)

Polska instrukcja i historia wersji. Opis po angielsku: [README.md](../README.md).

Wersja **0.4.7** (24.10.2026). 0.4.7: **jedno demo** przy starcie — utwór w stylu brzmienia 303 + 909 z „Da Funk” Daft Punk (111 BPM, g-moll, przesterowana linia acid na prostym groovie 909; nuty i wzory są nasze). Sceny A–D to intro, groove z basem, przerwa bez stopy i pełna wersja; FOLLOW gra je jako piosenkę. NEW PROJECT zaczyna od pustego projektu (120 BPM, domyślne pokrętła).
0.4.6: obsługa **klawiatury M5Stack Tab5 Keyboard** ze skrótami jak w Ableton Live (rozdział 3a); cały interfejs i komunikaty **po angielsku**; większe okno ustawień i nowa opcja **Interface size** (Ustawienia → Display: Standard / Large / Extra large — większe okna, listy i pola dotyku); lepszy dotyk przycisków górnego paska; w powiększonym panelu BROWSER kategoria (np. Drums) od razu otwiera pliki w dużym widoku, a widok BROWSER ma listę kategorii obok wyszukiwarki; BASS bez pokrętła TUNE, za to linia i KEYS sięgają oktawę niżej i wyżej (C0–C5); przewodnik ? ma karty ze skrótami klawiatury.
0.4.5: konsola testowa zajmuje pamięć dopiero po pierwszej komendzie z komputera
(6 KB RAM więcej dla aplikacji), długie odpowiedzi nie utykają już w USB.
0.4.4: **mniejsze opóźnienie Bluetooth po stronie Tab5** — ORBIT co 2 s
odczytuje, ile dźwięku czeka w buforze Pico (Ustawienia → Bluetooth: „bufor Pico ok. X ms”), a gdy bufor
narośnie (dryf zegarów, przerwy), opróżnia go w chwili ciszy; **konsola testowa** przez USB
(`tools/device_test.py`): komputer może sam stukać w ekran, czytać stan i robić zrzuty ekranu.
0.4.3: klawiatura KEYS gra **kilkoma palcami naraz** (brzmi ostatnio
wciśnięty klawisz, a po jego puszczeniu ten wciąż trzymany; z SLIDE płynnie, np. tryl), a pokrętła i przyciski
działają, gdy trzymasz klawisze. Pokrętła BASS nie zacinają się już (obracanie nie przerysowuje klawiatury
ani linii), a widok BASS buduje się szybciej (pełny ekran).
0.4.2: **klawiatura KEYS w widoku BASS** — przycisk KEYS w nagłówku zamienia
linię i klawiaturę kroków na dwie oktawy do grania basem na żywo (pokrętła brzmienia zostają); nic nie
zapisuje się do linii. OCT −/+ przesuwa zakres (od C1–C3 do C4–C6), ACCENT gra z akcentem, SLIDE: przesunięcie
palca na inny klawisz płynnie przechodzi na nową nutę (jak slide w 303). Grająca linia podświetla swój klawisz,
a trzymany klawisz ma pierwszeństwo przed linią.
0.4.1: dotyk nie zawiesza się już, gdy magistrala I2C utknie (odczyt panelu
z limitem czasu i automatyczne odblokowanie magistrali; wcześniej wątek dotyku blokował rdzeń aż do wyłączenia).
0.4.0 dodała:
- **ostre powiększanie okien**: powiększone okno jest budowane od nowa w dużym rozmiarze (większe czcionki
  i ikony), zamiast rozciągać obraz; okna wypełniają cały obszar pod paskiem;
- **BASS 303 na cały ekran**: przycisk z narożnikami obok PLAY w widoku BASS;
- **kolor główny**: 6 kolorów tła i paneli (LIGHT, WARM, SILVER, SLATE, DARK, BLACK) oraz **dowolny kolor
  akcentu** suwakami barwy i nasycenia (obok 8 gotowych), Ustawienia → Display.

0.3.1: wykrywanie słuchawek przewodowych potwierdzone na Tab5 (tryb Auto sam przełącza głośnik ↔ słuchawki
i ich głośność), odczyt wykrywania odporny na błędy I2C przy pracującym module Pico. 0.3.0 dodała:
- **automat basowy w stylu 303** (widok BASS): własna linia w każdej scenie, akcent, slide, klawiatura, RANDOM;
- **pamięć głośności**: głośnik, słuchawki i słuchawki BT mają osobny poziom, przełączany samoczynnie; główne
  pokrętło VOLUME steruje głośnością wyjścia (także BT);
- **8 kolorów interfejsu** (Ustawienia → Display);
- **powiększanie każdego okna** ekranu głównego;
- **usuwanie projektów**; NEW PROJECT daje całkiem pusty projekt.

0.2.1: pokrętła z samą kropką, rozbłyski kroków, płynny ruch pokręteł. 0.2.0: Bluetooth przez moduł Pico z Aurory.
Firmware napisane od zera w C++17 na ESP-IDF 5.5.2 i LVGL 9.3.
Wygląd i zachowanie kontrolek pochodzą z białego prototypu ORBIT LIGHT. Funkcje, które
w prototypie były tylko symulacją, tu działają naprawdę.

> **Stan:** sprawdzone na PC (16 zestawów testów, zrzuty ekranu całego interfejsu). Na Tab5: start
> 0.3.0 bez błędów, pady, sekwencer i pokrętła działają, wykrywanie słuchawek potwierdzone, moduł Pico
> odpowiada. Nowości 0.3 i dźwięk przez Bluetooth czekają na pełny test na sprzęcie.

## 1. Instalacja przez launcher

1. Pobierz `ORBIT_DRUM_SD_<wersja>.zip` z zakładki Releases na GitHubie (albo zbuduj `release/KARTA_SD/`
   poleceniem `python -I tools/make_release.py`) i rozpakuj na kartę microSD (folder `ORBIT` w katalogu głównym).
   Karta może być FAT32 albo exFAT.
2. Włóż kartę do Tab5 i uruchom launcher. W zakładce **SD** wybierz
   `ORBIT/ORBIT_DRUM_0.4.7.bin` i zainstaluj go do slotu aplikacji, potem **BOOT**.
3. Powrót do launchera: **Ustawienia → About → BACK TO LAUNCHER** (zapyta o niezapisane zmiany)
   albo przycisk RESET.

Plik ma 6,21 MB (w tym większe czcionki do powiększania) i jest zbudowany dla ESP32-P4 v1.x (sprawdzony na v1.3). Limit to cały slot aplikacji
launchera w obecnym układzie (UiFlow2: 7552 KB = 7,73 MB); w układzie „2 Apps” slot ma 4096 KB (4,19 MB).
Nie używaj `idf.py flash` na Tab5 z launcherem — nadpisałoby to launcher pod adresem 0x10000.

## 2. Test sprzętu (zalecany przed pierwszym użyciem)

`ORBIT/TEST/ORBIT_SELFTEST_0.4.7.bin` instaluje się tak samo. Pokazuje paski kolorów, punkty dotyku
(do 5 palców) z częstotliwością odczytu, ton 440 Hz i metronom ze statystyką opóźnienia i przerw
(xrun), stan słuchawek (surowy bajt HP_DET), listę plików z karty, baterię i licznik FPS.
Procedura i co zanotować: `docs/SPRZET_TAB5.md`, sekcja 9. Najważniejsze do potwierdzenia:
kierunek obrazu i dotyku, dźwięk z głośnika i słuchawek, wykrywanie słuchawek, brak przerw w dźwięku.

## 3. Obsługa

| Element | Działanie |
|---|---|
| **PADS** | 16 padów, gra kilka palców naraz, dźwięk startuje w chwili dotknięcia. MUTE / SOLO przełączają tryb: dotknięcie padu wycisza go lub daje solo. |
| **SAMPLE** | Fala prawdziwego sampla; przeciągnij pomarańczowe znaczniki START/END, podwójne dotknięcie znacznika = przejście przez zero. Zakładki TRIM, SLICE, ENVELOPE, FILTER, PITCH, LFO zmieniają pokrętła. LAYER 1–4 (przytrzymaj: tryb warstw, wyczyść). ✎ otwiera SAMPLE DETAILS (normalizacja, odwrócenie, przycięcie, fade, mono, crush, UNDO, zapis WAV, choke, wyjście, polifonia). |
| **SEQUENCE** | 8 ścieżek × 16 kroków (A1–B4 / C1–D4), przeciąganie maluje kroki. VELOCITY, PROBABILITY i MORE (RATCHET, NUDGE, PITCH, COND) edytują wartości kroków. Swing, podział, długość do 64 kroków (BAR), FILL, RANDOM, CLEAR z UNDO. |
| **MIXER** | 8 szyn + MASTER: suwaki (podwójne dotknięcie = 0 dB), mierniki, M/S, L = limiter, C = metronom. |
| **BROWSER** | Sample i zestawy z karty, lista kategorii obok wyszukiwarki, wyszukiwarka z polską klawiaturą, podgląd fali, ▶ PREVIEW, LOAD TO pad, LOAD KIT (z potwierdzeniem i UNDO). |
| **FX** | Pogłos, delay zsynchronizowany z tempem, makra LO-FI / WIDE / PUMP / SPACE, limiter, master, metronom. |
| **BASS** | Automat basowy w stylu 303 (jeden głos, filtr 24 dB z rezonansem, akcent, slide). 16 kroków taktu (jak w SEQUENCE): dotknij kroku, potem klawisza (nuta + przejście do następnego kroku) albo rysuj nuty palcem po linii. GATE / ACCENT / SLIDE / OCT ± dla wybranego kroku, SAW / SQR, MUTE, RANDOM, CLEAR z UNDO. Przycisk **KEYS**: dwie oktawy do grania na żywo bez zapisu, kilkoma palcami naraz (OCT ± zakres, ACCENT, SLIDE = płynne przejście przy przesuwaniu palca, SAW / SQR); grająca linia podświetla swój klawisz, trzymany klawisz ma pierwszeństwo przed linią. Linia obejmuje C0–C5 (od 0.4.6, zamiast pokrętła TUNE). Pokrętła (w obu trybach): CUTOFF, RESO, ENV MOD, DECAY, ACCENT, DRIVE, FX SEND, LEVEL. Bas gra na szynie 8 miksera (BASS). |
| **Prawy panel** | VOLUME = głośność wyjścia, która gra teraz (SPK głośnik, HP słuchawki, BT); każde wyjście pamięta swój poziom i przełącza go samo. FX SEND i PAN wybranego padu (głośność padu: SAMPLE → TRIM → PAD VOL). Stop/powrót, start, nagrywanie; TAP, –/+ (przytrzymaj BPM, aby wpisać tempo). |
| **Powiększanie** | Narożniki w nagłówku PADS, BROWSER, SCENES i w prawym panelu (okno główne: przycisk obok zakładek) powiększają okno na obszar pod paskiem; ten sam przycisk je zmniejsza. Widok BASS ma własny przycisk (obok PLAY): bas na **cały ekran**, pasek górny chowa się do powrotu. Powiększone okno jest budowane od nowa w dużym rozmiarze z większymi czcionkami i ikonami, więc tekst i linie są ostre (nie rozciągnięty obrazek). |
| **SCENES** | Sceny A–D (zmiana kwantowana do taktu), „+”: kopiuj, wyczyść, cofnij. Siatka 16 slotów to aranżacja piosenki (FOLLOW ją odtwarza), wiersz FX = rzut efektów w danym slocie. GRID i LENGTH ustawiają scenę. |
| **Pasek górny** | Folder = projekty na karcie (zapis, otwieranie, NEW PROJECT = pusty projekt, DELETE = usunięcie po drugim dotknięciu, eksport JSON). Kolory interfejsu: Ustawienia → Display → **Base color** (tło, panele, pady i pokrętła: LIGHT, WARM, SILVER, SLATE, DARK, BLACK) i **Accent color** (8 gotowych albo CUSTOM: suwaki HUE i SATURATION z podglądem); zmiana po szybkim restarcie, projekt zapisuje się i otwiera ponownie. Zębatka = ustawienia (General, Audio, MIDI, Bluetooth, Display, Touch, Storage, About). ? = przewodnik. Chip **BT** (gdy Bluetooth jest włączony) otwiera ustawienia Bluetooth. |

Pokrętła: przeciągnij w pionie, podwójne dotknięcie przywraca wartość domyślną.
**Ustawienia → Display → Interface size** powiększa okna, listy wyboru i pola dotyku (pola dotyku całego ekranu po szybkim restarcie). W powiększonym panelu BROWSER dotknięcie kategorii od razu otwiera jej pliki w dużym widoku.
Animacje (rozbłyski kroków, ruch pokręteł, ripple) wyłącza **Ustawienia → Display → Animations**
albo tryb oszczędzania energii.

## 3a. Klawiatura Tab5 Keyboard

Klawiaturę M5Stack Tab5 Keyboard można podłączać i odłączać w trakcie pracy. Skróty jak w Ableton Live:

| Klawisz | Działanie |
|---|---|
| Spacja | Start od początku / stop |
| Shift+Spacja | Dalej od miejsca zatrzymania |
| Enter | Start (gdy stoi) |
| F9 | Nagrywanie (REC) |
| Tab / Shift+Tab | Następny / poprzedni widok |
| A W S E D F T G Y H U J K O L P ; ' | Nuty (klawiatura MIDI komputera): w BASS gra 303, w innych widokach pady A1–D4 |
| Z / X | Oktawa basu w dół / w górę |
| C / V | Siła uderzenia (velocity) w dół / w górę |
| M | Klawiatura MIDI włącz / wyłącz |
| Strzałki góra / dół | Scena |
| Strzałki lewo / prawo | Pad |
| 0 | Wycisz wybrany pad |
| F1–F8 | Wycisz pady pokazanego banku |
| F11 | Powiększ obszar roboczy (BASS: cały ekran) |
| Esc | Zamknij okno lub listę, zmniejsz powiększony panel |
| Ctrl+Z | Cofnij |
| Ctrl+S | Zapisz projekt |
| Ctrl+O, Ctrl+N, Ctrl+Shift+S | Menedżer projektów |
| Ctrl+, | Ustawienia |
| Ctrl+Alt+B / Ctrl+Alt+M | BROWSER / MIXER |
| Shift+F1 | Przewodnik |

Gdy otwarta jest klawiatura ekranowa (nazwa projektu, wyszukiwarka, tempo), klawisze piszą w niej: Enter = OK,
Esc = anuluj.

## 4. Własne sample i zestawy

```
/ORBIT/KITS/<nazwa zestawu>/      do 16 plików WAV → pady A1..D4 (kolejność nazw albo kit.txt: "01=plik.wav")
/ORBIT/SAMPLES/Drums|Bass|Synth|Vocals|FX/   kategorie przeglądarki
/ORBIT/USER/                      Twoje i edytowane sample (SAVE WAV)
/ORBIT/PROJECTS/                  projekty (.orb, JSON)
```

WAV 8/16/24/32-bit, float 32/64, mono/stereo, dowolna częstotliwość (44,1 kHz gra bez przeliczania).
Wbudowane zestawy **808 STYLE** i **909 STYLE** działają też bez karty. To nasza synteza z opisów
obwodów, bez sampli Rolanda (`docs/KITY_808_909.md`).

## 5. MIDI (USB-A)

Kontroler USB-MIDI podłączony do gniazda USB-A: nuty na kanale 10 (zmiana w Ustawieniach) grają pady
według mapy GM/909 (36 Kick, 38 Snare, 42 Cl Hat, 46 Op Hat, 39 Clap, 37 Rim, 41/45/48 tomy, 49 Crash,
51 Ride). Start/Stop/Continue sterują transportem, zegar wejściowy może prowadzić tempo, wyjściowy jest
opcjonalny. MIDI Learn: Ustawienia → MIDI → Learn, rusz pokrętło, potem kontroler (32 przypisania).

## 6. Słuchawki i głośniki Bluetooth (moduł Pico z Aurory)

Tab5 nie ma klasycznego Bluetooth (ESP32-C6 to tylko BLE), więc dźwięk do zwykłych słuchawek i głośników
A2DP idzie przez płytkę pomocniczą z Aurora Player: **M5Stamp Pico** z firmware „Pico Bluetooth bridge”
0.9.x — ten sam moduł i te same przewody co w Aurorze (wgrywanie i bezpieczeństwo:
`aurora-player/pico-bridge/README.md`).

| Tab5 | Pico | Sygnał |
|---|---|---|
| G5 / G18 / G19 | G26 / G25 / G22 | I2S: BCK / WS / DATA (Tab5 taktuje) |
| G6 | G32 | UART do Pico, impuls 120 ms budzi moduł |
| G46 | G33 | UART z Pico |
| 5V / GND | 5V / GND | zasilanie |

**Ustawienia → Bluetooth** (działa od razu, bez APPLY): przełącznik **Bluetooth** budzi moduł (wiersz
*Module* pokazuje wersję i stan); **Also speaker / jack** (domyślnie wyłączone) gra też na głośniku lub
w gnieździe — ok. 0,2 s przed słuchawkami; głośność słuchawek BT ustawia główne pokrętło **VOLUME**
(pokazuje BT, gdy gra Bluetooth, i pamięta ten poziom osobno); przy AirPods bateria i tryb
(OFF / ANC / TRANSPARENCY / ADAPTIVE). Niżej **Devices**: zapisane urządzenia (Pico pamięta 8) i wyniki
skanowania, razem do 32 — dotknięcie łączy; **SCAN** (20 s, słuchawki w trybie parowania), **RECONNECT**,
**TEST** (ton lewy / prawy). Przyciski słuchawek: Play = wznowienie od miejsca pauzy, Pause = stop,
Next / Previous = następna / poprzednia scena. Chip **BT** na pasku górnym: pomarańczowy = połączono,
szary = czeka na urządzenie.

Przy włączonym Bluetooth silnik pracuje z **44,1 kHz** (moduł przyjmuje 16 bit / 44,1 kHz), po wyłączeniu
wraca poprzednia częstotliwość. Włączony Bluetooth startuje sam po uruchomieniu, dopiero gdy interfejs i
dźwięk już działają (start nie jest dłuższy). Powrót do launchera najpierw usypia moduł. **Opóźnienie:** Bluetooth dodaje ok. 0,15–0,25 s — do
grania na padach na żywo używaj głośnika lub słuchawek przewodowych.

## 7. Budowanie ze źródeł

```bash
bash tools/run_host_tests.sh
```
Uruchamia 16 testów na PC (zig 0.17): silnik, sekwencer, głos basowy, WAV i obróbkę (z fuzzingiem),
kity, kontroler, widżety (z kontrastem tekstu w każdej palecie) i zrzuty całego interfejsu do
`host/shots/ui_*.png`. Kolory zrzutów: `ORBIT_BASE=0..5` (LIGHT … BLACK), `ORBIT_THEME=0..7` (akcent),
`ORBIT_HUE=0..359` + `ORBIT_SAT=20..100` (akcent CUSTOM); `UI_SHOT_PREFIX` zmienia miejsce zapisu.

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf_build.ps1 -Project firmware
```
Buduje firmware offline (ESP-IDF 5.5.2, komponenty są w `firmware/managed_components`). Analogicznie
`-Project firmware_selftest`. Po zmianie `sdkconfig.defaults` najpierw `-Target reconfigure`.

```bash
python -I tools/make_release.py
```
Sprawdza obrazy (ESP32-P4, rewizja, sumy, limit 7552 KB slotu app0) i składa `release/KARTA_SD` +
`manifest.json`.

Uwaga: `tools/fontgen/build.sh` i `tools/kitgen/build.sh` odtwarzają czcionki i kity w pełnej wersji
(o ~0,6 MB większe). Do wersji 0.2.1 obowiązywał limit 4 MB, dlatego wbudowane kity mają krótsze
ogony, a czcionki ograniczone pary kerningu (pliki WAV na karcie są pełne).

## 8. Struktura

| Folder | Zawartość |
|---|---|
| `firmware/main/core` | silnik audio (32 głosy, SVF, AHDSR, LFO, warstwy, choke, FX, limiter), sekwencer co do próbki, WAV, obróbka sampli, slicing, biblioteka, zapis projektów, kity |
| `firmware/main/hal` | Tab5: obraz bez rwania (PPA + 3 bufory), dotyk 5 punktów, I2S/ES8388 (≈6,7 ms), SD z exFAT, ustawienia, USB-MIDI, most Bluetooth do modułu Pico (UART + I2S) |
| `firmware/main/ui` | kontroler aplikacji, ekrany ORBIT LIGHT, widżety, czcionki, ikony |
| `host/` | uprząż PC i testy |
| `docs/` | specyfikacja, notatki sprzętowe, opis kitów, ta instrukcja |

## 9. Do sprawdzenia na urządzeniu

Błędy odczytu układu 0x43 przy pracującym module Pico (w 0.3.1 zapisują diagnozę w logu i próbują same się
naprawić), nowości 0.3 (BASS, powiększanie, kolory, usuwanie projektów, przełączanie głośności),
rzeczywiste obciążenie CPU audio (silnik sam ogranicza liczbę głosów przy przeciążeniu i to zgłasza),
czas startu, płynność obrazu, zachowanie USB-MIDI z prawdziwym kontrolerem, Bluetooth z prawdziwym
modułem Pico (sprawdzony tylko z symulowanym modułem na PC). Szczegóły:
`docs/SPRZET_TAB5.md` sekcja 10.

## Licencje

Kod projektu: własny. Czcionki: Lato, Montserrat (SIL OFL 1.1), DejaVu Sans (licencja Bitstream Vera),
Share Tech Mono (OFL) — pliki licencji w `tools/fonts/`. Obsługa modułu Pico: własna implementacja
udokumentowanego protokołu UART z Aurora Player (Aurora jest na GPL-3.0, jej kod nie jest kopiowany). Zestawy 808/909 STYLE: własna synteza.
Głos basowy: własna synteza (oscylator, filtr drabinkowy, obwiednie), bez kodu i próbek Rolanda.
„TR-808”, „TR-909” i „TB-303” to znaki towarowe Roland Corporation, użyte wyłącznie opisowo.
