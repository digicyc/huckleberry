#include "weather_app.h"
#include "gps_app.h"
#include "ui.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <mbedtls/platform.h>
#include <atomic>
#include <math.h>
#include <time.h>

static const uint32_t STALE_S = 30 * 60;       // refresh a forecast this old
static const double MOVED_KM = 10;             // or once the T-Deck is this far from its place
static const uint32_t RETRY_MS = 5 * 60000;    // wait this long after a failed update
static const uint32_t CHECK_MS = 5000;         // how often to check whether an update is due
static const uint8_t MIN_RAIN_SHOWN = 10;      // rain chances below this percent are left blank

static const lv_color_t RAIN_BLUE = LV_COLOR_MAKE(0x42, 0xA5, 0xF5);

// api.open-meteo.com has two servers: one's certificate chains up to Let's Encrypt's ISRG Root X1, the
// other's to ISRG Root X2 (valid until 2035 and 2040).
static const char ROOT_CA[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";

// ---------- Icons ----------

// Same order as the icons in tools/make_weather_icons.py.
enum WeatherIcon { WX_CLEAR_DAY, WX_CLEAR_NIGHT, WX_PARTLY_DAY, WX_PARTLY_NIGHT, WX_CLOUDY, WX_FOG, WX_RAIN, WX_SNOW, WX_THUNDER, WX_COUNT };
enum IconSize { BIG, SMALL };
static const int ICON_PX[] = {64, 28};  // tools/make_weather_icons.py's sizes
extern "C" const lv_image_dsc_t *const weather_icon_layers[2][WX_COUNT][2];  // weather_icons.c

static const lv_color_t SUN = LV_COLOR_MAKE(0xFF, 0xC1, 0x07), MOON = LV_COLOR_MAKE(0xFF, 0xE0, 0x82),
                        CLOUD_LIGHT = LV_COLOR_MAKE(0xE0, 0xE0, 0xE0), CLOUD_DARK = LV_COLOR_MAKE(0xB0, 0xBE, 0xC5),
                        FOG = LV_COLOR_MAKE(0x9E, 0x9E, 0x9E), WHITE = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t LAYER_COLORS[WX_COUNT][2] = {
    {SUN, SUN}, {MOON, MOON}, {SUN, CLOUD_LIGHT}, {MOON, CLOUD_LIGHT}, {CLOUD_DARK, CLOUD_DARK},
    {FOG, FOG}, {CLOUD_LIGHT, RAIN_BLUE}, {CLOUD_LIGHT, WHITE}, {CLOUD_DARK, SUN},
};

static WeatherIcon icon_for(uint8_t code, bool day)
{
    if (code <= 1) {
        return day ? WX_CLEAR_DAY : WX_CLEAR_NIGHT;
    }
    if (code == 2) {
        return day ? WX_PARTLY_DAY : WX_PARTLY_NIGHT;
    }
    if (code == 45 || code == 48) {
        return WX_FOG;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return WX_SNOW;
    }
    if (code >= 95) {
        return WX_THUNDER;
    }
    if (code >= 51) {
        return WX_RAIN;  // drizzle, rain, freezing rain, showers
    }
    return WX_CLOUDY;
}

static const char *describe(uint8_t code)
{
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Cloudy";
    case 45: return "Fog";
    case 48: return "Freezing fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: return "Snow showers";
    case 86: return "Heavy snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, hail";
    default: return "Unknown";
    }
}

// An icon is a box holding its layers stacked on top of each other.
static lv_obj_t *make_icon(lv_obj_t *parent, IconSize size)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, ICON_PX[size], ICON_PX[size]);
    lv_obj_remove_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    for (int i = 0; i < 2; i++) {
        lv_obj_t *layer = lv_image_create(box);
        lv_obj_set_style_image_recolor_opa(layer, LV_OPA_COVER, 0);
    }
    return box;
}

static void set_icon(lv_obj_t *box, IconSize size, uint8_t code, bool day)
{
    WeatherIcon icon = icon_for(code, day);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *layer = lv_obj_get_child(box, i);
        const lv_image_dsc_t *src = weather_icon_layers[size][icon][i];
        lv_obj_set_flag(layer, LV_OBJ_FLAG_HIDDEN, !src);
        if (src) {
            lv_image_set_src(layer, src);
            lv_obj_set_style_image_recolor(layer, LAYER_COLORS[icon][i], 0);
        }
    }
}

// ---------- Forecast ----------

enum FetchState { IDLE, RUNNING, DONE, FAILED };

