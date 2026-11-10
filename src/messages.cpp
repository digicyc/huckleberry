#include "messages.h"
#include "messenger.h"
#include "settings_app.h"
#include "sound.h"
#include "ui.h"
#include <Arduino.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const int MAX_LISTED_CHATS = 48;     // messenger.cpp keeps at most this many
static const int MAX_LISTED_CONTACTS = 128;  // MAX_CONTACTS
static const uint32_t BANNER_MS = 4000;
static const lv_color_t RED = LV_COLOR_MAKE(0xF4, 0x43, 0x36);  // lv_palette_main(LV_PALETTE_RED)
static const int COMPOSE_H = 36;
static const int CHAT_SHOWN = 20;  // newest messages built when a chat opens; building all 60 took 0.7 s

static lv_obj_t *home_screen;
static lv_obj_t *list_screen, *list_cont;
static lv_obj_t *chat_screen, *chat_title, *chat_cont, *compose_ta;
static lv_obj_t *info_screen, *info_title, *info_cont;
static lv_obj_t *new_screen, *new_cont;
static lv_obj_t *form_screen, *form_title, *form_label[2], *form_ta[2], *form_note, *form_button;
static void (*form_submit)(const char *, const char *);
static lv_obj_t *node_screen, *node_name_ta, *node_share_sw;
static lv_obj_t *banner, *banner_title, *banner_text;
static lv_timer_t *banner_timer;
static int banner_chat = -1;
static int open_chat = -1;  // chat shown on chat_screen and info_screen
// The chat view's bubbles are the newest shown_count messages as of shown_total (messenger's count
// of messages ever added), after the "Show earlier" button if there is one.
static int shown_count;
static uint32_t shown_total;
static bool shown_all;
static lv_obj_t *earlier_button;
static ContactSummary *contacts;  // MAX_LISTED_CONTACTS, for new_screen

static void open_chat_view(int chat);
static void rebuild_list();

// "3:42 PM" for today, "Oct 28" for earlier days, "" if the clock isn't set.
static const char *format_time(uint32_t t)
{
    static char buf[16];
    time_t when = t, now = time(NULL);
    struct tm w, n;
    localtime_r(&when, &w);
    localtime_r(&now, &n);
    if (w.tm_year + 1900 < 2025) {
        return "";
    }
    if (w.tm_year == n.tm_year && w.tm_yday == n.tm_yday) {
        snprintf(buf, sizeof(buf), "%d:%02d %s", w.tm_hour % 12 ? w.tm_hour % 12 : 12, w.tm_min, w.tm_hour < 12 ? "AM" : "PM");
    } else {
        strftime(buf, sizeof(buf), "%b %d", &w);
    }
    return buf;
}

// "just now", "5 min ago", "3 h ago", "2 d ago".
static const char *format_ago(uint32_t t)
{
    static char buf[16];
    long ago = (long)(time(NULL) - t);
    if (t < 1735689600 || ago < 0) {
        return "";
    }
    if (ago < 60) {
        return "just now";
    }
    if (ago < 3600) {
        snprintf(buf, sizeof(buf), "%ld min ago", ago / 60);
    } else if (ago < 86400) {
        snprintf(buf, sizeof(buf), "%ld h ago", ago / 3600);
    } else {
        snprintf(buf, sizeof(buf), "%ld d ago", ago / 86400);
    }
    return buf;
}

static lv_obj_t *section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = make_label(parent, text, &lv_font_montserrat_12, GRAY);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_margin_top(label, 4, 0);
    return label;
}

static lv_obj_t *note_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *note = make_label(parent, text, &lv_font_montserrat_12, GRAY);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    return note;
}

// ---------- Chat list ----------

static void chat_row_clicked_cb(lv_event_t *e)
{
    open_chat_view((int)(intptr_t)lv_event_get_user_data(e));
}

static void node_open_cb(lv_event_t *);

