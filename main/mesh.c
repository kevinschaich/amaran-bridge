#include "mesh.h"

#include <inttypes.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_ble_api.h"
#include "esp_ble_mesh_provisioning_api.h"

/* Internal stack headers. There is no public API for "load these keys without
 * being provisioned by anyone", which is exactly what we need to do -- we are
 * impersonating a node that the amaran app already knows about. main's
 * CMakeLists.txt adds the private include dirs that make these reachable. */
#include "mesh/main.h"          /* bt_mesh_provision, bt_mesh_is_provisioned  */
#include "local.h"              /* bt_mesh_node_local_app_key_add, ..._bind   */
#include "net.h"                /* struct bt_mesh -- no public seq accessor   */
#include "storage/settings.h"   /* bt_mesh_store_seq                          */

#include "config.h"
#include "mesh_config.h"
#include "telink.h"

static const char *TAG = "mesh";

#define NET_KEY_INDEX 0x0000
#define APP_KEY_INDEX 0x0000
#define DEFAULT_TTL   7

/* Spacing between consecutive mesh transmits. The advertising bearer is a
 * shared medium; firing a burst back-to-back makes fixtures miss packets. */
#define TX_GAP_MS 60
#define TX_QUEUE_DEPTH 32

/* Every command is sent this many times, spaced far enough apart to survive a
 * burst of Wi-Fi traffic hogging the shared 2.4 GHz front end.
 *
 * This is on top of net_transmit's copies, so one command is 3 x 2 = 6 radio
 * transmissions. That sounds excessive until you remember there is no ack and
 * no retry anywhere in this design: a fixture that misses the packet misses
 * the command permanently, and the failure is silent. Fixtures deduplicate on
 * sequence number, so repeats are harmless -- each repeat gets a fresh seq and
 * is simply obeyed again, setting the same value. */
#define TX_REPEATS 2
/* These gaps are not politeness, they are flow control. Each transmission
 * occupies an adv buffer for net_transmit's whole span (3 copies x 20ms), and
 * the pool is finite: queue faster than it drains and the stack starts
 * printing "Out of network buffers" and silently dropping commands -- which
 * looks exactly like a fixture being out of range. */
#define TX_REPEAT_GAP_MS 70

typedef struct {
    uint16_t dst;
    uint8_t payload[10];
} tx_msg_t;

static QueueHandle_t s_tx_queue;
static volatile bool s_ready;

/* ------------------------------------------------------------------ model */

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),   /* 3 copies, 20ms apart */
    .relay = ESP_BLE_MESH_RELAY_NOT_SUPPORTED,
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .default_ttl = DEFAULT_TTL,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
};

/* We register the 3-byte vendor form so the AppKey has something to bind to.
 * Transmission uses the 1-byte 0x26 form (see telink.h). */
static esp_ble_mesh_model_op_t vnd_op[] = {
    /* The 3-byte vendor form, so the AppKey has something conventional to bind
     * to... */
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_MODEL_OP_3(AMARAN_VENDOR_OPCODE,
                                                  AMARAN_VENDOR_COMPANY_ID), 1),
    /* ...and the 1-byte form, which is what the fixtures actually send and
     * expect. Without this line their replies arrive, decrypt, and are then
     * dropped for want of a handler. */
    ESP_BLE_MESH_MODEL_OP(AMARAN_VENDOR_OPCODE, 1),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(AMARAN_VENDOR_COMPANY_ID, 0x0000, vnd_op, NULL, NULL),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid = AMARAN_VENDOR_COMPANY_ID,
    .element_count = sizeof(elements) / sizeof(elements[0]),
    .elements = elements,
};

static uint8_t dev_uuid[16];
static esp_ble_mesh_prov_t provision = { .uuid = dev_uuid };

/* -------------------------------------------------------------- transmit */

