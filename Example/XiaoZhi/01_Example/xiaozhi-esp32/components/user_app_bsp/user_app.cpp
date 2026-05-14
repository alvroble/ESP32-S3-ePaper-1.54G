#include "user_app.h"
#include "button_bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "i2c_bsp.h"
#include "led_bsp.h"
#include "sdcard_bsp.h"
#include <stdio.h>
#include <string.h>

#include "GUI_BMPfile.h"
#include "GUI_Paint.h"
#include "epaper_port.h"

#include "client_bsp.h"
#include "json_data.h"

#include "nvs_flash.h"

#include "i2c_equipment.h"

// i2c_equipment_shtc3 *dev_shtc3 = NULL;

SemaphoreHandle_t  epaper_gui_semapHandle = NULL; // Mutual exclusion lock to prevent repeated refreshing
EventGroupHandle_t epaper_groups;                 // Event group for map refreshing
EventGroupHandle_t Green_led_Mode_queue = 0;      // Queue for LED blinking, mainly for storing mode parameters
uint8_t            Green_led_arg        = 0;      // Parameters for LED task
uint8_t            Red_led_arg          = 0;      // Parameters for LED task
static bool is_vabtflag = false;

static void Green_led_user_Task(void *arg) {
    uint8_t *led_arg = (uint8_t *) arg;
    for (;;) {
        EventBits_t even =
            xEventGroupWaitBits(Green_led_Mode_queue, set_bit_all, pdFALSE, pdFALSE, portMAX_DELAY);
        if (get_bit_data(even, 1)) {
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            xEventGroupClearBits(Green_led_Mode_queue, rset_bit_data(1));
        }
        if (get_bit_data(even, 2)) {
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            xEventGroupClearBits(Green_led_Mode_queue, rset_bit_data(2));
        }
        if (get_bit_data(even, 3)) {
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            xEventGroupClearBits(Green_led_Mode_queue, rset_bit_data(3));
        }
        if (get_bit_data(even, 4)) {
            led_set(LED_PIN_Green, LED_ON);
            xEventGroupClearBits(Green_led_Mode_queue, rset_bit_data(4));
        }
        if (get_bit_data(even, 5)) {
            led_set(LED_PIN_Green, LED_OFF);
            xEventGroupClearBits(Green_led_Mode_queue, rset_bit_data(5));
        }
        if (get_bit_data(even, 6)) {
            while (*led_arg) {
                led_set(LED_PIN_Green, LED_ON);
                vTaskDelay(pdMS_TO_TICKS(100));
                led_set(LED_PIN_Green, LED_OFF);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            xEventGroupClearBits(Green_led_Mode_queue, rset_bit_data(6));
        }
        if (get_bit_data(even, 7)) 
        {
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(LED_PIN_Green, LED_OFF);
            xEventGroupClearBits(Green_led_Mode_queue, rset_bit_data(7));
        }
    }
}

static void boot_button_user_Task(void *arg) {
    esp_err_t ret;

    for (;;) {
        EventBits_t even = xEventGroupWaitBits(boot_groups, (0x02), pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        if (get_bit_button(even, 1)) { 
            nvs_handle_t my_handle;
            ret = nvs_open("PhotoPainter", NVS_READWRITE, &my_handle);
            ESP_ERROR_CHECK(ret);
            uint8_t Mode_value = 0;
            ret                = nvs_get_u8(my_handle, "Mode_Flag", &Mode_value);
            ESP_ERROR_CHECK(ret);
            if (Mode_value == 0x01) { 
                xEventGroupClearBits(boot_groups, set_bit_button(0));
                ret = nvs_set_u8(my_handle, "Mode_Flag", 0x00);
                ESP_ERROR_CHECK(ret);
                ret = nvs_set_u8(my_handle, "PhotPainterMode", 0x04);
                ESP_ERROR_CHECK(ret);
                nvs_commit(my_handle);
                nvs_close(my_handle); 
                esp_restart();
            }
        }
    }
}
static void pwr_button_user_Task(void *arg) {

    for (;;) {
        EventBits_t even = xEventGroupWaitBits(pwr_groups, (0x02), pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        if (get_bit_button(even, 1)) { 
            if(is_vabtflag)
            {
                is_vabtflag = false;
                vTaskDelay(pdMS_TO_TICKS(200));
                gpio_set_level(GPIO_NUM_17, 0); //power off
            }
        }
        else if(get_bit_button(even,4))
        {
            if(!is_vabtflag)
            {
                is_vabtflag = true;
            }
        }
    }
}

void vbat_scanStatus(void)
{
    if(gpio_get_level(GPIO_NUM_18) && (!is_vabtflag))
    {
        is_vabtflag = true;
    }
}

void epaper_power_up(void) {
    gpio_config_t gpio6_conf = {};
    gpio6_conf.intr_type = GPIO_INTR_DISABLE;       
    gpio6_conf.mode = GPIO_MODE_OUTPUT;             
    gpio6_conf.pin_bit_mask = (0x1ULL << GPIO_NUM_6) | (0x1ULL << GPIO_NUM_42) | (0x1ULL << GPIO_NUM_46) | (0x1ULL << GPIO_NUM_17) ;     // 配置 GPIO6
    gpio6_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; 
    gpio6_conf.pull_up_en = GPIO_PULLUP_ENABLE;      
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio6_conf));
    gpio_set_level(GPIO_NUM_6, 0);                
    gpio_set_level(GPIO_NUM_42, 0);              
    gpio_set_level(GPIO_NUM_46, 1);        
    gpio_set_level(GPIO_NUM_17, 1); 
    vTaskDelay(pdMS_TO_TICKS(10));
}

uint8_t User_Mode_init(void) 
{
    epaper_power_up();
    epaper_gui_semapHandle = xSemaphoreCreateMutex(); /* Acquire the mutual exclusion lock to prevent re-flashing */
    i2c_master_Init();                                /* Must be initialized */
    led_init();                                       /* LED Blink Initialization */
    vbat_scanStatus();
    epaper_port_init();                               /* Ink Display Initialization */
    uint8_t sdcard_win = _sdcard_init();              /* SD Card Initialization */
    if (sdcard_win == 0)
        return 0;
    //get_bmp_count_by_scan("/sdcard/06_user_foundation_img");
    scan_imgs_and_save_list();
    Green_led_Mode_queue = xEventGroupCreate();
    epaper_groups        = xEventGroupCreate();

    xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(4));
    button_Init();
    xTaskCreate(boot_button_user_Task, "boot_button_user_Task", 4 * 1024, NULL, 3, NULL);
    xTaskCreate(pwr_button_user_Task, "pwr_button_user_Task", 4 * 1024, NULL, 3, NULL);
    xTaskCreate(Green_led_user_Task, "Green_led_user_Task", 3 * 1024, &Green_led_arg, 2, NULL);

    return 1;
}
