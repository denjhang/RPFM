#ifndef RPFM_HID_H
#define RPFM_HID_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t ack_seq;
    uint8_t status;
    uint16_t buf_level;
    uint32_t tick;  // firmware playback sample position (44100 Hz)
} rpfm_resp_t;

// Open/close RPFM HID device
bool rpfm_hid_open(void);
void rpfm_hid_close(void);
bool rpfm_hid_is_open(void);

// Send frame and get response
bool rpfm_hid_send_frame(uint8_t cmd, uint8_t seq,
                          const uint8_t *payload, uint8_t plen,
                          rpfm_resp_t *resp);

// Convenience: write register (YM 20us or AY 1us)
bool rpfm_write_reg(uint8_t slot, uint8_t addr, uint8_t data, rpfm_resp_t *resp);
bool rpfm_write_reg_ay(uint8_t slot, uint8_t addr, uint8_t data, rpfm_resp_t *resp);
bool rpfm_send_bootsel(void);
bool rpfm_send_reset(void);
bool rpfm_set_ay_delay(uint8_t delay_us);

// VGM streaming
bool rpfm_send_vgm_data(const uint8_t *data, uint8_t len, uint16_t *buf_level, uint32_t *tick);
bool rpfm_vgm_start(uint16_t loop_offset, uint8_t *status);
bool rpfm_vgm_stop(void);

#endif
