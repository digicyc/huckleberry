#include "map_download.h"
#include "map_app.h"
#include "ui.h"
#include <HTTPClient.h>
#include <LilyGoLib.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <atomic>

// USGS Topo: public-domain topographic maps of the US, free for offline use. The server puts {y}
// before {x}.
static const char *TILE_URL = "https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/%d/%d/%d";
static const char *MAP_DIR = "/sd/maps";
static const int MIN_ZOOM = 8;                // region overview; a few tiles
static const int TILE_KB = 20;                // typical USGS Topo JPEG, for the estimate
static const int US_TILE_KB = 14;             // overview tiles average less (a lot of them are sea)
static const size_t MAX_TILE_BYTES = 256 * 1024;
static const int MAX_FAILURES = 8;            // in a row, before giving up
static const int CHECKS_PER_TICK = 16;        // tiles looked up on the card per loop

static const int RADII_KM[] = {2, 5, 10, 25};
static const char *const RADIUS_MAP[] = {"2 km", "5 km", "10 km", "25 km", "All US", ""};
static const int ALL_US = 4;  // the last area choice
// "All US": a low-detail map of the whole country (about 2,750 tiles, 35-40 MB), which the Map app
// blows up wherever there are no detailed tiles, so it's never blank. West, south, east, north.
static const double US_BOXES[][4] = {
    {-125.0, 24.4, -66.9, 49.4},   // lower 48
    {-170.0, 51.0, -129.9, 71.5},  // Alaska
    {-160.5, 18.8, -154.7, 22.3},  // Hawaii
    {-67.3, 17.9, -65.2, 18.6},    // Puerto Rico
};
static const int US_MIN_ZOOM = 3, US_MAX_ZOOM = 8;
static const int MAX_BOXES = sizeof(US_BOXES) / sizeof(US_BOXES[0]);
static const int MAX_ZOOMS[] = {13, 14, 15, 16};
static const char *const DETAIL_MAP[] = {"Low", "Medium", "High", "Street", ""};

