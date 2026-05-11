/*
 * RPFM YM2413 Breadboard Test
 * RP2350A -> PIO -> SPFM bus -> YM2413 OPLL
 *
 * Hardware wiring (RP2350A GPIO -> SPFM sound card module):
 *
 *   PIO-controlled (9 consecutive pins, GPIO0-8):
 *     GPIO0-7  -> D0-D7   (data bus)
 *     GPIO8    -> WR#     (write strobe, active low)
 *
 *   CPU-controlled:
 *     GPIO9    -> A0      (address/data select)
 *     GPIO10   -> CS0     (chip select, active low)
 *     GPIO11   -> IC      (reset, active low)
 *     GPIO12   -> A1      (unused by YM2413, SPFM bus compat)
 *     GPIO13   -> A2      (unused by YM2413)
 *     GPIO14   -> A3      (unused by YM2413)
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
#include "tusb.h"

// ========== Pin Definitions ==========

// PIO-controlled (9 consecutive pins):
#define PIN_BUS_BASE  0     // GPIO0-8 = D0-D7 + WR#

// CPU-controlled:
#define PIN_A0   9
#define PIN_CS0  10
#define PIN_IC   11
#define PIN_A1   12
#define PIN_A2   13
#define PIN_A3   14
#define PIN_LED  25

// ========== YM2413 Constants ==========

#define YM2413_CLOCK    3579545.0
#define YM2413_FM_CLOCK (YM2413_CLOCK / 72.0)

// ========== Global PIO State ==========

static PIO s_pio = pio0;
static uint s_sm = 0;
static bool s_use_pio = true;

// ========== Bitbang YM2413 Driver (fallback for debugging) ==========

static inline void data_bus_write(uint8_t val) {
    gpio_put_masked(0xFF, (uint32_t)val);
}

void ym2413_write_reg_bitbang(uint8_t reg, uint8_t data) {
    gpio_put(PIN_CS0, 0);

    // Address phase: A0=0
    gpio_put(PIN_A0, 0);
    data_bus_write(reg);
    sleep_us(1);
    gpio_put(PIN_BUS_BASE + 8, 0);  // WR# low
    sleep_us(1);
    gpio_put(PIN_BUS_BASE + 8, 1);  // WR# high
    sleep_us(2);

    // Data phase: A0=1
    gpio_put(PIN_A0, 1);
    data_bus_write(data);
    sleep_us(1);
    gpio_put(PIN_BUS_BASE + 8, 0);  // WR# low
    sleep_us(1);
    gpio_put(PIN_BUS_BASE + 8, 1);  // WR# high
    sleep_us(2);

    gpio_put(PIN_CS0, 1);
}

// ========== PIO YM2413 Driver ==========

static void pio_ym2413_init(void) {
    printf("  PIO: adding program...\n");
    uint offset = pio_add_program(s_pio, &ym2413_write_program);
    printf("  PIO: program loaded at offset %u\n", offset);
    s_sm = pio_claim_unused_sm(s_pio, true);
    printf("  PIO: claimed SM %u\n", s_sm);
    ym2413_write_program_init(s_pio, s_sm, offset, PIN_BUS_BASE);
    printf("  PIO: SM initialized, enabled\n");
}

void ym2413_write_reg_pio(uint8_t reg, uint8_t data) {
    ym2413_pio_write_reg(s_pio, s_sm, PIN_A0, PIN_CS0, reg, data);
}

// ========== Unified Write Function ==========

static inline void ym2413_write_reg(uint8_t reg, uint8_t data) {
    if (s_use_pio) {
        ym2413_write_reg_pio(reg, data);
    } else {
        ym2413_write_reg_bitbang(reg, data);
    }
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
    // CPU-controlled pins
    gpio_init(PIN_A0);
    gpio_set_dir(PIN_A0, GPIO_OUT);
    gpio_put(PIN_A0, 0);

    gpio_init(PIN_CS0);
    gpio_set_dir(PIN_CS0, GPIO_OUT);
    gpio_put(PIN_CS0, 1);

    gpio_init(PIN_IC);
    gpio_set_dir(PIN_IC, GPIO_OUT);
    gpio_put(PIN_IC, 1);

    for (int i = PIN_A1; i <= PIN_A3; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_OUT);
        gpio_put(i, 0);
    }

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);

    // GPIO0-8 (D0-D7, WR) are initialized by PIO in pio_ym2413_init()
}

// ========== Main ==========

int main() {
    stdio_init_all();
    sleep_ms(2000);

    printf("=== RPFM YM2413 Test ===\n");
    printf("Initializing GPIO...\n");
    gpio_init_all();

    // Initialize data bus + WR# pins
    // PIO mode: these pins are initialized by pio_ym2413_init() - do NOT re-init here
    // Bitbang mode: we need to init them as GPIO outputs
    if (!s_use_pio) {
        for (int i = PIN_BUS_BASE; i < PIN_BUS_BASE + 9; i++) {
            gpio_init(i);
            gpio_set_dir(i, GPIO_OUT);
            gpio_put(i, 0);
        }
    }

    if (s_use_pio) {
        printf("Initializing PIO...\n");
        pio_ym2413_init();
    } else {
        printf("Using bitbang driver\n");
    }

    printf("Resetting YM2413...\n");
    ym2413_reset();
    ym2413_mute_all();

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

    // LED heartbeat
    bool led_state = false;
    absolute_time_t led_next = make_timeout_time_ms(500);

    while (true) {
        // Frame sync timeout
        if (--uart_idle_cnt == 0) {
            scci_parse_idx = 0;
            uart_idle_cnt = 10000;
        }

        // LED heartbeat — always runs
        if (time_reached(led_next)) {
            led_state = !led_state;
            gpio_put(PIN_LED, led_state);
            led_next = make_timeout_time_ms(500);
        }

        // Read one byte
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) continue;

        uint8_t uart_data = (uint8_t)ch;
        uart_idle_cnt = 10000;

        // Protocol parser — ym2151 style
        if (scci_parse_idx == 0) {
            if (uart_data == 0xFF) {
                tud_cdc_write("RS", 2);
                tud_cdc_write_flush();
            } else if (uart_data == 0xFE) {
                ym2413_reset();
                ym2413_mute_all();
                tud_cdc_write("OK", 2);
                tud_cdc_write_flush();
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
                ym2413_write_reg(scci_a & 0xFE, uart_data);
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
                ym2413_write_reg(scci_a | 0x01, uart_data);
            }
            scci_parse_idx = 0;
        }
    }

    return 0;
}
