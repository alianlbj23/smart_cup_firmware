/*
 * cup_state.c 喝水／加水事件偵測的情境測試（在電腦上跑：sh test/host/run.sh）
 *
 * 模擬 main.c live_loop 的節奏：每 100 ms 給一次姿態、移動、水位。
 * 「水位新鮮」照 cup_water_add_sample 的行為模擬：直立且靜止 ≥ 1 秒後才有新讀值。
 * 休眠（doze_loop）每 5 秒量一次水位，用 doze_sample() 模擬。
 *
 * run.sh 會編兩次：預設參數，以及 -DEVT_STATIC_DROP_ENABLE=0（關掉「直立不動時水位下降也算喝水」，
 * 這時定義 TEST_STATIC_DISABLED，改測「關掉後回到 0.3.0 的行為」）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cup_state.h"

#define TICK_MS 100
// 要和 cup_state.c 的預設值一致（另外 EVT_STATIC_MIN_SAMPLES = 3：休眠測試裡「連續 3 筆」寫死在情境裡）
#define STATIC_HOLD_MS      8000    // EVT_STATIC_HOLD_MS
#define DOZE_WATER_MS       5000    // main.c 的 DOZE_WATER_CHECK_MS

// 拿起放回結算時「下降不到門檻」之後的基準：0.4.0 起不往下跟，維持 A（小口可以累積）；
// EVT_STATIC_DROP_ENABLE=0 時回到 0.3.0 的行為，改成 B
#ifdef TEST_STATIC_DISABLED
#define SMALL_DROP_BASE(a, b) (b)
#else
#define SMALL_DROP_BASE(a, b) (a)
#endif

static uint32_t s_now;
static uint32_t s_upright_still_since;
static bool s_was_upright_still;
static cup_event_t s_events[16];
static int s_n_events;
static int s_fail;
static int s_checks;

#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { printf("    失敗：%s（第 %d 行）\n", #cond, __LINE__); s_fail++; } \
} while (0)

static void record(const cup_event_t *ev)
{
    if (s_n_events < 16) {
        s_events[s_n_events] = *ev;
    }
    s_n_events++;
}

static void reset_all(void)
{
    cup_event_reset();
    s_now = 1000;
    s_was_upright_still = false;
    s_n_events = 0;
}

// 持續 ms 毫秒給同樣的輸入
static void run(cup_pose_t pose, bool moving, float pct, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        bool us = (pose == CUP_POSE_UPRIGHT) && !moving;
        if (us && !s_was_upright_still) {
            s_upright_still_since = s_now;
        }
        s_was_upright_still = us;
        bool fresh = us && (s_now - s_upright_still_since) >= 1000;
        cup_event_t ev;
        if (cup_event_update(pose, moving, fresh, pct, s_now, &ev)) {
            record(&ev);
        }
        s_now += TICK_MS;
    }
}

static void put_down(float pct, uint32_t ms) { run(CUP_POSE_UPRIGHT, false, pct, ms); }

// 休眠中量到一筆水位（時間不前進）
static void doze_water(float pct)
{
    cup_event_t ev;
    if (cup_event_doze_water(pct, s_now, &ev)) {
        record(&ev);
    }
}

// 休眠中量一筆水位，然後過 5 秒（doze_loop 的節奏）
static void doze_sample(float pct)
{
    doze_water(pct);
    s_now += DOZE_WATER_MS;
}

// 一口：拿起（移動）→ 傾斜 → 飲用中 → 傾斜 → 放下（還在晃）
static void sip(float level_during)
{
    run(CUP_POSE_UPRIGHT, true, level_during, 500);
    run(CUP_POSE_TILTED, true, level_during, 500);
    run(CUP_POSE_DRINKING, true, level_during, 2000);
    run(CUP_POSE_TILTED, true, level_during, 500);
    run(CUP_POSE_UPRIGHT, true, level_during, 500);
}

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static void t_first_settle_only_baseline(void)
{
    printf("開機第一次穩定只建立基準\n");
    reset_all();
    run(CUP_POSE_TILTED, true, 50, 2000);
    put_down(50, 5000);
    CHECK(s_n_events == 0);
    float b;
    CHECK(cup_event_baseline(&b) && near(b, 50));
}

static void t_one_sip(void)
{
    printf("喝一口\n");
    reset_all();
    put_down(60, 5000);
    uint32_t lift_at = s_now;
    sip(60);
    put_down(55, 2900);
    CHECK(s_n_events == 0);   // 還沒穩定 3 秒
    put_down(55, 2000);
    CHECK(s_n_events == 1);
    cup_event_t *e = &s_events[0];
    CHECK(e->kind == CUP_EVT_DRINK);
    CHECK(near(e->before_pct, 60) && near(e->after_pct, 55));
    CHECK(e->saw_drinking && !e->too_long);
    CHECK(e->start_ms == lift_at);
    CHECK(e->end_ms - e->start_ms >= 4000 + 3000);
}

static void t_continuous_sips(void)
{
    printf("連續喝（放下不到 3 秒又拿起，算同一段）\n");
    reset_all();
    put_down(80, 5000);
    sip(80);
    put_down(75, 1500);
    sip(75);
    put_down(70, 1000);
    sip(70);
    put_down(66, 5000);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_DRINK);
    CHECK(near(s_events[0].before_pct, 80) && near(s_events[0].after_pct, 66));

    printf("喝兩口，中間有放穩 → 兩筆\n");
    reset_all();
    put_down(80, 5000);
    sip(80);
    put_down(75, 5000);
    sip(75);
    put_down(70, 5000);
    CHECK(s_n_events == 2);
    CHECK(near(s_events[0].after_pct, 75) && near(s_events[1].before_pct, 75));
}

static void t_refill(void)
{
    printf("加水（拿去裝水，沒有飲用姿態）\n");
    reset_all();
    put_down(20, 5000);
    run(CUP_POSE_UPRIGHT, true, 20, 3000);
    run(CUP_POSE_TILTED, true, 20, 5000);
    run(CUP_POSE_UPRIGHT, true, 20, 1000);
    put_down(85, 5000);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_REFILL);
    CHECK(!s_events[0].saw_drinking && !s_events[0].too_long);
    CHECK(near(s_events[0].before_pct, 20) && near(s_events[0].after_pct, 85));
}

static void t_drink_then_refill(void)
{
    printf("喝了又裝水 → refill + saw_drinking\n");
    reset_all();
    put_down(40, 5000);
    sip(40);
    run(CUP_POSE_TILTED, true, 40, 4000);   // 拿去裝水
    put_down(90, 5000);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_REFILL && s_events[0].saw_drinking);
}

static void t_drop_without_drinking(void)
{
    printf("水位下降但沒有飲用姿態（可能是倒掉）→ drink、saw_drinking = 0\n");
    reset_all();
    put_down(70, 5000);
    run(CUP_POSE_TILTED, true, 70, 3000);
    put_down(40, 5000);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_DRINK && !s_events[0].saw_drinking);
}

static void t_noise(void)
{
    printf("拿起放回、下降 < 3%% → 不產生事件；基準不往下跟，維持 A（EVT_STATIC_DROP_ENABLE=0 時照 0.3.0 改成 B）\n");
    reset_all();
    put_down(50, 5000);
    sip(50);
    put_down(48, 5000);
    CHECK(s_n_events == 0);
    float b;
    CHECK(cup_event_baseline(&b) && near(b, SMALL_DROP_BASE(50, 48)));
    CHECK(!cup_event_in_episode());
    // 下一口：0.4.0 起從原本的基準 50 算（這 2% 會累積進來）；關掉時和新的基準 48 比
    sip(48);
    put_down(45, 5000);
    CHECK(s_n_events == 1 && near(s_events[0].before_pct, SMALL_DROP_BASE(50, 48)));
    CHECK(near(s_events[0].after_pct, 45));

    printf("剛好等於門檻 3%% 也算\n");
    reset_all();
    put_down(50, 5000);
    sip(50);
    put_down(47, 5000);
    CHECK(s_n_events == 1);

    printf("放在桌上晃一下（直立但移動）、水位不變 → 不產生事件\n");
    reset_all();
    put_down(50, 5000);
    run(CUP_POSE_UPRIGHT, true, 50, 1000);
    put_down(50.5f, 5000);
    CHECK(s_n_events == 0);
}

#ifndef TEST_STATIC_DISABLED
// 所有加水事件的量加起來(%)
static float refill_total(void)
{
    float sum = 0;
    for (int i = 0; i < s_n_events && i < 16; i++) {
        if (s_events[i].kind == CUP_EVT_REFILL) {
            sum += s_events[i].after_pct - s_events[i].before_pct;
        }
    }
    return sum;
}
#endif

static void t_static_drift(void)
{
    reset_all();
    put_down(30, 5000);
    for (int i = 0; i <= 50; i++) {
        put_down(30 + i, 200);
    }
    put_down(80, 12000);
    float b;
    CHECK(cup_event_baseline(&b) && near(b, 80));
#ifdef TEST_STATIC_DISABLED
    printf("杯子不動時倒水進去 → 只更新基準(直立不動的規則關掉時)\n");
    CHECK(s_n_events == 0);
#else
    printf("杯子不動時慢慢倒水進去(10 秒)→ 拆成前後相接的加水事件,加起來等於實際加的量\n");
    CHECK(s_n_events >= 1);
    for (int i = 0; i < s_n_events && i < 16; i++) {
        CHECK(s_events[i].kind == CUP_EVT_REFILL && s_events[i].stationary);
        if (i > 0) {
            CHECK(near(s_events[i].before_pct, s_events[i - 1].after_pct));
        }
    }
    CHECK(near(s_events[0].before_pct, 30));
    CHECK(near(refill_total(), 50));
#endif
}

static void t_too_long(void)
{
    printf("過程超過 10 分鐘 → 照樣產生事件，帶 too_long\n");
    reset_all();
    put_down(90, 5000);
    sip(90);
    run(CUP_POSE_TILTED, true, 90, 11 * 60 * 1000);
    put_down(60, 5000);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_DRINK && s_events[0].too_long && s_events[0].saw_drinking);

    printf("倒下靜止 → 一樣算離開穩定\n");
    reset_all();
    put_down(50, 5000);
    run(CUP_POSE_TILTED, true, 50, 1000);
    run(CUP_POSE_LYING, false, 50, 5000);
    put_down(10, 5000);
    CHECK(s_n_events == 1 && s_events[0].kind == CUP_EVT_DRINK && !s_events[0].saw_drinking);
}

static void t_doze(void)
{
    printf("休眠中水位變化 → 只更新基準；喚醒後的過程和新基準比\n");
    reset_all();
    put_down(60, 31000);
    // 進入休眠（直立靜止）：休眠中每 5 秒量一次，有人直接往杯子裡倒水
    doze_water(60);
    s_now += 5000;
    doze_water(75);
    s_now += 5000;
    CHECK(s_n_events == 0);
    float b;
    CHECK(cup_event_baseline(&b) && near(b, 75));
    // 拿起杯子 → 喚醒
    uint32_t wake_at = s_now;
    cup_event_wake(s_now);
    CHECK(cup_event_in_episode());
    sip(75);
    put_down(70, 5000);
    CHECK(s_n_events == 1);
    CHECK(near(s_events[0].before_pct, 75) && near(s_events[0].after_pct, 70));
    CHECK(s_events[0].start_ms == wake_at);

    printf("喚醒後放回、水位沒變 → 不產生事件\n");
    reset_all();
    put_down(60, 5000);
    doze_water(60);
    cup_event_wake(s_now);
    run(CUP_POSE_UPRIGHT, true, 60, 500);
    put_down(60, 5000);
    CHECK(s_n_events == 0 && !cup_event_in_episode());
}

static void t_encode(void)
{
    printf("0x07 value 編碼\n");
    cup_event_t e = {
        .kind = CUP_EVT_REFILL, .before_pct = 60.0f, .after_pct = 55.4f,
        .saw_drinking = true, .too_long = false,
    };
    uint32_t v = cup_event_encode(&e, 2047, true);
    CHECK((v & 0xFF) == 120);
    CHECK(((v >> 8) & 0xFF) == 111);       // 55.4% → 110.8 → 111
    CHECK(((v >> 16) & 0x3) == 1);
    CHECK(((v >> 18) & 1) == 1);
    CHECK(((v >> 19) & 1) == 0);
    CHECK(((v >> 20) & 1) == 1);
    CHECK((v >> 21) == 2047);
    e.before_pct = 100.0f;
    e.after_pct = -3.0f;
    e.kind = CUP_EVT_DRINK;
    e.saw_drinking = false;
    e.too_long = true;
    v = cup_event_encode(&e, 5, false);
    CHECK((v & 0xFF) == 200 && ((v >> 8) & 0xFF) == 0);
    CHECK(((v >> 16) & 0x3) == 0 && ((v >> 18) & 1) == 0 && ((v >> 19) & 1) == 1 && ((v >> 20) & 1) == 0);
    CHECK((v >> 21) == 5);
}


/* ========================================================================== */
/*  直立不動時水位下降也算喝水（韌體 0.4.0，用吸管喝）                           */
/* ========================================================================== */
static bool baseline_is(float want)
{
    float b;
    return cup_event_baseline(&b) && near(b, want);
}

