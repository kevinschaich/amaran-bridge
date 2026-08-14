#include "ha.h"

#include <stdarg.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "soc/soc_caps.h"

#include "config.h"
#include "mesh.h"
#include "mesh_config.h"

static const char *TAG = "ha";

#define LIGHT_COUNT AMARAN_DEFAULT_LIGHT_COUNT
#if LIGHT_COUNT > 32
#error "dirty mask is a uint32_t; raise it or trim the fixture list"
#endif

#define AVAIL_TOPIC   MQTT_TOPIC_PREFIX "/bridge/availability"

/* How long a fixture may go unheard before Home Assistant is told it is
 * unavailable. Fixtures advertise every second or two; a minute of total
 * silence means it is powered off or out of range, and any command we send it
 * would vanish without a trace. */
#define PRESENCE_TIMEOUT_MS 150000
#define PRESENCE_POLL_MS    5000
/* How often to ask every fixture to report itself. A fixture that answers is
 * reachable by definition -- it heard our message and replied. */
#define PROBE_INTERVAL_MS   45000
#define HA_STATUS_TOPIC HA_DISCOVERY_PREFIX "/status"

/* Diagnostics channel. Publish a command here, read the answer on /reply.
 * This exists because the serial prompt cannot be trusted: putting console
 * input on USB-Serial-JTAG wedged the peripheral outright. MQTT is already
 * connected, already reliable, and reachable from anywhere. */
#define CMD_TOPIC   MQTT_TOPIC_PREFIX "/bridge/cmd"
#define REPLY_TOPIC MQTT_TOPIC_PREFIX "/bridge/reply"
#define DIAG_TOPIC  MQTT_TOPIC_PREFIX "/bridge/diagnostics"
#define DIAG_INTERVAL_MS 30000

#define WORK_CONNECT   (1u << 0)
#define WORK_STATE     (1u << 1)

/* Collapse a slider drag into one state publish. */
#define STATE_DEBOUNCE_MS 150
/* Breathing room between discovery publishes so the burst never monopolises
 * esp-mqtt's api lock long enough to starve the keepalive ping. A stalled
 * keepalive means a broker-side disconnect, which fires our last will, which
 * is what HA shows as "unavailable" a few seconds after every reconnect. */
#define DISCOVERY_GAP_MS 30

typedef struct {
    bool on;
    uint8_t bri;        /* 0..255, HA scale */
    bool is_hs;
    uint16_t kelvin;
    uint16_t hue;       /* 0..360 */
    uint8_t sat;        /* 0..100 */
    int8_t gm;          /* -10..+10 */
} light_state_t;

static light_state_t s_state[LIGHT_COUNT];
static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_worker;
static volatile bool s_wifi_up, s_mqtt_up;
static uint32_t s_dirty;
static portMUX_TYPE s_dirty_lock = portMUX_INITIALIZER_UNLOCKED;

bool amaran_ha_wifi_connected(void) { return s_wifi_up; }
bool amaran_ha_mqtt_connected(void) { return s_mqtt_up; }

/* ------------------------------------------------------------------ topics */

static void topic_set(char *buf, size_t n, const char *key)
{
    snprintf(buf, n, MQTT_TOPIC_PREFIX "/%.20s/set", key);
}
static void topic_state(char *buf, size_t n, const char *key)
{
    snprintf(buf, n, MQTT_TOPIC_PREFIX "/%.20s/state", key);
}
static void topic_gm_set(char *buf, size_t n, const char *key)
{
    snprintf(buf, n, MQTT_TOPIC_PREFIX "/%.20s/gm/set", key);
}
static void topic_gm_state(char *buf, size_t n, const char *key)
{
    snprintf(buf, n, MQTT_TOPIC_PREFIX "/%.20s/gm/state", key);
}
static void topic_avail(char *buf, size_t n, const char *key)
{
    snprintf(buf, n, MQTT_TOPIC_PREFIX "/%.20s/available", key);
}

static void mark_dirty(int i)
{
    portENTER_CRITICAL(&s_dirty_lock);
    s_dirty |= (1u << i);
    portEXIT_CRITICAL(&s_dirty_lock);
    if (s_worker) xTaskNotify(s_worker, WORK_STATE, eSetBits);
}

