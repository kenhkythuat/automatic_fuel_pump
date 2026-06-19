#ifndef _APPCOMMONINTERFACES_H_
#define _APPCOMMONINTERFACES_H_
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "cJSON.h"

#define FUEL_DISPENSER_MODE 1
#define FW_OTA_MODE 2
void wifi_sta_main(void);
//Modbus related functions
struct fuel_para {
    unsigned int liter;
    unsigned int money;
    unsigned int price; 
};
extern QueueHandle_t uplink_queue;

// List of Global variables which use for synchronize between submodules
extern uint8_t u8FwVerion;
extern uint16_t u16CurPrice;
extern uint8_t u8DeviceId;
extern char* deviceID;

// Wifi variables and function
void wifi_main(void);

// rs232 configuration
void rs232_config(void);

// Keypad submodule interfaces
void virtual_keypad_init();
void change_price_by_vir_keypad(void *arg);
void enter_qr_price_by_vir_keypad(void *arg);
void cancel_qr_money_by_vir_keypad(void *arg);
void end_session_by_vir_keypad(void *arg);

// OTA function
void ota_update(char *url);
void FD_wifi_mqtt_config();

#endif
