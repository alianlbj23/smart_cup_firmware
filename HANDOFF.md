# smart_cup_test 交棒說明

> 給接手的 Claude 對話：開始動手前先完整讀這份文件。這裡記錄了目前狀態，以及已經踩過、不用再查一次的坑。

## 1. 目前進度（2026-11-19）

- **已完成**：硬體接線測試程式 `main/main.c`，已燒錄實測。I2C、MPR121、IMU、IMU INT 全部通過；電池項目失敗是因為沒接電池（見第 5 節）
- **已完成**：銅膠帶電極接上，即時顯示新增「水位%」欄（電極0 兩點直線校正）
- **已完成**：BLE（NimBLE，`main/ble_gatt_svr.c`），協定跟 smart_cup_app 的 `cup_protocol.dart` 一致，另外新增 ATTITUDE（0x05）。用 `tools/ble_check.py` 和 App 實測都通過
- **已完成**：姿態（俯仰／翻滾／傾斜、直立／傾斜／飲用中／倒下、移動判斷）與「水位只在直立靜止時更新」，在 `main/cup_state.c`
- **還沒開始**：深度睡眠、RL 參考電極的 TI 比例算法、空杯自動重新校正

### BLE 實作備忘

- 協定細節、頻率、姿態門檻都寫在 README 第 5 節。改協定時 App（smart_cup_app）的 `lib/services/cup_protocol.dart` 要同步改
- 開 BLE 之後要重新產生 sdkconfig（`idf.py set-target esp32c6`），因為舊的 sdkconfig 裡 `CONFIG_BT_ENABLED` 是關的，sdkconfig.defaults 不會覆蓋已存在的設定
- NimBLE 的 API 照 `examples/bluetooth/nimble/bleprph` 寫；送資料用 `ble_gatts_notify_custom()`，om 由 NimBLE 釋放
- 128-bit UUID 用 `BLE_UUID128_INIT` 時是 little-endian（倒過來寫），characteristic 編號在第 12 個 byte
- 廣播封包放不下名稱（flags 3 + UUID128 18 bytes），名稱 `SmartCup-XXXX` 放在 scan response
- App 寫入用的是 write with response，characteristic 只開 `BLE_GATT_CHR_F_WRITE`
- 這台開發機有藍牙（hci0），可以用 bleak 直接測；App 連著時電腦連不上

### 水位量測結果（2026-11-19 實測）

- **電極0 是水位電極**：水越多讀值越小，跟水位接近直線關係
- **電極1 是 RL**（杯底 10 mm 的液體參考），變化小（空杯→滿杯約 −22），大部分變化發生在剛開始倒水時，跟它的位置相符。目前只顯示、不參與計算；`docs/wiring.pdf` 建議的 TI TIDA-00317 比例算法會用到它，還沒實作
- 校正步驟、燒錄步驟寫在 `README.md`；校正錄值用 `tools/read_levels.py`
- MPR121 的 0x5D 從預設 0x24 改成 **0x44**（充電時間 0.5 µs → 1 µs），讀值和變化量約放大一倍，雜訊不變（約 ±2）

| 狀態 | 電極0（0.5 µs） | 電極0（1 µs，目前設定） | 電極1（1 µs） |
|---|---|---|---|
| 空杯 | 205 | **403** | 541 |
| 目測半杯 | 177.5 | 337.5～345.5 | 521～522 |
| 滿杯 | 153 | **298** | 519 |

- 校正值在 `main.c` 的 `LEVEL_E0_EMPTY = 403`、`LEVEL_E0_FULL = 298`，公式：`水位% = (403 − 電極0) / (403 − 298) × 100`，超出 0～100 的部分截掉
- 目測半杯顯示 55%～62.5%，原因沒有確認（杯子上寬下窄？倒水不準？）。電容電極量的是**高度**不是**容量**；需要更準時，用尺量水深，或加入半杯當第三個校正點，改成兩段直線換算
- 空杯讀值有漂移：第一次量 218、之後量 205（0.5 µs），杯子移動、手靠近都會影響。正式產品要考慮自動重新校正空杯
- 換杯子、改電極、改 0x5C/0x5D 之後，都要重新量空杯、滿杯
- 「觸碰」欄不適合拿來判斷水位：開機時 MPR121 會把當下的值當成基準線，滿杯開機時就永遠不會觸發

