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
#include "audio_bsp.h"
#include "i2c_bsp.h"

#define TAG "AUDIO_APP"

#define RECORD_DURATION_MS    10000
#define RECORD_SAMPLE_RATE    16000
#define RECORD_CHANNEL        2
#define RECORD_BITS_PER_SAMPLE 16
#define RECORD_BUF_SIZE       (RECORD_SAMPLE_RATE * RECORD_CHANNEL * (RECORD_BITS_PER_SAMPLE/8) * (RECORD_DURATION_MS/1000))

epaper_driver_display *driver = NULL;
board_power_bsp_t board_div(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);

static EventGroupHandle_t button_groups;
static uint8_t *audio_ptr = NULL;

static bool is_music_playing = false;
static bool need_stop_music  = false;

void button_boot_task(void *arg);
void button_pwr_task(void *arg);
void i2s_audio_Test(void *arg);

void user_app_init(void)
{
    button_groups = xEventGroupCreate();
    audio_ptr = (uint8_t *)heap_caps_malloc(RECORD_BUF_SIZE, MALLOC_CAP_SPIRAM);

    board_div.POWEER_EPD_ON();
    board_div.POWEER_Audio_ON();
    i2c_master_Init();

    user_button_init();
    audio_bsp_init();
    audio_play_init();

    xTaskCreatePinnedToCore(button_boot_task, "button_boot_task", 4 * 1024, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(button_pwr_task,  "button_pwr_task",  4 * 1024, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(i2s_audio_Test,  "i2s_audio_Test",   4 * 1024, NULL, 3, NULL, 1);
}

//======================================================
// BOOT Button Long Press → Record Audio
//======================================================
void button_boot_task(void *arg)
{
    for(;;) {
        EventBits_t bits = xEventGroupWaitBits(
            boot_groups,
            set_bit_button(3),
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(200)
        );

        if (get_bit_button(bits, 3)) {
            ESP_LOGI(TAG, "BOOT long pressed → Start recording");
            xEventGroupSetBits(button_groups, 0x01);
        }
    }
}

//======================================================
// PWR Button:
//      double Press  → Play Music
//      Single Click → Stop Music
//======================================================
void button_pwr_task(void *arg)
{
    for(;;) {
        EventBits_t bits = xEventGroupWaitBits(
            pwr_groups,
            set_bit_all,
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(100)
        );

        // double press PWR → Play music
        if (get_bit_button(bits, 1)) {
            ESP_LOGI(TAG, "PWR double pressed → Play music");
            is_music_playing = true;
            need_stop_music  = false;
            xEventGroupSetBits(button_groups, 0x04);
        }

        // Single click PWR → Stop music
        if (get_bit_button(bits, 0)) {
            ESP_LOGI(TAG, "PWR clicked → Stop music");
            need_stop_music = true;
            is_music_playing = false;
        }
    }
}

//======================================================
// Main Audio Task: Record + Play Music + Stop Control
//======================================================
void i2s_audio_Test(void *arg)
{
    for(;;) {
        EventBits_t even = xEventGroupWaitBits(
            button_groups,
            0x01 | 0x04,
            pdTRUE, pdFALSE,
            pdMS_TO_TICKS(50)
        );

        // ===================== Recording =====================
        if (even & 0x01) {
            size_t record_len = 0;

            // Record while BOOT key is held
            while (gpio_get_level(BOOT_BUTTON_PIN) == 0 && record_len < RECORD_BUF_SIZE) {
                size_t to_read = 256;
                if (record_len + to_read > RECORD_BUF_SIZE)
                    to_read = RECORD_BUF_SIZE - record_len;

                audio_playback_read(audio_ptr + record_len, to_read);
                record_len += to_read;
            }

            ESP_LOGI(TAG, "Recording finished: %d bytes", record_len);

            // Play recording if data exists
            if (record_len > 0) {
                ESP_LOGI(TAG, "Play recording");
                audio_playback_write(audio_ptr, record_len);
                ESP_LOGI(TAG, "Playback finished!");
            }
        }

        // ===================== Music Playback =====================
        else if (even & 0x04) {
            audio_playback_set_vol(90);
            uint32_t bytes_size;
            uint8_t *data_ptr = i2s_get_handle(&bytes_size);
            size_t bytes_written = 0;

            is_music_playing = true;
            while (bytes_written < bytes_size && is_music_playing) {
                if (need_stop_music) break;
                audio_playback_write(data_ptr, 256);
                data_ptr += 256;
                bytes_written += 256;
            }

            ESP_LOGI(TAG, "Music stopped");
            need_stop_music = false;
            is_music_playing = false;
            audio_playback_set_vol(100);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}