#include "app_common_interfaces.h"
#include "driver/gpio.h"
#define PIN_CHANEL_SELECT_1A 14
#define PIN_CHANEL_SELECT_1B 12
#define PIN_CHANEL_SELECT_1C 13
#define PIN_INH_CLUSTER_1    32
#define PIN_CHANEL_SELECT_2A 19
#define PIN_CHANEL_SELECT_2B 18
#define PIN_CHANEL_SELECT_2C 5
#define PIN_INH_CLUSTER_2    23

static SemaphoreHandle_t set_price_task_sem;

char order[8] = {'P', '1', '2', '3','4', '5','6','E'};
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
pair_btn_pin mapping_table_1[8] = { // Mapping table 1 : between physical button and ESP output signal (row 2,3)
    {'$', 0},
    {'6', 1},
    {'4', 2},
    {'5', 3},
    {'2', 4},
    {'1', 5},
    {'L', 6},
    {'3', 7},
};
pair_btn_pin mapping_table_2[8] = { // Mapping table 2 : between physical button and ESP output signal (row 4,5)
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
        printf("ERROR: Invalid cluster\n");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void virtual_keypad_init() {
    for(int i=0; i<8; i++) {
        gpio_pad_select_gpio(pin_control[i]);
        gpio_set_direction(pin_control[i], GPIO_MODE_OUTPUT);
        if(pin_control[i] == PIN_INH_CLUSTER_1 || pin_control[i] == PIN_INH_CLUSTER_2) // Set INH to HIGH to disable all channels
            gpio_set_level(pin_control[i], 1); 
        else
            gpio_set_level(pin_control[i], 0);

    }
    set_price_task_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(set_price_task_sem);
}

void change_price_by_vir_keypad(char *price) {
    
    xSemaphoreTake(set_price_task_sem, portMAX_DELAY);
    //bset_price = true;
    printf("Press: ");
    for(int i = 0; i < sizeof(order); i++) {
        for(int j = 0; j < 8; j++) {
            if(order[i] == mapping_table_1[j].btn) 
                psudoe_press(1, mapping_table_1[j].pin);
            else if(order[i] == mapping_table_2[j].btn) 
                psudoe_press(2, mapping_table_2[j].pin);
        }
    }

    for(int i = 0; i < strlen(price); i++) {
        for(int j = 0; j < 8; j++) {
            if(price[i] == mapping_table_1[j].btn) 
                psudoe_press(1, mapping_table_1[j].pin);
            else if(price[i] == mapping_table_2[j].btn) 
                psudoe_press(2, mapping_table_2[j].pin);
        }
    }
    psudoe_press(2,1); // Press E to confirm the price
    printf("Session end.\n\n");
    //bset_price = false;
    xSemaphoreGive(set_price_task_sem);
    vTaskDelete(NULL);
    // vTaskSuspend(my_task_handler);
    // vTaskDelete(my_task_handler);
}