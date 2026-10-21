#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NEWS_FEED_MAX_ITEMS 24
#define NEWS_TITLE_MAX 16384

typedef struct {
    char title[NEWS_TITLE_MAX];
    char source[48];
    char time[12];
    char id[24];
    int64_t published_at;
    bool truncated;
} news_item_t;

typedef struct {
    news_item_t items[NEWS_FEED_MAX_ITEMS];
    size_t count;
    int64_t updated_at;
} news_feed_t;

typedef struct {
    news_feed_t feed;
    int64_t cursor;
    int64_t last_event_cursor;
} news_state_t;

typedef enum {
    NEWS_EVENT_INVALID = -1,
    NEWS_EVENT_UNCHANGED = 0,
    NEWS_EVENT_CHANGED = 1,
} news_event_result_t;

bool news_decode_snapshot(const char *payload, size_t length, news_feed_t *out, int64_t *cursor);
/* A complete, equally/newer snapshot is authoritative, including deletions and order. */
bool news_apply_snapshot(news_state_t *state, const news_feed_t *snapshot, int64_t cursor);
/* Caller owns scratch storage and serializes access to state. */
news_event_result_t news_apply_event(news_state_t *state, const char *payload, size_t length,
                                   news_item_t *scratch);

typedef struct {
    char *data;
    size_t capacity, length, frame_received, frame_length;
    bool active;
} news_ws_buffer_t;

/* 1: complete text message, 0: incomplete/control frame, -1: rejected. */
int news_ws_append(news_ws_buffer_t *buffer, int opcode, bool fin, int payload_length,
                   int offset, const char *data, int length);
