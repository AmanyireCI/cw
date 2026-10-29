idf_component_register(
    SRCS "main.c" "hw.c" "i2c_dev.c" "storage.c" "config.c" "auth.c"
         "coordinator.c" "entrance.c" "room.c" "schedule.c" "cli.c" "stress.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc fatfs sdmmc nvs_flash console mbedtls esp_timer vfs
)
