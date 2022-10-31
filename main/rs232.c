
#include "app_common_interfaces.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

/**
 *
 * - Port: configured UART
 * - Receive (Rx) buffer: on
 * - Transmit (Tx) buffer: off
 * - Flow control: off
 * - Event queue: off
 * - Pin assignment: see defines below (See Kconfig)
 */

#define FD_RS232_TXD (17)
#define FD_RS232_RXD (16)
#define FD_RS232_RTS (UART_PIN_NO_CHANGE)
#define FD_RS232_CTS (UART_PIN_NO_CHANGE)

#define FD_UART_PORT_NUM      2
#define FD_UART_BAUD_RATE     4800
#define FD_TASK_STACK_SIZE    2048

#define BUF_SIZE (1024)

#define E_INT_PIN (27)
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
typedef enum {
    IDLE = 0,
    FUEL_PUMPING,
    USER_SET_PRICE,
    USER_PRESS_T,
    USER_PRESS_T8_OR_TS,
    USER_RESET_WORKING_SHIFT, // T 8 
    USER_CHECK_WORKING_SHIFT, // T L
    USER_CHECK_ENTIRE_SHIFT, // T $ 123456
    USER_FILL_PW,
    RESET_WORKING_SHIFT, // After filling pw, press E to reset working shift.
} rs232_state;

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
bool user_pressed_E = false;
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
        int len = uart_read_bytes(FD_UART_PORT_NUM, atc_data, 1, 20 / portTICK_RATE_MS);
        if(*atc_data == 65) { // Check 'A'
            memset(atc_data, 0, 1);
            len = uart_read_bytes(FD_UART_PORT_NUM, atc_data, 1, 20 / portTICK_RATE_MS);
            if(*atc_data == 84) { // Check 'T'
                memset(atc_data, 0, 1);
                len = uart_read_bytes(FD_UART_PORT_NUM, atc_data, 1, 20 / portTICK_RATE_MS);
                if(*atc_data == 67) { // Check 'C'
                    memset(fd_op.data, 0, 21);
                    len = uart_read_bytes(FD_UART_PORT_NUM, fd_op.data, 21, 20 / portTICK_RATE_MS);
                    // printf("byte : %d  %d  %d  %d \n", data[18], data[19], data[20],data[21]);
                    /** check byte -2 
                     * 0x44 means nozzle is not lifting
                     * 0x40 means nozzle is being lifted
                     * 0x45 means device in setprice mode 
                     */
                    printf("Raw data:   ");
                    for(int j = 0; j < 21; j++) {
                        printf("%x ", fd_op.data[j]);
                    } 
                    printf("\t state: %d , E: %d\n   ", fd_op.state, user_pressed_E);



                    switch(fd_op.state) {
                        case IDLE:
                            user_pressed_E = false;
                            copy_21_bytes(fd_op.normal_data, fd_op.data);
                            // lift noozle, start pumping
                            if( (fd_op.data[19] & 0x0F) == 0x00) { // lift noozle, start pumping
                                ESP_LOGI("STATE_MACHINE", "user lift nozzle  >>>>> switch to FUEL_PUMPING mode");
                                fd_op.state = FUEL_PUMPING;
                            }
                            // Press T
                            if(compare_5_bytes(fd_op.data, T) && (fd_op.data[19] & 0x0F) == 0x05) {
                                ESP_LOGI("STATE_MACHINE", "user press T  >>>>> switch to USER_PRESS_T mode");
                                fd_op.state = USER_PRESS_T;
                            }
                            // Press P
                            if(compare_5_bytes(fd_op.data, FILLING_PW) && (fd_op.data[19] & 0x0F) == 0x05) {
                                ESP_LOGI("STATE_MACHINE", "user press P  >>>>> switch to USER_SET_PRICE mode");
                                fd_op.state = USER_SET_PRICE;
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
                            printf("REAL PUMPING: %d\n", real_pumping);
                            if( (fd_op.data[19] & 0x0F) == 0x04 && fd_op.prev_nozzle_stat == 0x00) { // unlift noozle, Finish pumping
                                if(real_pumping) {
                                    for(int j = 0; j < 21; j++) {
                                        DecToHexStr(fd_op.data[j], fd_op.c_data+j*2);
                                        printf("%x ", fd_op.data[j]);
                                    }
                                    printf("Raw data: %s\n", fd_op.data);
                                    if(xQueueSend(uplink_queue, (void *)&fd_op.c_data, 10) == pdTRUE) {   
                                        printf("Read successfully, send data: %s  to queue\n", fd_op.c_data);
                                        ESP_LOGI("STATE_MACHINE", "user unlift nozzle  >>>>> switch to IDLE mode");
                                        fd_op.state = IDLE;
                                    }
                                } else {
                                    printf("User lifted nozzle but did not pump\n");
                                    ESP_LOGI("STATE_MACHINE", "user unlift nozzle  >>>>> switch to IDLE mode");
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

                                if(xQueueSend(uplink_queue, (void *)&fd_op.c_data, 10) == pdTRUE) {   
                                    printf("Read successfully, send data: %s  to queue\n", fd_op.c_data);
                                    ESP_LOGI("STATE_MACHINE", "setting price is done  >>>>> switch to IDLE mode");
                                    fd_op.state = IDLE;
                                }
                            }
                            break;
                        case USER_PRESS_T:
                            if((fd_op.data[19] & 0x0F) == 0x04) {
                                ESP_LOGI("STATE_MACHINE", "user press C  >>>>> switch to IDLE mode");
                                fd_op.state = IDLE;
                            }
                            if(compare_5_bytes(fd_op.data, FILLING_PW)) {
                                ESP_LOGI("STATE_MACHINE", "user press 8 or $  >>>>> switch to USER_PRESS_T8_OR_TS mode");
                                fd_op.state = USER_PRESS_T8_OR_TS;
                            }
                            break;

                        case USER_PRESS_T8_OR_TS:
                            // waiting for user provide PW and press E
                            printf("PRev : %x  %d\n", fd_op.prev_data[11], gpio_get_level(27));
                            if(fd_op.prev_data[11] == 0x9c && user_pressed_E && (fd_op.data[19] & 0x0F) == 0x04) {  // Check final PW char and btn E
                                    ESP_LOGI("STATE_MACHINE", "Clear working shift  >>>>> switch to USER_RESET_WORKING_SHIFT mode");
                                    fd_op.state = USER_RESET_WORKING_SHIFT;
                            } else if (fd_op.prev_data[11] == 0x9c && user_pressed_E && (fd_op.data[19] & 0x0F) == 0x05) { // Pressed T$
                                    ESP_LOGI("STATE_MACHINE", "Check  the entire data  >>>>> switch to USER_CHECK_ENTIRE_SHIFT mode");
                                    fd_op.state = USER_CHECK_ENTIRE_SHIFT;
                            // } else if ((fd_op.data[19] & 0x0F) == 0x04) {
                            //     ESP_LOGI("STATE_MACHINE", "Check data succeed  >>>>> switch to IDLE mode");
                            //     fd_op.state = IDLE;
                            } else {
                                copy_21_bytes(fd_op.prev_data, fd_op.data);
                                continue;
                            }
                            //gpio_intr_enable(27);
                            break;
                        case USER_RESET_WORKING_SHIFT: 
                            for(int j = 0; j < 21; j++) {
                                DecToHexStr(fd_op.data[j],fd_op.c_data+j*2);
                                printf("%x ", fd_op.data[j]);
                            }
                            fd_op.c_data[0]=0xF0;
                            if(xQueueSend(uplink_queue, (void *)&fd_op.c_data, 10) == pdTRUE) {   
                                printf("Read successfully, send data: %s  to queue\n", fd_op.c_data);
                                ESP_LOGI("STATE_MACHINE", "Clear working shift succeed  >>>>> switch to IDLE mode");
                                fd_op.state = IDLE;
                            }
                            break;
                        case USER_CHECK_WORKING_SHIFT:
                        case USER_CHECK_ENTIRE_SHIFT:
                        case USER_FILL_PW:
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
static volatile int disable_intr_times = 0;  // use this to calculate how many times it go into interrupt

void IRAM_ATTR btn_E_gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    disable_intr_times++;
    esp_rom_printf("GPIO[%d] intr, val: %d, disable_intr_times = %d\n", gpio_num, gpio_get_level(gpio_num), disable_intr_times);
    gpio_intr_disable(gpio_num);
}

static gpio_config_t init_io(gpio_num_t num)
{
    //TEST_ASSERT(num < TEST_GPIO_OUTPUT_MAX);
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << num);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    return io_conf;
}

void config_btn_E_interrupt() {
    gpio_config_t io_int_config=init_io(E_INT_PIN);

    //io_int_config.pin_bit_mask = 1ULL<<E_INT_PIN;
    io_int_config.intr_type = GPIO_INTR_ANYEDGE;
    io_int_config.mode = GPIO_MODE_INPUT;
    io_int_config.pull_up_en = 1;
    gpio_config(&io_int_config);
    gpio_set_intr_type(E_INT_PIN, GPIO_INTR_LOW_LEVEL);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(E_INT_PIN, btn_E_gpio_isr_handler, (void *) E_INT_PIN);
    //gpio_set_level(TEST_GPIO_EXT_OUT_IO, 0);
    printf("get level:%d\n", gpio_get_level(E_INT_PIN));

    // gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1 );
    //gpio_isr_handler_add(E_INT_PIN, btn_E_gpio_isr_handler, (void*) E_INT_PIN);
    //gpio_set_level(1);
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
    printf("Configure interrupt pin BTN E \n");
    config_btn_E_interrupt();
   // pinMode()
    printf("Create RS232 task \n");
    xTaskCreate(&read_rs232_task, "read_rs232_task", FD_TASK_STACK_SIZE, NULL, configMAX_PRIORITIES, NULL);
}
