#include "wifi_app.h"
#include "ui.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <algorithm>
#include <atomic>
#include <vector>

static const uint32_t JOIN_TIMEOUT_MS = 20000;
static const uint32_t SCAN_RETRY_MS = 10000;  // scans fail while the radio is busy connecting; retry this long
static const int MAX_NETWORKS = 20;
static const lv_color_t GREEN = LV_COLOR_MAKE(0x4C, 0xAF, 0x50);   // lv_palette_main(LV_PALETTE_GREEN)
static const lv_color_t RED = LV_COLOR_MAKE(0xF4, 0x43, 0x36);     // lv_palette_main(LV_PALETTE_RED)
static const lv_color_t DIM = LV_COLOR_MAKE(0x5A, 0x5A, 0x5A);

struct WifiNetwork {
    String ssid;
    int rssi;
    bool secure;
};

static Preferences prefs;
static bool wifi_on;
static String saved_ssid, saved_pass;

// A join in progress: credentials are saved only once it connects.
static bool joining;
static String join_ssid, join_pass;
static uint32_t join_started;
static std::atomic<uint8_t> disconnect_reason{0};  // last join failure, set from the Wi-Fi event task
static String message;                             // last join result, shown under the status

// Internet time: SNTP starts when Wi-Fi connects and re-syncs hourly.
static bool sntp_running;
static std::atomic<bool> time_synced{false};  // set from the SNTP task
static time_t last_sync;                      // 0 if never; saved so it shows after a reboot

static std::vector<WifiNetwork> networks;
static bool scan_wanted, scanning, scan_failed;
static uint32_t scan_wanted_since;

static lv_obj_t *home_screen, *screen, *power_switch, *status_label, *detail_label, *list_cont;
static lv_obj_t *sync_row, *sync_label;
static lv_obj_t *pw_screen, *pw_title, *pw_ta, *pw_hint;
static String pw_ssid;  // network the password screen is for
static String shown_state;  // what list_cont was last built from

// Join failures worth reporting; other disconnect reasons follow them within milliseconds (201 is
// followed by 36, for example), so only these are kept.
static const char *failure_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "Wrong password";
    case WIFI_REASON_NO_AP_FOUND:
        return "Network not found";
    }
    return NULL;
}

static bool connected()
{
    return WiFi.status() == WL_CONNECTED;
}

// ---------- Radio ----------

static void radio_on()
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    if (saved_ssid.length()) {
        WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());
    }
}

static void request_scan()
{
    scan_wanted = true;
    scan_failed = false;
    scan_wanted_since = millis();
}

static void join(const String &ssid, const String &pass)
{
    joining = true;
    join_ssid = ssid;
    join_pass = pass;
    join_started = millis();
    disconnect_reason = 0;
    message = "";
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());
}

static void join_failed(const char *why)
{
    joining = false;
    message = String(why) + ": " + join_ssid;  // reason first; a long name gets the "..."
    WiFi.disconnect();
    if (saved_ssid.length() && saved_ssid != join_ssid) {
        WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());  // back to the network that worked
    }
}

static void start_clock_sync()
{
    char tz[64];  // configTzTime sets TZ; pass the current zone so it stays
    const char *current = getenv("TZ");
    strlcpy(tz, current ? current : "UTC0", sizeof(tz));
    configTzTime(tz, "pool.ntp.org", "time.google.com", "time.nist.gov");
    sntp_running = true;
}

static void stop_clock_sync()
{
    if (sntp_running) {
        esp_sntp_stop();
        sntp_running = false;
    }
}