#ifndef TEST_STATIC_DISABLED
static void t_static_drop(void)
{
    printf("直立不動、水位下降 ≥ 3%% 而且持續 → 一筆喝水（沒有飲用姿態）\n");
    reset_all();
    put_down(60, 5000);
    uint32_t drop_at = s_now;
    put_down(55, STATIC_HOLD_MS);          // 還差一筆才滿 8 秒
    CHECK(s_n_events == 0);
    CHECK(baseline_is(60));                // 確認之前基準不往下跟
    put_down(55, 100);
    CHECK(s_n_events == 1);
    cup_event_t *e = &s_events[0];
    CHECK(e->kind == CUP_EVT_DRINK);
    CHECK(near(e->before_pct, 60) && near(e->after_pct, 55));
    CHECK(!e->saw_drinking && !e->too_long && e->stationary);
    CHECK(e->start_ms == drop_at);
    CHECK(e->end_ms - e->start_ms == STATIC_HOLD_MS);
    CHECK(baseline_is(55) && !cup_event_in_episode());
    // 0x07 的 value：種類 0（喝水）、saw_drinking = 0
    uint32_t v = cup_event_encode(e, 7, false);
    CHECK((v & 0xFF) == 120 && ((v >> 8) & 0xFF) == 110);
    CHECK(((v >> 16) & 0x3) == 0 && ((v >> 18) & 1) == 0 && ((v >> 19) & 1) == 0);
    // 之後水位不變 → 不會再產生
    put_down(55, 60000);
    CHECK(s_n_events == 1);

    printf("剛好下降 3%% 也算；只下降 2.5%% 放再久也不算\n");
    reset_all();
    put_down(50, 5000);
    put_down(47, STATIC_HOLD_MS + 100);
    CHECK(s_n_events == 1 && near(s_events[0].before_pct, 50) && near(s_events[0].after_pct, 47));
    reset_all();
    put_down(50, 5000);
    put_down(47.5f, 120000);
    CHECK(s_n_events == 0 && baseline_is(50));

    printf("用吸管慢慢喝（每 2 秒降 0.5%%，共 10%%）→ 事件前後相接，加起來剛好 10%%\n");
    reset_all();
    put_down(60, 5000);
    for (int i = 1; i <= 20; i++) {
        put_down(60 - 0.5f * i, 2000);
    }
    put_down(50, 30000);
    CHECK(s_n_events >= 1 && s_n_events <= 16);
    float sum = 0;
    for (int i = 0; i < s_n_events && i < 16; i++) {
        CHECK(s_events[i].kind == CUP_EVT_DRINK && s_events[i].stationary && !s_events[i].saw_drinking);
        CHECK(near(s_events[i].before_pct, i == 0 ? 60 : s_events[i - 1].after_pct));
        sum += s_events[i].before_pct - s_events[i].after_pct;
    }
    CHECK(near(sum, 10));
    CHECK(baseline_is(50));
}

