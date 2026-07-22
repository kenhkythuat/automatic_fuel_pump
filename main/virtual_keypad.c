#include "app_common_interfaces.h"
#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG "VIR_KEYPAD"

#define PIN_CHANEL_SELECT_1A 14
#define PIN_CHANEL_SELECT_1B 13
#define PIN_CHANEL_SELECT_1C 12
#define PIN_INH_CLUSTER_1 8
#define PIN_CHANEL_SELECT_2A 16
#define PIN_CHANEL_SELECT_2B 17
#define PIN_CHANEL_SELECT_2C 18
#define PIN_INH_CLUSTER_2 19
#define ON_OFF_VIRTUAL_KEYPAD GPIO_NUM_5

// 1: route the real external keypad through 74HC4053.
// 0: route the ESP32 virtual keypad outputs through 74HC4053.
#define USE_EXTERNAL_PHYSICAL_KEYPAD 0
static SemaphoreHandle_t set_price_task_sem;
#define VIRTUAL_KEYPAD_SWEEP_TEST 0
#define VIRTUAL_KEYPAD_SWEEP_PRESS_MS 800
#define VIRTUAL_KEYPAD_SWEEP_PERIOD_MS 1000
#define VIRTUAL_KEYPAD_ADDR_SETTLE_MS 20
#define VIRTUAL_KEYPAD_SEQUENCE_SETTLE_MS 120
#define ORDER_CMD_SIZE 8
#define END_SESSION_CMD_SIZE 9
#define MAPPING_TABLE_SIZE 8

static bool virtual_keypad_external_physical_enabled;
char order[ORDER_CMD_SIZE] = {'P', '1', '2', '3', '4', '5', '6', 'E'};
char end_session[END_SESSION_CMD_SIZE] = {'T', '8', '1', '2', '3', '4', '5', '6', 'E'};
uint8_t pin_control[8] = {14, 13, 12, 8, 16, 17, 18, 19};
char keys[4][4] =
    {
        {'L', '3', '2', '1'},
        {'$', '6', '5', '4'},
        {'P', '9', '8', '7'},
        {'T', 'E', '0', 'C'},
};

typedef struct
{ // Same as pair in C++
    char btn;
    uint8_t pin;
} pair_btn_pin;
pair_btn_pin mapping_table_1[MAPPING_TABLE_SIZE] = {
    // Mapping table 1 : between physical button and ESP output signal (row 2,3)
    {'$', 0},
    {'6', 1},
    {'4', 2},
    {'5', 3},
    {'2', 4},
    {'1', 5},
    {'L', 6},
    {'3', 7},
};
pair_btn_pin mapping_table_2[MAPPING_TABLE_SIZE] = {
    // Mapping table 2 : between physical button and ESP output signal (row 4,5)
    {'T', 0},
    {'E', 1},
    {'C', 2},
    {'0', 3},
    {'8', 4},
    {'7', 5},
    {'P', 6},
    {'9', 7},
};

static void virtual_keypad_idle(void)
{
    gpio_set_level(PIN_INH_CLUSTER_1, 1);
    gpio_set_level(PIN_INH_CLUSTER_2, 1);

    gpio_set_level(PIN_CHANEL_SELECT_1A, 0);
    gpio_set_level(PIN_CHANEL_SELECT_1B, 0);
    gpio_set_level(PIN_CHANEL_SELECT_1C, 0);
    gpio_set_level(PIN_CHANEL_SELECT_2A, 0);
    gpio_set_level(PIN_CHANEL_SELECT_2B, 0);
    gpio_set_level(PIN_CHANEL_SELECT_2C, 0);
}

void virtual_keypad_set_external_physical(bool enabled)
{
    virtual_keypad_external_physical_enabled = enabled;
    virtual_keypad_idle();
    gpio_set_level(ON_OFF_VIRTUAL_KEYPAD, enabled ? 1 : 0);
    gpio_set_direction(ON_OFF_VIRTUAL_KEYPAD, GPIO_MODE_OUTPUT);
    ESP_LOGW(TAG, "Keypad route GPIO%d=%d (%s)",
             ON_OFF_VIRTUAL_KEYPAD,
             enabled ? 1 : 0,
             enabled ? "external physical keypad" : "ESP32 virtual keypad");
}

bool virtual_keypad_is_enabled(void)
{
    return !virtual_keypad_external_physical_enabled;
}

