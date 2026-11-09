// SD Card app: card status and space, a read/write test, a file browser, and a text viewer.
// The card is mounted at boot (SD library, VFS path /sd) and again when the app opens without one.
#pragma once
#include <lvgl.h>

// Builds the screens. `home` is the screen to return to.
void sdcard_init(lv_obj_t *home);

// Shows the SD Card screen.
void sdcard_open();
