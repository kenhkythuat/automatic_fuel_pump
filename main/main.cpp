/*******************************************************************************
 * 
 * ttn-esp32 - The Things Network device library for ESP-IDF / SX127x
 * 
 * Copyright (c) 2018 Manuel Bleichenbacher
 * 
 * Licensed under MIT License
 * https://opensource.org/licenses/MIT
 *
 * Sample program showing how to send and receive messages.
 *******************************************************************************/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "cJSON.h"
#ifdef LORA_COMM_ENABLE 
#include "TheThingsNetwork.h"
#endif
#ifdef __cplusplus
extern "C" {
#endif
#include <app_common_interfaces.h>
#ifdef __cplusplus
}
#endif

#define FW_URL "http://172.24.1.1:8181/atc_wifi_fw.bin"
uint8_t operationMode ;
uint16_t u16Price ;

static void getOperationMode_version()
{
    esp_err_t err;
    nvs_handle nodeconfig_hdl = 0;
    err=nvs_open("nodeconfig",NVS_READWRITE,&nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    err=nvs_get_u8(nodeconfig_hdl,"OperationMode",&operationMode);
    ESP_ERROR_CHECK(err);
    printf("OperationMode %d\n",operationMode);
    err=nvs_get_u16(nodeconfig_hdl,"price",&u16Price);
    ESP_ERROR_CHECK(err);
    printf("Current price %d\n",u16Price);
    err=nvs_get_u8(nodeconfig_hdl,"fwVerion",&u8FwVerion);
    printf("Fw version %d\n",u8FwVerion);
    ESP_ERROR_CHECK(err);
    nvs_close(nodeconfig_hdl);
}

extern "C" void app_main(void)
{
    printf("Starting app_main function...\n");
    esp_err_t err;
    // Initialize the GPIO ISR handler service
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    ESP_ERROR_CHECK(err);
    
    /* Initialize NVS partition */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated
         * and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());

        /* Retry nvs_flash_init */
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    //readKeysFromNVS();

    getOperationMode_version();
    if(operationMode==FUEL_DISPENSER_MODE)//run fuel dispenser app
    {        
        uplink_queue = xQueueCreate( 10, sizeof(uint16_t) );
        if (uplink_queue == NULL) abort();
        virtual_keypad_init();
        wifi_sta_main();//connecting to wifi AP
        FD_wifi_mqtt_config();        
    }
    else //run ota app
    {
        wifi_sta_main();//connecting to wifi AP
        ota_update(FW_URL); //start OTA update and Set operation mode back to FUEL DISPENSER
    }

}
