#include "keyboard.h"
#include <Arduino.h>
#include <Wire.h>
#include <string.h>

static const uint8_t ADDRESS = 0x55;
static const uint8_t CMD_LIGHT = 0x01, CMD_RAW_MODE = 0x03, CMD_KEY_MODE = 0x04;  // C3 firmware commands
static const int COLS = 5, ROWS = 7;
static const uint32_t POLL_MS = 10;
static const uint32_t REPEAT_DELAY_MS = 500, REPEAT_MS = 70;  // Backspace held down

// The C3 firmware's matrix (column, row). 0 marks the modifier and special keys handled below.
static const char BASE[COLS][ROWS] = {
    {'q', 'w', 0, 'a', 0, ' ', 0},
    {'e', 's', 'd', 'p', 'x', 'z', 0},
    {'r', 'g', 't', 0, 'v', 'c', 'f'},
    {'u', 'h', 'y', 0, 'b', 'n', 'j'},
    {'o', 'l', 'i', 0, '$', 'm', 'k'},
};
// With Sym: the symbols printed on the keys.
static const char SYM[COLS][ROWS] = {
    {'#', '1', 0, '*', 0, ' ', '0'},
    {'2', '4', '5', '@', '8', '7', 0},
    {'3', '/', '(', 0, '?', '9', '6'},
    {'_', ':', ')', 0, '!', ',', ';'},
    {'+', '"', '-', 0, '$', '.', '\''},
};
static bool is_sym(int c, int r) { return c == 0 && r == 2; }
static bool is_alt(int c, int r) { return c == 0 && r == 4; }
static bool is_mic(int c, int r) { return c == 0 && r == 6; }
static bool is_shift(int c, int r) { return (c == 1 && r == 6) || (c == 2 && r == 3); }
static bool is_enter(int c, int r) { return c == 3 && r == 3; }
static bool is_backspace(int c, int r) { return c == 4 && r == 3; }

static QueueHandle_t keys;
static int light_wanted = -1;  // set by keyboard_set_light(), sent by poll()
static int light_sent = -1;
static bool online, key_mode;
static int errors;

static uint8_t last_read[COLS], matrix[COLS];
static bool blip_pending;  // a lone character from older firmware, confirmed by the next read
static uint8_t blip;

// Modifiers: held, or tapped alone to apply to the next key.
struct Modifier {
    bool down, used, once;
    void press() { down = true; used = false; }
    void release()
    {
        down = false;
        if (!used) {
            once = !once;  // a second tap cancels
        }
    }
    bool active() const { return down || once; }
    void consume()
    {
        used = down;
        once = false;
    }
};
static Modifier shift, sym;
static bool alt_down;
static uint32_t repeat_key, repeat_at;

static void emit(uint32_t key)
{
    xQueueSend(keys, &key, 0);
}

static bool send(uint8_t cmd, int value = -1)
{
    Wire.beginTransmission(ADDRESS);
    Wire.write(cmd);
    if (value >= 0) {
        Wire.write((uint8_t)value);
    }
    return Wire.endTransmission() == 0;
}

// Older C3 firmware only has key mode: one character per press, already shifted.
static void key_mode_char(uint8_t c)
{
    if (c == 0x0D) {
        emit('\n');
    } else if (c == 0x08) {
        emit('\b');
    } else if (c == 0x04) {
        emit('&');  // Shift+$: the C3 sends '$' - 0x20, as it does for letters
    } else if (c >= 0x20 && c < 0x7F) {
        emit(c);
    }
}

static void key_pressed(int c, int r)
{
    if (is_sym(c, r)) {
        sym.press();
    } else if (is_shift(c, r)) {
        shift.press();
    } else if (is_alt(c, r)) {
        alt_down = true;
    } else if (alt_down) {
        // Alt+B (keyboard light) and Alt+C are the keyboard's own.
    } else if (is_enter(c, r)) {
        emit('\n');
    } else if (is_backspace(c, r)) {
        emit('\b');
        repeat_key = '\b';
        repeat_at = millis() + REPEAT_DELAY_MS;
    } else if (is_mic(c, r)) {
        emit(sym.active() ? '0' : KEY_MIC);
        sym.consume();
    } else {
        char ch = sym.active() ? SYM[c][r] : BASE[c][r];
        if (!sym.active() && shift.active() && ch >= 'a' && ch <= 'z') {
            ch -= 'a' - 'A';
        } else if (!sym.active() && shift.active() && ch == '$') {
            ch = '&';  // on no keycap
        }
        emit(ch);
        sym.consume();
        shift.consume();
    }
}