static light_state_t snapshot(int i);
static void commit(int i, const light_state_t *st);


/* ---------------------------------------------------------------- presence */

static bool s_avail[LIGHT_COUNT];
static bool s_avail_known[LIGHT_COUNT];

static void publish_avail(int i, bool up)
{
    char topic[96];
    topic_avail(topic, sizeof(topic), AMARAN_DEFAULT_LIGHTS[i].key);
    esp_mqtt_client_publish(s_client, topic, up ? "online" : "offline", 0, 1, 1);
}

/* One entity per fixture goes available/unavailable purely on whether we can
 * hear its BLE adverts. That is deliberately a radio-level fact rather than a
 * guess: an entity greyed out here means a command genuinely cannot reach it,
 * so the Home Assistant UI stops lying about fixtures that are switched off. */
/* A fixture told us what it is actually doing. Believe it over our optimistic
 * guess: this is what makes Home Assistant survive a reboot, and what makes
 * changes from the amaran app show up here. */
static void on_status(int i, const amaran_status_t *rep)
{
    light_state_t st = snapshot(i);
    st.on = rep->on;
    st.bri = (uint8_t)((rep->intensity * 255u) / 1000u);
    if (st.bri == 0 && rep->on) st.bri = 1;
    st.is_hs = rep->is_hs;
    if (rep->is_hs) {
        st.hue = rep->hue;
        st.sat = rep->sat;
    } else {
        if (rep->cct_kelvin) st.kelvin = rep->cct_kelvin;
        st.gm = (int8_t)rep->gm;
    }
    commit(i, &st);
    mark_dirty(i);
}

static void presence_task(void *arg)
{
    int64_t last_probe = 0, last_diag = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PRESENCE_POLL_MS));
        if (!s_mqtt_up) continue;

        int64_t now = esp_timer_get_time();

        if (now - last_diag > (int64_t)DIAG_INTERVAL_MS * 1000) {
            last_diag = now;
            int online = 0;
            for (int i = 0; i < LIGHT_COUNT; i++) {
                if (s_avail_known[i] && s_avail[i]) online++;
            }
            wifi_ap_record_t ap = { 0 };
            esp_wifi_sta_get_ap_info(&ap);
            char d[224];
            snprintf(d, sizeof(d),
                     "{\"uptime_s\":%lld,\"free_heap\":%u,\"rssi\":%d,"
                     "\"mesh_seq\":%" PRIu32 ",\"fixtures\":%d}",
                     (long long)(now / 1000000),
                     (unsigned)(esp_get_free_heap_size() / 1024),
                     ap.rssi, amaran_mesh_seq(), online);
            esp_mqtt_client_publish(s_client, DIAG_TOPIC, d, 0, 0, 1);
        }

        if (now - last_probe > (int64_t)PROBE_INTERVAL_MS * 1000) {
            last_probe = now;
            for (int i = 0; i < LIGHT_COUNT; i++) {
                amaran_mesh_status_request(AMARAN_DEFAULT_LIGHTS[i].address);
            }
        }

        for (int i = 0; i < LIGHT_COUNT; i++) {
            /* Availability means "this fixture answered us recently", not
             * "we can hear its Bluetooth adverts". A fixture can be powered on
             * and advertising while completely ignoring our mesh messages --
             * which is exactly the case that made the old signal useless. */
            int64_t reply = amaran_mesh_last_reply_us(i);
            bool up = reply != 0 &&
                      (now - reply) < ((int64_t)PRESENCE_TIMEOUT_MS * 1000);
            if (s_avail_known[i] && s_avail[i] == up) continue;
            s_avail[i] = up;
            s_avail_known[i] = true;
            publish_avail(i, up);
            ESP_LOGI(TAG, "%s is %s", AMARAN_DEFAULT_LIGHTS[i].key,
                     up ? "available" : "UNAVAILABLE");
        }
    }
}

/* --------------------------------------------------------------- publishing */

