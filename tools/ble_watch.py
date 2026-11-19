#!/usr/bin/env python3
"""
休眠測試用：持續掃描 SmartCup 的 BLE 廣播，印出「醒著 / 睡著」的時間軸。

深度睡眠時不會廣播，所以廣播斷掉 = 睡著、重新出現 = 醒來。不用接序列埠
（打開序列埠會讓板子重開機，會蓋掉「從休眠醒來」的狀態）。

用法：~/blevenv/bin/python tools/ble_watch.py [秒數，預設 120]
"""
import asyncio
import sys
import time

from bleak import BleakScanner

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 120
GAP_S = 3.0   # 超過 3 秒沒收到廣播就算睡著（廣播間隔遠小於 1 秒）


async def main():
    t0 = time.time()
    last_seen = None
    awake = False
    name = None

    def on_adv(dev, ad):
        nonlocal last_seen, awake, name
        if not (dev.name or ad.local_name or "").startswith("SmartCup-"):
            return
        name = dev.name or ad.local_name
        now = time.time()
        if not awake:
            print(f"{now - t0:6.1f}s  醒著：開始收到 {name} 的廣播", flush=True)
            awake = True
        last_seen = now

    async with BleakScanner(detection_callback=on_adv):
        while time.time() - t0 < SECONDS:
            await asyncio.sleep(0.5)
            if awake and last_seen and time.time() - last_seen > GAP_S:
                print(f"{last_seen - t0:6.1f}s  睡著：廣播停止", flush=True)
                awake = False
    print(f"{SECONDS:.0f} 秒觀察結束，目前{'醒著' if awake else '睡著'}")


asyncio.run(main())