static Preferences prefs;
static WeatherData data;  // what's shown
static std::atomic<int> fetch_state{IDLE};
static WeatherData fetched;  // the updater task's result, read once fetch_state is DONE
static float fetch_lat, fetch_lon;
static char error[48];  // why the last update failed; empty after a success
static uint32_t last_attempt;
static bool attempted, refresh_requested;
static bool dirty = true;  // the screen needs a refresh

static bool clock_set()
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_year + 1900 >= 2025;
}

static double distance_km(double lat1, double lon1, double lat2, double lon2)
{
    double x = (lon2 - lon1) * cos((lat1 + lat2) * M_PI / 360);
    double y = lat2 - lat1;
    return sqrt(x * x + y * y) * 111.195;
}

static bool parse(const String &body, WeatherData &w)
{
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        return false;
    }
    JsonObject cur = doc["current"], hourly = doc["hourly"], daily = doc["daily"];
    if (cur.isNull() || hourly.isNull() || daily.isNull()) {
        return false;
    }
    memset(&w, 0, sizeof(w));
    w.updated = time(NULL);
    w.lat = fetch_lat;
    w.lon = fetch_lon;
    strcpy(w.source, "Open-Meteo.com");
    w.temp_c = cur["temperature_2m"];
    w.feels_c = cur["apparent_temperature"];
    w.wind_kmh = cur["wind_speed_10m"];
    w.wind_dir = cur["wind_direction_10m"];
    w.humidity = cur["relative_humidity_2m"];
    w.code = cur["weather_code"];
    w.is_day = cur["is_day"] == 1;
    for (int i = 0; i < WEATHER_HOURS; i++) {
        auto &h = w.hours[i];
        h.time = hourly["time"][i];
        h.temp_c = hourly["temperature_2m"][i];
        h.code = hourly["weather_code"][i];
        h.rain_chance = hourly["precipitation_probability"][i];
        h.is_day = hourly["is_day"][i] == 1;
    }
    for (int i = 0; i < WEATHER_DAYS; i++) {
        auto &d = w.days[i];
        d.time = daily["time"][i];
        d.max_c = daily["temperature_2m_max"][i];
        d.min_c = daily["temperature_2m_min"][i];
        d.code = daily["weather_code"][i];
        d.rain_chance = daily["precipitation_probability_max"][i];
        d.sunrise = daily["sunrise"][i];
        d.sunset = daily["sunset"][i];
    }
    return true;
}

static void fetch()
{
    // Rounded to about 1 km: plenty for a forecast, and the exact position stays on the T-Deck.
    char url[480];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,"
             "wind_speed_10m,wind_direction_10m"
             "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
             "&timezone=auto&timeformat=unixtime&forecast_days=%d&forecast_hours=%d",
             fetch_lat, fetch_lon, WEATHER_DAYS, WEATHER_HOURS);
    NetworkClientSecure client;
    client.setCACert(ROOT_CA);
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, url)) {
        snprintf(error, sizeof(error), "bad request");
        fetch_state = FAILED;
        return;
    }
    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        snprintf(error, sizeof(error), "%s", status < 0 ? http.errorToString(status).c_str() : ("HTTP " + String(status)).c_str());
        fetch_state = FAILED;
    } else if (!parse(http.getString(), fetched)) {
        snprintf(error, sizeof(error), "unreadable forecast");
        fetch_state = FAILED;
    } else {
        fetch_state = DONE;
    }
    http.end();
}

// Runs on the other core so the screen and radio keep going during the ~1 s request, and sleeps
// until start_fetch() wakes it. Its stack is in PSRAM to save internal RAM, which Bluetooth needs.
static TaskHandle_t fetch_handle;
static void fetch_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        fetch();
    }
}

static void start_fetch(double lat, double lon)
{
    fetch_lat = lat;
    fetch_lon = lon;
    error[0] = '\0';
    attempted = true;
    last_attempt = millis();
    refresh_requested = false;
    fetch_state = RUNNING;
    dirty = true;
    xTaskNotifyGive(fetch_handle);
}

bool weather_now(int &temp_f, uint8_t &code, bool &is_day)
{
    uint32_t now = time(NULL);
    if (data.updated && now - data.updated < 3600) {
        temp_f = lroundf(data.temp_c * 9 / 5 + 32);
        code = data.code;
        is_day = data.is_day;
        return true;
    }
    for (const auto &h : data.hours) {
        if (h.time && h.time <= now && now < h.time + 3600) {
            temp_f = lroundf(h.temp_c * 9 / 5 + 32);
            code = h.code;
            is_day = h.is_day;
            return true;
        }
    }
    return false;
}