static void key_released(int c, int r)
{
    if (is_sym(c, r)) {
        sym.release();
    } else if (is_shift(c, r)) {
        shift.release();
    } else if (is_alt(c, r)) {
        alt_down = false;
    } else if (is_backspace(c, r)) {
        repeat_key = 0;
    }
}

static void poll_raw()
{
    uint8_t cur[COLS] = {};
    if (Wire.requestFrom(ADDRESS, (uint8_t)COLS) != COLS) {
        while (Wire.available()) {
            Wire.read();
        }
        online = ++errors < 20;
        return;
    }
    for (int i = 0; i < COLS; i++) {
        cur[i] = Wire.read();
    }
    errors = 0;

    // The C3 rescans each key every ~35 ms, so a real key shows in several reads in a row. Older
    // firmware ignores the raw mode command and answers a key press once, with its character in
    // every byte read (seen: "67 67 67 67 67" for g), then zeros.
    static const uint8_t ZERO[COLS] = {};
    bool zero = !memcmp(cur, ZERO, COLS);
    bool one_char = true;
    for (int i = 1; i < COLS; i++) {
        one_char &= cur[i] == cur[0] || cur[i] == 0;
    }
    if (blip_pending) {
        blip_pending = false;
        if (zero) {
            key_mode = true;
            send(CMD_KEY_MODE);
            key_mode_char(blip);
            Serial.println("Keyboard: older firmware, using key mode");
            memset(last_read, 0, COLS);
            return;
        }
    } else if (!zero && !memcmp(last_read, ZERO, COLS) && one_char &&
               (cur[0] == 0x04 || cur[0] == 0x08 || cur[0] == 0x0D || (cur[0] >= 0x21 && cur[0] < 0x7F) ||
                (cur[0] == ' ' && cur[1] == ' '))) {  // 0x20 alone is the space key's raw bit
        blip_pending = true;
        blip = cur[0];
    }

    // Act on a matrix once two reads agree, so a read during the C3's scan can't reorder keys.
    bool stable = !memcmp(cur, last_read, COLS);
    memcpy(last_read, cur, COLS);
    if (!stable || blip_pending) {
        return;
    }
    for (int c = 0; c < COLS; c++) {
        uint8_t changed = cur[c] ^ matrix[c];
        for (int r = 0; r < ROWS; r++) {
            if (changed & (1 << r)) {
                if (cur[c] & (1 << r)) {
                    key_pressed(c, r);
                } else {
                    key_released(c, r);
                }
            }
        }
        matrix[c] = cur[c];
    }
}

static void repeat()
{
    if (repeat_key && (int32_t)(millis() - repeat_at) >= 0) {
        emit(repeat_key);
        repeat_at += REPEAT_MS;
    }
}

static void poll_key_mode()
{
    if (Wire.requestFrom(ADDRESS, (uint8_t)1) != 1) {
        online = ++errors < 20;
        return;
    }
    errors = 0;
    key_mode_char(Wire.read());
}

// One poll, at most every POLL_MS (or every 500 ms while the keyboard doesn't answer).
static void poll()
{
    static uint32_t next_at;
    if ((int32_t)(millis() - next_at) < 0) {
        return;
    }
    next_at = millis() + POLL_MS;
    if (!online) {
        // Also waits out the keyboard's own boot.
        online = send(key_mode ? CMD_KEY_MODE : CMD_RAW_MODE);
        errors = 0;
        light_sent = -1;
        if (!online) {
            next_at = millis() + 500;
            return;
        }
        Serial.println("Keyboard ready");
    }
    if (light_wanted >= 0 && light_wanted != light_sent && send(CMD_LIGHT, light_wanted)) {
        light_sent = light_wanted;
    }
    if (key_mode) {
        poll_key_mode();
    } else {
        poll_raw();
        repeat();
    }
}

void keyboard_begin()
{
    keys = xQueueCreate(32, sizeof(uint32_t));
}

bool keyboard_read(uint32_t &key)
{
    poll();
    return xQueueReceive(keys, &key, 0) == pdTRUE;
}

void keyboard_set_light(uint8_t level)
{
    light_wanted = level;
}

