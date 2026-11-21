#include "contacts_app.h"
#include "messages.h"
#include "messenger.h"
#include "ui.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

// Icons from tools/make_contact_icons.py, alpha-only and tinted white on a badge in the type's color.
LV_IMAGE_DECLARE(contact_icon_companion);
LV_IMAGE_DECLARE(contact_icon_repeater);
LV_IMAGE_DECLARE(contact_icon_room);
LV_IMAGE_DECLARE(contact_icon_sensor);

struct TypeLook {
    const char *name;
    const lv_image_dsc_t *icon;
    lv_color_t color;
};

// Companions purple and repeaters cyan, as on the map.
static const TypeLook LOOKS[] = {
    {"Node", &contact_icon_companion, LV_COLOR_MAKE(0x75, 0x75, 0x75)},
    {"Companion", &contact_icon_companion, LV_COLOR_MAKE(0xAB, 0x47, 0xBC)},
    {"Repeater", &contact_icon_repeater, LV_COLOR_MAKE(0x00, 0xAC, 0xC1)},
    {"Room server", &contact_icon_room, LV_COLOR_MAKE(0x43, 0xA0, 0x47)},
    {"Sensor", &contact_icon_sensor, LV_COLOR_MAKE(0xFB, 0x8C, 0x00)},
};

static const TypeLook &look(uint8_t type)
{
    return LOOKS[type < sizeof(LOOKS) / sizeof(LOOKS[0]) ? type : 0];
}

static const lv_color_t RED = LV_COLOR_MAKE(0xF4, 0x43, 0x36);  // lv_palette_main(LV_PALETTE_RED)

static lv_obj_t *home_screen, *list_screen, *list_cont, *edit_label, *detail_screen, *detail_title, *detail_cont;
static bool editing;  // the list shows a delete button on each contact
static ContactSummary *contacts;  // MAX_CONTACTS, most recently heard first
static int contact_count;
static ContactSummary shown;  // the contact on detail_screen

// A round badge in the type's color with its icon.
static lv_obj_t *make_badge(lv_obj_t *parent, uint8_t type, int size)
{
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, size, size);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, look(type).color, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *icon = lv_image_create(badge);
    lv_image_set_src(icon, look(type).icon);
    lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_center(icon);
    return badge;
}

static void format_route(const ContactSummary &c, char *out, size_t size)
{
    if (c.hops < 0) {
        snprintf(out, size, "Flood");
    } else if (c.hops == 0) {
        snprintf(out, size, "Direct");
    } else {
        snprintf(out, size, "%d hop%s", c.hops, c.hops == 1 ? "" : "s");
    }
}

// ---------- Contact ----------

static void rebuild_list();

static void detail_back_cb(lv_event_t *)
{
    rebuild_list();
    ui_close(list_screen);
}

static void message_cb(lv_event_t *)
{
    int chat = messenger_contact_chat(shown.key);
    if (chat >= 0) {
        messages_open_chat(chat);
    }
}

static void delete_cb(lv_event_t *)
{
    messenger_remove_contact(shown.key);
    rebuild_list();
    ui_close(list_screen);
}

static void open_detail(const ContactSummary &c)
{
    shown = c;
    set_text(detail_title, c.name);
    lv_obj_clean(detail_cont);

    lv_obj_t *head = make_card(detail_cont, 42, NULL, NULL);
    lv_obj_align(make_badge(head, c.type, 30), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *name = make_label(head, c.name, &lv_font_montserrat_16, lv_color_white());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, 230, lv_font_get_line_height(&lv_font_montserrat_16));
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 40, -2);
    lv_obj_align(make_label(head, look(c.type).name, &lv_font_montserrat_12, GRAY), LV_ALIGN_BOTTOM_LEFT, 40, 2);

    char text[40];
    format_route(c, text, sizeof(text));
    lv_label_set_text(add_value_row(detail_cont, "Route"), text);
    lv_label_set_text(add_value_row(detail_cont, "Last heard"), format_ago(c.last_heard));
    snprintf(text, sizeof(text), "%02X%02X%02X%02X%02X%02X", c.key[0], c.key[1], c.key[2], c.key[3], c.key[4], c.key[5]);
    lv_label_set_text(add_value_row(detail_cont, "ID"), text);
    if (c.has_location) {
        snprintf(text, sizeof(text), "%.5f, %.5f", c.lat, c.lon);
        lv_label_set_text(add_value_row(detail_cont, "Position"), text);
    }
    if (c.type == CONTACT_CHAT) {
        make_wide_button(detail_cont, LV_SYMBOL_ENVELOPE "  Message", true, message_cb, NULL);
    }
    make_wide_button(detail_cont, LV_SYMBOL_TRASH "  Delete contact", false, delete_cb, NULL);

    lv_obj_scroll_to_y(detail_cont, 0, LV_ANIM_OFF);
    ui_open(detail_screen);
}

static void build_detail()
{
    detail_screen = make_screen();
    header_button(detail_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, detail_back_cb);
    detail_title = make_title(detail_screen, "");
    detail_cont = make_scroll_column(detail_screen);
}

// ---------- List ----------

static void contact_cb(lv_event_t *e)
{
    open_detail(contacts[(int)(intptr_t)lv_event_get_user_data(e)]);
}