lv_obj_t *weather_icon_create(lv_obj_t *parent)
{
    return make_icon(parent, SMALL);
}

void weather_icon_set(lv_obj_t *icon, uint8_t code, bool is_day)
{
    set_icon(icon, SMALL, code, is_day);
}

void weather_set(const WeatherData &w)
{
    data = w;
    prefs.putBytes("data", &data, sizeof(data));
    dirty = true;
}

// ---------- Screen ----------

static lv_obj_t *home_screen, *screen, *cont;
static lv_obj_t *now_row, *now_icon, *temp_label, *range_label, *cond_label, *status_label;
static lv_obj_t *humidity_label, *wind_label, *sunrise_label, *sunset_label;
static lv_obj_t *details_card, *hours_card, *days_card, *source_label;
static lv_obj_t *hour_cols[WEATHER_HOURS], *hour_time[WEATHER_HOURS], *hour_icon[WEATHER_HOURS],
    *hour_temp[WEATHER_HOURS], *hour_rain[WEATHER_HOURS];
static lv_obj_t *day_name[WEATHER_DAYS], *day_icon[WEATHER_DAYS], *day_rain[WEATHER_DAYS], *day_temps[WEATHER_DAYS];

static int fahrenheit(float c)
{
    return (int)lroundf(c * 9 / 5 + 32);
}

static void format_clock(char *buf, size_t size, time_t t, bool minutes)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    const char *ampm = tm.tm_hour < 12 ? "AM" : "PM";
    if (minutes) {
        snprintf(buf, size, "%d:%02d %s", h, tm.tm_min, ampm);
    } else {
        snprintf(buf, size, "%d %s", h, ampm);
    }
}

static bool same_day(time_t a, time_t b)
{
    struct tm ta, tb;
    localtime_r(&a, &ta);
    localtime_r(&b, &tb);
    return ta.tm_yday == tb.tm_yday && ta.tm_year == tb.tm_year;
}

// One line under the current conditions: what's happening with updates, or why there are none.
static void status_text(char *buf, size_t size)
{
    double lat, lon;
    bool located = gps_location(lat, lon) || data.updated;
    bool online = WiFi.status() == WL_CONNECTED;
    if (fetch_state == RUNNING) {
        snprintf(buf, size, "Updating...");
    } else if (error[0]) {
        snprintf(buf, size, "Update failed: %s", error);
    } else if (!data.updated) {
        snprintf(buf, size, !located ? (gps_status().on ? "Waiting for a GPS fix" : "Turn on GPS to get your location")
                                     : online ? "Waiting for the clock" : "Connect Wi-Fi to get the weather");
    } else {
        char when[16];
        format_clock(when, sizeof(when), data.updated, true);
        if (!same_day(data.updated, time(NULL))) {
            static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            time_t t = data.updated;
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(buf, size, "Updated %s %s%s", DAYS[tm.tm_wday], when, online ? "" : ", Wi-Fi off");
        } else {
            snprintf(buf, size, "Updated %s%s", when, online ? "" : ", Wi-Fi off");
        }
    }
}

static void update_status()
{
    char buf[64];
    status_text(buf, sizeof(buf));
    if (strcmp(buf, lv_label_get_text(status_label)) != 0) {
        lv_label_set_text(status_label, buf);
    }
}

