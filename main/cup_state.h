/*
 * cup_state — 水杯狀態計算（純計算，不碰硬體，方便日後單獨測試）
 *
 *   姿態：由加速度算俯仰角、翻滾角、總傾斜角，由陀螺儀判斷是否移動，再分類成
 *         直立／傾斜／飲用中／倒下（代碼與 App 的 CupPose 一致）
 *   水位：只在「直立且靜止」時更新；傾斜或移動時保持最後一次有效的水位
 *   電量：電池電壓 → 百分比
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 姿態代碼：BLE POSE 封包 bits 23..16，必須和 App 的 CupPose 列舉順序一致
typedef enum {
    CUP_POSE_UNKNOWN  = 0,
    CUP_POSE_UPRIGHT  = 1,   // 直立
    CUP_POSE_TILTED   = 2,   // 傾斜
    CUP_POSE_DRINKING = 3,   // 飲用中
    CUP_POSE_LYING    = 4,   // 倒下
} cup_pose_t;

typedef struct {
    float pitch_deg;     // 前後傾斜（正 = 杯口往前），扣掉開機時的零點
    float roll_deg;      // 左右傾斜（正 = 杯口往右），扣掉開機時的零點
    float tilt_deg;      // 相對直立的總傾斜角 0～180
    bool moving;         // 最近 0.5 秒內有轉動或晃動
    uint32_t still_ms;   // 已經連續靜止多久
    cup_pose_t pose;
    bool water_ok;       // 這一刻量到的水位可信（直立且靜止夠久）
} cup_motion_t;

// 開機時把「直立靜止」的重力方向與陀螺儀偏移記成零點。
// 傳入多次取樣的平均值；傾斜超過 10° 或不是靜止時不採用（回傳 false，零點維持 0）。
bool cup_motion_set_zero(float ax, float ay, float az, float gx, float gy, float gz);

// 每次讀到 IMU 就呼叫一次（加速度單位 g、角速度單位 度/秒）
void cup_motion_update(float ax, float ay, float az, float gx, float gy, float gz,
                       uint32_t now_ms, cup_motion_t *out);

const char *cup_pose_label(cup_pose_t pose);

// 水位：每次讀到電極0 就呼叫；valid = 這一刻量到的值可信（cup_motion_t.water_ok）
void cup_water_add_sample(uint16_t e0, bool valid);

// 取得目前的水位 %（0～100）。開機後還沒有任何有效讀值時回傳 false。
bool cup_water_get(float *pct);

// 目前的水位是不是「保持中」（傾斜或移動，沿用之前的值）
bool cup_water_holding(void);

// 電池電壓（mV）→ 電量 %；電壓太低（沒接電池）回傳 0xFF 代表未知
uint8_t cup_battery_percent(int bat_mv);