## 2. 硬體

| 項目 | 內容 |
|---|---|
| 主板 | Seeed Studio XIAO ESP32-C6（target `esp32c6`，4MB Flash，USB-C = USB Serial/JTAG） |
| 電容感測 | MPR121（HW-017 模組），I2C `0x5A`（ADD→GND） |
| IMU | MPU-9250 / MPU-6500 模組，I2C `0x68`（AD0→GND、NCS→3V3） |
| 電池 | 702030 鋰電池，焊在 XIAO 背面 BAT+/BAT- |
| 電池分壓 | BAT+ → 200kΩ → D0 → 200kΩ → GND |
| 電極 | 銅膠帶貼在保護套內壁，依 `docs/wiring.pdf`：MPR121 0 = 主條（水位）、1 = RL（杯底 10 mm 液體參考）、接地條在主條正對面接 XIAO GND |

| XIAO 腳 | GPIO | 用途 |
|---|---|---|
| D4 | GPIO22 | I2C SDA（共用） |
| D5 | GPIO23 | I2C SCL（共用） |
| D1 | GPIO1 | IMU INT（高電位有效、推挽） |
| D2 | GPIO2 | MPR121 IRQ（低電位有效、開汲極，要開內部上拉） |
| D0 | GPIO0 | 電池分壓，ADC1 通道 0 |

## 3. 開發環境（重要）

- **ESP-IDF v6.1**，由 EIM 安裝在 `~/.espressif/v6.1/esp-idf`
- `idf.py` **預設不在 PATH**。每個 shell 都要先執行：
  ```bash
  source ~/.espressif/tools/activate_idf_v6.1.sh
  ```
- VS Code ESP-IDF 擴充套件版本 2.3.0。**VS Code 必須開 `/home/user/workspace/smart_cup_test` 這個資料夾。** 如果開的是上一層 `/home/user/workspace`，狀態列的建置、燒錄按鈕會對錯的目錄執行，出現「沒有 CMakeLists.txt」或「找不到 sdkconfig」的錯誤。擴充套件的 log 在 `~/.vscode/extensions/espressif.esp-idf-extension-2.3.0/esp_idf_vsc_ext.log`。
- `/home/user/workspace/build` 是上一層開錯資料夾時留下的空資料夾，跟本專案無關。
- 編譯：`idf.py build`。燒錄並開監看：`idf.py -p /dev/ttyACM0 flash monitor`

### ESP-IDF v6 跟 v5 的差異（已查證，寫程式時要注意）

- **編譯警告預設當成錯誤**（`CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=n`）。`uint32_t` 要用 `PRIu32` 印。
- **libc 預設是 picolibc**。已確認連結的是 `__d_vfprintf`，printf 可以印浮點數。
- **驅動拆成獨立元件**：`esp_driver_i2c`、`esp_driver_gpio`、`esp_adc` 等，要寫在 `main/CMakeLists.txt` 的 `PRIV_REQUIRES`。
- **舊驅動已移除**：舊版 ADC（`driver/adc.h`）在 v6 已移除；舊版 I2C（`driver/i2c.h`）已 EOL。一律使用新版：
  - I2C：`driver/i2c_master.h`
  - ADC：`esp_adc/adc_oneshot.h`、`adc_cali_scheme.h`
