idf_component_register(
  SRCS "user_app.cpp"
  PRIV_REQUIRES
    driver
    esp_wifi_bsp
    esp_wifi
    esp_timer
    esp_http_client
    mbedtls
    nvs_flash
    esp_netif
    lwip
    esp_adc
    json
  REQUIRES
    epaper_src
    epaper_port
    lvgl_epaper_port
    INCLUDE_DIRS ".")