static void row_delete_cb(lv_event_t *e)
{
    messenger_remove_contact(contacts[(int)(intptr_t)lv_event_get_user_data(e)].key);
    int32_t y = lv_obj_get_scroll_y(list_cont);
    rebuild_list();
    lv_obj_update_layout(list_cont);
    lv_obj_scroll_to_y(list_cont, y, LV_ANIM_OFF);  // stays put for deleting the next one
}

static void add_row(int i)
{
    const ContactSummary &c = contacts[i];
    lv_obj_t *card = make_card(list_cont, 42, contact_cb, (void *)(intptr_t)i);
    lv_obj_set_style_pad_ver(card, 3, 0);
    lv_obj_align(make_badge(card, c.type, 28), LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *name = make_label(card, c.name, &lv_font_montserrat_16, lv_color_white());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, editing ? 200 : 180, lv_font_get_line_height(&lv_font_montserrat_16));
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 38, 0);

    char detail[48];
    format_route(c, detail, sizeof(detail));
    if (c.has_location) {
        strlcat(detail, "  " LV_SYMBOL_GPS, sizeof(detail));
    }
    lv_obj_align(make_label(card, detail, &lv_font_montserrat_12, GRAY), LV_ALIGN_BOTTOM_LEFT, 38, 0);
    if (!editing) {
        lv_obj_align(make_label(card, format_ago(c.last_heard), &lv_font_montserrat_12, GRAY), LV_ALIGN_TOP_RIGHT, 0, 2);
        return;
    }
    lv_obj_t *del = lv_button_create(card);
    lv_obj_set_size(del, 40, 30);
    lv_obj_set_style_radius(del, 8, 0);
    lv_obj_set_style_bg_color(del, RED, 0);
    lv_obj_set_style_shadow_width(del, 0, 0);
    lv_obj_center(make_label(del, LV_SYMBOL_TRASH, &lv_font_montserrat_16, lv_color_white()));
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, 4, 0);
    lv_obj_add_event_cb(del, row_delete_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
}

static void add_section(const char *title, bool (*in_section)(uint8_t type))
{
    int count = 0;
    for (int i = 0; i < contact_count; i++) {
        count += in_section(contacts[i].type);
    }
    if (!count) {
        return;
    }
    char text[40];
    snprintf(text, sizeof(text), "%s  %d", title, count);
    lv_obj_t *label = make_label(list_cont, text, &lv_font_montserrat_12, GRAY);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_margin_top(label, 4, 0);
    for (int i = 0; i < contact_count; i++) {
        if (in_section(contacts[i].type)) {
            add_row(i);
        }
    }
}

static void rebuild_list()
{
    lv_obj_clean(list_cont);
    contact_count = messenger_contacts(contacts, MAX_CONTACTS);
    char text[96];
    if (contact_count >= MAX_CONTACTS) {
        snprintf(text, sizeof(text), "%d of %d: full. New nodes aren't added until you delete some (" LV_SYMBOL_TRASH ").",
                 contact_count, MAX_CONTACTS);
    } else {
        snprintf(text, sizeof(text), "%d of %d contacts", contact_count, MAX_CONTACTS);
    }
    lv_obj_t *count = make_label(list_cont, text, &lv_font_montserrat_12, contact_count >= MAX_CONTACTS ? ORANGE : GRAY);
    lv_label_set_long_mode(count, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(count, LV_PCT(100));
    add_section("COMPANIONS", [](uint8_t t) { return t == CONTACT_CHAT; });
    add_section("REPEATERS", [](uint8_t t) { return t == CONTACT_REPEATER; });
    add_section("ROOMS AND SENSORS", [](uint8_t t) { return t != CONTACT_CHAT && t != CONTACT_REPEATER; });
    if (!contact_count) {
        lv_obj_t *note = make_label(list_cont, "No contacts yet. MeshCore nodes are added here when they advertise.",
                                    &lv_font_montserrat_12, GRAY);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note, LV_PCT(100));
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_margin_top(note, 30, 0);
    }
}

static void set_editing(bool on)
{
    editing = on;
    lv_label_set_text(edit_label, on ? LV_SYMBOL_OK : LV_SYMBOL_TRASH);
}

static void edit_cb(lv_event_t *)
{
    set_editing(!editing);
    int32_t y = lv_obj_get_scroll_y(list_cont);
    rebuild_list();
    lv_obj_update_layout(list_cont);
    lv_obj_scroll_to_y(list_cont, y, LV_ANIM_OFF);
}

static void list_back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

void contacts_init(lv_obj_t *home)
{
    home_screen = home;
    contacts = (ContactSummary *)ps_malloc(MAX_CONTACTS * sizeof(ContactSummary));
    list_screen = make_screen();
    header_button(list_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, list_back_cb);
    edit_label = lv_obj_get_child(header_button(list_screen, LV_SYMBOL_TRASH, LV_ALIGN_TOP_RIGHT, edit_cb), 0);
    make_title(list_screen, "Contacts");
    list_cont = make_scroll_column(list_screen);
    build_detail();
}

void contacts_open()
{
    set_editing(false);
    rebuild_list();
    lv_obj_scroll_to_y(list_cont, 0, LV_ANIM_OFF);
    ui_open(list_screen);
}
