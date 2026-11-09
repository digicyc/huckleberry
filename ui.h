// Shared look and input for app screens on the T-Deck's 320x240 screen: black screens, gray
// secondary text, dark cards, orange header buttons, and a status bar along the top of every screen.
//
// Input: the touch screen works as usual. The trackball moves a focus outline between the buttons
// and rows on screen in the direction it rolls (or scrolls when there's nothing that way), a click
// presses the focused item, and holding it goes home. The keyboard types into the focused text
// field (or the screen's text field, if it has one); Enter presses the focused item, and the mic key
// or Backspace outside a text field goes back. On a roller, up and down turn it and a click moves on.
#pragma once
#include <lvgl.h>

static const int SCREEN_W = 320, SCREEN_H = 240;
static const int STATUS_H = 20;               // status bar, drawn over every screen
static const int HEADER_H = 36;               // app title row under it
static const int CONTENT_Y = STATUS_H + HEADER_H;
static const int CONTENT_H = SCREEN_H - CONTENT_Y;

static const lv_color_t GRAY = LV_COLOR_MAKE(0x9E, 0x9E, 0x9E);
static const lv_color_t CARD = LV_COLOR_MAKE(0x1C, 0x1C, 0x1E);
static const lv_color_t ORANGE = LV_COLOR_MAKE(0xFF, 0x98, 0x00);  // lv_palette_main(LV_PALETTE_ORANGE)

lv_obj_t *make_screen();
lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

// The screen's title, centered in the header row.
lv_obj_t *make_title(lv_obj_t *screen, const char *text);

// A text-only button at the left or right end of the header row. The top-left one is the screen's
// back button, which the mic key and Backspace also press.
lv_obj_t *header_button(lv_obj_t *parent, const char *text, lv_align_t align, lv_event_cb_t cb);

// A vertically scrolling flex column filling the screen below the header.
lv_obj_t *make_scroll_column(lv_obj_t *screen);

// A full-width tappable card in a column, `height` px tall (LV_SIZE_CONTENT to fit).
lv_obj_t *make_card(lv_obj_t *parent, int32_t height, lv_event_cb_t cb, void *user_data);

// A one-line text field on a dark card. Enter sends LV_EVENT_READY.
lv_obj_t *make_text_field(lv_obj_t *parent, const char *placeholder, uint32_t max_length);

// A one-line row on a card: a caption on the left and a value on the right. Returns the value label.
lv_obj_t *add_value_row(lv_obj_t *parent, const char *caption);

// A row with a title (and an optional gray note under it) and an on/off switch on the right.
// Returns the switch; `cb` gets LV_EVENT_VALUE_CHANGED.
lv_obj_t *add_switch_row(lv_obj_t *parent, const char *title, const char *note, bool on, lv_event_cb_t cb);

// A full-width button: orange outline (or filled when `primary`).
lv_obj_t *make_wide_button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, void *user_data);

// Sets a label's text only when it changes, so unchanged labels aren't redrawn.
void set_text(lv_obj_t *label, const char *text);

// Slides an app screen in from the right, or back to `to` (the previous screen) to the right.
void ui_open(lv_obj_t *screen);
void ui_close(lv_obj_t *to);

// A short message over the bottom of whichever screen is showing.
void ui_toast(const char *text);

// ---------- Input ----------

// Starts the trackball and keyboard input; call after beginLvglHelper().
void ui_input_init(void (*go_home)());

// Lets the trackball focus a custom clickable object (cards are already focusable).
void ui_focusable(lv_obj_t *obj);

// Moves the focus to `obj` now or, if it isn't on screen yet, once it is.
void ui_focus(lv_obj_t *obj);

// Presses the active screen's back button.
void ui_back();

// Gives a screen its own trackball and key handling instead of moving the focus (the map pans and
// zooms with them). `on_ball` gets each trackball step (dx or dy is +-1); `on_key` gets keys
// (LV_KEY_ENTER for a trackball click) and returns true if it used the key.
void ui_set_screen_input(lv_obj_t *screen, void (*on_ball)(int dx, int dy), bool (*on_key)(uint32_t key));

// Call every loop: handles the trackball and keyboard. Returns true if there was input.
bool ui_input_tick();

// True while the screen is off; input then only wakes it.
void ui_set_screen_off(bool off);

// For testing over USB serial: queue a key, roll the trackball (dx/dy steps), click it, or tap the
// screen at x, y.
void ui_inject_key(uint32_t key);
void ui_inject_trackball(int dx, int dy);
void ui_inject_click(bool long_press);
void ui_inject_tap(int x, int y);
