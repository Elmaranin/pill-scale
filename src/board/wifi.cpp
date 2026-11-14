#include <stdio.h>
#include <string.h>
#include "board/wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

// NVS: namespace "net" — home network (ssid, pass).
static const char *NS = "net";
static const uint32_t STA_TIMEOUT_MS = 30000;
static const int GOT_IP = 1;

static esp_netif_t *ap_if = nullptr, *sta_if = nullptr;
static EventGroupHandle_t events;
static NetInfo info = {"off", "", "-", 0, "-"};
static volatile uint32_t ntp_unix = 0;  // last NTP reply, 0 — taken
static bool ntp_synced = false;
static bool sta_wanted = false;  // keep the connection (reconnect on drop)
static char ap_ssid[33], ap_pass[65];
static void (*on_up)() = nullptr;

static void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // The router is gone or the password is wrong: retry until the task
        // decides to give up (sta_wanted). The task itself counts the 30 s at start.
        snprintf(info.ip, sizeof(info.ip), "-");
        if (sta_wanted) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        snprintf(info.ip, sizeof(info.ip), IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(events, GOT_IP);
    }
}

// One init step: on failure print which one and why.
static bool step(const char *what, esp_err_t err) {
    if (err == ESP_OK) return true;
    printf("# ERROR: wifi %s: %s\n", what, esp_err_to_name(err));
    return false;
}

// The NTP reply arrives in the lwIP task: nothing heavy here, just remember it.
static void on_ntp(struct timeval *tv) {
    ntp_unix = (uint32_t)tv->tv_sec;
}

static void start_ntp() {
#ifdef CONFIG_PILLSCALE_NTP_SERVER
    if (!CONFIG_PILLSCALE_NTP_SERVER[0]) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_PILLSCALE_NTP_SERVER);
    cfg.wait_for_sync = false;
    cfg.sync_cb = on_ntp;  // repeats once an hour (CONFIG_LWIP_SNTP_UPDATE_DELAY)
    if (step("ntp", esp_netif_sntp_init(&cfg))) info.ntp = "wait";
#endif
}

static bool init_once() {
    if (!step("netif_init", esp_netif_init())) return false;
    if (!step("event_loop", esp_event_loop_create_default())) return false;
    ap_if = esp_netif_create_default_wifi_ap();
    sta_if = esp_netif_create_default_wifi_sta();
    if (!ap_if || !sta_if) { printf("# ERROR: wifi netif create\n"); return false; }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (!step("init", esp_wifi_init(&init))) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // we keep network settings ourselves, in NVS "net"
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr, nullptr);
    return true;
}

// Home network: connect and wait for an address within STA_TIMEOUT_MS.
static bool try_sta(const char *ssid, const char *pass) {
    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    if (!step("sta set_mode", esp_wifi_set_mode(WIFI_MODE_STA))) return false;
    if (!step("sta set_config", esp_wifi_set_config(WIFI_IF_STA, &cfg))) return false;
    sta_wanted = true;
    info.mode = "connecting";
    if (!step("sta start", esp_wifi_start())) { sta_wanted = false; return false; }
    esp_wifi_set_max_tx_power(CONFIG_PILLSCALE_WIFI_TX_POWER);  // only after start
    EventBits_t bits = xEventGroupWaitBits(events, GOT_IP, pdTRUE, pdFALSE, pdMS_TO_TICKS(STA_TIMEOUT_MS));
    if (bits & GOT_IP) return true;
    sta_wanted = false;
    esp_wifi_stop();
    return false;
}

static bool start_ap() {
    wifi_config_t cfg = {};
    size_t n = strlen(ap_ssid) < sizeof(cfg.ap.ssid) ? strlen(ap_ssid) : sizeof(cfg.ap.ssid);
    memcpy(cfg.ap.ssid, ap_ssid, n);
    cfg.ap.ssid_len = (uint8_t)n;
    strncpy((char *)cfg.ap.password, ap_pass, sizeof(cfg.ap.password) - 1);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = strlen(ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (!step("ap set_mode", esp_wifi_set_mode(WIFI_MODE_AP))) return false;
    if (!step("ap set_config", esp_wifi_set_config(WIFI_IF_AP, &cfg))) return false;
    if (!step("ap start", esp_wifi_start())) return false;
    // TX power: at full power the SuperMini is invisible to the phone (antenna).

    step("tx_power", esp_wifi_set_max_tx_power(CONFIG_PILLSCALE_WIFI_TX_POWER));
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ap_if, &ip) == ESP_OK) snprintf(info.ip, sizeof(info.ip), IPSTR, IP2STR(&ip.ip));
    return true;
}

static void task(void *) {
    bool up = false;
    char ssid[33];
    if (init_once()) {
        if (wifi_sta_get(ssid, sizeof(ssid))) {
            char pass[65] = "";
            nvs_handle_t h;
            if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
                size_t n = sizeof(pass);
                nvs_get_str(h, "pass", pass, &n);
                nvs_close(h);
            }
            printf("# wifi: connecting to %s (up to %lu s)\n", ssid, (unsigned long)(STA_TIMEOUT_MS / 1000));
            strncpy(info.ssid, ssid, sizeof(info.ssid) - 1);
            if (try_sta(ssid, pass)) {
                info.mode = "sta";
                up = true;
                printf("# wifi: %s, http://%s\n", ssid, info.ip);
                start_ntp();
            } else {
                printf("# wifi: %s not reachable, falling back to own network\n", ssid);
            }
        }
        if (!up) {
            strncpy(info.ssid, ap_ssid, sizeof(info.ssid) - 1);
            up = start_ap();
            info.mode = up ? "ap" : "off";
            if (up) printf("# wifi: %s, http://%s\n", ap_ssid, info.ip);
        }
    }
    if (!up) printf("# ERROR: wifi failed\n");
    if (up && on_up) on_up();
    vTaskDelete(nullptr);
}

void wifi_begin(const char *ssid, const char *password, void (*cb)()) {
    strncpy(ap_ssid, ssid, sizeof(ap_ssid) - 1);
    strncpy(ap_pass, password, sizeof(ap_pass) - 1);
    on_up = cb;
    events = xEventGroupCreate();
    if (xTaskCreate(task, "net", 6144, nullptr, 5, nullptr) != pdPASS) printf("# ERROR: wifi task\n");
}

void wifi_info(NetInfo &out) {
    out = info;
    out.clients = 0;
    if (strcmp(info.mode, "ap") == 0) {
        wifi_sta_list_t list;
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) out.clients = list.num;
    }
}

bool wifi_ntp_take(uint32_t &unix) {
    uint32_t t = ntp_unix;
    if (!t) return false;
    ntp_unix = 0;
    ntp_synced = true;
    info.ntp = "ok";
    unix = t;
    return true;
}

bool wifi_ntp_synced() { return ntp_synced; }

bool wifi_sta_get(char *ssid, size_t ssid_n) {
    ssid[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = ssid_n;
    bool ok = nvs_get_str(h, "ssid", ssid, &n) == ESP_OK && ssid[0];
    nvs_close(h);
    return ok;
}

bool wifi_sta_set(const char *ssid, const char *password) {
    if (strlen(ssid) > 32 || strlen(password) > 63) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    if (ssid[0]) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", password);
    } else {
        nvs_erase_all(h);
    }
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
