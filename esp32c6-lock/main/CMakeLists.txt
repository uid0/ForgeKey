# ESP32-C6 Lock Device - Component CMakeLists.txt

idf_component_register(
    SRCS "main.c"
         "wifi_setup.c"
         "provisioning.c"
         "mqtt_handler.c"
         "lock_state.c"
         "web_server.c"
         "ota_update.c"
         "credential_rotation.c"
         "forgekey_time.c"
         "command_validation.c"
         "firmware_verify.c"
         "watchdog_manager.c"
         "boards/lock_board_manifest.c"
         "power/power_manager.c"
    INCLUDE_DIRS "." "${FORGEKEY_LOCK_GENERATED_DIR}"
    REQUIRES 
        esp_wifi
        esp_event
        esp_netif
        esp_tls
        esp_https_ota
        esp_http_client
        esp_http_server
        esp-mqtt
        nvs_flash
        json
        esp_partition
        spi_flash
        update
        driver
        esp_timer
        esp_system
        esp_adc
        esp_timer
        lwip
        mbedtls
)

# Add compile definitions
target_compile_options(${COMPONENT_TARGET} PRIVATE -Wall)
