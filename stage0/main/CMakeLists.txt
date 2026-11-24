idf_component_register(
    SRCS "stage0_main.c"
         "gate_ppa.c"
         "gate_jpeg.c"
         "gate_sdcard.c"
         "gate_bigphoto.c"
         "gate_touch.c"
         "gate_display.c"
         "gate_backlight.c"
    INCLUDE_DIRS "." "../../main/board"
    REQUIRES esp_driver_ppa espressif__esp_new_jpeg esp_driver_jpeg esp_lcd esp_driver_i2c
             esp_driver_sdmmc sdmmc fatfs esp_driver_gpio esp_driver_ledc
             esp_psram esp_timer vfs spi_flash
)
