// Wi-Fi app: on/off switch, nearby networks, joining with a password, and the saved network, which
// reconnects automatically at boot.
#pragma once
#include <lvgl.h>

// Loads the saved setting and network, starts connecting if Wi-Fi is on, and builds the screens.
// `home` is the screen to return to.
void wifi_init(lv_obj_t *home);

// Shows the Wi-Fi screen and scans for networks.
void wifi_open();

// Signal bars for the status bar: 0 when not connected, otherwise 1 (weak) to 4 (strong).
int wifi_signal_level();
