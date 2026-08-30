idf_component_register(
  SRCS "lvgl_epaper_port.c"
  PRIV_REQUIRES
    esp_timer
  REQUIRES
    lvgl
    epaper_port
  INCLUDE_DIRS ".")
