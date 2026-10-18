// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// nRF52840: UARTE1 receives one byte per EasyDMA transfer, restarted by a shortcut, and an
// interrupt moves each byte into a software ring. UARTE has no receive timeout, so a partly
// filled multi-byte DMA buffer would hold data back; one-byte transfers avoid that.
#include <string.h>
#include "nrf.h"
#include "bsp/board_api.h"
#include "board.h"
#include "hal.h"

// Defaults follow the board's UART pins from the TinyUSB BSP (board.h).
#ifndef MIMHID_UART_RX_PIN
#define MIMHID_UART_RX_PIN UART_RX_PIN
#endif
#ifndef MIMHID_UART_TX_PIN
#define MIMHID_UART_TX_PIN UART_TX_PIN
#endif

#define UARTE_REG        NRF_UARTE1
#define UARTE_IRQn       UARTE1_IRQn
#define RX_RING_SIZE     1024
#define RX_RING_MASK     (RX_RING_SIZE - 1)
#define TX_CHUNK         128

static uint8_t           g_ring[RX_RING_SIZE];
static volatile uint16_t g_head = 0;
static volatile uint16_t g_tail = 0;
static volatile uint8_t  g_landing;     // EasyDMA target for the byte being received
static uint8_t           g_tx_buf[TX_CHUNK]; // EasyDMA cannot read from flash

// The BAUDRATE register takes fixed settings; the ones in the protocol's table. 921,600 runs at
// 941,176 (+2.1 %), within what UART receivers accept.
static uint32_t baud_setting(uint32_t baud) {
    switch (baud) {
    case 115200: return UARTE_BAUDRATE_BAUDRATE_Baud115200;
    case 230400: return UARTE_BAUDRATE_BAUDRATE_Baud230400;
    case 460800: return UARTE_BAUDRATE_BAUDRATE_Baud460800;
    case 921600: return UARTE_BAUDRATE_BAUDRATE_Baud921600;
    case 1000000: return UARTE_BAUDRATE_BAUDRATE_Baud1M;
    default: return 0;
    }
}

static NRF_GPIO_Type* gpio_port(uint32_t pin) {
    return pin < 32 ? NRF_P0 : NRF_P1;
}

void UARTE1_IRQHandler(void) {
    if (UARTE_REG->EVENTS_ENDRX) {
        UARTE_REG->EVENTS_ENDRX = 0;
        // A stopped transfer ends with nothing in it.
        if (UARTE_REG->RXD.AMOUNT == 0) return;
        uint16_t next = (uint16_t)((g_head + 1) & RX_RING_MASK);
        if (next != g_tail) {
            g_ring[g_head] = g_landing;
            g_head = next;
        }
    }
    if (UARTE_REG->EVENTS_ERROR) {
        UARTE_REG->EVENTS_ERROR = 0;
        UARTE_REG->ERRORSRC = 0xFUL; // overrun, parity, framing, break
    }
}

void hal_init(void) {
    board_init();

    // The BSP leaves UARTE0 enabled for its debug console on the same pins.
    NRF_UARTE0->ENABLE = 0;

    gpio_port(MIMHID_UART_TX_PIN)->OUTSET = 1UL << (MIMHID_UART_TX_PIN & 31);
    gpio_port(MIMHID_UART_TX_PIN)->DIRSET = 1UL << (MIMHID_UART_TX_PIN & 31);
    gpio_port(MIMHID_UART_RX_PIN)->PIN_CNF[MIMHID_UART_RX_PIN & 31] =
        (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
        (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);

    UARTE_REG->PSEL.TXD = MIMHID_UART_TX_PIN;
    UARTE_REG->PSEL.RXD = MIMHID_UART_RX_PIN;
    UARTE_REG->PSEL.RTS = 0xFFFFFFFFUL;
    UARTE_REG->PSEL.CTS = 0xFFFFFFFFUL;
    UARTE_REG->CONFIG = 0; // no flow control, no parity
    UARTE_REG->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud115200; // MIMHID_BOOT_BAUD
    UARTE_REG->ENABLE = UARTE_ENABLE_ENABLE_Enabled;

    UARTE_REG->RXD.PTR = (uint32_t)(uintptr_t)&g_landing;
    UARTE_REG->RXD.MAXCNT = 1;
    UARTE_REG->SHORTS = UARTE_SHORTS_ENDRX_STARTRX_Msk;
    UARTE_REG->EVENTS_ENDRX = 0;
    UARTE_REG->EVENTS_ERROR = 0;
    UARTE_REG->INTENSET = UARTE_INTENSET_ENDRX_Msk | UARTE_INTENSET_ERROR_Msk;
    // Above the USB interrupt so bytes are not dropped while a USB transfer is serviced.
    NVIC_SetPriority(UARTE_IRQn, 1);
    NVIC_EnableIRQ(UARTE_IRQn);

    UARTE_REG->TASKS_STARTRX = 1;
}

size_t hal_uart_read(uint8_t* dst, size_t max) {
    size_t n = 0;
    uint16_t tail = g_tail;
    while (n < max && tail != g_head) {
        dst[n++] = g_ring[tail];
        tail = (uint16_t)((tail + 1) & RX_RING_MASK);
    }
    g_tail = tail;
    return n;
}

void hal_uart_write(const uint8_t* data, size_t len) {
    while (len > 0) {
        size_t n = len < TX_CHUNK ? len : TX_CHUNK;
        memcpy(g_tx_buf, data, n);
        UARTE_REG->EVENTS_ENDTX = 0;
        UARTE_REG->EVENTS_TXSTOPPED = 0;
        UARTE_REG->TXD.PTR = (uint32_t)(uintptr_t)g_tx_buf;
        UARTE_REG->TXD.MAXCNT = (uint32_t)n;
        UARTE_REG->TASKS_STARTTX = 1;
        while (!UARTE_REG->EVENTS_ENDTX) {
        }
        UARTE_REG->TASKS_STOPTX = 1;
        while (!UARTE_REG->EVENTS_TXSTOPPED) {
        }
        data += n;
        len -= n;
    }
}

bool hal_uart_baud_ok(uint32_t baud) {
    return baud_setting(baud) != 0;
}

// hal_uart_write returns only once its bytes have left, so nothing is queued here. The
// receiver is stopped and the peripheral disabled while the rate changes.
void hal_uart_set_baud(uint32_t baud) {
    UARTE_REG->SHORTS = 0;
    UARTE_REG->EVENTS_RXTO = 0;
    UARTE_REG->TASKS_STOPRX = 1;
    while (!UARTE_REG->EVENTS_RXTO) {
    }
    UARTE_REG->EVENTS_RXTO = 0;
    UARTE_REG->ENABLE = UARTE_ENABLE_ENABLE_Disabled;
    UARTE_REG->BAUDRATE = baud_setting(baud);
    UARTE_REG->ENABLE = UARTE_ENABLE_ENABLE_Enabled;
    UARTE_REG->SHORTS = UARTE_SHORTS_ENDRX_STARTRX_Msk;
    UARTE_REG->TASKS_STARTRX = 1;
}

uint8_t hal_chip(void) {
    return MIMHID_CHIP_NRF52840;
}

uint32_t hal_millis(void) {
    return board_millis();
}

size_t hal_unique_id(uint8_t* out, size_t max) {
    size_t n = 0;
    for (size_t w = 0; w < 2; w++) {
        for (size_t b = 0; b < 4 && n < max; b++) {
            out[n++] = (uint8_t)(NRF_FICR->DEVICEID[w] >> (8 * b));
        }
    }
    return n;
}
