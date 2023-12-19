#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "app_common_interfaces.h"

#include "nvs.h"
#include "nvs_flash.h"
#define TAG "OTA"
#define FIRMWARE_VERSION    0.1
#define BLINK_GPIO          GPIO_NUM_4

// server certificates
extern const uint8_t cert_s3_pem_start[] asm("_binary_ca_s3_cert_pem_start");
extern const uint8_t cert_s3_pem_end[] asm("_binary_ca_s3_cert_pem_end");
char *fw_update_url;

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

    ESP_LOGI(TAG, "New app version: %s\n",new_app_info->version);

    // disable this feature temporarily because some issues related to set PROJECT_VER at build time
    // check App version
    // if (memcmp(new_app_info->version, running_app_info.version, sizeof(new_app_info->version)) == 0) {
    //     ESP_LOGW(TAG, "Current running version is the same as a new. We will not continue the update.");
    //     return ESP_FAIL;
    // }

#ifdef CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
    /**
     * Secure version check from firmware image header prevents subsequent download and flash write of
     * entire firmware image. However this is optional because it is also taken care in API
     * esp_https_ota_finish at the end of OTA update procedure.
     */
    const uint32_t hw_sec_version = esp_efuse_read_secure_version();
    if (new_app_info->secure_version < hw_sec_version) {
        ESP_LOGW(TAG, "New firmware security version is less than eFuse programmed, %d < %d", new_app_info->secure_version, hw_sec_version);
        return ESP_FAIL;
    }
#endif

    return ESP_OK;
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

static void update_task(void *pvParameter) {
    esp_err_t ota_finish_err = ESP_OK;
    esp_http_client_config_t ota_client_config = {
    .url = fw_update_url,
    .cert_pem = (char *)cert_s3_pem_start,
    .skip_cert_common_name_check = true,
    };


    esp_https_ota_config_t ota_config = {
        .http_config = &ota_client_config,
//        .http_client_init_cb = _http_client_init_cb, // Register a callback to be invoked after esp_http_client is initialized
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed");
        vTaskDelete(NULL);
    }
    //set Operation mode back to Fuel dispenser
    setOperationMode_version(FUEL_DISPENSER_MODE);

    // check OTA images header
    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_read_img_desc failed");
        goto ota_end;
    }
    err = validate_image_header(&app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image header verification failed");
        goto ota_end;
    }

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        // esp_https_ota_perform returns after every read operation which gives user the ability to
        // monitor the status of OTA upgrade by calling esp_https_ota_get_image_len_read, which gives length of image
        // data read so far.
        ESP_LOGD(TAG, "Image bytes read: %d", esp_https_ota_get_image_len_read(https_ota_handle));
    }

    if (esp_https_ota_is_complete_data_received(https_ota_handle) != true) {
        // the OTA image was not completely received and user can customise the response to this situation.
        ESP_LOGE(TAG, "Complete data was not received.");
    } else {
        ota_finish_err = esp_https_ota_finish(https_ota_handle);
        if ((err == ESP_OK) && (ota_finish_err == ESP_OK)) {
            ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful. Rebooting ...");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_restart();
        } else {
            if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
                ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            }
            ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed 0x%x", ota_finish_err);
            vTaskDelete(NULL);
        }
    }

    ota_end:
    esp_https_ota_abort(https_ota_handle);
    ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed");
    free(fw_update_url);
    vTaskDelete(NULL);
}

void ota_update(char *url) {
    if(url == NULL)
    {
        ESP_LOGE(TAG, "Invalid URL!!! \n");
        return;
    }
    fw_update_url = malloc(256);
    strcpy(fw_update_url, url);
    ESP_LOGI(TAG, "****************Starting updatefw from URL: %s \n", fw_update_url);
    // Check the validation of fw images. If OK, mark newly updated firmware image as active, or else rollback to previous fw
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                ESP_LOGI(TAG, "App is valid, rollback cancelled successfully");
            } else {
                ESP_LOGE(TAG, "Failed to cancel rollback");
            }
        }
    }
    // start the check update task
    xTaskCreate(&update_task, "update_task", 8192, NULL, 5, NULL);
}