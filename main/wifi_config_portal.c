#include "app_common_interfaces.h"

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include <ctype.h>
#include <stdlib.h>

#define TAG "WIFI_CONFIG"

#define WIFI_CONFIG_NAMESPACE "wifi_config"
#define WIFI_CONFIG_SSID_KEY "ssid"
#define WIFI_CONFIG_PASS_KEY "password"

#define WIFI_CONFIG_BUTTON_PIN GPIO_NUM_15
#define WIFI_CONFIG_BUTTON_ACTIVE_LEVEL 1
#define WIFI_CONFIG_BUTTON_HOLD_MS 5000
#define WIFI_CONFIG_BUTTON_POLL_MS 100
#define WIFI_CONFIG_BUTTON_TASK_STACK_SIZE 4096

#define WIFI_CONFIG_AP_CHANNEL 1
#define WIFI_CONFIG_AP_MAX_CONN 4
#define WIFI_CONFIG_AP_SSID_PREFIX "CAU HINH WIFI"

static TaskHandle_t wifi_config_button_task_handle;
static httpd_handle_t wifi_config_httpd;
static esp_netif_t *wifi_config_ap_netif;
static volatile bool wifi_config_portal_active;

static bool is_hex_char(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static uint8_t hex_to_u8(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(c - 'a' + 10);
    }
    return (uint8_t)(c - 'A' + 10);
}

static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;

    if (src == NULL || dst == NULL || dst_size == 0) {
        return;
    }

    while (*src != '\0' && di + 1 < dst_size) {
        if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else if (*src == '%' &&
                   is_hex_char(src[1]) &&
                   is_hex_char(src[2])) {
            dst[di++] = (char)((hex_to_u8(src[1]) << 4) | hex_to_u8(src[2]));
            src += 3;
        } else {
            dst[di++] = *src++;
        }
    }

    dst[di] = '\0';
}

static bool form_get_value(const char *body,
                           const char *key,
                           char *out,
                           size_t out_size)
{
    const char *p = body;
    size_t key_len = strlen(key);

    if (body == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }

    while (p != NULL && *p != '\0') {
        const char *next = strchr(p, '&');
        const char *eq = strchr(p, '=');

        if (eq != NULL && (next == NULL || eq < next) &&
            (size_t)(eq - p) == key_len &&
            strncmp(p, key, key_len) == 0) {
            char encoded[128];
            size_t value_len = next == NULL ? strlen(eq + 1) : (size_t)(next - (eq + 1));

            if (value_len >= sizeof(encoded)) {
                value_len = sizeof(encoded) - 1;
            }

            memcpy(encoded, eq + 1, value_len);
            encoded[value_len] = '\0';
            url_decode(encoded, out, out_size);
            return true;
        }

        p = next == NULL ? NULL : next + 1;
    }

    return false;
}

bool wifi_config_load_credentials(char *ssid,
                                  size_t ssid_size,
                                  char *password,
                                  size_t password_size)
{
    nvs_handle_t nvs = 0;
    size_t ssid_len = ssid_size;
    size_t pass_len = password_size;
    esp_err_t err;

    if (ssid == NULL || password == NULL ||
        ssid_size == 0 || password_size == 0) {
        return false;
    }

    ssid[0] = '\0';
    password[0] = '\0';

    err = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_get_str(nvs, WIFI_CONFIG_SSID_KEY, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, WIFI_CONFIG_PASS_KEY, password, &pass_len);
    }

    nvs_close(nvs);

    if (err != ESP_OK || ssid[0] == '\0') {
        ssid[0] = '\0';
        password[0] = '\0';
        return false;
    }

    return true;
}

