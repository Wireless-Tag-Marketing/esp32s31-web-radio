#ifndef WIFI_MANAGER_BRIDGE_H
#define WIFI_MANAGER_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MANAGER_EVENT_SCANNING = 0,
    WIFI_MANAGER_EVENT_CONNECTING,
    WIFI_MANAGER_EVENT_CONNECTED,
    WIFI_MANAGER_EVENT_DISCONNECTED,
    WIFI_MANAGER_EVENT_CONFIG_MODE_ENTER,
    WIFI_MANAGER_EVENT_CONFIG_MODE_EXIT,
} wifi_manager_event_t;

typedef struct {
    const char *ssid_prefix;
    const char *language;
    int station_scan_min_interval_seconds;
    int station_scan_max_interval_seconds;
    uint8_t station_failure_retry_count;
} wifi_manager_config_t;

typedef void (*wifi_manager_event_cb_t)(wifi_manager_event_t event,
                                        const char *data,
                                        void *user_data);

bool wifi_manager_initialize(const wifi_manager_config_t *config);
void wifi_manager_set_event_callback(wifi_manager_event_cb_t callback, void *user_data);
void wifi_manager_start_station(void);
void wifi_manager_start_config_ap(void);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_config_mode(void);
bool wifi_manager_has_saved_credentials(void);
void wifi_manager_get_ap_ssid(char *buffer, size_t buffer_size);
void wifi_manager_get_station_ssid(char *buffer, size_t buffer_size);
void wifi_manager_get_ip_address(char *buffer, size_t buffer_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WIFI_MANAGER_BRIDGE_H */
