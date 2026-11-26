"""
校正工具（tools/cup_tool.py）的自我更新：查 GitHub 最新 Release、下載、比對 SHA256SUMS.txt、換掉自己、重新啟動。

不依賴 Tkinter；網路（opener）和外部指令（run）都可以從參數換掉，方便測試。
Release 的檔案由 .github/workflows/build.yml 的 release job 產生：
    SmartCupTool-macOS.zip（SmartCupTool.app/）、SmartCupTool-Windows.zip（SmartCupTool.exe）、
    SmartCupTool-Linux.zip（SmartCupTool）、SHA256SUMS.txt
"""

from __future__ import annotations

import hashlib
import http.client
import json
import os
import re
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.request
import zipfile
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path

GITHUB_REPO = "alianlbj23/smart_cup_firmware"
LATEST_API = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
APP_NAME = "SmartCupTool"
SUMS_NAME = "SHA256SUMS.txt"
DEV = "dev"
_ASSETS = {"darwin": f"{APP_NAME}-macOS.zip", "win32": f"{APP_NAME}-Windows.zip", "linux": f"{APP_NAME}-Linux.zip"}
_VERSION_RE = re.compile(r"v?(\d+)\.(\d+)\.(\d+)")
_DEFINE_RE = re.compile(r"^#define\s+CUP_FW_(MAJOR|MINOR|PATCH)\s+(\d+)\s*$", re.MULTILINE)
_SUM_RE = re.compile(r"^([0-9a-fA-F]{64}) [ *](.+)$")
_EXE = {"darwin": f"{APP_NAME}.app", "win32": f"{APP_NAME}.exe", "linux": APP_NAME}
USER_AGENT = "smart-cup-tool"
MAX_JSON_BYTES = 2_000_000
CHUNK = 64 * 1024
# opener(request, timeout=秒) → 有 read(n) 的 context manager（預設是 urllib.request.urlopen + 憑證）
Opener = Callable[..., object]


class UpdateError(Exception):
    """更新失敗；訊息直接給使用者看"""


@dataclass(frozen=True)
class Update:
    version: str     # 例如 "0.6.0"
    tag: str         # 例如 "v0.6.0"
    url: str         # 這個平台的 zip
    size: int        # zip 的位元組數（GitHub 給的）
    sums_url: str    # SHA256SUMS.txt
    page_url: str    # Release 網頁（手動下載用）


# ---------------- 版號 ----------------
def parse_version(text: str) -> tuple[int, int, int]:
    """「0.5.0」或「v0.5.0」→ (0, 5, 0)；其他格式丟 ValueError"""
    m = _VERSION_RE.fullmatch(text.strip())
    if not m:
        raise ValueError(f"版號格式不對：{text!r}")
    return int(m[1]), int(m[2]), int(m[3])


def read_header_version(header_text: str) -> str:
    """main/ble_gatt_svr.h 的 CUP_FW_MAJOR/MINOR/PATCH → 「0.5.0」"""
    found = dict(_DEFINE_RE.findall(header_text))
    try:
        return f"{found['MAJOR']}.{found['MINOR']}.{found['PATCH']}"
    except KeyError as e:
        raise ValueError(f"標頭檔缺少 CUP_FW_{e.args[0]}") from None


def current_version(bundle_dir: Path | None, repo_root: Path) -> str:
    """打包後讀 bundle_dir/VERSION（CI 寫入）；從原始碼執行讀 main/ble_gatt_svr.h；都讀不到回 "dev"（不自我更新）"""
    try:
        if bundle_dir is not None:
            text = (bundle_dir / "VERSION").read_text(encoding="utf-8").strip()
        else:
            text = read_header_version((repo_root / "main" / "ble_gatt_svr.h").read_text(encoding="utf-8"))
        parse_version(text)
        return text.lstrip("v")
    except (OSError, ValueError):
        return DEV


# ---------------- Release ----------------
def asset_name(platform: str) -> str:
    """sys.platform → 這個平台在 Release 裡的 zip 檔名"""
    key = "linux" if platform.startswith("linux") else platform
    try:
        return _ASSETS[key]
    except KeyError:
        raise UpdateError(f"這個作業系統（{platform}）沒有自動更新，請到 GitHub Release 手動下載") from None


