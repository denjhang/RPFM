/*
 * RPFM Firmware — HID Protocol
 * RP2350A → PIO → SPFM bus → YM2413/YM2151/AY8910
 *
 * USB HID 64B 帧通信，天然流控，解决 CDC 数据溢出死机问题。
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "tusb.h"
#include "spfm_bus.pio.h"
#include "pio_cs.pio.h"
#include "ws2812.pio.h"
#include "hardware/timer.h"
#include "protocol.h"
#include "command_buffer.h"

// ========== Pin Definitions ==========
#define PIN_BUS_BASE  0
#define PIN_CS_BASE   17
#define PIN_IC        21
#define PIN_LED       25
#define PIN_WS2812    16
#define NUM_WS_LEDS   4

// ========== PIO State ==========
static PIO s_bus_pio = pio0;
static uint s_bus_sm = 0;
static uint s_ay_sm = 0;      // AY8910 dedicated SM (slower clock)
static PIO s_cs_pio = pio1;
static uint s_cs_sm = 0;
static PIO s_ws_pio = pio1;
static uint s_ws_sm = 0;
static bool s_ws_ready = false;
static absolute_time_t s_ws_off_time = {0};
static uint32_t s_ws_leds[NUM_WS_LEDS] = {0};
static const uint32_t cs_colors[NUM_WS_LEDS] = {
    0x006600, 0x000066, 0x666600, 0x660000,
};

// ========== Command Buffer ==========
static cmd_buf_t s_cmd;

// ========== Protocol State ==========
static volatile uint8_t s_last_seq = 0;
static volatile uint8_t s_status = 0;
volatile uint8_t s_ay_delay_100ns = 10;  // AY8910 /WR pulse delay (100ns units), default 1µs

// VGM player (needs s_status, cs_select, write_reg_ay — declared below via forward decls in vgm_player.h)
#include "vgm_player.h"

// Deferred write queue (HID callback → main loop)
#define DEFERRED_Q_SIZE 16
#define DQF_AY 0x01  // AY8910 fast timing
static volatile uint8_t s_deferred_q[DEFERRED_Q_SIZE][3];
static volatile uint8_t s_deferred_flags[DEFERRED_Q_SIZE];
static volatile uint8_t s_deferred_head = 0;
static volatile uint8_t s_deferred_tail = 0;

static inline void deferred_push(uint8_t slot, uint8_t addr, uint8_t data, uint8_t flags) {
    uint8_t next = (s_deferred_head + 1) % DEFERRED_Q_SIZE;
    if (next == s_deferred_tail) return;
    s_deferred_q[s_deferred_head][0] = slot;
    s_deferred_q[s_deferred_head][1] = addr;
    s_deferred_q[s_deferred_head][2] = data;
    s_deferred_flags[s_deferred_head] = flags;
    s_deferred_head = next;
}

static inline bool deferred_pop(uint8_t *slot, uint8_t *addr, uint8_t *data, uint8_t *flags) {
    if (s_deferred_head == s_deferred_tail) return false;
    *slot = s_deferred_q[s_deferred_tail][0];
    *addr = s_deferred_q[s_deferred_tail][1];
    *data = s_deferred_q[s_deferred_tail][2];
    *flags = s_deferred_flags[s_deferred_tail];
    s_deferred_tail = (s_deferred_tail + 1) % DEFERRED_Q_SIZE;
    return true;
}

// ========== PIO Init ==========

static void pio_bus_init(void) {
    uint offset = pio_add_program(s_bus_pio, &spfm_bus_write_program);
    s_bus_sm = pio_claim_unused_sm(s_bus_pio, true);
    spfm_bus_write_program_init(s_bus_pio, s_bus_sm, offset, PIN_BUS_BASE);

    // AY8910 dedicated SM — same program, clock div=32 for ~512ns per word
    s_ay_sm = pio_claim_unused_sm(s_bus_pio, true);
    ay8910_sm_init(s_bus_pio, s_ay_sm, offset, PIN_BUS_BASE);
}

#define CS_IDLE 0x0F

static inline uint32_t cs_pack(uint8_t slot) {
    return CS_IDLE & ~(1u << slot);
}

static inline void pio_cs_put(PIO pio, uint sm, uint32_t val) {
    pio_sm_put_blocking(pio, sm, val);
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) {}
}

static void pio_cs_init(void) {
    uint offset = pio_add_program(s_cs_pio, &cs_out_program);
    s_cs_sm = pio_claim_unused_sm(s_cs_pio, true);
    cs_out_program_init(s_cs_pio, s_cs_sm, offset, PIN_CS_BASE);
    pio_cs_put(s_cs_pio, s_cs_sm, CS_IDLE);
}

static inline void cs_select(uint8_t slot) {
    pio_cs_put(s_cs_pio, s_cs_sm, cs_pack(slot));
}

// ========== WS2812 ==========

static void ws2812_update(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++)
        pio_sm_put_blocking(s_ws_pio, s_ws_sm, s_ws_leds[i] << 8u);
}

static inline void ws_led_off_all(void);

static void ws2812_init(void) {
    uint offset = pio_add_program(s_ws_pio, &ws2812_program);
    s_ws_sm = pio_claim_unused_sm(s_ws_pio, true);
    ws2812_program_init(s_ws_pio, s_ws_sm, offset, PIN_WS2812, 800000, false);
    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = 0;
    ws2812_update();
}

static inline void ws_led_on(uint8_t slot) {
    if (s_ws_leds[slot] == cs_colors[slot]) return;
    s_ws_leds[slot] = cs_colors[slot];
    ws2812_update();
    s_ws_off_time = make_timeout_time_ms(50);
}

static inline void ws_led_off_all(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = 0;
    ws2812_update();
}

// ========== Chip Writes ==========

static inline void write_reg_ym(uint8_t slot, uint8_t addr, uint8_t data) {
    cs_select(slot);
    if (s_ws_ready) ws_led_on(slot);
    ym2413_pio_write_reg(s_bus_pio, s_bus_sm, addr, data);
}

static inline void write_reg_ay(uint8_t slot, uint8_t addr, uint8_t data) {
    cs_select(slot);
    if (s_ws_ready) ws_led_on(slot);
    ay8910_pio_write_reg(s_bus_pio, s_ay_sm, addr, data);
}

// ========== IC# Reset ==========

static void bus_ic_reset(void) {
    gpio_put(PIN_IC, 0);
    absolute_time_t t = make_timeout_time_ms(50);
    while (!time_reached(t)) { tud_task(); tight_loop_contents(); }
    gpio_put(PIN_IC, 1);
    t = make_timeout_time_ms(50);
    while (!time_reached(t)) { tud_task(); tight_loop_contents(); }
}

// ========== GPIO Init ==========

static void gpio_init_all(void) {
    gpio_init(PIN_IC);
    gpio_set_dir(PIN_IC, GPIO_OUT);
    gpio_put(PIN_IC, 1);
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);
}

// ========== HID Callbacks ==========

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;

    if (bufsize < 4) return;

    uint8_t cmd = buffer[0];
    uint8_t seq = buffer[1];
    uint8_t len = buffer[2];

    if (len > 60 || bufsize < (uint16_t)(3 + len + 1)) return;

    // CRC check
    if (rpfm_crc8(buffer, 63) != buffer[63]) {
        s_status |= STATUS_ERROR;
        return;
    }

    s_last_seq = seq;
    s_status &= ~STATUS_ERROR;

    const uint8_t *payload = buffer + 3;

    switch (cmd) {
    case CMD_WRITE_REG: {
        for (uint8_t i = 0; i + 2 < len; i += 3) {
            deferred_push(payload[i], payload[i+1], payload[i+2], 0);
        }
        break;
    }
    case CMD_WRITE_AY: {
        for (uint8_t i = 0; i + 2 < len; i += 3) {
            deferred_push(payload[i], payload[i+1], payload[i+2], DQF_AY);
        }
        break;
    }
    case CMD_WRITE_TICK: {
        for (uint8_t i = 0; i < len; i++) {
            cmd_buf_write(&s_cmd, payload[i]);
        }
        break;
    }
    case CMD_VGM_DATA: {
        cmd_buf_write_buf(&s_cmd, payload, len);
        break;
    }
    case CMD_RESET: {
        if (len >= 1) {
            bus_ic_reset();
        }
        break;
    }
    case CMD_VGM_START: {
        // payload: [loop_offset_lo, loop_offset_hi] (relative to cmd_buf start)
        uint32_t loop_off = 0;
        if (len >= 2) loop_off = payload[0] | ((uint32_t)payload[1] << 8);
        vgm_player_start(loop_off);
        break;
    }
    case CMD_VGM_STOP: {
        vgm_player_stop();
        cmd_buf_init(&s_cmd);
        break;
    }
    case CMD_BOOTSEL: {
        reset_usb_boot(0, 0);
        break;
    }
    case CMD_SET_DELAY: {
        if (len >= 1 && payload[0] <= 200)
            s_ay_delay_100ns = payload[0];
        break;
    }
    case CMD_SET_MUTE: {
        if (len >= 2) {
            uint8_t mask = payload[0];
            int8_t solo = (int8_t)payload[1];
            vgm_player_set_mute(mask, solo);
        }
        break;
    }
    case CMD_NOP:
    default:
        break;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;

    if (reqlen < RPFM_FRAME_SIZE) return 0;

    memset(buffer, 0, RPFM_FRAME_SIZE);
    buffer[0] = s_last_seq;
    uint32_t used = cmd_buf_used(&s_cmd);
    if (used > (CMD_BUF_SIZE * 3 / 4))
        s_status |= STATUS_BUF_HIGH;
    else
        s_status &= ~STATUS_BUF_HIGH;
    buffer[1] = s_status;
    buffer[2] = (uint8_t)(used & 0xFF);
    buffer[3] = (uint8_t)((used >> 8) & 0xFF);
    // Tick: current playback sample position (32-bit LE)
    uint32_t tick = s_vgm_tick;
    buffer[4] = (uint8_t)(tick & 0xFF);
    buffer[5] = (uint8_t)((tick >> 8) & 0xFF);
    buffer[6] = (uint8_t)((tick >> 16) & 0xFF);
    buffer[7] = (uint8_t)((tick >> 24) & 0xFF);

    return RPFM_FRAME_SIZE;
}

// ========== LED ==========

static bool s_led_state = false;
static absolute_time_t s_led_next = {0};

static void led_update(void) {
    if (time_reached(s_led_next)) {
        s_led_state = !s_led_state;
        gpio_put(PIN_LED, s_led_state);
        s_led_next = make_timeout_time_ms(500);
    }
}

// ========== Main ==========

int main() {
    stdio_init_all();

    gpio_init_all();
    pio_bus_init();
    pio_cs_init();
    ws2812_init();
    cmd_buf_init(&s_cmd);
    vgm_player_init(&s_cmd, s_bus_pio, s_ay_sm);

    // Init TinyUSB device stack
    tud_init(BOARD_TUD_RHPORT);

    // Launch VGM player on Core 1 (MegaGRRL-style cycle counter loop)
    multicore_launch_core1(core1_vgm_main);

    s_ws_ready = true;
    cs_select(0);
    bus_ic_reset();

    s_led_next = make_timeout_time_ms(500);

    while (true) {
        tud_task();
        led_update();

        // Execute deferred register writes
        uint8_t slot, addr, data, flags;
        while (deferred_pop(&slot, &addr, &data, &flags)) {
            if (flags & DQF_AY)
                write_reg_ay(slot, addr, data);
            else
                write_reg_ym(slot, addr, data);
        }

        // WS2812 auto-off after timeout
        if (s_ws_off_time && time_reached(s_ws_off_time)) {
            s_ws_off_time = (absolute_time_t){0};
            ws_led_off_all();
        }
    }

    return 0;
}
