#!/usr/bin/env python3
"""
智慧水杯校正工具（韌體 0.5.0 起）：用 USB 連水杯，量空杯、量滿杯、把校正值寫進水杯的 flash，也可以燒錄韌體。

    python3 -m venv .venv
    .venv/bin/pip install -r tools/requirements-cup-tool.txt
    .venv/bin/python tools/cup_tool.py          # 加 --no-update-check 不檢查工具更新

需要有 Tkinter 的 Python：macOS 用 python.org 的安裝檔（Homebrew 的要另外 brew install python-tk@3.13）、
Linux 要 sudo apt install python3-tk、Windows 的官方安裝檔內建。

和水杯之間的指令見 main/tool_cmd.h（PING、CAL?、CAL SET、CAL RESET、MEASURE、LIVE）。
同一個序列埠一次只能給一個程式用：VS Code 的 monitor、idf.py monitor 要先關掉。
"""

from __future__ import annotations

import contextlib
import functools
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
import urllib.error
import urllib.request
import webbrowser
import zipfile
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import serial
import serial.tools.list_ports

import cup_updater as updater

GITHUB_REPO = updater.GITHUB_REPO
ESPRESSIF_VID = 0x303A
MEASURE_MS = 5000
KEEPALIVE_S = 20          # 水杯在最後一個指令 60 秒後恢復休眠，連著時每 20 秒 PING 一次
READY_TIMEOUT_S = 15      # 打開序列埠可能讓板子重開機，開機約 5 秒後才收指令
MIN_SPAN = 20             # 和韌體的 LEVEL_MIN_SPAN 一致
WARN_SPAN = 60            # 空杯、滿杯差距小於這個值，大概量錯了
WARN_RL_DELTA = 25        # 杯底 RL 空杯→滿杯變化太小：杯子大概沒壓到保護套底部（實測壓到底約 50、沒壓到底約 10）
WARN_NOISE = 6            # 量測期間電極0 最大－最小超過這個值：杯子在動或手太近
WARN_STILL = 0.9          # 量測期間「直立靜止」的比例低於這個值：杯子沒放穩
POSES = {"upright": "直立", "tilted": "傾斜", "drinking": "飲用中", "lying": "倒下", "unknown": "未知"}
REPO_ROOT = Path(__file__).resolve().parent.parent
FROZEN = bool(getattr(sys, "frozen", False))   # PyInstaller 打包後才會自我更新
TOOL_VERSION = updater.current_version(Path(getattr(sys, "_MEIPASS", ".")) if FROZEN else None, REPO_ROOT)
UPDATE_DELAY_MS = 2000    # 開視窗後多久自動檢查工具更新
UPDATE_RETRY_MS = 5000    # 工具忙（燒錄、量測）或連線中時延後多久再問
CLEANUP_DELAY_MS = 5000   # 更新後啟動多久再刪舊版的 .old


def parse_kv(line: str) -> dict[str, str]:
    """「@MEAS n=50 e0=480.2 …」→ {"n": "50", "e0": "480.2", …}"""
    return dict(tok.split("=", 1) for tok in line.split()[1:] if "=" in tok)


def ssl_context():
    """python.org 的 macOS Python 預設沒有根憑證，有 certifi 就用它"""
    import ssl
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


def list_ports() -> list[tuple[str, str]]:
    """回傳 [(裝置, 說明)]，Espressif 的板子排前面；macOS 只列 /dev/cu.*（tty.* 打開時可能卡住）"""
    items = []
    for p in serial.tools.list_ports.comports():
        if sys.platform == "darwin" and p.device.startswith("/dev/tty."):
            continue
        is_esp = p.vid == ESPRESSIF_VID
        desc = ("★ 水杯（ESP32） " if is_esp else "") + (p.description or "")
        items.append((not is_esp, p.device, desc))
    items.sort()
    return [(dev, desc) for _, dev, desc in items]


class CupError(Exception):
    pass


