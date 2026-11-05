#include "wifi_manager_bridge.h"

#include <cstring>
#include <string>

#include "ssid_manager.h"
#include "wifi_manager.h"

static wifi_manager_event_cb_t s_event_callback = nullptr;
static void *s_event_user_data = nullptr;

static wifi_manager_event_t map_event(WifiEvent event)
{
    switch (event) {
    case WifiEvent::Scanning:
        return WIFI_MANAGER_EVENT_SCANNING;
    case WifiEvent::Connecting:
        return WIFI_MANAGER_EVENT_CONNECTING;
    case WifiEvent::Connected:
        return WIFI_MANAGER_EVENT_CONNECTED;
    case WifiEvent::Disconnected:
        return WIFI_MANAGER_EVENT_DISCONNECTED;
    case WifiEvent::ConfigModeEnter:
        return WIFI_MANAGER_EVENT_CONFIG_MODE_ENTER;
    case WifiEvent::ConfigModeExit:
        return WIFI_MANAGER_EVENT_CONFIG_MODE_EXIT;
    }

    return WIFI_MANAGER_EVENT_DISCONNECTED;
}

static void copy_string(const std::string& value, char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0) {
        return;
    }
    std::strncpy(buffer, value.empty() ? "-" : value.c_str(), buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
}

extern "C" bool wifi_manager_initialize(const wifi_manager_config_t *config)
{
    if (config == nullptr) {
        return false;
    }

    WifiManagerConfig manager_config;
    manager_config.ssid_prefix = config->ssid_prefix != nullptr ? config->ssid_prefix : "WEBRADIO";
    manager_config.language = config->language != nullptr ? config->language : "zh-CN";
    manager_config.station_scan_min_interval_seconds = config->station_scan_min_interval_seconds;
    manager_config.station_scan_max_interval_seconds = config->station_scan_max_interval_seconds;
    manager_config.station_failure_retry_cnt = config->station_failure_retry_count;
    manager_config.show_ota_config = false;
    manager_config.show_sleep_config = false;
    return WifiManager::GetInstance().Initialize(manager_config);
}

extern "C" void wifi_manager_set_event_callback(wifi_manager_event_cb_t callback, void *user_data)
{
    s_event_callback = callback;
    s_event_user_data = user_data;

    WifiManager::GetInstance().SetEventCallback([](WifiEvent event, const std::string& data) {
        if (s_event_callback != nullptr) {
            s_event_callback(map_event(event), data.c_str(), s_event_user_data);
        }
    });
}

extern "C" void wifi_manager_start_station(void)
{
    WifiManager::GetInstance().StartStation();
}

extern "C" void wifi_manager_start_config_ap(void)
{
    WifiManager::GetInstance().StartConfigAp();
}

extern "C" bool wifi_manager_is_connected(void)
{
    return WifiManager::GetInstance().IsConnected();
}

extern "C" bool wifi_manager_is_config_mode(void)
{
    return WifiManager::GetInstance().IsConfigMode();
}

extern "C" bool wifi_manager_has_saved_credentials(void)
{
    return !SsidManager::GetInstance().GetSsidList().empty();
}

extern "C" void wifi_manager_get_ap_ssid(char *buffer, size_t buffer_size)
{
    copy_string(WifiManager::GetInstance().GetApSsid(), buffer, buffer_size);
}

extern "C" void wifi_manager_get_station_ssid(char *buffer, size_t buffer_size)
{
    copy_string(WifiManager::GetInstance().GetSsid(), buffer, buffer_size);
}

extern "C" void wifi_manager_get_ip_address(char *buffer, size_t buffer_size)
{
    copy_string(WifiManager::GetInstance().GetIpAddress(), buffer, buffer_size);
}
