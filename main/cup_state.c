#include "cup_state.h"

#include <math.h>
#include <stddef.h>

#include "esp_attr.h"   // RTC_DATA_ATTR：深度睡眠時保留

/* ========================================================================== */
/*  可調參數                                                                    */
/* ========================================================================== */
// 水位校正（電極0 的濾波後資料）：水越多讀值越小，兩點之間用直線換算
// 1 µs 充電時實測（2026-11-26 重新校正，杯子擦乾、壓到保護套底部）：
// 空杯 480.2（479～481）；滿杯 = 水面和主條銅箔頂端切齊：356（355～357，連續兩次）。總變化 124
// 水倒得比銅箔頂端高時讀值會再低（實測過 339），所以 100% 一定要用「切齊銅箔頂端」當標準
// 注意：杯壁殘留水會讓空杯讀值低約 28；杯子沒壓到底，滿杯會少算約 10%（見 HANDOFF）
#define LEVEL_E0_EMPTY          480     // 空杯時電極0 的讀值
#define LEVEL_E0_FULL           356     // 滿杯（水面切齊主條頂端）時電極0 的讀值

// 軸向：IMU 裝在杯底、Z 軸朝上。實際裝好後如果 App 上左右或前後顛倒，把對應的值改成 -1
#define ROLL_SIGN               (+1.0f)
#define PITCH_SIGN              (+1.0f)

// 姿態分類門檻（度）
#define UPRIGHT_ENTER_DEG       20.0f   // 傾斜 < 20° → 直立
#define UPRIGHT_EXIT_DEG        25.0f   // 已經是直立時，超過 25° 才離開（避免在門檻附近跳來跳去）
#define DRINKING_DEG            35.0f   // ≥ 35° 且不是倒下 → 飲用中
#define LYING_DEG               60.0f   // ≥ 60° 且靜止夠久 → 倒下
#define LYING_STILL_MS          2000    // 倒下要連續靜止 2 秒（手拿著喝水一定會有微小晃動）

// 移動判斷：陀螺儀角速度或加速度偏離 1 g 超過門檻 → 這一刻在動；之後 0.5 秒內都算移動中
#define MOVE_GYRO_DPS           10.0f
#define MOVE_ACC_G              0.10f
#define MOVE_HOLD_MS            500

// 直立後要再靜止 1 秒，水面平穩了才採用水位讀值
#define WATER_SETTLE_MS         1000
#define WATER_AVG_N             10      // 取最近 10 筆（約 1 秒）平均
#define WATER_MIN_N             5       // 至少 5 筆才更新

// 喝水／加水事件：拿起來喝／加水（和 App 的 drink_detector.dart 一致）
#ifndef EVT_SETTLE_MS
#define EVT_SETTLE_MS           3000    // 直立、不在移動持續 3 秒才算「穩定」
#endif
#ifndef EVT_MIN_DELTA_PCT
#define EVT_MIN_DELTA_PCT       3.0f    // 水位變化 ≥ 3%（電極解析度約 2%）才算事件，否則當雜訊
#endif
#ifndef EVT_MAX_EPISODE_MS
#define EVT_MAX_EPISODE_MS      (10u * 60u * 1000u)   // 過程超過 10 分鐘：照樣產生事件，但帶 too_long
#endif

// 直立不動時水位下降也算喝水（韌體 0.4.0：用吸管喝，杯子放在桌上不會被拿起）。
// 穩定期間水位比基準低 ≥ EVT_STATIC_DELTA_PCT，而且「連續 EVT_STATIC_MIN_SAMPLES 筆」又「持續
// EVT_STATIC_HOLD_MS」都是，才產生一筆喝水事件；兩個條件都要滿足，中間有一筆回到門檻以內就重新算。
//   活動中每 100 ms 一筆（每筆是最近 1 秒 10 個讀值的平均）→ 預設實際上是「連續 8 秒」（80 筆）
//   休眠中每 5 秒一筆（每筆是 200 ms 內 10 個讀值的平均）  → 預設實際上是「連續 3 筆」（跨約 10 秒）
#ifndef EVT_STATIC_DROP_ENABLE
#define EVT_STATIC_DROP_ENABLE  1       // 0 = 關掉，回到 0.3.0 的行為：穩定期間水位升降都只更新基準，
                                        // 拿起放回結算時下降不到門檻，基準也照舊往下改成 B（小口不累積）
#endif
#ifndef EVT_STATIC_DELTA_PCT
#define EVT_STATIC_DELTA_PCT    EVT_MIN_DELTA_PCT   // 直立不動時下降多少（%）才算；預設和拿起來喝同一個門檻（合約 3%）。
                                                    // 漂移造成誤判時可以單獨調大，不影響拿起來喝的偵測
