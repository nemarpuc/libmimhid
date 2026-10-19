// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include <string.h>
#include "tusb.h"
#include "config.h"
#include "core.h"
#include "hal.h"

// -------------------------------------------------------------
// Report queues: one per USB interface (RX context produces, USB context consumes)
// -------------------------------------------------------------
#ifndef MIMHID_QUEUE_DEPTH
#define MIMHID_QUEUE_DEPTH 16
#endif
#define MIMHID_QUEUE_MASK (MIMHID_QUEUE_DEPTH - 1)
_Static_assert((MIMHID_QUEUE_DEPTH & MIMHID_QUEUE_MASK) == 0, "MIMHID_QUEUE_DEPTH must be a power of two");

typedef struct {
    uint8_t len;
    uint8_t data[MIMHID_MAX_REPORT_BYTES];
} report_t;

typedef struct {
    report_t items[MIMHID_QUEUE_DEPTH];
    volatile uint32_t head;
    volatile uint32_t tail;
} report_queue_t;

static report_queue_t g_queue[MIMHID_HID_ITFS];

static const report_t* queue_peek(uint8_t iface) {
    report_queue_t* q = &g_queue[iface];
    uint32_t t = q->tail;
    if (t == __atomic_load_n(&q->head, __ATOMIC_ACQUIRE)) return NULL;
    return &q->items[t];
}

static void queue_pop(uint8_t iface) {
    report_queue_t* q = &g_queue[iface];
    uint32_t t = q->tail;
    __atomic_store_n(&q->tail, (t + 1) & MIMHID_QUEUE_MASK, __ATOMIC_RELEASE);
}

