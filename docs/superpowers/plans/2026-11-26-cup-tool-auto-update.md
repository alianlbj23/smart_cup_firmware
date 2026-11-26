# 校正工具自動更新 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 校正工具打包成 macOS／Windows／Linux 執行檔，改版號推上 main 自動發版，工具啟動時偵測新版並更新自己。

**Architecture:** 新增純邏輯模組 `tools/cup_updater.py`（不依賴 Tkinter，網路與 subprocess 都可注入替身），
`tools/cup_tool.py` 只做介面串接；CI 在 `build.yml` 加 Linux 打包與最後的 `release` job。

**Tech Stack:** Python 3.13（CI）／3.12（本機）、Tkinter、PyInstaller 6.22.3、pytest 9.2.0、GitHub Actions。

**Spec:** `docs/superpowers/specs/2026-11-26-cup-tool-auto-update-design.md`

## Global Constraints
- 資產檔名：`SmartCupTool-macOS.zip`、`SmartCupTool-Windows.zip`、`SmartCupTool-Linux.zip`、`SHA256SUMS.txt`。
- zip 內容：macOS `SmartCupTool.app/`、Windows `SmartCupTool.exe`、Linux `SmartCupTool`（都在 zip 根目錄）。
- 版號來源：`main/ble_gatt_svr.h` `#define CUP_FW_MAJOR/MINOR/PATCH`；打包時寫成 `VERSION` 檔包進根目錄（`sys._MEIPASS/VERSION`）。
- Release API：`https://api.github.com/repos/alianlbj23/smart_cup_firmware/releases/latest`；網路 timeout 查詢 10 秒、下載 60 秒；下載上限 300 MB。
- 重新啟動要帶 `PYINSTALLER_RESET_ENVIRONMENT=1`（PyInstaller ≥6.10 規定）。
- 套件固定版本：pyserial==3.5、esptool==5.7.0、certifi==2026.11.20、pytest==9.2.0、pyinstaller==6.22.3。
- 不吞例外：失敗都要丟 `UpdateError`（訊息中文、給使用者看）或 log。

## Review Focus
- GitHub 回傳奇怪 JSON（不是 dict、assets 不是 list、size 不是 int）→ `UpdateError`，不崩潰。（Task 1 測試）
- 安裝到一半失敗（例如新檔搬不過去）→ 舊版原封不動還原。（Task 2 測試）
- zip 裡有 `../` 之類路徑（zip slip）→ 拒絕。（Task 2 測試）
- macOS App 從「下載」直接執行（App Translocation，路徑含 `AppTranslocation`）或資料夾沒寫入權限 → 提示先搬到「應用程式」或手動下載。（Task 2 測試）
- 從原始碼執行（沒打包、版號 dev）→ 不自我更新。（Task 1 測試）

---

### Task 1: 版號與 Release 解析（`tools/cup_updater.py`）

**Files:** Create `tools/cup_updater.py`、`tools/tests/conftest.py`（把 `tools/` 加進 `sys.path`）、`tools/tests/test_cup_updater.py`

**Produces:**
- `class UpdateError(Exception)`
- `parse_version(text: str) -> tuple[int, int, int]`：接受 `0.5.0`、`v0.5.0`（前後空白可），其他 `ValueError`
- `read_header_version(header_text: str) -> str`：缺任一 define → `ValueError`
- `current_version(bundle_dir: Path | None, repo_root: Path) -> str`：有 bundle_dir 讀 `VERSION`，否則讀標頭；失敗回 `"dev"`
- `asset_name(platform: str) -> str`：`darwin`/`win32`/`linux*`，其他 `UpdateError`
- `@dataclass(frozen=True) Update(version: str, tag: str, url: str, size: int, sums_url: str, page_url: str)`
- `pick_update(release: object, current: str, platform: str) -> Update | None`
- `parse_sums(text: str) -> dict[str, str]`（檔名 → 小寫 hex，容許 `*檔名` 二進位標記）

- [ ] 寫測試：`parse_version("v0.5.0") == (0,5,0)`；`parse_version("0.5")` 與 `"abc"` 丟 ValueError；
  `read_header_version` 讀出 `"0.5.0"`；`current_version` 讀 VERSION、讀標頭、兩者都壞回 `"dev"`；
  `asset_name("linux") == "SmartCupTool-Linux.zip"`、`"sunos5"` 丟 UpdateError；
  `pick_update`：新版回 Update、同版／舊版回 None、current=`"dev"` 回 None、沒有平台資產回 None、
  release 不是 dict／assets 不是 list／size 不是 int → UpdateError、缺 SHA256SUMS.txt → UpdateError；
  `parse_sums` 解析兩行、略過空行。
