#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the neopixel RMT channel on a given GPIO.
 *
 * Idempotent — subsequent calls are a no-op once initialised.
 *
 * @param gpio  GPIO number connected to the neopixel data line, or -1 to disable.
 * @return ESP_OK on success (or already initialised).
 */
esp_err_t neopixel_init(int gpio);

/**
 * @brief Set a single neopixel to the given RGB colour.
 *
 * Queues a non-blocking RMT transmission.  The neopixel must have been
 * initialised via neopixel_init() first.
 *
 * @param r  Red   (0-255)
 * @param g  Green (0-255)
 * @param b  Blue  (0-255)
 * @return esp_err_t
 */
esp_err_t neopixel_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Turn the neopixel off (set RGB to 0,0,0).
 */
static inline esp_err_t neopixel_off(void) {
    return neopixel_set_rgb(0, 0, 0);
}

#ifdef __cplusplus
}
#endif
