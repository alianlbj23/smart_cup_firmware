"""模擬 App 的 BLE 流程：掃描 → 連線 → 對時 → 讀電量 → 訂閱 → 收封包並 ACK（ATTITUDE 不 ACK）→ 統計。

Device Status（韌體 0.4.0 起 10 bytes）會印出杯子自己回報的 BLE 位址，並和掃描到的位址比對
（Linux／Windows 掃描到的就是 BLE 位址；macOS 給的是 UUID，沒辦法比）。

DRINK_EVENT（0x07）每一筆都印出來，並回 3-byte ACK [0x07, seq 低 8 bits, seq 高 3 bits]，韌體收到才從 flash 刪除。

  python tools/ble_check.py 10            # 收 10 秒
  python tools/ble_check.py 30 --no-ack   # 故意不回 0x07 的 ACK：測重送（2 秒逾時、最多重試 3 次）與下次連線補傳
"""
import argparse, asyncio, struct, sys, time
from collections import Counter
from bleak import BleakScanner, BleakClient

SVC = "3c8b0001-efc1-4061-946c-e5916790df93"
TIME = "3c8b0002-efc1-4061-946c-e5916790df93"
DATA = "3c8b0003-efc1-4061-946c-e5916790df93"
ACK = "3c8b0004-efc1-4061-946c-e5916790df93"
STATUS = "3c8b0005-efc1-4061-946c-e5916790df93"
POSES = ["未知", "直立", "傾斜", "飲用中", "倒下"]
MSG_DRINK_EVENT = 0x07

ap = argparse.ArgumentParser(description="用電腦模擬 App 測試 SmartCup BLE")
ap.add_argument("seconds", nargs="?", type=float, default=10, help="收幾秒（預設 10）")
ap.add_argument("--no-ack", action="store_true", help="不回 DRINK_EVENT（0x07）的 3-byte ACK，測試重送與斷線補傳")
ARGS = ap.parse_args()
SECONDS = ARGS.seconds


def fmt_time(ts: int) -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts))


def fmt_status(st: bytes, scan_addr: str = "") -> str:
    """Device Status：[電量, major, minor, patch]，長度 ≥ 10 時後面 6 bytes 是 BLE 位址（顯示順序，最高位元組在前）。"""
    if len(st) < 4:
        return f"Device Status: {list(st)}（長度 {len(st)}，少於 4 bytes，格式不對）"
    bat = "未知" if st[0] == 0xFF else f"{st[0]}%"
    text = f"Device Status: {list(st)}（電量 {bat}，韌體 {st[1]}.{st[2]}.{st[3]}"
    if len(st) >= 10:
        addr = ":".join(f"{b:02X}" for b in st[4:10])
        text += f"，位址 {addr}"
        scan = scan_addr.upper()
        if len(scan) == 17 and scan.count(":") == 5:   # 掃描到的是 BLE 位址（不是 macOS 的 UUID）才比對
            text += "，和掃描到的位址一致" if scan == addr else f"，和掃描到的位址 {scan} 不一致！"
    else:
        text += "，沒有位址：韌體 0.4.0 以前，或位址還沒準備好"
    return text + "）"


def decode_drink_event(v: int, ts: int) -> str:
    a = (v & 0xFF) / 2
    b = ((v >> 8) & 0xFF) / 2
    kind = {0: "喝水", 1: "加水"}.get((v >> 16) & 0x3, f"保留({(v >> 16) & 0x3})")
    saw = (v >> 18) & 1
    too_long = (v >> 19) & 1
    time_unknown = (v >> 20) & 1
    seq = v >> 21
    flags = []
    flags.append("有飲用姿態" if saw else "沒有飲用姿態")
    if too_long:
        flags.append("過程太久")
    when = "時間未知" if time_unknown else f"開始 {fmt_time(ts)}"
    return f"DRINK_EVENT seq {seq} {kind} {a:.1f}% → {b:.1f}%（{'、'.join(flags)}，{when}）"


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
    if t == 6:
        state = {0: "活動中", 1: "休眠中"}.get(v & 0xFF, f"未知({v & 0xFF})")
        return t, f"STATE {state}（已 {v >> 8} 秒）", ts
    if t == MSG_DRINK_EVENT:
        return t, decode_drink_event(v, ts), ts
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
        print(fmt_status(bytes(st), dev.address))
        counts = Counter()
        last = {}
        first_ts = []
        drink_seen = {}

        t_start = time.time()

        async def on_data(_, data: bytearray):
            t, text, ts = decode(bytes(data))
            if t in (6, MSG_DRINK_EVENT):   # 狀態改變、喝水事件很重要，每一筆都印出來
                print(f"  {time.time() - t_start:5.1f}s  {text}", flush=True)
            counts[t] += 1
            last[t] = text
            if not first_ts and t != MSG_DRINK_EVENT:   # 0x07 的時間戳是事件開始時間，不拿來比對時
                first_ts.append(ts)
            if t == MSG_DRINK_EVENT:
                v = struct.unpack_from("<I", data, 1)[0]
                seq = v >> 21
                if ARGS.no_ack:
                    print(f"          （--no-ack：不回 seq {seq} 的 ACK）", flush=True)
                else:
                    await c.write_gatt_char(ACK, bytes([MSG_DRINK_EVENT, seq & 0xFF, (seq >> 8) & 0x07]), response=True)
                    seen = drink_seen.setdefault(seq, 0)
                    drink_seen[seq] = seen + 1
                    print(f"          已回 ACK [0x07, 0x{seq & 0xFF:02x}, 0x{(seq >> 8) & 0x07:02x}]"
                          + ("（重複收到，可能是 ACK 晚到）" if seen else ""), flush=True)
            elif t != 5:
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
