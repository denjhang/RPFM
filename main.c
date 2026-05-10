/*
 * RPFM YM2413 PIO Debug Version
 * RP2350A -> PIO -> SPFM bus -> YM2413 OPLL
 *
 * Debug output via USB CDC (USB serial)
 */

#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "tusb.h"
#include "ym2413.pio.h"

// ========== Pin Definitions ==========

#define PIN_BUS_BASE  0     // GPIO0-8 = D0-D7 + WR#
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

// ========== Bitbang YM2413 Driver ==========

static inline void data_bus_write(uint8_t val) {
    gpio_put_masked(0xFF, (uint32_t)val);
}

void ym2413_write_reg_bitbang(uint8_t reg, uint8_t data) {
    gpio_put(PIN_CS0, 0);
    gpio_put(PIN_A0, 0);
    data_bus_write(reg);
    sleep_us(1);
    gpio_put(PIN_BUS_BASE + 8, 0);
    sleep_us(1);
    gpio_put(PIN_BUS_BASE + 8, 1);
    sleep_us(2);
    gpio_put(PIN_A0, 1);
    data_bus_write(data);
    sleep_us(1);
    gpio_put(PIN_BUS_BASE + 8, 0);
    sleep_us(1);
    gpio_put(PIN_BUS_BASE + 8, 1);
    sleep_us(2);
    gpio_put(PIN_CS0, 1);
}

// ========== PIO Debug Functions ==========

static void pio_dump_sm(PIO pio, uint sm) {
    printf("  SM%d regs:\n", sm);
    printf("    CLKDIV    = 0x%08x\n", pio->sm[sm].clkdiv);
    printf("    EXECCTRL  = 0x%08x\n", pio->sm[sm].execctrl);
    printf("    SHIFTCTRL = 0x%08x\n", pio->sm[sm].shiftctrl);
    printf("    ADDR      = %d\n", pio_sm_get_pc(pio, sm));
    printf("    INSTR     = 0x%04x\n", pio->sm[sm].instr);

    uint32_t flevel = (pio->flevel >> (sm * 4)) & 0xf;
    printf("    TX_FIFO   = %d/4\n", flevel);

    uint32_t pin_vals = 0;
    for (int i = PIN_BUS_BASE; i < PIN_BUS_BASE + 9; i++) {
        pin_vals |= (gpio_get(i) << (i - PIN_BUS_BASE));
    }
    printf("    GPIO0-8   = 0x%03x (D=0x%02x WR#=%d)\n",
           pin_vals, pin_vals & 0xFF, (pin_vals >> 8) & 1);

    printf("    DBG_PADOUT= 0x%03x\n", pio->sm[sm].pinctrl);
}

static void pio_test_single_out(void) {
    // Test: put one 32-bit word, verify PIO actually drives pins
    PIO pio = pio0;
    uint sm = 0;

    printf("[PIO TEST] Single out test\n");
    printf("  Before put: TX_FIFO full=%d\n",
           pio->flevel & 0xf);

    // pack_phase(0x55) = data=0x55 on bus, with WR# pulse
    uint32_t word = pack_phase(0x55);
    printf("  pack_phase(0x55) = 0x%08x\n", word);
    printf("    bit[8:0]   = 0x%03x (expect 0x155 = D:0x55 WR#:1)\n", word & 0x1FF);
    printf("    bit[17:9]  = 0x%03x (expect 0x055 = D:0x55 WR#:0)\n", (word >> 9) & 0x1FF);
    printf("    bit[26:18] = 0x%03x (expect 0x155 = D:0x55 WR#:1)\n", (word >> 18) & 0x1FF);

    pio_sm_put_blocking(pio, sm, word);
    printf("  After put: TX_FIFO level=%d\n",
           (pio->flevel >> (sm * 4)) & 0xf);

    sleep_us(10);

    pio_dump_sm(pio, sm);

    // Wait for PIO to finish executing
    sleep_us(100);
    printf("  After 100us:\n");
    pio_dump_sm(pio, sm);
}

