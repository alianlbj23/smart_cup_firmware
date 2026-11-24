#include "drink_log.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_rtc_time.h"
#include "nvs.h"

#include "ble_gatt_svr.h"

/* ========================================================================== */
/*  可調參數                                                                    */
/* ========================================================================== */
#ifndef EVT_STORE_MAX
#define EVT_STORE_MAX           256     // flash 最多存幾筆未確認的事件，滿了丟最舊的（要是 2 的次方、≤ 1024）
#endif
#define EVT_ACK_TIMEOUT_MS      2000    // 送出後等 ACK 多久，逾時重送
#define EVT_ACK_TIMEOUT_DOZE_MS 6000    // 休眠中連線間隔 0.5～1 秒，App 回 ACK 要好幾個連線事件，放寬
#define EVT_MAX_RETRY           3       // 同一筆最多重送 3 次，還沒 ACK 就暫停補傳到下次連線（舊 App 不會回 3-byte ACK）

#define NVS_NS                  "drinklog"
#define SEQ_MOD                 (CUP_EVT_SEQ_MASK + 1)   // seq 11 bits：0～2047

// 紀錄存在 slot = seq % EVT_STORE_MAX；未確認的 seq 範圍 < EVT_STORE_MAX，slot 不會撞
_Static_assert((EVT_STORE_MAX & (EVT_STORE_MAX - 1)) == 0 && EVT_STORE_MAX <= SEQ_MOD / 2,
               "EVT_STORE_MAX 要是 2 的次方，而且不超過 1024");

/*
 * NVS（namespace "drinklog"）：
 *   boot_id  u32  每次冷開機 +1（深度睡眠醒來不加）
 *   next_seq u16  下一筆事件的 seq
 *   tail     u16  最舊一筆未確認事件的 seq（== next_seq 表示沒有）
 *   eNNN     u64  slot NNN 的事件，一筆只佔一個 NVS entry（32 bytes），256 筆約 2 頁
 *
 * 每筆事件（u64）：
 *   bits 31..0 ：和 0x07 封包的 value 一樣（A、B、種類、saw_drinking、too_long、time_unknown、seq）
 *   bits 63..32：時間已知（bit 20 = 0）→ 事件開始的 Unix 秒
 *                時間未知（bit 20 = 1）→ bits 55..32 = 開始時的 RTC 秒數（上電起算，24 bits 約 194 天），
 *                                         bits 63..56 = boot_id 低 8 bits
 */
#define REC_VALUE(r)     ((uint32_t)(r))
#define REC_HI(r)        ((uint32_t)((r) >> 32))
#define REC_SEQ(r)       ((uint16_t)((REC_VALUE(r) >> 21) & CUP_EVT_SEQ_MASK))
#define REC_UNKNOWN(r)   ((REC_VALUE(r) & CUP_EVT_TIME_UNKNOWN) != 0)
#define MONO_MASK        0xFFFFFFu

/* ========================================================================== */
/*  狀態                                                                        */
/* ========================================================================== */
static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static uint32_t s_boot_id;
static uint16_t s_next_seq, s_tail;
static uint64_t s_rec[EVT_STORE_MAX];
static bool s_live[EVT_STORE_MAX];
static bool s_stale[EVT_STORE_MAX];   // 之前開機留下、時間未知的事件：換算不了，送出時帶 time_unknown
static uint32_t s_live_n;

// 對時過了沒（time(NULL) 是不是 Unix 秒）。系統時間在深度睡眠時會保留，所以這個旗標放 RTC 記憶體
static RTC_DATA_ATTR bool s_time_valid;

// 補傳
static uint32_t s_sub_seen;           // 上次看到的 ble_cup_subscribe_count()
static uint32_t s_sync_seen;          // 上次看到的 ble_cup_time_sync_count()
static bool s_wait_ready;             // 剛訂閱，等 SUBSCRIBE_RESEND_MS 再開始送
static uint32_t s_ready_at;
static bool s_paused;                 // 這次連線重送太多次沒 ACK，先暫停
static int s_inflight = -1;           // 在途（送出等 ACK）的 slot，-1 = 沒有
static uint32_t s_sent_at;
static int s_tries;                   // 在途這筆送了幾次

/* ========================================================================== */
/*  小工具                                                                      */
/* ========================================================================== */
uint32_t drink_log_clock_ms(void)
{
    return (uint32_t)(esp_rtc_get_time_us() / 1000);
}

