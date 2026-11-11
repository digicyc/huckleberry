// HuckleBerry: a phone-style home screen and apps for the LilyGo T-Deck (Plus): MeshCore LoRa
// messaging (also usable from the MeshCore phone app over Bluetooth), offline maps, GPS, weather,
// alarms, Wi-Fi, SD card and settings.
//
// The T-Deck has no RTC, so the clock starts unset at every boot. Internet time (when Wi-Fi
// connects), GPS time (with a fix) and the MeshCore phone app (when it connects) set it; so does
// `make settime`, which sends "T<unix seconds>\n" over USB serial. The system clock keeps UTC.
//
// USB serial also takes test commands, one per line: S (screenshot, read by tools/screenshot.py),
// K<text> (type UTF-8 text; \n Enter, \b Backspace, \e the mic key, \\ a backslash), B<dx>,<dy>
// (roll the trackball), C and L (click and hold the trackball), P<x>,<y> (tap the screen).
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <initializer_list>
#include <sys/time.h>
#include "alarms.h"
#include "bluetooth_app.h"
#include "gps_app.h"
#include "keyboard.h"
#include "lora_app.h"
#include "map_app.h"
#include "map_download.h"
#include "messages.h"
#include "messenger.h"
#include "sdcard_app.h"
#include "settings_app.h"
#include "sound.h"
#include "ui.h"
#include "version.h"
#include "weather_app.h"
#include "wifi_app.h"

// POSIX rule for America/Denver (MST/MDT).
static const char *TIME_ZONE = "MST7MDT,M3.2.0,M11.1.0";
static const lv_color_t GREEN = LV_COLOR_MAKE(0x4C, 0xAF, 0x50);  // lv_palette_main(LV_PALETTE_GREEN)
static const lv_color_t RED = LV_COLOR_MAKE(0xF4, 0x43, 0x36);    // lv_palette_main(LV_PALETTE_RED)
static const lv_color_t BLUE = LV_COLOR_MAKE(0x21, 0x96, 0xF3);   // lv_palette_main(LV_PALETTE_BLUE)

LV_IMAGE_DECLARE(huckleberry_logo);
LV_IMAGE_DECLARE(lora_icon);
LV_IMAGE_DECLARE(satellite_icon);
LV_IMAGE_DECLARE(weather_app_icon);
LV_IMAGE_DECLARE(wifi_icon_1);
LV_IMAGE_DECLARE(wifi_icon_2);
LV_IMAGE_DECLARE(wifi_icon_3);
LV_IMAGE_DECLARE(wifi_icon_4);

static lv_obj_t *boot_screen;  // until it's tapped
static lv_obj_t *home_screen;
static lv_obj_t *time_label, *ampm_label, *weekday_label, *date_label;
static lv_obj_t *weather_box, *weather_icon, *weather_label;  // conditions and temperature, under the date
static lv_obj_t *unread_badge;                                // on the Messages icon

// Status bar, on the top layer so it shows over every screen but the boot screen.
static lv_obj_t *status_bar, *bar_time, *bar_mail, *bar_bt, *bar_wifi, *bar_gps, *bar_gps_icon, *bar_gps_label, *bar_battery;
static lv_timer_t *status_timer;

static bool screen_on = true;
// Invisible full-screen layer, shown while the display is off, that catches the touch that wakes
// it so the tap doesn't also press whatever is underneath.
static lv_obj_t *wake_shield;

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_battery()
{
    int percent = (int)instance.getBatteryPercent();
    bool usb = instance.isAdapterConnected();
    lv_color_t color = usb ? GREEN : percent <= 15 ? RED : lv_color_white();
    const char *icon = usb ? LV_SYMBOL_CHARGE
                       : percent > 80 ? LV_SYMBOL_BATTERY_FULL
                       : percent > 55 ? LV_SYMBOL_BATTERY_3
                       : percent > 30 ? LV_SYMBOL_BATTERY_2
                       : percent > 10 ? LV_SYMBOL_BATTERY_1
                                      : LV_SYMBOL_BATTERY_EMPTY;
    char text[24];
    snprintf(text, sizeof(text), "%d%% %s", percent, icon);
    set_text(bar_battery, text);
    lv_obj_set_style_text_color(bar_battery, color, 0);
}