- **I2C NACK 的回傳值改了**：`i2c_master_transmit*` 遇到 NACK 改回傳 `ESP_ERR_INVALID_RESPONSE`（v5 是 `INVALID_STATE`）。
- **掃描時的 log**：`i2c_master_probe` 本身不會印 NACK 訊息，但逾時會用 tag `"i2c.master"` 印錯誤。
- **主控台設定**移到 `esp_stdio` 元件。`sdkconfig.defaults` 已設 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`。
- **ESP32-C6 的 ADC 校正**支援 curve fitting（`ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED`）。
- **API 用法以本機原始碼為準**：遇到不確定的 API，直接看 `~/.espressif/v6.1/esp-idf/components/` 底下的標頭檔，以及 `docs/en/migration-guides/release-6.x/`。不要靠記憶。

## 4. 開機測試（`main/main.c`）的結構

開機等 3 秒，讓 USB 重新連線，接著自動執行：

1. I2C 掃描 0x08～0x77（掃描時關掉 `i2c.master` 的 log）
2. MPR121：
   - 軟體重置後確認 0x5D=0x24
   - 進停止模式，設門檻 12/6 和基準線濾波器（0x2B～0x35）
   - ECR=0x82，寫完逐一讀回比對
3. IMU：
   - 讀 WHO_AM_I
   - 重置、喚醒，設定 ±2g、±250dps、100 Hz（DLPF=3、SMPLRT_DIV=9）
   - 讀回比對；靜止加速度合計要在 0.85～1.15 g
4. IMU INT：開資料就緒中斷，計算 1 秒內的上升緣，50～200 次算通過，測完關掉
5. 電池：取樣 32 次、用 curve fitting 換成 mV 後平均，再 ×2，2.8～4.4 V 算通過
6. MPR121 IRQ：開機無法自動測，在總結表標「手動」

測完印出總結表，失敗的項目會列出該檢查哪幾條線。接著記錄姿態零點（`motion_zero`）、啟動 BLE，進入 `live_loop`：每 100 ms 讀 IMU（加速度＋陀螺儀，`imu_read_all`）和 MPR121，每 200 ms 送 ATTITUDE、每 1 秒送 WATER／POSE、每 10 秒量電池；序列埠每 0.5 秒一行、每 20 行重印標題，模組沒接好的欄位印「--」。

**可以直接拿來用的部分**：`reg_read` / `reg_write`、`mpr121_read_filtered` / `mpr121_read_touch`、`imu_read_accel`、`imu_read_all`、`tilt_deg`、`battery_init` / `battery_read`、`display_width` / `print_col`（中文欄位對齊）。

## 5. 已知注意事項

- **ADC 來源阻抗偏高**：200k/200k 分壓從 D0 看進去約 100kΩ，ADC 讀值可能有幾十 mV 偏差。建議 D0 對 GND 加 100 nF。
- **分壓會一直耗電**：常態耗電約 4.2 V / 400 kΩ ≈ 10 µA，做深度睡眠時要算進功耗。
- **只插 USB、沒接電池時**，原本預期 BAT+ 讀值會飄或接近 4.2 V。2026-11-19 實測：開機約 0.9 V，之後慢慢降到約 0.55 V 停住，電池項目判定失敗。這是正常現象，不是接線問題。
- **用 Python 打開序列埠會讓板子重開機**（USB Serial/JTAG）：即使 DTR、RTS 都設 False，每次打開序列埠板子還是會重跑開機測試。錄資料時要丟掉時間倒退之前的舊行。另外 `idf.py monitor` 開著時，其他程式讀不到這個序列埠。
- **D1（GPIO1）、D2（GPIO2）都在 ESP32-C6 的 LP GPIO 範圍內**（GPIO0～7），理論上可以當深度睡眠的喚醒來源。實作前要用 v6.1 的標頭檔和文件確認。
- **MPR121 的 IRQ 每 0.5 秒才讀一次狀態（0x00）來清除**，所以很快點一下可能只算到一次下降緣。
- **使用者偏好**：畫面訊息用繁體中文，程式碼加中文註解。使用者只看 monitor，不會從序列埠輸入任何東西。
