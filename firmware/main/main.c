// Roomie Beacon: a Bluetooth room beacon that the Roomie app sets up over Bluetooth.
//
// - Unconfigured: advertises a "Roomie setup" signal (connectable, named "Roomie-XXXX") until
//   Roomie configures it.
// - Configured: for 3 minutes after power-on it alternates between the beacon signal and the
//   setup signal (so Roomie can change or reset it), then it's a beacon only.
// - Roomie writes the settings (UUID, major, minor, signal at 1 m, transmit power, interval) to one
//   characteristic; the beacon saves them and restarts.
//
// Kept small on purpose: Bluetooth LE only (no Wi-Fi in the firmware), the CPU at its lowest
// Bluetooth speed, no logging, nothing else running. It stays awake (no light sleep): beacons run
// on USB power, and an awake chip can always be found again for re-flashing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define FW_VERSION         2
#define SETUP_WINDOW_MS    (3 * 60 * 1000)   // configured beacons: setup is open this long after power-on
#define SETUP_ADV_MS       160               // setup signal interval (quick to find)
#define MIXED_SLOT_MS      500               // during the setup window: beacon / setup signals alternate this often
#define RESTART_DELAY_MS   400               // after a change: time for the write response to go out
#define ESPRESSIF_ID       0x02E5            // Bluetooth company ID used for the setup signal's status bytes
#define RECORD_LEN         26

#define CMD_CONFIGURE      0x01
#define CMD_RESET          0x02

static const char *TAG = "roomie";

// Setup service and its one characteristic (the same in every Roomie Beacon; Roomie scans for it).
// 09d317ba-23f0-4778-afc3-d819146ab127 / 9194e05c-5fdc-4b80-98f5-d9c4655a1d40, bytes reversed.
static const ble_uuid128_t setup_service_uuid = BLE_UUID128_INIT(
    0x27, 0xb1, 0x6a, 0x14, 0x19, 0xd8, 0xc3, 0xaf, 0x78, 0x47, 0xf0, 0x23, 0xba, 0x17, 0xd3, 0x09);
static const ble_uuid128_t config_char_uuid = BLE_UUID128_INIT(
    0x40, 0x1d, 0x5a, 0x65, 0xc4, 0xd9, 0xf5, 0x98, 0x80, 0x4b, 0xdc, 0x5f, 0x5c, 0xe0, 0x94, 0x91);

typedef struct {
    uint8_t  configured;
    uint8_t  uuid[16];        // proximity UUID, as written (big-endian)
    uint16_t major;
    uint16_t minor;
    int8_t   measured_power;  // RSSI at 1 m, as carried in the beacon packet
    int8_t   tx_power;        // dBm
    uint16_t interval_ms;     // beacon advertising interval
} beacon_config_t;

static beacon_config_t cfg = {
    .configured = 0, .measured_power = -59, .tx_power = 9, .interval_ms = 300,
};

typedef enum { MODE_SETUP, MODE_MIXED, MODE_BEACON } beacon_mode_t;

static beacon_mode_t mode;
static bool connected;
static bool beacon_slot;
static uint8_t own_addr_type;
static char name[16];
static struct ble_npl_callout slot_timer, window_timer, restart_timer;

// MARK: Settings (NVS)

static void load_config(void) {
    nvs_handle_t h;
    if (nvs_open("roomie", NVS_READONLY, &h) != ESP_OK) return;
    beacon_config_t stored;
    size_t len = sizeof stored;
    if (nvs_get_blob(h, "cfg", &stored, &len) == ESP_OK && len == sizeof stored) cfg = stored;
    nvs_close(h);
}

