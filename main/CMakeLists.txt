idf_component_register(
    SRCS
        "main.c"
        "mesh.c"
        "telink.c"
        "ha.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        # Self-provisioning with borrowed keys needs stack internals that have
        # no public API: bt_mesh_provision(), bt_mesh_node_local_app_key_add(),
        # and the bt_mesh struct's seq/iv_index fields.
        "${IDF_PATH}/components/bt/esp_ble_mesh/core/include"
        "${IDF_PATH}/components/bt/esp_ble_mesh/core"
        "${IDF_PATH}/components/bt/esp_ble_mesh/common/include"
    REQUIRES
        bt
        esp_coex
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        mqtt
        json
)
