# SPDX-License-Identifier: LicenseRef-FNCL-1.1
# Copyright (c) 2026 Cpt_Kirk
cmake_minimum_required(VERSION 3.16)

set(BETTAOS_RELEASE_VERSION "v0.8.2")

# sdkconfig.h is not yet generated here (top-level CMake runs before the
# application's kconfig). We infer the panel variant from the SDKCONFIG_DEFAULTS
# list so PROJECT_VER / project name reflect the target hardware.
set(_PV_SUFFIX "")
set(_PROJECT_NAME "betta-ha-panel")
set(_PANEL_MANIFEST "")
foreach(_sdk ${SDKCONFIG_DEFAULTS})
    if(_sdk MATCHES "p4eth101")
        set(_PV_SUFFIX "-p4eth")
        set(_PROJECT_NAME "betta-ha-panel-p4eth")
        set(_PANEL_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/main/idf_component.p4eth101.yml")
    elseif(_sdk MATCHES "panel43")
        set(_PV_SUFFIX "-43")
        set(_PROJECT_NAME "betta-ha-panel-43")
        set(_PANEL_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/main/idf_component.panel43.yml")
    endif()
endforeach()
if(_PANEL_MANIFEST STREQUAL "")
    message(FATAL_ERROR
        "Pick a board: -D SDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.defaults.panel43\" "
        "(Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3) or "
        "\"sdkconfig.defaults;sdkconfig.defaults.p4eth101\" (ESP32-P4-POE-ETH-KIT-B 10.1\")")
endif()
set(PROJECT_VER "${BETTAOS_RELEASE_VERSION}${_PV_SUFFIX}")

# The IDF component manager does not support Kconfig-based rules in
# idf_component.yml, so we materialise the variant-specific manifest here,
# BEFORE project() runs. The generated main/idf_component.yml is a build
# artifact and is listed in .gitignore.
configure_file("${_PANEL_MANIFEST}"
               "${CMAKE_CURRENT_LIST_DIR}/main/idf_component.yml"
               COPYONLY)
message(STATUS "betta-ha-panel variant manifest: ${_PANEL_MANIFEST}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${_PROJECT_NAME})

# The component manager (running inside project()) has populated
# managed_components/ by now, so this is the point where our fixes for the
# patched vendor components can be (re)applied. managed_components/ is
# git-ignored, so the patch files in patches/ are the published record of those
# fixes - see cmake/apply_vendor_patches.cmake.
set(_VENDOR_PATCH_ROOT "${CMAKE_CURRENT_LIST_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/apply_vendor_patches.cmake")

if(TARGET __idf_espressif__esp_audio_codec)
    foreach(_betta_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_betta_libs __idf_espressif__esp_audio_codec ${_betta_prop})
        if(_betta_libs)
            list(FILTER _betta_libs EXCLUDE REGEX "^-L ")
            set_target_properties(__idf_espressif__esp_audio_codec PROPERTIES ${_betta_prop} "${_betta_libs}")
        endif()
        unset(_betta_libs)
    endforeach()
    message(STATUS "betta-ha-panel: esp_audio_codec link dir is passed by absolute path only (unquoted '-L' breaks linking from paths containing spaces)")
    unset(_betta_prop)
endif()