static void save_config(void) {
    nvs_handle_t h;
    if (nvs_open("roomie", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &cfg, sizeof cfg);
    nvs_commit(h);
    nvs_close(h);
}

static void erase_config(void) {
    nvs_handle_t h;
    if (nvs_open("roomie", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "cfg");
    nvs_commit(h);
    nvs_close(h);
}

// MARK: Transmit power

// Each chip's full range: small boards often have weak antennas and need the top levels.
static const struct { int8_t dbm; esp_power_level_t level; } levels[] = {
    {-12, ESP_PWR_LVL_N12}, {-9, ESP_PWR_LVL_N9}, {-6, ESP_PWR_LVL_N6}, {-3, ESP_PWR_LVL_N3},
    {0, ESP_PWR_LVL_N0},    {3, ESP_PWR_LVL_P3},  {6, ESP_PWR_LVL_P6},  {9, ESP_PWR_LVL_P9},
#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3
    {12, ESP_PWR_LVL_P12},  {15, ESP_PWR_LVL_P15}, {18, ESP_PWR_LVL_P18}, {21, ESP_PWR_LVL_P21},
#elif CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2
    {12, ESP_PWR_LVL_P12},  {15, ESP_PWR_LVL_P15}, {18, ESP_PWR_LVL_P18}, {20, ESP_PWR_LVL_P20},
#endif
};
#define LEVEL_COUNT (sizeof levels / sizeof levels[0])

static esp_power_level_t power_level(int8_t dbm) {
    size_t best = 0;
    for (size_t i = 1; i < LEVEL_COUNT; i++) {
        if (abs(levels[i].dbm - dbm) < abs(levels[best].dbm - dbm)) best = i;
    }
    return levels[best].level;
}

/// The beacon signal uses the configured power; the setup signal always uses the chip's maximum,
/// so a board is easy to find and set up whatever its antenna is like.
static void apply_tx_power(bool setup) {
    esp_power_level_t level = setup ? levels[LEVEL_COUNT - 1].level : power_level(cfg.tx_power);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, level);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, level);
}

// MARK: Advertising

static int gap_event(struct ble_gap_event *event, void *arg);

static int start_beacon_adv(void) {
    // Standard proximity-beacon packet (the format iOS location services recognise).
    uint8_t d[30] = {0x02, 0x01, 0x06, 0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15};
    memcpy(d + 9, cfg.uuid, 16);
    d[25] = cfg.major >> 8;
    d[26] = cfg.major & 0xFF;
    d[27] = cfg.minor >> 8;
    d[28] = cfg.minor & 0xFF;
    d[29] = (uint8_t)cfg.measured_power;
    int rc = ble_gap_adv_set_data(d, sizeof d);
    if (rc) return rc;

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_NON;   // with disc_mode NON: a plain non-connectable beacon
    p.disc_mode = BLE_GAP_DISC_MODE_NON;
    p.itvl_min = p.itvl_max = BLE_GAP_ADV_ITVL_MS(cfg.interval_ms);
    return ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

static int start_setup_adv(void) {
    // Status for Roomie's scan: configured flag, major, minor (iOS hides proximity-beacon packets from
    // normal Bluetooth scans, so this is how Roomie knows which beacon is which).
    uint8_t status[7] = {
        ESPRESSIF_ID & 0xFF, ESPRESSIF_ID >> 8, cfg.configured ? 1 : 0,
        cfg.major >> 8, cfg.major & 0xFF, cfg.minor >> 8, cfg.minor & 0xFF,
    };
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &setup_service_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    f.mfg_data = status;
    f.mfg_data_len = sizeof status;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) return rc;

    struct ble_hs_adv_fields r = {0};
    r.name = (const uint8_t *)name;
    r.name_len = strlen(name);
    r.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc) return rc;

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = p.itvl_max = BLE_GAP_ADV_ITVL_MS(SETUP_ADV_MS);
    return ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

/// (Re)starts advertising for the current mode. Paused while Roomie is connected.
static void advertise(void) {
    if (connected) return;
    ble_gap_adv_stop();
    bool beacon = mode == MODE_BEACON || (mode == MODE_MIXED && beacon_slot);
    apply_tx_power(!beacon);
    int rc = beacon ? start_beacon_adv() : start_setup_adv();
    if (rc) ESP_LOGW(TAG, "advertising failed: %d", rc);
}

static void on_slot(struct ble_npl_event *ev) {
    if (mode != MODE_MIXED) return;
    beacon_slot = !beacon_slot;
    advertise();
    ble_npl_callout_reset(&slot_timer, ble_npl_time_ms_to_ticks32(MIXED_SLOT_MS));
}