static uint32_t clock_s(void)
{
    return (uint32_t)(esp_rtc_get_time_us() / 1000000);
}

static uint16_t seq_dist(uint16_t from, uint16_t to)
{
    return (uint16_t)((to - from) & CUP_EVT_SEQ_MASK);
}

static int slot_of(uint16_t seq)
{
    return seq % EVT_STORE_MAX;
}

static void slot_key(int slot, char key[16])
{
    snprintf(key, 16, "e%03d", slot);
}

static void nvs_check(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        printf("[事件] 寫入 flash 失敗（%s：%s）\n", what, esp_err_to_name(err));
    }
}

static void nvs_put_rec(int slot)
{
    if (!s_nvs_ok) {
        return;
    }
    char key[16];
    slot_key(slot, key);
    nvs_check(nvs_set_u64(s_nvs, key, s_rec[slot]), key);
}

static void nvs_del_rec(int slot)
{
    if (!s_nvs_ok) {
        return;
    }
    char key[16];
    slot_key(slot, key);
    esp_err_t err = nvs_erase_key(s_nvs, key);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_check(err, key);
    }
}

static void nvs_put_u16(const char *key, uint16_t v)
{
    if (s_nvs_ok) {
        nvs_check(nvs_set_u16(s_nvs, key, v), key);
    }
}

static void nvs_flush(void)
{
    if (s_nvs_ok) {
        nvs_check(nvs_commit(s_nvs), "commit");
    }
}

static const char *kind_label(uint32_t value)
{
    switch ((value >> 16) & 0x3) {
    case CUP_EVT_DRINK:  return "喝水";
    case CUP_EVT_REFILL: return "加水";
    default:             return "未知";
    }
}

// 把 tail 往前推過已經刪掉的洞
static void advance_tail(void)
{
    uint16_t old = s_tail;
    while (s_tail != s_next_seq && !s_live[slot_of(s_tail)]) {
        s_tail = (s_tail + 1) & CUP_EVT_SEQ_MASK;
    }
    if (s_tail != old) {
        nvs_put_u16("tail", s_tail);
    }
}

static void remove_slot(int slot)
{
    s_live[slot] = false;
    s_stale[slot] = false;
    s_live_n--;
    nvs_del_rec(slot);
    if (s_inflight == slot) {
        s_inflight = -1;
        s_tries = 0;
    }
}

/* ========================================================================== */
/*  初始化                                                                      */
/* ========================================================================== */
void drink_log_init(bool woke_from_deep_sleep)
{
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    s_nvs_ok = (err == ESP_OK);
    if (!s_nvs_ok) {
        printf("[事件] 打不開 flash（%s），事件只存在記憶體，斷電會不見\n", esp_err_to_name(err));
        return;
    }

    uint32_t boot_id = 0;
    (void)nvs_get_u32(s_nvs, "boot_id", &boot_id);
    if (!woke_from_deep_sleep) {
        boot_id++;
        nvs_check(nvs_set_u32(s_nvs, "boot_id", boot_id), "boot_id");
    }
    s_boot_id = boot_id;

    uint16_t next = 0, tail;
    (void)nvs_get_u16(s_nvs, "next_seq", &next);
    next &= CUP_EVT_SEQ_MASK;
    if (nvs_get_u16(s_nvs, "tail", &tail) != ESP_OK) {
        tail = next;
    }
    tail &= CUP_EVT_SEQ_MASK;
    if (seq_dist(tail, next) > EVT_STORE_MAX) {   // 不應該發生（改過 EVT_STORE_MAX？）：只看最新的 EVT_STORE_MAX 筆
        tail = (next - EVT_STORE_MAX) & CUP_EVT_SEQ_MASK;
    }
    s_next_seq = next;
    s_tail = tail;

    uint32_t n_unknown = 0;
    for (uint16_t seq = tail; seq != next; seq = (seq + 1) & CUP_EVT_SEQ_MASK) {
        int slot = slot_of(seq);
        char key[16];
        slot_key(slot, key);
        uint64_t r;
        if (nvs_get_u64(s_nvs, key, &r) != ESP_OK || REC_SEQ(r) != seq) {
            continue;   // 已確認刪掉的洞
        }
        s_rec[slot] = r;
        s_live[slot] = true;
        s_live_n++;
        // 冷開機時，之前開機留下的「時間未知」事件都換算不了；深度睡眠醒來還是同一次開機，看 boot_id 決定
        if (REC_UNKNOWN(r)) {
            s_stale[slot] = !woke_from_deep_sleep || (REC_HI(r) >> 24) != (s_boot_id & 0xFF);
            n_unknown++;
        }
    }
    advance_tail();
    nvs_flush();

    printf("[事件] 開機第 %" PRIu32 " 次 · flash 裡有 %" PRIu32 " 筆未確認的事件（其中 %" PRIu32
           " 筆時間未知）· 下一個 seq %u\n", s_boot_id, s_live_n, n_unknown, (unsigned)s_next_seq);
}

