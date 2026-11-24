#include "ble_gatt_svr.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* ========================================================================== */
/*  UUID                                                                        */
/* ========================================================================== */
// 3c8b00NN-efc1-4061-946c-e5916790df93，NimBLE 的 128-bit UUID 是 little-endian（倒過來寫），
// NN 在第 12 個 byte
#define CUP_UUID(nn) BLE_UUID128_INIT(0x93, 0xdf, 0x90, 0x67, 0x91, 0xe5, 0x6c, 0x94, \
                                      0x61, 0x40, 0xc1, 0xef, (nn), 0x00, 0x8b, 0x3c)

static const ble_uuid128_t k_svc_uuid    = CUP_UUID(0x01);
static const ble_uuid128_t k_time_uuid   = CUP_UUID(0x02);
static const ble_uuid128_t k_data_uuid   = CUP_UUID(0x03);
static const ble_uuid128_t k_ack_uuid    = CUP_UUID(0x04);
static const ble_uuid128_t k_status_uuid = CUP_UUID(0x05);

#define MSG_WATER     0x02
#define MSG_RAW       0x03
#define MSG_POSE      0x04
#define MSG_ATTITUDE  0x05
#define MSG_STATE     0x06
#define MSG_DRINK_EVT 0x07
#define DATA_LEN      9
#define FILL25_MAX    ((1u << 25) - 1)

// access callback 用 arg 分辨是哪個 characteristic
enum { CHR_TIME = 1, CHR_DATA, CHR_ACK, CHR_STATUS };

/* ========================================================================== */
/*  狀態                                                                        */
/* ========================================================================== */
static uint16_t s_data_handle, s_status_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_data_sub, s_status_sub;
static volatile bool s_started;
static uint8_t s_own_addr_type;
static char s_name[20];
static volatile uint8_t s_battery = 0xFF;
static volatile uint32_t s_notify_count, s_ack_count;
static volatile uint32_t s_subscribe_count;
static volatile bool s_low_power;
static volatile uint32_t s_time_sync_count;
// 0x07 的 ACK：在 NimBLE host task 收到，用 queue 交給主迴圈（drink_log.c）處理，不必加鎖
#define EVT_ACK_QUEUE_LEN 16
static QueueHandle_t s_evt_ack_q;

// 連線參數（間隔單位 1.25 ms、逾時單位 10 ms）
static const struct ble_gap_upd_params k_conn_fast = {
    .itvl_min = 24, .itvl_max = 40,          // 30～50 ms：ATTITUDE 每秒 5 次夠用
    .latency = 0, .supervision_timeout = 400, // 4 秒
    .min_ce_len = 0, .max_ce_len = 0,
};
static const struct ble_gap_upd_params k_conn_doze = {
    .itvl_min = 400, .itvl_max = 800,        // 0.5～1 秒：休眠時大部分時間都在睡
    .latency = 0, .supervision_timeout = 600, // 6 秒
    .min_ce_len = 0, .max_ce_len = 0,
};

static void request_conn_params(uint16_t conn)
{
    int rc = ble_gap_update_params(conn, s_low_power ? &k_conn_doze : &k_conn_fast);
    if (rc != 0) {
        printf("[BLE] 要求更新連線參數失敗 rc=%d\n", rc);
    }
}

static void advertise(void);

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

