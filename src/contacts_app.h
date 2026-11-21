// Contacts app: every node in MeshCore's contact list (at most MAX_CONTACTS), grouped into
// companions, repeaters, and room servers and sensors, each with an icon for its type. The trash
// button in the header shows a delete button on each contact. A contact's screen shows its route,
// when it was last heard, its ID and position, and can message a companion or delete the contact.
#pragma once
#include <lvgl.h>

// Builds the screens; call after messenger_init(). `home` is the screen to return to.
void contacts_init(lv_obj_t *home);

void contacts_open();
