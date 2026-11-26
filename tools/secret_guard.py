#!/usr/bin/env python3
"""
擋敏感資料上傳：敏感檔名（.env、私鑰、憑證…）＋ gitleaks 內容掃描（API key、token、密碼…）。

    python3 tools/secret_guard.py install      下載固定版本的 gitleaks 到 .tools/（比對 SHA256）
    python3 tools/secret_guard.py pre-commit   掃這次要 commit 的內容（.githooks/pre-commit 呼叫）
    python3 tools/secret_guard.py pre-push     掃要 push 的 commit（.githooks/pre-push 呼叫，refs 從 stdin 讀）
    python3 tools/secret_guard.py scan         掃整個 git 歷史（CI 用）

安裝 hook 用互動選單：bash tools/setup_hooks.sh
找到東西時 gitleaks 的輸出會遮住（--redact），不會把金鑰印到終端機或 CI 紀錄。
誤判：內容在那一行加註解 gitleaks:allow，或把 gitleaks 給的 Fingerprint 加進 .gitleaksignore；
      檔名（例如公開憑證 ca_cert.pem）把路徑的 glob 加進 .secret-guard-allow。
只用標準函式庫（hook 會用系統的 python3 執行）。
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import http.client
import io
import os
import platform
import re
import ssl
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from collections.abc import Callable, Iterable
from pathlib import Path, PurePosixPath

GITLEAKS_VERSION = "8.30.1"
# 來自 GitHub Release 的 digest（https://github.com/gitleaks/gitleaks/releases/tag/v8.30.1）
GITLEAKS_SHA256 = {
    "gitleaks_8.30.1_linux_x64.tar.gz": "551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb",
    "gitleaks_8.30.1_linux_arm64.tar.gz": "e4a487ee7ccd7d3a7f7ec08657610aa3606637dab924210b3aee62570fb4b080",
    "gitleaks_8.30.1_darwin_x64.tar.gz": "dfe101a4db2255fc85120ac7f3d25e4342c3c20cf749f2c20a18081af1952709",
    "gitleaks_8.30.1_darwin_arm64.tar.gz": "b40ab0ae55c505963e365f271a8d3846efbc170aa17f2607f13df610a9aeb6a5",
    "gitleaks_8.30.1_windows_x64.zip": "d29144deff3a68aa93ced33dddf84b7fdc26070add4aa0f4513094c8332afc4e",
    "gitleaks_8.30.1_windows_arm64.zip": "b95f5e4f5c425cedca7ee203d9afd29597e692c4924a12ed42f970537c72cc0f",
}
DOWNLOAD_URL = "https://github.com/gitleaks/gitleaks/releases/download/v{ver}/{asset}"
DOWNLOAD_TIMEOUT_S = 60
MAX_DOWNLOAD_BYTES = 50_000_000
GIT_TIMEOUT_S = 60
GITLEAKS_TIMEOUT_S = 600
LEAK_EXIT = 77          # gitleaks 找到東西時的結束代碼（1 是 gitleaks 自己出錯）
GITLEAKS_DIR = ".tools"   # gitleaks 裝在 repo 的 .tools/（.gitignore）
ALLOW_FILE = ".secret-guard-allow"   # 例外：每行一個路徑 glob（例如公開憑證 main/certs/*_cert.pem）
# 掃 merge commit 自己的變更（解衝突時寫進去的東西）；git log 預設不顯示 merge 的 diff
MERGE_OPTS = ["--diff-merges=first-parent"]

# 檔名（不分大小寫）符合就不准 commit，不管內容。.gitignore 要有同樣的規則（測試會檢查）
BLOCKED_NAME_PATTERNS = [
    ".env", ".env.*", "*.env", "*.pem", "*.key", "*.p12", "*.pfx", "*.p8", "*.jks", "*.keystore", "*.ppk",
    "*.kdbx", "*.tfstate", "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519",
    "credentials.json", "service-account*.json", "secrets.json", "secrets.yaml", "secrets.yml", "secrets.toml",
    ".netrc", ".pypirc", ".npmrc", ".git-credentials", ".htpasswd",
    "*flash_encryption_key*.bin", "nvs_keys*.bin",          # ESP32 flash／NVS 加密金鑰
]
ALLOWED_NAME_PATTERNS = [".env.example", ".env.sample", ".env.template"]
BLOCKED_PATH_SUFFIXES = [".claude/settings.local.json"]   # 任何一層資料夾底下都擋
_ZERO_SHA = re.compile(r"^0{40}$|^0{64}$")
_SHA = re.compile(r"^[0-9a-f]{40}$|^[0-9a-f]{64}$")

Opener = Callable[..., object]


class GuardError(Exception):
    """設定或執行錯誤（不是「找到敏感資料」）；訊息給人看"""


# ---------------- 敏感檔名 ----------------
def is_blocked_path(path: str, allow: Iterable[str] = ()) -> bool:
    """allow：.secret-guard-allow 的路徑 glob（不分大小寫），符合就放行"""
    full = PurePosixPath(path.replace("\\", "/")).as_posix().lower()
    name = full.rsplit("/", 1)[-1]
    if any(fnmatch.fnmatchcase(name, pat) for pat in ALLOWED_NAME_PATTERNS):
        return False
    if any(fnmatch.fnmatchcase(full, pat.lower()) for pat in allow):
        return False
    if any(fnmatch.fnmatchcase(name, pat) for pat in BLOCKED_NAME_PATTERNS):
        return True
    return any(full == suf or full.endswith("/" + suf) for suf in BLOCKED_PATH_SUFFIXES)


def blocked_paths(paths: Iterable[str], allow: Iterable[str] = ()) -> list[str]:
    allow = list(allow)
    return [p for p in paths if is_blocked_path(p, allow)]


def load_allow(repo: Path) -> list[str]:
    """讀 .secret-guard-allow：每行一個路徑 glob，# 開頭是註解"""
    try:
        text = (repo / ALLOW_FILE).read_text(encoding="utf-8")
    except FileNotFoundError:
        return []
    except OSError as e:
        raise GuardError(f"讀不到 {ALLOW_FILE}：{e}") from e
    return [ln.strip() for ln in text.splitlines() if ln.strip() and not ln.strip().startswith("#")]


