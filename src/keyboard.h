// The T-Deck's keyboard: an ESP32-C3 on I2C (0x55) that scans the key matrix. keyboard_read() polls
// it, at most every 10 ms, from the main loop: the touch screen shares the I2C bus, and touch reads
// stalled for a second at a time while a separate keyboard task used the bus.
//
// With C3 firmware from June 2025 or later, which reports the key matrix: Shift and Sym work held
// down or tapped first (a tap applies to the next key), holding Backspace repeats it, and Mic is back
// (Sym+Mic is 0). Older C3 firmware (as on this T-Deck) ignores those commands and sends one
// character per key press; that's detected on the first press and read instead, with Shift and Sym
// held down and no Mic key. Either way, Shift+$ types &, which no key shows.
//
// The light can be set by C3 firmware from 25 December 2024 on. Before that, the C3 has no I2C
// commands at all: the light starts off and only Alt+B, handled by the C3 itself, toggles it.
#pragma once
#include <stdint.h>

static const uint32_t KEY_MIC = 0xF0;

// Call after Wire is started (instance.begin()).
void keyboard_begin();

// Polls the keyboard (call every loop) and returns the next key typed, if any: a printable
// character, '\n' (Enter), '\b' (Backspace) or KEY_MIC.
bool keyboard_read(uint32_t &key);

// Keyboard backlight, 0 (off) to 255.
void keyboard_set_light(uint8_t level);
