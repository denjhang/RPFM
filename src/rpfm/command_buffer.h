#ifndef COMMAND_BUFFER_H
#define COMMAND_BUFFER_H

#include <stdint.h>
#include <stdbool.h>

#define CMD_BUF_SIZE 32768  // 32KB

typedef struct {
    uint8_t buf[CMD_BUF_SIZE];
    volatile uint32_t head;  // write (Core 0)
    volatile uint32_t tail;  // read (Core 1)
} cmd_buf_t;

static inline void cmd_buf_init(cmd_buf_t *b) {
    b->head = 0;
    b->tail = 0;
}

static inline uint32_t cmd_buf_used(cmd_buf_t *b) {
    return (b->head - b->tail) & (CMD_BUF_SIZE - 1);
}

static inline uint32_t cmd_buf_free(cmd_buf_t *b) {
    return (CMD_BUF_SIZE - 1) - cmd_buf_used(b);
}

static inline bool cmd_buf_write(cmd_buf_t *b, uint8_t byte) {
    if (cmd_buf_free(b) == 0) return false;
    b->buf[b->head] = byte;
    b->head = (b->head + 1) & (CMD_BUF_SIZE - 1);
    return true;
}

static inline bool cmd_buf_read(cmd_buf_t *b, uint8_t *byte) {
    if (cmd_buf_used(b) == 0) return false;
    *byte = b->buf[b->tail];
    b->tail = (b->tail + 1) & (CMD_BUF_SIZE - 1);
    return true;
}

// Write multiple bytes
static inline uint32_t cmd_buf_write_buf(cmd_buf_t *b, const uint8_t *data, uint32_t len) {
    uint32_t written = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (!cmd_buf_write(b, data[i])) break;
        written++;
    }
    return written;
}

#endif