// Runs every 500 ms whether or not the screen is showing: finishes joins and scans, and keeps the
// clock in sync.
static void tick()
{
    if (!wifi_on) {
        return;
    }
    if (connected() && !sntp_running) {
        start_clock_sync();
    }
    if (time_synced.exchange(false)) {
        time_t now = time(NULL);
        last_sync = now;
        prefs.putUInt("synced", (uint32_t)now);
    }
    if (joining) {
        const char *failure = failure_text(disconnect_reason);
        if (connected() && WiFi.SSID() == join_ssid) {
            joining = false;
            saved_ssid = join_ssid;
            saved_pass = join_pass;
            prefs.putString("ssid", saved_ssid);
            prefs.putString("pass", saved_pass);
        } else if (failure) {
            join_failed(failure);
        } else if (millis() - join_started > JOIN_TIMEOUT_MS) {
            join_failed("Timed out");
        }
    }
    if (scanning) {
        int16_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            return;
        }
        scanning = false;
        scan_failed = n < 0;
        networks.clear();
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            if (!ssid.length()) {
                continue;  // hidden network
            }
            auto same = std::find_if(networks.begin(), networks.end(), [&](const WifiNetwork &x) { return x.ssid == ssid; });
            if (same == networks.end()) {
                networks.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
            } else {
                same->rssi = std::max(same->rssi, (int)WiFi.RSSI(i));  // several access points, one network
            }
        }
        WiFi.scanDelete();
        std::sort(networks.begin(), networks.end(), [](const WifiNetwork &a, const WifiNetwork &b) { return a.rssi > b.rssi; });
        if (networks.size() > MAX_NETWORKS) {
            networks.resize(MAX_NETWORKS);
        }
    } else if (scan_wanted && !joining) {
        if (WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING) {
            scan_wanted = false;
            scanning = true;
        } else if (millis() - scan_wanted_since > SCAN_RETRY_MS) {
            scan_wanted = false;
            scan_failed = true;
        }
    }
}

// ---------- Main screen ----------

static void open_password(const String &ssid);

static void network_clicked_cb(lv_event_t *e)
{
    size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (i >= networks.size()) {
        return;
    }
    const WifiNetwork &n = networks[i];
    if (connected() && WiFi.SSID() == n.ssid) {
        return;
    }
    if (n.ssid == saved_ssid) {
        join(n.ssid, saved_pass);
    } else if (!n.secure) {
        join(n.ssid, "");
    } else {
        open_password(n.ssid);
    }
}

static void forget_cb(lv_event_t *)
{
    prefs.remove("ssid");
    prefs.remove("pass");
    saved_ssid = saved_pass = "";
    joining = false;
    message = "";
    WiFi.disconnect();
}

// Signal bars, 1-4, for an RSSI in dBm.
static int signal_level(int rssi)
{
    return rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
}

int wifi_signal_level()
{
    return wifi_on && connected() ? signal_level(WiFi.RSSI()) : 0;
}

