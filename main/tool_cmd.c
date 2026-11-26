#include "tool_cmd.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "ble_gatt_svr.h"

/* ========================================================================== */
/*  可調參數                                                                    */
/* ========================================================================== */
#define TOOL_KEEPAWAKE_MS       60000   // 最後一個指令之後多久內不休眠（工具連著時每 20 秒送一次 PING）
#define TOOL_LIVE_EVERY_MS      500     // LIVE 1 時多久送一筆 @LIVE
#define TOOL_MEASURE_MIN_MS     1000
#define TOOL_MEASURE_MAX_MS     30000
#define TOOL_FRESH_WAIT_MS      3000    // MEASURE 開始前最多等多久拿到新資料（休眠中要先醒來）
#define TOOL_APPLY_WAIT_MS      2000    // 新校正值最多等多久讓主迴圈套用
#define TOOL_LINE_MAX           64
#define TOOL_TASK_STACK         4096

#define NVS_NS                  "cupcal"
#define KEY_EMPTY               "e0_empty"
#define KEY_FULL                "e0_full"

/* ========================================================================== */
/*  狀態                                                                        */
/* ========================================================================== */
// 主迴圈（tool_cmd_feed）和背景工作共用，用 spinlock 保護
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static tool_sample_t s_sample;
static uint32_t s_sample_seq;           // 每 feed 一次加 1，用來判斷「有新資料」
static bool s_pending_cal;              // 工具寫入的新校正值，等主迴圈套用
static uint16_t s_pending_empty, s_pending_full;
static uint32_t s_applied_seq;          // 主迴圈每套用一次加 1

static volatile uint32_t s_last_cmd_ms;   // 32 bits：RISC-V 上一次讀寫完成，兩個 task 不會讀到一半
static volatile bool s_had_cmd;
static volatile bool s_live;
static bool s_cal_from_nvs;             // 目前的校正值是 NVS 來的（不是預設值）

/* ========================================================================== */
/*  小工具                                                                      */
/* ========================================================================== */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// @ 開頭的回覆一次印完整行，避免被主迴圈的輸出切開
static void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void reply(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    flockfile(stdout);
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static const char *pose_word(cup_pose_t p)
{
    switch (p) {
    case CUP_POSE_UPRIGHT:  return "upright";
    case CUP_POSE_TILTED:   return "tilted";
    case CUP_POSE_DRINKING: return "drinking";
    case CUP_POSE_LYING:    return "lying";
    default:                return "unknown";
    }
}

static void take_sample(tool_sample_t *out, uint32_t *seq)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_sample;
    *seq = s_sample_seq;
    portEXIT_CRITICAL(&s_lock);
}

static void reply_cal(void)
{
    uint16_t e, f, de, df;
    cup_water_get_calibration(&e, &f);
    cup_water_default_calibration(&de, &df);
    reply("@CAL empty=%u full=%u src=%s def_empty=%u def_full=%u",
          (unsigned)e, (unsigned)f, s_cal_from_nvs ? "nvs" : "default", (unsigned)de, (unsigned)df);
}

// 把新校正值交給主迴圈套用，等它套用完才回覆（主迴圈在休眠時最慢約 250 ms 醒來）
static bool apply_cal(uint16_t empty, uint16_t full)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t before = s_applied_seq;
    s_pending_empty = empty;
    s_pending_full = full;
    s_pending_cal = true;
    portEXIT_CRITICAL(&s_lock);
    uint32_t start = now_ms();
    while (now_ms() - start < TOOL_APPLY_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
        portENTER_CRITICAL(&s_lock);
        bool done = s_applied_seq != before;
        portEXIT_CRITICAL(&s_lock);
        if (done) {
            return true;
        }
    }
    return false;
}

/* ========================================================================== */
/*  NVS                                                                         */
/* ========================================================================== */
static esp_err_t nvs_save_cal(uint16_t empty, uint16_t full)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(h, KEY_EMPTY, empty);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, KEY_FULL, full);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_clear_cal(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void load_cal(void)
{
    nvs_handle_t h;
    uint16_t e = 0, f = 0;
    bool have = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        have = nvs_get_u16(h, KEY_EMPTY, &e) == ESP_OK && nvs_get_u16(h, KEY_FULL, &f) == ESP_OK;
        nvs_close(h);
    }
    uint16_t de, df;
    cup_water_default_calibration(&de, &df);
    if (have && cup_water_set_calibration(e, f)) {
        s_cal_from_nvs = true;
        printf("[校正] 使用 flash 裡的校正值：空杯 %u、滿杯 %u（程式預設 %u／%u）\n",
               (unsigned)e, (unsigned)f, (unsigned)de, (unsigned)df);
    } else {
        if (have) {
            printf("[校正] flash 裡的校正值不合理（空杯 %u、滿杯 %u），改用預設值\n", (unsigned)e, (unsigned)f);
        }
        printf("[校正] 使用程式預設的校正值：空杯 %u、滿杯 %u（可以用 tools/cup_tool.py 重新校正）\n",
               (unsigned)de, (unsigned)df);
    }
}

