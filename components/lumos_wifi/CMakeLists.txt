idf_component_register(
    SRCS "src/wifi_service.cpp" "src/captive_dns.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lumos_core lumos_preferences esp_wifi esp_event esp_netif esp_timer nvs_flash mdns lwip freertos
)
