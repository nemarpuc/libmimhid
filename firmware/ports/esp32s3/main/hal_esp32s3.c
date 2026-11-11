// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// ESP32-S3: UART1 through the IDF driver (interrupt-fed ring), built-in USB PHY.
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "soc/uart_reg.h"
#include "esp_private/usb_phy.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "hal.h"

// The wiring is the board's: the build states the two pins (no pin is assumed).
#ifndef MIMHID_UART_TX_PIN
#error MIMHID_UART_TX_PIN must be defined by the build
#endif
#ifndef MIMHID_UART_RX_PIN
#error MIMHID_UART_RX_PIN must be defined by the build
#endif

#define UART_PORT UART_NUM_1

static usb_phy_handle_t g_phy;
static QueueHandle_t g_uart_events;

// The UART's clock (APB) and its divider: 20 integer bits and 4 fractional. The smallest divider
// is 1, so the fastest rate is the clock divided by 16.
#define UART_CLK_HZ 80000000u

void hal_init(void) {
    // Built-in full-speed PHY on GPIO19/20.
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    usb_new_phy(&phy_conf, &g_phy);

    const uart_config_t cfg = {
        .baud_rate = MIMHID_BOOT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, 2048, 0, 32, &g_uart_events, 0);
    uart_param_config(UART_PORT, &cfg);
    uart_set_pin(UART_PORT, MIMHID_UART_TX_PIN, MIMHID_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // Hand bytes to the ring after a short idle (2 symbols) instead of waiting for a full FIFO.
    uart_set_rx_full_threshold(UART_PORT, 8);
    uart_set_rx_timeout(UART_PORT, 2);
    // The driver reports a framing error (and so a BREAK) only when its interrupt is enabled.
    uart_enable_intr_mask(UART_PORT, UART_FRM_ERR_INT_ENA);
}

size_t hal_uart_read(uint8_t* dst, size_t max) {
    int n = uart_read_bytes(UART_PORT, dst, (uint32_t)max, 0);
    return n > 0 ? (size_t)n : 0;
}

void hal_uart_write(const uint8_t* data, size_t len) {
    uart_write_bytes(UART_PORT, data, len);
}

uint32_t hal_uart_baud_actual(uint32_t baud) {
    if (baud == 0) return 0;
    uint64_t div16 = ((uint64_t)UART_CLK_HZ * 16u + baud / 2u) / baud;
    if (div16 < 16u) div16 = 16u;
    return (uint32_t)(((uint64_t)UART_CLK_HZ * 16u) / div16);
}

uint32_t hal_uart_max_baud(void) {
    return UART_CLK_HZ / 16u;
}

bool hal_uart_has_break(void) {
    return true;
}

// The driver reports line errors as events on its queue, mixed with data events; both are taken
// off here each time so the queue does not fill and drop an error.
uint8_t hal_uart_errors(void) {
    uint8_t out = 0;
    uart_event_t ev;
    // The queue is looked at once per millisecond: the call takes a lock shared with the driver.
    static uint32_t last_ms;
    const uint32_t now = hal_millis();
    if (now == last_ms) return 0;
    last_ms = now;
    while (xQueueReceive(g_uart_events, &ev, 0) == pdTRUE) {
        switch (ev.type) {
        case UART_BREAK: out |= HAL_UART_ERR_BREAK; break;
        case UART_FRAME_ERR: out |= HAL_UART_ERR_FRAMING; break;
        case UART_FIFO_OVF: out |= HAL_UART_ERR_OVERRUN; break;
        case UART_BUFFER_FULL: out |= HAL_UART_ERR_RING; break;
        default: break;
        }
    }
    return out;
}

void hal_uart_set_baud(uint32_t baud) {
    uart_wait_tx_done(UART_PORT, portMAX_DELAY);
    uart_set_baudrate(UART_PORT, baud);
}

uint8_t hal_chip(void) {
    return MIMHID_CHIP_ESP32S3;
}

uint32_t hal_millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

size_t hal_unique_id(uint8_t* out, size_t max) {
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return 0;
    size_t n = max < sizeof(mac) ? max : sizeof(mac);
    for (size_t i = 0; i < n; i++) out[i] = mac[i];
    return n;
}

// The download mode of the ROM (ESP32-S3 Technical Reference Manual, "Chip Boot Control"): with
// RTC_CNTL_FORCE_DOWNLOAD_BOOT set in RTC_CNTL_OPTION1_REG, the ROM stays in download mode after
// the next reset instead of starting the firmware, as it does when GPIO0 is held low, and takes
// new firmware over UART0 or the chip's USB port. The bit is cleared by the reset after that.
bool hal_has_bootloader(void) { return true; }

void hal_enter_bootloader(void) {
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(100)); // the answer has left
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

void hal_control_silent_drop(void) {}
