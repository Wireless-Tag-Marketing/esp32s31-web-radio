#include "audio_player.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_aac_dec.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "impl/esp_ts_dec.h"

#define URL_MAX_LENGTH       512
#define PLAYLIST_MAX_LENGTH  (32 * 1024)
#define HLS_MAX_SEGMENTS     16
#define AUDIO_COMMAND_PLAY   1
#define AUDIO_COMMAND_STOP   2
#define AUDIO_COMMAND_TEST   3
#define DECODE_INPUT_SIZE    4096
#define DECODE_OUTPUT_SIZE   16384
#define CONVERT_OUTPUT_SIZE  32768
#define NETWORK_READ_SIZE    4096
#define ENCODED_BUFFER_SIZE  (256 * 1024)
#define STARTUP_BUFFER_SECONDS 2
#define STARTUP_BUFFER_DEFAULT (16 * 1024)
#define STARTUP_BUFFER_MIN     (12 * 1024)
#define STARTUP_BUFFER_MAX     (64 * 1024)
#define RECOVERY_BUFFER_SECONDS 5
#define RECOVERY_BUFFER_DEFAULT (48 * 1024)
#define RECOVERY_BUFFER_MIN     (16 * 1024)
#define RECOVERY_BUFFER_MAX     (192 * 1024)
#define REBUFFER_LOW_MIN     (4 * 1024)
#define STREAM_READ_TIMEOUT_MS 250

typedef struct {
    int type;
    unsigned generation;
    char url[URL_MAX_LENGTH];
} audio_command_t;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} text_download_t;

typedef struct {
    esp_http_client_handle_t client;
    int metadata_interval;
    int audio_until_metadata;
    int bitrate_kbps;
    char content_type[64];
    char metadata[4081];
} stream_source_t;

typedef struct {
    esp_codec_dev_handle_t speaker;
    QueueHandle_t command_queue;
    radio_audio_event_callback_t callback;
    void *callback_user_data;
    stream_source_t source;
    atomic_uint generation;
    atomic_bool abort_requested;
    atomic_bool playing;
    atomic_int volume;
    atomic_uint pcm_callbacks;
    atomic_uint pcm_bytes;
    atomic_uint i2s_errors;
    atomic_uint buffer_underruns;
    atomic_int peak_level;
    atomic_int visual_peak;
    atomic_bool pcm_started_logged;
    atomic_bool network_stop;
    char codec[12];
} audio_context_t;

typedef struct {
    StreamBufferHandle_t buffer;
    atomic_bool done;
    esp_err_t result;
} encoded_reader_t;

typedef union {
    esp_aac_dec_cfg_t aac;
    esp_ts_dec_cfg_t ts;
} decoder_specific_cfg_t;

typedef struct {
    esp_audio_simple_dec_handle_t decoder;
    esp_audio_simple_dec_type_t type;
    uint8_t *decoded;
    size_t decoded_capacity;
    int16_t *converted;
    size_t converted_capacity;
    esp_audio_simple_dec_info_t info;
    bool info_ready;
    uint32_t rate_phase;
    int64_t sample_sum_left;
    int64_t sample_sum_right;
    uint32_t sample_sum_count;
} decoder_session_t;

static const char *TAG = "radio_audio";
static audio_context_t s_audio;

static void emit_event(const radio_audio_event_t *event)
{
    if (s_audio.callback != NULL) {
        s_audio.callback(event, s_audio.callback_user_data);
    }
}

static void emit_state(radio_audio_state_t state)
{
    radio_audio_event_t event = {
        .type = RADIO_AUDIO_EVENT_STATE,
        .state = state,
    };
    strlcpy(event.codec, s_audio.codec, sizeof(event.codec));
    emit_event(&event);
}

static void emit_title(const char *title)
{
    if (title == NULL || title[0] == '\0') {
        return;
    }
    radio_audio_event_t event = {
        .type = RADIO_AUDIO_EVENT_TITLE,
    };
    strlcpy(event.title, title, sizeof(event.title));
    emit_event(&event);
}

static int http_stream_event(esp_http_client_event_t *event)
{
    stream_source_t *source = (stream_source_t *)event->user_data;
    if (source == NULL || event->event_id != HTTP_EVENT_ON_HEADER ||
        event->header_key == NULL || event->header_value == NULL) {
        return ESP_OK;
    }

    if (strcasecmp(event->header_key, "icy-metaint") == 0) {
        source->metadata_interval = atoi(event->header_value);
        source->audio_until_metadata = source->metadata_interval;
    } else if (strcasecmp(event->header_key, "icy-br") == 0) {
        source->bitrate_kbps = atoi(event->header_value);
    } else if (strcasecmp(event->header_key, "Content-Type") == 0) {
        strlcpy(source->content_type, event->header_value, sizeof(source->content_type));
    }
    return ESP_OK;
}

static void source_close(void)
{
    if (s_audio.source.client != NULL) {
        esp_http_client_close(s_audio.source.client);
        esp_http_client_cleanup(s_audio.source.client);
        s_audio.source.client = NULL;
    }
    s_audio.source.metadata_interval = 0;
    s_audio.source.audio_until_metadata = 0;
    s_audio.source.bitrate_kbps = 0;
    s_audio.source.content_type[0] = '\0';
}