def _https(url: object, what: str) -> str:
    if not isinstance(url, str) or not url.startswith("https://"):
        raise UpdateError(f"Release 裡 {what} 的下載網址不對：{url!r}")
    return url


def pick_update(release: object, current: str, platform: str) -> Update | None:
    """GitHub /releases/latest 的 JSON → 比目前新、有這個平台的檔案就回 Update，否則 None。JSON 格式不對丟 UpdateError"""
    if not isinstance(release, dict):
        raise UpdateError("GitHub 回傳的 Release 格式不對")
    tag, assets = release.get("tag_name"), release.get("assets")
    if not isinstance(tag, str) or not isinstance(assets, list):
        raise UpdateError("GitHub 回傳的 Release 缺少 tag_name 或 assets")
    try:
        latest = parse_version(tag)
    except ValueError:
        raise UpdateError(f"最新 Release 的 tag 不是版號：{tag!r}") from None
    by_name: dict[str, dict] = {}
    for a in assets:
        if not isinstance(a, dict) or not isinstance(a.get("name"), str):
            raise UpdateError("GitHub 回傳的 Release 檔案清單格式不對")
        by_name[a["name"]] = a

    if current == DEV:
        return None
    try:
        if latest <= parse_version(current):
            return None
    except ValueError:
        return None
    asset = by_name.get(asset_name(platform))
    if asset is None:
        return None
    sums = by_name.get(SUMS_NAME)
    if sums is None:
        raise UpdateError(f"Release {tag} 沒有 {SUMS_NAME}，無法驗證檔案，不自動更新")
    size = asset.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        raise UpdateError(f"Release 裡 {asset['name']} 的大小不對：{size!r}")
    page = release.get("html_url")
    return Update(
        version="%d.%d.%d" % latest,
        tag=tag,
        url=_https(asset.get("browser_download_url"), asset["name"]),
        size=size,
        sums_url=_https(sums.get("browser_download_url"), SUMS_NAME),
        page_url=page if isinstance(page, str) else f"https://github.com/{GITHUB_REPO}/releases/latest",
    )


def parse_sums(text: str) -> dict[str, str]:
    """sha256sum 的輸出 → {檔名: 小寫 hex}；看不懂的行略過"""
    out = {}
    for line in text.splitlines():
        m = _SUM_RE.match(line.strip())
        if m:
            out[m[2].strip()] = m[1].lower()
    return out


# ---------------- 網路 ----------------
_NET_ERRORS = (urllib.error.URLError, OSError, http.client.HTTPException)   # IncompleteRead 不是 OSError


def _request(url: str, api: bool) -> urllib.request.Request:
    headers = {"User-Agent": USER_AGENT}
    if api:   # 下載檔案（browser_download_url）不帶 API 的 Accept
        headers["Accept"] = "application/vnd.github+json"
    return urllib.request.Request(url, headers=headers)


def _net_error(e: Exception, what: str) -> UpdateError:
    if isinstance(e, urllib.error.HTTPError):
        if e.code == 404:
            return UpdateError("GitHub 上還沒有 Release")
        return UpdateError(f"{what}失敗：HTTP {e.code}")
    return UpdateError(f"{what}失敗（連不上 GitHub？）：{e}")


def fetch_json(url: str, opener: Opener, timeout: float) -> object:
    """GET 一個 JSON（最多 2 MB）；網路、HTTP、JSON 錯誤都丟 UpdateError"""
    try:
        with opener(_request(url, api=True), timeout=timeout) as r:
            body = r.read(MAX_JSON_BYTES + 1)
    except _NET_ERRORS as e:   # HTTPError、逾時（TimeoutError）都是 OSError
        raise _net_error(e, "查詢最新版本") from e
    if len(body) > MAX_JSON_BYTES:
        raise UpdateError("GitHub 回傳的資料太大")
    try:
        return json.loads(body)
    except ValueError as e:
        raise UpdateError(f"GitHub 回傳的不是 JSON：{e}") from e


