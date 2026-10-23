#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the SLOOP / Tab5 platform layer for the PC (the step before ESP-IDF).
# Run from the repo root:  tab5/build_host.sh   ->  build/tab5/tab5_host
set -e
cd "$(dirname "$0")/.."
OUT=build/tab5
mkdir -p "$OUT" build/gen
python3 tab5/gen_headers.py
${CC:-cc} -O2 -w -DTAB5_HOST -Itab5/gen -Ifirmware/hal -Ifirmware/src -Itab5 \
    -c tab5/tab5_platform.c -o "$OUT/tab5_platform.o" -lm
${CC:-cc} -O2 -w -Itab5 tab5/tab5_host.c "$OUT/tab5_platform.o" -o "$OUT/tab5_host" -lm
${CC:-cc} -O2 -w -DTAB5_HOST -Itab5/gen -Ifirmware/hal -Ifirmware/src -Itab5 tab5/tab5_platform.c tab5/panel_ui.c tab5/panel_test.c -o "$OUT/panel_test" -lm
${CC:-cc} -O2 -w -DTAB5_HOST -Itab5/gen -Ifirmware/hal -Ifirmware/src -Itab5 tab5/tab5_platform.c tab5/panel_ui.c tab5/panel_midi_test.c -o "$OUT/panel_midi_test" -lm
echo "built $OUT/tab5_host, $OUT/panel_test and $OUT/panel_midi_test"
