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

#include "config.h"
#include "control.h"
#include "core.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "fake_board.h"
#include "hal.h"
#include "tusb.h"

uint8_t const* tud_descriptor_device_cb(void);
uint8_t const* tud_descriptor_configuration_cb(uint8_t index);
uint8_t const* tud_descriptor_bos_cb(void);
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid);
void tud_suspend_cb(bool remote_wakeup_en);
void tud_resume_cb(void);
void __wrap_dcd_event_handler(dcd_event_t const* event, bool in_isr);

static int g_fd = -1;
static pthread_t g_thread;
static volatile bool g_run;
static pthread_mutex_t g_tx_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned g_out_head, g_out_tail;
static volatile uint32_t g_board_baud = MIMHID_BOOT_BAUD;
static volatile uint32_t g_adapter_limit = 0;
static volatile uint32_t g_board_max = 3000000u; // what the fake UART makes
static uint8_t g_errors;

// What the host's side sends at: the pty shares one termios between its ends.
extern uint32_t fake_board_pty_baud(int fd); // pty_baud.c (Linux: TCGETS2) or tcgetattr
static uint32_t host_baud(void) {
    uint32_t baud = fake_board_pty_baud(g_fd);
    if (g_adapter_limit != 0 && baud > g_adapter_limit)
        baud = g_adapter_limit;
    return baud;
}

static bool line_matches(void) { return host_baud() == g_board_baud; }

// ---- HAL ----
void hal_init(void) {}

size_t hal_uart_read(uint8_t* dst, size_t max) {
    ssize_t n = read(g_fd, dst, max);
    // At different rates the bytes would arrive as framing errors; they are dropped.
    return n > 0 && line_matches() ? (size_t)n : 0;
}

uint32_t hal_uart_baud_actual(uint32_t baud) {
    return (baud == 0 || baud > g_board_max) ? 0u : baud;
}
uint32_t hal_uart_max_baud(void) { return g_board_max; }
bool hal_uart_has_break(void) { return true; }
uint8_t hal_uart_errors(void) { return __atomic_exchange_n(&g_errors, 0, __ATOMIC_ACQ_REL); }
void hal_uart_set_baud(uint32_t baud) { g_board_baud = baud; }
uint8_t hal_chip(void) { return MIMHID_CHIP_UNKNOWN; }
bool hal_has_bootloader(void) { return false; }
void hal_enter_bootloader(void) {}
void hal_control_silent_drop(void) {}
void __real_dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    (void)ep_addr;
}
bool __real_dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes) {
    (void)rhport;
    (void)ep_addr;
    (void)buffer;
    (void)total_bytes;
    return true;
}
void usbd_defer_func(osal_task_func_t func, void* param, bool in_isr) {
    (void)in_isr;
    func(param);
}

void hal_uart_write(const uint8_t* data, size_t len) {
    if (!line_matches())
        return;
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
static bool g_mounted, g_suspended, g_connect_pending;
// The pull-up on D+: a target enumerates a device when the pull-up appears, and only then. A
// board that is already on the bus and is told to connect again changes nothing the target can
// see, so no new enumeration follows (a real board showed this: a configuration sent to a board
// a crashed program had left attached was never mounted).
static bool g_pulled_up;
static unsigned g_connects;
static unsigned g_mount_delay_ms = 5;
static uint32_t g_connect_at;

bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t* init) {
    (void)rhport;
    (void)init;
    return true;
}
void tud_task_ext(uint32_t timeout_ms, bool in_isr) {
    (void)timeout_ms;
    (void)in_isr;
}
bool tud_mounted(void) { return g_mounted; }
bool tud_suspended(void) { return g_suspended; }