static void publish_state(int i)
{
    const amaran_light_t *l = &AMARAN_DEFAULT_LIGHTS[i];
    char topic[128], payload[192];

    /* Snapshot under the lock: the MQTT task writes s_state[] while we format,
     * and a half-updated read publishes a colour mode with the wrong values. */
    light_state_t snap = snapshot(i);
    const light_state_t *st = &snap;

    topic_state(topic, sizeof(topic), l->key);
    if (!st->on) {
        snprintf(payload, sizeof(payload), "{\"state\":\"OFF\"}");
    } else if (st->is_hs) {
        snprintf(payload, sizeof(payload),
                 "{\"state\":\"ON\",\"brightness\":%u,\"color_mode\":\"hs\","
                 "\"color\":{\"h\":%u,\"s\":%u}}",
                 st->bri, st->hue, st->sat);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"state\":\"ON\",\"brightness\":%u,"
                 "\"color_mode\":\"color_temp\",\"color_temp\":%u}",
                 st->bri, st->kelvin);
    }
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);

    if (l->has_gm) {
        char gm_topic[96], gm_payload[8];
        topic_gm_state(gm_topic, sizeof(gm_topic), l->key);
        snprintf(gm_payload, sizeof(gm_payload), "%d", st->gm);
        esp_mqtt_client_publish(s_client, gm_topic, gm_payload, 0, 0, 1);
    }
}

#define DEVICE_JSON \
    "\"device\":{\"identifiers\":[\"" DEVICE_ID "\"]," \
    "\"name\":\"amaran mesh bridge\",\"manufacturer\":\"amaran (Aputure)\"," \
    "\"model\":\"Telink BLE Mesh\"}"

/* Two availability sources, both required: the bridge has to be up AND we
 * have to be hearing that particular fixture. */
#define AVAIL_JSON_FMT \
    "\"availability_mode\":\"all\"," \
    "\"availability\":[{\"topic\":\"" AVAIL_TOPIC "\"}," \
    "{\"topic\":\"%.95s\"}]," \
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\""

/* Wipe retained payloads off our own command topics.
 *
 * A retained message on a command topic is a booby trap: the broker replays it
 * to us every single time we subscribe, and we dutifully act on it. Home
 * Assistant publishes commands retained when a discovery config says
 * "retain": true -- which the old firmware's config did -- so a stale "turn
 * on" from days ago can sit there and switch the lights on at every reconnect.
 * We never want retained commands, so clear them before subscribing. */
