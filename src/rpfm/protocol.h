#ifndef RPFM_PROTOCOL_H
#define RPFM_PROTOCOL_H

#include <stdint.h>

// Frame size
#define RPFM_FRAME_SIZE  64

// Downlink frame (host → device, 64 bytes):
//   [0]  CMD
//   [1]  SEQ
//   [2]  LEN (payload length, 0-60)
//   [3..62] PAYLOAD
//   [63] CRC8

// Uplink frame (device → host, 64 bytes):
//   [0]  ACK (= SEQ)
//   [1]  STATUS
//   [2..3] BUF_LVL (little-endian)
//   [4..63] DATA

// CMD
#define CMD_WRITE_REG   0x01  // [slot, addr, data] × N — YM2413 20µs timing
#define CMD_WRITE_AY    0x08  // [slot, addr, data] × N — AY8910 1µs timing
#define CMD_WRITE_TICK  0x02  // [tick_count, uint24 LE]
#define CMD_RESET       0x03  // [slot_mask]
#define CMD_VGM_DATA    0x04  // [vgm_bytes...]
#define CMD_VGM_START   0x05  // [sample_rate(2B LE)]
#define CMD_VGM_STOP    0x06
#define CMD_VGM_PAUSE   0x09  // [1=pause, 0=resume]
#define CMD_QUERY       0x10
#define CMD_BOOTSEL     0x20
#define CMD_SET_DELAY   0x21  // [delay_us(1B)] — set AY8910 PIO write delay (0-20µs)
#define CMD_NOP         0xFF

// STATUS bits
#define STATUS_PLAYING   0x01
#define STATUS_BUF_HIGH  0x02  // buffer > 75%
#define STATUS_ERROR     0x04

// CRC8 (Dallas/Maxim polynomial 0x31)
static inline uint8_t rpfm_crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x31;
            else
                crc <<= 1;
        }
    }
    return crc;
}

#endif