// Consumer side only: discards everything queued so far.
static void queue_flush_all(void) {
    for (uint8_t i = 0; i < MIMHID_HID_ITFS; i++) {
        report_queue_t* q = &g_queue[i];
        __atomic_store_n(&q->tail, __atomic_load_n(&q->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    }
}

// -------------------------------------------------------------
// Frames to the host
// -------------------------------------------------------------
static void send_frame(uint8_t type, const uint8_t* payload, uint8_t len) {
    uint8_t frame[4 + MIMHID_MAX_FRAME_PAYLOAD];
    uint8_t x = (uint8_t)(type ^ len);
    frame[0] = MIMHID_SYNC;
    frame[1] = type;
    frame[2] = len;
    for (uint8_t i = 0; i < len; i++) {
        frame[3 + i] = payload[i];
        x ^= payload[i];
    }
    frame[3 + len] = x;
    hal_uart_write(frame, (size_t)len + 4u);
}

static void send_event(uint8_t kind, const uint8_t* data, uint8_t len) {
    uint8_t payload[1 + MIMHID_MAX_REPORT_BYTES + 8];
    if (len > sizeof(payload) - 1) len = (uint8_t)(sizeof(payload) - 1);
    payload[0] = kind;
    if (len && data) memcpy(&payload[1], data, len);
    send_frame(MIMHID_T_EVENT, payload, (uint8_t)(1 + len));
}

static void send_ack(uint8_t seq, uint8_t type) {
    const uint8_t p[2] = {seq, type};
    send_frame(MIMHID_T_ACK, p, 2);
}

static void send_nak(uint8_t seq, uint8_t type, uint8_t code) {
    const uint8_t p[3] = {seq, type, code};
    send_frame(MIMHID_T_NAK, p, 3);
}

// -------------------------------------------------------------
// Commands: parsed by the RX side, executed by the USB side
// -------------------------------------------------------------
#define CMD_SLOTS 4
#define CMD_MASK  (CMD_SLOTS - 1)

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t data[MIMHID_MAX_FRAME_PAYLOAD];
} command_t;

static command_t g_cmd[CMD_SLOTS];
static volatile uint32_t g_cmd_head = 0;
static volatile uint32_t g_cmd_tail = 0;

static bool cmd_push(uint8_t type, const uint8_t* data, uint8_t len) {
    uint32_t h = g_cmd_head;
    uint32_t n = (h + 1) & CMD_MASK;
    if (n == __atomic_load_n(&g_cmd_tail, __ATOMIC_ACQUIRE)) return false;
    g_cmd[h].type = type;
    g_cmd[h].len = len;
    memcpy(g_cmd[h].data, data, len);
    __atomic_store_n(&g_cmd_head, n, __ATOMIC_RELEASE);
    return true;
}

// -------------------------------------------------------------
// RX parser
// -------------------------------------------------------------
#define RX_SIZE 1024
#define RX_MASK (RX_SIZE - 1)

static uint8_t  g_rx[RX_SIZE];
static uint32_t g_rd = 0;
static uint32_t g_wr = 0;
static uint8_t  g_unique_id[MIMHID_UNIQUE_ID_MAX];
static uint8_t  g_unique_id_len = 0;
static uint32_t g_last_rx_ms = 0;

// The line rate: an index into MIMHID_BAUD_RATES. A rate other than the boot rate stays
// unconfirmed until a valid frame arrives at it.
static const uint32_t g_rates[MIMHID_BAUD_COUNT] = MIMHID_BAUD_RATES;
static uint16_t g_baud_mask = 0;
static uint8_t  g_baud_index = 0;
static bool     g_baud_pending = false;
static uint32_t g_baud_since_ms = 0;

// A corrupt header can announce a long frame whose bytes never come. Waiting that long for
// the payload would swallow the valid frames behind it, so a frame that stays incomplete with
// the line idle for this long is abandoned one byte at a time.
#define RX_IDLE_RESYNC_MS 10

static void rx_fill(void) {
    for (;;) {
        uint32_t free_bytes = RX_SIZE - 1 - ((g_wr - g_rd) & RX_MASK);
        if (free_bytes == 0) return;
        uint32_t chunk = RX_SIZE - g_wr;
        if (chunk > free_bytes) chunk = free_bytes;
        size_t n = hal_uart_read(&g_rx[g_wr], chunk);
        if (n == 0) return;
        g_last_rx_ms = hal_millis();
        g_wr = (g_wr + (uint32_t)n) & RX_MASK;
    }
}

static bool known_command(uint8_t type) {
    return type == MIMHID_T_PING || (type >= MIMHID_T_DEVICE_CONFIG && type <= MIMHID_T_SET_BAUD);
}

static void set_baud_index(uint8_t index) {
    hal_uart_set_baud(g_rates[index]);
    g_baud_index = index;
    g_baud_pending = index != 0;
    g_baud_since_ms = hal_millis();
}

// Runs on the receive side: the ACK has to leave at the old rate before the switch.
static void cmd_set_baud(const uint8_t* data, uint8_t len) {
    const uint8_t seq = data[0];
    if (len != 2 || data[1] >= MIMHID_BAUD_COUNT || !(g_baud_mask & (1u << data[1]))) {
        send_nak(seq, MIMHID_T_SET_BAUD, MIMHID_NAK_PARAM);
        return;
    }
    send_ack(seq, MIMHID_T_SET_BAUD);
    set_baud_index(data[1]);
}

static void send_pong(void) {
    uint8_t p[MIMHID_PONG_MIN_LEN + MIMHID_UNIQUE_ID_MAX];
    p[0] = FW_VERSION_MAJOR;
    p[1] = FW_VERSION_MINOR;
    p[2] = FW_VERSION_PATCH;
    p[3] = tud_ready() ? 1 : 0;
    p[4] = MIMHID_PROTO_VERSION;
    p[5] = MIMHID_MAX_NODES;
    p[6] = MIMHID_HID_ITFS;
    p[7] = MIMHID_MAX_REPORT_BYTES;
    p[8] = (uint8_t)(MIMHID_MAX_DESC_BYTES & 0xFF);
    p[9] = (uint8_t)(MIMHID_MAX_DESC_BYTES >> 8);
    p[10] = g_unique_id_len ? MIMHID_PONG_FLAG_UNIQUE_ID : 0;
    p[11] = hal_chip();
#if TUD_OPT_HIGH_SPEED
    p[12] = 1;
#else
    p[12] = 0;
#endif
    p[13] = g_baud_index;
    p[14] = (uint8_t)(g_baud_mask & 0xFF);
    p[15] = (uint8_t)(g_baud_mask >> 8);
    p[16] = g_unique_id_len;
    memcpy(&p[17], g_unique_id, g_unique_id_len);
    send_frame(MIMHID_T_PONG, p, (uint8_t)(MIMHID_PONG_MIN_LEN + g_unique_id_len));
}

// Handles at most one frame or skips one byte. Returns false when more input is needed.
// A frame that fails validation is skipped one byte at a time so the parser
// resynchronizes on the next sync byte instead of swallowing a following valid frame.
static bool rx_step(void) {
    uint32_t read_idx = g_rd;
    uint32_t available = (g_wr - read_idx) & RX_MASK;
    if (available == 0) return false;

    if (g_rx[read_idx] != MIMHID_SYNC) {
        g_rd = (read_idx + 1) & RX_MASK;
        return true;
    }
    if (available < 3) return false;

    const uint8_t type = g_rx[(read_idx + 1) & RX_MASK];
    const uint8_t len = g_rx[(read_idx + 2) & RX_MASK];
    const bool is_report = type >= MIMHID_T_REPORT_BASE && type <= MIMHID_T_REPORT_LAST;

    // Reject unknown types and impossible lengths before waiting for the payload.
    if (!is_report && !known_command(type)) {
        g_rd = (read_idx + 1) & RX_MASK;
        return true;
    }
    if (is_report ? (len == 0 || len > MIMHID_MAX_REPORT_BYTES) : (type == MIMHID_T_PING ? len != 1 : len < 1)) {
        g_rd = (read_idx + 1) & RX_MASK;
        return true;
    }

    const uint32_t total = 4u + len;
    if (available < total) return false;

    if (is_report) {
        uint8_t node = (uint8_t)(type & 0x0F);
        uint8_t iface = mimhid_active_node_interface(node);
        report_queue_t* q = (iface < MIMHID_HID_ITFS) ? &g_queue[iface] : NULL;
        uint32_t h = 0;
        uint32_t next_h = 0;
        uint8_t* dst = NULL;
        if (q != NULL) {
            h = q->head;
            next_h = (h + 1) & MIMHID_QUEUE_MASK;
            if (next_h != __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE)) {
                dst = q->items[h].data;
            }
        }
        uint8_t x = (uint8_t)(type ^ len);
        for (uint8_t i = 0; i < len; i++) {
            uint8_t b = g_rx[(read_idx + 3 + i) & RX_MASK];
            if (dst != NULL) dst[i] = b;
            x ^= b;
        }
        if (x != g_rx[(read_idx + 3 + len) & RX_MASK]) {
            g_rd = (read_idx + 1) & RX_MASK;
            return true;
        }
        g_baud_pending = false;
        if (dst != NULL) {
            q->items[h].len = len;
            __atomic_store_n(&q->head, next_h, __ATOMIC_RELEASE);
        }
    } else {
        uint8_t buf[MIMHID_MAX_FRAME_PAYLOAD];
        uint8_t x = (uint8_t)(type ^ len);
        for (uint8_t i = 0; i < len; i++) {
            uint8_t b = g_rx[(read_idx + 3 + i) & RX_MASK];
            buf[i] = b;
            x ^= b;
        }
        if (x != g_rx[(read_idx + 3 + len) & RX_MASK]) {
            g_rd = (read_idx + 1) & RX_MASK;
            return true;
        }
        g_baud_pending = false;
        g_rd = (read_idx + total) & RX_MASK;
        if (type == MIMHID_T_PING) {
            send_pong();
        } else if (type == MIMHID_T_SET_BAUD) {
            cmd_set_baud(buf, len);
        } else {
            cmd_push(type, buf, len); // a full mailbox drops the frame; the host retries on no ACK
        }
        return true;
    }
    g_rd = (read_idx + total) & RX_MASK;
    return true;
}

