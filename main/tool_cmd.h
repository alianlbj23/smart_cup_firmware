/*
 * tool_cmd — 給電腦校正工具（tools/cup_tool.py）用的 USB 序列埠指令（韌體 0.5.0 起）
 *
 *   水位校正值存在 NVS（namespace "cupcal"），開機時載入；沒存過就用 cup_state.c 的預設值。
 *   電腦送一行文字指令（\n 或 \r 結尾），水杯回一行「@」開頭的結果：
 *     PING                    → @PONG fw=0.5.0 name=SmartCup-XXXX empty=480 full=356 src=nvs
 *     CAL?                    → @CAL empty=480 full=356 src=nvs|default def_empty=480 def_full=356
 *     CAL SET <空杯> <滿杯>   → 存進 NVS、馬上生效，回 @CAL …；不合理的值回 @ERR
 *     CAL RESET               → 清掉 NVS 的校正值，回到預設值，回 @CAL …
 *     MEASURE <毫秒>          → 量一段時間（1000～30000 ms）的電極平均，
 *                               回 @MEAS n= e0= e0min= e0max= e1= e1min= e1max= still= pct=
 *     LIVE 1 / LIVE 0         → 每 0.5 秒送 @LIVE e0= e1= pct= pose= moving=（即時顯示用）
 *     HELP                    → 指令說明（給人看）
 *   錯誤回 @ERR <原因>。
 *
 *   收到任何指令後 TOOL_KEEPAWAKE_MS（60 秒）內不休眠（休眠中會醒來），量測才有即時資料；
 *   這段期間 main.c 不印序列埠表格，避免和 @ 開頭的回覆混在同一行。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cup_state.h"

// 開機時呼叫一次（nvs_flash_init 之後）：從 NVS 載入校正值、安裝 USB 序列埠驅動、開背景工作
void tool_cmd_init(void);

// 一筆即時資料（main.c 的 live_loop 每 100 ms 給一次）
typedef struct {
    bool have_e;          // 這一刻有讀到 MPR121
    uint16_t e0, e1;
    bool have_motion;     // 有 IMU 資料
    cup_pose_t pose;
    bool moving;
} tool_sample_t;

// live_loop 每一輪呼叫：更新即時資料；工具寫入的新校正值也在這裡（主迴圈的 task）套用
void tool_cmd_feed(const tool_sample_t *s);

// 校正工具最近（TOOL_KEEPAWAKE_MS 內）有送指令：不要休眠、不要印表格
bool tool_cmd_active(void);
