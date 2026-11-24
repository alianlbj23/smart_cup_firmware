/*
 * drink_log — 喝水／加水事件的儲存與補傳
 *
 *   事件（cup_state.c 偵測）先寫進 NVS，再透過 BLE 0x07 DRINK_EVENT 送給 App；
 *   App 回 3-byte ACK [0x07, seq 低 8 bits, seq 高 3 bits] 後才從 flash 刪除。
 *   斷線時存著，App 重新訂閱 1.5 秒後依序補傳，一次只在途一筆（逾時重送，最多重試 3 次就暫停到下次連線）。
 *   對時前產生的事件：同一次開機內在對時時換算成 Unix 秒；換算不了的送出時帶 time_unknown、timestamp = 0。
 *
 * 所有函式都只在主迴圈（main.c 的 live_loop／doze_loop）呼叫，不需要加鎖；
 * BLE host task 收到的 ACK 由 ble_gatt_svr.c 用 FreeRTOS queue 轉交過來。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cup_state.h"

// 開機時呼叫一次（nvs_flash_init 之後、ble_cup_init 之前）：載入 flash 裡未確認的事件。
// woke_from_deep_sleep = true 時 boot_id 不加 1（同一次開機，對時前的事件之後還能換算時間）
void drink_log_init(bool woke_from_deep_sleep);

// 事件偵測用的時鐘（ms）：RTC 計時器，從上電起算，深度睡眠時也繼續走（uint32 約 49 天繞一圈，只拿來算差值）
uint32_t drink_log_clock_ms(void);

// 新事件：寫進 NVS，有連線且已訂閱就立刻送出
void drink_log_add(const cup_event_t *ev);

// 主迴圈每一輪呼叫（活動中 100 ms、休眠中 250 ms）：處理 ACK、對時換算、補傳與重送。
// dozing = true 時連線間隔較長，ACK 逾時放寬
void drink_log_poll(bool dozing);

// 還沒被 App 確認的事件筆數
uint32_t drink_log_pending(void);
