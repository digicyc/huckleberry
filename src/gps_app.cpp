#include "gps_app.h"
#include "messenger.h"
#include "ui.h"
#include <LilyGoLib.h>
#include <Preferences.h>
#include <sys/time.h>

static const uint32_t FIX_MAX_AGE_MS = 5000;  // older positions are from before the GPS lost its fix
static const uint32_t CLOCK_SYNC_INTERVAL_MS = 3600000;  // re-set the clock hourly while there's a fix
static const int MAX_BARS = 14;               // satellites shown, strongest first; fills the card
static const int BAR_MAX_HEIGHT = 50;         // px for a C/N0 of FULL_CN0 or more
static const int FULL_CN0 = 50;               // dB-Hz
static const lv_color_t GREEN = LV_COLOR_MAKE(0x4C, 0xAF, 0x50);  // lv_palette_main(LV_PALETTE_GREEN)
static const lv_color_t DIM = LV_COLOR_MAKE(0x5A, 0x5A, 0x5A);

enum { ROW_LAT, ROW_LON, ROW_ALT, ROW_SPEED, ROW_HDOP, ROW_COUNT };
static const char *ROW_NAMES[ROW_COUNT] = {"Latitude", "Longitude", "Altitude", "Speed", "HDOP"};

static Preferences prefs;
static bool gps_on;          // the user's setting
static bool running = true;  // LilyGoLib starts the receiver in begin()
static GNSSSatelliteInfo sats[GNSS_MAX_SATELLITES];
static bool clock_synced;
static uint32_t clock_synced_ms;

static lv_obj_t *home_screen, *screen, *power_switch, *status_label, *values[ROW_COUNT];
static lv_obj_t *sats_count_label, *bars_cont;

// LilyGoLib probes for the receiver at boot by querying it, which fails when a u-blox is set to
// another baud rate or is too busy to answer (seen on a T-Deck Plus whose earlier firmware left it
// at 115200 with extra UBX output). Then this listens for NMEA at each likely rate instead.
static const uint32_t LISTEN_BAUDS[] = {38400, 115200, 9600};
static const uint32_t LISTEN_MS = 1500;
static int listen_index = -1;       // LISTEN_BAUDS entry being tried, -1 before starting
static uint32_t listen_started_ms;
static int valid_sentences;
static bool heard_nmea;             // found it by listening; we feed it to the parser
static bool heard_ubx;              // and it also sent UBX frames, so it's a u-blox

static bool online()
{
    return heard_nmea || (instance.getDeviceProbe() & HW_GPS_ONLINE);
}

// Counts NMEA sentences with a valid checksum, and spots UBX frames, in what the receiver sends.
static void scan_byte(char c)
{
    static char line[100];
    static size_t len;
    static char prev;
    if (prev == (char)0xB5 && c == 0x62) {
        heard_ubx = true;
    }
    prev = c;
    if (c == '$') {
        len = 0;
    }
    if (c == '\r' || c == '\n') {
        line[len] = '\0';
        char *star = strchr(line, '*');
        if (line[0] == '$' && star && star - line > 6) {
            uint8_t sum = 0;
            for (char *p = line + 1; p < star; p++) {
                sum ^= *p;
            }
            valid_sentences += strtol(star + 1, NULL, 16) == sum;
        }
        len = 0;
    } else if (len < sizeof(line) - 1) {
        line[len++] = c;
    }
}

static void listen_for_receiver()
{
    if (listen_index < 0 || millis() - listen_started_ms > LISTEN_MS) {
        if (valid_sentences >= 2) {
            heard_nmea = true;
            Serial.printf("GPS found by listening at %u baud%s\n", (unsigned)LISTEN_BAUDS[listen_index],
                          heard_ubx ? " (u-blox)" : "");
            return;
        }
        listen_index = (listen_index + 1) % (sizeof(LISTEN_BAUDS) / sizeof(LISTEN_BAUDS[0]));
        Serial1.updateBaudRate(LISTEN_BAUDS[listen_index]);
        while (Serial1.available()) {
            Serial1.read();
        }
        listen_started_ms = millis();
        valid_sentences = 0;
        heard_ubx = false;
    }
    while (Serial1.available()) {
        scan_byte(Serial1.read());
    }
}

static bool has_fix()
{
    return running && instance.gps.location.isValid() && instance.gps.location.age() < FIX_MAX_AGE_MS;
}

GpsStatus gps_status()
{
    GpsStatus s = {gps_on, has_fix(), 0};
    if (s.fix) {
        s.satellites = instance.gps.satellites.value();
    } else if (running && online()) {
        s.satellites = instance.gps.getSatelliteCount();
    }
    return s;
}