void mimhid_rx_poll(void) {
    // An unconfirmed rate that heard nothing valid in time falls back to the boot rate.
    if (g_baud_pending && (int32_t)(hal_millis() - g_baud_since_ms) >= MIMHID_BAUD_CONFIRM_MS)
        set_baud_index(0);
    for (;;) {
        rx_fill();
        if (rx_step()) continue;
        if (g_rd != g_wr && (int32_t)(hal_millis() - g_last_rx_ms) >= RX_IDLE_RESYNC_MS) {
            g_rd = (g_rd + 1) & RX_MASK;
            continue;
        }
        return;
    }
}

// -------------------------------------------------------------
// USB side: configuration commands
// -------------------------------------------------------------
static mimhid_config_t g_staged;

static bool read_string(char* dst, const uint8_t* p, uint8_t len, uint8_t* pos) {
    if (*pos >= len) return false;
    uint8_t n = p[*pos];
    if (n > MIMHID_STRING_MAX || (uint32_t)*pos + 1u + n > len) return false;
    memcpy(dst, &p[*pos + 1], n);
    dst[n] = '\0';
    *pos = (uint8_t)(*pos + 1 + n);
    return true;
}

static void cmd_device_config(const command_t* c) {
    const uint8_t seq = c->data[0];
    const uint8_t* p = &c->data[1];
    const uint8_t len = (uint8_t)(c->len - 1);
    if (len < 13 + 4) { send_nak(seq, c->type, MIMHID_NAK_PARAM); return; }

    memset(&g_staged, 0, sizeof(g_staged));
    mimhid_dev_cfg_t* d = &g_staged.dev;
    d->vendor_id = (uint16_t)(p[0] | (p[1] << 8));
    d->product_id = (uint16_t)(p[2] | (p[3] << 8));
    d->bcd_device = (uint16_t)(p[4] | (p[5] << 8));
    d->device_class = p[6];
    d->device_subclass = p[7];
    d->device_protocol = p[8];
    d->language_id = (uint16_t)(p[9] | (p[10] << 8));
    d->flags = p[11];
    d->max_power = p[12];
    uint8_t pos = 13;
    if (!read_string(d->manufacturer, p, len, &pos) || !read_string(d->product, p, len, &pos) ||
        !read_string(d->serial, p, len, &pos) || !read_string(d->configuration, p, len, &pos)) {
        g_staged.valid = false;
        send_nak(seq, c->type, MIMHID_NAK_PARAM);
        return;
    }
    g_staged.valid = true;
    send_ack(seq, c->type);
}