def download(url: str, dest: Path, opener: Opener, timeout: float, max_bytes: int,
             expected_size: int | None = None) -> Path:
    """下載到 dest；超過 max_bytes、大小和 expected_size 不同、網路錯誤都丟 UpdateError 並刪掉半個檔案"""
    total = 0
    try:
        with opener(_request(url, api=False), timeout=timeout) as r, open(dest, "wb") as f:
            while chunk := r.read(CHUNK):
                total += len(chunk)
                if total > max_bytes:
                    raise UpdateError(f"下載的檔案超過 {max_bytes // 1_000_000} MB，停止下載")
                f.write(chunk)
        if expected_size is not None and total != expected_size:
            raise UpdateError(f"下載不完整：收到 {total} bytes，應該是 {expected_size}")
    except UpdateError:
        dest.unlink(missing_ok=True)
        raise
    except _NET_ERRORS as e:
        dest.unlink(missing_ok=True)
        raise _net_error(e, "下載新版") from e
    return dest


def verify_sha256(path: Path, expected: str) -> None:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(CHUNK):
            h.update(chunk)
    if h.hexdigest() != expected.lower():
        raise UpdateError(f"{path.name} 的 SHA256 和 Release 的 SHA256SUMS.txt 不符，檔案可能損壞或被竄改，不安裝")


# ---------------- 安裝 ----------------
def _key(platform: str) -> str:
    return "linux" if platform.startswith("linux") else platform


def install_target(exe: Path, platform: str) -> Path:
    """要換掉的東西：macOS 是整個 .app（exe 在 X.app/Contents/MacOS/ 裡），其他平台是執行檔本身"""
    if _key(platform) != "darwin":
        return exe
    if "AppTranslocation" in exe.parts:
        raise UpdateError("macOS 從「下載」直接開的 App 不能自動更新：請先把 SmartCupTool 拖到「應用程式」資料夾，從那裡打開")
    app = exe.parents[2] if len(exe.parents) > 2 else exe
    if app.suffix != ".app" or exe.parent.name != "MacOS":
        raise UpdateError(f"找不到 App 的位置（{exe}），請到 GitHub Release 手動下載")
    return app


def check_writable(folder: Path) -> None:
    """自動更新要在程式所在的資料夾寫檔；不行就請使用者手動下載"""
    try:
        with tempfile.NamedTemporaryFile(dir=folder, prefix=".write-test-"):
            pass
    except OSError as e:
        raise UpdateError(f"沒有權限寫入 {folder}（{e.strerror}），無法自動更新，請到 GitHub Release 手動下載") from e


def _extract(zip_path: Path, platform: str, work: Path, run) -> Path:
    name = _EXE[_key(platform)]
    if _key(platform) == "darwin":
        # .app 裡有 symlink，Python 的 zipfile 解不回來；ditto 是 macOS 內建的，CI 也是用它壓的
        try:
            run(["ditto", "-x", "-k", str(zip_path), str(work)], check=True, timeout=120)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError) as e:
            raise UpdateError(f"解壓縮新版失敗：{e}") from e
        new = work / name
        if not new.is_dir() or new.is_symlink():
            raise UpdateError(f"新版的 zip 裡沒有 {name}")
        return new
    # 只取根目錄那一個檔案，其他路徑（包括 ../ 之類）一律不解
    try:
        with zipfile.ZipFile(zip_path) as z:
            try:
                info = z.getinfo(name)
            except KeyError:
                raise UpdateError(f"新版的 zip 裡沒有 {name}") from None
            new = work / name
            with z.open(info) as src, open(new, "wb") as dst:
                shutil.copyfileobj(src, dst)
    except (zipfile.BadZipFile, OSError) as e:
        raise UpdateError(f"新版的 zip 壞了：{e}") from e
    return new


def _old(target: Path) -> Path:
    return target.with_name(target.name + ".old")


