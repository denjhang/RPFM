// vgm_player.h — VGM stream player for RP2350A
// Timer ISR consumes VGM bytes from cmd_buf, writes registers via PIO

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

static volatile uint32_t s_vgm_samples_remain = 0;
static volatile uint32_t s_vgm_loop_offset = 0;  // 0 = no loop
static volatile int s_vgm_loop_count = 0;
static volatile int s_vgm_max_loops = 2;
static volatile uint8_t s_vgm_alarm_num = 0;
static volatile bool s_vgm_alarm_claimed = false;

// Forward declarations from main.c
static inline void cs_select(uint8_t slot);
static inline void write_reg_ay(uint8_t slot, uint8_t addr, uint8_t data);

// Helper: read 1 byte from cmd_buf (ISR context)
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

// ========== Timer ISR ==========
static void vgm_timer_callback(uint alarm_num) {
    (void)alarm_num;

    if (!(s_status & STATUS_PLAYING)) return;

    // Process VGM commands until we hit a wait
    while (true) {
        uint8_t cmd;
        if (!vgm_read_byte(&cmd)) {
            // Buffer empty — firmware underrun
            s_status &= ~STATUS_PLAYING;
            return;
        }

        if (cmd == 0xA0) {
            // AY8910 register write: reg, data
            uint8_t reg, data;
            if (!vgm_read_byte(&reg) || !vgm_read_byte(&data)) {
                s_status &= ~STATUS_PLAYING;
                return;
            }
            write_reg_ay(0, reg, data);
            // Don't break — process next command immediately
        }
        else if (cmd == 0x61) {
            // Wait N samples
            uint8_t lo, hi;
            if (!vgm_read_byte(&lo) || !vgm_read_byte(&hi)) {
                s_status &= ~STATUS_PLAYING;
                return;
            }
            s_vgm_samples_remain = lo | ((uint32_t)hi << 8);
            break;
        }
        else if (cmd == 0x62) {
            s_vgm_samples_remain = 735;  // 60Hz
            break;
        }
        else if (cmd == 0x63) {
            s_vgm_samples_remain = 882;  // 50Hz
            break;
        }
        else if (cmd >= 0x70 && cmd <= 0x7F) {
            s_vgm_samples_remain = (cmd & 0x0F) + 1;
            break;
        }
        else if (cmd == 0x66) {
            // End of data
            if (s_vgm_loop_offset > 0 && s_vgm_loop_count < s_vgm_max_loops) {
                // Loop: seek cmd_buf tail back
                s_vgm_buf->tail = s_vgm_loop_offset & (CMD_BUF_SIZE - 1);
                s_vgm_loop_count++;
                // Continue processing from loop point
            } else {
                s_status &= ~STATUS_PLAYING;
                return;
            }
        }
        else if (cmd == 0x67) {
            // Data block: skip 3 header bytes + block size
            uint8_t sz[3];
            for (int i = 0; i < 3; i++) {
                if (!vgm_read_byte(&sz[i])) { s_status &= ~STATUS_PLAYING; return; }
            }
            uint32_t block_sz = sz[0] | ((uint32_t)sz[1] << 8) | ((uint32_t)sz[2] << 16);
            for (uint32_t i = 0; i < block_sz; i++) {
                uint8_t dummy;
                if (!vgm_read_byte(&dummy)) { s_status &= ~STATUS_PLAYING; return; }
            }
        }
        else {
            // Unknown/unhandled command — skip
            uint8_t skip = VGM_CMD_LEN[cmd];
            if (skip > 1) vgm_skip_bytes(skip - 1);
        }
    }

    // Schedule next alarm
    if (s_vgm_samples_remain > 0 && (s_status & STATUS_PLAYING)) {
        // 1 sample = 1/44100 sec ≈ 22.676 µs
        // Use integer math: samples * 1000000 / 44100
        uint64_t delay_us = (uint64_t)s_vgm_samples_remain * 1000000ULL / 44100ULL;
        if (delay_us < 1) delay_us = 1;
        hardware_alarm_set_target(s_vgm_alarm_num, make_timeout_time_us(delay_us));
    }
}

// ========== API ==========

static void vgm_player_init(cmd_buf_t *buf, PIO pio, uint sm) {
    s_vgm_buf = buf;
    s_vgm_pio = pio;
    s_vgm_sm = sm;
}

static void vgm_player_start(uint32_t loop_offset) {
    s_vgm_samples_remain = 0;
    s_vgm_loop_offset = loop_offset;
    s_vgm_loop_count = 0;
    s_status |= STATUS_PLAYING;

    if (!s_vgm_alarm_claimed) {
        s_vgm_alarm_num = hardware_alarm_claim_unused(true);
        s_vgm_alarm_claimed = true;
        hardware_alarm_set_callback(s_vgm_alarm_num, vgm_timer_callback);
    }

    // Fire first alarm immediately to start processing
    hardware_alarm_set_target(s_vgm_alarm_num, make_timeout_time_us(1));
}

static void vgm_player_stop(void) {
    s_status &= ~STATUS_PLAYING;
    if (s_vgm_alarm_claimed) {
        hardware_alarm_cancel(s_vgm_alarm_num);
    }
}

#endif
