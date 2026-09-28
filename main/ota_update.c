#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_common_interfaces.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "OTA"

#define OTA_GITHUB_VERSION_URL \
    "https://raw.githubusercontent.com/kenhkythuat/automatic_fuel_pump/read_rs232_printer/releases/esp32s3/version.json"
#define OTA_DEFAULT_FIRMWARE_URL \
    "https://raw.githubusercontent.com/kenhkythuat/automatic_fuel_pump/read_rs232_printer/releases/esp32s3/atc_wifi_fw.bin"
#define OTA_VERSION_CHECK_INTERVAL_MS 30000
#define OTA_HTTP_TIMEOUT_MS 10000
#define OTA_VERSION_JSON_MAX_SIZE 1024
#define OTA_URL_MAX_SIZE 256
#define OTA_CHECK_TASK_STACK_SIZE 6144
#define OTA_UPDATE_TASK_STACK_SIZE 8192

static volatile bool ota_in_progress;
static volatile bool ota_version_check_started;
static uint8_t pending_fw_build;

static void set_operation_mode(uint8_t operation_mode)
{
    esp_err_t err;
    nvs_handle nodeconfig_hdl = 0;

    err = nvs_open("nodeconfig", NVS_READWRITE, &nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    err = nvs_set_u8(nodeconfig_hdl, "OperationMode", operation_mode);
    ESP_ERROR_CHECK(err);
    err = nvs_commit(nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    nvs_close(nodeconfig_hdl);
}

static void set_fw_version(uint8_t fw_version)
{
    esp_err_t err;
    nvs_handle nodeconfig_hdl = 0;

    err = nvs_open("nodeconfig", NVS_READWRITE, &nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    err = nvs_set_u8(nodeconfig_hdl, "fwVerion", fw_version);
    ESP_ERROR_CHECK(err);
    err = nvs_commit(nodeconfig_hdl);
    ESP_ERROR_CHECK(err);
    nvs_close(nodeconfig_hdl);
}

void ota_mark_app_valid_after_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "Running OTA app marked valid, rollback cancelled");
        } else {
            ESP_LOGE(TAG, "Failed to mark running OTA app valid");
        }
    }
}

static esp_err_t validate_image_header(esp_app_desc_t *new_app_info)
{
    if (new_app_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
    }

    ESP_LOGI(TAG, "New app version: %s", new_app_info->version);
    return ESP_OK;
}

static void update_task(void *pv_parameter)
{
    char *fw_update_url = (char *)pv_parameter;
    esp_err_t ota_finish_err = ESP_OK;

    if (fw_update_url == NULL) {
        ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t ota_client_config = {
        .url = fw_update_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &ota_client_config,
    };

    ESP_LOGI(TAG, "Starting OTA from URL: %s", fw_update_url);

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA begin failed: %s", esp_err_to_name(err));
        goto ota_fail_no_abort;
    }

    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_get_img_desc failed: %s", esp_err_to_name(err));
        goto ota_fail;
    }

    err = validate_image_header(&app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image header verification failed: %s", esp_err_to_name(err));
        goto ota_fail;
    }

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        ESP_LOGD(TAG, "Image bytes read: %d",
                 esp_https_ota_get_image_len_read(https_ota_handle));
    }

    if (!esp_https_ota_is_complete_data_received(https_ota_handle)) {
        ESP_LOGE(TAG, "Complete OTA image was not received");
        goto ota_fail;
    }

    ota_finish_err = esp_https_ota_finish(https_ota_handle);
    https_ota_handle = NULL;
    if (err == ESP_OK && ota_finish_err == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful. New build=%u. Rebooting...",
                 (unsigned int)pending_fw_build);
        if (pending_fw_build != 0) {
            set_fw_version(pending_fw_build);
        }
        set_operation_mode(FUEL_DISPENSER_MODE);
        free(fw_update_url);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
        ESP_LOGE(TAG, "OTA image validation failed, image is corrupted");
    }
    ESP_LOGE(TAG, "OTA failed: perform=%s finish=0x%x",
             esp_err_to_name(err),
             ota_finish_err);

ota_fail:
    if (https_ota_handle != NULL) {
        esp_https_ota_abort(https_ota_handle);
    }
ota_fail_no_abort:
    free(fw_update_url);
    ota_in_progress = false;
    vTaskDelete(NULL);
}

void ota_update(char *url)
{
    if (url == NULL || url[0] == '\0') {
        ESP_LOGE(TAG, "Invalid OTA URL");
        return;
    }

    if (ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress, skip new request");
        return;
    }

    char *task_url = (char *)malloc(OTA_URL_MAX_SIZE);
    if (task_url == NULL) {
        ESP_LOGE(TAG, "Failed to allocate OTA URL");
        return;
    }

    strlcpy(task_url, url, OTA_URL_MAX_SIZE);
    ota_in_progress = true;

    BaseType_t task_created = xTaskCreate(update_task,
                                          "update_task",
                                          OTA_UPDATE_TASK_STACK_SIZE,
                                          task_url,
                                          5,
                                          NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA update task");
        ota_in_progress = false;
        free(task_url);
    }
}