/* ========================================================================== */
/*  新事件                                                                      */
/* ========================================================================== */
static void pump(uint32_t now_ms, bool dozing);

void drink_log_add(const cup_event_t *ev)
{
    uint32_t now_ms = drink_log_clock_ms();
    uint32_t ago_s = (uint32_t)(now_ms - ev->start_ms) / 1000;   // 過程開始到現在幾秒

    // 滿了：丟最舊的（seq 範圍要 < EVT_STORE_MAX，slot 才不會撞）
    while (seq_dist(s_tail, s_next_seq) >= EVT_STORE_MAX) {
        int slot = slot_of(s_tail);
        if (s_live[slot]) {
            printf("[事件] 警告：flash 存滿 %d 筆未確認的事件，丟掉最舊的 seq %u（%s）\n",
                   EVT_STORE_MAX, (unsigned)s_tail, kind_label(REC_VALUE(s_rec[slot])));
            remove_slot(slot);
        }
        s_tail = (s_tail + 1) & CUP_EVT_SEQ_MASK;
        advance_tail();
        nvs_put_u16("tail", s_tail);
    }

    uint16_t seq = s_next_seq;
    int slot = slot_of(seq);
    bool unknown = !s_time_valid;
    uint32_t hi;
    if (!unknown) {
        hi = (uint32_t)time(NULL) - ago_s;   // 事件開始的 Unix 秒
    } else {
        uint32_t now_s = clock_s();
        uint32_t start_s = now_s >= ago_s ? now_s - ago_s : 0;
        hi = (start_s & MONO_MASK) | ((s_boot_id & 0xFF) << 24);
    }
    s_rec[slot] = (uint64_t)cup_event_encode(ev, seq, unknown) | ((uint64_t)hi << 32);
    s_live[slot] = true;
    s_stale[slot] = false;
    s_live_n++;
    s_next_seq = (seq + 1) & CUP_EVT_SEQ_MASK;
    if (s_live_n == 1) {
        s_tail = seq;
        nvs_put_u16("tail", s_tail);
    }

    // 先寫 flash 再送
    nvs_put_rec(slot);
    nvs_put_u16("next_seq", s_next_seq);
    nvs_flush();

    printf("[事件] %s %.1f%% → %.1f%%（seq %u%s%s%s，%" PRIu32 " 秒前開始）已存進 flash，未確認 %" PRIu32 " 筆\n",
           ev->kind == CUP_EVT_DRINK ? "喝水" : "加水", ev->before_pct, ev->after_pct, (unsigned)seq,
           ev->saw_drinking ? "，有飲用姿態" : "，沒有飲用姿態",
           ev->too_long ? "，過程太久" : "",
           unknown ? "，還沒對時" : "", ago_s, s_live_n);

    // 新事件給這次連線一次新機會（之前可能因為重送太多次暫停了）
    s_paused = false;
    pump(now_ms, false);
}

/* ========================================================================== */
/*  ACK、對時、補傳                                                             */
/* ========================================================================== */
static void handle_acks(void)
{
    uint16_t seq;
    bool changed = false;
    while (ble_cup_take_event_ack(&seq)) {
        seq &= CUP_EVT_SEQ_MASK;
        int slot = slot_of(seq);
        if (!s_live[slot] || REC_SEQ(s_rec[slot]) != seq) {
            printf("[事件] 收到 seq %u 的 ACK，但沒有這筆（可能已經確認過）\n", (unsigned)seq);
            continue;
        }
        remove_slot(slot);
        changed = true;
        printf("[事件] seq %u 已確認，從 flash 刪除（還有 %" PRIu32 " 筆未確認）\n", (unsigned)seq, s_live_n);
    }
    if (changed) {
        advance_tail();
        nvs_flush();
    }
}

