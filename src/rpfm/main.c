/*
 * RPFM Protocol Firmware
 * RP2350A -> PIO -> SPFM bus -> YM2413/YM2151/AY8910/...
 *
 * Custom protocol: framed packets with checksum, flow control.
 * Replaces SCCI byte-by-byte passthrough to solve USB CDC data loss.
 *
 * Hardware wiring (RP2350A GPIO -> SPFM sound card module):
 *
 *   PIO0 SM0 (14 consecutive pins, GPIO0-13):
 *     GPIO0-7  -> D0-D7   (data bus)
 *     GPIO8    -> WR#     (write strobe, active low)
 *     GPIO9    -> RD#     (read strobe, active low)
 *     GPIO10   -> A0      (address/data select)
 *     GPIO11   -> A1
 *     GPIO12   -> A2
 *     GPIO13   -> A3
 *
 *   PIO1 SM1 (4 consecutive pins, GPIO17-20):
 *     GPIO17   -> CS0#    (chip select 0, active low)
 *     GPIO18   -> CS1#    (chip select 1, active low)
 *     GPIO19   -> CS2#    (chip select 2, active low)
 *     GPIO20   -> CS3#    (chip select 3, active low)
 *
 *   CPU-controlled:
 *     GPIO21   -> IC#     (reset, active low, power-on only)
 *     GPIO25   -> LED     (heartbeat)
 *     GPIO16   -> WS2812  (4x RGB, CS activity indicator)
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/bootrom.h"
#include "ym2413.pio.h"
#include "pio_cs.pio.h"
#include "ws2812.pio.h"
#include "tusb.h"

// ========== Pin Definitions ==========

#define PIN_BUS_BASE  0     // GPIO0-13
#define PIN_CS_BASE   17    // GPIO17-20
#define PIN_IC        21
#define PIN_LED       25
#define PIN_WS2812    16

#define NUM_WS_LEDS   4

// ========== Global PIO State ==========

static PIO s_bus_pio = pio0;
static uint s_bus_sm = 0;
static PIO s_cs_pio = pio1;
static uint s_cs_sm = 0;

// WS2812
static PIO s_ws_pio = pio1;
static uint s_ws_sm = 0;
static bool s_ws_ready = false;
static bool s_ws_led_pending = false;
static uint32_t s_ws_leds[NUM_WS_LEDS] = {0};

static const uint32_t cs_colors[NUM_WS_LEDS] = {
    0x006600, 0x000066, 0x666600, 0x660000,
};

// ========== PIO Bus Driver ==========

static void pio_bus_init(void) {
    uint offset = pio_add_program(s_bus_pio, &ym2413_write_program);
    s_bus_sm = pio_claim_unused_sm(s_bus_pio, true);
    ym2413_write_program_init(s_bus_pio, s_bus_sm, offset, PIN_BUS_BASE);
}

// ========== PIO CS Driver ==========

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

static inline void cs_deselect_all(void) {
    pio_cs_put(s_cs_pio, s_cs_sm, CS_IDLE);
}

// ========== WS2812 LED ==========

static void ws2812_update(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++) {
        pio_sm_put_blocking(s_ws_pio, s_ws_sm, s_ws_leds[i] << 8u);
    }
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
    s_ws_led_pending = true;
}

static inline void ws_led_off_all(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = 0;
    ws2812_update();
}

// ========== Chip Write Functions ==========

static inline void ym2413_write_reg(uint8_t reg, uint8_t data) {
    if (s_ws_ready) ws_led_on(0);
    ym2413_pio_write_reg(s_bus_pio, s_bus_sm, reg, data);
}

static inline void ay8910_write_reg(uint8_t reg, uint8_t data) {
    PIO pio = s_bus_pio;
    uint sm = s_bus_sm;
    pio_bus_put(pio, sm, pack_bus(reg, 0, true, true));
    pio_bus_put(pio, sm, pack_bus(reg, 0, false, true));
    busy_wait_us_32(5);
    pio_bus_put(pio, sm, pack_bus(reg, 0, true, true));
    pio_bus_put(pio, sm, pack_bus(data, 1, true, true));
    pio_bus_put(pio, sm, pack_bus(data, 1, false, true));
    busy_wait_us_32(5);
    pio_bus_put(pio, sm, pack_bus(data, 1, true, true));
    pio_bus_put(pio, sm, BUS_IDLE);
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

// ========== RPFM Protocol ==========

// TODO: define RPFM protocol packet format

// ========== Main ==========

int main() {
    stdio_init_all();
    sleep_ms(2000);

    printf("=== RPFM Firmware ===\n");

    gpio_init_all();
    pio_bus_init();
    pio_cs_init();
    ws2812_init();

    s_ws_ready = true;

    cs_select(0);
    bus_ic_reset();

    // Startup LED blink
    for (int i = 0; i < 3; i++) {
        gpio_put(PIN_LED, 1); sleep_ms(150);
        gpio_put(PIN_LED, 0); sleep_ms(150);
    }

    printf("Ready.\n");

    while (true) {
        tud_task();
        tight_loop_contents();
    }

    return 0;
}
