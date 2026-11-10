#include "map_app.h"
#include "gps_app.h"
#include "map_download.h"
#include "messenger.h"
#include "track.h"
#include "ui.h"
#include <Preferences.h>
#define LODEPNG_NO_COMPILE_CPP  // LVGL builds lodepng as C
#include <libs/lodepng/lodepng.h>  // not in lvgl.h
#include <libs/tjpgd/tjpgd.h>
#include <misc/cache/instance/lv_image_cache.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>
#include <vector>

// LVGL reads files through its POSIX drive 'A', which is rooted at the SD card's mount point.
static const char *MAP_DIR = LV_FS_POSIX_PATH "/maps";
static const int TILE = 256;
static const int COLS = 3, ROWS = 2;  // tile slots needed to cover the 320x240 screen at any offset
// Decoded tiles kept in PSRAM (128 KB each): the 3x3 on screen plus the next zoom level in and out.
static const int CACHE_TILES = 32;
// Where a tile isn't on the card, the nearest one up to this many zoom levels out is blown up in
// its place (say, a whole-US overview at zoom 8 under detailed tiles at 13-16).
static const int MAX_OVERZOOM = 8;
static const double MAX_LAT = 85.05112878;                  // web map projection limit
static const lv_color_t BLUE = LV_COLOR_MAKE(0x21, 0x96, 0xF3);    // lv_palette_main(LV_PALETTE_BLUE)
// Breadcrumb trail colors by activity; picked to stand out on topo maps and from the blue dot.
static const lv_color_t ACTIVITY_COLORS[ACT_COUNT] = {
    LV_COLOR_MAKE(0x75, 0x75, 0x75),  // unknown
    LV_COLOR_MAKE(0x75, 0x75, 0x75),  // still
    LV_COLOR_MAKE(0x2E, 0xB8, 0x3C),  // walking: green
    LV_COLOR_MAKE(0xFF, 0x6D, 0x00),  // running: orange
    LV_COLOR_MAKE(0xD5, 0x00, 0xF9),  // cycling: purple
    LV_COLOR_MAKE(0xE5, 0x1C, 0x23),  // driving: red
};
static const lv_color_t PIN = LV_COLOR_MAKE(0xAB, 0x47, 0xBC);           // companions and other nodes: purple
static const lv_color_t REPEATER_PIN = LV_COLOR_MAKE(0x00, 0xAC, 0xC1);  // repeaters: lv_palette_darken(CYAN, 1)
static const int MAX_PINS = 24;  // pins on screen at once
static const int PAN_STEP = 40;  // px per trackball step
static const uint32_t TRAIL_GAP_S = 120;  // points further apart in time aren't joined
static const int TRAIL_MIN_PX = 3;        // shorter steps are merged into the next one

// A tile decoded to RGB565, ready to draw without touching the card again.
struct Tile {
    int z = -1, x, y;
    uint32_t used;  // last use, for evicting the least recently used tile
    bool blown_up;  // made from a lower zoom's tile, as this one isn't on the card
    lv_image_dsc_t dsc;
};

struct Slot {
    lv_obj_t *box;  // placeholder square, visible until the tile is decoded or where it's missing
    lv_obj_t *img;
    int z, x, y;    // tile wanted here; z = -1 for none
    Tile *tile;     // decoded tile shown, or NULL
    bool pending;   // waiting to be decoded
};

static Preferences prefs;
static lv_obj_t *home_screen, *screen, *layer, *trail, *marker, *zoom_label, *status_label, *locate_btn, *legend;
// MeshCore nodes whose adverts carry a position (companions, repeaters, room servers, sensors),
// drawn as pins: a diamond for a repeater, a dot for the rest. Each pin shows whichever located
// contact is in view, most recently heard first, with its name unless that would cover a pin placed
// before it.
static lv_obj_t *pins[MAX_PINS];
static int8_t pin_repeater[MAX_PINS];  // how each pin is styled: 1 repeater, 0 not, -1 not yet
static ContactSummary *contacts;      // MAX_CONTACTS; the located ones first
static int located_count;
static Slot slots[COLS * ROWS];
static Tile cache[CACHE_TILES];
static uint32_t use_clock;
static std::vector<uint64_t> missing;  // tiles not on the card (or undecodable), so they aren't retried

// One JPEG at a time is decoded by a task on the other CPU core, so the screen and radio keep
// running meanwhile. The UI side reads the file and picks the buffer; the task only decodes.
enum JobState { JOB_IDLE, JOB_DECODING, JOB_DONE };
static struct {
    std::atomic<int> state{JOB_IDLE};
    Tile *tile;  // destination, taken out of the cache (z = -1) while decoding
    int z, x, y;
    uint8_t *data;
    size_t len;
    bool ok;
} job;
static TaskHandle_t decoder_task;
static std::vector<int> zooms;  // zoom levels offered: lowest to highest on the card, ascending
static int zoom = -1;
static double cx, cy;           // map center in world pixels at `zoom`
static bool following = true;   // keep the map centered on the GPS position

// ---------- Projection (web Mercator) ----------

