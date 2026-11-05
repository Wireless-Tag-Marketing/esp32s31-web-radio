#include "radio_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"
#include "stations.h"

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 4
#error "This UI is validated against LVGL 9.4.x"
#endif

typedef struct {
    bool ready;
    bool updating;
    radio_ui_event_callback_t callback;
    void *callback_user_data;
    lv_obj_t *wifi_pill;
    lv_obj_t *wifi_label;
    lv_obj_t *station_counter;
    lv_obj_t *station_name;
    lv_obj_t *stream_title;
    lv_obj_t *codec_info;
    lv_obj_t *play_icon;
    lv_obj_t *volume_slider;
    lv_obj_t *volume_label;
    lv_obj_t *hint_label;
    lv_obj_t *picker;
    lv_obj_t *eq_bars[12];
    int last_station;
    int last_station_count;
    int last_volume;
    int last_sample_rate;
    int last_wifi_state;
    bool last_audio_active;
    bool last_audio_running;
    bool audio_state_valid;
    uint8_t visual_phase;
    char last_station_name[72];
    char last_stream_title[128];
    char last_codec[12];
    char last_wifi_name[64];
} radio_ui_context_t;

static const char *TAG = "radio_ui";
static radio_ui_context_t s_ui;

static const lv_color_t COLOR_BG = LV_COLOR_MAKE(0x08, 0x0B, 0x14);
static const lv_color_t COLOR_PANEL = LV_COLOR_MAKE(0x12, 0x17, 0x25);
static const lv_color_t COLOR_PANEL_ALT = LV_COLOR_MAKE(0x1A, 0x21, 0x34);
static const lv_color_t COLOR_ACCENT = LV_COLOR_MAKE(0x7B, 0x61, 0xFF);
static const lv_color_t COLOR_ACCENT_2 = LV_COLOR_MAKE(0x12, 0xD6, 0xC5);
static const lv_color_t COLOR_TEXT = LV_COLOR_MAKE(0xF4, 0xF6, 0xFF);
static const lv_color_t COLOR_MUTED = LV_COLOR_MAKE(0x91, 0x9A, 0xB2);

static void emit_event(radio_ui_event_type_t type, int value)
{
    if (s_ui.callback == NULL) {
        return;
    }
    const radio_ui_event_t event = {
        .type = type,
        .value = value,
    };
    s_ui.callback(&event, s_ui.callback_user_data);
}

static void control_clicked(lv_event_t *event)
{
    emit_event((radio_ui_event_type_t)(intptr_t)lv_event_get_user_data(event), 0);
}

static void wifi_setup_clicked(lv_event_t *event)
{
    (void)event;
    emit_event(RADIO_UI_EVENT_WIFI_SETUP, 0);
}

static void test_tone_clicked(lv_event_t *event)
{
    (void)event;
    emit_event(RADIO_UI_EVENT_TEST_TONE, 0);
}

static lv_obj_t *create_card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_bg_color(card, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x252D42), 0);
    lv_obj_set_style_shadow_width(card, 20, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, 80, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *create_control_button(lv_obj_t *parent, const char *symbol,
                                       radio_ui_event_type_t event_type, bool primary)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, primary ? 76 : 58, primary ? 76 : 58);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, primary ? COLOR_ACCENT : COLOR_PANEL_ALT, 0);
    lv_obj_set_style_bg_grad_color(button, primary ? COLOR_ACCENT_2 : COLOR_PANEL, 0);
    lv_obj_set_style_bg_grad_dir(button, primary ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, primary ? 18 : 8, 0);
    lv_obj_set_style_shadow_color(button, primary ? COLOR_ACCENT : lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(button, primary ? 100 : 60, 0);
    lv_obj_add_event_cb(button, control_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)event_type);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(label, primary ? &lv_font_montserrat_36 : &lv_font_montserrat_28, 0);
    lv_obj_center(label);
    if (event_type == RADIO_UI_EVENT_PLAY_PAUSE) {
        s_ui.play_icon = label;
    }
    return button;
}

