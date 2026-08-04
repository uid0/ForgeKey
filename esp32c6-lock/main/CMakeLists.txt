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
    # Component names must match the ESP-IDF component directory names exactly;
    # a typo here fails the build at configure time with "Failed to resolve
    # component". Note the ones that do not follow the esp_* convention:
    # esp-tls is hyphenated, the MQTT component is mqtt (esp-mqtt is the
    # submodule inside it), and the OTA ops component is app_update.
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp-tls
        esp_https_ota
        esp_http_client
        esp_http_server
        mqtt
        nvs_flash
        json
        esp_partition
        spi_flash
        app_update
        # app_update only privately requires efuse, so esp_efuse.h is not on our
        # include path without this. ota_update.c guards its anti-rollback checks
        # with __has_include("esp_efuse.h") — drop this and they silently compile
        # out and the device reports ota_anti_rollback_supported=false.
        efuse
        driver
        esp_timer
        esp_system
        esp_adc
        lwip
        mbedtls
)

# Add compile definitions
target_compile_options(${COMPONENT_TARGET} PRIVATE -Wall)
