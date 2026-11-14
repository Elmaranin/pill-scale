#include "board/led.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

static const int PIN = CONFIG_PILLSCALE_PIN_LED;
static bool have = false;
static int state = -1;  // unknown


void led_begin() {
    if (PIN < 0) return;
    gpio_reset_pin((gpio_num_t)PIN);
    gpio_set_direction((gpio_num_t)PIN, GPIO_MODE_OUTPUT);
    have = true;
    led_set(false);
}

void led_set(bool on) {
    if (!have || state == (int)on) return;
    state = on;
#if CONFIG_PILLSCALE_LED_ACTIVE_LOW
    gpio_set_level((gpio_num_t)PIN, on ? 0 : 1);
#else
    gpio_set_level((gpio_num_t)PIN, on ? 1 : 0);
#endif
}
