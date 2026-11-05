#ifndef RADIO_UI_H
#define RADIO_UI_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    RADIO_UI_WIFI_DISCONNECTED = 0,
    RADIO_UI_WIFI_CONNECTING,
    RADIO_UI_WIFI_CONNECTED,
    RADIO_UI_WIFI_CONFIG_AP,
} radio_ui_wifi_state_t;

typedef enum {
    RADIO_UI_EVENT_PREVIOUS = 0,
    RADIO_UI_EVENT_NEXT,
    RADIO_UI_EVENT_PLAY_PAUSE,
    RADIO_UI_EVENT_VOLUME,
    RADIO_UI_EVENT_SELECT_STATION,
    RADIO_UI_EVENT_WIFI_SETUP,
    RADIO_UI_EVENT_TEST_TONE,
} radio_ui_event_type_t;

typedef struct {
    radio_ui_event_type_t type;
    int value;
} radio_ui_event_t;

typedef void (*radio_ui_event_callback_t)(const radio_ui_event_t *event, void *user_data);

typedef struct {
    int station_index;
    int station_count;
    const char *station_name;
    const char *stream_title;
    int volume;
    bool audio_active;
    bool audio_running;
    radio_ui_wifi_state_t wifi_state;
    const char *wifi_name;
    const char *ip_address;
    const char *codec;
    int source_sample_rate;
} radio_ui_view_t;

esp_err_t radio_ui_init(radio_ui_event_callback_t callback, void *user_data);
void radio_ui_render(const radio_ui_view_t *view);
void radio_ui_set_audio_level(int level);

#endif /* RADIO_UI_H */
