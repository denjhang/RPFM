/*
 * RPFM YM2413 Breadboard Test
 * RP2350A -> PIO -> SPFM bus -> YM2413 OPLL
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
 *     GND      -> GND     (must be connected!)
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/bootrom.h"
#include "ym2413.pio.h"
#include "pio_cs.pio.h"
#include "ws2812.pio.h"
#include "tusb.h"

// ========== Pin Definitions ==========

// PIO0 SM0 (14 consecutive pins): D0-D7 + WR# + RD# + A0-A3
#define PIN_BUS_BASE  0     // GPIO0-13

// PIO1 SM1 (4 consecutive pins): CS0-CS3
#define PIN_CS_BASE   17    // GPIO17-20

// CPU-controlled:
#define PIN_IC   21
#define PIN_LED  25
#define PIN_WS2812 16

// WS2812
#define NUM_WS_LEDS  4
#define WS_FREQ      8000000

// ========== YM2413 Constants ==========

#define YM2413_CLOCK    3579545.0
#define YM2413_FM_CLOCK (YM2413_CLOCK / 72.0)

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

// CS LED colors (urgb format, half brightness): CS0=green, CS1=blue, CS2=yellow, CS3=red
static const uint32_t cs_colors[NUM_WS_LEDS] = {
    0x006600, // dim green
    0x000066, // dim blue
    0x666600, // dim yellow
    0x660000, // dim red
};

// ========== PIO Bus Driver (14-bit) ==========

static void pio_bus_init(void) {
    printf("  Bus PIO: adding program...\n");
    uint offset = pio_add_program(s_bus_pio, &ym2413_write_program);
    printf("  Bus PIO: program loaded at offset %u\n", offset);
    s_bus_sm = pio_claim_unused_sm(s_bus_pio, true);
    printf("  Bus PIO: claimed SM %u\n", s_bus_sm);
    ym2413_write_program_init(s_bus_pio, s_bus_sm, offset, PIN_BUS_BASE);
    printf("  Bus PIO: SM initialized, enabled\n");
}

// ========== PIO CS Driver (4-bit) ==========

// CS word format (4-bit): bit[0]=CS0#, bit[1]=CS1#, bit[2]=CS2#, bit[3]=CS3#
// 1=idle (high), 0=selected (low)
#define CS_IDLE 0x0F

static inline uint32_t cs_pack(uint8_t slot) {
    return CS_IDLE & ~(1u << slot);
}

static inline void pio_cs_put(PIO pio, uint sm, uint32_t val) {
    pio_sm_put_blocking(pio, sm, val);
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) {}
}

static void pio_cs_init(void) {
    printf("  CS PIO: adding program...\n");
    uint offset = pio_add_program(s_cs_pio, &cs_out_program);
    printf("  CS PIO: program loaded at offset %u\n", offset);
    s_cs_sm = pio_claim_unused_sm(s_cs_pio, true);
    printf("  CS PIO: claimed SM %u\n", s_cs_sm);
    cs_out_program_init(s_cs_pio, s_cs_sm, offset, PIN_CS_BASE);
    printf("  CS PIO: SM initialized, enabled\n");

    // Push idle state (all CS high)
    pio_cs_put(s_cs_pio, s_cs_sm, CS_IDLE);
}

static inline void cs_select(uint8_t slot) {
    pio_cs_put(s_cs_pio, s_cs_sm, cs_pack(slot));
}

static inline void cs_deselect_all(void) {
    pio_cs_put(s_cs_pio, s_cs_sm, CS_IDLE);
}

// ========== WS2812 LED Driver ==========

static inline void ws_led_off_all(void);

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
}

static void ws2812_init(void) {
    printf("  WS2812: adding program...\n");
    uint offset = pio_add_program(s_ws_pio, &ws2812_program);
    printf("  WS2812: program loaded at offset %u\n", offset);
    s_ws_sm = pio_claim_unused_sm(s_ws_pio, true);
    printf("  WS2812: claimed SM %u\n", s_ws_sm);
    ws2812_program_init(s_ws_pio, s_ws_sm, offset, PIN_WS2812, 800000, false);
    printf("  WS2812: SM initialized, enabled\n");
    ws_led_off_all();
}

// Send all 4 pixels to WS2812 (must always send complete frame)
static void ws2812_update(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++) {
        pio_sm_put_blocking(s_ws_pio, s_ws_sm, s_ws_leds[i] << 8u);
    }
}

static inline void ws_led_on(uint8_t slot) {
    s_ws_leds[slot] = cs_colors[slot];
    ws2812_update();
    s_ws_led_pending = true;
}

static inline void ws_led_off_all(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = 0;
    ws2812_update();
}

// ========== YM2413 Write (bus + CS combined) ==========

static inline void ym2413_write_reg(uint8_t reg, uint8_t data) {
    // CS0# is slot 0, flash WS2812 LED
    if (s_ws_ready) {
        ws_led_on(0);
    }
    ym2413_pio_write_reg(s_bus_pio, s_bus_sm, reg, data);
}

// ========== YM2413 Control Functions ==========

static void ym2413_reset(void) {
    gpio_put(PIN_IC, 0);
    sleep_ms(1);
    gpio_put(PIN_IC, 1);
    sleep_ms(10);
}

static void ym2413_mute_all(void) {
    for (int ch = 0; ch < 9; ch++) {
        ym2413_write_reg(0x20 + ch, 0x00);
    }
    for (int ch = 0; ch < 9; ch++) {
        ym2413_write_reg(0x30 + ch, 0x0F);
    }
    ym2413_write_reg(0x0E, 0x00);
}

// ========== MIDI Note Conversion ==========

static void midi_to_ym2413(uint8_t midi_note, uint8_t *block, uint16_t *fnumber) {
    double freq = 440.0 * pow(2.0, (midi_note - 69.0) / 12.0);
    for (int b = 0; b <= 7; b++) {
        double fn = freq * (double)(1 << (20 - b)) / YM2413_FM_CLOCK;
        if (fn >= 0.0 && fn < 1024.0) {
            *block = (uint8_t)b;
            *fnumber = (uint16_t)round(fn);
            return;
        }
    }
    *block = 0;
    *fnumber = 0;
}

// ========== Note Playing ==========

static void ym2413_note_on(uint8_t channel, uint8_t instrument, uint8_t volume, uint8_t midi_note) {
    uint8_t blk;
    uint16_t fn;
    midi_to_ym2413(midi_note, &blk, &fn);

    ym2413_write_reg(0x30 + channel, ((instrument & 0x0F) << 4) | (volume & 0x0F));
    ym2413_write_reg(0x10 + channel, fn & 0xFF);
    ym2413_write_reg(0x20 + channel, (blk << 1) | ((fn >> 8) & 1));
    sleep_us(50);
    ym2413_write_reg(0x20 + channel, 0x10 | (blk << 1) | ((fn >> 8) & 1));
}

static void ym2413_note_off(uint8_t channel) {
    ym2413_write_reg(0x20 + channel, 0x00);
}

// ========== Serial Command ==========

static char s_cmd_buf[32];
static int s_cmd_pos = 0;

static bool check_serial_command(void) {
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT) return false;
    if (ch == '\r' || ch == '\n') {
        s_cmd_buf[s_cmd_pos] = '\0';
        if (strcmp(s_cmd_buf, "BOOTSEL") == 0) {
            printf("Entering BOOTSEL mode...\n");
            rom_reset_usb_boot(0, 0);
        }
        s_cmd_pos = 0;
    } else if (s_cmd_pos < (int)sizeof(s_cmd_buf) - 1) {
        s_cmd_buf[s_cmd_pos++] = (char)ch;
    }
    return true;
}

// ========== Demo Melodies ==========

static const uint8_t melody_notes[] = {
    60, 60, 67, 67, 69, 69, 67, 0,
    65, 65, 64, 64, 62, 62, 60, 0,
    67, 67, 65, 65, 64, 64, 62, 0,
    67, 67, 65, 65, 64, 64, 62, 0,
    60, 60, 67, 67, 69, 69, 67, 0,
    65, 65, 64, 64, 62, 62, 60, 0,
};
static const uint8_t melody_dur[] = {
    1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 1, 1, 1, 2, 1,
};

#define MELODY_LEN (sizeof(melody_notes) / sizeof(melody_notes[0]))
#define QUARTER_MS 350

static void play_melody(void) {
    for (int i = 0; i < MELODY_LEN; i++) {
        if (melody_notes[i] == 0) {
            sleep_ms(QUARTER_MS);
        } else {
            ym2413_note_on(0, 0x03, 0x00, melody_notes[i]);
            sleep_ms(QUARTER_MS * melody_dur[i]);
            ym2413_note_off(0);
            sleep_ms(20);
        }
        check_serial_command();
    }
}

static void play_test_tone(void) {
    printf("Test tone: Organ C4 (261.63 Hz) 2s\n");
    ym2413_note_on(0, 0x08, 0x00, 60);
    sleep_ms(2000);
    ym2413_note_off(0);
    sleep_ms(500);
    check_serial_command();
}

static void play_scale(void) {
    printf("C major scale\n");
    static const uint8_t scale[] = {60, 62, 64, 65, 67, 69, 71, 72};
    for (int i = 0; i < 8; i++) {
        ym2413_note_on(0, 0x08, 0x00, scale[i]);
        sleep_ms(400);
        ym2413_note_off(0);
        sleep_ms(30);
        check_serial_command();
    }
    sleep_ms(500);
}

static void play_rhythm_demo(void) {
    /*
     * YM2413 rhythm channels (register 0x0E):
     *   bit 5: rhythm enable (1=on)
     *   bit 4: Hi-Hat
     *   bit 3: Top Cymbal / Top Tom
     *   bit 2: Tom Tom
     *   bit 1: Snare Drum
     *   bit 0: Bass Drum
     *
     * When rhythm is enabled, melodic channels 6-8 become
     * rhythm sound channels. Rhythm sounds use custom instruments.
     *
     * Rhythm volume: register 0x37 low nibble controls
     * HiHat/TopCym/TomTom volume.
     * Register 0x38 low nibble controls Snare/Bass volume.
     */
    static const char *rhythm_names[] = {
        "Bass Drum", "Snare Drum", "Tom Tom", "Top Cym", "Hi-Hat"
    };

    // Enable rhythm mode with all bits
    ym2413_write_reg(0x0E, 0x3F);
    sleep_ms(100);

    // Set rhythm volume (0x37, 0x38) - max volume (0x00)
    ym2413_write_reg(0x37, 0x00);
    ym2413_write_reg(0x38, 0x00);
    sleep_ms(100);

    // Play each rhythm sound one at a time
    for (int i = 0; i < 5; i++) {
        uint8_t bit = (i == 4) ? 0x10 : (i == 3) ? 0x04 : (i == 2) ? 0x02 : (i == 1) ? 0x08 : 0x01;
        printf("  Rhythm: %s (bit 0x%02X)\n", rhythm_names[i], bit);
        ym2413_write_reg(0x0E, 0x20 | bit);
        sleep_ms(400);
        ym2413_write_reg(0x0E, 0x20); // silence
        sleep_ms(100);
    }

    // Play all rhythms together for 1 second
    printf("  All rhythms together\n");
    ym2413_write_reg(0x0E, 0x3F);
    sleep_ms(1000);
    ym2413_write_reg(0x0E, 0x20); // silence

    // Disable rhythm mode
    ym2413_write_reg(0x0E, 0x00);
    sleep_ms(100);
}

