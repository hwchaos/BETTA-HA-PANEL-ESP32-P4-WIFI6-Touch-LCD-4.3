# SPDX-License-Identifier: LicenseRef-FNCL-1.1
# Copyright (c) 2026 Cpt_Kirk

# ---- Display / touch drivers -----------------------------------------------
# Both boards (4.3" and the 10.1" ETH kit) use the MIPI-DSI driver pair; the
# board differences are #if branches inside them.
set(PANEL_DRIVER_SRCS
    "drivers/display_init_panel7.c"
    "drivers/touch_init_panel7.c"
)

# Network link: Wi-Fi manager, or the Ethernet manager (same wifi_mgr.h API)
# on the ESP32-P4-ETH kit.
if(CONFIG_APP_PANEL_BOARD_WAVESHARE_P4ETH101)
    set(NET_MGR_SRCS "net/eth_mgr.c")
else()
    set(NET_MGR_SRCS "net/wifi_mgr.c")
endif()

# Optional OV5647 MIPI-CSI camera (CONFIG_APP_FEATURE_LOCAL_CAMERA).
set(APP_LOCAL_CAMERA_SRCS "")
if(CONFIG_APP_FEATURE_LOCAL_CAMERA)
    list(APPEND APP_LOCAL_CAMERA_SRCS
        "camera/local_camera.c"
        "api/api_camera_local.c")
endif()
idf_component_register(
    SRCS        "app_main.c"
        "app_events.c"
        "api/http_server.c"
        "api/http_guard.c"
        "api/api_routes.c"
        "api/api_layout.c"
        "api/api_energy.c"
        "api/api_entities.c"
        "api/api_wakeword.c"
        "api/api_state.c"
        "api/api_settings.c"
        "api/api_sd.c"
        "api/api_i18n.c"
        "api/api_wifi.c"
        "api/api_version.c"
        "api/api_screenshot.c"
        "api/api_ota.c"
        "api/api_theme.c"
        "api/api_ha_diagnostics.c"
        "api/api_cameras.c"
        "api/api_system_log.c"
        "diag/system_log.c"
        "diag/display_flash_watch.c"
        "diag/dsi_underrun_watch.c"
        "diag/lvgl_log_bridge.c"
        "drivers/display_frame_buffer.c"
        "api/api_diagnostics.c"
        "api/api_backup.c"
        "api/api_pages.c"
        "diag/boot_guard.c"
        "diag/crash_core.c"
        "diag/storage_guard.c"
        "sd/sd_card.c"
        "sd/sd_logs.c"
        ${PANEL_DRIVER_SRCS}
        "ha/ha_client.c"
        "ha/ha_ws.c"
        "ha/ha_model.c"
        "ha/ha_energy_model.c"
        "ha/ha_light_capabilities.c"
        "ha/ha_cover_fetcher.c"
        "ha/ha_alarm_events.c"
        "ha/ha_cards.c"
        "layout/layout_store.c"
        "layout/layout_validate.c"
        "settings/runtime_settings.c"
        "settings/i18n_store.c"
        ${NET_MGR_SRCS}
        "net/net_health.c"
        "net/time_sync.c"
        "mqtt/panel_mqtt.c"
        "ui/ui_runtime.c"
        "ui/ui_boot_splash.c"
        "ui/ui_ota_progress.c"
        "ui/ui_pages.c"
        "ui/ui_settings.c"
        "ui/ui_page_style.c"
        "ui/ui_page_transition.c"
        "ui/ui_press_feedback.c"
        "ui/ui_screen_saver.c"
        "ui/ui_saver_cards.c"
        "ui/ui_slider_touch.c"
        "ui/ui_theme_router.c"
        "ui/ui_value_anim.c"
        "ui/ui_energy_page.c"
        "ui/ui_cameras_page.c"
        "ui/ui_music_page.c"
        "ui/ui_radio_page.c"
        "ui/ui_radio_stations.c"
        "ui/ui_i18n.c"
        "ui/ui_widget_factory.c"
        "ui/ui_tile_style.c"
        "ui/ui_mdi_icon.c"
        "ui/ui_sensor_history.c"
        "ui/ui_bindings.c"
        "ui/lv_psram_mem.c"
        "ui/widgets/w_sensor.c"
        "ui/widgets/w_button.c"
        "ui/widgets/w_slider.c"
        "ui/widgets/w_graph.c"
        "ui/widgets/w_empty_tile.c"
        "ui/widgets/w_light_tile.c"
        "ui/widgets/w_heating_tile.c"
        "ui/widgets/w_weather_tile.c"
        "ui/widgets/w_todo.c"
        "ui/widgets/w_media_player.c"
        "ui/widgets/w_roborock.c"
        "ui/widgets/w_binary_sensor.c"
        "ui/widgets/w_presence.c"
        "ui/widgets/w_chart.c"
        "ui/widgets/w_mower.c"
        "ui/widgets/w_lock.c"
        "ui/widgets/w_fan.c"
        "ui/widgets/w_select.c"
        "ui/widgets/w_number.c"
        "ui/widgets/w_alarm_tile.c"
        "ui/widgets/w_clock_alarm.c"
        "ui/widgets/w_cover_tile.c"
        "ui/widgets/w_scene_tile.c"
        "ui/widgets/w_person_tile.c"
        "ui/widgets/w_timer_tile.c"
        "ui/fonts/mdi_font_registry.c"
        "ui/fonts/mdi_names.c"
        "ui/theme/theme_default.c"
        "ui/theme/theme_fx.c"
        "ui/theme/theme_palette.c"
        "ui/theme/theme_store.c"
        ${APP_LOCAL_CAMERA_SRCS}
        "camera/camera_store.c"
        "util/json_util.c"
        "util/ringbuf.c"
        "util/svg_map.c"
        "radio/panel_radio.c"
        "audio/panel_audio.c"
        "assist/panel_assist.c"
        "assist/panel_wake.cc"
        "ui/ui_assist.c"
        "ui/ui_notify.c"
        "ui/ui_geometry.c"
        "ui/ui_topbar_waste.c"
        "ui/ui_topbar_alerts.c"
        "radar/ld2410.c"
    INCLUDE_DIRS
        "."
        "api"
        "camera"
        "diag"
        "drivers"
        "ha"
        "layout"
        "mqtt"
        "net"
        "radio"
        "sd"
        "ui"
        "ui/theme"
        "util"
        "audio"
    REQUIRES
        nvs_flash
        bootloader_support
        esp_netif
        esp_event
        esp_wifi
        esp_eth
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_uart
        esp_driver_ledc
        esp_driver_sdspi
        esp_driver_sdmmc
        sdmmc
        fatfs
        esp_websocket_client
        esp_http_client
        esp_http_server
        espressif__esp-tflite-micro
        esphome__esp-micro-speech-features
        mqtt
        esp_rom
        esp_timer
        app_update
        esp_app_format
        espcoredump
        spi_flash
        esp_lcd
        lvgl
        json
        littlefs
        webui
        esp_driver_jpeg
        esp_codec_dev
        esp-tls
        esp_driver_tsens
        esp_mm
        soc
        hal
)

