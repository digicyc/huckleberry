// Alarms app: list/edit screens, persistent storage, and ringing.
#pragma once
#include <lvgl.h>

// Loads saved alarms and builds the alarm screens. `home` is the screen to return to.
void alarms_init(lv_obj_t *home);

// Shows the alarm list screen.
void alarms_open();

// Call every loop: fires due alarms and drives the ringing screen and sound.
void alarms_tick();

bool alarms_ringing();
