/*
 * smart_cup_firmware — 接線測試、水位與姿態量測、BLE 推送
 *
 * 硬體：Seeed Studio XIAO ESP32-C6
 *       + MPR121 電容感測模組（HW-017，I2C 0x5A）
 *       + MPU-9250 / MPU-6500 IMU（I2C 0x68）
 *       + 702030 鋰電池（BAT+ → 200kΩ → D0 → 200kΩ → GND 分壓）
 *
 * 開機後全自動執行，不需要從 monitor 輸入任何東西：
 *   第一部分：每個項目自動測一次，最後印出總結表（通過／失敗，失敗時列出該檢查的線）
 *   第二部分：每 100 ms 讀一次感測器，算水位、姿態，偵測喝水／加水事件，透過 BLE 推送給 App
 *             （ble_gatt_svr.c），序列埠每 0.5 秒印一行
 *   靜止 30 秒進入休眠（淺眠，BLE 連線不斷）；深度睡眠是選項（DEEP_SLEEP_ENABLE，預設關閉）
 *
 * 水位、姿態、喝水／加水事件的計算在 cup_state.c（水位校正值 LEVEL_E0_EMPTY / LEVEL_E0_FULL 也在那裡）；
 * 事件存 flash、補傳給 App 在 drink_log.c。
 * 依 ESP-IDF v6.1 的 API 撰寫（新版 I2C 驅動 driver/i2c_master.h、esp_adc oneshot + 校正、NimBLE）。
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "driver/usb_serial_jtag.h"
#include "nvs_flash.h"

#include "cup_state.h"
#include "ble_gatt_svr.h"
#include "drink_log.h"
#include "tool_cmd.h"

/* ========================================================================== */
/*  腳位（XIAO ESP32-C6 的 Dx 標示 → GPIO 編號）                                */
/* ========================================================================== */
#define PIN_I2C_SDA         GPIO_NUM_22     // D4：I2C SDA（MPR121 與 IMU 共用）
#define PIN_I2C_SCL         GPIO_NUM_23     // D5：I2C SCL（共用）
#define PIN_IMU_INT         GPIO_NUM_1      // D1：IMU INT（高電位有效、推挽輸出）
#define PIN_MPR_IRQ         GPIO_NUM_2      // D2：MPR121 IRQ（低電位有效、開汲極 → 需上拉）
#define BAT_ADC_UNIT        ADC_UNIT_1      // D0 = GPIO0 = ADC1 通道 0
#define BAT_ADC_CHAN        ADC_CHANNEL_0
#define BAT_ADC_ATTEN       ADC_ATTEN_DB_12 // 衰減 12 dB（最大量程）；電池 4.2 V 時 D0 約 2.1 V，在量程內

/* ========================================================================== */
/*  測試參數                                                                    */
/* ========================================================================== */
#define I2C_FREQ_HZ             100000  // 接線測試用 100 kHz，對杜邦線、長線比較寬容
#define I2C_TIMEOUT_MS          100     // 一般讀寫逾時
#define I2C_PROBE_TIMEOUT_MS    50      // 掃描時每個位址的逾時
#define I2C_SCAN_FIRST          0x08
#define I2C_SCAN_LAST           0x77

#define MPR121_TOUCH_TH         12      // 觸碰門檻
#define MPR121_RELEASE_TH       6       // 放開門檻

#define ACC_TOTAL_MIN_G         0.85f   // 靜止時加速度合計的合理範圍
#define ACC_TOTAL_MAX_G         1.15f
#define IMU_INT_MIN             50      // 1 秒內 INT 上升緣次數的合理範圍（設定 100 Hz）
#define IMU_INT_MAX             200
#define BAT_SAMPLES             32      // 電池電壓取樣次數（取平均）
#define BAT_MIN_MV              2800
#define BAT_MAX_MV              4400

// 即時量測的節奏：每 100 ms 讀一次感測器，其他動作是它的倍數
#define SENSE_PERIOD_MS         100
#define ATTITUDE_EVERY          2       // 每 200 ms 送一次 ATTITUDE（5 次/秒）
#define PRINT_EVERY             5       // 每 500 ms 序列埠印一行
#define WATER_EVERY             10      // 每 1 秒送一次 WATER、POSE（姿態改變時會立刻送 POSE）
#define BATTERY_EVERY           100     // 每 10 秒量一次電池
#define LIVE_HEADER_EVERY       20      // 每 20 行重印一次欄位標題

// 休眠（淺眠）：杯子靜止一段時間就進入「休眠」，BLE 連線保持不斷，拿起杯子（IMU wake-on-motion → D1）馬上恢復。
// 休眠中 CPU 大部分時間在自動淺眠（esp_pm），連線間隔拉長到 0.5～1 秒；水位照樣定期量（加水時杯子不一定會動）
#ifndef DOZE_IDLE_MS
#define DOZE_IDLE_MS            30000   // 靜止 30 秒就休眠（從最後一次移動或 App 連上算起，取較晚的）
#endif
#define DOZE_POLL_MS            250     // 休眠中每 250 ms 看一次 D1（IMU 的 INT 是閂鎖的，不會漏掉）
#define DOZE_KEEPALIVE_MS       10000   // 休眠中每 10 秒送一次 STATE，讓 App 知道還在休眠
#define DOZE_WATER_CHECK_MS     5000    // 休眠中每 5 秒量一次水位
#define DOZE_WATER_FORCE_MS     30000   // 水位沒變也每 30 秒送一次
#define DOZE_WATER_DELTA_PCT    1.0f    // 水位變化 ≥ 1% 就立刻送
#define DOZE_BATTERY_MS         60000   // 休眠中每 60 秒量一次電池

// 深度睡眠（預設關閉）：打開後，休眠中「沒有 App 連線」超過 DEEP_SLEEP_AFTER_MS 就進深度睡眠，
// 比淺眠省很多電，但 BLE 會關掉（App 要等水杯醒來再自動連回）
#ifndef DEEP_SLEEP_ENABLE
#define DEEP_SLEEP_ENABLE       0
#endif
#define DEEP_SLEEP_AFTER_MS     (30 * 60 * 1000)
#ifndef SLEEP_TIMER_WAKE_S
#define SLEEP_TIMER_WAKE_S      1800    // 深度睡眠後每 30 分鐘自己醒來量一次水位、廣播一下
#endif
#ifndef SLEEP_ON_USB
#define SLEEP_ON_USB            0       // 1 = 接著電腦 USB 也會進深度睡眠（只在測試時用）
#endif
#define WOM_THRESHOLD           12      // wake-on-motion 門檻，單位 4 mg → 48 mg
#define WOM_LP_ODR              5       // 低功耗加速度取樣率代碼：5 = 7.81 Hz

#define RAD_TO_DEG              57.2957795f

/* ========================================================================== */
/*  MPR121 暫存器                                                               */
/* ========================================================================== */
#define MPR121_ADDR             0x5A    // ADD 腳接 GND → 0x5A（接 3V3=0x5B、SDA=0x5C、SCL=0x5D）

#define MPR121_REG_TOUCH_STATUS 0x00    // 0x00～0x01：觸碰狀態。0x00 bit0=電極0、bit1=電極1…
                                        //   0x01 bit7 = OVCF（REXT 腳過電流旗標）
                                        //   讀取觸碰狀態時，IRQ 腳會被釋放（回到高電位）
#define MPR121_REG_FILT_DATA    0x04    // 0x04～0x1D：各電極「濾波後資料」，每個電極 2 bytes，
                                        //   低位元組在前，只有低 10 位元有效（0～1023）
                                        //   電極0 = 0x04/0x05，電極1 = 0x06/0x07
#define MPR121_REG_MHDR         0x2B    // 基準線濾波（上升方向）：最大半差值 Max Half Delta Rising
#define MPR121_REG_NHDR         0x2C    //   雜訊半差值 Noise Half Delta Rising
#define MPR121_REG_NCLR         0x2D    //   雜訊計數上限 Noise Count Limit Rising
#define MPR121_REG_FDLR         0x2E    //   濾波延遲 Filter Delay Limit Rising
#define MPR121_REG_MHDF         0x2F    // 基準線濾波（下降方向）：Max Half Delta Falling
#define MPR121_REG_NHDF         0x30    //   Noise Half Delta Falling
#define MPR121_REG_NCLF         0x31    //   Noise Count Limit Falling
#define MPR121_REG_FDLF         0x32    //   Filter Delay Limit Falling
#define MPR121_REG_NHDT         0x33    // 基準線濾波（觸碰中）：Noise Half Delta Touched
#define MPR121_REG_NCLT         0x34    //   Noise Count Limit Touched
#define MPR121_REG_FDLT         0x35    //   Filter Delay Limit Touched
#define MPR121_REG_TOUCH_TH(n)   (0x41 + 2 * (n))   // 電極 n 的觸碰門檻（基準線 - 量測值 > 門檻 → 觸碰）
#define MPR121_REG_RELEASE_TH(n) (0x42 + 2 * (n))   // 電極 n 的放開門檻（差值 < 門檻 → 放開）
#define MPR121_REG_CONFIG2      0x5D    // AFE 設定 2：CDT 充電時間 / SFI 第二級濾波 / ESI 取樣間隔
                                        //   重置後預設 0x24（0.5 µs、4 次、16 ms），用來確認晶片身分
#define MPR121_REG_ECR          0x5E    // 電極設定暫存器（Electrode Configuration Register）
                                        //   0x00 = 停止模式（多數設定只能在停止模式下修改）
                                        //   bit7:6 CL = 基準線追蹤模式、bit3:0 = 啟用電極數量
#define MPR121_REG_SOFTRESET    0x80    // 寫入 0x63 = 軟體重置

#define MPR121_CONFIG2_RESET    0x24    // 重置後 0x5D 的預設值
#define MPR121_CONFIG2_RUN      0x44    // 實際使用的 0x5D：CDT=010（充電 1 µs）、SFI=00（4 次）、ESI=100（16 ms）
                                        //   充電時間從 0.5 µs 加倍 → 讀值與水位變化量約放大一倍，解析度較好
