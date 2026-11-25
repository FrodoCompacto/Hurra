#include "usb_trace.h"

#include <stdbool.h>
#include <stdio.h>
#include "pico/critical_section.h"
#include "pico/time.h"
#include "pio_usb_configuration.h"
#include "tusb.h"

#define TRACE_DEPTH 12

typedef struct {
    uint32_t ms;
    usb_trace_kind_t kind;
    uint8_t dev;
    uint8_t detail;
} trace_event_t;

typedef struct {
    trace_event_t ring[TRACE_DEPTH];
    unsigned next;
    unsigned count;
    bool frozen;
    uint32_t root_ms, hub_ep_ms, mouse_ms, key_ms;
    uint32_t hid_ms[CFG_TUH_HID];
} trace_log_t;

static critical_section_t lock;
static trace_log_t log_data;
static bool hid_device[CFG_TUH_DEVICE_MAX + 1];
static struct {
    uint8_t dev, ep, failures;
    bool delivered;
} endpoints[PIO_USB_EP_POOL_CNT];

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

// Called with the lock held. Only the serial reader formats text.
static void append(uint32_t ms, usb_trace_kind_t kind, uint8_t dev, uint8_t detail) {
    log_data.ring[log_data.next] = (trace_event_t){ms, kind, dev, detail};
    log_data.next = (log_data.next + 1) % TRACE_DEPTH;
    if (log_data.count < TRACE_DEPTH) log_data.count++;
}

void usb_trace_init(void) {
    critical_section_init(&lock);
    append(now_ms(), TRACE_BOOT, 0, 0);
}

void usb_trace_event(usb_trace_kind_t kind, uint8_t dev, uint8_t detail) {
    critical_section_enter_blocking(&lock);
    if (!log_data.frozen) {
        uint32_t ms = now_ms();
        switch (kind) {
        case TRACE_ROOT_CONNECT: log_data.root_ms = ms; break;
        case TRACE_HID_MOUNT:
            if (detail < CFG_TUH_HID) log_data.hid_ms[detail] = ms;
            if (dev <= CFG_TUH_DEVICE_MAX) hid_device[dev] = true;
            break;
        case TRACE_UNMOUNT:
            if (dev <= CFG_TUH_DEVICE_MAX) hid_device[dev] = false;
            break;
        case TRACE_MOUSE: log_data.mouse_ms = ms; break;
        case TRACE_KEY: log_data.key_ms = ms; break;
        default: break;
        }
        append(ms, kind, dev, detail);
    }
    critical_section_exit(&lock);
}

void usb_trace_open(unsigned slot, uint8_t dev, uint8_t ep) {
    critical_section_enter_blocking(&lock);
    if (!log_data.frozen) {
        endpoints[slot].dev = dev;
        endpoints[slot].ep = ep;
        endpoints[slot].delivered = false;
        endpoints[slot].failures = 0;
        uint32_t ms = now_ms();
        // TinyUSB allocates hub addresses after the non-hub address range.
        if (dev > CFG_TUH_DEVICE_MAX) log_data.hub_ep_ms = ms;
        append(ms, TRACE_OPEN, dev, ep);
    }
    critical_section_exit(&lock);
}

void usb_trace_in(unsigned slot, usb_trace_kind_t result) {
    critical_section_enter_blocking(&lock);
    if (!log_data.frozen) {
        uint8_t dev = endpoints[slot].dev;
        if (result == TRACE_DATA || result == TRACE_NAK) {
            endpoints[slot].failures = 0;
            if (result == TRACE_DATA) endpoints[slot].delivered = true;
        } else {
            append(now_ms(), result, dev, endpoints[slot].ep);
            if (endpoints[slot].delivered && dev <= CFG_TUH_DEVICE_MAX &&
                hid_device[dev] && ++endpoints[slot].failures >= 3) {
                log_data.frozen = true;
            }
        }
    }
    critical_section_exit(&lock);
}

void usb_trace_format(char *buf, size_t capacity) {
    static const char *const names[] = {
        [TRACE_PACKET]="packet", [TRACE_NO_START]="no_start", [TRACE_NO_EOP]="no_eop",
        [TRACE_CRC]="crc", [TRACE_ACK_TX]="ack_tx", [TRACE_TOKEN_TX]="token_tx",
        [TRACE_STALL]="stall", [TRACE_BOOT]="boot", [TRACE_ROOT_CONNECT]="root_connect",
        [TRACE_OPEN]="ep_open", [TRACE_HID_MOUNT]="hid_mount", [TRACE_UNMOUNT]="unmount",
        [TRACE_MOUSE]="mouse", [TRACE_KEY]="key"
    };
    critical_section_enter_blocking(&lock);
    trace_log_t snapshot = log_data;
    critical_section_exit(&lock);

    int n = snprintf(buf, capacity,
        "KMBOX_TRACE:%s root_ms=%lu hub_ep_ms=%lu mouse_ms=%lu key_ms=%lu\r\n",
        snapshot.frozen ? "frozen" : "live", (unsigned long)snapshot.root_ms,
        (unsigned long)snapshot.hub_ep_ms,
        (unsigned long)snapshot.mouse_ms, (unsigned long)snapshot.key_ms);
    if (n < 0 || (size_t)n >= capacity) return;
    size_t used = (size_t)n;
    for (unsigned itf = 0; itf < CFG_TUH_HID; itf++) {
        if (!snapshot.hid_ms[itf]) continue;
        n = snprintf(buf + used, capacity - used, "hid_itf=%u mount_ms=%lu\r\n",
                     itf, (unsigned long)snapshot.hid_ms[itf]);
        if (n < 0 || (size_t)n >= capacity - used) return;
        used += (size_t)n;
    }
    unsigned i = (snapshot.next + TRACE_DEPTH - snapshot.count) % TRACE_DEPTH;
    for (unsigned j = 0; j < snapshot.count; j++, i = (i + 1) % TRACE_DEPTH) {
        trace_event_t e = snapshot.ring[i];
        n = snprintf(buf + used, capacity - used, "%lu %s dev=%u arg=%02X\r\n",
                     (unsigned long)e.ms, names[e.kind], e.dev, e.detail);
        if (n < 0 || (size_t)n >= capacity - used) break;
        used += (size_t)n;
    }
}
