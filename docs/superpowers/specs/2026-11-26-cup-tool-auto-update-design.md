# 校正工具：三平台執行檔、自動發版、自動更新 — 設計

## 目的
`tools/cup_tool.py`（Tkinter 校正＋燒錄）目前只打包 macOS／Windows，而且要手動推 tag 才有 Release。
目標：不懂 Python 的人下載一個檔案就能用（macOS／Windows／Linux），開發者改版號推上 main 就自動發版，
使用者打開工具時自動偵測新版、確認後自己換成新版並重新啟動。

## 使用者決定的部分
- 自動更新 = 工具更新自己（下載 → 替換 exe／App → 重新啟動），不是只提示。
- 發版 = 推上 main 時 CI 讀版號，`v<版號>` tag 還不存在就自動建 tag + Release；不必手動打 tag。
- 工具版號 = 韌體版號（`main/ble_gatt_svr.h` 的 `CUP_FW_MAJOR/MINOR/PATCH`），一個 Release 同時放韌體與三平台工具。
- 新增 pytest、pyinstaller（固定版本，放 `tools/requirements-dev.txt`）；執行用套件也改固定版本。

## 範圍
- 目標：Linux 執行檔、自動發版 job、工具自我更新、pytest 測試（TDD）並在 CI 跑。
- 非目標：程式碼簽章／公證（macOS Gatekeeper、Windows SmartScreen 警告維持現狀）、Intel Mac 版、差異更新、韌體自動燒錄。
- 反目標：更新失敗把使用者手上能用的工具弄壞；下載的檔案沒驗證就執行。

## 設計
### 版本
- PyInstaller 打包時把 `VERSION` 檔（內容如 `0.5.0`）包進去；執行時讀它。從原始碼執行（沒打包）時讀 `main/ble_gatt_svr.h`，
  且**不做自我更新**（只在紀錄提示有新版）。

### Release 資產（合約）
- 韌體：沿用現有檔名（`flash_args`、`*.bin`…），燒錄說明改名 `FLASHING.txt`（Release 檔名不能有中文）。
- 工具：`SmartCupTool-macOS.zip`（ditto 壓的 .app）、`SmartCupTool-Windows.zip`（`SmartCupTool.exe`）、
  `SmartCupTool-Linux.zip`（`SmartCupTool` 單一執行檔）。
- `SHA256SUMS.txt`：所有資產的 `sha256  檔名`。工具更新時**沒有這個檔或雜湊不符就拒絕更新**。

### 自我更新流程（`tools/cup_updater.py`，不依賴 Tkinter，可單元測試）
1. 啟動 2 秒後在背景查 `GET /repos/alianlbj23/smart_cup_firmware/releases/latest`（timeout 10 秒），驗證 JSON 型別。
2. 最新版號 > 目前版號（語意版號三段整數比較；目前版號是 `dev`／解析失敗就不更新）且有對應平台資產 → 問使用者。
3. 下載到同一磁碟的暫存位置（timeout、大小上限 300 MB、比對 `SHA256SUMS.txt`）。
4. 安裝：
   - Windows：解出新 exe → 把執行中的 exe 改名成 `.old` → 新 exe 移到原位置；下次啟動刪 `.old`。
   - Linux：解出新檔 → `chmod 755` → `os.replace` 蓋掉原檔。
   - macOS：用 `ditto -x -k` 解壓（保留 .app 裡的 symlink）→ 舊 .app 改名 `.old` → 新 .app 移到原位置；`.old` 下次啟動才刪（執行中的程式碼還在裡面）。
   - 任何一步失敗就把 `.old` 改回來，並回報錯誤；原位置沒有寫入權限時提示手動下載。
5. 重新啟動新版（macOS 用 `open -n`），舊程式結束。
- 正在燒錄或連線中不打擾：更新提示等工具閒置時才跳；使用者也可以按「檢查工具更新」（連線中會提醒要中斷連線）。
- 從原始碼執行：自動檢查只寫紀錄；手動檢查才問要不要開 Release 網頁。
- SHA256SUMS 和 zip 在同一個 Release，只防損壞／傳輸錯誤，防不了 repo 或帳號被入侵（簽章是非目標）。
- `--no-update-check` 參數與 `--self-test` 不查更新。

### CI（`.github/workflows/build.yml`）
- `host-test` 加跑 `pytest tools/tests`。
- `tool` matrix 加 Linux（固定 ubuntu-24.04，glibc 2.39，避免 runner 升級後舊系統跑不起來；`xvfb-run` 跑 `--self-test`）；三平台都寫 `VERSION` 並 `--add-data`。
- 移除各 job 裡的 Release 步驟，改成最後一個 `release` job：只在 push main 跑（使用者決定：手動推 tag 不發版），
  下載全部 Artifacts、產生 `SHA256SUMS.txt`；已有發布過且含 `SHA256SUMS.txt` 的 `v<版號>` Release 就跳過。
  先上傳到 draft，全部上傳完才發布（發布時才建 tag），中途失敗重跑能補上；main 不 cancel-in-progress。

## 留給實作者
- 模組切分、函式命名、測試替身（假 opener／假檔案系統用 `tmp_path`）。