static esp_err_t source_open(const char *url)
{
    source_close();
    stream_source_t *source = &s_audio.source;
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_stream_event,
        .user_data = source,
        .user_agent = "ESP32S31-Korvo1-WebRadio/1.0",
        .timeout_ms = 8000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
    };

    source->client = esp_http_client_init(&config);
    if (source->client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(source->client, "Icy-MetaData", "1");
    esp_http_client_set_header(source->client, "Accept", "audio/aac,audio/mpeg,audio/*;q=0.9,*/*;q=0.1");

    esp_err_t err = ESP_FAIL;
    for (int redirect = 0; redirect < 5; ++redirect) {
        source->metadata_interval = 0;
        source->audio_until_metadata = 0;
        source->bitrate_kbps = 0;
        source->content_type[0] = '\0';

        err = esp_http_client_open(source->client, 0);
        if (err != ESP_OK) {
            break;
        }
        const int64_t content_length = esp_http_client_fetch_headers(source->client);
        const int status = esp_http_client_get_status_code(source->client);
        ESP_LOGI(TAG, "HTTP %d, length=%lld, type=%s", status, content_length,
                 source->content_type[0] ? source->content_type : "unknown");

        if (status >= 300 && status < 400) {
            err = esp_http_client_set_redirection(source->client);
            esp_http_client_close(source->client);
            if (err == ESP_OK) {
                continue;
            }
            break;
        }
        if (status == 200 || status == 206) {
            esp_http_client_set_timeout_ms(source->client, STREAM_READ_TIMEOUT_MS);
            return ESP_OK;
        }
        err = ESP_ERR_HTTP_FETCH_HEADER;
        break;
    }

    ESP_LOGE(TAG, "Unable to open stream %s: %s", url, esp_err_to_name(err));
    source_close();
    return err;
}

static int read_http_fully(uint8_t *buffer, int length)
{
    int total = 0;
    while (total < length && !atomic_load(&s_audio.abort_requested) &&
           !atomic_load(&s_audio.network_stop)) {
        const int read = esp_http_client_read(s_audio.source.client,
                                              (char *)buffer + total,
                                              length - total);
        if (read == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        }
        if (read <= 0) {
            return total > 0 ? total : read;
        }
        total += read;
    }
    return total;
}

static void parse_icy_metadata(char *metadata, size_t length)
{
    metadata[length] = '\0';
    const char *key = strcasestr(metadata, "StreamTitle='");
    if (key == NULL) {
        return;
    }
    key += strlen("StreamTitle='");
    const char *end = strchr(key, '\'');
    if (end == NULL || end <= key) {
        return;
    }

    char title[128];
    size_t title_length = (size_t)(end - key);
    if (title_length >= sizeof(title)) {
        title_length = sizeof(title) - 1;
    }
    memcpy(title, key, title_length);
    title[title_length] = '\0';
    emit_title(title);
}

static int source_read_callback(uint8_t *buffer, int requested, void *context)
{
    (void)context;
    stream_source_t *source = &s_audio.source;
    if (source->client == NULL || atomic_load(&s_audio.abort_requested) ||
        atomic_load(&s_audio.network_stop)) {
        return 0;
    }

    if (source->metadata_interval <= 0) {
        const int read = read_http_fully(buffer, requested);
        return read;
    }

    int output = 0;
    while (output < requested && !atomic_load(&s_audio.abort_requested) &&
           !atomic_load(&s_audio.network_stop)) {
        if (source->audio_until_metadata > 0) {
            int amount = requested - output;
            if (amount > source->audio_until_metadata) {
                amount = source->audio_until_metadata;
            }
            const int read = read_http_fully(buffer + output, amount);
            if (read <= 0) {
                return output > 0 ? output : read;
            }
            output += read;
            source->audio_until_metadata -= read;
            if (read != amount) {
                return output;
            }
            continue;
        }

        uint8_t length_byte = 0;
        const int length_read = read_http_fully(&length_byte, 1);
        if (length_read != 1) {
            return output > 0 ? output : length_read;
        }
        const int metadata_length = length_byte * 16;
        if (metadata_length > 0) {
            const int read = read_http_fully((uint8_t *)source->metadata, metadata_length);
            if (read != metadata_length) {
                return output > 0 ? output : (read < 0 ? read : 0);
            }
            parse_icy_metadata(source->metadata, metadata_length);
        }
        source->audio_until_metadata = source->metadata_interval;
    }
    return output;
}

static void decoder_session_close(decoder_session_t *session)
{
    if (session->decoder != NULL) {
        esp_audio_simple_dec_close(session->decoder);
    }
    free(session->decoded);
    free(session->converted);
    memset(session, 0, sizeof(*session));
}

