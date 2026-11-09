#include "track.h"
#include <LilyGoLib.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TRACK_DIR = "/sd/tracks";
static const int MAX_POINTS = 20000;          // a long day of driving at one point every 3 s
static const double MIN_STEP_M = 10;          // record a point after moving this far
static const uint32_t MIN_INTERVAL_S = 3;     // and no more often than this
static const double MAX_HDOP = 5;             // skip fixes worse than this
static const uint32_t TICK_MS = 1000;
static const char ACTIVITY_CODES[ACT_COUNT] = {'U', 'S', 'W', 'R', 'B', 'D'};  // in the CSV

static double avg_kmh = -1;  // smoothed GPS speed, -1 before the first fix
static Activity activity = ACT_UNKNOWN;
static Preferences prefs;
static bool recording;
static TrackPoint *points;  // MAX_POINTS in PSRAM
static int point_count;
static uint32_t version;
static double last_lat, last_lon;  // last recorded point
static char day[11];               // local date of the loaded track, "YYYY-MM-DD"

// ---------- Points ----------

static uint32_t to_fraction(double v)
{
    return (uint32_t)(fmin(fmax(v, 0.0), 0.9999999999) * 4294967296.0);
}

static void add_point(double lat, double lon, uint32_t time, uint8_t act)
{
    if (point_count == MAX_POINTS) {
        return;
    }
    double s = sin(lat * M_PI / 180);
    TrackPoint &p = points[point_count++];
    p.x = to_fraction((lon + 180) / 360);
    p.y = to_fraction(0.5 - log((1 + s) / (1 - s)) / (4 * M_PI));
    p.time = time;
    p.activity = act;
    last_lat = lat;
    last_lon = lon;
    version++;
}

static void track_path(char *path, size_t size)
{
    snprintf(path, size, "%s/%s.csv", TRACK_DIR, day);
}

static bool card_ready()
{
    return instance.getDeviceProbe() & HW_SD_ONLINE;
}

// Loads the day's saved track, if the card has one.
static void load_day()
{
    point_count = 0;
    version++;
    if (!card_ready()) {
        return;
    }
    char path[48], line[80];
    track_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long t;
        double lat, lon;
        char code;
        if (sscanf(line, "%lu,%lf,%lf,%c", &t, &lat, &lon, &code) == 4) {
            const char *c = strchr(ACTIVITY_CODES, code);
            add_point(lat, lon, t, c ? c - ACTIVITY_CODES : ACT_UNKNOWN);
        }
    }
    fclose(f);
}

static void save_point(double lat, double lon, uint32_t time)
{
    if (!card_ready()) {
        return;  // kept in memory only
    }
    mkdir(TRACK_DIR, 0777);
    char path[48];
    track_path(path, sizeof(path));
    struct stat st;
    bool is_new = stat(path, &st) != 0;
    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    if (is_new) {
        fputs("time,lat,lon,activity\n", f);
    }
    fprintf(f, "%lu,%.6f,%.6f,%c\n", (unsigned long)time, lat, lon, ACTIVITY_CODES[activity]);
    fclose(f);
}

// Distance in meters; an equirectangular approximation is plenty for 10 m steps.
static double distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double x = (lon2 - lon1) * cos((lat1 + lat2) * M_PI / 360);
    double y = lat2 - lat1;
    return sqrt(x * x + y * y) * 111195;
}

// ---------- Activity ----------

// The T-Deck has no motion sensor, so the activity is guessed from the smoothed GPS speed.
static void update_activity()
{
    GPS &gps = instance.gps;
    if (!gps.speed.isValid() || gps.speed.age() > 2000 || !gps.location.isValid()) {
        return;
    }
    static uint32_t last_ms;
    if (millis() - last_ms < 1000) {
        return;
    }
    last_ms = millis();
    double kmh = gps.speed.kmph();
    avg_kmh = avg_kmh < 0 ? kmh : avg_kmh * 0.7 + kmh * 0.3;
    Activity a = avg_kmh < 1.5 ? ACT_STILL : avg_kmh < 7 ? ACT_WALK : avg_kmh < 13 ? ACT_RUN : avg_kmh < 30 ? ACT_BIKE : ACT_DRIVE;
    if (a != activity) {
        activity = a;
        Serial.printf("Activity: %s\n", activity_name(activity));
    }
}

// ---------- Public API ----------

void track_init()
{
    points = (TrackPoint *)ps_calloc(MAX_POINTS, sizeof(TrackPoint));
    prefs.begin("track");
    recording = prefs.getBool("on", true);
}

void track_tick()
{
    update_activity();

    static uint32_t last_tick;
    if (millis() - last_tick < TICK_MS) {
        return;
    }
    last_tick = millis();

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2025) {
        return;  // the clock isn't set, so the day is unknown
    }
    char today[11];
    strftime(today, sizeof(today), "%Y-%m-%d", &local);
    if (strcmp(today, day) != 0) {
        strcpy(day, today);  // a new day starts a new track
        load_day();
    }

    GPS &gps = instance.gps;
    if (!recording || activity == ACT_STILL || activity == ACT_UNKNOWN || !gps.location.isValid() || gps.location.age() > 2000 ||
        (gps.hdop.isValid() && gps.hdop.hdop() > MAX_HDOP)) {
        return;
    }
    double lat = gps.location.lat(), lon = gps.location.lng();
    if (point_count) {
        const TrackPoint &last = points[point_count - 1];
        if ((uint32_t)now - last.time < MIN_INTERVAL_S || distance_m(last_lat, last_lon, lat, lon) < MIN_STEP_M) {
            return;
        }
    }
    add_point(lat, lon, now, activity);
    save_point(lat, lon, now);
}

Activity track_activity()
{
    return activity;
}

const char *activity_name(Activity a)
{
    static const char *const NAMES[ACT_COUNT] = {"Unknown", "Still", "Walking", "Running", "Cycling", "Driving"};
    return NAMES[a];
}

bool track_recording()
{
    return recording;
}

void track_set_recording(bool on)
{
    recording = on;
    prefs.putBool("on", on);
}

int track_point_count()
{
    return point_count;
}

const TrackPoint *track_points()
{
    return points;
}

uint32_t track_version()
{
    return version;
}