- [ ] 跑 `pytest tools/tests -q` 確認 FAIL（ImportError）
- [ ] 實作
- [ ] 跑測試 PASS

### Task 2: 下載、驗證、安裝、重新啟動（`tools/cup_updater.py`）

**Consumes:** Task 1。**Produces:**
- `download(url: str, dest: Path, opener, timeout: float, max_bytes: int, expected_size: int | None = None) -> Path`
  （`opener(request, timeout=...)` 回傳有 `.read(n)` 的 context manager；超過上限或大小不符 → UpdateError）
- `verify_sha256(path: Path, expected: str) -> None`
- `install_target(exe: Path, platform: str) -> Path`：macOS 回 `exe.parents[2]`（必須是 `*.app`），其他回 exe；
  路徑含 `AppTranslocation` → UpdateError（請先把 App 搬到「應用程式」）
- `install(zip_path: Path, platform: str, target: Path, run=subprocess.run) -> Path`：依規格換檔；失敗還原 `.old`；
  macOS 用 `run(["ditto", "-x", "-k", zip, dir], check=True, timeout=120)` 解壓；其他用 zipfile 並擋 zip slip
- `cleanup_old(target: Path) -> None`：刪 `<target>.old`（檔案或資料夾）
- `restart_command(platform: str, target: Path) -> list[str]`：macOS `["open", "-n", str(target)]`，其他 `[str(target)]`
- `restart_env(environ: Mapping[str, str]) -> dict[str, str]`：加 `PYINSTALLER_RESET_ENVIRONMENT=1`
- `check_writable(dir: Path) -> None`：無法寫入 → UpdateError

- [ ] 寫測試（`tmp_path`、假 opener、假 run）：下載成功／超過上限／大小不符；sha256 相符／不符；
  Linux／Windows 安裝後內容是新版、Windows 留 `.old`、Linux 有執行權限；
  zip 缺檔 → UpdateError 且舊檔還在；zip slip → UpdateError；`os.replace` 失敗（monkeypatch）→ 舊檔還原；
  macOS 用假 run 建出 .app 後替換、舊 .app 被刪；`install_target` 的 .app／Translocation；`cleanup_old`；`restart_*`。
- [ ] FAIL → 實作 → PASS

### Task 3: 介面串接（`tools/cup_tool.py`）

**Consumes:** Task 1、2。
- 啟動時 `cleanup_old`；標題加版號；「3. 燒錄韌體」旁加「檢查工具更新」按鈕；啟動 2 秒後背景查（`--no-update-check` 關掉）。
- 未打包（`sys.frozen` 不存在）：查到新版只寫紀錄並提示 Release 頁，不安裝。
- 打包：askyesno → 背景下載 zip 與 SHA256SUMS → 驗證 → install → `subprocess.Popen(restart_command, env=restart_env)` → 關視窗。
- 工具忙（燒錄、量測中）時自動檢查延後。
- [ ] `--self-test` 增加檢查 `current_version` 可讀（打包時必須不是 dev）並且不連網。
- [ ] `python tools/cup_tool.py --self-test`（xvfb-run）通過。

### Task 4: CI、套件、文件

**Files:** `.github/workflows/build.yml`、`tools/requirements-cup-tool.txt`、`tools/requirements-dev.txt`（新）、`README.md`、`HANDOFF.md`（如有工具段落）
- host-test：安裝 requirements-dev，跑 `python -m pytest tools/tests -q`。
- tool matrix 加 Linux（ubuntu-latest，裝 xvfb、`xvfb-run -a` 跑 self-test，`zip` 打包）；三平台寫 `VERSION` 並 `--add-data`。
- 移除 build／tool job 的 Release 步驟，新增 `release` job（needs build、tool；push main 或 v* tag），
  tag 已存在跳過；tag push 時 tag 必須等於版號；產生 `SHA256SUMS.txt`；`make_latest: true`。
- [ ] 本機：`pytest`、`actionlint`（可用時）或 YAML 解析檢查、Linux PyInstaller 打包＋self-test。
