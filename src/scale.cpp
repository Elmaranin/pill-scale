#include <math.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "scale.h"

// Calibration in NVS (non-volatile storage): namespace "scale".
// NVS has no float type, so scale is stored as a blob.
static const char *NS = "scale";

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

bool Scale::begin() {
    if (!cell_.begin()) return false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {  // no entry — keep the defaults
        nvs_get_i32(h, "offset", &offset_);
        size_t len = sizeof(scale_);
        nvs_get_blob(h, "scale", &scale_, &len);
        nvs_close(h);
    }
    return true;
}

bool Scale::collect(int n, RunningStats &out) {
    out.reset();
    // Generous timeout: twice the time needed at the nominal rate.
    uint32_t timeout_ms = (uint32_t)n * 2000 / cell_.sps() + 500;
    uint32_t t0 = now_ms();

    while ((int)out.count() < n) {
        if (cell_.ready()) {
            out.add(cell_.read());
        } else if (now_ms() - t0 > timeout_ms) {
            return false;
        } else {
            vTaskDelay(1);  // yield to USB and the watchdog
        }
    }
    return true;
}

bool Scale::tare(int n) {
    RunningStats s;
    if (!collect(n, s)) return false;
    offset_ = (int32_t)lround(s.mean());
    save();
    return true;
}

bool Scale::calibrate(float known_g, int n) {
    if (known_g <= 0) return false;
    RunningStats s;
    if (!collect(n, s)) return false;
    double counts = s.mean() - offset_;
    if (counts == 0) return false;
    // Keep the sign: with S+/S- swapped the scale becomes negative and grams
    // still come out positive.
    scale_ = (float)(counts / known_g);
    save();
    return true;
}

void Scale::reset_calibration() {
    offset_ = 0;
    scale_ = 1.0f;
    save();
}

void Scale::save() {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "offset", offset_);
    nvs_set_blob(h, "scale", &scale_, sizeof(scale_));
    nvs_commit(h);
    nvs_close(h);
}