static void t_static_noise(void)
{
    printf("單筆雜訊（一筆掉 10%% 馬上回來）→ 不產生事件、基準不變\n");
    reset_all();
    put_down(60, 5000);
    put_down(50, 100);
    put_down(60, 20000);
    CHECK(s_n_events == 0 && baseline_is(60));

    printf("掉下去快 8 秒又回到基準 → 不產生事件；再掉要重新連續算滿 8 秒\n");
    reset_all();
    put_down(60, 5000);
    put_down(55, STATIC_HOLD_MS);
    put_down(60, 100);
    uint32_t again_at = s_now;
    put_down(55, STATIC_HOLD_MS);
    CHECK(s_n_events == 0 && baseline_is(60));
    put_down(55, 100);
    CHECK(s_n_events == 1 && s_events[0].start_ms == again_at);

    printf("中間有一筆回到門檻以內（只低 2%%）→ 一樣重新算\n");
    reset_all();
    put_down(60, 5000);
    put_down(55, 4000);
    put_down(58, 100);
    again_at = s_now;
    put_down(55, STATIC_HOLD_MS);
    CHECK(s_n_events == 0 && baseline_is(60));
    put_down(55, 100);
    CHECK(s_n_events == 1 && s_events[0].start_ms == again_at);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 55));
}

