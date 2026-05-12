#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_TUD_RHPORT      0
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_HID             1
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#define CFG_TUD_HID_EP_BUFSIZE  64

#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))

#ifdef __cplusplus
}
#endif

#endif
