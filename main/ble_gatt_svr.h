/*
 * ble_gatt_svr — 智慧水杯 BLE GATT Server（NimBLE）
 *
 * 協定與 App（smart_cup_app 的 lib/services/cup_protocol.dart）一致：
 *   Service      3c8b0001-efc1-4061-946c-e5916790df93
 *   Time Sync    3c8b0002-…  App→杯  Write   Unix 秒數 uint32 LE
 *   Data Notify  3c8b0003-…  杯→App  Notify  type(1) + value(uint32 LE) + timestamp(uint32 LE) = 9 bytes
 *   ACK          3c8b0004-…  App→杯  Write   0x01 = 收到
 *   Device Status 3c8b0005-… 杯→App  Read/Notify  [電量 %（0xFF=未知）, 韌體 major, minor, patch]
 *
 * Data Notify 的 type：
 *   0x02 WATER    bits 24..0 = 水量 25-bit 分數（0～2^25-1），bits 28..25 = 刻度 0～10
 *   0x03 RAW      電極0 濾波後讀值（除錯用）
 *   0x04 POSE     bits 15..0 = 傾斜角 0.1°（0～1800），bits 23..16 = 姿態代碼，bits 31..24 = 移動中
 *   0x05 ATTITUDE bits 15..0 = pitch int16 0.1°，bits 31..16 = roll int16 0.1°（App 不回 ACK）
 *   0x06 STATE    bits 7..0 = 狀態（0 活動中、1 休眠中），bits 31..8 = 進入目前狀態幾秒
 *                 狀態改變時、App 訂閱後立刻送；休眠中每 10 秒送一次
 *
 * 廣播名稱「SmartCup-XXXX」，XXXX = BLE 位址最後兩個 byte。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define CUP_FW_MAJOR 0
#define CUP_FW_MINOR 2
#define CUP_FW_PATCH 0

// 初始化 NVS、NimBLE、GATT 服務並開始廣播
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
