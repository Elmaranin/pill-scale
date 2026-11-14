#pragma once
#include <stddef.h>
// Network. If a home network is set in NVS — try to join it for 30 s;
// failed (or not set) — start our own access point (the scale is 192.168.4.1).
// All of it in a separate task: the main loop must never wait.

struct NetInfo {
    const char *mode;  // "off", "ap", "sta", "connecting"
    char ssid[33];
    char ip[16];       // "-" while there is no address
    int clients;       // connected to our AP (ap mode only)
    const char *ntp;   // "-" not used, "wait" waiting for a reply, "ok" time received
};

// Start the network task. on_up() is called from it once the network is up
// (the web server starts there). An AP password shorter than 8 chars — open network.
void wifi_begin(const char *ap_ssid, const char *ap_password, void (*on_up)());
void wifi_info(NetInfo &out);

// NTP (home network only). The server reply arrives in the network task; the
// main loop takes it from here and sets the time itself: true — a new value.
bool wifi_ntp_take(uint32_t &unix);
bool wifi_ntp_synced();  // time came from NTP at least once — the page does not override it

// Home network in NVS. Applied at the next power-on.
bool wifi_sta_get(char *ssid, size_t ssid_n);  // false — not set
bool wifi_sta_set(const char *ssid, const char *password);  // empty ssid — forget

