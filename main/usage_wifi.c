#include "usage_transport.h"
#include "usage_settings.h"
#include "usage_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <time.h>
#include <sys/time.h>

#define WIFI_CONNECTED BIT0
#define INITIAL_RESPONSE_CAPACITY (512 * 1024)
#define WS_RESPONSE_CAPACITY (128 * 1024)
#define FEED_REFRESH_INTERVAL_MS 30000
#define WSCN_LIVE_URL "https://api-one-wscn.awtmt.com/apiv1/content/lives?channel=global-channel&client=pc&limit=24&first_page=true&accept=live%2Cvip-live"
#define WSCN_WS_URL "wss://streamer-prod-wscn.awtmt.com/wsv1/realtime"

static const char *TAG = "finance_news";
static EventGroupHandle_t wifi_events;
static NEWS_FEED_STORAGE_ATTR char http_response[INITIAL_RESPONSE_CAPACITY];
static size_t http_response_length;
static NEWS_FEED_STORAGE_ATTR char ws_response[WS_RESPONSE_CAPACITY];
static news_ws_buffer_t ws_buffer = {.data = ws_response, .capacity = sizeof(ws_response)};
static NEWS_FEED_STORAGE_ATTR news_state_t news_state;
static NEWS_FEED_STORAGE_ATTR news_feed_t decoded_feed;
static NEWS_FEED_STORAGE_ATTR news_item_t decoded_live_item;
static SemaphoreHandle_t feed_mutex;
static atomic_bool system_time_synchronized = ATOMIC_VAR_INIT(false);
static atomic_bool wifi_connected = ATOMIC_VAR_INIT(false);
static atomic_bool restart_websocket = ATOMIC_VAR_INIT(false);
static atomic_bool force_rest_refresh = ATOMIC_VAR_INIT(false);
static atomic_int last_source_contact_s = ATOMIC_VAR_INIT(-1);
static atomic_int last_ws_contact_s = ATOMIC_VAR_INIT(0);
static int64_t subscription_cursor; /* protected by feed_mutex */

bool usage_wifi_is_connected(void) {
    return atomic_load(&wifi_connected);
}

bool usage_wifi_source_is_fresh(void) {
    const int last = atomic_load(&last_source_contact_s);
    return usage_wifi_is_connected() && last >= 0 && esp_timer_get_time() / 1000000 - last < 90;
}

bool usage_wifi_time_is_synchronized(void) {
    return atomic_load(&system_time_synchronized);
}

static void time_sync_notification(struct timeval *tv) {
    if (!tv) return;
    atomic_store(&system_time_synchronized, true);
    ESP_LOGI(TAG, "System time synchronized for verified TLS (epoch %lld)", (long long)tv->tv_sec);
}

static int month_number(const char *month) {
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++) if (strncmp(month, months[i], 3) == 0) return i;
    return -1;
}

static bool sync_system_time_from_http_date(const char *date_header) {
    if (!date_header) return false;
    /* HTTPS Date bootstraps the clock once; SNTP owns subsequent corrections. */
    if (usage_wifi_time_is_synchronized()) return true;
    char weekday[4] = {0}, month[4] = {0}, timezone[4] = {0};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(date_header, "%3[^,], %d %3s %d %d:%d:%d %3s",
               weekday, &day, month, &year, &hour, &minute, &second, timezone) != 8 ||
        strcmp(timezone, "GMT") != 0 || day < 1 || day > 31 || year < 2024 ||
        hour > 23 || minute > 59 || second > 60) return false;
    const int month_index = month_number(month);
    if (month_index < 0) return false;
    struct tm server_time = {
        .tm_year = year - 1900,
        .tm_mon = month_index,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
        .tm_isdst = 0,
    };
    const time_t epoch = mktime(&server_time);
    if (epoch < 1704067200) return false;
    const struct timeval now = {.tv_sec = epoch, .tv_usec = 0};
    if (settimeofday(&now, NULL) != 0) return false;
    atomic_store(&system_time_synchronized, true);
    ESP_LOGI(TAG, "System time synchronized from HTTPS Date header (epoch %lld)", (long long)epoch);
    return true;
}

