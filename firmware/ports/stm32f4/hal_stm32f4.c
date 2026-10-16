// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// STM32F4: USART2 (PA2 TX, PA3 RX) received by a circular DMA1 stream 5 channel 4 transfer.
// The TinyUSB BSP board_init() already enables USART2, its pins and its clock; this file
// adds the DMA receiver and raises the baud rate.
#include <string.h>
#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"
#include "bsp/board_api.h"
#include "hal.h"

#ifndef MIMHID_BAUD_RATE
#define MIMHID_BAUD_RATE 921600u
#endif
#define BAUD_RATE    MIMHID_BAUD_RATE
#define DMA_BUF_SIZE 512u
#define DMA_BUF_MASK (DMA_BUF_SIZE - 1u)

static uint8_t  g_dma_buf[DMA_BUF_SIZE];
static uint32_t g_read_idx = 0;

void hal_init(void) {
    board_init();

    __HAL_RCC_DMA1_CLK_ENABLE();

    // Reprogram USART2 for the target rate, 8N1, oversampling by 16.
    USART2->CR1 &= ~USART_CR1_UE;
    USART2->BRR = (HAL_RCC_GetPCLK1Freq() + BAUD_RATE / 2u) / BAUD_RATE;
    USART2->CR2 = 0;
    USART2->CR3 = USART_CR3_DMAR;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

    // USART2_RX is DMA1 stream 5, channel 4: peripheral to memory, circular, byte-wide.
    DMA1_Stream5->CR = 0;
    while (DMA1_Stream5->CR & DMA_SxCR_EN) {
    }
    DMA1->HIFCR = DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CTEIF5 |
                  DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5;
    DMA1_Stream5->PAR = (uint32_t)(uintptr_t)&USART2->DR;
    DMA1_Stream5->M0AR = (uint32_t)(uintptr_t)g_dma_buf;
    DMA1_Stream5->NDTR = DMA_BUF_SIZE;
    DMA1_Stream5->FCR = 0;
    DMA1_Stream5->CR = (4u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MINC | DMA_SxCR_CIRC |
                       DMA_SxCR_PL_1 | DMA_SxCR_EN;
}

size_t hal_uart_read(uint8_t* dst, size_t max) {
    // NDTR counts down and reloads to DMA_BUF_SIZE on wrap.
    uint32_t write_idx = (DMA_BUF_SIZE - DMA1_Stream5->NDTR) & DMA_BUF_MASK;
    uint32_t avail = (write_idx - g_read_idx) & DMA_BUF_MASK;
    if (avail == 0 || max == 0) return 0;
    if (avail > max) avail = (uint32_t)max;
    uint32_t first = DMA_BUF_SIZE - g_read_idx;
    if (first > avail) first = avail;
    memcpy(dst, &g_dma_buf[g_read_idx], first);
    if (avail > first) {
        memcpy(dst + first, &g_dma_buf[0], avail - first);
    }
    g_read_idx = (g_read_idx + avail) & DMA_BUF_MASK;
    return avail;
}

void hal_uart_write(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        while (!(USART2->SR & USART_SR_TXE)) {
        }
        USART2->DR = data[i];
    }
    while (!(USART2->SR & USART_SR_TC)) {
    }
}

uint32_t hal_millis(void) {
    return board_millis();
}

size_t hal_unique_id(uint8_t* out, size_t max) {
    const uint8_t* uid = (const uint8_t*)UID_BASE;
    size_t n = max < 12 ? max : 12;
    for (size_t i = 0; i < n; i++) out[i] = uid[i];
    return n;
}
