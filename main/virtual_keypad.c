#include "app_common_interfaces.h"
#include "driver/gpio.h"
#include <stdlib.h>

#define TAG "VIR_KEYPAD"

#define PIN_CHANEL_SELECT_1A 14
#define PIN_CHANEL_SELECT_1B 12
#define PIN_CHANEL_SELECT_1C 13
#define PIN_INH_CLUSTER_1    32
#define PIN_CHANEL_SELECT_2A 19
#define PIN_CHANEL_SELECT_2B 18
#define PIN_CHANEL_SELECT_2C 5
#define PIN_INH_CLUSTER_2    23

static SemaphoreHandle_t set_price_task_sem;
#define ORDER_CMD_SIZE 8
#define END_SESSION_CMD_SIZE 9
#define MAPPING_TABLE_SIZE 8

char order[ORDER_CMD_SIZE] = {'P', '1', '2', '3','4', '5','6','E'};
char end_session[END_SESSION_CMD_SIZE] = {'T', '8','1', '2', '3','4', '5','6','E'};
uint8_t pin_control[8] = {13,12,14,32,5,18,19,23};
char keys[4][4] =
{
    {'L', '3', '2', '1'},
    {'$', '6', '5', '4'},
    {'P', '9', '8', '7'},
    {'T', 'E', '0', 'C'},
};

typedef struct { // Same as pair in C++
    char btn;
    uint8_t pin;
} pair_btn_pin;
pair_btn_pin mapping_table_1[MAPPING_TABLE_SIZE] = { // Mapping table 1 : between physical button and ESP output signal (row 2,3)
    {'$', 0},
    {'6', 1},
    {'4', 2},
    {'5', 3},
    {'2', 4},
    {'1', 5},
    {'L', 6},
    {'3', 7},
};
pair_btn_pin mapping_table_2[MAPPING_TABLE_SIZE] = { // Mapping table 2 : between physical button and ESP output signal (row 4,5)
    {'T', 0},
    {'E', 1},
    {'C', 2},
    {'0', 3},
    {'8', 4},
    {'7', 5},
    {'P', 6},
    {'9', 7},
};

static void psudoe_press(uint8_t cluster, uint8_t pin) {
    if(cluster == 1) {
        gpio_set_level(PIN_CHANEL_SELECT_1A, pin&0x01);
        gpio_set_level(PIN_CHANEL_SELECT_1B, pin&0x02);
        gpio_set_level(PIN_CHANEL_SELECT_1C, pin&0x04);
        gpio_set_level(PIN_INH_CLUSTER_1, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(PIN_INH_CLUSTER_1, 1);  
    } else if(cluster == 2) {
        gpio_set_level(PIN_CHANEL_SELECT_2A, pin&0x01);
        gpio_set_level(PIN_CHANEL_SELECT_2B, pin&0x02);
        gpio_set_level(PIN_CHANEL_SELECT_2C, pin&0x04);
        gpio_set_level(PIN_INH_CLUSTER_2, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(PIN_INH_CLUSTER_2, 1); 
    } else {
        ESP_LOGI(TAG,"ERROR: Invalid cluster\n");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void press_key(char key)
{
    for(int j = 0; j < MAPPING_TABLE_SIZE; j++) {
        if(key == mapping_table_1[j].btn) {
            psudoe_press(1, mapping_table_1[j].pin);
            return;
        } else if(key == mapping_table_2[j].btn) {
            psudoe_press(2, mapping_table_2[j].pin);
            return;
        }
    }
    ESP_LOGW(TAG, "Unsupported virtual key: %c", key);
}

void virtual_keypad_init() {
    for(int i=0; i<MAPPING_TABLE_SIZE; i++) {
        esp_rom_gpio_pad_select_gpio(pin_control[i]);
        gpio_set_direction(pin_control[i], GPIO_MODE_OUTPUT);
        if(pin_control[i] == PIN_INH_CLUSTER_1 || pin_control[i] == PIN_INH_CLUSTER_2) // Set INH to HIGH to disable all channels
            gpio_set_level(pin_control[i], 1); 
        else
            gpio_set_level(pin_control[i], 0);

    }
    set_price_task_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(set_price_task_sem);
}

void change_price_by_vir_keypad(void *arg) {
    char *price = (char *)arg;
    
    xSemaphoreTake(set_price_task_sem, portMAX_DELAY);
    //bset_price = true;
    ESP_LOGI(TAG,"Press: ");
    for(int i = 0; i < sizeof(order); i++) {
        press_key(order[i]);
    }

    for(int i = 0; i < strlen(price); i++) {
        press_key(price[i]);
    }
    press_key('E'); // Press E to confirm the price
    ESP_LOGI(TAG,"Change price Session completed.\n\n");
    //bset_price = false;
    xSemaphoreGive(set_price_task_sem);
    free(price);
    vTaskDelete(NULL);
    // vTaskSuspend(my_task_handler);
    // vTaskDelete(my_task_handler);
}

void enter_qr_price_by_vir_keypad(void *arg) {
    char *amount = (char *)arg;

    xSemaphoreTake(set_price_task_sem, portMAX_DELAY);
    ESP_LOGI(TAG, "QR price input start: %s", amount);

    press_key('$');
    vTaskDelay(pdMS_TO_TICKS(1000));
    for(int i = 0; i < strlen(amount); i++) {
        press_key(amount[i]);
    }
    press_key('E');

    ESP_LOGI(TAG, "QR price input completed.\n\n");
    xSemaphoreGive(set_price_task_sem);
    free(amount);
    vTaskDelete(NULL);
}

void cancel_qr_money_by_vir_keypad(void *arg) {
    (void)arg;

    xSemaphoreTake(set_price_task_sem, portMAX_DELAY);
    ESP_LOGI(TAG, "Cancel QR money start: press C");

    press_key('C');

    ESP_LOGI(TAG, "Cancel QR money completed.\n\n");
    xSemaphoreGive(set_price_task_sem);
    vTaskDelete(NULL);
}

void end_session_by_vir_keypad(void *arg) {
    (void)arg;
    
    xSemaphoreTake(set_price_task_sem, portMAX_DELAY);
    //bset_price = true;
    ESP_LOGI(TAG,"End_Session start..........\n ");
    //end_session[9] = {'T', '8','1', '2', '3','4', '5','6','E'} -> array of characters for end current session
    for(int i = 0; i < sizeof(end_session); i++) {
        press_key(end_session[i]);
    }
    ESP_LOGI(TAG,"End_Session completed\n\n");    
    //bset_price = false;
    xSemaphoreGive(set_price_task_sem);
    vTaskDelete(NULL);
    // vTaskSuspend(my_task_handler);
    // vTaskDelete(my_task_handler);
}
