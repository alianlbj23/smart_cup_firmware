"""模擬 App 的 BLE 流程：掃描 → 連線 → 對時 → 讀電量 → 訂閱 → 收封包並 ACK（ATTITUDE 不 ACK）→ 統計。"""
import asyncio, struct, sys, time
from collections import Counter
from bleak import BleakScanner, BleakClient

SVC = "3c8b0001-efc1-4061-946c-e5916790df93"
TIME = "3c8b0002-efc1-4061-946c-e5916790df93"
DATA = "3c8b0003-efc1-4061-946c-e5916790df93"
ACK = "3c8b0004-efc1-4061-946c-e5916790df93"
STATUS = "3c8b0005-efc1-4061-946c-e5916790df93"
POSES = ["未知", "直立", "傾斜", "飲用中", "倒下"]
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 10


def decode(b: bytes):
    t, v, ts = struct.unpack("<BII", b)
    if t == 2:
        fill = v & ((1 << 25) - 1)
        return t, f"WATER {fill / ((1 << 25) - 1) * 100:.2f}% 刻度 {(v >> 25) & 0xF}", ts
    if t == 3:
        return t, f"RAW {v}", ts
    if t == 4:
        return t, f"POSE {POSES[(v >> 16) & 0xFF] if ((v >> 16) & 0xFF) < 5 else '?'} 傾斜 {(v & 0xFFFF) / 10:.1f}° 移動 {(v >> 24) & 0xFF}", ts
    if t == 5:
        p, r = struct.unpack("<hh", struct.pack("<I", v))
        return t, f"ATTITUDE pitch {p / 10:+.1f}° roll {r / 10:+.1f}°", ts
    return t, f"type 0x{t:02x} value {v}", ts


async def main():
    print("掃描中…")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "").startswith("SmartCup-") or SVC in [u.lower() for u in ad.service_uuids],
        timeout=15)
    if dev is None:
        sys.exit("找不到 SmartCup")
    print(f"找到 {dev.name} {dev.address}")
    async with BleakClient(dev) as c:
        svc_uuids = [s.uuid for s in c.services]
        print("Service 存在:", SVC in svc_uuids)
        now = int(time.time())
        await c.write_gatt_char(TIME, struct.pack("<I", now), response=True)
        st = await c.read_gatt_char(STATUS)
        print(f"Device Status: {list(st)}（電量 {'未知' if st[0] == 0xFF else str(st[0]) + '%'}，韌體 {st[1]}.{st[2]}.{st[3]}）")
        counts = Counter()
        last = {}
        first_ts = []

        async def on_data(_, data: bytearray):
            t, text, ts = decode(bytes(data))
            counts[t] += 1
            last[t] = text
            if not first_ts:
                first_ts.append(ts)
            if t != 5:
                await c.write_gatt_char(ACK, b"\x01", response=True)

        await c.start_notify(DATA, on_data)
        await asyncio.sleep(SECONDS)
        await c.stop_notify(DATA)
        print(f"{SECONDS:.0f} 秒內收到：", {f"0x{k:02x}": v for k, v in sorted(counts.items())})
        for k in sorted(last):
            print("  最後一筆", last[k])
        if first_ts:
            print(f"  封包時間戳與對時差 {first_ts[0] - now} 秒")

asyncio.run(main())
