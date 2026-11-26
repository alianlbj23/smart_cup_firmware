# smart_cup_test 交棒說明

> 給接手的 Claude 對話：開始動手前先完整讀這份文件。這裡記錄了目前狀態，以及已經踩過、不用再查一次的坑。

## 1. 目前進度（2026-11-25）

- **已完成**：硬體接線測試程式 `main/main.c`，已燒錄實測。I2C、MPR121、IMU、IMU INT 全部通過；電池項目失敗是因為沒接電池（見第 5 節）
- **已完成**：銅膠帶電極接上，即時顯示新增「水位%」欄（電極0 兩點直線校正）
- **已完成**：BLE（NimBLE，`main/ble_gatt_svr.c`），協定跟 smart_cup_app 的 `cup_protocol.dart` 一致，另外新增 ATTITUDE（0x05）。用 `tools/ble_check.py` 和 App 實測都通過
- **已完成**：姿態（俯仰／翻滾／傾斜、直立／傾斜／飲用中／倒下、移動判斷）與「水位只在直立靜止時更新」，在 `main/cup_state.c`
- **已完成**：休眠（README 第 6 節）。**預設是淺眠、BLE 連線不斷**（使用者不想看到斷線／重新連線）：靜止 30 秒 → IMU wake-on-motion、連線間隔拉長、CPU 自動淺眠（esp_pm），新增 STATE 封包（0x06）讓 App 顯示「水杯休眠中」。深度睡眠保留成選項 `DEEP_SLEEP_ENABLE`（預設 0）。App 端也加了「找不到水杯時每 20 秒背景重試」，深度睡眠模式時用得到
- **耗電**：C6 沒有 32 kHz 晶振，BLE 淺眠用主晶振，官方數據約 3.3 mA，粗估電池撐 4～5 天（沒實測）。要更省電：開深度睡眠，或硬體加 32 kHz 晶振
- **已完成（2026-11-26，韌體 0.5.0）**：**校正工具**。水位校正值改存 NVS（namespace `cupcal`，key `e0_empty`／`e0_full`），開機載入，沒有才用 `cup_state.c` 的預設值；`main/tool_cmd.c` 在 USB 序列埠收指令（`PING`、`CAL?`、`CAL SET`、`CAL RESET`、`MEASURE`、`LIVE`，回覆都是 `@` 開頭一行）。電腦端 `tools/cup_tool.py`（Tkinter）：連線、量空杯／滿杯（5 秒平均）、檢查（跳動、沒直立靜止、差距太小、RL 沒變化 = 杯子沒壓到底）、寫入、恢復預設、燒錄韌體（GitHub Release／Actions zip／本機 build，用 flash_args 分開燒，保留 NVS）。Actions 會用 PyInstaller 打包成 macOS App、Windows exe、Linux 執行檔（打包後跑 `--self-test`）。實機測過：指令、寫入後重開機保留、恢復預設、程式內呼叫 esptool 燒錄 build/（6 秒）、打包後的 macOS App self-test；**視窗操作本身還沒有人實際點過**
- **已完成（2026-11-26）**：GitHub Actions 自動建置（`.github/workflows/build.yml`）：推上 main／PR／手動執行 → host 單元測試 → `espressif/esp-idf-ci-action` 在 `espressif/idf:v6.1` 容器裡 `idf.py build && idf.py merge-bin` → Artifacts 上傳燒錄檔；發版方式見下一項。merged 檔從 0x0 蓋到程式結尾，會把中間的 NVS（0x9000～0xEFFF）填成 0xFF，**燒 merged 檔會清掉未確認的喝水事件**，平常用 `write-flash @flash_args`
- **已寫完（2026-11-26）——還沒在 GitHub Actions 上跑過**：校正工具三平台＋自動發版＋自動更新（設計 `docs/superpowers/specs/2026-11-26-cup-tool-auto-update-design.md`）。CI 打包 macOS App／Windows exe／Linux 執行檔（Linux 用 `xvfb-run` 跑 `--self-test`），版號（`CUP_FW_*`）寫成 `VERSION` 包進執行檔；`release` job 只在 push main、其他 job 都通過後跑：`gh release view` 查到已發布且有 `SHA256SUMS.txt` 的 `v<版號>` Release 就跳過，否則下載全部 Artifacts、產生 `SHA256SUMS.txt`、先上傳到 draft Release、全部上傳完才 `gh release edit --draft=false --latest`（發布時才建 tag，所以中途失敗重跑會補上；main 的執行不會被 cancel-in-progress 取消）。**所以改版號推上 main 就是發版**，版號沒改不會發。遠端目前沒有任何 tag，這份 workflow 第一次推上 main 就會建 `v0.5.0`。工具端 `tools/cup_updater.py`：啟動時查 GitHub 最新 Release，有新版詢問 → 下載、比對 `SHA256SUMS.txt`（沒有或不符就拒絕）→ 替換並重新啟動；連線／燒錄／量測中自動檢查會延後；從原始碼執行只記紀錄不更新；Linux 執行檔需要 glibc 2.39+（ubuntu-latest 建置）；macOS App 要先搬到「應用程式」（App Translocation 下無法替換）；`--no-update-check` 關閉。套件全部固定版本：執行用 `tools/requirements-cup-tool.txt`，開發（pytest、pyinstaller）`tools/requirements-dev.txt`，測試 `python -m pytest tools/tests`
- **已完成（2026-11-24，還沒實機測）**：韌體端偵測喝水／加水（`cup_state.c` 的 `cup_event_*`，規則照 App 的 `lib/state/drink_detector.dart`），新封包 0x07 DRINK_EVENT，事件存 NVS（`main/drink_log.c`）、App 回 3-byte ACK 才刪除、重新訂閱後補傳。韌體版本 **0.3.0**：App 看到 ≥ 0.3.0 要關掉自己的偵測，連線中的事件也只由韌體送。格式見 README 第 5 節「喝水／加水事件」。偵測邏輯有 host 單元測試（`sh test/host/run.sh`）；NVS 儲存、補傳、ACK 只有編譯過，還沒實機驗證
- **已寫完（2026-11-25，韌體 0.4.0）——沒有編譯過、沒有燒錄、沒有實機測**。寫的那台機器沒有 ESP-IDF，也沒有接水杯。介面依據是 `food_intake-viz/docs/smart-cup/CONTRACT.md` §1（韌體 ↔ App ↔ Server 的合約，改介面要先改那份）：
  1. **直立不動時水位下降也算喝水**（住民用吸管喝，杯子不會被拿起）：`cup_state.c` 的 `track_stable()`。穩定期間水位比基準低 ≥ 3%，而且連續 ≥ 3 筆、持續 ≥ 8 秒 → 喝水事件（`saw_drinking = 0`，A = 原基準、B = 當下水位）；上升只更新基準；還不到門檻時基準不往下跟（小口可以累積）。**拿起放回結算時下降不到 3%，基準也不往下跟、維持 A**（使用者 2026-11-25 決定；上升不到 3% 照舊往上跟），所以基準只會經由喝水事件往下走。休眠中每 5 秒量到的水位走同一條規則：`cup_event_doze_water()` 的簽名改成 `bool cup_event_doze_water(float pct, uint32_t now_ms, cup_event_t *out)`，`main.c` 的 `doze_measure_water()` 拿到事件就 `drink_log_add()`。新參數 `EVT_STATIC_DROP_ENABLE`／`EVT_STATIC_DELTA_PCT`／`EVT_STATIC_HOLD_MS`／`EVT_STATIC_MIN_SAMPLES`，規則、已知風險、怎麼調寫在 README 第 5 節
  2. **Device Status 加 BLE 位址**：4 bytes → 10 bytes `[電量, major, minor, patch, 位址 6 bytes（最高位元組在前）]`，Read 和 Notify 都是。內容由 `cup_state.c` 的 `cup_status_encode()` 組（純計算，有單元測試），`ble_gatt_svr.c` 在 `on_sync` 把 `ble_hs_id_copy_addr` 的位址存起來。位址取不到時只回 4 bytes
  3. `tools/ble_check.py` 印出位址並和掃描到的位址比對
  - **驗證到哪裡**：`cup_state.c`（偵測邏輯、Device Status 內容）有 host 單元測試，`sh test/host/run.sh` 全部通過（預設參數 199 項檢查；`EVT_STATIC_DROP_ENABLE=0` 再跑一次 73 項）。`main.c`、`ble_gatt_svr.c`、`drink_log.c` 的改動**只用 host 的 gcc 加上手寫的假標頭做過語法檢查**（假標頭是憑記憶寫的，不等於真的 IDF），所以**要在有 IDF 的機器上 `idf.py build`，再實機測**
  - **實機要測**：(a) `tools/ble_check.py` 的 Device Status 是 10 bytes、位址和掃描到的一致、韌體 0.4.0；(b) 杯子放桌上用吸管喝，序列埠出現 `[事件] 喝水 …，杯子直立不動` 而且量合理；(c) 靜置 30 分鐘以上（活動中＋休眠中）、手在杯子旁邊靠近再離開、空杯靜置，都**不應該**出現事件，出現的話照 README 調 `EVT_STATIC_DELTA_PCT` 或關掉；(d) 休眠中用吸管喝、有連線時 App 收得到；(e) 原本拿起來喝／加水的事件照常、沒有重複；(f) 沒喝水、反覆拿起放回不同位置 20 次以上，**不應該**出現事件；每次只喝一小口（不到 3%），幾次之後要出現一筆合計的事件
