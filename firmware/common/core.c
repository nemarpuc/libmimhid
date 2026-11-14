// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#include "core.h"
#include "config.h"
#include "control.h"
#include "generic.h"
#include "hal.h"
#include "tusb.h"
#include "tx.h"
#include <string.h>

// The port's stated endpoint packet size and the speed the stack runs at must agree, and the
// highest endpoint number must be one the stack can open.
_Static_assert((MIMUSB_MAX_ENDPOINT_BYTES == 1024) == (TUD_OPT_HIGH_SPEED != 0),
               "MIMUSB_MAX_ENDPOINT_BYTES is 1024 at high speed and 64 at full speed");
_Static_assert(MIMUSB_MAX_ENDPOINT_NUMBER < CFG_TUD_ENDPPOINT_MAX,
               "MIMUSB_MAX_ENDPOINT_NUMBER is an endpoint number the stack opens");

// -------------------------------------------------------------
// Loss reporting
// -------------------------------------------------------------
// Loss reporting is off until the host asks (LOSS_CONFIG): nothing is counted or sent before.
// Then at most one event per interval. The counts themselves are the generic driver's.
static bool g_lost_on;
static uint16_t g_lost_interval_ms;
static uint32_t g_lost_sent_ms;

// -------------------------------------------------------------
// Frames to the host
// -------------------------------------------------------------
// A frame is encoded into one of two rings (tx.c), one for each context that sends: the USB side
// (events, EP_OUT data, the answers to configuration commands) and the receive side (PONG and the
// answers to SET_BAUD, COUNTERS and CONTROL_REPLY).
#define SEND_USB(type, payload, len) mimusb_tx_frame(MIMUSB_TX_USB_SIDE, type, payload, len)
#define SEND_RX(type, payload, len)  mimusb_tx_frame(MIMUSB_TX_RX_SIDE, type, payload, len)

static uint8_t g_event[1 + MIMUSB_MAX_FRAME_PAYLOAD - 1];

// USB side only.
void mimusb_send_event(uint8_t kind, const uint8_t* data, uint16_t len) {
    if (len > sizeof(g_event) - 1)
        len = (uint16_t)(sizeof(g_event) - 1);
    g_event[0] = kind;
    if (len && data)
        memcpy(&g_event[1], data, len);
    SEND_USB(MIMUSB_T_EVENT, g_event, (uint16_t)(1 + len));
}

// USB side only: a packet the target sent on an OUT endpoint, to
// the host, as it came (see MIMUSB_T_EP_OUT_BASE in mimusb_proto.h).
void mimusb_send_ep_out(uint8_t number, const uint8_t* data, uint16_t length) {
    SEND_USB((uint8_t)(MIMUSB_T_EP_OUT_BASE | (number & 0x0F)), data, length);
}

static void send_ack(mimusb_tx_side side, uint8_t seq, uint8_t type) {
    const uint8_t p[2] = {seq, type};
    mimusb_tx_frame(side, MIMUSB_T_ACK, p, 2);
}

static void send_ack_data(mimusb_tx_side side, uint8_t seq, uint8_t type, const uint8_t* data, uint16_t len) {
    uint8_t p[2 + 16];
    if (len > sizeof(p) - 2)
        len = (uint16_t)(sizeof(p) - 2);
    p[0] = seq;
    p[1] = type;
    memcpy(&p[2], data, len);
    mimusb_tx_frame(side, MIMUSB_T_ACK, p, (uint16_t)(2 + len));
}

static void send_nak(mimusb_tx_side side, uint8_t seq, uint8_t type, uint8_t code) {
    const uint8_t p[3] = {seq, type, code};
    mimusb_tx_frame(side, MIMUSB_T_NAK, p, 3);
}

// -------------------------------------------------------------
// Commands: parsed by the RX side, executed by the USB side
// -------------------------------------------------------------
// A host that heard nothing back sends the same command again with the same sequence number;
// the board answers it again from here instead of running it twice. A PING (a new host, which
// starts its numbers anew) makes it forget.
typedef struct {
    bool valid;
    uint8_t seq;
    uint8_t type;
    uint8_t code; // 0 for an ACK
    uint32_t ping;
} answer_t;

static answer_t g_answer_last;
static uint32_t g_pings;     // RX side counts, USB side reads
static uint32_t g_cmd_pings; // g_pings when the running command was taken

// USB side only: answers the running command and keeps the answer.
static void cmd_answer(uint8_t seq, uint8_t type, uint8_t code) {
    g_answer_last.valid = true;
    g_answer_last.seq = seq;
    g_answer_last.type = type;
    g_answer_last.code = code;
    g_answer_last.ping = g_cmd_pings;
    if (code)
        send_nak(MIMUSB_TX_USB_SIDE, seq, type, code);
    else
        send_ack(MIMUSB_TX_USB_SIDE, seq, type);
}

#define CMD_SLOTS 4
#define CMD_MASK (CMD_SLOTS - 1)

typedef struct {
    uint8_t type;
    uint16_t len;
    uint8_t data[MIMUSB_MAX_FRAME_PAYLOAD];
} command_t;

static command_t g_cmd[CMD_SLOTS];
static volatile uint32_t g_cmd_head = 0;
static volatile uint32_t g_cmd_tail = 0;

