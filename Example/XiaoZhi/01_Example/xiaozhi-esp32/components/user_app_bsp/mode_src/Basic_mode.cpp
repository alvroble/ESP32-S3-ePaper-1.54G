#include "button_bsp.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "json_data.h"
#include "led_bsp.h"
#include "sdcard_bsp.h"
#include "user_app.h"
#include <stdio.h>

#include "GUI_BMPfile.h"
#include "GUI_Paint.h"
#include "epaper_port.h"

#define ext_wakeup_pin_1 GPIO_NUM_0  //boot
#define ext_wakeup_pin_3 GPIO_NUM_18 //PWR

static uint8_t *epd_blackImage = NULL;
static uint32_t Imagesize;

// The address for saving image files
char img_path[256];

static RTC_DATA_ATTR int basic_rtc_set_time = 13 * 60;

static uint8_t           Basic_sleep_arg = 0;
static SemaphoreHandle_t sleep_Semp;

static void boot_button_user_Task(void *arg)
{
    Imagesize = ((EXAMPLE_LCD_WIDTH % 4 == 0) ? (EXAMPLE_LCD_WIDTH / 4) : (EXAMPLE_LCD_WIDTH / 4 + 1)) * EXAMPLE_LCD_HEIGHT;
    epd_blackImage = (uint8_t *)heap_caps_malloc(Imagesize, MALLOC_CAP_SPIRAM);
    assert(epd_blackImage);

    Paint_NewImage(epd_blackImage, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, 0, EPD_1IN54G_WHITE);
    Paint_SetScale(4);
    Paint_SelectImage(epd_blackImage);
    xEventGroupClearBits(boot_groups, set_bit_all);
    for (;;)
    {
        EventBits_t even = xEventGroupWaitBits(boot_groups, set_bit_button(0), pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));

        if (get_bit_button(even, 0))
        {
            xEventGroupClearBits(boot_groups, set_bit_button(0));
            ESP_LOGI("PWR", "Single click → Refresh the image");

            if (pdTRUE == xSemaphoreTake(epaper_gui_semapHandle, 2000))
            {
                xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(6));
                Green_led_arg = 1;

                get_img_name_by_index(img_path, sizeof(img_path));
                ESP_LOGI("TAG", "Image path: %s", img_path);
                GUI_ReadBmp_RGB_4Color(img_path,0,0);
                epaper_port_display(epd_blackImage);

                xSemaphoreGive(epaper_gui_semapHandle);
                Green_led_arg = 0;

                xSemaphoreGive(sleep_Semp); 
                Basic_sleep_arg = 1;
            }
        }
    }
}

static void default_sleep_user_Task(void *arg) {
    uint8_t *sleep_arg = (uint8_t *) arg;
    for (;;) {
        if (pdTRUE == xSemaphoreTake(sleep_Semp, portMAX_DELAY)) {
            if (*sleep_arg == 1) {
                esp_sleep_pd_config(
                    ESP_PD_DOMAIN_MAX,
                    ESP_PD_OPTION_AUTO);   
                esp_sleep_disable_wakeup_source(
                    ESP_SLEEP_WAKEUP_ALL); 
                const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
                const uint64_t ext_wakeup_pin_3_mask = 1ULL << ext_wakeup_pin_3;
                ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
                    ext_wakeup_pin_1_mask | ext_wakeup_pin_3_mask,
                    ESP_EXT1_WAKEUP_ANY_LOW)); 
                ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(ext_wakeup_pin_3));
                ESP_ERROR_CHECK(rtc_gpio_pullup_en(ext_wakeup_pin_3));
                esp_sleep_enable_timer_wakeup(basic_rtc_set_time * 1000 * 1000);
                //esp sleep
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_hold_en(GPIO_NUM_17); 
                gpio_deep_sleep_hold_en();
                esp_deep_sleep_start();  
            }
        }
    }
}

static void get_wakeup_gpio(void) {
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    //Unlock and reconfigure the GPIO17
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
        if (wakeup_pins == 0){
            return;
        }
        if (wakeup_pins & (1ULL << ext_wakeup_pin_1)) {
            //Refresh image
            xEventGroupSetBits(boot_groups, set_bit_button(0)); 
        } else if (wakeup_pins & (1ULL << ext_wakeup_pin_3)) {
            return;
        }
    } else if (ESP_SLEEP_WAKEUP_TIMER == wakeup_reason) {
        xEventGroupSetBits(boot_groups, set_bit_button(0)); 
    }else
    {
        xEventGroupSetBits(boot_groups, set_bit_button(0));
    }
}

void User_Basic_mode_app_init(void)
{
    ESP_LOGI("Basic", "Basic_mode_app init");
    if (boot_groups != NULL) {
        xEventGroupClearBits(boot_groups, set_bit_all);
    }
    sleep_Semp = xSemaphoreCreateBinary();

    ai_model_t *ai_model_data = NULL;
    if ((13 * 60) == basic_rtc_set_time)
    {
        ai_model_data = json_sdcard_txt_aimodel();
        if (ai_model_data != NULL)
        {
            basic_rtc_set_time = ai_model_data->time;
            ESP_LOGI("TIMER", "time: %d", basic_rtc_set_time);
        }
    }
    if (ai_model_data != NULL)
    {
        free(ai_model_data);
    }
    ESP_LOGI("Basic", "Create task");
    xTaskCreate(boot_button_user_Task,"boot_button_user_Task",  6*1024, NULL, 3, NULL);
    xTaskCreate(default_sleep_user_Task, "default_sleep_user_Task", 4 * 1024, &Basic_sleep_arg, 3, NULL); 
    get_wakeup_gpio();
}