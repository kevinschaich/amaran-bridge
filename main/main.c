/*
 * amaran BLE Mesh -> Home Assistant bridge.
 *
 *   mesh.c   -- self-provisioning BLE Mesh node, transmit queue
 *   telink.c -- the 10-byte vendor payloads that actually move the lights
 *   ha.c     -- Wi-Fi, MQTT, Home Assistant discovery, state, availability
 *
 * That is the whole program. Diagnostics are driven over MQTT rather than a
 * serial prompt: publish to amaran/bridge/cmd, read amaran/bridge/reply.
 */

#include "esp_log.h"
#include "nvs_flash.h"

#include "ha.h"
#include "mesh.h"

static const char *TAG = "amaran";

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    nvs_init();

    /* Mesh first: it is the only part that matters, and bringing it up before
     * Wi-Fi gives the coexistence arbiter a settled BLE state to work around. */
    ESP_ERROR_CHECK(amaran_mesh_start());
    ESP_ERROR_CHECK(amaran_ha_start());

    ESP_LOGI(TAG, "up -- publish to amaran/bridge/cmd for diagnostics");
}
