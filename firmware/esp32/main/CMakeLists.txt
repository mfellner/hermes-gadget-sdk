if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/wifi_setup.html" HG_WIFI_SETUP_HTML)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/wifi_setup.html")
    configure_file(wifi_setup_page.hpp.in wifi_setup_page.hpp @ONLY)
endif()

# Chips without an I80 LCD peripheral (ESP32-C6) build without the parallel panel.
set(hg_optional_srcs)
if(CONFIG_SOC_LCD_I80_SUPPORTED)
    list(APPEND hg_optional_srcs "port_parallel.cpp")
endif()
# The ESP32-C6 board runs as an app of the esp32-playground multi-app platform
# (requirements may depend on the target, not on Kconfig).
idf_build_get_property(hg_target IDF_TARGET)
set(hg_platform_requires)
if(hg_target STREQUAL "esp32c6")
    list(APPEND hg_platform_requires mfellner__app_switch)
endif()

idf_component_register(
    SRCS
        ${hg_optional_srcs}
        "main.cpp"
        "board.cpp"
        "port_amoled.cpp"
        "port_audio.cpp"
        "port_buttons.cpp"
        "port_codec.cpp"
        "port_cores3.cpp"
        "port_console.cpp"
        "port_diag.cpp"
        "port_display.cpp"
        "port_events.cpp"
        "port_ota.cpp"
        "port_platform.cpp"
        "port_power.cpp"
        "port_latch_power.cpp"
        "../../drivers/axp2101.cpp"
        "../../drivers/cores3.cpp"
        "port_storage.cpp"
        "port_system.cpp"
        "port_touch.cpp"
        "port_wifi.cpp"
        "port_wifi_setup.cpp"
        "port_ws.cpp"
    INCLUDE_DIRS "." "../../drivers" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES core
    # "driver" pulls in GPIO, I2S, LEDC and SPI on every ESP-IDF 5.x release.
    # ESP-IDF 6.x dropped I2S and LEDC from that catch-all, so name them too;
    # both components exist in 5.x as well, where this is a no-op.
    PRIV_REQUIRES
        app_update
        driver
        esp_driver_i2s
        esp_driver_ledc
        esp_app_format
        esp_adc
        esp_event
        esp_lcd
        esp_netif
        esp_http_server
        esp_psram
        esp_timer
        esp_wifi
        console
        nvs_flash
        mbedtls
        esp_websocket_client
        ${hg_platform_requires}
)
