#pragma once
// LED on one pin (from Kconfig). Pin -1 — no LED, the functions do nothing.
void led_begin();
void led_set(bool on);  // the level is written only when the state changes