- **直立不動規則的備忘**：
  - 基準在穩定期間等於「看過的最高值」（只往上跟）。所以往下的漂移、手靠近再離開，都可能被算成喝水——這是規則本身的限制，加長持續時間擋不掉，只能調門檻或關掉
  - 還沒確認的下降（`s_drop_n`、`s_drop_since`）**故意不放 RTC 記憶體**：深度睡眠醒來後重新確認。放了的話「睡前 1 筆 + 醒來 1 筆」就會滿足持續時間。基準本身在 RTC 記憶體而且沒有往下跟，不會漏
  - 事件的開始時間是「連續低於 基準 − 門檻 的第一筆」，不是「第一次略低於基準」（後者會被雜訊拉到好幾個小時前）
  - 電極讀取失敗時水位是舊值，`live_loop`（`have_e`）和 `doze_measure_water`（10 筆都要讀到）都不把舊值拿去偵測，否則同一個舊值會被當成「連續好幾筆偏低」
  - `drink_log_add()` 現在休眠中也會被呼叫，送出時的 ACK 逾時跟著上一次 `drink_log_poll()` 的 `dozing`（`s_dozing`）
  - 「拿起 → 放回」結算下降不到 3% 時基準維持 A（`settle()` 最後的 else）。**這一點和 App 的 `drink_detector.dart` 不一致**（App 是改成 B；韌體 ≥ 0.3.0 時 App 的偵測本來就關閉）。既有測試「雜訊（變化 < 3%）」的預期跟著改了：預設參數下基準留在 50、下一口記 50 → 45；`EVT_STATIC_DROP_ENABLE=0` 時仍是 0.3.0 的 48、48 → 45
  - `EVT_STATIC_DROP_ENABLE=0` 同時關掉「直立不動算喝水」和「結算時基準不往下跟」：穩定期間基準會往下跟的話，結算時留著 A 也沒有用
  - 副作用：每次放回位置不同造成的讀值差（往下、不到 3%）不再被吸收，會累積；實機要看反覆拿起放下、沒喝水時會不會冒出事件
  - host 測試要有 `cc`。沒有的機器可以用有 gcc 的 Docker image 跑，例如 `docker run --rm --network none --user "$(id -u):$(id -g)" -v "$PWD":/src -w /src node:22-bookworm sh test/host/run.sh`