static void on_window_end(struct ble_npl_event *ev) {
    if (mode != MODE_MIXED) return;
    mode = MODE_BEACON;   // setup closes: beacon signal only from now on
    ble_npl_callout_stop(&slot_timer);
    advertise();
}

static void on_restart(struct ble_npl_event *ev) {
    esp_restart();
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        connected = event->connect.status == 0;
        if (!connected) advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connected = false;
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

// MARK: Setup characteristic
//
// Record (26 bytes), read and written in the same layout:
//   [0]      read: firmware version · write: command (0x01 configure, 0x02 reset to unconfigured)
//   [1]      configured (read only; 1 = yes)
//   [2..17]  proximity UUID (big-endian)
//   [18..19] major (big-endian)
//   [20..21] minor (big-endian)
//   [22]     measured power at 1 m (int8, dBm)
//   [23]     transmit power (int8, dBm; -12…21, rounded to the nearest level the chip has)
//   [24..25] advertising interval (uint16, little-endian, ms; 100…10000)
// A reset needs only byte 0. After a write the beacon restarts with the new settings.

static void fill_record(uint8_t *r) {
    r[0] = FW_VERSION;
    r[1] = cfg.configured ? 1 : 0;
    memcpy(r + 2, cfg.uuid, 16);
    r[18] = cfg.major >> 8;
    r[19] = cfg.major & 0xFF;
    r[20] = cfg.minor >> 8;
    r[21] = cfg.minor & 0xFF;
    r[22] = (uint8_t)cfg.measured_power;
    r[23] = (uint8_t)cfg.tx_power;
    r[24] = cfg.interval_ms & 0xFF;
    r[25] = cfg.interval_ms >> 8;
}

static int config_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t r[RECORD_LEN];
        fill_record(r);
        return os_mbuf_append(ctxt->om, r, sizeof r) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint8_t w[RECORD_LEN] = {0};
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, w, sizeof w, &len) != 0 || len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    if (w[0] == CMD_RESET) {
        erase_config();
    } else if (w[0] == CMD_CONFIGURE && len == RECORD_LEN) {
        uint16_t interval = w[24] | (w[25] << 8);
        int8_t tx = (int8_t)w[23];
        if (interval < 100 || interval > 10000 || tx < -12 || tx > 21) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        cfg.configured = 1;
        memcpy(cfg.uuid, w + 2, 16);
        cfg.major = (w[18] << 8) | w[19];
        cfg.minor = (w[20] << 8) | w[21];
        cfg.measured_power = (int8_t)w[22];
        cfg.tx_power = tx;
        cfg.interval_ms = interval;
        save_config();
    } else {
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    ble_npl_callout_reset(&restart_timer, ble_npl_time_ms_to_ticks32(RESTART_DELAY_MS));
    return 0;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &setup_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &config_char_uuid.u,
                .access_cb = config_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {0},
        },
    },
    {0},
};

// MARK: Start-up

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
    if (mode == MODE_MIXED) {
        ble_npl_callout_reset(&slot_timer, ble_npl_time_ms_to_ticks32(MIXED_SLOT_MS));
        ble_npl_callout_reset(&window_timer, ble_npl_time_ms_to_ticks32(SETUP_WINDOW_MS));
    }
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "Bluetooth host reset: %d", reason);
}

static void host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    load_config();
    mode = cfg.configured ? MODE_MIXED : MODE_SETUP;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(name, sizeof name, "Roomie-%02X%02X", mac[4], mac[5]);

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth failed to start");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(services);
    ble_gatts_add_svcs(services);
    ble_svc_gap_device_name_set(name);

    struct ble_npl_eventq *queue = nimble_port_get_dflt_eventq();
    ble_npl_callout_init(&slot_timer, queue, on_slot, NULL);
    ble_npl_callout_init(&window_timer, queue, on_window_end, NULL);
    ble_npl_callout_init(&restart_timer, queue, on_restart, NULL);

    nimble_port_freertos_init(host_task);
}
