// Weather app: current conditions, the next 12 hours and the next 5 days where the T-Deck is. Over
// Wi-Fi it fetches a forecast from Open-Meteo for the GPS position (or, without a fix, for the
// place of the last forecast), and refreshes it every 30 minutes or after moving 10 km. The last
// forecast is saved, so it shows right away after a reboot.
#pragma once
#include <lvgl.h>
#include <stdint.h>

static const int WEATHER_HOURS = 12, WEATHER_DAYS = 5;

// A forecast in metric units with times in epoch seconds. Conditions are WMO weather codes, the
// ones Open-Meteo and most weather services use. Any source can fill one in: Wi-Fi today, a phone
// app over Bluetooth later.
struct WeatherData {
    uint32_t updated;  // when it was fetched; 0 for none
    float lat, lon;    // where it's for
    char source[16];   // shown at the bottom of the app, such as "Open-Meteo.com"
    float temp_c, feels_c, wind_kmh;
    uint16_t wind_dir;  // degrees the wind blows from
    uint8_t humidity, code;
    bool is_day;
    struct {
        uint32_t time;
        float temp_c;
        uint8_t code, rain_chance;  // rain_chance in percent
        bool is_day;
    } hours[WEATHER_HOURS];  // starting with the current hour
    struct {
        uint32_t time;  // local midnight
        float max_c, min_c;
        uint8_t code, rain_chance;
        uint32_t sunrise, sunset;
    } days[WEATHER_DAYS];  // starting today
};

// Loads the saved forecast and builds the screen. `home` is the screen to return to.
void weather_init(lv_obj_t *home);

void weather_open();

// Call every loop: starts a Wi-Fi update when one is due and shows its result.
void weather_tick();

// For the home screen: the conditions now, from the latest forecast or, once that's over an hour old,
// its forecast for the current hour. False when there's nothing that recent.
bool weather_now(int &temp_f, uint8_t &code, bool &is_day);

// A small (28 px) condition icon, as in the app's forecasts, and setting what it shows.
lv_obj_t *weather_icon_create(lv_obj_t *parent);
void weather_icon_set(lv_obj_t *icon, uint8_t code, bool is_day);

// Shows a new forecast and saves it. The Wi-Fi updater calls this; so can other sources.
void weather_set(const WeatherData &w);
