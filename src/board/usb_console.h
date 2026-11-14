#pragma once
#include <stddef.h>

// Console over the built-in USB (ESP32-C3 USB Serial/JTAG).
// Output — plain printf. Input — non-blocking, via console_read().

// Install the driver. Call once, before the first printf.
void console_begin();

// Is the port open on the computer (is anyone at the other end).
bool console_connected();

// Take up to max bytes already received. Does not wait: no data — returns 0.

size_t console_read(char *buf, size_t max);