static void psudoe_press_timed(uint8_t cluster, uint8_t pin, uint32_t press_ms, uint32_t gap_ms)
{
    if (cluster == 1)
    {
        gpio_set_level(PIN_CHANEL_SELECT_1A, (pin >> 0) & 0x01);
        gpio_set_level(PIN_CHANEL_SELECT_1B, (pin >> 1) & 0x01);
        gpio_set_level(PIN_CHANEL_SELECT_1C, (pin >> 2) & 0x01);
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEYPAD_ADDR_SETTLE_MS));
        gpio_set_level(PIN_INH_CLUSTER_1, 0);
        vTaskDelay(pdMS_TO_TICKS(press_ms));
        gpio_set_level(PIN_INH_CLUSTER_1, 1);
        virtual_keypad_idle();
    }
    else if (cluster == 2)
    {
        gpio_set_level(PIN_CHANEL_SELECT_2A, (pin >> 0) & 0x01);
        gpio_set_level(PIN_CHANEL_SELECT_2B, (pin >> 1) & 0x01);
        gpio_set_level(PIN_CHANEL_SELECT_2C, (pin >> 2) & 0x01);
        vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEYPAD_ADDR_SETTLE_MS));
        gpio_set_level(PIN_INH_CLUSTER_2, 0);
        vTaskDelay(pdMS_TO_TICKS(press_ms));
        gpio_set_level(PIN_INH_CLUSTER_2, 1);
        virtual_keypad_idle();
    }
    else
    {
        ESP_LOGI(TAG, "ERROR: Invalid cluster\n");
        virtual_keypad_idle();
    }
    vTaskDelay(pdMS_TO_TICKS(gap_ms));
}

static void psudoe_press(uint8_t cluster, uint8_t pin)
{
    psudoe_press_timed(cluster, pin, 500, 1000);
}

static void press_key(char key)
{
    if (virtual_keypad_external_physical_enabled) {
        ESP_LOGW(TAG, "Skip virtual key=%c: external physical keypad mode is active", key);
        return;
    }

    for (int j = 0; j < MAPPING_TABLE_SIZE; j++)
    {
        if (key == mapping_table_1[j].btn)
        {
            ESP_LOGI(TAG, "Virtual press key=%c cluster=1 mux_pin=%u",
                     key,
                     mapping_table_1[j].pin);
            psudoe_press(1, mapping_table_1[j].pin);
            return;
        }
        else if (key == mapping_table_2[j].btn)
        {
            ESP_LOGI(TAG, "Virtual press key=%c cluster=2 mux_pin=%u",
                     key,
                     mapping_table_2[j].pin);
            psudoe_press(2, mapping_table_2[j].pin);
            return;
        }
    }
    ESP_LOGW(TAG, "Unsupported virtual key: %c", key);
}

static bool press_key_timed(char key, uint32_t press_ms, uint32_t gap_ms)
{
    if (virtual_keypad_external_physical_enabled) {
        ESP_LOGW(TAG, "SWEEP skip key=%c: external physical keypad mode is active", key);
        return false;
    }

    for (int j = 0; j < MAPPING_TABLE_SIZE; j++)
    {
        if (key == mapping_table_1[j].btn)
        {
            ESP_LOGI(TAG, "SWEEP press key=%c cluster=1 mux_pin=%u", key, mapping_table_1[j].pin);
            psudoe_press_timed(1, mapping_table_1[j].pin, press_ms, gap_ms);
            return true;
        }
        else if (key == mapping_table_2[j].btn)
        {
            ESP_LOGI(TAG, "SWEEP press key=%c cluster=2 mux_pin=%u", key, mapping_table_2[j].pin);
            psudoe_press_timed(2, mapping_table_2[j].pin, press_ms, gap_ms);
            return true;
        }
    }

    ESP_LOGW(TAG, "SWEEP unsupported virtual key: %c", key);
    return false;
}

static char virtual_keypad_expected_key(uint8_t cluster, uint8_t pin)
{
    pair_btn_pin *table = (cluster == 1) ? mapping_table_1 : mapping_table_2;

    for (int i = 0; i < MAPPING_TABLE_SIZE; i++)
    {
        if (table[i].pin == pin)
        {
            return table[i].btn;
        }
    }

    return '?';
}

static void virtual_keypad_sequence_begin(const char *sequence_name)
{
    xSemaphoreTake(set_price_task_sem, portMAX_DELAY);

#if !USE_EXTERNAL_PHYSICAL_KEYPAD
    keypad_master_scan_pause_for_virtual_keypad();
#endif

    vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEYPAD_SEQUENCE_SETTLE_MS));

    ESP_LOGI(TAG, "Virtual keypad sequence begin: %s", sequence_name);
}

static void virtual_keypad_sequence_end(const char *sequence_name)
{
    ESP_LOGI(TAG, "Virtual keypad sequence end: %s", sequence_name);

    vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEYPAD_SEQUENCE_SETTLE_MS));

#if !USE_EXTERNAL_PHYSICAL_KEYPAD
    keypad_master_scan_resume_after_virtual_keypad();
#endif

    xSemaphoreGive(set_price_task_sem);
}