# `esp_video` provides the V4L2-style camera stack used by camera/local_camera.c
# and api/api_camera_local.c. It is attached with optional_requires so panel
# variants that build without the camera feature keep the same component graph.
if(CONFIG_APP_FEATURE_LOCAL_CAMERA)
    idf_component_optional_requires(PUBLIC esp_video)
endif()

# ESP-Hosted (P4 + C6 coprocessor) provides Wi-Fi on the 4.3" board; the
# ETH kit has no C6.
if(NOT CONFIG_APP_PANEL_BOARD_WAVESHARE_P4ETH101)
    idf_component_optional_requires(PRIVATE esp_wifi_remote esp_hosted)
endif()

# The ESP32-P4-ETH kit uses the Nano BSP behind the driver's BSP header.
if(CONFIG_APP_PANEL_BOARD_WAVESHARE_P4ETH101)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/drivers/p4eth_bsp_shim"
    )
endif()

# The 4.3" board: this shim redirects the driver's BSP header to the 4.3" BSP.
if(CONFIG_APP_PANEL_BOARD_WAVESHARE_43)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/drivers/panel43_bsp_shim"
    )
endif()

# Full Material Design Icons webfont for ui_mdi_icon.c (any "mdi:..." icon,
# rendered at runtime by LVGL's Tiny TTF engine).
# Built-in wake word model (assist/panel_wake.cc).
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/assist/models/okay_nabu.tflite" BINARY)

if(CONFIG_LV_USE_TINY_TTF)
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/materialdesignicons-webfont.ttf" BINARY)
    # Text font for full-screen messages (ui_notify.c), rendered at any size.
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/ui/fonts/Poppins-Regular.ttf" BINARY)
endif()