- **App 端還要做（smart_cup_app）**：
  - 0.3.0 的部分：`cup_protocol.dart` 解 0x07、回 3-byte ACK `[0x07, seq & 0xFF, (seq >> 8) & 0x07]`（重複收到也要回）、用 seq + 時間戳 + A/B 去重；韌體 ≥ 0.3.0 時關掉 `DrinkDetector`
  - 0.4.0（合約 §1.1）：讀 Device Status，長度 ≥ 10 時把 byte 4..9 組成大寫、冒號分隔的字串（例如 `58:E6:C5:12:E0:CE`）當 `cup_hardware_id`；長度 < 10（舊韌體，或位址取不到）用平台給的 remoteId。Notify 也是 10 bytes，解析不能寫死長度 4
  - 0.4.0（合約 §1.2）：種類 0（喝水）的事件一律計入喝水量、`status = ok`，**不論 `saw_drinking`、`too_long`**（之前「沒有 `saw_drinking` 標成待確認」的做法要拿掉）；`time_unknown` 的事件用收到的時間、另外記 `time_estimated = true`，一樣計入；種類 1（加水）照樣記錄，不計入喝水量
  - 一口氣用吸管喝很久會收到好幾筆前後相接的喝水事件（上一筆的 B = 下一筆的 A），都要計入，不要當成重複