static bool cmd_push(uint8_t type, const uint8_t* data, uint16_t len) {
    uint32_t h = g_cmd_head;
    uint32_t n = (h + 1) & CMD_MASK;
    if (n == __atomic_load_n(&g_cmd_tail, __ATOMIC_ACQUIRE))
        return false;
    g_cmd[h].type = type;
    g_cmd[h].len = len;
    memcpy(g_cmd[h].data, data, len);
    __atomic_store_n(&g_cmd_head, n, __ATOMIC_RELEASE);
    return true;
}

// -------------------------------------------------------------
// RX parser
// -------------------------------------------------------------
// Room for a few of the longest frames the board accepts; a power of two.
#if MIMUSB_MAX_FRAME_PAYLOAD <= 250
#define RX_SIZE 1024
#elif MIMUSB_MAX_FRAME_PAYLOAD <= 1500
#define RX_SIZE 4096
#else
#define RX_SIZE 8192
#endif
#define RX_MASK (RX_SIZE - 1)

static uint8_t g_rx[RX_SIZE];
static uint32_t g_rd = 0;
static uint32_t g_wr = 0;
static uint8_t g_unique_id[MIMUSB_UNIQUE_ID_MAX];
static uint8_t g_unique_id_len = 0;
static uint32_t g_last_rx_ms = 0;

// The line rate. A rate other than the boot rate stays unconfirmed until a valid frame arrives
// at it.
static uint32_t g_max_baud = MIMUSB_BOOT_BAUD;
static uint32_t g_baud = MIMUSB_BOOT_BAUD;
static bool g_baud_pending = false;
static uint32_t g_baud_since_ms = 0;

// Link counters (see mimusb_proto.h): the RX side counts, nothing else touches them.
enum { CNT_PROBES, CNT_BAD_CRC, CNT_FRAMING, CNT_UART_OVERRUN, CNT_RING_OVERRUN };
static uint16_t g_counters[MIMUSB_COUNTER_COUNT];

static void count(unsigned which) {
    if (g_counters[which] != 0xFFFFu)
        g_counters[which]++;
}

// A corrupt header can announce a long frame whose bytes never come. Waiting that long for
// the payload would swallow the valid frames behind it, so a frame that stays incomplete with
// the line idle for this long is abandoned one byte at a time.
#define RX_IDLE_RESYNC_MS 10

static void rx_fill(void) {
    for (;;) {
        uint32_t free_bytes = RX_SIZE - 1 - ((g_wr - g_rd) & RX_MASK);
        if (free_bytes == 0)
            return;
        uint32_t chunk = RX_SIZE - g_wr;
        if (chunk > free_bytes)
            chunk = free_bytes;
        size_t n = hal_uart_read(&g_rx[g_wr], chunk);
        if (n == 0)
            return;
        g_last_rx_ms = hal_millis();
        g_wr = (g_wr + (uint32_t)n) & RX_MASK;
    }
}

static bool known_command(uint8_t type) {
    return type == MIMUSB_T_PING || (type >= MIMUSB_T_DESC_RESET && type <= MIMUSB_T_RULE_DATA);
}

static void set_baud(uint32_t baud) {
    mimusb_tx_set_baud(baud); // what is queued leaves at the old rate first
    g_baud = baud;
    g_baud_pending = baud != MIMUSB_BOOT_BAUD;
    g_baud_since_ms = hal_millis();
}

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// The divider's error for a requested rate, in parts per million.
static uint32_t baud_error_ppm(uint32_t asked, uint32_t actual) {
    const uint64_t diff = asked > actual ? (uint64_t)(asked - actual) : (uint64_t)(actual - asked);
    return (uint32_t)(diff * 1000000u / asked);
}

// Runs on the receive side: the ACK has to leave at the old rate before the switch.
static void cmd_set_baud(const uint8_t* data, uint16_t len) {
    const uint8_t seq = data[0];
    if (len != 5) {
        send_nak(MIMUSB_TX_RX_SIDE, seq, MIMUSB_T_SET_BAUD, MIMUSB_NAK_PARAM);
        return;
    }
    const uint32_t baud = get_u32(&data[1]);
    const uint32_t actual = (baud == 0 || baud > g_max_baud) ? 0 : hal_uart_baud_actual(baud);
    if (actual == 0 || baud_error_ppm(baud, actual) > MIMUSB_BAUD_TOLERANCE_PPM) {
        send_nak(MIMUSB_TX_RX_SIDE, seq, MIMUSB_T_SET_BAUD, MIMUSB_NAK_PARAM);
        return;
    }
    uint8_t out[4];
    put_u32(out, actual);
    send_ack_data(MIMUSB_TX_RX_SIDE, seq, MIMUSB_T_SET_BAUD, out, 4);
    set_baud(baud);
}

// Runs on the receive side, like SET_BAUD: the counters belong to it.
static void cmd_counters(const uint8_t* data, uint16_t len) {
    const uint8_t seq = data[0];
    if (len != 2) {
        send_nak(MIMUSB_TX_RX_SIDE, seq, MIMUSB_T_COUNTERS, MIMUSB_NAK_PARAM);
        return;
    }
    uint8_t out[2 * MIMUSB_COUNTER_COUNT];
    for (unsigned i = 0; i < MIMUSB_COUNTER_COUNT; i++) {
        out[2 * i] = (uint8_t)(g_counters[i] & 0xFF);
        out[2 * i + 1] = (uint8_t)(g_counters[i] >> 8);
    }
    send_ack_data(MIMUSB_TX_RX_SIDE, seq, MIMUSB_T_COUNTERS, out, sizeof(out));
    if (data[1] & MIMUSB_COUNTERS_F_RESET)
        memset(g_counters, 0, sizeof(g_counters));
}