#define MPR121_ECR_STOP         0x00
#define MPR121_ECR_RUN_E0_E1    0x82    // CL=10（以第一次量測值的高 5 位元當基準線初值並開始追蹤）
                                        // ELEPROX=00（不用近接偵測）、ELE_EN=0010（啟用電極 0、1）

/* ========================================================================== */
/*  MPU-9250 / MPU-6500 暫存器                                                  */
/* ========================================================================== */
#define MPU_ADDR                0x68    // AD0 接 GND → 0x68（接 3V3 會變 0x69）

#define MPU_REG_SMPLRT_DIV      0x19    // 取樣率除頻：取樣率 = 1 kHz / (1 + SMPLRT_DIV)
#define MPU_REG_CONFIG          0x1A    // bit2:0 DLPF_CFG：陀螺儀數位低通濾波（1～6 時內部取樣為 1 kHz）
#define MPU_REG_GYRO_CONFIG     0x1B    // bit4:3 GYRO_FS_SEL：00 = ±250 度/秒；bit1:0 FCHOICE_B=00 使用 DLPF
#define MPU_REG_ACCEL_CONFIG    0x1C    // bit4:3 ACCEL_FS_SEL：00 = ±2g（16384 LSB/g）
#define MPU_REG_ACCEL_CONFIG2   0x1D    // bit3 ACCEL_FCHOICE_B、bit2:0 A_DLPF_CFG：加速度低通濾波
#define MPU_REG_INT_PIN_CFG     0x37    // INT 腳設定：bit7 ACTL（0=高電位有效）、bit6 OPEN（0=推挽）
                                        //   bit5 LATCH_INT_EN（0=每次發 50 µs 脈衝）
#define MPU_REG_LP_ACCEL_ODR    0x1E    // 低功耗加速度取樣率（wake-on-motion 用）
#define MPU_REG_WOM_THR         0x1F    // wake-on-motion 門檻，單位 4 mg
#define MPU_REG_INT_STATUS      0x3A    // 中斷狀態，讀取會清除（閂鎖模式下 INT 腳回到低電位）
#define MPU_REG_MOT_DETECT_CTRL 0x69    // bit7 ACCEL_INTEL_EN、bit6 ACCEL_INTEL_MODE（1 = 和前一筆比較）
#define MPU_REG_INT_ENABLE      0x38    // 中斷致能：bit0 RAW_RDY_EN = 資料就緒中斷
#define MPU_REG_ACCEL_XOUT_H    0x3B    // 0x3B～0x40：加速度 X/Y/Z，各 16 位元有號數，高位元組在前
#define MPU_REG_PWR_MGMT_1      0x6B    // 電源管理 1：bit7 H_RESET（重置）、bit6 SLEEP、bit2:0 CLKSEL
#define MPU_REG_PWR_MGMT_2      0x6C    // 電源管理 2：各軸停用位元，0x00 = 加速度、陀螺儀全部啟用
#define MPU_REG_WHO_AM_I        0x75    // 晶片 ID：0x71 = MPU-9250、0x70 = MPU-6500

#define MPU_WHOAMI_9250         0x71
#define MPU_WHOAMI_6500         0x70
#define MPU_ACC_LSB_PER_G       16384.0f    // ±2g 量程時 1 g = 16384
#define MPU_GYRO_LSB_PER_DPS    131.0f      // ±250 度/秒量程時 1 度/秒 = 131

/* ========================================================================== */
/*  測試結果與總結表                                                            */
/* ========================================================================== */
typedef enum {
    ST_FAIL,    // 失敗（放第一個 = 預設值；萬一某項漏測，不會被誤判成通過）
    ST_PASS,    // 通過
    ST_MANUAL,  // 需手動確認
} status_t;

typedef struct {
    const char *name;        // 總結表上的項目名稱
    const char *short_name;  // 即時顯示中精簡總結用的短名
    status_t st;
    char detail[320];        // 量測結果說明
    char hint[1024];         // 失敗時該檢查哪些線（多行，以 \n 分隔）
} test_item_t;

enum {
    IT_SCAN,
    IT_MPR,
    IT_IMU_ID,
    IT_IMU_ACC,
    IT_IMU_INT,
    IT_BAT,
    IT_MPR_IRQ,
    IT_COUNT
};

static test_item_t s_items[IT_COUNT] = {
    [IT_SCAN]    = { .name = "1. I2C 掃描",            .short_name = "I2C" },
    [IT_MPR]     = { .name = "2. MPR121 重置與設定",   .short_name = "MPR121" },
    [IT_IMU_ID]  = { .name = "3. IMU 識別與設定",      .short_name = "IMU" },
    [IT_IMU_ACC] = { .name = "4. IMU 靜止加速度",      .short_name = "加速度" },
    [IT_IMU_INT] = { .name = "5. IMU INT 中斷 (D1)",   .short_name = "INT(D1)" },
    [IT_BAT]     = { .name = "6. 電池電壓 (D0)",       .short_name = "電池" },
    [IT_MPR_IRQ] = { .name = "7. MPR121 IRQ (D2)",     .short_name = "IRQ(D2)" },
};

/* ========================================================================== */
/*  全域狀態                                                                    */
/* ========================================================================== */
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_mpr_dev;   // 掃描有找到才會建立
static i2c_master_dev_handle_t s_imu_dev;
static bool s_found[0x80];                  // I2C 掃描結果
static bool s_mpr_ready;                    // MPR121 初始化成功 → 即時顯示才讀它
static bool s_imu_ready;                    // IMU 初始化成功 → 即時顯示才讀它

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static const char *s_adc_cali_name = "未校正";

static volatile uint32_t s_imu_int_count;   // D1 上升緣計數（IMU 資料就緒）
static volatile uint32_t s_mpr_irq_count;   // D2 下降緣計數（MPR121 IRQ）

// 休眠相關（RTC 記憶體，深度睡眠時保留；冷開機清成 0）
static RTC_DATA_ATTR uint32_t s_sleep_count;   // 進入深度睡眠的次數
static bool s_woke_from_sleep;                 // 這次開機是不是從深度睡眠醒來

/* ========================================================================== */
/*  小工具：結果字串、中文對齊                                                  */
/* ========================================================================== */
static void item_set(test_item_t *it, status_t st, const char *fmt, ...)
{
    it->st = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it->detail, sizeof(it->detail), fmt, ap);
    va_end(ap);
}

// 在提示欄追加一行（失敗時要檢查的線）
static void hint_add(test_item_t *it, const char *fmt, ...)
{
    size_t used = strlen(it->hint);
    if (used + 2 >= sizeof(it->hint)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it->hint + used, sizeof(it->hint) - used - 1, fmt, ap);
    va_end(ap);
    used = strlen(it->hint);
    it->hint[used] = '\n';
    it->hint[used + 1] = '\0';
}

// 判斷 Unicode 字元在終端機上是否佔 2 格（中日韓文字、全形標點）
static bool is_wide_char(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6);
}

// 計算 UTF-8 字串的顯示寬度（printf 的 %-10s 是算 byte，中文會對不齊）
static int display_width(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    int w = 0;
    while (*p) {
        uint32_t cp = *p;
        int len = 1;
        if (*p >= 0xF0)      { cp = *p & 0x07; len = 4; }
        else if (*p >= 0xE0) { cp = *p & 0x0F; len = 3; }
        else if (*p >= 0xC0) { cp = *p & 0x1F; len = 2; }
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) {   // 不完整的 UTF-8 序列（含字串結尾）
                len = i;
                break;
            }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        w += is_wide_char(cp) ? 2 : 1;
        p += len;
    }
    return w;
}

// 依顯示寬度補空白印出一欄
static void print_col(const char *s, int width, bool right_align)
{
    int pad = width - display_width(s);
    if (pad < 0) {
        pad = 0;
    }
    if (right_align) {
        printf("%*s%s", pad, "", s);
    } else {
        printf("%s%*s", s, pad, "");
    }
}

static void print_rule(char c)
{
    for (int i = 0; i < 88; i++) {
        putchar(c);
    }
    putchar('\n');
}

static const char *status_tag(status_t st)
{
    switch (st) {
    case ST_PASS: return "[通過]";
    case ST_FAIL: return "[失敗]";
    default:      return "[手動]";
    }
}

static const char *status_word(status_t st)
{
    switch (st) {
    case ST_PASS: return "通過";
    case ST_FAIL: return "失敗";
    default:      return "手動";
    }
}

/* ========================================================================== */
/*  I2C 讀寫                                                                    */
/* ========================================================================== */
static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

// 先寫暫存器位址，再以 repeated start 連續讀 len 個 byte（兩顆晶片都支援位址自動遞增）
static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

static esp_err_t add_i2c_device(uint16_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, out);
}

/* ========================================================================== */
/*  GPIO 中斷計數                                                               */
/* ========================================================================== */
// 共用 ISR：arg 指向要累加的計數器
static void IRAM_ATTR gpio_count_isr(void *arg)
{
    volatile uint32_t *counter = (volatile uint32_t *)arg;
    (*counter)++;
}

/* ========================================================================== */
/*  測試 1：I2C 掃描                                                            */
/* ========================================================================== */
static const char *addr_label(uint8_t addr)
{
    switch (addr) {
    case MPR121_ADDR: return "(MPR121)";
    case MPU_ADDR:    return "(IMU)";
    case 0x5B: case 0x5C: case 0x5D: return "(MPR121?位址不對)";
    case 0x69:        return "(IMU?位址不對)";
    case 0x0C:        return "(AK8963 磁力計)";
    default:          return "(未知)";
    }
}

