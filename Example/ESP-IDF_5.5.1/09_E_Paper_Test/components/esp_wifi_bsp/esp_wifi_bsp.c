#include <stdio.h>
#include <string.h>
#include "esp_wifi_bsp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define TAG "WIFI"

// Credentials are configured via 'idf.py menuconfig' -> WiFi Configuration.
// Defaults are intentionally empty so a fresh build won't leak secrets via
// a public repo. The first time you flash, run menuconfig and set
// WIFI_SSID + WIFI_PASSWORD (or override them in sdkconfig).
#define WIFI_SSID     CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD

#define MAX_RETRY 10

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_num = 0;
static bool s_connected = false;

// RTC_DATA_ATTR: survives deep sleep, cleared on power cycle.
// Gates the expensive one-time init (NVS, netif, event loop, handlers)
// which would otherwise be re-registered on every wake, leaking memory
// and eventually crashing the Wi-Fi stack after dozens of cycles.
RTC_DATA_ATTR static bool s_wifi_bsd_initialized = false;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "retry %d/%d", s_retry_num, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void espwifi_Init(void)
{
    // ---- Idempotent inits: MUST run every wake ----
    // Their in-RAM state is wiped by esp_restart() and may be wiped by
    // some deep-sleep wake paths even though the flash data is preserved.
    // Worse: s_wifi_bsd_initialized lives in RTC memory and can read as
    // a stale non-zero value after a flash update (the old firmware had
    // different RTC variables, so the linker-placed slot has garbage).
    // Calling these unconditionally closes that hole.
    //
    // nvs_flash_init()                       -> ESP_OK if already init'd.
    // esp_netif_init()                        -> ESP_OK if already init'd.
    // esp_event_loop_create_default()         -> ESP_OK if loop exists.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ---- Non-idempotent inits: gated by RTC, but tolerant of stale flag ----
    // These create new resources (a default STA netif, event handler
    // instances). Calling them on every wake would leak. The RTC flag is
    // our best-effort signal that we are on a "first boot per power cycle"
    // but it is unreliable (see above). If the gate is wrong we use
    // ESP_ERROR_CHECK_WITHOUT_ABORT so a "already exists" error from the
    // underlying API does not crash the firmware -- it just logs and
    // continues with whatever was there before.
    if (!s_wifi_bsd_initialized) {
        // esp_netif_create_default_wifi_sta returns esp_netif_t*, not esp_err_t
        // so we can't use ESP_ERROR_CHECK_WITHOUT_ABORT -- check explicitly.
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGW(TAG, "default WiFi STA netif already exists (stale gate)");
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register(
                WIFI_EVENT, ESP_EVENT_ANY_ID,
                &event_handler, NULL, NULL));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register(
                IP_EVENT, IP_EVENT_STA_GOT_IP,
                &event_handler, NULL, NULL));
        s_wifi_bsd_initialized = true;
        ESP_LOGI(TAG, "one-time WiFi BSP init complete");
    }

    // ---- Per-wake init ----
    // EventGroup lives in regular RAM (preserved across deep sleep): reuse
    // the handle, just clear the bits from the previous cycle.
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    } else {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    s_retry_num = 0;
    s_connected = false;

    // Wi-Fi driver itself must be re-init'd every wake: its ~30 KB of
    // internal buffers live in regular RAM and need fresh init/start.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "espwifi_Init finished, waiting for IP...");
}

void espwifi_Deinit(void)
{
    // Stop and deinit the Wi-Fi driver before deep sleep. Without
    // esp_wifi_deinit() the driver's internal state (scanned AP list,
    // tx/rx buffers, ~30 KB) is leaked across every wake cycle.
    esp_wifi_disconnect();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_deinit());
}

EventGroupHandle_t espwifi_GetEventGroup(void)
{
    return s_wifi_event_group;
}

bool espwifi_IsConnected(void)
{
    return s_connected;
}
