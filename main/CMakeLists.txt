idf_component_register(
    SRCS "main.c"
         "board/display.c"
         "board/sdcard.c"
         "board/touch.c"
         "board/gsl3680/esp_lcd_gsl3680.c"
         "board/gsl3680/gsl_point_id.c"
         "photo/photo.c"
         "photo/playlist.c"
         "photo/transitions.c"
         "ui/overlay.c"
         "ui/theme.c"
         "ui/icons.c"
         "ui/fonts.c"
         "ui/lv_mem_psram.c"
         "net/sun_moon.c"
         "net/wifi.c"
         "net/webui.c"
         "net/upload.c"
         "net/fetch.c"
         "net/weather.c"
         "net/airquality.c"
         "net/immich.c"
         "settings.c"
    EMBED_FILES "ui/fonts/IBMPlexSansCondensed-Regular.ttf"
                "ui/fonts/IBMPlexSansCondensed-SemiBold.ttf"
    INCLUDE_DIRS "." "board" "board/gsl3680" "photo" "ui" "net"
    REQUIRES esp_psram esp_timer spi_flash esp_driver_gpio
             esp_lcd esp_driver_ppa esp_driver_jpeg
             esp_driver_sdmmc sdmmc fatfs vfs esp_driver_i2c
             espressif__esp_new_jpeg lvgl__lvgl nvs_flash espressif__esp_lcd_touch
             esp_wifi esp_netif esp_event lwip espressif__esp_hosted esp_http_server esp_http_client esp_hw_support esp-tls mbedtls json
    WHOLE_ARCHIVE
)

# LVGL is configured with LV_STDLIB_CUSTOM, so it references lv_malloc_core
# and friends without providing them. Those live in ui/lv_mem_psram.c here in
# main, which the linker searches AFTER liblvgl -- so without WHOLE_ARCHIVE
# above the symbols are never pulled in and the link fails with undefined
# references from inside LVGL itself.
