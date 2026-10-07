<!-- SPDX-License-Identifier: LicenseRef-FNCL-1.1 -->
# Variante `panel43`: Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3

> **KI-generiert:** Diese Variante wurde mit **Claude (Anthropic)** erstellt und an
> einem einzelnen Gerät getestet. Sie ist kein offizieller Teil von BETTA HA Panel.
> Oberfläche, Übersetzungen und diese Beschreibung sind auf **Deutsch** ausgerichtet.
>
> *AI-generated with Claude, tested on one device, German-oriented. Not part of the
> official BETTA HA Panel project.*

## Hardware

- Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3: 480×800 MIPI-DSI (ST7701), GT911-Touch
- ESP32-P4 Revision 1.x, 32 MB Flash, WLAN über ESP32-C6 (ESP-Hosted)
- Betrieb im **Querformat** (800×480, Drehung um 90° in Software)

## Bauen und Flashen

ESP-IDF 5.5:

```sh
idf.py -B build-panel43 -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.panel43" -D SDKCONFIG=sdkconfig.panel43 set-target esp32p4
idf.py -B build-panel43 -p COMx flash monitor
```

Die Variante baut auf dem 7B-Build auf (`CONFIG_APP_PANEL_BOARD_WAVESHARE_43`).
`dependencies.lock` ist für alle Varianten gemeinsam und wird beim Bauen auf die
4.3"-BSP umgeschrieben; diese Änderung gehört nicht ins Repo.

## Unterschiede zur 7B-Variante

- Kompakte Layouts für 480 px Höhe: Kachelgrößen (abgestimmt mit dem Web-Editor),
  Boot-Bildschirm, Energie-Seite, Einstellungen, Kacheln ab 120 px Höhe
- Untere Leiste: Haus ganz links, danach die eigenen Seiten von links nach rechts
- Xiaozhi- und Kamera-Seiten erscheinen nur, wenn sie im Editor angelegt wurden;
  die Kamera-Seite öffnet sich über das Symbol oben links und lässt sich wischen
- Wochentag vor dem Datum, deutsche Texte für das Einstellungsmenü am Gerät
- MQTT: stabile Geräte-ID (keine doppelten Geräte in Home Assistant) und Schalter
  „Display“, der den Bildschirm sofort aus- und einschaltet (z. B. per Präsenzmelder)

## Bekannte Einschränkungen

- Screenshots über die Web-API sind um 90° gedreht (Hochformat des Panels).
- Die Radio-Seite hat keinen festen Zugang mehr; bei Bedarf im Editor anlegen.
- Die Hintergrundbeleuchtung geht bei 0 % nicht ganz aus; „Display AUS“ legt
  deshalb zusätzlich eine schwarze Fläche über den Bildschirm.
