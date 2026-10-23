/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "esp_err.h"

/* One USB-MIDI event packet as the device sent it (4 bytes: cable/CIN, status, d1, d2), little-endian in a uint32.
 * Called from the USB client task: do not block. */
typedef void (*usb_midi_rx_cb)(uint32_t pkt);

/* Class-compliant USB MIDI devices (keyboards, pads, controllers, interfaces), up to 3 at once, hot-plug.
 * Needs the USB Host library installed first (bsp_usb_host_start does it and powers the port). */
esp_err_t usb_midi_host_start(usb_midi_rx_cb cb);
int usb_midi_host_devices(void);                  /* how many are plugged in and streaming now */