bool gps_location(double &lat, double &lon)
{
    if (!has_fix()) {
        return false;
    }
    lat = instance.gps.location.lat();
    lon = instance.gps.location.lng();
    return true;
}

bool gps_speed_kmh(double &kmh)
{
    if (!has_fix() || !instance.gps.speed.isValid()) {
        return false;
    }
    kmh = instance.gps.speed.kmph();
    return true;
}

// Seconds since 1970 for a UTC date and time (newlib has no timegm).
static time_t utc_epoch(int y, int mon, int d, int h, int min, int sec)
{
    y -= mon <= 2;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long days = era * 146097L + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + h * 3600 + min * 60 + sec;
}

// Sets the clock from GPS time on the first fix after boot and hourly after. The T-Deck has no RTC,
// so this is usually how it learns the time away from Wi-Fi and the phone. The receiver reports a
// time before it has a fix too, but it can be wrong.
static void sync_clock()
{
    GPS &gps = instance.gps;
    if ((clock_synced && millis() - clock_synced_ms < CLOCK_SYNC_INTERVAL_MS) || !has_fix() || !gps.date.isValid() ||
        !gps.time.isValid() || gps.date.age() > 1000 || gps.time.age() > 1000 || gps.date.year() < 2025) {
        return;
    }
    time_t t = utc_epoch(gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(), gps.time.minute(),
                         gps.time.second());
    struct timeval tv = {.tv_sec = t, .tv_usec = 0};
    settimeofday(&tv, NULL);
    clock_synced = true;
    clock_synced_ms = millis();
    Serial.printf("Clock set from GPS: %s", ctime(&t));
}

// A u-blox receiver (as on this T-Deck Plus) can sleep in backup mode, keeping its satellite data.
// The L76K fitted to some T-Decks has no sleep command (only a pin the board doesn't wire), so off
// only stops using it.
static bool is_ublox()
{
    if (heard_nmea) {
        return heard_ubx;
    }
    String model = instance.gps.getModel();  // LilyGoLib names the others; u-blox reports its own model
    return model != "L76K" && model != "LS550G" && model != "Unknown" && model.length();
}

static void receiver_sleep()
{
    if (!is_ublox()) {
        return;
    }
    // UBX-RXM-PMREQ: backup mode (forced) with no time limit, woken by activity on its UART RX line.
    static const uint8_t PMREQ[] = {0xB5, 0x62, 0x02, 0x41, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x61, 0x6B};
    Serial1.write(PMREQ, sizeof(PMREQ));
}

static void receiver_wake()
{
    if (is_ublox()) {
        static const uint8_t WAKE[] = {0xFF, 0xFF, 0xFF, 0xFF};  // lost while it wakes; any edge will do
        Serial1.write(WAKE, sizeof(WAKE));
    }
}

void gps_tick()
{
    GPS &gps = instance.gps;
    // Switch off only after LilyGoLib's boot-time probe finishes, or the receiver never comes online.
    if (!gps_on && running && gps.probeDone()) {
        if (online()) {
            receiver_sleep();
        }
        running = false;
    } else if (gps_on && !running) {
        if (online()) {
            receiver_wake();
        }
        running = true;
    }
    if (running && gps.probeDone() && !gps.probeSuccess()) {
        if (!heard_nmea) {
            listen_for_receiver();
        } else {
            while (Serial1.available()) {
                gps.encode(Serial1.read());  // what instance.loop() does for a receiver it found
            }
        }
    }
    if (running && online()) {
        gps.loop();  // instance.loop() parses NMEA; this also drops satellites unheard for 5 s
        sync_clock();
        if (has_fix()) {
            messenger_set_location(gps.location.lat(), gps.location.lng(), gps.altitude.meters());
        }
    }
}

// ---------- Screen ----------

static void set_value(int row, const char *fmt, ...)
{
    char buf[32];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    set_text(values[row], buf);
}

static void update_bars()
{
    lv_obj_clean(bars_cont);
    size_t n = running && online() ? instance.gps.copySatellites(sats, GNSS_MAX_SATELLITES) : 0;
    int used = 0;
    for (size_t i = 0; i < n; i++) {
        used += sats[i].used;
    }
    lv_label_set_text_fmt(sats_count_label, "%d used / %d in view", used, (int)n);
    if (!n) {
        make_label(bars_cont, gps_on ? "None in view yet" : "GPS is off", &lv_font_montserrat_14, GRAY);
        return;
    }
    // Strongest first.
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && sats[j - 1].cn0 * sats[j - 1].has_cn0 < sats[j].cn0 * sats[j].has_cn0; j--) {
            GNSSSatelliteInfo tmp = sats[j];
            sats[j] = sats[j - 1];
            sats[j - 1] = tmp;
        }
    }
    for (size_t i = 0; i < n && i < (size_t)MAX_BARS; i++) {
        lv_obj_t *col = lv_obj_create(bars_cont);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 18, LV_PCT(100));
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 2, 0);

        int cn0 = sats[i].has_cn0 ? sats[i].cn0 : 0;
        lv_obj_t *bar = lv_obj_create(col);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 12, LV_MAX(3, LV_MIN(cn0, FULL_CN0) * BAR_MAX_HEIGHT / FULL_CN0));
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_color(bar, sats[i].used ? GREEN : DIM, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        lv_obj_t *prn = make_label(col, "", &lv_font_montserrat_10, GRAY);
        lv_label_set_text_fmt(prn, "%d", sats[i].prn);
    }
}

