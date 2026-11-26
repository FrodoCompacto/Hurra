#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TRACE_PACKET = 1, TRACE_NO_START, TRACE_NO_EOP, TRACE_CRC, TRACE_ACK_TX,
    TRACE_TOKEN_TX, TRACE_NAK, TRACE_STALL, TRACE_DATA, TRACE_BOOT,
    TRACE_ROOT_CONNECT, TRACE_OPEN, TRACE_HID_MOUNT, TRACE_UNMOUNT,
    TRACE_MOUSE, TRACE_KEY
} usb_trace_kind_t;

typedef enum {
    TRACE_PC_DISCONNECT, TRACE_PC_CONNECT, TRACE_PC_MOUNT,
    TRACE_UART_DMA_INIT, TRACE_UART_DMA_RX, TRACE_UART_DMA_TX_READY,
    TRACE_MILESTONE_COUNT
} usb_trace_milestone_t;

void usb_trace_mark(usb_trace_milestone_t milestone);
void usb_trace_init(void);
void usb_trace_open(unsigned slot, uint8_t dev, uint8_t ep);
typedef struct {
    uint32_t us, frame, frame_gap_us, elapsed_us;
    uint8_t irq, fifo, rx_pc, edge_pc, tx_pc, dp, dm, tx_oe, pid_byte;
    uint8_t rx_enabled, edge_enabled;
    bool sof_sent, low_speed, pre;
    usb_trace_kind_t result;
} usb_bus_sample_t;

void usb_trace_close(unsigned slot);
void usb_trace_in(unsigned slot, const usb_bus_sample_t *sample);
void usb_trace_setup(uint8_t dev, const uint8_t *setup);
void usb_trace_control_in(uint8_t dev, const uint8_t *data, int len);
void usb_trace_bus_index(char *buf, size_t cap, uint32_t frame, uint32_t core1_beats, const char *build);
void usb_trace_bus_page(unsigned page, char *buf, size_t cap);
void usb_trace_event(usb_trace_kind_t kind, uint8_t dev, uint8_t detail);
void usb_trace_format(char *buf, size_t capacity);
