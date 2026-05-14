#ifndef SCC_H
#define SCC_H

#include <stdint.h>
#include <stdbool.h>

// Konami K051649 (SCC) — 5ch waveform synthesizer
// VGM command: 0xD2 [port][reg][data] (4 bytes)
//   port: 0=waveform, 1=frequency, 2=volume, 3=keyonoff, 5=test
//   reg:  latch register (channel/specific index)
//   data: value to write

#define SCC_CHANS   5
#define SCC_WAVELEN 32
#define SCC_FREQ_BITS 16

typedef struct {
    uint32_t counter;
    uint16_t frequency;
    uint8_t  volume;
    uint8_t  key;
    int8_t   waveram[SCC_WAVELEN];
} scc_channel_t;

typedef struct {
    scc_channel_t ch[SCC_CHANS];
    uint32_t clock;
    uint32_t rate;
    uint8_t  mode_plus;  // 0=SCC (K051649), 1=SCC+ (K052539)
    uint8_t  test;
} scc_state_t;

void scc_init(scc_state_t *s, uint32_t clock_hz);
void scc_reset(scc_state_t *s);
void scc_write(scc_state_t *s, uint8_t port, uint8_t reg, uint8_t data);
int8_t scc_render(scc_state_t *s);

#endif
