// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// RP2040 and RP2350: hardware UART0 received by a circular DMA channel.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include "hardware/uart.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "bsp/board.h"
#include "pico/unique_id.h"
#include "pico/bootrom.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/usb_dpram.h"
#include "hal.h"

// The wiring is the board's: the build states the UART and its two pins (no pin is assumed).
#ifndef MIMHID_UART_ID
#error MIMHID_UART_ID (uart0 or uart1) must be defined by the build
#endif
#ifndef MIMHID_UART_TX_PIN
#error MIMHID_UART_TX_PIN must be defined by the build
#endif
#ifndef MIMHID_UART_RX_PIN
#error MIMHID_UART_RX_PIN must be defined by the build
#endif

#define UART_ID          MIMHID_UART_ID
#define UART_TX_PIN      MIMHID_UART_TX_PIN
#define UART_RX_PIN      MIMHID_UART_RX_PIN

// Both cores write to the UART (Pong from core 1, events and ACK from core 0).
static critical_section_t g_uart_tx_lock;

// DMA Ring Buffer placed in dedicated SRAM Bank 5 (.scratch_y)
#define DMA_BUF_SIZE     1024u
#define DMA_BUF_MASK     (DMA_BUF_SIZE - 1u)
static uint8_t uart_rx_buffer[DMA_BUF_SIZE] __attribute__((aligned(DMA_BUF_SIZE), section(".scratch_y")));
static int dma_chan = -1;
static int dma_reload_chan = -1;
static uint32_t dma_read_idx = 0;
// Set when a read finds the ring three quarters full: a lap cannot be seen once it happened, so
// the warning is the near miss.
static bool g_ring_high = false;
// On the RP2350 this value selects the ENDLESS transfer mode; on the RP2040 it is a plain count.
#ifndef DMA_RX_TRANSFER_COUNT
#define DMA_RX_TRANSFER_COUNT 0xFFFFFFFFu
#endif
static const uint32_t dma_reload_count = DMA_RX_TRANSFER_COUNT;

void hal_init(void) {
    board_init();

    critical_section_init(&g_uart_tx_lock);

    // 1. Hardware UART Setup
    uart_init(UART_ID, MIMHID_BOOT_BAUD);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_hw_flow(UART_ID, false, false);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    // Disable FIFO so incoming bytes trigger DREQ with zero character-timeout delay
    uart_set_fifo_enabled(UART_ID, false);

    // 2. Hardware DMA Setup (Circular ring buffer 512 bytes)
    // The data channel chains to a one-word channel that rewrites its transfer count and
    // retriggers it, so reception never stops when the count runs out.
    dma_chan = dma_claim_unused_channel(true);
    dma_reload_chan = dma_claim_unused_channel(true);

    dma_channel_config rc = dma_channel_get_default_config(dma_reload_chan);
    channel_config_set_transfer_data_size(&rc, DMA_SIZE_32);
    channel_config_set_read_increment(&rc, false);
    channel_config_set_write_increment(&rc, false);
    dma_channel_configure(
        dma_reload_chan,
        &rc,
        &dma_hw->ch[dma_chan].al1_transfer_count_trig,
        &dma_reload_count,
        1,
        false
    );

    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, uart_get_dreq(UART_ID, false));
    channel_config_set_ring(&c, true, 10); // 2^10 = 1024 bytes wrap-around
    channel_config_set_chain_to(&c, dma_reload_chan);

    dma_channel_configure(
        dma_chan,
        &c,
        uart_rx_buffer,
        &uart_get_hw(UART_ID)->dr,
        DMA_RX_TRANSFER_COUNT,
        true
    );
}

size_t hal_uart_read(uint8_t* dst, size_t max) {
    // Read the DMA write address directly to avoid 0xFFFFFFFF decrement / wrap bugs
    uint32_t write_idx = ((uint32_t)dma_channel_hw_addr(dma_chan)->write_addr - (uint32_t)uart_rx_buffer) & DMA_BUF_MASK;
    uint32_t avail = (write_idx - dma_read_idx) & DMA_BUF_MASK;
    if (avail == 0 || max == 0) return 0;
    if (avail >= DMA_BUF_SIZE - DMA_BUF_SIZE / 4u) g_ring_high = true;
    if (avail > max) avail = (uint32_t)max;
    uint32_t first = DMA_BUF_SIZE - dma_read_idx;
    if (first > avail) first = avail;
    memcpy(dst, &uart_rx_buffer[dma_read_idx], first);
    if (avail > first) {
        memcpy(dst + first, &uart_rx_buffer[0], avail - first);
    }
    dma_read_idx = (dma_read_idx + avail) & DMA_BUF_MASK;
    return avail;
}

