/* Wi-Fi Provisioning Manager Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

//#include <wifi_provisioning/manager.h>
#include <app_common_interfaces.h>

#define TAG "app"

/* Signal Wi-Fi events on this event-group */
const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;


/* Event handler for catching system events */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        esp_wifi_connect();
    }
}

/* static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
} */

/* Handler for the optional provisioning endpoint registered by the application.
 * The data format can be chosen by applications. Here, we are using plain ascii text.
 * Applications can choose to use other formats like protobuf, JSON, XML, etc.
 */
// esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
//                                           uint8_t **outbuf, ssize_t *outlen, void *priv_data)
// {
//     if (inbuf) {
//         ESP_LOGI(TAG, "Received data: %.*s", inlen, (char *)inbuf);
//     }
//     char response[] = "SUCCESS";
//     *outbuf = (uint8_t *)strdup(response);
//     if (*outbuf == NULL) {
//         ESP_LOGE(TAG, "System out of memory");
//         return ESP_ERR_NO_MEM;
//     }
//     *outlen = strlen(response) + 1; /* +1 for NULL terminating byte */

//     return ESP_OK;
// }

#define EXAMPLE_ESP_WIFI_SSID      "Technical IOT"
#define EXAMPLE_ESP_WIFI_PASS      "123456789"
#define EXAMPLE_ESP_MAXIMUM_RETRY  5
#define WIFI_AP_TIMEOUT  120000/portTICK_PERIOD_MS //ms
extern void wifi_sta_main(void)
{
    const esp_partition_t *next_partition;
    esp_err_t err;
    /* Initialize TCP/IP */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize the event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();

    /* Register our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    /* Initialize Wi-Fi including netif with default config */
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
        },
    };

        /* Start Wi-Fi station */
        //wifi_init_sta();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    /* Wait for Wi-Fi connection */
    int ret=xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, WIFI_AP_TIMEOUT);
    ESP_LOGI(TAG, "Wifi return : %d",ret);
    if(ret==0) //timeout occurs should return to main boot partition (LORA)
    {
        next_partition = esp_ota_get_next_update_partition(NULL);
        err = esp_ota_set_boot_partition(next_partition);
        ESP_LOGE(TAG, "Wifi AP not found, reboot to main partition\n");
        esp_restart();
    }
    else
    {
        ESP_LOGI(TAG, "WIFI connected \n");
    }

#ifdef MODBUS_COMM_ENABLE
    //Call MQTT and modbus init
    // wifi_main();
#endif
//#endif
    //lora_ota_update();
}
