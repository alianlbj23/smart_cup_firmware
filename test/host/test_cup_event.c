/*
 * cup_state.c 喝水／加水事件偵測的情境測試（在電腦上跑：sh test/host/run.sh）
 *
 * 模擬 main.c live_loop 的節奏：每 100 ms 給一次姿態、移動、水位。
 * 「水位新鮮」照 cup_water_add_sample 的行為模擬：直立且靜止 ≥ 1 秒後才有新讀值。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cup_state.h"

#define TICK_MS 100

static uint32_t s_now;
static uint32_t s_upright_still_since;
static bool s_was_upright_still;
static cup_event_t s_events[16];
static int s_n_events;
static int s_fail;

#define CHECK(cond) do { \
    if (!(cond)) { printf("    失敗：%s（第 %d 行）\n", #cond, __LINE__); s_fail++; } \
} while (0)

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
            if (s_n_events < 16) {
                s_events[s_n_events] = ev;
            }
            s_n_events++;
        }
        s_now += TICK_MS;
    }
}

static void put_down(float pct, uint32_t ms) { run(CUP_POSE_UPRIGHT, false, pct, ms); }

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
    printf("雜訊（變化 < 3%%）→ 不產生事件，基準改成 B\n");
    reset_all();
    put_down(50, 5000);
    sip(50);
    put_down(48, 5000);
    CHECK(s_n_events == 0);
    float b;
    CHECK(cup_event_baseline(&b) && near(b, 48));
    CHECK(!cup_event_in_episode());
    // 下一口和新的基準 48 比
    sip(48);
    put_down(45, 5000);
    CHECK(s_n_events == 1 && near(s_events[0].before_pct, 48));

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

static void t_static_drift(void)
{
    printf("杯子不動時倒水進去 → 只更新基準\n");
    reset_all();
    put_down(30, 5000);
    for (int i = 0; i <= 50; i++) {
        put_down(30 + i, 200);
    }
    put_down(80, 3000);
    CHECK(s_n_events == 0);
    float b;
    CHECK(cup_event_baseline(&b) && near(b, 80));
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
    cup_event_doze_water(60);
    s_now += 5000;
    cup_event_doze_water(75);
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
    cup_event_doze_water(60);
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
    if (s_fail) {
        printf("\n%d 項失敗\n", s_fail);
        return 1;
    }
    printf("\n全部通過\n");
    return 0;
}
