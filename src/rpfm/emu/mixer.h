#ifndef MIXER_H
#define MIXER_H

#include "emu_common.h"
#include <stdint.h>
#include <stdbool.h>

#define MIXER_MAX_SOURCES 8

typedef struct {
    spfm_emu_t *sources[MIXER_MAX_SOURCES];
    int         active_count;
    volatile int8_t dac_sample;
    volatile bool   dac_active;
} mixer_t;

static inline void mixer_init(mixer_t *m) {
    m->active_count = 0;
    m->dac_sample = 0;
    m->dac_active = false;
    for (int i = 0; i < MIXER_MAX_SOURCES; i++)
        m->sources[i] = NULL;
}

static inline void mixer_add_source(mixer_t *m, spfm_emu_t *emu) {
    if (m->active_count < MIXER_MAX_SOURCES)
        m->sources[m->active_count++] = emu;
}

static inline void mixer_clear_sources(mixer_t *m) {
    m->active_count = 0;
    for (int i = 0; i < MIXER_MAX_SOURCES; i++)
        m->sources[i] = NULL;
}

static inline uint8_t mixer_render(mixer_t *m) {
    int16_t mix = 0;
    for (int i = 0; i < m->active_count; i++) {
        if (!m->sources[i] || !m->sources[i]->active) continue;
        int8_t s = m->sources[i]->render(m->sources[i]->state);
        mix += (int16_t)s * (int16_t)m->sources[i]->volume / 128;
    }
    if (m->dac_active)
        mix += m->dac_sample;
    if (mix > 127) mix = 127;
    if (mix < -128) mix = -128;
    return (uint8_t)(mix + 128);
}

#endif