# ============================================================================
#  序列埠連線
# ============================================================================
class CupLink:
    """背景執行緒一直讀序列埠：@LIVE 交給 on_live，其他 @ 回覆給 request() 等待，一般文字交給 on_text。"""

    def __init__(self, port: str, on_live, on_text):
        self.port = port
        self.on_live = on_live
        self.on_text = on_text
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.1
        self.ser.dtr = False   # 不拉 DTR／RTS，盡量不讓板子重開機（USB Serial/JTAG 還是可能重開）
        self.ser.rts = False
        self.ser.open()
        self.replies: queue.Queue[str] = queue.Queue()
        self.req_lock = threading.Lock()
        self.alive = True
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _read_loop(self):
        buf = b""
        while self.alive:
            try:
                chunk = self.ser.read(4096)
            except (serial.SerialException, OSError) as e:
                if self.alive:
                    self.on_text(f"[工具] 序列埠讀取失敗：{e}")
                    self.alive = False
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue
                at = line.find("@")
                if at >= 0 and line[at + 1:at + 2].isupper():
                    msg = line[at:]
                    if msg.startswith("@LIVE"):
                        self.on_live(parse_kv(msg))
                    else:
                        self.replies.put(msg)
                    continue
                self.on_text(line)

    def send(self, cmd: str):
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()

    def request(self, cmd: str, tag: str, timeout: float = 5.0) -> dict[str, str]:
        """送一行指令，等回覆的 tag（例如 @CAL）；收到 @ERR 或逾時丟 CupError"""
        with self.req_lock:
            while not self.replies.empty():
                self.replies.get_nowait()
            self.send(cmd)
            end = time.time() + timeout
            while time.time() < end:
                try:
                    msg = self.replies.get(timeout=0.1)
                except queue.Empty:
                    if not self.alive:
                        raise CupError("序列埠已經斷開")
                    continue
                if msg.startswith("@ERR"):
                    raise CupError(msg[4:].strip())
                if msg.startswith(tag):
                    return parse_kv(msg)
            raise CupError(f"水杯沒有回應「{cmd}」（等了 {timeout:.0f} 秒）")

    def wait_ready(self, timeout: float = READY_TIMEOUT_S) -> dict[str, str]:
        """打開序列埠可能讓板子重開機：每秒送一次 PING，直到收到 @PONG"""
        end = time.time() + timeout
        while time.time() < end:
            try:
                return self.request("PING", "@PONG", timeout=1.0)
            except CupError:
                if not self.alive:
                    raise
        raise CupError("等不到水杯回應。韌體是 0.5.0 以上嗎？VS Code 的 monitor 有關掉嗎？")

    def close(self):
        self.alive = False
        with contextlib.suppress(Exception):
            self.send("LIVE 0")
        with contextlib.suppress(Exception):
            self.ser.close()


# ============================================================================
#  韌體檔案
# ============================================================================
def read_flash_args(folder: Path) -> list[str]:
    """把 flash_args（--flash-mode … 和「位址 檔案」）轉成 esptool 參數，檔案改成絕對路徑"""
    text = (folder / "flash_args").read_text()
    args: list[str] = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0].startswith("0x"):
            path = folder / parts[1]
            if not path.exists():
                raise CupError(f"找不到 {path}")
            args += [parts[0], str(path)]
        else:
            args += parts
    return args


# 燒錄說明檔：Release 的檔名不能有中文（GitHub 會改成 default.txt），內容還是中文。舊的 Actions zip 是 燒錄說明.txt
FLASH_NOTE = "FLASHING.txt"
_OLD_FLASH_NOTE = "燒錄說明.txt"
RELEASE_FIRMWARE_FILES = frozenset({"flash_args", "bootloader.bin", "partition-table.bin", "smart_cup_test.bin", FLASH_NOTE})


def firmware_version(folder: Path) -> str:
    for name in (FLASH_NOTE, _OLD_FLASH_NOTE):
        note = folder / name
        if note.exists():
            first = note.read_text(encoding="utf-8").splitlines()[0]
            return first.split("（")[0].replace("韌體", "").strip()
    return "本機建置"


def download_latest_release(dest: Path, log) -> Path:
    """從 GitHub Release（改版號推上 main 時 Actions 自動建立）下載最新版的燒錄檔"""
    ctx = ssl_context()
    api = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
    req = urllib.request.Request(api, headers={"Accept": "application/vnd.github+json", "User-Agent": "cup-tool"})
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=20) as r:
            rel = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise CupError("GitHub 上還沒有 Release（改 main/ble_gatt_svr.h 的版號推上 main，Actions 才會建立）")
        raise CupError(f"讀取 GitHub Release 失敗：{e}")
    except OSError as e:
        raise CupError(f"連不上 GitHub：{e}")
    want = RELEASE_FIRMWARE_FILES
    assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
    missing = want - set(assets) - {FLASH_NOTE}
    if missing:
        raise CupError(f"Release {rel.get('tag_name')} 缺少檔案：{', '.join(sorted(missing))}")
    log(f"[韌體] 下載 GitHub Release {rel.get('tag_name')} …")
    for name in want & set(assets):
        req = urllib.request.Request(assets[name], headers={"User-Agent": "cup-tool"})
        with urllib.request.urlopen(req, context=ctx, timeout=60) as r:
            (dest / name).write_bytes(r.read())
    return dest


