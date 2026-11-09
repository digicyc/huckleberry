#include "alarms.h"
#include "sound.h"
#include "ui.h"
#include <LilyGoLib.h>
#include <Preferences.h>

static const uint8_t MAX_ALARMS = 8;
static const time_t SNOOZE_SECONDS = 5 * 60;
static const uint32_t RING_TIMEOUT_MS = 60 * 1000;
static const lv_color_t RED = LV_COLOR_MAKE(0xF4, 0x43, 0x36);  // lv_palette_main(LV_PALETTE_RED)

// Bit n of `days` is weekday n (0 = Sunday, matching tm_wday).
static const uint8_t DAYS_ONCE = 0x00, DAYS_WEEKDAYS = 0x3E, DAYS_WEEKENDS = 0x41, DAYS_DAILY = 0x7F;

struct Alarm {
    uint8_t hour;  // 0-23, local time
    uint8_t minute;
    uint8_t enabled;
    uint8_t days;  // repeat days; DAYS_ONCE rings at the next occurrence, then disables itself
};

static Alarm alarms[MAX_ALARMS];
static uint8_t alarm_count;
static Preferences prefs;

static lv_obj_t *home_screen;
static lv_obj_t *list_screen, *list_cont, *add_btn;
static lv_obj_t *edit_screen, *edit_title, *hour_roller, *minute_roller, *ampm_roller, *delete_btn;
static lv_obj_t *day_buttons[7], *days_label;
static uint8_t edit_days;
static lv_obj_t *ring_screen, *ring_time_label, *ring_ampm_label;
static int editing = -1;  // index of the alarm in the editor, -1 for a new alarm

static bool ringing;
static uint8_t ring_hour, ring_minute;
static uint32_t ring_started_ms;
static time_t snooze_until;

static void rebuild_list();

static void save_alarms()
{
    // Keep alarms sorted by time of day.
    for (int i = 1; i < alarm_count; i++) {
        for (int j = i; j > 0 && alarms[j - 1].hour * 60 + alarms[j - 1].minute > alarms[j].hour * 60 + alarms[j].minute; j--) {
            Alarm tmp = alarms[j];
            alarms[j] = alarms[j - 1];
            alarms[j - 1] = tmp;
        }
    }
    if (alarm_count) {
        prefs.putBytes("list", alarms, alarm_count * sizeof(Alarm));
    } else {
        prefs.remove("list");
    }
}

static void load_alarms()
{
    if (prefs.isKey("list")) {
        alarm_count = min(prefs.getBytesLength("list") / sizeof(Alarm), (size_t)MAX_ALARMS);
        prefs.getBytes("list", alarms, alarm_count * sizeof(Alarm));
    }
}

static const char *days_summary(uint8_t days)
{
    static const char *names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static char buf[32];
    switch (days) {
    case DAYS_ONCE: return "Once";
    case DAYS_DAILY: return "Every day";
    case DAYS_WEEKDAYS: return "Weekdays";
    case DAYS_WEEKENDS: return "Weekends";
    }
    char *p = buf;
    for (int d = 0; d < 7; d++) {
        if (days & (1 << d)) {
            p += sprintf(p, p == buf ? "%s" : " %s", names[d]);
        }
    }
    return buf;
}

static int hour12(uint8_t hour)
{
    return hour % 12 ? hour % 12 : 12;
}

static lv_obj_t *pill_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_color_t fg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 140, 48);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_center(make_label(btn, text, &lv_font_montserrat_20, fg));
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

// ---------- Ringing ----------

static void start_ringing(uint8_t hour, uint8_t minute)
{
    ring_hour = hour;
    ring_minute = minute;
    lv_label_set_text_fmt(ring_time_label, "%d:%02d", hour12(hour), minute);
    lv_label_set_text(ring_ampm_label, hour < 12 ? "AM" : "PM");
    lv_obj_align_to(ring_ampm_label, ring_time_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -8);
    lv_screen_load(ring_screen);
    ring_started_ms = millis();
    snooze_until = 0;
    ringing = true;
    sound_set_alarm(true);
    Serial.printf("Alarm ringing: %02d:%02d\n", hour, minute);
}

