#include "settings_app.h"
#include "ui.h"
#include <LilyGoLib.h>
#include <Preferences.h>

static const int MIN_LEVEL = 1, MAX_LEVEL = DEVICE_MAX_BRIGHTNESS_LEVEL;  // backlight levels
static const uint8_t KEYBOARD_LIGHT = 127;

static const uint16_t TIMEOUTS_S[] = {15, 30, 60, 120, 300, 600};
static const char *const TIMEOUT_MAP[] = {"15s", "30s", "1m", "2m", "5m", "10m", ""};
static const int NUM_TIMEOUTS = sizeof(TIMEOUTS_S) / sizeof(TIMEOUTS_S[0]);
static const int DEFAULT_TIMEOUT = 2;  // 1 min

static Preferences prefs;
static int brightness;
static int timeout_index;
static bool message_sound, keyboard_light;
static lv_obj_t *home_screen, *screen, *slider, *value_label, *battery_label;

uint8_t settings_brightness()
{
    return brightness;
}

uint32_t settings_screen_timeout_ms()
{
    return TIMEOUTS_S[timeout_index] * 1000;
}

bool settings_message_sound()
{
    return message_sound;
}

uint8_t settings_keyboard_light()
{
    return keyboard_light ? KEYBOARD_LIGHT : 0;
}

static void show_value()
{
    lv_label_set_text_fmt(value_label, "%d%%", (brightness * 100 + MAX_LEVEL / 2) / MAX_LEVEL);
}

// Dragging (or the trackball) changes the brightness live; it's saved when it settles.
static void slider_cb(lv_event_t *e)
{
    brightness = lv_slider_get_value(slider);
    show_value();
    instance.setBrightness(brightness);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_DEFOCUSED ||
        !lv_obj_has_state(slider, LV_STATE_PRESSED)) {
        prefs.putUChar("bright", brightness);
    }
}

static void timeout_cb(lv_event_t *e)
{
    timeout_index = lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e));
    prefs.putUShort("timeout", TIMEOUTS_S[timeout_index]);
}

static void sound_cb(lv_event_t *e)
{
    message_sound = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    prefs.putBool("msg_sound", message_sound);
}

static void keyboard_cb(lv_event_t *e)
{
    keyboard_light = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    prefs.putBool("kb_light", keyboard_light);
    instance.kb.setBrightness(settings_keyboard_light());
}

static void show_battery()
{
    char text[32];
    snprintf(text, sizeof(text), "%d%%  (%.2f V)", (int)instance.getBatteryPercent(), instance.getBattVoltage() / 1000);
    set_text(battery_label, text);
}

static lv_obj_t *titled_card(lv_obj_t *parent, const char *title, int height)
{
    lv_obj_t *card = make_card(parent, height, NULL, NULL);
    lv_obj_t *name = make_label(card, title, &lv_font_montserrat_16, lv_color_white());
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
    return card;
}

static void back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

void settings_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("settings");
    brightness = constrain(prefs.getUChar("bright", 12), MIN_LEVEL, MAX_LEVEL);
    message_sound = prefs.getBool("msg_sound", true);
    keyboard_light = prefs.getBool("kb_light", true);
    uint16_t timeout_s = prefs.getUShort("timeout", TIMEOUTS_S[DEFAULT_TIMEOUT]);
    timeout_index = DEFAULT_TIMEOUT;
    for (int i = 0; i < NUM_TIMEOUTS; i++) {
        if (TIMEOUTS_S[i] == timeout_s) {
            timeout_index = i;
        }
    }

    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    make_title(screen, "Settings");
    lv_obj_t *cont = make_scroll_column(screen);

    lv_obj_t *card = titled_card(cont, "Brightness", 62);
    value_label = make_label(card, "", &lv_font_montserrat_14, GRAY);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, 0, 1);
    slider = lv_slider_create(card);
    lv_obj_set_size(slider, LV_PCT(92), 8);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_slider_set_range(slider, MIN_LEVEL, MAX_LEVEL);
    lv_slider_set_value(slider, brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, ORANGE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 12);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_RELEASED, NULL);
    show_value();

    // Screen timeout: one of a few fixed times.
    card = titled_card(cont, "Screen timeout", 70);
    lv_obj_t *times = lv_buttonmatrix_create(card);
    lv_buttonmatrix_set_map(times, TIMEOUT_MAP);
    lv_buttonmatrix_set_button_ctrl_all(times, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(times, true);
    lv_buttonmatrix_set_button_ctrl(times, timeout_index, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(times, LV_PCT(100), 30);
    lv_obj_align(times, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(times, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(times, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(times, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(times, 4, LV_PART_MAIN);
    lv_obj_set_style_text_font(times, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_text_color(times, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(times, lv_color_hex(0x3A3A3C), LV_PART_ITEMS);
    lv_obj_set_style_radius(times, 8, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(times, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(times, ORANGE, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(times, lv_color_black(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(times, timeout_cb, LV_EVENT_VALUE_CHANGED, NULL);
    ui_focusable(times);

    // New messages always wake the screen; this only silences the chime.
    add_switch_row(cont, "Message sound", "A chime for new messages", message_sound, sound_cb);
    add_switch_row(cont, "Keyboard light", "While the screen is on", keyboard_light, keyboard_cb);

    battery_label = add_value_row(cont, "Battery");
    lv_obj_t *about = make_label(cont, "HuckleBerry for LilyGo T-Deck", &lv_font_montserrat_12, GRAY);
    lv_obj_set_style_margin_top(about, 4, 0);

    lv_timer_create([](lv_timer_t *) {
        if (lv_screen_active() == screen) {
            show_battery();
        }
    }, 2000, NULL);
}

void settings_open()
{
    show_battery();
    ui_open(screen);
}