bool tud_disconnect(void) {
    pthread_mutex_lock(&g_lock);
    g_mounted = false;
    g_pulled_up = false;
    g_connect_pending = false;
    g_out_tail = g_out_head; // what the target had not sent is gone with the connection
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool tud_connect(void) {
    pthread_mutex_lock(&g_lock);
    g_connects++;
    if (!g_pulled_up) {
        g_pulled_up = true;
        g_connect_pending = true;
        g_connect_at = hal_millis() + g_mount_delay_ms;
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool tud_remote_wakeup(void) { return true; }
static volatile int g_hold_reports;
void fake_board_hold_reports(int hold) { g_hold_reports = hold; }

// ---- the endpoints of the generic driver: the controller and the target's side of them ----
// The calls of the stack's device layer the driver makes (src/device/usbd.c), with the state they
// keep: usbd_edpt_stall sets both stalled and busy, usbd_edpt_clear_stall clears both, and
// usbd_edpt_xfer refuses an endpoint that is busy. All of it runs on the board thread.
static struct {
    bool opened, busy, stalled;
    uint8_t* buffer;
    uint16_t length;
    uint16_t iso_buffer; // bytes usbd_edpt_iso_alloc gave: 0 for an endpoint that has none
    uint16_t packet; // wMaxPacketSize of the descriptor the endpoint was opened or activated with
} g_ep[2][16];       // [0] OUT, [1] IN
#define EPSTATE(addr) g_ep[((addr) >> 7) & 1][(addr)&0x0F]

bool usbd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const* desc) {
    (void)rhport;
    if (EPSTATE(desc->bEndpointAddress).opened)
        return false;
    EPSTATE(desc->bEndpointAddress).opened = true;
    EPSTATE(desc->bEndpointAddress).packet = tu_edpt_packet_size(desc);
    return true;
}
// The isochronous calls of the stack (usbd_edpt_iso_alloc and usbd_edpt_iso_activate in
// src/device/usbd.c, with the RP2040 controller driver under them): the buffer is given once, for
// the largest packet, and activating an endpoint again starts it over (the stack clears its busy
// and stalled flags). usbd_edpt_close does nothing on such a controller.
bool usbd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size) {
    (void)rhport;
    if (EPSTATE(ep_addr).iso_buffer != 0 || largest_packet_size == 0)
        return false;
    EPSTATE(ep_addr).iso_buffer = largest_packet_size;
    return true;
}
bool usbd_edpt_iso_activate(uint8_t rhport, tusb_desc_endpoint_t const* desc) {
    (void)rhport;
    const uint16_t size = tu_edpt_packet_size(desc);
    if (EPSTATE(desc->bEndpointAddress).iso_buffer == 0 ||
        size > EPSTATE(desc->bEndpointAddress).iso_buffer)
        return false;
    EPSTATE(desc->bEndpointAddress).opened = true;
    EPSTATE(desc->bEndpointAddress).busy = false;
    EPSTATE(desc->bEndpointAddress).stalled = false;
    EPSTATE(desc->bEndpointAddress).packet = size;
    return true;
}
// Stalls of endpoint 0, which the stack's own failure path makes with this call.
static bool g_ep0_stalled;
void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    if ((ep_addr & 0x0F) == 0)
        g_ep0_stalled = true;
}
bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    return EPSTATE(ep_addr).busy;
}
bool usbd_edpt_stalled(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    return EPSTATE(ep_addr).stalled;
}
bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes) {
    (void)rhport;
    if (!EPSTATE(ep_addr).opened || EPSTATE(ep_addr).busy)
        return false;
    EPSTATE(ep_addr).busy = true;
    EPSTATE(ep_addr).buffer = buffer;
    EPSTATE(ep_addr).length = total_bytes;
    return true;
}
void usbd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    EPSTATE(ep_addr).stalled = true;
    EPSTATE(ep_addr).busy = true;
}
void usbd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    EPSTATE(ep_addr).stalled = false;
    EPSTATE(ep_addr).busy = false;
}

static const usbd_class_driver_t* generic_driver(void) {
    uint8_t count = 0;
    return usbd_app_driver_get_cb(&count);
}

// What the stack's SET_CONFIGURATION does (process_set_config in src/device/usbd.c): the drivers'
// reset, then open for each interface in descriptor order (an association first, skipped; a whole
// group with one call); what open returns is where the walk goes on. Every interface, HID or not,
// is the generic driver's.
static void sim_set_config(void) {
    const usbd_class_driver_t* d = generic_driver();
    memset(g_ep, 0, sizeof(g_ep));
    d->reset(0);
    const uint8_t* cfg = tud_descriptor_configuration_cb(0);
    const uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    uint16_t pos = 9;
    while (pos < total) {
        if (cfg[pos + 1] == TUSB_DESC_INTERFACE_ASSOCIATION) {
            pos = (uint16_t)(pos + cfg[pos]);
            continue;
        }
        const uint16_t used =
            d->open(0, (const tusb_desc_interface_t*)&cfg[pos], (uint16_t)(total - pos));
        if (used < 9)
            return;
        pos = (uint16_t)(pos + used);
    }
}