static void send_pong(void) {
    uint8_t p[MIMUSB_PONG_MIN_LEN + MIMUSB_UNIQUE_ID_MAX];
    p[0] = FW_VERSION_MAJOR;
    p[1] = FW_VERSION_MINOR;
    p[2] = FW_VERSION_PATCH;
    p[3] = tud_ready() ? 1 : 0;
    p[4] = 0; // reserved
    p[5] = 0; // reserved
    p[6] = g_unique_id_len ? MIMUSB_PONG_FLAG_UNIQUE_ID : 0;
    p[7] = hal_chip();
#if TUD_OPT_HIGH_SPEED
    p[8] = 1;
#else
    p[8] = 0;
#endif
    put_u32(&p[9], g_baud);
    put_u32(&p[13], g_max_baud);
    put_u32(&p[17], hal_uart_has_break() ? MIMUSB_PONG_UART_BREAK : 0u);
    p[21] = 0; // reserved
    p[22] = 0;
    p[23] = (uint8_t)(MIMUSB_MAX_REPORT_BYTES & 0xFF);
    p[24] = (uint8_t)(MIMUSB_MAX_REPORT_BYTES >> 8);
    p[25] = 0; // reserved
    p[26] = 0;
    p[27] = (uint8_t)(MIMUSB_MAX_ENDPOINT_BYTES & 0xFF);
    p[28] = (uint8_t)(MIMUSB_MAX_ENDPOINT_BYTES >> 8);
    p[29] = (uint8_t)(MIMUSB_MAX_FRAME_PAYLOAD & 0xFF);
    p[30] = (uint8_t)(MIMUSB_MAX_FRAME_PAYLOAD >> 8);
    p[31] = (uint8_t)(MIMUSB_CONTROL_DATA_MAX & 0xFF);
    p[32] = (uint8_t)(MIMUSB_CONTROL_DATA_MAX >> 8);
    p[33] = MIMUSB_CONTROL_RULES;
    p[34] = MIMUSB_MAX_ENDPOINT_NUMBER;
    p[35] = g_unique_id_len;
    p[36] = MIMUSB_MAX_INTERFACES;
    p[37] = (uint8_t)(MIMUSB_MAX_CONFIG_BYTES & 0xFF);
    p[38] = (uint8_t)(MIMUSB_MAX_CONFIG_BYTES >> 8);
    p[39] = MIMUSB_MAX_STRINGS;
    p[40] = (uint8_t)(MIMUSB_MAX_TRANSFER_BYTES & 0xFF);
    p[41] = (uint8_t)(MIMUSB_MAX_TRANSFER_BYTES >> 8);
    p[42] = (uint8_t)(MIMUSB_EP_POOL_SLOTS & 0xFF);
    p[43] = (uint8_t)(MIMUSB_EP_POOL_SLOTS >> 8);
    p[44] = (uint8_t)((1u << TUSB_XFER_BULK) | (1u << TUSB_XFER_INTERRUPT) |
                      (MIMUSB_ISO_BYTES != 0 ? (1u << TUSB_XFER_ISOCHRONOUS) : 0u));
    p[45] = (uint8_t)(MIMUSB_ANSWER_BYTES & 0xFF);
    p[46] = (uint8_t)(MIMUSB_ANSWER_BYTES >> 8);
    p[47] = (uint8_t)(MIMUSB_MAX_BOS_BYTES & 0xFF);
    p[48] = (uint8_t)(MIMUSB_MAX_BOS_BYTES >> 8);
    memcpy(&p[MIMUSB_PONG_MIN_LEN], g_unique_id, g_unique_id_len);
    SEND_RX(MIMUSB_T_PONG, p, (uint16_t)(MIMUSB_PONG_MIN_LEN + g_unique_id_len));
}

// The command frame being read; used by the receive side only.
static uint8_t g_rx_cmd[MIMUSB_MAX_FRAME_PAYLOAD];

static bool crc_matches(uint16_t crc, uint32_t at) {
    return g_rx[at & RX_MASK] == (uint8_t)(crc & 0xFF) &&
           g_rx[(at + 1) & RX_MASK] == (uint8_t)(crc >> 8);
}

