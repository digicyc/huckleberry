#include "sdcard_app.h"
#include "ui.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <vector>
#include <algorithm>

static const char *TEST_FILE = "/huckleberry-sdtest.bin";
static const size_t TEST_BYTES = 256 * 1024;
static const size_t VIEW_BYTES = 2048;  // shown from the start of a file
static const int MAX_ENTRIES = 64;      // listed per folder
// LilyGoLib mounts the T-Deck's card at 800 kHz; it shares the SPI bus with the screen and radio,
// which run much faster, so the card does too.
static const uint32_t SD_SPI_HZ = 20000000;
static const lv_color_t RED = LV_COLOR_MAKE(0xF4, 0x43, 0x36);    // lv_palette_main(LV_PALETTE_RED)
static const lv_color_t GREEN = LV_COLOR_MAKE(0x4C, 0xAF, 0x50);  // lv_palette_main(LV_PALETTE_GREEN)

struct Entry {
    String name;
    bool dir;
    size_t size;
};

static lv_obj_t *home_screen, *screen, *cont, *status_label, *test_btn, *retry_btn, *format_btn, *result_label;
static lv_obj_t *path_label, *files_cont;
static lv_obj_t *view_screen, *view_title, *view_text;
static String cwd = "/";
static uint64_t used_bytes, total_bytes;  // cached per mount; usedBytes() can scan the FAT

// The T-Deck has no card-detect switch, so a missing card and an unreadable one look the same.
static bool mounted()
{
    return instance.getDeviceProbe() & HW_SD_ONLINE;
}

static void format_size(char *buf, size_t len, uint64_t bytes)
{
    if (bytes >= 1000ULL * 1000 * 1000) {
        snprintf(buf, len, "%.1f GB", bytes / 1e9);
    } else if (bytes >= 1000 * 1000) {
        snprintf(buf, len, "%.1f MB", bytes / 1e6);
    } else if (bytes >= 1000) {
        snprintf(buf, len, "%.1f KB", bytes / 1e3);
    } else {
        snprintf(buf, len, "%u B", (unsigned)bytes);
    }
}

static void show_result(const char *text, lv_color_t color)
{
    lv_label_set_text(result_label, text);
    lv_obj_set_style_text_color(result_label, color, 0);
}

static void show_busy(const char *text)
{
    show_result(text, GRAY);
    lv_refr_now(NULL);  // the work below blocks the UI
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------- File list ----------

static void rebuild_files();

static void open_viewer(const String &path, size_t size)
{
    lv_label_set_text(view_title, path.substring(path.lastIndexOf('/') + 1).c_str());
    static char text[VIEW_BYTES + 64];
    size_t n = 0;
    File f = SD.open(path);
    bool opened = f;  // a File is false again once closed
    if (opened) {
        n = f.read((uint8_t *)text, VIEW_BYTES);
        f.close();
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t c = text[i];
        if ((c < 0x20 && c != '\n' && c != '\t') || c >= 0x7F) {
            text[i] = '.';  // the fonts only have printable ASCII
        }
    }
    text[n] = '\0';
    if (!opened) {
        strcpy(text, "Couldn't open the file.");
    } else if (size > n) {
        char total[16];
        format_size(total, sizeof(total), size);
        snprintf(text + n, sizeof(text) - n, "\n\n(First %u bytes of %s)", (unsigned)n, total);
    } else if (!n) {
        strcpy(text, "(Empty file)");
    }
    lv_label_set_text(view_text, text);
    lv_obj_scroll_to_y(lv_obj_get_parent(view_text), 0, LV_ANIM_OFF);
    ui_open(view_screen);
}

static std::vector<Entry> entries;  // rows of files_cont, in order (after any ".." row)

static void entry_clicked_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0) {  // ".."
        cwd = cwd.substring(0, cwd.lastIndexOf('/'));
        if (cwd.isEmpty()) {
            cwd = "/";
        }
        rebuild_files();
        return;
    }
    const Entry &entry = entries[index];
    String path = cwd == "/" ? "/" + entry.name : cwd + "/" + entry.name;
    if (entry.dir) {
        cwd = path;
        rebuild_files();
    } else {
        open_viewer(path, entry.size);
    }
}

