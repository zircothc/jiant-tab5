/* SPDX-License-Identifier: GPL-3.0-only
 * A minimal USB MIDI class host on ESP-IDF's usb_host API: finds the MIDIStreaming interface (class 1, subclass 3),
 * claims it, keeps one bulk-IN transfer in flight and hands every 4-byte USB-MIDI event packet to a callback.
 * Input only: the SLOOP port does not send MIDI to the keyboard. */
#include <string.h>
#include "usb_midi_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#define TAG "usb_midi"
#define MAX_DEV 3

typedef struct {
    uint8_t used, addr, iface, ep_in, gone, inflight, restarts;
    uint16_t mps;
    usb_device_handle_t dev;
    usb_transfer_t *xfer;
} midi_dev_t;

static midi_dev_t s_dev[MAX_DEV];
static usb_host_client_handle_t s_client;
static usb_midi_rx_cb s_cb;

int usb_midi_host_devices(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_DEV; i++)
        if (s_dev[i].used && !s_dev[i].gone)
            n++;
    return n;
}

/* the first MIDIStreaming interface (alternate setting 0) with a bulk or interrupt IN endpoint */
static int find_midi_in(const usb_config_desc_t *cfg, uint8_t *iface, uint8_t *ep, uint16_t *mps)
{
    const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
    int cur_if = -1, midi = 0, alt0 = 0;
    while (p + 2 <= end && p[0] >= 2) {
        if (p[1] == 4 && p[0] >= 9) {                       /* interface descriptor */
            cur_if = p[2];
            alt0 = p[3] == 0;
            midi = p[5] == 0x01 && p[6] == 0x03;           /* audio class, MIDIStreaming */
        } else if (p[1] == 5 && p[0] >= 7 && midi && alt0 && (p[2] & 0x80) && ((p[3] & 3) == 2 || (p[3] & 3) == 3)) {
            *iface = (uint8_t)cur_if;                       /* an IN endpoint, bulk (2) or interrupt (3) */
            *ep = p[2];
            *mps = (uint16_t)((p[4] | (p[5] << 8)) & 0x7FF);
            return 1;
        }
        p += p[0];
    }
    return 0;
}

static void xfer_cb(usb_transfer_t *t)
{
    midi_dev_t *d = (midi_dev_t *)t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        int i;
        for (i = 0; i + 4 <= t->actual_num_bytes; i += 4) {
            const uint8_t *b = t->data_buffer + i;
            if ((b[0] & 0x0F) < 2)                          /* CIN 0, 1: reserved / padding */
                continue;
            if (s_cb)
                s_cb((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
        }
        if (!d->gone) {
            t->num_bytes = d->mps;
            if (usb_host_transfer_submit(t) == ESP_OK)
                return;
        }
    } else if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        d->gone = 1;
    }
    d->inflight = 0;                                        /* the sweep decides: restart, or clean up */
}

static void open_device(uint8_t addr)
{
    int i, slot = -1;
    usb_device_handle_t dev;
    const usb_config_desc_t *cfg;
    uint8_t iface, ep;
    uint16_t mps;
    for (i = 0; i < MAX_DEV; i++) {
        if (s_dev[i].used && s_dev[i].addr == addr)
            return;                                         /* already ours */
        if (!s_dev[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0 || usb_host_device_open(s_client, addr, &dev) != ESP_OK)
        return;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || !find_midi_in(cfg, &iface, &ep, &mps) || mps == 0 ||
        usb_host_interface_claim(s_client, dev, iface, 0) != ESP_OK) {
        usb_host_device_close(s_client, dev);               /* not a MIDI device (or it is busy): not ours */
        return;
    }
    memset(&s_dev[slot], 0, sizeof s_dev[slot]);
    s_dev[slot].used = 1;
    s_dev[slot].addr = addr;
    s_dev[slot].iface = iface;
    s_dev[slot].ep_in = ep;
    s_dev[slot].mps = mps;
    s_dev[slot].dev = dev;
    if (usb_host_transfer_alloc(mps, 0, &s_dev[slot].xfer) != ESP_OK) {
        usb_host_interface_release(s_client, dev, iface);
        usb_host_device_close(s_client, dev);
        s_dev[slot].used = 0;
        return;
    }
    s_dev[slot].xfer->device_handle = dev;
    s_dev[slot].xfer->bEndpointAddress = ep;
    s_dev[slot].xfer->callback = xfer_cb;
    s_dev[slot].xfer->context = &s_dev[slot];
    s_dev[slot].xfer->num_bytes = mps;                      /* a multiple of the endpoint's packet size, as IN needs */
    s_dev[slot].inflight = 1;
    if (usb_host_transfer_submit(s_dev[slot].xfer) != ESP_OK)
        s_dev[slot].inflight = 0;
    ESP_LOGI(TAG, "MIDI device at address %u: interface %u, IN endpoint 0x%02X, packet size %u", addr, iface, ep, mps);
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    int i;
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        open_device(msg->new_dev.address);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (i = 0; i < MAX_DEV; i++)
            if (s_dev[i].used && s_dev[i].dev == msg->dev_gone.dev_hdl)
                s_dev[i].gone = 1;                          /* freed by the sweep once its transfer has returned */
    }
}

/* after each batch of events: free what has gone, restart what stopped without the device leaving */
static void sweep(void)
{
    int i;
    for (i = 0; i < MAX_DEV; i++) {
        midi_dev_t *d = &s_dev[i];
        if (!d->used || d->inflight)
            continue;
        if (d->gone) {
            usb_host_transfer_free(d->xfer);
            usb_host_interface_release(s_client, d->dev, d->iface);
            usb_host_device_close(s_client, d->dev);
            ESP_LOGI(TAG, "MIDI device at address %u removed", d->addr);
            memset(d, 0, sizeof *d);
        } else if (d->restarts < 20) {                      /* a stall or an error: clear the endpoint, try again */
            d->restarts++;
            usb_host_endpoint_clear(d->dev, d->ep_in);
            d->xfer->num_bytes = d->mps;
            d->inflight = 1;
            if (usb_host_transfer_submit(d->xfer) != ESP_OK)
                d->inflight = 0;
        }
    }
}

static void client_task(void *arg)
{
    uint8_t addrs[8];
    int n = 0, i;
    (void)arg;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) == ESP_OK)   /* devices already plugged in */
        for (i = 0; i < n; i++)
            open_device(addrs[i]);
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(50));
        sweep();
    }
}

esp_err_t usb_midi_host_start(usb_midi_rx_cb cb)
{
    usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {.client_event_callback = client_event_cb, .callback_arg = NULL},
    };
    esp_err_t e;
    s_cb = cb;
    e = usb_host_client_register(&cfg, &s_client);
    if (e != ESP_OK)
        return e;
    return xTaskCreate(client_task, "usb_midi", 4096, NULL, 9, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