static void clear_retained_commands(void)
{
    char topic[96];
    for (int i = 0; i < LIGHT_COUNT; i++) {
        const char *key = AMARAN_DEFAULT_LIGHTS[i].key;
        topic_set(topic, sizeof(topic), key);
        esp_mqtt_client_publish(s_client, topic, "", 0, 0, 1);
        topic_gm_set(topic, sizeof(topic), key);
        esp_mqtt_client_publish(s_client, topic, "", 0, 0, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "cleared retained commands on %d command topics", LIGHT_COUNT * 2);
}

static void publish_discovery(void)
{
    char topic[160], set_t[96], state_t[96], uniq[72], avail_t[96];
    static char payload[896];

    for (int i = 0; i < LIGHT_COUNT; i++) {
        const amaran_light_t *l = &AMARAN_DEFAULT_LIGHTS[i];
        snprintf(uniq, sizeof(uniq), DEVICE_ID "_%.20s", l->key);
        topic_set(set_t, sizeof(set_t), l->key);
        topic_state(state_t, sizeof(state_t), l->key);
        topic_avail(avail_t, sizeof(avail_t), l->key);

        snprintf(topic, sizeof(topic),
                 HA_DISCOVERY_PREFIX "/light/%.63s/config", uniq);
        snprintf(payload, sizeof(payload),
                 "{\"name\":\"%.31s\",\"unique_id\":\"%.63s\",\"schema\":\"json\","
                 "\"command_topic\":\"%.95s\",\"state_topic\":\"%.95s\","
                 "\"brightness\":true,\"brightness_scale\":255,"
                 "\"supported_color_modes\":[\"color_temp\"%s],"
                 "\"color_temp_kelvin\":true,\"min_kelvin\":%u,\"max_kelvin\":%u,"
                 AVAIL_JSON_FMT "," DEVICE_JSON "}",
                 l->name, uniq, set_t, state_t,
                 l->has_color ? ",\"hs\"" : "",
                 l->cct_min, l->cct_max, avail_t);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
        vTaskDelay(pdMS_TO_TICKS(DISCOVERY_GAP_MS));

        snprintf(topic, sizeof(topic),
                 HA_DISCOVERY_PREFIX "/number/%.63s_gm/config", uniq);
        if (l->has_gm) {
            char gm_set[96], gm_state[96];
            topic_gm_set(gm_set, sizeof(gm_set), l->key);
            topic_gm_state(gm_state, sizeof(gm_state), l->key);
            snprintf(payload, sizeof(payload),
                     "{\"name\":\"%.31s G/M\",\"unique_id\":\"%.63s_gm\","
                     "\"command_topic\":\"%.95s\",\"state_topic\":\"%.95s\","
                     "\"min\":-10,\"max\":10,\"step\":1,\"mode\":\"slider\","
                     "\"icon\":\"mdi:invert-colors\",\"entity_category\":\"config\","
                     AVAIL_JSON_FMT "," DEVICE_JSON "}",
                     l->name, uniq, gm_set, gm_state, avail_t);
            esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
        } else {
            /* Empty retained payload deletes a stale entity. */
            esp_mqtt_client_publish(s_client, topic, "", 0, 0, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(DISCOVERY_GAP_MS));
    }
    /* A deliberately small diagnostic set. The old firmware published 18 of
     * these; most were noise, and once its retained configs outlived it they
     * sat in Home Assistant showing frozen values forever. Five that are
     * actually worth looking at, all recomputed every 30s. */
    static const struct { const char *id, *name, *unit, *icon; } diags[] = {
        { "uptime_s",  "Uptime",          "s",   "mdi:timer-outline" },
        { "free_heap", "Free heap",       "kB",  "mdi:memory" },
        { "rssi",      "Wi-Fi RSSI",      "dBm", "mdi:wifi" },
        { "mesh_seq",  "Mesh sequence",   NULL,  "mdi:counter" },
        { "fixtures",  "Fixtures online", NULL,  "mdi:lightbulb-group" },
    };
    for (size_t d = 0; d < sizeof(diags) / sizeof(diags[0]); d++) {
        snprintf(topic, sizeof(topic),
                 HA_DISCOVERY_PREFIX "/sensor/" DEVICE_ID "_%s/config", diags[d].id);
        int n = snprintf(payload, sizeof(payload),
                 "{\"name\":\"%s\",\"unique_id\":\"" DEVICE_ID "_%s\","
                 "\"state_topic\":\"" DIAG_TOPIC "\","
                 "\"value_template\":\"{{ value_json.%s }}\","
                 "\"entity_category\":\"diagnostic\",\"icon\":\"%s\",",
                 diags[d].name, diags[d].id, diags[d].id, diags[d].icon);
        if (diags[d].unit) {
            n += snprintf(payload + n, sizeof(payload) - n,
                          "\"unit_of_measurement\":\"%s\",", diags[d].unit);
        }
        snprintf(payload + n, sizeof(payload) - n,
                 "\"availability_topic\":\"" AVAIL_TOPIC "\","
                 "\"payload_available\":\"online\","
                 "\"payload_not_available\":\"offline\"," DEVICE_JSON "}");
        esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
        vTaskDelay(pdMS_TO_TICKS(DISCOVERY_GAP_MS));
    }

    ESP_LOGI(TAG, "discovery published for %d fixtures", LIGHT_COUNT);
}

static void worker_task(void *arg)
{
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

        if (bits & WORK_CONNECT) {
            /* The whole connect sequence runs here, not in the MQTT event
             * handler: a QoS 1 publish made inside the handler waits on the
             * same lock the client task needs in order to send it. */
            esp_mqtt_client_publish(s_client, AVAIL_TOPIC, "online", 0, 1, 1);

            /* Clear before subscribing, so the broker has nothing stale left
             * to replay at us. */
            clear_retained_commands();

            esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_PREFIX "/+/set", 0);
            esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_PREFIX "/+/gm/set", 0);
            esp_mqtt_client_subscribe(s_client, HA_STATUS_TOPIC, 0);
            esp_mqtt_client_subscribe(s_client, CMD_TOPIC, 0);

            publish_discovery();
            for (int i = 0; i < LIGHT_COUNT; i++) {
                s_avail_known[i] = false;   /* force a fresh availability publish */
            }
            /* Deliberately do NOT publish light state here. Until a fixture has
             * been commanded we have no idea what it is doing -- state is
             * optimistic, not observed -- and publishing a made-up "OFF" just
             * makes Home Assistant confidently wrong. Any retained state from a
             * previous run is more truthful than anything we could invent. */
        }

        if (bits & WORK_STATE) {
            vTaskDelay(pdMS_TO_TICKS(STATE_DEBOUNCE_MS));
            uint32_t mask;
            portENTER_CRITICAL(&s_dirty_lock);
            mask = s_dirty;
            s_dirty = 0;
            portEXIT_CRITICAL(&s_dirty_lock);
            for (int i = 0; i < LIGHT_COUNT && s_mqtt_up; i++) {
                if (mask & (1u << i)) publish_state(i);
            }
        }
    }
}


