#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "mqtt_client.h"

typedef struct {
    char msg_id[33];
    uint32_t money;
    uint32_t litter;
    uint16_t price;
    uint8_t type;
    bool active;
    bool started;
    bool cancelled;
} payment_saved_t;

esp_err_t payment_journal_init(void);
bool payment_journal_available(void);
bool payment_journal_save_active(const payment_saved_t *active);
bool payment_journal_load_active(payment_saved_t *active);
bool payment_journal_complete(const char *topic, const char *payload,
                              const char *receipt_time, uint32_t money, uint32_t milliliters);
bool payment_journal_receipt_seen(const char *receipt_time);
void payment_journal_attach(esp_mqtt_client_handle_t client);
void payment_journal_connected(bool connected);
void payment_journal_puback(int packet_id);
