/*
 * cup_state — 水杯狀態計算（純計算，不碰硬體，方便日後單獨測試）
 *
 *   姿態：由加速度算俯仰角、翻滾角、總傾斜角，由陀螺儀判斷是否移動，再分類成
 *         直立／傾斜／飲用中／倒下（代碼與 App 的 CupPose 一致）
 *   水位：只在「直立且靜止」時更新；傾斜或移動時保持最後一次有效的水位
 *   零點與最後的水位存在 RTC 記憶體，深度睡眠喚醒後沿用
 *   事件：由姿態與水位偵測喝水／加水（規則和 App 的 lib/state/drink_detector.dart 一致），
 *         產生 BLE 0x07 DRINK_EVENT 的 value；儲存與補傳在 drink_log.c
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

// 是否已經有零點（冷開機後設定過；深度睡眠喚醒後仍保留）
bool cup_motion_has_zero(void);

// 每次讀到 IMU 就呼叫一次（加速度單位 g、角速度單位 度/秒）
void cup_motion_update(float ax, float ay, float az, float gx, float gy, float gz,
                       uint32_t now_ms, cup_motion_t *out);

// 從休眠醒來時呼叫：把「最後一次移動」設成現在，避免醒來後馬上又因為「已靜止很久」睡回去
void cup_motion_mark_moving(uint32_t now_ms);

const char *cup_pose_label(cup_pose_t pose);

// 水位：每次讀到電極0 就呼叫；valid = 這一刻量到的值可信（cup_motion_t.water_ok）
void cup_water_add_sample(uint16_t e0, bool valid);

// 取得目前的水位 %（0～100）。開機後還沒有任何有效讀值時回傳 false。
bool cup_water_get(float *pct);

// 目前的水位是不是「保持中」（傾斜或移動，沿用之前的值）
bool cup_water_holding(void);

/* ========================================================================== */
/*  喝水／加水事件                                                              */
/* ========================================================================== */
// 「穩定」= 直立、不在移動，持續 ≥ EVT_SETTLE_MS，而且有新鮮的水位讀值。穩定時的水位是基準 A，
// 穩定期間基準跟著水位走（慢慢漂移、杯子放著不動時倒水進去，都只更新基準）。
// 離開穩定 → 開始一段「過程」；再次穩定得到水位 B：A − B ≥ 門檻 → 喝水，B − A ≥ 門檻 → 加水。
// 不論有沒有事件，基準都改成 B。還沒有基準時，第一次穩定只建立基準。
// 「待確認」由 App 決定，這裡只給原始旗標（saw_drinking、too_long）。
typedef enum {
    CUP_EVT_DRINK  = 0,   // 水位下降
    CUP_EVT_REFILL = 1,   // 水位上升
} cup_event_kind_t;

typedef struct {
    cup_event_kind_t kind;
    float before_pct;     // 拿起前的水位 A（%）
    float after_pct;      // 放回穩定後的水位 B（%）
    bool saw_drinking;    // 過程中出現過「飲用中」姿態
    bool too_long;        // 過程超過 EVT_MAX_EPISODE_MS
    uint32_t start_ms;    // 過程開始（離開穩定）的時間，和呼叫端傳入的 now_ms 同一個時鐘
    uint32_t end_ms;      // 重新穩定的時間
} cup_event_t;

// 每次姿態更新後呼叫一次（活動中每 100 ms）。
//   water_fresh：這一刻的水位是在目前這段直立靜止期間量到的（cup_water_get 成功且 !cup_water_holding）
// 產生事件時回傳 true 並填 *out。
bool cup_event_update(cup_pose_t pose, bool moving, bool water_fresh, float water_pct,
                      uint32_t now_ms, cup_event_t *out);

// 休眠中量到水位時呼叫（只有進入休眠時是直立靜止才呼叫）：視為穩定，只更新基準，不產生事件
void cup_event_doze_water(float water_pct);

// 從休眠喚醒時呼叫：視為離開穩定（有基準時開始一段過程）
void cup_event_wake(uint32_t now_ms);

// 目前的基準水位（序列埠顯示用）；還沒有基準時回傳 false
bool cup_event_baseline(float *pct);
bool cup_event_in_episode(void);

// 清掉基準與進行中的過程（單元測試用；冷開機時 RTC 記憶體本來就會清掉）
void cup_event_reset(void);

// 0x07 DRINK_EVENT 的 value（uint32）：
//   bits 7..0 = A（0.5%，0～200）、bits 15..8 = B（0.5%）、bits 17..16 = 種類、bit 18 = saw_drinking、
//   bit 19 = too_long、bit 20 = time_unknown、bits 31..21 = seq（0～2047）
#define CUP_EVT_SEQ_MASK        0x7FFu
#define CUP_EVT_TIME_UNKNOWN    (1u << 20)
uint32_t cup_event_encode(const cup_event_t *ev, uint16_t seq, bool time_unknown);

// 電池電壓（mV）→ 電量 %；電壓太低（沒接電池）回傳 0xFF 代表未知
uint8_t cup_battery_percent(int bat_mv);