def install(zip_path: Path, platform: str, target: Path,
            run: Callable[..., object] = subprocess.run) -> Path:
    """
    用 zip 裡的新版換掉 target（install_target 的回傳值），回傳 target。
    Linux：os.replace 直接蓋（執行中的檔案可以換）。
    Windows／macOS：舊的改名成 .old 再把新的搬過去（Windows 執行中的 exe 只能改名不能刪），
    搬失敗就把 .old 改回來；.old 等下次啟動時 cleanup_old 刪。
    注意：換檔之後，執行中的舊程式不要再 import 還沒載入的模組（PyInstaller 會從已經換掉的檔案讀），只做重新啟動。
    """
    key = _key(platform)
    try:
        work = Path(tempfile.mkdtemp(prefix=f".{APP_NAME}-update-", dir=target.parent))
    except OSError as e:
        raise UpdateError(f"沒有權限寫入 {target.parent}，無法自動更新：{e}") from e
    try:
        new = _extract(zip_path, platform, work, run)
        try:
            if key == "linux":
                new.chmod(0o755)
                os.replace(new, target)
                return target
            cleanup_old(target)
            old = _old(target)
            os.replace(target, old)
            try:
                os.replace(new, target)
            except OSError:
                os.replace(old, target)
                raise
        except OSError as e:
            raise UpdateError(f"換成新版失敗（舊版保留不動）：{e}") from e
        return target
    finally:
        shutil.rmtree(work, ignore_errors=True)   # 暫存資料夾；刪不掉只是留垃圾，不影響更新結果


def cleanup_old(target: Path) -> None:
    """刪掉上次更新留下的 <target>.old；刪不掉丟 OSError 給呼叫端記錄"""
    old = _old(target)
    if old.is_dir() and not old.is_symlink():
        shutil.rmtree(old)
    elif old.exists() or old.is_symlink():
        old.unlink()


def restart_command(platform: str, target: Path) -> list[str]:
    if _key(platform) == "darwin":
        return ["open", "-n", str(target)]
    return [str(target)]


def restart_env(environ: Mapping[str, str]) -> dict[str, str]:
    """PyInstaller 6.10 起，重新啟動自己要設這個，新程式才會重新解開，不用舊程式結束時會刪的暫存檔"""
    return {**environ, "PYINSTALLER_RESET_ENVIRONMENT": "1"}


# ---------------- 整套流程（cup_tool.py 呼叫） ----------------
CHECK_TIMEOUT_S = 10
DOWNLOAD_TIMEOUT_S = 60
MAX_ZIP_BYTES = 300_000_000
MAX_SUMS_BYTES = 1_000_000


def latest_update(current: str, platform: str, opener: Opener) -> Update | None:
    """查 GitHub 最新 Release；比目前新回 Update，否則 None"""
    return pick_update(fetch_json(LATEST_API, opener, CHECK_TIMEOUT_S), current, platform)


def apply_update(update: Update, platform: str, target: Path, opener: Opener,
                 run: Callable[..., object] = subprocess.run, log: Callable[[str], None] = lambda s: None) -> Path:
    """下載 zip 和 SHA256SUMS.txt → 比對 → 換掉 target。任何一步失敗丟 UpdateError，舊版不動"""
    check_writable(target.parent)
    name = asset_name(platform)
    with tempfile.TemporaryDirectory(prefix=f"{APP_NAME}-download-") as tmp:
        sums_file = download(update.sums_url, Path(tmp) / SUMS_NAME, opener, DOWNLOAD_TIMEOUT_S, MAX_SUMS_BYTES)
        expected = parse_sums(sums_file.read_text(encoding="utf-8", errors="replace")).get(name)
        if expected is None:
            raise UpdateError(f"{SUMS_NAME} 裡沒有 {name}，無法驗證，不安裝")
        log(f"[更新] 下載 {update.tag} 的 {name}（{update.size / 1_000_000:.1f} MB）…")
        zip_path = download(update.url, Path(tmp) / name, opener, DOWNLOAD_TIMEOUT_S, MAX_ZIP_BYTES, update.size)
        verify_sha256(zip_path, expected)
        log("[更新] SHA256 相符，安裝中 …")
        return install(zip_path, platform, target, run=run)
