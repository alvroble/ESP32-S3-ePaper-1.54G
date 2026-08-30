#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "lvgl.h"

#include "lvgl_epaper_port.h"
#include "epaper_port.h"

#define TAG "LVGL"
#define EPD_W EXAMPLE_LCD_WIDTH
#define EPD_H EXAMPLE_LCD_HEIGHT

static lv_display_t *s_disp = NULL;
static uint16_t *s_fb_raw = NULL;
static uint16_t *s_fb = NULL;       // 16-byte aligned, RGB565, 200x200
static uint8_t  *s_epd_buf = NULL;  // 200/4 * 200 = 10000 bytes, in SPIRAM
static SemaphoreHandle_t s_mutex = NULL;

static inline uint8_t rgb565_to_eink(uint16_t px)
{
    // Convert RGB565 → closest of the 4 ePaper colors.
    // Color palette of the 1.54G ePaper:
    //   0x0 = black, 0x1 = white, 0x2 = yellow, 0x3 = red.
    uint8_t r = ((px >> 11) & 0x1F);
    uint8_t g = ((px >> 5)  & 0x3F);
    uint8_t b = ( px        & 0x1F);
    // Expand to 8-bit
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);

    // Standard perception luminance (0..255)
    int lum = (299 * (int)r + 587 * (int)g + 114 * (int)b) / 1000;

    // Check color saturation (chroma)
    int max_c = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int min_c = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int sat = max_c - min_c;

    // Grayscale / font anti-aliasing edges (low saturation):
    // Use crisp binary threshold to make letters sharp without color fringing or blur
    if (sat < 45) {
        return (lum < 160) ? 0x0 : 0x1; // Black if < 160, White otherwise
    }

    // High chroma colors (Yellow / Red)
    if (r > 150 && g > 130 && b < 100) return 0x2; // Yellow
    if (r > 140 && g < 110 && b < 110) return 0x3; // Red

    return (lum < 160) ? 0x0 : 0x1;
}

// Pack 4 ePaper pixels (2 bits each) into 1 byte for the ePaper buffer.
static inline void pack_pixels(uint8_t *out, const uint8_t *in4)
{
    *out = (in4[0] << 6) | (in4[1] << 4) | (in4[2] << 2) | in4[3];
}

// Convert the LVGL RGB565 framebuffer (200x200) into the ePaper 4-color buffer.
static void convert_fb_to_epd(void)
{
    int row_bytes = (EPD_W % 4 == 0) ? (EPD_W / 4) : (EPD_W / 4 + 1);
    for (int y = 0; y < EPD_H; y++) {
        uint8_t pixels4[4];
        for (int x = 0; x < EPD_W; x += 4) {
            for (int k = 0; k < 4; k++) {
                pixels4[k] = rgb565_to_eink(s_fb[y * EPD_W + (x + k)]);
            }
            pack_pixels(&s_epd_buf[y * row_bytes + (x / 4)], pixels4);
        }
    }
}

// LVGL flush callback: copy the RGB565 area from the LVGL internal buffer to
// our shared framebuffer. The actual ePaper refresh happens later, in
// lvgl_epaper_flush_to_epaper().
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    uint16_t *src = (uint16_t *)px_map;
    for (uint32_t y = 0; y < h; y++) {
        memcpy(&s_fb[(area->y1 + y) * EPD_W + area->x1],
               &src[y * w],
               w * sizeof(uint16_t));
    }
    lv_disp_flush_ready(disp);
}

// LVGL tick callback — increments a millisecond counter.
static uint32_t lvgl_tick_get(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_tick_task(void *arg)
{
    while (1) {
        xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
        uint32_t delay = lv_timer_handler();
        xSemaphoreGiveRecursive(s_mutex);
        if (delay > 30) delay = 30;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

void lvgl_epaper_init(void)
{
    ESP_LOGI(TAG, "init");

    // Allocate extra room so we can align the buffer to a 64-byte boundary
    // (LV_DRAW_BUF_ALIGN / cache-line). SPIRAM alloc does not guarantee this.
    const size_t fb_bytes = EPD_W * EPD_H * sizeof(uint16_t);
    const size_t alloc_bytes = fb_bytes + 64;
    s_fb_raw = (uint16_t *)heap_caps_malloc(alloc_bytes, MALLOC_CAP_SPIRAM);
    if (!s_fb_raw) s_fb_raw = (uint16_t *)malloc(alloc_bytes);
    if (!s_fb_raw) {
        ESP_LOGE(TAG, "fb alloc failed");
        return;
    }
    uintptr_t aligned = ((uintptr_t)s_fb_raw + 63) & ~(uintptr_t)63;
    s_fb = (uint16_t *)aligned;
    memset(s_fb, 0xFF, fb_bytes);  // white

    int row_bytes = (EPD_W % 4 == 0) ? (EPD_W / 4) : (EPD_W / 4 + 1);
    s_epd_buf = (uint8_t *)heap_caps_malloc(row_bytes * EPD_H,
                                            MALLOC_CAP_SPIRAM);
    if (!s_epd_buf) s_epd_buf = (uint8_t *)malloc(row_bytes * EPD_H);
    if (!s_epd_buf) {
        ESP_LOGE(TAG, "epd buf alloc failed");
        return;
    }

    s_mutex = xSemaphoreCreateRecursiveMutex();

    lv_init();
    lv_tick_set_cb(lvgl_tick_get);

    s_disp = lv_display_create(EPD_W, EPD_H);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    // Single buffer mode: lvgl renders directly into s_fb.
    lv_display_set_buffers(s_disp, s_fb, NULL,
                           EPD_W * EPD_H * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_FULL);

    xTaskCreate(lvgl_tick_task, "lvgl", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ready");
}

lv_display_t *lvgl_epaper_display(void)
{
    return s_disp;
}

void lvgl_epaper_tick(void)
{
    // No-op: handled by lvgl_tick_task.
}

void lvgl_epaper_flush_to_epaper(void)
{
    if (!s_epd_buf || !s_fb) return;

    // LVGL mutex
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    // force a render
    lv_timer_handler();
    xSemaphoreGiveRecursive(s_mutex);

    // Convert to 4-color ePaper buffer
    convert_fb_to_epd();

    // Push to ePaper
    epaper_port_display(s_epd_buf);
}
