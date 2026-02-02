# Common source files for all targets
set(COMMON_SRCS
    "src/esp/esp_network.cc"
    "src/esp/esp_ssl.cc"
    "src/esp/esp_tcp.cc"
    "src/esp/esp_mqtt.cc"
    "src/esp/esp_udp.cc"
    "src/web_socket.cc"
    "src/http_client.cc"
)

# Additional source files for non-ESP32 targets (uart-uhci not supported on ESP32)
if(NOT IDF_TARGET STREQUAL "esp32")
    list(APPEND COMMON_SRCS
        "src/at_uart.cc"
        "src/at_modem.cc"
        "src/ec801e/ec801e_at_modem.cc"
        "src/ec801e/ec801e_tcp.cc"
        "src/ec801e/ec801e_ssl.cc"
        "src/ec801e/ec801e_udp.cc"
        "src/ec801e/ec801e_mqtt.cc"
        "src/ml307/ml307_at_modem.cc"
        "src/ml307/ml307_tcp.cc"
        "src/ml307/ml307_ssl.cc"
        "src/ml307/ml307_mqtt.cc"
        "src/ml307/ml307_udp.cc"
        "src/ml307/ml307_http.cc"
    )
endif()

idf_component_register(
    SRCS
        ${COMMON_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    REQUIRES
        "esp_driver_gpio"
        "esp_driver_uart"
        "esp_pm"
        "esp-tls"
        "pthread"
        "mqtt"
)
