#include "app_health.h"

#include <inttypes.h>

#include "temperature_control.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_health";

#define FEED_PERIOD_MS 1000      /* watchdog feed cadence (task WDT is 5 s) */
#define CHECK_PERIOD_MS 5000
#define STALL_TICKS_ESCALATE 2   /* 10 s without progress */
#define STALL_TICKS_RESTART 12   /* 60 s without progress */

static volatile uint32_t s_progress[APP_HEALTH_COUNT];
static uint32_t s_snapshot[APP_HEALTH_COUNT];
static uint32_t s_stall_ticks;
static bool s_escalated;

void app_health_note(app_health_task_t who)
{
    if (who < APP_HEALTH_COUNT) {
        s_progress[who]++;
    }
}

static void monitor_task(void *arg)
{
    (void)arg;

    esp_err_t err = esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog unavailable (%s), escalation ladder still active",
                 esp_err_to_name(err));
    } else {
        esp_task_wdt_reset(); /* feed immediately after subscribing */
    }

    for (;;) {
        /* Feed every second so the 5 s watchdog deadline never races the
         * 5 s progress check (the old feed-at-deadline cadence triggered
         * periodic task-WDT resets). */
        for (int i = 0; i < CHECK_PERIOD_MS / FEED_PERIOD_MS; i++) {
            vTaskDelay(pdMS_TO_TICKS(FEED_PERIOD_MS));
            esp_task_wdt_reset();
        }

        bool progressed = true;
        for (int i = 0; i < APP_HEALTH_COUNT; i++) {
            if (i == APP_HEALTH_NET) {
                continue; /* net tasks report liveness through MQTT, not here */
            }
            if (s_progress[i] != s_snapshot[i]) {
                s_snapshot[i] = s_progress[i];
            } else {
                progressed = false;
                ESP_LOGW(TAG, "task %d made no progress in %d ms", i, CHECK_PERIOD_MS);
            }
        }

        if (progressed) {
            s_stall_ticks = 0;
        } else {
            s_stall_ticks++;
            /* The stall is handled by the escalation ladder below instead
             * of an immediate hardware reset. */

            if (s_stall_ticks >= STALL_TICKS_ESCALATE && !s_escalated) {
                s_escalated = true;
                ESP_LOGE(TAG, "task stalled: forcing relay off and SAFE_LOCKED");
                temperature_control_emergency_off();
            }
            if (s_stall_ticks >= STALL_TICKS_RESTART) {
                ESP_LOGE(TAG, "task still unresponsive after %d ms, controlled restart",
                         STALL_TICKS_RESTART * CHECK_PERIOD_MS);
                esp_restart();
            }
        }
    }
}

void app_health_start(bool safe_locked)
{
    if (safe_locked) {
        /* Guard lockout: start from the escalated state. */
        s_escalated = true;
    }
    xTaskCreate(monitor_task, "health", 3072, NULL, 12, NULL);
}