// 固定種子的亂數（LCG），回傳 −amp～+amp 的雜訊，每次跑結果都一樣
static uint32_t s_rng;
static float noise(float amp)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return ((float)(s_rng >> 8) / 8388608.0f - 1.0f) * amp;   // 24 bits → −1～+1
}

static void t_static_with_noise(void)
{
    printf("讀值有 ±1%% 雜訊，靜置 1 小時（活動中）→ 不產生事件；接著用吸管喝 5%% → 剛好一筆\n");
    reset_all();
    s_rng = 12345;
    for (int i = 0; i < 36000; i++) {
        put_down(60 + noise(1.0f), 100);
    }
    CHECK(s_n_events == 0);
    for (int i = 0; i < 300; i++) {
        put_down(55 + noise(1.0f), 100);
    }
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_DRINK && s_events[0].stationary);
    // 基準是穩定期間看過的最高值，A 會比真正的水位高一點（最多一個雜訊幅度），B 也帶雜訊
    float got = s_events[0].before_pct - s_events[0].after_pct;
    CHECK(got >= 3.0f && got <= 7.0f);
    for (int i = 0; i < 6000; i++) {
        put_down(55 + noise(1.0f), 100);
    }
    CHECK(s_n_events == 1);

    printf("休眠中讀值有 ±1%% 雜訊，靜置 2 小時 → 不產生事件；接著喝 5%% → 剛好一筆\n");
    reset_all();
    s_rng = 67890;
    put_down(60, 31000);
    for (int i = 0; i < 1440; i++) {
        doze_sample(60 + noise(1.0f));
    }
    CHECK(s_n_events == 0);
    for (int i = 0; i < 12; i++) {
        doze_sample(55 + noise(1.0f));
    }
    CHECK(s_n_events == 1);
    got = s_events[0].before_pct - s_events[0].after_pct;
    CHECK(s_events[0].stationary && got >= 3.0f && got <= 7.0f);
    for (int i = 0; i < 720; i++) {
        doze_sample(55 + noise(1.0f));
    }
    CHECK(s_n_events == 1);
}