static void refresh()
{
    dirty = false;
    update_status();
    bool have = data.updated != 0;
    lv_obj_set_flag(now_row, LV_OBJ_FLAG_HIDDEN, !have);
    lv_obj_set_flag(details_card, LV_OBJ_FLAG_HIDDEN, !have);
    lv_obj_set_flag(hours_card, LV_OBJ_FLAG_HIDDEN, !have);
    lv_obj_set_flag(days_card, LV_OBJ_FLAG_HIDDEN, !have);
    lv_obj_set_flag(source_label, LV_OBJ_FLAG_HIDDEN, !have);
    if (!have) {
        lv_label_set_text(cond_label, "No weather yet");
        return;
    }

    char buf[48], a[16], b[16];
    set_icon(now_icon, BIG, data.code, data.is_day);
    lv_label_set_text_fmt(temp_label, "%d\xC2\xB0", fahrenheit(data.temp_c));
    lv_label_set_text_fmt(range_label, "H %d\xC2\xB0  L %d\xC2\xB0\nFeels %d\xC2\xB0", fahrenheit(data.days[0].max_c),
                          fahrenheit(data.days[0].min_c), fahrenheit(data.feels_c));
    lv_label_set_text(cond_label, describe(data.code));

    static const char *const COMPASS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    lv_label_set_text_fmt(humidity_label, "%d%%", data.humidity);
    lv_label_set_text_fmt(wind_label, "%d mph %s", (int)lroundf(data.wind_kmh * 0.621371f),
                          COMPASS[((data.wind_dir + 22) / 45) % 8]);
    format_clock(a, sizeof(a), data.days[0].sunrise, true);
    format_clock(b, sizeof(b), data.days[0].sunset, true);
    lv_label_set_text(sunrise_label, a);
    lv_label_set_text(sunset_label, b);

    // Hours that have already ended are left out.
    time_t now = time(NULL);
    for (int i = 0; i < WEATHER_HOURS; i++) {
        const auto &h = data.hours[i];
        lv_obj_set_flag(hour_cols[i], LV_OBJ_FLAG_HIDDEN, h.time + 3600 <= (uint32_t)now);
        if (h.time <= (uint32_t)now) {
            strcpy(buf, "Now");
        } else {
            format_clock(buf, sizeof(buf), h.time, false);
        }
        lv_label_set_text(hour_time[i], buf);
        set_icon(hour_icon[i], SMALL, h.code, h.is_day);
        lv_label_set_text_fmt(hour_temp[i], "%d\xC2\xB0", fahrenheit(h.temp_c));
        lv_label_set_text_fmt(hour_rain[i], h.rain_chance >= MIN_RAIN_SHOWN ? "%d%%" : "", h.rain_chance);
    }

    static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (int i = 0; i < WEATHER_DAYS; i++) {
        const auto &d = data.days[i];
        time_t noon = d.time + 12 * 3600;  // well inside the day whatever the time zone does
        struct tm tm;
        localtime_r(&noon, &tm);
        lv_label_set_text(day_name[i], same_day(noon, now) ? "Today" : DAYS[tm.tm_wday]);
        set_icon(day_icon[i], SMALL, d.code, true);
        lv_label_set_text_fmt(day_rain[i], d.rain_chance >= MIN_RAIN_SHOWN ? "%d%%" : "", d.rain_chance);
        lv_label_set_text_fmt(day_temps[i], "%d\xC2\xB0  #9E9E9E %d\xC2\xB0#", fahrenheit(d.max_c), fahrenheit(d.min_c));
    }
    lv_label_set_text_fmt(source_label, "Weather data by %s", data.source);
}

static void back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

static void refresh_cb(lv_event_t *)
{
    refresh_requested = true;
    error[0] = '\0';
    attempted = false;  // skip the wait after a failure
    update_status();
}

static lv_obj_t *weather_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = make_card(parent, LV_SIZE_CONTENT, NULL, NULL);
    lv_obj_set_style_pad_ver(card, 8, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    if (title) {
        make_label(card, title, &lv_font_montserrat_16, lv_color_white());
    }
    return card;
}