static void stop_ringing()
{
    ringing = false;
    sound_set_alarm(false);
    lv_screen_load(home_screen);
}

static void snooze_cb(lv_event_t *)
{
    stop_ringing();
    snooze_until = time(NULL) + SNOOZE_SECONDS;
    Serial.println("Alarm snoozed");
}

static void stop_cb(lv_event_t *)
{
    stop_ringing();
    Serial.println("Alarm stopped");
}

void alarms_tick()
{
    time_t now = time(NULL);
    static time_t last_minute = now / 60;
    bool new_minute = now / 60 != last_minute;
    last_minute = now / 60;

    if (ringing) {
        lv_display_trigger_activity(NULL);  // keep the screen on
        if (millis() - ring_started_ms > RING_TIMEOUT_MS) {
            stop_ringing();
        }
        return;
    }

    if (snooze_until && now >= snooze_until) {
        start_ringing(ring_hour, ring_minute);
        return;
    }

    struct tm t;
    localtime_r(&now, &t);
    if (!new_minute || t.tm_year + 1900 < 2025) {
        return;  // the clock isn't set yet
    }
    for (int i = 0; i < alarm_count; i++) {
        Alarm &a = alarms[i];
        if (a.enabled && a.hour == t.tm_hour && a.minute == t.tm_min && (a.days == DAYS_ONCE || a.days & (1 << t.tm_wday))) {
            start_ringing(a.hour, a.minute);
            if (a.days == DAYS_ONCE) {
                a.enabled = 0;
                save_alarms();  // re-sorts, so `a` is not used after this
                rebuild_list();
            }
            return;
        }
    }
}

bool alarms_ringing()
{
    return ringing;
}

// ---------- Editor ----------