// 「拿起放回、下降不到門檻時基準不往下跟」：小口可以跨過好幾次拿起放回累積，而且只會被記一次
static void t_small_drop_accumulate(void)
{
    printf("用吸管小口 2%% → 杯子被碰一下 → 再小口 2%% → 一筆（直立不動），量是 4%%\n");
    reset_all();
    put_down(60, 5000);
    put_down(58, 20000);
    run(CUP_POSE_UPRIGHT, true, 58, 1000);      // 被碰一下
    put_down(58, 5000);
    CHECK(s_n_events == 0 && baseline_is(60) && !cup_event_in_episode());
    put_down(56, STATIC_HOLD_MS);
    CHECK(s_n_events == 0);
    put_down(56, 100);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_DRINK && s_events[0].stationary && !s_events[0].saw_drinking);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 56));
    put_down(56, 60000);
    CHECK(s_n_events == 1 && baseline_is(56));

    printf("拿起來小口 2%% → 放回 → 再拿起來小口 2%% → 一筆（拿起放回），量是 4%%，之後不重複\n");
    reset_all();
    put_down(60, 5000);
    sip(60);
    put_down(58, 5000);
    CHECK(s_n_events == 0 && baseline_is(60));
    uint32_t second_at = s_now;
    sip(58);
    put_down(56, 5000);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_DRINK && !s_events[0].stationary && s_events[0].saw_drinking);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 56));
    CHECK(s_events[0].start_ms == second_at && !s_events[0].too_long);   // 開始時間是跨過門檻的那一次拿起
    put_down(56, 60000);
    CHECK(s_n_events == 1 && baseline_is(56));

    printf("每次拿起來只喝 1%% → 第三次才一筆 60 → 57\n");
    reset_all();
    put_down(60, 5000);
    sip(60);
    put_down(59, 5000);
    sip(59);
    put_down(58, 5000);
    CHECK(s_n_events == 0 && baseline_is(60));
    sip(58);
    put_down(57, 5000);
    CHECK(s_n_events == 1 && near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 57));

    printf("拿起放回降了 2%%（不到門檻）之後，直立不動再降 2%% → 直立不動的事件從原本的基準算，只有一筆\n");
    reset_all();
    put_down(60, 5000);
    sip(60);
    put_down(58, 5000);
    put_down(56, STATIC_HOLD_MS + 100);
    CHECK(s_n_events == 1 && s_events[0].stationary);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 56));
    sip(56);
    put_down(56, 5000);
    CHECK(s_n_events == 1 && baseline_is(56));

    printf("拿起放回、上升不到門檻 → 基準照舊跟著往上\n");
    reset_all();
    put_down(60, 5000);
    run(CUP_POSE_UPRIGHT, true, 60, 1000);
    put_down(62, 5000);
    CHECK(s_n_events == 0 && baseline_is(62));
    // 先降 2%（基準留在 60），再放回時是 59：還是比基準低，不動；放回時是 61：往上跟
    reset_all();
    put_down(60, 5000);
    sip(60);
    put_down(58, 5000);
    run(CUP_POSE_UPRIGHT, true, 58, 1000);
    put_down(59, 5000);
    CHECK(s_n_events == 0 && baseline_is(60));
    run(CUP_POSE_UPRIGHT, true, 59, 1000);
    put_down(61, 5000);
    CHECK(s_n_events == 0 && baseline_is(61));

    printf("小口降了 2%% 之後拿去加水 → 加水事件從原本的基準算\n");
    reset_all();
    put_down(60, 5000);
    sip(60);
    put_down(58, 5000);
    run(CUP_POSE_TILTED, true, 58, 3000);
    put_down(90, 5000);
    CHECK(s_n_events == 1 && s_events[0].kind == CUP_EVT_REFILL);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 90) && baseline_is(90));

    printf("沒喝水、反覆拿起放下 200 次、讀值有 ±1%% 雜訊 → 不產生事件\n");
    reset_all();
    s_rng = 24680;
    put_down(60, 5000);
    for (int i = 0; i < 200; i++) {
        float lv = 60 + noise(1.0f);
        run(CUP_POSE_UPRIGHT, true, lv, 500);
        run(CUP_POSE_TILTED, true, lv, 1000);
        run(CUP_POSE_UPRIGHT, true, lv, 500);
        put_down(lv, 5000);
    }
    CHECK(s_n_events == 0);

    printf("混合：小口、被碰、拿起來喝、直立不動交錯，共降 20%% → 每一筆前後相接，總和 = 基準下降的量，不多不少\n");
    reset_all();
    put_down(80, 5000);
    float level = 80;
    s_rng = 13579;
    for (int i = 0; i < 20; i++) {
        level -= 1.0f;                          // 每一步喝掉 1%
        s_rng = s_rng * 1664525u + 1013904223u;
        switch ((s_rng >> 16) % 4) {
        case 0:  put_down(level, 12000); break;                                               // 用吸管，杯子沒動
        case 1:  run(CUP_POSE_UPRIGHT, true, level, 800); put_down(level, 5000); break;       // 用吸管時碰到杯子
        case 2:  sip(level + 1.0f); put_down(level, 5000); break;                             // 拿起來喝
        default: sip(level + 1.0f); put_down(level, 1500); break;                             // 拿起來喝，放下不到 3 秒
        }
    }
    put_down(60, 30000);
    CHECK(s_n_events >= 1 && s_n_events <= 16);
    float sum = 0;
    for (int i = 0; i < s_n_events && i < 16; i++) {
        CHECK(s_events[i].kind == CUP_EVT_DRINK);
        CHECK(near(s_events[i].before_pct, i == 0 ? 80 : s_events[i - 1].after_pct));
        sum += s_events[i].before_pct - s_events[i].after_pct;
    }
    float base = 0;
    CHECK(cup_event_baseline(&base));
    CHECK(near(sum, 80 - base));                // 記到的量 = 基準下降的量
    CHECK(base - 60 >= -0.01f && base - 60 < 3.0f);   // 還沒記到的（不到門檻）留在基準和水位之間，沒有丟掉
}

static void t_static_small_sips(void)
{
    printf("多次小口（每次 1%%）→ 累積到 3%% 才一筆，A 是原本的基準\n");
    reset_all();
    put_down(60, 5000);
    put_down(59, 20000);
    CHECK(s_n_events == 0 && baseline_is(60));
    put_down(58, 20000);
    CHECK(s_n_events == 0 && baseline_is(60));
    uint32_t at = s_now;
    put_down(57, STATIC_HOLD_MS);
    CHECK(s_n_events == 0);
    put_down(57, 100);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_DRINK && s_events[0].stationary);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 57));
    CHECK(s_events[0].start_ms == at && !s_events[0].too_long);
    CHECK(baseline_is(57));
}

static void t_static_rise(void)
{
    printf("直立不動時倒水進去 → 持續 8 秒後記一筆加水;之後的下降和新的基準比\n");
    reset_all();
    put_down(30, 5000);
    uint32_t poured_at = s_now;
    put_down(45, STATIC_HOLD_MS - 200);
    CHECK(s_n_events == 0 && baseline_is(45));   // 還沒持續夠久;基準照舊馬上跟上去
    put_down(45, 400);
    CHECK(s_n_events == 1);
    cup_event_t *e = &s_events[0];
    CHECK(e->kind == CUP_EVT_REFILL && e->stationary && !e->saw_drinking && !e->too_long);
    CHECK(near(e->before_pct, 30) && near(e->after_pct, 45));
    CHECK(e->start_ms == poured_at);
    put_down(45, 60000);
    CHECK(s_n_events == 1 && baseline_is(45));   // 同一次加水只記一筆
    put_down(41, STATIC_HOLD_MS + 100);
    CHECK(s_n_events == 2 && s_events[1].kind == CUP_EVT_DRINK);
    CHECK(near(s_events[1].before_pct, 45) && near(s_events[1].after_pct, 41));

    printf("小口喝了 2%%（還沒到門檻）之後有人倒水進去 → 加水從原本的基準算起\n");
    reset_all();
    put_down(60, 5000);
    put_down(58, 20000);
    put_down(70, 20000);
    CHECK(s_n_events == 1 && s_events[0].kind == CUP_EVT_REFILL && baseline_is(70));
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 70));
    put_down(68, 60000);
    CHECK(s_n_events == 1 && baseline_is(70));
}

