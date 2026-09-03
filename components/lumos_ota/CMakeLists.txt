idf_component_register(
    SRCS "src/ota_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lumos_core app_update esp_http_server esp_https_ota esp_http_client
)