static void cmd_node_begin(const command_t* c) {
    const uint8_t seq = c->data[0];
    const uint8_t* p = &c->data[1];
    const uint8_t len = (uint8_t)(c->len - 1);
    if (!g_staged.valid) { send_nak(seq, c->type, MIMHID_NAK_STATE); return; }
    if (len < 13) { send_nak(seq, c->type, MIMHID_NAK_PARAM); return; }

    const uint8_t id = p[0];
    const uint16_t desc_len = (uint16_t)(p[2] | (p[3] << 8));
    const uint8_t ep_size = p[5];
    if (id >= MIMHID_MAX_NODES || desc_len == 0 || p[9] > 2 ||
        ep_size > 64) {
        send_nak(seq, c->type, MIMHID_NAK_PARAM);
        return;
    }

    uint8_t ep_out_size = 0;
    uint8_t pos = 12;
    if (len >= 14 && p[12] <= 64 && ((uint32_t)14 + p[13] <= len)) {
        ep_out_size = p[12];
        pos = 13;
    } else if (len >= 14 && (uint32_t)14 + p[13] <= len) {
        send_nak(seq, c->type, MIMHID_NAK_PARAM);
        return;
    }

    if (g_staged.node[id].used) {
        // The host repeats a frame whose ACK it did not see; the same node is accepted again.
        const mimhid_node_cfg_t* n = &g_staged.node[id];
        bool same = n->iface == p[1] && n->desc_len == desc_len && n->ep_out_size == ep_out_size;
        if (same) send_ack(seq, c->type);
        else send_nak(seq, c->type, MIMHID_NAK_STATE);
        return;
    }
    if (p[1] >= MIMHID_HID_ITFS) { send_nak(seq, c->type, MIMHID_NAK_OVERFLOW); return; }
    if ((uint32_t)g_staged.pool_used + desc_len > MIMHID_MAX_DESC_BYTES) {
        send_nak(seq, c->type, MIMHID_NAK_OVERFLOW);
        return;
    }

    mimhid_node_cfg_t* n = &g_staged.node[id];
    memset(n, 0, sizeof(*n));
    n->used = true;
    n->iface = p[1];
    n->desc_len = desc_len;
    n->poll_interval = p[4];
    n->ep_size = ep_size;
    n->ep_out_size = ep_out_size;
    n->bcd_hid = (uint16_t)(p[6] | (p[7] << 8));
    n->country = p[8];
    n->boot_protocol = p[9];
    if (!read_string(n->interface_string, p, len, &pos)) {
        n->used = false;
        send_nak(seq, c->type, MIMHID_NAK_PARAM);
        return;
    }
    n->desc_off = g_staged.pool_used;
    g_staged.pool_used = (uint16_t)(g_staged.pool_used + desc_len);
    send_ack(seq, c->type);
}