static void t_static_refill(void)
{
    printf("加水不到 3%% → 不記\n");
    reset_all();
    put_down(50, 5000);
    put_down(52.5f, 60000);
    CHECK(s_n_events == 0 && baseline_is(52.5f));

    printf("讀值上下跳(±1.5%%)很久 → 不會被當成加水\n");
    reset_all();
    put_down(50, 5000);
    for (int i = 0; i < 600; i++) {
        put_down(50 + (i % 3 == 0 ? 1.5f : i % 3 == 1 ? -1.5f : 0.0f), 500);
    }
    CHECK(s_n_events == 0);

    printf("讀值慢慢往上漂(每分鐘 0.5%%,10 分鐘共 5%%)→ 不會被當成加水\n");
    reset_all();
    put_down(50, 5000);
    for (int i = 1; i <= 10; i++) {
        put_down(50 + 0.5f * i, 60000);
    }
    CHECK(s_n_events == 0 && baseline_is(55));

    printf("水位衝高不到 8 秒又回來(有人碰到感應區)→ 不記\n");
    reset_all();
    put_down(50, 5000);
    put_down(60, 3000);
    put_down(50, 60000);
    CHECK(refill_total() == 0);

    printf("倒完水 3 秒就拿起來喝一口(還沒滿 8 秒)→ 拿起時補記加水,喝的那一口照常另外記,不互相抵銷\n");
    reset_all();
    put_down(30, 5000);
    uint32_t poured = s_now;
    put_down(45, 3000);
    CHECK(s_n_events == 0);
    sip(45);
    CHECK(s_n_events == 1 && s_events[0].kind == CUP_EVT_REFILL && s_events[0].stationary);
    CHECK(near(s_events[0].before_pct, 30) && near(s_events[0].after_pct, 45));
    CHECK(s_events[0].start_ms == poured);
    put_down(41, 5000);
    CHECK(s_n_events == 2 && s_events[1].kind == CUP_EVT_DRINK && s_events[1].saw_drinking);
    CHECK(near(s_events[1].before_pct, 45) && near(s_events[1].after_pct, 41));
    put_down(41, 60000);
    CHECK(s_n_events == 2);

    printf("水位跳高不到 2 秒杯子就被拿起(手碰到感應區)→ 不會被當成加水\n");
    reset_all();
    put_down(50, 5000);
    put_down(56, 1000);
    sip(56);
    put_down(50, 5000);
    CHECK(refill_total() == 0);

    printf("倒水進去之後馬上用吸管喝 → 加水記到最高的水位,喝水另外記一筆,兩筆相接\n");
    reset_all();
    put_down(30, 5000);
    put_down(45, 3000);
    put_down(41, 30000);
    CHECK(s_n_events == 2);
    CHECK(s_events[0].kind == CUP_EVT_REFILL);
    CHECK(near(s_events[0].before_pct, 30) && near(s_events[0].after_pct, 45));
    CHECK(s_events[1].kind == CUP_EVT_DRINK);
    CHECK(near(s_events[1].before_pct, 45) && near(s_events[1].after_pct, 41));

    printf("休眠中倒水進去 → 連續 3 筆(約 10 秒)後記一筆加水\n");
    reset_all();
    put_down(30, 5000);
    doze_sample(30);
    doze_sample(50);
    doze_sample(50);
    CHECK(s_n_events == 0);
    doze_sample(50);
    CHECK(s_n_events == 1 && s_events[0].kind == CUP_EVT_REFILL && s_events[0].stationary);
    CHECK(near(s_events[0].before_pct, 30) && near(s_events[0].after_pct, 50));
    doze_sample(50);
    doze_sample(50);
    CHECK(s_n_events == 1);

    printf("休眠中倒水進去、量到 2 筆之後被拿起來(還沒滿 3 筆)→ 喚醒時補記加水\n");
    reset_all();
    put_down(30, 5000);
    doze_sample(30);
    doze_sample(50);
    doze_sample(50);
    CHECK(s_n_events == 0);
    cup_event_wake(s_now);
    run(CUP_POSE_UPRIGHT, true, 50, 1000);
    CHECK(s_n_events == 1 && s_events[0].kind == CUP_EVT_REFILL);
    CHECK(near(s_events[0].before_pct, 30) && near(s_events[0].after_pct, 50));
    put_down(50, 5000);
    CHECK(s_n_events == 1);

    printf("休眠中喚醒後杯子馬上又直立靜止 → 補記的加水照樣送出\n");
    reset_all();
    put_down(30, 5000);
    doze_sample(30);
    doze_sample(50);
    doze_sample(50);
    cup_event_wake(s_now);
    put_down(50, 5000);
    CHECK(s_n_events == 1 && s_events[0].kind == CUP_EVT_REFILL);
    CHECK(near(s_events[0].before_pct, 30) && near(s_events[0].after_pct, 50));

    printf("休眠中只量到 1 筆就被拿起來 → 來不及確認,不記加水;之後喝的照常從倒完的水位算\n");
    reset_all();
    put_down(30, 5000);
    doze_sample(30);
    doze_sample(50);
    cup_event_wake(s_now);
    sip(50);
    put_down(45, 5000);
    CHECK(s_n_events == 1 && s_events[0].kind == CUP_EVT_DRINK);
    CHECK(near(s_events[0].before_pct, 50) && near(s_events[0].after_pct, 45));
}