static void update_status(lv_timer_t *)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char buf[40];
    bool clock_set = t.tm_year + 1900 >= 2025;
    int hour12 = t.tm_hour % 12 ? t.tm_hour % 12 : 12;

    // Status bar.
    if (clock_set) {
        snprintf(buf, sizeof(buf), "%d:%02d", hour12, t.tm_min);
    } else {
        strcpy(buf, "--:--");
    }
    set_text(bar_time, buf);

    int unread = messenger_unread_total();
    set_hidden(bar_mail, unread == 0);

    if (messenger_bt_enabled()) {
        lv_obj_set_style_text_color(bar_bt, messenger_bt_connected() ? BLUE : GRAY, 0);
    }
    set_hidden(bar_bt, !messenger_bt_enabled());

    static const lv_image_dsc_t *const WIFI_ICONS[] = {&wifi_icon_1, &wifi_icon_2, &wifi_icon_3, &wifi_icon_4};
    int level = wifi_signal_level();
    if (level) {
        lv_image_set_src(bar_wifi, WIFI_ICONS[level - 1]);
    }
    set_hidden(bar_wifi, level == 0);

    // GPS: hidden when off, gray while searching, green with a fix; the count is satellites in view
    // while searching and used once fixed.
    GpsStatus gps = gps_status();
    if (gps.on) {
        lv_color_t color = gps.fix ? GREEN : GRAY;
        snprintf(buf, sizeof(buf), "%d", gps.satellites);
        set_text(bar_gps_label, buf);
        lv_obj_set_style_text_color(bar_gps_label, color, 0);
        lv_obj_set_style_image_recolor(bar_gps_icon, color, 0);
    }
    set_hidden(bar_gps, !gps.on);

    static uint32_t battery_at;
    if (!battery_at || millis() - battery_at > 5000) {
        battery_at = millis();
        update_battery();
    }

    // Home screen.
    if (lv_screen_active() != home_screen) {
        return;
    }
    if (clock_set) {
        snprintf(buf, sizeof(buf), "%d:%02d", hour12, t.tm_min);
        set_text(time_label, buf);
        set_text(ampm_label, t.tm_hour < 12 ? "AM" : "PM");
        strftime(buf, sizeof(buf), "%A", &t);
        set_text(weekday_label, buf);
        char month[16];
        strftime(month, sizeof(month), "%B", &t);  // newlib's strftime has no "%-d"
        snprintf(buf, sizeof(buf), "%s %d", month, t.tm_mday);
        set_text(date_label, buf);
    } else {
        set_text(time_label, "--:--");
        set_text(ampm_label, "");
        set_text(weekday_label, "Clock not set");
        set_text(date_label, "Waiting for GPS or Wi-Fi");
    }
    lv_obj_align_to(ampm_label, time_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);

    int temp_f;
    uint8_t code;
    bool is_day;
    if (weather_now(temp_f, code, is_day)) {
        weather_icon_set(weather_icon, code, is_day);
        snprintf(buf, sizeof(buf), "%d\xC2\xB0" "F", temp_f);
        set_text(weather_label, buf);
    }
    set_hidden(weather_box, !weather_now(temp_f, code, is_day));

    if (unread) {
        snprintf(buf, sizeof(buf), "%d", unread > 99 ? 99 : unread);
        set_text(lv_obj_get_child(unread_badge, 0), buf);
    }
    set_hidden(unread_badge, unread == 0);
}

static void hide_wake_shield(lv_event_t *)
{
    lv_obj_add_flag(wake_shield, LV_OBJ_FLAG_HIDDEN);
}

// Turns the display and keyboard light off after the Settings timeout without input; any input
// wakes it on the same screen. LVGL resets its inactivity time on touch; keys, the trackball,
// alarms and messages reset it via lv_display_trigger_activity().
static void update_screen_power()
{
    bool idle = lv_display_get_inactive_time(NULL) > settings_screen_timeout_ms();
    if (idle && screen_on) {
        screen_on = false;
        ui_set_screen_off(true);
        lv_timer_pause(status_timer);
        instance.sleepDisplay();
        keyboard_set_light(0);
        lv_obj_remove_flag(wake_shield, LV_OBJ_FLAG_HIDDEN);
    } else if (!idle && !screen_on) {
        screen_on = true;
        ui_set_screen_off(false);
        // A waking touch is still on the shield and hides it when lifted; other wakes don't touch
        // it, so hide it now.
        lv_indev_t *touch = lv_get_touch_indev();
        if (!touch || lv_indev_get_state(touch) != LV_INDEV_STATE_PRESSED) {
            hide_wake_shield(NULL);
        }
        update_status(NULL);
        lv_timer_resume(status_timer);
        lv_refr_now(NULL);
        instance.wakeupDisplay();
        instance.setBrightness(settings_brightness());
        keyboard_set_light(settings_keyboard_light());
    }
}

