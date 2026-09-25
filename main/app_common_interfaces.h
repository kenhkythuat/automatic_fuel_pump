#ifndef _APPCOMMONINTERFACES_H_
#define _APPCOMMONINTERFACES_H_
#include <stdio.h>
#include <stdbool.h>
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

// 1: main RS232 fuel dispenser flow.
//    set_qr_money is entered exactly as the server sends it.
// 0: alternate keypad flow.
//    set_qr_money keeps the existing scaled keypad amount behavior.
#ifndef MAIN_RS232
#define MAIN_RS232 1
#endif

void wifi_sta_main(void);
//Modbus related functions
struct fuel_para {
    unsigned int liter;
    unsigned int money;
    unsigned int price; 
};

typedef struct {
    uint32_t money;
    uint32_t liter_milliliters;
    uint32_t price;
} rs232_receipt_data_t;
extern QueueHandle_t uplink_queue;

// List of Global variables which use for synchronize between submodules
extern uint8_t u8FwVerion;
extern uint16_t u16CurPrice;
extern uint8_t u8DeviceId;
extern char* deviceID;
extern char* gwPayID;
extern char* mqttClientID;

// Wifi variables and function
void wifi_main(void);
bool wifi_config_load_credentials(char *ssid,
                                  size_t ssid_size,
                                  char *password,
                                  size_t password_size);
void wifi_config_button_init(void);
void wifi_config_portal_start(void);
bool wifi_config_portal_is_active(void);

// Status LED
void status_led_init(void);
void status_led_set_wifi_connected(bool connected);

// rs232 configuration
void rs232_config(void);
void rs232_receipt_reset(void);
bool rs232_receipt_get(rs232_receipt_data_t *result);
void keypad_master_scan_pause_for_virtual_keypad(void);
void keypad_master_scan_resume_after_virtual_keypad(void);
void keypad_master_scan_disable_for_external_physical_keypad(void);
void keypad_master_scan_enable_for_virtual_keypad(void);
void payment_input_switch_update(uint8_t level);
bool payment_control_switch_can_follow_input(void);
void payment_set_qr_money_keypad_done(void);
void input_switch_refresh_control_switch(void);
void keypad_password_handle_key_event(char key, bool pressed);
bool keypad_password_is_unlocked(void);

// Keypad submodule interfaces
void virtual_keypad_init();
bool virtual_keypad_press_key(char key);
void virtual_keypad_boot_clear(void);
void virtual_keypad_set_external_physical(bool enabled);
bool virtual_keypad_is_enabled(void);
bool virtual_keypad_is_external_physical_enabled(void);
void change_price_by_vir_keypad(void *arg);
void enter_qr_price_by_vir_keypad(void *arg);
void enter_qr_litter_by_vir_keypad(void *arg);
void cancel_qr_money_by_vir_keypad(void *arg);
void end_session_by_vir_keypad(void *arg);

// OTA function
void ota_update(char *url);
void ota_start_github_version_check(void);
void ota_mark_app_valid_after_boot(void);
void FD_wifi_mqtt_config();
void FD_wifi_mqtt_stop_for_config_portal(void);

#endif