// The transfers the target read from IN endpoints (a zero-length one has length 0).
static fake_transfer_t g_xfers[1024];
static unsigned g_xfer_head, g_xfer_tail;
// Packets the target wants to send on OUT endpoints; one waits until its endpoint is armed.
static struct {
    uint8_t address;
    uint16_t length;
    uint8_t data[1024];
} g_out_pending[64];
static unsigned g_poll_budget = 64;

// The controller: with the target polling, every IN endpoint that holds a transfer completes it,
// and every OUT endpoint that is armed takes the packet waiting for it.
static void endpoints_poll(void) {
    if (!g_mounted || g_hold_reports)
        return;
    const usbd_class_driver_t* d = generic_driver();
    for (unsigned round = 0; round < g_poll_budget; round++) {
        bool any = false;
        for (uint8_t n = 1; n < 16; n++) {
            const uint8_t address = (uint8_t)(0x80 | n);
            if (EPSTATE(address).opened && EPSTATE(address).busy && !EPSTATE(address).stalled) {
                any = true;
                pthread_mutex_lock(&g_lock);
                fake_transfer_t* t = &g_xfers[g_xfer_head % 1024];
                t->address = address;
                t->length = EPSTATE(address).length;
                memcpy(t->data, EPSTATE(address).buffer, t->length);
                g_xfer_head++;
                pthread_cond_broadcast(&g_cond);
                pthread_mutex_unlock(&g_lock);
                EPSTATE(address).busy = false;
                d->xfer_cb(0, address, XFER_RESULT_SUCCESS, t->length);
            }
        }
        for (unsigned i = g_out_tail; i != g_out_head; i++) {
            const uint8_t address = g_out_pending[i % 64].address;
            if (!EPSTATE(address).opened || !EPSTATE(address).busy || EPSTATE(address).stalled)
                break; // the target retries the packet: NAK until the endpoint is armed
            if (i != g_out_tail)
                break;
            any = true;
            const uint16_t n = g_out_pending[i % 64].length;
            memcpy(EPSTATE(address).buffer, g_out_pending[i % 64].data, n);
            EPSTATE(address).busy = false;
            g_out_tail++;
            d->xfer_cb(0, address, XFER_RESULT_SUCCESS, n);
        }
        if (!any)
            break;
    }
}

// ---- control transfers: the stack's side of them ----
static fake_control_result_t g_control[32];
static unsigned g_control_head, g_control_tail;
static uint8_t g_control_out[4100];
static size_t g_control_out_length;

static struct {
    int kind; // 1 data stage, 2 status only
    uint8_t* buffer;
    uint16_t length;
} g_xfer;

void dcd_int_disable(uint8_t rhport) { (void)rhport; }
void dcd_int_enable(uint8_t rhport) { (void)rhport; }

bool tud_control_xfer(uint8_t rhport, tusb_control_request_t const* request, void* buffer,
                      uint16_t len) {
    (void)rhport;
    g_xfer.kind = 1;
    g_xfer.buffer = buffer;
    g_xfer.length = len < request->wLength ? len : request->wLength; // as tud_control_xfer cuts it
    return true;
}

bool tud_control_status(uint8_t rhport, tusb_control_request_t const* request) {
    (void)rhport;
    (void)request;
    g_xfer.kind = 2;
    return true;
}

// Set while the board thread plays a control transfer the test does not want to hear about (the
// target's HID requests the helpers below make): its outcome is not recorded.
static bool g_control_silent;

