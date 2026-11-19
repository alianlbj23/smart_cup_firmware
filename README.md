# smart_cup_firmware

智慧水杯韌體。用貼在保護套內壁的銅膠帶電極，隔著玻璃杯量測水位。

開機時自動檢查每個模組的接線，接著每 100 ms 量一次水位和姿態，透過 **BLE** 推送給手機 App [smart_cup_app](https://github.com/alianlbj23/smart_cup_app)，序列埠也會每 0.5 秒印一行數值。杯子靜止 30 秒就進入**休眠**省電（藍牙保持連線），拿起杯子就會醒來（見[第 6 節](#6-休眠)）。

| 項目 | 內容 |
|---|---|
| 主板 | Seeed Studio XIAO ESP32-C6（4MB Flash，USB-C = USB Serial/JTAG） |
| 電容感測 | MPR121（HW-017 模組），I2C `0x5A` |
| IMU | MPU-9250 / MPU-6500，I2C `0x68` |
| 電池 | 702030 鋰電池，焊在 XIAO 背面 BAT+／BAT− |
| 開發環境 | ESP-IDF **v6.1** |

---

## 用法總覽

**韌體只有一個版本**，沒有另外的「校正版」。校正就是用同一份韌體量出空杯、滿杯的讀值，把兩個數字寫進程式，再燒錄一次。

| 你要做的事 | 怎麼做 | 要改程式嗎 |
|---|---|---|
| **平常使用**：看水位、姿態 | 燒錄一次，之後用 App 連線（見[第 5 節](#5-ble-與-app)），或開 monitor 看「水位%」欄（見[第 3 節](#3-燒錄)） | 不用 |
| **校正**：第一次組裝、換杯子、重貼電極之後 | 量空杯、滿杯 → 改 `main/cup_state.c` 的兩個數字 → 重新燒錄。見[第 4 節](#4-水位校正) | 改兩個 `#define` |

```
                 ┌──────────── 校正（只在需要時做）─────────────┐
                 │                                              │
燒錄韌體 ──► 量空杯讀值 ──► 量滿杯讀值 ──► 改 LEVEL_E0_EMPTY ──► 重新燒錄 ──► 平常使用：看「水位%」
                                            LEVEL_E0_FULL
```

目前的校正值（空杯 403、滿杯 298）是 2026-11-19 用現在這個杯子和電極量的。杯子、電極都沒換的話，**直接燒錄就能用，不必再校正**。

---

## 1. 接線

完整接線圖、電極尺寸和接線順序都在 **[docs/wiring.pdf](docs/wiring.pdf)**。

![接線圖](docs/wiring-1.png)

![保護套內壁電極尺寸](docs/wiring-2.png)

> PDF 第 1 頁的接線圖右側被切掉了，XIAO 背面腳位和電池那一段看不到。以下面的接線表為準。

| XIAO 腳位 | 接到 | 說明 |
|---|---|---|
| 3V3 | MPR121 3.3V、IMU VCC、IMU NCS | 用電池供電時 5V 腳位沒電，全部接 3V3 |
| GND | MPR121 GND、MPR121 ADD、IMU GND、IMU AD0、**接地條** | ADD、AD0 接地 → 位址固定為 0x5A、0x68 |
| D4（GPIO22） | MPR121 SDA、IMU SDA | I2C SDA（共用） |
| D5（GPIO23） | MPR121 SCL、IMU SCL | I2C SCL（共用） |
| D1（GPIO1） | IMU INT | 高電位有效；休眠時 IMU 偵測到晃動會拉高 D1，把水杯叫醒 |
| D2（GPIO2） | MPR121 IRQ | 低電位有效、開汲極（程式會開內部上拉） |
| D0（GPIO0） | 電池分壓中點 | BAT+ → 200kΩ → D0 → 200kΩ → GND，讀值 ×2 = 電池電壓 |
| 背面 BAT+／BAT− | 電池紅線／黑線 | 焊之前用三用電表確認正負極 |

| MPR121 電極 | 銅膠帶 | 位置 |
|---|---|---|
| 0 | **主條**（LEVEL） | 寬 15 mm，從保護套底部貼到最高水位，貼在把手對面 |
| 1 | **RL**（液體參考） | 寬 15 × 高 10 mm，在主條旁邊、間隔 8 mm，底邊對齊杯子內底 |
| —（接 XIAO GND） | **接地條** | 寬 15 mm，從底貼到頂，貼在主條**正對面** |

銅膠帶朝杯子那一面都要貼 Kapton，電極不碰水，是隔著玻璃感應。

---

## 2. 開發環境

ESP-IDF v6.1 由 EIM 安裝在 `~/.espressif/v6.1/esp-idf`。`idf.py` 預設不在 PATH，**每開一個新的終端機都要先執行**：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
```

用 VS Code 的話，要開**本專案的資料夾**，不要開上一層。開錯的話，狀態列的建置、燒錄按鈕會在錯的目錄執行，出現「沒有 CMakeLists.txt」或「找不到 sdkconfig」的錯誤。

---

## 3. 燒錄

### 第一次（剛 clone 下來）

`sdkconfig` 沒有放進 git，第一次要先指定晶片，ESP-IDF 會依 `sdkconfig.defaults` 產生設定：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32c6
```

### 編譯、燒錄、看輸出

用 USB-C 線接上 XIAO，序列埠通常是 `/dev/ttyACM0`：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

- 離開 monitor：`Ctrl+]`
- 在 monitor 裡重開機、重跑開機測試：`Ctrl+T` 再按 `Ctrl+R`
- 燒錄失敗、找不到板子時：按住 XIAO 的 **BOOT** 鍵，再按一下 **RESET**，放開 BOOT，就會進入下載模式，然後重新燒錄。燒完按一次 RESET。

### 開機後會看到什麼

1. 等 3 秒，讓 USB 重新連線。
2. 自動跑 6 項測試，印出總結表：I2C 掃描、MPR121、IMU、IMU INT、電池、MPR121 IRQ。失敗的項目會列出要檢查哪幾條線。
3. 記錄姿態零點（開機時杯子要**直立放在桌上**），啟動 BLE 並開始廣播 `SmartCup-XXXX`。
4. 進入即時量測，每 0.5 秒印一行：

```
BLE 名稱：SmartCup-E0CE　｜　水位只在直立靜止時更新，「水位%」後面的 * 代表沿用上一次的值
 時間(s) 電極0 電極1  水位%   觸碰 IRQ數 |  俯仰°  翻滾°  傾斜°   姿態 移動 |  電池V    BLE  休眠
     6.5   338   522     62     無     0 |   +0.0   -0.0    0.0   直立   否 |   3.98 傳送中   24s
```

| 欄位 | 意思 |
|---|---|
| 電極0 | 主條的讀值（0～1023）。**水越多，數字越小** |
| 電極1 | RL 的讀值。只有水位很低時變化大，目前只顯示、不參與計算 |
| 水位% | 由電極0 換算（見下一節）。傾斜或移動時沿用上一次的值，後面加 `*` |
| 觸碰 | MPR121 內建的觸碰判斷。**不能拿來判斷水位**：開機時當下的值會被當成基準線 |
| 俯仰°／翻滾° | 前後／左右傾斜角，已扣掉開機零點 |
| 傾斜° | 相對直立的總傾斜角 0～180° |
| 姿態 | 直立／傾斜／飲用中／倒下，判斷方式見[第 5 節](#5-ble-與-app) |
| 移動 | 最近 0.5 秒內有沒有轉動或晃動 |
| BLE | 廣播中（等 App 連線）／已連線／傳送中（App 已訂閱資料） |
| 休眠 | 還剩幾秒進入休眠（見[第 6 節](#6-休眠)） |

**只插 USB、沒接電池時**，電池欄會顯示 0.5～0.9 V，總結表裡電池那項判定失敗。這是正常現象：BAT+ 沒接東西，讀到的是殘留電荷慢慢放掉的電壓。

---

## 4. 水位校正

### 原理

MPR121 讀到的是電極充電後的電壓。水越多，主條的電容越大，讀值就越小。實測主條讀值跟水位高度接近直線關係，所以用**空杯、滿杯兩點**做直線換算：

```
水位% = (空杯值 − 電極0) / (空杯值 − 滿杯值) × 100     （超出 0～100 的部分截掉）
```

### 校正程式碼

水位換算在 [main/cup_state.c](main/cup_state.c)：

```c
// 水位校正值：電極0 在空杯、滿杯時的讀值
#define LEVEL_E0_EMPTY          403
#define LEVEL_E0_FULL           298

static float e0_to_pct(float e0)
{
    float pct = (LEVEL_E0_EMPTY - e0) * 100.0f / (float)(LEVEL_E0_EMPTY - LEVEL_E0_FULL);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
}
```

傳進來的 `e0` 是杯子**直立靜止**時最近 10 筆（約 1 秒）電極0 讀值的平均。杯子傾斜或移動時不更新，沿用上一次的水位。

MPR121 的充電時間設定在 [main/main.c](main/main.c)：

```c
// MPR121 0x5D：充電時間 1 µs（預設 0.5 µs）。讀值和水位變化量約放大一倍，雜訊不變
#define MPR121_CONFIG2_RUN      0x44
```

### 什麼時候要重新校正

- 第一次組裝好
- 換了杯子或保護套
- 重貼、改動銅膠帶或電極線
- 改了 MPR121 的充電電流（0x5C）或充電時間（0x5D）

以上都沒發生的話，不用校正，直接用[第 3 節](#3-燒錄)燒錄就好。

### 校正步驟

用的是同一份韌體，不用另外燒「校正版」。量的時候手不要碰杯子或保護套：手的電容比水位變化大很多，讀數會亂跳。

**步驟 1：燒錄韌體**（板子上已經是這份韌體的話，可以跳過）

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py -p /dev/ttyACM0 flash
```

**步驟 2：量空杯**

杯子放進保護套、**不裝水**、杯壁擦乾，手離開杯子，然後執行：

```bash
python tools/read_levels.py
```

如果開著 `idf.py monitor`，要先按 `Ctrl+]` 關掉。同一個序列埠一次只能給一個程式讀。

錄 15 秒後，最後幾行會像這樣。記下 **「電極0 平均」**，這裡是 403.3：

```
有效筆數 15
電極0 平均 403.3（402～404）
電極1 平均 541.5（541～542）
水位% 平均 6.8（6～8）
```

最後一行的水位% 是用舊校正值算的，校正時不用管。

**步驟 3：量滿杯**

倒水到**最高水位**，手離開杯子，再執行一次 `python tools/read_levels.py`，記下「電極0 平均」，例如 298.0。

**步驟 4：把數字寫進程式**

打開 [main/cup_state.c](main/cup_state.c)，搜尋 `LEVEL_E0_EMPTY`，把兩個數字改成步驟 2、3 量到的值，四捨五入成整數：

```c
#define LEVEL_E0_EMPTY          403     // 空杯時電極0 的讀值   ← 步驟 2
#define LEVEL_E0_FULL           298     // 滿杯時電極0 的讀值   ← 步驟 3
```

**步驟 5：重新燒錄並驗證**

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

`flash` 會自動重新編譯，不用另外執行 `idf.py build`。燒完看「水位%」欄：空杯應顯示約 0%，滿杯約 100%。完成後這份韌體就是正式使用的版本。

#### 關於 tools/read_levels.py

- 預設讀 `/dev/ttyACM0`、錄 15 秒。可以用 `-p` 改序列埠、用 `-s` 改秒數，例如 `python tools/read_levels.py -p /dev/ttyACM1 -s 20`。
- 打開序列埠時板子會重開機，跑完開機測試大約要 5 秒，所以秒數不要少於 10 秒。重開機前的舊資料，腳本會自動丟掉。
- 要在 ESP-IDF 的 Python 環境執行（先 `source` 過 activate 腳本），裡面已經有 pyserial。
- 不想用腳本的話，開 monitor 直接看「電極0」欄，抓個大概的平均值也可以。

### 目前的校正數據（2026-11-19）

| 狀態 | 電極0（0.5 µs） | 電極0（1 µs，目前設定） | 電極1（1 µs） | 顯示水位 |
|---|---|---|---|---|
| 空杯 | 205 | **403**（±1） | 541 | 0% |
| 目測半杯 | 177.5 | 337.5～345.5 | 521～522 | 55%～63% |
| 滿杯 | 153 | **298**（±3） | 519 | 100% |

- 空杯到滿杯總共變化約 105，雜訊約 ±2，所以解析度大約是 2%。
- 目測半杯顯示 55%～63%，原因還沒確認：可能是杯子上寬下窄，也可能是倒水不準。電極量的是**水的高度**，不是容量。需要更準時，用尺量水深來比對，或加入半杯當第三個校正點，改成兩段直線換算。

### 還沒做

- [docs/wiring.pdf](docs/wiring.pdf) 建議的 TI TIDA-00317 算法：用 RL 當液體參考，算 `水位 = RL 高度 × (主條 − 空杯主條) ÷ (RL − 空杯 RL)`，可以抵銷水質、溫度造成的變化。目前只用主條兩點校正。
- 空杯值的自動重新校正：杯子移動、環境改變時，空杯讀值會漂移約 10。

---

## 5. BLE 與 App

韌體開機後會一直廣播，App [smart_cup_app](https://github.com/alianlbj23/smart_cup_app) 按「配對水杯」就能找到它。

### 連線

1. 杯子**直立放在桌上**再接上電源（USB 或電池）。開機時的姿態會被當成「直立」的零點。
2. 等序列埠出現 `[BLE] 開始廣播：SmartCup-XXXX`。XXXX 是 BLE 位址的最後兩個 byte，例如 `SmartCup-E0CE`。
3. App 按「配對水杯」，從清單選 `SmartCup-XXXX`。連上後 App 會自動對時、訂閱資料，序列埠的 BLE 欄會變成「傳送中」。
4. 斷線後韌體會自動重新廣播；App 配對過的水杯下次會自動重連。水杯休眠時連線不斷，App 顯示「水杯休眠中」，拿起杯子就恢復（見[第 6 節](#6-休眠)）。

### 送給 App 的資料

格式跟 App 的 `lib/services/cup_protocol.dart` 一致，程式在 [main/ble_gatt_svr.c](main/ble_gatt_svr.c)。

| UUID（`3c8b00NN-efc1-4061-946c-e5916790df93`） | 方向 | 內容 |
|---|---|---|
| `…0001` Service | | |
| `…0002` Time Sync | App → 杯，Write | Unix 秒數（uint32 LE），連線後 App 會寫一次 |
| `…0003` Data Notify | 杯 → App，Notify | 9 bytes：type(1) + value(uint32 LE) + 時間戳(uint32 LE) |
| `…0004` ACK | App → 杯，Write | `0x01` = 收到 |
| `…0005` Device Status | 杯 → App，Read/Notify | `[電量 %, 韌體 major, minor, patch]`，電量 `0xFF` = 未知（沒接電池） |

| type | 名稱 | 頻率 | value |
|---|---|---|---|
| `0x02` | WATER | 1 次/秒（休眠中：變化 ≥ 1% 或每 30 秒） | bits 24..0 = 水量 25-bit 分數（0～2²⁵−1），bits 28..25 = 刻度 0～10 |
| `0x04` | POSE | 1 次/秒，姿態改變時立刻送（休眠中不送） | bits 15..0 = 傾斜角 0.1°，bits 23..16 = 姿態（1 直立、2 傾斜、3 飲用中、4 倒下），bits 31..24 = 移動中 |
| `0x05` | ATTITUDE | 5 次/秒（休眠中不送） | bits 15..0 = pitch（int16，0.1°），bits 31..16 = roll（int16，0.1°）。即時資料，App 不回 ACK |
| `0x06` | STATE | 狀態改變時、App 訂閱後 1.5 秒；休眠中每 10 秒 | bits 7..0 = 狀態（0 活動中、1 休眠中），bits 31..8 = 進入目前狀態幾秒 |
| `0x03` | RAW | 目前不送 | 電極0 讀值，除錯用 |

開機後還沒量到有效水位之前（約 1 秒），不會送 WATER。

### 姿態怎麼判斷

都在 [main/cup_state.c](main/cup_state.c) 開頭，可以依實測調整：

| 姿態 | 條件 |
|---|---|
| 直立 | 傾斜 < 20°（已經是直立時，超過 25° 才離開） |
| 倒下 | 傾斜 ≥ 60°，而且連續靜止 2 秒 |
| 飲用中 | 傾斜 ≥ 35°（不是倒下） |
| 傾斜 | 其他 |
| 移動中 | 陀螺儀 > 10 度/秒，或加速度偏離 1 g 超過 0.1 g；之後 0.5 秒內都算移動中 |

- **水位只在直立、而且靜止 1 秒以上時更新**。傾斜、移動、拿在手上時沿用上一次的值，避免水面晃動或手的電容造成誤判。
- 俯仰角（pitch）正值 = 杯口往前，翻滾角（roll）正值 = 杯口往右。實際裝好後，如果 App 上的方向跟實際相反，把 `cup_state.c` 的 `PITCH_SIGN` 或 `ROLL_SIGN` 改成 `-1.0f`。
- 開機時如果杯子不是直立靜止（傾斜超過 10° 或在晃動），就不記零點，傾斜角可能會差幾度。

### 用電腦測試 BLE（不用 App）

[tools/ble_check.py](tools/ble_check.py) 會模擬 App 的流程：掃描 `SmartCup-`、對時、讀電量、訂閱資料、回 ACK，最後統計收到的封包。電腦要有藍牙，先裝 [bleak](https://pypi.org/project/bleak/)：

```bash
python3 -m venv ~/blevenv && ~/blevenv/bin/pip install bleak
~/blevenv/bin/python tools/ble_check.py 10     # 收 10 秒
```

正常的輸出（10 秒內 WATER、POSE 各約 10 筆，ATTITUDE 約 50 筆）：

```
找到 SmartCup-E0CE 58:E6:C5:12:E0:CE
Service 存在: True
Device Status: [255, 0, 2, 0]（電量 未知，韌體 0.2.0）
10 秒內收到： {'0x02': 10, '0x04': 10, '0x05': 50}
  最後一筆 WATER 61.14% 刻度 6
  最後一筆 POSE 直立 傾斜 0.2° 移動 0
  最後一筆 ATTITUDE pitch -0.1° roll -0.1°
```

App 連著水杯時，電腦連不上（一次只能連一個）。

---

## 6. 休眠

杯子靜止 30 秒就進入「休眠」：**藍牙連線保持不斷**，App 上會顯示「水杯休眠中」，拿起杯子馬上恢復即時資料。

### 休眠時做什麼

| 項目 | 活動中 | 休眠中 |
|---|---|---|
| BLE 連線 | 連線間隔 30～50 ms | **保持連線**，連線間隔拉長到 0.5～1 秒（沒連線時廣播放慢到每秒一次） |
| CPU | 每 100 ms 讀感測器 | 大部分時間自動淺眠（esp_pm），每 250 ms 醒來看一下 D1 |
| IMU | 一般模式 | 低功耗 wake-on-motion，晃動超過 48 mg 就把 D1 拉高 |
| 水位 | 每秒送 | 每 5 秒量一次，變化 ≥ 1% 立刻送，否則每 30 秒送一次（加水時杯子不一定會動） |
| 姿態 | POSE 每秒、ATTITUDE 每秒 5 次 | 不送 |
| STATE（0x06） | 狀態改變、App 訂閱後送 | 每 10 秒送一次，讓 App 知道還在休眠 |

- **什麼時候休眠**：從「最後一次移動」或「App 連上」（取較晚的）起算，靜止 30 秒。App 剛連上時一定先有 30 秒即時資料。
- **什麼時候醒**：拿起或晃動杯子，D1 被 IMU 拉高，250 ms 內恢復，App 收到 STATE「活動中」後畫面回到即時模式。
- **休眠中被 App 連上**：訂閱後 1.5 秒補送「休眠中」、最新水位和休眠前最後的姿態，App 一連上就有完整畫面（太早送 App 會漏掉，實測過）。
- **接著電腦 USB 時**：休眠流程照常（App 照樣顯示休眠中），但 CPU 不會真的淺眠（`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`），序列埠 monitor 不會斷。

序列埠會印出 `[休眠] 已靜止 30 秒，進入休眠（藍牙保持連線，拿起杯子就會喚醒）`、`[休眠] 偵測到移動，喚醒`。

### 耗電

這塊板子沒有外接 32 kHz 晶振，BLE 淺眠時要用主晶振當時脈，所以淺眠沒辦法省到很低。Espressif 的 `power_save` 範例文件列出 ESP32-C6「light sleep（main XTAL）」約 **3.3 mA**（官方數據，我們沒有實測）。702030 電池（約 300～400 mAh）粗估大概能撐 **4～5 天**休眠。

要更省電，可以打開深度睡眠（下一節），或之後在板子上加 32 kHz 晶振（官方數據淺眠約 34 µA）。

### 深度睡眠（預設關閉）

把 [main/main.c](main/main.c) 的 `DEEP_SLEEP_ENABLE` 改成 1 之後：休眠中**沒有 App 連線**超過 30 分鐘（`DEEP_SLEEP_AFTER_MS`），就進深度睡眠，藍牙關閉，非常省電。拿起杯子（IMU → D1，ext1 喚醒）或每 30 分鐘（`SLEEP_TIMER_WAKE_S`）會醒來。醒來後零點和水位從 RTC 記憶體沿用；App 找不到水杯時會每 20 秒背景重試，醒來後自動連回。接著電腦 USB 時不會進深度睡眠。

### 參數

都在 [main/main.c](main/main.c) 開頭：

| 參數 | 預設 | 說明 |
|---|---|---|
| `DOZE_IDLE_MS` | 30000 | 靜止多久就休眠（ms） |
| `DOZE_KEEPALIVE_MS` | 10000 | 休眠中多久送一次 STATE |
| `DOZE_WATER_CHECK_MS` / `DOZE_WATER_FORCE_MS` / `DOZE_WATER_DELTA_PCT` | 5000 / 30000 / 1.0 | 休眠中量水位的間隔、沒變化時多久送一次、變化多少就立刻送 |
| `SUBSCRIBE_RESEND_MS` | 1500 | App 訂閱後多久補送目前狀態 |
| `WOM_THRESHOLD` | 12 | 晃動喚醒門檻，單位 4 mg（12 = 48 mg）。太敏感就調大 |
| `WOM_LP_ODR` | 5 | 休眠時 IMU 的取樣率代碼（5 = 7.81 Hz） |
| `DEEP_SLEEP_ENABLE` | 0 | 1 = 休眠中沒連線太久就進深度睡眠 |

BLE 的連線間隔在 [main/ble_gatt_svr.c](main/ble_gatt_svr.c) 的 `k_conn_fast`、`k_conn_doze`。手機不一定照要求的間隔，以手機決定的為準（序列埠會印出 `[BLE] 連線間隔 … ms`）。

### 怎麼測試

- **用 App**：連上後放著不動 30 秒，畫面出現「水杯休眠中」；拿起杯子，畫面回到即時。全程不應該出現斷線。
- **用電腦**：[tools/ble_check.py](tools/ble_check.py) 會把每一筆 STATE 印出來：

```
~/blevenv/bin/python tools/ble_check.py 35
    0.3s  STATE 休眠中（已 54 秒）
   10.4s  STATE 休眠中（已 65 秒）
```

- **注意**：同一時間只能有一個 App 連著水杯。Linux 上 App 如果沒有正常關閉（例如被強制結束），BlueZ 會留著連線，水杯就不會廣播，其他程式都找不到它。用 `bluetoothctl disconnect <位址>` 斷開。
- 深度睡眠的測試方式（看 `/dev/ttyACM0` 在不在、[tools/ble_watch.py](tools/ble_watch.py) 看廣播時間軸、測試版參數）寫在 [HANDOFF.md](HANDOFF.md)。

2026-11-19 實測：休眠中連線全程不斷；在休眠中連上，1.4 秒內 App 就拿到「休眠中」、水位和姿態；晃杯子約 5 秒內 App 回到即時畫面，靜止 30 秒後再次休眠。

---

## 7. 專案結構

```
main/main.c          接線測試、即時量測迴圈、序列埠顯示、休眠（淺眠／深度睡眠）、MPR121／IMU／電池讀取
main/cup_state.c/.h  水位換算與保持、姿態（俯仰／翻滾／傾斜、分類、移動判斷）、電量 %
main/ble_gatt_svr.c/.h  BLE GATT Server（NimBLE）：廣播、對時、Data Notify、Device Status
main/CMakeLists.txt  用到的 ESP-IDF 元件（esp_driver_i2c、esp_driver_gpio、esp_adc、esp_timer、bt、nvs_flash、esp_driver_usb_serial_jtag、esp_pm）
sdkconfig.defaults   主控台走 USB Serial/JTAG、4MB Flash、NimBLE、電源管理（自動淺眠）
tools/read_levels.py 校正用：錄一段電極讀值並算平均
tools/ble_check.py   用電腦模擬 App 測試 BLE
tools/ble_watch.py   休眠測試：印出 BLE 廣播出現／消失的時間軸
docs/wiring.pdf      接線圖、電極尺寸、接線順序
HANDOFF.md           開發交接筆記（ESP-IDF v6 注意事項、已踩過的坑）
```
