/*
 * Wi-Fi station + MQTT + Home Assistant discovery.
 *
 * State is optimistic: we publish back exactly what we were told to do. The
 * fixtures do report their real state, but only to the amaran app's address,
 * and catching that requires patching ESP-IDF's mesh core. Not worth it --
 * changes made from the amaran app simply will not show up in HA.
 */

#ifndef AMARAN_HA_H
#define AMARAN_HA_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t amaran_ha_start(void);

/* For the `status` console command. */
bool amaran_ha_wifi_connected(void);
bool amaran_ha_mqtt_connected(void);

#endif /* AMARAN_HA_H */
