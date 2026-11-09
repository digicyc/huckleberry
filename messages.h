// Messages app: the chat list (group channels and direct messages), chats with a compose line for
// the keyboard, joining and creating group channels, nearby contacts, and this node's name. New
// messages chime, wake the screen and show a banner over whatever is open.
#pragma once
#include <lvgl.h>

// Builds the screens and listens for messenger changes; call after messenger_init().
// `home` is the screen to return to.
void messages_init(lv_obj_t *home);

// Shows the chat list.
void messages_open();