static void t_static_and_lift(void)
{
    printf("直立不動已經產生事件，之後拿起來喝 → 從新的基準算，不重複計算\n");
    reset_all();
    put_down(60, 5000);
    put_down(55, STATIC_HOLD_MS + 100);
    CHECK(s_n_events == 1);
    sip(55);
    put_down(50, 5000);
    CHECK(s_n_events == 2);
    CHECK(s_events[0].stationary && !s_events[0].saw_drinking);
    CHECK(!s_events[1].stationary && s_events[1].saw_drinking);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 55));
    CHECK(near(s_events[1].before_pct, 55) && near(s_events[1].after_pct, 50));
    put_down(50, 60000);
    CHECK(s_n_events == 2);

    printf("小口喝了 2%%（還沒到門檻）就拿起來喝 → 一筆，從原本的基準算，不漏\n");
    reset_all();
    put_down(60, 5000);
    put_down(58, 20000);
    CHECK(s_n_events == 0);
    sip(58);
    put_down(54, 5000);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_DRINK && s_events[0].saw_drinking && !s_events[0].stationary);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 54));
    put_down(54, 60000);
    CHECK(s_n_events == 1 && baseline_is(54));

    printf("下降 4%% 才 3 秒（還沒確認）杯子被碰了一下 → 由放穩後的結算算一筆，之後不重複\n");
    reset_all();
    put_down(60, 5000);
    put_down(56, 3000);
    CHECK(s_n_events == 0);
    uint32_t bump_at = s_now;
    run(CUP_POSE_UPRIGHT, true, 56, 1000);
    put_down(56, 5000);
    CHECK(s_n_events == 1);
    CHECK(s_events[0].kind == CUP_EVT_DRINK && !s_events[0].saw_drinking && !s_events[0].stationary);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 56));
    CHECK(s_events[0].start_ms == bump_at);
    put_down(56, 60000);
    CHECK(s_n_events == 1 && baseline_is(56));

    printf("拿起來喝完放回，水位之後又慢慢降 4%%（例如杯壁水膜流下來）→ 兩筆相接，總和等於實際下降\n");
    reset_all();
    put_down(80, 5000);
    sip(80);
    put_down(74, 5000);
    CHECK(s_n_events == 1);
    put_down(70, STATIC_HOLD_MS + 100);
    CHECK(s_n_events == 2);
    CHECK(near(s_events[0].before_pct, 80) && near(s_events[0].after_pct, 74));
    CHECK(near(s_events[1].before_pct, 74) && near(s_events[1].after_pct, 70));
}

static void t_doze_static(void)
{
    printf("休眠中直立不動下降，連續 3 筆（每 5 秒一筆）→ 一筆喝水；喚醒後從新的基準算\n");
    reset_all();
    put_down(60, 31000);
    doze_sample(60);
    doze_sample(60);
    uint32_t at = s_now;
    doze_sample(56);
    doze_sample(56);
    CHECK(s_n_events == 0 && baseline_is(60));
    doze_sample(56);
    CHECK(s_n_events == 1);
    cup_event_t *e = &s_events[0];
    CHECK(e->kind == CUP_EVT_DRINK && e->stationary && !e->saw_drinking && !e->too_long);
    CHECK(near(e->before_pct, 60) && near(e->after_pct, 56));
    CHECK(e->start_ms == at && e->end_ms == at + 2 * DOZE_WATER_MS);
    CHECK(baseline_is(56) && !cup_event_in_episode());
    doze_sample(56);
    doze_sample(56);
    doze_sample(56);
    CHECK(s_n_events == 1);
    cup_event_wake(s_now);
    sip(56);
    put_down(50, 5000);
    CHECK(s_n_events == 2);
    CHECK(near(s_events[1].before_pct, 56) && near(s_events[1].after_pct, 50) && !s_events[1].stationary);

    printf("休眠中單筆雜訊、只連續 2 筆 → 不產生事件、基準不變\n");
    reset_all();
    put_down(60, 31000);
    doze_sample(60);
    doze_sample(50);
    doze_sample(60);
    doze_sample(55);
    doze_sample(55);
    doze_sample(60);
    CHECK(s_n_events == 0 && baseline_is(60));

    printf("休眠中小口累積到 3%% → 一筆\n");
    reset_all();
    put_down(60, 31000);
    doze_sample(60);
    doze_sample(59);
    doze_sample(59);
    doze_sample(58);
    doze_sample(58);
    doze_sample(57);
    doze_sample(57);
    CHECK(s_n_events == 0 && baseline_is(60));
    doze_sample(57);
    CHECK(s_n_events == 1 && near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 57));

    printf("休眠中下降還沒確認就被拿起 → 喚醒後放穩結算一筆，之後不重複\n");
    reset_all();
    put_down(60, 31000);
    doze_sample(60);
    doze_sample(55);
    doze_sample(55);
    CHECK(s_n_events == 0);
    cup_event_wake(s_now);
    run(CUP_POSE_UPRIGHT, true, 55, 500);
    put_down(55, 5000);
    CHECK(s_n_events == 1);
    CHECK(near(s_events[0].before_pct, 60) && near(s_events[0].after_pct, 55) && !s_events[0].stationary);
    put_down(55, 60000);
    CHECK(s_n_events == 1);

    printf("筆數條件：時間夠久但只有 2 筆 → 還不算，第 3 筆才算\n");
    reset_all();
    put_down(60, 31000);
    doze_water(55);
    s_now += 60000;
    doze_water(55);
    CHECK(s_n_events == 0);
    s_now += 60000;
    doze_water(55);
    CHECK(s_n_events == 1);

    printf("還沒有基準時，休眠中的讀值只建立基準\n");
    reset_all();
    doze_sample(50);
    CHECK(s_n_events == 0 && baseline_is(50));
}
#else
static void t_static_disabled(void)
{
    printf("EVT_STATIC_DROP_ENABLE=0：直立不動下降只更新基準（0.3.0 的行為），不產生事件\n");
    reset_all();
    put_down(60, 5000);
    put_down(55, 60000);
    CHECK(s_n_events == 0 && baseline_is(55));
    reset_all();
    put_down(60, 31000);
    doze_sample(60);
    doze_sample(50);
    doze_sample(50);
    doze_sample(50);
    doze_sample(50);
    CHECK(s_n_events == 0 && baseline_is(50));
    // 拿起來喝照常
    cup_event_wake(s_now);
    sip(50);
    put_down(45, 5000);
    CHECK(s_n_events == 1 && near(s_events[0].before_pct, 50) && near(s_events[0].after_pct, 45));
}
#endif