# ---------------- pre-push ----------------
def push_ranges(stdin_text: str) -> list[str]:
    """
    git pre-push 從 stdin 給「<local ref> <local sha> <remote ref> <remote sha>」，每行一個 ref。
    回傳要掃的 git log 範圍：已有的分支掃 remote..local；新分支掃遠端還沒有的 commit；刪分支不用掃。
    """
    ranges = []
    for line in stdin_text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if len(parts) != 4:   # 看不懂就擋（fail closed），不能當成沒東西要掃
            raise GuardError(f"pre-push 收到看不懂的資料：{line!r}")
        local_sha, remote_sha = parts[1], parts[3]
        for sha in (local_sha, remote_sha):
            if not _SHA.match(sha):
                raise GuardError(f"pre-push 收到的 commit 編號格式不對：{sha!r}")
        if _ZERO_SHA.match(local_sha):
            continue
        ranges.append(f"{local_sha} --not --remotes" if _ZERO_SHA.match(remote_sha) else f"{remote_sha}..{local_sha}")
    return ranges


# ---------------- gitleaks 安裝 ----------------
def gitleaks_asset(system: str, machine: str) -> str:
    """platform.system()、platform.machine() → Release 檔名"""
    os_name = {"Linux": "linux", "Darwin": "darwin", "Windows": "windows"}.get(system)
    arch = {"x86_64": "x64", "amd64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(machine.lower())
    if not os_name or not arch:
        raise GuardError(f"沒有 {system}/{machine} 的 gitleaks，請自行安裝 gitleaks {GITLEAKS_VERSION} 放到 {GITLEAKS_DIR}/")
    ext = "zip" if os_name == "windows" else "tar.gz"
    asset = f"gitleaks_{GITLEAKS_VERSION}_{os_name}_{arch}.{ext}"
    if asset not in GITLEAKS_SHA256:
        raise GuardError(f"沒有 {asset} 的 SHA256，不下載")
    return asset


def _binary_name(system: str) -> str:
    return "gitleaks.exe" if system == "Windows" else "gitleaks"


def _ssl_context() -> ssl.SSLContext:
    try:   # python.org 的 macOS Python 沒有根憑證，有 certifi 就用
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


def default_opener(req, timeout):
    return urllib.request.urlopen(req, timeout=timeout, context=_ssl_context())


def _download(url: str, opener: Opener) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "secret-guard"})
    try:
        with opener(req, timeout=DOWNLOAD_TIMEOUT_S) as r:
            data = r.read(MAX_DOWNLOAD_BYTES + 1)
    except (urllib.error.URLError, OSError, http.client.HTTPException) as e:
        raise GuardError(f"下載 gitleaks 失敗：{e}") from e
    if len(data) > MAX_DOWNLOAD_BYTES:
        raise GuardError("下載的 gitleaks 檔案太大，停止")
    return data