static void pio_test_write_reg(uint8_t reg, uint8_t data) {
    PIO pio = pio0;
    uint sm = 0;

    printf("[PIO TEST] Write reg=0x%02x data=0x%02x\n", reg, data);

    gpio_put(PIN_CS0, 0);
    printf("  CS#=0, A0=0\n");

    gpio_put(PIN_A0, 0);
    sleep_us(1);

    // Address phase
    pio_sm_put_blocking(pio, sm, pack_phase(reg));
    printf("  Addr phase pushed, waiting FIFO...\n");
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) {
        tight_loop_contents();
    }
    printf("  Addr phase done\n");
    pio_dump_sm(pio, sm);
    sleep_us(5);

    // Data phase
    gpio_put(PIN_A0, 1);
    printf("  A0=1\n");

    pio_sm_put_blocking(pio, sm, pack_phase(data));
    printf("  Data phase pushed, waiting FIFO...\n");
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) {
        tight_loop_contents();
    }
    printf("  Data phase done\n");
    pio_dump_sm(pio, sm);

    sleep_us(1);
    gpio_put(PIN_CS0, 1);
    printf("  CS#=1\n");
}

// ========== YM2413 Control ==========

static void ym2413_reset(void) {
    gpio_put(PIN_IC, 0);
    sleep_ms(1);
    gpio_put(PIN_IC, 1);
    sleep_ms(10);
}

static void ym2413_mute_all_bitbang(void) {
    for (int ch = 0; ch < 9; ch++) {
        ym2413_write_reg_bitbang(0x20 + ch, 0x00);
    }
    for (int ch = 0; ch < 9; ch++) {
        ym2413_write_reg_bitbang(0x30 + ch, 0x0F);
    }
    ym2413_write_reg_bitbang(0x0E, 0x00);
}

static void ym2413_mute_all_pio(void) {
    PIO pio = pio0;
    uint sm = 0;
    for (int ch = 0; ch < 9; ch++) {
        pio_test_write_reg(0x20 + ch, 0x00);
    }
    for (int ch = 0; ch < 9; ch++) {
        pio_test_write_reg(0x30 + ch, 0x0F);
    }
    pio_test_write_reg(0x0E, 0x00);
}

// ========== Note Playing ==========

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

static void ym2413_note_on_bitbang(uint8_t channel, uint8_t instrument, uint8_t volume, uint8_t midi_note) {
    uint8_t blk;
    uint16_t fn;
    midi_to_ym2413(midi_note, &blk, &fn);
    ym2413_write_reg_bitbang(0x30 + channel, ((instrument & 0x0F) << 4) | (volume & 0x0F));
    ym2413_write_reg_bitbang(0x10 + channel, fn & 0xFF);
    ym2413_write_reg_bitbang(0x20 + channel, (blk << 1) | ((fn >> 8) & 1));
    sleep_us(50);
    ym2413_write_reg_bitbang(0x20 + channel, 0x10 | (blk << 1) | ((fn >> 8) & 1));
}

static void ym2413_note_on_pio(uint8_t channel, uint8_t instrument, uint8_t volume, uint8_t midi_note) {
    uint8_t blk;
    uint16_t fn;
    midi_to_ym2413(midi_note, &blk, &fn);
    printf("[NOTE ON] ch=%d inst=%d vol=%d note=%d blk=%d fn=%d\n",
           channel, instrument, volume, midi_note, blk, fn);
    pio_test_write_reg(0x30 + channel, ((instrument & 0x0F) << 4) | (volume & 0x0F));
    pio_test_write_reg(0x10 + channel, fn & 0xFF);
    pio_test_write_reg(0x20 + channel, (blk << 1) | ((fn >> 8) & 1));
    sleep_us(50);
    pio_test_write_reg(0x20 + channel, 0x10 | (blk << 1) | ((fn >> 8) & 1));
}

// ========== Main ==========

