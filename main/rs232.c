
#include "app_common_interfaces.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#define TAG "RS232"

/**
 *
 * - Port: configured UART
 * - Receive (Rx) buffer: on
 * - Transmit (Tx) buffer: off
 * - Flow control: off
 * - Event queue: off
 * - Pin assignment: see defines below (See Kconfig)
 */

#define FD_RS232_TXD (10)
#define FD_RS232_RXD (11)
#define FD_RS232_RTS (UART_PIN_NO_CHANGE)
#define FD_RS232_CTS (UART_PIN_NO_CHANGE)

#define FD_UART_PORT_NUM      2
#define FD_UART_BAUD_RATE     4800
#define FD_TASK_STACK_SIZE    2048

#define BUF_SIZE (1024)


// // old HW
// #define COL_8_INT_PIN (27) //button col 8
// #define ROW_8_INT_PIN (26) //button row 8
// #define ROW_E_INT_PIN (25) //button row E
// #define COL_E_INT_PIN (33) //button col E

// ESP32-S3 keypad matrix wiring
#define KEYPAD_ROW_1_PIN GPIO_NUM_41
#define KEYPAD_ROW_2_PIN GPIO_NUM_40
#define KEYPAD_ROW_3_PIN GPIO_NUM_39
#define KEYPAD_ROW_4_PIN GPIO_NUM_38
#define KEYPAD_ROW_5_PIN GPIO_NUM_20

#define KEYPAD_COL_1_PIN GPIO_NUM_37
#define KEYPAD_COL_2_PIN GPIO_NUM_36
#define KEYPAD_COL_3_PIN GPIO_NUM_35
#define KEYPAD_COL_4_PIN GPIO_NUM_21

// Compatibility aliases used by the existing state machine.
#define ROW_8_INT_PIN KEYPAD_ROW_3_PIN
#define COL_8_INT_PIN KEYPAD_COL_2_PIN
#define ROW_E_INT_PIN KEYPAD_ROW_4_PIN
#define COL_E_INT_PIN KEYPAD_COL_3_PIN

static volatile int col_8_detected=0,row_8_detected=0;
static volatile int col_E_detected=0,row_E_detected=0;

#define KEYPAD_DEBOUNCE_MS 250
#define KEYPAD_ROW_COUNT 5
#define KEYPAD_COL_COUNT 4

static gpio_num_t keypad_row_pins[KEYPAD_ROW_COUNT] = {
    KEYPAD_ROW_1_PIN, KEYPAD_ROW_2_PIN, KEYPAD_ROW_3_PIN,
    KEYPAD_ROW_4_PIN, KEYPAD_ROW_5_PIN
};

static gpio_num_t keypad_col_pins[KEYPAD_COL_COUNT] = {
    KEYPAD_COL_1_PIN, KEYPAD_COL_2_PIN,
    KEYPAD_COL_3_PIN, KEYPAD_COL_4_PIN
};

static char keypad_keys[KEYPAD_ROW_COUNT][KEYPAD_COL_COUNT] = {
    {'1', '2', '3', 'L'},
    {'4', '5', '6', '$'},
    {'7', '8', '9', 'P'},
    {'C', '0', 'E', 'T'},
    {'1', '2', '3', '4'} // F1..F4; row 5 is formatted specially.
};

static volatile TickType_t keypad_last_press_tick[KEYPAD_ROW_COUNT][KEYPAD_COL_COUNT];

// Util functions
void DecToHexStr(int dec, char *str) { 
    sprintf(str, "%2x", dec); 
    if(str[0] == ' ') 
        str[0] = '0';
}
int convert_hex_int(int hex) {
    return (hex & 0x0f) + (hex >> 4)*10;
}
bool compare_5_bytes(const char arr1[], const char arr2[]) {
    for(int i = 0; i < 5; i++) {
        if(arr1[i] != arr2[i])
            return false;
    }
    return true;
}
void copy_21_bytes(char dest[], const char source[]) {
    for(int i = 0; i < 21; i++) {
        dest[i] = source[i];
    }
}

