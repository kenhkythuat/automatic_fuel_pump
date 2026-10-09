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
#include "esp_event.h"
#include "cJSON.h"
#ifdef __cplusplus
extern "C"
{
#endif
#include <app_common_interfaces.h>
#ifdef __cplusplus
}
#endif

#define TAG "MAIN"

// OTA server URL
#define FW_URL "https://raw.githubusercontent.com/kenhkythuat/automatic_fuel_pump/read_rs232_printer/releases/esp32s3/atc_wifi_fw.bin"
#define DEFAULT_DEVICE_ID "node_pay_001"
#define DEFAULT_GW_PAY_ID "gw_pay_001"
#define DEFAULT_MQTT_CLIENT_ID "node_qr_001"
#define DEFAULT_FW_VERSION 21
#define DEFAULT_PRICE 10000

uint8_t operationMode; // OTA submodule: there are 2 modes: FUEL_DISPENSER_MODE and OTA_MODE

uint8_t u8FwVerion = 0;
uint16_t u16CurPrice = 0;
uint8_t u8DeviceId = 0;
char *deviceID = NULL;
char *gwPayID = NULL;
char *mqttClientID = NULL;

QueueHandle_t uplink_queue = NULL;

static void nvs_set_default_u8(nvs_handle handle, const char *key, uint8_t value)
{
    ESP_ERROR_CHECK(nvs_set_u8(handle, key, value));
}

static void nvs_set_default_u16(nvs_handle handle, const char *key, uint16_t value)
{
    ESP_ERROR_CHECK(nvs_set_u16(handle, key, value));
}

static void nvs_set_default_str(nvs_handle handle, const char *key, const char *value)
{
    ESP_ERROR_CHECK(nvs_set_str(handle, key, value));
}

static bool nvs_needs_default(esp_err_t err)
{
    return err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH;
}

static char *nvs_load_or_set_default_str(nvs_handle handle,
                                         const char *key,
                                         const char *default_value)
{
    esp_err_t err;
    size_t str_len = 0;
    char *value = NULL;

    err = nvs_get_str(handle, key, NULL, &str_len);
    if (nvs_needs_default(err))
    {
        nvs_set_default_str(handle, key, default_value);
        str_len = strlen(default_value) + 1;
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }

    value = (char *)malloc(str_len);
    if (value == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate NVS string %s", key);
        abort();
    }

    err = nvs_get_str(handle, key, value, &str_len);
    ESP_ERROR_CHECK(err);
    return value;
}

// get NVS values
static void getOperationMode_version()
{
    esp_err_t err;
    nvs_handle nodeconfig_hdl = 0;
    u8DeviceId = 0;
    err = nvs_open("nodeconfig", NVS_READWRITE, &nodeconfig_hdl);
    ESP_ERROR_CHECK(err);

    err = nvs_get_u8(nodeconfig_hdl, "OperationMode", &operationMode);
    if (nvs_needs_default(err))
    {
        operationMode = FUEL_DISPENSER_MODE;
        nvs_set_default_u8(nodeconfig_hdl, "OperationMode", operationMode);
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }
    ESP_LOGI(TAG, "OperationMode %d\n", operationMode);

    err = nvs_get_u16(nodeconfig_hdl, "price", &u16CurPrice);
    if (nvs_needs_default(err))
    {
        u16CurPrice = DEFAULT_PRICE;
        nvs_set_default_u16(nodeconfig_hdl, "price", u16CurPrice);
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }
    ESP_LOGI(TAG, "Current price %d\n", u16CurPrice);

    err = nvs_get_u8(nodeconfig_hdl, "fwVerion", &u8FwVerion);
    if (nvs_needs_default(err))
    {
        u8FwVerion = DEFAULT_FW_VERSION;
        nvs_set_default_u8(nodeconfig_hdl, "fwVerion", u8FwVerion);
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }
    ESP_LOGI(TAG, "Fw version %d\n", u8FwVerion);

    deviceID = nvs_load_or_set_default_str(nodeconfig_hdl, "deviceId", DEFAULT_DEVICE_ID);
    gwPayID = nvs_load_or_set_default_str(nodeconfig_hdl, "gw_pay", DEFAULT_GW_PAY_ID);
    mqttClientID = nvs_load_or_set_default_str(nodeconfig_hdl, "mqttClientId", DEFAULT_MQTT_CLIENT_ID);
    ESP_ERROR_CHECK(nvs_commit(nodeconfig_hdl));
    ESP_LOGI(TAG, "Device ID %s", deviceID);
    ESP_LOGI(TAG, "Gateway payment ID %s", gwPayID);
    ESP_LOGI(TAG, "MQTT client ID %s", mqttClientID);

    nvs_close(nodeconfig_hdl);
}

extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_VERBOSE);
    ESP_LOGI(TAG, "Starting app_main function...\n");
    ESP_LOGI(TAG, "FW version 1.0\n");
    status_led_init();

    esp_err_t err;
    // Initialize the GPIO ISR handler service
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    ESP_ERROR_CHECK(err);

    /* Initialize NVS partition */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /* NVS partition was truncated
         * and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());

        /* Retry nvs_flash_init */
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    // readKeysFromNVS();

    ota_mark_app_valid_after_boot();
    getOperationMode_version();
    if (operationMode == FUEL_DISPENSER_MODE) // run fuel dispenser app
    {
        ESP_LOGI(TAG, "Device is running Fuel Dispenser mode");
        // Create a queue for threads communication
        uplink_queue = xQueueCreate(10, sizeof(char *));
        if (uplink_queue == NULL)
            abort();
        virtual_keypad_init();
        wifi_sta_main(); // connecting to wifi AP
        FD_wifi_mqtt_config();
    }
    else // run ota app
    {
        wifi_sta_main();    // connecting to wifi AP
        ota_update(FW_URL); // start OTA update and Set operation mode back to FUEL DISPENSER
    }
}