def prepare_folder(path: Path, tmp: Path) -> Path:
    """使用者選的 zip（Actions 的 Artifacts）或資料夾 → 有 flash_args 的資料夾"""
    if path.is_file() and path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as z:
            z.extractall(tmp)
        path = tmp
    if (path / "flash_args").exists():
        return path
    for sub in path.rglob("flash_args"):
        return sub.parent
    raise CupError(f"{path} 裡面找不到 flash_args")


class _QueueWriter:
    """把 esptool 印的東西逐行丟給 log"""

    def __init__(self, log):
        self.log = log
        self.buf = ""

    def write(self, s):
        self.buf += s.replace("\r", "\n")
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            if line.strip():
                self.log(line)
        return len(s)

    def flush(self):
        pass


def run_esptool(port: str, folder: Path, log):
    import esptool
    argv = ["--chip", "esp32c6", "-p", port, "-b", "460800",
            "--before", "default-reset", "--after", "hard-reset", "write-flash", *read_flash_args(folder)]
    log("[韌體] esptool " + " ".join(argv))
    w = _QueueWriter(log)
    with contextlib.redirect_stdout(w), contextlib.redirect_stderr(w):
        try:
            esptool.main(argv)
        except SystemExit as e:
            if e.code not in (0, None):
                raise CupError(f"esptool 結束代碼 {e.code}")
        except Exception as e:   # esptool 的 FatalError 等
            raise CupError(f"燒錄失敗：{e}")