#endif
#ifndef EVT_STATIC_HOLD_MS
#define EVT_STATIC_HOLD_MS      8000    // 要持續多久（ms）。8 秒：水位是 1 秒的移動平均，單筆雜訊最多影響 1 秒，留 8 倍；
                                        // 又比休眠中 3 筆的跨度（約 10 秒）短一點，RTC 時鐘有 1% 誤差也不會卡在邊界多等一輪
#endif
#ifndef EVT_STATIC_MIN_SAMPLES
#define EVT_STATIC_MIN_SAMPLES  3       // 至少連續幾筆。主要管休眠（5 秒才一筆）：1 筆是單筆雜訊、2 筆仍可能是巧合，
                                        // 取 3；也避免量測間隔被拉長時，只靠 2 筆就滿足「持續 8 秒」
#endif

#define RAD_TO_DEG_F            57.2957795f
#define ZERO_MAX_TILT_DEG       10.0f

/* ========================================================================== */
/*  姿態                                                                        */
/* ========================================================================== */
// 零點存在 RTC 記憶體：深度睡眠喚醒後沿用（醒來時杯子可能是拿在手上的，不能重新歸零）；
// 拔電、重新燒錄等冷開機時會清成 0
static RTC_DATA_ATTR float s_pitch0, s_roll0;        // 開機零點（度）
static RTC_DATA_ATTR float s_gx0, s_gy0, s_gz0;      // 陀螺儀偏移（度/秒）
static RTC_DATA_ATTR bool s_zero_valid;
static uint32_t s_last_move_ms;
static bool s_have_sample;
static cup_pose_t s_pose = CUP_POSE_UNKNOWN;