static esp_err_t wifi_config_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs = 0;
    esp_err_t err;

    err = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, WIFI_CONFIG_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_CONFIG_PASS_KEY, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static void wifi_config_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t wifi_config_root_get_handler(httpd_req_t *req)
{
    const char page[] =
        "<!doctype html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Fuel Device WiFi Config</title>"
        "<style>body{font-family:sans-serif;margin:24px;max-width:420px}"
        "input,button{font-size:18px;width:100%;padding:10px;margin:8px 0}</style>"
        "</head><body>"
        "<h2>Fuel Device WiFi Config</h2>"
        "<form method=\"post\" action=\"/save\">"
        "<label>WiFi SSID</label><input name=\"ssid\" maxlength=\"32\" required>"
        "<label>Password</label><input name=\"password\" type=\"password\" maxlength=\"64\">"
        "<button type=\"submit\">Save & Reboot</button>"
        "</form></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_config_favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t wifi_config_save_post_handler(httpd_req_t *req)
{
    char body[256];
    char ssid[33];
    char password[65];
    int received = 0;
    int remaining = req->content_len;

    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form size");
        return ESP_OK;
    }

    while (remaining > 0) {
        int ret = httpd_req_recv(req,
                                 body + received,
                                 remaining);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
            return ESP_OK;
        }
        received += ret;
        remaining -= ret;
    }
    body[received] = '\0';

    if (!form_get_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_OK;
    }
    if (!form_get_value(body, "password", password, sizeof(password))) {
        password[0] = '\0';
    }

    esp_err_t err = wifi_config_save_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WiFi config: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save WiFi config");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Saved WiFi credentials: ssid=%s, password_len=%u",
             ssid,
             (unsigned int)strlen(password));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
                       "<html><body><h2>Saved. Device will reboot...</h2></body></html>");
    xTaskCreate(wifi_config_reboot_task,
                "wifi_config_reboot",
                2048,
                NULL,
                5,
                NULL);
    return ESP_OK;
}

static esp_err_t wifi_config_start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = wifi_config_root_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = wifi_config_save_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = wifi_config_favicon_get_handler,
        .user_ctx = NULL,
    };

    config.stack_size = 4096;

    esp_err_t err = httpd_start(&wifi_config_httpd, &config);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(httpd_register_uri_handler(wifi_config_httpd, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(wifi_config_httpd, &save_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(wifi_config_httpd, &favicon_uri));
    return ESP_OK;
}

bool wifi_config_portal_is_active(void)
{
    return wifi_config_portal_active;
}

void wifi_config_portal_start(void)
{
    wifi_config_t ap_config = {0};
    char ap_ssid[32];

    if (wifi_config_portal_active) {
        return;
    }

    wifi_config_portal_active = true;
    status_led_set_wifi_connected(false);

    snprintf(ap_ssid, sizeof(ap_ssid), "%s_%s",
             WIFI_CONFIG_AP_SSID_PREFIX,
             deviceID != NULL ? deviceID : "device");

    ESP_LOGW(TAG, "Starting WiFi config portal AP: ssid=%s", ap_ssid);

    FD_wifi_mqtt_stop_for_config_portal();

    if (wifi_config_ap_netif == NULL) {
        wifi_config_ap_netif = esp_netif_create_default_wifi_ap();
    }

    esp_wifi_disconnect();
    esp_wifi_stop();

    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = WIFI_CONFIG_AP_CHANNEL;
    ap_config.ap.max_connection = WIFI_CONFIG_AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_err_t err = wifi_config_start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start config web server: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGW(TAG, "WiFi config portal ready. Connect to SSID '%s', open http://192.168.4.1",
             ap_ssid);
}

static void wifi_config_button_task(void *arg)
{
    (void)arg;
    TickType_t pressed_since = 0;
    bool fired = false;

    for (;;) {
        bool pressed = gpio_get_level(WIFI_CONFIG_BUTTON_PIN) == WIFI_CONFIG_BUTTON_ACTIVE_LEVEL;

        if (pressed) {
            if (pressed_since == 0) {
                pressed_since = xTaskGetTickCount();
                fired = false;
            } else if (!fired &&
                       (xTaskGetTickCount() - pressed_since) >= pdMS_TO_TICKS(WIFI_CONFIG_BUTTON_HOLD_MS)) {
                fired = true;
                ESP_LOGW(TAG, "GPIO%d held for %d ms, entering WiFi config AP mode",
                         WIFI_CONFIG_BUTTON_PIN,
                         WIFI_CONFIG_BUTTON_HOLD_MS);
                wifi_config_portal_start();
            }
        } else {
            pressed_since = 0;
            fired = false;
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_CONFIG_BUTTON_POLL_MS));
    }
}

void wifi_config_button_init(void)
{
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << WIFI_CONFIG_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));

    if (wifi_config_button_task_handle != NULL) {
        return;
    }

    BaseType_t task_created = xTaskCreate(wifi_config_button_task,
                                          "wifi_cfg_btn",
                                          WIFI_CONFIG_BUTTON_TASK_STACK_SIZE,
                                          NULL,
                                          5,
                                          &wifi_config_button_task_handle);
    if (task_created != pdPASS) {
        wifi_config_button_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create WiFi config button task");
        return;
    }

    ESP_LOGI(TAG, "GPIO%d WiFi config button enabled, hold %d ms to start AP portal",
             WIFI_CONFIG_BUTTON_PIN,
             WIFI_CONFIG_BUTTON_HOLD_MS);
}
