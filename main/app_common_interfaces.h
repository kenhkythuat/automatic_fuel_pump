#ifndef _APPCOMMONINTERFACES_H_
#define _APPCOMMONINTERFACES_H_
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
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
QueueHandle_t uplink_queue;
TaskHandle_t my_task_handler;


uint8_t u8FwVerion;
uint16_t u16CurPrice;
uint8_t u8DeviceId;
// Wifi variables and function
esp_mqtt_client_handle_t mqtt_client;

void wifi_main(void);
void rs232_config(void);
void virtual_keypad_init();
void change_price_by_vir_keypad();
//void change_price_by_vir_keypad(char *price);
void end_session_by_vir_keypad();
// OTA function
void ota_update(char *url);
void FD_wifi_mqtt_config();

#endif