static void tx_task(void *arg)
{
    tx_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_tx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* Queued sends made before provisioning finished wait here rather than
         * being dropped -- Home Assistant may well command a light within a
         * second of boot. */
        for (int waited = 0; !s_ready; waited++) {
            /* Silent for the first few seconds of boot, loud after that: a mesh
             * that never becomes ready is the one failure that would otherwise
             * look exactly like "the lights are ignoring me". */
            if (waited && waited % 100 == 0) {
                ESP_LOGE(TAG, "mesh still not provisioned after %ds -- commands "
                              "are queued, not sent", waited / 10);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        esp_ble_mesh_msg_ctx_t ctx = {
            .net_idx = NET_KEY_INDEX,
            .app_idx = APP_KEY_INDEX,
            .addr = msg.dst,
            .send_ttl = DEFAULT_TTL,
        };

        /* Fire and forget: response_opcode 0, no ack. Waiting for acks these
         * fixtures do not reliably send is what makes control feel broken;
         * blind repetition is what makes it feel solid. */
        esp_err_t err = ESP_OK;
        for (int r = 0; r < TX_REPEATS; r++) {
            err = esp_ble_mesh_server_model_send_msg(
                &vnd_models[0], &ctx, AMARAN_VENDOR_OPCODE, sizeof(msg.payload),
                msg.payload);
            if (err != ESP_OK) break;
            if (r + 1 < TX_REPEATS) vTaskDelay(pdMS_TO_TICKS(TX_REPEAT_GAP_MS));
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send to 0x%04x failed: %s", msg.dst, esp_err_to_name(err));
        } else {
            /* cmd byte: 0x8c on/off, 0x8f brightness, 0x82.. CCT, 0x81 HSI.
             * If a fixture moves with no line here, we did not command it. */
            ESP_LOGI(TAG, "tx 0x%04x cmd=0x%02x seq=0x%06" PRIx32,
                     msg.dst, msg.payload[9], (uint32_t)bt_mesh.seq);
        }
        vTaskDelay(pdMS_TO_TICKS(TX_GAP_MS));
    }
}

static void enqueue(uint16_t dst, const uint8_t payload[10])
{
    tx_msg_t msg = { .dst = dst };
    memcpy(msg.payload, payload, sizeof(msg.payload));
    if (xQueueSend(s_tx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full, dropping command for 0x%04x", dst);
    }
}

void amaran_mesh_onoff(uint16_t dst, bool on)
{
    uint8_t p[10];
    enqueue(dst, amaran_telink_onoff(on, p));
}

void amaran_mesh_brightness(uint16_t dst, uint16_t intensity)
{
    uint8_t p[10];
    enqueue(dst, amaran_telink_brightness(intensity, p));
}

void amaran_mesh_cct(uint16_t dst, uint16_t kelvin, uint16_t intensity, int gm)
{
    uint8_t p[10];
    enqueue(dst, amaran_telink_cct(kelvin, intensity, gm, p));
}

void amaran_mesh_hsi(uint16_t dst, uint16_t hue, uint8_t sat, uint16_t intensity)
{
    uint8_t p[10];
    enqueue(dst, amaran_telink_hsi(hue, sat, intensity, p));
}


/* ------------------------------------------------------------------ presence */

static int64_t s_last_seen_us[AMARAN_DEFAULT_LIGHT_COUNT];   /* BLE adverts */
static int64_t s_last_reply_us[AMARAN_DEFAULT_LIGHT_COUNT];  /* mesh messages */

int64_t amaran_mesh_last_reply_us(int index)
{
    if (index < 0 || index >= AMARAN_DEFAULT_LIGHT_COUNT) return 0;
    return s_last_reply_us[index];
}

void amaran_mesh_status_request(uint16_t dst)
{
    uint8_t p[10];
    enqueue(dst, amaran_telink_status_request(p));
}

static int index_of_addr(uint16_t addr)
{
    for (int i = 0; i < AMARAN_DEFAULT_LIGHT_COUNT; i++) {
        if (AMARAN_DEFAULT_LIGHTS[i].address == addr) return i;
    }
    return -1;
}

/* Strong implementation of the hook patched into ESP-IDF's network receive
 * path. It fires for every network-layer-decrypted PDU, whoever it is
 * addressed to.
 *
 * This exists because fixtures never reply to us: with availability built on
 * model callbacks, every one of the 13 reported "never" -- including the one
 * that visibly obeys commands. Hooking a layer lower sees their traffic
 * regardless of destination.
 *
 * Requires patches/apply-net-hook.py to have been run against the IDF
 * checkout. The weak default there makes an unpatched build compile and
 * silently do nothing, so `seen` reporting all -1 means the patch is missing. */
static amaran_status_cb_t s_status_cb;

void amaran_mesh_set_status_cb(amaran_status_cb_t cb) { s_status_cb = cb; }

/* The decrypted access payload of any Telink message on the mesh, courtesy of
 * the same IDF patch. data[0] is the 1-byte 0x26 opcode; data[1..10] is the
 * 10-byte payload. Fixtures address these to the provisioner, so this is the
 * only place their real state is visible. */
void amaran_mesh_access_rx(uint16_t src, uint16_t dst,
                           const uint8_t *data, uint16_t len)
{
    (void)dst;
    if (len < 11 || data[0] != AMARAN_VENDOR_OPCODE) return;

    int i = index_of_addr(src);
    if (i < 0) return;
    s_last_reply_us[i] = esp_timer_get_time();

    amaran_status_t st;
    if (!amaran_telink_decode_status(data + 1, &st)) return;  /* 0x0a page etc */

    ESP_LOGI(TAG, "state %s: %s bri=%u %s", AMARAN_DEFAULT_LIGHTS[i].key,
             st.on ? "on" : "off", st.intensity, st.is_hs ? "hsi" : "cct");
    if (s_status_cb) s_status_cb(i, &st);
}

/* Signature matches the weak default in the patched net.c (bool, not uint8_t). */
void amaran_mesh_net_rx(uint16_t src, uint16_t dst, bool ctl)
{
    (void)dst; (void)ctl;
    int i = index_of_addr(src);
    if (i >= 0) {
        s_last_reply_us[i] = esp_timer_get_time();
    }
}

/* Anything a fixture sends us. We sit on the provisioner address, so the
 * status replies fixtures broadcast at 0x0001 are addressed to us and reach
 * this handler instead of being discarded at the transport layer. */
static void custom_model_cb(esp_ble_mesh_model_cb_event_t event,
                            esp_ble_mesh_model_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT) return;

    uint16_t src = param->model_operation.ctx->addr;
    int i = index_of_addr(src);
    if (i < 0) {
        ESP_LOGI(TAG, "rx from unknown src 0x%04x (opcode 0x%06" PRIx32 ")",
                 src, (uint32_t)param->model_operation.opcode);
        return;
    }
    s_last_reply_us[i] = esp_timer_get_time();

    amaran_status_t st;
    if (param->model_operation.length >= 10 &&
        amaran_telink_decode_status(param->model_operation.msg, &st)) {
        ESP_LOGI(TAG, "rx %s: %s bri=%u %s",
                 AMARAN_DEFAULT_LIGHTS[i].key, st.on ? "on" : "off",
                 st.intensity, st.is_hs ? "hsi" : "cct");
    } else {
        ESP_LOGI(TAG, "rx %s: %d bytes (not a state reply)",
                 AMARAN_DEFAULT_LIGHTS[i].key, param->model_operation.length);
    }
}

#define UNKNOWN_MAX 12
static struct { uint8_t mac[6]; int8_t rssi; int64_t when; } s_unknown[UNKNOWN_MAX];
static int s_unknown_n;

int64_t amaran_mesh_last_seen_us(int index)
{
    if (index < 0 || index >= AMARAN_DEFAULT_LIGHT_COUNT) return 0;
    return s_last_seen_us[index];
}

bool amaran_mesh_fixture_seen(int index, uint32_t within_ms)
{
    int64_t seen = amaran_mesh_last_seen_us(index);
    if (seen == 0) return false;
    return (esp_timer_get_time() - seen) < ((int64_t)within_ms * 1000);
}

static void note_unknown(const uint8_t mac[6], int8_t rssi)
{
    for (int i = 0; i < s_unknown_n; i++) {
        if (memcmp(s_unknown[i].mac, mac, 6) == 0) {
            s_unknown[i].rssi = rssi;
            s_unknown[i].when = esp_timer_get_time();
            return;
        }
    }
    if (s_unknown_n >= UNKNOWN_MAX) return;
    memcpy(s_unknown[s_unknown_n].mac, mac, 6);
    s_unknown[s_unknown_n].rssi = rssi;
    s_unknown[s_unknown_n].when = esp_timer_get_time();
    s_unknown_n++;
}

void amaran_mesh_dump_unknown(void)
{
    int64_t now = esp_timer_get_time();
    printf("other BLE devices heard (first %d):\n", UNKNOWN_MAX);
    for (int i = 0; i < s_unknown_n; i++) {
        printf("  %02x:%02x:%02x:%02x:%02x:%02x  rssi %4d  %llds ago\n",
               s_unknown[i].mac[0], s_unknown[i].mac[1], s_unknown[i].mac[2],
               s_unknown[i].mac[3], s_unknown[i].mac[4], s_unknown[i].mac[5],
               s_unknown[i].rssi, (long long)((now - s_unknown[i].when) / 1000000));
    }
    if (s_unknown_n == 0) printf("  (none)\n");
}

static void ble_cb(esp_ble_mesh_ble_cb_event_t event,
                   esp_ble_mesh_ble_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_SCAN_BLE_ADVERTISING_PKT_EVT) return;

    const uint8_t *adv = param->scan_ble_adv_pkt.addr;
    for (int i = 0; i < AMARAN_DEFAULT_LIGHT_COUNT; i++) {
        if (memcmp(adv, AMARAN_DEFAULT_LIGHTS[i].mac, 6) == 0) {
            s_last_seen_us[i] = esp_timer_get_time();
            return;
        }
    }
    note_unknown(adv, param->scan_ble_adv_pkt.rssi);
}

