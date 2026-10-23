// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// ESP32-S3: UART1 through the IDF driver (interrupt-fed ring), built-in USB PHY.
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "esp_private/usb_phy.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "hal.h"

#ifndef MIMHID_UART_TX_PIN
#define MIMHID_UART_TX_PIN 17
#endif
#ifndef MIMHID_UART_RX_PIN
#define MIMHID_UART_RX_PIN 18
#endif

#define UART_PORT UART_NUM_1

static usb_phy_handle_t g_phy;

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
    uart_driver_install(UART_PORT, 2048, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &cfg);
    uart_set_pin(UART_PORT, MIMHID_UART_TX_PIN, MIMHID_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // Hand bytes to the ring after a short idle (2 symbols) instead of waiting for a full FIFO.
    uart_set_rx_full_threshold(UART_PORT, 8);
    uart_set_rx_timeout(UART_PORT, 2);
}

size_t hal_uart_read(uint8_t* dst, size_t max) {
    int n = uart_read_bytes(UART_PORT, dst, (uint32_t)max, 0);
    return n > 0 ? (size_t)n : 0;
}

void hal_uart_write(const uint8_t* data, size_t len) {
    uart_write_bytes(UART_PORT, data, len);
}

// The UART runs up to 5 Mbaud from its 80 MHz clock with a fractional divider.
bool hal_uart_baud_ok(uint32_t baud) {
    return baud <= 5000000u;
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

void hal_control_silent_drop(void) {}
