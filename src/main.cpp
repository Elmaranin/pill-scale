// pillscale — firmware entry point.
// Protocol: docs/api.md, plan: docs/roadmap.md.

#include <stdio.h>
#include <stdlib.h>
#include "board/usb_console.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "load_cell.h"
#include "scale.h"
#include "cmd.h"
#include "logbook.h"
#include "web.h"
#include "board/wifi.h"
#include "board/led.h"
#include "board/buzzer.h"
#include "sdkconfig.h"

static Scale scale(load_cell_instance());
static CommandLine cmdline(scale);

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

// Network is up (called from the network task) — start the web server.
static void on_net_up() {
    if (!web_begin()) printf("# ERROR: web server failed\n");
}

// Time from NTP (received in the network task). Apply it if we had none or the
// chip clock drifted by more than 2 s: every apply adds a "time set" record to
// the journal, and crystal drift per hour is a fraction of a second, so there
// will be almost no records.
static void apply_ntp() {
    uint32_t unix;
    if (!wifi_ntp_take(unix)) return;
    int64_t have = logbook_now_unix();
    if (!logbook_time_known() || llabs(have - (int64_t)unix) > 2) logbook_set_time(unix, logbook_tz());
}

// LED: time known — steady on; unknown — blinks once a second.
// Without time the schedule cannot fire, and that must be visible without a phone.
static void indicate(uint32_t ms) {
    led_set(logbook_time_known() || (ms % 1000) < 500);
}

static void nvs_begin() {
    esp_err_t err = nvs_flash_init();
    // NVS partition corrupted or from another IDF version — erase and start over
    // (calibration is lost and has to be repeated).
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

extern "C" void app_main() {
    nvs_begin();
    logbook_begin();  // journal in flash: writes a "boot" record
    console_begin();
    led_begin();
    buzzer_begin();

    // Wait up to 3 s for the port to open so the startup lines are not lost.
    uint32_t t0 = now_ms();
    while (!console_connected() && now_ms() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(10));

    printf("# pillscale %s, adc=%s\n", esp_app_get_description()->version, scale.cell().name());
    if (!scale.begin()) {
        printf("# ERROR: ADC not responding, check wiring (docs/hardware.md)\n");
    } else {
        cmdline.begin_tracking();  // the tracker and the journal work without a computer
    }
    if (!logbook_ok()) printf("# WARN: no 'log' partition — flash with the new partitions.csv\n");

#if CONFIG_PILLSCALE_WIFI_AP
    // Network in its own task: home network (if configured, up to 30 s) or own AP.
    // The task prints the page address; the net command shows the same.
    wifi_begin(CONFIG_PILLSCALE_AP_SSID, CONFIG_PILLSCALE_AP_PASSWORD, on_net_up);
#endif

    // Main loop: never waits, only polls (commands and streaming are in cmd.cpp).
    for (;;) {
        cmdline.poll();
        buzzer_poll();
        apply_ntp();
        indicate(now_ms());
        vTaskDelay(1);  // 1 ms: yield to USB and the watchdog
    }
}
