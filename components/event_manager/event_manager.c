#include "event_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "event_manager";

ESP_EVENT_DEFINE_BASE(APP_EVENT);

static esp_event_loop_handle_t s_loop;
static portMUX_TYPE s_hist_lock = portMUX_INITIALIZER_UNLOCKED;
static app_event_history_entry_t s_hist[APP_EVENT_HISTORY_LEN];
static uint32_t s_hist_next;
static uint32_t s_hist_count;
static volatile uint32_t s_dropped;

esp_err_t event_manager_init(void)
{
    if (s_loop != NULL) {
        return ESP_OK;
    }
    esp_event_loop_args_t args = {
        .queue_size = 32,
        .task_name = "app_event",
        .task_priority = 10,
        .task_stack_size = 4096,
    };
    esp_err_t err = esp_event_loop_create(&args, &s_loop);
    if (err != ESP_OK) {
        s_loop = NULL;
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void history_record(app_event_id_t id, const char *source, int err)
{
    app_event_history_entry_t e = {
        .t_ms = esp_timer_get_time() / 1000,
        .event_id = (int32_t)id,
        .err = (int32_t)err,
    };
    snprintf(e.source, sizeof(e.source), "%s", source ? source : "?");

    taskENTER_CRITICAL(&s_hist_lock);
    s_hist[s_hist_next % APP_EVENT_HISTORY_LEN] = e;
    s_hist_next++;
    if (s_hist_count < APP_EVENT_HISTORY_LEN) {
        s_hist_count++;
    }
    taskEXIT_CRITICAL(&s_hist_lock);
}

esp_err_t event_manager_publish(app_event_id_t id, const char *source, int err,
                                const void *data, size_t data_size)
{
    history_record(id, source, err);

    if (id >= APP_EVENT_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_event_post_to(s_loop, APP_EVENT, id, data, data_size, 0);
    if (ret != ESP_OK) {
        s_dropped++;
        ESP_LOGW(TAG, "post event %d from %s failed: %s (dropped=%" PRIu32 ")",
                 (int)id, source ? source : "?", esp_err_to_name(ret), s_dropped);
    }
    return ret;
}

esp_err_t event_manager_register(app_event_id_t id, esp_event_handler_t handler, void *arg)
{
    if (s_loop == NULL || handler == NULL || id >= APP_EVENT_ID_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_event_handler_register_with(s_loop, APP_EVENT, id, handler, arg);
}

size_t event_manager_history_read(app_event_history_entry_t *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }
    size_t n = 0;
    taskENTER_CRITICAL(&s_hist_lock);
    uint32_t total = s_hist_count;
    for (size_t i = 0; i < total && i < max; i++) {
        /* Newest first; slot s_hist_next-1 is the newest entry. */
        uint32_t slot = (s_hist_next + APP_EVENT_HISTORY_LEN - 1 - i) % APP_EVENT_HISTORY_LEN;
        out[i] = s_hist[slot];
        n++;
    }
    taskEXIT_CRITICAL(&s_hist_lock);
    return n;
}

uint32_t event_manager_dropped(void)
{
    return s_dropped;
}
