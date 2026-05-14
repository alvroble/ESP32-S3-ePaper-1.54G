#include "button_bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "led_bsp.h"
#include "server_bsp.h"
#include "user_app.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include <stdio.h>

#include "GUI_BMPfile.h"
#include "GUI_Paint.h"
#include "epaper_port.h"

#include "driver/rtc_io.h"

#define ext_wakeup_pin_1 GPIO_NUM_0

static const char *TAG = "NET";

static EventGroupHandle_t sleep_group;
static uint8_t           *epd_blackImage = NULL;
static uint32_t           Imagesize;

static void Network_user_Task(void *arg) {
    Imagesize =((EXAMPLE_LCD_WIDTH % 4 == 0) ? (EXAMPLE_LCD_WIDTH / 4) : (EXAMPLE_LCD_WIDTH / 4 + 1)) * EXAMPLE_LCD_HEIGHT;
    epd_blackImage = (uint8_t *) heap_caps_malloc(Imagesize * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(epd_blackImage);
    
    Paint_NewImage(epd_blackImage, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, 0, EPD_1IN54G_WHITE);
    Paint_SetScale(4);
    Paint_SelectImage(epd_blackImage);
    Paint_SetRotate(0);

    for (;;) {
        EventBits_t even =
            xEventGroupWaitBits(server_groups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        if (get_bit_button(even, 0)) 
        {
        } else if (get_bit_button(even, 1)) {

        } else if (get_bit_button(even, 2)) 
        {
            ESP_LOGI(TAG, "show bmp");
            if (pdTRUE == xSemaphoreTake(epaper_gui_semapHandle, 2000)) 
            {
                xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(6));
                Green_led_arg = 1;
                GUI_ReadBmp_RGB_4Color("/sdcard/02_sys_ap_img/user_send.bmp", 0, 0);
                epaper_port_display(epd_blackImage);    
                xSemaphoreGive(epaper_gui_semapHandle); 
                Green_led_arg = 0; 
                led_set(LED_PIN_Green, LED_ON);
            }
        } else if (get_bit_button(even, 5)) 
        {
            esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO); 
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);      
            const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
            ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
                ext_wakeup_pin_1_mask, ESP_EXT1_WAKEUP_ANY_LOW)); 
            ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(ext_wakeup_pin_1));
            ESP_ERROR_CHECK(rtc_gpio_pullup_en(ext_wakeup_pin_1));
            esp_sleep_enable_timer_wakeup(30 * 1000 * 1000); 
            ESP_LOGI(TAG, "30s wake-up timer has been turned on");
            set_espWifi_sleep();                             
            vTaskDelay(pdMS_TO_TICKS(500)); 
            led_set(LED_PIN_Green, LED_OFF);
            gpio_hold_en(GPIO_NUM_17);
            gpio_deep_sleep_hold_en();                       
            esp_deep_sleep_start();                          
        } else if (get_bit_button(even, 4))                  
        {
            xEventGroupClearBits(sleep_group, rset_bit_data(0)); 
            xEventGroupSetBits(sleep_group, set_bit_button(1));  
        }
    }
}

static void Network_sleep_Task(void *arg) {
    size_t time = 0;

    for (;;) {
        EventBits_t even =
            xEventGroupWaitBits(sleep_group, set_bit_all, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if (get_bit_button(even, 0)) 
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            time++;

            if (time == 60) {
                esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO); 
                esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);      
                const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
                ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
                    ext_wakeup_pin_1_mask, ESP_EXT1_WAKEUP_ANY_LOW)); 
                ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(ext_wakeup_pin_1));
                ESP_ERROR_CHECK(rtc_gpio_pullup_en(ext_wakeup_pin_1));
                esp_sleep_enable_timer_wakeup(30 * 1000 * 1000);
                set_espWifi_sleep();                             
                vTaskDelay(pdMS_TO_TICKS(500));     
                led_set(LED_PIN_Green, LED_OFF);
                gpio_hold_en(GPIO_NUM_17);
                gpio_deep_sleep_hold_en();                  
                esp_deep_sleep_start();                          
            }
        } else if (get_bit_button(even, 1)) 
        {
            time = 0;
            xEventGroupClearBits(sleep_group, rset_bit_data(1)); 
        }
    }
}

static void get_wakeup_gpio(void) {
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    ESP_LOGI(TAG, "------------------------");
    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Wake source: Timer wake");
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "Wake source: Boot wake");
    } else {
        ESP_LOGI(TAG, "Wake source: Power on/Restart wake");
    }
    ESP_LOGI(TAG, "------------------------");

    gpio_hold_dis(GPIO_NUM_17);

    gpio_config_t gpio17_conf = {
        .pin_bit_mask = 1ULL << GPIO_NUM_17,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&gpio17_conf);
    gpio_set_level(GPIO_NUM_17, 1);
    if (ESP_SLEEP_WAKEUP_EXT1 == wakeup_reason) {
        uint64_t wakeup_pins = esp_sleep_get_ext1_wakeup_status();
        if (wakeup_pins == 0)
            return;
        if (wakeup_pins & (1ULL << ext_wakeup_pin_1)) {
            //xEventGroupClearBits(sleep_group, rset_bit_data(0)); 
            led_set(LED_PIN_Green, LED_ON);
        }
    } else if (ESP_SLEEP_WAKEUP_TIMER == wakeup_reason) {
        led_set(LED_PIN_Green, LED_ON);
    }
}

static void pwr_button_user_Task(void *arg) {
    for (;;) {
        EventBits_t even =
            xEventGroupWaitBits(boot_groups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        if (get_bit_button(even, 0)) 
        {
            esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO); 
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);      
            const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
            ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
                ext_wakeup_pin_1_mask, ESP_EXT1_WAKEUP_ANY_LOW)); 
            ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(ext_wakeup_pin_1));
            ESP_ERROR_CHECK(rtc_gpio_pullup_en(ext_wakeup_pin_1));
            esp_sleep_enable_timer_wakeup(30 * 1000 * 1000);
            set_espWifi_sleep();     
            vTaskDelay(pdMS_TO_TICKS(500)); 
            led_set(LED_PIN_Green, LED_OFF);
            gpio_hold_en(GPIO_NUM_17);
            gpio_deep_sleep_hold_en();   
            esp_deep_sleep_start();  
        }
    }
}

void User_Network_mode_app_init(void) {
    sleep_group = xEventGroupCreate();
    xEventGroupSetBits(sleep_group, set_bit_button(0)); 
    Network_wifi_ap_init();                             
    http_server_init();                                 
    xTaskCreate(Network_user_Task, "Network_user_Task", 6 * 1024, NULL, 2, NULL);
    xTaskCreate(Network_sleep_Task, "Network_sleep_Task", 5 * 1024, NULL, 2, NULL);
    xTaskCreate(pwr_button_user_Task, "pwr_button_user_Task", 5 * 1024, NULL, 2, NULL);
    get_wakeup_gpio(); 
}