static double world_size(int z)
{
    return TILE * ldexp(1.0, z);
}

static void project(double lat, double lon, int z, double &x, double &y)
{
    lat = std::max(-MAX_LAT, std::min(MAX_LAT, lat));
    double n = world_size(z), s = sin(lat * M_PI / 180);
    x = (lon + 180) / 360 * n;
    y = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * n;
}

static void unproject(double x, double y, int z, double &lat, double &lon)
{
    double n = world_size(z);
    lon = x / n * 360 - 180;
    lat = atan(sinh(M_PI * (1 - 2 * y / n))) * 180 / M_PI;
}

// ---------- Tiles on the card ----------

// Numeric entries of a directory (zoom levels, tile columns, or tile rows by file name).
static std::vector<int> list_numbers(const char *path)
{
    std::vector<int> out;
    DIR *dir = opendir(path);
    if (!dir) {
        return out;
    }
    for (dirent *e; (e = readdir(dir));) {
        char *end;
        long v = strtol(e->d_name, &end, 10);
        if (end != e->d_name && (*end == '\0' || *end == '.')) {
            out.push_back((int)v);
        }
    }
    closedir(dir);
    std::sort(out.begin(), out.end());
    return out;
}

// Center of the tiles stored at zoom z, for when there's no GPS fix or saved view.
static bool tiles_center(int z, double &x, double &y)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%d", MAP_DIR, z);
    std::vector<int> xs = list_numbers(path);
    if (xs.empty()) {
        return false;
    }
    int mid_x = xs[xs.size() / 2];
    snprintf(path, sizeof(path), "%s/%d/%d", MAP_DIR, z, mid_x);
    std::vector<int> ys = list_numbers(path);
    if (ys.empty()) {
        return false;
    }
    x = (xs.front() + xs.back() + 1) / 2.0 * TILE;
    y = (ys.front() + ys.back() + 1) / 2.0 * TILE;
    return true;
}

static int closest_zoom(int wanted)
{
    int best = zooms.front();
    for (int z : zooms) {
        if (abs(z - wanted) < abs(best - wanted)) {
            best = z;
        }
    }
    return best;
}

static uint64_t tile_key(int z, int x, int y)
{
    return ((uint64_t)z << 48) | ((uint64_t)x << 24) | (uint64_t)y;
}

static bool is_missing(int z, int x, int y)
{
    return std::find(missing.begin(), missing.end(), tile_key(z, x, y)) != missing.end();
}

static Tile *find_tile(int z, int x, int y)
{
    for (Tile &t : cache) {
        if (t.z == z && t.x == x && t.y == y) {
            return &t;
        }
    }
    return NULL;
}

// Decoder input/output state: the whole file in memory and the RGB565 tile being written.
struct JpegIo {
    const uint8_t *data;
    size_t len, pos;
    uint16_t *px;
};

static size_t jpeg_input(JDEC *jd, uint8_t *buf, size_t n)
{
    JpegIo *io = (JpegIo *)jd->device;
    n = std::min(n, io->len - io->pos);
    if (buf) {
        memcpy(buf, io->data + io->pos, n);
    }
    io->pos += n;
    return n;
}

static int jpeg_output(JDEC *jd, void *bitmap, JRECT *r)
{
    JpegIo *io = (JpegIo *)jd->device;
    const uint8_t *bgr = (const uint8_t *)bitmap;  // LVGL's TJpgDec outputs B, G, R
    for (int y = r->top; y <= r->bottom; y++) {
        uint16_t *row = io->px + y * TILE;
        for (int x = r->left; x <= r->right; x++, bgr += 3) {
            row[x] = ((bgr[2] & 0xF8) << 8) | ((bgr[1] & 0xFC) << 3) | (bgr[0] >> 3);
        }
    }
    return 1;
}

static bool decode_jpeg(const uint8_t *data, size_t len, uint16_t *px)
{
    static uint8_t pool[4096];  // TJpgDec work area
    JpegIo io = {data, len, 0, px};
    JDEC jd;
    return jd_prepare(&jd, jpeg_input, pool, sizeof(pool), &io) == JDR_OK && jd.width == TILE &&
           jd.height == TILE && jd_decomp(&jd, jpeg_output, 0) == JDR_OK;
}

static bool decode_png(const uint8_t *data, size_t len, uint16_t *px)
{
    // LVGL's lodepng returns an lv_draw_buf_t holding R, G, B, A bytes.
    lv_draw_buf_t *rgba = NULL;
    unsigned w, h;
    unsigned err = lodepng_decode32((unsigned char **)&rgba, &w, &h, data, len);
    bool ok = !err && w == TILE && h == TILE;
    for (int i = 0; ok && i < TILE * TILE; i++) {
        const uint8_t *p = rgba->data + i * 4;
        px[i] = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
    }
    if (rgba) {
        lv_draw_buf_destroy(rgba);
    }
    return ok;
}

