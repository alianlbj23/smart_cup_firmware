#include "cup_state.h"

#include <math.h>
#include <stddef.h>

#include "esp_attr.h"   // RTC_DATA_ATTR：深度睡眠時保留

/* ========================================================================== */
/*  可調參數                                                                    */
/* ========================================================================== */
// 水位校正（電極0 的濾波後資料）：水越多讀值越小，兩點之間用直線換算
// 1 µs 充電時實測（2026-11-19）：空杯 403.3（±1）、滿杯 298.0（±3），總變化 105
#define LEVEL_E0_EMPTY          403     // 空杯時電極0 的讀值
#define LEVEL_E0_FULL           298     // 滿杯時電極0 的讀值

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
