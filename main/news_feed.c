#include "news_feed.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

/* cJSON's general nesting limit is much larger than an embedded task stack.
   Check string-aware nesting before entering its recursive parser. */
static cJSON *parse_json(const char *payload, size_t length) {
    if (!payload || !length) return NULL;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; i++) {
        const char ch = payload[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 16) return NULL; }
        else if (ch == '}' || ch == ']') { if (!depth) return NULL; depth--; }
    }
    if (quoted || depth) return NULL;
    return cJSON_ParseWithLength(payload, length);
}

static const cJSON *field(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static int64_t integer(const cJSON *value) {
    if (cJSON_IsNumber(value)) {
        const double n = value->valuedouble;
        if (!isfinite(n) || n <= 0 || n > 9007199254740991.0 || floor(n) != n) return 0;
        return (int64_t)n;
    }
    if (!cJSON_IsString(value) || !value->valuestring[0]) return 0;
    for (const char *p = value->valuestring; *p; p++) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    char *end = NULL;
    const long long n = strtoll(value->valuestring, &end, 10);
    return errno || !end || *end || n <= 0 ? 0 : n;
}

static bool copy_text(char *out, size_t capacity, const char *text, bool *truncated) {
    if (!text || !out || capacity < 4) return false;
    size_t used = 0;
    bool space = false;
    *truncated = false;
    while (*text) {
        const unsigned char ch = (unsigned char)*text;
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            space = used > 0;
            text++;
            continue;
        }
        size_t bytes = ch < 0x80 ? 1 : (ch & 0xe0) == 0xc0 ? 2 :
                       (ch & 0xf0) == 0xe0 ? 3 : (ch & 0xf8) == 0xf0 ? 4 : 0;
        if (!bytes) { text++; continue; }
        bool valid = true;
        for (size_t i = 1; i < bytes; i++) {
            if (!text[i] || ((unsigned char)text[i] & 0xc0) != 0x80) { valid = false; break; }
        }
        if (!valid) { text++; continue; }
        /* Leave room for a visible ASCII truncation marker and the NUL. */
        if (used + bytes + (space ? 1 : 0) > capacity - 4) { *truncated = true; break; }
        if (space) out[used++] = ' ';
        space = false;
        memcpy(out + used, text, bytes);
        used += bytes;
        text += bytes;
    }
    if (*truncated) { memcpy(out + used, "...", 3); used += 3; }
    out[used] = '\0';
    return used > 0;
}

static bool decode_item(const cJSON *json, news_item_t *out) {
    if (!cJSON_IsObject(json)) return false;
    const int64_t id = integer(field(json, "id"));
    int64_t timestamp = integer(field(json, "display_time"));
    if (timestamp > 1000000000000LL) timestamp /= 1000;
    if (!id || timestamp < 1704067200LL || timestamp > 4102444800LL) return false;
    const cJSON *title = field(json, "title");
    const cJSON *body = field(json, "content_text");
    /* Keep the existing headline-first presentation; text-only bulletins use the full body. */
    const char *text = cJSON_IsString(title) && title->valuestring[0] ? title->valuestring :
                       cJSON_IsString(body) ? body->valuestring : NULL;
    memset(out, 0, sizeof(*out));
    if (!copy_text(out->title, sizeof(out->title), text, &out->truncated)) return false;
    snprintf(out->id, sizeof(out->id), "%lld", (long long)id);
    snprintf(out->source, sizeof(out->source), "华尔街见闻");
    out->published_at = timestamp;
    int minute = (int)((timestamp / 60 + 8 * 60) % 1440);
    snprintf(out->time, sizeof(out->time), "%02d:%02d", minute / 60, minute % 60);
    return true;
}

static bool same_item(const news_item_t *a, const news_item_t *b) {
    return a->published_at == b->published_at && a->truncated == b->truncated &&
           strcmp(a->id, b->id) == 0 && strcmp(a->title, b->title) == 0;
}

static size_t find_item(const news_feed_t *feed, const char *id) {
    for (size_t i = 0; i < feed->count; i++) if (strcmp(feed->items[i].id, id) == 0) return i;
    return SIZE_MAX;
}

static bool newer(const news_item_t *a, const news_item_t *b) {
    return a->published_at > b->published_at ||
           (a->published_at == b->published_at && strtoll(a->id, NULL, 10) > strtoll(b->id, NULL, 10));
}

static void remove_at(news_feed_t *feed, size_t index) {
    if (index >= feed->count) return;
    memmove(&feed->items[index], &feed->items[index + 1],
            (feed->count - index - 1) * sizeof(feed->items[0]));
    feed->count--;
}

static void insert_item(news_feed_t *feed, const news_item_t *item) {
    size_t at = 0;
    while (at < feed->count && !newer(item, &feed->items[at])) at++;
    if (at >= NEWS_FEED_MAX_ITEMS) return;
    if (feed->count < NEWS_FEED_MAX_ITEMS) feed->count++;
    memmove(&feed->items[at + 1], &feed->items[at], (feed->count - at - 1) * sizeof(feed->items[0]));
    feed->items[at] = *item;
}

static int compare_items(const void *a, const void *b) {
    return newer(a, b) ? -1 : newer(b, a) ? 1 : 0;
}