static void rebuild_list()
{
    lv_obj_clean(list_cont);
    int ids[MAX_LISTED_CHATS];
    int n = messenger_chats(ids, MAX_LISTED_CHATS);
    bool any_dm = false;
    for (int i = 0; i < n; i++) {
        ChatSummary s;
        if (!messenger_chat_summary(ids[i], s)) {
            continue;
        }
        any_dm |= !s.is_channel;
        lv_obj_t *row = make_card(list_cont, 46, chat_row_clicked_cb, (void *)(intptr_t)ids[i]);
        lv_obj_set_style_pad_ver(row, 4, 0);

        lv_obj_t *name = make_label(row, s.name, &lv_font_montserrat_16, s.is_channel ? ORANGE : lv_color_white());
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_size(name, 200, lv_font_get_line_height(&lv_font_montserrat_16));
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *when = make_label(row, s.last_message ? format_time(s.last_activity) : "", &lv_font_montserrat_12, GRAY);
        lv_obj_align(when, LV_ALIGN_TOP_RIGHT, 0, 2);

        char preview[MAX_CHAT_TEXT + 40];
        const ChatMessage *m = s.last_message;
        if (!m) {
            strcpy(preview, s.is_channel ? "Group channel" : "No messages yet");
        } else if (m->status != MSG_RECEIVED) {
            snprintf(preview, sizeof(preview), "You: %s", m->text);
        } else if (s.is_channel && m->sender[0]) {
            snprintf(preview, sizeof(preview), "%s: %s", m->sender, m->text);
        } else {
            strlcpy(preview, m->text, sizeof(preview));
        }
        lv_obj_t *text = make_label(row, preview, &lv_font_montserrat_14, s.unread ? lv_color_white() : GRAY);
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_obj_set_size(text, s.unread ? 240 : 276, lv_font_get_line_height(&lv_font_montserrat_14));  // one line
        lv_obj_align(text, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        if (s.unread) {
            lv_obj_t *badge = lv_obj_create(row);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, LV_SIZE_CONTENT, 18);
            lv_obj_set_style_min_width(badge, 18, 0);
            lv_obj_set_style_pad_hor(badge, 5, 0);
            lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(badge, ORANGE, 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *count = make_label(badge, "", &lv_font_montserrat_12, lv_color_black());
            lv_label_set_text_fmt(count, "%d", s.unread);
            lv_obj_center(count);
            lv_obj_align(badge, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        }
    }
    if (!any_dm) {
        note_label(list_cont, "People appear when nearby MeshCore companions advertise. Tap + to message one, "
                              "join a group, or advertise this T-Deck.");
    }

    // This node's name as others see it; tap for name, location sharing and adverts.
    lv_obj_t *me = make_card(list_cont, 44, node_open_cb, NULL);
    lv_obj_set_style_bg_opa(me, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(me, 2, 0);
    lv_obj_set_style_border_color(me, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_pad_ver(me, 2, 0);
    lv_obj_t *caption = make_label(me, "This T-Deck", &lv_font_montserrat_12, GRAY);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *name = make_label(me, messenger_node_name(), &lv_font_montserrat_16, lv_color_white());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, 240, lv_font_get_line_height(&lv_font_montserrat_16));
    lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *edit = make_label(me, LV_SYMBOL_EDIT, &lv_font_montserrat_16, ORANGE);
    lv_obj_align(edit, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void list_back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

static void rebuild_new();

static void new_open_cb(lv_event_t *)
{
    rebuild_new();
    lv_obj_scroll_to_y(new_cont, 0, LV_ANIM_OFF);
    ui_open(new_screen);
}

static void build_list()
{
    list_screen = make_screen();
    header_button(list_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, list_back_cb);
    header_button(list_screen, LV_SYMBOL_PLUS, LV_ALIGN_TOP_RIGHT, new_open_cb);
    make_title(list_screen, "Messages");
    list_cont = make_scroll_column(list_screen);
}

// ---------- Chat view ----------

static const char *status_text(uint8_t status)
{
    switch (status) {
    case MSG_QUEUED: return "Sending...";
    case MSG_SENT: return "Sent";
    case MSG_DELIVERED: return "Delivered " LV_SYMBOL_OK;
    case MSG_FAILED: return "Failed";
    }
    return "";
}

// The line under a bubble: the time, and for our own messages the delivery status.
static void format_meta(const ChatMessage &m, char *meta, size_t size)
{
    const char *when = format_time(m.time);
    if (m.status != MSG_RECEIVED) {
        snprintf(meta, size, "%s%s%s", when, when[0] ? "  " : "", status_text(m.status));
    } else {
        strlcpy(meta, when, size);
    }
}

static void add_bubble(const ChatMessage &m, bool is_channel)
{
    bool mine = m.status != MSG_RECEIVED;

    lv_obj_t *item = lv_obj_create(chat_cont);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_flex_align_t side = mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START;
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, side, side);
    lv_obj_set_style_pad_row(item, 2, 0);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);

    if (!mine && is_channel && m.sender[0]) {
        lv_obj_t *sender = make_label(item, m.sender, &lv_font_montserrat_12, ORANGE);
        lv_obj_set_style_margin_left(sender, 8, 0);
    }

    lv_obj_t *bubble = lv_obj_create(item);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(bubble, 10, 0);
    lv_obj_set_style_pad_ver(bubble, 6, 0);
    lv_obj_set_style_radius(bubble, 12, 0);
    lv_obj_set_style_bg_color(bubble, mine ? ORANGE : CARD, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *text = make_label(bubble, m.text, &lv_font_montserrat_16, mine ? lv_color_black() : lv_color_white());
    lv_obj_set_style_max_width(text, 220, 0);

    char meta[48];
    format_meta(m, meta, sizeof(meta));
    if (meta[0]) {
        lv_obj_t *label = make_label(item, meta, &lv_font_montserrat_12, m.status == MSG_FAILED ? RED : GRAY);
        lv_obj_set_style_margin_hor(label, 8, 0);
        lv_obj_set_user_data(item, label);  // for update_chat() to show Delivered, say
    }
}

static void rebuild_chat();

static void earlier_cb(lv_event_t *)
{
    // After this event: rebuilding deletes the button.
    lv_async_call([](void *) {
        int first = messenger_message_count(open_chat) - shown_count;  // the oldest message shown now
        shown_all = true;
        rebuild_chat();
        lv_obj_update_layout(chat_cont);
        if (lv_obj_t *oldest = lv_obj_get_child(chat_cont, first)) {
            lv_obj_scroll_to_view(oldest, LV_ANIM_OFF);
        }
    }, NULL);
}

static void add_earlier_button()
{
    earlier_button = make_wide_button(chat_cont, "Show earlier messages", false, earlier_cb, NULL);
}

static void rebuild_chat()
{
    lv_obj_clean(chat_cont);
    ChatSummary s;
    if (!messenger_chat_summary(open_chat, s)) {
        return;
    }
    lv_label_set_text(chat_title, s.name);
    int count = messenger_message_count(open_chat);
    if (!count) {
        lv_obj_t *empty = make_label(chat_cont,
                                     messenger_is_public(open_chat) ? "No messages yet.\nEveryone nearby can read this channel."
                                     : s.is_channel                 ? "No messages yet.\nEveryone with this channel can read it."
                                                                    : "No messages yet",
                                     &lv_font_montserrat_14, GRAY);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_margin_top(empty, 40, 0);
    }
    shown_count = shown_all || count < CHAT_SHOWN ? count : CHAT_SHOWN;
    shown_total = messenger_message_total(open_chat);
    earlier_button = NULL;
    if (count > shown_count) {
        add_earlier_button();
    }
    for (int i = count - shown_count; i < count; i++) {
        const ChatMessage *m = messenger_message(open_chat, i);
        if (m) {
            add_bubble(*m, s.is_channel);
        }
    }
    lv_obj_update_layout(chat_cont);
    lv_obj_scroll_to_y(chat_cont, LV_COORD_MAX, LV_ANIM_OFF);
}

// Brings the open chat's view up to date, adding just the new bubbles when it can.
static void update_chat()
{
    ChatSummary s;
    uint32_t total = messenger_message_total(open_chat);
    int count = messenger_message_count(open_chat);
    int added = (int)(total - shown_total);
    if (!messenger_chat_summary(open_chat, s) || shown_count == 0 || added < 0 || added > CHAT_SHOWN) {
        rebuild_chat();
        return;
    }
    lv_label_set_text(chat_title, s.name);
    bool at_bottom = lv_obj_get_scroll_bottom(chat_cont) < 20;
    for (int i = count - added; i < count; i++) {
        add_bubble(*messenger_message(open_chat, i), s.is_channel);
    }
    shown_count += added;
    shown_total = total;
    // Drop the oldest bubbles past the window, or past what the messenger still keeps.
    int keep = shown_all ? count : CHAT_SHOWN;
    while (shown_count > keep) {
        lv_obj_delete(lv_obj_get_child(chat_cont, earlier_button ? 1 : 0));
        shown_count--;
    }
    if (!earlier_button && count > shown_count) {
        add_earlier_button();
        lv_obj_move_to_index(earlier_button, 0);
    }
    // Our recent messages' Sending... / Delivered / Failed.
    int first = earlier_button ? 1 : 0;
    for (int j = shown_count - 1; j >= 0 && j >= shown_count - 8; j--) {
        const ChatMessage *m = messenger_message(open_chat, count - shown_count + j);
        lv_obj_t *meta = (lv_obj_t *)lv_obj_get_user_data(lv_obj_get_child(chat_cont, first + j));
        if (m && meta && m->status != MSG_RECEIVED) {
            char text[48];
            format_meta(*m, text, sizeof(text));
            set_text(meta, text);
            lv_obj_set_style_text_color(meta, m->status == MSG_FAILED ? RED : GRAY, 0);
        }
    }
    if (at_bottom) {
        lv_obj_update_layout(chat_cont);
        lv_obj_scroll_to_y(chat_cont, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void open_chat_view(int chat)
{
    ChatSummary s;
    if (!messenger_chat_summary(chat, s)) {
        return;
    }
    open_chat = chat;
    shown_all = false;
    messenger_mark_read(chat);
    lv_textarea_set_text(compose_ta, "");
    lv_textarea_set_max_length(compose_ta, messenger_max_text(chat));
    lv_textarea_set_placeholder_text(compose_ta, s.is_channel ? "Message the group" : "Message");
    rebuild_chat();
    rebuild_list();
    if (lv_screen_active() != chat_screen) {
        ui_open(chat_screen);
    }
    ui_focus(compose_ta);
}

static void chat_back_cb(lv_event_t *)
{
    ui_close(list_screen);
}

static void send_cb(lv_event_t *)
{
    const char *text = lv_textarea_get_text(compose_ta);
    while (*text == ' ') {
        text++;
    }
    if (!*text) {
        return;
    }
    if (messenger_send(open_chat, text)) {
        lv_textarea_set_text(compose_ta, "");
        lv_obj_scroll_to_x(compose_ta, 0, LV_ANIM_OFF);  // a long message left it scrolled to its end
    } else {
        ui_toast("Couldn't send");
    }
}

static void rebuild_info();

static void info_open_cb(lv_event_t *)
{
    rebuild_info();
    ui_open(info_screen);
}

static void build_chat()
{
    chat_screen = make_screen();
    header_button(chat_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, chat_back_cb);
    header_button(chat_screen, LV_SYMBOL_LIST, LV_ALIGN_TOP_RIGHT, info_open_cb);
    chat_title = make_title(chat_screen, "");

    chat_cont = make_scroll_column(chat_screen);
    lv_obj_set_height(chat_cont, CONTENT_H - COMPOSE_H - 8);
    lv_obj_set_flex_align(chat_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_bottom(chat_cont, 4, 0);

    compose_ta = make_text_field(chat_screen, "Message", MAX_CHAT_TEXT);
    lv_obj_set_size(compose_ta, SCREEN_W - 20 - 46, COMPOSE_H);
    lv_obj_set_style_pad_ver(compose_ta, 8, 0);
    lv_obj_align(compose_ta, LV_ALIGN_BOTTOM_LEFT, 10, -4);
    lv_obj_add_event_cb(compose_ta, send_cb, LV_EVENT_READY, NULL);

    lv_obj_t *send = lv_button_create(chat_screen);
    lv_obj_set_size(send, 40, COMPOSE_H);
    lv_obj_set_style_radius(send, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(send, ORANGE, 0);
    lv_obj_set_style_shadow_width(send, 0, 0);
    lv_obj_center(make_label(send, LV_SYMBOL_UP, &lv_font_montserrat_16, lv_color_black()));
    lv_obj_align(send, LV_ALIGN_BOTTOM_RIGHT, -10, -4);
    lv_obj_add_event_cb(send, send_cb, LV_EVENT_CLICKED, NULL);
}

// ---------- Chat info ----------

static void info_back_cb(lv_event_t *)
{
    ui_close(chat_screen);
}

static void clear_cb(lv_event_t *)
{
    messenger_clear_chat(open_chat);
    ui_toast("History cleared");
    ui_close(chat_screen);
}

static void leave_cb(lv_event_t *)
{
    messenger_remove_channel(open_chat);
    open_chat = -1;
    rebuild_list();
    ui_close(list_screen);
}

static uint8_t info_contact_key[6];

static void delete_contact_cb(lv_event_t *)
{
    messenger_remove_contact(info_contact_key);
    open_chat = -1;
    rebuild_list();
    ui_close(list_screen);
}

static void rebuild_info()
{
    lv_obj_clean(info_cont);
    ChatSummary s;
    if (!messenger_chat_summary(open_chat, s)) {
        return;
    }
    lv_label_set_text(info_title, s.name);
    if (s.is_channel) {
        bool is_public = messenger_is_public(open_chat);
        lv_label_set_text(add_value_row(info_cont, "Type"), is_public ? "Public channel" : s.name[0] == '#' ? "Hashtag channel" : "Private group");
        if (!is_public) {
            char key[33];
            messenger_channel_key(open_chat, key);
            lv_obj_t *card = make_card(info_cont, LV_SIZE_CONTENT, NULL, NULL);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            make_label(card, "Key", &lv_font_montserrat_14, GRAY);
            // Two lines of two 8-digit groups, as it doesn't fit on one.
            char shown[40];
            snprintf(shown, sizeof(shown), "%.8s %.8s\n%.8s %.8s", key, key + 8, key + 16, key + 24);
            lv_obj_t *value = make_label(card, shown, &lv_font_montserrat_18, lv_color_white());
            lv_obj_set_style_text_letter_space(value, 1, 0);
            note_label(info_cont, s.name[0] == '#' ? "Anyone who joins the same #name can read this channel."
                                                   : "Share the group name and key so others can join.");
        } else {
            note_label(info_cont, "Every MeshCore node nearby can read the Public channel.");
        }
    } else {
        ContactSummary c;
        if (messenger_chat_contact(open_chat, c)) {
            memcpy(info_contact_key, c.key, 6);
            char route[24];
            if (c.hops < 0) {
                strcpy(route, "Flood");
            } else if (c.hops == 0) {
                strcpy(route, "Direct");
            } else {
                snprintf(route, sizeof(route), "%d hop%s", c.hops, c.hops == 1 ? "" : "s");
            }
            lv_label_set_text(add_value_row(info_cont, "Route"), route);
            lv_label_set_text(add_value_row(info_cont, "Last heard"), format_ago(c.last_heard));
            char id[20];
            snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X", c.key[0], c.key[1], c.key[2], c.key[3], c.key[4], c.key[5]);
            lv_label_set_text(add_value_row(info_cont, "ID"), id);
        }
    }
    make_wide_button(info_cont, "Clear history", false, clear_cb, NULL);
    if (s.is_channel && !messenger_is_public(open_chat)) {
        make_wide_button(info_cont, "Leave group", false, leave_cb, NULL);
    } else if (!s.is_channel) {
        make_wide_button(info_cont, "Delete contact", false, delete_contact_cb, NULL);
    }
}

static void build_info()
{
    info_screen = make_screen();
    header_button(info_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, info_back_cb);
    info_title = make_title(info_screen, "");
    info_cont = make_scroll_column(info_screen);
}

// ---------- Form (join or create a group) ----------

static void form_back_cb(lv_event_t *)
{
    ui_close(new_screen);
}

static void form_submit_cb(lv_event_t *)
{
    form_submit(lv_textarea_get_text(form_ta[0]), lv_textarea_get_text(form_ta[1]));
}

static void form_next_cb(lv_event_t *)
{
    if (lv_obj_has_flag(form_ta[1], LV_OBJ_FLAG_HIDDEN)) {
        form_submit_cb(NULL);
    } else {
        ui_focus(form_ta[1]);
    }
}

static void build_form()
{
    form_screen = make_screen();
    header_button(form_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, form_back_cb);
    form_title = make_title(form_screen, "");
    lv_obj_t *cont = make_scroll_column(form_screen);
    for (int i = 0; i < 2; i++) {
        form_label[i] = section_label(cont, "");
        form_ta[i] = make_text_field(cont, "", MAX_CHANNEL_NAME);
    }
    lv_obj_add_event_cb(form_ta[0], form_next_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(form_ta[1], form_submit_cb, LV_EVENT_READY, NULL);
    form_note = note_label(cont, "");
    form_button = make_wide_button(cont, "", true, form_submit_cb, NULL);
}

static void create_submit(const char *text, const char *);

// Shows the form with one field, or two when label2 isn't NULL.
static void form_open(const char *title, const char *label1, const char *placeholder1, const char *label2,
                      const char *placeholder2, uint32_t max2, const char *note, void (*submit)(const char *, const char *))
{
    lv_label_set_text(form_title, title);
    lv_label_set_text(form_label[0], label1);
    lv_textarea_set_placeholder_text(form_ta[0], placeholder1);
    lv_textarea_set_text(form_ta[0], "");
    for (lv_obj_t *obj : {form_label[1], form_ta[1]}) {
        if (label2) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (label2) {
        lv_label_set_text(form_label[1], label2);
        lv_textarea_set_placeholder_text(form_ta[1], placeholder2);
        lv_textarea_set_max_length(form_ta[1], max2);
    }
    lv_textarea_set_text(form_ta[1], "");
    lv_label_set_text(form_note, note);
    lv_label_set_text(lv_obj_get_child(form_button, 0), submit == create_submit ? "Create" : "Join");
    form_submit = submit;
    ui_open(form_screen);
    ui_focus(form_ta[0]);
}

static void trim(const char *in, char *out, size_t size)
{
    while (*in == ' ') {
        in++;
    }
    strlcpy(out, in, size);
    for (size_t len = strlen(out); len && out[len - 1] == ' '; len--) {
        out[len - 1] = '\0';
    }
}

static void joined(int chat)
{
    if (chat < 0) {
        ui_toast("No free channel slot; leave a group first");
        return;
    }
    rebuild_list();
    open_chat_view(chat);
}

static void hashtag_submit(const char *text, const char *)
{
    char name[MAX_CHANNEL_NAME + 1] = "#";
    trim(text[0] == '#' ? text + 1 : text, name + 1, sizeof(name) - 1);
    for (char *p = name; *p; p++) {
        if (*p == ' ') {
            *p = '-';  // hashtags have no spaces
        }
    }
    if (!name[1]) {
        return;
    }
    joined(messenger_add_channel(name, NULL));
}

static void create_submit(const char *text, const char *)
{
    char name[MAX_CHANNEL_NAME + 1];
    trim(text[0] == '#' ? text + 1 : text, name, sizeof(name));  // '#' would make it a hashtag channel
    if (!name[0]) {
        return;
    }
    int chat = messenger_add_channel(name, NULL);
    joined(chat);
    if (chat >= 0) {
        rebuild_info();
        ui_open(info_screen);  // show the key to share
    }
}

static void join_private_submit(const char *text, const char *key)
{
    char name[MAX_CHANNEL_NAME + 1], hex[40];
    trim(text[0] == '#' ? text + 1 : text, name, sizeof(name));
    trim(key, hex, sizeof(hex));
    if (!name[0]) {
        ui_focus(form_ta[0]);
        return;
    }
    if (strlen(hex) != 32) {
        ui_toast("The key is 32 hex digits (0-9, a-f)");
        ui_focus(form_ta[1]);
        return;
    }
    int chat = messenger_add_channel(name, hex);
    if (chat < 0 && strspn(hex, "0123456789abcdefABCDEF") != 32) {
        ui_toast("The key is 32 hex digits (0-9, a-f)");
        return;
    }
    joined(chat);
}

// ---------- New conversation ----------

static void new_back_cb(lv_event_t *)
{
    ui_close(list_screen);
}

static void hashtag_cb(lv_event_t *)
{
    form_open("Hashtag channel", "Channel name", "#hiking", NULL, NULL, 0,
              "Anyone who joins the same #name can read and post. The key comes from the name.", hashtag_submit);
}

static void create_cb(lv_event_t *)
{
    form_open("New private group", "Group name", "Family", NULL, NULL, 0,
              "A new random key is made; share the name and key so others can join.", create_submit);
}

static void join_cb(lv_event_t *)
{
    form_open("Join private group", "Group name", "Family", "Key", "32 hex digits", 32,
              "Get the key from someone in the group, or from the MeshCore app's channel settings.", join_private_submit);
}

static void contact_cb(lv_event_t *e)
{
    const ContactSummary &c = contacts[(int)(intptr_t)lv_event_get_user_data(e)];
    int chat = messenger_contact_chat(c.key);
    if (chat < 0) {
        ui_toast("Use the MeshCore app for repeaters and rooms");
        return;
    }
    rebuild_list();
    open_chat_view(chat);
}

static void advertise()
{
    messenger_send_advert();
    static char text[64];
    snprintf(text, sizeof(text), "Advertised as %s", messenger_node_name());
    ui_toast(text);
}

static void advert_cb(lv_event_t *)
{
    advertise();
}

static void rebuild_new()
{
    lv_obj_clean(new_cont);
    section_label(new_cont, "GROUPS");
    lv_obj_t *card;
    card = make_card(new_cont, 34, hashtag_cb, NULL);
    lv_obj_align(make_label(card, "#  Join a hashtag channel", &lv_font_montserrat_16, lv_color_white()), LV_ALIGN_LEFT_MID, 0, 0);
    card = make_card(new_cont, 34, create_cb, NULL);
    lv_obj_align(make_label(card, LV_SYMBOL_PLUS "  Create a private group", &lv_font_montserrat_16, lv_color_white()),
                 LV_ALIGN_LEFT_MID, 0, 0);
    card = make_card(new_cont, 34, join_cb, NULL);
    lv_obj_align(make_label(card, LV_SYMBOL_DOWNLOAD "  Join a private group", &lv_font_montserrat_16, lv_color_white()),
                 LV_ALIGN_LEFT_MID, 0, 0);

    section_label(new_cont, "PEOPLE AND NODES HEARD");
    static const char *const TYPES[] = {"", "Companion", "Repeater", "Room", "Sensor"};
    int n = messenger_contacts(contacts, MAX_LISTED_CONTACTS);
    for (int i = 0; i < n; i++) {
        const ContactSummary &c = contacts[i];
        bool chat = c.type == CONTACT_CHAT;
        card = make_card(new_cont, 42, contact_cb, (void *)(intptr_t)i);
        lv_obj_set_style_pad_ver(card, 3, 0);
        lv_obj_t *name = make_label(card, c.name, &lv_font_montserrat_16, chat ? lv_color_white() : GRAY);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_size(name, 220, lv_font_get_line_height(&lv_font_montserrat_16));
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
        char detail[64];
        char hops[16] = "";
        if (c.hops == 0) {
            strcpy(hops, " · direct");
        } else if (c.hops > 0) {
            snprintf(hops, sizeof(hops), " · %d hop%s", c.hops, c.hops == 1 ? "" : "s");
        }
        snprintf(detail, sizeof(detail), "%s%s%s", c.type < 5 ? TYPES[c.type] : "Node", hops,
                 c.has_location ? " · " LV_SYMBOL_GPS : "");
        lv_obj_align(make_label(card, detail, &lv_font_montserrat_12, GRAY), LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_align(make_label(card, format_ago(c.last_heard), &lv_font_montserrat_12, GRAY), LV_ALIGN_TOP_RIGHT, 0, 2);
    }
    if (!n) {
        note_label(new_cont, "No one heard yet. Nodes show up here when they advertise.");
    }
    lv_obj_t *btn = make_wide_button(new_cont, LV_SYMBOL_WIFI "  Advertise this T-Deck", false, advert_cb, NULL);
    lv_obj_set_style_margin_top(btn, 4, 0);
}

static void build_new()
{
    new_screen = make_screen();
    header_button(new_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, new_back_cb);
    make_title(new_screen, "New conversation");
    new_cont = make_scroll_column(new_screen);
}

// ---------- This node ----------

static void node_back_cb(lv_event_t *)
{
    rebuild_list();
    ui_close(list_screen);
}

static void node_name_cb(lv_event_t *)
{
    char name[MAX_NODE_NAME + 1];
    trim(lv_textarea_get_text(node_name_ta), name, sizeof(name));
    if (!name[0] || strcmp(name, messenger_node_name()) == 0) {
        return;
    }
    messenger_set_node_name(name);
    rebuild_list();
    advertise();  // so other nodes pick up the new name
}

static void share_cb(lv_event_t *)
{
    messenger_set_share_location(lv_obj_has_state(node_share_sw, LV_STATE_CHECKED));
}

static void node_open_cb(lv_event_t *)
{
    lv_textarea_set_text(node_name_ta, messenger_node_name());
    if (messenger_share_location()) {
        lv_obj_add_state(node_share_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(node_share_sw, LV_STATE_CHECKED);
    }
    ui_open(node_screen);
}

static void build_node()
{
    node_screen = make_screen();
    header_button(node_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, node_back_cb);
    make_title(node_screen, "This T-Deck");
    lv_obj_t *cont = make_scroll_column(node_screen);
    section_label(cont, "NAME OTHERS SEE (ENTER TO SAVE)");
    node_name_ta = make_text_field(cont, "Name", MAX_NODE_NAME);
    lv_obj_add_event_cb(node_name_ta, node_name_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(node_name_ta, node_name_cb, LV_EVENT_DEFOCUSED, NULL);
    node_share_sw = add_switch_row(cont, "Share my location", "GPS position in adverts", false, share_cb);
    make_wide_button(cont, LV_SYMBOL_WIFI "  Advertise now", false, advert_cb, NULL);
    note_label(cont, "An advert lets nearby MeshCore nodes add this T-Deck as a contact.");
}

// ---------- Notifications ----------

static void hide_banner()
{
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(banner_timer);
}

static void banner_cb(lv_event_t *)
{
    hide_banner();
    if (banner_chat >= 0) {
        open_chat_view(banner_chat);
    }
}

static void show_banner(int chat)
{
    ChatSummary s;
    if (!messenger_chat_summary(chat, s) || !s.last_message) {
        return;
    }
    banner_chat = chat;
    char title[48];
    if (s.is_channel && s.last_message->sender[0]) {
        snprintf(title, sizeof(title), "%s  ·  %s", s.last_message->sender, s.name);
    } else {
        strlcpy(title, s.name, sizeof(title));
    }
    lv_label_set_text(banner_title, title);
    lv_label_set_text(banner_text, s.last_message->text);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(banner);
    lv_timer_reset(banner_timer);
    lv_timer_resume(banner_timer);
}

static void build_banner()
{
    banner = lv_obj_create(lv_layer_top());
    lv_obj_set_size(banner, SCREEN_W - 16, 46);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, STATUS_H + 2);
    lv_obj_set_style_radius(banner, 12, 0);
    lv_obj_set_style_bg_color(banner, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_width(banner, 1, 0);
    lv_obj_set_style_border_color(banner, ORANGE, 0);
    lv_obj_set_style_pad_hor(banner, 10, 0);
    lv_obj_set_style_pad_ver(banner, 4, 0);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(banner, banner_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon = make_label(banner, LV_SYMBOL_ENVELOPE, &lv_font_montserrat_16, ORANGE);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    banner_title = make_label(banner, "", &lv_font_montserrat_14, lv_color_white());
    lv_label_set_long_mode(banner_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(banner_title, SCREEN_W - 70, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_obj_align(banner_title, LV_ALIGN_TOP_LEFT, 26, 0);
    banner_text = make_label(banner, "", &lv_font_montserrat_14, GRAY);
    lv_label_set_long_mode(banner_text, LV_LABEL_LONG_DOT);
    lv_obj_set_size(banner_text, SCREEN_W - 70, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_obj_align(banner_text, LV_ALIGN_BOTTOM_LEFT, 26, 0);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    banner_timer = lv_timer_create([](lv_timer_t *) { hide_banner(); }, BANNER_MS, NULL);
    lv_timer_pause(banner_timer);
}

// ---------- Setup ----------

static void messenger_changed(int chat, bool incoming)
{
    lv_obj_t *active = lv_screen_active();
    if (chat < 0) {  // contacts changed
        if (active == new_screen) {
            int32_t y = lv_obj_get_scroll_y(new_cont);
            rebuild_new();
            lv_obj_update_layout(new_cont);
            lv_obj_scroll_to_y(new_cont, y, LV_ANIM_OFF);
        }
        return;
    }
    bool viewing = chat == open_chat && active == chat_screen;
    if (incoming) {
        if (settings_message_sound()) {
            sound_chime();
        }
        lv_display_trigger_activity(NULL);  // wakes the screen and restarts its timeout
        if (!viewing) {
            show_banner(chat);
        }
    }
    if (chat == open_chat) {
        ChatSummary s;
        if (!messenger_chat_summary(chat, s)) {  // removed, say from the phone app
            open_chat = -1;
            if (active == chat_screen || active == info_screen) {
                ui_close(list_screen);
            }
        } else {
            if (viewing) {
                messenger_mark_read(chat);
            }
            update_chat();
        }
    }
    rebuild_list();
}

void messages_init(lv_obj_t *home)
{
    home_screen = home;
    contacts = (ContactSummary *)ps_malloc(MAX_LISTED_CONTACTS * sizeof(ContactSummary));
    build_list();
    build_chat();
    build_info();
    build_form();
    build_new();
    build_node();
    build_banner();
    rebuild_list();
    messenger_set_listener(messenger_changed);
}

void messages_open()
{
    rebuild_list();
    lv_obj_scroll_to_y(list_cont, 0, LV_ANIM_OFF);
    ui_open(list_screen);
}