int main() {
    stdio_init_all();

    // Wait for USB CDC connection
    sleep_ms(2000);

    printf("\n\n=== RPFM YM2413 PIO Debug ===\n");
    printf("USB CDC connected!\n");

    // Init CPU-controlled GPIO
    printf("[INIT] Setting up CPU GPIO...\n");
    gpio_init(PIN_A0);  gpio_set_dir(PIN_A0, GPIO_OUT);  gpio_put(PIN_A0, 0);
    gpio_init(PIN_CS0); gpio_set_dir(PIN_CS0, GPIO_OUT); gpio_put(PIN_CS0, 1);
    gpio_init(PIN_IC);  gpio_set_dir(PIN_IC, GPIO_OUT);  gpio_put(PIN_IC, 1);
    for (int i = PIN_A1; i <= PIN_A3; i++) {
        gpio_init(i); gpio_set_dir(i, GPIO_OUT); gpio_put(i, 0);
    }
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT); gpio_put(PIN_LED, 0);

    // Init data bus GPIO (SIO mode first, PIO will take over)
    printf("[INIT] Setting up bus GPIO 0-8...\n");
    for (int i = PIN_BUS_BASE; i < PIN_BUS_BASE + 9; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_OUT);
        gpio_put(i, 0);
    }

    // === Test 1: Bitbang sanity check ===
    printf("\n=== TEST 1: Bitbang sanity check ===\n");
    printf("Resetting YM2413...\n");
    ym2413_reset();
    ym2413_mute_all_bitbang();
    printf("Bitbang mute done.\n");

    printf("Playing C4 via bitbang (2s)...\n");
    ym2413_note_on_bitbang(0, 0x08, 0x00, 60);
    sleep_ms(2000);
    ym2413_write_reg_bitbang(0x20, 0x00);
    sleep_ms(500);
    printf("Bitbang test done. Did you hear sound?\n");

    // === Test 2: PIO init ===
    printf("\n=== TEST 2: PIO init ===\n");
    uint offset = pio_add_program(pio0, &ym2413_write_program);
    printf("PIO program loaded at offset %d\n", offset);
    uint sm = pio_claim_unused_sm(pio0, true);
    printf("SM %d claimed\n", sm);

    ym2413_write_program_init(pio0, sm, offset, PIN_BUS_BASE);
    printf("PIO SM initialized. Pin func check:\n");
    for (int i = 0; i < 9; i++) {
        printf("  GPIO%d func=%d\n", i, gpio_get_function(i));
    }

    sleep_ms(100);
    pio_dump_sm(pio0, sm);

    // === Test 3: PIO single out ===
    printf("\n=== TEST 3: PIO single out ===\n");
    pio_test_single_out();

    // === Test 4: PIO write register ===
    printf("\n=== TEST 4: PIO register write ===\n");
    printf("Resetting YM2413...\n");
    ym2413_reset();
    ym2413_mute_all_pio();
    printf("PIO mute done.\n");

    // === Test 5: PIO play note ===
    printf("\n=== TEST 5: PIO play C4 (2s) ===\n");
    ym2413_note_on_pio(0, 0x08, 0x00, 60);
    sleep_ms(2000);
    pio_test_write_reg(0x20, 0x00);
    sleep_ms(500);
    printf("PIO note test done. Did you hear sound?\n");

    // === Done ===
    printf("\n=== All tests done ===\n");
    printf("Looping bitbang melody now...\n");

    static const uint8_t scale[] = {60, 62, 64, 65, 67, 69, 71, 72};
    while (true) {
        gpio_put(PIN_LED, 1);
        for (int i = 0; i < 8; i++) {
            ym2413_note_on_bitbang(0, 0x08, 0x00, scale[i]);
            sleep_ms(400);
            ym2413_write_reg_bitbang(0x20, 0x00);
            sleep_ms(30);
        }
        gpio_put(PIN_LED, 0);
        sleep_ms(1000);
    }

    return 0;
}
