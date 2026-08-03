#ifndef DISCOVERY_SERVICE_H
#define DISCOVERY_SERVICE_H

#include "esp_err.h"

esp_err_t discovery_service_start(void);

/**
 * @brief Notify discovery service that hostname or board_id changed.
 *
 * Re-reads both values from NVS, updates the mDNS hostname/instance name,
 * and re-publishes the DNS-SD TXT record so changes take effect immediately
 * without requiring a reboot.
 *
 * @return ESP_OK on success.
 */
esp_err_t discovery_service_notify_config_changed(void);

#endif
