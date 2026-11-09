// GPS app: on/off setting, position, and satellite signal bars, plus a status for the status bar.
#pragma once
#include <lvgl.h>

// Loads the saved on/off setting and builds the screen. `home` is the screen to return to.
void gps_init(lv_obj_t *home);

// Shows the GPS screen.
void gps_open();

// Call every loop: applies the on/off setting to the receiver and keeps the satellite list fresh.
void gps_tick();

struct GpsStatus {
    bool on;         // the user's setting
    bool fix;        // a current position fix
    int satellites;  // used in the fix, or in view while searching
};
GpsStatus gps_status();

// The current position, if there is a fix.
bool gps_location(double &lat, double &lon);

// The current ground speed, if there is a fix.
bool gps_speed_kmh(double &kmh);