static uint8_t *read_file(const char *path, size_t &len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)lv_malloc(len ? len : 1);
    if (data && fread(data, 1, len, f) != len) {
        lv_free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

// Least recently used cached tile that isn't on screen, being decoded, or `keep`.
static Tile *pick_victim(const Tile *keep)
{
    Tile *victim = NULL;
    for (Tile &t : cache) {
        bool shown = &t == keep || (job.state != JOB_IDLE && &t == job.tile);
        for (Slot &s : slots) {
            shown |= s.tile == &t;
        }
        if (!shown && (!victim || t.z < 0 || (victim->z >= 0 && t.used < victim->used))) {
            victim = &t;
        }
    }
    return victim;
}

// A cache entry to fill: an evicted tile with its buffer, taken out of the cache (z = -1).
static Tile *take_tile(const Tile *keep)
{
    Tile *t = pick_victim(keep);
    if (t->dsc.data) {
        lv_image_cache_drop(&t->dsc);
    } else {
        t->dsc.data = (const uint8_t *)lv_malloc(TILE * TILE * 2);
        if (!t->dsc.data) {
            return NULL;
        }
    }
    t->z = -1;
    t->blown_up = false;
    return t;
}

static void set_cached(Tile *t, int z, int x, int y)
{
    t->z = z;
    t->x = x;
    t->y = y;
    t->used = ++use_clock;
    t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    t->dsc.header.w = TILE;
    t->dsc.header.h = TILE;
    t->dsc.header.stride = TILE * 2;
    t->dsc.data_size = TILE * TILE * 2;
}

// Makes tile (z, x, y) by enlarging its part of `from`, a tile `from_z - z` zoom levels out, with
// bilinear smoothing.
static Tile *blow_up(Tile *from, int z, int x, int y)
{
    Tile *t = take_tile(from);
    if (!t) {
        return NULL;
    }
    int k = z - from->z, n = TILE >> k;
    int ox = (x & ((1 << k) - 1)) * n, oy = (y & ((1 << k) - 1)) * n;  // the part, in `from`
    const uint16_t *src = (const uint16_t *)from->dsc.data;
    uint16_t *dst = (uint16_t *)t->dsc.data;
    // Source positions of the output pixels' centers, in 1/256 px, kept inside the tile.
    auto sample = [&](int i, int origin, int &p0, int &p1, int &w) {
        int pos = origin * 256 + (((2 * i + 1) * 256) >> (k + 1)) - 128;
        pos = std::max(0, std::min((TILE - 1) * 256, pos));
        p0 = pos >> 8;
        p1 = std::min(p0 + 1, TILE - 1);
        w = pos & 255;
    };
    static int x0s[TILE], x1s[TILE], wxs[TILE];  // static: too big for the loop task's stack
    for (int i = 0; i < TILE; i++) {
        sample(i, ox, x0s[i], x1s[i], wxs[i]);
    }
    for (int j = 0; j < TILE; j++) {
        int y0, y1, wy;
        sample(j, oy, y0, y1, wy);
        const uint16_t *r0 = src + y0 * TILE, *r1 = src + y1 * TILE;
        for (int i = 0; i < TILE; i++) {
            uint16_t a = r0[x0s[i]], b = r0[x1s[i]], c = r1[x0s[i]], d = r1[x1s[i]];
            int wx = wxs[i];
            int wa = (256 - wx) * (256 - wy), wb = wx * (256 - wy), wc = (256 - wx) * wy, wd = wx * wy;
            int red = ((a >> 11) * wa + (b >> 11) * wb + (c >> 11) * wc + (d >> 11) * wd) >> 16;
            int green = (((a >> 5) & 63) * wa + ((b >> 5) & 63) * wb + ((c >> 5) & 63) * wc + ((d >> 5) & 63) * wd) >> 16;
            int blue = ((a & 31) * wa + (b & 31) * wb + (c & 31) * wc + (d & 31) * wd) >> 16;
            dst[j * TILE + i] = (red << 11) | (green << 5) | blue;
        }
    }
    set_cached(t, z, x, y);
    t->blown_up = true;
    return t;
}

static void resolve(Slot &s);

// Completes a load: caches the tile, or remembers that it's missing so it isn't tried again, and
// updates the slots waiting for it (or for a tile it's blown up into).
static void finish_load(Tile *t, int z, int x, int y, bool ok)
{
    if (ok) {
        set_cached(t, z, x, y);
    } else {
        missing.push_back(tile_key(z, x, y));
    }
    for (Slot &s : slots) {
        if (s.pending) {
            resolve(s);
        }
    }
}

// Starts loading a tile from the card into the cache. PNGs are decoded right here (rare, and
// lodepng allocates through LVGL, which only this task may use); JPEGs go to the decoder task.
static void start_load(int z, int x, int y)
{
    static const char *exts[] = {"png", "jpg"};
    for (const char *ext : exts) {
        char path[128];
        snprintf(path, sizeof(path), "%s/%d/%d/%d.%s", MAP_DIR, z, x, y, ext);
        size_t len;
        uint8_t *data = read_file(path, len);
        if (!data) {
            continue;
        }
        Tile *t = take_tile(NULL);  // only now, so a missing tile doesn't evict one
        if (!t) {
            lv_free(data);
            return;
        }
        if (ext[0] == 'p') {
            bool ok = decode_png(data, len, (uint16_t *)t->dsc.data);
            lv_free(data);
            finish_load(t, z, x, y, ok);
            return;
        }
        job.tile = t;
        job.z = z;
        job.x = x;
        job.y = y;
        job.data = data;
        job.len = len;
        job.state = JOB_DECODING;
        xTaskNotifyGive(decoder_task);
        return;
    }
    finish_load(NULL, z, x, y, false);
}

static void decoder_main(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        job.ok = decode_jpeg(job.data, job.len, (uint16_t *)job.tile->dsc.data);
        job.state = JOB_DONE;
    }
}