/* ------------------------------------------------------------ provisioning */

/* -EEXIST / -EALREADY mean the key survived in NVS across a reboot. Success. */
static bool already_done(int rc) { return rc == -17 || rc == -120; }

static void ensure_seq_floor(void)
{
    if (bt_mesh.seq < AMARAN_SEQ_START) {
        ESP_LOGW(TAG, "raising seq 0x%06" PRIx32 " -> 0x%06x (replay floor)",
                 (uint32_t)bt_mesh.seq, AMARAN_SEQ_START);
        bt_mesh.seq = AMARAN_SEQ_START;
        bt_mesh_store_seq();
    }
}

static void finish_provisioning(void)
{
    static bool done;
    if (done) return;

    /* On a warm boot the stack restores whatever identity is in NVS and fires
     * PROV_COMPLETE before app_main gets a look in. If that identity is not the
     * one config.h asks for -- e.g. this board previously ran the old firmware
     * at 0x0010 -- binding would fail against an element that does not exist.
     * Say nothing here; amaran_mesh_start() resets and re-provisions. */
    uint16_t addr = esp_ble_mesh_get_primary_element_address();
    if (addr != AMARAN_NODE_ADDR) {
        return;
    }

    int rc = bt_mesh_node_local_app_key_add(NET_KEY_INDEX, APP_KEY_INDEX,
                                            (uint8_t *)AMARAN_APP_KEY);
    if (rc && !already_done(rc)) {
        ESP_LOGE(TAG, "app key add failed: %d", rc);
        return;
    }

    rc = bt_mesh_node_bind_app_key_to_model(AMARAN_NODE_ADDR, 0x0000,
                                            AMARAN_VENDOR_COMPANY_ID, APP_KEY_INDEX);
    if (rc && !already_done(rc)) {
        ESP_LOGE(TAG, "app key bind failed: %d", rc);
        return;
    }

    ensure_seq_floor();
    done = true;
    s_ready = true;
    ESP_LOGI(TAG, "ready: addr=0x%04x iv=0x%08" PRIx32 " seq=0x%06" PRIx32,
             AMARAN_NODE_ADDR, (uint32_t)bt_mesh.iv_index, (uint32_t)bt_mesh.seq);
}