// FUEL DISPENSER STATE
typedef enum {
    IDLE = 0,
    FUEL_PUMPING,
    USER_SET_PRICE,
    USER_PRESS_T,
    USER_PRESS_T8,
    USER_RESET_WORKING_SHIFT, // T 8 
    USER_CHECK_WORKING_SHIFT, // T L
    USER_CHECK_ENTIRE_SHIFT, // T $ 123456
    USER_FILL_PW,
    RESET_WORKING_SHIFT, // After filling pw, press E to reset working shift.
    USER_SETUP_MONEY_LITTER, // $ or L
} rs232_state;


// Main struct to manage rs232 data
typedef struct {
    rs232_state state;
    char *data;
    char *prev_data;
    char *normal_data;
    char *c_data;
    char *liter;
    char *money;
    char *price;
    int8_t nozzle_stat;
    int8_t prev_nozzle_stat;
} rs232_obj;
const char T[5]  = {0x80, 0x80, 0x80, 0x80, 0x80};
const char FILLING_PW[5] = {0xff, 0x92, 0x92, 0x88, 0x8c};
const char SETUP_MONEY[5] = {0x87, 0x88, 0x21, 0xf9, 0x88};
const char SETUP_LITTER[5] = {0x87, 0x88, 0xa1, 0xf9, 0x88};
const char LIFT_NOZZLE[5] = {0, 0, 0, 0, 0};
bool user_pressed_E = false;

