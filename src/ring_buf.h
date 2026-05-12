#ifndef RING_BUF_H
#define RING_BUF_H

#include <stdint.h>
#include <stdbool.h>

#define RING_SIZE 16384  // 16KB

typedef struct {
    uint8_t buf[RING_SIZE];
    volatile uint16_t head;  // write position (Core 0)
    volatile uint16_t tail;  // read position (Core 1)
} ring_buf_t;

static inline void ring_init(ring_buf_t *r) {
    r->head = 0;
    r->tail = 0;
}

static inline uint16_t ring_used(ring_buf_t *r) {
    return (r->head - r->tail) & (RING_SIZE - 1);
}

static inline uint16_t ring_free(ring_buf_t *r) {
    return (RING_SIZE - 1) - ring_used(r);
}

static inline bool ring_write(ring_buf_t *r, uint8_t byte) {
    if (ring_free(r) == 0) return false;
    r->buf[r->head] = byte;
    r->head = (r->head + 1) & (RING_SIZE - 1);
    return true;
}

static inline bool ring_read(ring_buf_t *r, uint8_t *byte) {
    if (ring_used(r) == 0) return false;
    *byte = r->buf[r->tail];
    r->tail = (r->tail + 1) & (RING_SIZE - 1);
    return true;
}

static inline uint8_t ring_peek(ring_buf_t *r) {
    return r->buf[r->tail];
}

#endif
