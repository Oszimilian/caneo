#!/usr/bin/env python3
"""Minimal XCP-on-CAN(-FD) slave for testing caneo's XCP master (no dependencies).

    scripts/xcp_sim.py --iface vcan0 [--fd] [--rx 0x7F0] [--tx 0x7F1] [--elf tests/data/xcp_target.elf]
                       [--sine <addr>] [--counter <addr>] [--id-field 0..3]

Memory is a sparse byte map, preloaded from the ELF's .data section if --elf is given. --sine writes a float32
sine (1 Hz) and --counter increments a uint32 every 1 ms. Supports polling (SHORT_UPLOAD / UPLOAD), DOWNLOAD and
dynamic DAQ with two events ("10ms", "100ms") and 4-byte timestamps in us.
"""
import argparse
import math
import socket
import struct
import threading
import time

CAN_FMT = "=IB3x8s"
CANFD_FMT = "=IBBBx64s"
CANFD_BRS = 0x01


def fd_len(n):
    if n <= 8:
        return n
    for l in (12, 16, 20, 24, 32, 48, 64):
        if n <= l:
            return l
    return 64


class Elf:
    """Just enough ELF parsing to preload .data / .rodata (little endian, 32/64 bit)."""

    @staticmethod
    def sections(path):
        d = open(path, "rb").read()
        if d[:4] != b"\x7fELF":
            return []
        is64 = d[4] == 2
        le = "<" if d[5] == 1 else ">"
        if is64:
            shoff, = struct.unpack_from(le + "Q", d, 0x28)
            shentsize, shnum, shstrndx = struct.unpack_from(le + "HHH", d, 0x3A)
        else:
            shoff, = struct.unpack_from(le + "I", d, 0x20)
            shentsize, shnum, shstrndx = struct.unpack_from(le + "HHH", d, 0x2E)
        secs = []
        for i in range(shnum):
            o = shoff + i * shentsize
            if is64:
                name, typ, flags, addr, off, size = struct.unpack_from(le + "IIQQQQ", d, o)
            else:
                name, typ, flags, addr, off, size = struct.unpack_from(le + "IIIIII", d, o)
            secs.append((name, typ, addr, off, size))
        strtab = secs[shstrndx]
        out = []
        for name, typ, addr, off, size in secs:
            n = d[strtab[3] + name:d.index(b"\0", strtab[3] + name)].decode()
            if typ == 1 and addr and n in (".data", ".rodata", ".sdata"):   # PROGBITS
                out.append((addr, d[off:off + size]))
        return out


