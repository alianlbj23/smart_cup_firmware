/*
 * ble_gatt_svr — 智慧水杯 BLE GATT Server（NimBLE）
 *
 * 協定與 App（smart_cup_app 的 lib/services/cup_protocol.dart）一致：
 *   Service      3c8b0001-efc1-4061-946c-e5916790df93
 *   Time Sync    3c8b0002-…  App→杯  Write   Unix 秒數 uint32 LE
 *   Data Notify  3c8b0003-…  杯→App  Notify  type(1) + value(uint32 LE) + timestamp(uint32 LE) = 9 bytes
 *   ACK          3c8b0004-…  App→杯  Write   1 byte [0x01] = 收到（只計數）；
 *                                              3 bytes [0x07, seq 低 8 bits, seq 高 3 bits] = 確認某一筆 DRINK_EVENT
 *   Device Status 3c8b0005-… 杯→App  Read/Notify  10 bytes（韌體 0.4.0 起；之前是前 4 bytes）：
 *                                              [電量 %（0xFF=未知）, 韌體 major, minor, patch,
 *                                               BLE 位址 6 bytes，顯示順序（最高位元組在前）]
 *                                              例：位址 58:E6:C5:12:E0:CE → 58 E6 C5 12 E0 CE。
 *                                              App 用這 6 bytes 當 cup_hardware_id（iOS 拿不到 BLE 位址）。
 *                                              內容由 cup_state.c 的 cup_status_encode 組出來
 *
 * Data Notify 的 type：
 *   0x02 WATER    bits 24..0 = 水量 25-bit 分數（0～2^25-1），bits 28..25 = 刻度 0～10
 *   0x03 RAW      電極0 濾波後讀值（除錯用）
 *   0x04 POSE     bits 15..0 = 傾斜角 0.1°（0～1800），bits 23..16 = 姿態代碼，bits 31..24 = 移動中
 *   0x05 ATTITUDE bits 15..0 = pitch int16 0.1°，bits 31..16 = roll int16 0.1°（App 不回 ACK）
 *   0x06 STATE    bits 7..0 = 狀態（0 活動中、1 休眠中），bits 31..8 = 進入目前狀態幾秒
 *                 狀態改變時、App 訂閱後 1.5 秒送；休眠中每 10 秒送一次
 *   0x07 DRINK_EVENT（韌體 0.3.0 起）喝水／加水事件，偵測在 cup_state.c、存 flash 與補傳在 drink_log.c。
 *                 韌體 0.4.0 起，直立不動時水位下降（用吸管喝）也會產生喝水事件：種類 0、saw_drinking = 0，格式不變
 *                 bits 7..0   = 拿起前水位 A，單位 0.5%（0～200）
 *                 bits 15..8  = 放回後水位 B，單位 0.5%（0～200）
 *                 bits 17..16 = 種類（0 喝水、1 加水，其他保留）
 *                 bit 18      = saw_drinking（過程中出現過「飲用中」姿態）
 *                 bit 19      = too_long（過程超過 10 分鐘）
 *                 bit 20      = time_unknown（時間不可信，timestamp 填 0）
 *                 bits 31..21 = seq（0～2047 循環）
 *                 timestamp   = 事件開始時間（Unix 秒）
 *                 App 收到後要寫 3-byte ACK，韌體才把它從 flash 刪除；沒收到 ACK 會重送（同一筆可能收到不只一次，
 *                 App 要回 ACK 並自己去重）。連線中即時送，斷線時存著，重新訂閱 1.5 秒後依序補傳，一次只在途一筆
 *
 * 改協定時 App（smart_cup_app）的 lib/services/cup_protocol.dart 要同步改。
 *
 * 廣播名稱「SmartCup-XXXX」，XXXX = BLE 位址最後兩個 byte。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define CUP_FW_MAJOR 0
#define CUP_FW_MINOR 5
#define CUP_FW_PATCH 0

// App 訂閱 Data Notify 後等多久才補送目前狀態與未確認的事件（太早送，App 可能還沒準備好接收而漏掉）
#define SUBSCRIBE_RESEND_MS 1500

// 初始化 NimBLE、GATT 服務並開始廣播（NVS 要先在 app_main 初始化好）
esp_err_t ble_cup_init(void);

typedef enum {
    BLE_CUP_OFF,          // 還沒初始化或初始化失敗
    BLE_CUP_ADVERTISING,  // 廣播中，等待 App 連線
    BLE_CUP_CONNECTED,    // 已連線，App 還沒訂閱 Data Notify
    BLE_CUP_SUBSCRIBED,   // 已訂閱，資料會送出
} ble_cup_state_t;

ble_cup_state_t ble_cup_state(void);
const char *ble_cup_name(void);   // 廣播名稱，sync 之前是空字串

// 送出 Data Notify（沒有訂閱時直接略過）
void ble_cup_send_water(float pct);
void ble_cup_send_raw(uint16_t count);
void ble_cup_send_pose(float tilt_deg, uint8_t pose, bool moving);
void ble_cup_send_attitude(float pitch_deg, float roll_deg);

#define CUP_STATE_ACTIVE 0
#define CUP_STATE_DOZING 1
void ble_cup_send_state(uint8_t state, uint32_t seconds_in_state);

// 送 0x07 DRINK_EVENT（value 由 cup_event_encode 產生，timestamp = 事件開始 Unix 秒或 0）。
// 交給 NimBLE 成功回傳 true（不代表 App 收到，要等 ACK）
bool ble_cup_send_drink_event(uint32_t value, uint32_t timestamp);

// 取出一筆 App 寫來的 DRINK_EVENT ACK（seq）；沒有時回傳 false。只在主迴圈呼叫
bool ble_cup_take_event_ack(uint16_t *seq);

// App 每寫一次 Time Sync 就加 1（主程式用來判斷「剛對時」）
uint32_t ble_cup_time_sync_count(void);

// 休眠（淺眠）時呼叫 true：連線中改用長連線間隔（約 0.5～1 秒）、沒連線時放慢廣播（1 秒），省電但連線不斷；
// 醒來呼叫 false 恢復
void ble_cup_set_low_power(bool on);

// App 每訂閱一次 Data Notify 就加 1（主程式用來判斷「剛訂閱」，馬上補送目前狀態）
uint32_t ble_cup_subscribe_count(void);

// 更新電量（0～100，0xFF = 未知）；數值有變而且 App 有訂閱時會 Notify
void ble_cup_set_battery(uint8_t pct);

// 休眠前呼叫：主動斷開連線、停止廣播，App 會立刻知道水杯斷線（不必等連線逾時）
void ble_cup_shutdown(void);

// 統計（序列埠顯示用）
uint32_t ble_cup_notify_count(void);
uint32_t ble_cup_ack_count(void);
