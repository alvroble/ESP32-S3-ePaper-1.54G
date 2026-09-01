#ifndef ESP_WIFI_BSP_H
#define ESP_WIFI_BSP_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

void espwifi_Init(void);
void espwifi_Deinit(void);
EventGroupHandle_t espwifi_GetEventGroup(void);
bool espwifi_IsConnected(void);

#ifdef __cplusplus
}
#endif

#endif
