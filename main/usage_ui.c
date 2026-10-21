#include "usage_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "usage_transport.h"
#include "usage_wifi.h"

extern const lv_font_t news_cjk_16;
extern const lv_font_t news_cjk_22;
extern const lv_font_t lv_font_montserrat_14;

#define BG 0x050505
#define HOT_ROW 0x171307
#define RULE 0x302A1A
#define TEXT 0xF4F1E8
#define MUTED 0x9B978D
#define AMBER 0xFFB000
#define GREEN 0x26B36B
#define TOOLBAR_BG 0x101729

#define NEWS_COUNT NEWS_FEED_MAX_ITEMS
#define BODY_X 18
#define BODY_Y 168
#define BODY_W 564
#define BODY_H 738
#define TITLE_X 18
#define TITLE_Y 31
#define TITLE_W (BODY_W - 36)

static lv_obj_t *headline_labels[NEWS_COUNT];
static lv_obj_t *time_labels[NEWS_COUNT];
static lv_obj_t *rows[NEWS_COUNT];
static lv_obj_t *rails[NEWS_COUNT];
static lv_obj_t *rules[NEWS_COUNT];
static lv_obj_t *status_label;
static lv_obj_t *last_update_label;
static lv_obj_t *live_badge;
static lv_obj_t *live_badge_label;
static lv_obj_t *clock_value_label;
static lv_obj_t *wifi_dot;
static lv_obj_t *news_body;
static const lv_font_t *font_small = &news_cjk_16;
static const lv_font_t *font_headline = &news_cjk_22;
static NEWS_FEED_STORAGE_ATTR news_feed_t previous_feed;
static bool have_previous_feed;

static bool clock_is_synchronized(void) {
    return usage_wifi_time_is_synchronized();
}