# ============================================================================
#  視窗
# ============================================================================
class App:
    def __init__(self, root: tk.Tk, auto_update_check: bool = True):
        self.root = root
        root.title(f"智慧水杯 校正工具 {TOOL_VERSION}")
        self.opener = functools.partial(urllib.request.urlopen, context=ssl_context())
        root.minsize(760, 640)
        self.link: CupLink | None = None
        self.ui_q: queue.Queue = queue.Queue()
        self.busy = False
        self.closing = False          # 視窗關掉後，佇列裡剩下的介面更新都不做
        self.checking_update = False  # 同時只查一次，避免跳兩個更新對話框
        self.empty: dict | None = None
        self.full: dict | None = None
        self.device_cal: tuple[int, int] | None = None
        self.default_cal: tuple[int, int] | None = None
        self.last_ping = 0.0
        self._build()
        self.refresh_ports()
        root.after(50, self._pump)
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        if FROZEN:   # Windows 舊版的行程剛結束時 .old 可能還鎖著，等一下再刪
            root.after(CLEANUP_DELAY_MS, self._cleanup_old_version)
        if auto_update_check:
            root.after(UPDATE_DELAY_MS, lambda: self.check_update(auto=True))

    # ---------------- 介面 ----------------
    def _build(self):
        pad = {"padx": 8, "pady": 4}
        top = ttk.LabelFrame(self.root, text="1. 連線（USB）")
        top.pack(fill="x", **pad)
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(top, textvariable=self.port_var, width=48, state="readonly")
        self.port_box.grid(row=0, column=0, padx=6, pady=6)
        ttk.Button(top, text="重新整理", command=self.refresh_ports).grid(row=0, column=1, padx=4)
        self.conn_btn = ttk.Button(top, text="連線", command=self.toggle_connect)
        self.conn_btn.grid(row=0, column=2, padx=4)
        self.update_btn = ttk.Button(top, text="檢查工具更新", command=lambda: self.check_update(auto=False))
        self.update_btn.grid(row=0, column=3, padx=4)
        self.status_var = tk.StringVar(value="還沒連線。VS Code 的 monitor 要先關掉。")
        ttk.Label(top, textvariable=self.status_var).grid(row=1, column=0, columnspan=3, sticky="w", padx=6, pady=(0, 6))

        live = ttk.LabelFrame(self.root, text="即時讀值")
        live.pack(fill="x", **pad)
        self.pct_var = tk.StringVar(value="--")
        self.live_var = tk.StringVar(value="")
        ttk.Label(live, textvariable=self.pct_var, font=("TkDefaultFont", 32, "bold"), width=7, anchor="e").grid(
            row=0, column=0, rowspan=2, padx=10)
        ttk.Label(live, textvariable=self.live_var).grid(row=0, column=1, sticky="w")
        ttk.Label(live, text="水位用水杯目前的校正值換算；校正時看電極0 的數字").grid(row=1, column=1, sticky="w")

        cal = ttk.LabelFrame(self.root, text="2. 校正")
        cal.pack(fill="x", **pad)
        tips = ("杯子擦乾、壓到保護套底部、直立放在桌上，手離開再按。\n"
                "滿杯 = 水面和主條銅箔的頂端切齊（倒更滿讀值會偏）。")
        ttk.Label(cal, text=tips, foreground="#555").grid(row=0, column=0, columnspan=4, sticky="w", padx=6, pady=4)
        self.empty_btn = ttk.Button(cal, text="量空杯（5 秒）", command=lambda: self.measure("empty"))
        self.empty_btn.grid(row=1, column=0, padx=6, pady=4, sticky="w")
        self.empty_var = tk.StringVar(value="—")
        ttk.Label(cal, textvariable=self.empty_var).grid(row=1, column=1, columnspan=3, sticky="w")
        self.full_btn = ttk.Button(cal, text="量滿杯（5 秒）", command=lambda: self.measure("full"))
        self.full_btn.grid(row=2, column=0, padx=6, pady=4, sticky="w")
        self.full_var = tk.StringVar(value="—")
        ttk.Label(cal, textvariable=self.full_var).grid(row=2, column=1, columnspan=3, sticky="w")

        row3 = ttk.Frame(cal)
        row3.grid(row=3, column=0, columnspan=4, sticky="w", padx=6, pady=6)
        ttk.Label(row3, text="要寫入的校正值：空杯").pack(side="left")
        self.new_empty = tk.StringVar()
        ttk.Entry(row3, textvariable=self.new_empty, width=6).pack(side="left", padx=4)
        ttk.Label(row3, text="滿杯").pack(side="left")
        self.new_full = tk.StringVar()
        ttk.Entry(row3, textvariable=self.new_full, width=6).pack(side="left", padx=4)
        for v in (self.new_empty, self.new_full):   # 手動改數字時也重新檢查
            v.trace_add("write", lambda *_: self._check())
        self.write_btn = ttk.Button(row3, text="寫入水杯", command=self.write_cal)
        self.write_btn.pack(side="left", padx=8)
        self.reset_btn = ttk.Button(row3, text="恢復程式預設值", command=self.reset_cal)
        self.reset_btn.pack(side="left", padx=4)
        self.check_var = tk.StringVar(value="")
        ttk.Label(cal, textvariable=self.check_var, foreground="#b35c00", wraplength=700, justify="left").grid(
            row=4, column=0, columnspan=4, sticky="w", padx=6, pady=(0, 6))

        fw = ttk.LabelFrame(self.root, text="3. 燒錄韌體（會保留 flash 裡的校正值和未確認的喝水事件）")
        fw.pack(fill="x", **pad)
        self.fw_btn_release = ttk.Button(fw, text="下載 GitHub 最新版並燒錄", command=self.flash_release)
        self.fw_btn_release.grid(row=0, column=0, padx=6, pady=6)
        self.fw_btn_file = ttk.Button(fw, text="選擇檔案燒錄（Actions 的 zip 或資料夾）…", command=self.flash_file)
        self.fw_btn_file.grid(row=0, column=1, padx=6)
        self.fw_btn_build = ttk.Button(fw, text="燒錄本機 build/", command=self.flash_build)
        self.fw_btn_build.grid(row=0, column=2, padx=6)

        logf = ttk.LabelFrame(self.root, text="紀錄")
        logf.pack(fill="both", expand=True, **pad)
        self.log_text = tk.Text(logf, height=12, wrap="word", state="disabled")
        sb = ttk.Scrollbar(logf, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=sb.set)
        self.log_text.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self._update_buttons()

    def _update_buttons(self):
        connected = self.link is not None and not self.busy
        for b in (self.empty_btn, self.full_btn, self.write_btn, self.reset_btn):
            b.state(["!disabled"] if connected else ["disabled"])
        for b in (self.fw_btn_release, self.fw_btn_file, self.fw_btn_build):
            b.state(["!disabled"] if not self.busy and self.port_var.get() else ["disabled"])
        self.conn_btn.state(["disabled"] if self.busy else ["!disabled"])
        self.update_btn.state(["disabled"] if self.busy else ["!disabled"])
        self.conn_btn.configure(text="中斷連線" if self.link else "連線")

    # ---------------- 執行緒 → 介面 ----------------
    def ui(self, fn, *args):
        self.ui_q.put((fn, args))

    def log(self, text: str):
        self.ui(self._append_log, text)

    def _append_log(self, text: str):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", time.strftime("%H:%M:%S ") + text + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _pump(self):
        try:
            while not self.closing:
                fn, args = self.ui_q.get_nowait()
                fn(*args)
        except queue.Empty:
            pass
        if self.closing:
            return
        # 連著時定期 PING，水杯才不會回去休眠
        if self.link and not self.busy and time.time() - self.last_ping > KEEPALIVE_S:
            self.last_ping = time.time()
            self.background(self._keepalive, quiet=True)
        if self.link and not self.link.alive:
            self._disconnected("序列埠斷開了（拔線或板子重開？）")
        self.root.after(50, self._pump)

    def background(self, fn, *args, quiet=False):
        """在背景執行緒跑 fn；執行期間按鈕停用，CupError 跳對話框"""
        if self.busy:
            return
        self.busy = not quiet
        self._update_buttons()

        def run():
            try:
                fn(*args)
            except CupError as e:
                if not quiet:
                    self.ui(messagebox.showerror, "智慧水杯", str(e))
                self.log(f"[錯誤] {e}")
            except Exception as e:   # 不預期的錯誤也要讓使用者看到
                self.ui(messagebox.showerror, "智慧水杯", f"發生錯誤：{e!r}")
                self.log(f"[錯誤] {e!r}")
            finally:
                if not quiet:
                    self.ui(self._done)

        threading.Thread(target=run, daemon=True).start()

    def _done(self):
        self.busy = False
        if not self.closing:
            self._update_buttons()

    # ---------------- 連線 ----------------
    def refresh_ports(self):
        ports = list_ports()
        self.port_map = {f"{dev}  {desc}": dev for dev, desc in ports}
        self.port_box["values"] = list(self.port_map)
        if ports and not self.port_var.get():
            self.port_box.current(0)
        self._update_buttons()

    def selected_port(self) -> str:
        dev = self.port_map.get(self.port_var.get())
        if not dev:
            raise CupError("請先選擇序列埠（水杯用 USB 線接上電腦後按「重新整理」）")
        return dev

    def toggle_connect(self):
        if self.link:
            self._disconnected("已中斷連線")
            return
        try:
            port = self.selected_port()   # Tk 的變數只能在主執行緒讀
        except CupError as e:
            messagebox.showerror("智慧水杯", str(e))
            return
        self.background(self._connect, port)

    def _connect(self, port: str):
        self.log(f"[工具] 打開 {port} …（板子可能會重開機，最多等 {READY_TIMEOUT_S} 秒）")
        try:
            link = CupLink(port, on_live=lambda d: self.ui(self._on_live, d), on_text=self._on_text)
        except serial.SerialException as e:
            raise CupError(f"打不開 {port}：{e}\n\n其他程式（VS Code 的 monitor、idf.py monitor）用著這個序列埠嗎？")
        try:
            pong = link.wait_ready()
            for _ in range(4):   # 剛開機時藍牙名稱還沒產生（name=-），多等一下
                if pong.get("name", "-") != "-":
                    break
                time.sleep(1)
                pong = link.request("PING", "@PONG")
            link.request("LIVE 1", "@OK")
        except CupError:
            link.close()
            raise
        self.link = link
        self.last_ping = time.time()
        self.ui(self._on_connected, pong)

    def _on_connected(self, pong: dict):
        self.device_cal = (int(pong.get("empty", 0)), int(pong.get("full", 0)))
        src = "flash 裡的值" if pong.get("src") == "nvs" else "程式預設值"
        self.status_var.set(f"已連線：韌體 {pong.get('fw')} · {pong.get('name', '-')} · "
                            f"目前校正值 空杯 {self.device_cal[0]}、滿杯 {self.device_cal[1]}（{src}）")
        self.log(f"[工具] 已連線：{pong}")
        self._update_buttons()

    def _keepalive(self):
        link = self.link
        if link:
            with contextlib.suppress(CupError):
                link.request("PING", "@PONG", timeout=3)

    def _disconnected(self, why: str):
        if self.link:
            self.link.close()
        self.link = None
        self.status_var.set(why)
        self.pct_var.set("--")
        self.live_var.set("")
        self.log(f"[工具] {why}")
        self._update_buttons()

    def _on_text(self, line: str):
        # 韌體的一般輸出：只記重要的（校正、事件、休眠、錯誤、開機）
        keys = ("[校正]", "[事件]", "[休眠]", "[BLE]", "[工具]", "smart_cup", "Guru", "abort", "panic")
        if any(k in line for k in keys):
            self.log(line)

    def _on_live(self, d: dict):
        if not self.link:
            return
        self.pct_var.set(f"{float(d.get('pct', 0)):.0f}%")
        self.live_var.set(f"電極0 {d.get('e0')}　電極1（杯底 RL）{d.get('e1')}　"
                          f"姿態 {POSES.get(d.get('pose', 'unknown'), '?')}{'（移動中）' if d.get('moving') == '1' else ''}")

    # ---------------- 校正 ----------------
    def measure(self, which: str):
        self.background(self._measure, which)

    def _measure(self, which: str):
        name = "空杯" if which == "empty" else "滿杯"
        self.log(f"[校正] 量{name} {MEASURE_MS / 1000:.0f} 秒，杯子不要動 …")
        d = self.link.request(f"MEASURE {MEASURE_MS}", "@MEAS", timeout=MEASURE_MS / 1000 + 8)
        m = {k: float(v) for k, v in d.items()}
        self.log(f"[校正] {name}：電極0 平均 {m['e0']:.1f}（{m['e0min']:.0f}～{m['e0max']:.0f}）、"
                 f"電極1 平均 {m['e1']:.1f}、直立靜止 {m['still']:.0f}/{m['n']:.0f} 筆")
        self.ui(self._on_measured, which, m)

    def _on_measured(self, which: str, m: dict):
        text = (f"電極0 {m['e0']:.1f}（{m['e0min']:.0f}～{m['e0max']:.0f}）　電極1 {m['e1']:.1f}　"
                f"目前換算 {m['pct']:.0f}%")
        warn = []
        if m["e0max"] - m["e0min"] > WARN_NOISE:
            warn.append("讀值跳動大（杯子在動或手太近？）")
        if m["n"] and m["still"] / m["n"] < WARN_STILL:
            warn.append("杯子沒有一直直立靜止")
        if warn:
            text += "　⚠ " + "、".join(warn)
        if which == "empty":
            self.empty = m
            self.empty_var.set(text)
            self.new_empty.set(str(round(m["e0"])))
        else:
            self.full = m
            self.full_var.set(text)
            self.new_full.set(str(round(m["e0"])))
        self._check()

    def _check(self) -> list[str]:
        """檢查要寫入的值，回傳警告（也顯示在畫面上）"""
        warn = []
        try:
            e, f = int(self.new_empty.get()), int(self.new_full.get())
        except ValueError:
            self.check_var.set("")
            return warn
        if f >= e or e - f < MIN_SPAN:
            warn.append(f"滿杯要比空杯小至少 {MIN_SPAN}（水越多讀值越小），這組值水杯不會接受")
        elif e - f < WARN_SPAN:
            warn.append(f"空杯和滿杯只差 {e - f}，比平常（約 120～140）小很多，空杯或滿杯可能量錯了")
        if self.empty and self.full:
            rl = self.empty["e1"] - self.full["e1"]
            if rl < WARN_RL_DELTA:
                warn.append(f"杯底 RL 從空杯到滿杯只變了 {rl:.0f}（正常約 40～50）："
                            "杯子可能沒壓到保護套底部，壓到底後重量滿杯")
        self.check_var.set("⚠ " + "\n⚠ ".join(warn) if warn else f"✓ 空杯 {e}、滿杯 {f}，差距 {e - f}")
        return warn

    def write_cal(self):
        try:
            e, f = int(self.new_empty.get()), int(self.new_full.get())
        except ValueError:
            messagebox.showerror("智慧水杯", "空杯、滿杯要填整數（先按「量空杯」「量滿杯」會自動填）")
            return
        warn = self._check()
        msg = f"把校正值寫進水杯？\n\n空杯 {e}、滿杯 {f}"
        if self.device_cal:
            msg += f"\n（目前是空杯 {self.device_cal[0]}、滿杯 {self.device_cal[1]}）"
        if warn:
            msg += "\n\n注意：\n" + "\n".join(warn)
        if not messagebox.askyesno("智慧水杯", msg, icon="warning" if warn else "question"):
            return
        self.background(self._write_cal, e, f)

    def _write_cal(self, e: int, f: int):
        d = self.link.request(f"CAL SET {e} {f}", "@CAL", timeout=6)
        self.log(f"[校正] 已寫入水杯並生效：空杯 {d['empty']}、滿杯 {d['full']}（存在 flash，重開機、燒錄韌體都會保留）")
        self.ui(self._cal_changed, d)

    def reset_cal(self):
        if not messagebox.askyesno("智慧水杯", "清掉水杯 flash 裡的校正值，回到韌體程式裡的預設值？"):
            return
        self.background(self._reset_cal)

    def _reset_cal(self):
        d = self.link.request("CAL RESET", "@CAL", timeout=6)
        self.log(f"[校正] 已恢復程式預設值：空杯 {d['empty']}、滿杯 {d['full']}")
        self.ui(self._cal_changed, d)

    def _cal_changed(self, d: dict):
        self.device_cal = (int(d["empty"]), int(d["full"]))
        src = "flash 裡的值" if d.get("src") == "nvs" else "程式預設值"
        cur = self.status_var.get().split(" · 目前校正值")[0]
        self.status_var.set(f"{cur} · 目前校正值 空杯 {d['empty']}、滿杯 {d['full']}（{src}）")
        messagebox.showinfo("智慧水杯", f"完成。水杯現在的校正值：空杯 {d['empty']}、滿杯 {d['full']}。\n"
                                       "看上面的即時水位驗證（空杯約 0%、滿杯約 100%）。")

    # ---------------- 燒錄 ----------------
    def flash_release(self):
        self._start_flash(lambda tmp: download_latest_release(tmp, self.log))

    def flash_file(self):
        path = filedialog.askopenfilename(title="選擇 Actions 下載的 zip（取消可以改選資料夾）",
                                          filetypes=[("zip", "*.zip"), ("全部", "*")])
        if not path:
            path = filedialog.askdirectory(title="選擇有 flash_args 的資料夾")
        if not path:
            return
        self._start_flash(lambda tmp: prepare_folder(Path(path), tmp))

    def flash_build(self):
        build = REPO_ROOT / "build"
        if not (build / "flash_args").exists():
            messagebox.showerror("智慧水杯", f"{build} 沒有建置結果，先執行 idf.py build")
            return
        self._start_flash(lambda tmp: build)

    def _start_flash(self, get_folder):
        try:
            port = self.selected_port()
        except CupError as e:
            messagebox.showerror("智慧水杯", str(e))
            return
        if not messagebox.askyesno("智慧水杯", f"燒錄韌體到 {port}？\n\n"
                                              "會保留 flash 裡的校正值和還沒送給 App 的喝水事件。\n燒錄約需 30 秒，期間不要拔線。"):
            return
        was_connected = self.link is not None
        if self.link:
            self._disconnected("燒錄前先中斷連線")
        self.background(self._flash, port, get_folder, was_connected)

    def _flash(self, port: str, get_folder, reconnect: bool):
        with tempfile.TemporaryDirectory() as tmp:
            folder = get_folder(Path(tmp))
            self.log(f"[韌體] 燒錄 {firmware_version(folder)}（{folder}）")
            run_esptool(port, folder, self.log)
        self.log("[韌體] 燒錄完成，板子重開機中")
        self.ui(messagebox.showinfo, "智慧水杯", "燒錄完成。板子會自動重開機，約 10 秒後可以重新連線。")
        if reconnect:
            time.sleep(3)
            self.ui(self._reconnect_after_flash)

    def _reconnect_after_flash(self):
        self.refresh_ports()
        self.toggle_connect()

    # ---------------- 工具自我更新（cup_updater.py） ----------------
    def _cleanup_old_version(self):
        """上次更新留下的 .old（Windows 執行中的 exe 只能改名，要等下次啟動才刪得掉）"""
        try:
            updater.cleanup_old(updater.install_target(Path(sys.executable), sys.platform))
        except (OSError, updater.UpdateError) as e:
            self.log(f"[更新] 清理舊版失敗（不影響使用）：{e}")

    def check_update(self, auto: bool):
        if self.checking_update:
            return
        if self.busy:
            if auto:
                self.root.after(UPDATE_RETRY_MS, lambda: self.check_update(auto=True))
            return
        self.checking_update = True
        threading.Thread(target=self._check_update_thread, args=(auto,), daemon=True).start()

    def _check_update_thread(self, auto: bool):
        """背景執行緒：任何例外都記下來（打包成 --windowed 後沒有 stderr，不接住就無聲無息）"""
        try:
            self._check_update(auto)
        except Exception as e:   # 不預期的錯誤也要讓使用者看到
            self.log(f"[更新] 檢查工具更新發生錯誤：{e!r}")
            if not auto:
                self.ui(messagebox.showerror, "智慧水杯", f"檢查工具更新發生錯誤：{e!r}")
        finally:
            self.ui(self._check_finished)

    def _check_finished(self):
        self.checking_update = False

    def _check_update(self, auto: bool):
        if TOOL_VERSION == updater.DEV:
            self.log("[更新] 版號是 dev（讀不到 VERSION 或 main/ble_gatt_svr.h），不檢查更新")
            return
        try:
            u = updater.latest_update(TOOL_VERSION, sys.platform, self.opener)
        except updater.UpdateError as e:
            self.log(f"[更新] 檢查工具更新失敗：{e}")
            if not auto:
                self.ui(messagebox.showerror, "智慧水杯", f"檢查工具更新失敗：{e}")
            return
        if u is None:
            self.log(f"[更新] 工具 {TOOL_VERSION} 已經是最新版")
            if not auto:
                self.ui(messagebox.showinfo, "智慧水杯", f"工具 {TOOL_VERSION} 已經是最新版。")
            return
        self.log(f"[更新] 發現新版 {u.version}（目前 {TOOL_VERSION}）：{u.page_url}")
        if not FROZEN and auto:   # 從原始碼執行：自動檢查只記在紀錄，不跳視窗
            self.log("[更新] 從原始碼執行不會自動更新，git pull 即可")
            return
        self.ui(self._offer_update, u, auto)

    def _offer_update(self, u: updater.Update, auto: bool):
        # 燒錄、量測中一律延後；自動檢查連線中也延後（不打斷校正，沒寫入的量測值會不見）
        if self.busy or (auto and self.link):
            self.root.after(UPDATE_RETRY_MS, lambda: self._offer_update(u, auto))
            return
        if not FROZEN:
            if messagebox.askyesno("智慧水杯", f"有新版工具 {u.version}（目前 {TOOL_VERSION}）。\n\n"
                                              "從原始碼執行不會自動更新（git pull 即可）。要打開 Release 網頁嗎？"):
                webbrowser.open(u.page_url)
            return
        msg = (f"有新版工具 {u.version}（目前 {TOOL_VERSION}），要現在更新嗎？\n\n"
               "會下載新版、比對檔案雜湊，換好後自動重新啟動。")
        if self.link:
            msg += "\n\n注意：會中斷和水杯的連線，還沒按「寫入水杯」的量測值會消失。"
        if not messagebox.askyesno("智慧水杯", msg):
            return
        if self.link:
            self._disconnected("更新工具前先中斷連線")
        self.background(self._apply_update, u)

    def _apply_update(self, u: updater.Update):
        try:
            target = updater.install_target(Path(sys.executable), sys.platform)
            updater.apply_update(u, sys.platform, target, self.opener, log=self.log)
        except updater.UpdateError as e:
            raise CupError(f"{e}\n\n也可以到 {u.page_url} 手動下載。") from e
        self.log(f"[更新] 已換成 {u.version}，重新啟動")
        self.ui(self._restart, target)

    def _restart(self, target: Path):
        try:
            subprocess.Popen(updater.restart_command(sys.platform, target), env=updater.restart_env(os.environ))
        except OSError as e:
            self.log(f"[更新] 重新啟動失敗：{e}")
            messagebox.showerror("智慧水杯", f"已經更新，但重新啟動失敗（{e}），請手動打開 {target}")
            return
        self.on_close()

    def on_close(self):
        self.closing = True
        if self.link:
            self.link.close()
        self.root.destroy()


