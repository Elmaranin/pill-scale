#pragma once
#include <stdint.h>
// Buzzer on one pin (from Kconfig).
// Non-blocking: beep() turns it on, poll() from the main loop turns it off on time.
void buzzer_begin();
bool buzzer_present();
void buzzer_beep(uint32_t ms, uint32_t hz = 0);  // hz: tone of this beep; 0 — from Kconfig
uint32_t buzzer_hz();                              // default tone (0 — active buzzer)

void buzzer_poll();