- **還沒開始**：RL 參考電極的 TI 比例算法、空杯自動重新校正

### 休眠實作備忘（2026-11-19 踩過的坑）

淺眠（預設）：

- sdkconfig 照 `examples/bluetooth/nimble/power_save/sdkconfig.40m.esp32c6`（主晶振當 BLE 睡眠時脈），另外開 `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`，接著電腦時不自動淺眠，序列埠才不會斷
- 休眠迴圈每 250 ms 用 `gpio_get_level` 看 D1（IMU INT 是閂鎖的，不會漏），不需要設定 GPIO 喚醒源；`vTaskDelay` 期間 tickless idle 會自動淺眠
- **App 訂閱的當下立刻送資料，App 會漏掉**（實測：剛連上時水位、姿態顯示「--」）。改成訂閱後 1.5 秒補送（`SUBSCRIBE_RESEND_MS`）
- 喚醒時用 `test_imu_id()` 把 IMU 重置回一般模式（同時清掉 WOM 閂鎖），並呼叫 `cup_motion_mark_moving()`，不然醒來後「已靜止很久」會馬上又睡
- **同一時間只能有一個 App 連著**：測試時發現使用者開著舊版 Linux App 自動連上，其他程式都掃不到水杯；而且 Linux App 關掉後 BlueZ 會留著連線（`bluetoothctl info` 顯示 Connected: yes），要 `bluetoothctl disconnect`。App 已改成關閉視窗前先斷線
- bleak 和 App 同時連著時，bleak 每筆通知會收到兩次（BlueZ 的行為，不是韌體送兩次；App 的接收紀錄沒有重複）

深度睡眠（`DEEP_SLEEP_ENABLE=1` 時）：

- v6 起喚醒原因用 `esp_sleep_get_wakeup_causes()`（bitmap），舊的 `esp_sleep_get_wakeup_cause()` 已 deprecated；ext1 用 `esp_sleep_enable_ext1_wakeup_io()`，C6 的 GPIO 0～7 可用，每支腳可各自設定電位
- **IMU 剛切到 wake-on-motion 時，頭幾筆低功耗取樣會觸發中斷**：沒處理的話一睡下去 0.4 秒就被叫醒。現在切換後等 300 ms，再清 INT_STATUS、等超過一個取樣週期（150 ms）確認 D1 是低電位才睡
- **閒置時間要從「App 連上」重新算**：只看最後一次移動的話，水杯醒著靜止一陣子後 App 才連上，等待上限一換成「連線中」就會立刻睡
- 喚醒路徑跳過 `test_imu_int`，但 D1 還是要 `gpio_config` 成輸入（它會呼叫 `rtc_gpio_deinit`，把喚醒用過的 LP IO 切回一般 GPIO）
- **測休眠不要開序列埠**：打開序列埠（pyserial 設 DTR/RTS）會讓板子重開機，蓋掉喚醒狀態。改看 `/dev/ttyACM0` 在不在（睡著時 USB 斷開），或用 `tools/ble_watch.py` 看廣播
- 測試版做法：暫時改 `SLEEP_ON_USB=1`、`SLEEP_TIMER_WAKE_S=45`，用 `idf.py -B build_sleeptest` 另外建置並燒錄，**燒錄完才把原始碼改回來**（`idf.py flash` 會先重新編譯，太早改回會燒到正式版）；測完刪掉 `build_sleeptest`（沒有被 .gitignore 涵蓋）
- 舊的測試版韌體會自己睡著，燒錄前要等 `/dev/ttyACM0` 出現再馬上燒

