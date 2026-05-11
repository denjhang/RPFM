/*
 * USB CDC test - hello_usb pattern with tinyusb 0.20.0
 */
#include <stdio.h>
#include "pico/stdlib.h"

int main() {
    const uint LED_PIN = 25;
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    stdio_init_all();

    int count = 0;
    while (true) {
        gpio_put(LED_PIN, 1);
        printf("Hello %d\n", count++);
        sleep_ms(500);
        gpio_put(LED_PIN, 0);
        sleep_ms(500);
    }
}
