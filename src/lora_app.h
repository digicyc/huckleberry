// LoRa app: MeshCore's radio settings, as in the MeshCore phone app's settings: a region preset or
// the frequency, bandwidth, spreading factor, coding rate and transmit power; the path hash size
// (bytes per repeater in a message's path); extra delivery ACKs and adding heard nodes as contacts.
// Also shows what the radio hears.
#pragma once
#include <lvgl.h>

// Builds the screens. `home` is the screen to return to.
void lora_init(lv_obj_t *home);

void lora_open();
