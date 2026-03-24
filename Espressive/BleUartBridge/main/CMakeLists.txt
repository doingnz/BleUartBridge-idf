idf_component_register(
    SRCS
        "main.c"
        "ble_nus.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        driver
        esp_driver_uart
        esp_driver_gpio
        nvs_flash
        esp_system
)
