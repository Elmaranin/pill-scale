// HX711 driver: channel A, gain 128.
//
// Protocol: DOUT goes low — data ready. Then 24 SCK pulses, one bit read on
// each (MSB first), and one more pulse selects channel A / gain 128 for the
// next conversion.
//
// Important: if SCK stays high longer than ~60 us the chip goes to sleep, so
// the read runs in a critical section (interrupts disabled). Timings — check
// against the HX711 datasheet.

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "load_cell.h"

static const gpio_num_t PIN_DOUT = (gpio_num_t)CONFIG_PILLSCALE_PIN_HX711_DOUT;
static const gpio_num_t PIN_SCK = (gpio_num_t)CONFIG_PILLSCALE_PIN_HX711_SCK;

class Hx711Cell : public LoadCell {
public:
    bool begin() override {
        gpio_reset_pin(PIN_SCK);
        gpio_set_direction(PIN_SCK, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_SCK, 0);

        // Pull-up: with the module disconnected DOUT does not float and
        // ready() honestly returns false instead of random garbage.
        gpio_reset_pin(PIN_DOUT);
        gpio_set_direction(PIN_DOUT, GPIO_MODE_INPUT);
        gpio_set_pull_mode(PIN_DOUT, GPIO_PULLUP_ONLY);

        // Wait up to 1 s for the first sample. At 10 SPS it arrives in ~100 ms.
        int64_t t0 = esp_timer_get_time();
        while (!ready()) {
            if (esp_timer_get_time() - t0 > 1000 * 1000) return false;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        read();  // discard the first sample after power-on
        return true;
    }

    bool ready() override { return gpio_get_level(PIN_DOUT) == 0; }

    int32_t read() override {
        uint32_t v = 0;

        portENTER_CRITICAL(&mux_);
        for (int i = 0; i < 24; i++) {
            gpio_set_level(PIN_SCK, 1);
            esp_rom_delay_us(1);
            v = (v << 1) | (gpio_get_level(PIN_DOUT) ? 1u : 0u);
            gpio_set_level(PIN_SCK, 0);
            esp_rom_delay_us(1);
        }
        // 25th pulse: channel A, gain 128
        gpio_set_level(PIN_SCK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_SCK, 0);
        portEXIT_CRITICAL(&mux_);

        // 24-bit two's complement → int32

        if (v & 0x800000) v |= 0xFF000000;
        return (int32_t)v;
    }

    const char *name() const override { return "hx711"; }
    int sps() const override { return 10; }

private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

LoadCell &load_cell_instance() {
    static Hx711Cell cell;
    return cell;
}
