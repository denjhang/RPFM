// Stub: spfm.h — maps SPFM low-level calls to RPFM HID
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "rpfm_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

// In RPFM, spfm_write_reg maps to rpfm_write_reg_ay
static inline void spfm_write_reg(uint8_t slot, uint8_t port, uint8_t addr, uint8_t data) {
    (void)port;
    rpfm_write_reg_ay(slot, addr, data, NULL);
}

static inline void spfm_hw_wait(uint32_t samples) {
    (void)samples;
}

static inline bool spfm_flush(void) {
    return true;
}

static inline int spfm_init(int) { return 0; }
static inline void spfm_cleanup(void) {}
static inline void spfm_reset(void) {}
static inline void spfm_chip_reset(void) {}
static inline void spfm_write_regs(uint8_t, const void*, uint32_t, uint32_t) {}
static inline void spfm_write_data(uint8_t, uint8_t) {}
static inline void spfm_write_raw(const uint8_t*, uint32_t) {}
static inline void spfm_wait_and_write_reg(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t) {}

#ifdef __cplusplus
}
#endif