static void control_result(const fake_control_result_t* r) {
    if (g_control_silent)
        return;
    pthread_mutex_lock(&g_lock);
    g_control[g_control_head++ % 32] = *r;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

// What reaches the stack from the board's wrapper.
void __real_dcd_event_handler(dcd_event_t const* event, bool in_isr) {
    (void)in_isr;
    if (event->event_id != DCD_EVENT_SETUP_RECEIVED)
        return;
    tusb_control_request_t req = event->setup_received;
    fake_control_result_t r;
    memset(&r, 0, sizeof(r));
    memcpy(r.setup, &req, 8);
    if (req.bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        r.outcome = FAKE_CONTROL_LEFT;
        control_result(&r);
        return;
    }
    memset(&g_xfer, 0, sizeof(g_xfer));
    if (!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req)) {
        r.outcome = FAKE_CONTROL_STALLED;
        control_result(&r);
        return;
    }
    if (g_xfer.kind == 1 && req.bmRequestType_bit.direction == TUSB_DIR_IN) {
        r.outcome = FAKE_CONTROL_ANSWERED;
        r.length = g_xfer.length;
        memcpy(r.data, g_xfer.buffer, g_xfer.length);
        tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req);
    } else if (g_xfer.kind == 1) {
        size_t n = g_control_out_length < g_xfer.length ? g_control_out_length : g_xfer.length;
        memcpy(g_xfer.buffer, g_control_out, n);
        r.length = g_xfer.length;
        memcpy(r.data, g_xfer.buffer, g_xfer.length);
        r.outcome = tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req) ? FAKE_CONTROL_ACKED
                                                                            : FAKE_CONTROL_STALLED;
    } else {
        r.outcome = FAKE_CONTROL_ACKED;
    }
    tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req);
    control_result(&r);
}

// ---- target actions, run on the board thread ----
typedef struct {
    int kind;
    uint8_t a, b, c;
    uint8_t data[4100];
    size_t length;
} action_t;

static action_t g_actions[256];
static unsigned g_action_head, g_action_tail;

static void post(action_t act) {
    pthread_mutex_lock(&g_lock);
    g_actions[g_action_head++ % 256] = act;
    pthread_mutex_unlock(&g_lock);
}

static void run_actions(void) {
    for (;;) {
        pthread_mutex_lock(&g_lock);
        if (g_action_tail == g_action_head) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
        action_t act = g_actions[g_action_tail++ % 256];
        pthread_mutex_unlock(&g_lock);
        switch (act.kind) {
        case 2:
            g_suspended = true;
            tud_suspend_cb(act.a != 0);
            break;
        case 3:
            g_suspended = false;
            tud_resume_cb();
            break;
        case 6: {
            dcd_event_t e;
            memset(&e, 0, sizeof(e));
            e.rhport = 0;
            e.event_id = DCD_EVENT_SETUP_RECEIVED;
            memcpy(&e.setup_received, act.data, 8);
            g_control_out_length = act.length - 8;
            memcpy(g_control_out, act.data + 8, g_control_out_length);
            g_control_silent = act.a != 0;
            __wrap_dcd_event_handler(&e, true);
            g_control_silent = false;
            break;
        }
        case 7: { // a packet on an OUT endpoint
            if (g_out_head - g_out_tail < 64) {
                g_out_pending[g_out_head % 64].address = act.a;
                g_out_pending[g_out_head % 64].length = (uint16_t)act.length;
                memcpy(g_out_pending[g_out_head % 64].data, act.data, act.length);
                g_out_head++;
            }
            break;
        }
        case 9: { // the target's SET_INTERFACE (a = interface, b = setting) or, with c = 1,
                  // GET_INTERFACE
            tusb_control_request_t r;
            memset(&r, 0, sizeof(r));
            r.bmRequestType = act.c ? 0x81 : 0x01; // standard, to an interface
            r.bRequest = act.c ? TUSB_REQ_GET_INTERFACE : TUSB_REQ_SET_INTERFACE;
            r.wValue = act.b;
            r.wIndex = act.a;
            r.wLength = act.c ? 1 : 0;
            memset(&g_xfer, 0, sizeof(g_xfer));
            g_ep0_stalled = false;
            const bool taken = generic_driver()->control_xfer_cb(0, CONTROL_STAGE_SETUP, &r);
            fake_control_result_t res;
            memset(&res, 0, sizeof(res));
            memcpy(res.setup, &r, 8);
            if (g_ep0_stalled) {
                res.outcome = FAKE_CONTROL_STALLED;
            } else if (!taken) {
                res.outcome = FAKE_CONTROL_LEFT;
            } else if (g_xfer.kind == 1) {
                res.outcome = FAKE_CONTROL_ANSWERED;
                res.length = g_xfer.length;
                memcpy(res.data, g_xfer.buffer, g_xfer.length);
            } else {
                res.outcome = FAKE_CONTROL_ACKED;
            }
            control_result(&res);
            break;
        }
        case 8: { // the target sets (b = 1) or clears (b = 0) the Halt feature of an endpoint
            if (act.b)
                usbd_edpt_stall(0, act.a);
            else
                usbd_edpt_clear_stall(0, act.a);
            tusb_control_request_t r;
            memset(&r, 0, sizeof(r));
            r.bmRequestType = 0x02; // host to device, standard, endpoint
            r.bRequest = act.b ? TUSB_REQ_SET_FEATURE : TUSB_REQ_CLEAR_FEATURE;
            r.wValue = TUSB_REQ_FEATURE_EDPT_HALT;
            r.wIndex = act.a;
            generic_driver()->control_xfer_cb(0, CONTROL_STAGE_SETUP, &r);
            break;
        }
        default:
            break;
        }
    }
}

