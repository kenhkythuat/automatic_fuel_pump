/* MQTT (over TCP) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/


#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "app_common_interfaces.h"
//#include "protocol_examples_common.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "cJSON.h"

static const char *TAG = "MQTT";

//extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
//extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

const int MQTT_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t mqtt_conn_event_group;
static SemaphoreHandle_t push_msg_sem;
esp_mqtt_client_handle_t client;
cJSON *json_obj;
char payload[256];
//int currentPrice=0;

static void log_error_if_nonzero(const char * message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}
static void setOperationMode_version(uint8_t OperationMode)
{
    esp_err_t err;
    nvs_handle nodeconfig_hdl = 0;
    err=nvs_open("nodeconfig",NVS_READWRITE,&nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    err=nvs_set_u8(nodeconfig_hdl,"OperationMode",OperationMode);
    ESP_ERROR_CHECK(err);
    nvs_close(nodeconfig_hdl);
}

static void setFW_version(uint8_t u8FwVerion)
{
    esp_err_t err;
    nvs_handle nodeconfig_hdl = 0;
    err=nvs_open("nodeconfig",NVS_READWRITE,&nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    err=nvs_set_u8(nodeconfig_hdl,"fwVerion",u8FwVerion);
    ESP_ERROR_CHECK(err);
    nvs_close(nodeconfig_hdl);
}

static void setNewPrice(uint16_t u16NewPrice)
{
    esp_err_t err;
    nvs_handle nodeconfig_hdl = 0;
    err=nvs_open("nodeconfig",NVS_READWRITE,&nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    err=nvs_set_u8(nodeconfig_hdl,"price",u16NewPrice);
    ESP_ERROR_CHECK(err);
    nvs_close(nodeconfig_hdl);
}

static esp_err_t FD_mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id=0;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            // subcribe to data topic QoS0
            msg_id = esp_mqtt_client_subscribe(client, "/station/price/diesel_4", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
            msg_id = esp_mqtt_client_subscribe(client, "/station/fw_version/diesel_4", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
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
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            json_obj = cJSON_Parse(event->data);
            if(json_obj == NULL) {
                printf("Json can not parse\n");
                break;
            }
            
            if(cJSON_GetObjectItem(json_obj, "price") != NULL) 
                if(atoi(cJSON_GetObjectItem(json_obj, "price")->valuestring) !=u16CurPrice)
                {                    
                    //update current price to new price and store in NVS
                    u16CurPrice=atoi(cJSON_GetObjectItem(json_obj, "price")->valuestring);
                    setNewPrice(u16CurPrice);

                    //Send new price to device via simulated keypad
                    xTaskCreate(&change_price_by_vir_keypad, 
                                "change_price_by_vir_keypad", 
                                2048, 
                                cJSON_GetObjectItem(json_obj, "price")->valuestring, 
                                configMAX_PRIORITIES-1, 
                                NULL);
                                // &my_task_handler);
                    // change_price_by_vir_keypad(cJSON_GetObjectItem(json_obj, "price")->valuestring);
                }
            if(cJSON_GetObjectItem(json_obj, "fw_version") != NULL)
            {
                int getFwVer=atoi(cJSON_GetObjectItem(json_obj, "fw_version")->valuestring);
                printf("FW version : %d\n",getFwVer);
                if(getFwVer>u8FwVerion)
                {
                    u8FwVerion=getFwVer;
                    setFW_version(u8FwVerion);
                    // update Operation Mode in NVS
                    setOperationMode_version(FW_OTA_MODE);
                    esp_restart();  
                }
            }  
               
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
                ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

            }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void FD_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    FD_mqtt_event_handler_cb(event_data);
}
#define ORP 1
static void FD_mqtt_app_start(void)
{   
    esp_mqtt_client_config_t mqtt_cfg = {
        #ifdef ORP
        .uri = "mqtt://172.24.1.1/", //@ORP
        #else
        .uri = "mqtt://192.168.5.1/", //RPI4
        #endif
        .port = 1883,
        // .username = "aVq7QQIRXQ82gCQiXfjX"
    };


    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, FD_mqtt_event_handler, client);
    esp_mqtt_client_start(client);
    printf("Setup done \n");
    vTaskDelay(pdMS_TO_TICKS(10000));
}

static int push_msg(char *msg_payload, uint16_t msg_len) {
    int stat;
    xSemaphoreTake(push_msg_sem, portMAX_DELAY);
    stat = esp_mqtt_client_publish(client, "/station/data", msg_payload, msg_len, 0, false);
    xSemaphoreGive(push_msg_sem);
    return stat;
}

//Send heart beat msg every 5min to maintain connection with TB server
//Send heart beat msg every 5min to maintain connection with TB server
static int push_heartbeat_msg(char *msg_payload, uint16_t msg_len)
{
    int stat;
    xSemaphoreTake(push_msg_sem, portMAX_DELAY);
    stat=esp_mqtt_client_publish(client, "/station/ping", msg_payload, msg_len, 0, false);
    xSemaphoreGive(push_msg_sem);
    return stat;
}
//int currentPrice=0;    
void push_msg_to_broker(void) {
    wifi_ap_record_t ap;
    int stat;
    int idx=0;
    int prev_stat = 0;
    uint32_t lit=0;
    
    char *c_data = (char *)malloc(42);
    char *liter = (char *)malloc(11); memset(liter, 0, 11); // liter[11] = '/0';
    char *money = (char *)malloc(11); memset(money, 0, 11); // money[11] = '/0';
    char *price = (char *)malloc(7);  memset(price, 0, 7);  // price[7] = '/0';
    for(;;) {          
        if (xQueueReceive(uplink_queue, &c_data, portMAX_DELAY) == pdTRUE)
        {
            printf("\nData: %s\n", c_data);
            strncpy(liter, c_data, 10);
            strncpy(money, c_data+10, 10);
            strncpy(price, c_data+20, 6);
            printf("liter: %s\n", liter);
                            
            esp_wifi_sta_get_ap_info(&ap);
            printf("Free heap size: %d bytes\n", esp_get_minimum_free_heap_size());
            // printf("c_data[39] = %d\n",c_data[39]);
            // if(c_data[39] == 5)
            //     continue;
            // if int(byte_hex[-3]) != prev_stat and int(byte_hex[-3]) == 4:
            //printf("Setprice value : %d\n", bset_price);
            // if(!bset_price) {
#if 0
                lit = rand() % 100;
                sprintf(payload, "{\"DevID\": \"diesel_%d\", \"fuel_type\": \"%s\", \"Liter\": %d, \"Money\": %d, \"Price\": %d, \"RSSI\": %d}", 
                        3, 
                        "diesel", 
                        lit, 
                        lit * atoi(price), 
                        atoi(price),  
                        ap.rssi);
#else
                //if(currentPrice !=atoi(price))
                {
                    currentPrice=atoi(price);
                }
                sprintf(payload, "{\"DevID\": \"diesel_%d\", \"fuel_type\": \"%s\", \"Liter\": %d, \"Money\": %d, \"Price\": %d, \"RSSI\": %d}", 
                        4, 
                        "gasoline", 
                        atoi(liter), 
                        atoi(money), 
                        atoi(price),  
                        ap.rssi);
#endif
                // For testing:
                // idx++;
                // sprintf(payload, "{\"DevID\": \"diesel_%d\", \"fuel_type\": \"%s\", \"Liter\": %d, \"Money\": %d, \"Price\": %d, \"RSSI\": %d}", 1, "gasoline", idx, idx, idx, ap.rssi);
                printf("Payload: %s\n", payload);
                // stat = esp_mqtt_client_publish(client, "/station/data", payload, strlen(payload), 0, false);
                stat = push_msg(payload, strlen(payload));
                ESP_LOGI(TAG, "sent publish, stat=%d", stat);
                // vTaskDelay(pdMS_TO_TICKS(5000));
/*            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
*/ 

        }
    }
}