static void cmd_node_data(const command_t* c) {
    const uint8_t seq = c->data[0];
    const uint8_t* p = &c->data[1];
    const uint8_t len = (uint8_t)(c->len - 1);
    if (len < 4) { send_nak(seq, c->type, MIMHID_NAK_PARAM); return; }
    const uint8_t id = p[0];
    const uint16_t off = (uint16_t)(p[1] | (p[2] << 8));
    const uint8_t n = (uint8_t)(len - 3);
    if (id >= MIMHID_MAX_NODES || !g_staged.node[id].used) { send_nak(seq, c->type, MIMHID_NAK_STATE); return; }
    mimhid_node_cfg_t* node = &g_staged.node[id];
    if ((uint32_t)off + n > node->desc_len || off > node->desc_recv) {
        send_nak(seq, c->type, MIMHID_NAK_PARAM);
        return;
    }
    memcpy(&g_staged.pool[node->desc_off + off], &p[3], n); // a repeated chunk rewrites the same bytes
    if ((uint32_t)off + n > node->desc_recv) node->desc_recv = (uint16_t)(off + n);
    send_ack(seq, c->type);
}

static void cmd_feature(const command_t* c) {
    const uint8_t seq = c->data[0];
    const uint8_t* p = &c->data[1];
    const uint8_t len = (uint8_t)(c->len - 1);
    if (len < 2 || len - 2 > MIMHID_FEATURE_MAX) { send_nak(seq, c->type, MIMHID_NAK_PARAM); return; }
    if (p[0] >= MIMHID_MAX_NODES || !g_staged.node[p[0]].used) { send_nak(seq, c->type, MIMHID_NAK_STATE); return; }
    mimhid_feature_t* slot = NULL;
    for (uint8_t i = 0; i < MIMHID_MAX_FEATURES; i++) {
        mimhid_feature_t* f = &g_staged.feature[i];
        if (f->used && f->node == p[0] && f->report_id == p[1]) { slot = f; break; }
        if (!f->used && !slot) slot = f;
    }
    if (!slot) { send_nak(seq, c->type, MIMHID_NAK_OVERFLOW); return; }
    slot->used = true;
    slot->node = p[0];
    slot->report_id = p[1];
    slot->len = (uint8_t)(len - 2);
    memcpy(slot->data, &p[2], slot->len);
    mimhid_active_set_feature(p[0], p[1], slot->data, slot->len); // takes effect without a COMMIT
    send_ack(seq, c->type);
}

static void cmd_commit(const command_t* c) {
    const uint8_t seq = c->data[0];
    if (c->len >= 2 && c->data[1] == 1) {
        // Detach: leave the target's bus and forget the active configuration.
        tud_disconnect();
        queue_flush_all();
        mimhid_clear_config();
        send_ack(seq, c->type);
        return;
    }
    if (!mimhid_apply_config(&g_staged)) {
        send_nak(seq, c->type, MIMHID_NAK_DESCRIPTOR);
        return;
    }
    queue_flush_all();
    tud_connect();
    send_ack(seq, c->type);
}

static void run_command(const command_t* c) {
    if (c->type == MIMHID_T_PING) return;
    const uint8_t seq = c->data[0];
    switch (c->type) {
    case MIMHID_T_DEVICE_CONFIG: cmd_device_config(c); break;
    case MIMHID_T_NODE_BEGIN:    cmd_node_begin(c); break;
    case MIMHID_T_NODE_DATA:     cmd_node_data(c); break;
    case MIMHID_T_FEATURE:       cmd_feature(c); break;
    case MIMHID_T_COMMIT:        cmd_commit(c); break;
    case MIMHID_T_RESET:
        queue_flush_all();
        send_ack(seq, c->type);
        break;
    case MIMHID_T_REMOTE_WAKEUP:
        if (tud_suspended()) {
            tud_remote_wakeup();
            send_ack(seq, c->type);
        } else {
            send_nak(seq, c->type, MIMHID_NAK_STATE);
        }
        break;
    default:
        send_nak(seq, c->type, MIMHID_NAK_UNSUPPORTED);
        break;
    }
}