void hal_uart_write(const uint8_t* data, size_t len) {
    critical_section_enter_blocking(&g_uart_tx_lock);
    uart_write_blocking(UART_ID, data, len);
    critical_section_exit(&g_uart_tx_lock);
}

// The PL011's divider, as the SDK's uart_set_baudrate computes it (7 fractional bits), without
// applying it: the rate the line would run at.
uint32_t hal_uart_baud_actual(uint32_t baud) {
    if (baud == 0) return 0;
    const uint32_t clk = clock_get_hz(clk_peri);
    uint32_t div = (uint32_t)(((uint64_t)8 * clk) / baud);
    uint32_t ibrd = div >> 7;
    uint32_t fbrd;
    if (ibrd == 0) {
        ibrd = 1;
        fbrd = 0;
    } else if (ibrd >= 65535) {
        ibrd = 65535;
        fbrd = 0;
    } else {
        fbrd = ((div & 0x7f) + 1) / 2;
    }
    return (uint32_t)(((uint64_t)4 * clk) / (64u * ibrd + fbrd));
}

// The smallest divider is 1: clk_peri / 16.
uint32_t hal_uart_max_baud(void) {
    return clock_get_hz(clk_peri) / 16u;
}

// The PL011 latches a break error in its raw interrupt status, which stays set while the DMA
// takes the bytes.
bool hal_uart_has_break(void) {
    return true;
}

uint8_t hal_uart_errors(void) {
    uart_hw_t* hw = uart_get_hw(UART_ID);
    const uint32_t ris = hw->ris;
    uint8_t out = 0;
    if (ris & UART_UARTRIS_BERIS_BITS) out |= HAL_UART_ERR_BREAK;
    else if (ris & UART_UARTRIS_FERIS_BITS) out |= HAL_UART_ERR_FRAMING;
    if (ris & UART_UARTRIS_OERIS_BITS) out |= HAL_UART_ERR_OVERRUN;
    // Clear only what was read: a flag raised since belongs to the next call.
    hw->icr = ris & (UART_UARTRIS_BERIS_BITS | UART_UARTRIS_FERIS_BITS | UART_UARTRIS_OERIS_BITS);
    if (g_ring_high) {
        out |= HAL_UART_ERR_RING;
        g_ring_high = false;
    }
    return out;
}

void hal_uart_set_baud(uint32_t baud) {
    critical_section_enter_blocking(&g_uart_tx_lock);
    uart_tx_wait_blocking(UART_ID);
    uart_set_baudrate(UART_ID, baud);
    critical_section_exit(&g_uart_tx_lock);
}

uint32_t hal_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

uint8_t hal_chip(void) {
#if PICO_RP2350
    return MIMHID_CHIP_RP2350;
#else
    return MIMHID_CHIP_RP2040;
#endif
}

size_t hal_unique_id(uint8_t* out, size_t max) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    size_t n = sizeof(id.id) < max ? sizeof(id.id) : max;
    for (size_t i = 0; i < n; i++) out[i] = id.id[i];
    return n;
}

bool hal_has_bootloader(void) { return true; }

// The bootloader in ROM (RP2040 datasheet 2.8, RP2350 datasheet 5.5): reset_usb_boot restarts the
// chip as the RPI-RP2 mass-storage device, with no activity LED and both of its interfaces.
void hal_enter_bootloader(void) {
    uart_tx_wait_blocking(UART_ID);
    reset_usb_boot(0, 0);
}

void hal_control_silent_drop(void) {
    hw_clear_alias(usb_hw)->ep_stall_arm = USB_EP_STALL_ARM_EP0_IN_BITS | USB_EP_STALL_ARM_EP0_OUT_BITS;
    usb_dpram->ep_buf_ctrl[0].in = 0;
    usb_dpram->ep_buf_ctrl[0].out = 0;
}
