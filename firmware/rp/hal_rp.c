// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// RP2040 and RP2350: hardware UART0 received by a circular DMA channel.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include "hardware/uart.h"
#include "hardware/dma.h"
#include "bsp/board.h"
#include "pico/unique_id.h"
#include "hal.h"

#ifndef MIMHID_UART_ID
#define MIMHID_UART_ID   uart0
#endif
#ifndef MIMHID_BAUD_RATE
#define MIMHID_BAUD_RATE 921600
#endif
#ifndef MIMHID_UART_TX_PIN
#define MIMHID_UART_TX_PIN 0
#endif
#ifndef MIMHID_UART_RX_PIN
#define MIMHID_UART_RX_PIN 1
#endif

#define UART_ID          MIMHID_UART_ID
#define BAUD_RATE        MIMHID_BAUD_RATE
#define UART_TX_PIN      MIMHID_UART_TX_PIN
#define UART_RX_PIN      MIMHID_UART_RX_PIN

// Both cores write to the UART (Pong from core 1, events and ACK from core 0).
static critical_section_t g_uart_tx_lock;

// DMA Ring Buffer placed in dedicated SRAM Bank 5 (.scratch_y)
#define DMA_BUF_SIZE     512u
#define DMA_BUF_MASK     (DMA_BUF_SIZE - 1u)
static uint8_t uart_rx_buffer[DMA_BUF_SIZE] __attribute__((aligned(DMA_BUF_SIZE), section(".scratch_y")));
static int dma_chan = -1;
static int dma_reload_chan = -1;
static uint32_t dma_read_idx = 0;
// On the RP2350 this value selects the ENDLESS transfer mode; on the RP2040 it is a plain count.
#ifndef DMA_RX_TRANSFER_COUNT
#define DMA_RX_TRANSFER_COUNT 0xFFFFFFFFu
#endif
static const uint32_t dma_reload_count = DMA_RX_TRANSFER_COUNT;

void hal_init(void) {
    board_init();

    critical_section_init(&g_uart_tx_lock);

    // 1. Hardware UART Setup
    uart_init(UART_ID, BAUD_RATE);
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
    channel_config_set_ring(&c, true, 9); // 2^9 = 512 bytes wrap-around
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

uint32_t hal_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

size_t hal_unique_id(uint8_t* out, size_t max) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    size_t n = sizeof(id.id) < max ? sizeof(id.id) : max;
    for (size_t i = 0; i < n; i++) out[i] = id.id[i];
    return n;
}