static void seed_system_time_from_build(void) {
    char month[4] = {0};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) return;
    const int month_index = month_number(month);
    if (month_index < 0) return;
    struct tm build_time = {
        .tm_year = year - 1900,
        .tm_mon = month_index,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
    };
    setenv("TZ", "UTC0", 1);
    tzset();
    const time_t epoch = mktime(&build_time);
    if (epoch < 1704067200) return;
    const struct timeval now = {.tv_sec = epoch, .tv_usec = 0};
    settimeofday(&now, NULL);
}

static esp_err_t http_event(esp_http_client_event_t *event) {
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        strcasecmp(event->header_key, "Date") == 0) {
        sync_system_time_from_http_date(event->header_value);
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0) return ESP_OK;
    if (http_response_length + (size_t)event->data_len >= sizeof(http_response)) {
        http_response_length = sizeof(http_response);
        return ESP_FAIL;
    }
    memcpy(http_response + http_response_length, event->data, (size_t)event->data_len);
    http_response_length += (size_t)event->data_len;
    http_response[http_response_length] = '\0';
    return ESP_OK;
}

static bool fetch_latest_feed(void) {
    const esp_http_client_config_t config = {
        .url = WSCN_LIVE_URL,
        .event_handler = http_event,
        .timeout_ms = 15000,
        .buffer_size = 8192,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_header(client, "Referer", "https://wallstreetcn.com/live/global");
    esp_http_client_set_header(client, "Accept", "application/json");
    http_response_length = 0;
    http_response[0] = '\0';
    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK || status != 200 || http_response_length == 0 || http_response_length >= sizeof(http_response)) {
        ESP_LOGW(TAG, "Initial feed request failed: %s, HTTP %d, response bytes %u%s",
                 esp_err_to_name(result), status, (unsigned)http_response_length,
                 http_response_length >= sizeof(http_response) ? " (buffer limit reached)" : "");
        return false;
    }

    int64_t cursor = 0;
    if (!news_decode_snapshot(http_response, http_response_length, &decoded_feed, &cursor)) {
        ESP_LOGW(TAG, "REST response malformed; retaining previous headlines");
        return false;
    }
    const int now_s = (int)(esp_timer_get_time() / 1000000);
    xSemaphoreTake(feed_mutex, portMAX_DELAY);
    const bool stale = cursor < news_state.cursor;
    const bool changed = news_apply_snapshot(&news_state, &decoded_feed, cursor);
    if (changed) usage_transport_publish_news(&news_state.feed, "Wi-Fi 新闻源已同步");
    /* A moving source cursor with no matching WS traffic indicates a silent subscription. */
    const int64_t ws_cursor = news_state.last_event_cursor > subscription_cursor ?
                              news_state.last_event_cursor : subscription_cursor;
    if (subscription_cursor > 0 && cursor > ws_cursor &&
        now_s - atomic_load(&last_ws_contact_s) >= 90) atomic_store(&restart_websocket, true);
    xSemaphoreGive(feed_mutex);
    if (!stale) atomic_store(&last_source_contact_s, now_s);
    ESP_LOGI(TAG, "REST feed sync: %u headlines, cursor %lld, %s",
             (unsigned)decoded_feed.count, (long long)cursor,
             stale ? "stale snapshot ignored" : changed ? "display refreshed" : "no new stories");
    static unsigned sync_count;
    if (sync_count++ % 10 == 0) {
        ESP_LOGI(TAG, "Memory: internal free %u, PSRAM free %u, REST task stack watermark %u, heap integrity %s",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 heap_caps_check_integrity_all(true) ? "OK" : "FAILED");
    }
    return true;
}

static void accept_live_event(const char *payload, size_t length) {
    xSemaphoreTake(feed_mutex, portMAX_DELAY);
    news_event_result_t result = news_apply_event(&news_state, payload, length, &decoded_live_item);
    const int64_t cursor = news_state.cursor;
    if (result == NEWS_EVENT_CHANGED) usage_transport_publish_news(&news_state.feed, "Wi-Fi 实时快讯已更新");
    xSemaphoreGive(feed_mutex);
    if (result >= NEWS_EVENT_UNCHANGED) {
        const int now_s = (int)(esp_timer_get_time() / 1000000);
        atomic_store(&last_ws_contact_s, now_s);
        atomic_store(&last_source_contact_s, now_s);
    }
    if (result == NEWS_EVENT_CHANGED) ESP_LOGI(TAG, "WS news applied (cursor %lld)", (long long)cursor);
    else if (result == NEWS_EVENT_INVALID) ESP_LOGW(TAG, "Unrecognized WS event; REST will reconcile: %.200s", payload);
}

