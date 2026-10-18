// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "tusb.h"
#include "config.h"
#include "core.h"
#include "fake_board.h"
#include "hal.h"

uint8_t const* tud_descriptor_device_cb(void);
uint8_t const* tud_descriptor_configuration_cb(uint8_t index);
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance);
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid);
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen);
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize);
void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol);
bool tud_hid_set_idle_cb(uint8_t instance, uint8_t idle_rate);
void tud_suspend_cb(bool remote_wakeup_en);
void tud_resume_cb(void);

static int g_fd = -1;
static pthread_t g_thread;
static volatile bool g_run;
static pthread_mutex_t g_tx_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile uint32_t g_board_baud = MIMHID_BOOT_BAUD;
static volatile uint32_t g_adapter_limit = 0;

// What the host's side sends at: the pty shares one termios between its ends.
static uint32_t host_baud(void) {
    struct termios tio;
    if (tcgetattr(g_fd, &tio) != 0) return 0;
    uint32_t baud = 0;
#ifdef __APPLE__
    baud = (uint32_t)cfgetospeed(&tio); // speeds are plain numbers there
#else
    switch (cfgetospeed(&tio)) {
    case B115200: baud = 115200; break;
    case B230400: baud = 230400; break;
    case B460800: baud = 460800; break;
    case B921600: baud = 921600; break;
    case B1000000: baud = 1000000; break;
    case B1500000: baud = 1500000; break;
    case B2000000: baud = 2000000; break;
    case B3000000: baud = 3000000; break;
    case B4000000: baud = 4000000; break;
    default: break;
    }
#endif
    if (g_adapter_limit != 0 && baud > g_adapter_limit) baud = g_adapter_limit;
    return baud;
}

static bool line_matches(void) {
    return host_baud() == g_board_baud;
}

// ---- HAL ----
void hal_init(void) {}

size_t hal_uart_read(uint8_t* dst, size_t max) {
    ssize_t n = read(g_fd, dst, max);
    // At different rates the bytes would arrive as framing errors; they are dropped.
    return n > 0 && line_matches() ? (size_t)n : 0;
}

bool hal_uart_baud_ok(uint32_t baud) { return baud <= 3000000u; }
void hal_uart_set_baud(uint32_t baud) { g_board_baud = baud; }
uint8_t hal_chip(void) { return MIMHID_CHIP_UNKNOWN; }

void hal_uart_write(const uint8_t* data, size_t len) {
    if (!line_matches()) return;
    pthread_mutex_lock(&g_tx_lock);
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(g_fd, data + done, len - done);
        if (n > 0) {
            done += (size_t)n;
        } else if (n < 0 && errno == EAGAIN) {
            struct pollfd p = {g_fd, POLLOUT, 0};
            poll(&p, 1, 10);
        } else if (n < 0 && errno != EINTR) {
            break;
        }
    }
    pthread_mutex_unlock(&g_tx_lock);
}

uint32_t hal_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

size_t hal_unique_id(uint8_t* out, size_t max) {
    static const uint8_t id[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    size_t n = max < sizeof(id) ? max : sizeof(id);
    memcpy(out, id, n);
    return n;
}

// ---- recording TinyUSB stubs ----
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static fake_report_t g_reports[512];
static unsigned g_report_head, g_report_tail;
static bool g_mounted, g_suspended, g_connect_pending;
static unsigned g_connects;
static unsigned g_mount_delay_ms = 5;
static uint32_t g_connect_at;

bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t* init) { (void)rhport; (void)init; return true; }
void tud_task_ext(uint32_t timeout_ms, bool in_isr) { (void)timeout_ms; (void)in_isr; }
bool tud_mounted(void) { return g_mounted; }
bool tud_suspended(void) { return g_suspended; }