bool news_decode_snapshot(const char *payload, size_t length, news_feed_t *out, int64_t *cursor) {
    if (!payload || !out || !cursor) return false;
    cJSON *root = parse_json(payload, length);
    const cJSON *data = field(root, "data");
    const cJSON *items = field(data, "items");
    *cursor = integer(field(data, "op_cursor"));
    if (integer(field(root, "code")) != 20000 || !*cursor || !cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return false;
    }
    out->count = 0;
    out->updated_at = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (out->count == NEWS_FEED_MAX_ITEMS) break;
        news_item_t *decoded = &out->items[out->count];
        if (decode_item(item, decoded) && find_item(out, decoded->id) == SIZE_MAX) out->count++;
    }
    const bool valid = out->count > 0 || cJSON_GetArraySize(items) == 0;
    cJSON_Delete(root);
    qsort(out->items, out->count, sizeof(out->items[0]), compare_items);
    return valid;
}

bool news_apply_snapshot(news_state_t *state, const news_feed_t *snapshot, int64_t cursor) {
    if (!state || !snapshot || snapshot->count > NEWS_FEED_MAX_ITEMS ||
        cursor <= 0 || cursor < state->cursor) return false;
    bool changed = state->feed.count != snapshot->count;
    for (size_t i = 0; !changed && i < snapshot->count; i++)
        changed = !same_item(&state->feed.items[i], &snapshot->items[i]);
    if (changed) state->feed = *snapshot;
    state->cursor = cursor;
    return changed;
}

news_event_result_t news_apply_event(news_state_t *state, const char *payload, size_t length,
                                   news_item_t *scratch) {
    if (!state || !payload || !scratch) return NEWS_EVENT_INVALID;
    cJSON *root = parse_json(payload, length);
    const cJSON *content = field(root, "content");
    int64_t cursor = integer(field(content, "op_id"));
    if (!cursor) cursor = integer(field(root, "op_id"));
    const cJSON *op = field(content, "op_name");
    if (!cJSON_IsString(op)) op = field(root, "op_name");
    if (!cJSON_IsString(op)) op = field(root, "command");
    const char *action = cJSON_IsString(op) ? op->valuestring : "";
    const bool remove = strcmp(action, "delete") == 0 || strcmp(action, "remove") == 0;
    const bool write = strcmp(action, "create") == 0 || strcmp(action, "update") == 0 ||
                       strcmp(action, "reorder") == 0;
    if (!cJSON_IsObject(content) || !cursor || (!remove && !write)) {
        cJSON_Delete(root);
        return NEWS_EVENT_INVALID;
    }
    if (cursor <= state->cursor) {
        if (cursor > state->last_event_cursor) state->last_event_cursor = cursor;
        cJSON_Delete(root);
        return NEWS_EVENT_UNCHANGED;
    }
    const cJSON *channels = field(content, "channels");
    bool global = false;
    const cJSON *channel = NULL;
    cJSON_ArrayForEach(channel, channels) {
        if (cJSON_IsString(channel) && strcmp(channel->valuestring, "global-channel") == 0) global = true;
    }
    const int64_t item_id = integer(field(content, "id"));
    char id[24];
    snprintf(id, sizeof(id), "%lld", (long long)item_id);
    const size_t existing = find_item(&state->feed, id);
    if (!cJSON_IsArray(channels)) global = existing != SIZE_MAX;
    const bool decoded = write && global && decode_item(content, scratch);
    /* No pointers into the JSON object survive this point. */
    cJSON_Delete(root);
    if (!remove && global && !decoded) return NEWS_EVENT_INVALID;
    if (cursor > state->last_event_cursor) state->last_event_cursor = cursor;
    state->cursor = cursor;
    if ((remove || !global) && existing != SIZE_MAX) {
        remove_at(&state->feed, existing);
        return NEWS_EVENT_CHANGED;
    }
    if (!decoded) return NEWS_EVENT_UNCHANGED;
    if (existing != SIZE_MAX && same_item(&state->feed.items[existing], scratch)) return NEWS_EVENT_UNCHANGED;
    if (existing == SIZE_MAX && state->feed.count == NEWS_FEED_MAX_ITEMS &&
        !newer(scratch, &state->feed.items[state->feed.count - 1])) return NEWS_EVENT_UNCHANGED;
    if (existing != SIZE_MAX) remove_at(&state->feed, existing);
    insert_item(&state->feed, scratch);
    return NEWS_EVENT_CHANGED;
}

int news_ws_append(news_ws_buffer_t *buffer, int opcode, bool fin, int payload_length,
                   int offset, const char *data, int length) {
    if (!buffer || !buffer->data || buffer->capacity < 2) return -1;
    if (opcode >= 8) return 0; /* PING/PONG may occur between continuation frames. */
    if (opcode != 0 && opcode != 1) goto invalid;
    if (payload_length < 0 || offset < 0 || length < 0 || (length && !data)) goto invalid;
    if (offset == 0) {
        if (opcode == 1) { buffer->length = 0; buffer->active = true; }
        else if (!buffer->active) goto invalid;
        buffer->frame_received = 0;
        buffer->frame_length = (size_t)payload_length;
    }
    if (!buffer->active || (size_t)offset != buffer->frame_received ||
        (size_t)payload_length != buffer->frame_length ||
        (size_t)length > buffer->frame_length - buffer->frame_received ||
        (size_t)length >= buffer->capacity - buffer->length) goto invalid;
    if (length) memcpy(buffer->data + buffer->length, data, (size_t)length);
    buffer->length += (size_t)length;
    buffer->frame_received += (size_t)length;
    if (buffer->frame_received != buffer->frame_length || !fin) return 0;
    buffer->data[buffer->length] = '\0';
    buffer->active = false;
    return 1;
invalid:
    buffer->active = false;
    buffer->length = buffer->frame_received = buffer->frame_length = 0;
    return -1;
}
