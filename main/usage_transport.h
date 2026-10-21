#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_attr.h"
#include "news_feed.h"
#define NEWS_FEED_STORAGE_ATTR EXT_RAM_BSS_ATTR

void usage_transport_start(void);
void usage_transport_set_status(const char *status);
void usage_transport_get_status(char *out, size_t capacity);
bool usage_transport_publish_news(const news_feed_t *feed, const char *status);
bool usage_transport_get_news(news_feed_t *out, int64_t *received_at_ms);
void usage_wifi_start(void);