### BLE 實作備忘

- 協定細節、頻率、姿態門檻都寫在 README 第 5 節。**改協定時 App（smart_cup_app）的 `lib/services/cup_protocol.dart` 要同步改**（0x07 的 bit 配置、ACK 格式、版本號判斷、Device Status 的長度都在那裡），喝水偵測規則改了的話 `lib/state/drink_detector.dart` 也要對照（0.4.0 的「直立不動」規則只有韌體有）
- `ble_hs_id_copy_addr` 給的位址是**最低位元組在前**；Device Status、序列埠、廣播名稱用的都是反過來的顯示順序
- NVS 在 `app_main` 一開始初始化一次（`nvs_flash_init`），`ble_cup_init` 不再初始化。喝水事件存在 namespace `drinklog`：每筆一個 `u64`（一個 NVS entry，256 筆約 2 頁，預設分割表 nvs 24 KB 放得下）。key = `e` + slot（seq % 256），另外有 `boot_id`、`next_seq`、`tail`
- 事件偵測和補傳用的時鐘是 `esp_rtc_get_time_us()`（RTC 計時器，深度睡眠也繼續走），不是 `esp_timer`。C6 沒有 32 kHz 晶振，RTC 慢時脈是內部 RC（有校正），長時間會有約 1% 的誤差，只影響「對時前事件換算的時間」和 too_long 的判斷
- ACK 在 NimBLE host task 收到，用 FreeRTOS queue（`ble_cup_take_event_ack`）交給主迴圈；`drink_log.c` 只在主迴圈呼叫，不用加鎖
- 開 BLE 之後要重新產生 sdkconfig（`idf.py set-target esp32c6`），因為舊的 sdkconfig 裡 `CONFIG_BT_ENABLED` 是關的，sdkconfig.defaults 不會覆蓋已存在的設定
- NimBLE 的 API 照 `examples/bluetooth/nimble/bleprph` 寫；送資料用 `ble_gatts_notify_custom()`，om 由 NimBLE 釋放
- 128-bit UUID 用 `BLE_UUID128_INIT` 時是 little-endian（倒過來寫），characteristic 編號在第 12 個 byte
- 廣播封包放不下名稱（flags 3 + UUID128 18 bytes），名稱 `SmartCup-XXXX` 放在 scan response
- App 寫入用的是 write with response，characteristic 只開 `BLE_GATT_CHR_F_WRITE`
- 這台開發機有藍牙（hci0），可以用 bleak 直接測；App 連著時電腦連不上
- 電池接上後（2026-11-19）：開機測試電池項目通過（約 3.97 V），藍牙回報電量 70～80%

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