static void* board_main(void* arg) {
    (void)arg;
    mimhid_core_init();
    mimhid_control_set_single_context(true);
    while (__atomic_load_n(&g_run, __ATOMIC_ACQUIRE)) {
        mimhid_rx_poll();
        mimhid_usb_poll();
        run_actions();
        endpoints_poll();
        pthread_mutex_lock(&g_lock);
        bool mount = g_connect_pending && (int32_t)(hal_millis() - g_connect_at) >= 0;
        if (mount) {
            g_connect_pending = false;
            g_mounted = true;
        }
        pthread_mutex_unlock(&g_lock);
        if (mount) {
            sim_set_config();
            tud_mount_cb();
        }
        struct timespec ts = {0, 100000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int fake_board_start(int fd) {
    g_fd = fd;
    __atomic_store_n(&g_run, true, __ATOMIC_RELEASE);
    return pthread_create(&g_thread, NULL, board_main, NULL);
}

void fake_board_stop(void) {
    __atomic_store_n(&g_run, false, __ATOMIC_RELEASE);
    pthread_join(g_thread, NULL);
}

// The interface that owns an IN endpoint of the configuration the board runs, 0xFF when none.
static uint8_t interface_of_endpoint(uint8_t address) {
    const uint8_t* c = tud_descriptor_configuration_cb(0);
    const uint16_t total = (uint16_t)(c[2] | (c[3] << 8));
    uint8_t current = 0xFF;
    for (uint16_t i = 0; i < total && c[i] != 0; i = (uint16_t)(i + c[i])) {
        if (c[i + 1] == TUSB_DESC_INTERFACE)
            current = c[i + 2];
        else if (c[i + 1] == TUSB_DESC_ENDPOINT && c[i + 2] == address)
            return current;
    }
    return 0xFF;
}

// What a HID report was: a transfer of the interface's IN endpoint (HID reports are the generic
// driver's now), told by the interface that owns the endpoint.
int fake_board_wait_report(fake_report_t* out, uint32_t timeout_ms) {
    fake_transfer_t t;
    if (!fake_board_wait_transfer(0, &t, timeout_ms))
        return 0;
    out->iface = interface_of_endpoint(t.address);
    out->length = t.length;
    memcpy(out->data, t.data, t.length);
    return 1;
}

void fake_board_clear_reports(void) { fake_board_clear_transfers(); }

int fake_board_mounted(void) { return g_mounted; }
unsigned fake_board_connects(void) { return g_connects; }
unsigned fake_board_interface_count(void) { return mimhid_active_interfaces(); }

const uint8_t* fake_board_config_descriptor(uint16_t* length) {
    const uint8_t* c = tud_descriptor_configuration_cb(0);
    *length = (uint16_t)(c[2] | (c[3] << 8));
    return c;
}

// A control request as the target makes it, played out on the board thread; waits for its outcome.
// Every request of a helper below is a request of the class or the standard, answered or refused by
// the rules the host wrote.
static int control_in(const uint8_t setup[8], fake_control_result_t* out) {
    fake_control_result_t stale;
    while (fake_board_wait_control(&stale, 0)) {
    }
    fake_board_target_control(setup, NULL, 0);
    return fake_board_wait_control(out, 2000);
}

const uint8_t* fake_board_report_descriptor(uint8_t iface, uint16_t* length) {
    // GET_DESCRIPTOR of the report descriptor (HID 1.11 7.1.1), as the target reads it.
    static fake_control_result_t r;
    const uint8_t setup[8] = {0x81, 0x06, 0x00, 0x22, iface, 0x00, 0xFF, 0x0F};
    *length = 0;
    if (!control_in(setup, &r) || r.outcome != FAKE_CONTROL_ANSWERED)
        return NULL;
    *length = (uint16_t)r.length;
    return r.data;
}

const uint8_t* fake_board_device_descriptor(void) { return tud_descriptor_device_cb(); }

const uint8_t* fake_board_bos_descriptor(uint16_t* length) {
    const uint8_t* b = tud_descriptor_bos_cb();
    *length = b == NULL ? 0 : (uint16_t)(b[2] | (b[3] << 8));
    return b;
}

size_t fake_board_string(uint8_t index, char* out, size_t capacity) {
    // The strings are served for the language they were given in: the one string 0 lists.
    const uint16_t* languages = tud_descriptor_string_cb(0, 0);
    if (languages == NULL)
        return 0;
    const uint16_t* s = tud_descriptor_string_cb(index, languages[1]);
    if (s == NULL)
        return 0;
    const size_t units = ((s[0] & 0xFF) - 2) / 2;
    size_t n = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t code = s[1 + i];
        if (code >= 0xD800 && code < 0xDC00 && i + 1 < units) {
            code = 0x10000 + ((code - 0xD800) << 10) + (s[2 + i] - 0xDC00);
            i++;
        }
        char bytes[4];
        size_t count;
        if (code < 0x80) {
            bytes[0] = (char)code;
            count = 1;
        } else if (code < 0x800) {
            bytes[0] = (char)(0xC0 | (code >> 6));
            bytes[1] = (char)(0x80 | (code & 0x3F));
            count = 2;
        } else if (code < 0x10000) {
            bytes[0] = (char)(0xE0 | (code >> 12));
            bytes[1] = (char)(0x80 | ((code >> 6) & 0x3F));
            bytes[2] = (char)(0x80 | (code & 0x3F));
            count = 3;
        } else {
            bytes[0] = (char)(0xF0 | (code >> 18));
            bytes[1] = (char)(0x80 | ((code >> 12) & 0x3F));
            bytes[2] = (char)(0x80 | ((code >> 6) & 0x3F));
            bytes[3] = (char)(0x80 | (code & 0x3F));
            count = 4;
        }
        if (n + count + 1 > capacity)
            break;
        for (size_t k = 0; k < count; k++)
            out[n++] = bytes[k];
    }
    out[n] = '\0';
    return n;
}

// GET_REPORT of a Feature report as the target asks (HID 1.11 7.2.1); the length of the answer, or
// 0 when the request was refused (a STALL) or answered with nothing. The Report ID leads the data
// when it is not 0.
size_t fake_board_feature(uint8_t iface, uint8_t report_id, uint8_t* out, size_t capacity) {
    fake_control_result_t r;
    const uint8_t setup[8] = {0xA1,
                              0x01,
                              report_id,
                              0x03,
                              iface,
                              0x00,
                              (uint8_t)(capacity & 0xFF),
                              (uint8_t)(capacity >> 8)};
    if (!control_in(setup, &r) || r.outcome != FAKE_CONTROL_ANSWERED)
        return 0;
    memcpy(out, r.data, r.length < capacity ? r.length : capacity);
    return r.length;
}

// A class request of the target to an interface with its data stage, played out silently.
static void silent_class_request(uint8_t request, uint16_t value, uint8_t iface,
                                 const uint8_t* data, size_t length) {
    action_t a = {6, 1, 0, 0, {0}, 0};
    const uint8_t setup[8] = {0x21,  request, (uint8_t)(value & 0xFF),  (uint8_t)(value >> 8),
                              iface, 0x00,    (uint8_t)(length & 0xFF), (uint8_t)(length >> 8)};
    memcpy(a.data, setup, 8);
    if (length > sizeof(a.data) - 8)
        length = sizeof(a.data) - 8;
    if (length)
        memcpy(a.data + 8, data, length);
    a.length = 8 + length;
    post(a);
}

// SET_REPORT (HID 1.11 7.2.2): the type in the high byte of wValue and the Report ID in the low;
// with a Report ID the data stage starts with it.
void fake_board_target_set_report(uint8_t iface, uint8_t report_type, uint8_t report_id,
                                  const uint8_t* data, size_t length) {
    uint8_t stage[1100];
    size_t n = 0;
    if (report_id != 0)
        stage[n++] = report_id;
    if (length > sizeof(stage) - n)
        length = sizeof(stage) - n;
    if (length)
        memcpy(stage + n, data, length);
    silent_class_request(0x09, (uint16_t)((report_type << 8) | report_id), iface, stage,
                         n + length);
}
void fake_board_target_control(const uint8_t setup[8], const uint8_t* out_data, size_t out_length) {
    action_t a = {6, 0, 0, 0, {0}, 8};
    memcpy(a.data, setup, 8);
    if (out_length > sizeof(a.data) - 8)
        out_length = sizeof(a.data) - 8;
    if (out_length)
        memcpy(a.data + 8, out_data, out_length);
    a.length = 8 + out_length;
    post(a);
}

int fake_board_wait_control(fake_control_result_t* out, uint32_t timeout_ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_lock);
    while (g_control_tail == g_control_head) {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &until) != 0)
            break;
    }
    int ok = g_control_tail != g_control_head;
    if (ok)
        *out = g_control[g_control_tail++ % 32];
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void fake_board_target_suspend(int remote_wakeup) {
    action_t a = {2, (uint8_t)remote_wakeup, 0, 0, {0}, 0};
    post(a);
}
void fake_board_target_resume(void) {
    action_t a = {3, 0, 0, 0, {0}, 0};
    post(a);
}
// SET_PROTOCOL (HID 1.11 7.2.6): the protocol is wValue. SET_IDLE (7.2.4): the rate is the high
// byte.
void fake_board_target_set_protocol(uint8_t iface, uint8_t protocol) {
    silent_class_request(0x0B, protocol, iface, NULL, 0);
}
void fake_board_target_set_idle(uint8_t iface, uint8_t rate) {
    silent_class_request(0x0A, (uint16_t)(rate << 8), iface, NULL, 0);
}
void fake_board_set_mount_delay_ms(unsigned ms) { g_mount_delay_ms = ms; }
uint32_t fake_board_baud(void) { return g_board_baud; }
void fake_board_set_adapter_limit(uint32_t baud) { g_adapter_limit = baud; }
void fake_board_inject_errors(uint8_t bits) {
    __atomic_fetch_or(&g_errors, bits, __ATOMIC_ACQ_REL);
}

