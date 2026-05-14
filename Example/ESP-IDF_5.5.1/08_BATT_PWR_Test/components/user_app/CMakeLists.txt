idf_component_register(
  SRCS "user_app.cpp"
  PRIV_REQUIRES
  board_power_bsp
  driver
  main
  button_bsp
  REQUIRES
  INCLUDE_DIRS ".")
