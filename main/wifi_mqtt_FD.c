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
#define TBMQ_KEEPALIVE_SEC 60
#define TBMQ_MSG_ID_LEN 32
#define TBMQ_TOPIC_MAX_LEN 128
#define VIRTUAL_KEYPAD_TASK_STACK_SIZE 6144
#define RS232_RECEIPT_WAIT_MS 2000
#define RS232_RECEIPT_POLL_MS 50
#define PAYMENT_RECEIPT_QUEUE_LENGTH 2
#define PAYMENT_RECEIPT_TASK_STACK_SIZE 4096

typedef enum {
    PAYMENT_COMMAND_NONE = 0,
    PAYMENT_COMMAND_MONEY,
    PAYMENT_COMMAND_LITTER,
} payment_command_type_t;

typedef struct {
    char msg_id[TBMQ_MSG_ID_LEN + 1];
    payment_command_type_t type;
    uint16_t configured_price;
    bool has_msg_id;
} payment_completion_context_t;

//extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
//extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

const int MQTT_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t mqtt_conn_event_group;
static SemaphoreHandle_t push_msg_sem;
static SemaphoreHandle_t payment_ctx_sem;
static QueueHandle_t payment_receipt_queue;
static esp_mqtt_client_handle_t client;
static bool mqtt_connected;
static char mqtt_client_id[96];
static char tbmq_command_topic[TBMQ_TOPIC_MAX_LEN];
static char tbmq_telemetry_topic[TBMQ_TOPIC_MAX_LEN];
static char tbmq_ack_topic[TBMQ_TOPIC_MAX_LEN];
static char tbmq_event_topic[TBMQ_TOPIC_MAX_LEN];
static char active_payment_msg_id[TBMQ_MSG_ID_LEN + 1];
static payment_command_type_t active_payment_type;
static uint32_t active_payment_money;
static uint32_t active_payment_litter;
static bool active_payment_active;
static bool active_payment_has_msg_id;
static bool active_payment_input_started;
static bool active_payment_money_keypad_done;
cJSON *json_obj;
char payload[512];
//int currentPrice=0;

static int publish_to_tbmq(const char *topic, const char *msg_payload, uint16_t msg_len);

static const char *safe_gw_pay_id(void)
{
    return (gwPayID != NULL && gwPayID[0] != '\0') ? gwPayID : "gw_pay_001";
}

static const char *safe_device_id(void)
{
    return (deviceID != NULL && deviceID[0] != '\0') ? deviceID : "node_pay_001";
}

static const char *safe_mqtt_client_id(void)
{
    return (mqttClientID != NULL && mqttClientID[0] != '\0') ? mqttClientID : "node_qr_001";
}

static bool build_tbmq_topics(void)
{
    const char *gw_id = safe_gw_pay_id();
    const char *node_id = safe_device_id();
    int ret;

    ret = snprintf(tbmq_command_topic,
                   sizeof(tbmq_command_topic),
                   "tbmq/payment/%s/%s/command",
                   gw_id,
                   node_id);
    if (ret < 0 || ret >= (int)sizeof(tbmq_command_topic)) {
        return false;
    }

    ret = snprintf(tbmq_telemetry_topic,
                   sizeof(tbmq_telemetry_topic),
                   "tbmq/payment/%s/%s/telemetry",
                   gw_id,
                   node_id);
    if (ret < 0 || ret >= (int)sizeof(tbmq_telemetry_topic)) {
        return false;
    }

    ret = snprintf(tbmq_ack_topic,
                   sizeof(tbmq_ack_topic),
                   "tbmq/payment/%s/%s/ack",
                   gw_id,
                   node_id);
    if (ret < 0 || ret >= (int)sizeof(tbmq_ack_topic)) {
        return false;
    }

    ret = snprintf(tbmq_event_topic,
                   sizeof(tbmq_event_topic),
                   "tbmq/payment/%s/%s/event",
                   gw_id,
                   node_id);
    if (ret < 0 || ret >= (int)sizeof(tbmq_event_topic)) {
        return false;
    }

    ESP_LOGI(TAG, "TBMQ topics: command=%s telemetry=%s ack=%s event=%s",
             tbmq_command_topic,
             tbmq_telemetry_topic,
             tbmq_ack_topic,
             tbmq_event_topic);
    return true;
}

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