static void show_tile(Slot &s, Tile *t)
{
    s.tile = t;
    s.pending = false;
    t->used = ++use_clock;
    lv_image_set_src(s.img, &t->dsc);
    lv_obj_remove_flag(s.img, LV_OBJ_FLAG_HIDDEN);
}

// Points a slot at a tile: shows it at once if it's decoded, otherwise leaves the placeholder
// for decode_cb to fill in.
static void set_tile(Slot &s, int z, int x, int y)
{
    if (s.z == z && s.x == x && s.y == y) {
        return;
    }
    s.z = z;
    s.x = x;
    s.y = y;
    s.tile = NULL;
    s.pending = false;
    lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
    if (z < 0) {  // above or below the map's edge
        lv_obj_add_flag(s.box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s.box, LV_OBJ_FLAG_HIDDEN);
    resolve(s);
}

// The tile a slot needs loaded from the card next: its own, or when that's missing, the nearest
// lower zoom's tile covering it. False if none is left to try.
static bool slot_target(const Slot &s, int &z, int &x, int &y)
{
    for (int k = 0; k <= MAX_OVERZOOM && s.z - k >= zooms.front(); k++) {
        z = s.z - k;
        x = s.x >> k;
        y = s.y >> k;
        if (!is_missing(z, x, y)) {
            return true;
        }
    }
    return false;
}

// Shows the slot's tile if it's cached, or blows up the cached lower-zoom tile that stands in for
// it; otherwise leaves the slot pending while there's a tile on the card left to try.
static void resolve(Slot &s)
{
    int z, x, y;
    s.pending = false;
    Tile *t = find_tile(s.z, s.x, s.y);  // loaded, or blown up before
    if (t) {
        show_tile(s, t);
        return;
    }
    if (!slot_target(s, z, x, y)) {
        return;
    }
    t = find_tile(z, x, y);
    if (t && z != s.z) {
        t = blow_up(t, s.z, s.x, s.y);
    }
    if (t) {
        show_tile(s, t);
    } else {
        s.pending = true;
    }
}

// ---------- Trail ----------

// Clips the segment a-b to the rectangle (Liang-Barsky); false if none of it is inside.
static bool clip_segment(double &ax, double &ay, double &bx, double &by, double x0, double y0, double x1, double y1)
{
    double t0 = 0, t1 = 1, dx = bx - ax, dy = by - ay;
    const double p[4] = {-dx, dx, -dy, dy}, q[4] = {ax - x0, x1 - ax, ay - y0, y1 - ay};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) {
                return false;
            }
        } else {
            double t = q[i] / p[i];
            if (p[i] < 0) {
                t0 = std::max(t0, t);
            } else {
                t1 = std::min(t1, t);
            }
        }
    }
    if (t0 > t1) {
        return false;
    }
    double sx = ax, sy = ay;
    ax = sx + t0 * dx;
    ay = sy + t0 * dy;
    bx = sx + t1 * dx;
    by = sy + t1 * dy;
    return true;
}

// Draws today's track over the tiles, each step in the color of the activity it was recorded in.
static void trail_draw_cb(lv_event_t *e)
{
    int n = track_point_count();
    if (zoom < 0 || n < 2) {
        return;
    }
    const TrackPoint *pts = track_points();
    double scale = world_size(zoom) / 4294967296.0;
    lv_area_t area;
    lv_obj_get_coords(trail, &area);
    double left = cx - SCREEN_W / 2.0 - area.x1, top = cy - SCREEN_H / 2.0 - area.y1;
    const double margin = 10;  // so round caps at the edge aren't cut short
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.width = 6;
    dsc.round_start = dsc.round_end = 1;
    lv_layer_t *l = lv_event_get_layer(e);
    double px = 0, py = 0;
    uint32_t prev_time = 0;
    for (int i = 0; i < n; i++) {
        double x = pts[i].x * scale - left, y = pts[i].y * scale - top;
        if (i == 0 || pts[i].time - prev_time > TRAIL_GAP_S) {
            px = x;
            py = y;
            prev_time = pts[i].time;
            continue;
        }
        prev_time = pts[i].time;
        if (fabs(x - px) < TRAIL_MIN_PX && fabs(y - py) < TRAIL_MIN_PX) {
            continue;
        }
        double ax = px, ay = py, bx = x, by = y;
        px = x;
        py = y;
        if (!clip_segment(ax, ay, bx, by, area.x1 - margin, area.y1 - margin, area.x2 + margin, area.y2 + margin)) {
            continue;
        }
        dsc.color = ACTIVITY_COLORS[pts[i].activity < ACT_COUNT ? pts[i].activity : ACT_UNKNOWN];
        dsc.p1.x = (lv_value_precise_t)lround(ax);
        dsc.p1.y = (lv_value_precise_t)lround(ay);
        dsc.p2.x = (lv_value_precise_t)lround(bx);
        dsc.p2.y = (lv_value_precise_t)lround(by);
        lv_draw_line(l, &dsc);
    }
}

