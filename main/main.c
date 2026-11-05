#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "audio_player.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "radio_ui.h"
#include "stations.h"
#include "wifi_manager_bridge.h"

typedef enum {
    APP_EVENT_WIFI = 0,
    APP_EVENT_AUDIO,
    APP_EVENT_UI,
    APP_EVENT_SERIAL,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    wifi_manager_event_t wifi_event;
    radio_audio_event_t audio_event;
    radio_ui_event_t ui_event;
    char serial_command;
    char data[96];
} app_event_t;

typedef struct {
    int station;
    int volume;
    bool audio_active;
    bool audio_running;
    bool wifi_connected;
    bool restart_after_connect;
    radio_ui_wifi_state_t wifi_state;
    char wifi_name[64];
    char ip_address[32];
    char stream_title[128];
    char codec[12];
    int sample_rate;
    int64_t station_change_deadline;
    int64_t volume_save_deadline;
    int64_t retry_deadline;
    int64_t restart_deadline;
} app_state_t;

static const char *TAG = "webradio";
static QueueHandle_t s_app_queue;
static nvs_handle_t s_settings_nvs;

static void queue_wifi_event(wifi_manager_event_t event, const char *data, void *user_data)
{
    (void)user_data;
    if (s_app_queue == NULL) {
        return;
    }
    app_event_t app_event = {
        .type = APP_EVENT_WIFI,
        .wifi_event = event,
    };
    if (data != NULL) {
        strlcpy(app_event.data, data, sizeof(app_event.data));
    }
    xQueueSend(s_app_queue, &app_event, 0);
}

static void queue_audio_event(const radio_audio_event_t *event, void *user_data)
{
    (void)user_data;
    if (s_app_queue == NULL || event == NULL) {
        return;
    }
    app_event_t app_event = {
        .type = APP_EVENT_AUDIO,
        .audio_event = *event,
    };
    xQueueSend(s_app_queue, &app_event, 0);
}

static void queue_ui_event(const radio_ui_event_t *event, void *user_data)
{
    (void)user_data;
    if (s_app_queue == NULL || event == NULL) {
        return;
    }
    const app_event_t app_event = {
        .type = APP_EVENT_UI,
        .ui_event = *event,
    };
    xQueueSend(s_app_queue, &app_event, 0);
}

