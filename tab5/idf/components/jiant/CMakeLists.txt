# SPDX-License-Identifier: GPL-3.0-only
# JIANT firmware + the Tab5 platform layer, compiled as the single unit tab5_platform.c is.
idf_component_register(
    SRCS "${JIANT_ROOT}/tab5/tab5_platform.c" "${JIANT_ROOT}/tab5/panel_ui.c"
    INCLUDE_DIRS "${JIANT_ROOT}/tab5" "include"
    REQUIRES nvs_flash
    PRIV_INCLUDE_DIRS "${JIANT_ROOT}/tab5/gen" "${JIANT_ROOT}/firmware/hal" "${JIANT_ROOT}/firmware/src")
# -w: the firmware is not warning-clean by IDF's standards; TAB5_STRIP_ATTR: no JieLi linker sections here
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w -std=gnu17 -DTAB5_STRIP_ATTR)

# The firmware's static data is ~800 KB of .bss, more than the P4's internal RAM leaves free. These buffers are
# large and accessed sequentially (delay line, punch ring, audio ring, screen frame, UI canvas), so they go to
# PSRAM: their sections are renamed in the finished archive, no source is touched. (-fdata-sections names a
# file-scope static `x` as `.bss.x`; IDF's linker script collects `.ext_ram.bss.*` into PSRAM.)
set(JIANT_PSRAM_BUFFERS proj_slot proj_scratch rev_x rev_comb sl_buf dly_u comb_buf sec_stage tab5_ring)
set(_rename_args "")
foreach(_s ${JIANT_PSRAM_BUFFERS})
    list(APPEND _rename_args --rename-section .bss.${_s}=.ext_ram.bss.${_s})
endforeach()
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${_rename_args} $<TARGET_FILE:${COMPONENT_LIB}>
    COMMENT "jiant: moving the big buffers to PSRAM")

# ESP32-P4 silicon older than v3 (what the official Tab5 examples build for) needs this option; a binary built for
# v3.1+ is rejected at boot by older chips. It comes from sdkconfig.defaults: if it is missing, that file was not used.
if(CONFIG_IDF_TARGET_ESP32P4 AND NOT CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
    message(WARNING "jiant: building for ESP32-P4 rev v3.1+ (CONFIG_ESP32P4_SELECTS_REV_LESS_V3 is off). "
                    "If your Tab5 has an older chip it will not boot. Delete sdkconfig and build/, then set the target again.")
endif()
