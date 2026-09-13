#!/usr/bin/env python3
"""Drive Flycast's GDB server to answer checklist steps 8 and 9 (docs/baseline-game.md).

Flycast (Debug.GDBEnabled = yes, Debug.GDBPort = 3263) speaks the GDB Remote Serial Protocol for
the SH-4. This client needs no GDB install: it reads the BIOS syscall vector table, plants
software breakpoints on the vector targets, and logs every call with its arguments (step 8, which
syscalls the game uses and how often), and it can sample the PC at intervals to find where the game
idles (step 9, is it waiting on VBlank).

  gdbtrace.py syscalls [--seconds 120] [--out trace.jsonl]
  gdbtrace.py sample   [--seconds 20] [--interval 0.05] [--out pcs.json]
  gdbtrace.py regs                       # one-shot register dump, sanity check
  gdbtrace.py mem ADDR LEN --out FILE    # dump guest memory (e.g. code the game copied into RAM)

Start Flycast first, e.g.
  /Applications/Flycast.app/Contents/MacOS/Flycast -config config:Debug.GDBEnabled=yes \\
      -config config:Debug.GDBWaitForConnection=yes games/crazytaxi.chd
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time

SYSCALL_VECTORS = {
    0x8C0000B0: "SYSINFO",
    0x8C0000B4: "ROMFONT",
    0x8C0000B8: "FLASHROM",
    0x8C0000BC: "GDROM_MISC",
    0x8C0000E0: "SYSTEM",
}

# GDB's SH-4 register numbering: r0-r15, pc, pr, gbr, vbr, mach, macl, sr, fpul, fpscr, fr0-15.
REG_NAMES = [f"r{i}" for i in range(16)] + ["pc", "pr", "gbr", "vbr", "mach", "macl", "sr", "fpul", "fpscr"]

GDROM_FUNCS = {  # r7 when r6 == 0 (GD-ROM group) per the Dreamcast BIOS documentation
    0: "SEND_COMMAND", 1: "CHECK_COMMAND", 2: "MAINLOOP", 3: "INIT", 4: "CHECK_DRIVE",
    8: "ABORT_COMMAND", 9: "RESET", 10: "SECTOR_MODE",
}
GDROM_COMMANDS = {  # r4 of SEND_COMMAND; numbering per KallistiOS dc/cdrom.h and Flycast reios
    2: "CHECK_LICENSE", 4: "REQ_SPI_CMD", 16: "PIOREAD", 17: "DMAREAD", 18: "GETTOC", 19: "GETTOC2",
    20: "PLAY", 21: "PLAY2", 22: "PAUSE", 23: "RELEASE", 24: "INIT", 25: "DMA_ABORT", 26: "OPEN_TRAY",
    27: "SEEK", 28: "DMAREAD_STREAM", 29: "NOP", 30: "REQ_MODE", 31: "SET_MODE", 32: "SCAN_CD",
    33: "STOP", 34: "GETSCD", 35: "GETSES", 36: "REQ_STAT", 37: "PIOREAD_STREAM",
    38: "DMAREAD_STREAM_EX", 39: "PIOREAD_STREAM_EX", 40: "GET_VERS",
}


class Rsp:
    def __init__(self, host: str, port: int, timeout: float = 10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""

    # -- framing ---------------------------------------------------------------------------

    @staticmethod
    def _csum(payload: bytes) -> int:
        return sum(payload) & 0xFF

    def send(self, payload: str) -> None:
        p = payload.encode()
        self.sock.sendall(b"$" + p + b"#" + f"{self._csum(p):02x}".encode())

    def _recv_packet(self, timeout: float | None = None) -> str:
        self.sock.settimeout(timeout if timeout is not None else 10.0)
        while True:
            i = self.buf.find(b"$")
            if i >= 0:
                j = self.buf.find(b"#", i)
                if j >= 0 and len(self.buf) >= j + 3:
                    payload = self.buf[i + 1:j]
                    self.buf = self.buf[j + 3:]
                    self.sock.sendall(b"+")
                    return payload.decode(errors="replace")
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("gdb server closed the connection")
            self.buf += chunk

    def cmd(self, payload: str, timeout: float | None = None) -> str:
        self.send(payload)
        # skip acks
        while True:
            r = self._recv_packet(timeout)
            if r != "":
                return r
            return r

    # -- operations ------------------------------------------------------------------------

    def status(self) -> str:
        return self.cmd("?")

    def read_regs(self) -> dict[str, int]:
        hexdata = self.cmd("g")
        raw = bytes.fromhex(hexdata[: (len(hexdata) // 8) * 8])
        regs = {}
        for i, name in enumerate(REG_NAMES):
            if 4 * i + 4 <= len(raw):
                regs[name] = struct.unpack_from("<I", raw, 4 * i)[0]
        return regs

    def read_mem(self, addr: int, length: int) -> bytes:
        r = self.cmd(f"m{addr:x},{length:x}")
        if r.startswith("E"):
            raise IOError(f"memory read {addr:#x} failed: {r}")
        return bytes.fromhex(r)

    def read_u32(self, addr: int) -> int:
        return struct.unpack("<I", self.read_mem(addr, 4))[0]

    def set_breakpoint(self, addr: int) -> bool:
        return self.cmd(f"Z0,{addr:x},2") == "OK"

    def clear_breakpoint(self, addr: int) -> bool:
        return self.cmd(f"z0,{addr:x},2") == "OK"

    def cont(self, timeout: float) -> str | None:
        """Continue; return the stop reply, or None if nothing stopped within timeout."""
        self.send("c")
        try:
            return self._recv_packet(timeout)
        except socket.timeout:
            return None

    def interrupt(self) -> str:
        self.sock.sendall(b"\x03")
        return self._recv_packet(5.0)

    def step(self) -> str:
        return self.cmd("s")


def connect(args) -> Rsp:
    deadline = time.time() + args.connect_timeout
    while True:
        try:
            rsp = Rsp(args.host, args.port)
            break
        except OSError:
            if time.time() > deadline:
                sys.exit(f"could not connect to Flycast GDB server at {args.host}:{args.port}")
            time.sleep(0.5)
    st = rsp.status()
    print(f"connected; status {st}", file=sys.stderr)
    if getattr(args, "boot_wait", 0) > 0:
        # With GDBWaitForConnection the machine is halted before the BIOS runs, so the syscall
        # vectors are not populated yet. Let it boot, then halt it again.
        print(f"letting the machine run {args.boot_wait:.0f}s to boot...", file=sys.stderr)
        rsp.send("c")
        time.sleep(args.boot_wait)
        st = rsp.interrupt()
        print(f"halted; status {st}", file=sys.stderr)
    return rsp


def cmd_regs(args) -> int:
    rsp = connect(args)
    regs = rsp.read_regs()
    for k, v in regs.items():
        print(f"{k:6s} {v:08x}")
    return 0


def describe(vec: str, r: dict[str, int]) -> str:
    if vec == "GDROM_MISC":
        if r["r6"] == 0:
            f = GDROM_FUNCS.get(r["r7"], f"gd_fn{r['r7']}")
            if f == "SEND_COMMAND":
                return f"GDROM {f} {GDROM_COMMANDS.get(r['r4'], f'cmd{r['r4']}')} params@{r['r5']:08x}"
            return f"GDROM {f} r4={r['r4']:08x} r5={r['r5']:08x}"
        return f"MISC fn{r['r7']} r4={r['r4']:08x} r5={r['r5']:08x} r6={r['r6']:08x}"
    return f"{vec} r4={r['r4']:08x} r5={r['r5']:08x} r6={r['r6']:08x} r7={r['r7']:08x}"


def cmd_syscalls(args) -> int:
    rsp = connect(args)
    targets: dict[int, str] = {}
    for vec, name in SYSCALL_VECTORS.items():
        try:
            t = rsp.read_u32(vec)
        except IOError as e:
            print(f"{name}: {e}", file=sys.stderr)
            continue
        print(f"vector {vec:08x} {name:10s} -> {t:08x}", file=sys.stderr)
        if t and t != 0xFFFFFFFF:
            targets[t] = name
    if not targets:
        sys.exit("no syscall vectors readable; is the game booted?")
    for t in targets:
        ok = rsp.set_breakpoint(t)
        print(f"breakpoint {t:08x} {targets[t]}: {'ok' if ok else 'REFUSED'}", file=sys.stderr)

    out = open(args.out, "w") if args.out else None
    counts: dict[str, int] = {}
    t0 = time.time()
    n = 0
    last_pc = None
    while time.time() - t0 < args.seconds:
        stop = rsp.cont(timeout=max(1.0, args.seconds - (time.time() - t0)))
        if stop is None:
            break
        regs = rsp.read_regs()
        pc = regs.get("pc", 0)
        vec = targets.get(pc)
        if vec is None:
            # stopped somewhere else (e.g. user pause); resume
            if pc == last_pc:
                break
            last_pc = pc
            continue
        desc = describe(vec, regs)
        counts[desc.split(" params")[0] if "params" in desc else desc.split(" r4=")[0]] = \
            counts.get(desc.split(" params")[0] if "params" in desc else desc.split(" r4=")[0], 0) + 1
        rec = {"t": round(time.time() - t0, 3), "vector": vec, "pc": f"{pc:08x}", "pr": f"{regs['pr']:08x}",
               "r4": f"{regs['r4']:08x}", "r5": f"{regs['r5']:08x}", "r6": f"{regs['r6']:08x}",
               "r7": f"{regs['r7']:08x}", "desc": desc}
        n += 1
        if out:
            out.write(json.dumps(rec) + "\n")
        if n <= 40 or n % 200 == 0:
            print(f"[{rec['t']:8.3f}] {desc}  (from {rec['pr']})", file=sys.stderr)
        # step over the breakpoint: remove, single-step, re-insert
        rsp.clear_breakpoint(pc)
        rsp.step()
        rsp.set_breakpoint(pc)
    for t in targets:
        rsp.clear_breakpoint(t)
    rsp.send("c")
    print(f"\n{n} syscalls in {time.time() - t0:.1f}s", file=sys.stderr)
    for k, v in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"{v:7d}  {k}")
    if out:
        out.close()
    return 0


def cmd_mem(args) -> int:
    """Dump guest memory to a file (and hex to stderr) for offline disassembly."""
    rsp = connect(args)
    data = b""
    addr = args.addr
    while len(data) < args.length:
        chunk = min(256, args.length - len(data))
        data += rsp.read_mem(addr + len(data), chunk)
    with open(args.out, "wb") as f:
        f.write(data)
    for off in range(0, len(data), 16):
        print(f"{addr + off:08x}  " + " ".join(f"{b:02x}" for b in data[off:off + 16]), file=sys.stderr)
    rsp.send("c")
    print(f"wrote {len(data)} bytes to {args.out}", file=sys.stderr)
    return 0


def cmd_sample(args) -> int:
    rsp = connect(args)
    pcs: dict[str, int] = {}
    t0 = time.time()
    n = 0
    rsp.send("c")
    while time.time() - t0 < args.seconds:
        time.sleep(args.interval)
        try:
            rsp.interrupt()
        except socket.timeout:
            continue
        regs = rsp.read_regs()
        pc = f"{regs.get('pc', 0):08x}"
        pcs[pc] = pcs.get(pc, 0) + 1
        n += 1
        rsp.send("c")
    top = sorted(pcs.items(), key=lambda kv: -kv[1])[:25]
    print(f"{n} samples; top PCs:")
    for pc, c in top:
        print(f"{c:6d}  {pc}")
    if args.out:
        json.dump({"samples": n, "pcs": pcs}, open(args.out, "w"), indent=1)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3263)
    ap.add_argument("--connect-timeout", type=float, default=60.0)
    ap.add_argument("--boot-wait", type=float, default=0.0,
                    help="seconds to let the machine run before halting it (use with GDBWaitForConnection)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("regs"); s.set_defaults(fn=cmd_regs)
    s = sub.add_parser("syscalls"); s.add_argument("--seconds", type=float, default=120)
    s.add_argument("--out"); s.set_defaults(fn=cmd_syscalls)
    s = sub.add_parser("mem"); s.add_argument("addr", type=lambda x: int(x, 0))
    s.add_argument("length", type=lambda x: int(x, 0)); s.add_argument("--out", required=True)
    s.set_defaults(fn=cmd_mem)
    s = sub.add_parser("sample"); s.add_argument("--seconds", type=float, default=20)
    s.add_argument("--interval", type=float, default=0.05); s.add_argument("--out")
    s.set_defaults(fn=cmd_sample)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