static bool format_china_time(time_t epoch, char out[9]) {
    if (epoch < 1704067200) return false;
    /* Render China Standard Time without changing the process-wide TZ used by
       the feed parser. */
    const time_t china_time = epoch + 8 * 60 * 60;
    struct tm tm_utc = {0};
    if (!gmtime_r(&china_time, &tm_utc)) return false;
    snprintf(out, 9, "%02d:%02d:%02d", tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return true;
}

static void update_clock(void) {
    if (!clock_value_label) return;
    char text[9] = "--:--:--";
    if (clock_is_synchronized()) format_china_time(time(NULL), text);
    lv_label_set_text(clock_value_label, text);
}

static void update_last_update(int64_t updated_at) {
    char update_time[9] = "--:--:--";
    if (updated_at > 0 && clock_is_synchronized()) format_china_time((time_t)updated_at, update_time);
    char footer[32];
    snprintf(footer, sizeof(footer), "LAST UPDATE %s", update_time);
    lv_label_set_text(last_update_label, footer);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                       const lv_font_t *font, uint32_t color) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *rule(lv_obj_t *parent, int x, int y, int width, uint32_t color) {
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, width, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

static void make_row(int index) {
    lv_obj_t *row = lv_obj_create(news_body);
    rows[index] = row;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(row, 0, 0);
    lv_obj_set_size(row, BODY_W, 82);
    lv_obj_set_style_bg_color(row, lv_color_hex(index == 0 ? HOT_ROW : BG), 0);
    lv_obj_set_style_bg_opa(row, index == 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    rails[index] = lv_obj_create(row);
    lv_obj_set_pos(rails[index], 0, 0);
    lv_obj_set_size(rails[index], index == 0 ? 4 : 2, 81);
    lv_obj_set_style_bg_color(rails[index], lv_color_hex(index == 0 ? AMBER : RULE), 0);
    lv_obj_set_style_bg_opa(rails[index], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rails[index], 0, 0);
    lv_obj_set_style_radius(rails[index], 0, 0);
    lv_obj_clear_flag(rails[index], LV_OBJ_FLAG_SCROLLABLE);

    time_labels[index] = label(row, "--:--", TITLE_X, 5, 110, 24, &lv_font_montserrat_16, AMBER);
    if (index == 0) {
        lv_obj_t *latest_label = label(row, "最新", 132, 5, 62, 24, font_small, AMBER);
        lv_obj_clear_flag(latest_label, LV_OBJ_FLAG_SCROLLABLE);
    }
    headline_labels[index] = label(row, "等待实时快讯", TITLE_X, TITLE_Y, TITLE_W, 42,
                                   font_headline, TEXT);
    /* Labels are scrollable by default in LVGL. They have no overflow here,
       so let vertical drags pass through them to the news list container. */
    lv_obj_clear_flag(time_labels[index], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(headline_labels[index], LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_long_mode(headline_labels[index], LV_LABEL_LONG_WRAP);
    rules[index] = rule(row, 0, 89, BODY_W, RULE);
}

static void update_connection_indicator(void) {
    const bool connected = usage_wifi_is_connected();
    const bool fresh = usage_wifi_source_is_fresh();
    const uint32_t color = !connected ? MUTED : fresh ? GREEN : AMBER;
    lv_label_set_text(status_label, !connected ? "WIFI CONNECTING" : fresh ? "WIFI CONNECTED" : "SOURCE RETRYING");
    lv_obj_set_style_text_color(status_label, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(wifi_dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(live_badge, lv_color_hex(AMBER), 0);
    lv_obj_set_style_text_color(live_badge_label, lv_color_hex(BG), 0);
}

static bool feed_changed(const news_feed_t *feed) {
    size_t count = feed->count < NEWS_COUNT ? feed->count : NEWS_COUNT;
    if (!have_previous_feed || previous_feed.count != count) return true;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(previous_feed.items[i].title, feed->items[i].title) != 0 ||
            strcmp(previous_feed.items[i].id, feed->items[i].id) != 0 ||
            strcmp(previous_feed.items[i].time, feed->items[i].time) != 0) return true;
    }
    return false;
}

static void layout_feed(const news_feed_t *feed) {
    const int32_t old_scroll_y = lv_obj_get_scroll_y(news_body);
    char anchor_id[24] = {0};
    int32_t anchor_offset = 0;
    if (have_previous_feed && old_scroll_y > 8) {
        for (size_t i = 0; i < previous_feed.count; i++) {
            const int32_t y = lv_obj_get_y(rows[i]);
            if (y + lv_obj_get_height(rows[i]) > old_scroll_y) {
                snprintf(anchor_id, sizeof(anchor_id), "%s", previous_feed.items[i].id);
                anchor_offset = old_scroll_y - y;
                break;
            }
        }
    }
    int32_t row_y = 0;
    const size_t count = feed->count < NEWS_COUNT ? feed->count : NEWS_COUNT;

    for (size_t i = 0; i < NEWS_COUNT; i++) {
        if (i >= count) {
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(time_labels[i], feed->items[i].time);
        lv_label_set_text(headline_labels[i], feed->items[i].title);
        lv_obj_set_style_text_color(headline_labels[i], lv_color_hex(i == 0 ? 0xFFD36A : TEXT), 0);

        lv_point_t measured = {0};
        lv_text_get_size(&measured, feed->items[i].title, font_headline,
                         lv_obj_get_style_text_letter_space(headline_labels[i], LV_PART_MAIN),
                         lv_obj_get_style_text_line_space(headline_labels[i], LV_PART_MAIN),
                         TITLE_W, LV_TEXT_FLAG_NONE);
        int32_t title_height = measured.y + 2;
        if (title_height < font_headline->line_height) title_height = font_headline->line_height;
        const int32_t row_height = TITLE_Y + title_height + 9; /* trim bottom padding; keep headline font size */

        lv_obj_set_pos(rows[i], 0, row_y);
        lv_obj_set_size(rows[i], BODY_W, row_height);
        lv_obj_set_pos(headline_labels[i], TITLE_X, TITLE_Y);
        lv_obj_set_size(headline_labels[i], TITLE_W, title_height);
        lv_obj_set_size(rails[i], i == 0 ? 4 : 2, row_height - 1);
        lv_obj_set_pos(rules[i], 0, row_height - 1);
        row_y += row_height + 1; /* compact divider gap */
    }

    lv_obj_update_layout(news_body);
    if (old_scroll_y <= 8) lv_obj_scroll_to_y(news_body, 0, LV_ANIM_OFF);
    else {
        int32_t target = old_scroll_y;
        for (size_t i = 0; anchor_id[0] && i < count; i++) {
            if (strcmp(feed->items[i].id, anchor_id) == 0) {
                if (anchor_offset >= lv_obj_get_height(rows[i])) anchor_offset = lv_obj_get_height(rows[i]) - 1;
                target = lv_obj_get_y(rows[i]) + anchor_offset;
                break;
            }
        }
        lv_obj_scroll_to_y(news_body, target, LV_ANIM_OFF);
    }
    previous_feed = *feed;
    previous_feed.count = count;
    have_previous_feed = true;
}

void usage_ui_init(void) {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *masthead = lv_obj_create(screen);
    lv_obj_set_pos(masthead, 0, 0);
    lv_obj_set_size(masthead, 600, 78);
    lv_obj_set_style_bg_color(masthead, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(masthead, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(masthead, 0, 0);
    lv_obj_set_style_pad_all(masthead, 0, 0);
    lv_obj_clear_flag(masthead, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *masthead_rail = lv_obj_create(masthead);
    lv_obj_set_pos(masthead_rail, 0, 0);
    lv_obj_set_size(masthead_rail, 5, 78);
    lv_obj_set_style_bg_color(masthead_rail, lv_color_hex(AMBER), 0);
    lv_obj_set_style_bg_opa(masthead_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(masthead_rail, 0, 0);
    lv_obj_set_style_radius(masthead_rail, 0, 0);
    lv_obj_clear_flag(masthead_rail, LV_OBJ_FLAG_SCROLLABLE);

    const int brand_height = lv_font_montserrat_32.line_height;
    /* LVGL's built-in Montserrat face is medium weight; a one-pixel second
       pass gives the wordmark the heavier terminal-logo treatment. */
    lv_obj_t *brand = label(masthead, "Bloomberg", 0, 0, 250, brand_height,
                            &lv_font_montserrat_32, TEXT);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_t *brand_bold_pass = label(masthead, "Bloomberg", 0, 0, 250, brand_height,
                                      &lv_font_montserrat_32, TEXT);
    lv_obj_align(brand_bold_pass, LV_ALIGN_LEFT_MID, 25, 0);

    live_badge = lv_obj_create(masthead);
    lv_obj_set_pos(live_badge, 448, 24);
    lv_obj_set_size(live_badge, 128, 40);
    lv_obj_set_style_bg_color(live_badge, lv_color_hex(AMBER), 0);
    lv_obj_set_style_bg_opa(live_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(live_badge, 0, 0);
    lv_obj_set_style_pad_all(live_badge, 0, 0);
    lv_obj_set_style_radius(live_badge, 2, 0);
    /* Center the text's line box vertically too. A 40px label starts glyph
       rendering at its top edge in LVGL, even when the label object is centered. */
    lv_obj_t *live_dot = lv_obj_create(live_badge);
    lv_obj_set_pos(live_dot, 22, 15);
    lv_obj_set_size(live_dot, 10, 10);
    lv_obj_set_style_bg_color(live_dot, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(live_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(live_dot, 0, 0);
    lv_obj_set_style_radius(live_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(live_dot, LV_OBJ_FLAG_SCROLLABLE);
    const int badge_text_height = lv_font_montserrat_20.line_height;
    live_badge_label = label(live_badge, "LIVE", 42, 0, 70, badge_text_height,
                             &lv_font_montserrat_20, BG);
    lv_obj_set_style_text_align(live_badge_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(live_badge_label, LV_ALIGN_CENTER, 12, 0);

    lv_obj_t *terminal_strip = lv_obj_create(screen);
    lv_obj_set_pos(terminal_strip, 0, 79);
    lv_obj_set_size(terminal_strip, 600, 51);
    lv_obj_set_style_bg_color(terminal_strip, lv_color_hex(TOOLBAR_BG), 0);
    lv_obj_set_style_bg_opa(terminal_strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(terminal_strip, 0, 0);
    lv_obj_set_style_pad_all(terminal_strip, 0, 0);
    lv_obj_set_style_radius(terminal_strip, 0, 0);
    lv_obj_clear_flag(terminal_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *news_button = lv_obj_create(terminal_strip);
    lv_obj_set_pos(news_button, 23, 8);
    lv_obj_set_size(news_button, 92, 34);
    lv_obj_set_style_bg_color(news_button, lv_color_hex(GREEN), 0);
    lv_obj_set_style_bg_opa(news_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(news_button, 0, 0);
    lv_obj_set_style_radius(news_button, 3, 0);
    lv_obj_set_style_pad_all(news_button, 0, 0);
    lv_obj_clear_flag(news_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(news_button, 1, 0);
    lv_obj_set_style_border_color(news_button, lv_color_hex(0x54CB80), 0);
    lv_obj_set_style_bg_grad_color(news_button, lv_color_hex(0x168A48), 0);
    lv_obj_set_style_bg_grad_dir(news_button, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(news_button, 5, 0);
    lv_obj_set_style_shadow_color(news_button, lv_color_hex(0x1C8F50), 0);
    lv_obj_set_style_shadow_opa(news_button, LV_OPA_40, 0);
    lv_obj_t *news_button_highlight = lv_obj_create(news_button);
    lv_obj_set_pos(news_button_highlight, 1, 1);
    lv_obj_set_size(news_button_highlight, 88, 1);
    lv_obj_set_style_bg_color(news_button_highlight, lv_color_hex(0xA2F5BC), 0);
    lv_obj_set_style_bg_opa(news_button_highlight, LV_OPA_60, 0);
    lv_obj_set_style_border_width(news_button_highlight, 0, 0);
    lv_obj_clear_flag(news_button_highlight, LV_OBJ_FLAG_SCROLLABLE);
    const int news_text_height = lv_font_montserrat_16.line_height;
    lv_obj_t *news_button_label = label(news_button, "NEWS", 0, 0, 92, news_text_height,
                                        &lv_font_montserrat_16, BG);
    lv_obj_set_style_text_align(news_button_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(news_button_label, LV_ALIGN_CENTER, 0, 0);
    label(terminal_strip, "LOCAL", 385, 13, 55, 24, &lv_font_montserrat_14, TEXT);
    clock_value_label = label(terminal_strip, "--:--:--", 440, 13, 136, 24,
                              &lv_font_montserrat_14, AMBER);
    lv_obj_set_style_text_align(clock_value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(clock_value_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *selected_mark = lv_obj_create(terminal_strip);
    lv_obj_set_pos(selected_mark, 23, 49);
    lv_obj_set_size(selected_mark, 92, 2);
    lv_obj_set_style_bg_color(selected_mark, lv_color_hex(AMBER), 0);
    lv_obj_set_style_bg_opa(selected_mark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(selected_mark, 0, 0);
    lv_obj_clear_flag(selected_mark, LV_OBJ_FLAG_SCROLLABLE);

    rule(screen, 20, 130, 560, RULE);
    label(screen, "TOP STORIES", 23, 138, 220, 24,
          &lv_font_montserrat_14, 0xAFC0D6);

    news_body = lv_obj_create(screen);
    lv_obj_set_pos(news_body, BODY_X, BODY_Y);
    lv_obj_set_size(news_body, BODY_W, BODY_H);
    lv_obj_set_style_bg_color(news_body, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(news_body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(news_body, 0, 0);
    lv_obj_set_style_radius(news_body, 0, 0);
    lv_obj_set_style_pad_all(news_body, 0, 0);
    lv_obj_add_flag(news_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(news_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(news_body, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_width(news_body, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(news_body, lv_color_hex(AMBER), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(news_body, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(news_body, 2, LV_PART_SCROLLBAR);
    for (int i = 0; i < NEWS_COUNT; i++) make_row(i);

    lv_obj_t *footer = lv_obj_create(screen);
    lv_obj_set_pos(footer, 0, 918);
    lv_obj_set_size(footer, 600, 106);
    lv_obj_set_style_bg_color(footer, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    rule(footer, 20, 0, 560, RULE);
    wifi_dot = lv_obj_create(footer);
    lv_obj_set_pos(wifi_dot, 23, 44);
    lv_obj_set_size(wifi_dot, 10, 10);
    lv_obj_set_style_bg_color(wifi_dot, lv_color_hex(GREEN), 0);
    lv_obj_set_style_bg_opa(wifi_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_dot, 0, 0);
    lv_obj_set_style_radius(wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(wifi_dot, LV_OBJ_FLAG_SCROLLABLE);
    status_label = label(footer, "WIFI CONNECTING", 42, 35, 240, 28,
                         &lv_font_montserrat_16, MUTED);
    last_update_label = label(footer, "LAST UPDATE --:--:--", 300, 35, 276, 28,
                              &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_align(last_update_label, LV_TEXT_ALIGN_RIGHT, 0);
}

void usage_ui_apply_news(const news_feed_t *feed, int64_t age_seconds) {
    (void)age_seconds;
    if (!feed) return;
    update_clock();
    update_last_update(feed->updated_at);
    /* Finish the current gesture before changing row geometry underneath the finger. */
    if (!lv_obj_is_scrolling(news_body) && feed_changed(feed)) layout_feed(feed);
    update_connection_indicator();
}

void usage_ui_set_offline(int64_t age_seconds) {
    (void)age_seconds;
    update_clock();
    update_connection_indicator();
}