/* ------------------------------------------------------------------- admin */

static void reply(const char *fmt, ...)
{
    static char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    esp_mqtt_client_publish(s_client, REPLY_TOPIC, buf, 0, 0, 0);
    ESP_LOGI(TAG, "reply: %s", buf);
}

static void admin_seen(void)
{
    static char buf[1024];
    int64_t now = esp_timer_get_time();
    size_t at = 0;
    at += snprintf(buf + at, sizeof(buf) - at, "key/addr adv_age reply_age\n");
    for (int i = 0; i < LIGHT_COUNT && at < sizeof(buf) - 64; i++) {
        int64_t adv = amaran_mesh_last_seen_us(i);
        int64_t rep = amaran_mesh_last_reply_us(i);
        at += snprintf(buf + at, sizeof(buf) - at, "%-11s 0x%04x %5lld %5lld\n",
                       AMARAN_DEFAULT_LIGHTS[i].key,
                       AMARAN_DEFAULT_LIGHTS[i].address,
                       adv ? (long long)((now - adv) / 1000000) : -1LL,
                       rep ? (long long)((now - rep) / 1000000) : -1LL);
    }
    esp_mqtt_client_publish(s_client, REPLY_TOPIC, buf, 0, 0, 0);
    ESP_LOGI(TAG, "\n%s", buf);
}

/* Commands: status | seen | probe | raw <addr> <on|off> | seqbump [n] */
static void handle_admin(const char *data, int len)
{
    char buf[96];
    if (len <= 0 || len >= (int)sizeof(buf)) return;
    memcpy(buf, data, len);
    buf[len] = '\0';
    ESP_LOGI(TAG, "admin: %s", buf);

    char verb[24] = {0}, a1[32] = {0}, a2[24] = {0};
    sscanf(buf, "%23s %31s %23s", verb, a1, a2);

    if (strcasecmp(verb, "status") == 0) {
        reply("addr=0x%04x seq=0x%06" PRIx32 " mesh=%s wifi=%d mqtt=%d",
              AMARAN_NODE_ADDR, amaran_mesh_seq(),
              amaran_mesh_ready() ? "ready" : "NOT-READY", s_wifi_up, s_mqtt_up);
    } else if (strcasecmp(verb, "seen") == 0) {
        admin_seen();
    } else if (strcasecmp(verb, "probe") == 0) {
        for (int i = 0; i < LIGHT_COUNT; i++) {
            amaran_mesh_status_request(AMARAN_DEFAULT_LIGHTS[i].address);
        }
        reply("probed %d fixtures; ask `seen` in ~10s", LIGHT_COUNT);
    } else if (strcasecmp(verb, "raw") == 0) {
        uint16_t addr = (uint16_t)strtoul(a1, NULL, 0);
        bool on = (strcasecmp(a2, "on") == 0);
        amaran_mesh_onoff(addr, on);
        reply("raw 0x%04x %s", addr, on ? "on" : "off");
    } else if (strcasecmp(verb, "seqbump") == 0) {
        uint32_t by = a1[0] ? (uint32_t)strtoul(a1, NULL, 0) : 0x10000;
        amaran_mesh_bump_seq(by);
        reply("seq now 0x%06" PRIx32, amaran_mesh_seq());
    } else {
        reply("commands: status | seen | probe | raw <addr> <on|off> | seqbump [n]");
    }
}