// A box without its own look, for laying out a card's contents.
static lv_obj_t *make_box(lv_obj_t *parent, lv_flex_flow_t flow, int32_t w, int32_t h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

// A gray caption over a value, for the details card's 2x2 grid.
static lv_obj_t *make_detail(lv_obj_t *grid, const char *caption)
{
    lv_obj_t *cell = make_box(grid, LV_FLEX_FLOW_COLUMN, LV_PCT(48), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(cell, 2, 0);
    make_label(cell, caption, &lv_font_montserrat_12, GRAY);
    return make_label(cell, "", &lv_font_montserrat_16, lv_color_white());
}

static void build_ui()
{
    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    header_button(screen, LV_SYMBOL_REFRESH, LV_ALIGN_TOP_RIGHT, refresh_cb);
    make_title(screen, "Weather");

    cont = make_scroll_column(screen);

    // Now: icon and temperature, today's high and low, the conditions, and the update status.
    lv_obj_t *card = weather_card(cont, NULL);
    lv_obj_t *row = now_row = make_box(card, LV_FLEX_FLOW_ROW, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    now_icon = make_icon(row, BIG);
    temp_label = make_label(row, "", &lv_font_montserrat_48, lv_color_white());
    range_label = make_label(row, "", &lv_font_montserrat_14, GRAY);
    lv_obj_set_flex_grow(range_label, 1);
    lv_obj_set_style_text_align(range_label, LV_TEXT_ALIGN_RIGHT, 0);
    cond_label = make_label(card, "", &lv_font_montserrat_20, lv_color_white());
    status_label = make_label(card, "", &lv_font_montserrat_12, GRAY);
    lv_obj_set_width(status_label, LV_PCT(100));
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_MODE_WRAP);

    details_card = weather_card(cont, NULL);
    lv_obj_t *grid = make_box(details_card, LV_FLEX_FLOW_ROW_WRAP, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 6, 0);
    humidity_label = make_detail(grid, "Humidity");
    wind_label = make_detail(grid, "Wind");
    sunrise_label = make_detail(grid, "Sunrise");
    sunset_label = make_detail(grid, "Sunset");

    // The coming hours, side by side; drag sideways for more.
    hours_card = weather_card(cont, "Hourly");
    row = make_box(hours_card, LV_FLEX_FLOW_ROW, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    for (int i = 0; i < WEATHER_HOURS; i++) {
        lv_obj_t *col = hour_cols[i] = make_box(row, LV_FLEX_FLOW_COLUMN, 42, LV_SIZE_CONTENT);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 2, 0);
        hour_time[i] = make_label(col, "", &lv_font_montserrat_12, GRAY);
        hour_icon[i] = make_icon(col, SMALL);
        hour_temp[i] = make_label(col, "", &lv_font_montserrat_14, lv_color_white());
        hour_rain[i] = make_label(col, "", &lv_font_montserrat_12, RAIN_BLUE);
    }

    days_card = weather_card(cont, "5 days");
    for (int i = 0; i < WEATHER_DAYS; i++) {
        row = make_box(days_card, LV_FLEX_FLOW_ROW, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        day_name[i] = make_label(row, "", &lv_font_montserrat_16, lv_color_white());
        lv_obj_set_width(day_name[i], 56);
        day_icon[i] = make_icon(row, SMALL);
        day_rain[i] = make_label(row, "", &lv_font_montserrat_12, RAIN_BLUE);
        lv_obj_set_width(day_rain[i], 36);
        day_temps[i] = make_label(row, "", &lv_font_montserrat_16, lv_color_white());
        lv_label_set_recolor(day_temps[i], true);
        lv_obj_set_flex_grow(day_temps[i], 1);
        lv_obj_set_style_text_align(day_temps[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    source_label = make_label(cont, "", &lv_font_montserrat_12, GRAY);
}

void weather_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("weather");
    if (prefs.getBytesLength("data") == sizeof(data)) {
        prefs.getBytes("data", &data, sizeof(data));
    }
    // TLS allocates two 16 KB record buffers. The core keeps them in internal RAM, where they don't
    // fit next to Bluetooth, so send mbedTLS's memory to PSRAM like ESP-IDF's
    // CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC would.
    mbedtls_platform_set_calloc_free(
        [](size_t n, size_t size) {
            return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        },
        heap_caps_free);
    static const uint32_t STACK = 12288;
    static StaticTask_t task;
    fetch_handle = xTaskCreateStaticPinnedToCore(fetch_task, "weather", STACK, NULL, 1,
                                                 (StackType_t *)ps_malloc(STACK), &task, 0);
    build_ui();
}

void weather_open()
{
    refresh();
    lv_obj_scroll_to_y(cont, 0, LV_ANIM_OFF);
    ui_open(screen);
}

void weather_tick()
{
    int state = fetch_state;
    if (state == DONE) {
        fetch_state = IDLE;
        weather_set(fetched);
        Serial.printf("Weather updated: %d F, %s\n", fahrenheit(data.temp_c), describe(data.code));
    } else if (state == FAILED) {
        fetch_state = IDLE;
        Serial.printf("Weather update failed: %s\n", error);
        dirty = true;
    }

    static uint32_t last_check;
    if (millis() - last_check >= CHECK_MS || refresh_requested) {
        last_check = millis();
        if (lv_screen_active() == screen) {
            update_status();  // follows Wi-Fi and GPS coming and going
        }
        double lat, lon;
        bool fix = gps_location(lat, lon);
        if (!fix && data.updated) {
            lat = data.lat;  // no fix: stay with the place of the last forecast
            lon = data.lon;
        }
        bool due = refresh_requested || !data.updated || (uint32_t)time(NULL) - data.updated >= STALE_S ||
                   (fix && distance_km(lat, lon, data.lat, data.lon) >= MOVED_KM);
        if (fetch_state == IDLE && due && (fix || data.updated) && WiFi.status() == WL_CONNECTED && clock_set() &&
            (!attempted || millis() - last_attempt >= RETRY_MS)) {
            start_fetch(lat, lon);
        }
    }

    if (dirty && lv_screen_active() == screen) {
        refresh();
    }
}
