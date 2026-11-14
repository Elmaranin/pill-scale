#include "board/buzzer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "sdkconfig.h"

// Two kinds of buzzers. An active one (with its own oscillator) sounds on a high
// level. A passive one is a coil: it needs a square wave at its resonant
// frequency, otherwise a single click. The LEDC PWM block makes the square
// wave by itself, the CPU is not involved.
static const int PIN = CONFIG_PILLSCALE_PIN_BUZZER;
#ifdef CONFIG_PILLSCALE_BUZZER_HZ
static const int HZ = CONFIG_PILLSCALE_BUZZER_HZ;
#else
static const int HZ = 0;
#endif
static const ledc_timer_bit_t RES = LEDC_TIMER_10_BIT;  // 1024 steps, 50% = 512

static bool have = false;
static bool sounding = false;
static int64_t off_at_us = 0;

static void drive(bool on) {
    if (HZ > 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 512 : 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    } else {
        gpio_set_level((gpio_num_t)PIN, on ? 1 : 0);
    }
}

void buzzer_begin() {
    if (PIN < 0) return;
    if (HZ > 0) {
        ledc_timer_config_t t = {};
        t.speed_mode = LEDC_LOW_SPEED_MODE;
        t.duty_resolution = RES;
        t.timer_num = LEDC_TIMER_0;
        t.freq_hz = (uint32_t)HZ;
        t.clk_cfg = LEDC_AUTO_CLK;
        if (ledc_timer_config(&t) != ESP_OK) return;
        ledc_channel_config_t c = {};
        c.gpio_num = PIN;
        c.speed_mode = LEDC_LOW_SPEED_MODE;
        c.channel = LEDC_CHANNEL_0;
        c.timer_sel = LEDC_TIMER_0;
        c.duty = 0;  // silent until asked

        c.hpoint = 0;
        if (ledc_channel_config(&c) != ESP_OK) return;
    } else {
        gpio_reset_pin((gpio_num_t)PIN);
        gpio_set_direction((gpio_num_t)PIN, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)PIN, 0);
    }
    have = true;
}

bool buzzer_present() { return have; }

uint32_t buzzer_hz() { return HZ > 0 ? (uint32_t)HZ : 0; }

void buzzer_beep(uint32_t ms, uint32_t hz) {
    if (!have) return;
    if (HZ > 0) ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, hz ? hz : (uint32_t)HZ);
    off_at_us = esp_timer_get_time() + (int64_t)ms * 1000;
    if (!sounding) { drive(true); sounding = true; }
}

void buzzer_poll() {
    if (!sounding || esp_timer_get_time() < off_at_us) return;
    drive(false);
    sounding = false;
}