/* ----------------------------------------------------------------- commands */

static int find_light(const char *key, size_t len)
{
    for (int i = 0; i < LIGHT_COUNT; i++) {
        const char *k = AMARAN_DEFAULT_LIGHTS[i].key;
        if (strlen(k) == len && memcmp(k, key, len) == 0) return i;
    }
    return -1;
}

/* Push the current cached state to the fixture. `explicit_on` is true when the
 * command payload said "state":"ON" -- HA always includes it on a turn_on, and
 * re-sending the wake packet then is what recovers a light whose real state
 * has drifted from ours. */
static void push(int i, const light_state_t *st, bool explicit_on, bool was_on,
                 bool with_values)
{
    uint16_t addr = AMARAN_DEFAULT_LIGHTS[i].address;

    if (!st->on) {
        amaran_mesh_onoff(addr, false);
        return;
    }
    if (explicit_on || !was_on) {
        amaran_mesh_onoff(addr, true);
    }
    /* A bare {"state":"ON"} carries nothing to set -- the fixture restores its
     * own last look when it wakes. Sending a colour packet anyway doubles mesh
     * traffic for no benefit, and on a 13-fixture "all on" that is what pushes
     * the adv buffer pool over the edge. */
    if (!with_values) {
        return;
    }
    uint16_t intensity = (uint16_t)(((uint32_t)st->bri * 1000u) / 255u);
    if (intensity == 0) intensity = 1;

    if (st->is_hs) {
        amaran_mesh_hsi(addr, st->hue, st->sat, intensity);
    } else {
        amaran_mesh_cct(addr, st->kelvin, intensity, st->gm);
    }
}

static void commit(int i, const light_state_t *st)
{
    portENTER_CRITICAL(&s_dirty_lock);
    s_state[i] = *st;
    portEXIT_CRITICAL(&s_dirty_lock);
}

static light_state_t snapshot(int i)
{
    light_state_t st;
    portENTER_CRITICAL(&s_dirty_lock);
    st = s_state[i];
    portEXIT_CRITICAL(&s_dirty_lock);
    return st;
}

static void handle_light_cmd(int i, const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "bad JSON for %s", AMARAN_DEFAULT_LIGHTS[i].key);
        return;
    }
    /* Mutate a local copy and commit it in one go, so the publish task never
     * reads a half-updated fixture. */
    light_state_t st = snapshot(i);
    bool was_on = st.on;
    bool explicit_on = false;

    cJSON *j = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(j)) {
        st.on = (strcasecmp(j->valuestring, "ON") == 0);
        explicit_on = st.on;
    }

    j = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(j)) {
        int v = j->valueint;
        st.bri = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    if (st.on && st.bri == 0) st.bri = 255;

    cJSON *color = cJSON_GetObjectItem(root, "color");
    j = cJSON_GetObjectItem(root, "color_temp");
    bool with_values = cJSON_IsObject(color) || cJSON_IsNumber(j) ||
                       cJSON_IsNumber(cJSON_GetObjectItem(root, "brightness"));
    if (cJSON_IsObject(color)) {
        cJSON *h = cJSON_GetObjectItem(color, "h");
        cJSON *sa = cJSON_GetObjectItem(color, "s");
        if (cJSON_IsNumber(h)) st.hue = (uint16_t)(h->valuedouble < 0 ? 0 :
                                       (h->valuedouble > 360 ? 360 : h->valuedouble));
        if (cJSON_IsNumber(sa)) st.sat = (uint8_t)(sa->valuedouble < 0 ? 0 :
                                        (sa->valuedouble > 100 ? 100 : sa->valuedouble));
        st.is_hs = true;
    } else if (cJSON_IsNumber(j)) {
        /* color_temp_kelvin:true in discovery, so this arrives in kelvin. */
        int k = j->valueint;
        const amaran_light_t *l = &AMARAN_DEFAULT_LIGHTS[i];
        if (k < l->cct_min) k = l->cct_min;
        if (k > l->cct_max) k = l->cct_max;
        st.kelvin = (uint16_t)k;
        st.is_hs = false;
    }
    cJSON_Delete(root);

    commit(i, &st);
    push(i, &st, explicit_on, was_on, with_values);
    mark_dirty(i);
}