static void set_active_payment_context(const char *msg_id,
                                       payment_command_type_t type,
                                       uint32_t money,
                                       uint32_t litter)
{
    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    if (msg_id != NULL && msg_id[0] != '\0') {
        snprintf(active_payment_msg_id, sizeof(active_payment_msg_id), "%s", msg_id);
        active_payment_has_msg_id = true;
    } else {
        active_payment_msg_id[0] = '\0';
        active_payment_has_msg_id = false;
    }

    active_payment_type = type;
    active_payment_money = money;
    active_payment_litter = litter;
    active_payment_active = true;
    active_payment_input_started = false;
    active_payment_money_keypad_done = !virtual_keypad_is_enabled();

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

static void clear_active_payment_context(const char *completed_msg_id)
{
    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    if (active_payment_active &&
        (completed_msg_id == NULL ||
         !active_payment_has_msg_id ||
         strcmp(active_payment_msg_id, completed_msg_id) == 0)) {
        active_payment_msg_id[0] = '\0';
        active_payment_type = PAYMENT_COMMAND_NONE;
        active_payment_money = 0;
        active_payment_litter = 0;
        active_payment_active = false;
        active_payment_has_msg_id = false;
        active_payment_input_started = false;
        active_payment_money_keypad_done = false;
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreGive(payment_ctx_sem);
    }
}

static bool claim_active_payment_context(payment_completion_context_t *context)
{
    bool claimed = false;

    if (context == NULL) {
        return false;
    }

    memset(context, 0, sizeof(*context));

    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    /* A receipt can complete only a transaction that has actually started.
     * This rejects stale printer data received after a command but before
     * INPUT_SWITCH has ever reached the ACTIVE level. */
    if (active_payment_active && active_payment_input_started) {
        context->type = active_payment_type;
        context->configured_price = u16CurPrice;
        context->has_msg_id = active_payment_has_msg_id;
        if (active_payment_has_msg_id) {
            snprintf(context->msg_id,
                     sizeof(context->msg_id),
                     "%s",
                     active_payment_msg_id);
        }

        /* Claim and clear atomically.  This guarantees that the RS232-ready
         * path and the INPUT_SWITCH timeout path cannot publish twice. */
        active_payment_msg_id[0] = '\0';
        active_payment_type = PAYMENT_COMMAND_NONE;
        active_payment_money = 0;
        active_payment_litter = 0;
        active_payment_active = false;
        active_payment_has_msg_id = false;
        active_payment_input_started = false;
        active_payment_money_keypad_done = false;
        claimed = true;
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreGive(payment_ctx_sem);
    }

    return claimed;
}

bool payment_control_switch_can_follow_input(void)
{
    bool can_follow = true;

    if (!virtual_keypad_is_enabled()) {
        return true;
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    can_follow = active_payment_active && active_payment_money_keypad_done;

    if (payment_ctx_sem != NULL) {
        xSemaphoreGive(payment_ctx_sem);
    }

    return can_follow;
}

void payment_set_qr_money_keypad_done(void)
{
    bool should_refresh_control = false;

    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    if (active_payment_active) {
        active_payment_money_keypad_done = true;
        should_refresh_control = true;
        ESP_LOGI(TAG, "%s virtual keypad done, control switch can follow input now msg_id=%s",
                 active_payment_type == PAYMENT_COMMAND_LITTER ? "set_qr_litter" : "set_qr_money",
                 active_payment_has_msg_id ? active_payment_msg_id : "");
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreGive(payment_ctx_sem);
    }

    if (should_refresh_control) {
        input_switch_refresh_control_switch();
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

#if MAIN_RS232
    keypad_amount = raw_amount;
#else
    keypad_amount = raw_amount / 100;
#endif
    snprintf(amount_buf, amount_buf_size, "%lu", (unsigned long)keypad_amount);
    if (raw_amount_value != NULL) {
        *raw_amount_value = raw_amount;
    }
    return true;
}

static bool parse_qr_litter_item(const cJSON *item, char *litter_buf, size_t litter_buf_size, uint32_t *raw_litter_value)
{
    char raw_litter_buf[16];
    uint32_t raw_litter = 0;
    uint32_t keypad_litter = 0;

    if (!parse_number_item(item, raw_litter_buf, sizeof(raw_litter_buf), UINT32_MAX / 10, &raw_litter)) {
        return false;
    }

    keypad_litter = raw_litter * 10;
    snprintf(litter_buf, litter_buf_size, "%lu", (unsigned long)keypad_litter);
    if (raw_litter_value != NULL) {
        *raw_litter_value = raw_litter;
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

    publish_to_tbmq(tbmq_ack_topic, ack_payload, strlen(ack_payload));
}

static esp_err_t start_change_price_sequence(const char *price_buf, uint16_t new_price, bool *changed)
{
    char *task_price = NULL;

    if (changed != NULL) {
        *changed = false;
    }

    ESP_LOGI(TAG, "Price request received: old=%u new=%u", u16CurPrice, new_price);

    task_price = strdup(price_buf);
    if (task_price == NULL) {
        ESP_LOGE(TAG, "Failed to allocate price task parameter");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreate(change_price_by_vir_keypad,
                                          "change_price_by_vir_keypad",
                                          VIRTUAL_KEYPAD_TASK_STACK_SIZE,
                                          task_price,
                                          configMAX_PRIORITIES - 1,
                                          NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create change price task");
        free(task_price);
        return ESP_FAIL;
    }

    u16CurPrice = new_price;
    setNewPrice(u16CurPrice);
    ESP_LOGI(TAG, "Change price task created for price %s", price_buf);

    if (changed != NULL) {
        *changed = true;
    }

    return ESP_OK;
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
        msg_id[0] = '\0';
    }

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

    if (strcmp(cmd, "enable_virtual_key") == 0) {
        const cJSON *enable_item = cJSON_GetObjectItemCaseSensitive(param, "enable_virtual_key");
        char enable_buf[4];
        uint32_t enable_value = 0;

        if (!parse_number_item(enable_item,
                               enable_buf,
                               sizeof(enable_buf),
                               1,
                               &enable_value)) {
            ESP_LOGE(TAG, "Invalid param.enable_virtual_key, expected 0 or 1");
            publish_payment_ack(cmd, "error", "invalid enable_virtual_key", msg_id);
            return;
        }

        if (enable_value != 0) {
            virtual_keypad_set_external_physical(false);
            keypad_master_scan_enable_for_virtual_keypad();
            ESP_LOGW(TAG, "Virtual keypad enabled by MQTT command, msg_id=%s", msg_id);
        } else {
            virtual_keypad_set_external_physical(true);
            keypad_master_scan_disable_for_external_physical_keypad();
            ESP_LOGW(TAG, "Virtual keypad disabled by MQTT command, external physical keypad enabled, msg_id=%s",
                     msg_id);
        }

        publish_payment_ack(cmd, "ok", NULL, msg_id);
        return;
    }

    if (strcmp(cmd, "set_price") == 0) {
        const cJSON *price_item = cJSON_GetObjectItemCaseSensitive(param, "price");
        char price_buf[16];
        uint16_t new_price = 0;
        bool price_changed = false;
        esp_err_t err;

        if (!parse_price_item(price_item, price_buf, sizeof(price_buf), &new_price)) {
            ESP_LOGE(TAG, "Invalid param.price");
            publish_payment_ack(cmd, "error", "invalid price", msg_id);
            return;
        }

        err = start_change_price_sequence(price_buf, new_price, &price_changed);
        if (err != ESP_OK) {
            publish_payment_ack(cmd,
                                "error",
                                err == ESP_ERR_NO_MEM ? "out of memory" : "task create failed",
                                msg_id);
            return;
        }

        ESP_LOGI(TAG,
                 "Payment command accepted: cmd=%s price=%u changed=%d msg_id=%s",
                 cmd,
                 new_price,
                 price_changed ? 1 : 0,
                 msg_id);
        publish_payment_ack(cmd, "ok", NULL, msg_id);
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

        set_active_payment_context(msg_id,
                                   PAYMENT_COMMAND_MONEY,
                                   qr_money,
                                   0);
        ESP_LOGI(TAG,
                 "Payment command accepted: cmd=%s qr_money=%lu keypad=%s msg_id=%s",
                 cmd,
                 (unsigned long)qr_money,
                 amount_buf,
                 msg_id);
        publish_payment_ack(cmd, "ok", NULL, msg_id);
        return;
    }

    if (strcmp(cmd, "set_qr_litter") == 0) {
        const cJSON *qr_litter_item = cJSON_GetObjectItemCaseSensitive(param, "qr_litter");
        char litter_buf[16];
        uint32_t qr_litter = 0;

        if (!parse_qr_litter_item(qr_litter_item, litter_buf, sizeof(litter_buf), &qr_litter)) {
            ESP_LOGE(TAG, "Invalid param.qr_litter");
            publish_payment_ack(cmd, "error", "fail_config_litter", msg_id);
            return;
        }

        char *task_litter = strdup(litter_buf);
        if (task_litter == NULL) {
            ESP_LOGE(TAG, "Failed to allocate QR litter task parameter");
            publish_payment_ack(cmd, "error", "fail_config_litter", msg_id);
            return;
        }

        BaseType_t task_created = xTaskCreate(enter_qr_litter_by_vir_keypad,
                                              "enter_qr_litter",
                                              VIRTUAL_KEYPAD_TASK_STACK_SIZE,
                                              task_litter,
                                              configMAX_PRIORITIES - 1,
                                              NULL);
        if (task_created != pdPASS) {
            free(task_litter);
            ESP_LOGE(TAG, "Failed to create QR litter keypad task");
            publish_payment_ack(cmd, "error", "fail_config_litter", msg_id);
            return;
        }

        set_active_payment_context(msg_id,
                                   PAYMENT_COMMAND_LITTER,
                                   0,
                                   qr_litter);
        ESP_LOGI(TAG,
                 "Payment command accepted: cmd=%s qr_litter=%lu keypad=%s msg_id=%s",
                 cmd,
                 (unsigned long)qr_litter,
                 litter_buf,
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

        clear_active_payment_context(msg_id);
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
            msg_id = esp_mqtt_client_subscribe(client, tbmq_command_topic, 1);
            ESP_LOGI(TAG, "subscribe to topic: %s QoS=1, msg_id=%d", tbmq_command_topic, msg_id);

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

            if (topic_matches(event, tbmq_command_topic)) {
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
                esp_err_t price_err;

                if (!parse_price_item(cJSON_GetObjectItem(json_obj, "price"), price_buf, sizeof(price_buf), &new_price)) {
                    ESP_LOGE(TAG, "Invalid price value");
                    cJSON_Delete(json_obj);
                    break;
                }

                price_err = start_change_price_sequence(price_buf, new_price, NULL);
                if (price_err != ESP_OK) {
                    cJSON_Delete(json_obj);
                    break;
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
    if (!build_tbmq_topics()) {
        ESP_LOGE(TAG, "Failed to build TBMQ topics from gwPayID/deviceID");
        return;
    }

    int client_id_len = snprintf(mqtt_client_id,
                                 sizeof(mqtt_client_id),
                                 "%s",
                                 safe_mqtt_client_id());
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
    stat = publish_to_tbmq(tbmq_telemetry_topic, msg_payload, msg_len);
    xSemaphoreGive(push_msg_sem);
    return stat;
}

static int push_completion_event(char *msg_payload, uint16_t msg_len) {
    int stat;
    xSemaphoreTake(push_msg_sem, portMAX_DELAY);
    stat = publish_to_tbmq(tbmq_event_topic, msg_payload, msg_len);
    xSemaphoreGive(push_msg_sem);
    return stat;
}

static bool publish_payment_completion(const rs232_receipt_data_t *receipt_data,
                                       bool missing_receipt,
                                       const char *source)
{
    payment_completion_context_t completed = {0};
    char completion_payload[224];

    if (!claim_active_payment_context(&completed)) {
        ESP_LOGD(TAG,
                 "Payment completion from %s ignored: no active transaction",
                 source != NULL ? source : "unknown");
        return false;
    }

    if (!missing_receipt && receipt_data != NULL) {
        ESP_LOGI(TAG,
                 "Use RS232 receipt immediately: money=%lu liter=%lu.%03lu price=%lu",
                 (unsigned long)receipt_data->money,
                 (unsigned long)(receipt_data->liter_milliliters / 1000U),
                 (unsigned long)(receipt_data->liter_milliliters % 1000U),
                 (unsigned long)receipt_data->price);

        if (completed.has_msg_id) {
            snprintf(completion_payload,
                     sizeof(completion_payload),
                     "{\"ts\":%lld,\"event\":\"completed\",\"msg_id\":\"%s\",\"money\":%lu,\"liter\":%lu.%03lu,\"price\":%lu}",
                     protocol_timestamp_seconds(),
                     completed.msg_id,
                     (unsigned long)receipt_data->money,
                     (unsigned long)(receipt_data->liter_milliliters / 1000U),
                     (unsigned long)(receipt_data->liter_milliliters % 1000U),
                     (unsigned long)receipt_data->price);
        } else {
            snprintf(completion_payload,
                     sizeof(completion_payload),
                     "{\"ts\":%lld,\"event\":\"completed\",\"money\":%lu,\"liter\":%lu.%03lu,\"price\":%lu}",
                     protocol_timestamp_seconds(),
                     (unsigned long)receipt_data->money,
                     (unsigned long)(receipt_data->liter_milliliters / 1000U),
                     (unsigned long)(receipt_data->liter_milliliters % 1000U),
                     (unsigned long)receipt_data->price);
        }
    } else {
        ESP_LOGE(TAG,
                 "RS232 receipt missing for %s after %u ms: mark transaction failed with money=0 liter=0",
                 completed.type == PAYMENT_COMMAND_LITTER ? "set_qr_litter" : "set_qr_money",
                 (unsigned int)RS232_RECEIPT_WAIT_MS);

        if (completed.has_msg_id) {
            snprintf(completion_payload,
                     sizeof(completion_payload),
                     "{\"ts\":%lld,\"event\":\"completed\",\"msg_id\":\"%s\",\"result\":\"error\",\"description\":\"missing_rs232_receipt\",\"money\":0,\"liter\":0,\"price\":%u}",
                     protocol_timestamp_seconds(),
                     completed.msg_id,
                     (unsigned int)completed.configured_price);
        } else {
            snprintf(completion_payload,
                     sizeof(completion_payload),
                     "{\"ts\":%lld,\"event\":\"completed\",\"result\":\"error\",\"description\":\"missing_rs232_receipt\",\"money\":0,\"liter\":0,\"price\":%u}",
                     protocol_timestamp_seconds(),
                     (unsigned int)completed.configured_price);
        }
    }

    ESP_LOGI(TAG,
             "Payment completion triggered by %s, payload=%s",
             source != NULL ? source : "unknown",
             completion_payload);
    int stat = push_completion_event(completion_payload, strlen(completion_payload));
    ESP_LOGI(TAG, "completion event publish stat=%d", stat);
    return true;
}

void payment_rs232_receipt_ready(const rs232_receipt_data_t *result)
{
    if (result == NULL || payment_receipt_queue == NULL) {
        ESP_LOGW(TAG, "RS232 receipt notification ignored: MQTT receipt queue is not ready");
        return;
    }

    if (xQueueSend(payment_receipt_queue, result, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RS232 receipt notification queue is full");
    }
}

static void payment_receipt_task(void *arg)
{
    (void)arg;
    rs232_receipt_data_t receipt_data;

    for (;;) {
        if (xQueueReceive(payment_receipt_queue,
                          &receipt_data,
                          portMAX_DELAY) == pdTRUE) {
            (void)publish_payment_completion(&receipt_data,
                                             false,
                                             "RS232 receipt");
        }
    }
}

void payment_input_switch_update(uint8_t level)
{
    char completed_msg_id[TBMQ_MSG_ID_LEN + 1];
    rs232_receipt_data_t receipt_data = {0};
    payment_command_type_t active_type = PAYMENT_COMMAND_NONE;
    bool has_payment = false;
    bool should_check_receipt = false;
    bool should_reset_receipt = false;

    completed_msg_id[0] = '\0';

    if (payment_ctx_sem != NULL) {
        xSemaphoreTake(payment_ctx_sem, portMAX_DELAY);
    }

    has_payment = active_payment_active;
    if (has_payment) {
        active_type = active_payment_type;
        if (active_payment_has_msg_id) {
            snprintf(completed_msg_id, sizeof(completed_msg_id), "%s", active_payment_msg_id);
        }

        if (level != 0) {
            if (!active_payment_input_started) {
                ESP_LOGI(TAG, "Payment input switch ACTIVE: dispense started type=%d msg_id=%s",
                         active_type,
                         completed_msg_id);
                should_reset_receipt = true;
            }
            active_payment_input_started = true;
        } else if (active_payment_input_started) {
            should_check_receipt = true;
        }
    }

    if (payment_ctx_sem != NULL) {
        xSemaphoreGive(payment_ctx_sem);
    }

    if (should_reset_receipt) {
        rs232_receipt_reset();
    }

    if (!has_payment) {
        ESP_LOGD(TAG, "Payment input switch level=%u ignored: no active set_qr_money/set_qr_litter",
                 level);
        return;
    }

    if (level == 0 && !should_check_receipt) {
        ESP_LOGD(TAG, "Payment input switch IDLE ignored: dispense was not started msg_id=%s",
                 completed_msg_id);
        return;
    }

    if (!should_check_receipt) {
        return;
    }

    for (uint32_t elapsed_ms = 0; elapsed_ms <= RS232_RECEIPT_WAIT_MS;
         elapsed_ms += RS232_RECEIPT_POLL_MS) {
        if (rs232_receipt_get(&receipt_data)) {
            /* Usually the RS232 queue task has already sent this event before
             * the handle is returned.  Calling the common function is safe:
             * the atomic context claim prevents duplicate publication. */
            (void)publish_payment_completion(&receipt_data,
                                             false,
                                             "INPUT_SWITCH receipt fallback");
            return;
        }

        if (elapsed_ms < RS232_RECEIPT_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(RS232_RECEIPT_POLL_MS));
        }
    }

    (void)publish_payment_completion(NULL,
                                     true,
                                     "INPUT_SWITCH timeout");
}

static int push_special_action_msg(char *msg_payload, uint16_t msg_len) {
    int stat;
    xSemaphoreTake(push_msg_sem, portMAX_DELAY);
    stat = publish_to_tbmq(tbmq_ack_topic, msg_payload, msg_len);
    xSemaphoreGive(push_msg_sem);
    return stat;
}

//Send heart beat msg every 5min to maintain connection with TB server
//Send heart beat msg every 5min to maintain connection with TB server
static int push_heartbeat_msg(char *msg_payload, uint16_t msg_len)
{
    int stat;
    xSemaphoreTake(push_msg_sem, portMAX_DELAY);
    stat = publish_to_tbmq(tbmq_telemetry_topic, msg_payload, msg_len);
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
                             "{\"ts\":%lld,\"msg_id\":\"%s\",\"DevID\":\"%s\",\"fuel_type\":\"%s\",\"version\":%u,\"liter\":%d,\"money\":%d,\"price\":%d,\"RSSI\":%d,\"data\":{\"price\":%d,\"money\":%d,\"liter\":%d,\"device_status\":\"ok\"}}",
                             protocol_timestamp_seconds(),
                             payment_msg_id,
                             deviceID,
                             "diesel",
                             (unsigned int)u8FwVerion,
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
                             "{\"ts\":%lld,\"DevID\":\"%s\",\"fuel_type\":\"%s\",\"version\":%u,\"liter\":%d,\"money\":%d,\"price\":%d,\"RSSI\":%d,\"data\":{\"price\":%d,\"money\":%d,\"liter\":%d,\"device_status\":\"ok\"}}",
                             protocol_timestamp_seconds(),
                             deviceID,
                             "diesel",
                             (unsigned int)u8FwVerion,
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
    int enable_virtual_key = 0;
    //wifi_ap_record_t ap;
    for(;;) {
        //esp_wifi_sta_get_ap_info(&ap);
        //printf("Free heap size: %d bytes\n", esp_get_minimum_free_heap_size());
 
        // stat = esp_mqtt_client_publish(client, "/station/data", payload_ping, strlen(payload_ping), 0, false);
        esp_wifi_sta_get_ap_info(&ap);
        enable_virtual_key = virtual_keypad_is_enabled() ? 1 : 0;
        if (get_active_payment_msg_id(payment_msg_id, sizeof(payment_msg_id))) {
            snprintf(payload,
                     sizeof(payload),
                     "{\"ts\":%lld,\"msg_id\":\"%s\",\"DevID\":\"%s\",\"fuel_type\":\"%s\",\"keep_alive\":%d,\"RSSI\":%d,\"enable_virtual_key\":%d,\"version\":%u,\"price\":%u}",
                     protocol_timestamp_seconds(),
                     payment_msg_id,
                     deviceID,
                     "diesel",
                     1,
                     ap.rssi,
                     enable_virtual_key,
                     (unsigned int)u8FwVerion,
                     (unsigned int)u16CurPrice);
        } else {
            snprintf(payload,
                     sizeof(payload),
                     "{\"ts\":%lld,\"DevID\":\"%s\",\"fuel_type\":\"%s\",\"keep_alive\":%d,\"RSSI\":%d,\"enable_virtual_key\":%d,\"version\":%u,\"price\":%u}",
                     protocol_timestamp_seconds(),
                     deviceID,
                     "diesel",
                     1,
                     ap.rssi,
                     enable_virtual_key,
                     (unsigned int)u8FwVerion,
                     (unsigned int)u16CurPrice);
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

    payment_receipt_queue = xQueueCreate(PAYMENT_RECEIPT_QUEUE_LENGTH,
                                         sizeof(rs232_receipt_data_t));
    if (payment_receipt_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create RS232 payment receipt queue");
        return;
    }

    BaseType_t receipt_task_created = xTaskCreate(payment_receipt_task,
                                                   "payment_receipt",
                                                   PAYMENT_RECEIPT_TASK_STACK_SIZE,
                                                   NULL,
                                                   5,
                                                   NULL);
    if (receipt_task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RS232 payment receipt task");
        vQueueDelete(payment_receipt_queue);
        payment_receipt_queue = NULL;
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
    virtual_keypad_boot_clear();
    xTaskCreate(push_msg_to_broker, "push_msg_to_broker", 4096, NULL, 5, NULL);
    xTaskCreate(ping_tb, "ping_tb", 4096, NULL, 5, NULL);
    ota_start_github_version_check();
}