void mimhid_core_init(void) {
    g_unique_id_len = (uint8_t)hal_unique_id(g_unique_id, sizeof(g_unique_id));
    for (uint8_t i = 0; i < MIMHID_BAUD_COUNT; i++) {
        if (hal_uart_baud_ok(g_rates[i])) g_baud_mask = (uint16_t)(g_baud_mask | (1u << i));
    }
    g_baud_mask |= 1u; // the boot rate is always there
    tusb_init();
    tud_disconnect();
}

static uint8_t g_rr = 0;

void mimhid_usb_poll(void) {
    // Commands from the receive side
    while (g_cmd_tail != __atomic_load_n(&g_cmd_head, __ATOMIC_ACQUIRE)) {
        run_command(&g_cmd[g_cmd_tail]);
        __atomic_store_n(&g_cmd_tail, (g_cmd_tail + 1) & CMD_MASK, __ATOMIC_RELEASE);
    }

    // Each queue is serviced independently; the start index rotates for fairness.
    const uint8_t count = mimhid_active_interfaces();
    for (uint8_t n = 0; n < count; n++) {
        uint8_t iface = (uint8_t)((g_rr + n) % count);
        if (!tud_hid_n_ready(iface)) continue;
        const report_t* item = queue_peek(iface);
        if (item && tud_hid_n_report(iface, 0, item->data, item->len)) {
            queue_pop(iface);
        }
    }
    if (count) g_rr = (uint8_t)((g_rr + 1) % count);
}

void mimhid_run(void) {
    for (;;) {
        tud_task();
        mimhid_rx_poll();
        mimhid_usb_poll();
    }
}

// -------------------------------------------------------------
// TinyUSB callbacks
// -------------------------------------------------------------
void tud_mount_cb(void) {
    send_event(MIMHID_EV_MOUNTED, NULL, 0);
}

void tud_umount_cb(void) {
    send_event(MIMHID_EV_UNMOUNTED, NULL, 0);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    const uint8_t v = remote_wakeup_en ? 1 : 0;
    send_event(MIMHID_EV_SUSPEND, &v, 1);
}

void tud_resume_cb(void) {
    send_event(MIMHID_EV_RESUME, NULL, 0);
}

void tud_hid_report_complete_cb(uint8_t itf, uint8_t const* report, uint16_t len) {
    (void)itf;
    (void)report;
    (void)len;
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    if (report_type == HID_REPORT_TYPE_FEATURE) {
        const mimhid_feature_t* f = mimhid_find_feature(itf, report_id);
        if (f) {
            uint16_t n = f->len < reqlen ? f->len : reqlen;
            memcpy(buffer, f->data, n);
            return n;
        }
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    if (report_type == HID_REPORT_TYPE_OUTPUT && report_id == 0) {
        uint8_t ev[2 + MIMHID_MAX_REPORT_BYTES];
        uint8_t n = bufsize > MIMHID_MAX_REPORT_BYTES ? MIMHID_MAX_REPORT_BYTES : (uint8_t)bufsize;
        ev[0] = itf;
        ev[1] = n;
        if (n && buffer) memcpy(&ev[2], buffer, n);
        send_event(MIMHID_EV_OUTPUT_REPORT, ev, (uint8_t)(2 + n));
    } else {
        uint8_t ev[3 + 48];
        uint8_t n = bufsize > 48 ? 48 : (uint8_t)bufsize;
        ev[0] = itf;
        ev[1] = (uint8_t)report_type;
        ev[2] = report_id;
        if (n && buffer) memcpy(&ev[3], buffer, n);
        send_event(MIMHID_EV_SET_REPORT, ev, (uint8_t)(3 + n));
    }
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol) {
    const uint8_t ev[2] = {instance, protocol};
    send_event(MIMHID_EV_SET_PROTOCOL, ev, 2);
}

bool tud_hid_set_idle_cb(uint8_t instance, uint8_t idle_rate) {
    const uint8_t ev[2] = {instance, idle_rate};
    send_event(MIMHID_EV_SET_IDLE, ev, 2);
    return true;
}
