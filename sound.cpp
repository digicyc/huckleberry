#include "sound.h"
#include <LilyGoLib.h>
#include <math.h>

static const uint32_t SAMPLE_RATE = 16000;

static volatile bool alarm_on;
static volatile bool chime_pending;

// A tone with a short fade in and out so it doesn't click.
static void tone(int16_t *buf, size_t n, float hz, float volume)
{
    size_t fade = SAMPLE_RATE / 200;
    for (size_t i = 0; i < n; i++) {
        float env = i < fade ? (float)i / fade : i > n - fade ? (float)(n - i) / fade : 1.0f;
        buf[i] = (int16_t)(volume * env * sinf(2 * PI * hz * i / SAMPLE_RATE));
    }
}

static void sound_task(void *)
{
    static int16_t beep[SAMPLE_RATE * 150 / 1000];
    static int16_t chime_lo[SAMPLE_RATE * 90 / 1000], chime_hi[SAMPLE_RATE * 160 / 1000];
    static int16_t silence[SAMPLE_RATE * 100 / 1000];
    tone(beep, sizeof(beep) / 2, 2000, 12000);
    tone(chime_lo, sizeof(chime_lo) / 2, 1319, 9000);  // E6
    tone(chime_hi, sizeof(chime_hi) / 2, 1760, 9000);  // A6
    AudioOutputIf *out = instance.getAudioOutput();
    bool open = false;
    for (;;) {
        if (!alarm_on && !chime_pending) {
            if (open) {
                out->write((uint8_t *)silence, sizeof(silence));
                out->close();  // also powers the speaker amplifier down
                open = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!open) {
            out->open(16, 1, SAMPLE_RATE);
            open = true;
        }
        if (chime_pending) {
            chime_pending = false;
            out->write((uint8_t *)chime_lo, sizeof(chime_lo));
            out->write((uint8_t *)chime_hi, sizeof(chime_hi));
            continue;
        }
        out->write((uint8_t *)beep, sizeof(beep));
        out->write((uint8_t *)silence, sizeof(silence));
        out->write((uint8_t *)beep, sizeof(beep));
        for (int i = 0; i < 6 && alarm_on; i++) {
            out->write((uint8_t *)silence, sizeof(silence));
        }
    }
}

void sound_init()
{
    xTaskCreatePinnedToCore(sound_task, "sound", 4096, NULL, 1, NULL, 0);
}

void sound_chime()
{
    chime_pending = true;
}

void sound_set_alarm(bool on)
{
    alarm_on = on;
}
