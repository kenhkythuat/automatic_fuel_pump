#include "payment_journal.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* One atomic NVS blob contains both active payment and pending completions.
 * A reset can therefore never fall between removing active and saving event. */
#define JOURNAL_MAGIC 0x504a0001UL
#define JOURNAL_SLOTS 4
typedef struct {
    char topic[128]; char payload[256]; char receipt_time[24];
    uint32_t money; uint32_t milliliters;
} saved_event_t;
typedef struct {
    uint32_t magic;
    payment_saved_t active;
    uint32_t count;
    saved_event_t events[JOURNAL_SLOTS];
    char last_time[24];
    uint32_t last_money;
    uint32_t last_milliliters;
} journal_t;
static journal_t journal;
static SemaphoreHandle_t lock;
static QueueHandle_t ack_queue;
static esp_mqtt_client_handle_t mqtt;
static volatile bool online;
static bool healthy;
static int inflight = -1;
static TickType_t sent_at;

/* Caller holds lock. Only replace RAM after the flash commit succeeds. */
static bool commit(const journal_t *next)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("pay_journal", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "state", next, sizeof(*next));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        healthy = false;
        ESP_LOGE("PAY_JOURNAL", "NVS commit failed: %s; block new payments", esp_err_to_name(err));
        return false;
    }
    journal = *next;
    healthy = true;
    return true;
}

bool payment_journal_available(void)
{
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool available = healthy && journal.count < JOURNAL_SLOTS && !journal.active.active;
    xSemaphoreGive(lock);
    return available;
}

bool payment_journal_save_active(const payment_saved_t *active)
{
    if (!lock || !active) return false;
    journal_t *next = malloc(sizeof(*next));
    if (!next) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    *next = journal;
    next->active = *active;
    bool ok = commit(next);
    xSemaphoreGive(lock);
    free(next);
    return ok;
}

bool payment_journal_load_active(payment_saved_t *active)
{
    if (!lock || !active) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    *active = journal.active;
    xSemaphoreGive(lock);
    return active->active;
}

bool payment_journal_receipt_seen(const char *receipt_time)
{
    if (!lock || !receipt_time || !receipt_time[0]) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool seen = strcmp(journal.last_time, receipt_time) == 0;
    for (uint32_t i = 0; i < journal.count; ++i) {
        seen |= strcmp(journal.events[i].receipt_time, receipt_time) == 0;
    }
    xSemaphoreGive(lock);
    return seen;
}

bool payment_journal_complete(const char *topic, const char *payload,
                              const char *receipt_time, uint32_t money, uint32_t milliliters)
{
    if (!lock || !topic || !payload || strlen(topic) >= 128 || strlen(payload) >= 256) return false;
    journal_t *next = malloc(sizeof(*next));
    if (!next) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = false;
    if (journal.count < JOURNAL_SLOTS) {
        *next = journal;
        strcpy(next->events[next->count].topic, topic);
        strcpy(next->events[next->count].payload, payload);
        if (receipt_time) {
            snprintf(next->events[next->count].receipt_time, 24, "%s", receipt_time);
            next->events[next->count].money = money;
            next->events[next->count].milliliters = milliliters;
        }
        next->count++;
        memset(&next->active, 0, sizeof(next->active));
        ok = commit(next);
        if (ok) ESP_LOGI("PAY_JOURNAL", "Completion saved before MQTT, pending=%lu", (unsigned long)journal.count);
    }
    xSemaphoreGive(lock);
    free(next);
    return ok;
}

void payment_journal_attach(esp_mqtt_client_handle_t client) { mqtt = client; }
void payment_journal_connected(bool connected) { online = connected; }
/* Callbacks only queue PUBACK; the worker commits flash outside MQTT callbacks.
 * No MQTT API is called while holding the journal lock. */
void payment_journal_puback(int packet_id)
{
    if (ack_queue) (void)xQueueSend(ack_queue, &packet_id, 0);
}

static void sender(void *arg)
{
    (void)arg;
    for (;;) {
        int ack;
        xSemaphoreTake(lock, portMAX_DELAY);
        while (xQueueReceive(ack_queue, &ack, 0) == pdTRUE) {
            if (inflight > 0 && ack == inflight && journal.count) {
                journal_t *next = malloc(sizeof(*next));
                if (next) {
                    *next = journal;
                    if (next->events[0].receipt_time[0]) {
                        strcpy(next->last_time, next->events[0].receipt_time);
                        next->last_money = next->events[0].money;
                        next->last_milliliters = next->events[0].milliliters;
                    }
                    next->count--;
                    memmove(next->events, next->events + 1, next->count * sizeof(saved_event_t));
                    memset(&next->events[next->count], 0, sizeof(saved_event_t));
                    if (commit(next)) {
                        ESP_LOGI("PAY_JOURNAL", "Broker PUBACK packet=%d; pending=%lu", ack, (unsigned long)journal.count);
                        inflight = -1;
                    }
                    free(next);
                }
            }
        }
        bool send = online && mqtt && journal.count &&
            (inflight < 0 || (TickType_t)(xTaskGetTickCount() - sent_at) > pdMS_TO_TICKS(30000));
        saved_event_t event = {0};
        if (send) event = journal.events[0];
        xSemaphoreGive(lock);
        if (send) {
            /* enqueue performs no socket write; one durable event in flight.
             * An expired/replayed packet may cause duplicates: server dedupes
             * by device + payment msg_id + event. */
            inflight = esp_mqtt_client_enqueue(mqtt, event.topic,
                        event.payload, 0, 1, 0, true);
            sent_at = xTaskGetTickCount();
            ESP_LOGI("PAY_JOURNAL", "Enqueued durable event packet=%d", inflight);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

esp_err_t payment_journal_init(void)
{
    lock = xSemaphoreCreateMutex();
    ack_queue = xQueueCreate(32, sizeof(int));
    if (!lock || !ack_queue) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("pay_journal", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    size_t size = sizeof(journal);
    err = nvs_get_blob(handle, "state", &journal, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&journal, 0, sizeof(journal));
        journal.magic = JOURNAL_MAGIC;
        if (!commit(&journal)) return ESP_FAIL;
    } else if (err != ESP_OK || size != sizeof(journal) ||
               journal.magic != JOURNAL_MAGIC || journal.count > JOURNAL_SLOTS) {
        ESP_LOGE("PAY_JOURNAL", "Cannot read journal; preserve flash and block payments");
        return ESP_FAIL;
    }
    healthy = true;
    ESP_LOGI("PAY_JOURNAL", "Restored pending=%lu active=%d", (unsigned long)journal.count, journal.active.active);
    if (xTaskCreate(sender, "payment_outbox", 4096, NULL, 4, NULL) != pdPASS) {
        healthy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
