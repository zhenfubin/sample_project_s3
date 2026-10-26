#include "app_boot.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "app_boot";

/* RTC 守护魔数 / 崩溃循环判定阈值 */
#define RTC_GUARD_MAGIC   0x5AFE5AFEu
#define BOOT_COUNT_LIMIT  5

/* 重启守护数据(存 RTC 内存,复位/深睡后仍保留) */
typedef struct {
    uint32_t magic;        /* RTC_GUARD_MAGIC = 数据有效 */
    uint32_t boot_count;   /* 连续启动计数(崩溃循环检测) */
    uint32_t healthy_flag; /* 非 0 = 本会话已稳定运行 5 分钟 */
    uint32_t lockout;      /* 非 0 = 安全锁定生效 */
} rtc_guard_t;

RTC_DATA_ATTR static rtc_guard_t s_rtc; /* RTC 慢速内存,跨复位保留 */
static boot_state_t s_state = BOOT_STATE_SAFE;
static bool s_have_state = false;   /* 首次调用必打日志(即使停留在 BOOT_SAFE) */

/* 初始化步骤总数(正常路径 11 步,末态共享第 11 步) */
#define BOOT_STEP_TOTAL 11

/* 初始化步骤号:用于串口进度显示 [n/11] */
static int boot_state_step(boot_state_t st)
{
    switch (st) {
    case BOOT_STATE_SAFE:         return 1;
    case BOOT_STATE_INIT_NVS:     return 2;
    case BOOT_STATE_LOAD_CONFIG:  return 3;
    case BOOT_STATE_INIT_DRIVERS: return 4;
    case BOOT_STATE_INIT_CONTROL: return 5;
    case BOOT_STATE_INIT_LED:     return 6;
    case BOOT_STATE_INIT_WIFI:    return 7;
    case BOOT_STATE_DISCOVERY:    return 8;
    case BOOT_STATE_INIT_MQTT:    return 9;
    case BOOT_STATE_SYNC_STATE:   return 10;
    default:                      return 11; /* RUNNING/DEGRADED/RECOVERY/SAFE_LOCKED */
    }
}

/* 初始化步骤中文说明(串口日志用) */
static const char *boot_state_cn_str(boot_state_t st)
{
    switch (st) {
    case BOOT_STATE_SAFE:         return "安全输出初始化(继电器置安全电平)";
    case BOOT_STATE_INIT_NVS:     return "NVS 与事件总线初始化";
    case BOOT_STATE_LOAD_CONFIG:  return "加载配置";
    case BOOT_STATE_INIT_DRIVERS: return "外设驱动初始化(SHT40)";
    case BOOT_STATE_INIT_CONTROL: return "控制回路初始化(温控+风扇+保护)";
    case BOOT_STATE_INIT_LED:     return "RGB 状态灯初始化";
    case BOOT_STATE_INIT_WIFI:    return "WiFi 启动";
    case BOOT_STATE_DISCOVERY:    return "能力发现准备";
    case BOOT_STATE_INIT_MQTT:    return "MQTT 客户端初始化";
    case BOOT_STATE_SYNC_STATE:   return "等待网络就绪";
    case BOOT_STATE_RUNNING:      return "启动完成:正常运行";
    case BOOT_STATE_DEGRADED:     return "启动完成:降级运行(无网络,本地自治)";
    case BOOT_STATE_RECOVERY:     return "恢复模式";
    case BOOT_STATE_SAFE_LOCKED:  return "启动完成:安全锁定(禁止加热)";
    default:                      return "未知阶段";
    }
}

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
    if (!s_have_state || st != s_state) {
        s_have_state = true;
        s_state = st;
        /* 初始化进度串口日志:步骤号 + 中文说明(英文状态名便于检索) */
        ESP_LOGI(TAG, "初始化进度 [%d/%d]: %s (boot state -> %s)", boot_state_step(st),
                 BOOT_STEP_TOTAL, boot_state_cn_str(st), boot_state_str(st));
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