// Handles at most one frame or skips one byte. Returns false when more input is needed.
// A frame that fails validation is skipped one byte at a time so the parser
// resynchronizes on the next sync byte instead of swallowing a following valid frame.
static bool rx_step(void) {
    uint32_t read_idx = g_rd;
    uint32_t available = (g_wr - read_idx) & RX_MASK;
    if (available == 0)
        return false;

    if (g_rx[read_idx] != MIMUSB_SYNC) {
        g_rd = (read_idx + 1) & RX_MASK;
        return true;
    }
    if (available < 3)
        return false;

    const uint8_t type = g_rx[(read_idx + 1) & RX_MASK];
    const uint8_t len_low = g_rx[(read_idx + 2) & RX_MASK];
    uint8_t len_high = 0;
    uint32_t header = 3;
    uint16_t len = len_low;
    if (len_low & 0x80) {
        if (available < 4)
            return false;
        len_high = g_rx[(read_idx + 3) & RX_MASK];
        len = (uint16_t)((len_low & 0x7F) | ((uint16_t)len_high << 7));
        header = 4;
    }
    // Endpoint 0 has no IN data frame: 0x40 itself is neither a report nor a command.
    const bool is_report = type > MIMUSB_T_EP_IN_BASE && type <= MIMUSB_T_EP_IN_LAST;

    // Reject unknown types and impossible lengths before waiting for the payload.
    if (!is_report && !known_command(type)) {
        g_rd = (read_idx + 1) & RX_MASK;
        return true;
    }
    if (is_report
            ? len > MIMUSB_MAX_TRANSFER_BYTES // 0 is a zero-length packet (generic endpoints only)
            : (type == MIMUSB_T_PING ? len != 0 : (len < 1 || len > MIMUSB_MAX_FRAME_PAYLOAD))) {
        g_rd = (read_idx + 1) & RX_MASK;
        return true;
    }

    const uint32_t total = header + len + 2u;
    if (available < total)
        return false;

    uint16_t crc = mimusb_crc16_update(MIMUSB_CRC16_INIT, type);
    crc = mimusb_crc16_update(crc, len_low);
    if (header == 4)
        crc = mimusb_crc16_update(crc, len_high);
    const uint32_t body = read_idx + header;
    if (is_report) {
        // The queue of this IN endpoint in the active configuration (generic.c), or none, in which
        // case the frame is dropped. A zero-length transfer is a zero-length packet.
        const uint8_t number = (uint8_t)(type & 0x0F);
        uint8_t* dst = mimusb_generic_in_reserve(number);
        for (uint16_t i = 0; i < len; i++) {
            uint8_t b = g_rx[(body + i) & RX_MASK];
            if (dst != NULL)
                dst[i] = b;
            crc = mimusb_crc16_update(crc, b);
        }
        if (!crc_matches(crc, body + len)) {
            count(CNT_BAD_CRC);
            mimusb_generic_in_discard();
            g_rd = (read_idx + 1) & RX_MASK;
            return true;
        }
        g_baud_pending = false;
        if (dst != NULL)
            mimusb_generic_in_commit(len);
    } else {
        for (uint16_t i = 0; i < len; i++) {
            uint8_t b = g_rx[(body + i) & RX_MASK];
            g_rx_cmd[i] = b;
            crc = mimusb_crc16_update(crc, b);
        }
        if (!crc_matches(crc, body + len)) {
            count(CNT_BAD_CRC);
            g_rd = (read_idx + 1) & RX_MASK;
            return true;
        }
        g_baud_pending = false;
        g_rd = (read_idx + total) & RX_MASK;
        if (type == MIMUSB_T_PING) {
            __atomic_fetch_add(&g_pings, 1u, __ATOMIC_RELEASE);
            send_pong();
        } else if (type == MIMUSB_T_SET_BAUD) {
            cmd_set_baud(g_rx_cmd, len);
        } else if (type == MIMUSB_T_COUNTERS) {
            cmd_counters(g_rx_cmd, len);
        } else if (type == MIMUSB_T_PROBE) {
            bool right = true;
            for (uint16_t i = 0; i < len; i++) {
                if (g_rx_cmd[i] != MIMUSB_PROBE_BYTE(i)) {
                    right = false;
                    break;
                }
            }
            if (right)
                count(CNT_PROBES);
            else
                count(CNT_BAD_CRC); // a pattern that does not match with a good CRC is still damage
        } else if (type == MIMUSB_T_CONTROL_REPLY) {
            // The USB side may be waiting in a TinyUSB callback, so the reply is taken here.
            const uint8_t code = mimusb_control_reply(&g_rx_cmd[1], (uint16_t)(len - 1));
            if (code)
                send_nak(MIMUSB_TX_RX_SIDE, g_rx_cmd[0], type, code);
            else
                send_ack(MIMUSB_TX_RX_SIDE, g_rx_cmd[0], type);
        } else {
            cmd_push(type, g_rx_cmd,
                     len); // a full mailbox drops the frame; the host retries on no ACK
        }
        return true;
    }
    g_rd = (read_idx + total) & RX_MASK;
    return true;
}

