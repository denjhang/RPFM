#ifndef EMU_COMMON_H
#define EMU_COMMON_H

#include <stdint.h>
#include <stdbool.h>

typedef struct spfm_emu {
    uint8_t  chip_id;
    bool     active;
    uint8_t  volume;  // 0-255, default 128
    void    *state;

    void  (*write_reg)(void *state, uint8_t reg, uint8_t data);
    int8_t (*render)(void *state);
    void  (*init)(void *state, uint32_t clock_hz);
    void  (*reset)(void *state);
} spfm_emu_t;

#endif
