#include "usage_transport.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "usage_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t snapshot_mutex;
static char status_text[80] = "正在启动财经新闻源";
static NEWS_FEED_STORAGE_ATTR news_feed_t latest_news;
static bool has_news;
static int64_t news_received_at_ms;

void usage_transport_start(void) {
    if (!snapshot_mutex) snapshot_mutex = xSemaphoreCreateMutex();
    if (!snapshot_mutex) { ESP_LOGE("news_transport", "Could not create snapshot mutex"); return; }
    usage_wifi_start();
}

bool usage_transport_publish_news(const news_feed_t *feed, const char *status) {
    if (!snapshot_mutex || !feed || feed->count > NEWS_FEED_MAX_ITEMS) return false;
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    latest_news = *feed;
    latest_news.updated_at = usage_wifi_time_is_synchronized() ? (int64_t)time(NULL) : 0;
    news_received_at_ms = esp_timer_get_time() / 1000;
    has_news = true;
    snprintf(status_text, sizeof(status_text), "%s", status ? status : "实时快讯已更新");
    xSemaphoreGive(snapshot_mutex);
    return true;
}

bool usage_transport_get_news(news_feed_t *out, int64_t *received_at) {
    if (!snapshot_mutex || !out) return false;
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    bool available = has_news;
    if (available) {
        if (latest_news.updated_at == 0 && usage_wifi_time_is_synchronized()) {
            const int64_t age_s = (esp_timer_get_time() / 1000 - news_received_at_ms) / 1000;
            latest_news.updated_at = (int64_t)time(NULL) - age_s;
        }
        *out = latest_news;
        if (received_at) *received_at = news_received_at_ms;
    }
    xSemaphoreGive(snapshot_mutex);
    return available;
}

void usage_transport_set_status(const char *status) {
    if (!snapshot_mutex) return;
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    snprintf(status_text, sizeof(status_text), "%s", status ? status : "Unavailable");
    xSemaphoreGive(snapshot_mutex);
}

void usage_transport_get_status(char *out, size_t capacity) {
    if (!out || capacity == 0) return;
    if (!snapshot_mutex) { snprintf(out, capacity, "Starting"); return; }
    xSemaphoreTake(snapshot_mutex, portMAX_DELAY);
    snprintf(out, capacity, "%s", status_text);
    xSemaphoreGive(snapshot_mutex);
}