def _extract_binary(data: bytes, asset: str, name: str) -> bytes:
    try:
        if asset.endswith(".zip"):
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                return z.read(name)
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            member = t.getmember(name)
            f = t.extractfile(member) if member.isfile() else None
            if f is None:
                raise KeyError(name)
            return f.read()
    except KeyError:
        raise GuardError(f"{asset} 裡沒有 {name}") from None
    except (zipfile.BadZipFile, tarfile.TarError, OSError) as e:
        raise GuardError(f"{asset} 壞了：{e}") from e


def install_gitleaks(dest_dir: Path, opener: Opener = default_opener,
                     system: str | None = None, machine: str | None = None) -> Path:
    """下載固定版本 gitleaks、比對 SHA256，只取出執行檔放到 dest_dir；回傳執行檔路徑"""
    system = system or platform.system()
    machine = machine or platform.machine()
    asset = gitleaks_asset(system, machine)
    data = _download(DOWNLOAD_URL.format(ver=GITLEAKS_VERSION, asset=asset), opener)
    if hashlib.sha256(data).hexdigest() != GITLEAKS_SHA256[asset]:
        raise GuardError(f"{asset} 的 SHA256 不符，檔案可能損壞或被竄改，不安裝")
    name = _binary_name(system)
    binary = _extract_binary(data, asset, name)
    try:
        dest_dir.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix=".gitleaks-", dir=dest_dir)
    except OSError as e:
        raise GuardError(f"建立 {dest_dir} 失敗：{e}") from e
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(binary)
        os.chmod(tmp, 0o755)
        target = dest_dir / name
        os.replace(tmp, target)
    except OSError as e:
        Path(tmp).unlink(missing_ok=True)
        raise GuardError(f"寫入 {dest_dir} 失敗：{e}") from e
    return target


def find_gitleaks(repo_root: Path) -> Path:
    """只用 .tools/ 裡固定版本的 gitleaks（PATH 上的版本不一定一樣）"""
    for name in ("gitleaks", "gitleaks.exe"):
        p = repo_root / GITLEAKS_DIR / name
        if p.exists():
            return p
    raise GuardError(f"找不到 {GITLEAKS_DIR}/gitleaks：執行 bash tools/setup_hooks.sh 選「安裝」，"
                     "或 python3 tools/secret_guard.py install")


# ---------------- 掃描 ----------------
def _git(repo: Path, *args: str) -> str:
    try:
        res = subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True,
                             timeout=GIT_TIMEOUT_S, encoding="utf-8", errors="replace")
    except (OSError, subprocess.TimeoutExpired) as e:
        raise GuardError(f"git {' '.join(args)} 失敗：{e}") from e
    if res.returncode != 0:
        raise GuardError(f"git {' '.join(args)} 失敗：{res.stderr.strip()}")
    return res.stdout


def _split_nul(out: str) -> list[str]:
    return [n for n in out.split("\0") if n.strip()]


def _gitleaks_found(repo: Path, extra: list[str]) -> bool:
    """跑 gitleaks git；找到東西回 True。輸出直接給使用者看（已遮住金鑰）"""
    cmd = [str(find_gitleaks(repo)), "git", "--no-banner", "--redact", "--verbose",
           "--exit-code", str(LEAK_EXIT), "--timeout", str(GITLEAKS_TIMEOUT_S), *extra, str(repo)]
    try:
        res = subprocess.run(cmd, cwd=repo, timeout=GITLEAKS_TIMEOUT_S + 30)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise GuardError(f"執行 gitleaks 失敗：{e}") from e
    if res.returncode == LEAK_EXIT:
        return True
    if res.returncode != 0:
        raise GuardError(f"gitleaks 執行錯誤（結束代碼 {res.returncode}）")
    return False


