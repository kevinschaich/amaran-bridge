/*
 * BLE Mesh node that talks to amaran/Telink fixtures.
 *
 * There is no GATT connection to anything, ever. We self-provision onto the
 * amaran mesh using the keys lifted from the desktop app and then transmit on
 * the advertising bearer like any other mesh node. "Connecting to the lights"
 * is not a thing that happens -- if a fixture is in radio range of us or of a
 * relay, it hears us. That is the whole model.
 */

#ifndef AMARAN_MESH_H
#define AMARAN_MESH_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "telink.h"

/* Brings up the controller, Bluedroid, the mesh stack, self-provisions and
 * starts the transmit task. Call once from app_main, after nvs_flash_init. */
esp_err_t amaran_mesh_start(void);

/* True once self-provisioning finished and the AppKey is bound. Sends before
 * this are queued, not dropped. */
bool amaran_mesh_ready(void);

/* --- presence -------------------------------------------------------------
 *
 * Two independent signals, and the difference between them is diagnostic:
 *
 *   last_seen  -- the fixture's plain BLE adverts carry its MAC, which
 *                 mesh_config.h knows. Means "powered on and in range".
 *   last_reply -- a mesh message from that fixture, via the patched receive
 *                 path. Means "it heard us and answered".
 *
 * Availability uses last_reply, because adverts proved useless on their own:
 * eight fixtures advertised happily while ignoring every command we sent. */

/* Microseconds since boot when we last received a mesh message FROM this
 * fixture, or 0 if never. This is the strong signal: it means the fixture is
 * alive, on our network, using our keys, and talking -- and if it followed a
 * status request from us, that it heard us. */
int64_t amaran_mesh_last_reply_us(int index);

/* Ask a fixture to report its state. The reply lands in the receive path and
 * updates last_reply_us. */
void amaran_mesh_status_request(uint16_t dst);

/* Called when a fixture reports its actual state. `index` indexes
 * AMARAN_DEFAULT_LIGHTS. This is observed truth, not an echo of what we
 * commanded -- it also fires when someone changes a light from the amaran app
 * or a panel. */
typedef void (*amaran_status_cb_t)(int index, const amaran_status_t *st);
void amaran_mesh_set_status_cb(amaran_status_cb_t cb);

/* Microseconds since boot when fixture `index` was last heard, or 0 if never.
 * `index` is an index into AMARAN_DEFAULT_LIGHTS. */
int64_t amaran_mesh_last_seen_us(int index);

/* True if that fixture has been heard within the last `within_ms`. */
bool amaran_mesh_fixture_seen(int index, uint32_t within_ms);

/* Number of distinct BLE devices heard, and a dump of recent unknown ones --
 * for the `scan` console command, when a fixture's MAC does not match. */
void amaran_mesh_dump_unknown(void);

/* Current outbound sequence number, for the `status` REPL command. */
uint32_t amaran_mesh_seq(void);

/* Force the outbound sequence number forward. Fixtures silently discard any
 * message whose seq is not strictly greater than the last one they recorded
 * from us, so a stale replay-protection entry looks exactly like "the light is
 * broken". See the AMARAN_SEQ_START comment in config.h. */
void amaran_mesh_bump_seq(uint32_t by);

/* Queue one Telink command for `dst` (a fixture unicast address, or
 * AMARAN_GROUP_ALL / AMARAN_BROADCAST). Non-blocking: safe to call from the
 * MQTT event handler, the console task, anywhere. Intensity is 0..1000. */
void amaran_mesh_onoff(uint16_t dst, bool on);
void amaran_mesh_brightness(uint16_t dst, uint16_t intensity_0_1000);
void amaran_mesh_cct(uint16_t dst, uint16_t kelvin, uint16_t intensity_0_1000,
                     int gm_offset);
void amaran_mesh_hsi(uint16_t dst, uint16_t hue_deg, uint8_t sat_0_100,
                     uint16_t intensity_0_1000);

#endif /* AMARAN_MESH_H */