void mimusb_rx_poll(void) {
    // The UART takes the next piece of what is queued; on the RP boards this loop is what keeps it fed.
    mimusb_tx_pump();
    // An unconfirmed rate that heard nothing valid in time falls back to the boot rate.
    if (g_baud_pending && (int32_t)(hal_millis() - g_baud_since_ms) >= MIMUSB_BAUD_CONFIRM_MS)
        set_baud(MIMUSB_BOOT_BAUD);
    const uint8_t errors = hal_uart_errors();
    if (errors & HAL_UART_ERR_BREAK) {
        // Whatever rate the board was on, a BREAK brings it back to the one both ends know, and
        // what it holds was sent at the old rate and means nothing.
        if (g_baud != MIMUSB_BOOT_BAUD)
            set_baud(MIMUSB_BOOT_BAUD);
        g_baud_pending = false;
        uint8_t sink[64];
        while (hal_uart_read(sink, sizeof(sink)) != 0) {
        }
        g_rd = g_wr = 0;
        return;
    }
    if (errors & HAL_UART_ERR_FRAMING)
        count(CNT_FRAMING);
    if (errors & HAL_UART_ERR_OVERRUN)
        count(CNT_UART_OVERRUN);
    if (errors & HAL_UART_ERR_RING)
        count(CNT_RING_OVERRUN);
    for (;;) {
        rx_fill();
        if (rx_step())
            continue;
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
static mimusb_config_t g_staged;
// True from DESC_RESET until the next COMMIT that applies a configuration (and again after a
// detach): the staged configuration is being built and is not the active one, so an EP_CONFIG
// only stages. Otherwise the staged configuration is the active one and an EP_CONFIG also takes
// effect at once.
static bool g_staged_fresh = true;

static void cmd_desc_reset(const command_t* c) {
    const uint8_t seq = c->data[0];
    if (c->len != 1) {
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    memset(&g_staged, 0, sizeof(g_staged));
    mimusb_control_reset();
    g_staged_fresh = true;
    cmd_answer(seq, c->type, 0);
}

static uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// The staged string of an index and a language, or NULL.
static mimusb_string_t* find_string(uint8_t index, uint16_t language) {
    for (uint8_t i = 0; i < MIMUSB_MAX_STRINGS; i++) {
        mimusb_string_t* s = &g_staged.string[i];
        if (s->d.used && s->index == index && s->language == language)
            return s;
    }
    return NULL;
}

// The staged descriptor a kind, index and language name: its bookkeeping and where its bytes are
// kept. False when the combination names none (a fixed kind with a wrong index, or a string or
// report descriptor that was not announced); *ref->d may be unused for a device or configuration
// descriptor that was not announced yet.
typedef struct {
    mimusb_desc_t* d;
    uint8_t* bytes;
} desc_ref_t;

static bool find_desc(uint8_t kind, uint8_t index, uint16_t language, desc_ref_t* ref) {
    switch (kind) {
    case MIMUSB_DESC_DEVICE:
        ref->d = &g_staged.device;
        ref->bytes = g_staged.device_bytes;
        return index == 0 && language == 0;
    case MIMUSB_DESC_CONFIGURATION:
        ref->d = &g_staged.config;
        ref->bytes = g_staged.config_bytes;
        return index == 0 && language == 0;
    case MIMUSB_DESC_STRING: {
        mimusb_string_t* s = find_string(index, language);
        if (s == NULL)
            return false;
        ref->d = &s->d;
        ref->bytes = &g_staged.string_pool[s->off];
        return true;
    }
    case MIMUSB_DESC_BOS:
        ref->d = &g_staged.bos;
        ref->bytes = g_staged.bos_bytes;
        return index == 0 && language == 0;
    case MIMUSB_DESC_ANSWER:
        // The bytes of a staged ANSWER rule (control.c keeps them).
        return language == 0 && mimusb_control_staged_answer(index, &ref->d, &ref->bytes);
    default:
        return false;
    }
}

static void cmd_desc_begin(const command_t* c) {
    const uint8_t seq = c->data[0];
    const uint8_t* p = &c->data[1];
    if (c->len != 1 + 7) {
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    const uint8_t kind = p[0];
    const uint8_t index = p[1];
    const uint16_t language = get_u16(&p[2]);
    const uint16_t total = get_u16(&p[4]);
    const uint8_t flags = p[6];

    // A descriptor announced again as it was announced is accepted again (the host repeats a
    // frame whose ACK it did not see); with other values it is another descriptor, and refused.
    desc_ref_t ref = {NULL, NULL};
    if (find_desc(kind, index, language, &ref) && ref.d->used) {
        const mimusb_string_t* s = kind == MIMUSB_DESC_STRING ? find_string(index, language) : NULL;
        const uint8_t had = (s != NULL && s->from_id) ? MIMUSB_DESC_F_SERIAL_FROM_ID : 0;
        const bool same = (s != NULL && s->from_id) ? total == 0 : ref.d->total == total;
        cmd_answer(seq, c->type, same && flags == had ? 0 : MIMUSB_NAK_STATE);
        return;
    }

    // Nothing is filled in: the index, the language and the flags are what the kind needs.
    switch (kind) {
    case MIMUSB_DESC_DEVICE:
        // USB 2.0 9.6.1: 18 bytes. A longer blob cannot be held; a shorter one is refused when the
        // configuration is committed.
        if (index != 0 || language != 0 || flags != 0 || total == 0) {
            cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
            return;
        }
        if (total > sizeof(g_staged.device_bytes)) {
            cmd_answer(seq, c->type, MIMUSB_NAK_OVERFLOW);
            return;
        }
        g_staged.device = (mimusb_desc_t){.used = true, .total = total, .recv = 0};
        break;
    case MIMUSB_DESC_CONFIGURATION:
        if (index != 0 || language != 0 || flags != 0 || total == 0) {
            cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
            return;
        }
        if (total > sizeof(g_staged.config_bytes)) {
            cmd_answer(seq, c->type, MIMUSB_NAK_OVERFLOW);
            return;
        }
        g_staged.config = (mimusb_desc_t){.used = true, .total = total, .recv = 0};
        break;
    case MIMUSB_DESC_BOS:
        // USB 3.2 9.6.2: the 5-byte header and at least one capability is checked at COMMIT; here
        // only that the whole of it can be held.
        if (index != 0 || language != 0 || flags != 0 || total == 0) {
            cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
            return;
        }
        if (total > sizeof(g_staged.bos_bytes)) {
            cmd_answer(seq, c->type, MIMUSB_NAK_OVERFLOW);
            return;
        }
        g_staged.bos = (mimusb_desc_t){.used = true, .total = total, .recv = 0};
        break;
    case MIMUSB_DESC_STRING: {
        // USB 2.0 9.6.7: bLength, 3 and UTF-16LE units, so an even length of 2 to 254 bytes.
        // Index 0 is the language list, which the board makes itself.
        const bool from_id = (flags & MIMUSB_DESC_F_SERIAL_FROM_ID) != 0;
        if (index == 0 || language == 0 || (flags & ~MIMUSB_DESC_F_SERIAL_FROM_ID) != 0 ||
            (from_id ? total != 0 : (total < 2 || total > MIMUSB_STRING_DESC_MAX || (total & 1)))) {
            cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
            return;
        }
        mimusb_string_t* slot = NULL;
        for (uint8_t i = 0; i < MIMUSB_MAX_STRINGS && slot == NULL; i++) {
            if (!g_staged.string[i].d.used)
                slot = &g_staged.string[i];
        }
        uint8_t made[MIMUSB_STRING_DESC_MAX];
        const uint16_t made_len = from_id ? mimusb_unique_id_string(made) : 0;
        const uint16_t room = from_id ? made_len : total;
        if (from_id && made_len == 0) {
            cmd_answer(seq, c->type, MIMUSB_NAK_UNSUPPORTED);
            return;
        } // no chip id
        if (slot == NULL || (uint32_t)g_staged.string_used + room > sizeof(g_staged.string_pool)) {
            cmd_answer(seq, c->type, MIMUSB_NAK_OVERFLOW);
            return;
        }
        slot->index = index;
        slot->language = language;
        slot->from_id = from_id;
        slot->off = g_staged.string_used;
        slot->d = (mimusb_desc_t){.used = true, .total = room, .recv = from_id ? made_len : 0};
        if (from_id)
            memcpy(&g_staged.string_pool[slot->off], made, made_len);
        g_staged.string_used = (uint16_t)(g_staged.string_used + room);
        break;
    }
    case MIMUSB_DESC_ANSWER: {
        // The rule is staged first, with its length; the bytes are announced with that length.
        if (language != 0 || flags != 0) {
            cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
            return;
        }
        if (ref.d == NULL) {
            cmd_answer(seq, c->type, MIMUSB_NAK_STATE); // no such rule, or one with no bytes
            return;
        }
        if (total != ref.d->total) {
            cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
            return;
        }
        ref.d->used = true;
        ref.d->recv = 0;
        break;
    }
    default:
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    cmd_answer(seq, c->type, 0);
}

static void cmd_desc_data(const command_t* c) {
    const uint8_t seq = c->data[0];
    const uint8_t* p = &c->data[1];
    const uint16_t len = (uint16_t)(c->len - 1);
    if (len < 7 || len - 6 > MIMUSB_DESC_DATA_CHUNK) {
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    const uint16_t off = get_u16(&p[4]);
    const uint16_t n = (uint16_t)(len - 6);
    desc_ref_t ref;
    if (!find_desc(p[0], p[1], get_u16(&p[2]), &ref) || !ref.d->used) {
        cmd_answer(seq, c->type, MIMUSB_NAK_STATE);
        return;
    }
    // The board's own strings have all their bytes already.
    const mimusb_string_t* str = p[0] == MIMUSB_DESC_STRING ? find_string(p[1], get_u16(&p[2])) : NULL;
    if (str != NULL && str->from_id) {
        cmd_answer(seq, c->type, MIMUSB_NAK_STATE);
        return;
    }
    if ((uint32_t)off + n > ref.d->total || off > ref.d->recv) {
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    memcpy(&ref.bytes[off], &p[6], n); // a repeated chunk rewrites the same bytes
    if ((uint32_t)off + n > ref.d->recv)
        ref.d->recv = (uint16_t)(off + n);
    cmd_answer(seq, c->type, 0);
}

// The layout of the staged configuration, when it is complete and holds together.
static bool staged_layout(mimusb_layout_t* layout) { return mimusb_walk_config(&g_staged, layout); }

static void cmd_rule(const command_t* c) {
    cmd_answer(c->data[0], c->type, mimusb_control_stage(&c->data[1], (uint16_t)(c->len - 1)));
}

// RULES_APPLY: the staged rules become the active ones, checked against the active configuration.
static void cmd_rules_apply(const command_t* c) {
    const uint8_t seq = c->data[0];
    if (c->len != 3 || (c->data[1] & ~1u) != 0) {
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    uint8_t code = mimusb_control_check(mimusb_active_layout(), c->data[2]);
    if (code == 0)
        code = mimusb_control_apply((c->data[1] & 1u) != 0, c->data[2], false);
    cmd_answer(seq, c->type, code);
}

static void cmd_rule_data(const command_t* c) {
    cmd_answer(c->data[0], c->type, mimusb_control_rule_data(&c->data[1], (uint16_t)(c->len - 1)));
}

static void cmd_ep_config(const command_t* c) {
    const uint8_t seq = c->data[0];
    if (c->len != 4) {
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    const uint8_t address = c->data[1], depth = c->data[2], policy = c->data[3];
    const uint8_t number = address & 0x0F;
    // USB 2.0 9.6.6: bits 6..4 of an endpoint address are zero and its number is not 0.
    if ((address & 0x70) != 0 || number == 0 || depth == 0 || policy < MIMUSB_EP_FULL_DROP_NEWEST ||
        policy > MIMUSB_EP_FULL_WAIT) {
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    if ((address & 0x80) == 0) { // the board queues IN transfers only
        cmd_answer(seq, c->type, MIMUSB_NAK_UNSUPPORTED);
        return;
    }
    mimusb_layout_t layout;
    if (!staged_layout(&layout) || layout.ep[1][number].kind == MIMUSB_EP_NONE) {
        cmd_answer(seq, c->type, MIMUSB_NAK_STATE);
        return;
    }
    // A queue that cannot be cut from the pool at all is refused here, and one
    // that is too big together with the others at COMMIT.
    if (mimusb_generic_slots(depth, policy) > MIMUSB_EP_POOL_SLOTS) {
        cmd_answer(seq, c->type, MIMUSB_NAK_OVERFLOW);
        return;
    }
    if (!g_staged_fresh) {
        const uint8_t code = mimusb_generic_set_policy(address, depth, policy);
        if (code) {
            cmd_answer(seq, c->type, code);
            return;
        }
    }
    g_staged.ep[number].depth = depth;
    g_staged.ep[number].policy = policy;
    cmd_answer(seq, c->type, 0);
}

static void cmd_ep_stall(const command_t* c) {
    const uint8_t seq = c->data[0];
    if (c->len != 3 || c->data[2] > 1 || (c->data[1] & 0x70) != 0 || (c->data[1] & 0x0F) == 0) {
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    cmd_answer(seq, c->type, mimusb_generic_stall(c->data[1], c->data[2] == 1));
}

static void cmd_ep_flush(const command_t* c) {
    const uint8_t seq = c->data[0];
    if (c->len != 2 || (c->data[1] & 0x70) != 0 || (c->data[1] & 0x0F) == 0) {
        cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        return;
    }
    cmd_answer(seq, c->type, mimusb_generic_flush(c->data[1]));
}

// True while the board's D+ pull-up is meant to be on: from a COMMIT that applied a configuration
// until a detach. A port whose power events can raise the pull-up by themselves (nRF52840) reads
// this to take it down again.
static volatile bool g_attached = false;
bool mimusb_core_attached(void) { return g_attached; }

static void cmd_commit(const command_t* c) {
    const uint8_t seq = c->data[0];
    if (c->len >= 2 && c->data[1] == 1) {
        // Detach: leave the target's bus and forget the active configuration.
        g_attached = false;
        tud_disconnect();
        mimusb_control_detach();
        mimusb_clear_config();
        static const mimusb_layout_t none; // no endpoints: every queue of the pool is released
        mimusb_generic_apply(&g_staged, &none);
        g_staged_fresh = true;
        cmd_answer(seq, c->type, 0);
        return;
    }
    // A COMMIT that attaches is for a board that is off the bus. On a board that is attached it
    // would change descriptors under a device the target has enumerated (it would never see the new
    // device); the host detaches first, and so must any other.
    if (g_attached) {
        cmd_answer(seq, c->type, MIMUSB_NAK_STATE);
        return;
    }
    // Everything that can refuse is checked before anything changes: the walk, then the pool.
    mimusb_layout_t layout;
    if (!staged_layout(&layout)) {
        cmd_answer(seq, c->type, MIMUSB_NAK_DESCRIPTOR);
        return;
    }
    uint8_t code = 0;
    if (!mimusb_generic_plan(&g_staged, &layout, &code)) {
        cmd_answer(seq, c->type, code);
        return;
    }
    // The staged rules must be complete and fit the configuration that is about to run.
    code = mimusb_control_check(&layout, MIMUSB_CONTROL_ALL);
    if (code) {
        cmd_answer(seq, c->type, code);
        return;
    }
    if (!mimusb_apply_config(&g_staged)) {
        cmd_answer(seq, c->type, MIMUSB_NAK_DESCRIPTOR);
        return;
    }
    mimusb_generic_apply(&g_staged, &layout);
    g_staged_fresh = false;
    mimusb_control_apply(false, MIMUSB_CONTROL_ALL, true);
    g_attached = true;
    tud_connect();
    cmd_answer(seq, c->type, 0);
}

static void run_command(const command_t* c) {
    if (c->type == MIMUSB_T_PING)
        return;
    const uint8_t seq = c->data[0];
    g_cmd_pings = __atomic_load_n(&g_pings, __ATOMIC_ACQUIRE);
    const answer_t* last = &g_answer_last;
    if (last->valid && last->ping == g_cmd_pings && last->seq == seq && last->type == c->type) {
        if (last->code)
            send_nak(MIMUSB_TX_USB_SIDE, seq, c->type, last->code);
        else
            send_ack(MIMUSB_TX_USB_SIDE, seq, c->type);
        return;
    }
    switch (c->type) {
    case MIMUSB_T_DESC_RESET:
        cmd_desc_reset(c);
        break;
    case MIMUSB_T_DESC_BEGIN:
        cmd_desc_begin(c);
        break;
    case MIMUSB_T_DESC_DATA:
        cmd_desc_data(c);
        break;
    case MIMUSB_T_RULE:
        cmd_rule(c);
        break;
    case MIMUSB_T_RULES_APPLY:
        cmd_rules_apply(c);
        break;
    case MIMUSB_T_RULE_DATA:
        cmd_rule_data(c);
        break;
    case MIMUSB_T_COMMIT:
        cmd_commit(c);
        break;
    case MIMUSB_T_EP_CONFIG:
        cmd_ep_config(c);
        break;
    case MIMUSB_T_EP_STALL:
        cmd_ep_stall(c);
        break;
    case MIMUSB_T_EP_FLUSH:
        cmd_ep_flush(c);
        break;
    case MIMUSB_T_LOSS_CONFIG:
        if (c->len != 4 || c->data[1] > 1 || (c->data[1] == 0 && (c->data[2] | c->data[3]))) {
            cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
            break;
        }
        __atomic_store_n(&g_lost_on, false, __ATOMIC_RELEASE);
        mimusb_generic_loss_set(false);
        g_lost_interval_ms = (uint16_t)(c->data[2] | (c->data[3] << 8));
        g_lost_sent_ms = hal_millis() - g_lost_interval_ms;
        __atomic_store_n(&g_lost_on, c->data[1] == 1, __ATOMIC_RELEASE);
        mimusb_generic_loss_set(c->data[1] == 1);
        cmd_answer(seq, c->type, 0);
        break;
    case MIMUSB_T_RESET:
        mimusb_generic_drop_all();
        mimusb_control_forget_sent();
        cmd_answer(seq, c->type, 0);
        break;
    case MIMUSB_T_REMOTE_WAKEUP:
        if (tud_suspended()) {
            tud_remote_wakeup();
            cmd_answer(seq, c->type, 0);
        } else {
            cmd_answer(seq, c->type, MIMUSB_NAK_STATE);
        }
        break;
    case MIMUSB_T_BOOTLOADER:
        if (c->len != 1 + 4 || memcmp(&c->data[1], MIMUSB_BOOTLOADER_KEY, 4) != 0) {
            cmd_answer(seq, c->type, MIMUSB_NAK_PARAM);
        } else if (!hal_has_bootloader()) {
            cmd_answer(seq, c->type, MIMUSB_NAK_UNSUPPORTED);
        } else {
            // The target sees the device leave, the host gets its answer, and the chip restarts.
            tud_disconnect();
            cmd_answer(seq, c->type, 0);
            mimusb_tx_flush(); // the answer has left the pin before the chip restarts
            hal_enter_bootloader();
        }
        break;
    default:
        cmd_answer(seq, c->type, MIMUSB_NAK_UNSUPPORTED);
        break;
    }
}

void mimusb_core_init(void) {
    g_unique_id_len = (uint8_t)hal_unique_id(g_unique_id, sizeof(g_unique_id));
    g_max_baud = hal_uart_max_baud();
    if (g_max_baud < MIMUSB_BOOT_BAUD)
        g_max_baud = MIMUSB_BOOT_BAUD; // the boot rate is always there
    mimusb_clear_config(); // the descriptors of a board with nothing committed
    tusb_init();
    g_attached = false;
    tud_disconnect();
}

void mimusb_usb_poll(void) {
    // Commands from the receive side
    while (g_cmd_tail != __atomic_load_n(&g_cmd_head, __ATOMIC_ACQUIRE)) {
        run_command(&g_cmd[g_cmd_tail]);
        __atomic_store_n(&g_cmd_tail, (g_cmd_tail + 1) & CMD_MASK, __ATOMIC_RELEASE);
    }
    mimusb_control_poll();

    mimusb_generic_poll();

    // A full queue drops what it is not given room for; a host that asked is told how many.
    if (g_lost_on && (int32_t)(hal_millis() - g_lost_sent_ms) >= (int32_t)g_lost_interval_ms) {
        for (uint8_t number = 1; number < 16; number++) {
            const uint32_t n = mimusb_generic_loss_take(number);
            if (n == 0)
                continue;
            const uint16_t sat = n > 0xFFFFu ? 0xFFFFu : (uint16_t)n;
            const uint8_t ev[3] = {(uint8_t)(0x80u | number), (uint8_t)(sat & 0xFF),
                                   (uint8_t)(sat >> 8)};
            mimusb_send_event(MIMUSB_EV_TRANSFER_LOST, ev, sizeof(ev));
            g_lost_sent_ms = hal_millis();
        }
    }
}

void mimusb_run(void) {
    for (;;) {
        tud_task();
        mimusb_rx_poll();
        mimusb_usb_poll();
    }
}

// -------------------------------------------------------------
// TinyUSB callbacks
// -------------------------------------------------------------
void tud_mount_cb(void) { mimusb_send_event(MIMUSB_EV_MOUNTED, NULL, 0); }

void tud_umount_cb(void) { mimusb_send_event(MIMUSB_EV_UNMOUNTED, NULL, 0); }

void tud_suspend_cb(bool remote_wakeup_en) {
    const uint8_t v = remote_wakeup_en ? 1 : 0;
    mimusb_send_event(MIMUSB_EV_SUSPEND, &v, 1);
}

void tud_resume_cb(void) { mimusb_send_event(MIMUSB_EV_RESUME, NULL, 0); }