// basemap.nationalmap.gov's certificate chains up to DigiCert Global Root G2 (valid until 2038).
static const char ROOT_CA[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
    "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
    "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
    "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
    "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
    "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
    "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
    "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
    "MrY=\n"
    "-----END CERTIFICATE-----\n";

// The area as tile ranges per zoom level.
struct ZoomRange {
    int x0, x1, y0, y1;
};

static lv_obj_t *map_screen, *screen, *center_label, *radius_bm, *detail_bm, *estimate_label, *progress_bar,
    *progress_label, *start_label;
static double center_lat, center_lon;
static int radius_index = 1, detail_index = 1;

static bool running;
static ZoomRange ranges[MAX_BOXES][20];  // per box and zoom
static int box_count, min_zoom, max_zoom;
static int cur_z, cur_box, cur_x, cur_y;  // next tile to consider
static uint32_t total, done, saved, failures, skipped;
static char error[64];

// The fetch runs on the other core: the main loop hands it a URL and saves what comes back, since
// the SD card shares the SPI bus with the screen and radio.
enum FetchState { F_IDLE, F_BUSY, F_DONE, F_FAILED };
static std::atomic<int> fetch_state{F_IDLE};
static char fetch_url[160];
static int fetch_z, fetch_x, fetch_y;
static uint8_t *fetch_buf;  // MAX_TILE_BYTES in PSRAM
static size_t fetch_len;
static char fetch_error[48];
static TaskHandle_t fetch_handle;

static void tile_xy(double lat, double lon, int z, int &x, int &y)
{
    lat = fmax(-85.05112878, fmin(85.05112878, lat));
    int n = 1 << z;
    double s = sin(lat * M_PI / 180);
    double fx = (lon + 180) / 360 * n, fy = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * n;
    x = (int)fmin(n - 1, fmax(0, fx));
    y = (int)fmin(n - 1, fmax(0, fy));
}

// Works out the tiles for the chosen area and detail; returns how many (tiles shared by two boxes
// count twice, and the second is skipped as already there).
static uint32_t plan()
{
    double boxes[MAX_BOXES][4];
    if (radius_index == ALL_US) {
        memcpy(boxes, US_BOXES, sizeof(boxes));
        box_count = MAX_BOXES;
        min_zoom = US_MIN_ZOOM;
        max_zoom = US_MAX_ZOOM;
    } else {
        double km = RADII_KM[radius_index];
        double dlat = km / 111.32, dlon = km / (111.32 * fmax(0.01, cos(center_lat * M_PI / 180)));
        double box[4] = {center_lon - dlon, center_lat - dlat, center_lon + dlon, center_lat + dlat};
        memcpy(boxes[0], box, sizeof(box));
        box_count = 1;
        min_zoom = MIN_ZOOM;
        max_zoom = MAX_ZOOMS[detail_index];
    }
    uint32_t n = 0;
    for (int b = 0; b < box_count; b++) {
        for (int z = min_zoom; z <= max_zoom; z++) {
            ZoomRange &r = ranges[b][z];
            tile_xy(boxes[b][3], boxes[b][0], z, r.x0, r.y0);
            tile_xy(boxes[b][1], boxes[b][2], z, r.x1, r.y1);
            n += (uint32_t)(r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1);
        }
    }
    return n;
}

static bool card_ready()
{
    return instance.getDeviceProbe() & HW_SD_ONLINE;
}

static void fetch()
{
    static NetworkClientSecure *client;
    static HTTPClient *http;
    if (!http) {
        client = new NetworkClientSecure;
        client->setCACert(ROOT_CA);
        http = new HTTPClient;
        http->setReuse(true);  // one TLS connection for the whole download
        http->setTimeout(15000);
        http->setUserAgent("HuckleBerry/1.0 (LilyGo T-Deck offline map)");
    }
    fetch_error[0] = '\0';
    if (!http->begin(*client, fetch_url)) {
        strcpy(fetch_error, "bad request");
        fetch_state = F_FAILED;
        return;
    }
    int status = http->GET();
    if (status != HTTP_CODE_OK) {
        snprintf(fetch_error, sizeof(fetch_error), "%s", status < 0 ? http->errorToString(status).c_str() : ("HTTP " + String(status)).c_str());
        http->end();
        fetch_state = F_FAILED;
        return;
    }
    int size = http->getSize();  // -1 when chunked
    NetworkClient *stream = http->getStreamPtr();
    size_t len = 0;
    uint32_t last_data = millis();
    while ((size < 0 || len < (size_t)size) && len < MAX_TILE_BYTES && millis() - last_data < 10000) {
        int avail = stream->available();
        if (avail > 0) {
            len += stream->read(fetch_buf + len, std::min((size_t)avail, MAX_TILE_BYTES - len));
            last_data = millis();
        } else if (!stream->connected() && size < 0) {
            break;
        } else {
            vTaskDelay(1);
        }
    }
    http->end();
    fetch_len = len;
    bool image = len > 4 && ((fetch_buf[0] == 0xFF && fetch_buf[1] == 0xD8) || memcmp(fetch_buf, "\x89PNG", 4) == 0);
    if (!image || (size > 0 && len != (size_t)size)) {
        strcpy(fetch_error, image ? "connection dropped" : "not a map tile");
        fetch_state = F_FAILED;
        return;
    }
    fetch_state = F_DONE;
}

// Its stack is in PSRAM to save internal RAM, which Bluetooth needs; the Weather app sends mbedTLS's
// buffers to PSRAM too.
static void fetch_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        fetch();
    }
}

static void tile_path(char *path, size_t size, int z, int x, int y, const char *ext)
{
    snprintf(path, size, "%s/%d/%d/%d.%s", MAP_DIR, z, x, y, ext);
}

static bool tile_exists(int z, int x, int y)
{
    char path[64];
    struct stat st;
    tile_path(path, sizeof(path), z, x, y, "jpg");
    if (stat(path, &st) == 0 && st.st_size > 0) {
        return true;
    }
    tile_path(path, sizeof(path), z, x, y, "png");
    return stat(path, &st) == 0 && st.st_size > 0;
}