static void serial_input_task(void *argument)
{
    (void)argument;
    while (true) {
        const int input = getchar();
        if (input >= 0 && input != '\r' && input != '\n') {
            const app_event_t event = {
                .type = APP_EVENT_SERIAL,
                .serial_command = (char)input,
            };
            xQueueSend(s_app_queue, &event, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void save_i32(const char *key, int value)
{
    if (s_settings_nvs == 0) {
        return;
    }
    esp_err_t err = nvs_set_i32(s_settings_nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(s_settings_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save %s: %s", key, esp_err_to_name(err));
    }
}

static int wrap_station(int station)
{
    while (station < 0) {
        station += (int)g_radio_station_count;
    }
    return station % (int)g_radio_station_count;
}

static void select_station(app_state_t *state, int station)
{
    state->station = wrap_station(station);
    state->stream_title[0] = '\0';
    state->station_change_deadline = esp_timer_get_time() +
                                     (int64_t)APP_STATION_DEBOUNCE_MS * 1000;
    state->retry_deadline = 0;
    ESP_LOGI(TAG, "Selected station %d: %s", state->station,
             g_radio_stations[state->station].name);
}

static void set_volume(app_state_t *state, int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > APP_MAX_VOLUME) {
        volume = APP_MAX_VOLUME;
    }
    if (volume == state->volume) {
        return;
    }
    state->volume = volume;
    radio_audio_set_volume(volume);
    state->volume_save_deadline = esp_timer_get_time() + 750000;
    ESP_LOGI(TAG, "Volume %d%%", volume);
}

static void start_current_station(app_state_t *state)
{
    state->station_change_deadline = 0;
    state->retry_deadline = 0;
    save_i32("station", state->station);
    if (!state->wifi_connected) {
        return;
    }
    state->audio_active = true;
    state->audio_running = false;
    state->codec[0] = '\0';
    state->sample_rate = 0;
    ESP_LOGI(TAG, "Starting %s", g_radio_stations[state->station].url);
    if (radio_audio_play(g_radio_stations[state->station].url) != ESP_OK) {
        state->audio_active = false;
        state->retry_deadline = esp_timer_get_time() + 3000000;
    }
}

static void handle_wifi_event(app_state_t *state, wifi_manager_event_t event)
{
    switch (event) {
    case WIFI_MANAGER_EVENT_SCANNING:
    case WIFI_MANAGER_EVENT_CONNECTING:
        state->wifi_state = RADIO_UI_WIFI_CONNECTING;
        break;
    case WIFI_MANAGER_EVENT_CONNECTED:
        state->wifi_connected = true;
        state->wifi_state = RADIO_UI_WIFI_CONNECTED;
        wifi_manager_get_station_ssid(state->wifi_name, sizeof(state->wifi_name));
        wifi_manager_get_ip_address(state->ip_address, sizeof(state->ip_address));
        ESP_LOGI(TAG, "Wi-Fi connected: %s, %s", state->wifi_name, state->ip_address);
        if (state->restart_after_connect) {
            state->restart_deadline = esp_timer_get_time() + 1000000;
        } else {
            start_current_station(state);
        }
        break;
    case WIFI_MANAGER_EVENT_DISCONNECTED:
        state->wifi_connected = false;
        state->wifi_state = RADIO_UI_WIFI_DISCONNECTED;
        state->audio_active = false;
        state->audio_running = false;
        state->ip_address[0] = '\0';
        radio_audio_stop();
        break;
    case WIFI_MANAGER_EVENT_CONFIG_MODE_ENTER:
        state->wifi_connected = false;
        state->wifi_state = RADIO_UI_WIFI_CONFIG_AP;
        wifi_manager_get_ap_ssid(state->wifi_name, sizeof(state->wifi_name));
        strlcpy(state->ip_address, "192.168.4.1", sizeof(state->ip_address));
        state->audio_active = false;
        state->audio_running = false;
        radio_audio_stop();
        ESP_LOGI(TAG, "Provisioning AP: %s; open http://192.168.4.1", state->wifi_name);
        break;
    case WIFI_MANAGER_EVENT_CONFIG_MODE_EXIT:
        state->restart_after_connect = true;
        state->wifi_state = RADIO_UI_WIFI_CONNECTING;
        wifi_manager_start_station();
        break;
    }
}

static void handle_audio_event(app_state_t *state, const radio_audio_event_t *event)
{
    if (event->type == RADIO_AUDIO_EVENT_TITLE) {
        strlcpy(state->stream_title, event->title, sizeof(state->stream_title));
        ESP_LOGI(TAG, "Now playing: %s", state->stream_title);
    } else if (event->type == RADIO_AUDIO_EVENT_INFO) {
        state->sample_rate = event->sample_rate;
        strlcpy(state->codec, event->codec, sizeof(state->codec));
        ESP_LOGI(TAG, "%s source: %d Hz, %d ch, %d bit", state->codec,
                 event->sample_rate, event->channels, event->bits);
    } else if (event->type == RADIO_AUDIO_EVENT_STATE) {
        if (event->state == RADIO_AUDIO_RUNNING) {
            state->audio_active = true;
            state->audio_running = true;
            state->retry_deadline = 0;
        } else {
            state->audio_active = false;
            state->audio_running = false;
        }
        if (event->codec[0] != '\0') {
            strlcpy(state->codec, event->codec, sizeof(state->codec));
        }
        if ((event->state == RADIO_AUDIO_ERROR || event->state == RADIO_AUDIO_FINISHED) &&
            state->wifi_connected && state->station_change_deadline == 0) {
            state->retry_deadline = esp_timer_get_time() + 3000000;
        }
    }
}

static void handle_ui_event(app_state_t *state, const radio_ui_event_t *event)
{
    switch (event->type) {
    case RADIO_UI_EVENT_PREVIOUS:
        select_station(state, state->station - 1);
        break;
    case RADIO_UI_EVENT_NEXT:
        select_station(state, state->station + 1);
        break;
    case RADIO_UI_EVENT_SELECT_STATION:
        select_station(state, event->value);
        break;
    case RADIO_UI_EVENT_PLAY_PAUSE:
        if (state->audio_active) {
            state->retry_deadline = 0;
            state->audio_active = false;
            state->audio_running = false;
            radio_audio_stop();
        } else {
            start_current_station(state);
        }
        break;
    case RADIO_UI_EVENT_VOLUME:
        set_volume(state, event->value);
        break;
    case RADIO_UI_EVENT_WIFI_SETUP:
        if (!wifi_manager_is_config_mode()) {
            ESP_LOGI(TAG, "Entering Wi-Fi provisioning mode from touchscreen");
            wifi_manager_start_config_ap();
        }
        break;
    case RADIO_UI_EVENT_TEST_TONE:
        state->audio_active = false;
        state->audio_running = false;
        state->retry_deadline = 0;
        ESP_LOGI(TAG, "Starting local speaker test from touchscreen");
        radio_audio_test_tone();
        break;
    }
}

static void print_status(const app_state_t *state)
{
    radio_audio_diagnostics_t diagnostics = {0};
    radio_audio_get_diagnostics(&diagnostics);
    const char *audio_state = state->audio_running ? "playing" :
                              (state->audio_active ? "connecting" : "stopped");
    ESP_LOGI(TAG, "station=%d/%u '%s', volume=%d, wifi=%s, audio=%s, codec=%s",
             state->station, (unsigned)g_radio_station_count,
             g_radio_stations[state->station].name, state->volume,
             state->wifi_connected ? state->ip_address : "offline",
             audio_state,
             state->codec[0] ? state->codec : "-");
    ESP_LOGI(TAG, "audio diag: pcm_callbacks=%u, pcm_bytes=%u, peak=%d, codec_errors=%u, buffer_underruns=%u",
             (unsigned)diagnostics.pcm_callbacks,
             (unsigned)diagnostics.pcm_bytes,
             diagnostics.peak_level,
             (unsigned)diagnostics.i2s_errors,
             (unsigned)diagnostics.buffer_underruns);
}

static void handle_serial_command(app_state_t *state, char command)
{
    switch (command) {
    case 'r':
    case 'R':
        start_current_station(state);
        break;
    case 'n':
    case 'N':
        select_station(state, state->station + 1);
        break;
    case 'p':
    case 'P':
        select_station(state, state->station - 1);
        break;
    case '+':
        set_volume(state, state->volume + 5);
        break;
    case '-':
        set_volume(state, state->volume - 5);
        break;
    case 't':
    case 'T':
        state->audio_active = false;
        state->audio_running = false;
        state->retry_deadline = 0;
        ESP_LOGI(TAG, "Starting local ES8389 speaker test");
        radio_audio_test_tone();
        break;
    case 'w':
    case 'W':
        wifi_manager_start_config_ap();
        break;
    case '?':
        print_status(state);
        break;
    default:
        ESP_LOGI(TAG, "Commands: r=retry, n/p=station, +/-=volume, w=Wi-Fi setup, t=test, ?=status");
        break;
    }
}

static void render_state(const app_state_t *state)
{
    const radio_ui_view_t view = {
        .station_index = state->station,
        .station_count = (int)g_radio_station_count,
        .station_name = g_radio_stations[state->station].name,
        .stream_title = state->stream_title,
        .volume = state->volume,
        .audio_active = state->audio_active,
        .audio_running = state->audio_running,
        .wifi_state = state->wifi_state,
        .wifi_name = state->wifi_name,
        .ip_address = state->ip_address,
        .codec = state->codec,
        .source_sample_rate = state->sample_rate,
    };
    radio_ui_render(&view);
}

static esp_err_t initialize_system(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp-netif init failed");
    err = esp_event_loop_create_default();
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

void app_main(void)
{
    vTaskPrioritySet(NULL, 6);
    ESP_ERROR_CHECK(initialize_system());
    s_app_queue = xQueueCreate(32, sizeof(app_event_t));
    ESP_ERROR_CHECK(s_app_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    app_state_t state = {
        .station = APP_DEFAULT_STATION,
        .volume = APP_DEFAULT_VOLUME,
        .wifi_state = RADIO_UI_WIFI_DISCONNECTED,
    };
    strlcpy(state.wifi_name, "-", sizeof(state.wifi_name));
    strlcpy(state.ip_address, "-", sizeof(state.ip_address));

    esp_err_t err = nvs_open("radio", NVS_READWRITE, &s_settings_nvs);
    if (err == ESP_OK) {
        int32_t stored = 0;
        int32_t station_list_version = 0;
        const bool station_layout_current =
            nvs_get_i32(s_settings_nvs, "station_ver", &station_list_version) == ESP_OK &&
            station_list_version == APP_STATION_LIST_VERSION;
        if (station_layout_current &&
            nvs_get_i32(s_settings_nvs, "station", &stored) == ESP_OK &&
            stored >= 0 && stored < (int32_t)g_radio_station_count) {
            state.station = stored;
        } else {
            ESP_LOGI(TAG, "Station list updated; selecting default station %d", APP_DEFAULT_STATION);
            nvs_set_i32(s_settings_nvs, "station", APP_DEFAULT_STATION);
            nvs_set_i32(s_settings_nvs, "station_ver", APP_STATION_LIST_VERSION);
            nvs_commit(s_settings_nvs);
        }
        if (nvs_get_i32(s_settings_nvs, "volume", &stored) == ESP_OK &&
            stored >= 0 && stored <= APP_MAX_VOLUME) {
            state.volume = stored;
        }
    } else {
        ESP_LOGW(TAG, "Settings NVS unavailable: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(radio_ui_init(queue_ui_event, NULL));
    render_state(&state);
    ESP_ERROR_CHECK(radio_audio_init(queue_audio_event, NULL));
    radio_audio_set_volume(state.volume);
    ESP_LOGI(TAG, "Boot self-test: ES8389 + NS4150, 1 kHz for 2 seconds");
    ESP_ERROR_CHECK(radio_audio_test_tone());
    vTaskDelay(pdMS_TO_TICKS(2200));

    const wifi_manager_config_t wifi_config = {
        .ssid_prefix = APP_WIFI_AP_PREFIX,
        .language = "zh-CN",
        .station_scan_min_interval_seconds = 10,
        .station_scan_max_interval_seconds = 300,
        .station_failure_retry_count = 3,
    };
    ESP_ERROR_CHECK(wifi_manager_initialize(&wifi_config) ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "Wi-Fi power save disabled for continuous audio streaming");
    wifi_manager_set_event_callback(queue_wifi_event, NULL);

    if (xTaskCreate(serial_input_task, "serial_input", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Serial command task unavailable");
    }

    ESP_LOGI(TAG, "ESP32-S31 Korvo-1 WebRadio ready; %u stations; volume=%d%%",
             (unsigned)g_radio_station_count, state.volume);
    ESP_LOGI(TAG, "Touch: tap/swipe stations, transport, volume, Wi-Fi setup and speaker test");

    if (wifi_manager_has_saved_credentials()) {
        state.wifi_state = RADIO_UI_WIFI_CONNECTING;
        wifi_manager_start_station();
    } else {
        state.wifi_state = RADIO_UI_WIFI_CONFIG_AP;
        wifi_manager_start_config_ap();
    }

    bool redraw = true;
    int64_t next_heartbeat = esp_timer_get_time() + 30000000;
    int64_t next_visualizer = esp_timer_get_time() + 100000;
    while (true) {
        app_event_t event;
        if (xQueueReceive(s_app_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (event.type == APP_EVENT_WIFI) {
                handle_wifi_event(&state, event.wifi_event);
            } else if (event.type == APP_EVENT_AUDIO) {
                handle_audio_event(&state, &event.audio_event);
            } else if (event.type == APP_EVENT_UI) {
                handle_ui_event(&state, &event.ui_event);
            } else if (event.type == APP_EVENT_SERIAL) {
                handle_serial_command(&state, event.serial_command);
            }
            redraw = true;
        }

        const int64_t now = esp_timer_get_time();
        if (state.station_change_deadline > 0 && now >= state.station_change_deadline) {
            start_current_station(&state);
            redraw = true;
        }
        if (state.volume_save_deadline > 0 && now >= state.volume_save_deadline) {
            save_i32("volume", state.volume);
            state.volume_save_deadline = 0;
        }
        if (state.retry_deadline > 0 && now >= state.retry_deadline) {
            ESP_LOGI(TAG, "Retrying current station");
            start_current_station(&state);
            redraw = true;
        }
        if (state.restart_deadline > 0 && now >= state.restart_deadline) {
            ESP_LOGI(TAG, "Provisioned network is ready; restarting");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        if (redraw) {
            render_state(&state);
            redraw = false;
        }
        if (now >= next_visualizer) {
            const int level = radio_audio_take_visual_level();
            if (state.audio_running) {
                radio_ui_set_audio_level(level);
            }
            next_visualizer = now + 100000;
        }
        if (now >= next_heartbeat) {
            print_status(&state);
            next_heartbeat = now + 30000000;
        }
    }
}