def _report_files(bad: list[str], where: str) -> None:
    print(f"[secret-guard] {where}有敏感檔案（不准 commit／push）：", file=sys.stderr)
    for b in bad:
        print(f"  - {b}", file=sys.stderr)


# 這些函式回傳 True = 找到敏感資料
NAME_FILTER = "--diff-filter=ACMRT"   # 新增、複製、修改、改名、型別變更（刪除不用管）


def found_in_staged(repo: Path) -> bool:
    names = _split_nul(_git(repo, "diff", "--cached", "--name-only", "-z", NAME_FILTER))
    bad = blocked_paths(names, load_allow(repo))
    if bad:
        _report_files(bad, "這次要 commit 的檔案裡")
    leaked = _gitleaks_found(repo, ["--pre-commit", "--staged"])
    return bool(bad) or leaked


def found_in_push(repo: Path, stdin_text: str) -> bool:
    found = False
    allow = load_allow(repo)
    for rng in push_ranges(stdin_text):
        names = _split_nul(_git(repo, "log", "--format=", "--name-only", "-z", NAME_FILTER, *MERGE_OPTS, *rng.split()))
        bad = sorted(set(blocked_paths(names, allow)))
        if bad:
            _report_files(bad, "要 push 的 commit 裡")
        leaked = _gitleaks_found(repo, [f"--log-opts={' '.join(MERGE_OPTS)} {rng}"])
        found = found or bool(bad) or leaked
    return found


def found_in_history(repo: Path) -> bool:
    """整個歷史：曾經出現過的敏感檔名（含改名來的）＋ gitleaks 掃全部 commit"""
    names = _split_nul(_git(repo, "log", "--all", "--format=", "--name-only", "-z", NAME_FILTER, *MERGE_OPTS))
    names += _split_nul(_git(repo, "ls-files", "-z"))
    bad = sorted(set(blocked_paths(names, load_allow(repo))))
    if bad:
        _report_files(bad, "git 歷史")
    leaked = _gitleaks_found(repo, [f"--log-opts=--all {' '.join(MERGE_OPTS)}"])
    return bool(bad) or leaked


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="擋敏感資料上傳（敏感檔名＋gitleaks）")
    ap.add_argument("command", choices=["install", "pre-commit", "pre-push", "scan"])
    ap.add_argument("hook_args", nargs="*", help="pre-push 時 git 給的 remote 名稱和網址（不使用）")
    args = ap.parse_args(argv)
    try:
        repo = Path(_git(Path.cwd(), "rev-parse", "--show-toplevel").strip())
        if args.command == "install":
            path = install_gitleaks(repo / GITLEAKS_DIR)
            print(f"[secret-guard] 已安裝 gitleaks {GITLEAKS_VERSION}：{path}")
            return 0
        if args.command == "pre-commit":
            found = found_in_staged(repo)
        elif args.command == "pre-push":
            # git 給的是 UTF-8；Windows 中文系統的預設編碼（cp950）會解錯
            found = found_in_push(repo, sys.stdin.buffer.read().decode("utf-8", errors="replace"))
        else:
            found = found_in_history(repo)
    except (GuardError, OSError) as e:
        print(f"[secret-guard] 錯誤：{e}\n[secret-guard] 沒辦法確認有沒有敏感資料，先擋下。", file=sys.stderr)
        return 2
    if found:
        print("[secret-guard] 擋下了：移除上面的敏感資料再試一次（金鑰已外流的話要去服務那邊撤銷重發）。\n"
              "[secret-guard] 誤判：內容在該行加註解 gitleaks:allow 或把 Fingerprint 加進 .gitleaksignore；"
              f"檔名把路徑 glob 加進 {ALLOW_FILE}。", file=sys.stderr)
        return 1
    print("[secret-guard] 沒有發現敏感資料")
    return 0


if __name__ == "__main__":
    sys.exit(main())