static void test_i2c_scan(void)
{
    test_item_t *it = &s_items[IT_SCAN];

    // 建立 I2C 主機匯流排（新版驅動）
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                         // 自動挑選 I2C 控制器
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,                 // 濾掉 7 個時脈以內的毛刺
        .flags.enable_internal_pullup = true,   // 開內部上拉；模組板上通常也有 10k 上拉
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;
        item_set(it, ST_FAIL, "I2C 驅動初始化失敗（%s）", esp_err_to_name(err));
        hint_add(it, "→ 這是軟體／腳位設定問題，確認 GPIO22、GPIO23 沒有被其他程式占用");
        return;
    }

    // 掃描時關掉 i2c.master 的 log，避免沒裝置的位址印出一堆 NACK／逾時訊息
    esp_log_level_t old_level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    int found_cnt = 0;
    int timeout_cnt = 0;
    for (int addr = I2C_SCAN_FIRST; addr <= I2C_SCAN_LAST; addr++) {
        err = i2c_master_probe(s_bus, (uint16_t)addr, I2C_PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            s_found[addr] = true;
            found_cnt++;
        } else if (err == ESP_ERR_TIMEOUT) {
            timeout_cnt++;       // 逾時通常代表 SDA/SCL 被拉住或沒有上拉
        }
    }

    esp_log_level_set("i2c.master", old_level);  // 掃完恢復原本的 log 等級

    // 組出「找到的位址」清單（最多列 8 個）
    char list[256] = "";
    int listed = 0;
    for (int addr = I2C_SCAN_FIRST; addr <= I2C_SCAN_LAST; addr++) {
        if (!s_found[addr]) {
            continue;
        }
        if (listed < 8) {
            size_t used = strlen(list);
            snprintf(list + used, sizeof(list) - used, "%s0x%02X%s",
                     listed ? " " : "", addr, addr_label((uint8_t)addr));
        }
        listed++;
    }
    if (listed > 8) {
        size_t used = strlen(list);
        snprintf(list + used, sizeof(list) - used, " …共 %d 個", listed);
    }

    bool mpr = s_found[MPR121_ADDR];
    bool imu = s_found[MPU_ADDR];

    if (found_cnt == 0) {
        item_set(it, ST_FAIL, "沒有找到任何裝置%s", timeout_cnt ? "（而且有逾時）" : "");
    } else {
        item_set(it, (mpr && imu) ? ST_PASS : ST_FAIL, "找到：%s", list);
    }
    if (timeout_cnt > 0) {
        size_t used = strlen(it->detail);
        snprintf(it->detail + used, sizeof(it->detail) - used, "，逾時 %d 次", timeout_cnt);
    }

    // ---- 失敗提示 ----
    if (found_cnt == 0) {
        hint_add(it, "→ 匯流排上完全沒有回應，請檢查：");
        hint_add(it, "  · XIAO D4(GPIO22) ↔ 兩個模組的 SDA");
        hint_add(it, "  · XIAO D5(GPIO23) ↔ 兩個模組的 SCL（SDA、SCL 有沒有接反）");
        hint_add(it, "  · XIAO 3V3 ↔ 兩個模組的 VCC，XIAO GND ↔ 兩個模組的 GND");
    }
    if (timeout_cnt > 0) {
        hint_add(it, "%s有 %d 次逾時：SDA 或 SCL 可能短路到 GND，或兩條線互相短路",
                 found_cnt == 0 ? "  · " : "→ ", timeout_cnt);
    }
    if (found_cnt > 8) {
        hint_add(it, "→ 幾乎每個位址都有回應：SDA 可能被拉到 GND（短路），檢查 SDA 線");
    }
    if (found_cnt > 0 && !mpr) {
        hint_add(it, "→ MPR121 (0x5A) 沒找到：檢查 MPR121 的 VCC、GND、SDA、SCL 四條線");
        if (s_found[0x5B]) hint_add(it, "  · 0x5B 有裝置：MPR121 的 ADD 腳接到 3V3 了，應該接 GND");
        if (s_found[0x5C]) hint_add(it, "  · 0x5C 有裝置：MPR121 的 ADD 腳接到 SDA 了，應該接 GND");
        if (s_found[0x5D]) hint_add(it, "  · 0x5D 有裝置：MPR121 的 ADD 腳接到 SCL 了，應該接 GND");
    }
    if (found_cnt > 0 && !imu) {
        hint_add(it, "→ IMU (0x68) 沒找到：檢查 IMU 的 VCC、GND、SDA、SCL 四條線");
        hint_add(it, "  · NCS 要接 3V3（NCS 接地或空接，晶片可能進入 SPI 模式）");
        if (s_found[0x69]) hint_add(it, "  · 0x69 有裝置：IMU 的 AD0 腳沒接 GND（現在是高電位）");
    }

    // 只替有找到的裝置建立 handle；沒找到的後續測試直接標失敗，不去碰它
    if (mpr && add_i2c_device(MPR121_ADDR, &s_mpr_dev) != ESP_OK) {
        s_mpr_dev = NULL;
    }
    if (imu && add_i2c_device(MPU_ADDR, &s_imu_dev) != ESP_OK) {
        s_imu_dev = NULL;
    }
}

/* ========================================================================== */
/*  測試 2：MPR121 重置與設定                                                   */
/* ========================================================================== */
typedef struct {
    uint8_t reg;
    uint8_t val;
} reg_val_t;

// 停止模式下寫入的設定（寫完會逐一讀回比對）
static const reg_val_t k_mpr121_cfg[] = {
    // 電極 0、1 的觸碰／放開門檻
    { MPR121_REG_TOUCH_TH(0),   MPR121_TOUCH_TH },
    { MPR121_REG_RELEASE_TH(0), MPR121_RELEASE_TH },
    { MPR121_REG_TOUCH_TH(1),   MPR121_TOUCH_TH },
    { MPR121_REG_RELEASE_TH(1), MPR121_RELEASE_TH },
    // 基準線追蹤濾波器（NXP 應用筆記的常用值）：重置後這些都是 0，
    // 設定後基準線才會慢慢跟上環境變化（溫度、濕度造成的漂移）
    { MPR121_REG_MHDR, 0x01 }, { MPR121_REG_NHDR, 0x01 },
    { MPR121_REG_NCLR, 0x0E }, { MPR121_REG_FDLR, 0x00 },
    { MPR121_REG_MHDF, 0x01 }, { MPR121_REG_NHDF, 0x05 },
    { MPR121_REG_NCLF, 0x01 }, { MPR121_REG_FDLF, 0x00 },
    { MPR121_REG_NHDT, 0x00 }, { MPR121_REG_NCLT, 0x00 },
    { MPR121_REG_FDLT, 0x00 },
    // 充電時間 0.5 µs → 1 µs（取樣間隔維持 16 ms）。0x5C（充電電流 16 µA）維持重置預設值
    { MPR121_REG_CONFIG2, MPR121_CONFIG2_RUN },
};

static bool mpr121_read_filtered(uint16_t *e0, uint16_t *e1)
{
    uint8_t d[4];
    if (reg_read(s_mpr_dev, MPR121_REG_FILT_DATA, d, sizeof(d)) != ESP_OK) {
        return false;
    }
    *e0 = (uint16_t)(((d[1] & 0x03) << 8) | d[0]);   // 10 位元：高位元組只取低 2 bit
    *e1 = (uint16_t)(((d[3] & 0x03) << 8) | d[2]);
    return true;
}

// 讀觸碰狀態（0x00、0x01），讀取同時會清除 MPR121 的 IRQ
static bool mpr121_read_touch(uint8_t st[2])
{
    return reg_read(s_mpr_dev, MPR121_REG_TOUCH_STATUS, st, 2) == ESP_OK;
}

