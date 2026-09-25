#include "app_common_interfaces.h"
#include "driver/gpio.h"
#include "nvs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG "VIR_KEYPAD"
#define VIRTUAL_KEYPAD_NVS_NAMESPACE "nodeconfig"
#define VIRTUAL_KEYPAD_NVS_EXTERNAL_KEY "externalKeypad"

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
#define VIRTUAL_KEYPAD_SWEEP_PRESS_MS 200
#define VIRTUAL_KEYPAD_SWEEP_PERIOD_MS 200
#define VIRTUAL_KEYPAD_BOOT_CLEAR_DELAY_MS 1000
#define VIRTUAL_KEYPAD_BOOT_CLEAR_COUNT 3
#define VIRTUAL_KEYPAD_ADDR_SETTLE_MS 20
#define VIRTUAL_KEYPAD_SEQUENCE_SETTLE_MS 120
#define ORDER_CMD_SIZE 10
#define ALT_SET_PRICE_CMD_SIZE 8
#define ALT_SET_PRICE_PASSWORD_SIZE 7
#define END_SESSION_CMD_SIZE 9
#define MAPPING_TABLE_SIZE 8

static bool virtual_keypad_external_physical_enabled;
char order[ORDER_CMD_SIZE] = {'C','C','P','1', '2', '3', '4', '5', '6', 'E'};
char alt_set_price_cmd[ALT_SET_PRICE_CMD_SIZE] = {'C','C','T', 'P', '0', '1', '2', 'E'};
char alt_set_price_password[ALT_SET_PRICE_PASSWORD_SIZE] = {'2', '2', '2', '2', '2', '2','E'};
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