static void handle_gm_cmd(int i, const char *data, int len)
{
    char buf[16];
    if (len <= 0 || len >= (int)sizeof(buf)) return;
    memcpy(buf, data, len);
    buf[len] = '\0';

    int v = atoi(buf);
    if (v < -10) v = -10;
    if (v > 10) v = 10;

    light_state_t st = snapshot(i);
    st.gm = (int8_t)v;
    commit(i, &st);
    mark_dirty(i);

    /* G/M only exists in CCT mode. Every colour fixture has both, so if the
     * light is currently showing a hue, remember the tint but do not resend --
     * pushing a CCT packet here would yank the fixture to white. */
    if (!st.is_hs) {
        push(i, &st, false, st.on, true);
    }
}

/* Topic is "amaran/<key>/set" or "amaran/<key>/gm/set". */
static void route(const char *topic, int tlen, const char *data, int dlen)
{
    static const char pfx[] = MQTT_TOPIC_PREFIX "/";
    const int pfx_len = sizeof(pfx) - 1;

    if (tlen == (int)strlen(HA_STATUS_TOPIC) &&
        memcmp(topic, HA_STATUS_TOPIC, tlen) == 0) {
        if (dlen >= 6 && memcmp(data, "online", 6) == 0) {
            ESP_LOGI(TAG, "Home Assistant restarted, republishing discovery");
            xTaskNotify(s_worker, WORK_CONNECT, eSetBits);
        }
        return;
    }

    if (tlen == (int)strlen(CMD_TOPIC) && memcmp(topic, CMD_TOPIC, tlen) == 0) {
        handle_admin(data, dlen);
        return;
    }

    if (tlen <= pfx_len || memcmp(topic, pfx, pfx_len) != 0) return;
    const char *rest = topic + pfx_len;
    int rest_len = tlen - pfx_len;

    const char *slash = memchr(rest, '/', rest_len);
    if (!slash) return;
    int key_len = (int)(slash - rest);
    int suffix_len = rest_len - key_len - 1;
    const char *suffix = slash + 1;

    int i = find_light(rest, key_len);
    if (i < 0) return;

    /* Logged unconditionally: if a fixture moves and nothing appears here,
     * the command did not come from us. */
    if (suffix_len == 3 && memcmp(suffix, "set", 3) == 0) {
        ESP_LOGI(TAG, "cmd %s <- %.*s", AMARAN_DEFAULT_LIGHTS[i].key, dlen, data);
        handle_light_cmd(i, data, dlen);
    } else if (suffix_len == 6 && memcmp(suffix, "gm/set", 6) == 0) {
        ESP_LOGI(TAG, "gm  %s <- %.*s", AMARAN_DEFAULT_LIGHTS[i].key, dlen, data);
        handle_gm_cmd(i, data, dlen);
    }
}

