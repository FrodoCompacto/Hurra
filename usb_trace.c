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
    struct {
        uint32_t before_ms, after_ms;
    } milestones[TRACE_MILESTONE_COUNT];
} trace_log_t;

static critical_section_t lock;
static trace_log_t log_data;
static bool hid_device[CFG_TUH_DEVICE_MAX + 1];
typedef struct {
    uint32_t ms;
    uint8_t dev, type, request;
    uint16_t value, index;
} setup_record_t;

typedef struct {
    uint32_t ms;
    uint8_t dev;
    uint16_t port, status, change;
} hub_record_t;

static setup_record_t last_setup;
static hub_record_t last_port;

typedef struct {
    uint8_t dev, ep;
    bool open, delivered;
    uint32_t data, nak, errors, streak;
    usb_bus_sample_t good, first, latest;
    setup_record_t setup;
    hub_record_t port;
} bus_endpoint_t;

// The first persistent failure survives subsequent transfers, close and slot reuse.
static bus_endpoint_t live_bus[PIO_USB_EP_POOL_CNT], fault_bus[PIO_USB_EP_POOL_CNT];

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

// Keep the last mark before the HID fault and the first one afterwards.
// The HID event ring remains frozen even if USB reconnects or DMA starts later.
void usb_trace_mark(usb_trace_milestone_t milestone) {
    critical_section_enter_blocking(&lock);
    if (!log_data.frozen) {
        log_data.milestones[milestone].before_ms = now_ms();
    } else if (!log_data.milestones[milestone].after_ms) {
        log_data.milestones[milestone].after_ms = now_ms();
    }
    critical_section_exit(&lock);
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

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void usb_trace_setup(uint8_t dev, const uint8_t *setup) {
    critical_section_enter_blocking(&lock);
    last_setup = (setup_record_t){now_ms(), dev, setup[0], setup[1],
                                  le16(setup + 2), le16(setup + 4)};
    critical_section_exit(&lock);
}

void usb_trace_control_in(uint8_t dev, const uint8_t *data, int len) {
    critical_section_enter_blocking(&lock);
    // TinyUSB serializes EP0. Only interpret a hub class GET_STATUS(port) reply.
    if (len == 4 && dev > CFG_TUH_DEVICE_MAX && dev == last_setup.dev &&
        last_setup.type == 0xa3 && last_setup.request == 0) {
        last_port = (hub_record_t){now_ms(), dev, last_setup.index, le16(data), le16(data + 2)};
    }
    critical_section_exit(&lock);
}

void usb_trace_close(unsigned slot) {
    critical_section_enter_blocking(&lock);
    live_bus[slot].open = false;
    critical_section_exit(&lock);
}

void usb_trace_open(unsigned slot, uint8_t dev, uint8_t ep) {
    critical_section_enter_blocking(&lock);
    live_bus[slot] = (bus_endpoint_t){.dev = dev, .ep = ep, .open = true};
    if (!log_data.frozen) {
        uint32_t ms = now_ms();
        // TinyUSB allocates hub addresses after the non-hub address range.
        if (dev > CFG_TUH_DEVICE_MAX) log_data.hub_ep_ms = ms;
        append(ms, TRACE_OPEN, dev, ep);
    }
    critical_section_exit(&lock);
}

void usb_trace_in(unsigned slot, const usb_bus_sample_t *sample) {
    usb_trace_kind_t result = sample->result;
    critical_section_enter_blocking(&lock);
    bus_endpoint_t *bus = &live_bus[slot];
    bus->latest = *sample;
    if (result == TRACE_DATA || result == TRACE_NAK) {
        if (result == TRACE_DATA) {
            bus->data++;
            bus->delivered = true;
        } else {
            bus->nak++;
        }
        bus->streak = 0;
        bus->good = *sample;
    } else {
        bus->errors++;
        if (bus->streak == 0) {
            bus->first = *sample;
            bus->setup = last_setup;
            bus->port = last_port;
        }
        if (bus->streak < 3) {
            bus->streak++;
            if (bus->streak == 3 && !fault_bus[slot].ep) fault_bus[slot] = *bus;
        }
        if (!log_data.frozen) {
            append(now_ms(), result, bus->dev, bus->ep);
            if (bus->delivered && bus->streak == 3 && bus->dev <= CFG_TUH_DEVICE_MAX &&
                hid_device[bus->dev]) log_data.frozen = true;
        }
    }
    critical_section_exit(&lock);
}

static const char *const names[] = {
        [TRACE_PACKET]="packet", [TRACE_NO_START]="no_start", [TRACE_NO_EOP]="no_eop",
        [TRACE_CRC]="crc", [TRACE_ACK_TX]="ack_tx", [TRACE_TOKEN_TX]="token_tx",
        [TRACE_STALL]="stall", [TRACE_BOOT]="boot", [TRACE_ROOT_CONNECT]="root_connect",
        [TRACE_OPEN]="ep_open", [TRACE_HID_MOUNT]="hid_mount", [TRACE_UNMOUNT]="unmount",
        [TRACE_DATA]="data", [TRACE_NAK]="nak", [TRACE_MOUSE]="mouse", [TRACE_KEY]="key"
    };

void usb_trace_format(char *buf, size_t capacity) {
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
    static const char *const milestones[TRACE_MILESTONE_COUNT] = {
        [TRACE_PC_DISCONNECT]="pc_disconnect", [TRACE_PC_CONNECT]="pc_connect",
        [TRACE_PC_MOUNT]="pc_mount", [TRACE_UART_DMA_INIT]="uart_dma_init",
        [TRACE_UART_DMA_RX]="uart_dma_rx", [TRACE_UART_DMA_TX_READY]="uart_dma_tx_ready"
    };
    for (unsigned m = 0; m < TRACE_MILESTONE_COUNT; m++) {
        if (!snapshot.milestones[m].before_ms && !snapshot.milestones[m].after_ms) continue;
        n = snprintf(buf + used, capacity - used, "%s before_ms=%lu after_ms=%lu\r\n",
                     milestones[m], (unsigned long)snapshot.milestones[m].before_ms,
                     (unsigned long)snapshot.milestones[m].after_ms);
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

// One requested page fits in the existing UART DMA record. Copy the endpoint
// under the lock, then format on Core0 without holding up USB host polling.
static size_t format_sample(char *buf, size_t cap, const char *label, const usb_bus_sample_t *s) {
    if (!s->result || !cap) return 0;
    int n = snprintf(buf, cap,
        "%s %s us=%lu frame=%lu gap_us=%lu poll_us=%lu sof=%u ls=%u pre=%u "
        "dp=%u dm=%u tx_oe=%u irq=%02X fifo=%u rx_pc=%u edge_pc=%u tx_pc=%u rx_on=%u edge_on=%u pid_buf=%02X\r\n",
        label, names[s->result], (unsigned long)s->us, (unsigned long)s->frame,
        (unsigned long)s->frame_gap_us, (unsigned long)s->elapsed_us, s->sof_sent,
        s->low_speed, s->pre, s->dp, s->dm, s->tx_oe, s->irq, s->fifo, s->rx_pc, s->edge_pc,
        s->tx_pc, s->rx_enabled, s->edge_enabled, s->pid_byte);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

void usb_trace_bus_index(char *buf, size_t cap, uint32_t frame, uint32_t core1_beats, const char *build) {
    bool pages[2 * PIO_USB_EP_POOL_CNT];
    critical_section_enter_blocking(&lock);
    for (unsigned i = 0; i < PIO_USB_EP_POOL_CNT; i++) {
        pages[2 * i] = live_bus[i].ep != 0;
        pages[2 * i + 1] = fault_bus[i].ep != 0;
    }
    critical_section_exit(&lock);
    int n = snprintf(buf, cap, "KMBOX_BUS:v1 build=%s ms=%lu frame=%lu core1_beats=%lu pages=",
                     build, (unsigned long)now_ms(), (unsigned long)frame, (unsigned long)core1_beats);
    if (n < 0 || (size_t)n >= cap) return;
    size_t used = (size_t)n;
    for (unsigned i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        if (!pages[i]) continue;
        n = snprintf(buf + used, cap - used, " %u", i);
        if (n < 0 || (size_t)n >= cap - used) return;
        used += (size_t)n;
    }
    snprintf(buf + used, cap - used, "\r\n");
}

void usb_trace_bus_page(unsigned page, char *buf, size_t cap) {
    unsigned slot = page / 2;
    if (slot >= PIO_USB_EP_POOL_CNT) {
        snprintf(buf, cap, "KMBOX_BUS:invalid page\r\n");
        return;
    }
    bool frozen = page % 2 != 0;
    critical_section_enter_blocking(&lock);
    bus_endpoint_t ep = frozen ? fault_bus[slot] : live_bus[slot];
    critical_section_exit(&lock);
    int n = snprintf(buf, cap, "KMBOX_BUS:%s slot=%u dev=%u ep=%02X open=%u data=%lu nak=%lu err=%lu\r\n",
                     frozen ? "fault" : "live", slot, ep.dev, ep.ep, ep.open,
                     (unsigned long)ep.data, (unsigned long)ep.nak, (unsigned long)ep.errors);
    if (n < 0 || (size_t)n >= cap) return;
    size_t used = (size_t)n;
    used += format_sample(buf + used, cap - used, "good", &ep.good);
    if (ep.streak) {
        used += format_sample(buf + used, cap - used, "first_bad", &ep.first);
        if (ep.setup.ms) {
            n = snprintf(buf + used, cap - used,
                         "setup ms=%lu dev=%u type=%02X req=%02X value=%04X index=%04X\r\n",
                         (unsigned long)ep.setup.ms, ep.setup.dev, ep.setup.type, ep.setup.request,
                         ep.setup.value, ep.setup.index);
            if (n < 0 || (size_t)n >= cap - used) return;
            used += (size_t)n;
        }
        if (ep.port.ms) {
            n = snprintf(buf + used, cap - used, "hub ms=%lu dev=%u port=%u status=%04X change=%04X\r\n",
                         (unsigned long)ep.port.ms, ep.port.dev, ep.port.port,
                         ep.port.status, ep.port.change);
            if (n < 0 || (size_t)n >= cap - used) return;
            used += (size_t)n;
        }
    }
    used += format_sample(buf + used, cap - used, "last", &ep.latest);
    snprintf(buf + used, cap - used, "KMBOX_BUS_END\r\n");
}