bool tud_disconnect(void) {
    pthread_mutex_lock(&g_lock);
    g_mounted = false;
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool tud_connect(void) {
    pthread_mutex_lock(&g_lock);
    g_connects++;
    g_connect_pending = true;
    g_connect_at = hal_millis() + g_mount_delay_ms;
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool tud_remote_wakeup(void) { return true; }
bool tud_hid_n_ready(uint8_t itf) { (void)itf; return g_mounted; }

bool tud_hid_n_report(uint8_t itf, uint8_t rid, void const* r, uint16_t len) {
    (void)rid;
    pthread_mutex_lock(&g_lock);
    fake_report_t* e = &g_reports[g_report_head % 512];
    e->iface = itf;
    e->length = (uint8_t)len;
    memcpy(e->data, r, len);
    g_report_head++;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return true;
}

// ---- target actions, run on the board thread ----
typedef struct {
    int kind;
    uint8_t a, b, c;
    uint8_t data[48];
    size_t length;
} action_t;

static action_t g_actions[16];
static unsigned g_action_head, g_action_tail;

static void post(action_t act) {
    pthread_mutex_lock(&g_lock);
    g_actions[g_action_head++ % 16] = act;
    pthread_mutex_unlock(&g_lock);
}

static void run_actions(void) {
    for (;;) {
        pthread_mutex_lock(&g_lock);
        if (g_action_tail == g_action_head) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
        action_t act = g_actions[g_action_tail++ % 16];
        pthread_mutex_unlock(&g_lock);
        switch (act.kind) {
        case 1: tud_hid_set_report_cb(act.a, act.c, (hid_report_type_t)act.b, act.data, (uint16_t)act.length); break;
        case 2: g_suspended = true; tud_suspend_cb(act.a != 0); break;
        case 3: g_suspended = false; tud_resume_cb(); break;
        case 4: tud_hid_set_protocol_cb(act.a, act.b); break;
        case 5: tud_hid_set_idle_cb(act.a, act.b); break;
        default: break;
        }
    }
}

static void* board_main(void* arg) {
    (void)arg;
    mimhid_core_init();
    while (g_run) {
        mimhid_rx_poll();
        mimhid_usb_poll();
        run_actions();
        pthread_mutex_lock(&g_lock);
        bool mount = g_connect_pending && (int32_t)(hal_millis() - g_connect_at) >= 0;
        if (mount) {
            g_connect_pending = false;
            g_mounted = true;
        }
        pthread_mutex_unlock(&g_lock);
        if (mount) tud_mount_cb();
        struct timespec ts = {0, 100000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int fake_board_start(int fd) {
    g_fd = fd;
    g_run = true;
    return pthread_create(&g_thread, NULL, board_main, NULL);
}

void fake_board_stop(void) {
    g_run = false;
    pthread_join(g_thread, NULL);
}

int fake_board_wait_report(fake_report_t* out, uint32_t timeout_ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_lock);
    while (g_report_tail == g_report_head) {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &until) != 0) break;
    }
    int ok = g_report_tail != g_report_head;
    if (ok) *out = g_reports[g_report_tail++ % 512];
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void fake_board_clear_reports(void) {
    pthread_mutex_lock(&g_lock);
    g_report_tail = g_report_head;
    pthread_mutex_unlock(&g_lock);
}

int fake_board_mounted(void) { return g_mounted; }
unsigned fake_board_connects(void) { return g_connects; }
unsigned fake_board_interface_count(void) { return mimhid_active_interfaces(); }

const uint8_t* fake_board_config_descriptor(uint16_t* length) {
    const uint8_t* c = tud_descriptor_configuration_cb(0);
    *length = (uint16_t)(c[2] | (c[3] << 8));
    return c;
}

const uint8_t* fake_board_report_descriptor(uint8_t iface, uint16_t* length) {
    const uint8_t* c = tud_descriptor_configuration_cb(0);
    uint16_t total = (uint16_t)(c[2] | (c[3] << 8));
    int seen = 0;
    *length = 0;
    for (uint16_t i = 0; i < total; i += c[i]) {
        if (c[i + 1] == 0x21 && seen++ == iface) *length = (uint16_t)(c[i + 7] | (c[i + 8] << 8));
    }
    return tud_hid_descriptor_report_cb(iface);
}

const uint8_t* fake_board_device_descriptor(void) { return tud_descriptor_device_cb(); }

size_t fake_board_string(uint8_t index, char* out, size_t capacity) {
    const uint16_t* s = tud_descriptor_string_cb(index, 0x0409);
    if (s == NULL) return 0;
    size_t n = ((s[0] & 0xFF) - 2) / 2;
    if (n + 1 > capacity) n = capacity - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)s[1 + i];
    out[n] = '\0';
    return n;
}

size_t fake_board_feature(uint8_t iface, uint8_t report_id, uint8_t* out, size_t capacity) {
    return tud_hid_get_report_cb(iface, report_id, HID_REPORT_TYPE_FEATURE, out, (uint16_t)capacity);
}

void fake_board_target_set_report(uint8_t iface, uint8_t report_type, uint8_t report_id,
                                  const uint8_t* data, size_t length) {
    action_t a = {1, iface, report_type, report_id, {0}, length > 48 ? 48 : length};
    memcpy(a.data, data, a.length);
    post(a);
}
void fake_board_target_suspend(int remote_wakeup) { action_t a = {2, (uint8_t)remote_wakeup, 0, 0, {0}, 0}; post(a); }
void fake_board_target_resume(void) { action_t a = {3, 0, 0, 0, {0}, 0}; post(a); }
void fake_board_target_set_protocol(uint8_t iface, uint8_t protocol) { action_t a = {4, iface, protocol, 0, {0}, 0}; post(a); }
void fake_board_target_set_idle(uint8_t iface, uint8_t rate) { action_t a = {5, iface, rate, 0, {0}, 0}; post(a); }
void fake_board_set_mount_delay_ms(unsigned ms) { g_mount_delay_ms = ms; }
uint32_t fake_board_baud(void) { return g_board_baud; }
void fake_board_set_adapter_limit(uint32_t baud) { g_adapter_limit = baud; }
