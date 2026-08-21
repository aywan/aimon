#include "render.h"

#include <inttypes.h>
#include <stdlib.h>

#include "build_info.h"
#include "driver/temperature_sensor.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "events.h"
#include "mic.h"
#include "rtc.h"
#include "sd.h"
#include "state.h"

#include "libs/lodepng/lodepng.h"

// state_set_*() (and therefore on_state_changed(), and therefore the LVGL
// lock) can be called from other tasks — e.g. battery's own timer callback.
// A bounded timeout keeps a stuck LVGL task from also wedging whatever timer
// task called in, instead of the block-forever convention render_init() uses
// for itself (0 == forever, per esp_lvgl_port's lvgl_port_lock()).
#define EXTERNAL_LOCK_TIMEOUT_MS 100

// Linear 3.3–4.2 V percent on this pack already means "plug in" at 30% —
// phone-style 20% would be too late. See docs/KNOWLEDGE_BASE.md (render).
#define BAT_ICON_WARN_PCT 30

static const char *TAG = "render";

static lv_obj_t *s_screens[APP_SCREEN_COUNT];
static app_screen_t s_shown_screen = APP_SCREEN_MAIN;

static lv_obj_t *s_charge_value_label;
static lv_obj_t *s_voltage_value_label;
static lv_obj_t *s_uptime_value_label;
static lv_obj_t *s_conn_value_label;
static lv_obj_t *s_heap_value_label;
static lv_obj_t *s_temp_value_label;
static lv_obj_t *s_noise_chart;
static lv_obj_t *s_noise_value_label;
static lv_chart_series_t *s_noise_series;
static lv_timer_t *s_status_tick_timer;
static lv_timer_t *s_noise_tick_timer;
static temperature_sensor_handle_t s_tsens;

static lv_obj_t *s_pair_btn;
static lv_obj_t *s_pair_btn_label;
static lv_obj_t *s_passkey_label;

typedef struct {
    lv_obj_t *box;
    lv_obj_t *gauge;
    lv_obj_t *value_lbl;
    lv_obj_t *text_lbl;
    lv_obj_t *caption;
    app_slot_type_t shown_type;
} slot_ui_t;

static slot_ui_t s_slot_ui[APP_SLOT_COUNT];
static lv_obj_t *s_clock_row;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_battery_icon;
static lv_obj_t *s_stale_led;
static lv_timer_t *s_clock_tick_timer;

// Decoded SD wallpapers. Pixel buffers stay for the process lifetime
// (full_refresh redraws them every frame); compressed PNG and the SD
// stack are freed before create_screens(). See docs/KNOWLEDGE_BASE.md.
static lv_draw_buf_t *s_bg_buf;
static lv_draw_buf_t *s_fg_buf;

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    bool pressed = false;
    esp_err_t err = lcd_touch_read(&raw_x, &raw_y, &pressed);
    if (err != ESP_OK) {
        static int64_t s_last_warn_us;
        int64_t now = esp_timer_get_time();
        if (now - s_last_warn_us > 5 * 1000 * 1000) {
            ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(err));
            s_last_warn_us = now;
        }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (!pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Native (unrotated) panel space: lv_display_set_rotation(270) already
    // remaps pointer coords in indev_pointer_proc(). Feeding pre-rotated
    // logical points double-transforms them — swipes still work (they only
    // need a direction) but widgets in the center of the 640x172 frame
    // never see the hit. raw_y is the 172-wide axis, raw_x the 640-tall one.
    data->point.x = raw_y;
    data->point.y = LCD_V_RES - 1 - raw_x;
    data->state = LV_INDEV_STATE_PRESSED;
}

static void gesture_event_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    // Physical swipe along the short edge is UP/DOWN in the 270°-rotated
    // logical frame (LEFT/RIGHT was an artifact of double-rotating the
    // pointer — see touch_read_cb).
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP) {
        events_post(APP_EVENT_SCREEN_NEXT);
    } else if (dir == LV_DIR_BOTTOM) {
        events_post(APP_EVENT_SCREEN_PREV);
    }
}

static lv_obj_t *create_base_screen(lv_color_t bg_color)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, bg_color, 0);
    // Screens here are swiped between, never scrolled internally. A
    // scrollable screen (the lv_obj_create() default) steals horizontal drags
    // for its own scroll instead of emitting LV_EVENT_GESTURE, which silently
    // breaks the swipe-to-switch-screen gesture below.
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_GESTURE, NULL);
    return scr;
}

