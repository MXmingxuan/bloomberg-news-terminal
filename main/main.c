#include "usage_transport.h"
#include "usage_ui.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "nvs_flash.h"

static const char *TAG = "usage_dashboard";
static NEWS_FEED_STORAGE_ATTR news_feed_t latest_feed;

static void refresh_dashboard(lv_timer_t *timer) {
    (void)timer;
    int64_t received_at_ms = 0;
    if (!usage_transport_get_news(&latest_feed, &received_at_ms)) {
        usage_ui_set_offline(0);
        return;
    }
    int64_t age_ms = esp_timer_get_time() / 1000 - received_at_ms;
    if (age_ms < 0) age_ms = 0;
    usage_ui_apply_news(&latest_feed, age_ms / 1000);
}

void app_main(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s); preserving existing partitions", esp_err_to_name(nvs));
        return;
    }
    bsp_display_cfg_t display_config = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        /* Compensate for mounting the board upside down so the USB-C power
           connector faces upward and does not obstruct the desk stand. */
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        /* Keep taps and drag vectors aligned after the 180-degree display turn. */
        .touch_flags = {.swap_xy = 1, .mirror_x = 0, .mirror_y = 1},
    };
    lv_display_t *display = bsp_display_start_with_config(&display_config);
    if (!display) { ESP_LOGE(TAG, "Display initialization failed"); return; }
    if (!bsp_display_lock(-1)) { ESP_LOGE(TAG, "LVGL lock failed"); return; }
    usage_ui_init();
    lv_timer_create(refresh_dashboard, 1000, NULL);
    bsp_display_unlock();
    bsp_display_backlight_on();
    usage_transport_start();
    ESP_LOGI(TAG, "Finance news display ready (portrait 600x1024)");
}