// Thread read RS232 data and send it to MQTT thread
static void read_rs232_task(void *arg)
{
    int index=0;
    // Configure a temporary buffer for the incoming data
    uint8_t *atc_data = (uint8_t *) malloc(1);
    bool real_pumping = false;
    // uint8_t *data = (uint8_t *) malloc(21);
    
    rs232_obj fd_op;
    fd_op.state = IDLE;
    fd_op.prev_nozzle_stat = 4;
    fd_op.data = (char *) malloc(21);
    fd_op.prev_data = (char *) malloc(21);
    fd_op.normal_data = (char *) malloc(21);
    fd_op.c_data = (char *)malloc(42);
    fd_op.liter = (char *)malloc(11); memset(fd_op.liter, 0, 11);
    fd_op.money = (char *)malloc(11); memset(fd_op.money, 0, 11);
    fd_op.price = (char *)malloc(7);  memset(fd_op.price, 0, 7);
    
    while (1) {
        
        // Read data from the UART
        memset(atc_data, 0, 1);
        int len = uart_read_bytes(FD_UART_PORT_NUM, atc_data, 1, 20 / portTICK_PERIOD_MS);
        if(*atc_data == 65) { // Check 'A'
            memset(atc_data, 0, 1);
            len = uart_read_bytes(FD_UART_PORT_NUM, atc_data, 1, 20 / portTICK_PERIOD_MS);
            if(*atc_data == 84) { // Check 'T'
                memset(atc_data, 0, 1);
                len = uart_read_bytes(FD_UART_PORT_NUM, atc_data, 1, 20 / portTICK_PERIOD_MS);
                if(*atc_data == 67) { // Check 'C'
                    memset(fd_op.data, 0, 21);
                    len = uart_read_bytes(FD_UART_PORT_NUM, fd_op.data, 21, 20 / portTICK_PERIOD_MS);
                    // printf("byte : %d  %d  %d  %d \n", data[18], data[19], data[20],data[21]);
                    /** check byte -2 
                     * 0x44 means nozzle is not lifting
                     * 0x40 means nozzle is being lifted
                     * 0x45 means device in setprice mode 
                     */
                    #ifdef RAW_DATA
                    printf("Raw data:   ");
                    for(int j = 0; j < 21; j++) {
                        printf("%x ", fd_op.data[j]);
                    } 
                    printf("\t state: %d , E: %d\n   ", fd_op.state, user_pressed_E);
                    #endif


                    switch(fd_op.state) {
                        case IDLE:
                            user_pressed_E = false;
                            copy_21_bytes(fd_op.normal_data, fd_op.data);
                            if ((fd_op.data[19] & 0x0F) == 0x05) {
                                ESP_LOGI("STATE_MACHINE",
                                         "IDLE key candidate: data[0..4]=%02x %02x %02x %02x %02x, data[19]=0x%02x, T_match=%d",
                                         (uint8_t)fd_op.data[0],
                                         (uint8_t)fd_op.data[1],
                                         (uint8_t)fd_op.data[2],
                                         (uint8_t)fd_op.data[3],
                                         (uint8_t)fd_op.data[4],
                                         (uint8_t)fd_op.data[19],
                                         compare_5_bytes(fd_op.data, T));
                            }
                            // lift noozle, start pumping
                            if( (fd_op.data[19] & 0x0F) == 0x00 ) { // lift noozle, start pumping
                                ESP_LOGD("STATE_MACHINE", "user lift nozzle  >>>>> switch to FUEL_PUMPING mode");
                                fd_op.state = FUEL_PUMPING;
                            }
                            // Press T
                            if(compare_5_bytes(fd_op.data, T) && (fd_op.data[19] & 0x0F) == 0x05) {
                                ESP_LOGI("STATE_MACHINE", "user press T >>>>> switch to USER_PRESS_T mode");
                                fd_op.state = USER_PRESS_T;
                            }
                            // Press P
                            if(compare_5_bytes(fd_op.data, FILLING_PW) && (fd_op.data[19] & 0x0F) == 0x05) {
                                ESP_LOGD("STATE_MACHINE", "user press P  >>>>> switch to USER_SET_PRICE mode");
                                fd_op.state = USER_SET_PRICE;
                            }
                            // Press L
                            if(compare_5_bytes(fd_op.data, SETUP_LITTER) && (fd_op.data[19] & 0x0F) == 0x05) {
                                ESP_LOGD("STATE_MACHINE", "user press L  >>>>> switch to USER_SETUP_MONEY_LITTER mode");
                                fd_op.state = USER_SETUP_MONEY_LITTER;
                            }
                            // Press $
                            if(compare_5_bytes(fd_op.data + 6, SETUP_MONEY) && (fd_op.data[19] & 0x0F) == 0x05) {
                                ESP_LOGD("STATE_MACHINE", "user press $  >>>>> switch to USER_SETUP_MONEY_LITTER mode");
                                fd_op.state = USER_SETUP_MONEY_LITTER;
                            }
                            

                            break;

                        case USER_SETUP_MONEY_LITTER:
                            if((fd_op.data[19] & 0x0F) == 0x04) {
                                ESP_LOGD("STATE_MACHINE", "user press C  >>>>> switch to IDLE mode");
                                fd_op.state = IDLE;
                                break;
                            }

                            // lift noozle, start pumping
                            if( (fd_op.data[19] & 0x0F) == 0x00 && compare_5_bytes(fd_op.data, LIFT_NOZZLE)) { // lift noozle, start pumping
                                ESP_LOGD("STATE_MACHINE", "user lift nozzle  >>>>> switch to FUEL_PUMPING mode");
                                fd_op.state = FUEL_PUMPING;
                            }

                            break;
                        case FUEL_PUMPING:
                            if(        (convert_hex_int(fd_op.prev_data[4]) + 
                                        convert_hex_int(fd_op.prev_data[3])*100 + 
                                        convert_hex_int(fd_op.prev_data[2])*10000 + 
                                        convert_hex_int(fd_op.prev_data[1])*1000000) < 60  ) {
                                real_pumping = false;
                            } else {
                                real_pumping = true;
                            }
                            // ESP_LOGI(TAG,"REAL PUMPING: %d\n", real_pumping);
                            if( (fd_op.data[19] & 0x0F) == 0x04 && fd_op.prev_nozzle_stat == 0x00) { // unlift noozle, Finish pumping
                                if(real_pumping) {
                                    for(int j = 0; j < 21; j++) {
                                        DecToHexStr(fd_op.data[j], fd_op.c_data+j*2);
                                        // ESP_LOGI(TAG,"%x ", fd_op.data[j]);
                                    }
                                    // ESP_LOGI(TAG,"Raw data: %s\n", fd_op.data);

                                    // After received data, send it to MQTT thread
                                    if(xQueueSend(uplink_queue, (void *)&fd_op.c_data, 10) == pdTRUE) {   
                                        ESP_LOGI(TAG,"Read successfully, send data to queue");
                                        ESP_LOGD("STATE_MACHINE", "user unlift nozzle  >>>>> switch to IDLE mode");
                                        fd_op.state = IDLE;
                                    }
                                } else {
                                    ESP_LOGI(TAG,"User lifted nozzle but did not pump");
                                    ESP_LOGD("STATE_MACHINE", "user unlift nozzle  >>>>> switch to IDLE mode");
                                    fd_op.state = IDLE;
                                }
                            }
                            break;
                        case USER_SET_PRICE:
                            // waiting until set price successfully
                            if((fd_op.data[19] & 0x0F) == 0x04 && fd_op.prev_nozzle_stat == 0x05) {
                                 for(int j = 0; j < 21; j++) {
                                    if(j < 10)
                                        DecToHexStr(0, fd_op.c_data + j*2);
                                    else
                                        DecToHexStr(fd_op.data[j], fd_op.c_data + j*2);
                                }
                                // After set price success, send the new price to MQTT thread
                                if(xQueueSend(uplink_queue, (void *)&fd_op.c_data, 10) == pdTRUE) {   
                                    ESP_LOGI(TAG,"Read successfully, send data to queue");
                                    ESP_LOGD("STATE_MACHINE", "setting price is done  >>>>> switch to IDLE mode");
                                    fd_op.state = IDLE;
                                }
                            }
                            break;
                        case USER_PRESS_T:
                            if((fd_op.data[19] & 0x0F) == 0x04) {
                                ESP_LOGI("STATE_MACHINE", "USER_PRESS_T: user press C >>>>> switch to IDLE mode");
                                fd_op.state = IDLE;
                                break;
                            }
                            gpio_intr_enable(ROW_8_INT_PIN);
                            // gpio_intr_enable(COL_E_INT_PIN);
                            // gpio_intr_enable(ROW_E_INT_PIN);
                            //Checking button 8 is press
                            if(row_8_detected && col_8_detected)
                            {
                                if(compare_5_bytes(fd_op.data, FILLING_PW) /*and button 8 is press*/) 
                                {                                
                                    ESP_LOGI("STATE_MACHINE", "USER_PRESS_T: row/col 8 detected, user press 8 >>>>> switch to USER_PRESS_T8 mode");
                                    fd_op.state = USER_PRESS_T8;
                                    row_8_detected=col_8_detected=0; //clear int pins
                                    gpio_intr_disable(ROW_8_INT_PIN);
                                }
                                else {
                                    ESP_LOGI("STATE_MACHINE", "USER_PRESS_T: row/col 8 detected but RS232 data does not match FILLING_PW, data[19]=0x%02x", fd_op.data[19]);
                                    row_8_detected = col_8_detected = 0;
                                    gpio_intr_enable(ROW_8_INT_PIN);
                                }
                            }
                            else if(row_8_detected || col_8_detected)
                            {
                                ESP_LOGI("STATE_MACHINE", "USER_PRESS_T: partial 8 detect row=%d col=%d, clear and wait again", row_8_detected, col_8_detected);
                                row_8_detected=col_8_detected=0;
                                gpio_intr_enable(ROW_8_INT_PIN);
                            }
                            break;

                        case USER_PRESS_T8:
                            // waiting for user provide PW and press E
                            //printf("PRev : %x  %d\n", fd_op.prev_data[11], gpio_get_level(27));
                            if(fd_op.data[11] == 0x9c ) //Password is filled
                            {
                                fd_op.state=USER_FILL_PW;
                                row_E_detected=col_E_detected=0;
                                gpio_intr_enable(ROW_E_INT_PIN);
                                ESP_LOGD("STATE_MACHINE", "USER_PRESS_T8 >>>>> USER_FILL_PW");
                            }
                            if((fd_op.data[19] & 0x0F) == 0x04) {
                                ESP_LOGD("STATE_MACHINE", "user press C  >>>>> switch to IDLE mode");
                                fd_op.state = IDLE;
                                break;
                            }
                            copy_21_bytes(fd_op.prev_data, fd_op.data); //save current data                                         

                            //gpio_intr_enable(27);
                            break;
                        case USER_RESET_WORKING_SHIFT: 
                            // if((fd_op.data[19] & 0x0F) == 0x05) {
                            //     ESP_LOGI("tmp", "Skip 1st message");
                            //     break;
                            // }
                            if((fd_op.data[19] & 0x0F) == 0x04) {  
                                for(int j = 0; j < 21; j++) {
                                    DecToHexStr(fd_op.data[j],fd_op.c_data+j*2);
                                    // printf("%x ", fd_op.data[j]);
                                }
                                fd_op.c_data[0]=0xF0; //special character for sending end shift
                                if(xQueueSend(uplink_queue, (void *)&fd_op.c_data, 10) == pdTRUE) {   
                                    ESP_LOGI(TAG,"Read successfully, send data to queue");
                                    ESP_LOGD("STATE_MACHINE", "Clear working shift succeed  >>>>> switch to IDLE mode");
                                    fd_op.state = IDLE;
                                }
                            }
                            else
                            {
                                fd_op.state=USER_PRESS_T8;
                                ESP_LOGD("STATE_MACHINE", "Wrong password >>>>> switch to USER_PRESS_T8 mode");
                            }
                            break;
                        case USER_CHECK_WORKING_SHIFT:
                        case USER_CHECK_ENTIRE_SHIFT:
                            break;
                        case USER_FILL_PW:
                            if(row_E_detected && col_E_detected) // Check final PW char and btn E
                            {
                                ESP_LOGI(TAG,"row_E_detected %d, col_E_detected %d, fd_op.data[19] %d",row_E_detected,col_E_detected, fd_op.data[19]);
                                user_pressed_E = true;
                                row_E_detected=col_E_detected=0; //clear int pins
                                gpio_intr_disable(ROW_E_INT_PIN);
                                
                                // if((fd_op.data[19] & 0x0F) == 0x04) {  
                                //         ESP_LOGD("STATE_MACHINE", "Clear working shift  >>>>> switch to USER_RESET_WORKING_SHIFT mode");
                                //         fd_op.state = USER_RESET_WORKING_SHIFT;//USER_RESET_WORKING_SHIFT_E_BTN_CHECK;
                                // }
                            }
                            else if(row_E_detected || col_E_detected)
                            {
                                row_E_detected=col_E_detected=0; 
                                gpio_intr_enable(ROW_E_INT_PIN);
                                // printf("Clear col and row to zero\n");
                            } 
                            if(((fd_op.data[19] & 0x0F) == 0x04) && user_pressed_E) {
                                fd_op.state = USER_RESET_WORKING_SHIFT;//USER_RESET_WORKING_SHIFT_E_BTN_CHECK;
                                ESP_LOGD("STATE_MACHINE", "USER_FILL_PW >>>>> Enable E USER_RESET_WORKING_SHIFT");\
                                break;
                            } else if (((fd_op.data[19] & 0x0F) == 0x05) && user_pressed_E && fd_op.data[6] == 0xff) {
                                fd_op.state=USER_PRESS_T8;
                                ESP_LOGD("STATE_MACHINE", "Wrong password >>>>> switch to USER_PRESS_T8 mode");
                                break;
                            }

                            if((fd_op.data[19] & 0x0F) == 0x04) {
                                ESP_LOGD("STATE_MACHINE", "user press C  >>>>> switch to IDLE mode");
                                fd_op.state = IDLE;
                                break;
                            }
                            break;
                        case RESET_WORKING_SHIFT:
                            break;
                    }
                    fd_op.prev_nozzle_stat = fd_op.data[19] & 0x0F;
                    copy_21_bytes(fd_op.prev_data, fd_op.data);
                }
            }
        }

    }
}

