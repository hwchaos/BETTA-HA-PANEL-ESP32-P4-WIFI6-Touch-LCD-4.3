/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_radio_stations.h"

/*
 * Fallback station list for the radio page.
 *
 * Every entry is a direct stream URL that was probed with curl (HTTP 200/302 +
 * an audio content type) when the table was written; the player resolves
 * nothing, Home Assistant only has to send play_media with media_content_type
 * "music" and this URL as media_content_id.  Trim, reorder or extend the table
 * freely - the page reads it in order and shows as many tiles as fit.
 *
 * Transports: MP3 or AAC over plain HTTP(S), because that is what every HA
 * media_player integration (WiiM/LinkPlay, HEOS, Music Assistant, Chromecast)
 * accepts for a URL.  HLS-only stations should be avoided here: several
 * integrations refuse to open them directly.
 */
static const ui_radio_station_t s_stations[] = {
    /* Polish stations. */
    {"RMF FM", "https://rs101-krk-cyfronet.rmfstream.pl/RMFFM48"},
    {"RMF MAXXX", "https://rs6-krk2-cyfronet.rmfstream.pl/RMFMAXXX48"},
    {"RMF Classic", "https://rs102-krk-cyfronet.rmfstream.pl/RMFCLASSIC48"},
    {"Radio ZET", "https://playerservices.streamtheworld.com/api/livestream-redirect/RADIO_ZET.mp3"},
    {"Antyradio", "https://an.cdn.eurozet.pl/ant-web.mp3"},
    {"Radio Nowy Świat", "https://stream.rcs.revma.com/ypqt40u0x1zuv"},
    {"Radio 357", "https://stream.rcs.revma.com/ye5kghkgcm0uv"},
    {"Radio Kampus", "https://stream.radiokampus.fm/kampus"},
    /* International stations. */
    {"Radio Paradise", "https://stream.radioparadise.com/mp3-192"},
    {"SomaFM Groove Salad", "https://ice2.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Lush", "https://ice2.somafm.com/lush-128-mp3"},
    {"SomaFM Drone Zone", "https://ice2.somafm.com/dronezone-128-mp3"},
    {"SomaFM Secret Agent", "https://ice2.somafm.com/secretagent-128-mp3"},
    {"SomaFM Underground 80s", "https://ice2.somafm.com/u80s-128-mp3"},
    {"SomaFM Indie Pop", "https://ice2.somafm.com/indiepop-128-mp3"},
};

size_t ui_radio_stations_count(void)
{
    return sizeof(s_stations) / sizeof(s_stations[0]);
}

const ui_radio_station_t *ui_radio_stations_get(size_t index)
{
    if (index >= ui_radio_stations_count()) {
        return NULL;
    }
    return &s_stations[index];
}
