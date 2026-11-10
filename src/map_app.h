// Map app: offline map tiles from the SD card with the GPS position on top.
//
// Tiles are standard 256 px web map (XYZ / "slippy map") tiles stored as
// <SD card>/maps/<zoom>/<x>/<y>.png (or .jpg), the layout the Map app's downloader and
// tools/download_map_tiles.py write.
#pragma once
#include <lvgl.h>

// Builds the screen. `home` is the screen to return to.
void map_init(lv_obj_t *home);

// Shows the map, centered on the GPS position if there is a fix.
void map_open();