// 剛對時：同一次開機、對時前產生的事件，用「對時當下的 Unix 秒 −（現在 RTC 秒 − 事件 RTC 秒）」換算
static void handle_time_sync(void)
{
    uint32_t n = ble_cup_time_sync_count();
    if (n == s_sync_seen) {
        return;
    }
    s_sync_seen = n;
    s_time_valid = true;

    uint32_t now_unix = (uint32_t)time(NULL);
    uint32_t now_s = clock_s();
    uint32_t fixed = 0;
    for (int slot = 0; slot < EVT_STORE_MAX; slot++) {
        uint64_t r = s_rec[slot];
        if (!s_live[slot] || !REC_UNKNOWN(r) || s_stale[slot]) {
            continue;
        }
        uint32_t ago = (now_s - (REC_HI(r) & MONO_MASK)) & MONO_MASK;   // 24 bits 繞圈也算得對（< 194 天）
        if (ago > now_unix) {
            continue;
        }
        uint32_t value = REC_VALUE(r) & ~CUP_EVT_TIME_UNKNOWN;
        s_rec[slot] = (uint64_t)value | ((uint64_t)(now_unix - ago) << 32);
        nvs_put_rec(slot);
        fixed++;
    }
    if (fixed > 0) {
        nvs_flush();
        printf("[事件] 已對時，%" PRIu32 " 筆對時前的事件換算成實際時間\n", fixed);
    }
}

static void send_slot(int slot, uint32_t now_ms)
{
    uint64_t r = s_rec[slot];
    uint32_t value = REC_VALUE(r);
    uint32_t ts = REC_UNKNOWN(r) ? 0 : REC_HI(r);
    (void)ble_cup_send_drink_event(value, ts);   // 送不出去（mbuf 不夠）就當作送了，逾時會重送
    s_inflight = slot;
    s_sent_at = now_ms;
    s_tries++;
}

static int oldest_slot(void)
{
    for (uint16_t seq = s_tail; seq != s_next_seq; seq = (seq + 1) & CUP_EVT_SEQ_MASK) {
        if (s_live[slot_of(seq)]) {
            return slot_of(seq);
        }
    }
    return -1;
}

static void pump(uint32_t now_ms, bool dozing)
{
    // App 剛訂閱（新連線，或休眠醒來前後重新訂閱）：等 SUBSCRIBE_RESEND_MS 再開始，前一次連線的狀態清掉
    uint32_t sub = ble_cup_subscribe_count();
    if (sub != s_sub_seen) {
        s_sub_seen = sub;
        s_wait_ready = true;
        s_ready_at = now_ms + SUBSCRIBE_RESEND_MS;
        s_paused = false;
        s_inflight = -1;
        s_tries = 0;
        if (s_live_n > 0) {
            printf("[事件] App 已訂閱，%.1f 秒後補傳 %" PRIu32 " 筆未確認的事件\n",
                   SUBSCRIBE_RESEND_MS / 1000.0, s_live_n);
        }
    }
    if (ble_cup_state() != BLE_CUP_SUBSCRIBED) {
        s_inflight = -1;
        s_tries = 0;
        return;
    }
    if (s_wait_ready) {
        if ((int32_t)(now_ms - s_ready_at) < 0) {
            return;
        }
        s_wait_ready = false;
    }

    if (s_inflight >= 0) {
        uint32_t timeout = dozing ? EVT_ACK_TIMEOUT_DOZE_MS : EVT_ACK_TIMEOUT_MS;
        if (now_ms - s_sent_at < timeout) {
            return;
        }
        uint16_t seq = REC_SEQ(s_rec[s_inflight]);
        if (s_tries > EVT_MAX_RETRY) {
            printf("[事件] seq %u 重送 %d 次都沒有 ACK，這次連線先暫停補傳（舊版 App 不會回 3-byte ACK），"
                   "下次連線再送\n", (unsigned)seq, EVT_MAX_RETRY);
            s_paused = true;
            s_inflight = -1;
            s_tries = 0;
            return;
        }
        printf("[事件] seq %u 等不到 ACK，重送（第 %d 次）\n", (unsigned)seq, s_tries);
        send_slot(s_inflight, now_ms);
        return;
    }
    if (s_paused) {
        return;
    }
    int slot = oldest_slot();
    if (slot >= 0) {
        s_tries = 0;
        send_slot(slot, now_ms);
    }
}

void drink_log_poll(bool dozing)
{
    handle_acks();
    handle_time_sync();
    pump(drink_log_clock_ms(), dozing);
}

uint32_t drink_log_pending(void)
{
    return s_live_n;
}
