
idf_component_register(
    SRCS "main.cpp"
    SRCS "wifi_mqtt_FD.c"
    SRCS "wifi_sta.c"     
    SRCS "ota_update.c"
    SRCS "virtual_keypad.c"
    SRCS "rs232.c"
    INCLUDE_DIRS "."
  )  #  REQUIRES wifi_provisioning qrcode protocol_examples_common mqtt freemodbus json esp_https_ota app_update )

