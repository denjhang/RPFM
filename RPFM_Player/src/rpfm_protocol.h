#ifndef RPFM_PROTOCOL_H
#define RPFM_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#define RPFM_VID 0x2E8A
#define RPFM_PID 0x1090
#define RPFM_FRAME_SIZE 64

// CMD
#define CMD_WRITE_REG  0x01
#define CMD_WRITE_AY   0x08
#define CMD_WRITE_TICK 0x02
#define CMD_RESET      0x03
#define CMD_VGM_DATA   0x04
#define CMD_VGM_START  0x05
#define CMD_VGM_STOP   0x06
#define CMD_VGM_PAUSE  0x09
#define CMD_BOOTSEL    0x20
#define CMD_SET_DELAY  0x21
#define CMD_NOP        0xFF

// STATUS bits
#define STATUS_PLAYING   0x01
#define STATUS_BUF_HIGH  0x02
#define STATUS_ERROR     0x04

static inline uint8_t rpfm_crc8(const uint8_t *data, int len) {
    uint8_t crc = 0x00;
    for (int i = 0; i < len; i++) {
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

static inline void rpfm_make_frame(uint8_t *frame, uint8_t cmd, uint8_t seq,
                                    const uint8_t *payload, uint8_t plen) {
    memset(frame, 0, RPFM_FRAME_SIZE);
    frame[0] = cmd;
    frame[1] = seq;
    frame[2] = plen;
    if (plen > 0 && payload)
        memcpy(frame + 3, payload, plen);
    frame[63] = rpfm_crc8(frame, 63);
}

#endif
