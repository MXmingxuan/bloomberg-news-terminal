#pragma once

#include <stdint.h>
#include "usage_transport.h"

void usage_ui_init(void);
void usage_ui_apply_news(const news_feed_t *feed, int64_t age_seconds);
void usage_ui_set_offline(int64_t age_seconds);
