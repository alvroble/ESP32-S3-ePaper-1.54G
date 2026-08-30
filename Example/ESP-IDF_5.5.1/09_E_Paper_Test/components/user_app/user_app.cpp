#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_wifi_bsp.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "user_app.h"
#include "driver/gpio.h"

#include "lvgl.h"
#include "lvgl_epaper_port.h"

#define TAG "BTC"

#define SLEEP_DURATION_SEC  (5 * 60)  // 5 minutes Deep Sleep
#define SEED_EPOCH          1735689600

#define BTC_URL "https://api.exchange.coinbase.com/products/BTC-USD/candles?granularity=3600"
#define HTTP_TIMEOUT_MS  15000
#define HTTP_BUF_SIZE    4096

#define VBAT_PWR_PIN GPIO_NUM_17
#define EPD_PWR_PIN  GPIO_NUM_6

typedef struct {
    char  data[HTTP_BUF_SIZE];
    int   len;
} http_buf_t;

typedef struct {
    double current_price;
    double change_24h;
    double high_24h;
    double low_24h;
    int32_t history[24];
    int count;
} btc_market_data_t;

static lv_obj_t *s_price_label;
static lv_obj_t *s_sats_label;
static lv_obj_t *s_change_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_batt_label;
static lv_obj_t *s_high_label;
static lv_obj_t *s_low_label;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;

#define CHART_POINTS 24
RTC_DATA_ATTR static uint32_t s_boot_count = 0;

static float read_battery_voltage(void)
{
    adc_oneshot_unit_handle_t adc1_handle = NULL;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_config1, &adc1_handle);
    if (err != ESP_OK) return 0.0f;

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &config);

    adc_cali_handle_t cali_handle = NULL;
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    bool cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK);

    // Read multiple samples and average
    int raw_sum = 0;
    const int samples = 8;
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &raw);
        raw_sum += raw;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    int raw_avg = raw_sum / samples;

    float voltage = 0.0f;
    if (cali_ok && cali_handle) {
        int voltage_mv = 0;
        adc_cali_raw_to_voltage(cali_handle, raw_avg, &voltage_mv);
        voltage = (float)voltage_mv * 0.001f * 2.0f; // 1/2 resistor divider on board
        adc_cali_delete_scheme_curve_fitting(cali_handle);
    } else {
        voltage = ((float)raw_avg / 4095.0f) * 3.3f * 2.0f;
    }

    adc_oneshot_del_unit(adc1_handle);
    return voltage;
}

static int battery_voltage_to_percentage(float v)
{
    if (v >= 4.20f) return 100;
    if (v <= 3.30f) return 0;
    if (v >= 4.05f) return 90 + (int)((v - 4.05f) / 0.15f * 10);
    if (v >= 3.95f) return 80 + (int)((v - 3.95f) / 0.10f * 10);
    if (v >= 3.85f) return 70 + (int)((v - 3.85f) / 0.10f * 10);
    if (v >= 3.80f) return 60 + (int)((v - 3.80f) / 0.05f * 10);
    if (v >= 3.75f) return 50 + (int)((v - 3.75f) / 0.05f * 10);
    if (v >= 3.70f) return 40 + (int)((v - 3.70f) / 0.05f * 10);
    if (v >= 3.65f) return 30 + (int)((v - 3.65f) / 0.05f * 10);
    if (v >= 3.60f) return 20 + (int)((v - 3.60f) / 0.05f * 10);
    if (v >= 3.50f) return 10 + (int)((v - 3.50f) / 0.10f * 10);
    return (int)((v - 3.30f) / 0.20f * 10);
}

static const char *get_battery_symbol(int pct, float v)
{
    if (v >= 4.30f) return LV_SYMBOL_USB;
    if (pct >= 85) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 60) return LV_SYMBOL_BATTERY_3;
    if (pct >= 35) return LV_SYMBOL_BATTERY_2;
    if (pct >= 15) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void seed_time(void)
{
    if (time(NULL) < 1700000000) {
        struct timeval tv = { .tv_sec = SEED_EPOCH, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "RTC seeded");
    }
}

