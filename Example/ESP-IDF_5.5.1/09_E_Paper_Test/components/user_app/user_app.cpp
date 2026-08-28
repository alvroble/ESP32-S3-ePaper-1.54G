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
#include "esp_wifi_bsp.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "user_app.h"

#include "driver/gpio.h"

#include "GUI_Paint.h"
#include "ImageData.h"
#include "fonts.h"

#define TAG "BTC"

#define UPDATE_PERIOD_MS  (10 * 60 * 1000)

// Fixed seed epoch (Jan 2025) so cert validation passes even before NTP syncs.
#define SEED_EPOCH 1735689600

#define BTC_URL "https://api.binance.com/api/v3/ticker/price?symbol=BTCUSDT"
#define HTTP_TIMEOUT_MS  30000
#define HTTP_BUF_SIZE    1024

#define EPD_PWR_PIN GPIO_NUM_6

typedef struct {
    char  data[HTTP_BUF_SIZE];
    int   len;
} http_buf_t;

static uint8_t *s_epd_image = NULL;

static void seed_time(void)
{
    if (time(NULL) < 1700000000) {
        struct timeval tv = { .tv_sec = SEED_EPOCH, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "RTC unset, seeded to %lld", (long long)SEED_EPOCH);
    } else {
        ESP_LOGI(TAG, "RTC OK time=%lld", (long long)time(NULL));
    }
}

static void epd_power_init(void)
{
    gpio_config_t io = {};
    io.intr_type    = GPIO_INTR_DISABLE;
    io.mode         = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << EPD_PWR_PIN);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io));
}