static void IRAM_ATTR keypad_row_gpio_isr_handler(void *arg)
{
    uint32_t row = (uint32_t)(uintptr_t)arg;
    uint32_t active_columns = 0;

    for (uint32_t col = 0; col < KEYPAD_COL_COUNT; col++) {
        if (gpio_get_level(keypad_col_pins[col]) == 1) {
            active_columns |= (1U << col);
        }
    }

    // Accept only a one-hot column sample. Zero or multiple active columns
    // mean that the scan phase is not suitable for identifying a key.
    if (active_columns == 0 ||
        (active_columns & (active_columns - 1U)) != 0) {
        return;
    }

    uint32_t col = 0;
    while ((active_columns & (1U << col)) == 0) {
        col++;
    }

    TickType_t now = xTaskGetTickCountFromISR();
    if ((now - keypad_last_press_tick[row][col]) <
        pdMS_TO_TICKS(KEYPAD_DEBOUNCE_MS)) {
        return;
    }
    keypad_last_press_tick[row][col] = now;

    if (row == 4) {
        esp_rom_printf("KEY PRESSED: F%c (R%u C%u)\n",
                       keypad_keys[row][col], row + 1, col + 1);
    } else {
        esp_rom_printf("KEY PRESSED: %c (R%u C%u)\n",
                       keypad_keys[row][col], row + 1, col + 1);
    }
}

