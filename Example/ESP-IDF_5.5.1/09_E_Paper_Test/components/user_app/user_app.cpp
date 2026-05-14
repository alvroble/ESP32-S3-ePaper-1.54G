#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "user_app.h"
#include "driver/gpio.h"
#include "user_config.h"
#include "board_power_bsp.h"
#include "esp_log.h"
#include "esp_err.h"

#include "driver/rtc_io.h"
#include "esp_sleep.h"

#include "button_bsp.h"
#include "led_bsp.h"
#include "GUI_Paint.h"

#include "ImageData.h"

#define TAG "EPD"

#define Power_on gpio_set_level(GPIO_NUM_6, 0);  
#define Power_off gpio_set_level(GPIO_NUM_6, 1);  

void epaper_power_up(void) {
    gpio_config_t gpio6_conf = {};
    gpio6_conf.intr_type = GPIO_INTR_DISABLE;
    gpio6_conf.mode = GPIO_MODE_OUTPUT;       
    gpio6_conf.pin_bit_mask = (0x1ULL << GPIO_NUM_6);
    gpio6_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; 
    gpio6_conf.pull_up_en = GPIO_PULLUP_ENABLE;     
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio6_conf));
    Power_on;                  
    vTaskDelay(pdMS_TO_TICKS(10));                    
}

void user_app_init(void)
{
    ESP_LOGI(TAG,"e-Paper Init and Clear...");
    epaper_power_up();
    epaper_port_init();
    epaper_port_clear(EPD_1IN54G_WHITE);
    vTaskDelay(pdMS_TO_TICKS(2000));

    epaper_port_display(Image4color);
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint32_t Imagesize = ((EXAMPLE_LCD_WIDTH % 4 == 0) ? (EXAMPLE_LCD_WIDTH / 4) : (EXAMPLE_LCD_WIDTH / 4 + 1)) * EXAMPLE_LCD_HEIGHT;;    
    uint8_t *epd_blackImage = (uint8_t *) heap_caps_malloc(Imagesize * sizeof(uint8_t), MALLOC_CAP_SPIRAM); 
    assert(epd_blackImage);

    Paint_NewImage(epd_blackImage, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, 0, EPD_1IN54G_WHITE);
    Paint_SetScale(4);

    ESP_LOGI(TAG,"show Image...");
    Paint_SelectImage(epd_blackImage); 
    Paint_Clear(EPD_1IN54G_WHITE);    

     // 2.Drawing on the image
    ESP_LOGI(TAG,"Drawing:BlackImage\r\n");
    Paint_DrawPoint(10, 80, EPD_1IN54G_RED, DOT_PIXEL_1X1, DOT_STYLE_DFT);
    Paint_DrawPoint(10, 90, EPD_1IN54G_YELLOW, DOT_PIXEL_2X2, DOT_STYLE_DFT);
    Paint_DrawPoint(10, 100, EPD_1IN54G_BLACK, DOT_PIXEL_3X3, DOT_STYLE_DFT);
    Paint_DrawLine(20, 70, 70, 120, EPD_1IN54G_RED, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawLine(70, 70, 20, 120, EPD_1IN54G_RED, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawRectangle(20, 70, 70, 120, EPD_1IN54G_YELLOW, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawRectangle(80, 70, 130, 120, EPD_1IN54G_YELLOW, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawCircle(45, 95, 20, EPD_1IN54G_BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawCircle(105, 95, 20, EPD_1IN54G_BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawLine(85, 95, 125, 95, EPD_1IN54G_RED, DOT_PIXEL_1X1, LINE_STYLE_DOTTED);
    Paint_DrawLine(105, 75, 105, 115, EPD_1IN54G_YELLOW, DOT_PIXEL_1X1, LINE_STYLE_DOTTED);
    Paint_DrawString_EN(10, 0, "Red,yellow,", &Font12, EPD_1IN54G_RED, EPD_1IN54G_YELLOW);
    Paint_DrawString_EN(10, 20, "white and black", &Font12, EPD_1IN54G_RED, EPD_1IN54G_YELLOW);
    Paint_DrawString_EN(10, 40, "Four color e-Paper", &Font12, EPD_1IN54G_YELLOW, EPD_1IN54G_BLACK);
    Paint_DrawString_CN(10, 150, "微雪电子", &Font24CN, EPD_1IN54G_RED, EPD_1IN54G_WHITE);
    Paint_DrawNum(10, 135, 123456, &Font16, EPD_1IN54G_RED, EPD_1IN54G_WHITE);

    ESP_LOGI(TAG , "EPD_Display\r\n");
    epaper_port_display(epd_blackImage);
    vTaskDelay(pdMS_TO_TICKS(3000));

    epaper_port_clear(EPD_1IN54G_WHITE);
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG , "EPD_Sleep\r\n");
    epaper_port_sleep();

    free(epd_blackImage);
    epd_blackImage = NULL;
    vTaskDelay(pdMS_TO_TICKS(2000));

    Power_off;

}