static void add_file_row(const char *icon, const char *name, const char *detail, int index)
{
    lv_obj_t *row = make_card(files_cont, 32, entry_clicked_cb, (void *)(intptr_t)index);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_t *i = make_label(row, icon, &lv_font_montserrat_16, detail[0] == '\0' ? ORANGE : GRAY);
    lv_obj_align(i, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *n = make_label(row, name, &lv_font_montserrat_16, lv_color_white());
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_size(n, 190, lv_font_get_line_height(&lv_font_montserrat_16));
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 26, 0);
    lv_obj_t *d = make_label(row, detail, &lv_font_montserrat_14, GRAY);
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void rebuild_files()
{
    lv_obj_clean(files_cont);
    entries.clear();
    lv_label_set_text_fmt(path_label, "Files in %s", cwd.c_str());
    if (!mounted()) {
        lv_label_set_text(path_label, "");
        return;
    }
    File dir = SD.open(cwd);
    if (!dir || !dir.isDirectory()) {
        cwd = "/";
        dir = SD.open(cwd);
    }
    int skipped = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if ((int)entries.size() < MAX_ENTRIES) {
            entries.push_back({f.name(), f.isDirectory(), f.isDirectory() ? 0 : f.size()});
        } else {
            skipped++;
        }
        f.close();
    }
    dir.close();
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.dir != b.dir ? a.dir : strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    if (cwd != "/") {
        add_file_row(LV_SYMBOL_UP, "..", "", -1);
    }
    for (size_t i = 0; i < entries.size(); i++) {
        char size[16] = "";
        if (!entries[i].dir) {
            format_size(size, sizeof(size), entries[i].size);
        }
        add_file_row(entries[i].dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, entries[i].name.c_str(), size, i);
    }
    if (entries.empty()) {
        make_label(files_cont, "Empty folder", &lv_font_montserrat_14, GRAY);
    }
    if (skipped) {
        lv_obj_t *more = make_label(files_cont, "", &lv_font_montserrat_14, GRAY);
        lv_label_set_text_fmt(more, "...and %d more", skipped);
    }
}

// ---------- Status and actions ----------

static void update_status()
{
    bool on = mounted();
    if (on && !total_bytes) {
        total_bytes = SD.totalBytes();
        used_bytes = SD.usedBytes();
    } else if (!on) {
        total_bytes = used_bytes = 0;
    }

    if (!on) {
        lv_label_set_text(status_label, "No readable card. Cards must be FAT32 (exFAT isn't supported); "
                                        "Format erases a card and makes it FAT32.");
    } else {
        static const char *types[] = {"", "MMC", "SDSC", "SDHC"};
        uint8_t type = SD.cardType();
        char total[16], used[16], free_space[16];
        format_size(total, sizeof(total), total_bytes);
        format_size(used, sizeof(used), used_bytes);
        format_size(free_space, sizeof(free_space), total_bytes - used_bytes);
        lv_label_set_text_fmt(status_label, "%s %s card\n%s used, %s free", total, type < 4 ? types[type] : "", used,
                              free_space);
    }
    set_hidden(test_btn, !on);
    set_hidden(retry_btn, on);
    set_hidden(format_btn, on);
}

static void remount()
{
    instance.installSD(SD_SPI_HZ);
    total_bytes = 0;
    cwd = "/";
    update_status();
    rebuild_files();
}

// Writes TEST_BYTES of a known pattern, reads it back and compares, then deletes the file.
static void test_cb(lv_event_t *)
{
    show_busy("Testing...");
    static uint8_t block[4096];
    char result[160];
    bool ok = true;
    const char *step = "write";
    size_t done = 0;  // bytes of the current step completed

    uint32_t start = millis();
    File f = SD.open(TEST_FILE, FILE_WRITE);
    for (size_t pos = 0; f && ok && pos < TEST_BYTES; pos += sizeof(block)) {
        for (size_t i = 0; i < sizeof(block); i++) {
            block[i] = (uint8_t)((pos + i) * 31 + (pos >> 12));
        }
        ok = f.write(block, sizeof(block)) == sizeof(block);
        done += ok ? sizeof(block) : 0;
    }
    ok = ok && f;
    if (f) {
        f.close();
    }
    uint32_t write_ms = millis() - start;

    size_t bad_at = SIZE_MAX;
    start = millis();
    if (ok) {
        step = "read";
        done = 0;
        f = SD.open(TEST_FILE, FILE_READ);
        ok = f && f.size() == TEST_BYTES;
        for (size_t pos = 0; ok && pos < TEST_BYTES; pos += sizeof(block)) {
            ok = f.read(block, sizeof(block)) == sizeof(block);
            done += ok ? sizeof(block) : 0;
            for (size_t i = 0; ok && i < sizeof(block); i++) {
                if (block[i] != (uint8_t)((pos + i) * 31 + (pos >> 12))) {
                    bad_at = pos + i;
                    ok = false;
                }
            }
        }
        if (f) {
            f.close();
        }
    }
    uint32_t read_ms = millis() - start;
    SD.remove(TEST_FILE);

    if (ok) {
        snprintf(result, sizeof(result), "Passed: wrote and read back %u KB.\nWrite %u KB/s, read %u KB/s.",
                 (unsigned)(TEST_BYTES / 1024), (unsigned)(TEST_BYTES / (write_ms ? write_ms : 1)),
                 (unsigned)(TEST_BYTES / (read_ms ? read_ms : 1)));
    } else if (bad_at != SIZE_MAX) {
        snprintf(result, sizeof(result), "Failed: data read back differs at byte %u.", (unsigned)bad_at);
    } else {
        snprintf(result, sizeof(result), "Failed: couldn't %s %s (stopped after %u KB).", step, TEST_FILE,
                 (unsigned)(done / 1024));
    }
    show_result(result, ok ? GREEN : RED);
    total_bytes = 0;  // recount space
    update_status();
    rebuild_files();
}

