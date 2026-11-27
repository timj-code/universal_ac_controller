idf_component_register(
    SRC_DIRS "."
    PRIV_INCLUDE_DIRS
        "."
        "${ESP_MATTER_PATH}/examples/common/utils"

    PRIV_REQUIRES
        bc7215_ac
        device
        esp_matter
        esp_matter_console
        chip
        nvs_flash
        esp_driver_gpio
        esp_driver_uart
        esp_driver_i2c
        esp_driver_ledc
)

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE "-DCHIP_HAVE_CONFIG_H"
)