set(APP_HAVE_HOSTED_C6_FW_IMAGE 0)
if(NOT CONFIG_APP_PANEL_BOARD_WAVESHARE_P4ETH101)
set(HOSTED_C6_FW_BIN "")
set(HOSTED_C6_FW_EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/hosted_c6_fw")
set(HOSTED_C6_FW_BIN_RELEASE_GENERIC "${CMAKE_SOURCE_DIR}/release/network_adapter_esp32c6.bin")
file(MAKE_DIRECTORY "${HOSTED_C6_FW_EMBED_DIR}")

set(HOSTED_C6_HOST_VERSION_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp_hosted/host/esp_hosted_host_fw_ver.h")
set(HOSTED_C6_EXPECTED_VERSION "unknown")
if(EXISTS "${HOSTED_C6_HOST_VERSION_FILE}")
    file(READ "${HOSTED_C6_HOST_VERSION_FILE}" HOSTED_C6_HOST_VERSION_TEXT)
    string(REGEX MATCH "#define[ \t]+ESP_HOSTED_VERSION_MAJOR_1[ \t]+([0-9]+)"
        _HOSTED_C6_MAJOR_MATCH "${HOSTED_C6_HOST_VERSION_TEXT}")
    set(HOSTED_C6_VER_MAJOR "${CMAKE_MATCH_1}")
    string(REGEX MATCH "#define[ \t]+ESP_HOSTED_VERSION_MINOR_1[ \t]+([0-9]+)"
        _HOSTED_C6_MINOR_MATCH "${HOSTED_C6_HOST_VERSION_TEXT}")
    set(HOSTED_C6_VER_MINOR "${CMAKE_MATCH_1}")
    string(REGEX MATCH "#define[ \t]+ESP_HOSTED_VERSION_PATCH_1[ \t]+([0-9]+)"
        _HOSTED_C6_PATCH_MATCH "${HOSTED_C6_HOST_VERSION_TEXT}")
    set(HOSTED_C6_VER_PATCH "${CMAKE_MATCH_1}")
    if(NOT HOSTED_C6_VER_MAJOR STREQUAL "" AND
       NOT HOSTED_C6_VER_MINOR STREQUAL "" AND
       NOT HOSTED_C6_VER_PATCH STREQUAL "")
        set(HOSTED_C6_EXPECTED_VERSION
            "${HOSTED_C6_VER_MAJOR}.${HOSTED_C6_VER_MINOR}.${HOSTED_C6_VER_PATCH}")
    endif()
endif()

if(NOT HOSTED_C6_EXPECTED_VERSION STREQUAL "unknown")
    set(HOSTED_C6_FW_BIN_RELEASE_VERSIONED
        "${CMAKE_SOURCE_DIR}/release/network_adapter_esp32c6_${HOSTED_C6_EXPECTED_VERSION}.bin")
endif()
if(DEFINED HOSTED_C6_FW_BIN_RELEASE_VERSIONED AND EXISTS "${HOSTED_C6_FW_BIN_RELEASE_VERSIONED}")
    set(HOSTED_C6_FW_BIN "${HOSTED_C6_FW_BIN_RELEASE_VERSIONED}")
    message(STATUS "Using release C6 firmware image: ${HOSTED_C6_FW_BIN_RELEASE_VERSIONED}")
elseif(EXISTS "${HOSTED_C6_FW_BIN_RELEASE_GENERIC}")
    set(HOSTED_C6_FW_BIN "${HOSTED_C6_FW_BIN_RELEASE_GENERIC}")
    message(STATUS "Using release C6 firmware image: ${HOSTED_C6_FW_BIN_RELEASE_GENERIC}")
elseif(NOT HOSTED_C6_EXPECTED_VERSION STREQUAL "unknown")
    message(WARNING
        "Missing release C6 firmware image for host version ${HOSTED_C6_EXPECTED_VERSION}. "
        "Expected: ${CMAKE_SOURCE_DIR}/release/network_adapter_esp32c6_${HOSTED_C6_EXPECTED_VERSION}.bin")
endif()

if(HOSTED_C6_FW_BIN)
    set(HOSTED_C6_FW_EMBED "${HOSTED_C6_FW_EMBED_DIR}/hosted_c6_fw.bin")
    configure_file("${HOSTED_C6_FW_BIN}" "${HOSTED_C6_FW_EMBED}" COPYONLY)
    target_add_binary_data(${COMPONENT_LIB} "${HOSTED_C6_FW_EMBED}" BINARY)
    set(APP_HAVE_HOSTED_C6_FW_IMAGE 1)
    message(STATUS "Embedding ESP-Hosted C6 firmware image: ${HOSTED_C6_FW_BIN}")
else()
    message(WARNING
        "No ESP-Hosted C6 firmware image found in release/. "
        "Runtime C6 auto-update will be skipped until a release image is provided.")
endif()
endif() # NOT CONFIG_APP_PANEL_BOARD_WAVESHARE_P4ETH101

target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_HAVE_HOSTED_C6_FW_IMAGE=${APP_HAVE_HOSTED_C6_FW_IMAGE})

# The frame-gap witness (diag/dsi_underrun_watch.c) has to see the scan-out DMA
# completion interrupt, because that interrupt is the only per-frame beacon that
# keeps telling the truth while the flash driver has the caches off.  The DPI
# panel's own callback slot for it is taken by the LVGL adapter, so the witness
# interposes on the one call that owns the DMA channel - IDF's own - with the
# linker's --wrap.
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=dw_gdma_channel_register_event_callbacks" APPEND)

set(SPLASH_HOUSE_IMAGE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/assets/splash_house_image.c")
if(EXISTS "${SPLASH_HOUSE_IMAGE_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${SPLASH_HOUSE_IMAGE_SOURCE}")
endif()

set(SPLASH_SMART86_BETTA_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/assets/SMART86OS_Betta.c")
set(SPLASH_SMART86_BETTA_AVAILABLE 0)
if(EXISTS "${SPLASH_SMART86_BETTA_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${SPLASH_SMART86_BETTA_SOURCE}")
    set(SPLASH_SMART86_BETTA_AVAILABLE 1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_HAVE_SMART86OS_BETTA_IMAGE=${SPLASH_SMART86_BETTA_AVAILABLE})

set(LIGHT_TEMP_BUTTON_IMAGE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/assets/Temp.c")
set(LIGHT_RGB_BUTTON_IMAGE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/assets/RGB.c")
set(LIGHT_RGB_SLIDER_IMAGE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/assets/visiblecolor.c")
set(LIGHT_COLOR_BUTTON_IMAGES_AVAILABLE 0)
set(LIGHT_RGB_SLIDER_IMAGE_AVAILABLE 0)
if(EXISTS "${LIGHT_TEMP_BUTTON_IMAGE_SOURCE}" AND EXISTS "${LIGHT_RGB_BUTTON_IMAGE_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${LIGHT_TEMP_BUTTON_IMAGE_SOURCE}" "${LIGHT_RGB_BUTTON_IMAGE_SOURCE}")
    set(LIGHT_COLOR_BUTTON_IMAGES_AVAILABLE 1)
endif()
if(EXISTS "${LIGHT_RGB_SLIDER_IMAGE_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${LIGHT_RGB_SLIDER_IMAGE_SOURCE}")
    set(LIGHT_RGB_SLIDER_IMAGE_AVAILABLE 1)
endif()
target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        APP_HAVE_LIGHT_COLOR_BUTTON_IMAGES=${LIGHT_COLOR_BUTTON_IMAGES_AVAILABLE}
        APP_HAVE_LIGHT_RGB_SLIDER_IMAGE=${LIGHT_RGB_SLIDER_IMAGE_AVAILABLE})

set(MDI_TOP50_FONT72_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/mditop50icons72.c")
set(MDI_TOP50_FONT56_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/mditop50icons56.c")
set(MDI_TOP50_FONT42_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/mditop50icons42.c")
set(MDI_TOP50_FONT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/mditop50icons.c")
set(MDI_STANDARD_FONT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/mdi_standard_icons_56.c")
set(MDI_WEATHER_FONT20_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/mdiweatherIcons20.c")
set(MDI_ENERGY_FONT42_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/mdi_energy_42.c")
set(MDI_ARROWS_FONT20_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/mdi_arrows_20.c")
set(MDI_TOPBAR_FONT24_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/mdi_topbar_24.c")
set(POPPINS_REGULAR_12_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_12.c")
set(POPPINS_REGULAR_14_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_14.c")
set(POPPINS_REGULAR_16_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_16.c")
set(POPPINS_REGULAR_18_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_18.c")
set(POPPINS_REGULAR_20_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_20.c")
set(POPPINS_REGULAR_22_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_22.c")
set(POPPINS_REGULAR_24_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_24.c")
set(POPPINS_REGULAR_28_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_28.c")
set(POPPINS_REGULAR_34_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_34.c")
set(POPPINS_REGULAR_36_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_36.c")
set(POPPINS_REGULAR_38_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_38.c")
set(POPPINS_REGULAR_40_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/poppins_regular_40.c")
set(CLOCK_DIGITS_84_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/clock_digits_84.c")

set(MDI_TOP50_FONT72_AVAILABLE 0)
if(EXISTS "${MDI_TOP50_FONT72_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_TOP50_FONT72_SOURCE}")
    set(MDI_TOP50_FONT72_AVAILABLE 1)
endif()

set(MDI_TOP50_FONT56_AVAILABLE 0)
if(EXISTS "${MDI_TOP50_FONT56_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_TOP50_FONT56_SOURCE}")
    set(MDI_TOP50_FONT56_AVAILABLE 1)
endif()

set(MDI_TOP50_FONT42_AVAILABLE 0)
if(EXISTS "${MDI_TOP50_FONT42_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_TOP50_FONT42_SOURCE}")
    set(MDI_TOP50_FONT42_AVAILABLE 1)
endif()

set(MDI_WEATHER_FONT20_AVAILABLE 0)
if(EXISTS "${MDI_WEATHER_FONT20_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_WEATHER_FONT20_SOURCE}")
    set(MDI_WEATHER_FONT20_AVAILABLE 1)
endif()

set(APP_HAVE_MDI_ENERGY_FONT_42 0)
if(EXISTS "${MDI_ENERGY_FONT42_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_ENERGY_FONT42_SOURCE}")
    set(APP_HAVE_MDI_ENERGY_FONT_42 1)
endif()

set(APP_HAVE_MDI_ARROWS_FONT_20 0)
if(EXISTS "${MDI_ARROWS_FONT20_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_ARROWS_FONT20_SOURCE}")
    set(APP_HAVE_MDI_ARROWS_FONT_20 1)
endif()

set(APP_HAVE_MDI_TOPBAR_FONT 0)
if(EXISTS "${MDI_TOPBAR_FONT24_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_TOPBAR_FONT24_SOURCE}")
    set(APP_HAVE_MDI_TOPBAR_FONT 1)
endif()

set(APP_HAVE_MDI_ENERGY_FONT_100 0)
if(EXISTS "${MDI_ENERGY_FONT100_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_ENERGY_FONT100_SOURCE}")
    set(APP_HAVE_MDI_ENERGY_FONT_100 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_12 0)
if(EXISTS "${POPPINS_REGULAR_12_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_12_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_12 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_14 0)
if(EXISTS "${POPPINS_REGULAR_14_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_14_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_14 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_16 0)
if(EXISTS "${POPPINS_REGULAR_16_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_16_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_16 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_18 0)
if(EXISTS "${POPPINS_REGULAR_18_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_18_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_18 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_20 0)
if(EXISTS "${POPPINS_REGULAR_20_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_20_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_20 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_22 0)
if(EXISTS "${POPPINS_REGULAR_22_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_22_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_22 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_24 0)
if(EXISTS "${POPPINS_REGULAR_24_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_24_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_24 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_28 0)
if(EXISTS "${POPPINS_REGULAR_28_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_28_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_28 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_34 0)
if(EXISTS "${POPPINS_REGULAR_34_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_34_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_34 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_36 0)
if(EXISTS "${POPPINS_REGULAR_36_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_36_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_36 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_38 0)
if(EXISTS "${POPPINS_REGULAR_38_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_38_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_38 1)
endif()

set(APP_HAVE_POPPINS_REGULAR_40 0)
if(EXISTS "${POPPINS_REGULAR_40_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${POPPINS_REGULAR_40_SOURCE}")
    set(APP_HAVE_POPPINS_REGULAR_40 1)
endif()

set(APP_HAVE_CLOCK_DIGITS_84 0)
if(EXISTS "${CLOCK_DIGITS_84_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${CLOCK_DIGITS_84_SOURCE}")
    set(APP_HAVE_CLOCK_DIGITS_84 1)
endif()

if(MDI_TOP50_FONT42_AVAILABLE OR MDI_TOP50_FONT56_AVAILABLE OR MDI_TOP50_FONT72_AVAILABLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_HAVE_MDI_ICON_FONT=1 APP_HAVE_MDI_TOP50_FONT=1 APP_HAVE_MDI_TOP50_FONT_72=${MDI_TOP50_FONT72_AVAILABLE} APP_HAVE_MDI_TOP50_FONT_56=${MDI_TOP50_FONT56_AVAILABLE} APP_HAVE_MDI_TOP50_FONT_42=${MDI_TOP50_FONT42_AVAILABLE} APP_HAVE_MDI_TOP50_FONT_LEGACY=0 APP_HAVE_MDI_STANDARD_FONT=0 APP_HAVE_MDI_WEATHER_FONT_20=${MDI_WEATHER_FONT20_AVAILABLE})
elseif(EXISTS "${MDI_TOP50_FONT_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_TOP50_FONT_SOURCE}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_HAVE_MDI_ICON_FONT=1 APP_HAVE_MDI_TOP50_FONT=1 APP_HAVE_MDI_TOP50_FONT_72=0 APP_HAVE_MDI_TOP50_FONT_56=0 APP_HAVE_MDI_TOP50_FONT_42=0 APP_HAVE_MDI_TOP50_FONT_LEGACY=1 APP_HAVE_MDI_STANDARD_FONT=0 APP_HAVE_MDI_WEATHER_FONT_20=${MDI_WEATHER_FONT20_AVAILABLE})
elseif(EXISTS "${MDI_STANDARD_FONT_SOURCE}")
    target_sources(${COMPONENT_LIB} PRIVATE "${MDI_STANDARD_FONT_SOURCE}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_HAVE_MDI_ICON_FONT=1 APP_HAVE_MDI_TOP50_FONT=0 APP_HAVE_MDI_TOP50_FONT_72=0 APP_HAVE_MDI_TOP50_FONT_56=0 APP_HAVE_MDI_TOP50_FONT_42=0 APP_HAVE_MDI_TOP50_FONT_LEGACY=0 APP_HAVE_MDI_STANDARD_FONT=1 APP_HAVE_MDI_WEATHER_FONT_20=${MDI_WEATHER_FONT20_AVAILABLE})
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_HAVE_MDI_ICON_FONT=0 APP_HAVE_MDI_TOP50_FONT=0 APP_HAVE_MDI_TOP50_FONT_72=0 APP_HAVE_MDI_TOP50_FONT_56=0 APP_HAVE_MDI_TOP50_FONT_42=0 APP_HAVE_MDI_TOP50_FONT_LEGACY=0 APP_HAVE_MDI_STANDARD_FONT=0 APP_HAVE_MDI_WEATHER_FONT_20=${MDI_WEATHER_FONT20_AVAILABLE})
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_HAVE_MDI_TOPBAR_FONT=${APP_HAVE_MDI_TOPBAR_FONT})

target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        APP_HAVE_POPPINS_REGULAR_12=${APP_HAVE_POPPINS_REGULAR_12}
        APP_HAVE_POPPINS_REGULAR_14=${APP_HAVE_POPPINS_REGULAR_14}
        APP_HAVE_POPPINS_REGULAR_16=${APP_HAVE_POPPINS_REGULAR_16}
        APP_HAVE_POPPINS_REGULAR_18=${APP_HAVE_POPPINS_REGULAR_18}
        APP_HAVE_POPPINS_REGULAR_20=${APP_HAVE_POPPINS_REGULAR_20}
        APP_HAVE_POPPINS_REGULAR_22=${APP_HAVE_POPPINS_REGULAR_22}
        APP_HAVE_POPPINS_REGULAR_24=${APP_HAVE_POPPINS_REGULAR_24}
        APP_HAVE_POPPINS_REGULAR_28=${APP_HAVE_POPPINS_REGULAR_28}
        APP_HAVE_POPPINS_REGULAR_34=${APP_HAVE_POPPINS_REGULAR_34}
        APP_HAVE_POPPINS_REGULAR_36=${APP_HAVE_POPPINS_REGULAR_36}
        APP_HAVE_POPPINS_REGULAR_38=${APP_HAVE_POPPINS_REGULAR_38}
        APP_HAVE_POPPINS_REGULAR_40=${APP_HAVE_POPPINS_REGULAR_40}
        APP_HAVE_CLOCK_DIGITS_84=${APP_HAVE_CLOCK_DIGITS_84}
        APP_HAVE_MDI_ENERGY_FONT_42=${APP_HAVE_MDI_ENERGY_FONT_42}
        APP_HAVE_MDI_ENERGY_FONT_100=${APP_HAVE_MDI_ENERGY_FONT_100}
        APP_HAVE_MDI_ARROWS_FONT_20=${APP_HAVE_MDI_ARROWS_FONT_20})

set(APP_UI_WEATHER_LOTTIE_ASSETS 0)
set(APP_UI_BETTA_LOTTIE_ASSET 0)
if(CONFIG_LV_USE_LOTTIE)
    set(APP_UI_WEATHER_LOTTIE_ASSETS 1)
    set(WEATHER_LOTTIE_DIR "${CMAKE_CURRENT_LIST_DIR}/ui/weather_icons/fill/lottie")
    set(WEATHER_LOTTIE_EMBEDS
        "clear-day:clear_day"
        "clear-night:clear_night"
        "cloudy:cloudy"
        "fog:fog"
        "hail:hail"
        "partly-cloudy-day:partly_cloudy_day"
        "partly-cloudy-night:partly_cloudy_night"
        "sleet:sleet"
        "wind:wind"
        "overcast-day:overcast_day"
        "extreme:extreme"
        "extreme-rain:extreme_rain"
        "rain:rain"
        "snow:snow"
        "thunderstorms:thunderstorms"
        "thunderstorms-rain:thunderstorms_rain")

    foreach(WEATHER_LOTTIE_PAIR IN LISTS WEATHER_LOTTIE_EMBEDS)
        string(REPLACE ":" ";" WEATHER_LOTTIE_PARTS "${WEATHER_LOTTIE_PAIR}")
        list(GET WEATHER_LOTTIE_PARTS 0 WEATHER_LOTTIE_SRC_STEM)
        list(GET WEATHER_LOTTIE_PARTS 1 WEATHER_LOTTIE_DST_STEM)

        set(WEATHER_LOTTIE_SRC_FILE "${WEATHER_LOTTIE_DIR}/${WEATHER_LOTTIE_SRC_STEM}.json")
        set(WEATHER_LOTTIE_EMBED_FILE "${CMAKE_CURRENT_BINARY_DIR}/weather_${WEATHER_LOTTIE_DST_STEM}.json")
        if(EXISTS "${WEATHER_LOTTIE_SRC_FILE}")
            configure_file("${WEATHER_LOTTIE_SRC_FILE}" "${WEATHER_LOTTIE_EMBED_FILE}" COPYONLY)
            target_add_binary_data(${COMPONENT_LIB} "${WEATHER_LOTTIE_EMBED_FILE}" TEXT)
        else()
            set(APP_UI_WEATHER_LOTTIE_ASSETS 0)
            message(WARNING "Missing weather lottie asset: ${WEATHER_LOTTIE_SRC_FILE}")
        endif()
    endforeach()

    # Easter-egg betta fish lottie (shown after 7 taps on the nav-bar home button).
    set(BETTA_LOTTIE_SRC_FILE "${CMAKE_CURRENT_LIST_DIR}/ui/assets/betta.json")
    set(BETTA_LOTTIE_EMBED_FILE "${CMAKE_CURRENT_BINARY_DIR}/betta.json")
    if(EXISTS "${BETTA_LOTTIE_SRC_FILE}")
        configure_file("${BETTA_LOTTIE_SRC_FILE}" "${BETTA_LOTTIE_EMBED_FILE}" COPYONLY)
        target_add_binary_data(${COMPONENT_LIB} "${BETTA_LOTTIE_EMBED_FILE}" TEXT)
        set(APP_UI_BETTA_LOTTIE_ASSET 1)
    else()
        message(WARNING "Missing betta lottie asset: ${BETTA_LOTTIE_SRC_FILE}")
    endif()
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_UI_WEATHER_LOTTIE_ASSETS=${APP_UI_WEATHER_LOTTIE_ASSETS})
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_UI_BETTA_LOTTIE_ASSET=${APP_UI_BETTA_LOTTIE_ASSET})

# Keep strict warnings in project code, but silence an upstream ThorVG warning in lvgl.
if(TARGET __idf_lvgl__lvgl)
    target_compile_options(__idf_lvgl__lvgl PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-type-limits>)
endif()