static void prov_cb(esp_ble_mesh_prov_cb_event_t event,
                    esp_ble_mesh_prov_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT) {
        ESP_LOGI(TAG, "prov complete, addr=0x%04x",
                 param->node_prov_complete.addr);
        finish_provisioning();
    }
}

static void config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                             esp_ble_mesh_cfg_server_cb_param_t *param)
{
    (void)event; (void)param;   /* nobody configures us; log nothing */
}

/* Load the amaran network's keys directly instead of being provisioned.
 *
 * Order matters and is not obvious:
 *   1. prov_enable sets the NODE flag -- without it bt_mesh_is_provisioned()
 *      stays false even after bt_mesh_provision() succeeds.
 *   2. bt_mesh_provision loads NetKey + DevKey + our unicast address.
 *   3. Turn the provisioning bearers back off; nothing should provision us.
 *   4. AppKey add + model bind happen in PROV_COMPLETE, above. */
static esp_err_t self_provision(void)
{
    esp_err_t err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV);
    if (err != ESP_OK) return err;

    uint8_t dev_key[16];
    esp_fill_random(dev_key, sizeof(dev_key));   /* only used for Config traffic */

    int rc = bt_mesh_provision((uint8_t *)AMARAN_NET_KEY, NET_KEY_INDEX, 0,
                               AMARAN_IV_INDEX_INITIAL, AMARAN_NODE_ADDR, dev_key);
    if (rc && !already_done(rc)) {
        ESP_LOGE(TAG, "bt_mesh_provision failed: %d", rc);
        return ESP_FAIL;
    }

    esp_ble_mesh_node_prov_disable(ESP_BLE_MESH_PROV_ADV);
    return ESP_OK;
}