static void update_screen()
{
    GPS &gps = instance.gps;
    GpsStatus s = gps_status();
    char status[48];
    if (!gps_on) {
        strcpy(status, "Off");
    } else if (!online()) {
        strcpy(status, "Looking for the receiver...");
    } else if (s.fix) {
        snprintf(status, sizeof(status), "Fix with %d satellites", s.satellites);
    } else {
        snprintf(status, sizeof(status), "Searching, %d in view", s.satellites);
    }
    set_text(status_label, status);
    lv_obj_set_style_text_color(status_label, s.fix ? GREEN : GRAY, 0);

    if (s.fix) {
        double lat = gps.location.lat(), lng = gps.location.lng();
        set_value(ROW_LAT, "%.5f %c", fabs(lat), lat < 0 ? 'S' : 'N');
        set_value(ROW_LON, "%.5f %c", fabs(lng), lng < 0 ? 'W' : 'E');
        set_value(ROW_ALT, gps.altitude.isValid() ? "%.0f ft" : "--", gps.altitude.meters() * 3.28084);
        set_value(ROW_SPEED, gps.speed.isValid() ? "%.1f mph" : "--", gps.speed.kmph() * 0.621371);
        set_value(ROW_HDOP, gps.hdop.isValid() ? "%.1f" : "--", gps.hdop.hdop());
    } else {
        for (int row = 0; row < ROW_COUNT; row++) {
            set_text(values[row], "--");
        }
    }
    update_bars();
}

static void switch_cb(lv_event_t *)
{
    gps_on = lv_obj_has_state(power_switch, LV_STATE_CHECKED);
    prefs.putBool("on", gps_on);
    gps_tick();
    update_screen();
}

static void back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

// A switch row whose note is a live status label.
static lv_obj_t *status_switch_row(lv_obj_t *parent, const char *title, lv_event_cb_t cb, lv_obj_t **note)
{
    lv_obj_t *sw = add_switch_row(parent, title, " ", false, cb);
    *note = lv_obj_get_child(lv_obj_get_parent(sw), 1);
    return sw;
}

static void build_screen()
{
    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    make_title(screen, "GPS");
    lv_obj_t *cont = make_scroll_column(screen);

    power_switch = status_switch_row(cont, "GPS", switch_cb, &status_label);

    for (int row = 0; row < ROW_COUNT; row++) {
        values[row] = add_value_row(cont, ROW_NAMES[row]);
        lv_label_set_text(values[row], "--");
    }

    // Satellites: one bar per satellite, height by signal strength, green if used in the fix.
    lv_obj_t *sky = make_card(cont, LV_SIZE_CONTENT, NULL, NULL);
    lv_obj_set_flex_flow(sky, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sky, 6, 0);
    lv_obj_t *head = lv_obj_create(sky);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    make_label(head, "Satellites", &lv_font_montserrat_16, lv_color_white());
    sats_count_label = make_label(head, "", &lv_font_montserrat_12, GRAY);
    lv_obj_align(sats_count_label, LV_ALIGN_RIGHT_MID, 0, 0);
    bars_cont = lv_obj_create(sky);
    lv_obj_remove_style_all(bars_cont);
    lv_obj_set_size(bars_cont, LV_PCT(100), BAR_MAX_HEIGHT + 16);
    lv_obj_set_flex_flow(bars_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bars_cont, 2, 0);
    lv_obj_t *legend = make_label(sky, "Bar height is signal strength. Green bars are used in the fix.",
                                  &lv_font_montserrat_12, GRAY);
    lv_label_set_long_mode(legend, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(legend, LV_PCT(100));

    lv_timer_create([](lv_timer_t *) {
        if (lv_screen_active() == screen) {
            update_screen();
        }
    }, 1000, NULL);
}

void gps_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("gps");
    gps_on = prefs.getBool("on", true);
    build_screen();
    if (gps_on) {
        lv_obj_add_state(power_switch, LV_STATE_CHECKED);
    }
}

void gps_open()
{
    update_screen();
    ui_open(screen);
}