static void play_instrument_demo(void) {
    printf("Instrument demo: cycling all 16 instruments\n");
    static const char *inst_names[] = {
        "Custom", "Violin", "Guitar", "Piano",
        "Flute", "Clarinet", "Oboe", "Trumpet",
        "Organ", "Horn", "Synth", "Harp",
        "Vibraphone", "SynthBass", "AcBass", "ElecBass"
    };
    for (int i = 0; i < 16; i++) {
        printf("  Inst %d: %s\n", i, inst_names[i]);
        ym2413_note_on(0, i, 0x00, 60);
        sleep_ms(600);
        ym2413_note_off(0);
        sleep_ms(100);
        check_serial_command();
    }
}

// ========== GPIO Initialization ==========

static void gpio_init_all(void) {
    // IC# reset pin (CPU controlled, power-on reset only)
    gpio_init(PIN_IC);
    gpio_set_dir(PIN_IC, GPIO_OUT);
    gpio_put(PIN_IC, 1);

    // LED heartbeat
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);

    // GPIO0-13 (D0-D7, WR#, RD#, A0-A3) initialized by PIO0
    // GPIO17-20 (CS0-CS3) initialized by PIO1
}

// ========== Main ==========

int main() {
    stdio_init_all();
    sleep_ms(2000);

    printf("=== RPFM YM2413 Test ===\n");
    printf("Initializing GPIO...\n");
    gpio_init_all();

    printf("Initializing PIO bus (14-bit)...\n");
    pio_bus_init();

    printf("Initializing PIO CS (4-bit)...\n");
    pio_cs_init();

    printf("Initializing WS2812...\n");
    ws2812_init();
    s_ws_ready = true;

    // WS2812 startup: dim rainbow chase then off
    static const uint32_t rainbow[] = {
        0x100000, // dim red
        0x101000, // dim yellow
        0x001000, // dim green
        0x001010, // dim cyan
        0x000010, // dim blue
        0x100010, // dim purple
    };
    for (int c = 0; c < 6; c++) {
        for (int i = 0; i < NUM_WS_LEDS; i++) {
            int idx = (c + i) % 6;
            s_ws_leds[i] = rainbow[idx];
        }
        ws2812_update();
        sleep_ms(80);
    }
    ws_led_off_all();

    // Enable WS2812 CS indicator
    s_ws_ready = true;

    for (int i = 0; i < 3; i++) {
        gpio_put(PIN_LED, 1);
        sleep_ms(150);
        gpio_put(PIN_LED, 0);
        sleep_ms(150);
    }

    printf("Ready. Waiting for SCCI...\n");

    // SPFM protocol state — ym2151 style
    uint16_t uart_idle_cnt = 10000;
    uint8_t scci_parse_idx = 0;
    uint8_t scci_slot = 0;
    uint8_t scci_cmd = 0;
    uint8_t scci_a = 0;
    uint8_t scci_addr = 0;

    // LED heartbeat
    bool led_state = false;
    absolute_time_t led_next = make_timeout_time_ms(500);
    bool led_data_active = false;
    absolute_time_t led_data_timeout = nil_time;

    // WS2812 CS activity LED timeout
    absolute_time_t ws_led_timeout = nil_time;
    bool ws_led_timed = false;

    while (true) {
        // Frame sync timeout
        if (--uart_idle_cnt == 0) {
            scci_parse_idx = 0;
            uart_idle_cnt = 10000;
        }

        // LED: data blink (1ms per byte) or slow heartbeat when idle
        if (led_data_active) {
            if (time_reached(led_data_timeout)) {
                led_data_active = false;
                gpio_put(PIN_LED, 0);
            }
        } else {
            if (time_reached(led_next)) {
                led_state = !led_state;
                gpio_put(PIN_LED, led_state);
                led_next = make_timeout_time_ms(500);
            }
        }

        // WS2812 CS LED: auto off after 1ms
        if (s_ws_led_pending) {
            s_ws_led_pending = false;
            ws_led_timeout = make_timeout_time_ms(10);
            ws_led_timed = true;
        } else if (ws_led_timed && time_reached(ws_led_timeout)) {
            ws_led_off_all();
            ws_led_timed = false;
        }

        // Read one byte
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) continue;

        uint8_t uart_data = (uint8_t)ch;
        uart_idle_cnt = 10000;
        // LED: instant 1ms flash per byte
        led_data_active = true;
        led_data_timeout = make_timeout_time_ms(1);
        gpio_put(PIN_LED, 1);

        // Protocol parser — ym2151 style
        if (scci_parse_idx == 0) {
            if (uart_data == 0xFF) {
                tud_cdc_write("RS", 2);
                tud_cdc_write_flush();
                // WS2812 handshake: short blink x2
                for (int n = 0; n < 2; n++) {
                    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = cs_colors[i];
                    ws2812_update();
                    sleep_ms(50);
                    ws_led_off_all();
                    sleep_ms(50);
                }
                led_data_active = true;
                led_data_timeout = make_timeout_time_ms(1);
                gpio_put(PIN_LED, 1);
            } else if (uart_data == 0xFE) {
                ym2413_reset();
                ym2413_mute_all();
                tud_cdc_write("OK", 2);
                tud_cdc_write_flush();
                for (int n = 0; n < 2; n++) {
                    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = cs_colors[i];
                    ws2812_update();
                    sleep_ms(50);
                    ws_led_off_all();
                    sleep_ms(50);
                }
                led_data_active = true;
                led_data_timeout = make_timeout_time_ms(1);
                gpio_put(PIN_LED, 1);
            } else if ((uart_data & 0xF0) == 0x00) {
                scci_slot = uart_data & 0x0F;
                scci_parse_idx = 1;
            }
        }
        else if (scci_parse_idx == 1) {
            scci_cmd = uart_data & 0xF0;
            if (scci_cmd == 0x00 || scci_cmd == 0x80) {
                scci_a = uart_data & 0x0F;
                scci_parse_idx = 2;
            } else if (scci_cmd == 0x20) {
                scci_parse_idx = 2;
            } else {
                scci_parse_idx = 0;
            }
        }
        else if (scci_parse_idx == 2) {
            if (scci_cmd == 0x00) {
                scci_addr = uart_data;  // cache register address
                scci_parse_idx = 3;
            } else if (scci_cmd == 0x80) {
                ym2413_write_reg(scci_a, uart_data);
                scci_parse_idx = 0;
            } else if (scci_cmd == 0x20) {
                scci_parse_idx = 0;
            } else {
                scci_parse_idx = 0;
            }
        }
        else if (scci_parse_idx == 3) {
            if (scci_cmd == 0x00) {
                ym2413_write_reg(scci_addr, uart_data);
            }
            scci_parse_idx = 0;
        }
    }

    return 0;
}