static void websocket_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)base;
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)handler_args;
    esp_websocket_event_data_t *event = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        char subscription[160];
        char cursor[24];
        xSemaphoreTake(feed_mutex, portMAX_DELAY);
        subscription_cursor = news_state.cursor;
        snprintf(cursor, sizeof(cursor), "%lld", (long long)subscription_cursor);
        xSemaphoreGive(feed_mutex);
        snprintf(subscription, sizeof(subscription),
                 "{\"command\":\"ENTER_CHANNEL\",\"data\":{\"chann_name\":\"live\",\"cursor\":\"%s\"}}",
                 cursor);
        int sent = esp_websocket_client_send_text(client, subscription, (int)strlen(subscription), pdMS_TO_TICKS(5000));
        if (sent == (int)strlen(subscription)) {
            atomic_store(&last_ws_contact_s, (int)(esp_timer_get_time() / 1000000));
            usage_transport_set_status("Wi-Fi 已连接，实时快讯推送中");
            ESP_LOGI(TAG, "WallstreetCN live WebSocket connected; subscribed from cursor %s", cursor);
        } else {
            usage_transport_set_status("实时新闻订阅失败，正在重连");
            ESP_LOGW(TAG, "Failed to subscribe to the live channel");
            atomic_store(&restart_websocket, true);
        }
        ws_buffer.length = 0;
        ws_buffer.active = false;
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
        ws_buffer.length = 0;
        ws_buffer.active = false;
        usage_transport_set_status("实时推送断开，自动重连中");
        ESP_LOGW(TAG, "Live WebSocket disconnected; automatic reconnect enabled");
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW(TAG, "Live WebSocket transport error");
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || !event) return;
    const int result = news_ws_append(&ws_buffer, event->op_code, event->fin,
                                     event->payload_len, event->payload_offset,
                                     event->data_ptr, event->data_len);
    if (result == 1) accept_live_event(ws_buffer.data, ws_buffer.length);
    else if (result < 0) {
        ESP_LOGW(TAG, "Rejected WS message (opcode %d offset %d chunk %d frame %d); requesting REST sync",
                 event->op_code, event->payload_offset, event->data_len, event->payload_len);
        atomic_store(&force_rest_refresh, true);
    }
}

static esp_websocket_client_handle_t start_live_websocket(void) {
    static const char headers[] =
        "Origin: https://wallstreetcn.com\r\n"
        "Referer: https://wallstreetcn.com/live/global\r\n";
    const esp_websocket_client_config_t config = {
        .uri = WSCN_WS_URL,
        .headers = headers,
        .buffer_size = 4096,
        .network_timeout_ms = 15000,
        .reconnect_timeout_ms = 5000,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 60,
        .enable_close_reconnect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .task_name = "wscn-live-ws",
        .task_stack = 6144,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (!client) return NULL;
    if (esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event, client) != ESP_OK ||
        esp_websocket_client_start(client) != ESP_OK) {
        esp_websocket_client_destroy(client);
        return NULL;
    }
    return client;
}