static esp_err_t decoder_session_open(decoder_session_t *session,
                                      esp_audio_simple_dec_type_t type)
{
    decoder_session_close(session);
    decoder_specific_cfg_t specific = {0};
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = type,
        .use_frame_dec = false,
    };
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) {
        specific.aac.aac_plus_enable = true;
        config.dec_cfg = &specific.aac;
        config.cfg_size = sizeof(specific.aac);
    } else if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_TS) {
        specific.ts.aac_plus_enable = true;
        config.dec_cfg = &specific.ts;
        config.cfg_size = sizeof(specific.ts);
    }

    const esp_audio_err_t ret = esp_audio_simple_dec_open(&config, &session->decoder);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Lightweight decoder open failed: type=%d ret=%d", (int)type, ret);
        decoder_session_close(session);
        return ESP_FAIL;
    }
    session->decoded = malloc(DECODE_OUTPUT_SIZE);
    session->converted = malloc(CONVERT_OUTPUT_SIZE);
    if (session->decoded == NULL || session->converted == NULL) {
        decoder_session_close(session);
        return ESP_ERR_NO_MEM;
    }
    session->decoded_capacity = DECODE_OUTPUT_SIZE;
    session->converted_capacity = CONVERT_OUTPUT_SIZE;
    session->type = type;
    return ESP_OK;
}

