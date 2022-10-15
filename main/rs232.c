
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
void DecToHexStr(int dec, char *str) { 
    sprintf(str, "%2x", dec); 
    if(str[0] == ' ') 
        str[0] = '0';
}
int convert_hex_int(int hex) {
    return (hex & 0x0f) + (hex >> 4)*10;
}
static void read_rs232_task(void *arg)
{
    int index=0;
    int8_t prev_stat=4;
    // Configure a temporary buffer for the incoming data
    uint8_t *atc_data = (uint8_t *) malloc(1);
    bool fuel_pumping = false;
    // uint8_t *data = (uint8_t *) malloc(21);
    char *data = (char *) malloc(21);
    char *c_data = (char *)malloc(42);
    char *liter = (char *)malloc(11); memset(liter, 0, 11); liter[11] = '/0';
    char *money = (char *)malloc(11); memset(money, 0, 11); money[11] = '/0';
    char *price = (char *)malloc(7);  memset(price, 0, 7);  price[7] = '/0';
    
    while (1) {
        //printf("read from RS232\n");
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
                    memset(data, 0, 21);
                    len = uart_read_bytes(FD_UART_PORT_NUM, data, 21, 20 / portTICK_RATE_MS);
                    // printf("byte : %d  %d  %d  %d \n", data[18], data[19], data[20],data[21]);
                    /** check byte -2 
                     * 0x44 means nozzle is not lifting
                     * 0x40 means nozzle is being lifted
                     * 0x45 means device in setprice mode 
                     */
                    // printf("Bit snozzle status: %x, %x , litter: %d\n", data[19], data[19] & 0x0f,
                    //                             convert_hex_int(data[4]) + convert_hex_int(data[3])*100 + convert_hex_int(data[2])*10000 + convert_hex_int(data[1])*1000000);
                    // printf("Raw data: %x  %x  %x  %x  %x  ----- %d\n", data[0], data[1], data[2], data[3], data[4], fuel_pumping);
                    if( (data[19] & 0x0F) == 0x00) { // lift snoozle

////////////////////////// need to fix -- need to caculate on decimal instead of hex //////////////////////////////
                        if(    (convert_hex_int(data[4]) + 
                                convert_hex_int(data[3])*100 + 
                                convert_hex_int(data[2])*10000 + 
                                convert_hex_int(data[1])*1000000) < 60  ) {  // litter > 0.060 
                            fuel_pumping = false;
                            continue;
                        }
                        fuel_pumping = true;
                    }
                    
                    if( (data[19] & 0x0F) == 0x04 && prev_stat == 0x00 && fuel_pumping) { // unlift snoozle
                        for(int j = 0; j < 21; j++) {
                            DecToHexStr(data[j],c_data+j*2);
                            // printf("%x ", data[j]);
                        }
                        printf("Raw data: %s\n", data);
                        // if(index < 20) {
                        //     index++;
                        //     continue;
                        // }
                        // index=0;
                        printf("Prepare data\n");
                        if(xQueueSend(uplink_queue, (void *)&c_data, 10) == pdTRUE) {   
                            printf("Read successfully, send data: %s  to queue\n", c_data);
                        }
                        
                    } else if ((data[19] & 0x0F) == 0x04 && prev_stat == 0x05) { // change price -> send new price only
                        for(int j = 0; j < 21; j++) {
                            if(j < 10)
                                DecToHexStr(0, c_data + j*2);
                            else
                                DecToHexStr(data[j], c_data + j*2);
                        }

                        if(xQueueSend(uplink_queue, (void *)&c_data, 10) == pdTRUE) {   
                            printf("Read successfully, send data: %s  to queue\n", c_data);
                        }
                    }
                    
                    prev_stat = data[19] & 0x0F;

                }
            }
        }

    }
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

    printf("Create RS232 task \n");
    xTaskCreate(&read_rs232_task, "read_rs232_task", FD_TASK_STACK_SIZE, NULL, configMAX_PRIORITIES, NULL);
}