static bool save_tile()
{
    char path[64];
    mkdir(MAP_DIR, 0777);
    snprintf(path, sizeof(path), "%s/%d", MAP_DIR, fetch_z);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/%d/%d", MAP_DIR, fetch_z, fetch_x);
    mkdir(path, 0777);
    tile_path(path, sizeof(path), fetch_z, fetch_x, fetch_y, fetch_buf[0] == 0xFF ? "jpg" : "png");
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(fetch_buf, 1, fetch_len, f) == fetch_len;
    ok &= fclose(f) == 0;
    if (!ok) {
        remove(path);
    }
    return ok;
}

// Moves to the next tile in the plan (zoom by zoom, so the overview fills in first); false when
// there are none left.
static bool advance()
{
    if (++cur_y > ranges[cur_box][cur_z].y1) {
        if (++cur_x > ranges[cur_box][cur_z].x1) {
            if (++cur_box == box_count) {
                cur_box = 0;
                if (++cur_z > max_zoom) {
                    return false;
                }
            }
            cur_x = ranges[cur_box][cur_z].x0;
        }
        cur_y = ranges[cur_box][cur_z].y0;
    }
    return true;
}

// ---------- Screen ----------

static void update_screen()
{
    char text[96];
    if (radius_index == ALL_US) {
        strcpy(text, "United States");
    } else {
        snprintf(text, sizeof(text), "%.4f, %.4f", center_lat, center_lon);
    }
    set_text(center_label, text);
    lv_obj_set_state(detail_bm, LV_STATE_DISABLED, radius_index == ALL_US);  // the overview has one detail

    if (running) {
        snprintf(text, sizeof(text), "%u of %u tiles, %u new", (unsigned)done, (unsigned)total, (unsigned)saved);
        set_text(progress_label, text);
        lv_bar_set_value(progress_bar, total ? done * 100 / total : 0, LV_ANIM_OFF);
        set_text(start_label, "Stop");
    } else {
        set_text(start_label, LV_SYMBOL_DOWNLOAD "  Download");
        set_text(progress_label, error[0] ? error : "");
    }
    lv_obj_set_flag(progress_bar, LV_OBJ_FLAG_HIDDEN, !running);

    uint32_t n = running ? total : plan();
    uint32_t mb = (n * (radius_index == ALL_US ? US_TILE_KB : TILE_KB) + 1023) / 1024;
    char free_text[32] = "";
    if (card_ready()) {
        uint64_t free_mb = (SD.totalBytes() - SD.usedBytes()) >> 20;
        snprintf(free_text, sizeof(free_text), ", %u MB free", (unsigned)free_mb);
    }
    snprintf(text, sizeof(text), "Zoom %d-%d: %u tiles, about %u MB%s", min_zoom, max_zoom, (unsigned)n, (unsigned)mb, free_text);
    set_text(estimate_label, text);
}

static void finish(const char *message)
{
    running = false;
    strlcpy(error, message, sizeof(error));
    Serial.printf("Map download: %s (%u saved, %u already there, %u failed)\n", message, (unsigned)saved, (unsigned)skipped,
                  (unsigned)failures);
    ui_toast(message);
    update_screen();
}

static void start()
{
    if (WiFi.status() != WL_CONNECTED) {
        ui_toast("Connect to Wi-Fi first");
        return;
    }
    if (!card_ready()) {
        ui_toast("Insert an SD card first");
        return;
    }
    total = plan();
    done = saved = failures = skipped = 0;
    error[0] = '\0';
    cur_z = min_zoom;
    cur_box = 0;
    cur_x = ranges[0][cur_z].x0;
    cur_y = ranges[0][cur_z].y0 - 1;  // advance() moves to the first tile
    running = true;
    Serial.printf("Map download: %u tiles %s, zoom %d-%d\n", (unsigned)total, radius_index == ALL_US ? "for the US" : "nearby",
                  min_zoom, max_zoom);
    update_screen();
}

static void start_cb(lv_event_t *)
{
    if (running) {
        finish("Download stopped");
    } else {
        start();
    }
}