- 上表是 2026-11-19 的舊數據。**2026-11-26 電極重貼後重新校正**，最後採用：空杯 480.2（479～481）、滿杯 356（355～357，水面切齊主條銅箔頂端、杯子壓到底，連續兩次），電極1 滿杯約 468，總變化 124、雜訊約 ±1。中間一度用過滿杯 338.8，後來確認那次水倒得比銅箔頂端高，切齊時只顯示 88%，所以改用 356
- **杯子一定要壓到保護套底部**：沒壓到底時滿杯只讀到 89.7%，而且杯底的 RL 從空杯到滿杯只變 10（壓到底時變 50）。RL 幾乎沒反應是「杯子位置不對」的好指標
- **滿杯要有固定標準**：用「水面切齊主條銅箔頂端」，憑感覺倒會差到 10%
- **空杯讀值不穩定（2026-11-26 發現，還沒解決）**：同一個空杯量過 463.6（第一次）、436.0（倒空沒擦乾 → 顯示 22%，電極1 也低 6）、479.6 與 480.2（擦乾、杯子拿出保護套再放回；不動時兩次只差 0.6）。也就是**杯壁殘留水約 −28**、**杯子拿出再放回約 ±16**。喝完水杯壁一定是濕的，水位會偏高、喝水量少算，直到杯壁乾掉。改善方向：杯子在保護套裡固定緊一點、接地條改貼到主條正對面（目前貼在旁邊，兩條之間的杯壁水膜影響最大）、韌體做空杯自動重新校正；先做「拿出放回 3 次」和「倒空後讀值多久回復」的測試量化量的時候注意：杯子靜止 30 秒會休眠、即時表格停止輸出，要先重開機（`esptool chip-id` 會重設）再在 30 秒內錄；VS Code 的 monitor 開著時會搶序列埠資料、esptool 也鎖不到埠
- 校正值在 `main/cup_state.c` 的 `LEVEL_E0_EMPTY = 480`、`LEVEL_E0_FULL = 356`，公式：`水位% = (480 − 電極0) / (480 − 356) × 100`，超出 0～100 的部分截掉
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
- **macOS 開發機（2026-11-24 安裝）**：沒有用 EIM（Homebrew 的 eim tap 要 `brew trust`），改用 `git clone -b v6.1 --recursive` 到 `~/.espressif/v6.1/esp-idf` 再 `./install.sh esp32c6`。系統預設的 `python3` 是 3.15，沒有 SSL 憑證、下載工具會失敗，所以安裝時用 Homebrew 的 Python 3.13。`~/.espressif/tools/activate_idf_v6.1.sh` 是手寫的 wrapper（設 `IDF_PYTHON_ENV_PATH` 指到 `idf6.1_py3.13_env` 再 source `export.sh`），用法跟上面一樣；它也支援 `-e`（印出環境變數，仿 EIM 的 activation script，VS Code 擴充套件會呼叫）。cmake、ninja 用 `idf_tools.py install cmake ninja` 裝在 `~/.espressif/tools`（從 Dock 開的 VS Code 沒有 Homebrew 的 PATH）。VS Code 擴充套件靠手寫的 `~/.espressif/tools/eim_idf.json` 找到這份安裝。`~/esp/esp-idf` 是舊的 v5.5，不要用
- **Linux、macOS 共用同一份 `.vscode/settings.json`**：`idf.currentSetup` 寫成 `${env:HOME}/.espressif/v6.1/esp-idf`（擴充套件讀設定時會展開 `${env:…}`），`idf.port` 設成 `detect`（燒錄時由 esptool 自動找板子）。兩台都要把 ESP-IDF 裝在 `~/.espressif/v6.1/esp-idf`。**不要在 VS Code 用「Select Current ESP-IDF Version」重新選版本**：它會把 `idf.currentSetup` 改回這台機器的絕對路徑，commit 之後另一台就壞了；真的選了就把那行改回來。終端機指令兩台一樣（`source ~/.espressif/tools/activate_idf_v6.1.sh`），只有序列埠名稱不同（Linux `/dev/ttyACM0`、Mac `/dev/cu.usbmodem*`）
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
- **使用者偏好**：畫面訊息用繁體中文，程式碼加中文註解。使用者平常只看 monitor；0.5.0 起校正工具會從序列埠送指令（`tool_cmd.c`），收到指令後 60 秒內不休眠、不印表格。
- **打開序列埠會讓板子重開機（macOS 也會）**：不是每次，但常常發生。工具連線時每秒送一次 PING，等到 `@PONG`（開機約 5 秒後指令模組才啟動），不能一打開就送指令。0.5.0 起裝了 USB Serial/JTAG 驅動（printf 也走驅動：送不出去最多等 50 ms 就丟掉，沒接電腦不會卡住；驅動本身不擋自動淺眠）
