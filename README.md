<!-- SPDX-License-Identifier: LicenseRef-FNCL-1.1 | Copyright (c) 2026 Cpt_Kirk -->
# BETTA HA Panel for Waveshare ESP32-P4 boards (4.3" Wi-Fi and 10.1" Ethernet)

A Home Assistant wall panel firmware for two Waveshare ESP32-P4 boards. You build the dashboard
in the panel's own web editor, with no YAML and no firmware rebuilds.

> **Made entirely by vibe coding with Claude.** Every change in this fork was made by
> [Claude](https://claude.com/claude-code) (Anthropic), working from plain-language requests.
> That covers the code, the debugging on real hardware and this documentation. None of it was
> written by hand. Each board was tested on a single unit. This is a private hobby project and
> **not** an official BETTA HA Panel release.

---

## Where this comes from

This repository is a fork of a fork:

1. **[cptkirki/BETTA-HA-PANEL](https://github.com/cptkirki/BETTA-HA-PANEL)**: the original
   *BETTA HA Panel v0.8.2* by **Cpt_Kirk**.
2. **[kruzio1985/BETTA-HA-PANEL-7-Waveshare-7B](https://github.com/kruzio1985/BETTA-HA-PANEL-7-Waveshare-7B)**:
   ports BETTA to the Waveshare ESP32-P4 7" (7B) panel. It adds themes, a tile/page look engine,
   a screensaver, cameras, internet radio, Music Assistant, a weather page, microSD,
   diagnostics, backup/restore and MQTT discovery.
3. **This repository** adds two more ESP32-P4 boards and the features listed below.

Everything from the 7B fork still works here. The 7B and the other original variants still
build from this source tree, but only the two boards below were tested.

## Supported boards

| | Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 | Waveshare ESP32-P4-POE-ETH-KIT-B (10.1") |
|---|---|---|
| Build variant | `panel43` | `p4eth101` |
| Display | 4.3" 480×800 MIPI-DSI (ST7701), used in landscape (800×480) | 10.1" 800×1280 MIPI-DSI, used in landscape (1280×800) |
| Touch | GT911 | GT911 |
| Network | Wi-Fi 6 through the on-board ESP32-C6 (ESP-Hosted) | Ethernet (IP101), PoE. There is no Wi-Fi on this kit |
| Flash | 32 MB | 32 MB, with the first 16 MB laid out like ESPHome |
| sdkconfig | `sdkconfig.defaults;sdkconfig.defaults.panel43` | `sdkconfig.defaults;sdkconfig.defaults.p4eth101` |

Both boards need an ESP32-P4 chip revision 1.x and ESP-IDF **5.5**.

## What this fork changes

### New on both boards

- **Voice assistant (Home Assistant Assist):** a microphone button in the top bar works as
  push-to-talk; you can also tap and speak until a pause is detected.
  - You can pick the Assist pipeline (Whisper, Speech-to-Phrase, cloud and so on).
  - The answer can be read aloud on the panel's speaker.
  - Microphone gain and maximum recording time are adjustable.
- **Customisable status bar:**
  - Place every element (date, cameras, weather, waste, clock, alert, microphone, settings,
    Home Assistant, network) left, centre or right, or hide it.
  - The centre group sits exactly in the middle of the screen.
  - All icons have the same size, and the clock picks the largest font that fits.
  - Seven date formats are available, with or without weekday and year.
- **Waste collection in the status bar:**
  - Up to four bins, each with its own colour.
  - Each bin shows the days until collection and blinks on collection day.
  - Supported sources: sensors whose state is a number of days, *Waste Collection Schedule*
    (`daysTo`), date sensors and calendar entities.
- **Alerts:**
  - A red triangle shows Home Assistant persistent notifications and any entities you choose
    (for example a weather-warning level).
  - It blinks while alerts are unread. A tap opens a list with a "mark read" button.
- **Energy page:** a second page (swipe) with live power values next to the daily totals from
  the HA Energy dashboard.
- **Presence sensor (HLK-LD2410) on a UART:**
  - Wakes the display when someone approaches and can keep it on while someone is there.
  - The web editor shows live status and has a built-in loopback self-test.
- **Look:**
  - Page background gradients are rendered as a dithered RGB565 image, so they show no banding.
  - The *Aurora glow* theme effect now "breathes".
- **Web editor:**
  - Settings regrouped: MQTT under Home Assistant; time, UI, setup AP, microSD and backup under
    System; cameras under Status bar.
  - Logs toolbar with auto-scroll and download.
  - Remaining Polish texts translated.
  - Every text added by this fork exists in **English and German**, chosen by the UI language
    setting.
- **Robustness:**
  - The layout is validated against the largest canvas.
  - A rejected layout is kept as `layout.json.bad` instead of being lost.
  - Flash writers are paused around OTA state changes (this fixes a watchdog reset on the
    ESP32-P4).

### `panel43`: Waveshare 4.3" Wi-Fi

- Display driver for the ST7701 panel in landscape (software rotation), touch, backlight and
  audio for this board.
- Wi-Fi through the ESP32-C6 co-processor (ESP-Hosted).
- Compact layouts for 480 px height: tile sizes matched to the web editor, boot screen, energy
  page and on-device settings.
- "Display off" also lays a black layer over the screen, because the backlight never goes fully
  dark.
- Compact action and cover tiles, and a domain filter in the entity picker.
- Clean display reset on restart, so the panel does not show stripes after saving.

### `p4eth101`: Waveshare 10.1" PoE/Ethernet kit

- Ethernet network manager (IP101 PHY). The status bar shows an Ethernet icon, and the Wi-Fi and
  setup-AP settings are hidden.
- Optional **vertical page navigation on the left** instead of the bottom bar.
- An **ESPHome-compatible partition layout** (`partitions.esphome16.csv`). A kit that runs
  ESPHome can install this firmware as an ordinary ESPHome OTA update, with no USB cable needed.
  The panel's file storage lives above 16 MB.
- **Rollback guard:** the firmware marks itself as good only once the network is up. Without
  network for four minutes, or after a crash during start-up, the kit falls back to the previous
  firmware (for example ESPHome).

## Building

ESP-IDF 5.5, target `esp32p4`.

```sh
# 4.3" Wi-Fi panel
idf.py -B build-panel43 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.panel43" \
       -D SDKCONFIG=sdkconfig.panel43 set-target esp32p4
idf.py -B build-panel43 build

# 10.1" Ethernet kit
idf.py -B build-p4eth -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.p4eth101" \
       -D SDKCONFIG=sdkconfig.p4eth101 set-target esp32p4
idf.py -B build-p4eth build
```

The top-level `CMakeLists.txt` picks the matching component manifest
(`main/idf_component.<variant>.yml`). The build rewrites `dependencies.lock` for the board you
are building. That change is local and should not be committed.

## Flashing

- **USB:** `idf.py -B <build-dir> -p <port> flash`.
- **Updates later on:** in the web editor (Settings → Firmware Update), or with
  `curl -X POST --data-binary @build-panel43/betta-ha-panel-43.bin http://<panel-ip>/api/ota/upload`.
- **10.1" kit that runs ESPHome:** upload the app image with ESPHome's own tool, for example
  `esphome upload <your-config>.yaml --device <panel-ip> --file build-p4eth/betta-ha-panel-p4eth.bin`.
  This uses your ESPHome OTA password or key, which never needs to go into this repository. After
  the reboot the panel gets an address by DHCP and shows the web editor's address on screen. If
  it cannot reach the network, it returns to ESPHome by itself.

After the first start, open `http://<panel-ip>/` and enter your Home Assistant URL and a
long-lived access token. All personal settings (network, tokens, entities, layout) are stored on
the panel only.

## Known limitations

- The LD2410 support is untested with a working sensor: both test sensors were faulty. The UART
  path itself was checked with the loopback self-test.
- Screenshots taken through the web API come out rotated, because they are in the panels' native
  portrait orientation.
- Only one unit of each board was tested.

## Privacy

This repository contains no personal data. There are no network names, addresses, tokens,
passwords, keys or Home Assistant entity names of the author in it. Defaults in
`main/app_config.h` are placeholders. Your own configuration lives on the panel and in your
backup files, never in the source.

## License and attribution

- Original project: **BETTA HA Panel v0.8.2**, Copyright (c) 2026 **Cpt_Kirk**.
- License: **[LicenseRef-FNCL-1.1](LICENSE)** (Federation Non-Commercial License v1.1), for
  **non-commercial use only**. Commercial use requires a separate written licence from the
  copyright holder (see §11 of the licence).
- **This fork modifies the original software.** As §3 of the licence requires, the changes are
  described above ([What this fork changes](#what-this-fork-changes)), in the git history and,
  for the earlier 7B work, in [release-notes.md](release-notes.md).
- The original `SPDX-License-Identifier` and copyright headers are preserved in the source files.
