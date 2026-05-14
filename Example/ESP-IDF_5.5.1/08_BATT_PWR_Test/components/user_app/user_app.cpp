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

void led_init();
void user_task_init(void);

board_power_bsp_t board_div(EPD_PWR_PIN,Audio_PWR_PIN,VBAT_PWR_PIN);
static bool is_vabtflag = false;

void user_app_init(void)
{
    board_div.VBAT_POWER_ON();
    //board_div.POWEER_EPD_ON();
    //board_div.POWEER_Audio_ON();
    led_init();
    user_button_init();
    user_task_init();
}

void led_init()
{
    gpio_config_t gpio_conf = {};
  	gpio_conf.intr_type = GPIO_INTR_DISABLE;
  	gpio_conf.mode = GPIO_MODE_OUTPUT;
  	gpio_conf.pin_bit_mask = 0x1ULL<<3;
  	gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  	gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;

  	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    gpio_set_level((gpio_num_t)3,0);
}

void vbat_scanStatus(void)
{
    if(gpio_get_level(PWR_BUTTON_PIN) && (!is_vabtflag))
    {
        is_vabtflag = true;
    }
}

void button_power_task(void *arg)
{
    for(;;)
    {
        EventBits_t even = xEventGroupWaitBits(pwr_groups,set_bit_all,pdTRUE,pdFALSE,pdMS_TO_TICKS(2 * 1000));
        if(get_bit_button(even,2))
        {
            if(is_vabtflag)
            {
                is_vabtflag = false;
                vTaskDelay(pdMS_TO_TICKS(200));
                board_div.VBAT_POWER_OFF();
            }
        }
        else if(get_bit_button(even,3))
        {
            if(!is_vabtflag)
            {
                is_vabtflag = true;
            }
        }
    }
}

void user_task_init(void)
{
    vbat_scanStatus();
    xTaskCreatePinnedToCore(button_power_task, "button_power_task", 4 * 1024, NULL, 4, NULL,1);
}