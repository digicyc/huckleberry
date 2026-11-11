#include "lora_app.h"
#include "messenger.h"
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The MeshCore app's region presets (api.meshcore.nz/api/v1/config, November 2026). Some also set
// the path hash size their mesh uses; 0 leaves it as it is.
struct Preset {
    const char *name;
    float freq, bw;
    uint8_t sf, cr, path_hash_bytes;
};
static const Preset PRESETS[] = {
    {"Australia", 915.800, 250, 10, 5, 0},
    {"Australia (Narrow)", 916.575, 62.5, 7, 7, 0},
    {"Australia (Mid)", 915.075, 125, 9, 5, 0},
    {"Australia: SA, WA", 923.125, 62.5, 8, 8, 0},
    {"Australia: QLD", 923.125, 62.5, 8, 5, 0},
    {"Brazil", 923.125, 62.5, 8, 8, 0},
    {"Canada", 910.525, 62.5, 7, 5, 3},
    {"Costa Rica", 910.525, 125, 11, 5, 0},
    {"EU/UK (Narrow)", 869.618, 62.5, 8, 8, 0},
    {"EU/UK (Deprecated)", 869.525, 250, 11, 5, 0},
    {"Czech Republic (Narrow)", 869.432, 62.5, 7, 5, 2},
    {"EU 433MHz (Long Range)", 433.650, 250, 11, 5, 0},
    {"EU 433MHz (Narrow)", 433.650, 62.5, 8, 8, 0},
    {"Hungary", 869.618, 62.5, 7, 5, 2},
    {"Netherlands", 869.618, 62.5, 7, 5, 0},
    {"Netherlands (Limburg)", 869.618, 62.5, 8, 8, 2},
    {"New Zealand (Narrow)", 917.375, 62.5, 7, 5, 2},
    {"New Zealand (Gisborne)", 917.375, 250, 11, 5, 1},
    {"Portugal 433", 433.375, 62.5, 9, 6, 0},
    {"Portugal 868", 869.618, 62.5, 7, 6, 0},
    {"Slovakia", 869.618, 62.5, 7, 5, 2},
    {"Switzerland", 869.618, 62.5, 8, 8, 0},
    {"Thailand", 921.000, 62.5, 7, 5, 0},
    {"USA", 910.525, 62.5, 7, 5, 0},
    {"USA - Southern California", 927.875, 62.5, 7, 5, 3},
    {"Vietnam (Narrow)", 920.250, 62.5, 8, 5, 0},
    {"Vietnam (Deprecated)", 920.250, 250, 11, 5, 0},
};
static const int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

static const float BANDWIDTHS[] = {7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500};
static const char *const BW_MAP[] = {"7.8", "10.4", "15.6", "20.8", "31.25", "\n", "41.7", "62.5", "125", "250", "500", ""};
static const char *const SF_MAP[] = {"5", "6", "7", "8", "9", "10", "11", "12", ""};
static const char *const CR_MAP[] = {"4/5", "4/6", "4/7", "4/8", ""};
static const char *const HASH_MAP[] = {"1 byte", "2 bytes", "3 bytes", ""};

static lv_obj_t *home_screen, *screen, *preset_screen, *preset_cont;
static lv_obj_t *preset_label, *freq_ta, *bw_bm, *sf_bm, *cr_bm, *tx_slider, *tx_label, *hash_bm, *acks_sw, *auto_sw;
static lv_obj_t *noise_label, *last_label, *packets_label, *airtime_label;
static RadioSettings settings;  // as shown

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.0005f;
}

// The first preset the settings match: its radio values, and its path hash size if it has one.
static int current_preset()
{
    for (int i = 0; i < NUM_PRESETS; i++) {
        const Preset &p = PRESETS[i];
        if (near(p.freq, settings.freq) && near(p.bw, settings.bw) && p.sf == settings.sf && p.cr == settings.cr &&
            (!p.path_hash_bytes || p.path_hash_bytes == settings.path_hash_bytes)) {
            return i;
        }
    }
    return -1;
}

static void set_switch(lv_obj_t *sw, bool on)
{
    lv_obj_set_state(sw, LV_STATE_CHECKED, on);
}

static void show_tx(int dbm)
{
    char text[16];
    snprintf(text, sizeof(text), "%d dBm", dbm);
    set_text(tx_label, text);
}

