#include "led_status.h"

#include "sdkconfig.h"

#if CONFIG_APP_LED_WS2812_GPIO >= 0
#define LED_RGB_ENABLED 1
#else
#define LED_RGB_ENABLED 0
#endif

#if LED_RGB_ENABLED

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define LED_TASK_STACK   2560
#define LED_TASK_PRIO    3  /* lowest priority: never blocks control tasks */
#define LED_TICK_MIN_MS  1

static const char *TAG = "led_status";

static led_state_t s_state = {
    .wifi = LED_WIFI_INIT,
    .mqtt = LED_MQTT_INIT,
    .sensor = LED_SENSOR_UNKNOWN,
    .control_active = false,
    .fault = false,
    .ready = false,
};
static TaskHandle_t s_task;
static led_strip_handle_t s_strip;

static void notify_task(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

/* WS2812 (single-wire addressable RGB on GPIO48): colors are written to the
 * one pixel at index 0 through the led_strip driver (spec §5 forbids driving
 * a WS2812 with three plain GPIOs). Per-channel level is either 0 or
 * CONFIG_APP_LED_WS2812_BRIGHTNESS so hues stay exactly as the spec table. */
static esp_err_t apply_color(led_color_t c)
{
    bool r = false, g = false, b = false;
    led_color_rgb(c, &r, &g, &b);
    uint8_t lvl = CONFIG_APP_LED_WS2812_BRIGHTNESS;
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, 0, r ? lvl : 0, g ? lvl : 0, b ? lvl : 0),
                        TAG, "led_strip_set_pixel failed");
    return led_strip_refresh(s_strip);
}

/* LED task: the only code that touches the LED GPIOs (spec §6).
 * Event-driven segment playback: each state change restarts the pattern;
 * segment expiry is timed against esp_timer so early wakeups never distort
 * the blink periods defined in the spec table. */
static void led_task(void *arg)
{
    (void)arg;
    led_display_t cur = led_pattern_resolve(&s_state);
    unsigned seg = 0;
    int64_t seg_start_us = esp_timer_get_time();
    (void)apply_color(led_pattern_get(cur)->segs[seg].color);

    for (;;) {
        const led_pattern_t *p = led_pattern_get(cur);
        uint32_t seg_ms = p->segs[seg].dur_ms;
        TickType_t wait = portMAX_DELAY;

        if (seg_ms != 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - seg_start_us) / 1000;
            if (elapsed_ms >= (int64_t)seg_ms) {
                wait = pdMS_TO_TICKS(LED_TICK_MIN_MS);
            } else {
                wait = pdMS_TO_TICKS((uint32_t)(seg_ms - elapsed_ms));
            }
        }
        ulTaskNotifyTake(pdTRUE, wait);

        led_display_t d = led_pattern_resolve(&s_state);
        if (d != cur) {
            cur = d;
            seg = 0;
            seg_start_us = esp_timer_get_time();
            (void)apply_color(led_pattern_get(cur)->segs[seg].color);
            continue;
        }
        if (seg_ms != 0 && (esp_timer_get_time() - seg_start_us) / 1000 >= (int64_t)seg_ms) {
            seg = (seg + 1) % p->n_segs;
            seg_start_us = esp_timer_get_time();
            (void)apply_color(p->segs[seg].color);
        }
    }
}

esp_err_t led_status_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_APP_LED_WS2812_GPIO,
        .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz: WS2812 bit timing */
        .mem_block_symbols = 0,            /* driver default */
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG,
                        "led_strip init failed");
    ESP_RETURN_ON_ERROR(apply_color(LED_COLOR_OFF), TAG, "LED clear failed");

    BaseType_t ok = xTaskCreate(led_task, "led_status", LED_TASK_STACK, NULL, LED_TASK_PRIO, &s_task);
    ESP_RETURN_ON_ERROR(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM, TAG, "task create failed");

    ESP_LOGI(TAG, "WS2812 RGB LED ready: GPIO=%d brightness=%d", CONFIG_APP_LED_WS2812_GPIO,
             CONFIG_APP_LED_WS2812_BRIGHTNESS);
    return ESP_OK;
}

void led_status_set_wifi(led_wifi_state_t state)
{
    if (s_state.wifi != state) {
        s_state.wifi = state;
        notify_task();
    }
}

void led_status_set_mqtt(led_mqtt_state_t state)
{
    if (s_state.mqtt != state) {
        s_state.mqtt = state;
        notify_task();
    }
}

void led_status_set_sensor_ok(bool ok)
{
    led_sensor_state_t st = ok ? LED_SENSOR_OK : LED_SENSOR_FAIL;
    if (s_state.sensor != st) {
        s_state.sensor = st;
        notify_task();
    }
}

void led_status_set_control_active(bool active)
{
    if (s_state.control_active != active) {
        s_state.control_active = active;
        notify_task();
    }
}

void led_status_set_fault(bool fault)
{
    if (s_state.fault != fault) {
        s_state.fault = fault;
        notify_task();
    }
}

void led_status_set_ready(bool ready)
{
    if (s_state.ready != ready) {
        s_state.ready = ready;
        notify_task();
    }
}

#else /* LED_RGB_ENABLED */

static const char *TAG = "led_status";

esp_err_t led_status_init(void)
{
    ESP_LOGI(TAG, "WS2812 LED disabled (APP_LED_WS2812_GPIO=-1); LED_STATUS_NOOP=1");
    return ESP_OK;
}

void led_status_set_wifi(led_wifi_state_t state)
{
    (void)state;
}

void led_status_set_mqtt(led_mqtt_state_t state)
{
    (void)state;
}

void led_status_set_sensor_ok(bool ok)
{
    (void)ok;
}

void led_status_set_control_active(bool active)
{
    (void)active;
}

void led_status_set_fault(bool fault)
{
    (void)fault;
}

void led_status_set_ready(bool ready)
{
    (void)ready;
}

#endif /* LED_RGB_ENABLED */