void config_btn_E_interrupt() {
    uint64_t row_pin_mask = 0;
    uint64_t col_pin_mask = 0;

    for (uint32_t row = 0; row < KEYPAD_ROW_COUNT; row++) {
        row_pin_mask |= (1ULL << keypad_row_pins[row]);
    }
    for (uint32_t col = 0; col < KEYPAD_COL_COUNT; col++) {
        col_pin_mask |= (1ULL << keypad_col_pins[col]);
    }

    gpio_config_t row_config = {
        .pin_bit_mask = row_pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&row_config));

    gpio_config_t col_config = {
        .pin_bit_mask = col_pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&col_config));

    for (uint32_t row = 0; row < KEYPAD_ROW_COUNT; row++) {
        ESP_ERROR_CHECK(gpio_isr_handler_add(
            keypad_row_pins[row],
            keypad_row_gpio_isr_handler,
            (void *)(uintptr_t)row));
        gpio_intr_enable(keypad_row_pins[row]);
        ESP_LOGI(TAG, "Keypad R%lu GPIO%d idle=%d",
                 (unsigned long)(row + 1), keypad_row_pins[row],
                 gpio_get_level(keypad_row_pins[row]));
    }

    ESP_LOGI(TAG, "Keypad columns: C1=GPIO%d C2=GPIO%d C3=GPIO%d C4=GPIO%d",
             KEYPAD_COL_1_PIN, KEYPAD_COL_2_PIN,
             KEYPAD_COL_3_PIN, KEYPAD_COL_4_PIN);
}

void rs232_config(void)
{
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = FD_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_driver_install(FD_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(FD_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(FD_UART_PORT_NUM, FD_RS232_TXD, FD_RS232_RXD, FD_RS232_RTS, FD_RS232_CTS));
    ESP_LOGI(TAG,"Configure interrupt pin BTN E ");
    config_btn_E_interrupt();
   // pinMode()
    ESP_LOGI(TAG,"Create RS232 task \n");
    xTaskCreate(&read_rs232_task, "read_rs232_task", FD_TASK_STACK_SIZE, NULL, 10, NULL);
    // Raw row/column debug polling is intentionally disabled. The keypad
    // scan produces many pulses for one press; accepted keys are logged once
    // by the row ISR instead.
    // xTaskCreate(&button_input_debug_task, "btn_input_debug", 4096, NULL, 5, NULL);
}
