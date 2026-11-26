# smart_cup_firmware

智慧水杯韌體。用貼在保護套內壁的銅膠帶電極，隔著玻璃杯量測水位。

開機時自動檢查每個模組的接線，接著每 100 ms 量一次水位和姿態，透過 **BLE** 推送給手機 App [smart_cup_app](https://github.com/alianlbj23/smart_cup_app)，序列埠也會每 0.5 秒印一行數值。韌體自己偵測**喝水／加水**，存進 flash，App 沒連著時也不會漏（見[第 5 節](#喝水加水事件0x07)）。杯子靜止 30 秒就進入**休眠**省電（藍牙保持連線），拿起杯子就會醒來（見[第 6 節](#6-休眠)）。

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

目前的校正值（空杯 480、滿杯 356）是 2026-11-26 用現在這個杯子和電極量的。杯子、電極都沒換的話，**直接燒錄就能用，不必再校正**。

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

Linux 和 macOS 都可以開發，ESP-IDF 都裝在 `~/.espressif/v6.1/esp-idf`，指令一樣，只有序列埠名稱不同：Linux 是 `/dev/ttyACM0`，Mac 是 `/dev/cu.usbmodem*`（下面的指令把 `-p` 換掉即可）。

用 VS Code 的話，`.vscode/settings.json` 兩台共用：ESP-IDF 路徑寫成 `${env:HOME}/…`、序列埠設成 `detect`（自動偵測）。不要用「ESP-IDF: Select Current ESP-IDF Version」重新選版本，它會把路徑寫死成這台機器的。要開**本專案的資料夾**，不要開上一層。開錯的話，狀態列的建置、燒錄按鈕會在錯的目錄執行，出現「沒有 CMakeLists.txt」或「找不到 sdkconfig」的錯誤。

### 擋敏感資料上傳（clone 後做一次）

```bash
bash tools/setup_hooks.sh      # 選 1「安裝／更新 hook」
```

會下載固定版本的 gitleaks（8.30.1，比對 SHA256）到 `.tools/`，並啟用 `.githooks/`：

- `git commit` 前：檢查這次要 commit 的檔案。`git push` 前：檢查要推上去的 commit。
- 擋兩種東西：敏感檔名（`.env`、`*.pem`、`*.key`、`id_rsa`、`credentials.json`、ESP32 的 flash 加密金鑰等，見 `tools/secret_guard.py` 的 `BLOCKED_NAMES`），以及 gitleaks 抓到的 API key、token、密碼、私鑰內容。
- 擋下時會列出檔案和行數，金鑰本身會遮住。內容誤判的話，在那一行加註解 `gitleaks:allow`，或把 gitleaks 給的 Fingerprint 加進 `.gitleaksignore`；檔名誤判（例如要 commit 公開的 `ca_cert.pem`），把路徑 glob（例如 `main/certs/*_cert.pem`）一行一個加進 `.secret-guard-allow`。
- merge commit 裡解衝突時寫進去的東西、改名成敏感檔名的檔案也會擋。
- 金鑰已經推上去的話，刪掉檔案不夠（git 歷史裡還在），要去那個服務撤銷、重新產生。
- GitHub Actions 每次推上任何分支、開 PR 都會掃整個 git 歷史（`.github/workflows/secret-scan.yml`），沒過就不發版。`git commit --no-verify` 可以略過本機的 hook，但躲不過 CI。PR 用 base 分支（例如 main）的掃描程式和 `.secret-guard-allow`，PR 裡改規則不會讓自己過關。
- 注意：CI 是推上去**之後**才掃，真正能事前擋住的只有本機 hook，所以每台電腦 clone 後都要裝。

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

### 不裝 ESP-IDF：用 GitHub 自動建好的檔案燒錄

每次推上 main、開 PR，GitHub Actions（[.github/workflows/build.yml](.github/workflows/build.yml)）都會先跑喝水偵測的單元測試，再用 ESP-IDF v6.1 建置 esp32c6 韌體，並把校正工具打包成 macOS App、Windows 和 Linux 執行檔（見[第 4 節](#用校正工具建議韌體-050-起)）。**最簡單的燒法是用校正工具的「燒錄韌體」**，下面是不用工具的做法。

1. 到 GitHub 的 **Actions** 分頁 →「韌體建置」→ 點最新一次成功的執行，在最下面的 **Artifacts** 下載 `smart_cup_firmware-v<版本>-<commit>`（保留 90 天）。
2. 解壓縮，裡面有 `FLASHING.txt`（燒錄說明）。只需要 Python 的 esptool（`pip install esptool`），兩種燒法：

```bash
# A. 建議：分開燒 bootloader、分割表、程式，flash 裡未確認的喝水事件會保留
python -m esptool --chip esp32c6 -p <序列埠> -b 460800 write-flash @flash_args

# B. 單一檔案：從 0x0 整段蓋過去，中間的 NVS 區也會被清空（校正值、未確認的喝水事件、boot_id 都會不見）
python -m esptool --chip esp32c6 -p <序列埠> -b 460800 write-flash 0x0 smart_cup_firmware-v<版本>-<commit>-merged.bin
```

**發版**：改 `main/ble_gatt_svr.h` 的 `CUP_FW_MAJOR／MINOR／PATCH` 推上 main，單元測試、韌體建置、三平台工具打包都通過後，CI 會自動建 `v<版號>` tag 和 GitHub Release，附上韌體檔、三平台工具（`SmartCupTool-macOS.zip`、`SmartCupTool-Windows.zip`、`SmartCupTool-Linux.zip`）和 `SHA256SUMS.txt`。Release 先建成 draft，全部檔案上傳完才發布，所以工具不會看到缺檔的版本。這個版號已經有發布過、而且有 `SHA256SUMS.txt` 的 Release 就不發版，不用手動打 tag；上次發版中途失敗的話，在 Actions 按 Re-run 或再推一次 main 就會補上。只有推上 main 會發版（手動推 tag 不會觸發）。

GitHub 上建置失敗時（例如編譯警告，v6 會當成錯誤），Actions 分頁會顯示紅色叉叉，點進去看 log。

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
// 預設的水位校正值（flash 裡沒有工具寫入的校正值時才用）：電極0 在空杯、滿杯時的讀值
#define LEVEL_E0_EMPTY          480
#define LEVEL_E0_FULL           356

// s_cal_empty／s_cal_full：開機時是預設值，main.c 從 NVS 載入、校正工具寫入時換掉
float cup_water_e0_to_pct(float e0)
{
    float pct = ((float)s_cal_empty - e0) * 100.0f / (float)(s_cal_empty - s_cal_full);
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

### 用校正工具（建議，韌體 0.5.0 起）

韌體 0.5.0 起，校正值存在水杯的 flash（NVS），用電腦的**校正工具**量測並寫入，不用改程式、不用重新編譯燒錄。重開機、燒錄新韌體（用 `flash_args` 燒）都會保留。flash 裡沒有校正值時，才用程式裡的預設值（`cup_state.c` 的 `LEVEL_E0_EMPTY`／`LEVEL_E0_FULL`）。

**取得工具**（擇一）：

- **不裝 Python**：到 GitHub 最新的 **Release** 下載 `SmartCupTool-macOS.zip`（App）、`SmartCupTool-Windows.zip`（exe）或 `SmartCupTool-Linux.zip`（單一執行檔），解壓後雙擊／執行（還沒發版的 commit 在 **Actions** 分頁 → 該次執行的 Artifacts）。
  - macOS：第一次開會說「無法驗證開發者」（沒有簽章）：在 Finder 對 App **按右鍵 →「打開」**，之後就能直接開。**先把 App 搬到「應用程式」資料夾**，直接從「下載」執行時無法自動更新。
  - Linux：使用者要在 `dialout` 群組才能開序列埠（`sudo usermod -aG dialout $USER`，重新登入後生效）。執行檔固定在 GitHub 的 `ubuntu-24.04` 建置，需要 glibc 2.39 以上（Ubuntu 24.04、Debian 13 以後）；更舊的發行版請從原始碼執行。
- **用 Python 跑**：Python 要有 Tkinter（macOS 用 python.org 的安裝檔；Homebrew 的要 `brew install python-tk@3.13`；Linux 要 `sudo apt install python3-tk`）

```bash
python3 -m venv .venv
.venv/bin/pip install -r tools/requirements-cup-tool.txt
.venv/bin/python tools/cup_tool.py
```

**自動更新**：工具啟動時會查 GitHub 最新 Release，有新版會詢問，確認後下載、比對 `SHA256SUMS.txt`，換成新版並重新啟動（比對不符或沒有 `SHA256SUMS.txt` 就不更新）。連著水杯、燒錄或量測中時，自動檢查會等到閒置（沒連線）才問；手動按「檢查工具更新」則會提醒更新要中斷連線。Windows 更新後留下的 `SmartCupTool.exe.old`、macOS 的 `SmartCupTool.app.old` 會在下次啟動時自動刪掉。從原始碼執行時只在紀錄提示有新版（手動檢查才會問要不要打開 Release 網頁），不會更新自己。加 `--no-update-check` 參數可以關閉檢查。

**開發**：改工具要跑測試，打包用的 PyInstaller 也在同一份清單：

```bash
.venv/bin/pip install -r tools/requirements-dev.txt
.venv/bin/python -m pytest tools/tests
```

**使用步驟**（先關掉 VS Code 的 monitor，同一個序列埠一次只能給一個程式用）：

1. 水杯用 USB 線接電腦 → 選「★ 水杯（ESP32）」那個序列埠 → **連線**。板子可能會重開機，約 5 秒後連上，上方會顯示韌體版本和目前的校正值，「即時讀值」開始跳動。
2. 杯子**擦乾、壓到保護套底部**、直立放好、手離開 → **量空杯**（5 秒）。
3. 倒水到**水面和主條銅箔頂端切齊** → **量滿杯**（5 秒）。
4. 下面會自動填入要寫入的空杯／滿杯值並檢查，有問題會顯示 ⚠ → **寫入水杯**。馬上生效，看「即時讀值」驗證：滿杯約 100%，倒掉擦乾後約 0%。
5. 不滿意可以重量，或按**恢復程式預設值**。

工具會自動檢查的：讀值跳動太大（杯子在動、手太近）、量測期間不是直立靜止、空杯和滿杯差距太小、**杯底 RL 從空杯到滿杯幾乎沒變**（杯子沒壓到保護套底部，滿杯會少算約 10%）。

工具的「燒錄韌體」可以燒 GitHub 最新 Release、Actions 下載的 zip、或本機的 `build/`，都用 `flash_args` 分開燒，**會保留 flash 裡的校正值和未確認的喝水事件**。

工具和水杯之間的序列埠指令（也可以在 `idf.py monitor` 裡直接打）見 [main/tool_cmd.h](main/tool_cmd.h)：`PING`、`CAL?`、`CAL SET <空杯> <滿杯>`、`CAL RESET`、`MEASURE <毫秒>`、`LIVE 1`、`HELP`。

### 手動校正步驟（不用工具）

這是 0.5.0 以前的做法：量到的數字改成程式的**預設值**再重新燒錄。**flash 裡有工具寫入的校正值時，會優先用 flash 的值**，要讓預設值生效，先在工具按「恢復程式預設值」（或在 monitor 打 `CAL RESET`）。

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

### 目前的校正數據（2026-11-26，電極重貼後重新校正）

| 狀態 | 電極0（1 µs） | 電極1（1 µs） | 顯示水位 |
|---|---|---|---|
| 空杯 | **480.2**（479～481） | 529.7 | 0% |
| 滿杯（水面切齊主條銅箔頂端） | **356**（355～357） | 468 | 100%（燒錄後實測） |

- 杯子擦乾、**壓到保護套底部**再量。空杯到滿杯總共變化約 124，雜訊約 ±1，所以解析度大約是 1.6%。
- **滿杯的標準是「水面和主條銅箔頂端切齊」**。水倒得比銅箔頂端高時，讀值還會再低（實測過 338.8），會讓之後切齊時只顯示約 88%。
- **杯子沒壓到底**，滿杯會少算約 10%（實測 89.7%，杯底的 RL 幾乎沒反應）。
- **空杯讀值會變**：同一天量過 463.6（第一次）、436.0（倒空沒擦乾，顯示 22%）、479.6／480.2（擦乾、重新放回保護套）。杯壁殘留的水會讓讀值變低（水位偏高）；杯子拿出保護套再放回，整條讀值會平移。
- 半杯還沒重新量。

舊數據（2026-11-19，電極重貼前）：

| 狀態 | 電極0（0.5 µs） | 電極0（1 µs） | 電極1（1 µs） | 顯示水位 |
|---|---|---|---|---|
| 空杯 | 205 | 403（±1） | 541 | 0% |
| 目測半杯 | 177.5 | 337.5～345.5 | 521～522 | 55%～63% |
| 滿杯 | 153 | 298（±3） | 519 | 100% |

- 當時空杯到滿杯總共變化約 105，雜訊約 ±2，解析度大約是 2%。
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
| `…0004` ACK | App → 杯，Write | 1 byte `[0x01]` = 收到（只計數）；3 bytes `[0x07, seq 低 8 bits, seq 高 3 bits]` = 確認一筆 DRINK_EVENT，韌體收到才從 flash 刪除 |
| `…0005` Device Status | 杯 → App，Read/Notify | 10 bytes，見下方「Device Status」。韌體 0.4.0 以前只有前 4 bytes |

**Device Status（10 bytes，Read 和 Notify 相同）**：

| Byte | 內容 |
|---|---|
| 0 | 電量 %，`0xFF` = 未知（沒接電池） |
| 1..3 | 韌體版本 major、minor、patch（目前 **0.6.0**） |
| 4..9 | 杯子的 BLE 位址 6 bytes，**顯示順序**（最高位元組在前）。例：位址 `58:E6:C5:12:E0:CE` → `58 E6 C5 12 E0 CE`（韌體 0.4.0 新增） |

- 舊 App 只讀前 4 bytes，不受影響。App 讀到長度 ≥ 10 時，把後 6 bytes 組成大寫、冒號分隔的字串當 `cup_hardware_id`（iOS 拿不到 BLE 位址，只能靠這個）；長度 < 10 時用手機平台給的 remoteId。
- 位址來源是 NimBLE 的 `ble_hs_id_copy_addr`（它給的是最低位元組在前，韌體會反過來），和廣播名稱 `SmartCup-XXXX` 的 XXXX、序列埠 `[BLE] 開始廣播：…（位址 …）` 印的是同一個位址。
- 位址在 BLE 啟動（sync）時取得，之後才開始廣播，所以 App 連得上時一定已經有位址。萬一取不到（序列埠會印 `[BLE] 讀不到 BLE 位址`），Device Status **只回前 4 bytes**，不會回一個全 0 的假位址。
- 內容由 [main/cup_state.c](main/cup_state.c) 的 `cup_status_encode` 組出來，有單元測試。

| type | 名稱 | 頻率 | value |
|---|---|---|---|
| `0x02` | WATER | 1 次/秒（休眠中：變化 ≥ 1% 或每 30 秒） | bits 24..0 = 水量 25-bit 分數（0～2²⁵−1），bits 28..25 = 刻度 0～10 |
| `0x04` | POSE | 1 次/秒，姿態改變時立刻送（休眠中不送） | bits 15..0 = 傾斜角 0.1°，bits 23..16 = 姿態（1 直立、2 傾斜、3 飲用中、4 倒下），bits 31..24 = 移動中 |
| `0x05` | ATTITUDE | 5 次/秒（休眠中不送） | bits 15..0 = pitch（int16，0.1°），bits 31..16 = roll（int16，0.1°）。即時資料，App 不回 ACK |
| `0x06` | STATE | 狀態改變時、App 訂閱後 1.5 秒；休眠中每 10 秒 | bits 7..0 = 狀態（0 活動中、1 休眠中），bits 31..8 = 進入目前狀態幾秒 |
| `0x07` | DRINK_EVENT | 每筆事件；連線中即時送，斷線時存 flash、重新訂閱後補傳（休眠中也會） | 見下方「喝水／加水事件」。時間戳 = 事件開始時間，不是送出時間 |
| `0x03` | RAW | 目前不送 | 電極0 讀值，除錯用 |

開機後還沒量到有效水位之前（約 1 秒），不會送 WATER。

### 喝水／加水事件（0x07）

韌體 **0.3.0** 起，喝水／加水由韌體偵測（[main/cup_state.c](main/cup_state.c)），韌體是唯一的事件來源。App 看到 Device Status 的版本 ≥ 0.3.0 就要關掉自己的偵測，否則會重複計算。韌體 **0.4.0** 起，杯子直立不動時水位下降（用吸管喝）也會產生喝水事件，封包格式不變。

**偵測規則**。韌體 0.4.0 起和 App 的 `lib/state/drink_detector.dart` **不再完全一致**，差在兩點：(1)「直立不動」的規則只有韌體有；(2) 拿起放回結算時下降不到 3%，韌體的基準不往下跟（維持 A），App 的偵測器是改成 B。韌體 ≥ 0.3.0 時 App 的偵測本來就關閉，以韌體為準；之後如果要對照兩邊的結果，要記得這兩點不同。

- **穩定**：直立、不在移動，持續 3 秒以上，而且有新量到的水位。穩定時的水位是基準 A。
- **拿起來喝／加水**
  - **過程**：離開穩定（傾斜、飲用中、倒下、移動中）就開始一段過程，記下開始時間、過程中有沒有出現「飲用中」姿態。放下不到 3 秒又拿起，算同一段。
  - **結算**：再次穩定得到水位 B。A − B ≥ 3% → 喝水；B − A ≥ 3% → 加水；這兩種基準都改成 B。
  - **變化不到 3%**：不產生事件。上升（B ≥ A）→ 基準改成 B。**下降（B < A）→ 基準不往下跟，維持 A**（0.4.0 起）：這一小口留著，和之後的下降累積到 3% 再記。例：60% 拿起來喝到 58%（不記）→ 再拿起來喝到 56% → 一筆 60% → 56%，時間戳是第二次拿起的時間。
  - 過程超過 10 分鐘照樣產生事件，但帶 `too_long`。剛開機還沒有基準時，第一次穩定只建立基準。
- **直立不動（用吸管喝，杯子放在桌上沒有被拿起）**：穩定期間，每一筆水位都和基準比
  - **上升或持平**：基準跟著走。0.6.0 起，上升夠多會另外產生加水事件，見下一條「直立不動時加水」。
  - **比基準低、還不到 3%**：基準**不往下跟**。小口喝可以一直累積，累積到 3% 才算。
  - **比基準低 ≥ 3%，而且持續夠久**：產生一筆**喝水**事件，`saw_drinking = 0`，A = 原本的基準、B = 確認當下的水位，基準改成 B。「持續夠久」= **連續 3 筆以上、而且持續 8 秒以上**都低於門檻（兩個條件都要；中間只要有一筆回到門檻以內就重新算，單筆雜訊不會觸發）。活動中每 100 ms 一筆，實際上就是連續 8 秒；休眠中每 5 秒一筆，實際上就是連續 3 筆（約 10 秒）。
  - 事件的開始時間（封包的時間戳）= 水位開始**連續低於「基準 − 3%」**的那一筆的時間，也就是產生事件前約 8～10 秒。不是用「第一次略低於基準」的時間：基準在穩定期間等於看過的最高值，讀值的雜訊會讓水位幾乎一直略低於基準，用那個時間會早上好幾個小時，還會誤標 `too_long`。小口累積的情況，時間戳是「最後跨過 3% 的那一口」的時間。
  - 一口氣喝很多（連續下降超過 8 秒）會拆成好幾筆前後相接的事件（上一筆的 B = 下一筆的 A），加起來等於實際下降的量。
- **直立不動時加水（0.6.0 起；杯子放在桌上，直接把水倒進去，沒有被拿起）**：
  - 基準照舊跟著水位往上走（喝水的計算不變），另外記住「這段上升是從哪個水位開始的」（起點 = 基準第一次被往上推之前的水位）。
  - **比起點高 ≥ 3%，而且連續 3 筆以上、持續 8 秒以上**：產生一筆**加水**事件，`saw_drinking = 0`，A = 起點、B = 這段期間看過的最高水位。開始時間 = 水位開始連續高於「起點 + 3%」的那一筆。和直立不動的喝水一樣，活動中實際上是連續 8 秒，休眠中是連續 3 筆（約 10 秒）。
  - **倒完水、還沒滿 8 秒就拿起杯子**（倒完馬上拿去喝）：已經連續高於門檻 2 秒以上（至少 2 筆）就在拿起的當下補記加水，B = 當時的基準；之後喝掉的量照常從倒完的水位算起，**加水和喝水各記一筆，不會互相抵銷**。不到 2 秒的不記，當成手碰到感應區造成的瞬間跳動。休眠中只量到 1 筆就被拿起也來不及確認，這筆加水不記（喝水不受影響）。
  - **記下起點後 60 秒內沒有升到 3%**：當成讀值往上漂，起點作廢重新記，不產生事件。加水不到 3% 也不記。
  - 倒水倒很久（超過 8 秒）會拆成前後相接的幾筆加水，加起來等於實際加的量。倒完馬上用吸管喝：加水記到最高水位，喝水從那個水位另外記一筆。
  - 參數在 `main/cup_state.c`：`EVT_STATIC_RISE_ENABLE`（預設跟著 `EVT_STATIC_DROP_ENABLE`）、`EVT_STATIC_RISE_PCT`、`EVT_STATIC_RISE_QUICK_MS`、`EVT_STATIC_RISE_WINDOW_MS`。
  - **已知風險（還沒實機驗證）**：和直立不動的喝水一樣只看電容讀值。手扶著杯子不動超過 2 秒、或其他東西靠近感應區讓讀值升高 3% 以上並持續 8 秒，會被記成加水。加水不計入飲水量，誤判的影響比喝水小，但實機上要測過。
- **兩條規則共用同一個基準，不會重複計算、也不會漏**：
  - 直立不動已經產生事件 → 基準已經是 B，之後拿起來喝只會從 B 算起。
  - 直立不動的下降還沒確認（還不到 3%，或還沒滿 8 秒）就拿起杯子 → 還沒確認的部分作廢，改由「拿起 → 放回」的結算用原本的基準算一次。
  - 拿起來喝完放回、結算之後水位又繼續慢慢下降（例如杯壁的水膜流下來、手離開）≥ 3% → 再補一筆直立不動的事件，兩筆相接，總和等於實際下降。
  - 小口吸了 2%（還不到 3%）之後杯子被碰了一下、或拿起來又放回 → 結算時下降不到 3%，基準維持原本的 A，這 2% 沒有丟掉；之後再降 2%，不論是直立不動還是拿起來喝，都從 A 算起記一筆 4%。
  - 基準**只會經由喝水事件往下走**（上升、加水才會往上），所以所有喝水事件的量加起來，剛好等於基準下降的量：同一段下降只會被記一次，還沒記到的部分（不到 3%）留在基準和目前水位之間。
- **休眠中**：進入休眠時是直立靜止的話，休眠中每 5 秒量到的水位套用同一條「直立不動」規則，產生的事件照樣寫進 flash、有連線就送；喚醒視為離開穩定。
- **待確認由 App 決定**：韌體只送原始旗標（`saw_drinking`、`too_long`）。依介面合約，種類 0（喝水）的事件一律計入喝水量，不論 `saw_drinking`、`too_long`（住民可能用吸管喝）。

**直立不動規則的已知風險（還沒實機驗證，一定要先測）**：這條規則只看電容讀值，分不出「水真的變少」和「讀值自己變低」。基準在穩定期間等於「看過的最高值」，之後只要比它低 3% 而且持續 8 秒就算喝水，所以：

- **讀值慢慢往下漂**（空杯讀值會漂移、溫度變化、水慢慢蒸發）：累積到 3% 就會被算成一筆喝水。
- **手或東西靠近不動的杯子**（沒有碰到、IMU 沒感覺）：讀值會變高，基準跟上去；手離開後讀值回到原本的值，如果差 ≥ 3% 而且超過 8 秒，會被算成喝水。這種情況加長持續時間也擋不掉，只能調高門檻。
- **雜訊本身太大**：靜置時水位讀值的跳動範圍（最高 − 最低）如果接近 3%，就會誤判。先看序列埠「水位%」欄在杯子靜置時跳多少。
- **每次放回的位置不同、讀值差一點**（桌面材質、旁邊有金屬或手、杯套歪了）：以前拿起放回差不到 3% 會直接被基準吸收；現在往下的差異會留著累積，好幾次加起來超過 3% 就會被記成一筆喝水，或加到下一口的量裡。
- 讀值有雜訊時，A（看過的最高值）會比真正的水位高一點（大約一個雜訊幅度），每筆事件的量會多算一點。
- 深度睡眠（`DEEP_SLEEP_ENABLE`，預設關）中沒有量水位。醒來後如果水位比睡前的基準低 ≥ 3%，放穩約 11 秒後補一筆，時間戳是醒來之後的時間，不是實際喝的時間；睡著期間的漂移也會被算進去。

怎麼調（都在 [main/cup_state.c](main/cup_state.c) 開頭）：

- 誤判太多：把 `EVT_STATIC_DELTA_PCT` 調大（例如 5.0），只影響直立不動的規則，不影響拿起來喝的偵測；或把 `EVT_STATIC_HOLD_MS`、`EVT_STATIC_MIN_SAMPLES` 調大（對雜訊有用，對漂移和手靠近沒用）。
- 整條規則關掉：`EVT_STATIC_DROP_ENABLE` 改成 0，回到 0.3.0 的行為：穩定期間水位升降都只更新基準（用吸管喝不會有事件），拿起放回結算時下降不到 3% 基準也照舊往下改成 B（小口不累積）。兩件事是同一個開關：穩定期間基準會往下跟的話，結算時留著 A 也只留得住一筆讀值的時間。
- 序列埠每筆事件都會印出來，直立不動產生的會多一段「，杯子直立不動」，可以分辨是哪一條規則產生的。

**value（uint32 LE）**：

| bits | 內容 |
|---|---|
| 7..0 | 拿起前水位 A，單位 0.5%（0～200） |
| 15..8 | 放回後水位 B，單位 0.5%（0～200） |
| 17..16 | 種類：0 = 喝水、1 = 加水，其他保留 |
| 18 | `saw_drinking`：過程中出現過「飲用中」姿態 |
| 19 | `too_long`：過程超過 10 分鐘 |
| 20 | `time_unknown`：時間不可信，timestamp 填 0（App 用收到的時間，記成待確認） |
| 31..21 | seq（11 bits，0～2047 循環），ACK 用 |

韌體只有水位比例（%），沒有 mL 概念；mL = 水位比例 × 容量，容量在 App 設定。

**ACK 與補傳**（[main/drink_log.c](main/drink_log.c)）：

1. 產生事件時先寫進 flash（NVS），有連線而且已訂閱就立刻送出。
2. App 收到後寫 ACK characteristic 3 bytes `[0x07, seq & 0xFF, (seq >> 8) & 0x07]`。韌體收到相符的 ACK 才從 flash 刪除。舊 App 只回 1 byte `[0x01]`，不影響事件，事件會一直留在 flash（滿了丟最舊的），這是預期行為。
3. App 訂閱後 1.5 秒（`SUBSCRIBE_RESEND_MS`）開始依序補傳所有未確認的事件，一次只在途一筆：送出後等 ACK 2 秒（休眠中 6 秒），逾時重送；同一筆重送 3 次還沒 ACK，這次連線就先暫停，下次連線再試（有新事件時會再試一次）。
4. ACK 晚到時同一筆可能收到兩次，**App 要照樣回 ACK，並用 seq + 時間戳 + A/B 去重**。seq 在清空 flash 後會從 0 重新開始。
5. flash 最多存 256 筆未確認的事件（`EVT_STORE_MAX`），滿了丟最舊的，序列埠會印警告。拔電、重新燒錄（沒有清 flash）後還在。

**時間**：水杯沒有 RTC 晶片，時間只靠 App 的 Time Sync。對時後產生的事件直接記 Unix 秒。對時前產生的事件，如果是**同一次開機**，在對時當下用「對時的 Unix 秒 −（對時當下的開機秒數 − 事件的開機秒數）」換算；換算不了的（之前開機留下的，例如對時前就斷電），送出時帶 `time_unknown`、timestamp = 0。

**可調參數**：

| 參數 | 位置 | 預設 | 說明 |
|---|---|---|---|
| `EVT_SETTLE_MS` | cup_state.c | 3000 | 直立、不在移動持續多久算穩定（ms） |
| `EVT_MIN_DELTA_PCT` | cup_state.c | 3.0 | 水位變化多少（%）才算事件；電極解析度約 2% |
| `EVT_MAX_EPISODE_MS` | cup_state.c | 600000 | 過程超過多久帶 `too_long`（10 分鐘） |
| `EVT_STATIC_DROP_ENABLE` | cup_state.c | 1 | 「基準不往下跟」（0.4.0）：直立不動時水位下降也算喝水，拿起放回下降不到門檻時基準維持 A。0 = 兩個都關掉，回到 0.3.0 的行為 |
| `EVT_STATIC_DELTA_PCT` | cup_state.c | `EVT_MIN_DELTA_PCT`（3.0） | 直立不動時比基準低多少（%）才算。可以單獨調大，不影響拿起來喝 |
| `EVT_STATIC_HOLD_MS` | cup_state.c | 8000 | 要連續低於門檻多久（ms）。水位是 1 秒的移動平均，單筆雜訊最多影響 1 秒，8 秒留了 8 倍；又比休眠中 3 筆的跨度（約 10 秒）短，RTC 時鐘 1% 的誤差不會讓休眠多等一輪 |
| `EVT_STATIC_MIN_SAMPLES` | cup_state.c | 3 | 至少連續幾筆低於門檻。主要管休眠（5 秒才一筆）：1 筆是單筆雜訊、2 筆仍可能是巧合，取 3 |
| `EVT_STORE_MAX` | drink_log.c | 256 | flash 最多存幾筆未確認的事件（2 的次方、≤ 1024） |
| `EVT_ACK_TIMEOUT_MS` / `EVT_ACK_TIMEOUT_DOZE_MS` | drink_log.c | 2000 / 6000 | 等 ACK 多久就重送（活動中／休眠中） |
| `EVT_MAX_RETRY` | drink_log.c | 3 | 同一筆重送幾次沒 ACK 就暫停到下次連線 |
| `SUBSCRIBE_RESEND_MS` | ble_gatt_svr.h | 1500 | App 訂閱後多久開始補送狀態與事件 |

序列埠會印 `[事件] 喝水 62.0% → 55.5%（seq 12，有飲用姿態，8 秒前開始）已存進 flash`、`[事件] 喝水 55.5% → 51.0%（seq 13，沒有飲用姿態，杯子直立不動，8 秒前開始）已存進 flash`、`[事件] seq 12 已確認，從 flash 刪除`、`[事件] App 已訂閱，1.5 秒後補傳 3 筆未確認的事件` 等訊息；每 20 行重印的表頭會顯示目前的基準水位和未確認事件筆數。

偵測邏輯和 Device Status 的內容可以在電腦上跑單元測試（不需要 ESP-IDF，要有 C 編譯器 `cc`）：

```bash
sh test/host/run.sh
```

會編兩次、跑兩次：預設參數，以及 `EVT_STATIC_DROP_ENABLE=0`（確認關掉之後回到 0.3.0 的行為）。兩次最後都要印「全部通過」。

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

[tools/ble_check.py](tools/ble_check.py) 會模擬 App 的流程：掃描 `SmartCup-`、對時、讀 Device Status（電量、版本、位址）、訂閱資料、回 ACK（DRINK_EVENT 回 3-byte ACK），最後統計收到的封包。Device Status 長度 ≥ 10 時會印出杯子回報的 BLE 位址，並和掃描到的位址比對（macOS 掃描到的是 UUID，不比對）。每一筆 DRINK_EVENT 都會印出種類、A → B、旗標、seq、時間。電腦要有藍牙，先裝 [bleak](https://pypi.org/project/bleak/)：

```bash
python3 -m venv ~/blevenv && ~/blevenv/bin/pip install bleak
~/blevenv/bin/python tools/ble_check.py 10            # 收 10 秒
~/blevenv/bin/python tools/ble_check.py 30 --no-ack   # 故意不回 0x07 的 ACK，測重送與下次連線補傳
```

正常的輸出（10 秒內 WATER、POSE 各約 10 筆，ATTITUDE 約 50 筆）。Device Status 那一行是 0.4.0 應該印出來的樣子（照格式推出來的，**0.4.0 還沒有實機跑過**）：

```
找到 SmartCup-E0CE 58:E6:C5:12:E0:CE
Service 存在: True
Device Status: [255, 0, 4, 0, 88, 230, 197, 18, 224, 206]（電量 未知，韌體 0.4.0，位址 58:E6:C5:12:E0:CE，和掃描到的位址一致）
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
| 喝水事件（0x07） | 偵測到就存 flash 並立刻送；訂閱後補傳未確認的 | 直立不動時水位下降 ≥ 3% 而且連續 3 筆（約 10 秒）→ 產生喝水事件（用吸管喝），存 flash、有連線就送；水位上升只更新基準。**照樣補傳未確認的事件、處理 ACK**（ACK 逾時放寬到 6 秒） |

- **什麼時候休眠**：從「最後一次移動」或「App 連上」（取較晚的）起算，靜止 30 秒。App 剛連上時一定先有 30 秒即時資料。
- **什麼時候醒**：拿起或晃動杯子，D1 被 IMU 拉高，250 ms 內恢復，App 收到 STATE「活動中」後畫面回到即時模式。
- **休眠中被 App 連上**：訂閱後 1.5 秒補送「休眠中」、最新水位和休眠前最後的姿態，App 一連上就有完整畫面（太早送 App 會漏掉，實測過）。
- **接著電腦 USB 時**：休眠流程照常（App 照樣顯示休眠中），但 CPU 不會真的淺眠（`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`），序列埠 monitor 不會斷。

序列埠會印出 `[休眠] 已靜止 30 秒，進入休眠（藍牙保持連線，拿起杯子就會喚醒）`、`[休眠] 偵測到移動，喚醒`。

### 耗電

這塊板子沒有外接 32 kHz 晶振，BLE 淺眠時要用主晶振當時脈，所以淺眠沒辦法省到很低。Espressif 的 `power_save` 範例文件列出 ESP32-C6「light sleep（main XTAL）」約 **3.3 mA**（官方數據，我們沒有實測）。702030 電池（約 300～400 mAh）粗估大概能撐 **4～5 天**休眠。

要更省電，可以打開深度睡眠（下一節），或之後在板子上加 32 kHz 晶振（官方數據淺眠約 34 µA）。

### 深度睡眠（預設關閉）

把 [main/main.c](main/main.c) 的 `DEEP_SLEEP_ENABLE` 改成 1 之後：休眠中**沒有 App 連線**超過 30 分鐘（`DEEP_SLEEP_AFTER_MS`），就進深度睡眠，藍牙關閉，非常省電。拿起杯子（IMU → D1，ext1 喚醒）或每 30 分鐘（`SLEEP_TIMER_WAKE_S`）會醒來。醒來後零點、水位、喝水偵測的基準與進行中的過程從 RTC 記憶體沿用（未確認的事件本來就在 flash）；「直立不動」規則還沒確認的下降不保留，醒來後重新確認（基準沒有往下跟，不會漏，見[第 5 節](#喝水加水事件0x07)的已知風險）；App 找不到水杯時會每 20 秒背景重試，醒來後自動連回。接著電腦 USB 時不會進深度睡眠。

### 參數

都在 [main/main.c](main/main.c) 開頭：

| 參數 | 預設 | 說明 |
|---|---|---|
| `DOZE_IDLE_MS` | 30000 | 靜止多久就休眠（ms） |
| `DOZE_KEEPALIVE_MS` | 10000 | 休眠中多久送一次 STATE |
| `DOZE_WATER_CHECK_MS` / `DOZE_WATER_FORCE_MS` / `DOZE_WATER_DELTA_PCT` | 5000 / 30000 / 1.0 | 休眠中量水位的間隔、沒變化時多久送一次、變化多少就立刻送 |
| `SUBSCRIBE_RESEND_MS` | 1500 | App 訂閱後多久補送目前狀態與未確認的事件（在 [main/ble_gatt_svr.h](main/ble_gatt_svr.h)） |
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
main/cup_state.c/.h  水位換算與保持、姿態（俯仰／翻滾／傾斜、分類、移動判斷）、喝水／加水事件偵測、電量 %（純計算）
main/drink_log.c/.h  喝水事件存 flash（NVS 環形佇列）、ACK、對時換算、補傳
main/tool_cmd.c/.h   校正工具的 USB 序列埠指令；水位校正值存 NVS（0.5.0 起）
main/ble_gatt_svr.c/.h  BLE GATT Server（NimBLE）：廣播、對時、Data Notify、ACK、Device Status
main/CMakeLists.txt  用到的 ESP-IDF 元件（esp_driver_i2c、esp_driver_gpio、esp_adc、esp_timer、bt、nvs_flash、esp_driver_usb_serial_jtag、esp_pm）
sdkconfig.defaults   主控台走 USB Serial/JTAG、4MB Flash、NimBLE、電源管理（自動淺眠）
tools/read_levels.py 校正用：錄一段電極讀值並算平均
tools/cup_tool.py    校正工具（視窗程式）：量空杯／滿杯、寫入校正值、燒錄韌體；套件見 tools/requirements-cup-tool.txt
tools/cup_updater.py 校正工具的自動更新（查 Release、下載、比對 SHA256SUMS.txt、替換）；測試在 tools/tests/（開發套件 tools/requirements-dev.txt）
tools/secret_guard.py  擋敏感資料：敏感檔名＋gitleaks（git hook 和 CI 共用）；tools/setup_hooks.sh 安裝 hook（選單）
.githooks/           pre-commit、pre-push hook（呼叫 secret_guard.py）
tools/ble_check.py   用電腦模擬 App 測試 BLE（含 0x07 的 ACK；--no-ack 測重送）
test/host/           cup_state.c 事件偵測、Device Status 內容的單元測試，在電腦上跑（sh test/host/run.sh）
tools/ble_watch.py   休眠測試：印出 BLE 廣播出現／消失的時間軸
docs/wiring.pdf      接線圖、電極尺寸、接線順序
.github/workflows/secret-scan.yml  GitHub Actions：敏感資料掃描（整個 git 歷史）；main／PR 由 build.yml 呼叫，其他分支 push 時自己跑
.github/workflows/build.yml  GitHub Actions：敏感資料掃描、單元測試 → ESP-IDF v6.1 建置韌體、打包校正工具（macOS／Windows／Linux）→ 上傳；版號改了推上 main 自動建 tag 和 Release
HANDOFF.md           開發交接筆記（ESP-IDF v6 注意事項、已踩過的坑）
```
