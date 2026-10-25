#include "app_boot.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "app_boot";

#define RTC_GUARD_MAGIC   0x5AFE5AFEu
#define BOOT_COUNT_LIMIT  5

typedef struct {
    uint32_t magic;
    uint32_t boot_count;
    uint32_t healthy_flag;
    uint32_t lockout;
} rtc_guard_t;

RTC_DATA_ATTR static rtc_guard_t s_rtc;
static boot_state_t s_state = BOOT_STATE_SAFE;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXTERNAL";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
    case ESP_RST_WDT:       return "OTHER_WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
    }
}

void boot_log_reset_reason(void)
{
    ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(esp_reset_reason()));
}

guard_decision_t boot_guard_check(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool fresh_power = (reason == ESP_RST_POWERON || reason == ESP_RST_UNKNOWN ||
                        reason == ESP_RST_EXT || reason == ESP_RST_DEEPSLEEP);

    if (fresh_power || s_rtc.magic != RTC_GUARD_MAGIC) {
        s_rtc.magic = RTC_GUARD_MAGIC;
        s_rtc.boot_count = 1;
        s_rtc.healthy_flag = 0;
        s_rtc.lockout = 0;
    } else if (s_rtc.healthy_flag) {
        /* The previous session ran stably: this reset is intentional. */
        s_rtc.boot_count = 1;
        s_rtc.healthy_flag = 0;
    } else {
        s_rtc.boot_count++;
    }

    if (s_rtc.boot_count >= BOOT_COUNT_LIMIT) {
        s_rtc.lockout = 1;
    }

    ESP_LOGW(TAG, "boot count in window: %u, lockout=%u", (unsigned)s_rtc.boot_count,
             (unsigned)s_rtc.lockout);
    return s_rtc.lockout ? GUARD_SAFE_LOCKED : GUARD_NORMAL;
}

void boot_guard_mark_healthy(void)
{
    s_rtc.healthy_flag = 1;
}

void boot_guard_reset(void)
{
    s_rtc.boot_count = 1;
    s_rtc.healthy_flag = 1;
    s_rtc.lockout = 0;
}

bool boot_guard_lockout_active(void)
{
    return s_rtc.lockout != 0;
}

void boot_set_state(boot_state_t st)
{
    if (st != s_state) {
        s_state = st;
        ESP_LOGI(TAG, "boot state -> %s", boot_state_str(st));
    }
}

boot_state_t boot_get_state(void)
{
    return s_state;
}

const char *boot_state_str(boot_state_t st)
{
    switch (st) {
    case BOOT_STATE_SAFE:         return "BOOT_SAFE";
    case BOOT_STATE_INIT_NVS:     return "INIT_NVS";
    case BOOT_STATE_LOAD_CONFIG:  return "LOAD_CONFIG";
    case BOOT_STATE_INIT_DRIVERS: return "INIT_DRIVERS";
    case BOOT_STATE_INIT_CONTROL: return "INIT_CONTROL";
    case BOOT_STATE_INIT_LED:     return "INIT_LED";
    case BOOT_STATE_INIT_WIFI:    return "INIT_WIFI";
    case BOOT_STATE_DISCOVERY:    return "DISCOVERY";
    case BOOT_STATE_INIT_MQTT:    return "INIT_MQTT";
    case BOOT_STATE_SYNC_STATE:   return "SYNC_STATE";
    case BOOT_STATE_RUNNING:      return "RUNNING";
    case BOOT_STATE_DEGRADED:     return "DEGRADED";
    case BOOT_STATE_RECOVERY:     return "RECOVERY";
    case BOOT_STATE_SAFE_LOCKED:  return "SAFE_LOCKED";
    default:                      return "?";
    }
}