// Returns to the home screen from any app (holding the trackball down).
static void show_home()
{
    if (alarms_ringing() || lv_screen_active() == home_screen || boot_screen) {
        return;
    }
    lv_screen_load_anim(home_screen, LV_SCREEN_LOAD_ANIM_FADE_IN, 150, 0, false);
}

// ---------- Boot screen ----------

// The logo, name and version, shown from power-on, while the apps start, until the screen is tapped.
static void build_boot_screen()
{
    boot_screen = make_screen();

    lv_obj_t *logo = lv_image_create(boot_screen);
    lv_image_set_src(logo, &huckleberry_logo);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *name = make_label(boot_screen, "Huckle#B388FF Berry#", &lv_font_montserrat_28, lv_color_white());
    lv_label_set_recolor(name, true);
    lv_obj_align_to(name, logo, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_t *version = make_label(boot_screen, "Version " HUCKLEBERRY_VERSION, &lv_font_montserrat_14, GRAY);
    lv_obj_align_to(version, name, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    lv_obj_t *hint = make_label(boot_screen, "Tap the screen to start", &lv_font_montserrat_12, ORANGE);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, hint);
    lv_anim_set_values(&pulse, LV_OPA_COVER, LV_OPA_30);
    lv_anim_set_duration(&pulse, 1200);
    lv_anim_set_reverse_duration(&pulse, 1200);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&pulse, [](void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, v, 0); });
    lv_anim_start(&pulse);

    lv_obj_add_event_cb(boot_screen, [](lv_event_t *) {
        lv_screen_load_anim(home_screen, LV_SCREEN_LOAD_ANIM_FADE_IN, 200, 0, false);
    }, LV_EVENT_CLICKED, NULL);
    // An alarm or a message banner can also leave it.
    lv_obj_add_event_cb(boot_screen, [](lv_event_t *) {
        boot_screen = NULL;
        lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_add_event_cb(boot_screen, [](lv_event_t *e) {
        lv_obj_delete_async((lv_obj_t *)lv_event_get_target(e));
    }, LV_EVENT_SCREEN_UNLOADED, NULL);
}

// ---------- Home screen ----------

// Adds an app icon (a rounded square with a symbol or alpha-only image) and its name to the grid.
static lv_obj_t *add_app(lv_obj_t *grid, const void *icon_src, const char *name, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *item = lv_obj_create(grid);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, 60, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(item, 3, 0);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn = lv_button_create(item);
    lv_obj_set_size(btn, 46, 46);
    lv_obj_set_style_radius(btn, 13, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_outline_color(btn, lv_color_white(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(btn, 2, LV_STATE_FOCUS_KEY);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon = lv_image_create(btn);
    lv_image_set_src(icon, icon_src);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_22, 0);  // for symbols
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);  // for images
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_center(icon);

    make_label(item, name, &lv_font_montserrat_12, lv_color_white());
    return btn;
}

static void build_home()
{
    home_screen = make_screen();

    time_label = make_label(home_screen, "", &lv_font_montserrat_48, lv_color_white());
    lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, 14, STATUS_H + 4);
    ampm_label = make_label(home_screen, "", &lv_font_montserrat_16, GRAY);

    weekday_label = make_label(home_screen, "", &lv_font_montserrat_18, ORANGE);
    lv_obj_align(weekday_label, LV_ALIGN_TOP_RIGHT, -14, STATUS_H + 8);
    date_label = make_label(home_screen, "", &lv_font_montserrat_14, GRAY);
    lv_obj_align(date_label, LV_ALIGN_TOP_RIGHT, -14, STATUS_H + 30);

    weather_box = lv_obj_create(home_screen);
    lv_obj_remove_style_all(weather_box);
    lv_obj_set_size(weather_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(weather_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(weather_box, 4, 0);
    lv_obj_align(weather_box, LV_ALIGN_TOP_RIGHT, -12, STATUS_H + 48);
    weather_icon = weather_icon_create(weather_box);
    weather_label = make_label(weather_box, "", &lv_font_montserrat_16, lv_color_white());
    lv_obj_add_flag(weather_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *grid = lv_obj_create(home_screen);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, SCREEN_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_t *messages = add_app(grid, LV_SYMBOL_ENVELOPE, "Messages", lv_palette_main(LV_PALETTE_LIGHT_BLUE),
                                 [](lv_event_t *) { messages_open(); });
    add_app(grid, LV_SYMBOL_IMAGE, "Map", lv_palette_main(LV_PALETTE_TEAL), [](lv_event_t *) { map_open(); });
    add_app(grid, LV_SYMBOL_GPS, "GPS", lv_palette_main(LV_PALETTE_GREEN), [](lv_event_t *) { gps_open(); });
    add_app(grid, &weather_app_icon, "Weather", lv_palette_main(LV_PALETTE_CYAN), [](lv_event_t *) { weather_open(); });
    add_app(grid, LV_SYMBOL_BELL, "Alarms", lv_palette_main(LV_PALETTE_ORANGE), [](lv_event_t *) { alarms_open(); });
    add_app(grid, LV_SYMBOL_BLUETOOTH, "Bluetooth", lv_palette_main(LV_PALETTE_INDIGO), [](lv_event_t *) { bluetooth_open(); });
    add_app(grid, &lora_icon, "LoRa", lv_palette_main(LV_PALETTE_PINK), [](lv_event_t *) { lora_open(); });
    add_app(grid, LV_SYMBOL_WIFI, "Wi-Fi", lv_palette_main(LV_PALETTE_BLUE), [](lv_event_t *) { wifi_open(); });
    add_app(grid, LV_SYMBOL_SD_CARD, "SD Card", lv_palette_main(LV_PALETTE_DEEP_PURPLE), [](lv_event_t *) { sdcard_open(); });
    add_app(grid, LV_SYMBOL_SETTINGS, "Settings", lv_palette_main(LV_PALETTE_GREY), [](lv_event_t *) { settings_open(); });

    unread_badge = lv_obj_create(lv_obj_get_parent(messages));
    lv_obj_remove_style_all(unread_badge);
    lv_obj_add_flag(unread_badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(unread_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(unread_badge, LV_SIZE_CONTENT, 18);
    lv_obj_set_style_min_width(unread_badge, 18, 0);
    lv_obj_set_style_pad_hor(unread_badge, 4, 0);
    lv_obj_set_style_radius(unread_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(unread_badge, RED, 0);
    lv_obj_set_style_bg_opa(unread_badge, LV_OPA_COVER, 0);
    lv_obj_t *count = make_label(unread_badge, "", &lv_font_montserrat_12, lv_color_white());
    lv_obj_center(count);
    lv_obj_align(unread_badge, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_add_flag(unread_badge, LV_OBJ_FLAG_HIDDEN);
}

static void build_status_bar()
{
    lv_obj_t *bar = status_bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCREEN_W, STATUS_H);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    bar_time = make_label(bar, "", &lv_font_montserrat_14, lv_color_white());
    lv_obj_align(bar_time, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, STATUS_H);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);

    bar_mail = make_label(right, LV_SYMBOL_ENVELOPE, &lv_font_montserrat_14, ORANGE);
    bar_bt = make_label(right, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_14, GRAY);
    bar_wifi = lv_image_create(right);  // alpha-only, drawn in the recolor color
    lv_obj_set_style_image_recolor(bar_wifi, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(bar_wifi, LV_OPA_COVER, 0);
    bar_gps = lv_obj_create(right);
    lv_obj_remove_style_all(bar_gps);
    lv_obj_set_size(bar_gps, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar_gps, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar_gps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar_gps, 2, 0);
    lv_obj_remove_flag(bar_gps, LV_OBJ_FLAG_CLICKABLE);
    bar_gps_icon = lv_image_create(bar_gps);
    lv_image_set_src(bar_gps_icon, &satellite_icon);
    lv_obj_set_style_image_recolor_opa(bar_gps_icon, LV_OPA_COVER, 0);
    bar_gps_label = make_label(bar_gps, "", &lv_font_montserrat_14, GRAY);
    bar_battery = make_label(right, "", &lv_font_montserrat_14, lv_color_white());
    for (lv_obj_t *obj : {bar_mail, bar_bt, bar_wifi, bar_gps}) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------- USB serial ----------

// Sends what the screen shows: "SCREENSHOT <width> <height> <swapped>\n", width * height RGB565
// pixels (byte-swapped if <swapped> is 1), then "\nEND\n".
static void send_screenshot()
{
    lv_refr_now(NULL);
    lv_screen_capture_t cap;
    if (!lv_get_screen_capture(&cap)) {
        Serial.println("SCREENSHOT FAILED");
        return;
    }
    Serial.printf("SCREENSHOT %u %u %d\n", cap.width, cap.height, cap.rgb565_swapped ? 1 : 0);
    const uint8_t *p = (const uint8_t *)cap.pixels;
    size_t left = (size_t)cap.width * cap.height * 2;
    while (left) {
        size_t n = Serial.write(p, left < 4096 ? left : 4096);
        p += n;
        left -= n;
        if (!n) {
            delay(1);
        }
    }
    Serial.print("\nEND\n");
    Serial.flush();
}

static void type_text(const char *s)
{
    for (; *s; s++) {
        uint32_t key = (uint8_t)*s;
        if (*s == '\\' && s[1]) {
            s++;
            key = *s == 'n' ? '\n' : *s == 'b' ? '\b' : *s == 'e' ? KEY_MIC : (uint8_t)*s;
        } else if (key >= 0xC0) {
            // A multi-byte UTF-8 character (an emoji, say): LVGL takes its bytes packed in one key.
            size_t n = key >= 0xF0 ? 4 : key >= 0xE0 ? 3 : 2;
            key = 0;
            memcpy(&key, s, strnlen(s, n));
            s += strnlen(s, n) - 1;
        }
        ui_inject_key(key);
        // LVGL reads a key press and its release in two input reads, one per refresh period.
        for (uint32_t start = millis(); millis() - start < 2 * LV_DEF_REFR_PERIOD + 10;) {
            lv_timer_handler();
            delay(5);
        }
    }
}

static void handle_serial()
{
    static char line[200];
    static size_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c != '\n' && c != '\r') {
            if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
            continue;
        }
        line[len] = '\0';
        len = 0;
        int x, y;
        switch (line[0]) {
        case 'T': {
            time_t epoch = strtoll(line + 1, NULL, 10);
            if (epoch < 1735689600) {  // 2025-01-01
                Serial.printf("Rejected time: %s\n", line);
                break;
            }
            struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
            settimeofday(&tv, NULL);
            update_status(NULL);
            Serial.printf("Time set: %s", ctime(&epoch));
            break;
        }
        case 'S': send_screenshot(); break;
        case 'K': type_text(line + 1); break;
        case 'B':
            if (sscanf(line + 1, "%d,%d", &x, &y) == 2) {
                ui_inject_trackball(x, y);
            }
            break;
        case 'C': ui_inject_click(false); break;
        case 'L': ui_inject_click(true); break;
        case 'P':
            if (sscanf(line + 1, "%d,%d", &x, &y) == 2) {
                ui_inject_tap(x, y);
                lv_display_trigger_activity(NULL);
            }
            break;
        }
    }
}

void setup()
{
    Serial.begin(115200);

    // The radio is MeshCore's: messenger_init() starts it with MeshCore's settings. The keyboard is
    // keyboard.cpp's.
    LilyGoDeviceInitOptions options = instance.getDefaultInitOptions();
    options.initRadio = false;
    options.initKeyboard = false;
    instance.begin(options);
    setenv("TZ", TIME_ZONE, 1);
    tzset();

    beginLvglHelper(instance);
    lv_display_t *display = lv_display_get_default();
    lv_display_set_theme(display, lv_theme_default_init(display, ORANGE, lv_palette_main(LV_PALETTE_GREY), true,
                                                        &lv_font_montserrat_16));
    ui_input_init(show_home);

    // The boot screen shows while the rest starts; the backlight is off until then.
    build_boot_screen();
    lv_screen_load(boot_screen);
    build_home();
    settings_init(home_screen);  // for the backlight level
    lv_refr_now(NULL);
    instance.setBrightness(settings_brightness());
    build_status_bar();
    lv_obj_add_flag(status_bar, LV_OBJ_FLAG_HIDDEN);

    sdcard_init(home_screen);
    wifi_init(home_screen);
    weather_init(home_screen);  // before map_init: it moves mbedTLS's memory to PSRAM, which map downloads need
    map_init(home_screen);
    gps_init(home_screen);
    if (!messenger_init()) {
        Serial.println("LoRa radio failed to start");
    }
    messages_init(home_screen);
    bluetooth_init(home_screen);
    lora_init(home_screen);
    alarms_init(home_screen);
    sound_init();

    wake_shield = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(wake_shield);
    lv_obj_set_size(wake_shield, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(wake_shield, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(wake_shield, hide_wake_shield, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(wake_shield, hide_wake_shield, LV_EVENT_PRESS_LOST, NULL);

    update_status(NULL);
    status_timer = lv_timer_create(update_status, 1000, NULL);
    keyboard_set_light(settings_keyboard_light());
    Serial.printf("HuckleBerry " HUCKLEBERRY_VERSION " started in %lu ms\n", millis());
}

void loop()
{
    instance.loop();  // reads the trackball and GPS
    ui_input_tick();
    handle_serial();
    alarms_tick();
    gps_tick();
    weather_tick();
    map_download_tick();
    messenger_loop();
    update_screen_power();
    lv_timer_handler();
    delay(5);
}
