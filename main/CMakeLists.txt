
idf_component_register(
    SRCS
        "main.cpp"
        "wifi_mqtt_FD.c"
        "wifi_sta.c"
        "ota_update.c"
        "virtual_keypad.c"
        "rs232.c"
        "status_led.c"
        "wifi_config_portal.c"
        "keypad_password.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        driver
        esp_event
        esp_http_client
        esp_http_server
        esp_https_ota
        esp_netif
        esp_wifi
        json
        lwip
        mqtt
        nvs_flash
  )  #  REQUIRES wifi_provisioning protocol_examples_common mqtt freemodbus json esp_https_ota app_update )