// Shows MeshCore's current settings, which the phone app may have changed.
static void refresh()
{
    if (!messenger_radio_settings(settings)) {
        return;
    }
    int preset = current_preset();
    set_text(preset_label, preset >= 0 ? PRESETS[preset].name : "Custom");
    char freq[16];
    snprintf(freq, sizeof(freq), "%.3f", (double)settings.freq);
    lv_textarea_set_text(freq_ta, freq);
    int bw = -1;
    for (int i = 0; i < (int)(sizeof(BANDWIDTHS) / sizeof(BANDWIDTHS[0])); i++) {
        if (fabsf(BANDWIDTHS[i] - settings.bw) < 0.01f) {
            bw = i;
        }
    }
    set_choice(bw_bm, bw);
    set_choice(sf_bm, settings.sf - 5);
    set_choice(cr_bm, settings.cr - 5);
    lv_slider_set_value(tx_slider, settings.tx_dbm, LV_ANIM_OFF);
    show_tx(settings.tx_dbm);
    set_choice(hash_bm, settings.path_hash_bytes - 1);
    set_switch(acks_sw, settings.multi_acks);
    set_switch(auto_sw, settings.auto_add);
}

static void apply()
{
    if (!messenger_set_radio_settings(settings)) {
        ui_toast("The radio can't use that setting");
    }
    refresh();
}

static void show_stats()
{
    RadioStats s;
    messenger_radio_stats(s);
    char text[40];
    snprintf(text, sizeof(text), "%d dBm", s.noise_floor);
    set_text(noise_label, text);
    if (s.received) {
        snprintf(text, sizeof(text), "%.0f dBm, SNR %.1f dB", (double)s.last_rssi, (double)s.last_snr);
    } else {
        strcpy(text, "None yet");
    }
    set_text(last_label, text);
    snprintf(text, sizeof(text), "%u in, %u out", (unsigned)s.received, (unsigned)s.sent);
    set_text(packets_label, text);
    snprintf(text, sizeof(text), "%um %02us", (unsigned)(s.airtime_s / 60), (unsigned)(s.airtime_s % 60));
    set_text(airtime_label, text);
}

// ---------- Callbacks ----------

static int selected(lv_event_t *e)
{
    return lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e));
}

static void bw_cb(lv_event_t *e)
{
    int i = selected(e);
    if (i >= 0) {
        settings.bw = BANDWIDTHS[i];
        apply();
    }
}

static void sf_cb(lv_event_t *e)
{
    if (selected(e) >= 0) {
        settings.sf = 5 + selected(e);
        apply();
    }
}

static void cr_cb(lv_event_t *e)
{
    if (selected(e) >= 0) {
        settings.cr = 5 + selected(e);
        apply();
    }
}

static void hash_cb(lv_event_t *e)
{
    if (selected(e) >= 0) {
        settings.path_hash_bytes = 1 + selected(e);
        apply();
    }
}

static void freq_cb(lv_event_t *)
{
    const char *text = lv_textarea_get_text(freq_ta);
    char *end;
    float freq = strtof(text, &end);
    if (end == text || *end) {
        ui_toast("Type the frequency in MHz, like 910.525");
        refresh();
        return;
    }
    settings.freq = freq;
    apply();
}

// Dragging shows the power; it's applied when the slider is let go.
static void tx_cb(lv_event_t *e)
{
    int dbm = lv_slider_get_value(tx_slider);
    show_tx(dbm);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED || !lv_obj_has_state(tx_slider, LV_STATE_PRESSED)) {
        settings.tx_dbm = dbm;
        apply();
    }
}

static void acks_cb(lv_event_t *)
{
    settings.multi_acks = lv_obj_has_state(acks_sw, LV_STATE_CHECKED);
    apply();
}

static void auto_cb(lv_event_t *)
{
    settings.auto_add = lv_obj_has_state(auto_sw, LV_STATE_CHECKED);
    apply();
}

static void back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

// ---------- Presets ----------

static void preset_back_cb(lv_event_t *)
{
    ui_close(screen);
}

static void preset_cb(lv_event_t *e)
{
    const Preset &p = PRESETS[(int)(intptr_t)lv_event_get_user_data(e)];
    settings.freq = p.freq;
    settings.bw = p.bw;
    settings.sf = p.sf;
    settings.cr = p.cr;
    if (p.path_hash_bytes) {
        settings.path_hash_bytes = p.path_hash_bytes;
    }
    apply();
    static char text[48];
    snprintf(text, sizeof(text), "Radio set to %s", p.name);
    ui_toast(text);
    ui_close(screen);
}