static esp_err_t ota_fetch_version_json(char *json_buf, size_t json_buf_size)
{
    if (json_buf == NULL || json_buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = OTA_GITHUB_VERSION_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t http_client = esp_http_client_init(&config);
    if (http_client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(http_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open version.json: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http_client);
        return err;
    }

    (void)esp_http_client_fetch_headers(http_client);

    int total_read = 0;
    while (total_read < (int)json_buf_size - 1) {
        int read_len = esp_http_client_read(http_client,
                                            json_buf + total_read,
                                            (int)json_buf_size - 1 - total_read);
        if (read_len <= 0) {
            break;
        }
        total_read += read_len;
    }
    json_buf[total_read] = '\0';

    int status_code = esp_http_client_get_status_code(http_client);
    esp_http_client_close(http_client);
    esp_http_client_cleanup(http_client);

    if (status_code != 200 || total_read == 0) {
        ESP_LOGE(TAG, "Invalid version.json response: status=%d bytes=%d",
                 status_code,
                 total_read);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool ota_parse_version_json(const char *json,
                                   uint32_t *build_out,
                                   char *firmware_url_out,
                                   size_t firmware_url_size)
{
    bool ok = false;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse version.json");
        return false;
    }

    const cJSON *build_item = cJSON_GetObjectItemCaseSensitive(root, "build");
    const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "firmware_url");
    const cJSON *version_item = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *target_item = cJSON_GetObjectItemCaseSensitive(root, "target");

    if (!cJSON_IsNumber(build_item) || build_item->valueint < 0 ||
        build_item->valueint > 255) {
        ESP_LOGE(TAG, "version.json missing valid build 0..255");
        goto done;
    }

    if (cJSON_IsString(target_item) && target_item->valuestring != NULL &&
        strcmp(target_item->valuestring, "esp32s3") != 0) {
        ESP_LOGE(TAG, "version.json target mismatch: %s", target_item->valuestring);
        goto done;
    }

    if (cJSON_IsString(url_item) && url_item->valuestring != NULL &&
        url_item->valuestring[0] != '\0') {
        strlcpy(firmware_url_out, url_item->valuestring, firmware_url_size);
    } else {
        strlcpy(firmware_url_out, OTA_DEFAULT_FIRMWARE_URL, firmware_url_size);
    }

    *build_out = (uint32_t)build_item->valueint;
    ESP_LOGI(TAG, "GitHub version.json: version=%s build=%lu url=%s",
             cJSON_IsString(version_item) ? version_item->valuestring : "unknown",
             (unsigned long)*build_out,
             firmware_url_out);
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

static void ota_version_check_task(void *arg)
{
    (void)arg;
    char version_json[OTA_VERSION_JSON_MAX_SIZE];
    char firmware_url[OTA_URL_MAX_SIZE];

    for (;;) {
        if (!ota_in_progress) {
            uint32_t remote_build = 0;

            if (ota_fetch_version_json(version_json, sizeof(version_json)) == ESP_OK &&
                ota_parse_version_json(version_json,
                                       &remote_build,
                                       firmware_url,
                                       sizeof(firmware_url))) {
                if (remote_build > u8FwVerion) {
                    ESP_LOGW(TAG, "New firmware available: current=%u remote=%lu",
                             (unsigned int)u8FwVerion,
                             (unsigned long)remote_build);
                    pending_fw_build = (uint8_t)remote_build;
                    ota_update(firmware_url);
                } else {
                    ESP_LOGI(TAG, "Firmware is up to date: current=%u remote=%lu",
                             (unsigned int)u8FwVerion,
                             (unsigned long)remote_build);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(OTA_VERSION_CHECK_INTERVAL_MS));
    }
}

void ota_start_github_version_check(void)
{
    if (ota_version_check_started) {
        return;
    }

    ota_version_check_started = true;
    BaseType_t task_created = xTaskCreate(ota_version_check_task,
                                          "ota_version_check",
                                          OTA_CHECK_TASK_STACK_SIZE,
                                          NULL,
                                          4,
                                          NULL);
    if (task_created != pdPASS) {
        ota_version_check_started = false;
        ESP_LOGE(TAG, "Failed to create OTA version check task");
        return;
    }

    ESP_LOGI(TAG, "GitHub OTA version check started, interval=%d ms, url=%s",
             OTA_VERSION_CHECK_INTERVAL_MS,
             OTA_GITHUB_VERSION_URL);
}
