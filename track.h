// Activity track: the activity (walking, running, cycling, driving), guessed from the GPS speed, and
// a GPS track labeled with it, saved per day to /tracks/YYYY-MM-DD.csv on the SD card.
#pragma once
#include <stdint.h>

enum Activity : uint8_t { ACT_UNKNOWN, ACT_STILL, ACT_WALK, ACT_RUN, ACT_BIKE, ACT_DRIVE, ACT_COUNT };

// A recorded point. x and y are its web-Mercator position as a fraction of the world, scaled to
// 2^32, so the map can place it at any zoom without trigonometry.
struct TrackPoint {
    uint32_t x, y;
    uint32_t time;  // epoch seconds
    uint8_t activity;
};

// Loads the saved setting; the GPS app calls this and track_tick().
void track_init();

// Call every loop: follows the activity and records a point when the T-Deck has moved.
void track_tick();

Activity track_activity();
const char *activity_name(Activity a);  // "Walking", ...

bool track_recording();  // the user's setting
void track_set_recording(bool on);

// Today's points, oldest first. The version changes whenever points are added or cleared.
int track_point_count();
const TrackPoint *track_points();
uint32_t track_version();