// One card per preset, with its values; the current one has a check mark.
static void build_preset_list()
{
    lv_obj_clean(preset_cont);
    int current = current_preset();
    for (int i = 0; i < NUM_PRESETS; i++) {
        const Preset &p = PRESETS[i];
        lv_obj_t *card = make_card(preset_cont, 44, preset_cb, (void *)(intptr_t)i);
        lv_obj_set_style_pad_ver(card, 3, 0);
        lv_obj_align(make_label(card, p.name, &lv_font_montserrat_16, lv_color_white()), LV_ALIGN_TOP_LEFT, 0, 0);
        char detail[64];
        int n = snprintf(detail, sizeof(detail), "%.3f MHz  BW %g  SF%d  CR%d", (double)p.freq, (double)p.bw, p.sf, p.cr);
        if (p.path_hash_bytes) {
            snprintf(detail + n, sizeof(detail) - n, "  %d-byte paths", p.path_hash_bytes);
        }
        lv_obj_align(make_label(card, detail, &lv_font_montserrat_12, GRAY), LV_ALIGN_BOTTOM_LEFT, 0, 0);
        if (i == current) {
            lv_obj_align(make_label(card, LV_SYMBOL_OK, &lv_font_montserrat_16, ORANGE), LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
}

static void preset_open_cb(lv_event_t *)
{
    build_preset_list();
    ui_open(preset_screen);
}

// ---------- Screens ----------

static lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = make_label(parent, text, &lv_font_montserrat_12, GRAY);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_margin_top(label, 4, 0);
    return label;
}

static lv_obj_t *note(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = make_label(parent, text, &lv_font_montserrat_12, GRAY);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    return label;
}

void lora_init(lv_obj_t *home)
{
    home_screen = home;
    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    make_title(screen, "LoRa");
    lv_obj_t *cont = make_scroll_column(screen);

    section(cont, "RADIO");
    lv_obj_t *card = make_card(cont, 34, preset_open_cb, NULL);
    lv_obj_set_style_pad_ver(card, 0, 0);
    lv_obj_align(make_label(card, "Preset", &lv_font_montserrat_14, GRAY), LV_ALIGN_LEFT_MID, 0, 0);
    preset_label = make_label(card, "", &lv_font_montserrat_16, lv_color_white());
    lv_label_set_long_mode(preset_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(preset_label, 200, 0);
    lv_obj_align(preset_label, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_align(make_label(card, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, GRAY), LV_ALIGN_RIGHT_MID, 0, 0);

    section(cont, "FREQUENCY, MHZ (ENTER TO SAVE)");
    freq_ta = make_text_field(cont, "910.525", 10);
    lv_textarea_set_accepted_chars(freq_ta, "0123456789.");
    lv_obj_add_event_cb(freq_ta, freq_cb, LV_EVENT_READY, NULL);

    bw_bm = make_choices(cont, "Bandwidth, kHz", BW_MAP, -1, bw_cb);
    sf_bm = make_choices(cont, "Spreading factor", SF_MAP, -1, sf_cb);
    cr_bm = make_choices(cont, "Coding rate", CR_MAP, -1, cr_cb);

    card = make_card(cont, 62, NULL, NULL);
    make_label(card, "Transmit power", &lv_font_montserrat_14, GRAY);
    tx_label = make_label(card, "", &lv_font_montserrat_14, lv_color_white());
    lv_obj_align(tx_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    tx_slider = lv_slider_create(card);
    lv_obj_set_size(tx_slider, LV_PCT(92), 8);
    lv_obj_align(tx_slider, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_slider_set_range(tx_slider, MIN_TX_DBM, MAX_TX_DBM);
    lv_obj_set_style_bg_color(tx_slider, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tx_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tx_slider, ORANGE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(tx_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(tx_slider, 5, LV_PART_KNOB);
    lv_obj_set_ext_click_area(tx_slider, 12);
    lv_obj_add_event_cb(tx_slider, tx_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(tx_slider, tx_cb, LV_EVENT_RELEASED, NULL);

    note(cont, "Nodes only hear each other with the same frequency, bandwidth, spreading factor and coding rate.");

    section(cont, "MESSAGES");
    hash_bm = make_choices(cont, "Path hash size", HASH_MAP, -1, hash_cb);
    note(cont, "Each repeater a message passes adds this many bytes to its path. Bigger hashes keep repeaters "
               "apart in busy meshes; floods then reach 64, 32 or 21 hops.");
    acks_sw = add_switch_row(cont, "Extra ACKs", "Send delivery receipts twice", false, acks_cb);
    auto_sw = add_switch_row(cont, "Auto-add contacts", "Add every node heard", false, auto_cb);

    section(cont, "ACTIVITY SINCE START");
    noise_label = add_value_row(cont, "Noise floor");
    last_label = add_value_row(cont, "Last heard");
    packets_label = add_value_row(cont, "Packets");
    airtime_label = add_value_row(cont, "Airtime");

    note(cont, "These are MeshCore's settings: the MeshCore app shows and changes the same ones.");

    preset_screen = make_screen();
    header_button(preset_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, preset_back_cb);
    make_title(preset_screen, "Presets");
    preset_cont = make_scroll_column(preset_screen);

    lv_timer_create([](lv_timer_t *) {
        if (lv_screen_active() == screen) {
            show_stats();
        }
    }, 1000, NULL);
}

void lora_open()
{
    if (!messenger_radio_ok()) {
        ui_toast("The LoRa radio didn't start");
        return;
    }
    refresh();
    show_stats();
    ui_open(screen);
}
