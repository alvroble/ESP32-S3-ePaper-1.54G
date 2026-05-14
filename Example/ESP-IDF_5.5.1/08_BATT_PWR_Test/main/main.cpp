#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "user_app.h"
#include "lvgl.h"
#include "user_config.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "main_1_54G";

extern "C" void app_main(void)
{
	user_app_init();
}