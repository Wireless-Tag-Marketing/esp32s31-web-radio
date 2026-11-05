idf_component_register(
    SRCS
        "main.c"
        "audio_player.c"
        "radio_ui.c"
        "stations.c"
        "wifi_manager_bridge.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_event
        esp_http_client
        esp_netif
        esp_timer
        esp_wifi
        mbedtls
        nvs_flash
        vfs
)