static void test_mpr121(void)
{
    test_item_t *it = &s_items[IT_MPR];
    const char *wire_hint = "→ 檢查 MPR121 的 SDA ↔ D4、SCL ↔ D5 是否接觸不良，VCC ↔ 3V3、GND ↔ GND 是否穩固";

    if (s_mpr_dev == NULL) {
        item_set(it, ST_FAIL, "I2C 掃描沒找到 MPR121，無法測試");
        hint_add(it, "→ 先依第 1 項的提示修好 MPR121 的接線（VCC、GND、SDA、SCL、ADD→GND）");
        return;
    }

    // (1) 軟體重置：0x80 寫 0x63，所有暫存器回到預設值
    if (reg_write(s_mpr_dev, MPR121_REG_SOFTRESET, 0x63) != ESP_OK) {
        item_set(it, ST_FAIL, "軟體重置寫入失敗");
        hint_add(it, "%s", wire_hint);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // (2) 重置後 0x5D 應為 0x24，用來確認真的是 MPR121 且重置成功
    uint8_t cfg2 = 0;
    if (reg_read(s_mpr_dev, MPR121_REG_CONFIG2, &cfg2, 1) != ESP_OK) {
        item_set(it, ST_FAIL, "重置後讀取 0x5D 失敗");
        hint_add(it, "%s", wire_hint);
        return;
    }
    if (cfg2 != MPR121_CONFIG2_RESET) {
        item_set(it, ST_FAIL, "重置後 0x5D = 0x%02X（應為 0x24）", cfg2);
        hint_add(it, "→ 0x5A 上的裝置可能不是 MPR121，或 SDA/SCL 訊號不良（線太長、接觸不良）");
        hint_add(it, "%s", wire_hint);
        return;
    }

    // (3) 改設定前先進入停止模式：ECR(0x5E) = 0
    esp_err_t err = reg_write(s_mpr_dev, MPR121_REG_ECR, MPR121_ECR_STOP);

    // (4) 寫入門檻與濾波設定
    for (size_t i = 0; i < sizeof(k_mpr121_cfg) / sizeof(k_mpr121_cfg[0]) && err == ESP_OK; i++) {
        err = reg_write(s_mpr_dev, k_mpr121_cfg[i].reg, k_mpr121_cfg[i].val);
    }

    // (5) 啟用電極 0、1：ECR = 0x82，開始量測
    if (err == ESP_OK) {
        err = reg_write(s_mpr_dev, MPR121_REG_ECR, MPR121_ECR_RUN_E0_E1);
    }
    if (err != ESP_OK) {
        item_set(it, ST_FAIL, "0x5D=0x24 正確，但寫入設定時失敗（%s）", esp_err_to_name(err));
        hint_add(it, "%s", wire_hint);
        return;
    }

    // (6) 讀回比對，確認每個暫存器都真的寫進去了
    for (size_t i = 0; i < sizeof(k_mpr121_cfg) / sizeof(k_mpr121_cfg[0]); i++) {
        uint8_t v = 0;
        if (reg_read(s_mpr_dev, k_mpr121_cfg[i].reg, &v, 1) != ESP_OK || v != k_mpr121_cfg[i].val) {
            item_set(it, ST_FAIL, "暫存器 0x%02X 讀回 0x%02X，寫入的是 0x%02X",
                     k_mpr121_cfg[i].reg, v, k_mpr121_cfg[i].val);
            hint_add(it, "%s", wire_hint);
            return;
        }
    }
    uint8_t ecr = 0;
    if (reg_read(s_mpr_dev, MPR121_REG_ECR, &ecr, 1) != ESP_OK || ecr != MPR121_ECR_RUN_E0_E1) {
        item_set(it, ST_FAIL, "ECR(0x5E) 讀回 0x%02X（應為 0x82）", ecr);
        hint_add(it, "%s", wire_hint);
        return;
    }

    // (7) 等幾個取樣週期（16 ms/次），讀一次電極值並清掉開機時可能掛著的 IRQ
    vTaskDelay(pdMS_TO_TICKS(200));
    uint16_t e0 = 0, e1 = 0;
    uint8_t st[2];
    bool ok = mpr121_read_filtered(&e0, &e1);
    ok = mpr121_read_touch(st) && ok;
    if (!ok) {
        item_set(it, ST_FAIL, "設定完成，但讀取電極資料失敗");
        hint_add(it, "%s", wire_hint);
        return;
    }

    s_mpr_ready = true;
    item_set(it, ST_PASS, "0x5D=0x24→0x%02X（充電 1 µs），門檻 %d/%d，ECR=0x82；電極0=%u 電極1=%u",
             MPR121_CONFIG2_RUN, MPR121_TOUCH_TH, MPR121_RELEASE_TH, e0, e1);
}

/* ========================================================================== */
/*  測試 3、4：IMU 識別、設定、靜止加速度                                       */
/* ========================================================================== */
// 初始化設定（重置、喚醒之後寫入，寫完讀回比對）
static const reg_val_t k_imu_cfg[] = {
    { MPU_REG_PWR_MGMT_2,   0x00 }, // 加速度、陀螺儀各軸全部啟用
    { MPU_REG_CONFIG,       0x03 }, // DLPF_CFG=3：陀螺儀頻寬 41 Hz，內部取樣 1 kHz
    { MPU_REG_SMPLRT_DIV,   9    }, // 取樣率 = 1000 / (1 + 9) = 100 Hz
    { MPU_REG_GYRO_CONFIG,  0x00 }, // ±250 度/秒，FCHOICE_B=00（使用 DLPF）
    { MPU_REG_ACCEL_CONFIG, 0x00 }, // ±2g
    { MPU_REG_INT_PIN_CFG,  0x00 }, // INT：高電位有效、推挽、50 µs 脈衝（不閂鎖）
    { MPU_REG_INT_ENABLE,   0x00 }, // 先關閉中斷，INT 測試時才打開
};

static bool imu_read_accel(float *ax, float *ay, float *az)
{
    uint8_t d[6];
    if (reg_read(s_imu_dev, MPU_REG_ACCEL_XOUT_H, d, sizeof(d)) != ESP_OK) {
        return false;
    }
    int16_t x = (int16_t)((d[0] << 8) | d[1]);   // 高位元組在前
    int16_t y = (int16_t)((d[2] << 8) | d[3]);
    int16_t z = (int16_t)((d[4] << 8) | d[5]);
    *ax = x / MPU_ACC_LSB_PER_G;
    *ay = y / MPU_ACC_LSB_PER_G;
    *az = z / MPU_ACC_LSB_PER_G;
    return true;
}

// 傾斜角：加速度向量和 Z 軸的夾角（平放 ≈ 0 度、立起來 ≈ 90 度、倒過來 ≈ 180 度）
static float tilt_deg(float ax, float ay, float az)
{
    float total = sqrtf(ax * ax + ay * ay + az * az);
    if (total < 0.01f) {
        return 0.0f;
    }
    float c = az / total;
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c) * RAD_TO_DEG;
}

static void test_imu_id(void)
{
    test_item_t *it = &s_items[IT_IMU_ID];
    const char *wire_hint = "→ 檢查 IMU 的 SDA ↔ D4、SCL ↔ D5 是否接觸不良，VCC ↔ 3V3、GND ↔ GND 是否穩固";

    if (s_imu_dev == NULL) {
        item_set(it, ST_FAIL, "I2C 掃描沒找到 IMU，無法測試");
        hint_add(it, "→ 先依第 1 項的提示修好 IMU 的接線（VCC、GND、SDA、SCL、AD0→GND、NCS→3V3）");
        return;
    }

    // (1) 讀 WHO_AM_I（0x75）判斷晶片型號
    uint8_t who = 0;
    if (reg_read(s_imu_dev, MPU_REG_WHO_AM_I, &who, 1) != ESP_OK) {
        item_set(it, ST_FAIL, "讀取 WHO_AM_I 失敗");
        hint_add(it, "%s", wire_hint);
        return;
    }
    if (who == 0x00 || who == 0xFF) {
        item_set(it, ST_FAIL, "WHO_AM_I = 0x%02X（異常值）", who);
        hint_add(it, "→ 讀到全 0 或全 1 通常是晶片沒正常供電或 SDA 接觸不良");
        hint_add(it, "%s", wire_hint);
        return;
    }
    const char *chip = (who == MPU_WHOAMI_9250) ? "MPU-9250" :
                       (who == MPU_WHOAMI_6500) ? "MPU-6500" : "相容晶片";

    // (2) 重置：PWR_MGMT_1 寫 0x80（H_RESET），所有暫存器回到預設，晶片進入睡眠
    //     部分相容晶片在重置的瞬間不回 ACK，所以這一步的錯誤先忽略，後面喚醒時再確認
    (void)reg_write(s_imu_dev, MPU_REG_PWR_MGMT_1, 0x80);
    vTaskDelay(pdMS_TO_TICKS(100));

    // (3) 喚醒：PWR_MGMT_1 = 0x01（清除 SLEEP，時脈自動選 PLL）
    esp_err_t err = reg_write(s_imu_dev, MPU_REG_PWR_MGMT_1, 0x01);
    vTaskDelay(pdMS_TO_TICKS(50));

    // (4) 量程、低通濾波、取樣率、INT 腳設定
    for (size_t i = 0; i < sizeof(k_imu_cfg) / sizeof(k_imu_cfg[0]) && err == ESP_OK; i++) {
        err = reg_write(s_imu_dev, k_imu_cfg[i].reg, k_imu_cfg[i].val);
    }
    // 加速度低通濾波：A_DLPF_CFG=3（頻寬約 45 Hz）。MPU-6050 等舊相容晶片沒有這個暫存器，不讀回比對
    if (err == ESP_OK) {
        err = reg_write(s_imu_dev, MPU_REG_ACCEL_CONFIG2, 0x03);
    }
    if (err != ESP_OK) {
        item_set(it, ST_FAIL, "WHO_AM_I=0x%02X（%s），但寫入設定失敗（%s）",
                 who, chip, esp_err_to_name(err));
        hint_add(it, "%s", wire_hint);
        return;
    }

    // (5) 讀回比對
    uint8_t pwr1 = 0;
    if (reg_read(s_imu_dev, MPU_REG_PWR_MGMT_1, &pwr1, 1) != ESP_OK || pwr1 != 0x01) {
        item_set(it, ST_FAIL, "PWR_MGMT_1 讀回 0x%02X（應為 0x01），晶片沒有喚醒", pwr1);
        hint_add(it, "→ 檢查 IMU 的 VCC 供電是否穩定（3.3V），GND 是否接好");
        return;
    }
    for (size_t i = 0; i < sizeof(k_imu_cfg) / sizeof(k_imu_cfg[0]); i++) {
        uint8_t v = 0;
        if (reg_read(s_imu_dev, k_imu_cfg[i].reg, &v, 1) != ESP_OK || v != k_imu_cfg[i].val) {
            item_set(it, ST_FAIL, "暫存器 0x%02X 讀回 0x%02X，寫入的是 0x%02X",
                     k_imu_cfg[i].reg, v, k_imu_cfg[i].val);
            hint_add(it, "%s", wire_hint);
            return;
        }
    }

    s_imu_ready = true;
    item_set(it, ST_PASS, "WHO_AM_I=0x%02X → %s；±2g、±250 度/秒、100 Hz", who, chip);
}