def self_test() -> int:
    """打包後（PyInstaller）檢查：esptool 的燒錄 stub 有包進去、介面建得起來。CI 也會跑"""
    import esptool
    stub = Path(esptool.__file__).parent / "targets" / "stub_flasher" / "1" / "esp32c6.json"
    if not stub.exists():
        print(f"缺少 {stub}（PyInstaller 要加 --collect-data esptool）")
        return 1
    if FROZEN and TOOL_VERSION == updater.DEV:
        print("打包後讀不到 VERSION（PyInstaller 要加 --add-data VERSION:.），自動更新會失效")
        return 1
    updater.asset_name(sys.platform)   # 這個平台要有對應的 Release 檔名
    root = tk.Tk()
    root.withdraw()
    App(root, auto_update_check=False)
    root.update()
    root.destroy()
    print(f"self-test OK：工具 {TOOL_VERSION}、esptool {esptool.__version__}、序列埠 {len(list_ports())} 個")
    return 0


def main():
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    root = tk.Tk()
    if sys.platform.startswith("linux"):   # Linux 預設主題太舊，換 clam
        with contextlib.suppress(tk.TclError):
            ttk.Style().theme_use("clam")
    App(root, auto_update_check="--no-update-check" not in sys.argv)
    root.mainloop()


if __name__ == "__main__":
    main()
