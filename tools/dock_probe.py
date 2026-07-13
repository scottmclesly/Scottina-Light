#!/usr/bin/env python3
"""Speak the dock protocol to a real Light over USB CDC.

The bench counterpart to `pio test -e native`: the vectors prove the handler in
memory, this proves it on the wire, through a real SAMD51, a real Seeed FS, and a
real USB stack. DOCK-PROTOCOL.md is the contract.

    python3 tools/dock_probe.py /dev/cu.usbmodem4401

Read-only by default -- it will not PUT, COMMIT or DELETE anything. Pass
--throughput to time a GET of the largest log, which is the number Prime needs
for its chunk size and progress bar (and a truer number than an echo sketch,
because it measures the path Prime will actually use).
"""
import argparse
import sys
import time

import serial

SOF = 0xA5
T = {"HELLO": 0x01, "SET_CLOCK": 0x02, "LIST": 0x03, "PUT": 0x04,
     "COMMIT": 0x05, "GET": 0x06, "DELETE": 0x07, "BYE": 0x08, "ERROR": 0xEF}
TNAME = {v: k for k, v in T.items()}
ERRS = {1: "ERR_BAD_CRC", 2: "ERR_BAD_FRAME", 3: "ERR_UNKNOWN_TYPE", 4: "ERR_NO_SD",
        5: "ERR_PATH_REJECTED", 6: "ERR_NOT_FOUND", 7: "ERR_IO", 8: "ERR_HASH_MISMATCH",
        9: "ERR_BUSY", 10: "ERR_UNSUPPORTED_VER", 11: "ERR_TOO_LARGE"}
QUAL = {0: "unsynced", 1: "rtc", 2: "ntp"}


def crc16(data: bytes) -> int:
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


assert crc16(b"123456789") == 0x29B1, "CRC check value wrong -- fix this first"


def frame(t: int, seq: int, payload: bytes = b"") -> bytes:
    body = bytes([t, seq]) + len(payload).to_bytes(2, "little") + payload
    return bytes([SOF]) + body + crc16(body).to_bytes(2, "little")


def s(text: str) -> bytes:
    b = text.encode()
    return bytes([len(b)]) + b


class Dock:
    def __init__(self, port, timeout=3.0):
        self.p = serial.Serial(port, 115200, timeout=timeout)
        self.seq = 0

    def xfer(self, t, payload=b"", timeout=3.0):
        self.seq = (self.seq + 1) & 0xFF
        seq = self.seq
        self.p.reset_input_buffer()
        self.p.write(frame(T[t], seq, payload))
        self.p.flush()
        return self._read(timeout)

    def raw(self, blob: bytes, timeout=3.0):
        self.p.reset_input_buffer()
        self.p.write(blob)
        self.p.flush()
        return self._read(timeout)

    def _read(self, timeout):
        """Read one frame, tolerating the firmware's ASCII diagnostics on the
        same port -- [dock] lines and SMOKE output share this wire.

        NB: read(n) BLOCKS until n bytes arrive or the timeout expires. Asking
        for a fixed 256 makes every reply smaller than that wait out the full
        timeout -- which made the board look ~3 s slow per request and turned a
        190 KB pull into a ten-minute crawl. Take one byte (blocking), then
        drain whatever else is already buffered.
        """
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            b = self.p.read(1)
            if b:
                b += self.p.read(self.p.in_waiting or 0)
                buf += b
            i = buf.find(bytes([SOF]))
            if i < 0:
                continue
            f = buf[i:]
            if len(f) < 5:
                continue
            ln = int.from_bytes(f[3:5], "little")
            if len(f) < 7 + ln:
                continue
            body, rx = f[1:5 + ln], int.from_bytes(f[5 + ln:7 + ln], "little")
            if crc16(body) != rx:
                buf = buf[i + 1:]
                continue
            return {"type": f[1], "seq": f[2], "payload": f[5:5 + ln],
                    "noise": buf[:i]}
        return None

    def close(self):
        self.p.close()


