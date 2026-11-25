/*
 * cup_state — 水杯狀態計算（純計算，不碰硬體，方便日後單獨測試）
 *
 *   姿態：由加速度算俯仰角、翻滾角、總傾斜角，由陀螺儀判斷是否移動，再分類成
 *         直立／傾斜／飲用中／倒下（代碼與 App 的 CupPose 一致）
 *   水位：只在「直立且靜止」時更新；傾斜或移動時保持最後一次有效的水位
 *   零點與最後的水位存在 RTC 記憶體，深度睡眠喚醒後沿用
 *   事件：由姿態與水位偵測喝水／加水（拿起來喝的規則和 App 的 lib/state/drink_detector.dart 一致；
 *         0.4.0 起另外有「直立不動時水位下降也算喝水」，給用吸管喝的情況），
 *         產生 BLE 0x07 DRINK_EVENT 的 value；儲存與補傳在 drink_log.c
 *   Device Status：BLE 3c8b0005 的內容（電量、版本、BLE 位址）
 *   電量：電池電壓 → 百分比
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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
// 「穩定」= 直立、不在移動，持續 ≥ EVT_SETTLE_MS，而且有新鮮的水位讀值。穩定時的水位是基準 A。
//
// 拿起來喝／加水：離開穩定 → 開始一段「過程」；再次穩定得到水位 B：A − B ≥ 門檻 → 喝水，
//   B − A ≥ 門檻 → 加水，基準改成 B。變化不到門檻時不產生事件：上升 → 基準改成 B；
//   下降 → 基準不往下跟，維持 A（0.4.0 起；小口可以跨過好幾次拿起放回累積到門檻再記。
//   App 的 drink_detector.dart 這裡是改成 B，兩邊不再一致）。還沒有基準時，第一次穩定只建立基準。
//
// 直立不動（韌體 0.4.0 起，用吸管喝、杯子不會被拿起）：穩定期間每一筆水位和基準比
//   上升或持平            → 基準跟著走，不產生事件（杯子放著不動時倒水進去）
//   比基準低、還不到門檻  → 基準不往下跟（小口喝可以累積到門檻；這時拿起杯子，過程用原本的基準結算，不會漏）
//   比基準低 ≥ 門檻，而且連續 EVT_STATIC_MIN_SAMPLES 筆、持續 EVT_STATIC_HOLD_MS 都是
//                         → 喝水事件（saw_drinking = false、stationary = true），A = 原基準、B = 這一筆水位，
//                           基準改成 B。中間只要有一筆回到門檻以內就重新算，單筆雜訊不會觸發
//   休眠中每 5 秒量到的水位（cup_event_doze_water）套用同一條規則。
//   兩條路徑共用同一個基準，而且基準只會經由喝水事件往下走：直立不動產生事件後基準已經是 B，
//   之後拿起來只會從 B 算起，不會重複計算；所有喝水事件的量加起來 = 基準下降的量。
//
// 「待確認」由 App 決定，這裡只給原始旗標（saw_drinking、too_long）。
typedef enum {
    CUP_EVT_DRINK  = 0,   // 水位下降
    CUP_EVT_REFILL = 1,   // 水位上升
} cup_event_kind_t;

typedef struct {
    cup_event_kind_t kind;
    float before_pct;     // 拿起前的水位 A（%）；直立不動的事件 = 原本的基準
    float after_pct;      // 放回穩定後的水位 B（%）；直立不動的事件 = 確認當下的水位
    bool saw_drinking;    // 過程中出現過「飲用中」姿態（直立不動的事件一定是 false）
    bool too_long;        // 過程超過 EVT_MAX_EPISODE_MS
    bool stationary;      // 這筆是「直立不動時水位下降」產生的（只給序列埠顯示用，不會送給 App）
    uint32_t start_ms;    // 過程開始（離開穩定）的時間，和呼叫端傳入的 now_ms 同一個時鐘。
                          // 直立不動的事件 = 水位開始「連續低於基準 ≥ 門檻」的那一筆的時間
    uint32_t end_ms;      // 重新穩定的時間；直立不動的事件 = 確認（產生事件）的時間
} cup_event_t;

// 每次姿態更新後呼叫一次（活動中每 100 ms）。
//   water_fresh：這一刻的水位是在目前這段直立靜止期間量到的（cup_water_get 成功且 !cup_water_holding）
// 產生事件時回傳 true 並填 *out。
bool cup_event_update(cup_pose_t pose, bool moving, bool water_fresh, float water_pct,
                      uint32_t now_ms, cup_event_t *out);

// 休眠中量到水位時呼叫（只有進入休眠時是直立靜止才呼叫）：視為穩定，套用「直立不動」的規則
// （上升只更新基準；下降 ≥ 門檻而且持續夠久 → 喝水事件）。now_ms 要和 cup_event_update 用同一個時鐘。
// 產生事件時回傳 true 並填 *out（呼叫端要 drink_log_add）。有進行中的過程時不處理，回傳 false。
bool cup_event_doze_water(float water_pct, uint32_t now_ms, cup_event_t *out);

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

/* ========================================================================== */
/*  Device Status（BLE 3c8b0005）                                               */
/* ========================================================================== */
// 韌體 0.4.0 起 10 bytes：[電量 %（0xFF = 未知）, 韌體 major, minor, patch, BLE 位址 6 bytes]。
// 位址是「顯示順序」（最高位元組在前）：58:E6:C5:12:E0:CE → 58 E6 C5 12 E0 CE。
//   addr_le：NimBLE ble_hs_id_copy_addr 給的位址（最低位元組在前），這裡會反過來；
//            NULL = 位址還沒準備好（BLE 還沒 sync）→ 只填前 4 bytes。
// 回傳寫進 out 的長度：有位址 CUP_STATUS_LEN，沒有位址 CUP_STATUS_LEN_NO_ADDR
// （App 看到長度 < 10 會改用手機平台給的 remoteId，不會拿到一個全 0 的假位址）。
#define CUP_STATUS_LEN          10
#define CUP_STATUS_LEN_NO_ADDR  4
size_t cup_status_encode(uint8_t battery, uint8_t major, uint8_t minor, uint8_t patch,
                         const uint8_t *addr_le, uint8_t out[CUP_STATUS_LEN]);

// 電池電壓（mV）→ 電量 %；電壓太低（沒接電池）回傳 0xFF 代表未知
uint8_t cup_battery_percent(int bat_mv);
