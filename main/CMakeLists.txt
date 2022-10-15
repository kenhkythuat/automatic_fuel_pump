# Separate two config build for Lora and Wifi versions
set(LORA_ENABLE OFF)
#if(CMAKE_CONFIGURATION_TYPES EQUAL LORA_ENABLE)
#List of definition currently used in the project
# MODBUS_COMM_ENABLE 
# LORA_COMM_ENABLE 
# HELTEC_WIRELESS_STICK_LITE
# TTGO_LORA32
# WIFI_COMM_ENABLE
# BLE_COMM_ENABLE
# PROVISIONING_ENABLE
# FIRMWARE_OTA_ENABLE
# MODBUS_SIMULATION_ENABLE
# ADMIN_MODE
if(LORA_ENABLE)
message("BUILD FOR LORA VERSON")

idf_component_register(
    SRCS "main.cpp"
    SRCS "ble.c"        #For provisioning of Wifi ssid and password
    SRCS "modbus.c"    #Modbus operation and Wifi mqtt
    INCLUDE_DIRS "."
    REQUIRES ttn-esp32 wifi_provisioning qrcode protocol_examples_common mqtt freemodbus json )
#elseif(CMAKE_CONFIGURATION_TYPES EQUAL WIFI_ENABLE)
else()
message("BUILD FOR WIFI VERSON")

idf_component_register(
    SRCS "main.cpp"
    SRCS "wifi_mqtt.c"
    SRCS "wifi_mqtt_FD.c"
    SRCS "wifi_sta.c"     
    SRCS "ota_update.c"
    SRCS "virtual_keypad.c"
    SRCS "rs232.c"
    INCLUDE_DIRS "."
    REQUIRES wifi_provisioning qrcode protocol_examples_common mqtt freemodbus json esp_https_ota app_update )
endif()