static void test_imu_accel(void)
{
    test_item_t *it = &s_items[IT_IMU_ACC];

    if (!s_imu_ready) {
        item_set(it, ST_FAIL, "IMU 未就緒，無法測試");
        hint_add(it, "→ 先修好第 3 項");
        return;
    }

    // 等低通濾波穩定後，取 10 筆平均，減少雜訊
    vTaskDelay(pdMS_TO_TICKS(100));
    float sx = 0, sy = 0, sz = 0;
    const int n = 10;
    for (int i = 0; i < n; i++) {
        float ax, ay, az;
        if (!imu_read_accel(&ax, &ay, &az)) {
            item_set(it, ST_FAIL, "讀取加速度失敗");
            hint_add(it, "→ 檢查 IMU 的 SDA ↔ D4、SCL ↔ D5 是否接觸不良");
            return;
        }
        sx += ax; sy += ay; sz += az;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    sx /= n; sy /= n; sz /= n;
    float total = sqrtf(sx * sx + sy * sy + sz * sz);
    bool pass = total >= ACC_TOTAL_MIN_G && total <= ACC_TOTAL_MAX_G;

    item_set(it, pass ? ST_PASS : ST_FAIL,
             "合計 %.3f g（X=%+.2f Y=%+.2f Z=%+.2f），傾斜 %.1f 度",
             total, sx, sy, sz, tilt_deg(sx, sy, sz));

    if (!pass) {
        if (total < 0.1f) {
            hint_add(it, "→ 加速度幾乎是 0：感測器可能還在睡眠或損壞，檢查 IMU 的 VCC（3.3V）與 GND");
        } else {
            hint_add(it, "→ 應在 %.2f～%.2f g：測試時板子要放著不動", ACC_TOTAL_MIN_G, ACC_TOTAL_MAX_G);
            hint_add(it, "→ 板子確實靜止仍超出範圍：檢查 IMU 的 VCC/GND 供電是否穩定，也可能是相容晶片靈敏度不同");
        }
    }
}

/* ========================================================================== */
/*  測試 5：IMU INT（D1）資料就緒中斷                                           */
/* ========================================================================== */
static void test_imu_int(void)
{
    test_item_t *it = &s_items[IT_IMU_INT];

    if (!s_imu_ready) {
        item_set(it, ST_FAIL, "IMU 未就緒，無法測試");
        hint_add(it, "→ 先修好第 3 項");
        return;
    }

    // D1 設成輸入、偵測上升緣。開內部下拉：線沒接時固定在低電位，不會浮接亂跳造成誤判
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_IMU_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(PIN_IMU_INT, gpio_count_isr, (void *)&s_imu_int_count);
    }
    if (err != ESP_OK) {
        item_set(it, ST_FAIL, "D1 GPIO 中斷設定失敗（%s）", esp_err_to_name(err));
        return;
    }

    // 開啟 IMU 的資料就緒中斷：INT_PIN_CFG=0x00（高電位有效、推挽、50 µs 脈衝），INT_ENABLE=0x01
    err = reg_write(s_imu_dev, MPU_REG_INT_PIN_CFG, 0x00);
    if (err == ESP_OK) {
        err = reg_write(s_imu_dev, MPU_REG_INT_ENABLE, 0x01);
    }

    uint32_t count = 0;
    if (err == ESP_OK) {
        s_imu_int_count = 0;
        vTaskDelay(pdMS_TO_TICKS(1000));   // 數 1 秒
        count = s_imu_int_count;
    }

    // 測完關掉：IMU 端關中斷、ESP32 端移除 ISR
    (void)reg_write(s_imu_dev, MPU_REG_INT_ENABLE, 0x00);
    gpio_intr_disable(PIN_IMU_INT);
    gpio_isr_handler_remove(PIN_IMU_INT);

    if (err != ESP_OK) {
        item_set(it, ST_FAIL, "寫入 IMU 中斷設定失敗（%s）", esp_err_to_name(err));
        hint_add(it, "→ 檢查 IMU 的 SDA ↔ D4、SCL ↔ D5 是否接觸不良");
        return;
    }

    bool pass = count >= IMU_INT_MIN && count <= IMU_INT_MAX;
    item_set(it, pass ? ST_PASS : ST_FAIL, "1 秒收到 %" PRIu32 " 次上升緣（設定 100 Hz，%d～%d 算通過）",
             count, IMU_INT_MIN, IMU_INT_MAX);

    if (count == 0) {
        hint_add(it, "→ 完全沒收到脈衝：檢查 IMU 的 INT ↔ XIAO D1(GPIO1) 這條線");
        hint_add(it, "  · 有沒有接、有沒有插到隔壁的 D0 或 D2");
    } else if (count < IMU_INT_MIN) {
        hint_add(it, "→ 脈衝太少：IMU 的 INT ↔ XIAO D1(GPIO1) 可能接觸不良（時有時無）");
    } else if (count > IMU_INT_MAX) {
        hint_add(it, "→ 脈衝太多：D1 可能受到干擾，或 INT 線碰到其他訊號線（例如 SDA/SCL）");
        hint_add(it, "  · 檢查 IMU 的 INT ↔ XIAO D1(GPIO1) 這條線");
    }
}

/* ========================================================================== */
/*  測試 6：電池電壓（D0 / ADC1 通道 0）                                        */
/* ========================================================================== */
static esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        s_adc = NULL;
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,   // C6 為 12 位元
    };
    err = adc_oneshot_config_channel(s_adc, BAT_ADC_CHAN, &chan_cfg);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return err;
    }

    // ADC 校正：用 eFuse 裡出廠量測的參數把原始值換成 mV。ESP32-C6 支援 curve fitting
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHAN,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
        s_adc_cali_name = "curve fitting 校正";
    } else {
        s_adc_cali = NULL;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (s_adc_cali == NULL) {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = BAT_ADC_UNIT,
            .atten = BAT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
            s_adc_cali_name = "line fitting 校正";
        } else {
            s_adc_cali = NULL;
        }
    }
#endif
    return ESP_OK;
}

// 讀 32 次（每次換成 mV）取平均，回傳 D0 腳電壓與換算後的電池電壓
static bool battery_read(int *pin_mv, int *bat_mv)
{
    if (s_adc == NULL) {
        return false;
    }
    int32_t sum = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHAN, &raw) != ESP_OK) {
            return false;
        }
        if (s_adc_cali != NULL) {
            if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) != ESP_OK) {
                return false;
            }
        } else {
            mv = raw * 3300 / 4095;   // 沒有校正資料時的粗略估算
        }
        sum += mv;
    }
    *pin_mv = (int)(sum / BAT_SAMPLES);
    *bat_mv = *pin_mv * 2;   // 200k : 200k 分壓，D0 看到的是電池電壓的一半
    return true;
}

static void test_battery(void)
{
    test_item_t *it = &s_items[IT_BAT];

    esp_err_t err = battery_init();
    if (err != ESP_OK) {
        item_set(it, ST_FAIL, "ADC 初始化失敗（%s）", esp_err_to_name(err));
        hint_add(it, "→ 這是軟體設定問題，與接線無關");
        return;
    }

    int pin_mv = 0, bat_mv = 0;
    if (!battery_read(&pin_mv, &bat_mv)) {
        item_set(it, ST_FAIL, "ADC 讀取失敗");
        hint_add(it, "→ 這是軟體設定問題，與接線無關");
        return;
    }

    bool pass = bat_mv >= BAT_MIN_MV && bat_mv <= BAT_MAX_MV;
    item_set(it, pass ? ST_PASS : ST_FAIL, "%.2f V（D0 = %d mV × 2，%s）",
             bat_mv / 1000.0f, pin_mv, s_adc_cali_name);

    if (bat_mv < 500) {
        hint_add(it, "→ 幾乎量不到電壓，請檢查：");
        hint_add(it, "  · BAT+ → 200kΩ 這顆電阻有沒有接到 BAT+");
        hint_add(it, "  · D0(GPIO0) 有沒有接到兩顆 200kΩ 的中間點");
        hint_add(it, "  · 電池有沒有焊到 XIAO 背面的 BAT+ / BAT- 焊點、正負極有沒有接反");
    } else if (bat_mv < BAT_MIN_MV) {
        hint_add(it, "→ 電壓偏低（應 2.8～4.4 V）：");
        hint_add(it, "  · 電池可能沒電，插 USB 充一陣子再測");
        hint_add(it, "  · 分壓電阻值可能用錯（上面那顆比 200kΩ 大，或下面那顆比 200kΩ 小）");
    } else if (bat_mv > BAT_MAX_MV) {
        hint_add(it, "→ 電壓偏高（應 2.8～4.4 V）：");
        hint_add(it, "  · D0 → 200kΩ → GND 這顆電阻可能沒接到 GND（D0 被 BAT+ 直接拉高）");
        hint_add(it, "  · 分壓電阻值可能用錯（下面那顆比 200kΩ 大）");
    }
}

/* ========================================================================== */
/*  項目 7：MPR121 IRQ（D2）—— 開機無法自動測，設定好計數給即時顯示用            */
/* ========================================================================== */
static void setup_mpr_irq(void)
{
    test_item_t *it = &s_items[IT_MPR_IRQ];

    // D2 設成輸入、偵測下降緣。MPR121 的 IRQ 是開汲極、低電位有效，所以要開內部上拉
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_MPR_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(PIN_MPR_IRQ, gpio_count_isr, (void *)&s_mpr_irq_count);
    }

    item_set(it, ST_MANUAL, "看下面即時數值，用手碰電極腳");

    if (err != ESP_OK) {
        hint_add(it, "→ D2 GPIO 中斷設定失敗（%s），IRQ 次數不會增加", esp_err_to_name(err));
        return;
    }
    if (!s_mpr_ready) {
        hint_add(it, "→ MPR121 未就緒（見第 2 項），即時顯示的電極欄位會是 --");
        return;
    }

    // 再讀一次觸碰狀態把 IRQ 清掉，然後看 D2 的待機電位：正常應為高電位
    uint8_t st[2];
    (void)mpr121_read_touch(st);
    vTaskDelay(pdMS_TO_TICKS(5));
    int level = gpio_get_level(PIN_MPR_IRQ);
    size_t used = strlen(it->detail);
    snprintf(it->detail + used, sizeof(it->detail) - used, "（D2 待機電位：%s）",
             level ? "高，正常" : "低，異常");

    if (!level) {
        hint_add(it, "→ D2 待機時是低電位：IRQ ↔ D2 可能短路到 GND，或 D2 接到別的腳");
    }
    hint_add(it, "→ 判斷方式：碰電極時「觸碰」欄有變但「IRQ數」不動 → 檢查 MPR121 IRQ ↔ XIAO D2(GPIO2)");
}

/* ========================================================================== */
/*  總結表                                                                      */
/* ========================================================================== */
#define NAME_COL_WIDTH  22

