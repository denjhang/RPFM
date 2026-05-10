#ifndef PICO_SDK_CONFIG_H
#define PICO_SDK_CONFIG_H

// Disable vendor reset interface so Windows recognizes pure CDC
// and auto-loads usbser.sys driver with a COM port
#define PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_MS_OS_20_DESCRIPTOR 0
#define PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_RESET_TO_FLASH 0
#define PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_RESET_TO_BOOTSEL 0

#endif
