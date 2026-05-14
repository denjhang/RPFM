// vgm_player.h — VGM stream player for RP2350A
// Core 1 tight loop with cycle counter timing (MegaGRRL-style)
// No timer ISR, no interrupt latency, sample-accurate timing

#ifndef VGM_PLAYER_H
#define VGM_PLAYER_H

#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "command_buffer.h"
#include "spfm_bus.pio.h"
#include "protocol.h"

// ========== VGM Command Length Table ==========
// Total bytes including opcode. 0 = variable/special.
static const uint8_t VGM_CMD_LEN[256] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  // 0x00-0x0F
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,  // 0x10-0x1F
    3,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,  // 0x20-0x2F
    2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,  // 0x30-0x3F
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,2,  // 0x40-0x4F
    2,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,  // 0x50-0x5F
    0,3,1,1,0,0,0,0, 0,0,0,0,0,0,0,0,  // 0x60-0x6F
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,  // 0x70-0x7F
    2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,  // 0x80-0x8F
    5,5,6,0,0,0,0,0, 0,0,0,0,0,0,0,0,  // 0x90-0x9F
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,  // 0xA0-0xAF
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,  // 0xB0-0xBF
    5,5,5,4,4,4,4,4, 4,4,4,4,4,4,4,4,  // 0xC0-0xCF
    4,4,4,5,5,5,5,4, 4,4,4,4,4,4,4,4,  // 0xD0-0xDF
    5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,  // 0xE0-0xEF
    5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,  // 0xF0-0xFF
};

// ========== VGM Player State ==========
static cmd_buf_t *s_vgm_buf;
static PIO s_vgm_pio;
static uint s_vgm_sm;

static volatile uint32_t s_vgm_loop_offset = 0;  // 0 = no loop
static volatile int s_vgm_loop_count = 0;
static volatile int s_vgm_max_loops = 2;
static volatile bool s_vgm_started = false;
static volatile bool s_vgm_paused = false;
static volatile uint32_t s_vgm_tick = 0;  // current playback sample position (44100 Hz)

// Forward declarations from main.c
static inline void cs_select(uint8_t slot);
static inline void write_reg_ay(uint8_t slot, uint8_t addr, uint8_t data);

// Helper: read 1 byte from cmd_buf
static inline bool vgm_read_byte(uint8_t *byte) {
    return cmd_buf_read(s_vgm_buf, byte);
}

// Helper: skip N bytes from cmd_buf
static inline void vgm_skip_bytes(int n) {
    uint8_t dummy;
    for (int i = 0; i < n; i++) {
        if (!vgm_read_byte(&dummy)) break;
    }
}

// ========== Core 1 VGM Player (MegaGRRL-style) ==========

static void core1_vgm_main(void) {
    uint64_t cycle_us = 0;
    uint64_t last_cc = 0;
    uint64_t next_sample = 0;
    // 1 sample at 44100Hz = 1000000/44100 us approx 22.676 us
    // Instead, count in us and compare: next_time_us = next_sample * 1000000 / 44100

    while (true) {
        // Wait for start signal from Core 0
        if (!(s_status & STATUS_PLAYING)) {
            if (s_vgm_started) {
                // Just stopped — reset state
                s_vgm_started = false;
                cycle_us = 0;
                next_sample = 0;
            }
            // Tight idle — don't sleep, just spin
            continue;
        }

        if (!s_vgm_started) {
            s_vgm_started = true;
            cycle_us = 0;
            next_sample = 0;
            s_vgm_tick = 0;
            last_cc = timer_hw->timerawl;
        }

        // Pause: freeze time, don't consume buffer
        if (s_vgm_paused) {
            last_cc = timer_hw->timerawl;
            continue;
        }

        // Accumulate elapsed microseconds
        uint64_t cc = timer_hw->timerawl;
        cycle_us += (cc - last_cc);
        last_cc = cc;

        // Convert accumulated us to sample number
        uint64_t sample = cycle_us * 44100ULL / 1000000ULL;

        if (sample < next_sample) {
            // Not time yet — tight loop, Core 1 has nothing else to do
            continue;
        }

        // Process VGM commands until we hit a wait
        while (true) {
            uint8_t cmd;
            if (!vgm_read_byte(&cmd)) {
                s_status &= ~STATUS_PLAYING;
                break;
            }

            if (cmd == 0xA0) {
                uint8_t reg, data;
                if (!vgm_read_byte(&reg) || !vgm_read_byte(&data)) {
                    s_status &= ~STATUS_PLAYING;
                    break;
                }
                write_reg_ay(0, reg, data);
                // Don't break — process next command immediately
            }
            else if (cmd == 0x61) {
                uint8_t lo, hi;
                if (!vgm_read_byte(&lo) || !vgm_read_byte(&hi)) {
                    s_status &= ~STATUS_PLAYING;
                    break;
                }
                next_sample += lo | ((uint64_t)hi << 8);
                break;
            }
            else if (cmd == 0x62) {
                next_sample += 735;
                break;
            }
            else if (cmd == 0x63) {
                next_sample += 882;
                break;
            }
            else if (cmd >= 0x70 && cmd <= 0x7F) {
                next_sample += (cmd & 0x0F) + 1;
                break;
            }
            else if (cmd == 0x66) {
                if (s_vgm_loop_offset > 0 && s_vgm_loop_count < s_vgm_max_loops) {
                    s_vgm_buf->tail = s_vgm_loop_offset & (CMD_BUF_SIZE - 1);
                    s_vgm_loop_count++;
                } else {
                    s_status &= ~STATUS_PLAYING;
                    break;
                }
            }
            else if (cmd == 0x67) {
                uint8_t sz[3];
                for (int i = 0; i < 3; i++) {
                    if (!vgm_read_byte(&sz[i])) { s_status &= ~STATUS_PLAYING; break; }
                }
                if (!(s_status & STATUS_PLAYING)) break;
                uint32_t block_sz = sz[0] | ((uint32_t)sz[1] << 8) | ((uint32_t)sz[2] << 16);
                for (uint32_t i = 0; i < block_sz; i++) {
                    uint8_t dummy;
                    if (!vgm_read_byte(&dummy)) { s_status &= ~STATUS_PLAYING; break; }
                }
                if (!(s_status & STATUS_PLAYING)) break;
            }
            else {
                uint8_t skip = VGM_CMD_LEN[cmd];
                if (skip > 1) vgm_skip_bytes(skip - 1);
            }
        }

        // Publish current tick for host sync
        s_vgm_tick = (uint32_t)next_sample;
    }
}

// ========== API ==========

static void vgm_player_init(cmd_buf_t *buf, PIO pio, uint sm) {
    s_vgm_buf = buf;
    s_vgm_pio = pio;
    s_vgm_sm = sm;
}

static void vgm_player_start(uint32_t loop_offset) {
    s_vgm_loop_offset = loop_offset;
    s_vgm_loop_count = 0;
    s_vgm_paused = false;
    s_vgm_started = false;
    s_status |= STATUS_PLAYING;
}

static void vgm_player_stop(void) {
    s_status &= ~STATUS_PLAYING;
    s_vgm_paused = false;
}

static inline void vgm_player_pause(void)  { s_vgm_paused = true; }
static inline void vgm_player_resume(void) { s_vgm_paused = false; }

#endif