class Slave:
    def __init__(self, a):
        self.a = a
        self.fd = a.fd
        self.max_cto = 64 if a.fd else 8
        self.max_dto = 64 if a.fd else 8
        self.mem = {}
        self.lock = threading.Lock()
        self.mta = 0
        self.connected = False
        self.daq = []          # list of dicts: odts=[[(addr,size)]], event, mode, selected, running, first_pid
        self.ptr = (0, 0, 0)
        self.t0 = time.monotonic()
        self.sock = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        if a.fd:
            self.sock.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FD_FRAMES, 1)
        self.sock.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FILTER, struct.pack("=II", a.rx, 0x7FF))
        self.sock.bind((a.iface,))
        if a.elf:
            for addr, data in Elf.sections(a.elf):
                for i, b in enumerate(data):
                    self.mem[addr + i] = b
        self.ident = b"caneo_xcp_sim"

    # memory
    def read(self, addr, n):
        with self.lock:
            return bytes(self.mem.get(addr + i, 0) for i in range(n))

    def write(self, addr, data):
        with self.lock:
            for i, b in enumerate(data):
                self.mem[addr + i] = b

    # transport
    def send(self, data):
        data = bytes(data)
        if self.fd:
            n = fd_len(max(len(data), 8))
            frame = struct.pack(CANFD_FMT, self.a.tx, n, CANFD_BRS, 0, data.ljust(64, b"\0"))
        else:
            frame = struct.pack(CAN_FMT, self.a.tx, 8, data.ljust(8, b"\0"))
        try:
            self.sock.send(frame)
        except OSError as e:   # ENOBUFS
            time.sleep(0.001)
            self.sock.send(frame)

    def res(self, *data):
        self.send(bytes([0xFF]) + bytes(data))

    def err(self, code):
        self.send(bytes([0xFE, code]))

    # protocol
    def handle(self, p):
        c = p[0]
        if c == 0xFF:   # CONNECT
            self.connected = True
            self.res(0x05, 0x00, self.max_cto, *struct.pack("<H", self.max_dto), 1, 1)
            return
        if not self.connected:
            return
        if c == 0xFE:
            self.connected = False
            for d in self.daq:
                d["running"] = False
            self.res()
        elif c == 0xFD:
            self.res(0, 0, 0, 0, 0)
        elif c == 0xFC:
            self.err(0x00)
        elif c == 0xFA:   # GET_ID: via MTA
            self.mta, self.text, self.text_off = -1, self.ident, 0
            self.res(0, 0, 0, *struct.pack("<I", len(self.ident)))
        elif c == 0xF6:
            self.mta = struct.unpack_from("<I", p, 4)[0]
            self.res()
        elif c == 0xF5:
            n = p[1]
            if self.mta < 0:   # GET_ID / event name: self.text from offset self.text_off
                data = self.text[self.text_off:self.text_off + n].ljust(n, b"\0")
                self.text_off += n
                self.res(*data)
                return
            self.res(*self.read(self.mta, n))
            self.mta += n
        elif c == 0xF4:
            n, addr = p[1], struct.unpack_from("<I", p, 4)[0]
            self.res(*self.read(addr, n))
        elif c == 0xF0:
            n = p[1]
            self.write(self.mta, p[2:2 + n])
            self.mta += n
            self.res()
        elif c == 0xDA:   # GET_DAQ_PROCESSOR_INFO: dynamic, timestamps
            self.res(0x11, *struct.pack("<H", 16), *struct.pack("<H", 2), 0, self.a.id_field << 6)
        elif c == 0xD9:   # resolution: entry 1 / max 255, timestamp 4 bytes, unit 1 us, ticks 1
            self.res(1, 255, 1, 255, 0x34, *struct.pack("<H", 1))
        elif c == 0xD7:
            ev = struct.unpack_from("<H", p, 2)[0]
            names = [b"10ms", b"100ms"]
            if ev >= len(names):
                return self.err(0x22)
            self.mta, self.text, self.text_off = -1, names[ev], 0
            self.res(0x04, 16, len(names[ev]), 10 if ev == 0 else 100, 6, 0)
        elif c == 0xD6:
            self.daq = []
            self.res()
        elif c == 0xD5:
            n = struct.unpack_from("<H", p, 2)[0]
            self.daq = [dict(odts=[], event=0, mode=0, selected=False, running=False, first_pid=0) for _ in range(n)]
            self.res()
        elif c == 0xD4:
            l = struct.unpack_from("<H", p, 2)[0]
            self.daq[l]["odts"] = [[] for _ in range(p[4])]
            self.res()
        elif c == 0xD3:
            l = struct.unpack_from("<H", p, 2)[0]
            self.daq[l]["odts"][p[4]] = [None] * p[5]
            self.res()
        elif c == 0xE2:
            l = struct.unpack_from("<H", p, 2)[0]
            self.ptr = (l, p[4], p[5])
            self.res()
        elif c == 0xE1:
            l, o, e = self.ptr
            self.daq[l]["odts"][o][e] = (struct.unpack_from("<I", p, 4)[0], p[2])
            self.ptr = (l, o, e + 1)
            self.res()
        elif c == 0xE0:
            l, ev = struct.unpack_from("<HH", p, 2)
            self.daq[l]["mode"], self.daq[l]["event"] = p[1], ev
            self.res()
        elif c == 0xDE:
            l = struct.unpack_from("<H", p, 2)[0]
            first = sum(len(d["odts"]) for d in self.daq[:l]) if self.a.id_field == 0 else 0
            self.daq[l]["first_pid"] = first
            if p[1] == 2:
                self.daq[l]["selected"] = True
            elif p[1] == 1:
                self.daq[l]["running"] = True
            else:
                self.daq[l]["running"] = False
            self.res(first)
        elif c == 0xDD:
            for d in self.daq:
                if p[1] == 0:
                    d["running"] = False
                elif p[1] == 1 and d["selected"]:
                    d["running"], d["selected"] = True, False
                elif p[1] == 2 and d["selected"]:
                    d["running"], d["selected"] = False, False
            self.res()
        else:
            self.err(0x20)

    def daq_event(self, ev):
        ts = int((time.monotonic() - self.t0) * 1e6) & 0xFFFFFFFF
        for li, d in enumerate(self.daq):
            if not d["running"] or d["event"] != ev:
                continue
            for oi, odt in enumerate(d["odts"]):
                idf = self.a.id_field
                if idf == 0:
                    hdr = bytes([d["first_pid"] + oi])
                elif idf == 1:
                    hdr = bytes([oi, li])
                elif idf == 2:
                    hdr = bytes([oi]) + struct.pack("<H", li)
                else:
                    hdr = bytes([oi, 0]) + struct.pack("<H", li)
                if oi == 0 and d["mode"] & 0x10:
                    hdr += struct.pack("<I", ts)
                body = b"".join(self.read(a, n) for a, n in odt if a is not None)
                self.send(hdr + body)

    def sim(self):
        n = 0
        while True:
            time.sleep(0.001)
            n += 1
            t = time.monotonic() - self.t0
            if self.a.sine is not None:
                self.write(self.a.sine, struct.pack("<f", math.sin(2 * math.pi * t)))
            if self.a.counter is not None:
                c = struct.unpack("<I", self.read(self.a.counter, 4))[0]
                self.write(self.a.counter, struct.pack("<I", (c + 1) & 0xFFFFFFFF))
            if n % 10 == 0:
                self.daq_event(0)
            if n % 100 == 0:
                self.daq_event(1)

    def run(self):
        threading.Thread(target=self.sim, daemon=True).start()
        print(f"XCP slave on {self.a.iface} ({'CAN FD' if self.fd else 'CAN'}), rx 0x{self.a.rx:X}, tx 0x{self.a.tx:X}")
        while True:
            frame = self.sock.recv(72)
            if len(frame) == 72:
                cid, ln, flags, _, data = struct.unpack(CANFD_FMT, frame)
            else:
                cid, ln, data = struct.unpack(CAN_FMT, frame)
            p = data[:ln]
            if p:
                self.handle(p)


def main():
    ap = argparse.ArgumentParser()
    num = lambda s: int(s, 0)
    ap.add_argument("--iface", default="vcan0")
    ap.add_argument("--fd", action="store_true")
    ap.add_argument("--rx", type=num, default=0x7F0, help="master -> slave id")
    ap.add_argument("--tx", type=num, default=0x7F1, help="slave -> master id")
    ap.add_argument("--elf")
    ap.add_argument("--sine", type=num)
    ap.add_argument("--counter", type=num)
    ap.add_argument("--id-field", type=int, default=0, choices=range(4))
    Slave(ap.parse_args()).run()


if __name__ == "__main__":
    main()