static void print_summary(void)
{
    int n_pass = 0, n_fail = 0, n_manual = 0;

    printf("\n");
    print_rule('=');
    printf("  硬體接線測試　總結\n");
    print_rule('=');
    printf("  ");
    print_col("項目", NAME_COL_WIDTH, false);
    printf("  結果    說明\n");
    print_rule('-');

    for (int i = 0; i < IT_COUNT; i++) {
        const test_item_t *it = &s_items[i];
        printf("  ");
        print_col(it->name, NAME_COL_WIDTH, false);
        printf("  %s  %s\n", status_tag(it->st), it->detail);

        // 失敗或手動項目：逐行印出該檢查的線
        if (it->st != ST_PASS && it->hint[0] != '\0') {
            const char *p = it->hint;
            while (*p) {
                const char *nl = strchr(p, '\n');
                int len = nl ? (int)(nl - p) : (int)strlen(p);
                printf("        %.*s\n", len, p);
                p += len + (nl ? 1 : 0);
            }
        }

        if (it->st == ST_PASS)      n_pass++;
        else if (it->st == ST_FAIL) n_fail++;
        else                        n_manual++;
    }

    print_rule('-');
    printf("  通過 %d 項、失敗 %d 項、需手動確認 %d 項\n", n_pass, n_fail, n_manual);
    print_rule('=');
}

// 一行版的總結，跟著即時顯示的標題一起重印（萬一開機時的總結表沒看到）
static void print_compact_summary(void)
{
    printf("開機測試：");
    for (int i = 0; i < IT_COUNT; i++) {
        printf("%s %s%s", s_items[i].short_name, status_word(s_items[i].st),
               (i < IT_COUNT - 1) ? "｜" : "\n");
    }
}

/* ========================================================================== */
/*  第二部分：即時量測、BLE 推送、序列埠顯示                                    */
/* ========================================================================== */
// 讀加速度與陀螺儀（0x3B～0x48 一次讀 14 bytes：加速度 XYZ、溫度、陀螺儀 XYZ）
static bool imu_read_all(float acc[3], float gyro[3])
{
    uint8_t d[14];
    if (reg_read(s_imu_dev, MPU_REG_ACCEL_XOUT_H, d, sizeof(d)) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        acc[i] = (int16_t)((d[2 * i] << 8) | d[2 * i + 1]) / MPU_ACC_LSB_PER_G;
        gyro[i] = (int16_t)((d[8 + 2 * i] << 8) | d[8 + 2 * i + 1]) / MPU_GYRO_LSB_PER_DPS;
    }
    return true;
}

