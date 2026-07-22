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
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "MQTT"

#define TBMQ_BROKER_URI "mqtt://161.248.146.170:1883"
#define TBMQ_PASSWORD "123456789"
#define TBMQ_CLIENT_ID "node_qr_001"
#define TBMQ_COMMAND_TOPIC "tbmq/payment/gw_pay_001/node_pay_001/command"
#define TBMQ_TELEMETRY_TOPIC "tbmq/payment/gw_pay_001/node_pay_001/telemetry"
#define TBMQ_ACK_TOPIC "tbmq/payment/gw_pay_001/node_pay_001/ack"
#define TBMQ_EVENT_TOPIC "tbmq/payment/gw_pay_001/node_pay_001/event"
#define TBMQ_KEEPALIVE_SEC 60
#define TBMQ_MSG_ID_LEN 32
#define VIRTUAL_KEYPAD_TASK_STACK_SIZE 6144

//extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
//extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

const int MQTT_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t mqtt_conn_event_group;
static SemaphoreHandle_t push_msg_sem;
static SemaphoreHandle_t payment_ctx_sem;
static esp_mqtt_client_handle_t client;
static bool mqtt_connected;
static char mqtt_client_id[96];
static char active_payment_msg_id[TBMQ_MSG_ID_LEN + 1];
static bool active_payment_has_msg_id;
static bool active_payment_input_started;
cJSON *json_obj;
char payload[512];
//int currentPrice=0;

static int publish_to_tbmq(const char *topic, const char *msg_payload, uint16_t msg_len);

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
    err=nvs_commit(nodeconfig_hdl);
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
    err=nvs_commit(nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    nvs_close(nodeconfig_hdl);
}