static void pair_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ESP_LOGI(TAG, "settings button clicked, paired=%d", (int)state_get_ble_paired());
    if (state_get_ble_paired()) {
        events_post(APP_EVENT_BLE_FORGET);
    } else {
        events_post(APP_EVENT_BLE_PAIR);
    }
}

static void update_settings_widgets(void)
{
    if (!s_pair_btn || !s_passkey_label) {
        return;
    }

    if (state_get_ble_passkey_visible()) {
        lv_label_set_text_fmt(s_passkey_label, "PIN: %06" PRIu32, state_get_ble_passkey());
        lv_obj_remove_flag(s_passkey_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pair_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(s_passkey_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pair_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_pair_btn_label, state_get_ble_paired() ? "Forget" : "Pair");
}

static lv_obj_t *create_settings_screen(void)
{
    lv_obj_t *scr = create_base_screen(lv_color_hex(0x202020));

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    s_pair_btn = lv_button_create(scr);
    lv_obj_set_size(s_pair_btn, 200, 56);
    lv_obj_center(s_pair_btn);
    // Keep a short tap from bubbling as a screen-change gesture.
    lv_obj_remove_flag(s_pair_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_pair_btn, pair_btn_cb, LV_EVENT_CLICKED, NULL);

    s_pair_btn_label = lv_label_create(s_pair_btn);
    lv_label_set_text(s_pair_btn_label, "Pair");
    lv_obj_set_style_text_color(s_pair_btn_label, lv_color_white(), 0);
    lv_obj_center(s_pair_btn_label);

    s_passkey_label = lv_label_create(scr);
    lv_label_set_text(s_passkey_label, "PIN: ------");
    lv_obj_set_style_text_color(s_passkey_label, lv_color_white(), 0);
    lv_obj_center(s_passkey_label);
    lv_obj_add_flag(s_passkey_label, LV_OBJ_FLAG_HIDDEN);

    update_settings_widgets();
    return scr;
}

#define SLOT_GAUGE_FG_DEFAULT 0x3ecf8e
#define SLOT_GAUGE_BG_DEFAULT 0x333333
#define SLOT_TEXT_FG_DEFAULT  0xffffff

static void slot_ui_clear_children(slot_ui_t *ui)
{
    ui->gauge = NULL;
    ui->value_lbl = NULL;
    ui->text_lbl = NULL;
    ui->caption = NULL;
}

static lv_obj_t *slot_caption_create(lv_obj_t *parent)
{
    lv_obj_t *caption = lv_label_create(parent);
    lv_obj_set_style_text_color(caption, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
    lv_obj_set_width(caption, LV_PCT(100));
    return caption;
}

static void slot_build_gauge(slot_ui_t *ui)
{
    ui->gauge = lv_arc_create(ui->box);
    lv_obj_set_size(ui->gauge, 108, 108);
    lv_arc_set_rotation(ui->gauge, 135);
    lv_arc_set_bg_angles(ui->gauge, 0, 270);
    lv_arc_set_range(ui->gauge, 0, 100);
    lv_obj_remove_style(ui->gauge, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ui->gauge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ui->gauge, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui->gauge, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui->gauge, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui->gauge, lv_color_hex(0x3ecf8e), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui->gauge, LV_OPA_TRANSP, 0);

    ui->value_lbl = lv_label_create(ui->gauge);
    lv_obj_set_style_text_color(ui->value_lbl, lv_color_white(), 0);
    lv_obj_center(ui->value_lbl);

    ui->caption = slot_caption_create(ui->box);
    // 270° horseshoe leaves empty space at the bottom of the arc's
    // bounding box; pull the caption up into that gap.
    lv_obj_set_style_margin_top(ui->caption, -16, 0);
}

static void slot_build_text(slot_ui_t *ui)
{
    ui->text_lbl = lv_label_create(ui->box);
    lv_obj_set_style_text_color(ui->text_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(ui->text_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui->text_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->text_lbl, LV_PCT(100));

    ui->caption = slot_caption_create(ui->box);
}

static void slot_apply_content(slot_ui_t *ui, const app_slot_t *slot)
{
    switch (slot->type) {
    case APP_SLOT_GAUGE:
        if (ui->gauge) {
            lv_arc_set_value(ui->gauge, slot->value);
            lv_obj_set_style_arc_color(ui->gauge,
                                       lv_color_hex(slot->bg_set ? slot->bg : SLOT_GAUGE_BG_DEFAULT),
                                       LV_PART_MAIN);
            lv_obj_set_style_arc_color(ui->gauge,
                                       lv_color_hex(slot->fg_set ? slot->fg : SLOT_GAUGE_FG_DEFAULT),
                                       LV_PART_INDICATOR);
        }
        if (ui->value_lbl) {
            lv_label_set_text_fmt(ui->value_lbl, "%u%%", (unsigned)slot->value);
            lv_obj_center(ui->value_lbl);
        }
        if (ui->caption) {
            lv_label_set_text(ui->caption, slot->label);
        }
        break;
    case APP_SLOT_TEXT:
        if (ui->text_lbl) {
            const char *body = slot->text[0] ? slot->text : slot->label;
            lv_label_set_text(ui->text_lbl, body);
            lv_obj_set_style_text_color(ui->text_lbl,
                                        lv_color_hex(slot->fg_set ? slot->fg : SLOT_TEXT_FG_DEFAULT),
                                        0);
        }
        if (ui->caption) {
            // If body fell back to label, don't repeat it underneath.
            if (slot->text[0] && slot->label[0]) {
                lv_label_set_text(ui->caption, slot->label);
                lv_obj_remove_flag(ui->caption, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_label_set_text(ui->caption, "");
                lv_obj_add_flag(ui->caption, LV_OBJ_FLAG_HIDDEN);
            }
        }
        break;
    default:
        break;
    }
}

static void update_slot_ui(slot_ui_t *ui, const app_slot_t *slot)
{
    if (ui->shown_type != slot->type) {
        lv_obj_clean(ui->box);
        slot_ui_clear_children(ui);
        ui->shown_type = slot->type;
        if (slot->type == APP_SLOT_GAUGE) {
            slot_build_gauge(ui);
        } else if (slot->type == APP_SLOT_TEXT) {
            slot_build_text(ui);
        }
    }
    slot_apply_content(ui, slot);
}

static void update_stale_led(void)
{
    if (!s_stale_led) {
        return;
    }
    if (state_get_ble_data_stale()) {
        lv_obj_remove_flag(s_stale_led, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_stale_led, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_battery_icon(void)
{
    if (!s_battery_icon) {
        return;
    }
    if (!state_get_battery_present()) {
        lv_obj_add_flag(s_battery_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_battery_icon, LV_OBJ_FLAG_HIDDEN);

    uint8_t pct = state_get_battery_percent();
    const char *sym;
    if (pct >= 75) {
        sym = LV_SYMBOL_BATTERY_FULL;
    } else if (pct >= 50) {
        sym = LV_SYMBOL_BATTERY_3;
    } else if (pct > BAT_ICON_WARN_PCT) {
        sym = LV_SYMBOL_BATTERY_2;
    } else if (pct > 10) {
        sym = LV_SYMBOL_BATTERY_1;
    } else {
        sym = LV_SYMBOL_BATTERY_EMPTY;
    }
    lv_label_set_text(s_battery_icon, sym);
    lv_obj_set_style_text_color(s_battery_icon,
                                pct <= BAT_ICON_WARN_PCT ? lv_color_hex(0xff4a1a)
                                                         : lv_color_hex(0xcccccc),
                                0);
}

static void update_main_widgets(void)
{
    if (!s_slot_ui[0].box) {
        return;
    }
    for (uint8_t i = 0; i < APP_SLOT_COUNT; i++) {
        app_slot_t slot;
        state_get_slot(i, &slot);
        update_slot_ui(&s_slot_ui[i], &slot);
    }
    update_stale_led();
    update_battery_icon();
}

static void update_clock_label(void)
{
    if (!s_clock_label) {
        return;
    }
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (!rtc_clock_get_hms(&hour, &minute, NULL)) {
        lv_label_set_text(s_clock_label, "--:--");
        return;
    }
    lv_label_set_text_fmt(s_clock_label, "%02u:%02u", (unsigned)hour, (unsigned)minute);
}

// Wallpaper pixels are filled on the main task (CPU0) into PSRAM, then
// sampled by the LVGL task on CPU1. Default draw_buf handlers have no
// flush_cache_cb, so without an explicit C2M writeback the other core
// reads stale DCache lines — visual garbage, not a "bad PNG".
static void wallpaper_flush_cache(lv_draw_buf_t *buf)
{
    if (buf == NULL || buf->unaligned_data == NULL || buf->data_size == 0) {
        return;
    }
    const uint8_t *base = (const uint8_t *)buf->unaligned_data;
    const uint8_t *data = (const uint8_t *)buf->data;
    size_t bytes = buf->data_size;
    if (data >= base) {
        bytes += (size_t)(data - base);
    }
    esp_err_t err = esp_cache_msync(buf->unaligned_data, bytes,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wallpaper cache writeback failed: %s", esp_err_to_name(err));
    }
}

// ARGB8888 → native 16-bit (RGB565, or RGB565A8 if any pixel has alpha).
// Display color depth is 16; keeping ARGB8888 plus STRETCH/sw_rotate is a
// heavier blend path and was showing scrambled pixels on this board.
static lv_draw_buf_t *to_native_draw_buf(lv_draw_buf_t *buf)
{
    if (buf == NULL || buf->header.cf != LV_COLOR_FORMAT_ARGB8888) {
        return buf;
    }

    const uint32_t w = buf->header.w;
    const uint32_t h = buf->header.h;
    const uint32_t src_stride = buf->header.stride;
    bool opaque = true;
    for (uint32_t y = 0; y < h && opaque; y++) {
        const lv_color32_t *row = (const lv_color32_t *)(buf->data + y * src_stride);
        for (uint32_t x = 0; x < w; x++) {
            if (row[x].alpha != LV_OPA_COVER) {
                opaque = false;
                break;
            }
        }
    }

    lv_color_format_t cf = opaque ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
    lv_draw_buf_t *out = lv_draw_buf_create(w, h, cf, LV_STRIDE_AUTO);
    if (out == NULL) {
        return buf;
    }

    const uint32_t dst_stride = out->header.stride;
    uint8_t *alpha_plane = opaque ? NULL : (out->data + dst_stride * h);
    for (uint32_t y = 0; y < h; y++) {
        const lv_color32_t *src_row = (const lv_color32_t *)(buf->data + y * src_stride);
        uint16_t *dst_row = (uint16_t *)(out->data + y * dst_stride);
        uint8_t *a_row = opaque ? NULL : (alpha_plane + y * (dst_stride / 2U));
        for (uint32_t x = 0; x < w; x++) {
            dst_row[x] = lv_color_to_u16(lv_color_make(src_row[x].red, src_row[x].green,
                                                       src_row[x].blue));
            if (a_row != NULL) {
                a_row[x] = src_row[x].alpha;
            }
        }
    }
    lv_draw_buf_destroy(buf);
    return out;
}

// Decode PNG via LVGL's patched lodepng: *out is an lv_draw_buf_t (ARGB8888
// header, RGBA bytes in .data), not a raw RGBA malloc. Treating it as bytes
// copies the struct header as pixels → noise. R/B swap matches
// lv_lodepng.c convert_color_depth(). Pixel buffers stay for every
// full_refresh frame; the compressed PNG is freed by the caller.
static lv_draw_buf_t *decode_png_blob(const uint8_t *data, size_t size, const char *tag)
{
    if (data == NULL || size < 8) {
        return NULL;
    }

    lv_draw_buf_t *buf = NULL;
    unsigned w = 0;
    unsigned h = 0;
    unsigned err = lodepng_decode32((unsigned char **)&buf, &w, &h, data, size);
    if (err != 0 || buf == NULL) {
        ESP_LOGW(TAG, "%s: PNG decode failed (%u: %s)", tag, err,
                 lodepng_error_text(err));
        if (buf != NULL) {
            lv_draw_buf_destroy(buf);
        }
        return NULL;
    }

    const uint32_t pixels = (uint32_t)w * (uint32_t)h;
    if (pixels == 0 || pixels > (uint32_t)LCD_H_RES * (uint32_t)LCD_V_RES * 2U) {
        ESP_LOGW(TAG, "%s: %ux%u too large, dropped", tag, w, h);
        lv_draw_buf_destroy(buf);
        return NULL;
    }

    // Packed 4*w in LVGL's patched decoder, but walk by stride in case
    // the draw_buf is aligned.
    for (unsigned y = 0; y < h; y++) {
        lv_color32_t *row = (lv_color32_t *)(buf->data + (uint32_t)y * buf->header.stride);
        for (unsigned x = 0; x < w; x++) {
            uint8_t blue = row[x].blue;
            row[x].blue = row[x].red;
            row[x].red = blue;
        }
    }

    buf = to_native_draw_buf(buf);
    wallpaper_flush_cache(buf);
    ESP_LOGI(TAG, "%s: %dx%d cf=%d %u bytes", tag, (int)buf->header.w, (int)buf->header.h,
             (int)buf->header.cf, (unsigned)buf->data_size);
    return buf;
}

static lv_obj_t *create_layer_image(lv_obj_t *parent, const lv_draw_buf_t *src)
{
    lv_obj_t *img = lv_image_create(parent);
    // Pass the real lv_draw_buf_t, not a sliced lv_image_dsc_t: draw_buf_create
    // sets ALLOCATED, and lv_image_set_src then requires unaligned_data+handlers
    // or it logs "Invalid draw buffer" and keeps the widget empty.
    lv_image_set_src(img, src);
    // Logical 640×172 after rotation 270 — not LV_PCT on a FLOATING child,
    // which can resolve to 0 before the flex layout runs. Identity-size
    // images skip STRETCH so the SW scaler/transform path is not used.
    lv_obj_set_size(img, LCD_V_RES, LCD_H_RES);
    if (src->header.w == LCD_V_RES && src->header.h == LCD_H_RES) {
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_DEFAULT);
    } else {
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    }
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);
    // Same overlay contract as the clock: a flex/clickable child would
    // steal the swipe-to-switch-screen gesture.
    lv_obj_add_flag(img, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(img, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(img, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    return img;
}

static lv_obj_t *create_main_screen(void)
{
    lv_obj_t *scr = create_base_screen(lv_color_black());
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 6, 0);
    lv_obj_set_style_pad_column(scr, 6, 0);

    // Behind the four-slot row. Created first so later children (slots,
    // then fg, then clock) paint over it. Missing file → black screen bg.
    if (s_bg_buf) {
        create_layer_image(scr, s_bg_buf);
    }

    for (uint8_t i = 0; i < APP_SLOT_COUNT; i++) {
        slot_ui_t *ui = &s_slot_ui[i];
        ui->box = lv_obj_create(scr);
        lv_obj_remove_style_all(ui->box);
        lv_obj_set_width(ui->box, 0);
        lv_obj_set_flex_grow(ui->box, 1);
        lv_obj_set_height(ui->box, LV_PCT(100));
        lv_obj_set_flex_flow(ui->box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(ui->box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(ui->box, 0, 0);
        // Clickable children eat the pointer so the screen never sees
        // LV_EVENT_GESTURE — swipe to Status/Settings would die.
        lv_obj_remove_flag(ui->box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(ui->box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(ui->box, LV_OBJ_FLAG_GESTURE_BUBBLE);
        ui->shown_type = APP_SLOT_EMPTY;
        slot_ui_clear_children(ui);
    }

    // Over the slots, under the clock/stale HUD so HH:MM stays readable
    // even if fg.png is fully opaque in the corners.
    if (s_fg_buf) {
        create_layer_image(scr, s_fg_buf);
    }

    // Clock + battery share one floating overlay so the four-slot row-flex
    // does not treat them as extra cells. Same non-clickable / gesture-bubble
    // contract as the stale LED: a clickable child would kill the swipe.
    s_clock_row = lv_obj_create(scr);
    lv_obj_remove_style_all(s_clock_row);
    lv_obj_set_size(s_clock_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_clock_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_clock_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_clock_row, 4, 0);
    lv_obj_add_flag(s_clock_row, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_clock_row, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_clock_row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(s_clock_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_clock_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_clock_row, LV_ALIGN_TOP_RIGHT, -8, 4);

    s_clock_label = lv_label_create(s_clock_row);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(0xcccccc), 0);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_add_flag(s_clock_label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(s_clock_label, LV_OBJ_FLAG_CLICKABLE);

    s_battery_icon = lv_label_create(s_clock_row);
    lv_obj_set_style_text_font(s_battery_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery_icon, lv_color_hex(0xcccccc), 0);
    lv_label_set_text(s_battery_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_add_flag(s_battery_icon, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(s_battery_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_battery_icon, LV_OBJ_FLAG_HIDDEN);

    // Dim red "no BLE data" LED, same overlay contract as the clock. Hidden
    // until the 75s silence watchdog sets state_get_ble_data_stale().
    s_stale_led = lv_obj_create(scr);
    lv_obj_remove_style_all(s_stale_led);
    lv_obj_set_size(s_stale_led, 8, 8);
    lv_obj_set_style_radius(s_stale_led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_stale_led, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_stale_led, lv_color_hex(0x6a1818), 0);
    lv_obj_add_flag(s_stale_led, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_stale_led, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_stale_led, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(s_stale_led, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_stale_led, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_stale_led, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_stale_led, LV_ALIGN_TOP_LEFT, 8, 6);

    update_main_widgets();
    update_clock_label();
    return scr;
}

// LVGL grid style props store these array pointers directly (not a copy), so
// they must outlive the grid object — file scope, not stack locals.
// Columns are fixed pixel widths, not LV_GRID_CONTENT: with a proportional
// font, ticking digits (uptime's seconds, heap's free-byte count) change
// glyph width slightly every update, so a content-sized column visibly
// jittered the whole table on every refresh. Widths below are sized for the
// longest string each column ever holds ("Uptime: 00:00:00", "Voltage: 4.05
// V"), with margin. (Content-sizing the *container* is still fine — it just
// sums these fixed tracks — see s_status_grid_rows/create_status_screen.)
static const int32_t s_status_grid_cols[] = { 190, 190, LV_GRID_TEMPLATE_LAST };
static const int32_t s_status_grid_rows[] = {
    LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST,
};

static lv_obj_t *create_status_grid_cell(lv_obj_t *grid, uint8_t col, uint8_t row)
{
    lv_obj_t *label = lv_label_create(grid);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_grid_cell(label, LV_GRID_ALIGN_START, col, 1, LV_GRID_ALIGN_START, row, 1);
    return label;
}

static lv_obj_t *create_status_screen(void)
{
    lv_obj_t *scr = create_base_screen(lv_color_hex(0x103010));

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Status");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    // Metric table anchored top-left, sized to its own content (not the full
    // screen width) so there's room to its right for the noise chart, and so
    // it can't overflow the screen and turn on scrolling (see comment on
    // s_status_grid_cols).
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(grid, 28, 0);
    lv_obj_set_style_pad_row(grid, 4, 0);
    lv_obj_set_grid_dsc_array(grid, s_status_grid_cols, s_status_grid_rows);
    lv_obj_set_size(grid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 12, 36);

    s_charge_value_label = create_status_grid_cell(grid, 0, 0);
    s_voltage_value_label = create_status_grid_cell(grid, 1, 0);
    s_uptime_value_label = create_status_grid_cell(grid, 0, 1);
    s_conn_value_label = create_status_grid_cell(grid, 1, 1);
    s_heap_value_label = create_status_grid_cell(grid, 0, 2);
    s_temp_value_label = create_status_grid_cell(grid, 1, 2);

    // Static firmware identity, not a live metric: UTC stamp from CMake
    // (see gen_build_info.cmake), spanning both columns so the ISO string
    // fits without widening the jitter-proof fixed tracks above.
    lv_obj_t *build_label = lv_label_create(grid);
    lv_label_set_text(build_label, "Build: " APP_BUILD_TIMESTAMP);
    lv_obj_set_style_text_color(build_label, lv_color_hex(0x90b090), 0);
    lv_obj_set_grid_cell(build_label, LV_GRID_ALIGN_START, 0, 2, LV_GRID_ALIGN_START, 3, 1);

    // Rough live noise-level graph in the space the fixed-width grid leaves
    // free on the right (see mic.h — this is an approximate reading, not a
    // calibrated meter, so a small sparkline-style chart is all it deserves).
    s_noise_value_label = lv_label_create(scr);
    lv_label_set_text(s_noise_value_label, "Noise: --");
    lv_obj_set_style_text_color(s_noise_value_label, lv_color_white(), 0);
    lv_obj_align(s_noise_value_label, LV_ALIGN_TOP_RIGHT, -8, 8);

    s_noise_chart = lv_chart_create(scr);
    lv_obj_remove_flag(s_noise_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_noise_chart, 190, 120);
    lv_obj_align(s_noise_chart, LV_ALIGN_TOP_RIGHT, -8, 36);
    lv_obj_set_style_bg_color(s_noise_chart, lv_color_hex(0x0a1f0a), 0);
    lv_obj_set_style_border_width(s_noise_chart, 1, 0);
    lv_chart_set_type(s_noise_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_axis_range(s_noise_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(s_noise_chart, 40);
    lv_chart_set_update_mode(s_noise_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_noise_chart, 3, 0);
    s_noise_series = lv_chart_add_series(s_noise_chart, lv_color_hex(0x40e040), LV_CHART_AXIS_PRIMARY_Y);

    return scr;
}

static const char *connection_status_text(app_conn_status_t status)
{
    switch (status) {
    case APP_CONN_CONNECTING:
        return "BLE: connecting";
    case APP_CONN_SECURE:
        return "BLE: connected";
    case APP_CONN_DISCONNECTED:
    default:
        return "BLE: --";
    }
}

static void update_status_widgets(void)
{
    if (!s_charge_value_label) {
        return;
    }

    if (state_get_battery_present()) {
        lv_label_set_text_fmt(s_charge_value_label, "Charge: %u%%", (unsigned)state_get_battery_percent());
        uint16_t mv = state_get_battery_voltage_mv();
        lv_label_set_text_fmt(s_voltage_value_label, "Voltage: %u.%02u V", mv / 1000u, (mv % 1000u) / 10u);
    } else {
        lv_label_set_text(s_charge_value_label, "Charge: --");
        lv_label_set_text(s_voltage_value_label, "Voltage: --");
    }

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    lv_label_set_text_fmt(s_uptime_value_label, "Uptime: %02u:%02u:%02u",
                           (unsigned)(uptime_s / 3600), (unsigned)((uptime_s / 60) % 60), (unsigned)(uptime_s % 60));

    lv_label_set_text(s_conn_value_label, connection_status_text(state_get_connection_status()));

    lv_label_set_text_fmt(s_heap_value_label, "Heap: %u KB", (unsigned)(esp_get_free_heap_size() / 1024));

    float temp_c;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &temp_c) == ESP_OK) {
        int whole = (int)temp_c;
        int frac = (int)((temp_c - whole) * 10);
        lv_label_set_text_fmt(s_temp_value_label, "Temp: %d.%d C", whole, frac < 0 ? -frac : frac);
    } else {
        lv_label_set_text(s_temp_value_label, "Temp: --");
    }
}

// Battery/BLE state changes already trigger a redraw via on_state_changed(),
// but uptime has no state event of its own — it just needs to tick once a
// second while the status screen is the one on screen.
static void status_tick_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_shown_screen == APP_SCREEN_STATUS) {
        update_status_widgets();
    }
}

// Runs faster than status_tick_timer_cb so the noise graph actually looks
// live instead of stepping once a second like the rest of the table.
static void noise_tick_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_shown_screen == APP_SCREEN_STATUS) {
        uint8_t level = mic_get_noise_level();
        lv_chart_set_next_value(s_noise_chart, s_noise_series, level);
        lv_label_set_text_fmt(s_noise_value_label, "Noise: %u", (unsigned)level);
    }
}

static void clock_tick_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_shown_screen == APP_SCREEN_MAIN) {
        update_clock_label();
    }
}

static void create_screens(void)
{
    s_screens[APP_SCREEN_MAIN] = create_main_screen();
    s_screens[APP_SCREEN_STATUS] = create_status_screen();
    s_screens[APP_SCREEN_SETTINGS] = create_settings_screen();
    update_status_widgets();
    lv_screen_load(s_screens[APP_SCREEN_MAIN]);
}

// Fired by state after any state_set_*() call actually changes something —
// possibly from another task (e.g. battery's timer). Re-renders whatever
// changed under the LVGL lock, the only place in the app that needs to take
// it, since every other module only ever reaches the UI through
// state_set_*()/events_post().
static void on_state_changed(void)
{
    if (!lvgl_port_lock(EXTERNAL_LOCK_TIMEOUT_MS)) {
        return;
    }

    update_main_widgets();
    update_status_widgets();
    update_settings_widgets();

    app_screen_t next = state_get_active_screen();
    if (next != s_shown_screen) {
        // Infers the swipe direction from how the screen index moved — state
        // itself only tracks "what is active now", not "which way we got
        // there".
        int forward_steps = (next - s_shown_screen + APP_SCREEN_COUNT) % APP_SCREEN_COUNT;
        lv_scr_load_anim_t anim = (forward_steps == 1) ? LV_SCR_LOAD_ANIM_MOVE_TOP : LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
        lv_screen_load_anim(s_screens[next], anim, 200, 0, false);
        s_shown_screen = next;
    }

    lvgl_port_unlock();
}

void render_init(const lcd_handle_t *lcd)
{
    // Read wallpapers before LVGL allocates the double framebuffer so the
    // SDMMC/FATFS DMA pool is already released when those PSRAM/DRAM
    // allocs happen. Decode needs lv_init (lodepng), so compressed bytes
    // sit in PSRAM until then; sd_unmount itself is the DRAM win.
    uint8_t *bg_png = NULL;
    uint8_t *fg_png = NULL;
    size_t bg_png_len = 0;
    size_t fg_png_len = 0;
    if (sd_mount()) {
        bg_png = sd_read_file("bg.png", &bg_png_len);
        fg_png = sd_read_file("fg.png", &fg_png_len);
        sd_unmount();
    }

    // Best-effort: leave s_tsens NULL on failure, update_status_widgets()
    // falls back to "--" rather than treating this as fatal.
    temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&tsens_cfg, &s_tsens) == ESP_OK) {
        if (temperature_sensor_enable(s_tsens) != ESP_OK) {
            temperature_sensor_uninstall(s_tsens);
            s_tsens = NULL;
        }
    }

    ESP_LOGI(TAG, "Initializing LVGL port");
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // NimBLE is pinned to core 0 (CONFIG_BT_NIMBLE_PINNED_TO_CORE_0); keep
    // rendering off that core so a busy radio/host doesn't stall the UI (and
    // vice versa).
    lvgl_cfg.task_affinity = 1;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // Decode while the compressed blobs still exist, then free them
    // before the double framebuffer (~440 KB) is allocated.
    if (lvgl_port_lock(0)) {
        if (bg_png != NULL) {
            s_bg_buf = decode_png_blob(bg_png, bg_png_len, "bg.png");
            free(bg_png);
            bg_png = NULL;
        }
        if (fg_png != NULL) {
            s_fg_buf = decode_png_blob(fg_png, fg_png_len, "fg.png");
            free(fg_png);
            fg_png = NULL;
        }
        lvgl_port_unlock();
    } else {
        free(bg_png);
        free(fg_png);
        bg_png = NULL;
        fg_png = NULL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd->io_handle,
        .panel_handle = lcd->panel_handle,
        .buffer_size = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .swap_bytes = true,
            // The AXS15231B's QSPI draw path drops the row-address (RASET)
            // command, so partial-rect flushes land at the wrong Y offset.
            // Forcing a full-frame redraw every flush keeps the panel's
            // write window aligned with what LVGL intends to draw.
            .full_refresh = true,
            // esp_lcd_axs15231b's swap_xy is known-broken on this chip, so
            // rotate in software (via lv_display_set_rotation below) instead
            // of the panel's hardware swap_xy/mirror path.
            .sw_rotate = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_disp(indev, disp);

    if (lvgl_port_lock(0)) {
        create_screens();
        lvgl_port_unlock();
    }

    state_set_on_change_cb(on_state_changed);
    // Host-sync may have published ble_paired before this callback existed
    // (ble_app_init runs first). Re-read so Pair/Forget matches NVS bonds.
    if (lvgl_port_lock(0)) {
        update_settings_widgets();
        lvgl_port_unlock();
    }
    s_status_tick_timer = lv_timer_create(status_tick_timer_cb, 1000, NULL);
    s_noise_tick_timer = lv_timer_create(noise_tick_timer_cb, 200, NULL);
    s_clock_tick_timer = lv_timer_create(clock_tick_timer_cb, 1000, NULL);
}
