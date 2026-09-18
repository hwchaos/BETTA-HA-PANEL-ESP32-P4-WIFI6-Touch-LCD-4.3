<!-- SPDX-License-Identifier: LicenseRef-FNCL-1.1 | Copyright (c) 2026 Cpt_Kirk -->
<img src="images/BETTAOS.jpg" alt="BETTA OS Logo" width="10%" />

# Panel ścienny Waveshare 7" (fork BETTA HA Panel, wariant `panel7`)

**Konfigurowalny w locie panel ścienny Home Assistant dla Waveshare
ESP32-P4-WIFI6-Touch-LCD-7B (ekran dotykowy 7", 1024×600, MIPI-DSI).**
Swój pulpit budujesz bezpośrednio na urządzeniu — bez edycji YAML i bez
przebudowywania firmware.

To repozytorium to **fork [BETTA HA Panel v0.8.2](https://github.com/cptkirki/BETTA-HA-PANEL)**
autorstwa **Cpt_Kirk**, rozbudowany o obsługę panelu **Waveshare 7" (ESP32-P4)** oraz o duży
zestaw własnych funkcji: bogaty silnik wyglądu kafelków i stron, wbudowane motywy z automatycznym
przełączaniem dzień/noc, wygaszacz z zegarem typu flip, dodatkowe typy kafelków, integrację
z **Alarmo**, **radio internetowe** i **Music Assistant**, **stronę pogody**, kamery Home Assistant
oraz **wbudowaną kamerę OV5647** z detekcją ruchu, asystenta głosowego **Xiaozhi**, obsługę karty
microSD, profesjonalną diagnostykę (w tym watchdog klatek MIPI-DSI), kopię zapasową i odtwarzanie,
MQTT z autodiscovery Home Assistant oraz pełną polską lokalizację.

> 🇬🇧 **English documentation:** [README.md](README.md)

---

## Spis treści

- [Licencja i autorstwo (przeczytaj)](#licencja-i-autorstwo-przeczytaj)
- [Najważniejsze nowości](#najważniejsze-nowości)
- [Pełna, szczegółowa lista usprawnień](#pełna-szczegółowa-lista-usprawnień)
  - [A. Port na ESP32-P4 i sprzęt Waveshare 7B](#a-port-na-esp32-p4-i-sprzęt-waveshare-7b)
  - [B. Dotyk, reakcja panelu, płynność](#b-dotyk-reakcja-panelu-płynność)
  - [C. Wygląd kafelka (`tile_*`)](#c-wygląd-kafelka-tile_)
  - [D. Wygląd strony (`page_*`)](#d-wygląd-strony-page_)
  - [E. Motywy i tryb dzień/noc](#e-motywy-i-tryb-dzieńnoc)
  - [F. Wygaszacz ekranu, zegar flip, zarządzanie ekranem](#f-wygaszacz-ekranu-zegar-flip-zarządzanie-ekranem)
  - [G. Górny pasek i dolna nawigacja](#g-górny-pasek-i-dolna-nawigacja)
  - [H. Biblioteka kafelków i integracja z Alarmo](#h-biblioteka-kafelków-i-integracja-z-alarmo)
  - [I. Kamery: Home Assistant i kamera lokalna OV5647](#i-kamery-home-assistant-i-kamera-lokalna-ov5647)
  - [J. Asystent głosowy Xiaozhi](#j-asystent-głosowy-xiaozhi)
  - [K. Radio internetowe i Music Assistant](#k-radio-internetowe-i-music-assistant)
  - [L. Strona pogody](#l-strona-pogody)
  - [M. Sieć, czas, odporność łącza](#m-sieć-czas-odporność-łącza)
  - [N. Diagnostyka, dziennik, watchdogi](#n-diagnostyka-dziennik-watchdogi)
  - [O. Karta microSD](#o-karta-microsd)
  - [P. Kopie zapasowe, OTA, auto-restart](#p-kopie-zapasowe-ota-auto-restart)
  - [Q. Lokalizacja i języki](#q-lokalizacja-i-języki)
  - [R. Studium przypadku: niebieskie rozbłyski ekranu (usterka MIPI-DSI)](#r-studium-przypadku-niebieskie-rozbłyski-ekranu-usterka-mipi-dsi)
  - [S. Poprawki stabilności (zawieszanie UI, audio, RAM)](#s-poprawki-stabilności-zawieszanie-ui-audio-ram)
  - [T. Wyrównanie wariantów, kopie, dokumentacja](#t-wyrównanie-wariantów-kopie-dokumentacja)
  - [Świadomie poza zakresem](#świadomie-poza-zakresem)
- [Zrzuty ekranu — panel](#zrzuty-ekranu--panel)
- [Zrzuty ekranu — edytor WWW](#zrzuty-ekranu--edytor-www)
- [Obsługiwany sprzęt](#obsługiwany-sprzęt)
- [Funkcje przejęte z upstreamu](#funkcje-przejęte-z-upstreamu)
- [Pierwsze uruchomienie](#pierwsze-uruchomienie)
- [Budowanie ze źródeł](#budowanie-ze-źródeł)
- [Wgrywanie firmware i budżet pamięci flash](#wgrywanie-firmware-i-budżet-pamięci-flash)
- [Sekcje edytora WWW](#sekcje-edytora-www)
- [Biblioteka kafelków](#biblioteka-kafelków)
- [Wygląd — opcje `tile_*` i `page_*`](#wygląd--opcje-tile_-i-page_)
- [Najważniejsze ustawienia panelu](#najważniejsze-ustawienia-panelu)
- [API HTTP](#api-http)
- [MQTT — tematy, polecenia, encje discovery](#mqtt--tematy-polecenia-encje-discovery)
- [Uwagi i pułapki](#uwagi-i-pułapki)
- [Struktura projektu](#struktura-projektu)
- [Prywatność — brak danych osobowych w repozytorium](#prywatność--brak-danych-osobowych-w-repozytorium)
- [Licencja](#licencja)
- [Zastrzeżenie / Disclaimer](#zastrzeżenie--disclaimer)

---

## Licencja i autorstwo (przeczytaj)

- Projekt oryginalny: **BETTA HA Panel v0.8.2** — Copyright (c) 2026 **Cpt_Kirk**.
- Licencja: **[LicenseRef-FNCL-1.1](LICENSE)** (Federation Non-Commercial License v1.1) —
  **wyłącznie niekomercyjna**. Każde użycie komercyjne wymaga osobnej, pisemnej licencji
  od właściciela praw autorskich (patrz §11 licencji).
- **Ten fork modyfikuje oryginalne oprogramowanie.** Zgodnie z §3 licencji zmiany są wyraźnie
  oznaczone i opisane w sekcji
  [Pełna, szczegółowa lista usprawnień](#pełna-szczegółowa-lista-usprawnień)
  oraz w [release-notes.md](release-notes.md).
- Oryginalne nagłówki `SPDX-License-Identifier: LicenseRef-FNCL-1.1` i `Copyright (c) 2026 Cpt_Kirk`
  są zachowane we wszystkich plikach źródłowych.
- To projekt prywatny i **nie** jest oficjalnym wydaniem BETTA HA Panel; numer wersji upstreamu
  jest celowo zachowany (`v0.8.2` + sufiks wariantu `-7b`).

---

## Najważniejsze nowości

| Funkcja | Opis |
|---|---|
| 🖥 **Nowy wariant sprzętowy** | Pełne wsparcie **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B**: 7" 1024×600 MIPI-DSI (EK79007), dotyk GT911, 32 MB flash (adresowanie 4-bajtowe), Wi-Fi 6 przez ESP32-C6, microSD, ES8311. |
| 🎨 **Silnik wyglądu kafelka** | Tło, gradient, obramowanie, promień, krycie, cień, skala czcionki oraz pięć niezależnych kolorów tekstu (tytuł / opis encji / wartość / ikona / cały kafelek). Presety, cztery akcje „kopiuj wygląd" i reset. Stan ikony jest przemalowywany (OFF = szary, ON = żółty), więc kolor nigdy „nie zjada" się po odświeżeniu encji. |
| 🖼 **Wygląd strony** | Kolor tła, gradient, użycie tapety panelu, przyciemnienie tapety, motyw tylko dla tej strony i osiem presetów strony. |
| 🌗 **Motywy + dzień/noc** | Siedem wbudowanych motywów (`dark_v2`, `classic_v1`, `light`, `ocean`, `contrast`, `oled`, `retro`), motywy własne w edytorze oraz automatyczne przełączanie dzień/noc w zadanym oknie godzinowym. |
| ⏰ **Wygaszacz z zegarem flip** | Zegar klasyczny lub flip, 12 h / 24 h, data, sekundy, własne kolory, przyciemnienie, własna tapeta, tryb nocny i pełne wygaszenie ekranu. |
| 🧭 **Przebudowany górny pasek** | Zegar **idealnie na środku**, skróty **Radio** i **Pogoda** po lewej, status Wi-Fi / Home Assistant + ikona ustawień po prawej. Skróty pojawiają się tylko wtedy, gdy ich strony istnieją w układzie. |
| 📻 **Radio internetowe** | Pełnoekranowa siatka stacji (2–4 kolumny), „teraz gra", głośność i stop; strumień odtwarza Home Assistant (`media_player.play_media`), panel tylko wysyła URL. |
| 🎵 **Music Assistant** | Dedykowana strona z kolejką, wyborem odtwarzacza i szybkim startem ulubionych mediów. |
| 🌤 **Strona pogody** | Osobna strona o stałym identyfikatorze `pogoda` (bez zakładki w dolnym pasku — otwiera ją skrót w górnym pasku), konfigurowalna z edytora WWW kafelkami pogody, prognozy i czujników. |
| 📷 **Kamery** | Kamery Home Assistant (REST/MJPEG, logowanie, skalowanie, 1–60 s odświeżania) **oraz wbudowana kamera OV5647** (MIPI-CSI) z detekcją ruchu i wybudzaniem ekranu; podgląd działa tylko na stronie kamer, poza nią strumień jest zatrzymywany. |
| 🗣 **Xiaozhi AI** | Asystent głosowy (protokół WebSocket v3, Opus, mikrofon ES8311, push-to-talk, przerwanie odpowiedzi) z aktywacją w chmurze; token tylko w NVS. |
| 📊 **Diagnostyka** | Statystyki pamięci dla każdej puli, liczniki łącza Wi-Fi/HA, brakujące encje, dziennik systemowy w przeglądarce, zrzuty panik, **watchdog klatek MIPI-DSI** i licznik operacji flash — narzędzia, które pozwoliły znaleźć przyczynę rozbłysków ekranu. |
| 💾 **Kopia zapasowa / odtwarzanie** | Cała konfiguracja w jednym pliku JSON — układ, ustawienia publiczne, motywy i kamery. **Sekrety nigdy nie są eksportowane.** |
| 🗂 **Karta microSD** | Montowanie, formatowanie, przeglądanie i usuwanie plików, tapeta wygaszacza z karty, eksport logów, ochrona przed wyjściem ze ścieżki. |
| 📡 **MQTT + autodiscovery** | 24 klucze poleceń, tematy `state` / `set/+` / `status` i ~23 encje wykrywane automatycznie w Home Assistant. |
| ⚡ **Mniejszy i szybszy** | Build z `-O2`, interfejs WWW serwowany jako gzip, mniejsze zużycie RAM przez wyłączenie nadmiarowych logów, LVGL i czcionki w PSRAM. |
| 🇵🇱 **Język polski** | Polski jest językiem domyślnym; pełne tłumaczenia PL/EN/DE/ES/FR panelu i edytora, czcionki Poppins z polskimi znakami, własne tłumaczenia jako JSON. |

---

## Pełna, szczegółowa lista usprawnień

### A. Port na ESP32-P4 i sprzęt Waveshare 7B

| Element | Co zostało zrobione |
|---|---|
| **Wariant budowy** | Nowy wariant `panel7` (`sdkconfig.defaults.panel7`, `main/idf_component.panel7.yml`, `CMakeLists.txt`, `CMakePresets.json`); `PROJECT_VER = v0.8.2-7b`, projekt `betta-ha-panel-7b`. |
| **Wyświetlacz** | Inicjalizacja MIPI-DSI + kontroler **EK79007**, 1024×600, RGB565, panel w `main/drivers/display_init_panel7.c`. |
| **Dotyk** | **GT911** (do 5 punktów) na I²C (SCL = GPIO8, SDA = GPIO7 — magistrala dzielona z kamerą SCCB), `main/drivers/touch_init_panel7.c`. |
| **Rewizja P4** | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y` (P4 < rew 3.0, 360 MHz) — bez tego bootloader odmawia startu. |
| **32 MB flash** | Kwarowa pamięć flash adresowana 4-bajtowo: `CONFIG_IDF_EXPERIMENTAL_FEATURES=y` + `CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH=y`. Wgrywanie **wymaga** `--flash_size 32MB` — przy złym rozmiarze panel wpada w pętlę restartów z komunikatem `exceeds flash chip size`. |
| **PSRAM** | PSRAM HEX 200 MHz; bufory LVGL i czcionki alokowane w PSRAM (`main/ui/lv_psram_mem.c`), dzięki czemu wewnętrzny SRAM zostaje dla stosów i DMA. |
| **Wi-Fi** | ESP32-C6 przez SDIO (ESP-Hosted + `esp_wifi_remote`) z **dwoma patchami własnymi** (`patches/esp_hosted_2.11.7_transport_tx_graceful.patch`, `patches/esp_hosted_2.11.7_sdio_streaming_rx_graceful.patch`), nakładanymi automatycznie przez `cmake/apply_vendor_patches.cmake`. Pozwalają użyć wbudowanego firmware C6 w innej wersji niż stos hosta. |
| **Audio** | Kodek **ES8311** (mikrofon + głośnik), wspólny dla panelu i asystenta Xiaozhi. |
| **Kamera** | Wbudowany moduł **OV5647** na MIPI-CSI (RAW10 1280×960 z binningiem 2×2 @45 fps domyślnie, tryb zapasowy RAW8 800×800), sterowany przez `esp_video` + `esp_cam_sensor`. |
| **Karta microSD** | SDMMC z wewnętrznym LDO, obsługa FATFS z długimi nazwami (`CONFIG_FATFS_LFN_HEAP=y`, `MAX_LFN=255`). |
| **Optymalizacja** | `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (`-O2`) — szybszy interfejs i mniejszy firmware. |
| **Stos główny 16 kB** | `-O2` potrafi rozwinąć ścieżkę `snprintf` do newlib na tyle, że domyślny stos zadania głównego przestaje wystarczać — podniesiony do 16 kB. |
| **Stos przerwań 6144 B** | `CONFIG_FREERTOS_ISR_STACKSIZE=6144` — łańcuch ISR na rdzeniu 1 potrafił dojść do ostatniego bajtu stosu i powodować `Stack protection fault` (mcause 27) i restart. |
| **Uwierzytelnianie HTTP** | `CONFIG_ESP_HTTP_CLIENT_ENABLE_DIGEST_AUTH=y` — migawki kamer za loginem/hasłem. |
| **VFS / ISP** | `CONFIG_VFS_MAX_COUNT=16` (rejestracja VFS przez `esp_video`), `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y`. |
| **Logi i paniki** | `CONFIG_LOG_MAXIMUM_LEVEL=3`, mostek LVGL→log (`CONFIG_LV_USE_LOG=y`, poziom WARN), zrzuty panik do partycji coredump (format ELF, bez zrzutu DRAM, `CHECK_BOOT`, 24 zadania). |

### B. Dotyk, reakcja panelu, płynność

| Usprawnienie | Klucz / plik | Działanie |
|---|---|---|
| Efekt wciśnięcia kafelka | `display.tile_press_fx`, `tile_press_fx_dim`, `tile_press_fx_scale` | Kafelek przygasza się i/lub zmniejsza pod palcem — natychmiastowa informacja zwrotna na dużym ekranie 7". |
| Animacja wartości | `display.value_anim`, `value_anim_ms` | Zmiany liczb (temperatura, moc) animowane, bez „skakania" tekstu. |
| Przejścia stron | `display.page_transition`, `page_transition_ms` | Przesuwanie / przenikanie między stronami z konfigurowalnym czasem. |
| Obszar dotyku suwaka | `main/ui/ui_slider_touch.c` | Szerszy obszar chwytania suwaka i płynne przeciąganie — jasność i temperatura barwowa przestają „uciekać". |
| Pamięć podręczna jasności | `main/ha/ha_light_capabilities.*` | Możliwości światła (jasność / temperatura / RGB) pobierane raz i cache'owane, zamiast odpytywania przy każdym rysowaniu. |
| Brak blokowania pętli UI | `main/ui/ui_runtime.c` | Zdarzenia sieciowe nie są przetwarzane w pętli LVGL — brak mikro-zacięć przy dużym ruchu w Home Assistant. |
| Mniej logów | `sdkconfig.defaults.panel7` | Wyciszone logi INFO na konsoli: mniej DMA/IO, więcej czasu CPU dla LVGL. |

### C. Wygląd kafelka (`tile_*`)

Pełny zestaw niezależnych opcji wyglądu — ustawiasz je z edytora WWW w inspektorze kafelka
(sekcja „Widgets"), pojedynczo lub dla wszystkich kafelków naraz:

| Opcja | Znaczenie |
|---|---|
| `tile_bg_color`, `tile_bg_grad_color`, `tile_bg_grad_dir` | Kolor tła, kolor końca gradientu i jego kierunek (brak / pion / poziom). |
| `tile_border_color`, `tile_border_width`, `tile_radius` | Kolor i grubość obramowania oraz promień narożników (od ostrych kart do pigułek). |
| `tile_opacity`, `tile_shadow` | Przezroczystość kafelka i cień — efekt „szkła" na kolorowej tapecie. |
| `tile_font_scale` | Skalowanie czcionki kafelka (duże odczyty z drugiego końca pokoju). |
| `tile_icon_color`, `tile_label_color`, `tile_text_color`, `tile_title_color`, `tile_value_color` | Pięć niezależnych kolorów: ikona, opis encji, cały tekst, tytuł, wartość. |
| `tile_press_fx`, `tile_press_fx_dim`, `tile_press_fx_scale` | Efekt wciśnięcia (patrz sekcja B). |
| Akcje „kopiuj wygląd" | Kopiowanie wyglądu do wskazanego kafelka, do całej strony, do wszystkich stron oraz **reset** — koniec ręcznego powtarzania ustawień. |
| Kolor stanu ikony | Ikona jest przemalowywana przy każdej zmianie stanu (`main/ui/ui_tile_style.c`, `ui_widget_factory.c`): wyłączone = szary (`#8CA2B5`), włączone = żółty (`#FFCF6B`). Wcześniej kolor potrafił „utknąć" na wartości z chwili utworzenia kafelka. |

### D. Wygląd strony (`page_*`)

| Opcja | Znaczenie |
|---|---|
| `page_bg_color` | Kolor tła całej strony. |
| `page_bg_grad_color`, `page_bg_grad_dir` | Gradient tła i jego kierunek. |
| `page_wallpaper` | Użycie tapety panelu jako tła strony. |
| `page_dim` | Przyciemnienie tapety (0–90 %) — kafelki pozostają czytelne. |
| `page_theme` | Wymuszenie motywu **tylko dla tej strony** (np. ciemna strona kamer na jasnym pulpicie). |
| Presety strony | `auto` (motyw), `midnight`, `deep_sea`, `forest`, `sunset`, `plum`, `wallpaper`, `wallpaper_dim`. |
| Reset | Przywrócenie wyglądu strony do wartości motywu. |

### E. Motywy i tryb dzień/noc

- **Siedem motywów wbudowanych**: `dark_v2` (domyślny), `classic_v1`, `light`, `ocean`, `contrast`,
  `oled`, `retro` (`main/ui/theme/theme_palette.c`).
- **Motywy własne**: tworzenie, edycja, usuwanie i eksport z edytora WWW
  (`GET/PUT/DELETE /api/themes*`), zapis w LittleFS.
- **Automatyczne dzień/noc**: `display.theme_auto_enabled`, `theme_day_id`, `theme_night_id` oraz
  okno godzinowe trybu nocnego (`night_start_min`, `night_end_min`) — panel sam przechodzi na
  ciemny motyw wieczorem i wraca rano.
- **Motyw dla pojedynczej strony** (`page_theme`) i **motyw bazowy** aplikowany globalnie przez
  router motywów (`main/ui/ui_theme_router.c`).
- **Motyw wygaszacza**: kolor zegara i daty, styl zegara, przyciemnienie oraz osobna tapeta.

### F. Wygaszacz ekranu, zegar flip, zarządzanie ekranem

| Funkcja | Klucze ustawień | Opis |
|---|---|---|
| Wygaszacz | `display.screensaver_enabled`, `screensaver_timeout_sec` | Po zadanym czasie bezczynności panel przechodzi w wygaszacz (domyślnie 15 s). |
| Styl zegara | `display.saver_clock_style`, `saver_clock_color`, `saver_date_color` | Zegar **klasyczny** lub **flip** (cyfry na przewijanych kartach), własne kolory zegara i daty. |
| Zawartość zegara | `display.saver_show_date`, `saver_show_seconds`, `clock_24h` | Data, sekundy i format 12/24 h — niezależnie od siebie. |
| Podświetlenie wygaszacza | `display.saver_brightness`, `wallpaper_dim` | Poziom podświetlenia w wygaszaczu oraz przyciemnienie tapety (bez efektu „oślepienia" w nocy). |
| Tapeta wygaszacza | `display.wallpaper`, `DELETE /api/display/wallpaper` | Wgranie/usunięcie własnej tapety (z flash lub z karty microSD). |
| Wyłączenie ekranu | `display.screen_off_enabled`, `screen_off_timeout_sec` | Pełne wygaszenie podświetlenia po dłuższej bezczynności; wybudzenie dotykiem, z Home Assistant (`/api/display/activity`), przez MQTT (`wake`) lub **ruchem** wykrytym przez wbudowaną kamerę. |
| Tryb nocny | `display.night_mode_enabled`, `night_brightness`, `night_start_min`, `night_end_min`, `night_wake_sec` | W oknie nocnym panel świeci słabiej i nie wybudza się „na stałe" przy każdym dotknięciu. |
| Ekran startowy i OTA | `main/ui/ui_boot_splash.c`, `ui_ota_progress.c` | Ekran powitalny przy starcie i pasek postępu wgrywania firmware na panelu. |

### G. Górny pasek i dolna nawigacja

Górny pasek został przebudowany tak, aby układ był przewidywalny i nie „sklejał" ikon:

| Obszar | Zawartość |
|---|---|
| **Środek** | Zegar — **wyrównany do środka dostępnej przestrzeni** (`main/ui/ui_pages.c`, `ui_topbar_apply_layout()`), z automatycznym doborem rozmiaru czcionki (34 / 28 / 24 / 22 px), aby nigdy nie nachodził na ikony. |
| **Lewa strona** | Data, a obok niej skróty aplikacji: **Radio** i **Pogoda**. |
| **Prawa strona** | Status Wi-Fi, status Home Assistant i — jako ostatnia, zamykająca klaster — **ikona ustawień**. |

- Skróty Radio i Pogoda pojawiają się **tylko wtedy**, gdy ich strony istnieją w układzie
  (`ui_topbar_radio_available()`, `ui_topbar_weather_available()`) i podświetlają się, gdy jesteś
  na danej stronie.
- Ikona ustawień celowo zamyka prawy klaster — wcześniej stała obok radia i sąsiadowała ze skrótami
  aplikacji, co powodowało przypadkowe dotknięcia i bałagan wizualny.
- Kolory i widoczność: `display.topbar_custom_colors`, `topbar_bg`, `topbar_clock`, `topbar_date`,
  `topbar_gear`, `topbar_ha`, `topbar_wifi`, `topbar_show_clock`, `topbar_show_date`,
  `topbar_show_gear`, `topbar_show_status`, `topbar_icon_text` (ikony glifowe z fontu MDI
  `mdi_topbar_24` albo litery, gdy font glifów nie jest wkompilowany).
- Dolny pasek nawigacji: `display.nav_bar_bg`, `nav_bar_border_color`, `nav_button_bg`,
  `nav_button_border_color`, `nav_custom_colors`, `nav_home_active`, `nav_idle_color`,
  `nav_tab_active`, `nav_tab_idle_color`.
- Strony o **stałym identyfikatorze** (`radio`, `pogoda`, kamery, energia, Music Assistant) nie
  zaśmiecają dolnego paska — otwiera się je skrótami z górnego paska lub z pulpitu; w dolnym pasku
  zostają Twoje strony dashboardu (do 6).

### H. Biblioteka kafelków i integracja z Alarmo

- **28 pozycji w menu dodawania kafelka**, pogrupowanych na *Control*, *Data* i *Info* — pełna lista
  w sekcji [Biblioteka kafelków](#biblioteka-kafelków).
- **Kafelki dodane w tym forku**: `binary_sensor`, `cover`, `cover_tile`, `fan`, `lock`, `number`,
  `person_tile`, `presence`, `scene_tile`, `select`, `timer_tile`, `clock_alarm`, `empty_tile`.
- **Integracja z Alarmo** (pełna, sprawdzona na tym panelu — 89/89 pozycji zgodności z wersją
  kuchenną):
  - pięć trybów uzbrojenia (dom / poza domem / noc / wakacje / własny),
  - klawiatura PIN z konfigurowalną długością i automatycznym PIN-em z atrybutów encji,
  - listy **otwartych** i **pominiętych** czujek przed uzbrojeniem,
  - pominięcie opóźnienia wyjścia, potwierdzenie uzbrojenia z `force`,
  - odliczanie opóźnienia wejścia/wyjścia na kafelku,
  - maski gotowości trybów i powód odmowy uzbrojenia czytany wprost ze zdarzeń Alarmo.
- **Kafelek odtwarzacza multimediów**: okładka, tytuł, wykonawca, play/pauza, następny/poprzedni,
  głośność, wybór źródła.
- **Kafelek energii**: przepływ sieć / słońce / bateria / gaz / woda z modelu energii Home Assistant.
- **Wykres**: linia, linia wygładzona, słupki; próbkowanie do 4096 punktów z decymacją; historia
  zapisywana na karcie microSD, aby nie zużywać flash panelu.

### I. Kamery: Home Assistant i kamera lokalna OV5647

**Kamery Home Assistant** (do 4, `camera.*`):

| Funkcja | Opis |
|---|---|
| Źródło | Encja HA (`ha`) albo bezpośredni adres (`http`), z opcjonalnym loginem i hasłem kamery. |
| Strumień | MJPEG (REST), skalowanie klatek — kamery 2K nie zamulają interfejsu. |
| Odświeżanie | 1–60 s, konfigurowalne per kamera. |
| Obsługa błędów | Na kafelku widzisz konkretny powód („brak kamer", błąd pobierania, błąd połączenia, przekroczenie czasu), a nie puste miejsce. |
| REST | `GET /api/cameras`, `/api/camera/status`, `/api/camera/snapshot`, `/api/camera/stream`, `/api/camera/motion` |

**Wbudowana kamera OV5647** (MIPI-CSI, unikat dla tego wariantu sprzętowego):

| Funkcja | Klucze / pliki | Opis |
|---|---|---|
| Włączenie i tryb | `camera.enabled`, `camera.resolution`, `camera.jpeg_quality`, `camera.hflip`, `camera.vflip` | Domyślnie RAW10 1280×960 z binningiem 2×2 @45 fps; tryb zapasowy RAW8 800×800. |
| Migawka | `GET /api/camera/snapshot` | JPEG 1280×960 (jakość 55) ≈ 77–81 kB. |
| Strumień | `GET /api/camera/stream`, `camera.stream_enabled` | MJPEG ≈ 145 kB/s (≈ 2 fps) — celowo ograniczony, aby nie zabierać pasma i RAM. |
| Detekcja ruchu | `camera.motion_threshold`, `camera_motion.*` | Progi, minimalny obszar i czas ruchu, cooldown, ignorowanie zmian oświetlenia, opóźnienie startu oraz **strefy** (`x`, `y`, `w`, `h`) — możesz pilnować tylko wybranego fragmentu obrazu. |
| Wybudzanie ekranu | `camera.motion_wake` | Ruch przed panelem wybudza wyświetlacz. |
| Oszczędzanie zasobów | `main/ui/ui_cameras_page.c` | Strumień i odtwarzanie działają **tylko na stronie kamer**; po jej opuszczeniu podgląd jest zatrzymywany (pauza), więc kamery nie zjadają zasobów w tle. |

### J. Asystent głosowy Xiaozhi

- Folder `main/xiaozhi/` (8 plików): aktywacja w chmurze, klient WebSocket v3, ścieżka audio,
  ekran statusu.
- Stany: bezczynny / łączenie / połączony / słucham / mówię / błąd — widoczne na panelu.
- Mikrofon 16 kHz mono, kodowanie **Opus** (ramka 60 ms), odtwarzanie odpowiedzi TTS na głośniku,
  push-to-talk i przerwanie wypowiedzi asystenta.
- **Aktywacja**: rejestracja urządzenia przez `api.tenclass.net` (Xiaozhi OTA), po której panel
  dostaje adres serwera i token. Device-ID budowany z adresu MAC, dzięki czemu panel jest
  rozpoznawany na stronie Xiaozhi.
- Token i adres serwera trzymane są **wyłącznie w NVS panelu** — w kodzie źródłowym i w plikach
  konfiguracyjnych są tylko placeholdery.
- Ustawienia: `xiaozhi.enabled`, `xiaozhi.server`, `xiaozhi.device`, `xiaozhi.ota_url`.

### K. Radio internetowe i Music Assistant

**Radio internetowe** (`ui_radio_page.c`, `radio/panel_radio.c`):

- Pełnoekranowa siatka stacji, **2–4 kolumny** (klucz konfiguracji z edytora), nazwa i URL każdej
  stacji.
- Do **24 stacji** zdefiniowanych w edytorze WWW; gdy lista jest pusta, używana jest lista wbudowana
  w firmware.
- „Teraz gra", głośność i stop. **Strumień odtwarza Home Assistant** (`media_player.play_media`) —
  panel wysyła tylko URL, dzięki czemu odtwarzanie nie obciąża ESP32 i gra dalej bez buforowania
  w panelu.
- Domyślny odtwarzacz: encja `media_player.*` wybierana z listy encji HA.
- `GET /api/radio` — stan i konfiguracja radia dla edytora WWW.
- Skrót **Radio** w górnym pasku otwiera tę stronę (strona ma stały identyfikator, więc nie ma
  zakładki w dolnym pasku).

**Music Assistant**:

- Dedykowana strona (`ui_music_page.c`) z odtwarzaczem, kolejką i szybkim startem ulubionych mediów.
- Sekcja „Music Assistant" w edytorze WWW: wybór encji odtwarzacza, ustawienia strony i przycisk
  „Apply music config".
- Odtwarzanie ponownie realizuje Home Assistant / Music Assistant, a panel pełni rolę pilota.

### L. Strona pogody

- Osobna strona o **stałym identyfikatorze `pogoda`** (klucz `UI_WEATHER_PAGE_ID`), bez zakładki w
  dolnym pasku — otwiera ją **skrót „Pogoda"** w górnym pasku.
- Konfiguracja w edytorze WWW w sekcji **„Weather page"**: dodajesz dowolne kafelki — kafelek
  pogody (aktualne warunki), kafelek prognozy (3-dniowy) oraz zwykłe kafelki czujników, np.
  temperatury zewnętrznej, wiatru, opadów, ciśnienia ze stacji pogodowej czy z Twoich czujników.
- Skrót w pasku pokazuje encję pierwszego kafelka pogody (domyślnie `weather.dom`) — jeśli chcesz
  inną stację, zmień encję pierwszego kafelka pogody.
- Ikona skrótu pochodzi z fontu glifów pogodowych (chmurka z błyskawicą), a podpowiedzi i teksty
  strony są w pełni przetłumaczone.

### M. Sieć, czas, odporność łącza

| Funkcja | Klucze | Opis |
|---|---|---|
| Wi-Fi | `wifi.ssid`, `wifi.bssid`, `wifi.country_code` | Wybór sieci, przypięcie do BSSID (kilka AP o tej samej nazwie) i kod kraju (poprawny zakres kanałów). |
| Statyczny adres | `wifi.static_enabled`, `wifi.ip`, `wifi.netmask`, `wifi.gateway`, `wifi.dns` | Adresacja ręczna, gdy DHCP jest wyłączone. |
| Provisioning | AP `BETTA-Setup` (192.168.4.1) | Przy pierwszym uruchomieniu panel wystawia otwarty punkt dostępowy z kreatorem (Wi-Fi + Home Assistant + Quick Setup pulpitu). |
| Czas | `time_cfg.timezone`, `time_cfg.ntp_server` | Strefa czasowa i własny serwer NTP; synchronizacja przez SNTP. |
| Health-check łącza | `main/net/net_health.*` | Liczniki Wi-Fi i HA, przyczyny rozłączeń, automatyczne ponawianie połączenia, wykrywanie „martwego" gniazda. |
| Home Assistant | `ha.ws_url`, `ha.rest_enabled` | Podstawowe źródło: WebSocket (natychmiastowe zmiany stanów), zapasowe: REST (prognoza, stany, kamery). Token tylko w NVS. |
| MQTT | `mqtt.enabled`, `host`, `port`, `username`, `use_tls`, `discovery_prefix` | Publikacja stanu panelu, sterowanie z Home Assistant i autodiscovery (szczegóły w sekcji MQTT). |

### N. Diagnostyka, dziennik, watchdogi

| Narzędzie | Endpoint / plik | Co daje |
|---|---|---|
| Pełna diagnostyka | `GET /api/diagnostics` | Zajętość każdej puli pamięci (wewnętrzna, PSRAM, LVGL), stan Wi-Fi i HA, liczniki rozłączeń, brakujące encje, `flash_ops`, statystyki klatek MIPI-DSI (`frame_interval_us_max`, `dsi_underruns`, `flashes`, `washes`, `changes`). |
| Dziennik systemowy | `GET /api/logs`, `POST /api/logs/export`, `DELETE /api/logs` | Logi E/W oraz paniki, rotacja plików, podgląd w przeglądarce, eksport na kartę SD. |
| Zrzuty panik | `GET /api/crash`, `/api/crash/raw`, `POST /api/crash/erase` | Zrzut coredump do flash (ELF) z możliwością pobrania i skasowania. |
| Diagnostyka HA | `GET /api/ha/energy`, `api_ha_diagnostics.c` | Diagnoza połączenia z Home Assistant bez zaglądania do konsoli szeregowej. |
| Dziennik na SD | `diag/system_log.c`, `storage_guard.c` | Logi systemowe i historia wykresów zapisywane na karcie, aby **nie pisać do flash panelu** (patrz sekcja R). |
| Watchdog UI | `diag/system_log.c`, `ui/ui_runtime.c` | Jeśli pętla LVGL przestanie „bić", panel restartuje się sam, zamiast pozostać zawieszony na ścianie. |
| Watchdog klatek DSI | `main/drivers/display_init_panel7.c` + `--wrap=dw_gdma_channel_register_event_callbacks` | Wykrywa przerwy w skanowaniu obrazu i zapisuje je w diagnostyce (patrz sekcja R). |
| Zrzut ekranu | `GET /api/screenshot.bmp` | Zrzut ekranu panelu bezpośrednio z przeglądarki — wygodne przy zgłaszaniu błędów. |
| Monitor logów na żywo | sekcja „Logs" w edytorze WWW | Podgląd logów w przeglądarce, bez kabla szeregowego. |

### O. Karta microSD

| Funkcja | Endpoint | Opis |
|---|---|---|
| Montowanie i status | `GET /api/sd`, `/api/sd/status` | Stan karty, pojemność, błędy montowania. |
| Przeglądanie | `GET /api/sd/files`, `/api/sd/file` | Lista plików i podgląd zawartości. |
| Zapis i kasowanie | `PUT /api/sd`, `DELETE /api/sd/file` | Zapis plików i ich usuwanie, z **ochroną przed wyjściem ze ścieżki** (`../`). |
| Formatowanie | `POST /api/sd/format` | Formatowanie karty z panelu. |
| Eksport logów | `POST /api/sd/logs/export` | Przeniesienie dziennika systemowego na kartę. |
| Tapeta z karty | `ui_screen_saver.c` | Wygaszacz może wyświetlać tapetę wprost z karty microSD. |
| Ochrona pamięci | `diag/storage_guard.c` | Ogranicza tempo zapisów, pilnuje wolnego miejsca i miejsca w LittleFS. |

### P. Kopie zapasowe, OTA, auto-restart

| Funkcja | Endpoint / klucz | Opis |
|---|---|---|
| Kopia zapasowa | `GET /api/backup` | Jeden plik JSON: układ pulpitu, ustawienia publiczne, motywy własne i kamery. **Sekrety (hasło Wi-Fi, token HA, token Xiaozhi) nie są eksportowane.** |
| Odtwarzanie | `POST /api/backup/restore` | Przywrócenie konfiguracji z pliku, z walidacją układu. |
| OTA z pliku | `POST /api/ota/upload` | Wgranie `*.ota.bin` z przeglądarki (bez kabla), z paskiem postępu na panelu. |
| OTA z URL | `POST /api/ota/url`, `GET /api/ota/status` | Aktualizacja z adresu URL i podgląd stanu. |
| Auto-restart | `system.auto_restart_enabled`, `system.auto_restart_hours` | Zaplanowany, czysty restart panelu (domyślnie dobowy) — czyści drobne wycieki i fragmentację sterty. |
| Poziom logów | `system.log_verbosity` | Regulacja „gadatliwości" logów bez przebudowy firmware. |
| Boot guard / licznik startów | `api_diagnostics.c` | Panel raportuje liczbę startów i powód resetu (`boot_count`, `reset_reason`) — łatwo wychwycić ukryte restarty. |

### Q. Lokalizacja i języki

- Języki wbudowane: **polski (domyślny)**, angielski, niemiecki, hiszpański, francuski
  (`main/settings/i18n_store.c`).
- Własne tłumaczenia: pliki JSON w `/littlefs/i18n`, API `GET /api/i18n/languages`,
  `GET /api/i18n/effective`, `PUT /api/i18n/custom`.
- Interfejs panelu i edytor WWW przetłumaczone spójnie; font **Poppins** zawiera pełny zestaw
  polskich znaków diakrytycznych.
- Język ustawiasz z panelu i z edytora (`ui.language`).

### R. Studium przypadku: niebieskie rozbłyski ekranu (usterka MIPI-DSI)

> **To nie była tapeta ani jasny motyw.** Niebieskie rozbłyski to usterka toru obrazu
> (MIPI-DSI), która objawiała się jako bardzo krótki, jasnoniebieski błąd w **każdym** menu,
> bez związku z wygaszaczem, tapetą czy motywem. Poniżej pełna droga do przyczyny i naprawa.

**Objaw.** Co jakiś czas na ekranie pojawiał się na ułamek sekundy jasnoniebieski „błysk"
(firmware logował to jako jasną treść, stąd wcześniejsze mylne podejrzenie tapety wygaszacza).

**Przyczyna (potwierdzona pomiarami na sprzęcie).** Panel MIPI-DSI jest odświeżany 60×/s,
a skanowanie obrazu jest ponawiane **z przerwania** po każdej klatce (`is_last = true`).
**Każdy zapis do wewnętrznej pamięci flash** (`spi_flash_write` / `spi_flash_erase`) wyłącza
na chwilę cache i maskuje przerwania — w tym momencie GDMA nie zdąża z danymi, a most DSI
wysyła wtedy **kolor wypełnienia** (`DSIW_RSV_PROBE`). Efekt na ekranie: jasnoniebieski rozbłysk.
Dodatkowo zbyt mały stos ISR powodował `Stack protection fault` (mcause 27) i restart panelu,
który również wyglądał jak błysk ekranu.

**Jak to zostało udowodnione.** Kolor wypełnienia mostu DSI został chwilowo ustawiony na
**magenta** — po tej zmianie błyski były **magenta**, co jednoznacznie wskazało most DSI,
a nie tapetę czy aplikację. Równolegle licznik operacji flash pokazał, że błyski występują
wyłącznie w chwilach zapisu do flash, a po przeniesieniu zapisów na kartę SD zniknęły całkowicie.

**Naprawa.**

| Krok | Zmiana |
|---|---|
| 1 | **Koniec zapisów do flash w czasie pracy**: dziennik systemowy przeniesiony do `/sd/logs/system.log`, historia wykresów do `/sd/graphs` (karta microSD). |
| 2 | Kolor wypełnienia mostu DSI ustawiony na **czarny** (`0x0000`) — nawet w razie usterki ekran nie „strzela" już niebieskim. |
| 3 | Licznik **`flash_ops`** w `GET /api/diagnostics` — natychmiast widać, ile operacji zapisu/kasowania flash wykonano. |
| 4 | **Watchdog ciągłości obrazu** podpięty przez interpozycję linkera: `-Wl,--wrap=dw_gdma_channel_register_event_callbacks`, funkcja `dsiw_frame_tick()` w IRAM. Było to konieczne, ponieważ komponent `espressif__esp_lvgl_adapter` nadpisywał sloty wywołań zwrotnych mostu DSI. |
| 5 | `CONFIG_FREERTOS_ISR_STACKSIZE=6144` — koniec z przepełnieniem stosu przerwań (mcause 27) i restartami mylonymi z błyskiem. |

**Weryfikacja po naprawie (pomiary z panelu):** ≈ 60,2 klatki/s, `frame_interval_us_max`
16 607–16 622 µs, przerwy w skanowaniu: 0, `dsi_underruns = 0`, `flashes = 0`, `washes = 0`,
`changes = 0`, `flash_ops log n = 14` (wyłącznie przy starcie systemu) oraz 9,7-minutowy test
ciągły bez ani jednego błysku. Jeżeli kiedykolwiek zobaczysz podobny efekt, zajrzyj do
`GET /api/diagnostics` — liczniki `dsi_underruns` / `flashes` od razu powiedzą, czy problem wrócił.

### S. Poprawki stabilności (zawieszanie UI, audio, RAM)

| Problem | Przyczyna | Rozwiązanie |
|---|---|---|
| Panel zawieszał się po ~2 dniach pracy | wyciek/blokada LVGL w zadaniu UI | **Watchdog UI**: jeśli pętla LVGL nie „bije" przez 60 s, panel wykonuje czysty restart |
| Panel „zapychał się" przy gadatliwych czujnikach Zigbee | zalewanie kolejki zdarzeń | **Koalescencja zdarzeń HA** (okno 1 s, 8 slotów) + **eviction najstarszego** zdarzenia zamiast odrzucania nowych |
| Spowolnienia po długim uptime | drobne wycieki i fragmentacja sterty | **Auto-restart** (konfigurowalny, domyślnie dobowy) — `system.auto_restart_enabled`, `auto_restart_hours` |
| Restarty z `Stack protection fault` (mcause 27) | za mały stos przerwań przy łańcuchu ISR na rdzeniu 1 | `CONFIG_FREERTOS_ISR_STACKSIZE=6144` |
| Przepełnienie stosu zadania głównego przy `-O2` | rozwinięta ścieżka `snprintf` w newlib | stos zadania głównego podniesiony do **16 kB** |
| **Audio przyspieszone o ~300 % i zawieszanie odtwarzania** (radio / Music Assistant) | zbyt duże, jednoczesne zapotrzebowanie potoku audio na RAM i czas DMA (mikrofon/głośnik + strumień sieciowy) | ograniczenie zasobów audio w panelu (mniejsze bufory i limit pasma), odtwarzanie strumienia delegowane do Home Assistant, zatrzymywanie potoku po opuszczeniu strony odtwarzania |
| Kamery „zjadały" zasoby w tle | ciągłe dekodowanie MJPEG | podgląd działa **tylko** na stronie kamer; poza nią następuje pauza/stop strumienia |
| Chwilowe zniknięcie Wi-Fi / HA | brak reakcji na „martwe" połączenie | moduł **`net/net_health`**: liczniki, powód rozłączenia, automatyczne ponawianie |

### T. Wyrównanie wariantów, kopie, dokumentacja

- Wszystkie funkcje z pozostałych wariantów (Guition 4" `panels3`, Guition 10" `panel10`) zostały
  **przeniesione i sprawdzone na panelu Waveshare 7B** — checklisty zgodności obejmują m.in.
  Alarmo, MQTT, tapety, wygaszacz, zegar flip, pogodę, kafelki i wygląd.
- Utrzymywane są **pełne kopie zapasowe** (źródła + firmware + dokumentacja + obrazy release)
  oraz **wersja publiczna pozbawiona sekretów** (bez haseł, tokenów, adresów IP i nazw sieci).
- W dokumentacji każdego wariantu opisano: sprzęt, budowanie, wgrywanie, budżet pamięci,
  listę zmian i diagnostykę znanych problemów (m.in. studium rozbłysków DSI powyżej).

### Świadomie poza zakresem

- Panel **nie jest** niezależnym odtwarzaczem multimediów — radio i Music Assistant są sterowane
  przez Home Assistant (panel nie dekoduje audio strumieniowego lokalnie).
- Kamera wbudowana służy do podglądu i detekcji ruchu — **nie nagrywa** wideo na kartę SD.
- Brak obsługi Zigbee/BLE w samym P4 (Home Assistant pozostaje centrum integracji).
- Brak chmury: poza opcjonalnym Xiaozhi panel nie wysyła danych na zewnątrz.

---

## Zrzuty ekranu — panel

Wszystkie zrzuty pobrane bezpośrednio z działającego panelu (`GET /api/screenshot.bmp`, natywne
1024 × 600 px) i pokazują aktualny wygląd interfejsu — kolory tła, tapety i kafelki zależą od
konfiguracji, więc u Ciebie będą inne.

| Strona „Salon” | Oświetlenie RGB | Gniazdka |
|---|---|---|
| ![Strona Salon — kafelki](images/screenshots/panel-01-salon.png) | ![Oświetlenie RGB — kolory](images/screenshots/panel-02-led.png) | ![Sterowanie gniazdkami](images/screenshots/panel-03-sockets.png) |
| **Muzyka — Music Assistant** | **Pogoda** | **Asystent Xiaozhi AI** |
| ![Muzyka — Music Assistant](images/screenshots/panel-04-music.png) | ![Pogoda — prognoza](images/screenshots/panel-05-weather.png) | ![Asystent Xiaozhi AI](images/screenshots/panel-06-xiaozhi.png) |
| **Kamery — podgląd RTSP** | **Radio internetowe** | **Wygaszacz — zegar flip** |
| ![Kamery — podgląd RTSP](images/screenshots/panel-07-cameras.png) | ![Radio internetowe](images/screenshots/panel-08-radio.png) | ![Wygaszacz z zegarem flip](images/screenshots/panel-09-screensaver.png) |

Dolny pasek nawigacji (1024 × 145 px) — skróty do stron i przełącznik wygaszacza, z podświetleniem
aktywnej strony:

![Dolny pasek nawigacji](images/screenshots/panel-10-bottom-nav.png)

## Zrzuty ekranu — edytor WWW

Edytor otwiera się w przeglądarce pod adresem panelu i działa w tym samym układzie na komputerze
(poniższe zrzuty: 1900 px szerokości okna, dwie kolumny — nawigacja i płótno podglądu).

### Zakładka „Layout” — pełny widok

![Edytor WWW — zakładka Layout](images/screenshots/editor-01-layout.png)

### Strony, kafelki i podgląd

| | |
|---|---|
| **Pasek stron i kafelków** — dodawanie stron, kolejność, liczba kafelków<br>![Pasek stron i kafelków](images/screenshots/editor-02-pages-widgets.png) | **Menu „+ Dodaj”** — typy kafelków (światło, gniazdko, roleta, scena, kamera, radio, pogoda, zegar, minutnik…)<br>![Typy kafelków](images/screenshots/editor-04-widget-types.png) |
| **Inspektor kafelka** — encja, ikona, kolory, zachowanie po kliknięciu<br>![Inspektor kafelka](images/screenshots/editor-03-widget-inspector.png) | **Płótno podglądu** — wygląd panelu 1:1 (1024 × 600), kafelki przeciągane myszką<br>![Płótno podglądu](images/screenshots/editor-05-canvas.png) |
| **Kreator szybkiej konfiguracji** — pierwsze uruchomienie, Wi-Fi i Home Assistant krok po kroku<br>![Kreator konfiguracji](images/screenshots/editor-06-setup-wizard.png) | **Wybór światła** — dialog przypisania encji światła do kafelka<br>![Wybór światła](images/screenshots/editor-07-light-picker.png) |

### Sekcje ustawień

| | |
|---|---|
| **Ustawienia → Wi-Fi i sieć** — skan sieci, zapis wielu profili, adres IP, tryb oszczędzania<br>![Wi-Fi i sieć](images/screenshots/editor-08-wifi.png) | **Ustawienia → Home Assistant** — adres, token dostępu, szyfrowanie, test połączenia<br>![Home Assistant](images/screenshots/editor-09-home-assistant.png) |
| **Ustawienia → Xiaozhi AI** — serwer, klucz OTA, tryb asystenta głosowego<br>![Xiaozhi AI](images/screenshots/editor-10-xiaozhi-ai.png) | **Ustawienia → Kamery** — kamery z HA i strumienie RTSP, kanały, odświeżanie<br>![Kamery](images/screenshots/editor-11-cameras.png) |
| **Ustawienia → Kamera wbudowana** — czujnik, ekspozycja, detekcja ruchu<br>![Kamera wbudowana](images/screenshots/editor-12-builtin-camera.png) | **Ustawienia → Czas** — strefa, serwer NTP, format 12/24 h, synchronizacja<br>![Czas](images/screenshots/editor-13-time.png) |
| **Ustawienia → Ekran i wygaszacz** — jasność, przejścia stron, wygaszacz, styl zegara, tapeta *(sekcja jest długa — przewijana)*<br>![Ekran i wygaszacz](images/screenshots/editor-14-display-screensaver.png) | **Ustawienia → Karta microSD** — montowanie, formatowanie, pliki, tapeta z karty, eksport logów<br>![Karta microSD](images/screenshots/editor-15-sd-card.png) |
| **Ustawienia → Strony** — wygląd strony, tapeta, motyw strony, przejścia<br>![Strony](images/screenshots/editor-16-pages-transitions.png) | **Ustawienia → MQTT** — broker, dane logowania, autodiscovery dla Home Assistant<br>![MQTT](images/screenshots/editor-17-mqtt.png) |
| **Ustawienia → Interfejs** — kolory górnego i dolnego paska, widoczność ikon<br>![Interfejs](images/screenshots/editor-18-ui.png) | **Ustawienia → Motyw** — motywy dzień/noc, własne palety, akcenty<br>![Motyw](images/screenshots/editor-19-theme.png) |
| **Ustawienia → AP konfiguracyjny** — tryb punktu dostępu i jego hasło<br>![AP konfiguracyjny](images/screenshots/editor-20-config-ap.png) | **Ustawienia → Aktualizacja firmware** — OTA z URL lub z pliku, z możliwością wycofania<br>![Aktualizacja firmware](images/screenshots/editor-21-firmware-update.png) |
| **Ustawienia → System** — auto-restart, boot guard, restart urządzenia<br>![System](images/screenshots/editor-22-system.png) | **Ustawienia → Kopia zapasowa** — pełny backup i odtwarzanie ustawień z pliku JSON<br>![Kopia zapasowa](images/screenshots/editor-23-backup.png) |
| **Ustawienia → Diagnostyka** — pule pamięci, liczniki łączy, stan zadań, watchdog klatek<br>![Diagnostyka](images/screenshots/editor-24-diagnostics.png) | **Ustawienia → Logi** — dziennik systemowy, pobieranie i czyszczenie<br>![Logi](images/screenshots/editor-25-logs.png) |

> Dane w powyższych zrzutach zostały zamaskowane — nazwa sieci Wi-Fi, adresy IP, adresy serwerów
> i hasła na obrazkach są przykładowe.

---

## Obsługiwany sprzęt

| Parametr | Wartość |
|---|---|
| Płytka | Waveshare **ESP32-P4-WIFI6-Touch-LCD-7B** |
| Wyświetlacz | 7", 1024 × 600, **MIPI-DSI**, kontroler **EK79007**, RGB565 |
| Dotyk | pojemnościowy **GT911** (do 5 punktów), I²C: SCL = GPIO8, SDA = GPIO7 |
| SoC | **ESP32-P4** rewizja < 3.0, 360 MHz (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, `REV_MIN_100`) |
| Wi-Fi | przez koprocesor **ESP32-C6** (SDIO / ESP-Hosted + `esp_wifi_remote`, Wi-Fi 6) |
| Pamięć | **32 MB flash** (QIO/DIO, adresowanie 4-bajtowe), PSRAM HEX 200 MHz |
| Audio | kodek **ES8311** — wbudowany mikrofon i głośnik |
| Kamera | **OV5647** na MIPI-CSI (RAW10 1280×960 binning 2×2 @45 fps, zapasowo RAW8 800×800) |
| Peryferia | slot karty **microSD** (SDMMC + wewnętrzny LDO) |
| SDK | **ESP-IDF v5.5.x** (projekt budowany na 5.5.5) |
| Wariant budowy | `panel7` → `betta-ha-panel-7b`, wersja `v0.8.2-7b` |

> Pozostałe warianty w repozytorium (`panel4`, `panel10`, `panels3`, `s3`) dotyczą innych
> płyt; ten README opisuje wyłącznie panel Waveshare 7".

## Funkcje przejęte z upstreamu

- Silnik pulpitu i **edytor WWW** w przeglądarce (bez YAML, bez rekompilacji).
- Integracja z Home Assistant: **WebSocket** (zdarzenia) + **REST** (prognoza, stany, kamery).
- Biblioteka kafelków: sensor, przycisk, suwak, wykres, światło, ogrzewanie, pogoda, lista zadań
  (todo), odtwarzacz multimediów, Roborock, pusty kafelek.
- Motywy, walidacja układu, zapis w LittleFS, provisioning `BETTA-Setup`, OTA z przeglądarki,
  ekran startowy, pasek postępu OTA, zrzut ekranu, wielojęzyczność, MQTT z autodiscovery,
  panel energii.

## Pierwsze uruchomienie

1. **Wgraj firmware** (z katalogu [`firmware/`](firmware/README.md) — jeden obraz `factory`
   albo cztery pliki; zwróć uwagę na `--flash_size 32MB`).
2. Po starcie panel wystawi otwartą sieć **`BETTA-Setup`** — połącz się z nią telefonem lub
   komputerem i wejdź na **`192.168.4.1`**.
3. Podaj **nazwę i hasło Wi-Fi**, adres **Home Assistant** (np. `ws://homeassistant.local:8123/api/websocket`)
   oraz **token dostępu HA**. Token i hasła zostają w NVS panelu.
4. Uruchom **Quick Setup** w edytorze WWW — powstanie startowy pulpit z Twoimi encjami.
5. Otwórz `http://<adres-panelu>` i zbuduj własny układ: strony, kafelki, wygląd, motywy.
6. Opcjonalnie: włącz kamerę i detekcję ruchu, skonfiguruj radio / Music Assistant, stronę
   pogody, Xiaozhi, kartę microSD, MQTT i automatyczne kopie zapasowe.

## Budowanie ze źródeł

Wymagania: **ESP-IDF v5.5.x**, Python 3.11, PowerShell (skrypty pomocnicze), ~2 GB miejsca
na komponenty pobierane przez menedżer komponentów.

```powershell
# (jednorazowo) środowisko ESP-IDF w tej sesji
. $env:IDF_PATH\export.ps1

# Budowanie wariantu Waveshare 7B (panel7)
idf.py -B build-panel7 `
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.panel7" `
  -D SDKCONFIG="sdkconfig.panel7" build

# Wgranie po kablu (podaj swój port)
idf.py -B build-panel7 -p COM5 -b 460800 flash

# Paczka release: obraz factory + obraz OTA (z aktywnego środowiska ESP-IDF)
# Gotowe pliki są już w firmware/ — ten krok robisz tylko po własnej zmianie kodu.
pwsh tools\make_factory_bin.ps1 -BuildDir build-panel7 `
  -OutFile firmware\betta-ha-panel-7b.factory.bin `
  -OtaOutFile firmware\betta-ha-panel-7b.bin
```

Ważne przy budowaniu:

- W konsoli Windows ustaw **stronę kodową 65001 (UTF-8)** — `idf.py` wysypuje się na `cp1250`.
- **Zamknij monitor szeregowy** przed wgraniem (otwarcie portu liniami DTR/RTS resetuje P4).
- Flash **nie kasuje** NVS ani LittleFS — ustawienia, układ i motywy zostają.
- Nazwa komponentu głównego (`main/idf_component.yml`) jest **generowana** z
  `main/idf_component.panel7.yml` przez `CMakeLists.txt` i nie jest przechowywana w repozytorium.
- Patche dla ESP-Hosted z katalogu `patches/` są nakładane automatycznie
  (`cmake/apply_vendor_patches.cmake`).
- `tools/make_factory_bin.ps1` to skrypt upstreamu dla wariantów `panel4` / `panel10` / `panels3`;
  dla tego projektu wywołuj go z jawnymi ścieżkami wyjściowymi (jak wyżej). Bez `-OutFile`
  i `-OtaOutFile` zapisuje obrazy w `release/` pod nazwą `betta86-ha-panel-<wersja>.factory.bin`.

## Wgrywanie firmware i budżet pamięci flash

Gotowe obrazy leżą w katalogu [`firmware/`](firmware/README.md) (opis i sumy SHA256 w
[`firmware/README.md`](firmware/README.md)).

**Wariant A — jeden obraz (najwygodniej):**

```powershell
esptool.py --chip esp32p4 -p COM5 -b 460800 write_flash `
  --flash_mode dio --flash_size 32MB --flash_freq 80m `
  0x0 firmware\betta-ha-panel-7b.factory.bin
```

**Wariant B — cztery pliki (jak w `idf.py flash`):**

```powershell
esptool.py --chip esp32p4 -p COM5 -b 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 32MB --flash_freq 80m `
  0x2000  firmware\bootloader.bin `
  0x8000  firmware\partition-table.bin `
  0xf000  firmware\ota_data_initial.bin `
  0x20000 firmware\betta-ha-panel-7b.bin
```

**Sekret poprawnego wgrania:** `--flash_size 32MB`. Ten panel ma 32 MB flash z adresowaniem
4-bajtowym — przy innym rozmiarze bootloader zgłasza `exceeds flash chip size` i panel wpada
w pętlę restartów.

**Tabela partycji** ([partitions.csv](partitions.csv)):

| Partycja | Offset | Rozmiar | Przeznaczenie |
|---|---|---|---|
| `nvs` | 0x9000 | 24 kB | Ustawienia runtime i **sekrety** (hasło Wi-Fi, token HA, token Xiaozhi). |
| `otadata` | 0xF000 | 8 kB | Wskaźnik aktywnej partycji aplikacji (OTA). |
| `phy_init` | 0x11000 | 4 kB | Kalibracja RF. |
| `factory` | 0x20000 | 9 MB | Aplikacja główna (slot fabryczny). |
| `ota_0` | 0x920000 | 9 MB | Slot aktualizacji A. |
| `ota_1` | 0x1220000 | 8 MB | Slot aktualizacji B. |
| `coredump` | 0x1A20000 | 1 MB | Zrzuty panik (ELF). |
| `storage` | 0x1B20000 | ~4,9 MB | **LittleFS**: układ pulpitu, motywy, tłumaczenia, tapety, logi. |

**Budżet:** aplikacja `betta-ha-panel-7b.bin` zajmuje ≈ **4,88 MB** z 9 MB slotu, czyli
**ponad połowa partycji pozostaje wolna** — jest miejsce na dalszy rozwój bez zmiany tabeli
partycji. Interfejs WWW jest serwowany w wersji **spakowanej gzip** (mniejszy transfer i szybsze
ładowanie edytora), a logi i historia wykresów trafiają na kartę microSD, co dodatkowo odciąża
pamięć flash.

## Sekcje edytora WWW

Edytor otwiera się pod `http://<adres-panelu>` i zawiera następujące sekcje:

| Sekcja | Do czego służy |
|---|---|
| **Pages** | Dodawanie, kolejność i nazwy stron pulpitu (do 6 stron + strony stałe). |
| **Page look** | Wygląd strony: tło, gradient, tapeta, przyciemnienie, motyw i presety strony. |
| **Widgets** | Dodawanie i konfiguracja kafelków (27 typów) oraz **Quick Setup** gotowego pulpitu. |
| **Inspector** | Właściwości zaznaczonego kafelka: encja, ikona, teksty, wygląd, akcje. |
| **Canvas** | Wizualny podgląd pulpitu z przeciąganiem i zmianą rozmiaru kafelków. |
| **Energy Page** | Konfiguracja strony energii (przepływy sieć / słońce / bateria / gaz / woda). |
| **Music Assistant** | Wybór odtwarzacza i układ strony muzyki. |
| **Internet Radio** | Domyślny odtwarzacz, liczba kolumn (2–4) i lista stacji (do 24). |
| **Weather page** | Kafelki strony pogody (pogoda, prognoza, czujniki). |
| **Wi-Fi** | Wybór sieci (skan), hasło, BSSID, kod kraju, adresacja statyczna. |
| **Home Assistant** | Adres WebSocket, token, tryb REST, diagnostyka połączenia. |
| **MQTT / Home Assistant** | Broker, port, użytkownik, TLS, prefiks discovery. |
| **Xiaozhi AI** | Włączenie asystenta, serwer, urządzenie, URL aktywacji. |
| **Kamery** | Kamery HA: encja lub URL, logowanie, odświeżanie, skalowanie. |
| **Built-in camera** | Kamera OV5647: rozdzielczość, jakość, odbicia, strumień, detekcja ruchu i strefy. |
| **Time** | Strefa czasowa i serwer NTP. |
| **Display / Screensaver** | Jasność, format zegara, wygaszacz, zegar flip, tryb nocny, wyłączenie ekranu, tapety. |
| **Theme** | Wybór i edycja motywów, automatyczne dzień/noc. |
| **Pages / Page transition** | Animacje i czas przejść między stronami. |
| **microSD card** | Montowanie, przeglądanie, zapis, kasowanie, formatowanie, eksport logów. |
| **Setup AP** | Informacje o punkcie dostępowym konfiguracji (`BETTA-Setup`). |
| **Firmware Update** | Wgranie `*.ota.bin` z pliku lub z adresu URL, podgląd stanu OTA. |
| **System / Settings Actions** | Auto-restart, poziom logów, akcje systemowe (restart panelu). |
| **Backup / Restore** | Eksport i import konfiguracji (bez sekretów). |
| **Diagnostics** | Pamięć, Wi-Fi, HA, brakujące encje, liczniki flash i DSI, stan startów. |
| **Logs** | Monitor logów panelu w przeglądarce, eksport, czyszczenie. |
| **Actions / Choose Light** | Edycja akcji kafelków i wyboru świateł dla scen. |

## Biblioteka kafelków

Menu „+ Add" w edytorze jest pogrupowane; poniżej pełna lista pozycji:

| Grupa | Kafelek | Do czego |
|---|---|---|
| **Control** | Button | Wywołanie dowolnej sceny / usługi Home Assistant. |
| | Slider | Suwak jasności, pozycji, liczby (`light`, `media_player`, `cover`, `number`). |
| | Light Tile | Światło: jasność, temperatura barwowa, RGB (pokazywane tylko gdy encja to potrafi). |
| | Heating Tile | Termostat / ogrzewanie z nastawą i temperaturą bieżącą. |
| | Media Player | Odtwarzacz: okładka, tytuł, play/pauza, następny, głośność, źródło. |
| | Roborock | Odkurzacz: start, pauza, powrót do bazy, status. |
| | Cover | Roleta/żaluzja (sterowanie) oraz **Cover Tile** (wizualizacja pozycji). |
| | Lock | Zamek: zamknij / otwórz / status. |
| | Fan | Wentylator: obroty, tryb, włącz/wyłącz. |
| | Select | Lista wyboru (`select` / `input_select`). |
| | Number | Liczba (`number` / `input_number`). |
| | **Alarm Panel** | Alarmo: tryby uzbrojenia, klawiatura PIN, czujki, opóźnienia. |
| | Scene | Scena Home Assistant. |
| | Person | Osoba: obecność, strefa, zdjęcie. |
| | Timer | Licznik / minutnik. |
| | Clock | Zegar kafelkowy (z datą). |
| **Data** | Sensor | Dowolny czujnik z ikoną i jednostką. |
| | Binary Sensor | Czujnik binarny (drzwi, dym, zalanie) z kolorami stanu. |
| | Presence | Obecność w pomieszczeniu. |
| | Graph | Wykres: linia, linia wygładzona, słupki (do 4096 punktów). |
| | Todo List | Lista zadań Home Assistant. |
| **Info** | Weather | Aktualne warunki pogodowe. |
| | Weather Forecast | Prognoza 3-dniowa. |
| | Empty Tile | Pusty kafelek — separator i tło w układzie. |

## Wygląd — opcje `tile_*` i `page_*`

**Kafelek (28 opcji):**
`tile_bg_color`, `tile_bg_grad_color`, `tile_bg_grad_dir`, `tile_border_color`,
`tile_border_width`, `tile_font_scale`, `tile_icon_color`, `tile_label_color`, `tile_opacity`,
`tile_press_fx`, `tile_press_fx_dim`, `tile_press_fx_scale`, `tile_radius`, `tile_shadow`,
`tile_text_color`, `tile_title_color`, `tile_value_color`.

**Strona (11 opcji):**
`page_bg_color`, `page_bg_grad_color`, `page_bg_grad_dir`, `page_delay`, `page_dim`, `page_size`,
`page_style`, `page_theme`, `page_transition`, `page_transition_ms`, `page_wallpaper`.

## Najważniejsze ustawienia panelu

Ustawienia zapisywane są w NVS (a nie w kodzie) i wystawiane jako JSON przez `GET /api/settings`
oraz zapisywane przez **`PUT /api/settings`**. Grupy:

| Grupa | Przykładowe klucze |
|---|---|
| `display` | `brightness`, `clock_24h`, `screensaver_enabled`, `screensaver_timeout_sec`, `saver_clock_style`, `saver_brightness`, `saver_show_date`, `saver_show_seconds`, `saver_wallpaper_dim`, `screen_off_enabled`, `screen_off_timeout_sec`, `night_mode_enabled`, `night_brightness`, `night_start_min`, `night_end_min`, `night_wake_sec`, `page_transition`, `page_transition_ms`, `theme_auto_enabled`, `theme_day_id`, `theme_night_id`, `tile_press_fx`, `value_anim`, `value_anim_ms`, `topbar_*`, `nav_*` |
| `camera` | `enabled`, `resolution`, `jpeg_quality`, `hflip`, `vflip`, `motion_threshold`, `motion_wake`, `stream_enabled` |
| `camera_motion` | `cooldown_ms`, `ignore_lighting`, `min_area`, `min_duration_ms`, `start_delay_ms`, `zones[]` |
| `ha` | `ws_url`, `rest_enabled` |
| `mqtt` | `enabled`, `host`, `port`, `username`, `use_tls`, `discovery_prefix` |
| `wifi` | `ssid`, `bssid`, `country_code`, `static_enabled`, `ip`, `netmask`, `gateway`, `dns` |
| `time_cfg` | `timezone`, `ntp_server` |
| `ui` | `language` (`pl`, `en`, `de`, `es`, `fr`) |
| `xiaozhi` | `enabled`, `server`, `device`, `ota_url` |
| `storage` | `sd_enabled` |
| `system` | `log_verbosity`, `auto_restart_enabled`, `auto_restart_hours` |

## API HTTP

Wszystkie zasoby serwowane są **wyłącznie w wersji gzip** — przy testach z konsoli używaj
`curl --compressed`.

**GET**

| Endpoint | Opis |
|---|---|
| `/`, `/app.js`, `/styles.css`, `/favicon.ico` | Edytor WWW. |
| `/api/layout` | Układ pulpitu (strony, kafelki). |
| `/api/entities` | Lista encji Home Assistant. |
| `/api/ha/light_entities`, `/api/ha/energy` | Encje świateł i model energii. |
| `/api/state` | Bieżące stany encji. |
| `/api/settings` | Ustawienia runtime. |
| `/api/sd`, `/api/sd/status`, `/api/sd/files`, `/api/sd/file` | Karta microSD. |
| `/api/pages`, `/api/themes`, `/api/themes/active`, `/api/themes/get` | Strony i motywy. |
| `/api/cameras`, `/api/camera/status`, `/api/camera/snapshot`, `/api/camera/stream`, `/api/camera/motion` | Kamery (HA i wbudowana). |
| `/api/diagnostics`, `/api/status`, `/api/version` | Diagnostyka i informacje o wersji. |
| `/api/logs`, `/api/crash`, `/api/crash/raw` | Dziennik i zrzuty panik. |
| `/api/backup`, `/api/ota/status` | Kopia zapasowa i stan OTA. |
| `/api/i18n/languages`, `/api/i18n/effective` | Języki i aktywne tłumaczenia. |
| `/api/screenshot.bmp`, `/api/wifi/scan`, `/api/display/wallpaper`, `/api/radio` | Zrzut ekranu, skan Wi-Fi, tapeta, radio. |

**PUT:** `/api/layout`, `/api/settings`, `/api/sd`, `/api/themes/active`, `/api/themes/custom`,
`/api/cameras`, `/api/i18n/custom`

**POST:** `/api/backup/restore`, `/api/crash/erase`, `/api/display/activity`,
`/api/display/wallpaper`, `/api/pages/activate`, `/api/ota/upload`, `/api/ota/url`,
`/api/sd/format`, `/api/sd/logs/export`

**DELETE:** `/api/logs`, `/api/sd/file`, `/api/themes/custom`, `/api/ha/light_entities`,
`/api/display/wallpaper`

## MQTT — tematy, polecenia, encje discovery

| Element | Wartość |
|---|---|
| Temat bazowy | `betta_panel/<sufiks-węzła>` |
| Stan | `<baza>/state` (JSON ze stanem panelu) |
| Polecenia | `<baza>/set/<klucz>` |
| Dostępność | `<baza>/status` (`online` / `offline`) |
| Discovery HA | `<prefiks>/<komponent>/<object_id>/config` |

**Klucze poleceń (24):** `brightness`, `clock_24h`, `page`, `page_transition`,
`page_transition_ms`, `saver_brightness`, `saver_show_date`, `saver_show_seconds`,
`screen_off_enabled`, `screen_off_timeout_sec`, `screensaver_enabled`, `screensaver_timeout_sec`,
`tile_press_fx`, `tile_press_fx_dim`, `tile_press_fx_scale`, `topbar_custom_colors`,
`topbar_icon_text`, `topbar_show_clock`, `topbar_show_date`, `topbar_show_gear`,
`topbar_show_status`, `value_anim`, `value_anim_ms`, `wake`.

**Encje wykrywane automatycznie (~23):** wygaszacz, wyłączenie ekranu, zegar 24 h, sekundy, data,
jasność, jasność wygaszacza, czas wygaszacza, czas wyłączenia ekranu, wybudzenie ekranu, przejście
strony (+ czas), efekt wciśnięcia (+ przygaszenie, + skala), animacja wartości (+ czas), zegar
w górnym pasku, data w górnym pasku, ikona ustawień, ikony statusu, tryb ikon tekstowych i własne
kolory paska.

## Uwagi i pułapki

- **`--flash_size 32MB`** — obowiązkowe (patrz sekcja o wgrywaniu).
- **P4 rewizja < 3.0** — `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` / `REV_MIN_100` musi zostać włączone.
- **Monitor szeregowy** przed wgraniem musi być zamknięty.
- **Strona kodowa 65001** w konsoli Windows przed uruchomieniem `idf.py`.
- **Zasoby WWW tylko gzip** — testuj `curl --compressed`.
- **Logi i historia wykresów na karcie SD** — pisanie do wewnętrznego flash w czasie pracy
  powodowało rozbłyski obrazu (sekcja R). Bez karty panel też działa, ale wtedy część historii
  nie jest zapisywana.
- **Kamera i strumienie** — podgląd wyłącza się po opuszczeniu strony kamer (celowo, dla
  oszczędzania RAM i pasma).
- **Punkt dostępowy `BETTA-Setup` jest otwarty** — używaj go tylko w zaufanej sieci i po
  konfiguracji zmień ustawienia Wi-Fi.
- **Auto-restart** — dla paneli wiszących „na zawsze" warto zostawić włączony (dobowy restart
  czyści drobne wycieki).
- **Ustawienia zapisuj metodą PUT** (`/api/settings`), a nie POST.
- **`/api/diagnostics`** to pierwsze miejsce, do którego warto zajrzeć przy każdej nietypowej
  sytuacji (zawieszenie, brak encji, błyski ekranu, restarty).

## Struktura projektu

```
.
├─ CMakeLists.txt            # wybór wariantu (panel7 → betta-ha-panel-7b, v0.8.2-7b)
├─ CMakePresets.json
├─ partitions.csv            # nvs / otadata / phy_init / factory / ota_0 / ota_1 / coredump / storage
├─ sdkconfig.defaults*       # wspólne + per wariant (panel7 = Waveshare 7")
├─ dependencies.lock
├─ cmake/apply_vendor_patches.cmake
├─ patches/                  # patche ESP-Hosted (Wi-Fi przez ESP32-C6)
├─ components/
│  └─ webui/www/             # edytor WWW: index.html, app.js, styles.css
├─ main/
│  ├─ app_main.c, app_config.h, Kconfig.projbuild
│  ├─ api/                   # serwer HTTP + REST API (api_routes.c = tabela tras)
│  ├─ camera/                # kamera wbudowana OV5647 + baza kamer HA
│  ├─ diag/                  # dziennik, coredump, storage guard, mostek logów LVGL
│  ├─ drivers/               # display_init_panel7.c, touch_init_panel7.c, board_pins.h
│  ├─ ha/                    # klient HA (WebSocket + REST), model encji, energia
│  ├─ layout/                # zapis i walidacja układu pulpitu
│  ├─ mqtt/                  # panel_mqtt.c (tematy, polecenia, discovery)
│  ├─ net/                   # wifi_mgr.c, net_health.*, time_sync.c
│  ├─ radio/                 # panel_radio.c
│  ├─ sd/                    # obsługa karty microSD
│  ├─ settings/              # runtime_settings.c, i18n_store.c
│  ├─ ui/                    # LVGL: strony, kafelki, motywy, wygaszacz, górny pasek
│  │  ├─ theme/              # palety i store motywów
│  │  ├─ widgets/            # 27 typów kafelków (w_*.c)
│  │  └─ fonts/
│  ├─ util/                  # JSON, bufor pierścieniowy, tagi logów
│  └─ xiaozhi/               # asystent głosowy (aktywacja, klient WS v3, audio, UI)
├─ tools/make_factory_bin.ps1
├─ firmware/                 # gotowe obrazy: factory, aplikacja, bootloader, partycje, otadata
├─ images/                   # zrzuty ekranu i logo BETTA OS
├─ release/                  # obrazy upstream dla pozostałych wariantów
└─ LICENSE                   # FNCL-1.1
```

## Prywatność — brak danych osobowych w repozytorium

To repozytorium jest **wersją publiczną (oczyszczoną)** projektu. Świadomie nie zawiera:

- żadnych **haseł, tokenów ani kluczy** (w plikach konfiguracyjnych są wyłącznie placeholdery
  `YOUR_WIFI_SSID`, `YOUR_WIFI_PASSWORD`, `YOUR_HA_WS_URL`, `YOUR_HA_ACCESS_TOKEN`),
- żadnych **adresów IP**, nazw sieci Wi-Fi, nazw hostów ani adresów brokerów MQTT z instalacji autora,
- **notatek wewnętrznych** ani dzienników diagnostycznych z urządzenia.

Wszystkie dane konfiguracyjne trafiają do **NVS** panelu w chwili konfiguracji (Wi-Fi, token HA,
token Xiaozhi). Tokeny **nigdy nie są eksportowane** w kopii zapasowej (`GET /api/backup`).
Katalog `managed_components/` (zależności pobierane przez menedżera komponentów) oraz katalogi
budowania nie są publikowane.

## Licencja

Projekt na licencji **[Federation Non-Commercial License v1.1 (LicenseRef-FNCL-1.1)](LICENSE)** —
Copyright (c) 2026 Cpt_Kirk.

- Użycie **niekomercyjne** jest dozwolone na warunkach licencji.
- Użycie **komercyjne** wymaga pisemnej zgody właściciela praw autorskich.
- Modyfikacje (ten fork) muszą być oznaczone i opisane — patrz
  [release-notes.md](release-notes.md) oraz sekcja
  [Pełna, szczegółowa lista usprawnień](#pełna-szczegółowa-lista-usprawnień).
- Zachowaj plik `LICENSE` oraz nagłówki `Copyright (c) 2026 Cpt_Kirk` w plikach źródłowych.

## Zastrzeżenie / Disclaimer

**EN** — Provided "AS IS", without warranty of any kind. The author is not responsible for damage
to hardware, data loss or incorrect energy/server readings. Configuration values (Wi-Fi password,
Home Assistant token) are stored on the device (NVS/SD), not in this repository — keep your
backups private.

**PL** — Oprogramowanie udostępnione „AS IS", bez jakiejkolwiek gwarancji. Autor nie odpowiada za
uszkodzenia sprzętu, utratę danych ani błędne odczyty energii/serwerów. Dane konfiguracyjne
(hasło Wi-Fi, token Home Assistant) przechowywane są na urządzeniu (NVS/SD), nie w tym
repozytorium — kopie zapasowe trzymaj prywatnie.