static void retry_cb(lv_event_t *)
{
    show_busy("Checking...");
    remount();
    show_result(mounted() ? "" : "Still no readable card.", GRAY);
}

static void format_cb(lv_event_t *)
{
    show_busy("Formatting... this can take a minute.");
    bool ok = false;
    if (instance.lockSPI()) {
        SD.end();
        // format_if_empty formats only when there's no FAT volume, which is why the card is unreadable.
        ok = SD.begin(SD_CS, SPI, SD_SPI_HZ, "/sd", 5, true) && SD.cardType() != CARD_NONE;
        instance.unlockSPI();
    }
    remount();  // the way LilyGoLib mounts, which also updates its online flag
    show_result(ok ? "Formatted as FAT32." : "Format failed. Is a card inserted?", ok ? GREEN : RED);
}

static void back_cb(lv_event_t *)
{
    ui_close(home_screen);
}

static void view_back_cb(lv_event_t *)
{
    ui_close(screen);
}

static void build_screens()
{
    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    make_title(screen, "SD Card");
    cont = make_scroll_column(screen);

    lv_obj_t *status = make_card(cont, LV_SIZE_CONTENT, NULL, NULL);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(status, 10, 0);
    lv_obj_set_style_pad_row(status, 8, 0);
    status_label = make_label(status, "", &lv_font_montserrat_16, lv_color_white());
    lv_obj_set_width(status_label, LV_PCT(100));
    test_btn = make_wide_button(status, "Test read & write", true, test_cb, NULL);
    retry_btn = make_wide_button(status, "Check again", true, retry_cb, NULL);
    format_btn = make_wide_button(status, "Format card", false, format_cb, NULL);
    result_label = make_label(status, "", &lv_font_montserrat_14, GRAY);
    lv_obj_set_width(result_label, LV_PCT(100));

    path_label = make_label(cont, "", &lv_font_montserrat_14, GRAY);
    lv_label_set_long_mode(path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(path_label, LV_PCT(100));
    lv_obj_set_style_margin_top(path_label, 4, 0);
    files_cont = lv_obj_create(cont);
    lv_obj_remove_style_all(files_cont);
    lv_obj_set_size(files_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(files_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(files_cont, 4, 0);

    view_screen = make_screen();
    header_button(view_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, view_back_cb);
    view_title = make_title(view_screen, "");
    lv_obj_t *view_cont = make_scroll_column(view_screen);
    lv_obj_set_flex_align(view_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    view_text = make_label(view_cont, "", &lv_font_montserrat_14, lv_color_white());
    lv_obj_set_width(view_text, LV_PCT(100));
}

void sdcard_init(lv_obj_t *home)
{
    home_screen = home;
    // Remount at SD_SPI_HZ; this also retries a card that wasn't ready when LilyGoLib mounted it.
    instance.installSD(SD_SPI_HZ);
    build_screens();
}

void sdcard_open()
{
    if (!mounted()) {
        instance.installSD(SD_SPI_HZ);  // a card inserted since boot
    }
    show_result("", GRAY);
    cwd = "/";
    total_bytes = 0;
    update_status();
    rebuild_files();
    lv_obj_scroll_to_y(cont, 0, LV_ANIM_OFF);
    ui_open(screen);
}