// 開機時把「直立靜止」記成姿態零點：取 20 筆平均，傾斜超過 10° 或在晃動就不採用
static void motion_zero(void)
{
    if (!s_imu_ready) {
        return;
    }
    float acc_sum[3] = { 0 }, gyro_sum[3] = { 0 };
    int n = 0;
    for (int i = 0; i < 20; i++) {
        float acc[3], gyro[3];
        if (imu_read_all(acc, gyro)) {
            for (int k = 0; k < 3; k++) {
                acc_sum[k] += acc[k];
                gyro_sum[k] += gyro[k];
            }
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (n == 0) {
        printf("  姿態零點：讀 IMU 失敗，零點維持 0\n");
        return;
    }
    bool ok = cup_motion_set_zero(acc_sum[0] / n, acc_sum[1] / n, acc_sum[2] / n,
                                  gyro_sum[0] / n, gyro_sum[1] / n, gyro_sum[2] / n);
    printf("  姿態零點：%s\n", ok ? "已用開機時的姿態當直立零點"
                                    : "開機時杯子不是直立靜止，零點維持 0（傾斜角可能有幾度偏差）");
}

/* -------------------------------------------------------------------------- */
/*  休眠                                                                        */
/* -------------------------------------------------------------------------- */
// IMU 切到低功耗 wake-on-motion：只開加速度、低取樣率，晃動超過門檻就把 INT 拉高並閂鎖
// （依 MPU-6500 / MPU-9250 datasheet 的 Wake-on-Motion 設定步驟）
static esp_err_t imu_enter_wom(void)
{
    static const reg_val_t k_wom[] = {
        { MPU_REG_PWR_MGMT_1,      0x01 },  // 喚醒、時脈自動選擇
        { MPU_REG_PWR_MGMT_2,      0x07 },  // 關掉陀螺儀三軸，只留加速度
        { MPU_REG_ACCEL_CONFIG2,   0x01 },  // ACCEL_FCHOICE_B=0、A_DLPF_CFG=1（184 Hz）
        { MPU_REG_INT_PIN_CFG,     0x20 },  // 高電位有效、推挽、LATCH_INT_EN=1（INT 拉高後保持，直到讀 INT_STATUS）
        { MPU_REG_INT_ENABLE,      0x40 },  // 只開 WOM 中斷
        { MPU_REG_MOT_DETECT_CTRL, 0xC0 },  // ACCEL_INTEL_EN=1、ACCEL_INTEL_MODE=1
        { MPU_REG_WOM_THR,         WOM_THRESHOLD },
        { MPU_REG_LP_ACCEL_ODR,    WOM_LP_ODR },
        { MPU_REG_PWR_MGMT_1,      0x21 },  // CYCLE=1：週期性醒來取樣，其餘時間睡
    };
    for (size_t i = 0; i < sizeof(k_wom) / sizeof(k_wom[0]); i++) {
        esp_err_t err = reg_write(s_imu_dev, k_wom[i].reg, k_wom[i].val);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

// 啟用 wake-on-motion 並等它穩定。剛切換時，頭幾筆低功耗取樣會和初始參考值比較而觸發中斷
// （實測：沒處理的話一睡下去 0.4 秒就被叫醒），所以：
//   1. 先等 300 ms，讓 IMU 取完至少兩筆樣本（7.81 Hz → 每筆約 128 ms）
//   2. 讀 INT_STATUS 清掉閂鎖，再等超過一個取樣週期，確認 D1 維持低電位；
//      又被拉高就再清一次，最多 10 次
// 回傳 false = IMU 設定失敗（不能靠晃動喚醒）
static bool imu_wom_arm(void)
{
    if (imu_enter_wom() != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    for (int i = 0; i < 10; i++) {
        uint8_t int_status = 0;
        (void)reg_read(s_imu_dev, MPU_REG_INT_STATUS, &int_status, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        if (gpio_get_level(PIN_IMU_INT) == 0) {
            return true;
        }
    }
    printf("[休眠] D1 一直是高電位（杯子還在晃？）\n");
    return true;
}

#if DEEP_SLEEP_ENABLE
// 電腦的 USB 連著（收得到 SOF 封包）時不進深度睡眠：開發、校正時序列埠才不會斷。
// 接行動電源或充電器不算連著，照樣會睡。
static bool usb_host_connected(void)
{
#if SLEEP_ON_USB
    return false;
#else
    return usb_serial_jtag_is_connected();
#endif
}

// 進入深度睡眠（IMU 已在 wake-on-motion）。醒來後會從 app_main 重新開始（RTC_DATA_ATTR 的變數會保留）
static void enter_deep_sleep(void)
{
    printf("\n[休眠] 休眠中沒有 App 連線超過 %d 分鐘，進入深度睡眠（拿起杯子或 %d 分鐘後喚醒）\n",
           DEEP_SLEEP_AFTER_MS / 60000, SLEEP_TIMER_WAKE_S / 60);
    ble_cup_shutdown();
    // MPR121 進入停止模式（約 3 µA）；醒來後開機流程會重新設定
    if (s_mpr_ready) {
        (void)reg_write(s_mpr_dev, MPR121_REG_ECR, MPR121_ECR_STOP);
    }
    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_IMU_INT, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_TIMER_WAKE_S * 1000000ULL);
    s_sleep_count++;
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_deep_sleep_start();
}
#endif

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// 休眠中量水位：連讀 10 筆（每筆隔 20 ms）餵給 cup_state 平均。杯子不是直立時不更新水位。
// 直立時休眠中的讀值視為「穩定」，套用直立不動的規則（cup_state.c）：水位上升只更新基準
// （杯子放著不動時有人倒水進去）；下降 ≥ 門檻而且連續好幾筆 → 喝水事件（用吸管喝），存 flash、有連線就送
static void doze_measure_water(bool upright)
{
    if (!s_mpr_ready) {
        return;
    }
    int got = 0;   // 這一輪成功讀到幾筆
    for (int i = 0; i < 10; i++) {
        uint16_t e0, e1;
        if (mpr121_read_filtered(&e0, &e1)) {
            cup_water_add_sample(e0, upright);
            got++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    float pct;
    // 10 筆都讀到才算一筆新的量測：讀取失敗時 cup_water_get 拿到的是之前的舊值，
    // 不能讓同一個舊值被當成「連續好幾筆都偏低」
    if (got == 10 && upright && cup_water_get(&pct) && !cup_water_holding()) {
        cup_event_t ev;
        // 時鐘要和 live_loop 的 cup_event_update 一樣（RTC 計時器），事件的開始時間才對得上
        if (cup_event_doze_water(pct, drink_log_clock_ms(), &ev)) {
            drink_log_add(&ev);   // 先存 flash，有連線就立刻送 0x07
        }
    }
}

// 休眠：IMU 改成 wake-on-motion，BLE 改用省電參數但不斷線，CPU 大部分時間自動淺眠。
// 每 250 ms 看一次 D1，杯子被拿起或晃動就返回（回到一般的即時量測）
static void doze_loop(uint32_t idle_ms, const cup_motion_t *last)
{
    uint32_t start = uptime_ms();
    bool upright = (last->pose == CUP_POSE_UPRIGHT);
    printf("\n[休眠] 已靜止 %" PRIu32 " 秒，進入休眠（藍牙保持連線，拿起杯子就會喚醒）\n", idle_ms / 1000);

    if (!imu_wom_arm()) {
        printf("[休眠] IMU wake-on-motion 設定失敗，不休眠\n");
        return;
    }
    ble_cup_set_low_power(true);
    ble_cup_send_state(CUP_STATE_DOZING, 0);

    uint32_t sub_count = ble_cup_subscribe_count();
    uint32_t resend_at = 0;   // 0 = 沒有待補送
    uint32_t last_keepalive = start, last_water_check = start, last_water_sent = start, last_battery = start;
    float sent_pct = -100.0f;
    (void)cup_water_get(&sent_pct);

    // 杯子被拿起（D1）或電腦的校正工具送指令（要即時量測）就醒來
    while (gpio_get_level(PIN_IMU_INT) == 0 && !tool_cmd_active()) {
        vTaskDelay(pdMS_TO_TICKS(DOZE_POLL_MS));
        uint32_t now = uptime_ms();
        uint32_t secs = (now - start) / 1000;

        // 休眠中也要處理事件的 ACK 與補傳（App 可能在休眠中連上）
        drink_log_poll(true);

        // App 剛連上／訂閱：1.5 秒後補送「休眠中」、最新水位和休眠前最後的姿態，
        // App 一連上就有完整畫面（休眠中平常不送姿態）
        if (ble_cup_subscribe_count() != sub_count) {
            sub_count = ble_cup_subscribe_count();
            resend_at = now + SUBSCRIBE_RESEND_MS;
        }
        if (resend_at != 0 && (int32_t)(now - resend_at) >= 0) {
            resend_at = 0;
            ble_cup_send_state(CUP_STATE_DOZING, secs);
            float pct;
            if (cup_water_get(&pct)) {
                ble_cup_send_water(pct);
            }
            ble_cup_send_pose(last->tilt_deg, (uint8_t)last->pose, false);
            ble_cup_send_attitude(last->pitch_deg, last->roll_deg);
            last_keepalive = now;
        }
        if (now - last_keepalive >= DOZE_KEEPALIVE_MS) {
            ble_cup_send_state(CUP_STATE_DOZING, secs);
            last_keepalive = now;
        }
        if (now - last_water_check >= DOZE_WATER_CHECK_MS) {
            last_water_check = now;
            doze_measure_water(upright);
            float pct;
            if (cup_water_get(&pct) &&
                (fabsf(pct - sent_pct) >= DOZE_WATER_DELTA_PCT || now - last_water_sent >= DOZE_WATER_FORCE_MS)) {
                ble_cup_send_water(pct);
                sent_pct = pct;
                last_water_sent = now;
                printf("[休眠] 已休眠 %" PRIu32 " 秒 · 水位 %.0f%% · BLE %s\n", secs, pct,
                       ble_cup_state() == BLE_CUP_SUBSCRIBED ? "連線中" : "廣播中");
            }
        }
        if (now - last_battery >= DOZE_BATTERY_MS) {
            last_battery = now;
            int pin_mv, bat_mv;
            ble_cup_set_battery(battery_read(&pin_mv, &bat_mv) ? cup_battery_percent(bat_mv) : 0xFF);
        }
#if DEEP_SLEEP_ENABLE
        if (ble_cup_state() == BLE_CUP_ADVERTISING && !usb_host_connected() && now - start >= DEEP_SLEEP_AFTER_MS) {
            enter_deep_sleep();   // 不會返回
        }
#endif
    }

    // 喚醒：IMU 重置回一般模式（同時清掉 WOM 的閂鎖），BLE 恢復快速參數
    uint32_t dozed_s = (uptime_ms() - start) / 1000;
    test_imu_id();
    cup_motion_mark_moving(uptime_ms());
    cup_event_wake(drink_log_clock_ms());   // 喚醒 = 杯子被動到，視為離開穩定
    ble_cup_set_low_power(false);
    ble_cup_send_state(CUP_STATE_ACTIVE, 0);
    printf("[休眠] %s，喚醒（休眠了 %" PRIu32 " 秒）\n", tool_cmd_active() ? "校正工具送來指令" : "偵測到移動", dozed_s);
}

// 欄寬（以終端機顯示寬度計算，中文字算 2 格）
#define W_TIME   8
#define W_E      6
#define W_LEVEL  7
#define W_TOUCH  7
#define W_IRQ    6
#define W_ANG    7
#define W_POSE   7
#define W_MOVE   5
#define W_BAT    7
#define W_BLE    7
#define W_SLEEP  6

static void print_live_header(void)
{
    printf("\n");
    print_compact_summary();
    float base;
    char f_base[16] = "--";
    if (cup_event_baseline(&base)) {
        snprintf(f_base, sizeof(f_base), "%.1f%%", base);
    }
    printf("BLE 名稱：%s　｜　水位只在直立靜止時更新，「水位%%」後面的 * 代表沿用上一次的值\n",
           ble_cup_name()[0] ? ble_cup_name() : "（BLE 未啟動）");
    printf("喝水偵測：基準水位 %s%s　｜　未確認的事件 %" PRIu32 " 筆（App 回 ACK 後才從 flash 刪除）\n",
           f_base, cup_event_in_episode() ? "（杯子拿起中）" : "", drink_log_pending());
    print_col("時間(s)", W_TIME, true);
    print_col("電極0", W_E, true);
    print_col("電極1", W_E, true);
    print_col("水位%", W_LEVEL, true);
    print_col("觸碰", W_TOUCH, true);
    print_col("IRQ數", W_IRQ, true);
    printf(" |");
    print_col("俯仰°", W_ANG, true);
    print_col("翻滾°", W_ANG, true);
    print_col("傾斜°", W_ANG, true);
    print_col("姿態", W_POSE, true);
    print_col("移動", W_MOVE, true);
    printf(" |");
    print_col("電池V", W_BAT, true);
    print_col("BLE", W_BLE, true);
    print_col("休眠", W_SLEEP, true);
    printf("\n");
    for (int i = 0; i < W_TIME + 2 * W_E + W_LEVEL + W_TOUCH + W_IRQ + 2 + 3 * W_ANG + W_POSE + W_MOVE + 2 + W_BAT + W_BLE + W_SLEEP; i++) {
        putchar('-');
    }
    putchar('\n');
}

static const char *ble_state_word(ble_cup_state_t st)
{
    switch (st) {
    case BLE_CUP_ADVERTISING: return "廣播中";
    case BLE_CUP_CONNECTED:   return "已連線";
    case BLE_CUP_SUBSCRIBED:  return "傳送中";
    default:                  return "關閉";
    }
}

static void live_loop(void)
{
    printf("\n>>> 進入即時量測（每 %d ms 讀一次感測器，序列埠每 %d ms 印一行）\n",
           SENSE_PERIOD_MS, SENSE_PERIOD_MS * PRINT_EVERY);

    // 即時顯示時讀取失敗會直接印「--」，不需要驅動再印錯誤訊息把表格打亂
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    s_mpr_irq_count = 0;   // IRQ 次數從即時顯示開始累計
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t tick = 0, row = 0;
    cup_motion_t motion = { .pose = CUP_POSE_UNKNOWN };
    bool have_motion = false;
    cup_pose_t last_sent_pose = CUP_POSE_UNKNOWN;
    bool was_connected = false;
    uint32_t connected_at_ms = 0;
    uint32_t active_since_ms = uptime_ms();               // 這次「活動中」從什麼時候開始（STATE 的秒數）
    uint32_t sub_count = ble_cup_subscribe_count();
    uint32_t state_resend_at = 0;                         // 0 = 沒有待補送
    bool have_e = false;
    uint16_t e0 = 0, e1 = 0;
    int bat_mv = -1;

    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // ---- 讀感測器 ----
        if (s_imu_ready) {
            float acc[3], gyro[3];
            if (imu_read_all(acc, gyro)) {
                cup_motion_update(acc[0], acc[1], acc[2], gyro[0], gyro[1], gyro[2], now_ms, &motion);
                have_motion = true;
            }
        }
        if (s_mpr_ready) {
            have_e = mpr121_read_filtered(&e0, &e1);
            if (have_e) {
                // 沒有 IMU 時無法判斷姿態，就當作一直是直立靜止
                cup_water_add_sample(e0, have_motion ? motion.water_ok : true);
            }
        }
        // 給電腦校正工具的即時資料（工具寫入的新校正值也在這裡套用）
        tool_cmd_feed(&(tool_sample_t){
            .have_e = have_e, .e0 = e0, .e1 = e1,
            .have_motion = have_motion, .pose = motion.pose, .moving = motion.moving,
        });

        // ---- 喝水／加水事件（沒有 IMU 時看不出拿起放下，不偵測）----
        if (have_motion) {
            float wpct = 0.0f;
            // have_e：這一輪電極有讀到。讀取失敗時水位是舊值，不拿去偵測（直立不動的規則要「連續好幾筆」都是新量到的）
            bool fresh = have_e && cup_water_get(&wpct) && !cup_water_holding();
            cup_event_t ev;
            if (cup_event_update(motion.pose, motion.moving, fresh, wpct, drink_log_clock_ms(), &ev)) {
                drink_log_add(&ev);   // 先存 flash，有連線就立刻送 0x07
            }
        }
        drink_log_poll(false);
        if (tick % BATTERY_EVERY == 0) {
            int pin_mv;
            bat_mv = battery_read(&pin_mv, &bat_mv) ? bat_mv : -1;
            ble_cup_set_battery(bat_mv < 0 ? 0xFF : cup_battery_percent(bat_mv));
        }

        // ---- BLE 推送 ----
        if (have_motion && tick % ATTITUDE_EVERY == 0) {
            ble_cup_send_attitude(motion.pitch_deg, motion.roll_deg);
        }
        bool pose_changed = have_motion && motion.pose != last_sent_pose;
        if (have_motion && (pose_changed || tick % WATER_EVERY == 0)) {
            ble_cup_send_pose(motion.tilt_deg, (uint8_t)motion.pose, motion.moving);
            last_sent_pose = motion.pose;
        }
        if (tick % WATER_EVERY == 0) {
            float pct;
            if (cup_water_get(&pct)) {
                ble_cup_send_water(pct);
            }
        }

        // App 剛連上／訂閱：1.5 秒後告訴它「活動中」
        if (ble_cup_subscribe_count() != sub_count) {
            sub_count = ble_cup_subscribe_count();
            state_resend_at = now_ms + SUBSCRIBE_RESEND_MS;
        }
        if (state_resend_at != 0 && (int32_t)(now_ms - state_resend_at) >= 0) {
            state_resend_at = 0;
            ble_cup_send_state(CUP_STATE_ACTIVE, (now_ms - active_since_ms) / 1000);
        }

        // ---- 休眠判斷 ----
        ble_cup_state_t bst = ble_cup_state();
        bool connected = (bst == BLE_CUP_CONNECTED || bst == BLE_CUP_SUBSCRIBED);
        if (connected && !was_connected) {
            connected_at_ms = now_ms;   // App 剛連上：從現在重新算閒置時間，讓 App 先看到 30 秒即時資料
        }
        was_connected = connected;
        // 閒置時間 = 從「最後一次移動」或「App 連上」（取較晚的）到現在
        uint32_t idle_ms = motion.still_ms;
        if (connected && now_ms - connected_at_ms < idle_ms) {
            idle_ms = now_ms - connected_at_ms;
        }
        int32_t sleep_left_ms = -1;
        if (have_motion) {   // 沒有 IMU（休眠後叫不醒）時不休眠
            if (idle_ms >= DOZE_IDLE_MS && !tool_cmd_active()) {   // 校正工具連著時不休眠
                doze_loop(idle_ms, &motion);   // 杯子被拿起才返回
                // 回到即時量測：重新開始計時，馬上補送一次姿態與水位
                tick = 0;
                last_sent_pose = CUP_POSE_UNKNOWN;
                active_since_ms = uptime_ms();
                connected_at_ms = active_since_ms;
                sub_count = ble_cup_subscribe_count();
                state_resend_at = 0;
                last_wake = xTaskGetTickCount();
                continue;
            }
            sleep_left_ms = (int32_t)(DOZE_IDLE_MS - idle_ms);
        }

        // ---- 序列埠顯示（校正工具連著時不印，避免和工具的 @ 回覆混在一起）----
        if (tick % PRINT_EVERY == 0 && !tool_cmd_active()) {
            if (row % LIVE_HEADER_EVERY == 0) {
                print_live_header();
            }
            char f_time[16], f_e0[8] = "--", f_e1[8] = "--", f_level[12] = "--", f_touch[16] = "--", f_irq[12] = "--";
            char f_p[12] = "--", f_r[12] = "--", f_t[12] = "--", f_pose[16] = "--", f_move[12] = "--";
            char f_bat[12] = "--", f_sleep[12] = "--";
            if (sleep_left_ms >= 0) {
                snprintf(f_sleep, sizeof(f_sleep), "%" PRId32 "s", (sleep_left_ms + 999) / 1000);
            }

            snprintf(f_time, sizeof(f_time), "%.1f", now_ms / 1000.0);
            if (s_mpr_ready) {
                if (have_e) {
                    snprintf(f_e0, sizeof(f_e0), "%u", e0);
                    snprintf(f_e1, sizeof(f_e1), "%u", e1);
                }
                uint8_t st[2];
                if (mpr121_read_touch(st)) {   // 讀觸碰狀態同時清除 MPR121 的 IRQ
                    bool t0 = st[0] & 0x01;
                    bool t1 = st[0] & 0x02;
                    snprintf(f_touch, sizeof(f_touch), "%s",
                             (st[1] & 0x80) ? "過電流" : (t0 && t1) ? "E0+E1" : t0 ? "E0" : t1 ? "E1" : "無");
                }
                snprintf(f_irq, sizeof(f_irq), "%" PRIu32, s_mpr_irq_count);
            }
            float pct;
            if (cup_water_get(&pct)) {
                snprintf(f_level, sizeof(f_level), "%.0f%s", pct, cup_water_holding() ? "*" : "");
            }
            if (have_motion) {
                snprintf(f_p, sizeof(f_p), "%+.1f", motion.pitch_deg);
                snprintf(f_r, sizeof(f_r), "%+.1f", motion.roll_deg);
                snprintf(f_t, sizeof(f_t), "%.1f", motion.tilt_deg);
                snprintf(f_pose, sizeof(f_pose), "%s", cup_pose_label(motion.pose));
                snprintf(f_move, sizeof(f_move), "%s", motion.moving ? "是" : "否");
            }
            if (bat_mv >= 0) {
                snprintf(f_bat, sizeof(f_bat), "%.2f", bat_mv / 1000.0f);
            }

            print_col(f_time, W_TIME, true);
            print_col(f_e0, W_E, true);
            print_col(f_e1, W_E, true);
            print_col(f_level, W_LEVEL, true);
            print_col(f_touch, W_TOUCH, true);
            print_col(f_irq, W_IRQ, true);
            printf(" |");
            print_col(f_p, W_ANG, true);
            print_col(f_r, W_ANG, true);
            print_col(f_t, W_ANG, true);
            print_col(f_pose, W_POSE, true);
            print_col(f_move, W_MOVE, true);
            printf(" |");
            print_col(f_bat, W_BAT, true);
            print_col(ble_state_word(ble_cup_state()), W_BLE, true);
            print_col(f_sleep, W_SLEEP, true);
            printf("\n");
            fflush(stdout);
            row++;
        }

        tick++;
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SENSE_PERIOD_MS));
    }
}

/* ========================================================================== */
/*  主程式                                                                      */
/* ========================================================================== */
void app_main(void)
{
    // 這次開機是冷開機，還是從深度睡眠醒來（v6 起用 esp_sleep_get_wakeup_causes 的 bitmap）
    uint32_t causes = esp_sleep_get_wakeup_causes();
    bool by_motion = causes & BIT(ESP_SLEEP_WAKEUP_EXT1);
    bool by_timer = causes & BIT(ESP_SLEEP_WAKEUP_TIMER);
    s_woke_from_sleep = by_motion || by_timer;

    // NVS：喝水事件（drink_log.c）和 BLE 控制器的 PHY 校正資料都存在這裡，整個程式只初始化這一次。
    // 格式不相容時清掉重來（未確認的事件會不見，正常只有改分割表或 IDF 大改版才會發生）
    esp_err_t nvs_err = nvs_flash_init();
    bool nvs_erased = false;
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
        nvs_erased = true;
    }

    // USB Serial/JTAG：晶片重開後電腦要重新連上序列埠，先等一下以免開頭的輸出漏掉。
    // 從休眠醒來時跳過，盡快開始廣播
    if (!s_woke_from_sleep) {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    printf("\n\n");
    print_rule('=');
    printf("  smart_cup 硬體接線測試（XIAO ESP32-C6）\n");
    printf("  SDA=D4(GPIO22)  SCL=D5(GPIO23)  IMU INT=D1(GPIO1)  MPR121 IRQ=D2(GPIO2)  電池=D0(GPIO0)\n");
    print_rule('=');
    if (nvs_err != ESP_OK) {
        printf("  警告：NVS 初始化失敗（%s），喝水事件不會存進 flash、BLE 可能也起不來\n", esp_err_to_name(nvs_err));
    } else if (nvs_erased) {
        printf("  警告：NVS 格式不相容，已清空（之前未確認的喝水事件不見了）\n");
    }
    if (s_woke_from_sleep) {
        printf("  從休眠喚醒（累計睡過 %" PRIu32 " 次）：%s\n", s_sleep_count,
               by_motion ? "杯子被拿起或晃動（IMU → D1）" : "計時器定期喚醒");
    }

    // GPIO 中斷服務（D1、D2 的計數都靠它）
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("警告：GPIO 中斷服務安裝失敗（%s），INT/IRQ 計數會是 0\n", esp_err_to_name(err));
    }

    printf("[1/6] I2C 掃描 0x%02X～0x%02X …\n", I2C_SCAN_FIRST, I2C_SCAN_LAST);
    test_i2c_scan();

    printf("[2/6] MPR121 重置與設定 …\n");
    test_mpr121();

    printf("[3/6] IMU 識別與設定 …\n");
    test_imu_id();
    if (!s_woke_from_sleep) {
        printf("      IMU 靜止加速度（請讓板子保持不動）…\n");
        test_imu_accel();
        printf("[4/6] IMU INT 中斷計數 1 秒 …\n");
        test_imu_int();
    } else {
        // 醒來時杯子可能正在動，靜止測試沒意義；INT 測試要 1 秒，也略過。
        // D1 仍要設成輸入（gpio_config 會把喚醒時用過的 LP IO 切回一般 GPIO），休眠前要讀它的電位
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << PIN_IMU_INT,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&io);
        item_set(&s_items[IT_IMU_ACC], ST_MANUAL, "從休眠喚醒，略過");
        if (by_motion) {
            item_set(&s_items[IT_IMU_INT], ST_PASS, "D1 成功把 ESP32 從休眠叫醒");
        } else {
            item_set(&s_items[IT_IMU_INT], ST_MANUAL, "從休眠喚醒，略過");
        }
    }

    printf("[5/6] 電池電壓 …\n");
    test_battery();

    printf("[6/6] MPR121 IRQ 計數準備 …\n");
    setup_mpr_irq();

    print_summary();
    printf("  提示：在 monitor 按 Ctrl+T 再按 Ctrl+R 可以重開機，重新跑一次測試\n");

    // 姿態零點：冷開機時杯子應該直立放在桌上。從休眠醒來時沿用 RTC 記憶體裡的零點
    //（醒來時杯子可能拿在手上，不能重新歸零）
    if (!s_woke_from_sleep || !cup_motion_has_zero()) {
        motion_zero();
    }

#if CONFIG_PM_ENABLE
    // 電源管理：沒事做時 CPU 降到 40 MHz、自動淺眠（BLE 控制器會在連線事件之間一起睡）。
    // 接著電腦 USB 時不會淺眠（CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION），序列埠才不會斷
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = true,
    };
    err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        printf("  電源管理設定失敗（%s），不會自動淺眠\n", esp_err_to_name(err));
    }
#endif

    // 喝水事件：載入 flash 裡還沒被 App 確認的事件（BLE 啟動前，一連上就能補傳）
    if (nvs_err == ESP_OK) {
        drink_log_init(s_woke_from_sleep);
    }

    // 水位校正值（flash 裡有就用，沒有用預設）、電腦校正工具的序列埠指令（tools/cup_tool.py）
    tool_cmd_init();

    // BLE：廣播 SmartCup-XXXX，App 連上並訂閱後開始推送水位、姿態
    err = ble_cup_init();
    if (err != ESP_OK) {
        printf("  BLE 初始化失敗（%s），只有序列埠顯示\n", esp_err_to_name(err));
    }

    live_loop();
}
