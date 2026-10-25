#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * relay: the single owner of the heating relay GPIO.
 *  - No other module may drive this pin.
 *  - The GPIO is configured as a driven output at the SAFE (off) level
 *    before any other subsystem initialisation.
 * Semantics: the output is a commanded state (no contact feedback exists);
 * all status reporting describes the commanded output.
 */
esp_err_t relay_init(void);

/* Set the commanded output: heating=true drives the active level. */
void relay_set(bool heating);

/* Last commanded output state. */
bool relay_get(void);

#ifdef __cplusplus
}
#endif