void ping_tb(void) {
    int stat;
    //wifi_ap_record_t ap;
    for(;;) {
        //esp_wifi_sta_get_ap_info(&ap);
        //printf("Free heap size: %d bytes\n", esp_get_minimum_free_heap_size());
 
        // stat = esp_mqtt_client_publish(client, "/station/data", payload_ping, strlen(payload_ping), 0, false);
        sprintf(payload,"{\"DevID\": \"diesel_%d\", \"fuel_type\": \"%s\",\"keep_alive\":%d}",4,"gasoline",1);
        stat = push_heartbeat_msg(payload,strlen(payload));
        printf("Ping MSG successfully\n");
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
void FD_wifi_mqtt_config(void)
{

    mqtt_conn_event_group = xEventGroupCreate();
    json_obj = cJSON_CreateObject();
    //const esp_partition_t *running_partition = esp_ota_get_running_partition();
    //initialise_wifi(running_partition->label);

    FD_mqtt_app_start();
    rs232_config();
    printf("RS232 config done\n");
    push_msg_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(push_msg_sem);
    xTaskCreate(&push_msg_to_broker, "push_msg_to_broker", 2048, NULL, configMAX_PRIORITIES-1, NULL);
    xTaskCreate(&ping_tb, "ping_tb", 2048, NULL, configMAX_PRIORITIES-1, NULL);
}