// Four signal bars, lit by strength.
static void add_signal_bars(lv_obj_t *parent, int rssi)
{
    int lit = signal_level(rssi);
    lv_obj_t *bars = lv_obj_create(parent);
    lv_obj_remove_style_all(bars);
    lv_obj_set_size(bars, 4 * 4 + 3 * 2, 16);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(bars, 2, 0);
    lv_obj_remove_flag(bars, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(bars);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 4, 4 * (i + 1));
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_color(bar, i < lit ? lv_color_white() : DIM, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
    lv_obj_align(bars, LV_ALIGN_RIGHT_MID, 0, 0);
}

static lv_obj_t *add_note(const char *text)
{
    lv_obj_t *note = make_label(list_cont, text, &lv_font_montserrat_14, GRAY);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(note, 6, 0);
    return note;
}

static void rebuild_list()
{
    lv_obj_clean(list_cont);
    if (!wifi_on) {
        return;
    }
    String current = connected() ? WiFi.SSID() : "";
    if (saved_ssid.length()) {
        lv_obj_t *row = make_card(list_cont, 44, NULL, NULL);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_t *caption = make_label(row, "Saved network", &lv_font_montserrat_12, GRAY);
        lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *name = make_label(row, saved_ssid.c_str(), &lv_font_montserrat_16, lv_color_white());
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_size(name, 200, lv_font_get_line_height(&lv_font_montserrat_16));
        lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_t *forget = lv_button_create(row);
        lv_obj_set_style_bg_color(forget, lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_shadow_width(forget, 0, 0);
        lv_obj_set_style_radius(forget, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(forget, 12, 0);
        lv_obj_set_style_pad_ver(forget, 5, 0);
        make_label(forget, "Forget", &lv_font_montserrat_14, RED);
        lv_obj_align(forget, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(forget, forget_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *head = make_label(list_cont, "NETWORKS", &lv_font_montserrat_12, GRAY);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_style_margin_top(head, 4, 0);
    for (size_t i = 0; i < networks.size(); i++) {
        const WifiNetwork &n = networks[i];
        lv_obj_t *row = make_card(list_cont, 40, network_clicked_cb, (void *)(intptr_t)i);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_t *name = make_label(row, n.ssid.c_str(), &lv_font_montserrat_16, lv_color_white());
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_size(name, 240, lv_font_get_line_height(&lv_font_montserrat_16));
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
        bool is_current = n.ssid == current, is_joining = joining && n.ssid == join_ssid;
        const char *kind = is_current ? "Connected" : is_joining ? "Joining..." : n.ssid == saved_ssid ? "Saved"
                         : n.secure ? LV_SYMBOL_EYE_CLOSE " Secured" : "Open";
        lv_obj_t *sub = make_label(row, kind, &lv_font_montserrat_12, is_current ? GREEN : GRAY);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        add_signal_bars(row, n.rssi);
    }
    if (scanning || scan_wanted) {
        add_note("Scanning...");
    } else if (scan_failed) {
        add_note("Couldn't scan. Tap " LV_SYMBOL_REFRESH " to try again.");
    } else if (networks.empty()) {
        add_note("No networks found. Tap " LV_SYMBOL_REFRESH " to scan again.");
    }
}

// "3:42 PM" for today, "Oct 28, 3:42 PM" for earlier days.
static String format_sync_time(time_t t)
{
    time_t now = time(NULL);
    struct tm when, today;
    localtime_r(&t, &when);
    localtime_r(&now, &today);
    char buf[32];
    int n = 0;
    if (when.tm_year != today.tm_year || when.tm_yday != today.tm_yday) {
        n = strftime(buf, sizeof(buf), "%b %d, ", &when);
    }
    snprintf(buf + n, sizeof(buf) - n, "%d:%02d %s", when.tm_hour % 12 ? when.tm_hour % 12 : 12, when.tm_min,
             when.tm_hour < 12 ? "AM" : "PM");
    return buf;
}

static void update_screen()
{
    if (!wifi_on) {
        set_text(status_label, "Off");
        set_text(detail_label, "");
    } else if (joining) {
        set_text(status_label, ("Joining " + join_ssid + "...").c_str());
        set_text(detail_label, "");
    } else if (connected()) {
        set_text(status_label, ("Connected to " + WiFi.SSID()).c_str());
        set_text(detail_label, (WiFi.localIP().toString() + "   " + String(WiFi.RSSI()) + " dBm").c_str());
    } else {
        set_text(status_label, saved_ssid.length() ? ("Looking for " + saved_ssid + "...").c_str() : "Not connected");
        set_text(detail_label, message.c_str());
    }
    lv_obj_set_style_text_color(status_label, wifi_on && !joining && connected() ? GREEN : GRAY, 0);
    lv_obj_set_style_text_color(detail_label, message.length() && !connected() ? RED : GRAY, 0);

    bool syncing = wifi_on && connected() && !last_sync;
    if (last_sync || syncing) {
        set_text(sync_label, syncing ? "Waiting..." : format_sync_time(last_sync).c_str());
        lv_obj_remove_flag(sync_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sync_row, LV_OBJ_FLAG_HIDDEN);
    }

    // Rebuild the list only when something it shows has changed.
    String state = String(wifi_on) + joining + scanning + scan_wanted + scan_failed + (connected() ? WiFi.SSID() : "") +
                   "|" + saved_ssid + "|" + join_ssid;
    for (const WifiNetwork &n : networks) {
        state += "|" + n.ssid + signal_level(n.rssi);
    }
    if (state != shown_state) {
        shown_state = state;
        rebuild_list();
    }
}

static void timer_cb(lv_timer_t *)
{
    tick();
    if (lv_screen_active() == screen) {
        update_screen();
    }
}

static void switch_cb(lv_event_t *)
{
    wifi_on = lv_obj_has_state(power_switch, LV_STATE_CHECKED);
    prefs.putBool("on", wifi_on);
    joining = scanning = scan_wanted = scan_failed = false;
    message = "";
    networks.clear();
    if (wifi_on) {
        radio_on();
        request_scan();
    } else {
        stop_clock_sync();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    update_screen();
}

static void scan_cb(lv_event_t *)
{
    if (wifi_on && !scanning) {
        request_scan();
        update_screen();
    }
}

static void back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

static void build_screen()
{
    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    header_button(screen, LV_SYMBOL_REFRESH, LV_ALIGN_TOP_RIGHT, scan_cb);
    make_title(screen, "Wi-Fi");
    lv_obj_t *cont = make_scroll_column(screen);

    // On/off and status, with the address and signal (or the last join's result) under it.
    power_switch = add_switch_row(cont, "Wi-Fi", " ", false, switch_cb);
    status_label = lv_obj_get_child(lv_obj_get_parent(power_switch), 1);
    detail_label = make_label(cont, "", &lv_font_montserrat_12, GRAY);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(detail_label, LV_PCT(100));

    // When the clock was last set from internet time.
    sync_label = add_value_row(cont, "Time synced");
    sync_row = lv_obj_get_parent(sync_label);
    lv_obj_add_flag(sync_row, LV_OBJ_FLAG_HIDDEN);

    // Saved network and scan results.
    list_cont = lv_obj_create(cont);
    lv_obj_remove_style_all(list_cont);
    lv_obj_set_size(list_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_cont, 6, 0);
    lv_obj_remove_flag(list_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_timer_create(timer_cb, 500, NULL);
}

// ---------- Password ----------

static void pw_close()
{
    ui_close(screen);
}

static void pw_cancel_cb(lv_event_t *)
{
    pw_close();
}

static void pw_ready_cb(lv_event_t *)
{
    const char *pass = lv_textarea_get_text(pw_ta);
    if (strlen(pass) < 8) {
        set_text(pw_hint, "At least 8 characters");
        lv_obj_set_style_text_color(pw_hint, RED, 0);
        return;
    }
    join(pw_ssid, pass);
    update_screen();
    pw_close();
}

static void pw_show_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    bool hidden = lv_textarea_get_password_mode(pw_ta);
    lv_textarea_set_password_mode(pw_ta, !hidden);
    lv_label_set_text(lv_obj_get_child(btn, 0), hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void open_password(const String &ssid)
{
    pw_ssid = ssid;
    lv_label_set_text(pw_title, ssid.c_str());
    lv_textarea_set_text(pw_ta, "");
    lv_label_set_text(pw_hint, "PASSWORD (ENTER TO JOIN)");
    lv_obj_set_style_text_color(pw_hint, GRAY, 0);
    ui_open(pw_screen);
    ui_focus(pw_ta);
}

static void build_password()
{
    pw_screen = make_screen();
    header_button(pw_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, pw_cancel_cb);
    pw_title = make_title(pw_screen, "");
    lv_obj_t *cont = make_scroll_column(pw_screen);

    pw_hint = make_label(cont, "", &lv_font_montserrat_12, GRAY);
    lv_obj_set_width(pw_hint, LV_PCT(100));

    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    pw_ta = make_text_field(row, "", 63);
    lv_textarea_set_password_mode(pw_ta, true);
    lv_obj_set_flex_grow(pw_ta, 1);
    lv_obj_add_event_cb(pw_ta, pw_ready_cb, LV_EVENT_READY, NULL);

    lv_obj_t *show = lv_button_create(row);
    lv_obj_set_size(show, 40, 36);
    lv_obj_set_style_radius(show, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(show, CARD, 0);
    lv_obj_set_style_shadow_width(show, 0, 0);
    lv_obj_center(make_label(show, LV_SYMBOL_EYE_OPEN, &lv_font_montserrat_16, ORANGE));
    lv_obj_add_event_cb(show, pw_show_cb, LV_EVENT_CLICKED, NULL);

    make_wide_button(cont, "Join", true, pw_ready_cb, NULL);
}

// ---------- Setup ----------

void wifi_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("wifi");
    wifi_on = prefs.getBool("on", false);
    saved_ssid = prefs.getString("ssid", "");
    saved_pass = prefs.getString("pass", "");
    last_sync = prefs.getUInt("synced", 0);
    WiFi.persistent(false);  // credentials live in our own preferences
    esp_sntp_set_time_sync_notification_cb([](struct timeval *) { time_synced = true; });
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
        if (failure_text(info.wifi_sta_disconnected.reason)) {
            disconnect_reason = info.wifi_sta_disconnected.reason;
        }
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    build_screen();
    build_password();
    if (wifi_on) {
        lv_obj_add_state(power_switch, LV_STATE_CHECKED);
        radio_on();
    }
}

void wifi_open()
{
    if (wifi_on && !scanning) {
        request_scan();
    }
    update_screen();
    ui_open(screen);
}