static float wrap180(float deg)
{
    while (deg > 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

static void raw_angles(float ax, float ay, float az, float *pitch, float *roll)
{
    *roll = atan2f(ay, az) * RAD_TO_DEG_F;
    *pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * RAD_TO_DEG_F;
}

bool cup_motion_set_zero(float ax, float ay, float az, float gx, float gy, float gz)
{
    float total = sqrtf(ax * ax + ay * ay + az * az);
    float p, r;
    raw_angles(ax, ay, az, &p, &r);
    float tilt = acosf(fminf(1.0f, fmaxf(-1.0f, cosf(p / RAD_TO_DEG_F) * cosf(r / RAD_TO_DEG_F)))) * RAD_TO_DEG_F;
    float gyro = sqrtf(gx * gx + gy * gy + gz * gz);
    if (fabsf(total - 1.0f) > MOVE_ACC_G || tilt > ZERO_MAX_TILT_DEG || gyro > 3 * MOVE_GYRO_DPS) {
        return false;
    }
    s_pitch0 = p;
    s_roll0 = r;
    s_gx0 = gx;
    s_gy0 = gy;
    s_gz0 = gz;
    s_zero_valid = true;
    return true;
}

bool cup_motion_has_zero(void)
{
    return s_zero_valid;
}

void cup_motion_update(float ax, float ay, float az, float gx, float gy, float gz,
                       uint32_t now_ms, cup_motion_t *out)
{
    float p, r;
    raw_angles(ax, ay, az, &p, &r);
    float pitch = PITCH_SIGN * (p - s_pitch0);
    float roll = ROLL_SIGN * wrap180(r - s_roll0);
    float c = cosf(pitch / RAD_TO_DEG_F) * cosf(roll / RAD_TO_DEG_F);
    float tilt = acosf(fminf(1.0f, fmaxf(-1.0f, c))) * RAD_TO_DEG_F;

    // 移動判斷
    float dgx = gx - s_gx0, dgy = gy - s_gy0, dgz = gz - s_gz0;
    float gyro = sqrtf(dgx * dgx + dgy * dgy + dgz * dgz);
    float total = sqrtf(ax * ax + ay * ay + az * az);
    if (!s_have_sample || gyro > MOVE_GYRO_DPS || fabsf(total - 1.0f) > MOVE_ACC_G) {
        s_last_move_ms = now_ms;
        s_have_sample = true;
    }
    uint32_t still_ms = now_ms - s_last_move_ms;
    bool moving = still_ms < MOVE_HOLD_MS;

    // 姿態分類
    float upright_limit = (s_pose == CUP_POSE_UPRIGHT) ? UPRIGHT_EXIT_DEG : UPRIGHT_ENTER_DEG;
    cup_pose_t pose;
    if (tilt < upright_limit) {
        pose = CUP_POSE_UPRIGHT;
    } else if (tilt >= LYING_DEG && still_ms >= LYING_STILL_MS) {
        pose = CUP_POSE_LYING;
    } else if (tilt >= DRINKING_DEG) {
        pose = CUP_POSE_DRINKING;
    } else {
        pose = CUP_POSE_TILTED;
    }
    s_pose = pose;

    out->pitch_deg = pitch;
    out->roll_deg = roll;
    out->tilt_deg = tilt;
    out->moving = moving;
    out->still_ms = still_ms;
    out->pose = pose;
    out->water_ok = (pose == CUP_POSE_UPRIGHT) && still_ms >= WATER_SETTLE_MS;
}

void cup_motion_mark_moving(uint32_t now_ms)
{
    s_last_move_ms = now_ms;
    s_have_sample = true;
}

const char *cup_pose_label(cup_pose_t pose)
{
    switch (pose) {
    case CUP_POSE_UPRIGHT:  return "直立";
    case CUP_POSE_TILTED:   return "傾斜";
    case CUP_POSE_DRINKING: return "飲用中";
    case CUP_POSE_LYING:    return "倒下";
    default:                return "未知";
    }
}

/* ========================================================================== */
/*  水位                                                                        */
/* ========================================================================== */
static uint16_t s_buf[WATER_AVG_N];
static int s_buf_n, s_buf_pos;
// 最後一次有效的水位存在 RTC 記憶體：喚醒後杯子還沒放穩前，App 也能先拿到上次的水位
static RTC_DATA_ATTR bool s_have_water;
static RTC_DATA_ATTR float s_water_pct;
static bool s_holding = true;

static float e0_to_pct(float e0)
{
    float pct = (LEVEL_E0_EMPTY - e0) * 100.0f / (float)(LEVEL_E0_EMPTY - LEVEL_E0_FULL);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
}

void cup_water_add_sample(uint16_t e0, bool valid)
{
    if (!valid) {
        // 傾斜或移動：清掉暫存，回到直立靜止後重新累積，避免混到晃動中的讀值
        s_buf_n = 0;
        s_buf_pos = 0;
        s_holding = true;
        return;
    }
    s_buf[s_buf_pos] = e0;
    s_buf_pos = (s_buf_pos + 1) % WATER_AVG_N;
    if (s_buf_n < WATER_AVG_N) {
        s_buf_n++;
    }
    if (s_buf_n >= WATER_MIN_N) {
        uint32_t sum = 0;
        for (int i = 0; i < s_buf_n; i++) {
            sum += s_buf[i];
        }
        s_water_pct = e0_to_pct((float)sum / s_buf_n);
        s_have_water = true;
        s_holding = false;
    }
}

bool cup_water_get(float *pct)
{
    if (!s_have_water) {
        return false;
    }
    *pct = s_water_pct;
    return true;
}

bool cup_water_holding(void)
{
    return s_holding;
}

/* ========================================================================== */
/*  喝水／加水事件                                                              */
/* ========================================================================== */
// 基準與進行中的過程存在 RTC 記憶體：深度睡眠（若打開）醒來後接著算；斷電重開時清空。
// 時間（now_ms）由呼叫端提供，main.c 用 RTC 計時器（深度睡眠時也繼續走），過程的開始時間醒來後仍有效
static RTC_DATA_ATTR bool s_have_base;
static RTC_DATA_ATTR float s_base_pct;          // 基準水位 A
static RTC_DATA_ATTR bool s_episode;            // 是否有進行中的過程（拿起還沒放回穩定）
static RTC_DATA_ATTR uint32_t s_episode_start;
static RTC_DATA_ATTR bool s_saw_drinking;
static bool s_still;                            // 目前直立且不在移動
static uint32_t s_still_since;
// 直立不動時的下降確認。故意不放 RTC 記憶體：深度睡眠醒來後從頭確認（基準有保留、而且沒有往下跟，不會漏算）；
// 放了反而會讓「睡前 1 筆 + 醒來 1 筆」就滿足持續時間
static int s_drop_n;                            // 已經連續幾筆「比基準低 ≥ 門檻」（0 = 目前沒有）
static uint32_t s_drop_since;                   // 這一串第一筆的時間

static void drop_clear(void)
{
    s_drop_n = 0;
}

static void episode_begin(uint32_t now_ms)
{
    // 還沒有基準（剛開機）時不開始過程：第一次穩定只建立基準
    if (s_have_base && !s_episode) {
        s_episode = true;
        s_episode_start = now_ms;
        s_saw_drinking = false;
    }
}

// 穩定、沒有進行中的過程時拿到一筆水位（活動中每 100 ms、休眠中每 5 秒）：直立不動的規則
static bool track_stable(float level, uint32_t now_ms, cup_event_t *out)
{
    // 上升或持平（杯子放著不動時倒水進去、往上的漂移）：基準跟著走，不產生事件
    if (!EVT_STATIC_DROP_ENABLE || level >= s_base_pct) {
        s_base_pct = level;
        drop_clear();
        return false;
    }
    // 比基準低：基準不往下跟（小口喝可以累積；這時拿起杯子，過程會用原本的基準結算）
    const float th = EVT_STATIC_DELTA_PCT - 1e-4f;   // 容許浮點誤差，剛好等於門檻也算
    if (s_base_pct - level < th) {
        drop_clear();          // 還不到門檻（或回到門檻以內）：連續筆數重新算
        return false;
    }
    if (s_drop_n == 0) {
        s_drop_since = now_ms;
    }
    if (s_drop_n < 1000000) {
        s_drop_n++;
    }
    if (s_drop_n < EVT_STATIC_MIN_SAMPLES || (uint32_t)(now_ms - s_drop_since) < EVT_STATIC_HOLD_MS) {
        return false;          // 還沒持續夠久
    }
    out->kind = CUP_EVT_DRINK;
    out->before_pct = s_base_pct;
    out->after_pct = level;
    out->saw_drinking = false;
    out->too_long = (uint32_t)(now_ms - s_drop_since) > EVT_MAX_EPISODE_MS;
    out->stationary = true;
    out->start_ms = s_drop_since;   // 水位開始連續低於「基準 − 門檻」的時間
    out->end_ms = now_ms;
    s_base_pct = level;             // 之後（包括拿起來喝）都從 B 算起，不會重複計算
    drop_clear();
    return true;
}

// 穩定時拿到水位 B：還沒有基準 → 建立基準；有過程 → 結算。
// 結算後基準 = B，只有「下降不到門檻」例外（維持 A）。這樣基準只會經由喝水事件往下走，
// 所有喝水事件的量加起來 = 基準下降的量，不會多記也不會漏
static bool settle(float level, uint32_t now_ms, cup_event_t *out)
{
    float before = s_base_pct;
    bool had_base = s_have_base;
    s_base_pct = level;
    s_have_base = true;
    drop_clear();
    if (!had_base || !s_episode) {
        return false;
    }
    bool saw = s_saw_drinking;
    uint32_t start = s_episode_start;
    s_episode = false;
    s_saw_drinking = false;

    // 容許浮點誤差，剛好等於門檻也算
    const float th = EVT_MIN_DELTA_PCT - 1e-4f;
    cup_event_kind_t kind;
    if (before - level >= th) {
        kind = CUP_EVT_DRINK;
    } else if (level - before >= th) {
        kind = CUP_EVT_REFILL;
    } else {
        // 變化不到門檻，不產生事件。上升：基準照舊跟著往上（上面已經改成 B）。
        // 下降：基準不往下跟，維持 A（和直立不動的規則一致），這一小口留著和之後的下降累積到門檻再記；
        // 之後不論由哪一條規則記，都是從 A 算起、記完基準才改成 B，所以只會被記一次。
        // EVT_STATIC_DROP_ENABLE = 0 時照 0.3.0 改成 B（那時穩定期間基準本來就會往下跟，留著也沒有用）
        if (EVT_STATIC_DROP_ENABLE && level < before) {
            s_base_pct = before;
        }
        return false;
    }
    out->kind = kind;
    out->before_pct = before;
    out->after_pct = level;
    out->saw_drinking = saw;
    out->too_long = (uint32_t)(now_ms - start) > EVT_MAX_EPISODE_MS;
    out->stationary = false;
    out->start_ms = start;
    out->end_ms = now_ms;
    return true;
}

bool cup_event_update(cup_pose_t pose, bool moving, bool water_fresh, float water_pct,
                      uint32_t now_ms, cup_event_t *out)
{
    bool still = (pose == CUP_POSE_UPRIGHT) && !moving;
    if (!still) {
        // 離開穩定（傾斜／飲用中／倒下／移動／姿態未知）：開始或延續一段過程。
        // 放下不到 3 秒又拿起，過程還沒結算，算同一段
        s_still = false;
        drop_clear();   // 直立不動時還沒確認的下降交給這段過程結算（基準沒有往下跟，不會漏）
        episode_begin(now_ms);
        if (s_episode && pose == CUP_POSE_DRINKING) {
            s_saw_drinking = true;
        }
        return false;
    }
    if (!s_still) {
        s_still = true;
        s_still_since = now_ms;
    }
    if (!water_fresh || (uint32_t)(now_ms - s_still_since) < EVT_SETTLE_MS) {
        return false;
    }
    if (!s_have_base || s_episode) {
        return settle(water_pct, now_ms, out);
    }
    return track_stable(water_pct, now_ms, out);
}

bool cup_event_doze_water(float water_pct, uint32_t now_ms, cup_event_t *out)
{
    // 休眠中的讀值視為穩定（進入休眠前已經直立靜止 30 秒）。有進行中的過程時不處理：
    // 正常不會發生，留給喚醒後的 cup_event_update 結算
    if (s_episode) {
        return false;
    }
    if (!s_have_base) {
        s_base_pct = water_pct;
        s_have_base = true;
        drop_clear();
        return false;
    }
    return track_stable(water_pct, now_ms, out);
}

void cup_event_wake(uint32_t now_ms)
{
    s_still = false;
    drop_clear();   // 休眠中還沒確認的下降交給接下來這段過程結算
    episode_begin(now_ms);
}

bool cup_event_baseline(float *pct)
{
    if (!s_have_base) {
        return false;
    }
    *pct = s_base_pct;
    return true;
}

bool cup_event_in_episode(void)
{
    return s_episode;
}

void cup_event_reset(void)
{
    s_have_base = false;
    s_base_pct = 0.0f;
    s_episode = false;
    s_episode_start = 0;
    s_saw_drinking = false;
    s_still = false;
    s_still_since = 0;
    s_drop_n = 0;
    s_drop_since = 0;
}

static uint32_t pct_to_half(float pct)
{
    long v = lroundf(pct * 2.0f);   // 單位 0.5%
    if (v < 0) v = 0;
    if (v > 200) v = 200;
    return (uint32_t)v;
}

uint32_t cup_event_encode(const cup_event_t *ev, uint16_t seq, bool time_unknown)
{
    return pct_to_half(ev->before_pct)
         | (pct_to_half(ev->after_pct) << 8)
         | (((uint32_t)ev->kind & 0x3u) << 16)
         | ((ev->saw_drinking ? 1u : 0u) << 18)
         | ((ev->too_long ? 1u : 0u) << 19)
         | (time_unknown ? CUP_EVT_TIME_UNKNOWN : 0u)
         | (((uint32_t)seq & CUP_EVT_SEQ_MASK) << 21);
}

/* ========================================================================== */
/*  Device Status                                                               */
/* ========================================================================== */
size_t cup_status_encode(uint8_t battery, uint8_t major, uint8_t minor, uint8_t patch,
                         const uint8_t *addr_le, uint8_t out[CUP_STATUS_LEN])
{
    out[0] = battery;
    out[1] = major;
    out[2] = minor;
    out[3] = patch;
    if (addr_le == NULL) {
        return CUP_STATUS_LEN_NO_ADDR;
    }
    for (int i = 0; i < 6; i++) {
        out[4 + i] = addr_le[5 - i];   // 最低位元組在前 → 顯示順序（最高位元組在前）
    }
    return CUP_STATUS_LEN;
}

/* ========================================================================== */
/*  電量                                                                        */
/* ========================================================================== */
uint8_t cup_battery_percent(int bat_mv)
{
    // 低於 2.5 V：沒接電池（只插 USB 時 BAT+ 浮動，實測約 0.5～0.9 V）
    if (bat_mv < 2500) {
        return 0xFF;
    }
    // 鋰電池放電曲線（粗略），區間內線性內插
    static const struct { int mv; int pct; } k_curve[] = {
        { 3000, 0 }, { 3300, 5 }, { 3500, 10 }, { 3600, 20 }, { 3700, 35 },
        { 3800, 50 }, { 3900, 65 }, { 4000, 80 }, { 4100, 90 }, { 4200, 100 },
    };
    const size_t n = sizeof(k_curve) / sizeof(k_curve[0]);
    if (bat_mv <= k_curve[0].mv) return 0;
    if (bat_mv >= k_curve[n - 1].mv) return 100;
    for (size_t i = 1; i < n; i++) {
        if (bat_mv <= k_curve[i].mv) {
            int dv = k_curve[i].mv - k_curve[i - 1].mv;
            int dp = k_curve[i].pct - k_curve[i - 1].pct;
            return (uint8_t)(k_curve[i - 1].pct + (bat_mv - k_curve[i - 1].mv) * dp / dv);
        }
    }
    return 100;
}
