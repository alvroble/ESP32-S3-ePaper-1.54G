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

#include "cJSON.h"
#include "lvgl.h"
#include "lvgl_epaper_port.h"

#define TAG "BTC"

#define SLEEP_DURATION_SEC  (5 * 60)  // 5 minutes Deep Sleep
#define SEED_EPOCH          1735689600

#define BTC_URL "https://api.exchange.coinbase.com/products/BTC-USD/candles?granularity=3600"
#define COINGECKO_URL "https://api.coingecko.com/api/v3/coins/bitcoin/market_chart?vs_currency=usd&days=1"
#define HTTP_TIMEOUT_MS  20000
// Coinbase candles default returns ~21 KB (300 hourly candles); CoinGecko
// market_chart for days=1 returns ~30 KB (5-minute granularity).
// 32 KB covers both with margin. Buffer is allocated from PSRAM to avoid
// eating internal RAM.
#define HTTP_BUF_SIZE    32768
#define SNTP_MAX_RETRIES 5       // reduced from 20: many networks block UDP/123

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

// Cache of the last successful market data fetch. Survives deep sleep
// (RTC slow memory); lost on power cycle. Used to keep the UI useful
// when both Coinbase and CoinGecko are unreachable on a given cycle.
RTC_DATA_ATTR static btc_market_data_t s_last_good = { 0 };
RTC_DATA_ATTR static time_t s_last_success = 0;
#define CACHE_MIN_CANDLES 12  // minimum candles required for cache to be usable

// Provider abstraction: each price source has its own URL and JSON schema,
// so each carries its own parser. Tried in order; first success wins.
typedef bool (*market_parser_fn)(const char *body, btc_market_data_t *out);

typedef struct {
    const char       *url;
    market_parser_fn  parse;
    const char       *name;
} price_provider_t;

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
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGW(TAG, "JSON parse failed near: %.32s", err ? err : "(null)");
        return false;
    }

    if (!cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "expected JSON array at root, got type=%d", root->type);
        cJSON_Delete(root);
        return false;
    }

    double latest_close = 0.0;
    double oldest_open = 0.0;
    double max_h = 0.0;
    double min_l = 1e9;
    int count = 0;
    double temp_closes[24];

    cJSON *candle = NULL;
    cJSON_ArrayForEach(candle, root) {
        if (count >= 24) break;

        // Schema: [ time, low, high, open, close, volume ]
        if (!cJSON_IsArray(candle) || cJSON_GetArraySize(candle) < 5) break;

        cJSON *low_item   = cJSON_GetArrayItem(candle, 1);
        cJSON *high_item  = cJSON_GetArrayItem(candle, 2);
        cJSON *open_item  = cJSON_GetArrayItem(candle, 3);
        cJSON *close_item = cJSON_GetArrayItem(candle, 4);

        if (!cJSON_IsNumber(low_item)  || !cJSON_IsNumber(high_item) ||
            !cJSON_IsNumber(open_item) || !cJSON_IsNumber(close_item)) break;

        double low   = low_item->valuedouble;
        double high  = high_item->valuedouble;
        double open  = open_item->valuedouble;
        double close = close_item->valuedouble;

        // Sanity check: discard candles with non-positive prices.
        if (low <= 0 || high <= 0 || close <= 0) break;

        // Coinbase returns candles newest-first: first candle = most recent close.
        if (count == 0) latest_close = close;
        oldest_open = open;

        if (high > max_h) max_h = high;
        if (low  < min_l) min_l = low;

        temp_closes[count] = close;
        count++;
    }

    cJSON_Delete(root);

    if (count < 12) {
        ESP_LOGW(TAG, "only %d valid candles (need >= 12)", count);
        return false;
    }

    out->current_price = latest_close;
    out->high_24h = max_h;
    out->low_24h = min_l;
    if (oldest_open > 0) {
        out->change_24h = ((latest_close - oldest_open) / oldest_open) * 100.0;
    } else {
        out->change_24h = 0.0;
    }
    out->count = count;

    // Arrange chronological: index 0 is oldest (left), index count-1 is newest (right).
    for (int i = 0; i < count; i++) {
        out->history[i] = (int32_t)round(temp_closes[count - 1 - i]);
    }

    return true;
}

