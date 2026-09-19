<!-- SPDX-License-Identifier: LicenseRef-FNCL-1.1 | Copyright (c) 2026 Cpt_Kirk -->
<img src="images/BETTAOS.jpg" alt="BETTA OS Logo" width="10%" />

# Waveshare 7" Wall Panel (BETTA HA Panel fork, `panel7`)

**A fully on-device configurable Home Assistant wall panel for the Waveshare
ESP32-P4-WIFI6-Touch-LCD-7B (7" 1024×600 MIPI-DSI touchscreen).**
You build your dashboard on the panel itself — no YAML editing and no firmware rebuilds.

This repository is a **fork of [BETTA HA Panel v0.8.2](https://github.com/cptkirki/BETTA-HA-PANEL)**
by **Cpt_Kirk**, extended with support for the **Waveshare 7" (ESP32-P4)** panel plus a large set
of our own features: a rich tile/page appearance engine, built-in themes with automatic day/night
switching, a flip-clock screensaver, extra tile types, **Alarmo** integration, **internet radio**
and **Music Assistant**, a dedicated **weather page**, Home Assistant cameras and the
**built-in OV5647 camera** with motion detection, the **Xiaozhi** voice assistant, microSD support,
professional diagnostics (including a MIPI-DSI frame watchdog), backup/restore, MQTT with Home
Assistant discovery, and full Polish localisation.

> 🇵🇱 **Dokumentacja po polsku:** [README.pl.md](README.pl.md)

---

## Table of contents

- [License & attribution (please read)](#license--attribution-please-read)
- [Highlights](#highlights)
- [Detailed list of improvements](#detailed-list-of-improvements)
  - [A. The ESP32-P4 port and the Waveshare 7B hardware](#a-the-esp32-p4-port-and-the-waveshare-7b-hardware)
  - [B. Touch, responsiveness, fluidity](#b-touch-responsiveness-fluidity)
  - [C. Tile look (`tile_*`)](#c-tile-look-tile_)
  - [D. Page look (`page_*`)](#d-page-look-page_)
  - [E. Themes and day/night mode](#e-themes-and-daynight-mode)
  - [F. Screensaver, flip clock, screen management](#f-screensaver-flip-clock-screen-management)
  - [G. Top bar and bottom navigation](#g-top-bar-and-bottom-navigation)
  - [H. Tile library and Alarmo integration](#h-tile-library-and-alarmo-integration)
  - [I. Cameras: Home Assistant and the local OV5647](#i-cameras-home-assistant-and-the-local-ov5647)
  - [J. Xiaozhi voice assistant](#j-xiaozhi-voice-assistant)
  - [K. Internet radio and Music Assistant](#k-internet-radio-and-music-assistant)
  - [L. Weather page](#l-weather-page)
  - [M. Network, time, link resilience](#m-network-time-link-resilience)
  - [N. Diagnostics, logs, watchdogs](#n-diagnostics-logs-watchdogs)
  - [O. microSD card](#o-microsd-card)
  - [P. Backups, OTA, auto-restart](#p-backups-ota-auto-restart)
  - [Q. Localisation and languages](#q-localisation-and-languages)
  - [R. Case study: the blue screen flashes (MIPI-DSI fault)](#r-case-study-the-blue-screen-flashes-mipi-dsi-fault)
  - [S. Stability fixes (UI freezes, audio, RAM)](#s-stability-fixes-ui-freezes-audio-ram)
  - [T. Variant alignment, backups, documentation](#t-variant-alignment-backups-documentation)
  - [Intentionally out of scope](#intentionally-out-of-scope)
- [Screenshots — on the panel](#screenshots--on-the-panel)
- [Screenshots — web editor](#screenshots--web-editor)
- [Supported hardware](#supported-hardware)
- [Main features (inherited from upstream)](#main-features-inherited-from-upstream)
- [Getting started](#getting-started)
- [Building from source](#building-from-source)
- [Flashing and the flash budget](#flashing-and-the-flash-budget)
- [Web editor sections](#web-editor-sections)
- [Tile (widget) library](#tile-widget-library)
- [Appearance — `tile_*` and `page_*` options](#appearance--tile_-and-page_-options)
- [Main panel settings](#main-panel-settings)
- [HTTP API](#http-api)
- [MQTT — topics, commands, discovery entities](#mqtt--topics-commands-discovery-entities)
- [Notes and gotchas](#notes-and-gotchas)
- [Project structure](#project-structure)
- [Privacy — no personal data in this repository](#privacy--no-personal-data-in-this-repository)
- [License](#license)
- [Disclaimer / Zastrzeżenie](#disclaimer--zastrzeżenie)

---

## License & attribution (please read)

- Original project: **BETTA HA Panel v0.8.2** — Copyright (c) 2026 **Cpt_Kirk**.
- License: **[LicenseRef-FNCL-1.1](LICENSE)** (Federation Non-Commercial License v1.1) —
  **non-commercial use only**. Any commercial use requires a separate written licence from the
  copyright holder (see §11 of the licence).
- **This fork modifies the original software.** As required by §3 of the licence, all changes are
  clearly marked and described in
  [Detailed list of improvements](#detailed-list-of-improvements)
  and in [release-notes.md](release-notes.md).
- The original `SPDX-License-Identifier: LicenseRef-FNCL-1.1` and `Copyright (c) 2026 Cpt_Kirk`
  headers are preserved in every source file.
- This is a private project and **not** an official BETTA HA Panel release; the upstream version
  number is intentionally kept (`v0.8.2` plus the variant suffix `-7b`).

---

## Highlights

| Feature | Description |
|---|---|
| 🖥 **New hardware variant** | Full support for the **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B**: 7" 1024×600 MIPI-DSI (EK79007), GT911 touch, 32 MB flash (4-byte addressing), Wi-Fi 6 via the ESP32-C6, microSD, ES8311 codec. |
| 🎨 **Tile appearance engine** | Background, gradient, border, radius, opacity, shadow, font scale and five independent text colours (title / entity label / value / icon / whole tile). Presets, four "copy look" actions and reset. Icon colour is re-applied per state (OFF = grey, ON = yellow), so a colour can never "stick" after an entity refresh. |
| 🖼 **Page look** | Page background colour, gradient, panel wallpaper, wallpaper dimming, per-page theme override and eight page presets. |
| 🌗 **Themes + day/night** | Seven built-in themes (`dark_v2`, `classic_v1`, `light`, `ocean`, `contrast`, `oled`, `retro`), custom themes in the editor, and automatic day/night switching inside a configurable time window. |
| ⏰ **Flip-clock screensaver** | Classic or flip clock, 12 h / 24 h, date, seconds, custom colours, dimming, custom wallpaper, night mode and full screen-off. |
| 🧭 **Rebuilt top bar** | Clock **perfectly centred**, **Radio** and **Weather** shortcuts on the left, Wi-Fi / Home Assistant status plus the settings icon on the right. Shortcuts only appear when their pages exist in the layout. |
| 📻 **Internet radio** | Full-screen station grid (2–4 columns), now playing, volume and stop; Home Assistant plays the stream (`media_player.play_media`), the panel only sends the URL. |
| 🎵 **Music Assistant** | Dedicated page with player selection, queue and quick start of favourite media. |
| 🌤 **Weather page** | A separate page with the fixed id `pogoda` (no bottom-bar tab — opened from the top-bar chip), configured from the web editor with weather, forecast and sensor tiles. |
| 📷 **Cameras** | Home Assistant cameras (REST/MJPEG, authentication, scaling, 1–60 s refresh) **and the built-in OV5647** (MIPI-CSI) with motion detection and screen wake; previews run only while the cameras page is open. |
| 🗣 **Xiaozhi AI** | Voice assistant (WebSocket v3 protocol, Opus, ES8311 microphone, push-to-talk, abort) with cloud activation; the token lives in NVS only. |
| 📊 **Diagnostics** | Per-pool memory statistics, Wi-Fi/HA link counters, missing entities, in-browser system log, panic dumps, a **MIPI-DSI frame watchdog** and a flash-operation counter — the tools that uncovered the screen-flash root cause. |
| 💾 **Backup / restore** | The whole configuration in a single JSON file — layout, public settings, themes and cameras. **Secrets are never exported.** |
| 🗂 **microSD card** | Mounting, formatting, browsing and deleting files, screensaver wallpaper from the card, log export, path-traversal protection. |
| 📡 **MQTT + discovery** | 24 command keys, `state` / `set/+` / `status` topics and ~23 entities auto-discovered by Home Assistant. |
| ⚡ **Smaller and faster** | Built with `-O2`, web UI served gzip-compressed, less RAM pressure thanks to reduced logging and LVGL/fonts in PSRAM. |
| 🇵🇱 **Polish language** | Polish is the default; complete PL/EN/DE/ES/FR translations for both panel and editor, Poppins fonts with Polish diacritics, custom translation JSON. |

---

## Detailed list of improvements

### A. The ESP32-P4 port and the Waveshare 7B hardware

| Item | What was done |
|---|---|
| **Build variant** | New `panel7` variant (`sdkconfig.defaults.panel7`, `main/idf_component.panel7.yml`, `CMakeLists.txt`, `CMakePresets.json`); `PROJECT_VER = v0.8.2-7b`, project `betta-ha-panel-7b`. |
| **Display** | MIPI-DSI init with the **EK79007** controller, 1024×600, RGB565, panel code in `main/drivers/display_init_panel7.c`. |
| **Touch** | **GT911** (up to 5 points) on I²C (SCL = GPIO8, SDA = GPIO7 — bus shared with the camera SCCB), `main/drivers/touch_init_panel7.c`. |
| **P4 revision** | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y` (P4 < rev 3.0, 360 MHz) — without this the bootloader refuses to start. |
| **32 MB flash** | Quad flash with 4-byte addressing: `CONFIG_IDF_EXPERIMENTAL_FEATURES=y` + `CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH=y`. Flashing **requires** `--flash_size 32MB`; with the wrong size the panel boot-loops with `exceeds flash chip size`. |
| **PSRAM** | PSRAM HEX 200 MHz; LVGL buffers and fonts are allocated in PSRAM (`main/ui/lv_psram_mem.c`) so internal SRAM stays available for stacks and DMA. |
| **Wi-Fi** | ESP32-C6 over SDIO (ESP-Hosted + `esp_wifi_remote`) with **two local patches** (`patches/esp_hosted_2.11.7_transport_tx_graceful.patch`, `patches/esp_hosted_2.11.7_sdio_streaming_rx_graceful.patch`), applied automatically by `cmake/apply_vendor_patches.cmake`. They allow the bundled C6 firmware to differ in version from the host stack. |
| **Audio** | **ES8311** codec (microphone + speaker), shared by the panel UI and the Xiaozhi assistant. |
| **Camera** | Built-in **OV5647** on MIPI-CSI (RAW10 1280×960 with 2×2 binning @45 fps by default, RAW8 800×800 fallback), driven by `esp_video` + `esp_cam_sensor`. |
| **microSD** | SDMMC with the internal LDO, FATFS with long file names (`CONFIG_FATFS_LFN_HEAP=y`, `MAX_LFN=255`). |
| **Optimisation** | `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (`-O2`) — snappier UI and smaller firmware. |
| **16 kB main stack** | With `-O2` the inlined newlib `snprintf` path can outgrow the default main task stack, so it was raised to 16 kB. |
| **6144 B ISR stack** | `CONFIG_FREERTOS_ISR_STACKSIZE=6144` — an ISR chain on core 1 used to walk its stack down to the last byte, causing a `Stack protection fault` (mcause 27) and a reboot. |
| **HTTP authentication** | `CONFIG_ESP_HTTP_CLIENT_ENABLE_DIGEST_AUTH=y` — camera snapshots behind a login/password. |
| **VFS / ISP** | `CONFIG_VFS_MAX_COUNT=16` (VFS registration by `esp_video`), `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y`. |
| **Logging and panics** | `CONFIG_LOG_MAXIMUM_LEVEL=3`, LVGL→log bridge (`CONFIG_LV_USE_LOG=y`, WARN level), panic dumps into the coredump partition (ELF format, no DRAM capture, `CHECK_BOOT`, 24 tasks). |

### B. Touch, responsiveness, fluidity

| Improvement | Key / file | Effect |
|---|---|---|
| Tile press feedback | `display.tile_press_fx`, `tile_press_fx_dim`, `tile_press_fx_scale` | The tile dims and/or shrinks under your finger — immediate feedback on a large 7" screen. |
| Value animation | `display.value_anim`, `value_anim_ms` | Numeric changes (temperature, power) animate instead of the text "jumping". |
| Page transitions | `display.page_transition`, `page_transition_ms` | Slide / fade between pages with a configurable duration. |
| Slider hit area | `main/ui/ui_slider_touch.c` | Wider grab area and smooth dragging — brightness and colour temperature stop "running away". |
| Light capability cache | `main/ha/ha_light_capabilities.*` | Light capabilities (brightness / colour temp / RGB) are fetched once and cached instead of queried on every repaint. |
| No UI-loop blocking | `main/ui/ui_runtime.c` | Network work never runs inside the LVGL loop — no micro-stalls under heavy Home Assistant traffic. |
| Less logging | `sdkconfig.defaults.panel7` | INFO logs muted on the console: less DMA/IO pressure, more CPU for LVGL. |

### C. Tile look (`tile_*`)

A complete set of independent appearance options — configured from the web editor inspector
("Widgets" section), per tile or for all tiles at once:

| Option | Meaning |
|---|---|
| `tile_bg_color`, `tile_bg_grad_color`, `tile_bg_grad_dir` | Background colour, gradient end colour and gradient direction (none / vertical / horizontal). |
| `tile_border_color`, `tile_border_width`, `tile_radius` | Border colour and thickness, corner radius (from sharp cards to pills). |
| `tile_opacity`, `tile_shadow` | Tile transparency and shadow — the "glass" look over a colourful wallpaper. |
| `tile_font_scale` | Tile font scaling (large readings from across the room). |
| `tile_icon_color`, `tile_label_color`, `tile_text_color`, `tile_title_color`, `tile_value_color` | Five independent colours: icon, entity label, all text, title, value. |
| `tile_press_fx`, `tile_press_fx_dim`, `tile_press_fx_scale` | Press effect (see section B). |
| "Copy look" actions | Copy the look to a chosen tile, to the whole page, to all pages, plus **reset** — no more repeating settings by hand. |
| State icon colour | The icon is repainted on every state change (`main/ui/ui_tile_style.c`, `ui_widget_factory.c`): off = grey (`#8CA2B5`), on = yellow (`#FFCF6B`). Previously the colour could stick to the value captured when the tile was created. |

### D. Page look (`page_*`)

| Option | Meaning |
|---|---|
| `page_bg_color` | Background colour of the whole page. |
| `page_bg_grad_color`, `page_bg_grad_dir` | Background gradient and its direction. |
| `page_wallpaper` | Use the panel wallpaper as the page background. |
| `page_dim` | Wallpaper dimming (0–90 %) — tiles stay readable. |
| `page_theme` | Force a theme **for this page only** (e.g. a dark cameras page on a bright dashboard). |
| Page presets | `auto` (theme), `midnight`, `deep_sea`, `forest`, `sunset`, `plum`, `wallpaper`, `wallpaper_dim`. |
| Reset | Restore the page look to the theme defaults. |

### E. Themes and day/night mode

- **Seven built-in themes**: `dark_v2` (default), `classic_v1`, `light`, `ocean`, `contrast`,
  `oled`, `retro` (`main/ui/theme/theme_palette.c`).
- **Custom themes**: create, edit, delete and export from the web editor
  (`GET/PUT/DELETE /api/themes*`), stored in LittleFS.
- **Automatic day/night**: `display.theme_auto_enabled`, `theme_day_id`, `theme_night_id` and the
  night window (`night_start_min`, `night_end_min`) — the panel switches to the dark theme in the
  evening and back in the morning on its own.
- **Per-page theme** (`page_theme`) and a **base theme** applied globally by the theme router
  (`main/ui/ui_theme_router.c`).
- **Screensaver theme**: clock and date colours, clock style, dimming and a separate wallpaper.

### F. Screensaver, flip clock, screen management

| Feature | Settings keys | Description |
|---|---|---|
| Screensaver | `display.screensaver_enabled`, `screensaver_timeout_sec` | After the configured idle time the panel enters the screensaver (15 s by default). |
| Clock style | `display.saver_clock_style`, `saver_clock_color`, `saver_date_color` | **Classic** or **flip** clock (digits on flipping cards) with custom clock and date colours. |
| Clock content | `display.saver_show_date`, `saver_show_seconds`, `clock_24h` | Date, seconds and 12/24 h format — each independently. |
| Screensaver backlight | `display.saver_brightness`, `wallpaper_dim` | Backlight level in the screensaver and wallpaper dimming (no "blinding" glare at night). |
| Screensaver wallpaper | `display.wallpaper`, `DELETE /api/display/wallpaper` | Upload/remove your own wallpaper (from flash or from the microSD card). |
| Screen off | `display.screen_off_enabled`, `screen_off_timeout_sec` | Full backlight shutdown after a longer idle period; wake by touch, from Home Assistant (`/api/display/activity`), over MQTT (`wake`) or by **motion** detected by the built-in camera. |
| Night mode | `display.night_mode_enabled`, `night_brightness`, `night_start_min`, `night_end_min`, `night_wake_sec` | Inside the night window the panel is dimmed and does not stay "awake" on every touch. |
| Boot screen and OTA | `main/ui/ui_boot_splash.c`, `ui_ota_progress.c` | A welcome screen at boot and an on-panel firmware upload progress bar. |

### G. Top bar and bottom navigation

The top bar was rebuilt so that the layout is predictable and icons never "stick together":

| Area | Content |
|---|---|
| **Centre** | The clock — **centred within the available space** (`main/ui/ui_pages.c`, `ui_topbar_apply_layout()`), with automatic font sizing (34 / 28 / 24 / 22 px) so that it never overlaps the icons. |
| **Left side** | The date, and next to it the app shortcuts: **Radio** and **Weather**. |
| **Right side** | Wi-Fi status, Home Assistant status and — last, closing the cluster — the **settings icon**. |

- The Radio and Weather shortcuts appear **only when their pages exist** in the layout
  (`ui_topbar_radio_available()`, `ui_topbar_weather_available()`) and highlight while you are on
  that page.
- The settings icon deliberately closes the right-hand cluster — it used to sit next to the radio
  icon and the app shortcuts, which caused accidental taps and visual clutter.
- Colours and visibility: `display.topbar_custom_colors`, `topbar_bg`, `topbar_clock`,
  `topbar_date`, `topbar_gear`, `topbar_ha`, `topbar_wifi`, `topbar_show_clock`,
  `topbar_show_date`, `topbar_show_gear`, `topbar_show_status`, `topbar_icon_text` (glyph icons
  from the MDI font `mdi_topbar_24`, or letters when the glyph font is not compiled in).
- Bottom navigation bar: `display.nav_bar_bg`, `nav_bar_border_color`, `nav_button_bg`,
  `nav_button_border_color`, `nav_custom_colors`, `nav_home_active`, `nav_idle_color`,
  `nav_tab_active`, `nav_tab_idle_color`.
- **Fixed-id pages** (`radio`, `pogoda`, cameras, energy, Music Assistant) do not clutter the
  bottom bar — they are opened from top-bar shortcuts or from the dashboard; the bottom bar keeps
  your dashboard pages (up to 6).

### H. Tile library and Alarmo integration

- **28 entries in the add-tile menu**, grouped into *Control*, *Data* and *Info* — the full list is
  in the [Tile (widget) library](#tile-widget-library) section.
- **Tiles added in this fork**: `binary_sensor`, `cover`, `cover_tile`, `fan`, `lock`, `number`,
  `person_tile`, `presence`, `scene_tile`, `select`, `timer_tile`, `clock_alarm`, `empty_tile`.
- **Alarmo integration** (complete, verified on this panel — 89/89 parity items against the
  kitchen variant):
  - five arm modes (home / away / night / vacation / custom),
  - a PIN keypad with configurable length and a PIN taken automatically from entity attributes,
  - lists of **open** and **bypassed** sensors before arming,
  - skipping the exit delay, arming confirmation with `force`,
  - entry/exit delay countdown shown on the tile,
  - per-mode readiness masks and the arm-refusal reason read straight from Alarmo events.
- **Media player tile**: cover art, title, artist, play/pause, next/previous, volume, source
  selection.
- **Energy tile**: grid / solar / battery / gas / water flows from the Home Assistant energy
  dashboard.
- **Graph**: line, smoothed line, bars; up to 4096 samples with decimation; history is stored on
  the microSD card so the panel flash is not worn out.

### I. Cameras: Home Assistant and the local OV5647

**Home Assistant cameras** (up to 4, `camera.*`):

| Feature | Description |
|---|---|
| Source | A HA entity (`ha`) or a direct URL (`http`), with optional camera login and password. |
| Stream | MJPEG (REST) with frame scaling — 2K cameras do not bog the UI down. |
| Refresh | 1–60 s, configurable per camera. |
| Error handling | The tile shows the actual reason ("no cameras", download error, connection error, timeout) instead of an empty spot. |
| REST | `GET /api/cameras`, `/api/camera/status`, `/api/camera/snapshot`, `/api/camera/stream`, `/api/camera/motion` |

**Built-in OV5647 camera** (MIPI-CSI, unique to this hardware variant):

| Feature | Keys / files | Description |
|---|---|---|
| Enable and mode | `camera.enabled`, `camera.resolution`, `camera.jpeg_quality`, `camera.hflip`, `camera.vflip` | RAW10 1280×960 with 2×2 binning @45 fps by default; RAW8 800×800 as the fallback mode. |
| Snapshot | `GET /api/camera/snapshot` | JPEG 1280×960 (quality 55) ≈ 77–81 kB. |
| Stream | `GET /api/camera/stream`, `camera.stream_enabled` | MJPEG ≈ 145 kB/s (≈ 2 fps) — intentionally throttled so it does not eat bandwidth and RAM. |
| Motion detection | `camera.motion_threshold`, `camera_motion.*` | Thresholds, minimum area and duration, cooldown, ignoring lighting changes, start delay and **zones** (`x`, `y`, `w`, `h`) — you can watch only part of the image. |
| Screen wake | `camera.motion_wake` | Motion in front of the panel wakes the display. |
| Resource saving | `main/ui/ui_cameras_page.c` | The stream and playback run **only on the cameras page**; leaving it stops (pauses) the preview, so cameras do not consume resources in the background. |

### J. Xiaozhi voice assistant

- The `main/xiaozhi/` folder (8 files): cloud activation, the WebSocket v3 client, the audio path
  and a status screen.
- States: idle / connecting / connected / listening / speaking / error — all visible on the panel.
- 16 kHz mono microphone, **Opus** encoding (60 ms frames), TTS replies played on the speaker,
  push-to-talk and interrupting the assistant mid-sentence.
- **Activation**: device registration through `api.tenclass.net` (Xiaozhi OTA), after which the
  panel receives the server address and a token. The device ID is derived from the MAC address, so
  the panel is recognised on the Xiaozhi website.
- The token and server address are kept **only in the panel's NVS** — the source tree and the
  configuration files contain placeholders only.
- Settings: `xiaozhi.enabled`, `xiaozhi.server`, `xiaozhi.device`, `xiaozhi.ota_url`.

### K. Internet radio and Music Assistant

**Internet radio** (`ui_radio_page.c`, `radio/panel_radio.c`):

- A full-screen station grid with **2–4 columns** (configured from the editor), each station's
  name and URL.
- Up to **24 stations** defined in the web editor; when the list is empty, the built-in firmware
  list is used.
- "Now playing", volume and stop. **Home Assistant plays the stream**
  (`media_player.play_media`) — the panel only sends the URL, so playback does not load the ESP32
  and keeps running without buffering on the panel.
- Default player: a `media_player.*` entity picked from the HA entity list.
- `GET /api/radio` — radio state and configuration for the web editor.
- The **Radio** top-bar shortcut opens this page (the page has a fixed id, so it gets no bottom-bar
  tab).

**Music Assistant**:

- A dedicated page (`ui_music_page.c`) with the player, the queue and quick start of favourites.
- A "Music Assistant" section in the web editor: player entity selection, page settings and an
  "Apply music config" button.
- Again, playback is performed by Home Assistant / Music Assistant; the panel is a remote control.

### L. Weather page

- A separate page with the **fixed id `pogoda`** (`UI_WEATHER_PAGE_ID`), with no bottom-bar tab —
  it is opened by the **"Weather" shortcut** in the top bar.
- Configured in the web editor's **"Weather page"** section: add any tiles — a weather tile
  (current conditions), a forecast tile (3-day) and ordinary sensor tiles, e.g. outdoor
  temperature, wind, rainfall or pressure from a weather station or your own sensors.
- The top-bar shortcut shows the entity of the first weather tile (`weather.dom` by default) — if
  you want another station, change the entity of the first weather tile.
- The shortcut icon comes from the weather glyph font (a cloud with a lightning bolt), and the page
  texts and tooltips are fully translated.
- The animated condition icons (Lottie/Meteocons) are optional and are not shipped with this
  repository — see *Building from source*; without them the tiles show the static MDI icons.

### M. Network, time, link resilience

| Feature | Keys | Description |
|---|---|---|
| Wi-Fi | `wifi.ssid`, `wifi.bssid`, `wifi.country_code` | Network selection, pinning to a BSSID (several APs with the same name) and country code (correct channel range). |
| Static address | `wifi.static_enabled`, `wifi.ip`, `wifi.netmask`, `wifi.gateway`, `wifi.dns` | Manual addressing when DHCP is disabled. |
| Provisioning | AP `BETTA-Setup` (192.168.4.1) | On first start the panel brings up an open access point with a setup wizard (Wi-Fi + Home Assistant + dashboard Quick Setup). |
| Time | `time_cfg.timezone`, `time_cfg.ntp_server` | Time zone and a custom NTP server; synchronised over SNTP. |
| Link health check | `main/net/net_health.*` | Wi-Fi and HA counters, disconnect reasons, automatic reconnect, dead-socket detection. |
| Home Assistant | `ha.ws_url`, `ha.rest_enabled` | Primary source: WebSocket (instant state changes); fallback: REST (forecast, states, cameras). The token lives in NVS only. |
| MQTT | `mqtt.enabled`, `host`, `port`, `username`, `use_tls`, `discovery_prefix` | Publishing panel state, control from Home Assistant and autodiscovery (see the MQTT section). |

### N. Diagnostics, logs, watchdogs

| Tool | Endpoint / file | What it gives you |
|---|---|---|
| Full diagnostics | `GET /api/diagnostics` | Usage of every memory pool (internal, PSRAM, LVGL), Wi-Fi and HA state, disconnect counters, missing entities, `flash_ops`, MIPI-DSI frame statistics (`frame_interval_us_max`, `dsi_underruns`, `flashes`, `washes`, `changes`). |
| System log | `GET /api/logs`, `POST /api/logs/export`, `DELETE /api/logs` | E/W logs and panics, file rotation, in-browser preview, export to the SD card. |
| Panic dumps | `GET /api/crash`, `/api/crash/raw`, `POST /api/crash/erase` | Coredump to flash (ELF) with download and erase. |
| HA diagnostics | `GET /api/ha/energy`, `api_ha_diagnostics.c` | Diagnosing the Home Assistant connection without a serial console. |
| Log on SD | `diag/system_log.c`, `storage_guard.c` | System logs and graph history are written to the card so the panel flash is **not** written to (see section R). |
| UI watchdog | `diag/system_log.c`, `ui/ui_runtime.c` | If the LVGL loop stops ticking, the panel reboots itself instead of staying frozen on the wall. |
| DSI frame watchdog | `main/drivers/display_init_panel7.c` + `--wrap=dw_gdma_channel_register_event_callbacks` | Detects scan-out gaps and records them in diagnostics (see section R). |
| Screenshot | `GET /api/screenshot.bmp` | A panel screenshot straight from the browser — handy when reporting problems. |
| Live log monitor | the "Logs" section of the web editor | Log preview in the browser, no serial cable required. |

### O. microSD card

| Feature | Endpoint | Description |
|---|---|---|
| Mount and status | `GET /api/sd`, `/api/sd/status` | Card state, capacity, mount errors. |
| Browsing | `GET /api/sd/files`, `/api/sd/file` | File listing and content preview. |
| Writing and deleting | `PUT /api/sd`, `DELETE /api/sd/file` | Uploading and removing files, with **path-traversal protection** (`../`). |
| Formatting | `POST /api/sd/format` | Formatting the card from the panel. |
| Log export | `POST /api/sd/logs/export` | Moving the system log onto the card. |
| Wallpaper from the card | `ui_screen_saver.c` | The screensaver can display a wallpaper straight from the microSD card. |
| Storage guard | `diag/storage_guard.c` | Rate-limits writes and watches free space and LittleFS headroom. |

### P. Backups, OTA, auto-restart

| Feature | Endpoint / key | Description |
|---|---|---|
| Backup | `GET /api/backup` | A single JSON file: dashboard layout, public settings, custom themes and cameras. **Secrets (Wi-Fi password, HA token, Xiaozhi token) are never exported.** |
| Restore | `POST /api/backup/restore` | Restoring the configuration from a file, with layout validation. |
| OTA from file | `POST /api/ota/upload` | Uploading a `*.ota.bin` from the browser (no cable) with an on-panel progress bar. |
| OTA from URL | `POST /api/ota/url`, `GET /api/ota/status` | Updating from a URL and checking the status. |
| Auto-restart | `system.auto_restart_enabled`, `system.auto_restart_hours` | A scheduled, clean panel restart (daily by default) — clears small leaks and heap fragmentation. |
| Log level | `system.log_verbosity` | Adjusting log verbosity without rebuilding the firmware. |
| Boot guard / boot counter | `api_diagnostics.c` | The panel reports the boot count and the reset reason (`boot_count`, `reset_reason`) — hidden restarts are easy to spot. |

### Q. Localisation and languages

- Built-in languages: **Polish (default)**, English, German, Spanish, French
  (`main/settings/i18n_store.c`).
- Custom translations: JSON files in `/littlefs/i18n`, API `GET /api/i18n/languages`,
  `GET /api/i18n/effective`, `PUT /api/i18n/custom`.
- The panel UI and the web editor are translated consistently; the **Poppins** font contains the
  full set of Polish diacritics.
- The language is set from the panel and from the editor (`ui.language`).

### R. Case study: the blue screen flashes (MIPI-DSI fault)

> **This was not the wallpaper and not a bright theme.** The blue flashes were a fault in the
> display pipeline (MIPI-DSI) that showed up as a very brief light-blue glitch in **every** menu,
> unrelated to the screensaver, wallpaper or theme. Below is the full route to the root cause and
> the fix.

**Symptom.** Every now and then a light-blue "flash" appeared on screen for a fraction of a
second (the firmware logged it as bright content, which is why the screensaver wallpaper was
initially suspected by mistake).

**Root cause (confirmed by on-hardware measurements).** The MIPI-DSI panel is refreshed 60×/s and
the scan-out is re-armed **from an interrupt** after every frame (`is_last = true`). **Any write to
the internal flash** (`spi_flash_write` / `spi_flash_erase`) briefly disables the cache and masks
interrupts — at that moment GDMA cannot keep up with the data, and the DSI bridge then transmits
its **filler colour** (`DSIW_RSV_PROBE`). The effect on screen: a light-blue flash. On top of that,
an undersized ISR stack caused a `Stack protection fault` (mcause 27) and a panel reboot, which
also looked like a screen flash.

**How it was proven.** The DSI bridge filler colour was temporarily set to **magenta** — after
that change the flashes were **magenta**, which pointed unambiguously at the DSI bridge rather than
at a wallpaper or the application. At the same time a flash-operation counter showed that the
flashes occurred only at the moments of flash writes.

**The fix.**

| Step | Change |
|---|---|
| 1 | **No more flash writes at runtime**: the system log moved to `/sd/logs/system.log`, graph history to `/sd/graphs` (microSD card). |
| 2 | The DSI bridge filler colour set to **black** (`0x0000`) — even if a fault occurs, the screen no longer "fires" blue. |
| 3 | A **`flash_ops`** counter in `GET /api/diagnostics` — the number of flash write/erase operations is immediately visible. |
| 4 | An **image-continuity watchdog** hooked in via linker interposition: `-Wl,--wrap=dw_gdma_channel_register_event_callbacks`, with `dsiw_frame_tick()` in IRAM. This was necessary because the `espressif__esp_lvgl_adapter` component overwrote the DSI bridge callback slots. |
| 5 | `CONFIG_FREERTOS_ISR_STACKSIZE=6144` — no more interrupt stack overflows (mcause 27) and no more restarts mistaken for a flash. |

**Verification after the fix (measurements from the panel):** ≈ 60.2 fps, `frame_interval_us_max`
16 607–16 622 µs, scan-out gaps: 0, `dsi_underruns = 0`, `flashes = 0`, `washes = 0`, `changes = 0`,
`flash_ops log n = 14` (boot time only) and a 9.7-minute soak with not a single flash. If you ever
see a similar effect, look at `GET /api/diagnostics` — the `dsi_underruns` / `flashes` counters
will tell you straight away whether the problem is back.

### S. Stability fixes (UI freezes, audio, RAM)

| Problem | Cause | Fix |
|---|---|---|
| The panel froze after ~2 days of uptime | an LVGL leak/block in the UI task | **UI watchdog**: if the LVGL loop does not tick for 60 s, the panel performs a clean restart |
| The panel "choked" on chatty Zigbee sensors | event queue flooding | **HA event coalescing** (1 s window, 8 slots) + **eviction of the oldest** event instead of dropping new ones |
| Slowdowns after long uptime | small leaks and heap fragmentation | **Auto-restart** (configurable, daily by default) — `system.auto_restart_enabled`, `auto_restart_hours` |
| Restarts with `Stack protection fault` (mcause 27) | ISR stack too small for an ISR chain on core 1 | `CONFIG_FREERTOS_ISR_STACKSIZE=6144` |
| Main task stack overflow with `-O2` | the inlined newlib `snprintf` path | the main task stack raised to **16 kB** |
| **Audio playing ~300 % too fast, with playback freezing** (radio / Music Assistant) | the audio pipeline demanded too much RAM and DMA time at once (microphone/speaker + network stream) | audio resources limited in the panel (smaller buffers and a bandwidth cap), stream playback delegated to Home Assistant, pipeline stopped when leaving the playback page |
| Cameras "eating" resources in the background | continuous MJPEG decoding | the preview runs **only** on the cameras page; outside it the stream is paused/stopped |
| Wi-Fi / HA disappearing for a while | no reaction to a dead connection | the **`net/net_health`** module: counters, disconnect reason, automatic retry |

### T. Variant alignment, backups, documentation

- All features from the other variants (Guition 4" `panels3`, Guition 10" `panel10`) were
  **ported and verified on the Waveshare 7B panel** — the parity checklists cover Alarmo, MQTT,
  wallpapers, the screensaver, the flip clock, the weather page, tiles and appearance.
- **Full backups** are maintained (sources + firmware + documentation + release images) along with
  a **secret-free public version** (no passwords, tokens, IP addresses or network names).
- The documentation of every variant describes the hardware, building, flashing, the memory budget,
  the change list and the diagnostics of known problems (including the DSI flash case study above).

### Intentionally out of scope

- The panel is **not** a standalone media player — radio and Music Assistant are driven by Home
  Assistant (the panel does not decode streaming audio locally).
- The built-in camera is for preview and motion detection — it does **not** record video to the
  SD card.
- No Zigbee/BLE support in the P4 itself (Home Assistant remains the integration hub).
- No cloud: apart from the optional Xiaozhi assistant, the panel sends no data outside your network.

---

## Screenshots — on the panel

All captures were taken straight from the running panel (`GET /api/screenshot.bmp`, native
1024 × 600 px) and show the current UI — background colours, wallpapers and tiles depend on your
configuration, so yours will look different.

| “Living room” page | RGB lighting | Sockets |
|---|---|---|
| ![Living room page — tiles](images/screenshots/panel-01-salon.png) | ![RGB lighting — colours](images/screenshots/panel-02-led.png) | ![Socket control](images/screenshots/panel-03-sockets.png) |
| **Music — Music Assistant** | **Weather** | **Xiaozhi AI assistant** |
| ![Music — Music Assistant](images/screenshots/panel-04-music.png) | ![Weather — forecast](images/screenshots/panel-05-weather.png) | ![Xiaozhi AI assistant](images/screenshots/panel-06-xiaozhi.png) |
| **Cameras — RTSP preview** | **Internet radio** | **Screensaver — flip clock** |
| ![Cameras — RTSP preview](images/screenshots/panel-07-cameras.png) | ![Internet radio](images/screenshots/panel-08-radio.png) | ![Screensaver with a flip clock](images/screenshots/panel-09-screensaver.png) |

Bottom navigation bar (1024 × 145 px) — page shortcuts and the screensaver toggle, with the active
page highlighted:

![Bottom navigation bar](images/screenshots/panel-10-bottom-nav.png)

## Screenshots — web editor

The editor opens in any browser at the panel's address and uses the same layout as on a desktop
(captures below: 1900 px window width, two columns — navigation plus the preview canvas).

### “Layout” tab — full view

![Web editor — Layout tab](images/screenshots/editor-01-layout.png)

### Pages, tiles and preview

| | |
|---|---|
| **Page and tile rail** — add pages, reorder them, tile counts<br>![Page and tile rail](images/screenshots/editor-02-pages-widgets.png) | **“+ Add” menu** — tile types (light, socket, cover, scene, camera, radio, weather, clock, timer…)<br>![Tile types](images/screenshots/editor-04-widget-types.png) |
| **Tile inspector** — entity, icon, colours, tap behaviour<br>![Tile inspector](images/screenshots/editor-03-widget-inspector.png) | **Preview canvas** — 1:1 look of the panel (1024 × 600), tiles dragged with the mouse<br>![Preview canvas](images/screenshots/editor-05-canvas.png) |
| **Setup wizard** — first run, Wi-Fi and Home Assistant step by step<br>![Setup wizard](images/screenshots/editor-06-setup-wizard.png) | **Light picker** — dialog that assigns a light entity to a tile<br>![Light picker](images/screenshots/editor-07-light-picker.png) |

### Settings sections

| | |
|---|---|
| **Settings → Wi-Fi & network** — network scan, multiple saved profiles, IP address, power save<br>![Wi-Fi & network](images/screenshots/editor-08-wifi.png) | **Settings → Home Assistant** — address, access token, encryption, connection test<br>![Home Assistant](images/screenshots/editor-09-home-assistant.png) |
| **Settings → Xiaozhi AI** — server, OTA key, voice assistant mode<br>![Xiaozhi AI](images/screenshots/editor-10-xiaozhi-ai.png) | **Settings → Cameras** — HA cameras and RTSP streams, channels, refresh rate<br>![Cameras](images/screenshots/editor-11-cameras.png) |
| **Settings → Built-in camera** — sensor, exposure, motion detection<br>![Built-in camera](images/screenshots/editor-12-builtin-camera.png) | **Settings → Time** — time zone, NTP server, 12/24 h format, sync<br>![Time](images/screenshots/editor-13-time.png) |
| **Settings → Display & screensaver** — brightness, page transitions, screensaver, clock style, wallpaper *(long section — scrollable)*<br>![Display & screensaver](images/screenshots/editor-14-display-screensaver.png) | **Settings → microSD card** — mount, format, files, wallpaper from the card, log export<br>![microSD card](images/screenshots/editor-15-sd-card.png) |
| **Settings → Pages** — page look, wallpaper, per-page theme, transitions<br>![Pages](images/screenshots/editor-16-pages-transitions.png) | **Settings → MQTT** — broker, credentials, Home Assistant autodiscovery<br>![MQTT](images/screenshots/editor-17-mqtt.png) |
| **Settings → UI** — top and bottom bar colours, icon visibility<br>![UI](images/screenshots/editor-18-ui.png) | **Settings → Theme** — day/night themes, custom palettes, accents<br>![Theme](images/screenshots/editor-19-theme.png) |
| **Settings → Config AP** — access point mode and its password<br>![Config AP](images/screenshots/editor-20-config-ap.png) | **Settings → Firmware update** — OTA from URL or file, with rollback<br>![Firmware update](images/screenshots/editor-21-firmware-update.png) |
| **Settings → System** — auto-restart, boot guard, device reboot<br>![System](images/screenshots/editor-22-system.png) | **Settings → Backup** — full backup and restore of settings from a JSON file<br>![Backup](images/screenshots/editor-23-backup.png) |
| **Settings → Diagnostics** — memory pools, link counters, task state, frame watchdog<br>![Diagnostics](images/screenshots/editor-24-diagnostics.png) | **Settings → Logs** — system log, download and clear<br>![Logs](images/screenshots/editor-25-logs.png) |

> The data in these screenshots is masked — the Wi-Fi name, IP addresses, server addresses and
> passwords shown in the images are placeholders.

---

## Supported hardware

| Parameter | Value |
|---|---|
| Board | Waveshare **ESP32-P4-WIFI6-Touch-LCD-7B** |
| Display | 7", 1024 × 600, **MIPI-DSI**, **EK79007** controller, RGB565 |
| Touch | Capacitive **GT911** (up to 5 points), I²C: SCL = GPIO8, SDA = GPIO7 |
| SoC | **ESP32-P4** revision < 3.0, 360 MHz (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, `REV_MIN_100`) |
| Wi-Fi | via the **ESP32-C6** co-processor (SDIO / ESP-Hosted + `esp_wifi_remote`, Wi-Fi 6) |
| Memory | **32 MB flash** (QIO/DIO, 4-byte addressing), PSRAM HEX 200 MHz |
| Audio | **ES8311** codec — built-in microphone and speaker |
| Camera | **OV5647** on MIPI-CSI (RAW10 1280×960 2×2 binning @45 fps, RAW8 800×800 as fallback) |
| Peripherals | **microSD** card slot (SDMMC + internal LDO) |
| SDK | **ESP-IDF v5.5.x** (this project is built with 5.5.5) |
| Build variant | `panel7` → `betta-ha-panel-7b`, version `v0.8.2-7b` |

> The other variants in the repository (`panel4`, `panel10`, `panels3`, `s3`) target different
> boards; this README describes the Waveshare 7" panel only.

## Main features (inherited from upstream)

- The dashboard engine and the **web editor** in your browser (no YAML, no recompiling).
- Home Assistant integration: **WebSocket** (events) + **REST** (forecast, states, cameras).
- Tile library: sensor, button, slider, graph, light, heating, weather, todo list, media player,
  Roborock, empty tile.
- Themes, layout validation, storage in LittleFS, `BETTA-Setup` provisioning, OTA from the browser,
  boot screen, OTA progress bar, screenshot, multi-language support, MQTT with autodiscovery and
  the energy dashboard.

## Getting started

1. **Flash the firmware** (from the [`firmware/`](firmware/README.md) folder — either the single
   `factory` image or the four files; mind `--flash_size 32MB`).
2. After boot the panel brings up the open network **`BETTA-Setup`** — connect to it from a phone or
   computer and open **`192.168.4.1`**.
3. Enter the **Wi-Fi name and password**, the **Home Assistant** address (e.g.
   `ws://homeassistant.local:8123/api/websocket`) and the **HA access token**. The token and
   passwords stay in the panel's NVS.
4. Run **Quick Setup** in the web editor — it creates a starter dashboard from your entities.
5. Open `http://<panel-address>` and build your own layout: pages, tiles, appearance, themes,
   screensaver and top bar.
6. Optionally: enable the camera and motion detection, configure radio / Music Assistant, the
   weather page, Xiaozhi, the microSD card, MQTT and automatic backups.

## Building from source

Requirements: **ESP-IDF v5.5.x**, Python 3.11, PowerShell (for the helper scripts), ~2 GB of space
for the components fetched by the component manager.

```powershell
# (once) ESP-IDF environment in this session
. $env:IDF_PATH\export.ps1

# Build the Waveshare 7B variant (panel7)
idf.py -B build-panel7 `
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.panel7" `
  -D SDKCONFIG="sdkconfig.panel7" build

# Flash over the cable (use your own port)
idf.py -B build-panel7 -p COM5 -b 460800 flash

# Release package: factory image + OTA image (from an active ESP-IDF environment)
# The ready-made files are already in firmware/ — you need this only after changing the code.
pwsh tools\make_factory_bin.ps1 -BuildDir build-panel7 `
  -OutFile firmware\betta-ha-panel-7b.factory.bin `
  -OtaOutFile firmware\betta-ha-panel-7b.bin
```

Important when building:

- The **SoC target** (`esp32p4`) is set by the variant overlay
  (`sdkconfig.defaults.panel7`), so no separate `idf.py set-target` step is required — the
  command above works on a clean clone.
- Set the Windows console code page to **65001 (UTF-8)** — `idf.py` breaks on `cp1250`.
- **Close the serial monitor** before flashing (opening the port toggles DTR/RTS and resets the P4).
- Flashing **does not erase** NVS or LittleFS — your settings, layout and themes are preserved.
- The main component name file (`main/idf_component.yml`) is **generated** from
  `main/idf_component.panel7.yml` by `CMakeLists.txt` and is not stored in the repository.
- The ESP-Hosted patches from `patches/` are applied automatically
  (`cmake/apply_vendor_patches.cmake`).
- The animated weather assets (Meteocons Lottie JSON) are **not part of this repository**: the
  third-party icon set (`basmilius/weather-icons`, see `main/ui/weather_icons/UPSTREAM_LINK.txt`) is
  not redistributed here. A build without it is fully functional — you only get
  `Missing weather lottie asset` warnings at configure time and the weather tiles use the static MDI
  icons instead of animations. To get the animations, place `clear-day.json`, `cloudy.json`,
  `rain.json`, … (the full list is in `main/CMakeLists.txt`, `WEATHER_LOTTIE_EMBEDS`) in
  `main/ui/weather_icons/fill/lottie/` and rebuild.
- `tools/make_factory_bin.ps1` is the upstream packager for the `panel4` / `panel10` / `panels3`
  variants; for this project call it with explicit output paths (as above). Without `-OutFile`
  and `-OtaOutFile` it writes the images to `release/` as `betta86-ha-panel-<version>.factory.bin`.

## Flashing and the flash budget

Ready-to-flash images live in the [`firmware/`](firmware/README.md) folder (the description and
SHA256 sums are in [`firmware/README.md`](firmware/README.md)).

**Option A — a single image (easiest):**

```powershell
esptool.py --chip esp32p4 -p COM5 -b 460800 write_flash `
  --flash_mode dio --flash_size 32MB --flash_freq 80m `
  0x0 firmware\betta-ha-panel-7b.factory.bin
```

**Option B — four files (exactly what `idf.py flash` does):**

```powershell
esptool.py --chip esp32p4 -p COM5 -b 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 32MB --flash_freq 80m `
  0x2000  firmware\bootloader.bin `
  0x8000  firmware\partition-table.bin `
  0xf000  firmware\ota_data_initial.bin `
  0x20000 firmware\betta-ha-panel-7b.bin
```

**The secret of a successful flash:** `--flash_size 32MB`. This panel has 32 MB of flash with
4-byte addressing — with any other size the bootloader reports `exceeds flash chip size` and the
panel ends up in a reboot loop.

**Partition table** ([partitions.csv](partitions.csv)):

| Partition | Offset | Size | Purpose |
|---|---|---|---|
| `nvs` | 0x9000 | 24 kB | Runtime settings and **secrets** (Wi-Fi password, HA token, Xiaozhi token). |
| `otadata` | 0xF000 | 8 kB | Active application partition pointer (OTA). |
| `phy_init` | 0x11000 | 4 kB | RF calibration. |
| `factory` | 0x20000 | 9 MB | Main application (factory slot). |
| `ota_0` | 0x920000 | 9 MB | Update slot A. |
| `ota_1` | 0x1220000 | 8 MB | Update slot B. |
| `coredump` | 0x1A20000 | 1 MB | Panic dumps (ELF). |
| `storage` | 0x1B20000 | ~4.9 MB | **LittleFS**: dashboard layout, themes, translations, wallpapers, logs. |

**Budget:** the `betta-ha-panel-7b.bin` application takes ≈ **4.88 MB** of the 9 MB slot, leaving
**more than half of the partition free** — there is room for further development without touching
the partition table. The web UI is served **gzip-compressed** (less transfer, faster editor
loading), and logs and graph history go to the microSD card, which takes further load off the
flash.

## Web editor sections

The editor opens at `http://<panel-address>` and contains the following sections:

| Section | What it is for |
|---|---|
| **Pages** | Adding, ordering and naming dashboard pages (up to 6 pages + fixed pages). |
| **Page look** | Page appearance: background, gradient, wallpaper, dimming, theme and page presets. |
| **Widgets** | Adding and configuring tiles (27 types) and the **Quick Setup** starter dashboard. |
| **Inspector** | Properties of the selected tile: entity, icon, texts, appearance, actions. |
| **Canvas** | A visual dashboard preview with drag & drop and tile resizing. |
| **Energy Page** | Configuring the energy page (grid / solar / battery / gas / water flows). |
| **Music Assistant** | Player selection and the music page layout. |
| **Internet Radio** | Default player, column count (2–4) and the station list (up to 24). |
| **Weather page** | Tiles of the weather page (weather, forecast, sensors). |
| **Wi-Fi** | Network selection (scan), password, BSSID, country code, static addressing. |
| **Home Assistant** | WebSocket address, token, REST mode, connection diagnostics. |
| **MQTT / Home Assistant** | Broker, port, user, TLS, discovery prefix. |
| **Xiaozhi AI** | Enabling the assistant, server, device, activation URL. |
| **Cameras** | HA cameras: entity or URL, credentials, refresh, scaling. |
| **Built-in camera** | The OV5647 camera: resolution, quality, flips, stream, motion detection and zones. |
| **Time** | Time zone and NTP server. |
| **Display / Screensaver** | Brightness, clock format, screensaver, flip clock, night mode, screen off, wallpapers. |
| **Theme** | Theme selection and editing, automatic day/night. |
| **Pages / Page transition** | Page transition animations and duration. |
| **microSD card** | Mounting, browsing, writing, deleting, formatting, log export. |
| **Setup AP** | Information about the configuration access point (`BETTA-Setup`). |
| **Firmware Update** | Uploading a `*.ota.bin` from a file or a URL, OTA status preview. |
| **System / Settings Actions** | Auto-restart, log level, system actions (reboot the panel). |
| **Backup / Restore** | Exporting and importing the configuration (without secrets). |
| **Diagnostics** | Memory, Wi-Fi, HA, missing entities, flash and DSI counters, boot state. |
| **Logs** | Panel log monitor in the browser, export, clearing. |
| **Actions / Choose Light** | Editing tile actions and the light picker used for scenes. |

## Tile (widget) library

The "+ Add" menu in the editor is grouped; below is the complete list of entries:

| Group | Tile | What it is for |
|---|---|---|
| **Control** | Button | Calling any Home Assistant scene / service. |
| | Slider | Brightness, position or number slider (`light`, `media_player`, `cover`, `number`). |
| | Light Tile | Light: brightness, colour temperature, RGB (shown only when the entity supports it). |
| | Heating Tile | Thermostat / heating with set point and current temperature. |
| | Media Player | Player: cover art, title, play/pause, next, volume, source. |
| | Roborock | Vacuum: start, pause, return to base, status. |
| | Cover | Blind/shutter (control) and **Cover Tile** (position visualisation). |
| | Lock | Lock: lock / unlock / status. |
| | Fan | Fan: speed, mode, on/off. |
| | Select | Selection list (`select` / `input_select`). |
| | Number | Number (`number` / `input_number`). |
| | **Alarm Panel** | Alarmo: arm modes, PIN keypad, sensors, delays. |
| | Scene | Home Assistant scene. |
| | Person | Person: presence, zone, photo. |
| | Timer | Counter / countdown. |
| | Clock | Tile clock (with the date). |
| **Data** | Sensor | Any sensor with an icon and a unit. |
| | Binary Sensor | Binary sensor (door, smoke, water leak) with state colours. |
| | Presence | Room presence. |
| | Graph | Graph: line, smoothed line, bars (up to 4096 points). |
| | Todo List | A Home Assistant to-do list. |
| **Info** | Weather | Current weather conditions. |
| | Weather Forecast | 3-day forecast. |
| | Empty Tile | An empty tile — a spacer and background element in the layout. |

## Appearance — `tile_*` and `page_*` options

**Tile (17 options):**
`tile_bg_color`, `tile_bg_grad_color`, `tile_bg_grad_dir`, `tile_border_color`,
`tile_border_width`, `tile_font_scale`, `tile_icon_color`, `tile_label_color`, `tile_opacity`,
`tile_press_fx`, `tile_press_fx_dim`, `tile_press_fx_scale`, `tile_radius`, `tile_shadow`,
`tile_text_color`, `tile_title_color`, `tile_value_color`.

**Page (11 options):**
`page_bg_color`, `page_bg_grad_color`, `page_bg_grad_dir`, `page_delay`, `page_dim`, `page_size`,
`page_style`, `page_theme`, `page_transition`, `page_transition_ms`, `page_wallpaper`.

## Main panel settings

Settings are stored in NVS (not in code) and exposed as JSON by `GET /api/settings`, then saved
with **`PUT /api/settings`**. Groups:

| Group | Example keys |
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

## HTTP API

All assets are served **gzip-compressed only** — when testing from a console use
`curl --compressed`.

**GET**

| Endpoint | Description |
|---|---|
| `/`, `/app.js`, `/styles.css`, `/favicon.ico` | The web editor. |
| `/api/layout` | Dashboard layout (pages, tiles). |
| `/api/entities` | Home Assistant entity list. |
| `/api/ha/light_entities`, `/api/ha/energy` | Light entities and the energy dashboard. |
| `/api/state` | Current entity states. |
| `/api/settings` | Runtime settings. |
| `/api/sd`, `/api/sd/status`, `/api/sd/files`, `/api/sd/file` | microSD card. |
| `/api/pages`, `/api/themes`, `/api/themes/active`, `/api/themes/get` | Pages and themes. |
| `/api/cameras`, `/api/camera/status`, `/api/camera/snapshot`, `/api/camera/stream`, `/api/camera/motion` | Cameras (HA and the built-in one). |
| `/api/diagnostics`, `/api/status`, `/api/version` | Diagnostics and version information. |
| `/api/logs`, `/api/crash`, `/api/crash/raw` | Log and panic dumps. |
| `/api/backup`, `/api/ota/status` | Backup and OTA status. |
| `/api/i18n/languages`, `/api/i18n/effective` | Languages and active translations. |
| `/api/screenshot.bmp`, `/api/wifi/scan`, `/api/display/wallpaper`, `/api/radio` | Screenshot, Wi-Fi scan, wallpaper, radio. |

**PUT:** `/api/layout`, `/api/settings`, `/api/sd`, `/api/themes/active`, `/api/themes/custom`,
`/api/cameras`, `/api/i18n/custom`

**POST:** `/api/backup/restore`, `/api/crash/erase`, `/api/display/activity`,
`/api/display/wallpaper`, `/api/pages/activate`, `/api/ota/upload`, `/api/ota/url`,
`/api/sd/format`, `/api/sd/logs/export`

**DELETE:** `/api/logs`, `/api/sd/file`, `/api/themes/custom`, `/api/ha/light_entities`,
`/api/display/wallpaper`

## MQTT — topics, commands, discovery entities

| Item | Value |
|---|---|
| Base topic | `betta_panel/<node-suffix>` |
| State | `<base>/state` (JSON with the panel state) |
| Commands | `<base>/set/<key>` |
| Availability | `<base>/status` (`online` / `offline`) |
| HA discovery | `<prefix>/<component>/<object_id>/config` |

**Command keys (24):** `brightness`, `clock_24h`, `page`, `page_transition`,
`page_transition_ms`, `saver_brightness`, `saver_show_date`, `saver_show_seconds`,
`screen_off_enabled`, `screen_off_timeout_sec`, `screensaver_enabled`, `screensaver_timeout_sec`,
`tile_press_fx`, `tile_press_fx_dim`, `tile_press_fx_scale`, `topbar_custom_colors`,
`topbar_icon_text`, `topbar_show_clock`, `topbar_show_date`, `topbar_show_gear`,
`topbar_show_status`, `value_anim`, `value_anim_ms`, `wake`.

**Auto-discovered entities (~23):** screensaver, screen off, 24 h clock, seconds, date,
brightness, screensaver brightness, screensaver timeout, screen-off timeout, screen wake, page
transition (+ duration), press effect (+ dim, + scale), value animation (+ duration), top-bar
clock, top-bar date, settings icon, status icons, text-icon mode and custom top-bar colours.

## Notes and gotchas

- **`--flash_size 32MB`** — mandatory (see the flashing section).
- **P4 revision < 3.0** — `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` / `REV_MIN_100` must be enabled.
- **The serial monitor** must be closed before flashing.
- **Code page 65001** in the Windows console before running `idf.py`.
- **Web assets are gzip only** — test with `curl --compressed`.
- **Logs and graph history go to the SD card** — writing to the internal flash at runtime caused
  image flashes (section R). The panel works without a card, but part of the history is then not
  stored.
- **Camera and streams** — the preview stops when you leave the cameras page (deliberately, to save
  RAM and bandwidth).
- **The `BETTA-Setup` access point is open** — use it only on a trusted network and change the Wi-Fi
  settings after configuring the panel.
- **Auto-restart** — for panels mounted "forever" it is worth leaving enabled (a daily restart
  clears small leaks).
- **Save settings with PUT** (`/api/settings`), not POST.
- **`/api/diagnostics`** is the first place to look in any unusual situation (freeze, missing
  entities, screen flashes, restarts).

## Project structure

```
.
├─ CMakeLists.txt            # variant selection (panel7 → betta-ha-panel-7b, v0.8.2-7b)
├─ CMakePresets.json
├─ partitions.csv            # nvs / otadata / phy_init / factory / ota_0 / ota_1 / coredump / storage
├─ sdkconfig.defaults*       # shared + per variant (panel7 = Waveshare 7")
├─ dependencies.lock
├─ cmake/apply_vendor_patches.cmake
├─ patches/                  # ESP-Hosted patches (Wi-Fi through the ESP32-C6)
├─ components/
│  └─ webui/www/             # web editor: index.html, app.js, styles.css
├─ main/
│  ├─ app_main.c, app_config.h, Kconfig.projbuild
│  ├─ api/                   # HTTP server + REST API (api_routes.c = route table)
│  ├─ camera/                # built-in OV5647 camera + the HA camera base
│  ├─ diag/                  # log, coredump, storage guard, LVGL log bridge
│  ├─ drivers/               # display_init_panel7.c, touch_init_panel7.c, board_pins.h
│  ├─ ha/                    # HA client (WebSocket + REST), entity model, energy
│  ├─ layout/                # dashboard layout storage and validation
│  ├─ mqtt/                  # panel_mqtt.c (topics, commands, discovery)
│  ├─ net/                   # wifi_mgr.c, net_health.*, time_sync.c
│  ├─ radio/                 # panel_radio.c
│  ├─ sd/                    # microSD card support
│  ├─ settings/              # runtime_settings.c, i18n_store.c
│  ├─ ui/                    # LVGL: pages, tiles, themes, screensaver, top bar
│  │  ├─ theme/              # palettes and the theme store
│  │  ├─ widgets/            # the tile types (w_*.c)
│  │  └─ fonts/
│  ├─ util/                  # JSON, ring buffer, log tags
│  └─ xiaozhi/               # voice assistant (activation, WS v3 client, audio, UI)
├─ tools/make_factory_bin.ps1
├─ firmware/                 # ready images: factory, application, bootloader, partitions, otadata
├─ images/                   # screenshots and the BETTA OS logo
├─ release/                  # upstream images for the other variants
└─ LICENSE                   # FNCL-1.1
```

## Privacy — no personal data in this repository

This repository is the **public (sanitised)** version of the project. It deliberately contains:

- no **passwords, tokens or keys** (the configuration files hold placeholders only:
  `YOUR_WIFI_SSID`, `YOUR_WIFI_PASSWORD`, `YOUR_HA_WS_URL`, `YOUR_HA_ACCESS_TOKEN`),
- no **IP addresses**, Wi-Fi network names, host names or MQTT broker addresses from the author's
  installation,
- no **internal notes** and no diagnostic logs from the device.

All configuration data goes into the panel's **NVS** at configuration time (Wi-Fi, HA token,
Xiaozhi token). Tokens are **never exported** in a backup (`GET /api/backup`). The
`managed_components/` folder (dependencies fetched by the component manager) and build folders are
not published.

## License

The project is licensed under the
**[Federation Non-Commercial License v1.1 (LicenseRef-FNCL-1.1)](LICENSE)** —
Copyright (c) 2026 Cpt_Kirk.

- **Non-commercial** use is permitted under the terms of the licence.
- **Commercial** use requires written permission from the copyright holder.
- Modifications (this fork) must be marked and described — see [release-notes.md](release-notes.md)
  and the [Detailed list of improvements](#detailed-list-of-improvements) section.
- Keep the `LICENSE` file and the `Copyright (c) 2026 Cpt_Kirk` headers in the source files.

## Disclaimer / Zastrzeżenie

⚠️ **Disclaimer:** Provided "AS IS", without warranty of any kind. The author is not responsible
for damage to hardware, data loss or incorrect energy/server readings. Configuration values
(Wi-Fi password, Home Assistant token) are stored on the device (NVS/SD), not in this repository —
keep your backups private.

⚠️ **Zastrzeżenie:** Oprogramowanie udostępnione „AS IS", bez jakiejkolwiek gwarancji. Autor nie
odpowiada za uszkodzenia sprzętu, utratę danych ani błędne odczyty energii/serwerów. Dane
konfiguracyjne (hasło Wi-Fi, token Home Assistant) przechowywane są na urządzeniu (NVS/SD), nie
w tym repozytorium — kopie zapasowe trzymaj prywatnie.
