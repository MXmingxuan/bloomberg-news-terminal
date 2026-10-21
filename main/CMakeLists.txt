idf_component_register(
    SRCS "main.c" "usage_transport.c" "news_feed.c" "usage_wifi.c" "usage_ui.c" "usage_settings.c" "news_cjk_16.c" "news_cjk_22.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_wifi6_touch_lcd_7b lvgl__lvgl esp_wifi esp_event esp_netif nvs_flash
             esp_http_client esp_websocket_client json esp_timer lwip driver)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
