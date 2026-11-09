// Sounds on the T-Deck's speaker, played by a background task so the screen and radio keep running.
#pragma once

void sound_init();

// A short two-note chime, for a new message.
void sound_chime();

// While on, a repeating double beep, for a ringing alarm.
void sound_set_alarm(bool on);