def show(tag, r):
    if r is None:
        print(f"  {tag:34s} NO RESPONSE (timeout)")
        return None
    t = TNAME.get(r["type"], hex(r["type"]))
    if r["type"] == T["ERROR"]:
        code = int.from_bytes(r["payload"][0:2], "little")
        mlen = r["payload"][2]
        msg = r["payload"][3:3 + mlen].decode()
        print(f"  {tag:34s} {t}  {ERRS.get(code, code)}  \"{msg}\"")
    else:
        print(f"  {tag:34s} {t}  seq={r['seq']}  {len(r['payload'])}B payload")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--throughput", action="store_true")
    a = ap.parse_args()

    d = Dock(a.port)
    print(f"--- dock probe on {a.port} ---\n")

    # 1. HELLO -- identity, and proof the frame reader owns the port.
    print("HELLO")
    r = show("hello", d.xfer("HELLO"))
    if not r or r["type"] != T["HELLO"]:
        print("\nNo HELLO. The dock handler is not answering; nothing below will work.")
        d.close()
        sys.exit(1)

    p, i = r["payload"], 0
    proto = int.from_bytes(p[0:2], "little"); i = 2
    n = p[i]; product = p[i + 1:i + 1 + n].decode(); i += 1 + n
    n = p[i]; fw = p[i + 1:i + 1 + n].decode(); i += 1 + n
    sd = p[i]; i += 1
    epoch = int.from_bytes(p[i:i + 8], "little"); i += 8
    qual = p[i]; i += 1
    setboot = p[i]; i += 1
    maxpay = int.from_bytes(p[i:i + 2], "little"); i += 2
    flags = p[i]
    print(f"      proto_version : {proto}")
    print(f"      product       : {product!r}")
    print(f"      fw_version    : {fw!r}")
    print(f"      sd_present    : {sd}")
    print(f"      clock         : epoch={epoch} quality={QUAL.get(qual)} set_this_boot={setboot}")
    print(f"      max_payload   : {maxpay}")
    print(f"      flags         : 0x{flags:02x} (logging_was_active={flags & 1}, suspended={(flags >> 1) & 1})")

    # 2. The defences, over the wire on real hardware.
    print("\nREJECT PASS + ERROR HANDLING")
    show("unknown type 0x7f",
         d.raw(frame(0x7F, 200)))
    bad = bytearray(frame(T["HELLO"], 201))
    bad[-1] ^= 0xFF  # corrupt the CRC
    show("bad crc", d.raw(bytes(bad)))
    show("false SOF, huge LEN (expect silence)",
         d.raw(bytes([0xA5, 0xFF, 0x00, 0xFF, 0xFF]), timeout=1.0))
    show("write into /logs/",
         d.xfer("PUT", s("/logs/evil.log") + (0).to_bytes(4, "little") + (1).to_bytes(2, "little") + b"x"))
    show("read Tier-2 /config/i2c.json",
         d.xfer("GET", s("/config/i2c.json") + (0).to_bytes(4, "little") + (64).to_bytes(2, "little")))
    show("traversal ../",
         d.xfer("GET", s("/logs/../config/ui.json") + (0).to_bytes(4, "little") + (64).to_bytes(2, "little")))
    show("GET max_len 2048 > max_payload",
         d.xfer("GET", s("/logs/x.log") + (0).to_bytes(4, "little") + (2048).to_bytes(2, "little")))

    # 3. LIST, paginated.
    print("\nLIST")
    logs = []
    for root, hashes in (("/logs/", 0), ("/tables/", 1)):
        start, page = 0, 0
        while True:
            r = d.xfer("LIST", s(root) + bytes([hashes]) + start.to_bytes(2, "little"))
            if not r or r["type"] != T["LIST"]:
                show(f"list {root}", r)
                break
            p, i = r["payload"], 0
            si = int.from_bytes(p[0:2], "little")
            cnt = int.from_bytes(p[2:4], "little"); i = 4
            ents = []
            for _ in range(cnt):
                nl = p[i]; nm = p[i + 1:i + 1 + nl].decode(); i += 1 + nl
                sz = int.from_bytes(p[i:i + 4], "little"); i += 4
                i += 8  # mtime
                hp = p[i]; i += 1
                sha = p[i:i + 32].hex() if hp else None
                if hp:
                    i += 32
                ents.append((nm, sz, sha))
            more = p[i]
            print(f"  {root} page {page}: start={si} count={cnt} more={more}")
            for nm, sz, sha in ents:
                print(f"      {nm:28s} {sz:9d} B  {('sha=' + sha[:12] + '…') if sha else ''}")
                if root == "/logs/":
                    logs.append((nm, sz))
            page += 1
            if not more:
                break
            start = si + cnt

    # 4. Throughput on the real path -- what Prime actually needs.
    if a.throughput and logs:
        nm, sz = max(logs, key=lambda x: x[1])
        print(f"\nTHROUGHPUT -- GET /logs/{nm} ({sz} B) in {maxpay - 7} B chunks")
        chunk = maxpay - 7
        off, t0, rounds = 0, time.perf_counter(), 0
        while True:
            r = d.xfer("GET", s(f"/logs/{nm}") + off.to_bytes(4, "little") + chunk.to_bytes(2, "little"), timeout=5)
            if not r or r["type"] != T["GET"]:
                show("get", r)
                break
            ln = int.from_bytes(r["payload"][4:6], "little")
            eof = r["payload"][6 + ln]
            off += ln
            rounds += 1
            if eof:
                break
        dt = time.perf_counter() - t0
        if dt > 0 and rounds:
            print(f"      {off} B in {dt:.2f}s over {rounds} round trips")
            print(f"      {off / dt / 1024:.1f} KB/s   |   {dt / rounds * 1000:.2f} ms per round trip")
            print(f"      => an 8 MB log would take ~{8 * 1024 * 1024 / (off / dt) / 60:.1f} min at this chunk size")

    print("\nBYE")
    show("bye", d.xfer("BYE"))
    d.close()


if __name__ == "__main__":
    main()