static void choice_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target_obj(e);
    int index = lv_buttonmatrix_get_selected_button(bm);
    if (running || index < 0) {
        return;
    }
    (bm == radius_bm ? radius_index : detail_index) = index;
    update_screen();
}

static void back_cb(lv_event_t *)
{
    map_open();  // picks up the new zoom levels
}

void map_download_init(lv_obj_t *map)
{
    map_screen = map;
    fetch_buf = (uint8_t *)ps_malloc(MAX_TILE_BYTES);
    static const uint32_t STACK = 12288;
    static StaticTask_t task;
    fetch_handle = xTaskCreateStaticPinnedToCore(fetch_task, "map_fetch", STACK, NULL, 1, (StackType_t *)ps_malloc(STACK),
                                                 &task, 0);

    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    make_title(screen, "Download map");
    lv_obj_t *cont = make_scroll_column(screen);

    center_label = add_value_row(cont, "Around");
    radius_bm = make_choices(cont, "Area", RADIUS_MAP, radius_index, choice_cb);
    detail_bm = make_choices(cont, "Detail", DETAIL_MAP, detail_index, choice_cb);
    estimate_label = make_label(cont, "", &lv_font_montserrat_12, GRAY);
    start_label = lv_obj_get_child(make_wide_button(cont, "", true, start_cb, NULL), 0);
    progress_bar = lv_bar_create(cont);
    lv_obj_set_size(progress_bar, LV_PCT(100), 8);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, ORANGE, LV_PART_INDICATOR);
    progress_label = make_label(cont, "", &lv_font_montserrat_14, lv_color_white());
    lv_obj_t *note = make_label(cont, "USGS Topo maps (public domain, US only) over Wi-Fi to the SD card. "
                                      "All US is a low-detail map of the whole country that fills in around your detailed areas. "
                                      "Tiles already on the card are skipped, so a stopped download picks up where it left off.",
                                &lv_font_montserrat_12, GRAY);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));

    lv_timer_create([](lv_timer_t *) {
        if (running && lv_screen_active() == screen) {
            update_screen();
        }
    }, 500, NULL);
}

void map_download_open(double lat, double lon)
{
    if (!running) {
        center_lat = lat;
        center_lon = lon;
    }
    update_screen();
    ui_open(screen);
}

void map_download_tick()
{
    if (!running) {
        if (fetch_state == F_DONE || fetch_state == F_FAILED) {
            fetch_state = F_IDLE;  // a fetch that finished after Stop
        }
        return;
    }
    int state = fetch_state;
    if (state == F_BUSY) {
        return;
    }
    if (state == F_DONE || state == F_FAILED) {
        fetch_state = F_IDLE;
        done++;
        if (state == F_DONE && save_tile()) {
            saved++;
            failures = 0;
        } else {
            failures++;
            Serial.printf("Map tile %d/%d/%d failed: %s\n", fetch_z, fetch_x, fetch_y, state == F_DONE ? "can't write to the card" : fetch_error);
            if (failures >= MAX_FAILURES) {
                char message[64];
                snprintf(message, sizeof(message), "Download failed: %s", state == F_DONE ? "can't write to the card" : fetch_error);
                finish(message);
                return;
            }
        }
    }
    if (WiFi.status() != WL_CONNECTED) {
        finish("Download paused: Wi-Fi disconnected");
        return;
    }
    for (int i = 0; i < CHECKS_PER_TICK; i++) {
        if (!advance()) {
            char message[48];
            snprintf(message, sizeof(message), "Map downloaded: %u new tiles", (unsigned)saved);
            finish(message);
            return;
        }
        if (!tile_exists(cur_z, cur_x, cur_y)) {
            fetch_z = cur_z;
            fetch_x = cur_x;
            fetch_y = cur_y;
            snprintf(fetch_url, sizeof(fetch_url), TILE_URL, cur_z, cur_y, cur_x);
            fetch_state = F_BUSY;
            xTaskNotifyGive(fetch_handle);
            return;
        }
        done++;
        skipped++;
    }
}