static void set_edit_days(uint8_t days)
{
    edit_days = days;
    for (int d = 0; d < 7; d++) {
        if (days & (1 << d)) {
            lv_obj_add_state(day_buttons[d], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(day_buttons[d], LV_STATE_CHECKED);
        }
    }
    lv_label_set_text(days_label, days_summary(days));
}

static void day_toggled_cb(lv_event_t *e)
{
    int day = (int)(intptr_t)lv_event_get_user_data(e);
    set_edit_days(edit_days ^ (1 << day));
}

static void days_preset_cb(lv_event_t *e)
{
    set_edit_days((uint8_t)(intptr_t)lv_event_get_user_data(e));
}

static void open_editor(int index)
{
    editing = index;
    uint8_t hour = index < 0 ? 7 : alarms[index].hour;
    uint8_t minute = index < 0 ? 0 : alarms[index].minute;
    lv_roller_set_selected(hour_roller, hour % 12, LV_ANIM_OFF);
    lv_roller_set_selected(minute_roller, minute, LV_ANIM_OFF);
    lv_roller_set_selected(ampm_roller, hour >= 12, LV_ANIM_OFF);
    set_edit_days(index < 0 ? DAYS_ONCE : alarms[index].days);
    lv_label_set_text(edit_title, index < 0 ? "New alarm" : "Edit alarm");
    if (index < 0) {
        lv_obj_add_flag(delete_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(delete_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_scroll_to_y(lv_obj_get_parent(lv_obj_get_parent(hour_roller)), 0, LV_ANIM_OFF);
    ui_open(edit_screen);
    ui_focus(hour_roller);
}

static void close_editor()
{
    rebuild_list();
    ui_close(list_screen);
}

static void editor_save_cb(lv_event_t *)
{
    int index = editing < 0 ? alarm_count++ : editing;
    alarms[index].hour = lv_roller_get_selected(hour_roller) + 12 * lv_roller_get_selected(ampm_roller);
    alarms[index].minute = lv_roller_get_selected(minute_roller);
    alarms[index].enabled = 1;
    alarms[index].days = edit_days;
    save_alarms();
    close_editor();
}

static void editor_cancel_cb(lv_event_t *)
{
    close_editor();
}

static void editor_delete_cb(lv_event_t *)
{
    for (int i = editing; i < alarm_count - 1; i++) {
        alarms[i] = alarms[i + 1];
    }
    alarm_count--;
    save_alarms();
    close_editor();
}

static lv_obj_t *make_roller(lv_obj_t *parent, const char *options, lv_roller_mode_t mode)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, mode);
    lv_obj_set_width(roller, 76);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_24, 0);
    lv_roller_set_visible_row_count(roller, 3);  // after the font, which sets the row height
    lv_obj_set_style_text_color(roller, GRAY, 0);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(roller, 0, 0);
    lv_obj_set_style_bg_color(roller, CARD, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, ORANGE, LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, 10, LV_PART_SELECTED);
    lv_obj_set_style_outline_width(roller, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(roller, ORANGE, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_radius(roller, 10, 0);
    return roller;
}

static lv_obj_t *flex_row(lv_obj_t *parent, int gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    return row;
}

static void build_editor()
{
    edit_screen = make_screen();
    header_button(edit_screen, LV_SYMBOL_CLOSE, LV_ALIGN_TOP_LEFT, editor_cancel_cb);
    header_button(edit_screen, LV_SYMBOL_OK, LV_ALIGN_TOP_RIGHT, editor_save_cb);
    edit_title = make_title(edit_screen, "");
    lv_obj_t *cont = make_scroll_column(edit_screen);

    static char hours[64], minutes[200];
    char *p = hours;
    for (int h = 0; h < 12; h++) {
        p += sprintf(p, h ? "\n%d" : "%d", h ? h : 12);
    }
    p = minutes;
    for (int m = 0; m < 60; m++) {
        p += sprintf(p, m ? "\n%02d" : "%02d", m);
    }

    lv_obj_t *row = flex_row(cont, 6);
    hour_roller = make_roller(row, hours, LV_ROLLER_MODE_INFINITE);
    make_label(row, ":", &lv_font_montserrat_24, lv_color_white());
    minute_roller = make_roller(row, minutes, LV_ROLLER_MODE_INFINITE);
    ampm_roller = make_roller(row, "AM\nPM", LV_ROLLER_MODE_NORMAL);

    // Repeat days: S M T W T F S toggles, a summary, and presets.
    lv_obj_t *days_row = flex_row(cont, 6);
    lv_obj_set_style_margin_top(days_row, 4, 0);
    static const char *letters[7] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int d = 0; d < 7; d++) {
        lv_obj_t *btn = lv_button_create(days_row);
        lv_obj_set_size(btn, 32, 32);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, CARD, 0);
        lv_obj_set_style_text_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_color(btn, ORANGE, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_black(), LV_STATE_CHECKED);
        lv_obj_t *letter = lv_label_create(btn);  // inherits the button's state-dependent text color
        lv_label_set_text(letter, letters[d]);
        lv_obj_set_style_text_font(letter, &lv_font_montserrat_16, 0);
        lv_obj_center(letter);
        lv_obj_add_event_cb(btn, day_toggled_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
        day_buttons[d] = btn;
    }

    days_label = make_label(cont, "", &lv_font_montserrat_14, GRAY);

    lv_obj_t *presets = flex_row(cont, 6);
    static const struct { const char *name; uint8_t days; } preset_list[] = {
        {"Weekdays", DAYS_WEEKDAYS}, {"Weekends", DAYS_WEEKENDS}, {"Daily", DAYS_DAILY},
    };
    for (const auto &preset : preset_list) {
        lv_obj_t *chip = lv_button_create(presets);
        lv_obj_set_height(chip, 30);
        lv_obj_set_style_pad_hor(chip, 10, 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(chip, 0, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chip, 2, 0);
        lv_obj_set_style_border_color(chip, lv_color_hex(0x3A3A3C), 0);
        lv_obj_center(make_label(chip, preset.name, &lv_font_montserrat_14, ORANGE));
        lv_obj_add_event_cb(chip, days_preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)preset.days);
    }

    delete_btn = make_wide_button(cont, "Delete alarm", false, editor_delete_cb, NULL);
    lv_obj_set_style_border_color(delete_btn, RED, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(delete_btn, 0), RED, 0);
    lv_obj_set_style_margin_top(delete_btn, 6, 0);
}

// ---------- List ----------

static void row_clicked_cb(lv_event_t *e)
{
    open_editor((int)(intptr_t)lv_event_get_user_data(e));
}

static void row_switch_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    alarms[index].enabled = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    save_alarms();
    rebuild_list();
}

static void add_cb(lv_event_t *)
{
    open_editor(-1);
}

static void back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

static void rebuild_list()
{
    lv_obj_clean(list_cont);
    if (alarm_count >= MAX_ALARMS) {
        lv_obj_add_flag(add_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(add_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (!alarm_count) {
        lv_obj_t *empty = make_label(list_cont, "No alarms\nPress + to add one", &lv_font_montserrat_20, GRAY);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_margin_top(empty, 40, 0);
        return;
    }
    for (int i = 0; i < alarm_count; i++) {
        lv_color_t color = alarms[i].enabled ? lv_color_white() : GRAY;
        lv_obj_t *row = make_card(list_cont, 58, row_clicked_cb, (void *)(intptr_t)i);

        lv_obj_t *time = make_label(row, "", &lv_font_montserrat_28, color);
        lv_label_set_text_fmt(time, "%d:%02d", hour12(alarms[i].hour), alarms[i].minute);
        lv_obj_align(time, LV_ALIGN_LEFT_MID, 0, -8);
        lv_obj_t *ampm = make_label(row, alarms[i].hour < 12 ? "AM" : "PM", &lv_font_montserrat_14, GRAY);
        lv_obj_align_to(ampm, time, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);
        lv_obj_t *days = make_label(row, days_summary(alarms[i].days), &lv_font_montserrat_12, GRAY);
        lv_label_set_long_mode(days, LV_LABEL_LONG_DOT);
        lv_obj_set_width(days, 210);
        lv_obj_align(days, LV_ALIGN_LEFT_MID, 0, 15);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 46, 24);
        lv_obj_set_style_bg_color(sw, ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (alarms[i].enabled) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(sw, row_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
}

static void build_list()
{
    list_screen = make_screen();
    header_button(list_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    add_btn = header_button(list_screen, LV_SYMBOL_PLUS, LV_ALIGN_TOP_RIGHT, add_cb);
    make_title(list_screen, "Alarms");
    list_cont = make_scroll_column(list_screen);
}

static void build_ring_screen()
{
    ring_screen = make_screen();
    lv_obj_t *title = make_label(ring_screen, LV_SYMBOL_BELL "  Alarm", &lv_font_montserrat_20, ORANGE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_H + 14);

    ring_time_label = make_label(ring_screen, "", &lv_font_montserrat_48, lv_color_white());
    lv_obj_align(ring_time_label, LV_ALIGN_CENTER, -16, -14);
    ring_ampm_label = make_label(ring_screen, "", &lv_font_montserrat_20, GRAY);

    lv_obj_t *snooze = pill_button(ring_screen, "Snooze", lv_color_hex(0x3A3A3C), lv_color_white(), snooze_cb);
    lv_obj_align(snooze, LV_ALIGN_BOTTOM_LEFT, 14, -16);
    lv_obj_t *stop = pill_button(ring_screen, "Stop", ORANGE, lv_color_black(), stop_cb);
    lv_obj_align(stop, LV_ALIGN_BOTTOM_RIGHT, -14, -16);
}

void alarms_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("alarms");
    load_alarms();

    build_list();
    build_editor();
    build_ring_screen();
    rebuild_list();
}

void alarms_open()
{
    ui_open(list_screen);
}