/* -------------------------------------------------------------------- MQTT */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t e = event_data;
    /* Reassembly buffer for payloads split across MQTT_EVENT_DATA calls. */
    static char frag[1024];
    static int frag_len;
    static char frag_topic[128];
    static int frag_topic_len;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_up = true;
        ESP_LOGI(TAG, "MQTT connected");
        xTaskNotify(s_worker, WORK_CONNECT, eSetBits);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_up = false;
        /* Frequent disconnects mean another client is using the same
         * DEVICE_ID on this broker and evicting us. */
        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_DATA:
        if (e->total_data_len == e->data_len) {
            route(e->topic, e->topic_len, e->data, e->data_len);
            break;
        }
        if (e->total_data_len > (int)sizeof(frag)) break;   /* absurd, ignore */
        if (e->current_data_offset == 0) {
            frag_len = 0;
            frag_topic_len = e->topic_len < (int)sizeof(frag_topic)
                                 ? e->topic_len : 0;
            if (frag_topic_len) memcpy(frag_topic, e->topic, frag_topic_len);
        }
        if (frag_topic_len && e->current_data_offset == frag_len) {
            memcpy(frag + frag_len, e->data, e->data_len);
            frag_len += e->data_len;
            if (frag_len == e->total_data_len) {
                route(frag_topic, frag_topic_len, frag, frag_len);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error (type %d)", e->error_handle->error_type);
        break;

    default:
        break;
    }
}

static void mqtt_start(void)
{
    if (s_client) return;   /* exactly one client, forever -- see DEVICE_ID */

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.client_id = DEVICE_ID,
        .session.last_will.topic = AVAIL_TOPIC,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 7,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 60,
        .network.timeout_ms = 10000,
        .network.reconnect_timeout_ms = 5000,
        .task.stack_size = 5120,
    };
    if (strlen(MQTT_USERNAME) > 0) {
        cfg.credentials.username = MQTT_USERNAME;
        cfg.credentials.authentication.password = MQTT_PASSWORD;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
}

/* ------------------------------------------------------------------- Wi-Fi */

static void reconnect_cb(void *arg) { esp_wifi_connect(); }

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    static esp_timer_handle_t retry;

    /* Note: no connect on STA_START. That event fires from inside
     * esp_wifi_start(), before the band mode is pinned, so connecting there
     * would let the first association attempt wander onto 5 GHz. */
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        if (!retry) {
            esp_timer_create_args_t a = { .callback = reconnect_cb,
                                          .name = "wifi_retry" };
            esp_timer_create(&a, &retry);
        }
        esp_timer_stop(retry);
        esp_timer_start_once(retry, 2000000);
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying in 2s");
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        s_wifi_up = true;
        ESP_LOGI(TAG, "Wi-Fi up, ip=" IPSTR, IP2STR(&ev->ip_info.ip));
        mqtt_start();   /* no-op after the first time; esp-mqtt reconnects itself */
    }
}

static esp_err_t wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

#if defined(SOC_WIFI_SUPPORT_5G) && SOC_WIFI_SUPPORT_5G
    /* 2.4 GHz only, one set of credentials. The C5 can do 5 GHz, but BLE Mesh
     * lives on 2.4 GHz and sharing the front end between bands on the devkit
     * costs mesh reliability for no benefit here.
     *
     * This must come AFTER esp_wifi_start() -- before it, the call returns
     * ESP_ERR_WIFI_NOT_STARTED. */
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY));
#endif

    /* Modem sleep is required, not optional, and not about power: software
     * coexistence hands the BLE controller the idle slots that Wi-Fi gives up
     * here. With WIFI_PS_NONE the Wi-Fi radio never yields, BLE Mesh advertising
     * and scan windows get squeezed, and fixtures stop hearing us. The official
     * esp_ble_mesh/wifi_coexist example comments this line "must call this". */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    ESP_LOGI(TAG, "connecting to \"%s\" (2.4 GHz only)", WIFI_SSID);
    esp_wifi_connect();
    return ESP_OK;
}

/* ------------------------------------------------------------------ public */

esp_err_t amaran_ha_start(void)
{
    for (int i = 0; i < LIGHT_COUNT; i++) {
        s_state[i] = (light_state_t){ .on = false, .bri = 255, .is_hs = false,
                                      .kelvin = 4700, .hue = 0, .sat = 0,
                                      .gm = 0 };
    }
    xTaskCreate(worker_task, "amaran_pub", 4096, NULL, 4, &s_worker);
    amaran_mesh_set_status_cb(on_status);
    xTaskCreate(presence_task, "amaran_seen", 3072, NULL, 3, NULL);
    return wifi_start();
}