static void t_status(void)
{
    printf("Device Status：10 bytes，位址是顯示順序（最高位元組在前）\n");
    // NimBLE 給的位址是最低位元組在前：58:E6:C5:12:E0:CE → CE E0 12 C5 E6 58
    const uint8_t addr_le[6] = { 0xCE, 0xE0, 0x12, 0xC5, 0xE6, 0x58 };
    const uint8_t want[CUP_STATUS_LEN] = { 87, 0, 4, 0, 0x58, 0xE6, 0xC5, 0x12, 0xE0, 0xCE };
    uint8_t out[CUP_STATUS_LEN];
    for (int i = 0; i < CUP_STATUS_LEN; i++) {
        out[i] = 0xAA;
    }
    CHECK(cup_status_encode(87, 0, 4, 0, addr_le, out) == 10);
    for (int i = 0; i < CUP_STATUS_LEN; i++) {
        CHECK(out[i] == want[i]);
    }

    printf("Device Status：位址還沒準備好 → 只有 4 bytes（不送全 0 的假位址）\n");
    for (int i = 0; i < CUP_STATUS_LEN; i++) {
        out[i] = 0xAA;
    }
    CHECK(cup_status_encode(0xFF, 0, 4, 0, NULL, out) == 4);
    CHECK(out[0] == 0xFF && out[1] == 0 && out[2] == 4 && out[3] == 0);
    for (int i = 4; i < CUP_STATUS_LEN; i++) {
        CHECK(out[i] == 0xAA);   // 後面不動
    }
}

static void t_calibration(void)
{
    printf("水位校正值：預設、換成新值、拒絕不合理的值\n");
    uint16_t e, f, de, df;
    cup_water_default_calibration(&de, &df);
    cup_water_get_calibration(&e, &f);
    CHECK(e == de && f == df);
    CHECK(near(cup_water_e0_to_pct((float)de), 0.0f));
    CHECK(near(cup_water_e0_to_pct((float)df), 100.0f));

    CHECK(cup_water_set_calibration(500, 300));
    cup_water_get_calibration(&e, &f);
    CHECK(e == 500 && f == 300);
    CHECK(near(cup_water_e0_to_pct(400.0f), 50.0f));
    CHECK(near(cup_water_e0_to_pct(520.0f), 0.0f));     // 比空杯還高 → 截成 0
    CHECK(near(cup_water_e0_to_pct(250.0f), 100.0f));   // 比滿杯還低 → 截成 100

    // 已經有的水位馬上用新的校正值換算（存的是讀值，不是 %）
    for (int i = 0; i < 10; i++) {
        cup_water_add_sample(400, true);
    }
    float pct;
    CHECK(cup_water_get(&pct) && near(pct, 50.0f));
    CHECK(cup_water_set_calibration(450, 350));
    CHECK(cup_water_get(&pct) && near(pct, 50.0f));
    CHECK(cup_water_set_calibration(400, 300));
    CHECK(cup_water_get(&pct) && near(pct, 0.0f));

    CHECK(!cup_water_set_calibration(300, 400));    // 滿杯 ≥ 空杯
    CHECK(!cup_water_set_calibration(400, 400));
    CHECK(!cup_water_set_calibration(400, 390));    // 差距太小
    CHECK(!cup_water_set_calibration(1100, 300));   // 超過 10 bits
    cup_water_get_calibration(&e, &f);
    CHECK(e == 400 && f == 300);                    // 不合理的值不會改掉原本的

    CHECK(cup_water_set_calibration(de, df));       // 還原，免得影響其他測試
}

int main(void)
{
    t_first_settle_only_baseline();
    t_one_sip();
    t_continuous_sips();
    t_refill();
    t_drink_then_refill();
    t_drop_without_drinking();
    t_noise();
    t_static_drift();
    t_too_long();
    t_doze();
    t_encode();
#ifndef TEST_STATIC_DISABLED
    t_static_drop();
    t_static_noise();
    t_static_with_noise();
    t_static_small_sips();
    t_static_rise();
    t_static_refill();
    t_static_and_lift();
    t_doze_static();
    t_small_drop_accumulate();
#else
    t_static_disabled();
#endif
    t_status();
    t_calibration();
    if (s_fail) {
        printf("\n%d 項失敗（共 %d 項檢查）\n", s_fail, s_checks);
        return 1;
    }
    printf("\n全部通過（%d 項檢查）\n", s_checks);
    return 0;
}
