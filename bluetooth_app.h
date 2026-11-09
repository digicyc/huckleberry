// Bluetooth app: turns the MeshCore phone link on or off and shows what the phone needs to pair.
#pragma once
#include <lvgl.h>

// Builds the screen; call after messenger_init(). `home` is the screen to return to.
void bluetooth_init(lv_obj_t *home);

// Shows the Bluetooth screen.
void bluetooth_open();
