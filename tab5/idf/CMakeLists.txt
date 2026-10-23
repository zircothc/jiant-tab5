# SPDX-License-Identifier: GPL-3.0-only
# JIANT FM (M-VAVE FM-1 firmware) on the M5Stack Tab5 (ESP32-P4).
#   idf.py build                         the real Tab5 (pulls espressif/m5stack_tab5_noglib from the component registry)
#   idf.py -DJIANT_BOARD=null build      headless: compiles and sizes the firmware without board drivers (works offline)
cmake_minimum_required(VERSION 3.16)
set(JIANT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "checkout of juanjiant-bit/JIANT this tab5/ directory lives in")
set(JIANT_BOARD "bsp" CACHE STRING "board layer: bsp (the Tab5) | null (headless)")
# the generated headers (fonts, icons, tables...) ship pre-built in tab5/gen
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components" "${CMAKE_CURRENT_LIST_DIR}/boards/${JIANT_BOARD}")
# this firmware is for the ESP32-P4 only: do not let a missing .vscode setting fall back to plain "esp32"
set(ENV{IDF_TARGET} esp32p4)
set(IDF_TARGET esp32p4 CACHE STRING "target chip" FORCE)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jiant_tab5)