static void news_task(void *arg) {
    (void)arg;
    esp_websocket_client_handle_t websocket = NULL;
    bool have_snapshot = false;
    int64_t next_rest_ms = 0, next_ws_ms = 0, last_rest_attempt_ms = -5000;
    unsigned failures = 0;
    for (;;) {
        xEventGroupWaitBits(wifi_events, WIFI_CONNECTED, pdFALSE, pdTRUE, portMAX_DELAY);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (atomic_exchange(&force_rest_refresh, false) && now_ms - last_rest_attempt_ms >= 5000) next_rest_ms = 0;
        if (now_ms >= next_rest_ms) {
            last_rest_attempt_ms = now_ms;
            if (fetch_latest_feed()) {
                have_snapshot = true;
                failures = 0;
                next_rest_ms = esp_timer_get_time() / 1000 + FEED_REFRESH_INTERVAL_MS;
            } else {
                if (failures < 4) failures++;
                const int retry_ms = 5000 << (failures - 1);
                next_rest_ms = esp_timer_get_time() / 1000 + retry_ms;
                usage_transport_set_status("新闻源连接失败，保留快讯并自动重试");
                ESP_LOGW(TAG, "REST retry in %d ms", retry_ms);
            }
        }
        if (websocket && atomic_exchange(&restart_websocket, false)) {
            ESP_LOGW(TAG, "Restarting stalled WebSocket subscription from current REST cursor");
            esp_websocket_client_stop(websocket);
            esp_websocket_client_destroy(websocket);
            websocket = NULL;
            atomic_store(&restart_websocket, false);
            next_ws_ms = esp_timer_get_time() / 1000 + 1000;
        }
        now_ms = esp_timer_get_time() / 1000;
        if (have_snapshot && !websocket && now_ms >= next_ws_ms) {
            websocket = start_live_websocket();
            if (!websocket) {
                ESP_LOGW(TAG, "WebSocket start failed; REST updates remain active");
                next_ws_ms = now_ms + 5000;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&wifi_connected, false);
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED);
        usage_transport_set_status("Wi-Fi 断开，正在自动重连");
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&wifi_connected, true);
        atomic_store(&force_rest_refresh, true);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED);
        usage_transport_set_status("Wi-Fi 已连接，正在获取最新快讯");
        ESP_LOGI(TAG, "Wi-Fi connected; starting the direct news feed");
        static bool sntp_started;
        if (!sntp_started) {
            sntp_set_time_sync_notification_cb(time_sync_notification);
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "time.cloudflare.com");
            esp_sntp_init();
            sntp_started = true;
            ESP_LOGI(TAG, "Started network time synchronization (current epoch %lld)", (long long)time(NULL));
        }
    }
}

void usage_wifi_start(void) {
    feed_mutex = xSemaphoreCreateMutex();
    if (!feed_mutex) { usage_transport_set_status("新闻同步锁初始化失败"); return; }
    seed_system_time_from_build();
    char ssid[40] = {0};
    char password[72] = {0};
    const bool has_usage_config = usage_settings_get("ssid", ssid, sizeof(ssid)) == 0 && ssid[0];
    if (has_usage_config && usage_settings_get("password", password, sizeof(password)) != 0) password[0] = '\0';
    const bool has_build_config = !has_usage_config && CONFIG_NEWS_WIFI_SSID[0] != '\0';
    if (has_build_config) {
        snprintf(ssid, sizeof(ssid), "%s", CONFIG_NEWS_WIFI_SSID);
        snprintf(password, sizeof(password), "%s", CONFIG_NEWS_WIFI_PASSWORD);
    }
    wifi_events = xEventGroupCreate();
    if (!wifi_events) { usage_transport_set_status("Wi-Fi 初始化失败"); return; }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t config = {0};
    bool have_saved_driver_config = false;
    if (!has_usage_config && esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && config.sta.ssid[0]) {
        have_saved_driver_config = true;
    }
    if (!has_usage_config && !have_saved_driver_config && !has_build_config) {
        usage_transport_set_status("未找到已保存的 Wi-Fi 配置");
        ESP_LOGE(TAG, "No saved Wi-Fi configuration; configure Wi-Fi once before using the direct news feed");
        return;
    }
    if ((has_usage_config || has_build_config) && (strlen(ssid) > sizeof(config.sta.ssid) ||
        strlen(password) > sizeof(config.sta.password))) {
        usage_transport_set_status("Wi-Fi 配置长度无效，请重新配置");
        memset(password, 0, sizeof(password));
        return;
    }
    if (has_usage_config || has_build_config) {
        memset(&config, 0, sizeof(config));
        memcpy(config.sta.ssid, ssid, strlen(ssid));
        memcpy(config.sta.password, password, strlen(password));
        config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (has_usage_config || has_build_config) ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    memset(password, 0, sizeof(password));
    ESP_ERROR_CHECK(esp_wifi_start());
    usage_transport_set_status("正在连接已保存的 Wi-Fi");
    if (xTaskCreate(news_task, "finance_news", 8192, NULL, 5, NULL) != pdPASS) {
        usage_transport_set_status("财经新闻任务启动失败");
        ESP_LOGE(TAG, "Could not create the news task");
    }
}