static esp_err_t update_decoder_info(decoder_session_t *session)
{
    esp_audio_simple_dec_info_t info = {0};
    if (esp_audio_simple_dec_get_info(session->decoder, &info) != ESP_AUDIO_ERR_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (info.sample_rate == 0 || info.channel == 0 || info.channel > 2 ||
        info.bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported PCM: %lu Hz, %u ch, %u bit",
                 (unsigned long)info.sample_rate, info.channel, info.bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!session->info_ready || info.sample_rate != session->info.sample_rate ||
        info.channel != session->info.channel ||
        info.bits_per_sample != session->info.bits_per_sample) {
        session->info = info;
        session->info_ready = true;
        session->rate_phase = 0;
        session->sample_sum_left = 0;
        session->sample_sum_right = 0;
        session->sample_sum_count = 0;
        radio_audio_event_t event = {
            .type = RADIO_AUDIO_EVENT_INFO,
            .sample_rate = (int)info.sample_rate,
            .channels = info.channel,
            .bits = info.bits_per_sample,
            .bitrate = (int)info.bitrate,
        };
        strlcpy(event.codec, s_audio.codec, sizeof(event.codec));
        emit_event(&event);
        ESP_LOGI(TAG, "%s source: %lu Hz, %u ch, %u bit; local converter -> 16000 Hz stereo",
                 s_audio.codec, (unsigned long)info.sample_rate, info.channel, info.bits_per_sample);
    }
    return ESP_OK;
}

static esp_err_t write_converted_pcm(decoder_session_t *session,
                                     const uint8_t *pcm, size_t pcm_size)
{
    esp_err_t err = update_decoder_info(session);
    if (err != ESP_OK) {
        return err;
    }
    const uint32_t channels = session->info.channel;
    const uint32_t source_rate = session->info.sample_rate;
    const int16_t *input = (const int16_t *)pcm;
    const size_t input_frames = pcm_size / (sizeof(int16_t) * channels);
    const size_t maximum_output_frames =
        ((uint64_t)input_frames * APP_AUDIO_SAMPLE_RATE + source_rate - 1) / source_rate + 2;
    if (maximum_output_frames * 2 * sizeof(int16_t) > session->converted_capacity) {
        ESP_LOGE(TAG, "PCM conversion buffer too small: need %u bytes",
                 (unsigned)(maximum_output_frames * 2 * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }

    size_t output_frames = 0;
    for (size_t frame = 0; frame < input_frames; ++frame) {
        const int16_t left = input[frame * channels];
        const int16_t right = channels == 2 ? input[frame * channels + 1] : left;
        session->sample_sum_left += left;
        session->sample_sum_right += right;
        session->sample_sum_count++;
        session->rate_phase += APP_AUDIO_SAMPLE_RATE;
        if (session->rate_phase >= source_rate) {
            const int16_t output_left =
                (int16_t)(session->sample_sum_left / session->sample_sum_count);
            const int16_t output_right =
                (int16_t)(session->sample_sum_right / session->sample_sum_count);
            while (session->rate_phase >= source_rate) {
                session->converted[output_frames * 2] = output_left;
                session->converted[output_frames * 2 + 1] = output_right;
                output_frames++;
                session->rate_phase -= source_rate;
            }
            session->sample_sum_left = 0;
            session->sample_sum_right = 0;
            session->sample_sum_count = 0;
        }
    }
    if (output_frames == 0) {
        return ESP_OK;
    }

    int peak = 0;
    for (size_t i = 0; i < output_frames * 2; ++i) {
        const int sample = session->converted[i];
        const int level = sample < 0 ? -sample : sample;
        if (level > peak) {
            peak = level;
        }
    }
    const int output_bytes = (int)(output_frames * 2 * sizeof(int16_t));
    if (!atomic_exchange(&s_audio.playing, true)) {
        esp_codec_dev_set_out_mute(s_audio.speaker, false);
        emit_state(RADIO_AUDIO_RUNNING);
    }
    const int codec_err = esp_codec_dev_write(s_audio.speaker, session->converted, output_bytes);
    if (codec_err != ESP_CODEC_DEV_OK) {
        atomic_fetch_add(&s_audio.i2s_errors, 1);
        ESP_LOGW(TAG, "ES8389 write failed: %d (%d bytes)", codec_err, output_bytes);
        return ESP_FAIL;
    }
    atomic_fetch_add(&s_audio.pcm_callbacks, 1);
    atomic_fetch_add(&s_audio.pcm_bytes, (unsigned)output_bytes);
    if (peak > atomic_load(&s_audio.peak_level)) {
        atomic_store(&s_audio.peak_level, peak);
    }
    int visual_peak = atomic_load(&s_audio.visual_peak);
    while (peak > visual_peak &&
           !atomic_compare_exchange_weak(&s_audio.visual_peak, &visual_peak, peak)) {
    }
    if (!atomic_exchange(&s_audio.pcm_started_logged, true)) {
        ESP_LOGI(TAG, "Lightweight PCM reached ES8389: %d bytes, peak=%d, volume=%d",
                 output_bytes, peak, atomic_load(&s_audio.volume));
    }
    return ESP_OK;
}

static esp_err_t decode_feed(decoder_session_t *session, uint8_t *data, size_t data_size)
{
    esp_audio_simple_dec_raw_t raw = {
        .buffer = data,
        .len = data_size,
        .eos = false,
    };
    unsigned stalled = 0;
    while (raw.len > 0 && !atomic_load(&s_audio.abort_requested)) {
        esp_audio_simple_dec_out_t output = {
            .buffer = session->decoded,
            .len = session->decoded_capacity,
        };
        const esp_audio_err_t ret = esp_audio_simple_dec_process(session->decoder, &raw, &output);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (output.needed_size == 0 || output.needed_size > 64 * 1024) {
                return ESP_ERR_INVALID_SIZE;
            }
            uint8_t *larger = realloc(session->decoded, output.needed_size);
            if (larger == NULL) {
                return ESP_ERR_NO_MEM;
            }
            session->decoded = larger;
            session->decoded_capacity = output.needed_size;
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Lightweight decode failed: type=%d ret=%d", (int)session->type, ret);
            return ESP_FAIL;
        }
        if (output.decoded_size > 0) {
            esp_err_t err = write_converted_pcm(session, output.buffer, output.decoded_size);
            if (err != ESP_OK) {
                return err;
            }
        }
        if (raw.consumed > raw.len) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (raw.consumed == 0) {
            if (++stalled > 8) {
                ESP_LOGE(TAG, "Decoder made no progress with %lu input bytes", (unsigned long)raw.len);
                return ESP_FAIL;
            }
        } else {
            stalled = 0;
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }
    }
    return atomic_load(&s_audio.abort_requested) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static void encoded_reader_task(void *argument)
{
    encoded_reader_t *reader = (encoded_reader_t *)argument;
    uint8_t *input = malloc(NETWORK_READ_SIZE);
    reader->result = input == NULL ? ESP_ERR_NO_MEM : ESP_OK;

    while (input != NULL && !atomic_load(&s_audio.abort_requested) &&
           !atomic_load(&s_audio.network_stop)) {
        const int read = source_read_callback(input, NETWORK_READ_SIZE, NULL);
        if (read <= 0) {
            reader->result = read < 0 ? ESP_FAIL : ESP_OK;
            break;
        }

        size_t sent = 0;
        while (sent < (size_t)read && !atomic_load(&s_audio.abort_requested) &&
               !atomic_load(&s_audio.network_stop)) {
            sent += xStreamBufferSend(reader->buffer, input + sent,
                                      (size_t)read - sent, pdMS_TO_TICKS(50));
        }
    }

    free(input);
    atomic_store(&reader->done, true);
    vTaskDelete(NULL);
}

static size_t encoded_buffer_target(uint32_t decoder_bitrate, unsigned seconds,
                                    size_t default_target, size_t minimum,
                                    size_t maximum)
{
    uint64_t bitrate = decoder_bitrate;
    if (bitrate < 8000 || bitrate > 512000) {
        bitrate = s_audio.source.bitrate_kbps > 0 ?
                  (uint64_t)s_audio.source.bitrate_kbps * 1000 : 0;
    }
    uint64_t target = bitrate > 0 ? bitrate * seconds / 8 : default_target;
    if (target < minimum) {
        target = minimum;
    } else if (target > maximum) {
        target = maximum;
    }
    return (size_t)target;
}

static esp_err_t decode_http_source(decoder_session_t *session)
{
    uint8_t *input = malloc(DECODE_INPUT_SIZE);
    StreamBufferHandle_t encoded = xStreamBufferCreateWithCaps(
        ENCODED_BUFFER_SIZE, 1, MALLOC_CAP_SPIRAM);
    if (input == NULL || encoded == NULL) {
        ESP_LOGE(TAG, "Audio buffer allocation failed: internal free=%u largest=%u, PSRAM free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        free(input);
        if (encoded != NULL) {
            vStreamBufferDeleteWithCaps(encoded);
        }
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Encoded buffer allocated in PSRAM: %u KiB, PSRAM free=%u KiB",
             ENCODED_BUFFER_SIZE / 1024,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    encoded_reader_t reader = {
        .buffer = encoded,
        .result = ESP_OK,
    };
    atomic_init(&reader.done, false);
    atomic_store(&s_audio.network_stop, false);
    if (xTaskCreatePinnedToCore(encoded_reader_task, "audio_net", 4096, &reader,
                                5, NULL, 0) != pdPASS) {
        vStreamBufferDeleteWithCaps(encoded);
        free(input);
        return ESP_ERR_NO_MEM;
    }

    const size_t initial_target = encoded_buffer_target(
        0, STARTUP_BUFFER_SECONDS, STARTUP_BUFFER_DEFAULT,
        STARTUP_BUFFER_MIN, STARTUP_BUFFER_MAX);
    const int64_t buffering_started = esp_timer_get_time();
    while (!atomic_load(&s_audio.abort_requested) && !atomic_load(&reader.done) &&
           xStreamBufferBytesAvailable(encoded) < initial_target) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!atomic_load(&s_audio.abort_requested)) {
        const int64_t buffering_ms = (esp_timer_get_time() - buffering_started) / 1000;
        const size_t buffered = xStreamBufferBytesAvailable(encoded);
        const unsigned measured_kbps = buffering_ms > 0 ?
            (unsigned)((uint64_t)buffered * 8 / (uint64_t)buffering_ms) : 0;
        ESP_LOGI(TAG, "Startup buffered: %u/%u bytes in %lld ms (~%u kbps), icy-br=%d kbps; network CPU0 -> decoder CPU1",
                 (unsigned)buffered, (unsigned)initial_target,
                 buffering_ms, measured_kbps, s_audio.source.bitrate_kbps);
    }

    esp_err_t err = ESP_OK;
    bool pcm_started = false;
    while (!atomic_load(&s_audio.abort_requested)) {
        if (pcm_started && !atomic_load(&reader.done)) {
            const size_t recovery_target = encoded_buffer_target(
                session->info_ready ? session->info.bitrate : 0,
                RECOVERY_BUFFER_SECONDS, RECOVERY_BUFFER_DEFAULT,
                RECOVERY_BUFFER_MIN, RECOVERY_BUFFER_MAX);
            size_t low_watermark = recovery_target / 8;
            if (low_watermark < REBUFFER_LOW_MIN) {
                low_watermark = REBUFFER_LOW_MIN;
            }
            if (xStreamBufferBytesAvailable(encoded) < low_watermark) {
                const int64_t rebuffer_start = esp_timer_get_time();
                esp_codec_dev_set_out_mute(s_audio.speaker, true);
                atomic_fetch_add(&s_audio.buffer_underruns, 1);
                ESP_LOGW(TAG, "Encoded buffer low (%u bytes); rebuffering to %u bytes",
                         (unsigned)xStreamBufferBytesAvailable(encoded),
                         (unsigned)recovery_target);
                while (!atomic_load(&s_audio.abort_requested) &&
                       !atomic_load(&reader.done) &&
                       xStreamBufferBytesAvailable(encoded) < recovery_target) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                if (!atomic_load(&s_audio.abort_requested) && atomic_load(&s_audio.playing)) {
                    esp_codec_dev_set_out_mute(s_audio.speaker, false);
                }
                ESP_LOGI(TAG, "Rebuffer complete: %u bytes after %lld ms",
                         (unsigned)xStreamBufferBytesAvailable(encoded),
                         (esp_timer_get_time() - rebuffer_start) / 1000);
            }
        }
        const size_t read = xStreamBufferReceive(encoded, input, DECODE_INPUT_SIZE,
                                                 pdMS_TO_TICKS(100));
        if (read == 0) {
            if (atomic_load(&reader.done) && xStreamBufferBytesAvailable(encoded) == 0) {
                err = reader.result;
                break;
            }
            continue;
        }
        err = decode_feed(session, input, read);
        if (err != ESP_OK) {
            break;
        }
        pcm_started = atomic_load(&s_audio.playing);
    }

    atomic_store(&s_audio.network_stop, true);
    while (!atomic_load(&reader.done)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err == ESP_OK && reader.result != ESP_OK) {
        err = reader.result;
    }
    vStreamBufferDeleteWithCaps(encoded);
    free(input);
    return atomic_load(&s_audio.abort_requested) ? ESP_ERR_INVALID_STATE : err;
}

static int text_download_event(esp_http_client_event_t *event)
{
    text_download_t *download = (text_download_t *)event->user_data;
    if (download == NULL || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    size_t amount = (size_t)event->data_len;
    if (download->length + amount >= download->capacity) {
        amount = download->capacity - download->length - 1;
    }
    if (amount > 0) {
        memcpy(download->buffer + download->length, event->data, amount);
        download->length += amount;
        download->buffer[download->length] = '\0';
    }
    return ESP_OK;
}

static esp_err_t fetch_text(const char *url, char **result)
{
    *result = NULL;
    char *buffer = calloc(1, PLAYLIST_MAX_LENGTH);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    text_download_t download = {
        .buffer = buffer,
        .capacity = PLAYLIST_MAX_LENGTH,
    };
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = text_download_event,
        .user_data = &download,
        .user_agent = "ESP32S31-Korvo1-WebRadio/1.0",
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300 || download.length == 0) {
        ESP_LOGW(TAG, "Playlist request failed (%d): %s", status, esp_err_to_name(err));
        free(buffer);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    *result = buffer;
    return ESP_OK;
}

static char *trim_line(char *line)
{
    while (isspace((unsigned char)*line)) {
        ++line;
    }
    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return line;
}

static bool resolve_url(const char *base, const char *item, char *output, size_t output_size)
{
    if (strncasecmp(item, "http://", 7) == 0 || strncasecmp(item, "https://", 8) == 0) {
        return strlcpy(output, item, output_size) < output_size;
    }

    const char *scheme_end = strstr(base, "://");
    if (scheme_end == NULL) {
        return false;
    }
    if (item[0] == '/') {
        const char *host_end = strchr(scheme_end + 3, '/');
        const size_t prefix_length = host_end == NULL ? strlen(base) : (size_t)(host_end - base);
        return snprintf(output, output_size, "%.*s%s", (int)prefix_length, base, item) < (int)output_size;
    }

    const char *query = strchr(base, '?');
    size_t base_length = query == NULL ? strlen(base) : (size_t)(query - base);
    while (base_length > 0 && base[base_length - 1] != '/') {
        --base_length;
    }
    return snprintf(output, output_size, "%.*s%s", (int)base_length, base, item) < (int)output_size;
}

static bool url_path_has_suffix(const char *url, const char *suffix)
{
    const char *end = strpbrk(url, "?#");
    const size_t length = end == NULL ? strlen(url) : (size_t)(end - url);
    const size_t suffix_length = strlen(suffix);
    return length >= suffix_length && strncasecmp(url + length - suffix_length,
                                                  suffix, suffix_length) == 0;
}

static esp_err_t resolve_basic_playlist(const char *url, char *resolved, size_t resolved_size)
{
    char *text = NULL;
    esp_err_t err = fetch_text(url, &text);
    if (err != ESP_OK) {
        return err;
    }

    char *save = NULL;
    char *line = strtok_r(text, "\n", &save);
    err = ESP_ERR_NOT_FOUND;
    while (line != NULL) {
        line = trim_line(line);
        char *candidate = line;
        if (strncasecmp(line, "File", 4) == 0) {
            char *equals = strchr(line, '=');
            candidate = equals == NULL ? line : trim_line(equals + 1);
        }
        const bool explicit_url = strncasecmp(candidate, "http://", 7) == 0 ||
                                  strncasecmp(candidate, "https://", 8) == 0;
        const bool relative_media = candidate == line && strchr(candidate, '=') == NULL;
        if (candidate[0] != '\0' && candidate[0] != '#' && candidate[0] != '[' &&
            (explicit_url || relative_media)) {
            if (resolve_url(url, candidate, resolved, resolved_size)) {
                err = ESP_OK;
                break;
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    free(text);
    return err;
}

static const char *select_codec(const char *url)
{
    if (strcasestr(s_audio.source.content_type, "aac") != NULL ||
        url_path_has_suffix(url, ".aac")) {
        return "AAC";
    }
    if (strcasestr(s_audio.source.content_type, "mp2t") != NULL ||
        url_path_has_suffix(url, ".ts")) {
        return "TS";
    }
    return "MP3";
}

static esp_err_t prepare_source(const char *url, esp_audio_simple_dec_type_t *decoder_type)
{
    esp_err_t err = source_open(url);
    if (err != ESP_OK) {
        return err;
    }
    const char *codec = select_codec(url);
    strlcpy(s_audio.codec, codec, sizeof(s_audio.codec));
    *decoder_type = strcasecmp(codec, "AAC") == 0 ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC :
                    strcasecmp(codec, "TS") == 0 ? ESP_AUDIO_SIMPLE_DEC_TYPE_TS :
                    ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    return ESP_OK;
}

static esp_err_t play_direct_stream(const char *input_url, unsigned generation)
{
    char url[URL_MAX_LENGTH];
    strlcpy(url, input_url, sizeof(url));
    if (url_path_has_suffix(url, ".m3u") || url_path_has_suffix(url, ".pls")) {
        char playlist_item[URL_MAX_LENGTH];
        esp_err_t err = resolve_basic_playlist(url, playlist_item, sizeof(playlist_item));
        if (err != ESP_OK) {
            return err;
        }
        strlcpy(url, playlist_item, sizeof(url));
    }
    if (generation != atomic_load(&s_audio.generation) ||
        atomic_load(&s_audio.abort_requested)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_audio_simple_dec_type_t decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    esp_err_t err = prepare_source(url, &decoder_type);
    if (err != ESP_OK) {
        return err;
    }
    if (generation != atomic_load(&s_audio.generation) ||
        atomic_load(&s_audio.abort_requested)) {
        source_close();
        return ESP_ERR_INVALID_STATE;
    }
    decoder_session_t session = {0};
    err = decoder_session_open(&session, decoder_type);
    if (err == ESP_OK) {
        err = decode_http_source(&session);
    }
    decoder_session_close(&session);
    source_close();
    return err;
}

static int collect_hls_items(const char *playlist_url, char *text,
                             char items[][URL_MAX_LENGTH], int max_items,
                             bool *is_master)
{
    *is_master = strstr(text, "#EXT-X-STREAM-INF") != NULL;
    int count = 0;
    char *save = NULL;
    char *line = strtok_r(text, "\n", &save);
    while (line != NULL && count < max_items) {
        line = trim_line(line);
        if (line[0] != '\0' && line[0] != '#' &&
            resolve_url(playlist_url, line, items[count], URL_MAX_LENGTH)) {
            ++count;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return count;
}

static esp_err_t hls_fetch_items(const char *playlist_url,
                                 char items[][URL_MAX_LENGTH], int *count,
                                 bool *is_master)
{
    char *text = NULL;
    esp_err_t err = fetch_text(playlist_url, &text);
    if (err != ESP_OK) {
        return err;
    }
    *count = collect_hls_items(playlist_url, text, items, HLS_MAX_SEGMENTS, is_master);
    free(text);
    return *count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t play_hls(const char *master_url, unsigned generation)
{
    char playlist_url[URL_MAX_LENGTH];
    strlcpy(playlist_url, master_url, sizeof(playlist_url));
    char (*items)[URL_MAX_LENGTH] = calloc(HLS_MAX_SEGMENTS, sizeof(*items));
    if (items == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int count = 0;
    bool master = false;
    esp_err_t err = hls_fetch_items(playlist_url, items, &count, &master);
    if (err != ESP_OK) {
        free(items);
        return err;
    }
    if (master) {
        strlcpy(playlist_url, items[0], sizeof(playlist_url));
    }

    char last_segment[URL_MAX_LENGTH] = "";
    decoder_session_t session = {0};
    while (!atomic_load(&s_audio.abort_requested) &&
           generation == atomic_load(&s_audio.generation)) {
        err = hls_fetch_items(playlist_url, items, &count, &master);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        int start = 0;
        if (last_segment[0] == '\0') {
            start = count > 2 ? count - 2 : 0;
        } else {
            start = count;
            for (int i = 0; i < count; ++i) {
                if (strcmp(items[i], last_segment) == 0) {
                    start = i + 1;
                    break;
                }
            }
            if (start == count && strcmp(items[count - 1], last_segment) != 0) {
                start = count - 1;
            }
        }

        for (int i = start; i < count && !atomic_load(&s_audio.abort_requested); ++i) {
            esp_audio_simple_dec_type_t decoder_type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
            err = prepare_source(items[i], &decoder_type);
            if (err != ESP_OK) {
                break;
            }
            if (generation != atomic_load(&s_audio.generation)) {
                source_close();
                break;
            }
            if (session.decoder == NULL || session.type != decoder_type) {
                err = decoder_session_open(&session, decoder_type);
                if (err != ESP_OK) {
                    source_close();
                    break;
                }
            }
            err = decode_http_source(&session);
            source_close();
            if (err != ESP_OK && !atomic_load(&s_audio.abort_requested)) {
                break;
            }
            strlcpy(last_segment, items[i], sizeof(last_segment));
        }
        vTaskDelay(pdMS_TO_TICKS(750));
    }
    decoder_session_close(&session);
    source_close();
    free(items);
    return atomic_load(&s_audio.abort_requested) ? ESP_ERR_INVALID_STATE : err;
}

static void play_test_tone(unsigned generation)
{
    static const int16_t sine_1khz[16] = {
        0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
        0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
    };
    int16_t buffer[16 * 2 * 10];
    for (int cycle = 0; cycle < 10; ++cycle) {
        for (int i = 0; i < 16; ++i) {
            const int offset = (cycle * 16 + i) * 2;
            buffer[offset] = sine_1khz[i];
            buffer[offset + 1] = sine_1khz[i];
        }
    }

    esp_codec_dev_set_out_mute(s_audio.speaker, false);
    ESP_LOGI(TAG, "Speaker test: ES8389 + NS4150, 1 kHz, stereo, 16 kHz, 16-bit, 2 seconds");
    for (int block = 0; block < 200 &&
         generation == atomic_load(&s_audio.generation) &&
         !atomic_load(&s_audio.abort_requested); ++block) {
        const int err = esp_codec_dev_write(s_audio.speaker, buffer, sizeof(buffer));
        if (err != ESP_CODEC_DEV_OK) {
            atomic_fetch_add(&s_audio.i2s_errors, 1);
            ESP_LOGE(TAG, "Test tone codec write failed: %d", err);
            break;
        }
    }
    esp_codec_dev_set_out_mute(s_audio.speaker, true);
    ESP_LOGI(TAG, "Speaker test finished");
}

static void audio_worker(void *argument)
{
    (void)argument;
    audio_command_t command;
    while (true) {
        if (xQueueReceive(s_audio.command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        source_close();
        if (command.type == AUDIO_COMMAND_STOP) {
            atomic_store(&s_audio.playing, false);
            esp_codec_dev_set_out_mute(s_audio.speaker, true);
            emit_state(RADIO_AUDIO_STOPPED);
            continue;
        }

        if (command.generation != atomic_load(&s_audio.generation)) {
            continue;
        }
        atomic_store(&s_audio.abort_requested, false);
        atomic_store(&s_audio.network_stop, false);
        if (command.type == AUDIO_COMMAND_TEST) {
            play_test_tone(command.generation);
            emit_state(RADIO_AUDIO_STOPPED);
            continue;
        }
        ESP_LOGI(TAG, "Play: %s", command.url);
        atomic_store(&s_audio.playing, false);
        esp_codec_dev_set_out_mute(s_audio.speaker, true);

        esp_err_t err;
        if (url_path_has_suffix(command.url, ".m3u8")) {
            err = play_hls(command.url, command.generation);
        } else {
            err = play_direct_stream(command.url, command.generation);
        }
        source_close();
        atomic_store(&s_audio.playing, false);
        esp_codec_dev_set_out_mute(s_audio.speaker, true);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE &&
            command.generation == atomic_load(&s_audio.generation)) {
            ESP_LOGE(TAG, "Playback start failed: %s", esp_err_to_name(err));
            emit_state(RADIO_AUDIO_ERROR);
        } else if (err == ESP_OK && command.generation == atomic_load(&s_audio.generation)) {
            emit_state(RADIO_AUDIO_FINISHED);
        }
    }
}

esp_err_t radio_audio_init(radio_audio_event_callback_t callback, void *user_data)
{
    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.callback = callback;
    s_audio.callback_user_data = user_data;
    atomic_init(&s_audio.volume, APP_DEFAULT_VOLUME);
    atomic_init(&s_audio.generation, 0);
    atomic_init(&s_audio.abort_requested, false);
    atomic_init(&s_audio.playing, false);
    atomic_init(&s_audio.pcm_callbacks, 0);
    atomic_init(&s_audio.pcm_bytes, 0);
    atomic_init(&s_audio.i2s_errors, 0);
    atomic_init(&s_audio.buffer_underruns, 0);
    atomic_init(&s_audio.peak_level, 0);
    atomic_init(&s_audio.visual_peak, 0);
    atomic_init(&s_audio.pcm_started_logged, false);
    atomic_init(&s_audio.network_stop, false);

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(APP_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        return err;
    }
    err = bsp_audio_init(&standard_config);
    if (err != ESP_OK) {
        return err;
    }
    s_audio.speaker = bsp_audio_codec_speaker_init();
    if (s_audio.speaker == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = APP_AUDIO_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_audio.speaker, &sample_info) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(s_audio.speaker, APP_DEFAULT_VOLUME) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_mute(s_audio.speaker, true);
    ESP_LOGI(TAG, "Korvo-1 audio ready: ES8389 + NS4150, BCLK=GPIO%d LRCK=GPIO%d DOUT=GPIO%d, %d Hz stereo 16-bit",
             BSP_I2S_SCLK, BSP_I2S_LCLK, BSP_I2S_DOUT, APP_AUDIO_SAMPLE_RATE);

    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Lightweight MP3/AAC/TS decoder registered (GMF disabled)");

    s_audio.command_queue = xQueueCreate(1, sizeof(audio_command_t));
    if (s_audio.command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(audio_worker, "audio_decode", 12288, NULL, 7, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t radio_audio_play(const char *url)
{
    if (url == NULL || url[0] == '\0' || s_audio.command_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_command_t command = {
        .type = AUDIO_COMMAND_PLAY,
        .generation = atomic_fetch_add(&s_audio.generation, 1) + 1,
    };
    if (strlcpy(command.url, url, sizeof(command.url)) >= sizeof(command.url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    atomic_store(&s_audio.abort_requested, true);
    atomic_store(&s_audio.pcm_callbacks, 0);
    atomic_store(&s_audio.pcm_bytes, 0);
    atomic_store(&s_audio.i2s_errors, 0);
    atomic_store(&s_audio.buffer_underruns, 0);
    atomic_store(&s_audio.peak_level, 0);
    atomic_store(&s_audio.visual_peak, 0);
    atomic_store(&s_audio.pcm_started_logged, false);
    atomic_store(&s_audio.network_stop, true);
    return xQueueOverwrite(s_audio.command_queue, &command) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t radio_audio_test_tone(void)
{
    if (s_audio.command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_command_t command = {
        .type = AUDIO_COMMAND_TEST,
        .generation = atomic_fetch_add(&s_audio.generation, 1) + 1,
    };
    atomic_store(&s_audio.abort_requested, true);
    atomic_store(&s_audio.network_stop, true);
    return xQueueOverwrite(s_audio.command_queue, &command) == pdPASS ? ESP_OK : ESP_FAIL;
}

void radio_audio_stop(void)
{
    if (s_audio.command_queue == NULL) {
        return;
    }
    audio_command_t command = {
        .type = AUDIO_COMMAND_STOP,
        .generation = atomic_fetch_add(&s_audio.generation, 1) + 1,
    };
    atomic_store(&s_audio.abort_requested, true);
    atomic_store(&s_audio.network_stop, true);
    atomic_store(&s_audio.playing, false);
    if (s_audio.speaker != NULL) {
        esp_codec_dev_set_out_mute(s_audio.speaker, true);
    }
    xQueueOverwrite(s_audio.command_queue, &command);
}

void radio_audio_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > APP_MAX_VOLUME) {
        volume = APP_MAX_VOLUME;
    }
    atomic_store(&s_audio.volume, volume);
    if (s_audio.speaker != NULL) {
        const int err = esp_codec_dev_set_out_vol(s_audio.speaker, volume);
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "ES8389 volume update failed: %d", err);
        }
    }
}

bool radio_audio_is_running(void)
{
    return atomic_load(&s_audio.playing);
}

int radio_audio_take_visual_level(void)
{
    return atomic_exchange(&s_audio.visual_peak, 0);
}

void radio_audio_get_diagnostics(radio_audio_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL) {
        return;
    }
    diagnostics->pcm_callbacks = atomic_load(&s_audio.pcm_callbacks);
    diagnostics->pcm_bytes = atomic_load(&s_audio.pcm_bytes);
    diagnostics->i2s_errors = atomic_load(&s_audio.i2s_errors);
    diagnostics->buffer_underruns = atomic_load(&s_audio.buffer_underruns);
    diagnostics->peak_level = atomic_load(&s_audio.peak_level);
}
