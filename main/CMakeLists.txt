idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_event
        esp_netif
        esp_http_server
        esp_timer
        json
        lwip
        mdns
        freertos
        lumos_core
        lumos_preferences
        lumos_wifi
        lumos_doorbell
        lumos_hotkeys
        lumos_ota
        hotkeys_webui
)

