#include "relay.h"

#include "driver/gpio.h"
#include "sdkconfig.h"

static bool s_heating = false;

esp_err_t relay_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_APP_RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* Safe output first: drive the inactive level immediately. */
#if CONFIG_APP_RELAY_ACTIVE_HIGH
    gpio_set_level(CONFIG_APP_RELAY_GPIO, 0);
#else
    gpio_set_level(CONFIG_APP_RELAY_GPIO, 1);
#endif
    s_heating = false;
    return ESP_OK;
}

void relay_set(bool heating)
{
    s_heating = heating;
#if CONFIG_APP_RELAY_ACTIVE_HIGH
    gpio_set_level(CONFIG_APP_RELAY_GPIO, heating ? 1 : 0);
#else
    gpio_set_level(CONFIG_APP_RELAY_GPIO, heating ? 0 : 1);
#endif
}

bool relay_get(void)
{
    return s_heating;
}