// Parse CoinGecko /coins/{id}/market_chart response.
// Schema: { "prices": [[ts_ms, price_usd], ...], "market_caps": [...], "total_volumes": [...] }
// CoinGecko returns hourly points for days=1 (typically 25 points: current + 24 history).
// Already in chronological order (oldest first), so no reversal needed.
static bool parse_coingecko_market(const char *body, btc_market_data_t *out)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGW(TAG, "CoinGecko JSON parse failed near: %.32s", err ? err : "(null)");
        return false;
    }

    cJSON *prices = cJSON_GetObjectItem(root, "prices");
    if (!cJSON_IsArray(prices)) {
        ESP_LOGW(TAG, "CoinGecko: 'prices' missing or not an array (type=%d)",
                 prices ? prices->type : -1);
        cJSON_Delete(root);
        return false;
    }

    int total = cJSON_GetArraySize(prices);
    if (total < 12) {
        ESP_LOGW(TAG, "CoinGecko: only %d prices (need >= 12)", total);
        cJSON_Delete(root);
        return false;
    }

    // Take the most recent 24 points (CoinGecko often returns 25 incl. "now")
    int start = total > 24 ? total - 24 : 0;

    double latest_close = 0.0;
    double oldest_open = 0.0;
    double max_h = 0.0;
    double min_l = 1e9;
    int count = 0;
    double temp_closes[24];

    for (int i = start; i < total && count < 24; i++) {
        cJSON *point = cJSON_GetArrayItem(prices, i);
        if (!cJSON_IsArray(point) || cJSON_GetArraySize(point) < 2) continue;

        cJSON *price = cJSON_GetArrayItem(point, 1);
        if (!cJSON_IsNumber(price)) continue;

        double p = price->valuedouble;
        if (p <= 0) continue;

        if (count == 0) latest_close = p;  // last point in window = most recent
        oldest_open = p;                    // first point in window = oldest in window
        if (p > max_h) max_h = p;
        if (p < min_l) min_l = p;

        temp_closes[count] = p;
        count++;
    }

    cJSON_Delete(root);

    if (count < 12) {
        ESP_LOGW(TAG, "CoinGecko: only %d valid prices after filter", count);
        return false;
    }

    out->current_price = latest_close;
    out->high_24h = max_h;
    out->low_24h = min_l;
    if (oldest_open > 0) {
        out->change_24h = ((latest_close - oldest_open) / oldest_open) * 100.0;
    } else {
        out->change_24h = 0.0;
    }
    out->count = count;

    // Oldest-first, no reversal.
    for (int i = 0; i < count; i++) {
        out->history[i] = (int32_t)round(temp_closes[i]);
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

// Provider chain: tried in order, first success wins. Different CDNs/IPs
// give us resilience against any single endpoint being blocked or rate-limited.
static const price_provider_t s_providers[] = {
    {
        .url   = BTC_URL,                       // Cloudflare CDN
        .parse = parse_coinbase_candles,
        .name  = "coinbase",
    },
    {
        .url   = COINGECKO_URL,                 // Fastly/Google CDN
        .parse = parse_coingecko_market,
        .name  = "coingecko",
    },
};
#define PROVIDER_COUNT (sizeof(s_providers) / sizeof(s_providers[0]))

static bool fetch_from_provider(const price_provider_t *provider, btc_market_data_t *out)
{
    // Prefer PSRAM for the 32 KB response buffer: keeps internal RAM free for
    // Wi-Fi/LVGL. Fall back to internal heap if PSRAM is not initialized.
    http_buf_t *buf = (http_buf_t *)heap_caps_malloc(sizeof(http_buf_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (http_buf_t *)malloc(sizeof(http_buf_t));
    if (!buf) return false;
    buf->len = 0;
    buf->data[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = provider->url,
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
            ok = provider->parse(buf->data, out);
            if (!ok) {
                ESP_LOGW(TAG, "[%s] parse failed (HTTP %d, %d bytes)", provider->name, s, buf->len);
            }
        } else {
            ESP_LOGW(TAG, "[%s] HTTP status %d, len %d", provider->name, s, buf->len);
        }
    } else {
        ESP_LOGW(TAG, "[%s] fetch: %s", provider->name, esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
    free(buf);
    return ok;
}

static bool fetch_btc_market_data(btc_market_data_t *out)
{
    btc_market_data_t tmp = { 0 };

    for (int i = 0; i < (int)PROVIDER_COUNT; i++) {
        if (i > 0) {
            ESP_LOGW(TAG, "fallback to %s (%d/%d)",
                     s_providers[i].name, i + 1, (int)PROVIDER_COUNT);
            vTaskDelay(pdMS_TO_TICKS(2000));   // 2s between providers
        }
        if (fetch_from_provider(&s_providers[i], &tmp)) {
            *out = tmp;
            ESP_LOGI(TAG, "[%s] BTC = %.2f (24h: %.2f%%, H: %.2f, L: %.2f, n=%d)",
                     s_providers[i].name,
                     tmp.current_price, tmp.change_24h,
                     tmp.high_24h, tmp.low_24h, tmp.count);
            return true;
        }
    }

    ESP_LOGE(TAG, "all %d providers failed", (int)PROVIDER_COUNT);
    return false;
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

static void update_ui_market_data(const btc_market_data_t *data, bool is_stale)
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

    // 6. Footer Timestamp in LOCAL time (set via CONFIG_TIMEZONE in
    //    Kconfig.projbuild). When data is stale (served from the RTC cache
    //    because every provider failed), prefix "STALE" and switch the
    //    text to e-paper red so the user immediately sees the data is
    //    older than the current cycle.
    struct tm tm_data;
    time_t time_to_show = is_stale ? s_last_success : time(NULL);
    localtime_r(&time_to_show, &tm_data);

    if (is_stale) {
        snprintf(buf, sizeof(buf), "STALE %02d:%02d",
                 tm_data.tm_hour, tm_data.tm_min);
        // Pure red in RGB565 -> e-paper red (saturation 255, well above the
        // 45 threshold in the conversion algorithm).
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFF0000), 0);
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d",
                 tm_data.tm_hour, tm_data.tm_min);
        lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    }
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

// Most refresh cycles use the skip-clear path (no white pre-flash, just a
// direct transition to the new image). Every Nth cycle we do a full refresh
// (with white clear) to reset the panel and prevent ghost accumulation.
#define FULL_REFRESH_INTERVAL 10
RTC_DATA_ATTR static uint8_t s_refresh_count = 0;

static void push_to_epaper(void)
{
    bool do_full_refresh = (s_refresh_count % FULL_REFRESH_INTERVAL) == 0;
    s_refresh_count++;
    ESP_LOGI(TAG, "epaper refresh #%u (%s)", (unsigned)s_refresh_count,
             do_full_refresh ? "full + clear" : "skip-clear");

    // Let LVGL render the latest state
    vTaskDelay(pdMS_TO_TICKS(100));

    // Power up ePaper, resume SPI, re-send init commands (the ePaper lost
    // state when power was cut after the previous refresh).
    epd_power_on();
    vTaskDelay(pdMS_TO_TICKS(50));
    epaper_port_init();
    vTaskDelay(pdMS_TO_TICKS(50));

    if (do_full_refresh) {
        // Full refresh: clear to white then display. Causes the visible
        // white flash but resets the panel to a clean baseline, killing any
        // ghosting that has built up since the last full cycle.
        epaper_port_clear(EPD_1IN54G_WHITE);
        vTaskDelay(pdMS_TO_TICKS(2000));
        lvgl_epaper_flush_to_epaper();
        vTaskDelay(pdMS_TO_TICKS(3000));
    } else {
        // Skip-clear refresh: the LVGL framebuffer overwrites every pixel,
        // so an explicit white clear is unnecessary. Skipping it eliminates
        // the white flash before the image update. The full-refresh waveform
        // still produces a small settling flicker, but it is much less
        // jarring than the flash-then-image transition.
        lvgl_epaper_flush_to_epaper();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    epaper_port_sleep();
    epd_power_off();
}

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering Deep Sleep for %d min...", SLEEP_DURATION_SEC / 60);

    // 1. Shutdown Wi-Fi cleanly (stop + deinit, not just disconnect/stop)
    espwifi_Deinit();

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

    // Wait until internet connectivity is verified by an NTP reply (or max 5s).
    // SNTP wait shortened from 20s -> 5s: many home networks block UDP/123
    // outbound, so a long wait just burns battery before the HTTPS call fails too.
    int sntp_retry = 0;
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++sntp_retry <= SNTP_MAX_RETRIES) {
        ESP_LOGI(TAG, "Waiting for network route & clock sync... (%d/%d)", sntp_retry, SNTP_MAX_RETRIES);
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
        // Successful fetch: persist to RTC cache so the next wake can fall
        // back to it if both providers are down.
        s_last_good = market_data;
        s_last_success = time(NULL);
        update_ui_market_data(&s_last_good, false);
    } else if (s_last_good.count >= CACHE_MIN_CANDLES && s_last_success > 0) {
        // Fetch failed but we have a usable cache from a previous cycle.
        time_t now = time(NULL);
        long age_sec = (now > s_last_success) ? (long)(now - s_last_success) : 0;
        ESP_LOGW(TAG, "all providers failed, using cached data (%ld min old)",
                 age_sec / 60);
        update_ui_market_data(&s_last_good, true);
    } else {
        ESP_LOGE(TAG, "no data available (fetch failed, cache empty)");
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

    // Apply the configured POSIX TZ string BEFORE any call to localtime_r.
    // setenv + tzset make the new zone visible to the C runtime; SNTP still
    // uses UTC internally, so this is purely a display-side concern.
    setenv("TZ", CONFIG_TIMEZONE, 1);
    tzset();
    ESP_LOGI(TAG, "timezone: %s", CONFIG_TIMEZONE);

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