static void setNewPrice(uint16_t u16NewPrice)
{
    esp_err_t err;
    nvs_handle nodeconfig_hdl = 0;
    err=nvs_open("nodeconfig",NVS_READWRITE,&nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    err=nvs_set_u16(nodeconfig_hdl,"price",u16NewPrice);
    ESP_ERROR_CHECK(err);
    err=nvs_commit(nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    nvs_close(nodeconfig_hdl);
}

static void setDeviceID(char* m_deviceID) 
{
    if (!strcmp(deviceID, m_deviceID)) {
        return;
    }
    esp_err_t err;
    nvs_handle nodeconfig_hdl = 0;
    err=nvs_open("nodeconfig",NVS_READWRITE,&nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    err=nvs_set_str(nodeconfig_hdl,"deviceId",m_deviceID);
    ESP_ERROR_CHECK(err);
    err=nvs_commit(nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    nvs_close(nodeconfig_hdl);
    esp_restart();
}


static uint8_t u8_subscribed=false;

static long long protocol_timestamp_seconds(void)
{
    return (long long)time(NULL);
}

static const cJSON *get_protocol_msg_id_item(const cJSON *root)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "msg_id");
    if (item != NULL) {
        return item;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "cmd_id");
    if (item != NULL) {
        return item;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "command_id");
    if (item != NULL) {
        return item;
    }

    return cJSON_GetObjectItemCaseSensitive(root, "ack_to_id");
}

static bool parse_protocol_msg_id(const cJSON *root, char *msg_id_out, size_t msg_id_out_size)
{
    const cJSON *msg_id_item = get_protocol_msg_id_item(root);

    if (msg_id_out == NULL || msg_id_out_size < (TBMQ_MSG_ID_LEN + 1) ||
        !cJSON_IsString(msg_id_item) || msg_id_item->valuestring == NULL ||
        strlen(msg_id_item->valuestring) != TBMQ_MSG_ID_LEN) {
        return false;
    }

    for (size_t i = 0; i < TBMQ_MSG_ID_LEN; i++) {
        if (!isxdigit((unsigned char)msg_id_item->valuestring[i])) {
            return false;
        }
    }

    memcpy(msg_id_out, msg_id_item->valuestring, TBMQ_MSG_ID_LEN);
    msg_id_out[TBMQ_MSG_ID_LEN] = '\0';
    return true;
}

static void set_active_payment_msg_id(const char *msg_id)
{
    if (msg_id == NULL) {
        return;
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    snprintf(active_payment_msg_id, sizeof(active_payment_msg_id), "%s", msg_id);
    active_payment_has_msg_id = true;
    active_payment_input_started = false;

    if (payment_ctx_sem != NULL) {
        xSemaphoreGive(payment_ctx_sem);
    }
}

static bool get_active_payment_msg_id(char *msg_id_out, size_t msg_id_out_size)
{
    bool has_msg_id;

    if (msg_id_out == NULL || msg_id_out_size < (TBMQ_MSG_ID_LEN + 1)) {
        return false;
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    has_msg_id = active_payment_has_msg_id;
    if (has_msg_id) {
        snprintf(msg_id_out, msg_id_out_size, "%s", active_payment_msg_id);
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreGive(payment_ctx_sem);
    }

    return has_msg_id;
}

static void clear_active_payment_msg_id(const char *completed_msg_id)
{
    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    if (active_payment_has_msg_id &&
        (completed_msg_id == NULL ||
         strcmp(active_payment_msg_id, completed_msg_id) == 0)) {
        active_payment_msg_id[0] = '\0';
        active_payment_has_msg_id = false;
        active_payment_input_started = false;
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreGive(payment_ctx_sem);
    }
}

static bool parse_number_item(const cJSON *item, char *number_buf_out, size_t number_buf_size, uint32_t max_value, uint32_t *number_value)
{
    const char *number_str = NULL;
    char number_buf[16];
    char *end_ptr = NULL;
    unsigned long value;

    if (item == NULL || number_buf_out == NULL || number_buf_size == 0 || number_value == NULL) {
        return false;
    }

    if (cJSON_IsString(item) && item->valuestring != NULL) {
        number_str = item->valuestring;
    } else if (cJSON_IsNumber(item)) {
        snprintf(number_buf, sizeof(number_buf), "%d", item->valueint);
        number_str = number_buf;
    } else {
        return false;
    }

    errno = 0;
    value = strtoul(number_str, &end_ptr, 10);
    if (errno != 0 || end_ptr == number_str || *end_ptr != '\0' || value > max_value) {
        return false;
    }

    snprintf(number_buf_out, number_buf_size, "%lu", value);
    *number_value = (uint32_t)value;
    return true;
}

static bool parse_price_item(const cJSON *item, char *price_buf, size_t price_buf_size, uint16_t *price_value)
{
    uint32_t value = 0;

    if (!parse_number_item(item, price_buf, price_buf_size, UINT16_MAX, &value)) {
        return false;
    }

    *price_value = (uint16_t)value;
    return true;
}

static bool parse_qr_amount_item(const cJSON *item, char *amount_buf, size_t amount_buf_size, uint32_t *raw_amount_value)
{
    char raw_amount_buf[16];
    uint32_t raw_amount = 0;
    uint32_t keypad_amount = 0;

    if (!parse_number_item(item, raw_amount_buf, sizeof(raw_amount_buf), UINT32_MAX, &raw_amount)) {
        return false;
    }

    keypad_amount = raw_amount / 100;
    snprintf(amount_buf, amount_buf_size, "%lu", (unsigned long)keypad_amount);
    if (raw_amount_value != NULL) {
        *raw_amount_value = raw_amount;
    }
    return true;
}

static bool topic_matches(esp_mqtt_event_handle_t event, const char *topic)
{
    return event->topic_len == strlen(topic) && strncmp(event->topic, topic, event->topic_len) == 0;
}

static const cJSON *get_qr_price_item(const cJSON *json_obj)
{
    const cJSON *item = NULL;

    if (cJSON_IsNumber(json_obj) || cJSON_IsString(json_obj)) {
        return json_obj;
    }

    item = cJSON_GetObjectItem(json_obj, "qr_price");
    if (item != NULL) {
        return item;
    }

    item = cJSON_GetObjectItem(json_obj, "amount");
    if (item != NULL) {
        return item;
    }

    item = cJSON_GetObjectItem(json_obj, "money");
    if (item != NULL) {
        return item;
    }

    return cJSON_GetObjectItem(json_obj, "price");
}

static void publish_payment_ack(const char *cmd, const char *result, const char *description, const char *msg_id)
{
    char ack_payload[256];

    if (msg_id != NULL && msg_id[0] != '\0' && description != NULL) {
        snprintf(ack_payload,
                 sizeof(ack_payload),
                 "{\"ts\":%lld,\"ack_to\":\"%s\",\"result\":\"%s\",\"description\":\"%s\",\"msg_id\":\"%s\"}",
                 protocol_timestamp_seconds(),
                 cmd,
                 result,
                 description,
                 msg_id);
    } else if (msg_id != NULL && msg_id[0] != '\0') {
        snprintf(ack_payload,
                 sizeof(ack_payload),
                 "{\"ts\":%lld,\"ack_to\":\"%s\",\"result\":\"%s\",\"msg_id\":\"%s\"}",
                 protocol_timestamp_seconds(),
                 cmd,
                 result,
                 msg_id);
    } else if (description != NULL) {
        snprintf(ack_payload,
                 sizeof(ack_payload),
                 "{\"ts\":%lld,\"ack_to\":\"%s\",\"result\":\"%s\",\"description\":\"%s\"}",
                 protocol_timestamp_seconds(),
                 cmd,
                 result,
                 description);
    } else {
        snprintf(ack_payload,
                 sizeof(ack_payload),
                 "{\"ts\":%lld,\"ack_to\":\"%s\",\"result\":\"%s\"}",
                 protocol_timestamp_seconds(),
                 cmd,
                 result);
    }

    publish_to_tbmq(TBMQ_ACK_TOPIC, ack_payload, strlen(ack_payload));
}

static void handle_payment_command(const cJSON *root)
{
    const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *param = cJSON_GetObjectItemCaseSensitive(root, "param");
    char msg_id[TBMQ_MSG_ID_LEN + 1];
    bool msg_id_valid = false;

    if (!cJSON_IsString(cmd_item) || cmd_item->valuestring == NULL) {
        ESP_LOGE(TAG, "Payment command is missing string field 'cmd'");
        publish_payment_ack("unknown", "error", "missing cmd", NULL);
        return;
    }

    const char *cmd = cmd_item->valuestring;
    msg_id_valid = parse_protocol_msg_id(root, msg_id, sizeof(msg_id));
    if (!msg_id_valid) {
        ESP_LOGE(TAG, "%s is missing/invalid required msg_id", cmd);
        publish_payment_ack(cmd, "error", "missing_or_invalid_msg_id", NULL);
        return;
    }

    if (!cJSON_IsObject(param)) {
        ESP_LOGE(TAG, "%s is missing object field 'param'", cmd);
        publish_payment_ack(cmd, "error", "missing param", msg_id);
        return;
    }

    if (strcmp(cmd, "set_qr_money") == 0) {
        const cJSON *qr_money_item = cJSON_GetObjectItemCaseSensitive(param, "qr_money");
        char amount_buf[16];
        uint32_t qr_money = 0;

        if (!parse_qr_amount_item(qr_money_item, amount_buf, sizeof(amount_buf), &qr_money)) {
            ESP_LOGE(TAG, "Invalid param.qr_money");
            publish_payment_ack(cmd, "error", "invalid qr_money", msg_id);
            return;
        }

        char *task_amount = strdup(amount_buf);
        if (task_amount == NULL) {
            ESP_LOGE(TAG, "Failed to allocate QR money task parameter");
            publish_payment_ack(cmd, "error", "out of memory", msg_id);
            return;
        }

        BaseType_t task_created = xTaskCreate(enter_qr_price_by_vir_keypad,
                                              "enter_qr_money",
                                              VIRTUAL_KEYPAD_TASK_STACK_SIZE,
                                              task_amount,
                                              configMAX_PRIORITIES - 1,
                                              NULL);
        if (task_created != pdPASS) {
            free(task_amount);
            ESP_LOGE(TAG, "Failed to create QR money keypad task");
            publish_payment_ack(cmd, "error", "task create failed", msg_id);
            return;
        }

        set_active_payment_msg_id(msg_id);
        ESP_LOGI(TAG,
                 "Payment command accepted: cmd=%s qr_money=%lu keypad=%s msg_id=%s",
                 cmd,
                 (unsigned long)qr_money,
                 amount_buf,
                 msg_id);
        publish_payment_ack(cmd, "ok", NULL, msg_id);
        return;
    }

    if (strcmp(cmd, "cancel_qr_money") == 0) {
        const cJSON *cancel_item = cJSON_GetObjectItemCaseSensitive(param, "cancel_qr_money");
        char cancel_buf[4];
        uint32_t cancel_value = 0;

        if (!parse_number_item(cancel_item,
                               cancel_buf,
                               sizeof(cancel_buf),
                               1,
                               &cancel_value) ||
            cancel_value != 1) {
            ESP_LOGE(TAG, "Invalid param.cancel_qr_money, expected 1");
            publish_payment_ack(cmd, "error", "invalid cancel_qr_money", msg_id);
            return;
        }

        BaseType_t task_created = xTaskCreate(cancel_qr_money_by_vir_keypad,
                                              "cancel_qr_money",
                                              VIRTUAL_KEYPAD_TASK_STACK_SIZE,
                                              NULL,
                                              configMAX_PRIORITIES - 1,
                                              NULL);
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create cancel QR money keypad task");
            publish_payment_ack(cmd, "error", "task create failed", msg_id);
            return;
        }

        clear_active_payment_msg_id(msg_id);
        ESP_LOGI(TAG, "Payment command accepted: cmd=%s msg_id=%s", cmd, msg_id);
        publish_payment_ack(cmd, "ok", NULL, msg_id);
        return;
    }

    ESP_LOGW(TAG, "Unsupported payment command: %s", cmd);
    publish_payment_ack(cmd, "error", "unsupported command", msg_id);
}

static esp_err_t FD_mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id=0;
    char tb_topic_price[64],tb_topic_fw[64],tb_topic_endsession[64],tb_topic_devID[64],tb_topic_qr_price[80];
    
    // your_context_t *context = event->context;
    // sprintf(tb_topic_price, "/station/price/fs_node_1201440612_%d",u8DeviceId);
    // sprintf(tb_topic_fw, "/station/fw_version/fs_node_1201440612_%d",u8DeviceId);
    // sprintf(tb_topic_endsession, "/station/End_Session/fs_node_1201440612_%d",u8DeviceId);
    sprintf(tb_topic_price, "/station/price/%s",deviceID);
    sprintf(tb_topic_fw, "/station/fw_version/%s",deviceID);
    sprintf(tb_topic_endsession, "/station/End_Session/%s",deviceID);
    sprintf(tb_topic_devID, "/station/deviceID/%s",deviceID);
    sprintf(tb_topic_qr_price, "/station/qr_price/%s",deviceID);
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            mqtt_connected = true;
            xEventGroupSetBits(mqtt_conn_event_group, MQTT_CONNECTED_EVENT);
            msg_id = esp_mqtt_client_subscribe(client, TBMQ_COMMAND_TOPIC, 1);
            ESP_LOGI(TAG, "subscribe to topic: %s QoS=1, msg_id=%d", TBMQ_COMMAND_TOPIC, msg_id);

            // subcribe to data topic QoS0
            msg_id = esp_mqtt_client_subscribe(client, tb_topic_endsession, 0);
            ESP_LOGI(TAG, "subscribe to topic: %s  successful, msg_id=%d", tb_topic_endsession, msg_id);
            msg_id = esp_mqtt_client_subscribe(client, tb_topic_price, 0);
            ESP_LOGI(TAG, "subscribe to topic: %s  successful, msg_id=%d", tb_topic_price, msg_id);
            msg_id = esp_mqtt_client_subscribe(client, tb_topic_fw, 0);
            ESP_LOGI(TAG, "subscribe to topic: %s  successful, msg_id=%d", tb_topic_fw, msg_id);
            msg_id = esp_mqtt_client_subscribe(client, tb_topic_devID, 0);
            ESP_LOGI(TAG, "subscribe to topic: %s  successful, msg_id=%d", tb_topic_devID, msg_id);
            msg_id = esp_mqtt_client_subscribe(client, tb_topic_qr_price, 0);
            ESP_LOGI(TAG, "subscribe to topic: %s  successful, msg_id=%d", tb_topic_qr_price, msg_id);
            // msg_id = esp_mqtt_client_subscribe(client, "v1/devices/me/attributes", 0);
            // ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            xEventGroupClearBits(mqtt_conn_event_group, MQTT_CONNECTED_EVENT);
            ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED");
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
            ESP_LOGW(TAG,"TOPIC=%.*s\r\n", event->topic_len, event->topic);
            ESP_LOGW(TAG,"DATA=%.*s\r\n", event->data_len, event->data);
            json_obj = cJSON_ParseWithLength(event->data, event->data_len);
            if(json_obj == NULL) {
                ESP_LOGE(TAG, "Json can not parse");
                break;
            }

            if (topic_matches(event, TBMQ_COMMAND_TOPIC)) {
                handle_payment_command(json_obj);
                cJSON_Delete(json_obj);
                json_obj = NULL;
                break;
            }

            bool is_qr_price_topic = topic_matches(event, tb_topic_qr_price);
            if(!is_qr_price_topic && cJSON_GetObjectItem(json_obj, "price") != NULL) 
            {
                char price_buf[16];
                uint16_t new_price = 0;
                char *task_price = NULL;

                if (!parse_price_item(cJSON_GetObjectItem(json_obj, "price"), price_buf, sizeof(price_buf), &new_price)) {
                    ESP_LOGE(TAG, "Invalid price value");
                    cJSON_Delete(json_obj);
                    break;
                }

                ESP_LOGI(TAG, "Price request received: old=%u new=%u", u16CurPrice, new_price);
                if(new_price != u16CurPrice)
                {                    
                    task_price = (char *)malloc(strlen(price_buf) + 1);
                    if (task_price == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate price task parameter");
                        cJSON_Delete(json_obj);
                        break;
                    }
                    strcpy(task_price, price_buf);

                    // Send new price to device via simulated keypad
                    BaseType_t task_created = xTaskCreate(&change_price_by_vir_keypad,
                                                          "change_price_by_vir_keypad",
                                                          VIRTUAL_KEYPAD_TASK_STACK_SIZE,
                                                          task_price,
                                                          configMAX_PRIORITIES-1,
                                                          NULL);
                    if (task_created != pdPASS) {
                        ESP_LOGE(TAG, "Failed to create change price task");
                        free(task_price);
                        cJSON_Delete(json_obj);
                        break;
                    }

                    // Update current price in RAM/NVS after the keypad task was accepted.
                    u16CurPrice = new_price;
                    setNewPrice(u16CurPrice);
                    ESP_LOGI(TAG, "Change price task created for price %s", price_buf);
                }
                else {
                    ESP_LOGI(TAG, "Price is unchanged, keypad sequence skipped");
                }
                u8_subscribed=true;
            }

            if(is_qr_price_topic)
            {
                char amount_buf[16];
                uint32_t qr_amount = 0;
                char *task_amount = NULL;
                const cJSON *qr_price_item = get_qr_price_item(json_obj);

                if (!parse_qr_amount_item(qr_price_item, amount_buf, sizeof(amount_buf), &qr_amount)) {
                    ESP_LOGE(TAG, "Invalid QR price value");
                    cJSON_Delete(json_obj);
                    break;
                }

                ESP_LOGI(TAG, "QR price command received, raw=%lu, keypad=%s, execute keypad without RS232 validation",
                         (unsigned long)qr_amount,
                         amount_buf);
                task_amount = (char *)malloc(strlen(amount_buf) + 1);
                if (task_amount == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate QR price task parameter");
                    cJSON_Delete(json_obj);
                    break;
                }
                strcpy(task_amount, amount_buf);

                BaseType_t task_created = xTaskCreate(&enter_qr_price_by_vir_keypad,
                                                      "enter_qr_price",
                                                      VIRTUAL_KEYPAD_TASK_STACK_SIZE,
                                                      task_amount,
                                                      configMAX_PRIORITIES-1,
                                                      NULL);
                if (task_created != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create QR price task");
                    free(task_amount);
                    cJSON_Delete(json_obj);
                    break;
                }

                ESP_LOGI(TAG, "QR price task created for amount %s", amount_buf);
            }

            // TODO: should be removed. check the current situation.
            // if(u8_subscribed) //skip the first event after subscribed
            // {
                // OTA upgrade msg. Set the flag OperationMode and reboot
                if(cJSON_GetObjectItem(json_obj, "fw_version") != NULL)
                {
                    int getFwVer=atoi(cJSON_GetObjectItem(json_obj, "fw_version")->valuestring);
                    ESP_LOGI(TAG, "FW version : %d",getFwVer);
                    if(getFwVer>u8FwVerion)
                    {
                        u8FwVerion=getFwVer;
                        setFW_version(u8FwVerion);
                        // update Operation Mode in NVS
                        setOperationMode_version(FW_OTA_MODE);
                        esp_restart();  
                    }
                }  

                if (cJSON_GetObjectItem(json_obj, "deviceID") != NULL) {
                    setDeviceID(cJSON_GetObjectItem(json_obj, "deviceID")->valuestring);
                }
                // End Session msg. Create a thread to do endsession via virtual keyboard
                if(cJSON_GetObjectItem(json_obj, "End_Session") != NULL)
                {
                    int end_session=atoi(cJSON_GetObjectItem(json_obj, "End_Session")->valuestring);
                    ESP_LOGI(TAG, "End_Session request received: %d",end_session);

                    //Execute commands to End current session
                    xTaskCreate(&end_session_by_vir_keypad, 
                                "end_session_by_vir_keypad", 
                                VIRTUAL_KEYPAD_TASK_STACK_SIZE,
                                cJSON_GetObjectItem(json_obj, "End_Session")->valuestring, 
                                configMAX_PRIORITIES-1, 
                                NULL);

                }  
            // } 
            cJSON_Delete(json_obj);
            json_obj = NULL;
            break;
        case MQTT_EVENT_ERROR:
            mqtt_connected = false;
            xEventGroupClearBits(mqtt_conn_event_group, MQTT_CONNECTED_EVENT);
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
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
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    FD_mqtt_event_handler_cb(event_data);
}
static void FD_mqtt_app_start(void)
{
    int client_id_len = snprintf(mqtt_client_id, sizeof(mqtt_client_id), "%s", TBMQ_CLIENT_ID);
    if (client_id_len < 0 || client_id_len >= (int)sizeof(mqtt_client_id)) {
        ESP_LOGE(TAG, "MQTT client ID is invalid or too long");
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = TBMQ_BROKER_URI,
        .credentials.client_id = mqtt_client_id,
        .credentials.authentication.password = TBMQ_PASSWORD,
        .session.disable_clean_session = true,
        .session.keepalive = TBMQ_KEEPALIVE_SEC,
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .network.disable_auto_reconnect = false,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        client,
        ESP_EVENT_ANY_ID,
        FD_mqtt_event_handler,
        NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    ESP_LOGI(TAG,
             "MQTT start requested | broker=%s client_id=%s",
             TBMQ_BROKER_URI,
             mqtt_client_id);
}

void FD_wifi_mqtt_stop_for_config_portal(void)
{
    mqtt_connected = false;
    if (mqtt_conn_event_group != NULL) {
        xEventGroupClearBits(mqtt_conn_event_group, MQTT_CONNECTED_EVENT);
    }

    if (client == NULL) {
        return;
    }

    ESP_LOGW(TAG, "Stopping MQTT before WiFi config portal AP mode");
    esp_err_t err = esp_mqtt_client_stop(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT stop returned: %s", esp_err_to_name(err));
    }
}

static int publish_to_tbmq(const char *topic, const char *msg_payload, uint16_t msg_len)
{
    if (client == NULL || topic == NULL || msg_payload == NULL) {
        ESP_LOGE(TAG, "Invalid MQTT publish arguments");
        return -1;
    }

    if (!mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, skip topic=%s", topic);
        return -1;
    }

    return esp_mqtt_client_publish(client, topic, msg_payload, msg_len, 1, false);
}

static int push_msg(char *msg_payload, uint16_t msg_len) {
    int stat;
    xSemaphoreTake(push_msg_sem, portMAX_DELAY);
    stat = publish_to_tbmq(TBMQ_TELEMETRY_TOPIC, msg_payload, msg_len);
    xSemaphoreGive(push_msg_sem);
    return stat;
}

static int push_completion_event(char *msg_payload, uint16_t msg_len) {
    int stat;
    xSemaphoreTake(push_msg_sem, portMAX_DELAY);
    stat = publish_to_tbmq(TBMQ_EVENT_TOPIC, msg_payload, msg_len);
    xSemaphoreGive(push_msg_sem);
    return stat;
}

void payment_input_switch_update(uint8_t level)
{
    char completed_msg_id[TBMQ_MSG_ID_LEN + 1];
    char completion_payload[160];
    bool has_payment = false;
    bool should_publish_completion = false;

    completed_msg_id[0] = '\0';

    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    has_payment = active_payment_has_msg_id;
    if (has_payment) {
        snprintf(completed_msg_id, sizeof(completed_msg_id), "%s", active_payment_msg_id);

        if (level != 0) {
            if (!active_payment_input_started) {
                ESP_LOGI(TAG, "Payment input switch ACTIVE: dispense started msg_id=%s",
                         completed_msg_id);
            }
            active_payment_input_started = true;
        } else if (active_payment_input_started) {
            active_payment_input_started = false;
            should_publish_completion = true;
        }
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreGive(payment_ctx_sem);
    }

    if (!has_payment) {
        ESP_LOGD(TAG, "Payment input switch level=%u ignored: no active set_qr_money",
                 level);
        return;
    }

    if (level == 0 && !should_publish_completion) {
        ESP_LOGD(TAG, "Payment input switch IDLE ignored: dispense was not started msg_id=%s",
                 completed_msg_id);
        return;
    }

    if (!should_publish_completion) {
        return;
    }

    snprintf(completion_payload,
             sizeof(completion_payload),
             "{\"ts\":%lld,\"event\":\"completed\",\"msg_id\":\"%s\"}",
             protocol_timestamp_seconds(),
             completed_msg_id);
    ESP_LOGI(TAG, "Payment completed by INPUT_SWITCH GPIO6, payload=%s", completion_payload);
    int stat = push_completion_event(completion_payload, strlen(completion_payload));
    ESP_LOGI(TAG, "completion event publish stat=%d", stat);
    clear_active_payment_msg_id(completed_msg_id);
}

static int push_special_action_msg(char *msg_payload, uint16_t msg_len) {
    int stat;
    xSemaphoreTake(push_msg_sem, portMAX_DELAY);
    stat = publish_to_tbmq(TBMQ_ACK_TOPIC, msg_payload, msg_len);
    xSemaphoreGive(push_msg_sem);
    return stat;
}

//Send heart beat msg every 5min to maintain connection with TB server
//Send heart beat msg every 5min to maintain connection with TB server
static int push_heartbeat_msg(char *msg_payload, uint16_t msg_len)
{
    int stat;
    xSemaphoreTake(push_msg_sem, portMAX_DELAY);
    stat = publish_to_tbmq(TBMQ_TELEMETRY_TOPIC, msg_payload, msg_len);
    xSemaphoreGive(push_msg_sem);
    return stat;
}
//int currentPrice=0;    
static void push_msg_to_broker(void *arg) {
    wifi_ap_record_t ap;
    int stat;
    char *c_data = NULL;
    char *liter = (char *)malloc(11); memset(liter, 0, 11); // liter[11] = '/0';
    char *money = (char *)malloc(11); memset(money, 0, 11); // money[11] = '/0';
    char *price = (char *)malloc(7);  memset(price, 0, 7);  // price[7] = '/0';
    char payment_msg_id[TBMQ_MSG_ID_LEN + 1];
    for(;;) {          
        if (xQueueReceive(uplink_queue, &c_data, portMAX_DELAY) == pdTRUE)
        {
            if(c_data[0]==0xF0)//process for end session pressed
            {
                ESP_LOGI(TAG, "Sending end session signal to server");
                sprintf(payload,"{\"DevID\": \"%s\", \"fuel_type\": \"%s\",\"Client_End_Session\":%d}",deviceID,"diesel",1);
                ESP_LOGD(TAG, "Payload: %s", payload);
                stat = push_special_action_msg(payload,strlen(payload));
                ESP_LOGI(TAG, "sent publish, stat=%d", stat);
            } else {
                ESP_LOGI(TAG, "Data: %.42s", c_data);
                memset(liter, 0, 11);
                memset(money, 0, 11);
                memset(price, 0, 7);
                strncpy(liter, c_data, 10);
                strncpy(money, c_data+10, 10);
                strncpy(price, c_data+20, 6);
                int final_liter = atoi(liter);
                int final_money = atoi(money);
                int final_price = atoi(price);
                bool has_payment_msg_id = get_active_payment_msg_id(payment_msg_id,
                                                                    sizeof(payment_msg_id));
                                
                esp_wifi_sta_get_ap_info(&ap);
                ESP_LOGD(TAG, "Free heap size: %ld bytes", esp_get_minimum_free_heap_size());
                // printf("c_data[39] = %d\n",c_data[39]);
                // if(c_data[39] == 5)
                //     continue;
                // if int(byte_hex[-3]) != prev_stat and int(byte_hex[-3]) == 4:
                //printf("Setprice value : %d\n", bset_price);
                // if(!bset_price) {
#if 0
                lit = rand() % 100;
                sprintf(payload, "{\"DevID\": \"fs_node_1201440612_%d\", \"fuel_type\": \"%s\", \"Liter\": %d, \"Money\": %d, \"Price\": %d, \"RSSI\": %d}", 
                        3, 
                        "diesel", 
                        lit, 
                        lit * atoi(price), 
                        atoi(price),  
                        ap.rssi);
#else
                if (has_payment_msg_id) {
                    snprintf(payload,
                             sizeof(payload),
                             "{\"ts\":%lld,\"msg_id\":\"%s\",\"DevID\":\"%s\",\"fuel_type\":\"%s\",\"liter\":%d,\"money\":%d,\"price\":%d,\"RSSI\":%d,\"data\":{\"price\":%d,\"money\":%d,\"liter\":%d,\"device_status\":\"ok\"}}",
                             protocol_timestamp_seconds(),
                             payment_msg_id,
                             deviceID,
                             "diesel",
                             final_liter,
                             final_money,
                             final_price,
                             ap.rssi,
                             final_price,
                             final_money,
                             final_liter);
                } else {
                    snprintf(payload,
                             sizeof(payload),
                             "{\"ts\":%lld,\"DevID\":\"%s\",\"fuel_type\":\"%s\",\"liter\":%d,\"money\":%d,\"price\":%d,\"RSSI\":%d,\"data\":{\"price\":%d,\"money\":%d,\"liter\":%d,\"device_status\":\"ok\"}}",
                             protocol_timestamp_seconds(),
                             deviceID,
                             "diesel",
                             final_liter,
                             final_money,
                             final_price,
                             ap.rssi,
                             final_price,
                             final_money,
                             final_liter);
                }
#endif
                // For testing:
                // idx++;
                // sprintf(payload, "{\"DevID\": \"fs_node_1201440612_%d\", \"fuel_type\": \"%s\", \"Liter\": %d, \"Money\": %d, \"Price\": %d, \"RSSI\": %d}", 1, "gasoline", idx, idx, idx, ap.rssi);
                ESP_LOGD(TAG, "Payload: %s", payload);
                // stat = esp_mqtt_client_publish(client, "/station/data", payload, strlen(payload), 0, false);
                stat = push_msg(payload, strlen(payload));
                ESP_LOGI(TAG, "telemetry publish stat=%d", stat);
                // vTaskDelay(pdMS_TO_TICKS(5000));
/*            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
*/          }

        }
    }
}

static void ping_tb(void *arg) {
    int stat;
    wifi_ap_record_t ap;
    char payment_msg_id[TBMQ_MSG_ID_LEN + 1];
    //wifi_ap_record_t ap;
    for(;;) {
        //esp_wifi_sta_get_ap_info(&ap);
        //printf("Free heap size: %d bytes\n", esp_get_minimum_free_heap_size());
 
        // stat = esp_mqtt_client_publish(client, "/station/data", payload_ping, strlen(payload_ping), 0, false);
        esp_wifi_sta_get_ap_info(&ap);
        if (get_active_payment_msg_id(payment_msg_id, sizeof(payment_msg_id))) {
            snprintf(payload,
                     sizeof(payload),
                     "{\"ts\":%lld,\"msg_id\":\"%s\",\"DevID\":\"%s\",\"fuel_type\":\"%s\",\"keep_alive\":%d,\"RSSI\":%d}",
                     protocol_timestamp_seconds(),
                     payment_msg_id,
                     deviceID,
                     "diesel",
                     1,
                     ap.rssi);
        } else {
            snprintf(payload,
                     sizeof(payload),
                     "{\"ts\":%lld,\"DevID\":\"%s\",\"fuel_type\":\"%s\",\"keep_alive\":%d,\"RSSI\":%d}",
                     protocol_timestamp_seconds(),
                     deviceID,
                     "diesel",
                     1,
                     ap.rssi);
        }
        stat = push_heartbeat_msg(payload,strlen(payload));
        if (stat >= 0) {
            ESP_LOGI(TAG, "Ping MSG published, msg_id=%d", stat);
        } else {
            ESP_LOGW(TAG, "Ping MSG skipped or failed");
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
void FD_wifi_mqtt_config(void)
{

    mqtt_conn_event_group = xEventGroupCreate();
    if (mqtt_conn_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT event group");
        return;
    }

    push_msg_sem = xSemaphoreCreateMutex();
    if (push_msg_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT publish mutex");
        return;
    }

    payment_ctx_sem = xSemaphoreCreateMutex();
    if (payment_ctx_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create payment context mutex");
        return;
    }

    json_obj = cJSON_CreateObject();
    //const esp_partition_t *running_partition = esp_ota_get_running_partition();
    //initialise_wifi(running_partition->label);

    FD_mqtt_app_start();
    if (client == NULL) {
        ESP_LOGE(TAG, "MQTT setup failed");
        return;
    }

    rs232_config();
    ESP_LOGI(TAG, "RS232 config done\n");
    xTaskCreate(push_msg_to_broker, "push_msg_to_broker", 4096, NULL, 5, NULL);
    xTaskCreate(ping_tb, "ping_tb", 4096, NULL, 5, NULL);
}
