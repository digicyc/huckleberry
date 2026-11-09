#include "bluetooth_app.h"
#include "messenger.h"
#include "ui.h"
#include <stdio.h>

static const lv_color_t GREEN = LV_COLOR_MAKE(0x4C, 0xAF, 0x50);  // lv_palette_main(LV_PALETTE_GREEN)

static lv_obj_t *home_screen, *screen, *power_switch, *status_label, *name_label, *pin_label;

static void update_screen()
{
    bool on = messenger_bt_enabled(), connected = messenger_bt_connected();
    set_text(status_label, !on ? "Off" : connected ? "Phone connected" : "Waiting for phone");
    lv_obj_set_style_text_color(status_label, connected ? GREEN : GRAY, 0);
    set_text(name_label, messenger_bt_name());
    char pin[8];
    snprintf(pin, sizeof(pin), "%06u", (unsigned)messenger_bt_pin());
    set_text(pin_label, pin);
}

static void switch_cb(lv_event_t *)
{
    messenger_bt_set_enabled(lv_obj_has_state(power_switch, LV_STATE_CHECKED));
    update_screen();
}

static void back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

void bluetooth_init(lv_obj_t *home)
{
    home_screen = home;
    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    make_title(screen, "Bluetooth");
    lv_obj_t *cont = make_scroll_column(screen);

    power_switch = add_switch_row(cont, "Phone link", "MeshCore app companion", messenger_bt_enabled(), switch_cb);
    status_label = add_value_row(cont, "Status");
    name_label = add_value_row(cont, "Name");
    pin_label = add_value_row(cont, "PIN");
    lv_obj_set_style_text_font(pin_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(pin_label, 3, 0);

    lv_obj_t *note = make_label(cont, "In the MeshCore app, tap Connect, choose this T-Deck and enter the PIN. "
                                      "Messages it sends also show in Messages here.",
                                &lv_font_montserrat_12, GRAY);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);

    update_screen();
    lv_timer_create([](lv_timer_t *) {
        if (lv_screen_active() == screen) {
            update_screen();
        }
    }, 500, NULL);
}

void bluetooth_open()
{
    update_screen();
    ui_open(screen);
}