static void virtual_keypad_save_external_physical(bool enabled)
{
    nvs_handle nvs = 0;
    esp_err_t err = nvs_open(VIRTUAL_KEYPAD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for keypad route: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(nvs, VIRTUAL_KEYPAD_NVS_EXTERNAL_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save keypad route to NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved keypad route to NVS: external=%u", enabled ? 1 : 0);
    }

    nvs_close(nvs);
}

static bool virtual_keypad_load_external_physical(bool *enabled)
{
    nvs_handle nvs = 0;
    uint8_t stored_value = 0;
    esp_err_t err;

    if (enabled == NULL) {
        return false;
    }

    err = nvs_open(VIRTUAL_KEYPAD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for keypad route load: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_get_u8(nvs, VIRTUAL_KEYPAD_NVS_EXTERNAL_KEY, &stored_value);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH) {
        *enabled = (USE_EXTERNAL_PHYSICAL_KEYPAD != 0);
        ESP_LOGW(TAG,
                 "Keypad route not found in NVS, use default external=%u",
                 *enabled ? 1 : 0);
        err = nvs_set_u8(nvs, VIRTUAL_KEYPAD_NVS_EXTERNAL_KEY, *enabled ? 1 : 0);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
    } else if (err == ESP_OK) {
        *enabled = (stored_value != 0);
        ESP_LOGI(TAG, "Loaded keypad route from NVS: external=%u", *enabled ? 1 : 0);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load/save keypad route in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return false;
    }

    nvs_close(nvs);
    return true;
}

static void virtual_keypad_apply_external_physical(bool enabled, bool save_to_flash)
{
    virtual_keypad_external_physical_enabled = enabled;
    virtual_keypad_idle();
    gpio_set_level(ON_OFF_VIRTUAL_KEYPAD, enabled ? 1 : 0);
    gpio_set_direction(ON_OFF_VIRTUAL_KEYPAD, GPIO_MODE_OUTPUT);
    ESP_LOGW(TAG, "Keypad route GPIO%d=%d (%s)",
             ON_OFF_VIRTUAL_KEYPAD,
             enabled ? 1 : 0,
             enabled ? "external physical keypad" : "ESP32 virtual keypad");

    if (save_to_flash) {
        virtual_keypad_save_external_physical(enabled);
    }
}

void virtual_keypad_set_external_physical(bool enabled)
{
    virtual_keypad_apply_external_physical(enabled, true);
}

bool virtual_keypad_is_enabled(void)
{
    return !virtual_keypad_external_physical_enabled;
}

bool virtual_keypad_is_external_physical_enabled(void)
{
    return virtual_keypad_external_physical_enabled;
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
    psudoe_press_timed(cluster, pin, 300, 600);
}

static bool press_key(char key)
{
    if (virtual_keypad_external_physical_enabled) {
        ESP_LOGW(TAG, "Skip virtual key=%c: external physical keypad mode is active", key);
        return false;
    }

    for (int j = 0; j < MAPPING_TABLE_SIZE; j++)
    {
        if (key == mapping_table_1[j].btn)
        {
            ESP_LOGI(TAG, "Virtual press key=%c cluster=1 mux_pin=%u",
                     key,
                     mapping_table_1[j].pin);
            psudoe_press(1, mapping_table_1[j].pin);
            return true;
        }
        else if (key == mapping_table_2[j].btn)
        {
            ESP_LOGI(TAG, "Virtual press key=%c cluster=2 mux_pin=%u",
                     key,
                     mapping_table_2[j].pin);
            psudoe_press(2, mapping_table_2[j].pin);
            return true;
        }
    }
    ESP_LOGW(TAG, "Unsupported virtual key: %c", key);
    return false;
}

static void press_key_array(const char *keys_to_press, size_t key_count)
{
    for (size_t i = 0; i < key_count; i++)
    {
        press_key(keys_to_press[i]);
    }
}

static void press_key_string(const char *keys_to_press)
{
    for (size_t i = 0; i < strlen(keys_to_press); i++)
    {
        press_key(keys_to_press[i]);
    }
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

bool virtual_keypad_press_key(char key)
{
    bool pressed;

    if (set_price_task_sem == NULL) {
        ESP_LOGE(TAG, "Cannot press key=%c: virtual keypad is not initialized", key);
        return false;
    }

    virtual_keypad_sequence_begin("single_key");
    pressed = press_key(key);
    virtual_keypad_sequence_end("single_key");
    return pressed;
}

void virtual_keypad_boot_clear(void)
{
    bool restore_external_physical;

    if (set_price_task_sem == NULL) {
        ESP_LOGE(TAG, "Cannot run boot keypad clear: virtual keypad is not initialized");
        return;
    }

    ESP_LOGW(TAG,
             "Boot keypad clear scheduled: press C %u times after %u ms",
             (unsigned int)VIRTUAL_KEYPAD_BOOT_CLEAR_COUNT,
             (unsigned int)VIRTUAL_KEYPAD_BOOT_CLEAR_DELAY_MS);

    vTaskDelay(pdMS_TO_TICKS(VIRTUAL_KEYPAD_BOOT_CLEAR_DELAY_MS));

    virtual_keypad_sequence_begin("boot_clear");

    restore_external_physical = virtual_keypad_external_physical_enabled;
    if (restore_external_physical) {
        virtual_keypad_apply_external_physical(false, false);
    }

    for (uint32_t i = 0; i < VIRTUAL_KEYPAD_BOOT_CLEAR_COUNT; i++) {
        ESP_LOGI(TAG,
                 "Boot keypad clear %u/%u",
                 (unsigned int)(i + 1),
                 (unsigned int)VIRTUAL_KEYPAD_BOOT_CLEAR_COUNT);
        (void)press_key('C');
    }

    virtual_keypad_idle();
    if (restore_external_physical) {
        virtual_keypad_apply_external_physical(true, false);
    }

    virtual_keypad_sequence_end("boot_clear");

    ESP_LOGW(TAG, "Boot keypad clear completed");
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
    bool external_physical_enabled = (USE_EXTERNAL_PHYSICAL_KEYPAD != 0);

    for (int i = 0; i < MAPPING_TABLE_SIZE; i++)
    {
        esp_rom_gpio_pad_select_gpio(pin_control[i]);
        gpio_set_direction(pin_control[i], GPIO_MODE_OUTPUT);
    }

    (void)virtual_keypad_load_external_physical(&external_physical_enabled);
    virtual_keypad_apply_external_physical(external_physical_enabled, false);

    set_price_task_sem = xSemaphoreCreateBinary();
    if (set_price_task_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create virtual keypad semaphore");
        return;
    }
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
    ESP_LOGI(TAG, "Change price input start: price=%s mode=%s",
             price,
             MAIN_RS232 ? "MAIN_RS232" : "ALT_SET_PRICE");

#if MAIN_RS232
    press_key_array(order, sizeof(order));
#else
    press_key_array(alt_set_price_cmd, sizeof(alt_set_price_cmd));
    press_key_array(alt_set_price_password, sizeof(alt_set_price_password));
#endif

    press_key_string(price);
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
    vTaskDelay(pdMS_TO_TICKS(700));
    for (int i = 0; i < strlen(amount); i++)
    {
        press_key(amount[i]);
    }
    press_key('E');

    ESP_LOGI(TAG, "QR price input completed.\n\n");
    virtual_keypad_sequence_end("enter_qr_price");
    payment_set_qr_money_keypad_done();
    free(amount);
    vTaskDelete(NULL);
}

void enter_qr_litter_by_vir_keypad(void *arg)
{
    char *litter = (char *)arg;

    virtual_keypad_sequence_begin("enter_qr_litter");
    ESP_LOGI(TAG, "QR litter input start: %s", litter);

    press_key('C');
    press_key('L');
    vTaskDelay(pdMS_TO_TICKS(700));
    press_key_string(litter);
    press_key('E');

    ESP_LOGI(TAG, "QR litter input completed.\n\n");
    virtual_keypad_sequence_end("enter_qr_litter");
    payment_set_qr_money_keypad_done();
    free(litter);
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