static void epd_power_on(void)
{
    gpio_set_level(EPD_PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void epd_power_off(void)
{
    gpio_set_level(EPD_PWR_PIN, 1);
}

static bool parse_btc_price(const char *body, double *out_price)
{
    const char *key = "\"price\":\"";
    char *p = strstr(body, key);
    if (!p) return false;
    p += strlen(key);
    char *end = strchr(p, '"');
    if (!end) return false;
    char tmp[32];
    int n = end - p;
    if (n <= 0 || n >= (int)sizeof(tmp)) return false;
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    *out_price = strtod(tmp, NULL);
    return (*out_price > 0.0);
}

static bool tcp_blocking_connect(const char *host, uint16_t port, int timeout_ms)
{
    char ip_str[INET_ADDRSTRLEN] = { 0 };
    if (!inet_aton(host, ip_str)) {
        // host is a hostname, resolve
        struct addrinfo hints = { .ai_family = AF_INET };
        struct addrinfo *res = NULL;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            if (res) freeaddrinfo(res);
            return false;
        }
        struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;
        inet_ntop(AF_INET, &a->sin_addr, ip_str, sizeof(ip_str));
        freeaddrinfo(res);
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    inet_pton(AF_INET, ip_str, &dst.sin_addr);

    int err = connect(sock, (struct sockaddr *)&dst, sizeof(dst));
    close(sock);
    return err == 0;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *buf = (http_buf_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (buf && buf->len + evt->data_len < HTTP_BUF_SIZE) {
            memcpy(buf->data + buf->len, evt->data, evt->data_len);
            buf->len += evt->data_len;
            buf->data[buf->len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static bool fetch_btc_price(double *out_price)
{
    http_buf_t buf = { .len = 0 };
    buf.data[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = BTC_URL,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &buf,
        .skip_cert_common_name_check = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200 && buf.len > 0) {
            ok = parse_btc_price(buf.data, out_price);
            if (!ok) ESP_LOGW(TAG, "parse failed: %.*s", buf.len, buf.data);
        } else {
            ESP_LOGW(TAG, "HTTP %d", status);
        }
    } else {
        ESP_LOGW(TAG, "perform failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return ok;
}

static int text_width(const char *s, sFONT *font)
{
    return (int)strlen(s) * font->Width;
}

static void draw_btc_screen(double price)
{
    Paint_SelectImage(s_epd_image);
    Paint_Clear(EPD_1IN54G_WHITE);

    Paint_DrawRectangle(0, 0, EXAMPLE_LCD_WIDTH - 1, 28, EPD_1IN54G_RED,
                        DOT_PIXEL_1X1, DRAW_FILL_FULL);
    const char *title = "BTC / USD";
    int tw = text_width(title, &Font16);
    Paint_DrawString_EN((EXAMPLE_LCD_WIDTH - tw) / 2, 6, title, &Font16,
                        EPD_1IN54G_WHITE, EPD_1IN54G_RED);

    Paint_DrawString_EN(10, 34, "Bitcoin live price", &Font12,
                        EPD_1IN54G_BLACK, EPD_1IN54G_WHITE);

    char price_buf[24];
    snprintf(price_buf, sizeof(price_buf), "%.2f", price);
    char full_price[28];
    snprintf(full_price, sizeof(full_price), "$%s", price_buf);

    sFONT *pf = (strlen(full_price) > 11) ? &Font20 : &Font24;
    int pw = text_width(full_price, pf);
    int px = (EXAMPLE_LCD_WIDTH - pw) / 2;
    Paint_DrawString_EN(px, 60, full_price, pf,
                        EPD_1IN54G_BLACK, EPD_1IN54G_WHITE);

    Paint_DrawLine(10, 110, EXAMPLE_LCD_WIDTH - 10, 110,
                   EPD_1IN54G_BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    Paint_DrawRectangle(0, EXAMPLE_LCD_HEIGHT - 22, EXAMPLE_LCD_WIDTH - 1,
                        EXAMPLE_LCD_HEIGHT - 1, EPD_1IN54G_YELLOW,
                        DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(10, EXAMPLE_LCD_HEIGHT - 17,
                        "Refresh every 10 min", &Font12,
                        EPD_1IN54G_BLACK, EPD_1IN54G_YELLOW);

    epaper_port_display(s_epd_image);
}

static void draw_error_screen(const char *msg)
{
    Paint_SelectImage(s_epd_image);
    Paint_Clear(EPD_1IN54G_WHITE);

    Paint_DrawRectangle(0, 0, EXAMPLE_LCD_WIDTH - 1, 28, EPD_1IN54G_RED,
                        DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(10, 6, "BTC / USD", &Font16,
                        EPD_1IN54G_WHITE, EPD_1IN54G_RED);

    Paint_DrawString_EN(10, 70, "Fetch error", &Font24,
                        EPD_1IN54G_RED, EPD_1IN54G_WHITE);
    Paint_DrawString_EN(10, 110, msg, &Font12,
                        EPD_1IN54G_BLACK, EPD_1IN54G_WHITE);

    Paint_DrawRectangle(0, EXAMPLE_LCD_HEIGHT - 22, EXAMPLE_LCD_WIDTH - 1,
                        EXAMPLE_LCD_HEIGHT - 1, EPD_1IN54G_YELLOW,
                        DOT_PIXEL_1X1, DRAW_FILL_FULL);

    epaper_port_display(s_epd_image);
}

static void epd_refresh_task(void *arg)
{
    EventGroupHandle_t eg = espwifi_GetEventGroup();
    EventBits_t bits = xEventGroupWaitBits(eg,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_FAIL_BIT) {
        draw_error_screen("WiFi failed");
        vTaskDelay(pdMS_TO_TICKS(2000));
        epaper_port_sleep();
        epd_power_off();
        vTaskDelete(NULL);
        return;
    }

    bool ever_ok = false;
    bool net_warm = false;

    while (1) {
        seed_time();

        double price = 0.0;
        bool ok = false;

        if (!net_warm) {
            // First time: warm the network path with a plain TCP connect to
            // Binance's IP. Once we get TCP OK, subsequent HTTPS just works.
            ESP_LOGI(TAG, "warming net (plain TCP to api.binance.com:443)...");
            for (int attempt = 0; attempt < 5 && !net_warm; attempt++) {
                if (tcp_blocking_connect("api.binance.com", 443, 8000)) {
                    ESP_LOGI(TAG, "net warm");
                    net_warm = true;
                } else {
                    ESP_LOGW(TAG, "warmup TCP fail (attempt %d/5)", attempt + 1);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            }
        }

        // HTTPS attempt + retry
        ESP_LOGI(TAG, "fetching...");
        ok = fetch_btc_price(&price);
        if (!ok) {
            ESP_LOGW(TAG, "fetch failed, retrying in 5s...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            seed_time();
            ok = fetch_btc_price(&price);
        }

        epd_power_on();
        epaper_port_init();
        vTaskDelay(pdMS_TO_TICKS(2000));
        epaper_port_clear(EPD_1IN54G_WHITE);
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (ok) {
            ESP_LOGI(TAG, "BTC = %.2f USD", price);
            draw_btc_screen(price);
            ever_ok = true;
        } else {
            draw_error_screen("Network/API error");
            net_warm = false;  // mark path cold again, redo warmup next cycle
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
        epaper_port_sleep();
        epd_power_off();

        (void)ever_ok;
        vTaskDelay(pdMS_TO_TICKS(UPDATE_PERIOD_MS));
    }
}

void user_app_init(void)
{
    ESP_LOGI(TAG, "BTC ticker starting");

    s_epd_image = (uint8_t *)heap_caps_malloc(
        (EXAMPLE_LCD_WIDTH / 4) * EXAMPLE_LCD_HEIGHT, MALLOC_CAP_SPIRAM);
    if (!s_epd_image) {
        s_epd_image = (uint8_t *)malloc(
            (EXAMPLE_LCD_WIDTH / 4) * EXAMPLE_LCD_HEIGHT);
    }
    if (!s_epd_image) {
        ESP_LOGE(TAG, "alloc image failed");
        return;
    }

    Paint_NewImage(s_epd_image, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, 0,
                   EPD_1IN54G_WHITE);
    Paint_SetScale(4);

    epd_power_init();
    espwifi_Init();

    xTaskCreate(epd_refresh_task, "btc", 8192, NULL, 5, NULL);
}
