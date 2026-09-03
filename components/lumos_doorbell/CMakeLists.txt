idf_component_register(
    SRCS "src/doorbell_receiver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lumos_core lumos_preferences esp_wifi esp_timer driver freertos
)
