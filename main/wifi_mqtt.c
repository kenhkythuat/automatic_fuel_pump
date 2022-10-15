#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "app_common_interfaces.h"

static const char *TAG = "MQTTS";

extern const uint8_t client_cert_pem_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t client_cert_pem_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t client_key_pem_start[] asm("_binary_private_pem_key_start");
extern const uint8_t client_key_pem_end[] asm("_binary_private_pem_key_end");
extern const uint8_t server_cert_pem_start[] asm("_binary_root_CA_crt_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_root_CA_crt_end");

cJSON *json_obj;
cJSON *JSnodecfg_recv, *JScfgdata_recv, *JSinitdata_recv, *JSdesire, *JSstate_recv;
struct mb_data ZY0565C_recv[106];
// esp_mqtt_client_handle_t mqtt_client;


static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    int8_t msg_type = -1;
    char buf[10], topic_name[100];
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            // subcribe to OTA topic
            msg_id = esp_mqtt_client_subscribe(client, "updatefw_topic", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            // subcribe to data topic QoS0
            msg_id = esp_mqtt_client_subscribe(client, "$aws/things/Fuel_dispenser_wifi/medklinn_data", 0);
            
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
            // subcribe to data topic QoS1
            msg_id = esp_mqtt_client_subscribe(client, "$aws/things/Fuel_dispenser_wifi/medklinn_data", 1);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            sprintf(topic_name,"%.*s",event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            json_obj = cJSON_Parse(event->data);
            if(json_obj == NULL) {
                printf("Json can not parse\n");
                break;
            }

            if(cJSON_GetObjectItem(json_obj, "state") != NULL) {
                JSstate_recv = cJSON_GetObjectItem(json_obj, "state");
   
                // parse OTA update request to get URL
                if(cJSON_GetObjectItem(JSstate_recv, "OTA") != NULL) {
                    ota_update(cJSON_GetObjectItem(JSstate_recv, "OTA")->valuestring);
                }
            }
            cJSON_Delete(json_obj);

            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}


static esp_mqtt_client_handle_t mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtts://a3bdz3mxynvuyy-ats.iot.ap-northeast-1.amazonaws.com/things/Fuel_dispenser_wifi/shadow?name=fuel_dispenser_test",
        .client_cert_pem = (const char *)client_cert_pem_start,
        .client_key_pem = (const char *)client_key_pem_start,
        .cert_pem = (const char *)server_cert_pem_start,
        .buffer_size = 2048, //change mqtt message size to 2048 bytes
    };

    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
    return client;
}

void wifi_main(void)
{
    mqtt_client = mqtt_app_start();
    if (mqtt_client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize mqtt \n");
        abort();
    }
    json_obj = cJSON_CreateObject();
}