// Redraws the trail only when the view or the track changed; any redraw repaints the whole screen.
static void update_trail()
{
    static double shown_cx = -1, shown_cy = -1;
    static int shown_zoom = -2;
    static uint32_t shown_version = UINT32_MAX;
    uint32_t v = track_version();
    if (cx != shown_cx || cy != shown_cy || zoom != shown_zoom || v != shown_version) {
        shown_cx = cx;
        shown_cy = cy;
        shown_zoom = zoom;
        shown_version = v;
        lv_obj_invalidate(trail);
    }
    if (zoom >= 0 && track_point_count() > 1) {
        lv_obj_remove_flag(legend, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(legend, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------- View ----------

static void update_marker()
{
    double lat, lon, x, y;
    if (zoom < 0 || !gps_location(lat, lon)) {
        lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    project(lat, lon, zoom, x, y);
    lv_obj_set_pos(marker, (int)lround(x - (cx - SCREEN_W / 2.0)) - lv_obj_get_width(marker) / 2,
                   (int)lround(y - (cy - SCREEN_H / 2.0)) - lv_obj_get_height(marker) / 2);
    lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_pins()
{
    int n = messenger_contacts(contacts, MAX_CONTACTS);
    located_count = 0;
    for (int i = 0; i < n; i++) {
        if (contacts[i].has_location) {
            contacts[located_count++] = contacts[i];  // most recently heard first
        }
    }
}

static void style_pin(int i, const ContactSummary &c)
{
    int8_t repeater = c.type == CONTACT_REPEATER;
    if (pin_repeater[i] != repeater) {
        pin_repeater[i] = repeater;
        lv_obj_t *dot = lv_obj_get_child(pins[i], 0);
        lv_obj_set_style_bg_color(dot, repeater ? REPEATER_PIN : PIN, 0);
        lv_obj_set_style_radius(dot, repeater ? 2 : LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_transform_rotation(dot, repeater ? 450 : 0, 0);  // a square on its corner
    }
    set_text(lv_obj_get_child(pins[i], 1), c.name);
}

static void place_pins()
{
    lv_area_t placed[MAX_PINS];  // screen areas of the pins placed so far
    int used = 0;
    for (int i = 0; i < located_count && used < MAX_PINS && zoom >= 0; i++) {
        double x, y;
        project(contacts[i].lat, contacts[i].lon, zoom, x, y);
        int px = (int)lround(x - (cx - SCREEN_W / 2.0)), py = (int)lround(y - (cy - SCREEN_H / 2.0));
        if (px < -100 || px > SCREEN_W || py < -10 || py > SCREEN_H + 10) {
            continue;
        }
        lv_obj_t *pin = pins[used];
        style_pin(used, contacts[i]);
        lv_obj_set_pos(pin, px - 5, py - 8);  // the dot on the spot
        lv_obj_t *name = lv_obj_get_child(pin, 1);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(pin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(pin);
        lv_obj_get_coords(pin, &placed[used]);
        for (int j = 0; j < used; j++) {
            if (lv_area_is_on(&placed[used], &placed[j])) {
                lv_obj_add_flag(name, LV_OBJ_FLAG_HIDDEN);
                lv_area_set_width(&placed[used], 10);  // just the dot
                break;
            }
        }
        used++;
    }
    for (int i = used; i < MAX_PINS; i++) {
        lv_obj_add_flag(pins[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void layout()
{
    place_pins();
    if (zoom < 0) {
        for (Slot &s : slots) {
            set_tile(s, -1, 0, 0);
        }
        update_marker();
        update_trail();
        return;
    }
    double n = world_size(zoom);
    cx = fmod(fmod(cx, n) + n, n);  // wrap around the date line
    cy = std::max(0.0, std::min(n, cy));
    int tiles = 1 << zoom;
    double left = cx - SCREEN_W / 2.0, top = cy - SCREEN_H / 2.0;
    int tx0 = (int)floor(left / TILE), ty0 = (int)floor(top / TILE);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            Slot &s = slots[r * COLS + c];
            int tx = tx0 + c, ty = ty0 + r;
            lv_obj_set_pos(s.box, (int)lround(tx * TILE - left), (int)lround(ty * TILE - top));
            set_tile(s, ty >= 0 && ty < tiles ? zoom : -1, ((tx % tiles) + tiles) % tiles, ty);
        }
    }
    update_marker();
    update_trail();
}

static void update_status()
{
    GpsStatus gps = gps_status();
    const char *text = "";
    if (zooms.empty()) {
        text = "No maps on the SD card yet. Tap " LV_SYMBOL_DOWNLOAD " to download some over Wi-Fi.";
    } else if (!gps.on) {
        text = "GPS is off";
    } else if (!gps.fix) {
        text = "Waiting for GPS fix";
    }
    if (strcmp(lv_label_get_text(status_label), text) != 0) {  // unchanged text would still repaint
        lv_label_set_text(status_label, text);
    }
    if (text[0]) {
        lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (zoom >= 0) {
        char zoom_text[16];
        snprintf(zoom_text, sizeof(zoom_text), "Zoom %d", zoom);
        if (strcmp(lv_label_get_text(zoom_label), zoom_text) != 0) {
            lv_label_set_text(zoom_label, zoom_text);
        }
        lv_obj_remove_flag(zoom_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(zoom_label, LV_OBJ_FLAG_HIDDEN);
    }
    static int shown_active = -1;  // restyling repaints, so only on change
    int active = following && gps.fix;
    if (active != shown_active) {
        shown_active = active;
        lv_obj_set_style_bg_color(locate_btn, active ? ORANGE : lv_color_black(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(locate_btn, 0), active ? lv_color_black() : ORANGE, 0);
    }
}

static bool center_on_gps()
{
    double lat, lon;
    if (zoom < 0 || !gps_location(lat, lon)) {
        return false;
    }
    project(lat, lon, zoom, cx, cy);
    return true;
}

static void set_zoom(int z)
{
    if (z == zoom) {
        return;
    }
    double scale = ldexp(1.0, z - zoom);
    cx *= scale;
    cy *= scale;
    zoom = z;
    layout();
    update_status();
}

static void refresh_cb(lv_timer_t *)
{
    if (lv_screen_active() != screen) {
        return;
    }
    static uint32_t pins_at;
    if (millis() - pins_at > 10000) {
        pins_at = millis();
        refresh_pins();
    }
    if (following) {
        center_on_gps();
    }
    layout();
    update_status();
}

static void drag_cb(lv_event_t *)
{
    lv_point_t v;
    lv_indev_get_vect(lv_indev_active(), &v);
    if (v.x || v.y) {
        cx -= v.x;
        cy -= v.y;
        following = false;
        layout();
        update_status();
    }
}

// Next tile to load: the slots waiting for one, nearest the middle first, then (when the map isn't
// being dragged) the tiles one zoom level in and out, so the zoom buttons usually find them ready.
static bool next_tile(int &z, int &x, int &y)
{
    int best = INT32_MAX;
    for (Slot &s : slots) {
        int dx = lv_obj_get_x(s.box) + TILE / 2 - SCREEN_W / 2, dy = lv_obj_get_y(s.box) + TILE / 2 - SCREEN_H / 2;
        int sz, sx, sy;
        if (s.pending && dx * dx + dy * dy < best && slot_target(s, sz, sx, sy) && !find_tile(sz, sx, sy)) {
            best = dx * dx + dy * dy;
            z = sz;
            x = sx;
            y = sy;
        }
    }
    if (best != INT32_MAX) {
        return true;
    }
    if (lv_obj_has_state(layer, LV_STATE_PRESSED)) {
        return false;
    }
    auto it = std::find(zooms.begin(), zooms.end(), zoom);
    int near[2] = {it + 1 != zooms.end() ? *(it + 1) : -1, it != zooms.begin() ? *(it - 1) : -1};
    for (int nz : near) {
        if (nz < 0) {
            continue;
        }
        double scale = ldexp(1.0, nz - zoom), zx = cx * scale, zy = cy * scale;
        int tiles = 1 << nz;
        for (int ty = (int)floor((zy - SCREEN_H / 2.0) / TILE); ty <= (int)floor((zy + SCREEN_H / 2.0) / TILE); ty++) {
            for (int tx = (int)floor((zx - SCREEN_W / 2.0) / TILE); tx <= (int)floor((zx + SCREEN_W / 2.0) / TILE); tx++) {
                int wx = ((tx % tiles) + tiles) % tiles;
                if (ty >= 0 && ty < tiles && !find_tile(nz, wx, ty) && !is_missing(nz, wx, ty)) {
                    z = nz;
                    x = wx;
                    y = ty;
                    return true;
                }
            }
        }
    }
    return false;
}

static void decode_cb(lv_timer_t *)
{
    if (job.state == JOB_DECODING) {
        return;
    }
    if (job.state == JOB_DONE) {
        lv_free(job.data);
        job.state = JOB_IDLE;
        finish_load(job.tile, job.z, job.x, job.y, job.ok);
        // Fall through: the next tile decodes while the screen redraws with this one.
    }
    int z, x, y;
    if (lv_screen_active() == screen && zoom >= 0 && next_tile(z, x, y)) {
        start_load(z, x, y);
    }
}

static void zoom_step(int dir)
{
    auto it = std::find(zooms.begin(), zooms.end(), zoom);
    if (it == zooms.end()) {
        return;
    }
    if (dir > 0 && it + 1 != zooms.end()) {
        set_zoom(*(it + 1));
    } else if (dir < 0 && it != zooms.begin()) {
        set_zoom(*(it - 1));
    }
}

static void zoom_cb(lv_event_t *e)
{
    zoom_step((int)(intptr_t)lv_event_get_user_data(e));
}

static void locate_cb(lv_event_t *)
{
    following = true;
    center_on_gps();
    layout();
    update_status();
}

static void save_view()
{
    if (zoom >= 0) {  // reopen here when there's no GPS fix
        double lat, lon;
        unproject(cx, cy, zoom, lat, lon);
        prefs.putDouble("lat", lat);
        prefs.putDouble("lon", lon);
        prefs.putInt("zoom", zoom);
    }
}

static void back_cb(lv_event_t *)
{
    save_view();
    ui_close(home_screen);
}

// The map center, or the GPS position or saved view when there are no tiles yet.
static void center_latlon(double &lat, double &lon)
{
    if (zoom >= 0) {
        unproject(cx, cy, zoom, lat, lon);
    } else if (!gps_location(lat, lon)) {
        lat = prefs.getDouble("lat", 39.7392);  // Denver until there's a fix
        lon = prefs.getDouble("lon", -104.9903);
    }
}

static void download_cb(lv_event_t *)
{
    save_view();
    double lat, lon;
    center_latlon(lat, lon);
    map_download_open(lat, lon);
}

// The trackball pans; a click or + zooms in, - zooms out, and space re-centers on the GPS position.
static void ball_cb(int dx, int dy)
{
    if (zoom < 0) {
        return;
    }
    cx += dx * PAN_STEP;
    cy += dy * PAN_STEP;
    following = false;
    layout();
    update_status();
}

static bool key_cb(uint32_t key)
{
    switch (key) {
    case LV_KEY_ENTER: case '+': case '=': case 'i': case 'I':
        zoom_step(1);
        return true;
    case '-': case '_': case 'o': case 'O':
        zoom_step(-1);
        return true;
    case ' ': case 'c': case 'C':
        locate_cb(NULL);
        return true;
    case 'd': case 'D':
        download_cb(NULL);
        return true;
    }
    return false;
}

// A round translucent button floating over the map.
static lv_obj_t *map_button(const char *symbol, lv_align_t align, int x, int y, lv_event_cb_t cb, int arg,
                            lv_event_code_t code = LV_EVENT_CLICKED)
{
    lv_obj_t *btn = lv_button_create(screen);
    lv_obj_set_size(btn, 40, 40);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_center(make_label(btn, symbol, &lv_font_montserrat_20, ORANGE));
    lv_obj_align(btn, align, x, y);
    lv_obj_add_event_cb(btn, cb, code, (void *)(intptr_t)arg);
    return btn;
}

static lv_obj_t *pill_label(lv_align_t align, int y)
{
    lv_obj_t *label = make_label(screen, "", &lv_font_montserrat_14, lv_color_white());
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
    lv_obj_set_style_radius(label, 10, 0);
    lv_obj_set_style_pad_hor(label, 10, 0);
    lv_obj_set_style_pad_ver(label, 4, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_max_width(label, SCREEN_W - 2 * 56, 0);  // clear of the buttons; wraps longer messages
    lv_obj_align(label, align, 0, y);
    return label;
}

static void build_screen()
{
    screen = make_screen();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101010), 0);

    // Tiles live on a full-screen layer that takes the drag gestures.
    layer = lv_obj_create(screen);
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, SCREEN_W, SCREEN_H);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(layer, drag_cb, LV_EVENT_PRESSING, NULL);
    for (Slot &s : slots) {
        s.box = lv_obj_create(layer);
        lv_obj_remove_style_all(s.box);
        lv_obj_set_size(s.box, TILE, TILE);
        lv_obj_set_style_border_width(s.box, 1, 0);
        lv_obj_set_style_border_color(s.box, lv_color_hex(0x2A2A2A), 0);
        lv_obj_remove_flag(s.box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        s.img = lv_image_create(s.box);
        lv_obj_set_pos(s.img, -1, -1);  // over the border, which would otherwise shift the tile by 1 px
        lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
        s.z = -2;  // forces the first set_tile
        lv_obj_add_flag(s.box, LV_OBJ_FLAG_HIDDEN);
    }

    trail = lv_obj_create(layer);  // above the tiles
    lv_obj_remove_style_all(trail);
    lv_obj_set_size(trail, SCREEN_W, SCREEN_H);
    lv_obj_remove_flag(trail, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_event_cb(trail, trail_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    marker = lv_obj_create(screen);
    lv_obj_remove_style_all(marker);
    lv_obj_set_size(marker, 16, 16);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(marker, BLUE, 0);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(marker, lv_color_white(), 0);
    lv_obj_set_style_border_width(marker, 3, 0);
    lv_obj_set_style_shadow_width(marker, 8, 0);
    lv_obj_set_style_shadow_color(marker, lv_color_black(), 0);
    lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < MAX_PINS; i++) {
        pins[i] = lv_obj_create(screen);
        lv_obj_remove_style_all(pins[i]);
        lv_obj_set_size(pins[i], LV_SIZE_CONTENT, 16);  // fixed, so the dot stays put when the name hides
        lv_obj_set_flex_flow(pins[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pins[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(pins[i], 3, 0);
        lv_obj_remove_flag(pins[i], (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_t *dot = lv_obj_create(pins[i]);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_transform_pivot_x(dot, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(dot, LV_PCT(50), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        pin_repeater[i] = -1;  // style_pin() sets the shape and color
        lv_obj_set_style_border_color(dot, lv_color_white(), 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_t *name = make_label(pins[i], "", &lv_font_montserrat_12, lv_color_white());
        lv_obj_set_style_bg_color(name, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(name, LV_OPA_60, 0);
        lv_obj_set_style_pad_hor(name, 3, 0);
        lv_obj_set_style_radius(name, 3, 0);
        lv_obj_add_flag(pins[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *back = map_button(LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, 6, STATUS_H + 4, back_cb, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_USER_2);  // the screen's back button, for the mic key
    map_button(LV_SYMBOL_DOWNLOAD, LV_ALIGN_TOP_RIGHT, -6, STATUS_H + 4, download_cb, 0);
    // Zoom and locate act on touch-down: a redraw takes a moment, so waiting for the release
    // makes them feel late.
    map_button(LV_SYMBOL_PLUS, LV_ALIGN_RIGHT_MID, -6, -10, zoom_cb, 1, LV_EVENT_PRESSED);
    map_button(LV_SYMBOL_MINUS, LV_ALIGN_RIGHT_MID, -6, 36, zoom_cb, -1, LV_EVENT_PRESSED);
    locate_btn = map_button(LV_SYMBOL_GPS, LV_ALIGN_BOTTOM_RIGHT, -6, -6, locate_cb, 0, LV_EVENT_PRESSED);
    zoom_label = pill_label(LV_ALIGN_TOP_MID, STATUS_H + 10);
    status_label = pill_label(LV_ALIGN_BOTTOM_MID, -52);

    // Trail colors.
    legend = lv_obj_create(screen);
    lv_obj_remove_style_all(legend);
    lv_obj_set_size(legend, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(legend, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(legend, LV_OPA_70, 0);
    lv_obj_set_style_radius(legend, 10, 0);
    lv_obj_set_style_pad_hor(legend, 8, 0);
    lv_obj_set_style_pad_ver(legend, 4, 0);
    lv_obj_set_style_pad_column(legend, 4, 0);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(legend, LV_OBJ_FLAG_CLICKABLE);
    static const char *const LEGEND_NAMES[] = {"Walk", "Run", "Bike", "Drive"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(legend);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, ACTIVITY_COLORS[ACT_WALK + i], 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_t *name = make_label(legend, LEGEND_NAMES[i], &lv_font_montserrat_12, lv_color_white());
        lv_obj_set_style_margin_right(name, i < 3 ? 4 : 0, 0);
    }
    lv_obj_align(legend, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_add_flag(legend, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(refresh_cb, 1000, NULL);
    lv_timer_create(decode_cb, 0, NULL);
    ui_set_screen_input(screen, ball_cb, key_cb);
}

void map_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("map");
    xTaskCreatePinnedToCore(decoder_main, "map_decode", 4096, NULL, 1, &decoder_task, 0);
    contacts = (ContactSummary *)ps_malloc(MAX_CONTACTS * sizeof(ContactSummary));
    build_screen();
    map_download_init(screen);
}

void map_open()
{
    std::vector<int> on_card = list_numbers(MAP_DIR);
    zooms.clear();
    for (int z = on_card.empty() ? 0 : on_card.front(); !on_card.empty() && z <= on_card.back(); z++) {
        zooms.push_back(z);  // levels in between show blown-up tiles
    }
    if (zooms.empty()) {
        zoom = -1;
    } else {
        int saved_zoom = prefs.getInt("zoom", -1);
        int wanted = saved_zoom >= 0 ? saved_zoom : zooms[zooms.size() / 2];
        zoom = closest_zoom(zoom >= 0 ? zoom : wanted);
        following = true;
        if (!center_on_gps()) {
            if (prefs.isKey("lat")) {
                project(prefs.getDouble("lat"), prefs.getDouble("lon"), zoom, cx, cy);
            } else if (tiles_center(on_card.back(), cx, cy)) {
                double scale = ldexp(1.0, zoom - on_card.back());
                cx *= scale;
                cy *= scale;
            } else {
                cx = cy = world_size(zoom) / 2;
            }
        }
    }
    missing.clear();  // the card may have changed; look the tiles up again
    for (Tile &t : cache) {
        if (t.blown_up) {
            t.z = -1;  // the real tile may be there now
        }
    }
    for (Slot &s : slots) {
        s.z = -2;
    }
    refresh_pins();
    layout();
    update_status();
    if (lv_screen_active() != screen) {
        ui_open(screen);
    }
}



