<!-- SPDX-License-Identifier: LicenseRef-FNCL-1.1 | Copyright (c) 2026 Cpt_Kirk -->
# Firmware — Waveshare 7" (`panel7`) / Firmware — Waveshare 7" (`panel7`)

🇵🇱 [Polski](#-polski) · 🇬🇧 [English](#-english)

---

## 🇵🇱 Polski

### Zawartość katalogu

| Plik | Offset | Rozmiar | SHA256 |
|---|---|---|---|
| `betta-ha-panel-7b.factory.bin` | **0x0** | 5 009 792 B | `F47BB9805E4500D7D0D90E9BFF69B8A1172A5D17C9328C9E73D4C0F78D8A9EF0` |
| `betta-ha-panel-7b.bin` (aplikacja) | **0x20000** | 4 878 720 B | `817CF3D37AE87793BF8CEC54F457BCFAE61463A57AF8111819BD757A1DA17787` |
| `bootloader.bin` | **0x2000** | 23 488 B | `2F359EC9EC530DA82D4E0DA0C8DD5C440046819471AF3EB3D9E0239827793D5A` |
| `partition-table.bin` | **0x8000** | 3 072 B | `23E9387F6A1EDEA1F5D1E8A93E7721AC7E47B0CA6A520725D95C9705CFED5923` |
| `ota_data_initial.bin` | **0xF000** | 8 192 B | `7D2C7AC4888BFD75CD5F56E8D61F69595121183AFC81556C876732FD3782C62F` |
| `flasher_args.json` | — | 1 099 B | `DA8C1AFDFF798AD2E2EDDEAF973B89D4A170F061F980D48C95473609C075E6F7` |

- Wersja: **`v0.8.2-7b`** (projekt `betta-ha-panel-7b`), zbudowane z ESP-IDF **v5.5.5**.
- `betta-ha-panel-7b.factory.bin` to **pełny obraz** zmontowany z czterech plików powyżej
  (wolne obszary wypełnione `0xFF`) — wystarczy go wgrać od adresu `0x0`.
- Sumy SHA256 pozwalają sprawdzić kompletność plików po pobraniu
  (`Get-FileHash <plik> -Algorithm SHA256`).

### Wgrywanie — wariant A: jeden obraz (zalecany)

```powershell
esptool.py --chip esp32p4 -p COM5 -b 460800 write_flash `
  --flash_mode dio --flash_size 32MB --flash_freq 80m `
  0x0 betta-ha-panel-7b.factory.bin
```

### Wgrywanie — wariant B: cztery pliki

```powershell
esptool.py --chip esp32p4 -p COM5 -b 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 32MB --flash_freq 80m `
  0x2000  bootloader.bin `
  0x8000  partition-table.bin `
  0xf000  ota_data_initial.bin `
  0x20000 betta-ha-panel-7b.bin
```

### Wgrywanie — wariant C: aktualizacja bez kabla (OTA)

Zbuduj obraz OTA i wgraj go przez przeglądarkę (sekcja **Firmware Update** edytora WWW)
pod adresem `http://<adres-panelu>`:

```powershell
idf.py -B build-panel7 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.panel7" `
  -D SDKCONFIG="sdkconfig.panel7" app
# plik: build-panel7\betta-ha-panel-7b.bin  →  POST /api/ota/upload
```

### ⚠️ Najważniejsze ostrzeżenie

**Zawsze podawaj `--flash_size 32MB`.** Ten panel ma **32 MB flash z adresowaniem 4-bajtowym**;
przy innym rozmiarze bootloader zgłasza `exceeds flash chip size` i panel wpada w pętlę restartów.
Wgrywanie **nie kasuje** NVS ani LittleFS — ustawienia, układ pulpitu, motywy i tapety zostają.

### Po wgraniu

1. Panel wystawi otwarty punkt dostępowy **`BETTA-Setup`** — połącz się z nim i wejdź na
   **`192.168.4.1`**.
2. Podaj dane Wi-Fi, adres Home Assistant (`ws://homeassistant.local:8123/api/websocket`)
   i token HA. Hasła i tokeny trafiają wyłącznie do NVS panelu.
3. Uruchom **Quick Setup** w edytorze WWW, a następnie — jeśli chcesz — opcjonalny
   pełny reset ustawień (`POST /api/backup/restore` z pustą konfiguracją lub kasowanie NVS
   przez `idf.py erase-flash`; **uwaga: to usuwa też ustawienia**).

### Budżet pamięci

Aplikacja zajmuje ≈ 4,88 MB z 9 MB partycji `factory` — ponad połowa slotu pozostaje wolna.
Tabelę partycji znajdziesz w [`../partitions.csv`](../partitions.csv) oraz w README głównym.

---

## 🇬🇧 English

### Directory contents

| File | Offset | Size | SHA256 |
|---|---|---|---|
| `betta-ha-panel-7b.factory.bin` | **0x0** | 5 009 792 B | `F47BB9805E4500D7D0D90E9BFF69B8A1172A5D17C9328C9E73D4C0F78D8A9EF0` |
| `betta-ha-panel-7b.bin` (application) | **0x20000** | 4 878 720 B | `817CF3D37AE87793BF8CEC54F457BCFAE61463A57AF8111819BD757A1DA17787` |
| `bootloader.bin` | **0x2000** | 23 488 B | `2F359EC9EC530DA82D4E0DA0C8DD5C440046819471AF3EB3D9E0239827793D5A` |
| `partition-table.bin` | **0x8000** | 3 072 B | `23E9387F6A1EDEA1F5D1E8A93E7721AC7E47B0CA6A520725D95C9705CFED5923` |
| `ota_data_initial.bin` | **0xF000** | 8 192 B | `7D2C7AC4888BFD75CD5F56E8D61F69595121183AFC81556C876732FD3782C62F` |
| `flasher_args.json` | — | 1 099 B | `DA8C1AFDFF798AD2E2EDDEAF973B89D4A170F061F980D48C95473609C075E6F7` |

- Version: **`v0.8.2-7b`** (project `betta-ha-panel-7b`), built with ESP-IDF **v5.5.5**.
- `betta-ha-panel-7b.factory.bin` is the **complete image** merged from the four files above
  (unwritten areas filled with `0xFF`) — flash it starting at `0x0`.
- The SHA256 sums let you verify the files after downloading
  (`Get-FileHash <file> -Algorithm SHA256`).

### Flashing — option A: a single image (recommended)

```powershell
esptool.py --chip esp32p4 -p COM5 -b 460800 write_flash `
  --flash_mode dio --flash_size 32MB --flash_freq 80m `
  0x0 betta-ha-panel-7b.factory.bin
```

### Flashing — option B: four files

```powershell
esptool.py --chip esp32p4 -p COM5 -b 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 32MB --flash_freq 80m `
  0x2000  bootloader.bin `
  0x8000  partition-table.bin `
  0xf000  ota_data_initial.bin `
  0x20000 betta-ha-panel-7b.bin
```

### Flashing — option C: cable-free update (OTA)

Build the OTA image and upload it through the browser (the **Firmware Update** section of the web
editor) at `http://<panel-address>`:

```powershell
idf.py -B build-panel7 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.panel7" `
  -D SDKCONFIG="sdkconfig.panel7" app
# file: build-panel7\betta-ha-panel-7b.bin  →  POST /api/ota/upload
```

### ⚠️ The single most important warning

**Always pass `--flash_size 32MB`.** This panel has **32 MB of flash with 4-byte addressing**;
with any other size the bootloader reports `exceeds flash chip size` and the panel ends up in a
reboot loop. Flashing **does not erase** NVS or LittleFS — your settings, dashboard layout, themes
and wallpapers are preserved.

### After flashing

1. The panel brings up the open access point **`BETTA-Setup`** — connect to it and open
   **`192.168.4.1`**.
2. Enter the Wi-Fi credentials, the Home Assistant address
   (`ws://homeassistant.local:8123/api/websocket`) and the HA token. Passwords and tokens go into
   the panel's NVS only.
3. Run **Quick Setup** in the web editor and, if you wish, perform an optional full settings reset
   (restore an empty configuration via `POST /api/backup/restore`, or erase NVS with
   `idf.py erase-flash`; **note: this also removes your settings**).

### Memory budget

The application takes ≈ 4.88 MB of the 9 MB `factory` partition — more than half of the slot stays
free. The partition table lives in [`../partitions.csv`](../partitions.csv) and in the main README.

---

Back to the main documentation: [README.md](../README.md) · [README.pl.md](../README.pl.md)
