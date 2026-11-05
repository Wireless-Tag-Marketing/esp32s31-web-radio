#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    RADIO_AUDIO_EVENT_STATE = 0,
    RADIO_AUDIO_EVENT_TITLE,
    RADIO_AUDIO_EVENT_INFO,
} radio_audio_event_type_t;

typedef enum {
    RADIO_AUDIO_STOPPED = 0,
    RADIO_AUDIO_RUNNING,
    RADIO_AUDIO_FINISHED,
    RADIO_AUDIO_ERROR,
} radio_audio_state_t;

typedef struct {
    radio_audio_event_type_t type;
    radio_audio_state_t state;
    char title[128];
    char codec[12];
    int sample_rate;
    int channels;
    int bits;
    int bitrate;
} radio_audio_event_t;

typedef void (*radio_audio_event_callback_t)(const radio_audio_event_t *event,
                                             void *user_data);

typedef struct {
    uint32_t pcm_callbacks;
    uint32_t pcm_bytes;
    uint32_t i2s_errors;
    uint32_t buffer_underruns;
    int peak_level;
} radio_audio_diagnostics_t;

esp_err_t radio_audio_init(radio_audio_event_callback_t callback, void *user_data);
esp_err_t radio_audio_play(const char *url);
esp_err_t radio_audio_test_tone(void);
void radio_audio_stop(void);
void radio_audio_set_volume(int volume);
bool radio_audio_is_running(void);
int radio_audio_take_visual_level(void);
void radio_audio_get_diagnostics(radio_audio_diagnostics_t *diagnostics);

#endif /* AUDIO_PLAYER_H */
