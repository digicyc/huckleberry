// Offline map downloads: fetches USGS Topo map tiles (public domain, US only) for an area over Wi-Fi
// and saves them to the SD card in the layout the Map app reads, /maps/<zoom>/<x>/<y>.jpg.
#pragma once
#include <lvgl.h>

// Builds the download screen. `map_screen` is the screen to return to.
void map_download_init(lv_obj_t *map_screen);

// Shows the download screen for the area around lat, lon.
void map_download_open(double lat, double lon);

// Call every loop: saves downloaded tiles and starts the next one.
void map_download_tick();