/* ========================================================================== */
/*  指令                                                                        */
/* ========================================================================== */
static void cmd_measure(uint32_t ms)
{
    if (ms < TOOL_MEASURE_MIN_MS || ms > TOOL_MEASURE_MAX_MS) {
        reply("@ERR MEASURE 的時間要在 %d～%d 毫秒", TOOL_MEASURE_MIN_MS, TOOL_MEASURE_MAX_MS);
        return;
    }
    // 先等一筆新資料（休眠中收到指令，主迴圈要先醒來）
    tool_sample_t s;
    uint32_t seq0, seq;
    take_sample(&s, &seq0);
    uint32_t start = now_ms();
    do {
        vTaskDelay(pdMS_TO_TICKS(50));
        take_sample(&s, &seq);
    } while (seq == seq0 && now_ms() - start < TOOL_FRESH_WAIT_MS);
    if (seq == seq0) {
        reply("@ERR 沒有即時資料（水杯還沒進入即時量測？）");
        return;
    }

    uint32_t n = 0, n_e = 0, still = 0;
    uint32_t sum0 = 0, sum1 = 0;
    uint16_t min0 = UINT16_MAX, max0 = 0, min1 = UINT16_MAX, max1 = 0;
    uint32_t last_seq = seq - 1;
    start = now_ms();
    while (now_ms() - start < ms) {
        s_last_cmd_ms = now_ms();   // 量測中保持清醒
        take_sample(&s, &seq);
        if (seq != last_seq) {
            last_seq = seq;
            n++;
            if (s.have_e) {
                n_e++;
                sum0 += s.e0;
                sum1 += s.e1;
                if (s.e0 < min0) min0 = s.e0;
                if (s.e0 > max0) max0 = s.e0;
                if (s.e1 < min1) min1 = s.e1;
                if (s.e1 > max1) max1 = s.e1;
            }
            if (s.have_motion && s.pose == CUP_POSE_UPRIGHT && !s.moving) {
                still++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (n_e == 0) {
        reply("@ERR 量測期間讀不到 MPR121（電極0）");
        return;
    }
    float avg0 = (float)sum0 / n_e;
    float avg1 = (float)sum1 / n_e;
    reply("@MEAS n=%" PRIu32 " e0=%.1f e0min=%u e0max=%u e1=%.1f e1min=%u e1max=%u still=%" PRIu32 " pct=%.1f",
          n_e, avg0, (unsigned)min0, (unsigned)max0, avg1, (unsigned)min1, (unsigned)max1, still,
          cup_water_e0_to_pct(avg0));
}

static void cmd_cal_set(const char *args)
{
    char *end;
    long e = strtol(args, &end, 10);
    long f = strtol(end, &end, 10);
    if (end == args || e < 0 || f < 0 || e > UINT16_MAX || f > UINT16_MAX) {
        reply("@ERR 用法：CAL SET <空杯讀值> <滿杯讀值>");
        return;
    }
    if (!cup_water_calibration_valid((uint16_t)e, (uint16_t)f)) {
        reply("@ERR 校正值不合理（滿杯要比空杯小至少 20、都在 0～1023）：空杯 %ld、滿杯 %ld", e, f);
        return;
    }
    esp_err_t err = nvs_save_cal((uint16_t)e, (uint16_t)f);
    if (err != ESP_OK) {
        reply("@ERR 寫入 flash 失敗（%s）", esp_err_to_name(err));
        return;
    }
    if (!apply_cal((uint16_t)e, (uint16_t)f)) {
        reply("@ERR 已存進 flash，但主迴圈沒有回應（重開機後會生效）");
        return;
    }
    s_cal_from_nvs = true;
    printf("[校正] 新的校正值已存進 flash 並生效：空杯 %ld、滿杯 %ld\n", e, f);
    reply_cal();
}

static void cmd_cal_reset(void)
{
    esp_err_t err = nvs_clear_cal();
    if (err != ESP_OK) {
        reply("@ERR 清除 flash 的校正值失敗（%s）", esp_err_to_name(err));
        return;
    }
    uint16_t de, df;
    cup_water_default_calibration(&de, &df);
    if (!apply_cal(de, df)) {
        reply("@ERR 已清除 flash，但主迴圈沒有回應（重開機後會生效）");
        return;
    }
    s_cal_from_nvs = false;
    printf("[校正] 已清除 flash 的校正值，回到程式預設：空杯 %u、滿杯 %u\n", (unsigned)de, (unsigned)df);
    reply_cal();
}

static void cmd_help(void)
{
    flockfile(stdout);
    printf("校正工具指令（一行一個，Enter 結束）：\n"
           "  PING                 確認連線、看韌體版本與校正值\n"
           "  CAL?                 看目前的校正值\n"
           "  CAL SET <空杯> <滿杯>  寫入新的校正值（存 flash、馬上生效）\n"
           "  CAL RESET            清掉 flash 的校正值，回到程式預設\n"
           "  MEASURE <毫秒>        量一段時間的電極平均（1000～30000）\n"
           "  LIVE 1 / LIVE 0      每 0.5 秒送即時讀值 / 停止\n");
    fflush(stdout);
    funlockfile(stdout);
}

static void handle_line(char *line)
{
    // 去掉前後空白、轉大寫比對指令
    while (*line == ' ' || *line == '\t') line++;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = '\0';
    if (len == 0) {
        return;
    }
    for (char *p = line; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    }
    s_last_cmd_ms = now_ms();
    s_had_cmd = true;

    if (strcmp(line, "PING") == 0) {
        uint16_t e, f;
        cup_water_get_calibration(&e, &f);
        reply("@PONG fw=%d.%d.%d name=%s empty=%u full=%u src=%s", CUP_FW_MAJOR, CUP_FW_MINOR, CUP_FW_PATCH,
              ble_cup_name()[0] ? ble_cup_name() : "-", (unsigned)e, (unsigned)f, s_cal_from_nvs ? "nvs" : "default");
    } else if (strcmp(line, "CAL?") == 0) {
        reply_cal();
    } else if (strncmp(line, "CAL SET", 7) == 0) {
        cmd_cal_set(line + 7);
    } else if (strcmp(line, "CAL RESET") == 0) {
        cmd_cal_reset();
    } else if (strncmp(line, "MEASURE", 7) == 0) {
        cmd_measure((uint32_t)strtoul(line + 7, NULL, 10));
    } else if (strcmp(line, "LIVE 1") == 0) {
        s_live = true;
        reply("@OK LIVE 1");
    } else if (strcmp(line, "LIVE 0") == 0) {
        s_live = false;
        reply("@OK LIVE 0");
    } else if (strcmp(line, "HELP") == 0) {
        cmd_help();
    } else {
        reply("@ERR 不認得的指令：%s（輸入 HELP 看說明）", line);
    }
}

static void send_live(void)
{
    tool_sample_t s;
    uint32_t seq;
    take_sample(&s, &seq);
    if (!s.have_e) {
        return;
    }
    reply("@LIVE e0=%u e1=%u pct=%.1f pose=%s moving=%d", (unsigned)s.e0, (unsigned)s.e1,
          cup_water_e0_to_pct((float)s.e0), s.have_motion ? pose_word(s.pose) : "unknown",
          s.have_motion && s.moving ? 1 : 0);
}

static void tool_task(void *arg)
{
    char line[TOOL_LINE_MAX];
    size_t n = 0;
    bool overflow = false;
    uint32_t last_live = now_ms();
    for (;;) {
        uint8_t buf[32];
        int r = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < r; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (!overflow && n > 0) {
                    line[n] = '\0';
                    handle_line(line);
                } else if (overflow) {
                    reply("@ERR 指令太長");
                }
                n = 0;
                overflow = false;
            } else if (n < sizeof(line) - 1) {
                line[n++] = c;
            } else {
                overflow = true;
            }
        }
        if (s_live && tool_cmd_active() && now_ms() - last_live >= TOOL_LIVE_EVERY_MS) {
            last_live = now_ms();
            send_live();
        }
        if (s_live && !tool_cmd_active()) {
            s_live = false;   // 工具不見了（拔線、關掉）就停止
        }
    }
}

/* ========================================================================== */
/*  對外                                                                        */
/* ========================================================================== */
void tool_cmd_init(void)
{
    load_cal();

    // USB Serial/JTAG 驅動：才收得到電腦送來的指令。printf 也改走驅動
    // （沒接電腦或送不出去時最多等 50 ms 就丟掉，不會卡住；驅動本身不擋自動淺眠）
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 1024;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        printf("[校正] USB 序列埠驅動安裝失敗（%s），校正工具不能用\n", esp_err_to_name(err));
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(tool_task, "tool_cmd", TOOL_TASK_STACK, NULL, 2, NULL) != pdPASS) {
        printf("[校正] 建立指令工作失敗，校正工具不能用\n");
    }
}

void tool_cmd_feed(const tool_sample_t *s)
{
    bool apply = false;
    uint16_t e = 0, f = 0;
    portENTER_CRITICAL(&s_lock);
    s_sample = *s;
    s_sample_seq++;
    if (s_pending_cal) {
        apply = true;
        e = s_pending_empty;
        f = s_pending_full;
        s_pending_cal = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (apply) {
        (void)cup_water_set_calibration(e, f);   // 工具那邊驗證過了
        portENTER_CRITICAL(&s_lock);
        s_applied_seq++;
        portEXIT_CRITICAL(&s_lock);
    }
}

bool tool_cmd_active(void)
{
    return s_had_cmd && now_ms() - s_last_cmd_ms < TOOL_KEEPAWAKE_MS;
}
