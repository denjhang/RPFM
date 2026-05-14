#include "scc.h"
#include <string.h>

void scc_init(scc_state_t *s, uint32_t clock_hz) {
    memset(s, 0, sizeof(*s));
    s->clock = clock_hz;
    s->rate = clock_hz / 16;
}

void scc_reset(scc_state_t *s) {
    for (int i = 0; i < SCC_CHANS; i++) {
        s->ch[i].counter = 0;
        s->ch[i].frequency = 0;
        s->ch[i].volume = 0;
        s->ch[i].key = 0;
        memset(s->ch[i].waveram, 0, SCC_WAVELEN);
    }
    s->test = 0;
}

void scc_write(scc_state_t *s, uint8_t port, uint8_t reg, uint8_t data) {
    switch (port) {
    case 0: // Waveform RAM (reg = 0x00-0x7F)
        if (s->test & 0x40) return;
        if (!s->mode_plus) {
            // SCC: ch4 (0x60-0x7F) shares waveram with ch3
            if (reg >= 0x60) {
                s->ch[3].waveram[reg & 0x1f] = (int8_t)data;
                s->ch[4].waveram[reg & 0x1f] = (int8_t)data;
            } else {
                s->ch[reg >> 5].waveram[reg & 0x1f] = (int8_t)data;
            }
        } else {
            s->ch[reg >> 5].waveram[reg & 0x1f] = (int8_t)data;
        }
        break;

    case 1: // Frequency (reg: lo if bit0=0, hi if bit0=1, channel = reg>>1)
    {
        int ch = reg >> 1;
        if (ch < SCC_CHANS) {
            if (reg & 1)
                s->ch[ch].frequency = (s->ch[ch].frequency & 0x00FF) | ((uint16_t)(data & 0x0F) << 8);
            else
                s->ch[ch].frequency = (s->ch[ch].frequency & 0x0F00) | data;
            if (s->test & 0x20)
                s->ch[ch].counter = 0xFFFFFFFF;
            else if (s->ch[ch].frequency < 9)
                s->ch[ch].counter |= ((1 << SCC_FREQ_BITS) - 1);
        }
        break;
    }

    case 2: // Volume (reg = channel 0-4)
    {
        int ch = reg & 0x07;
        if (ch < SCC_CHANS)
            s->ch[ch].volume = data & 0x0F;
        break;
    }

    case 3: // Key on/off (data bits 0-4 = ch0-ch4)
        for (int i = 0; i < SCC_CHANS; i++)
            s->ch[i].key = (data >> i) & 1;
        break;

    case 5: // Test register
        s->test = data;
        break;
    }
}

int8_t scc_render(scc_state_t *s) {
    int16_t mix = 0;
    for (int i = 0; i < SCC_CHANS; i++) {
        scc_channel_t *c = &s->ch[i];
        if (c->frequency > 8 && c->key && c->volume > 0) {
            uint32_t step = (uint32_t)(
                ((uint64_t)s->clock << (SCC_FREQ_BITS + 1)) /
                ((uint32_t)(c->frequency + 1) * s->rate)
            );
            c->counter += step;
            uint32_t offs = (c->counter >> SCC_FREQ_BITS) & 0x1F;
            mix += (int16_t)c->waveram[offs] * c->volume;
        }
    }
    mix >>= 4;
    if (mix > 127) mix = 127;
    if (mix < -128) mix = -128;
    return (int8_t)mix;
}