static void virtual_keypad_sweep_test_task(void *arg)
{
    (void)arg;
    const uint32_t gap_ms = VIRTUAL_KEYPAD_SWEEP_PERIOD_MS - VIRTUAL_KEYPAD_SWEEP_PRESS_MS;

    virtual_keypad_sequence_begin("sweep_test");
    ESP_LOGW(TAG, "Virtual keypad CD4051 channel sweep enabled: 16 channels, %d ms/channel",
             VIRTUAL_KEYPAD_SWEEP_PERIOD_MS);

    for (;;)
    {
        for (uint8_t cluster = 1; cluster <= 2; cluster++)
        {
            for (uint8_t pin = 0; pin < MAPPING_TABLE_SIZE; pin++)
            {
                ESP_LOGI(TAG,
                         "SWEEP CD4051 cluster=%u channel=%u expected_key=%c A=%u B=%u C=%u",
                         cluster,
                         pin,
                         virtual_keypad_expected_key(cluster, pin),
                         (pin >> 0) & 0x01,
                         (pin >> 1) & 0x01,
                         (pin >> 2) & 0x01);
                psudoe_press_timed(cluster,
                                   pin,
                                   VIRTUAL_KEYPAD_SWEEP_PRESS_MS,
                                   gap_ms);
            }
        }

        ESP_LOGW(TAG, "Virtual keypad CD4051 channel sweep cycle completed");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void virtual_keypad_init()
{
    for (int i = 0; i < MAPPING_TABLE_SIZE; i++)
    {
        esp_rom_gpio_pad_select_gpio(pin_control[i]);
        gpio_set_direction(pin_control[i], GPIO_MODE_OUTPUT);
    }
    virtual_keypad_set_external_physical(USE_EXTERNAL_PHYSICAL_KEYPAD != 0);
    set_price_task_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(set_price_task_sem);

#if VIRTUAL_KEYPAD_SWEEP_TEST
    xTaskCreate(virtual_keypad_sweep_test_task,
                "virtual_keypad_sweep",
                3072,
                NULL,
                configMAX_PRIORITIES - 1,
                NULL);
#endif
}

void change_price_by_vir_keypad(void *arg)
{
    char *price = (char *)arg;

    virtual_keypad_sequence_begin("change_price");
    // bset_price = true;
    ESP_LOGI(TAG, "Press: ");
    for (int i = 0; i < sizeof(order); i++)
    {
        press_key(order[i]);
    }

    for (int i = 0; i < strlen(price); i++)
    {
        press_key(price[i]);
    }
    press_key('E'); // Press E to confirm the price
    ESP_LOGI(TAG, "Change price Session completed.\n\n");
    // bset_price = false;
    virtual_keypad_sequence_end("change_price");
    free(price);
    vTaskDelete(NULL);
    // vTaskSuspend(my_task_handler);
    // vTaskDelete(my_task_handler);
}

void enter_qr_price_by_vir_keypad(void *arg)
{
    char *amount = (char *)arg;

    virtual_keypad_sequence_begin("enter_qr_price");
    ESP_LOGI(TAG, "QR price input start: %s", amount);

    press_key('$');
    vTaskDelay(pdMS_TO_TICKS(1000));
    for (int i = 0; i < strlen(amount); i++)
    {
        press_key(amount[i]);
    }
    press_key('E');

    ESP_LOGI(TAG, "QR price input completed.\n\n");
    virtual_keypad_sequence_end("enter_qr_price");
    free(amount);
    vTaskDelete(NULL);
}

void cancel_qr_money_by_vir_keypad(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Cancel QR money task start, stack free=%u",
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    virtual_keypad_sequence_begin("cancel_qr_money");
    ESP_LOGI(TAG, "Cancel QR money after sequence begin, stack free=%u",
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "Cancel QR money start: press C");

    press_key('C');

    ESP_LOGI(TAG, "Cancel QR money completed.\n\n");
    virtual_keypad_sequence_end("cancel_qr_money");
    ESP_LOGI(TAG, "Cancel QR money task end, stack free=%u",
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

void end_session_by_vir_keypad(void *arg)
{
    (void)arg;

    virtual_keypad_sequence_begin("end_session");
    // bset_price = true;
    ESP_LOGI(TAG, "End_Session start..........\n ");
    // end_session[9] = {'T', '8','1', '2', '3','4', '5','6','E'} -> array of characters for end current session
    for (int i = 0; i < sizeof(end_session); i++)
    {
        press_key(end_session[i]);
    }
    ESP_LOGI(TAG, "End_Session completed\n\n");
    // bset_price = false;
    virtual_keypad_sequence_end("end_session");
    vTaskDelete(NULL);
    // vTaskSuspend(my_task_handler);
    // vTaskDelete(my_task_handler);
}