static void board_power_init(void)
{
    gpio_config_t io = {};
    io.intr_type    = GPIO_INTR_DISABLE;
    io.mode         = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << VBAT_PWR_PIN) | (1ULL << EPD_PWR_PIN);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io));

    // Latch battery power circuit ON immediately and keep held in deep sleep
    gpio_set_level(VBAT_PWR_PIN, 1);
    gpio_hold_en(VBAT_PWR_PIN);
    gpio_deep_sleep_hold_en();
}

static void epd_power_on(void)  { gpio_set_level(EPD_PWR_PIN, 0); }
static void epd_power_off(void) { gpio_set_level(EPD_PWR_PIN, 1); }

// Parse 24 hourly candles from Coinbase Exchange: [[time, low, high, open, close, volume], ...]
static bool parse_coinbase_candles(const char *body, btc_market_data_t *out)
{
    const char *p = strchr(body, '[');
    if (!p) return false;
    p++; // skip outer '['

    double latest_close = 0.0;
    double oldest_open = 0.0;
    double max_h = 0.0;
    double min_l = 1e9;
    int count = 0;
    double temp_closes[24];

    while (*p && count < 24) {
        p = strchr(p, '[');
        if (!p) break;
        p++; // skip inner '['

        // Format: [ time, low, high, open, close, volume ]
        strtol(p, (char **)&p, 10);
        if (*p == ',') p++;
        double low = strtod(p, (char **)&p);
        if (*p == ',') p++;
        double high = strtod(p, (char **)&p);
        if (*p == ',') p++;
        double open = strtod(p, (char **)&p);
        if (*p == ',') p++;
        double close = strtod(p, (char **)&p);

        if (low <= 0 || high <= 0 || close <= 0) break;

        if (count == 0) {
            latest_close = close;
        }
        oldest_open = open;

        if (high > max_h) max_h = high;
        if (low < min_l) min_l = low;

        temp_closes[count] = close;
        count++;

        p = strchr(p, ']');
        if (p) p++;
    }

    if (count < 12) return false;

    out->current_price = latest_close;
    out->high_24h = max_h;
    out->low_24h = min_l;
    if (oldest_open > 0) {
        out->change_24h = ((latest_close - oldest_open) / oldest_open) * 100.0;
    } else {
        out->change_24h = 0.0;
    }
    out->count = count;

    // Arrange chronological: index 0 is oldest (left), index count-1 is newest (right)
    for (int i = 0; i < count; i++) {
        out->history[i] = (int32_t)round(temp_closes[count - 1 - i]);
    }

    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *buf = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (buf && buf->len + evt->data_len < HTTP_BUF_SIZE) {
            memcpy(buf->data + buf->len, evt->data, evt->data_len);
            buf->len += evt->data_len;
            buf->data[buf->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool fetch_btc_market_data(btc_market_data_t *out)
{
    http_buf_t *buf = (http_buf_t *)malloc(sizeof(http_buf_t));
    if (!buf) return false;
    buf->len = 0;
    buf->data[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = BTC_URL,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = buf,
        .skip_cert_common_name_check = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(buf);
        return false;
    }

    esp_http_client_set_header(c, "Accept", "application/json");
    esp_http_client_set_header(c, "User-Agent", "ESP32-S3-ePaper-Ticker");

    bool ok = false;
    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) {
        int s = esp_http_client_get_status_code(c);
        if (s == 200 && buf->len > 0) {
            ok = parse_coinbase_candles(buf->data, out);
        } else {
            ESP_LOGW(TAG, "HTTP status: %d, len: %d", s, buf->len);
        }
    } else {
        ESP_LOGW(TAG, "fetch: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
    free(buf);
    return ok;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // ==========================================
    // 1. HEADER (Y: 0, H: 22, W: 200) - Black Background
    // ==========================================
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 200, 22);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_black(), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "BITCOIN TRACKER");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(title, 6, 4);

    lv_obj_t *live_label = lv_label_create(header);
    lv_label_set_text(live_label, LV_SYMBOL_WIFI " Live");
    lv_obj_set_style_text_color(live_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(live_label, &lv_font_montserrat_12, 0);
    lv_obj_align(live_label, LV_ALIGN_RIGHT_MID, -6, 0);

    // ==========================================
    // 2. PRICE CARD (Y: 22, H: 50, W: 200)
    // ==========================================
    s_price_label = lv_label_create(scr);
    lv_label_set_text(s_price_label, "$ --");
    lv_obj_set_style_text_color(s_price_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_price_label, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(s_price_label, 8, 25);

    // Sub-row: Left: "BTC / USD" (Montserrat 12), Right: "SATS/USD: --"
    lv_obj_t *pair_label = lv_label_create(scr);
    lv_label_set_text(pair_label, "BTC / USD");
    lv_obj_set_style_text_color(pair_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(pair_label, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(pair_label, 8, 54);

    s_sats_label = lv_label_create(scr);
    lv_label_set_text(s_sats_label, "-- sats/$");
    lv_obj_set_style_text_color(s_sats_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_sats_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_sats_label, LV_ALIGN_TOP_RIGHT, -8, 54);

    // ==========================================
    // 3. 24h SPARKLINE / CHART (Y: 74, H: 104, W: 200)
    // ==========================================
    // Trend header row
    lv_obj_t *trend_title = lv_label_create(scr);
    lv_label_set_text(trend_title, "24h Trend");
    lv_obj_set_style_text_color(trend_title, lv_color_black(), 0);
    lv_obj_set_style_text_font(trend_title, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(trend_title, 8, 74);

    s_change_label = lv_label_create(scr);
    lv_label_set_text(s_change_label, "+0.0%");
    lv_obj_set_style_text_color(s_change_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_change_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_change_label, LV_ALIGN_TOP_RIGHT, -8, 74);

    // lv_chart
    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, 184, 58);
    lv_obj_set_pos(s_chart, 8, 92);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_div_line_count(s_chart, 2, 3);
    lv_obj_set_style_bg_color(s_chart, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_chart, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_chart, 1, 0);
    lv_obj_set_style_radius(s_chart, 2, 0);
    lv_obj_set_style_pad_all(s_chart, 2, 0);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    // Hide large point dots for clean sparkline
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);

    s_series = lv_chart_add_series(s_chart, lv_color_black(), LV_CHART_AXIS_PRIMARY_Y);

    // Range High / Low footer row
    s_low_label = lv_label_create(scr);
    lv_label_set_text(s_low_label, "L: $ --");
    lv_obj_set_style_text_color(s_low_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_low_label, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(s_low_label, 8, 156);

    s_high_label = lv_label_create(scr);
    lv_label_set_text(s_high_label, "H: $ --");
    lv_obj_set_style_text_color(s_high_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_high_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_high_label, LV_ALIGN_TOP_RIGHT, -8, 156);

    // ==========================================
    // 4. FOOTER (Y: 178, H: 22, W: 200) - Black Background
    // ==========================================
    lv_obj_t *footer = lv_obj_create(scr);
    lv_obj_set_size(footer, 200, 22);
    lv_obj_set_pos(footer, 0, 178);
    lv_obj_set_style_bg_color(footer, lv_color_black(), 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);

    s_status_label = lv_label_create(footer);
    lv_label_set_text(s_status_label, "Refreshed: --:--");
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 6, 0);

    s_batt_label = lv_label_create(footer);
    lv_label_set_text(s_batt_label, LV_SYMBOL_BATTERY_3 " 85%");
    lv_obj_set_style_text_color(s_batt_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_batt_label, LV_ALIGN_RIGHT_MID, -6, 0);
}

static void update_ui_market_data(const btc_market_data_t *data)
{
    char buf[64];

    // 1. Price Formatting: "$ 79,209"
    long p_int = (long)round(data->current_price);
    if (p_int >= 1000) {
        snprintf(buf, sizeof(buf), "$ %ld,%03ld", p_int / 1000, p_int % 1000);
    } else {
        snprintf(buf, sizeof(buf), "$ %.2f", data->current_price);
    }
    lv_label_set_text(s_price_label, buf);

    // 2. Sats / USD calculation: 100,000,000 / price
    if (data->current_price > 0.0) {
        long sats = (long)round(100000000.0 / data->current_price);
        if (sats >= 1000) {
            snprintf(buf, sizeof(buf), "%ld,%03ld sats/$", sats / 1000, sats % 1000);
        } else {
            snprintf(buf, sizeof(buf), "%ld sats/$", sats);
        }
        lv_label_set_text(s_sats_label, buf);
        lv_obj_align(s_sats_label, LV_ALIGN_TOP_RIGHT, -8, 54);
    }

    // 3. 24h Real Chart Line
    int32_t min_p = (int32_t)floor(data->low_24h);
    int32_t max_p = (int32_t)ceil(data->high_24h);
    int32_t margin = (max_p - min_p) / 10;
    if (margin < 50) margin = 50;

    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, min_p - margin, max_p + margin);
    lv_chart_set_point_count(s_chart, data->count);

    for (int i = 0; i < data->count; i++) {
        lv_chart_set_value_by_id(s_chart, s_series, i, data->history[i]);
    }
    lv_chart_refresh(s_chart);

    // 4. Change %
    if (data->change_24h >= 0) {
        snprintf(buf, sizeof(buf), "+%.1f%% " LV_SYMBOL_UP, data->change_24h);
    } else {
        snprintf(buf, sizeof(buf), "%.1f%% " LV_SYMBOL_DOWN, data->change_24h);
    }
    lv_label_set_text(s_change_label, buf);
    lv_obj_align(s_change_label, LV_ALIGN_TOP_RIGHT, -8, 74);

    // 5. High / Low labels
    snprintf(buf, sizeof(buf), "L: $ %ld,%03ld", (long)min_p / 1000, (long)min_p % 1000);
    lv_label_set_text(s_low_label, buf);

    snprintf(buf, sizeof(buf), "H: $ %ld,%03ld", (long)max_p / 1000, (long)max_p % 1000);
    lv_label_set_text(s_high_label, buf);
    lv_obj_align(s_high_label, LV_ALIGN_TOP_RIGHT, -8, 156);

    // 6. Footer Timestamp (explicit UTC)
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    snprintf(buf, sizeof(buf), "%02d:%02d UTC", tm_utc.tm_hour, tm_utc.tm_min);
    lv_label_set_text(s_status_label, buf);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 6, 0);

    // 7. Footer Battery Status (Real ADC voltage measurement)
    float vbat = read_battery_voltage();
    char batt_buf[32];
    if (vbat >= 4.30f) {
        snprintf(batt_buf, sizeof(batt_buf), "%s USB (%.2fV)", LV_SYMBOL_USB, vbat);
    } else if (vbat >= 3.0f) {
        int pct = battery_voltage_to_percentage(vbat);
        snprintf(batt_buf, sizeof(batt_buf), "%s %d%% (%.2fV)", get_battery_symbol(pct, vbat), pct, vbat);
    } else {
        snprintf(batt_buf, sizeof(batt_buf), "%s --", LV_SYMBOL_BATTERY_EMPTY);
    }
    lv_label_set_text(s_batt_label, batt_buf);
    lv_obj_align(s_batt_label, LV_ALIGN_RIGHT_MID, -6, 0);
}

static void update_ui_status(const char *msg)
{
    lv_label_set_text(s_status_label, msg);
}

static void push_to_epaper(void)
{
    // Let LVGL render the latest state
    vTaskDelay(pdMS_TO_TICKS(100));

    // Power up ePaper, resume SPI, re-send init commands (the ePaper lost
    // state when power was cut after the previous refresh).
    epd_power_on();
    vTaskDelay(pdMS_TO_TICKS(50));
    epaper_port_init();
    vTaskDelay(pdMS_TO_TICKS(50));
    epaper_port_clear(EPD_1IN54G_WHITE);
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Push the LVGL-rendered frame
    lvgl_epaper_flush_to_epaper();

    vTaskDelay(pdMS_TO_TICKS(3000));
    epaper_port_sleep();
    epd_power_off();
}

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering Deep Sleep for %d min...", SLEEP_DURATION_SEC / 60);

    // 1. Shutdown Wi-Fi cleanly
    esp_wifi_disconnect();
    esp_wifi_stop();

    // 2. Ensure EPD power is OFF
    epaper_port_sleep();
    epd_power_off();

    // 3. Keep battery latch GPIO held during deep sleep
    gpio_hold_en(VBAT_PWR_PIN);
    gpio_deep_sleep_hold_en();

    // 4. Configure 5-minute timer wakeup
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_DURATION_SEC * 1000000ULL);

    // 5. Configure BOOT button (GPIO 0) wakeup for instant manual refresh
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

    // 6. Enter Deep Sleep
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}

static void epd_refresh_task(void *arg)
{
    s_boot_count++;
    ESP_LOGI(TAG, "Boot count (RTC): %lu", (unsigned long)s_boot_count);

    EventGroupHandle_t eg = espwifi_GetEventGroup();
    EventBits_t bits = xEventGroupWaitBits(eg,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi failed");
        lv_label_set_text(s_status_label, "WiFi failed");
        push_to_epaper();
        enter_deep_sleep();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "WiFi connected, waiting for internet route / SNTP sync...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.cloudflare.com");
    esp_sntp_init();

    // Wait until internet connectivity is verified by an NTP reply (or max 20s)
    int sntp_retry = 0;
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++sntp_retry <= 20) {
        ESP_LOGI(TAG, "Waiting for network route & clock sync... (%d/20)", sntp_retry);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        time_t now = time(NULL);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char strftime_buf[64];
        strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        ESP_LOGI(TAG, "Internet route verified & time synced: %s", strftime_buf);
    } else {
        ESP_LOGW(TAG, "SNTP sync wait elapsed, proceeding");
        seed_time();
    }

    ESP_LOGI(TAG, "fetching 24h candles from Coinbase...");
    btc_market_data_t market_data = { 0 };
    bool ok = false;

    for (int i = 0; i < 3 && !ok; i++) {
        if (i > 0) {
            ESP_LOGW(TAG, "retry %d/3", i + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        ok = fetch_btc_market_data(&market_data);
    }

    if (ok) {
        ESP_LOGI(TAG, "BTC = %.2f (24h: %.2f%%, High: %.2f, Low: %.2f, candles: %d)",
                 market_data.current_price, market_data.change_24h,
                 market_data.high_24h, market_data.low_24h, market_data.count);
        update_ui_market_data(&market_data);
    } else {
        update_ui_status("Network error");
    }

    push_to_epaper();

    // Refresh completed -> go to Deep Sleep for 5 minutes
    enter_deep_sleep();
    vTaskDelete(NULL);
}

void user_app_init(void)
{
    ESP_LOGI(TAG, "BTC ticker (LVGL) starting");

    // Enable detailed debug logs
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_VERBOSE);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("mbedtls", ESP_LOG_VERBOSE);
    esp_log_level_set("BTC", ESP_LOG_VERBOSE);
    esp_log_level_set("DIAG", ESP_LOG_VERBOSE);

    board_power_init();

    lvgl_epaper_init();
    build_ui();

    espwifi_Init();

    xTaskCreate(epd_refresh_task, "btc", 8192, NULL, 5, NULL);
}
