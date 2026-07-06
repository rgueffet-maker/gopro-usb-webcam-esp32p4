idf_component_register(
    SRCS
        "eth_hello_main.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        nvs_flash
        usb
        esp_netif
        esp_event
        esp_eth
        esp_http_server
        esp_http_client
        lwip
        esp_driver_gpio
        esp_timer
        esp_system

    PRIV_REQUIRES
        app_update
)