// ---- the generic endpoints, as the tests drive them ----
int fake_board_wait_transfer(uint8_t address, fake_transfer_t* out, uint32_t timeout_ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_lock);
    for (;;) {
        for (unsigned i = g_xfer_tail; i != g_xfer_head; i++) {
            if (address == 0 || g_xfers[i % 1024].address == address) {
                *out = g_xfers[i % 1024];
                // Remove it: later ones move up.
                for (unsigned k = i; k + 1 != g_xfer_head; k++)
                    g_xfers[k % 1024] = g_xfers[(k + 1) % 1024];
                g_xfer_head--;
                pthread_mutex_unlock(&g_lock);
                return 1;
            }
        }
        if (pthread_cond_timedwait(&g_cond, &g_lock, &until) != 0)
            break;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void fake_board_clear_transfers(void) {
    pthread_mutex_lock(&g_lock);
    g_xfer_tail = g_xfer_head;
    pthread_mutex_unlock(&g_lock);
}

void fake_board_target_out(uint8_t address, const uint8_t* data, size_t length) {
    action_t a = {7, address, 0, 0, {0}, length > sizeof(a.data) ? sizeof(a.data) : length};
    if (a.length)
        memcpy(a.data, data, a.length);
    post(a);
}

// The target's SET_INTERFACE and GET_INTERFACE; the outcome is read with fake_board_wait_control
// (ACKED, STALLED for a setting the interface does not have, ANSWERED with the setting).
void fake_board_target_set_interface(uint8_t interface_number, uint8_t setting) {
    action_t a = {9, interface_number, setting, 0, {0}, 0};
    post(a);
}
void fake_board_target_get_interface(uint8_t interface_number) {
    action_t a = {9, interface_number, 0, 1, {0}, 0};
    post(a);
}
// The packet size the controller has the endpoint at (the descriptor it was opened or activated
// with), or 0 while it is not open.
unsigned fake_board_endpoint_packet(uint8_t address) {
    return EPSTATE(address).opened ? EPSTATE(address).packet : 0U;
}

void fake_board_target_halt(uint8_t address, int set) {
    action_t a = {8, address, (uint8_t)(set ? 1 : 0), 0, {0}, 0};
    post(a);
}

int fake_board_endpoint_stalled(uint8_t address) { return EPSTATE(address).stalled; }
int fake_board_endpoint_busy(uint8_t address) { return EPSTATE(address).busy; }