/* ========================================================================== */
/*  GATT                                                                        */
/* ========================================================================== */
static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int which = (int)(intptr_t)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (which == CHR_STATUS) {
            uint8_t st[4] = { s_battery, CUP_FW_MAJOR, CUP_FW_MINOR, CUP_FW_PATCH };
            return os_mbuf_append(ctxt->om, st, sizeof(st)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[8];
        uint16_t len = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (which == CHR_TIME) {
            if (len != 4) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            uint32_t ts = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                          ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            struct timeval tv = { .tv_sec = (time_t)ts, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            time_t t = (time_t)ts;
            struct tm tm_local;
            localtime_r(&t, &tm_local);
            printf("[BLE] 對時完成：Unix %" PRIu32 "（UTC %04d-%02d-%02d %02d:%02d:%02d）\n", ts,
                   tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                   tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
            s_time_sync_count++;   // 主迴圈看到計數變了，就幫對時前的事件換算時間
            return 0;
        }
        if (which == CHR_ACK) {
            if (len < 1) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            s_ack_count++;
            // 3 bytes [0x07, seq 低 8 bits, seq 高 3 bits] = 確認收到某一筆 DRINK_EVENT；
            // 其他（舊 App 的 1 byte [0x01]）只計數
            if (len == 3 && buf[0] == MSG_DRINK_EVT && s_evt_ack_q != NULL) {
                uint16_t seq = (uint16_t)(buf[1] | ((buf[2] & 0x07) << 8));
                if (xQueueSend(s_evt_ack_q, &seq, 0) != pdTRUE) {
                    printf("[BLE] 事件 ACK 佇列滿了，seq %u 的 ACK 丟掉（之後會重送）\n", (unsigned)seq);
                }
            }
            return 0;
        }
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def k_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &k_time_uuid.u,
                .access_cb = chr_access,
                .arg = (void *)CHR_TIME,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &k_data_uuid.u,
                .access_cb = chr_access,
                .arg = (void *)CHR_DATA,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_data_handle,
            },
            {
                .uuid = &k_ack_uuid.u,
                .access_cb = chr_access,
                .arg = (void *)CHR_ACK,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &k_status_uuid.u,
                .access_cb = chr_access,
                .arg = (void *)CHR_STATUS,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ========================================================================== */
/*  GAP                                                                         */
/* ========================================================================== */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            printf("[BLE] App 已連線\n");
            if (s_low_power) {
                request_conn_params(event->connect.conn_handle);   // 休眠中連上：直接用省電參數
            }
        } else {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        printf("[BLE] App 已斷線（原因 0x%x），重新廣播\n", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_data_sub = false;
        s_status_sub = false;
        if (s_started) {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_started) {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_data_handle) {
            s_data_sub = event->subscribe.cur_notify;
            if (s_data_sub) {
                s_subscribe_count++;
            }
            printf("[BLE] Data Notify %s\n", s_data_sub ? "已訂閱，開始送資料" : "取消訂閱");
        } else if (event->subscribe.attr_handle == s_status_handle) {
            s_status_sub = event->subscribe.cur_notify;
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
                printf("[BLE] 連線間隔 %.1f ms\n", desc.conn_itvl * 1.25);
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        printf("[BLE] MTU = %d\n", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void advertise(void)
{
    // 廣播封包：flags(3) + 128-bit Service UUID(18) = 21 bytes；名稱放 scan response
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &k_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        printf("[BLE] 設定廣播資料失敗 rc=%d\n", rc);
        return;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        printf("[BLE] 設定 scan response 失敗 rc=%d\n", rc);
        return;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (s_low_power) {
        params.itvl_min = 1600;   // 1 秒（單位 0.625 ms）：休眠時放慢廣播省電
        params.itvl_max = 1600;
    }                             // 0 = NimBLE 預設的快速廣播
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        printf("[BLE] 開始廣播失敗 rc=%d\n", rc);
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        printf("[BLE] 取得 BLE 位址失敗\n");
        return;
    }
    uint8_t addr[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);   // addr[0] 是最低位元組
    snprintf(s_name, sizeof(s_name), "SmartCup-%02X%02X", addr[1], addr[0]);
    ble_svc_gap_device_name_set(s_name);
    s_started = true;
    printf("[BLE] 開始廣播：%s（位址 %02X:%02X:%02X:%02X:%02X:%02X）\n", s_name,
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    advertise();
}

static void on_reset(int reason)
{
    printf("[BLE] NimBLE 重置，原因 %d\n", reason);
}

static void host_task(void *param)
{
    nimble_port_run();   // nimble_port_stop() 之前不會返回
    nimble_port_freertos_deinit();
}

esp_err_t ble_cup_init(void)
{
    // NVS（BLE 控制器存 PHY 校正資料要用）已經在 app_main 一開始初始化過
    s_evt_ack_q = xQueueCreate(EVT_ACK_QUEUE_LEN, sizeof(uint16_t));
    if (s_evt_ack_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    // NimBLE 自己的 log 很多，會把序列埠表格打亂，只留警告以上
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(k_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(k_svcs);
    }
    if (rc != 0) {
        printf("[BLE] 註冊 GATT 服務失敗 rc=%d\n", rc);
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set("SmartCup");   // sync 後會換成 SmartCup-XXXX

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

/* ========================================================================== */
/*  送資料                                                                      */
/* ========================================================================== */
// 送出成功（交給 NimBLE）回傳 true；沒連線、沒訂閱或記憶體不足回傳 false
static bool notify_data_ts(uint8_t type, uint32_t value, uint32_t timestamp)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_data_sub) {
        return false;
    }
    uint8_t buf[DATA_LEN];
    buf[0] = type;
    put_u32(&buf[1], value);
    put_u32(&buf[5], timestamp);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (om == NULL) {
        return false;
    }
    if (ble_gatts_notify_custom(conn, s_data_handle, om) != 0) {   // om 由 NimBLE 釋放
        return false;
    }
    s_notify_count++;
    return true;
}

static void notify_data(uint8_t type, uint32_t value)
{
    (void)notify_data_ts(type, value, (uint32_t)time(NULL));   // 對時前是開機後的秒數
}

static int16_t to_deci(float deg, float limit)
{
    if (deg > limit) deg = limit;
    if (deg < -limit) deg = -limit;
    return (int16_t)lroundf(deg * 10.0f);
}

void ble_cup_send_water(float pct)
{
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    uint32_t fill25 = (uint32_t)lroundf(pct / 100.0f * (float)FILL25_MAX);
    if (fill25 > FILL25_MAX) fill25 = FILL25_MAX;
    uint32_t level = (uint32_t)lroundf(pct / 10.0f);
    notify_data(MSG_WATER, fill25 | (level << 25));
}

void ble_cup_send_raw(uint16_t count)
{
    notify_data(MSG_RAW, count);
}

void ble_cup_send_pose(float tilt_deg, uint8_t pose, bool moving)
{
    uint32_t tilt = (uint32_t)to_deci(tilt_deg < 0 ? 0 : tilt_deg, 180.0f);
    notify_data(MSG_POSE, (tilt & 0xFFFF) | ((uint32_t)pose << 16) | ((uint32_t)(moving ? 1 : 0) << 24));
}

void ble_cup_send_attitude(float pitch_deg, float roll_deg)
{
    uint16_t p = (uint16_t)to_deci(pitch_deg, 180.0f);
    uint16_t r = (uint16_t)to_deci(roll_deg, 180.0f);
    notify_data(MSG_ATTITUDE, (uint32_t)p | ((uint32_t)r << 16));
}

void ble_cup_set_battery(uint8_t pct)
{
    if (pct == s_battery) {
        return;
    }
    s_battery = pct;
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_status_sub) {
        return;
    }
    uint8_t st[4] = { pct, CUP_FW_MAJOR, CUP_FW_MINOR, CUP_FW_PATCH };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(st, sizeof(st));
    if (om != NULL) {
        ble_gatts_notify_custom(conn, s_status_handle, om);
    }
}

void ble_cup_send_state(uint8_t state, uint32_t seconds_in_state)
{
    if (seconds_in_state > 0xFFFFFF) {
        seconds_in_state = 0xFFFFFF;
    }
    notify_data(MSG_STATE, (uint32_t)state | (seconds_in_state << 8));
}

bool ble_cup_send_drink_event(uint32_t value, uint32_t timestamp)
{
    return notify_data_ts(MSG_DRINK_EVT, value, timestamp);
}

bool ble_cup_take_event_ack(uint16_t *seq)
{
    return s_evt_ack_q != NULL && xQueueReceive(s_evt_ack_q, seq, 0) == pdTRUE;
}

uint32_t ble_cup_time_sync_count(void)
{
    return s_time_sync_count;
}

void ble_cup_set_low_power(bool on)
{
    if (s_low_power == on) {
        return;
    }
    s_low_power = on;
    if (!s_started) {
        return;
    }
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        request_conn_params(conn);
    } else {
        // 廣播中：用新的間隔重新開始廣播
        ble_gap_adv_stop();
        advertise();
    }
}

uint32_t ble_cup_subscribe_count(void)
{
    return s_subscribe_count;
}

void ble_cup_shutdown(void)
{
    if (!s_started) {
        return;
    }
    s_started = false;   // 之後不再自動重新廣播
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        // 等斷線事件送出去（最多 300 ms）
        for (int i = 0; i < 30 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    ble_gap_adv_stop();
}

ble_cup_state_t ble_cup_state(void)
{
    if (!s_started) return BLE_CUP_OFF;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return BLE_CUP_ADVERTISING;
    return s_data_sub ? BLE_CUP_SUBSCRIBED : BLE_CUP_CONNECTED;
}

const char *ble_cup_name(void)
{
    return s_name;
}

uint32_t ble_cup_notify_count(void)
{
    return s_notify_count;
}

uint32_t ble_cup_ack_count(void)
{
    return s_ack_count;
}
