idf_component_register(
    SRCS "src/hotkeys_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lumos_core lumos_preferences lumos_wifi esp_http_client esp_timer json freertos driver
)
