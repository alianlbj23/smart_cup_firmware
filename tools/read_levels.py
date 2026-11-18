#!/usr/bin/env python3
"""
校正用：從序列埠錄一段即時顯示，算出電極0、電極1、水位% 的平均與範圍。

用法（先關掉 idf.py monitor，同一個序列埠一次只能給一個程式讀）：
    source ~/.espressif/tools/activate_idf_v6.1.sh   # ESP-IDF 的 Python 環境內建 pyserial
    python tools/read_levels.py                      # 預設 /dev/ttyACM0、錄 15 秒
    python tools/read_levels.py -p /dev/ttyACM1 -s 20

注意：XIAO ESP32-C6 的 USB Serial/JTAG 在打開序列埠時會讓板子重開機，
開機測試大約跑 5 秒才進入即時顯示，所以錄的秒數不要少於 10 秒。
重開機前殘留的舊資料（時間欄倒退之前的行）會自動丟掉。
"""

import argparse
import re
import statistics
import sys
import time

import serial

# 即時顯示的一行：時間 電極0 電極1 水位% 觸碰 IRQ數 | ...
ROW_RE = re.compile(r"\s*(\d+\.\d)\s+(\d+|--)\s+(\d+|--)\s+(\S+)\s+(\S+)\s+(\d+|--)\s*\|")


def capture(port: str, seconds: float) -> str:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    buf = b""
    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            buf += s.read(4096)
    finally:
        s.close()
    return buf.decode("utf-8", "replace")


def parse(text: str):
    rows = []
    for line in text.splitlines():
        m = ROW_RE.match(line)
        if m and m.group(2) != "--":
            lv = m.group(4).rstrip("*")   # 「*」代表傾斜／移動中沿用上一次的水位
            level = int(lv) if lv.isdigit() else None
            rows.append((float(m.group(1)), int(m.group(2)), int(m.group(3)), level))
    # 只保留最後一次開機之後的資料
    for i in range(len(rows) - 1, 0, -1):
        if rows[i][0] < rows[i - 1][0]:
            rows = rows[i:]
            break
    return rows


def describe(name: str, values):
    return f"{name} 平均 {statistics.mean(values):.1f}（{min(values)}～{max(values)}）"


def main():
    ap = argparse.ArgumentParser(description="錄一段 MPR121 電極讀值，用來校正空杯／滿杯")
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("-s", "--seconds", type=float, default=15)
    args = ap.parse_args()

    try:
        text = capture(args.port, args.seconds)
    except serial.SerialException as e:
        sys.exit(f"打不開或讀不到 {args.port}：{e}\n→ 確認板子有插上，而且 idf.py monitor 已經關掉")

    rows = parse(text)
    if not rows:
        sys.exit("沒有讀到即時顯示的資料（板子可能還在跑開機測試，把 -s 加長再試一次）")

    for t, e0, e1, lv in rows:
        print(f"{t:7.1f}  電極0={e0:4d}  電極1={e1:4d}  水位={lv if lv is not None else '--'}%")
    print("-" * 60)
    print(f"有效筆數 {len(rows)}")
    print(describe("電極0", [r[1] for r in rows]))
    print(describe("電極1", [r[2] for r in rows]))
    levels = [r[3] for r in rows if r[3] is not None]
    if levels:
        print(describe("水位%", levels))


if __name__ == "__main__":
    main()
