#include "app_common_interfaces.h"
#include "driver/gpio.h"

#define TAG "STATUS_LED"

#define STATUS_LED_PIN GPIO_NUM_3
#define STATUS_LED_ON_LEVEL 1
#define STATUS_LED_BOOT_BLINK_MS 200
#define STATUS_LED_WIFI_CONNECTED_BLINK_MS 3000
#define STATUS_LED_PULSE_MS 120
#define STATUS_LED_TASK_STACK_SIZE 2048

static TaskHandle_t status_led_task_handle;
static volatile TickType_t status_led_period_ticks = pdMS_TO_TICKS(STATUS_LED_BOOT_BLINK_MS);

static void status_led_set_level(bool on)
{
    gpio_set_level(STATUS_LED_PIN, on ? STATUS_LED_ON_LEVEL : !STATUS_LED_ON_LEVEL);
}

static void status_led_task(void *arg)
{
    (void)arg;

    for (;;) {
        TickType_t period_ticks = status_led_period_ticks;
        TickType_t pulse_ticks = pdMS_TO_TICKS(STATUS_LED_PULSE_MS);

        if (period_ticks <= pulse_ticks) {
            period_ticks = pulse_ticks + 1;
        }

        status_led_set_level(true);
        vTaskDelay(pulse_ticks);
        status_led_set_level(false);
        vTaskDelay(period_ticks - pulse_ticks);
    }
}

void status_led_init(void)
{
    gpio_config_t status_led_config = {
        .pin_bit_mask = (1ULL << STATUS_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&status_led_config));
    status_led_set_level(false);

    if (status_led_task_handle != NULL) {
        return;
    }

    BaseType_t task_created = xTaskCreate(status_led_task,
                                          "status_led",
                                          STATUS_LED_TASK_STACK_SIZE,
                                          NULL,
                                          3,
                                          &status_led_task_handle);
    if (task_created != pdPASS) {
        status_led_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create status LED task");
        return;
    }

    ESP_LOGI(TAG, "GPIO%d status LED started: boot blink every %d ms",
             STATUS_LED_PIN,
             STATUS_LED_BOOT_BLINK_MS);
}

void status_led_set_wifi_connected(bool connected)
{
    status_led_period_ticks = pdMS_TO_TICKS(connected ? STATUS_LED_WIFI_CONNECTED_BLINK_MS
                                                      : STATUS_LED_BOOT_BLINK_MS);
    ESP_LOGI(TAG, "WiFi status=%s, LED blink period=%d ms",
             connected ? "connected" : "not connected",
             connected ? STATUS_LED_WIFI_CONNECTED_BLINK_MS : STATUS_LED_BOOT_BLINK_MS);
}