static void create_equalizer(lv_obj_t *parent)
{
    for (int i = 0; i < 12; ++i) {
        lv_obj_t *bar = lv_obj_create(parent);
        s_ui.eq_bars[i] = bar;
        lv_obj_set_size(bar, 12, 8 + (i % 3) * 2);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 12 + i * 24, -8);
        lv_obj_set_style_radius(bar, 6, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, i < 6 ? COLOR_ACCENT : COLOR_ACCENT_2, 0);
        lv_obj_set_style_bg_opa(bar, 90, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void picker_hide(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(s_ui.picker, LV_OBJ_FLAG_HIDDEN);
}

static void picker_show(lv_event_t *event)
{
    (void)event;
    lv_obj_remove_flag(s_ui.picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.picker);
}

static void picker_station_selected(lv_event_t *event)
{
    const int station = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_add_flag(s_ui.picker, LV_OBJ_FLAG_HIDDEN);
    emit_event(RADIO_UI_EVENT_SELECT_STATION, station);
}

static void station_gesture(lv_event_t *event)
{
    (void)event;
    const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    if (direction == LV_DIR_LEFT) {
        emit_event(RADIO_UI_EVENT_NEXT, 0);
    } else if (direction == LV_DIR_RIGHT) {
        emit_event(RADIO_UI_EVENT_PREVIOUS, 0);
    }
}

static void volume_changed(lv_event_t *event)
{
    if (s_ui.updating) {
        return;
    }
    lv_obj_t *slider = lv_event_get_target_obj(event);
    emit_event(RADIO_UI_EVENT_VOLUME, lv_slider_get_value(slider));
}

static void create_station_picker(lv_obj_t *root)
{
    s_ui.picker = lv_obj_create(root);
    lv_obj_set_pos(s_ui.picker, 22, 18);
    lv_obj_set_size(s_ui.picker, 756, 444);
    lv_obj_set_style_radius(s_ui.picker, 28, 0);
    lv_obj_set_style_bg_color(s_ui.picker, lv_color_hex(0x0D1220), 0);
    lv_obj_set_style_bg_opa(s_ui.picker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.picker, 2, 0);
    lv_obj_set_style_border_color(s_ui.picker, COLOR_ACCENT, 0);
    lv_obj_set_style_shadow_width(s_ui.picker, 30, 0);
    lv_obj_set_style_shadow_color(s_ui.picker, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_ui.picker, 180, 0);
    lv_obj_remove_flag(s_ui.picker, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_ui.picker);
    lv_label_set_text(title, "CHOOSE A STATION");
    lv_obj_set_pos(title, 24, 12);
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *close = lv_button_create(s_ui.picker);
    lv_obj_set_pos(close, 666, 4);
    lv_obj_set_size(close, 56, 48);
    lv_obj_set_style_radius(close, 16, 0);
    lv_obj_set_style_bg_color(close, COLOR_PANEL_ALT, 0);
    lv_obj_add_event_cb(close, picker_hide, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(close_label, COLOR_TEXT, 0);
    lv_obj_center(close_label);

    lv_obj_t *list = lv_list_create(s_ui.picker);
    lv_obj_set_pos(list, 16, 66);
    lv_obj_set_size(list, 716, 352);
    lv_obj_set_style_radius(list, 18, 0);
    lv_obj_set_style_bg_color(list, COLOR_BG, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_text_color(list, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_row(list, 5, 0);

    for (size_t i = 0; i < g_radio_station_count; ++i) {
        lv_obj_t *button = lv_list_add_button(list, LV_SYMBOL_AUDIO, g_radio_stations[i].name);
        lv_obj_set_height(button, 54);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x222C43), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x35415F), 0);
        lv_obj_set_style_text_color(button, lv_color_hex(0xFAFBFF), 0);
        lv_obj_set_style_text_font(button, &lv_font_montserrat_20, 0);
        lv_obj_set_style_bg_color(button, COLOR_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(button, COLOR_ACCENT_2, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_PRESSED);

        const uint32_t child_count = lv_obj_get_child_count(button);
        for (uint32_t child_index = 0; child_index < child_count; ++child_index) {
            lv_obj_t *child = lv_obj_get_child(button, child_index);
            if (lv_obj_check_type(child, &lv_label_class)) {
                lv_obj_set_style_text_color(child, lv_color_hex(0xFAFBFF), 0);
                lv_obj_set_style_text_font(child, &lv_font_montserrat_20, 0);
            } else if (lv_obj_check_type(child, &lv_image_class)) {
                lv_obj_set_style_image_recolor(child, COLOR_ACCENT_2, 0);
                lv_obj_set_style_image_recolor_opa(child, LV_OPA_COVER, 0);
            }
        }
        lv_obj_add_event_cb(button, picker_station_selected, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_add_flag(s_ui.picker, LV_OBJ_FLAG_HIDDEN);
}

esp_err_t radio_ui_init(radio_ui_event_callback_t callback, void *user_data)
{
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.callback = callback;
    s_ui.callback_user_data = user_data;
    s_ui.last_station = -1;
    s_ui.last_station_count = -1;
    s_ui.last_volume = -1;
    s_ui.last_sample_rate = -1;
    s_ui.last_wifi_state = -1;

    bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };
    display_config.lvgl_port_cfg.task_priority = 5;
    display_config.lvgl_port_cfg.task_affinity = 1;
    display_config.lvgl_port_cfg.task_max_sleep_ms = 50;
    lv_display_t *display = bsp_display_start_with_config(&display_config);
    if (display == NULL) {
        return ESP_FAIL;
    }
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *root = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(root, COLOR_BG, 0);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(0x181233), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_text_color(root, COLOR_TEXT, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_pill = lv_obj_create(root);
    lv_obj_set_pos(title_pill, 24, 14);
    lv_obj_set_size(title_pill, 500, 42);
    lv_obj_set_style_radius(title_pill, 21, 0);
    lv_obj_set_style_bg_color(title_pill, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_grad_color(title_pill, COLOR_ACCENT_2, 0);
    lv_obj_set_style_bg_grad_dir(title_pill, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(title_pill, 0, 0);
    lv_obj_set_style_pad_all(title_pill, 0, 0);
    lv_obj_remove_flag(title_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *app_title = lv_label_create(title_pill);
    lv_label_set_text(app_title, LV_SYMBOL_AUDIO "  WebRadio By Wireless-Tag");
    lv_obj_set_style_text_color(app_title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(app_title, &lv_font_montserrat_20, 0);
    lv_obj_set_width(app_title, 460);
    lv_obj_set_style_text_align(app_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(app_title, LV_ALIGN_LEFT_MID, 18, 0);

    s_ui.wifi_pill = lv_obj_create(root);
    lv_obj_set_pos(s_ui.wifi_pill, 552, 14);
    lv_obj_set_size(s_ui.wifi_pill, 222, 42);
    lv_obj_set_style_radius(s_ui.wifi_pill, 21, 0);
    lv_obj_set_style_bg_color(s_ui.wifi_pill, COLOR_PANEL, 0);
    lv_obj_set_style_border_width(s_ui.wifi_pill, 0, 0);
    lv_obj_remove_flag(s_ui.wifi_pill, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.wifi_label = lv_label_create(s_ui.wifi_pill);
    lv_label_set_text(s_ui.wifi_label, LV_SYMBOL_WIFI "  OFFLINE");
    lv_obj_set_style_text_font(s_ui.wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_center(s_ui.wifi_label);

    lv_obj_t *station_card = create_card(root, 24, 72, 500, 294);
    lv_obj_add_flag(station_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(station_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(station_card, picker_show, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(station_card, station_gesture, LV_EVENT_GESTURE, NULL);

    s_ui.station_counter = lv_label_create(station_card);
    lv_label_set_text(s_ui.station_counter, "STATION 01 / 01");
    lv_obj_set_style_text_font(s_ui.station_counter, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ui.station_counter, COLOR_ACCENT_2, 0);
    lv_obj_set_pos(s_ui.station_counter, 2, 0);

    s_ui.station_name = lv_label_create(station_card);
    lv_label_set_text(s_ui.station_name, "Internet Radio");
    lv_obj_set_pos(s_ui.station_name, 0, 36);
    lv_obj_set_width(s_ui.station_name, 450);
    lv_label_set_long_mode(s_ui.station_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_ui.station_name, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(s_ui.station_name, COLOR_TEXT, 0);

    s_ui.stream_title = lv_label_create(station_card);
    lv_label_set_text(s_ui.stream_title, "Waiting for stream");
    lv_obj_set_pos(s_ui.stream_title, 0, 92);
    lv_obj_set_width(s_ui.stream_title, 450);
    lv_label_set_long_mode(s_ui.stream_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_ui.stream_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ui.stream_title, COLOR_MUTED, 0);

    s_ui.codec_info = lv_label_create(station_card);
    lv_label_set_text(s_ui.codec_info, "OUTPUT 16 kHz  |  SOURCE --");
    lv_obj_set_pos(s_ui.codec_info, 0, 142);
    lv_obj_set_style_text_font(s_ui.codec_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ui.codec_info, COLOR_MUTED, 0);

    s_ui.hint_label = lv_label_create(station_card);
    lv_label_set_text(s_ui.hint_label, "Tap to browse  |  Swipe to tune");
    lv_obj_set_pos(s_ui.hint_label, 0, 174);
    lv_obj_set_width(s_ui.hint_label, 450);
    lv_label_set_long_mode(s_ui.hint_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_ui.hint_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(s_ui.hint_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ui.hint_label, COLOR_MUTED, 0);
    create_equalizer(station_card);

    lv_obj_t *control_card = create_card(root, 548, 72, 228, 294);
    lv_obj_t *control_title = lv_label_create(control_card);
    lv_label_set_text(control_title, "LIVE CONTROLS");
    lv_obj_set_style_text_font(control_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(control_title, COLOR_MUTED, 0);
    lv_obj_align(control_title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *transport = lv_obj_create(control_card);
    lv_obj_set_pos(transport, -4, 48);
    lv_obj_set_size(transport, 196, 94);
    lv_obj_set_flex_flow(transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(transport, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(transport, 0, 0);
    lv_obj_set_style_pad_all(transport, 0, 0);
    lv_obj_remove_flag(transport, LV_OBJ_FLAG_SCROLLABLE);
    create_control_button(transport, LV_SYMBOL_PREV, RADIO_UI_EVENT_PREVIOUS, false);
    create_control_button(transport, LV_SYMBOL_PLAY, RADIO_UI_EVENT_PLAY_PAUSE, true);
    create_control_button(transport, LV_SYMBOL_NEXT, RADIO_UI_EVENT_NEXT, false);

    lv_obj_t *setup_button = lv_button_create(control_card);
    lv_obj_set_pos(setup_button, 0, 168);
    lv_obj_set_size(setup_button, 90, 62);
    lv_obj_set_style_radius(setup_button, 16, 0);
    lv_obj_set_style_bg_color(setup_button, COLOR_PANEL_ALT, 0);
    lv_obj_add_event_cb(setup_button, wifi_setup_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *setup_label = lv_label_create(setup_button);
    lv_label_set_text(setup_label, LV_SYMBOL_WIFI "\nSETUP");
    lv_obj_set_style_text_align(setup_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(setup_label);

    lv_obj_t *test_button = lv_button_create(control_card);
    lv_obj_set_pos(test_button, 104, 168);
    lv_obj_set_size(test_button, 90, 62);
    lv_obj_set_style_radius(test_button, 16, 0);
    lv_obj_set_style_bg_color(test_button, COLOR_PANEL_ALT, 0);
    lv_obj_add_event_cb(test_button, test_tone_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *test_label = lv_label_create(test_button);
    lv_label_set_text(test_label, LV_SYMBOL_VOLUME_MAX "\nTEST");
    lv_obj_set_style_text_align(test_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(test_label);

    lv_obj_t *volume_card = create_card(root, 24, 388, 752, 70);
    lv_obj_set_style_pad_all(volume_card, 12, 0);
    lv_obj_t *volume_icon = lv_label_create(volume_card);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_pos(volume_icon, 8, 10);
    lv_obj_set_style_text_font(volume_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(volume_icon, COLOR_ACCENT_2, 0);

    s_ui.volume_slider = lv_slider_create(volume_card);
    lv_obj_set_pos(s_ui.volume_slider, 64, 13);
    lv_obj_set_size(s_ui.volume_slider, 584, 22);
    lv_slider_set_range(s_ui.volume_slider, 0, APP_MAX_VOLUME);
    lv_slider_set_value(s_ui.volume_slider, APP_DEFAULT_VOLUME, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ui.volume_slider, COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.volume_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s_ui.volume_slider, COLOR_ACCENT_2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s_ui.volume_slider, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui.volume_slider, COLOR_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_ui.volume_slider, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(s_ui.volume_slider, volume_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_ui.volume_label = lv_label_create(volume_card);
    lv_label_set_text(s_ui.volume_label, "55%");
    lv_obj_set_pos(s_ui.volume_label, 664, 7);
    lv_obj_set_width(s_ui.volume_label, 60);
    lv_obj_set_style_text_align(s_ui.volume_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_ui.volume_label, &lv_font_montserrat_20, 0);

    create_station_picker(root);
    s_ui.ready = true;
    bsp_display_unlock();
    ESP_LOGI(TAG, "Touch UI ready: 800x480 RGB565, GT1151, LVGL %d.%d.%d, author Wireless-Tag",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return ESP_OK;
}

void radio_ui_render(const radio_ui_view_t *view)
{
    if (!s_ui.ready || view == NULL || !bsp_display_lock(50)) {
        return;
    }

    const char *station_name = view->station_name != NULL ? view->station_name : "Internet Radio";
    if (view->station_index != s_ui.last_station ||
        view->station_count != s_ui.last_station_count) {
        lv_label_set_text_fmt(s_ui.station_counter, "STATION %02d / %02d",
                              view->station_index + 1, view->station_count);
        s_ui.last_station = view->station_index;
        s_ui.last_station_count = view->station_count;
    }
    if (strcmp(station_name, s_ui.last_station_name) != 0) {
        lv_label_set_text(s_ui.station_name, station_name);
        strlcpy(s_ui.last_station_name, station_name, sizeof(s_ui.last_station_name));
    }
    const char *title = view->stream_title != NULL && view->stream_title[0] != '\0' ?
                        view->stream_title : (view->audio_running ? "Live stream" :
                                             (view->audio_active ? "Connecting & buffering..." :
                                                                  "Ready to play"));
    if (strcmp(title, s_ui.last_stream_title) != 0) {
        lv_label_set_text(s_ui.stream_title, title);
        strlcpy(s_ui.last_stream_title, title, sizeof(s_ui.last_stream_title));
    }
    const char *codec = view->codec != NULL && view->codec[0] != '\0' ? view->codec : "--";
    if (view->source_sample_rate != s_ui.last_sample_rate ||
        strcmp(codec, s_ui.last_codec) != 0) {
        if (view->source_sample_rate > 0) {
            lv_label_set_text_fmt(s_ui.codec_info, "OUTPUT 16 kHz  |  SOURCE %s %d kHz",
                                  codec, view->source_sample_rate / 1000);
        } else {
            lv_label_set_text(s_ui.codec_info, "OUTPUT 16 kHz  |  SOURCE --");
        }
        s_ui.last_sample_rate = view->source_sample_rate;
        strlcpy(s_ui.last_codec, codec, sizeof(s_ui.last_codec));
    }
    if (!s_ui.audio_state_valid || view->audio_active != s_ui.last_audio_active) {
        lv_label_set_text(s_ui.play_icon, view->audio_active ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        s_ui.last_audio_active = view->audio_active;
    }
    if (!s_ui.audio_state_valid || view->audio_running != s_ui.last_audio_running) {
        for (size_t i = 0; i < sizeof(s_ui.eq_bars) / sizeof(s_ui.eq_bars[0]); ++i) {
            lv_obj_set_style_bg_opa(s_ui.eq_bars[i], view->audio_running ? LV_OPA_COVER : 90, 0);
            if (!view->audio_running) {
                lv_obj_set_height(s_ui.eq_bars[i], 8 + (i % 3) * 2);
            }
        }
        s_ui.last_audio_running = view->audio_running;
        s_ui.audio_state_valid = true;
    }

    const char *wifi_name = view->wifi_name != NULL ? view->wifi_name : "";
    if ((int)view->wifi_state != s_ui.last_wifi_state ||
        strcmp(wifi_name, s_ui.last_wifi_name) != 0) {
        const char *wifi_text = "OFFLINE";
        lv_color_t wifi_color = LV_COLOR_MAKE(0x7B, 0x35, 0x48);
        if (view->wifi_state == RADIO_UI_WIFI_CONNECTED) {
            wifi_text = wifi_name[0] != '\0' ? wifi_name : "CONNECTED";
            wifi_color = lv_color_hex(0x137269);
        } else if (view->wifi_state == RADIO_UI_WIFI_CONNECTING) {
            wifi_text = "CONNECTING";
            wifi_color = lv_color_hex(0x6A5420);
        } else if (view->wifi_state == RADIO_UI_WIFI_CONFIG_AP) {
            wifi_text = wifi_name[0] != '\0' ? wifi_name : "SETUP AP";
            wifi_color = COLOR_ACCENT;
        }
        char wifi_line[72];
        snprintf(wifi_line, sizeof(wifi_line), LV_SYMBOL_WIFI "  %.24s", wifi_text);
        lv_label_set_text(s_ui.wifi_label, wifi_line);
        lv_obj_set_style_bg_color(s_ui.wifi_pill, wifi_color, 0);
        if (view->wifi_state == RADIO_UI_WIFI_CONFIG_AP) {
            lv_label_set_text_fmt(s_ui.hint_label, "Join %.20s  |  Open 192.168.4.1",
                                  wifi_name[0] != '\0' ? wifi_name : "WEBRADIO");
        } else {
            lv_label_set_text(s_ui.hint_label, "Tap to browse  |  Swipe to tune");
        }
        s_ui.last_wifi_state = (int)view->wifi_state;
        strlcpy(s_ui.last_wifi_name, wifi_name, sizeof(s_ui.last_wifi_name));
    }

    if (view->volume != s_ui.last_volume) {
        s_ui.updating = true;
        lv_slider_set_value(s_ui.volume_slider, view->volume, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_ui.volume_label, "%d%%", view->volume);
        s_ui.updating = false;
        s_ui.last_volume = view->volume;
    }

    bsp_display_unlock();
}

void radio_ui_set_audio_level(int level)
{
    if (!s_ui.ready || !s_ui.last_audio_running || !bsp_display_lock(20)) {
        return;
    }
    if (level < 0) {
        level = 0;
    } else if (level > 32767) {
        level = 32767;
    }

    static const uint8_t shape[12] = {62, 84, 70, 100, 76, 92, 68, 88, 72, 96, 78, 64};
    s_ui.visual_phase++;
    const int amplitude = 8 + level * 40 / 32767;
    for (size_t i = 0; i < sizeof(s_ui.eq_bars) / sizeof(s_ui.eq_bars[0]); ++i) {
        const int wobble = ((s_ui.visual_phase * (i + 3) + i * 7) % 9) - 4;
        int height = amplitude * shape[i] / 100 + wobble;
        if (height < 6) {
            height = 6;
        } else if (height > 48) {
            height = 48;
        }
        lv_obj_set_height(s_ui.eq_bars[i], height);
    }
    bsp_display_unlock();
}
