# SPDX-License-Identifier: LicenseRef-FNCL-1.1
# Copyright (c) 2026 Cpt_Kirk
#
# Re-apply BettaOS fixes to components that the ESP-IDF component manager
# downloads into managed_components/.  That directory is git-ignored and is
# (re)created on every configure, so without this step a fresh clone would
# silently build the unpatched vendor code again - and, for the esp_hosted cases
# below, would panic on the first large internal-DMA-starved SDIO Rx burst and
# on any Tx buffer allocation failure.
#
# The component manager runs during configure (idf_build_process via project()),
# so this include must stay AFTER project() in the top level CMakeLists.txt.
#
# Entry format:
#   <patch file, relative to the project>|<file carrying the fix, relative to the
#   project>|<marker string proving the fix is present>
#
# The patch is applied with the project root as git's working directory and its
# paths are project-root relative: git apply silently *skips* (exit code 0!) any
# path that is not under the current repository prefix, so the marker is checked
# again after applying to make that failure mode loud instead of silent.
#
# git apply runs with --ignore-whitespace because the component manager may
# materialise managed_components/ with either LF or CRLF endings depending on how
# it fetched the component; without the flag such a checkout looks like a context
# mismatch and a clean clone fails to configure.
#
# A patch is skipped when its target does not exist (component not downloaded
# for this variant), and when the marker is already found.  Anything else that
# goes wrong is a hard error on purpose: building unpatched vendor code must not
# happen quietly.

if(NOT DEFINED _VENDOR_PATCH_ROOT)
    set(_VENDOR_PATCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

set(_VENDOR_PATCHES
    "patches/esp_hosted_2.11.7_sdio_streaming_rx_graceful.patch|managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c|KB-PATCH"
    "patches/esp_hosted_2.11.7_transport_tx_graceful.patch|managed_components/espressif__esp_hosted/host/drivers/transport/transport_drv.c|KB-PATCH"
)

find_program(_VENDOR_PATCH_GIT git)

foreach(_entry IN LISTS _VENDOR_PATCHES)
    string(REPLACE "|" ";" _fields "${_entry}")
    list(GET _fields 0 _patch)
    list(GET _fields 1 _target)
    list(GET _fields 2 _marker)

    set(_patch_abs "${_VENDOR_PATCH_ROOT}/${_patch}")
    set(_target_abs "${_VENDOR_PATCH_ROOT}/${_target}")

    if(NOT EXISTS "${_target_abs}")
        message(STATUS "vendor patch: ${_target} not present, skipping")
        continue()
    endif()

    if(NOT EXISTS "${_patch_abs}")
        message(FATAL_ERROR "vendor patch: ${_patch} is missing, but ${_target} is not patched")
    endif()

    file(READ "${_target_abs}" _target_content)
    string(FIND "${_target_content}" "${_marker}" _marker_pos)
    if(NOT _marker_pos EQUAL -1)
        message(STATUS "vendor patch: ${_target} is up to date")
        continue()
    endif()

    if(NOT _VENDOR_PATCH_GIT)
        message(FATAL_ERROR "vendor patch: git was not found, cannot apply ${_patch}")
    endif()

    execute_process(
        COMMAND "${_VENDOR_PATCH_GIT}" apply --ignore-whitespace -p1 "${_patch_abs}"
        WORKING_DIRECTORY "${_VENDOR_PATCH_ROOT}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)

    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "vendor patch ${_patch} does not apply to ${_target} (git said: ${_err}${_out}). "
            "The upstream component probably changed - refresh the patch instead of "
            "building unpatched vendor code.")
    endif()

    file(READ "${_target_abs}" _target_content)
    string(FIND "${_target_content}" "${_marker}" _marker_pos)
    if(_marker_pos EQUAL -1)
        message(FATAL_ERROR
            "vendor patch ${_patch} did not change ${_target}. git apply reports success when it "
            "skips paths outside the current repository prefix, so check that the project root is "
            "the git work tree root (git rev-parse --show-toplevel) and that the paths inside "
            "${_patch} are relative to it.")
    endif()

    message(STATUS "vendor patch applied: ${_target}")
endforeach()

unset(_VENDOR_PATCHES)
unset(_VENDOR_PATCH_ROOT)