/* ----------------------------------------------------------------- public */

bool amaran_mesh_ready(void) { return s_ready; }

uint32_t amaran_mesh_seq(void) { return (uint32_t)bt_mesh.seq; }

void amaran_mesh_bump_seq(uint32_t by)
{
    bt_mesh.seq += by;
    bt_mesh_store_seq();
    ESP_LOGW(TAG, "seq bumped to 0x%06" PRIx32, (uint32_t)bt_mesh.seq);
}

esp_err_t amaran_mesh_start(void)
{
    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_msg_t));
    if (!s_tx_queue) return ESP_ERR_NO_MEM;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Default coexistence arbitration favours Wi-Fi and starves the mesh scan
     * windows. Without this, commands go out but fixtures answer into silence. */
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    esp_read_mac(dev_uuid, ESP_MAC_BT);   /* stable, unique, never advertised */

    /* bt_mesh_net_relay() warns once per mesh packet we decline to relay --
     * "No outbound bearer found" -- which, with relay and proxy off, is every
     * packet any other device on the mesh sends. Nothing is wrong; it just
     * drowns the log. Errors still get through. */
    esp_log_level_set("BLE_MESH", ESP_LOG_ERROR);

    esp_ble_mesh_register_prov_callback(prov_cb);
    esp_ble_mesh_register_config_server_callback(config_server_cb);
    esp_ble_mesh_register_custom_model_callback(custom_model_cb);

    esp_err_t err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_mesh_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Presence: hear which fixtures are advertising. The mesh scanner is
     * already running continuously, so this costs no extra radio time -- it
     * just stops throwing the non-mesh packets away. */
    esp_ble_mesh_register_ble_callback(ble_cb);
    esp_ble_mesh_ble_scan_param_t scan = { .duration = 0 };  /* until stopped */
    esp_ble_mesh_start_ble_scanning(&scan);

    xTaskCreate(tx_task, "amaran_tx", 3072, NULL, 5, NULL);

    if (!bt_mesh_is_provisioned()) {
        ESP_LOGI(TAG, "cold boot: self-provisioning");
        return self_provision();
    }

    /* Provisioned already -- but as whom? BLE Mesh identity lives in NVS and
     * survives a reflash, so a board that previously ran different firmware
     * comes back with that firmware's unicast address. config.h is the source
     * of truth: if they disagree, throw the stored identity away. */
    uint16_t addr = esp_ble_mesh_get_primary_element_address();
    if (addr != AMARAN_NODE_ADDR) {
        ESP_LOGW(TAG, "stored mesh identity is 0x%04x but config.h wants 0x%04x"
                      " -- discarding it and re-provisioning", addr, AMARAN_NODE_ADDR);
        bt_mesh_node_reset();       /* also zeroes seq; the floor is reapplied */
        return self_provision();
    }

    ESP_LOGI(TAG, "warm boot: mesh state restored from NVS");
    finish_provisioning();
    return ESP_OK;
}
