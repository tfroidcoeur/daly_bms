#!/usr/bin/env python3
"""
Fake three Daly BMS units on a CAN interface, using python-can.

Setup:
    python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt
    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan
    sudo ip link set up vcan0

Run:
    .venv/bin/python tools/daly_sim.py --scenario weak-cell

Then run the GUI simulator against the same interface, and watch raw traffic with
`candump vcan0`.

Because this goes through python-can, the same simulator drives a real adapter as
well as vcan - useful for bench-testing the ESP32 before the BMS units are wired
up:

    .venv/bin/python tools/daly_sim.py -I slcan -i /dev/ttyACM0 -b 250000

Scenarios:
    normal      three healthy packs slowly discharging
    weak-cell   pack 2 has one cell drifting low, spread widens over time
    offline     pack 3 stops answering after 15 s, returns after another 20 s
    charging    the bank is on charge, SoC rising
    alarm       pack 1 raises a cell-overvoltage alarm flag
    faults      several at once: a pack voltage mismatch, a BMS fault flag, a
                badly drifted cell, an over-temperature pack and a low SoC -
                enough to overflow the two footer lines and exercise the
                "+N more" count
    winch       winch duty: long standby broken by pulls that ramp the bank to
                about 600 A (200 A per pack), so the current gauge is driven
                over its full range

Protocol variants:
    --frame-base 0   number 0x95/0x96 frames from 0, as Daly's own document says
                     (default 1, as the firmware we have met does)
The decoder works this out rather than assuming it, and this switch is how that
gets exercised without owning a pack of each kind.
"""

import argparse
import math
import random
import struct
import sys
import time

try:
    import can
except ImportError:
    sys.exit("python-can is not installed.\n"
             "  python3 -m venv .venv && "
             ".venv/bin/pip install -r tools/requirements.txt")

PRIO = 0x18
HOST_ADDR = 0x40

CMD_SOC, CMD_CELL_MINMAX, CMD_TEMP_MINMAX = 0x90, 0x91, 0x92
CMD_MOS, CMD_STATUS, CMD_CELL_VOLTS = 0x93, 0x94, 0x95
CMD_CELL_TEMPS, CMD_BALANCE, CMD_FAULTS = 0x96, 0x97, 0x98

CELLS = 24          # 24S LiFePO4: 60.0 V empty, 87.6 V full
TEMPS = 4


def u16(v):
    return struct.pack(">H", max(0, min(0xFFFF, int(v))))


class Pack:
    """One simulated pack. All state is plain numbers advanced by tick()."""

    def __init__(self, addr, soc_pct, seed):
        self.addr = addr
        self.soc = soc_pct
        self.rng = random.Random(seed)
        self.cycles = 120 + addr * 7
        # Where byte 0 of a 0x95/0x96 burst starts counting; see --frame-base.
        self.frame_base = 1
        self.life = 0
        self.online = True
        self.alarm = bytearray(7)
        self.weak_cell = None       # (index, extra_droop_mv)
        self.weak_ramp_s = 60.0     # seconds for the droop to develop
        self.cell_offset_mv = 0     # shifts the whole pack, for mismatch tests
        self.temp_offset_c = 0
        self.peak_a = 0             # > 0 selects the winch duty cycle
        self.charging = False
        self.base_cell_mv = 3320 + addr
        self.t0 = time.monotonic()

    # -- derived quantities ------------------------------------------------

    def cell_mvs(self):
        t = time.monotonic() - self.t0
        out = []
        for i in range(CELLS):
            # A little static spread per cell plus a slow shared wander.
            mv = self.base_cell_mv + (i % 5) - 2 + 6.0 * math.sin(t / 30 + i)
            mv += (self.soc - 80) * 1.5           # SoC moves the whole stack
            mv += self.cell_offset_mv
            if self.weak_cell and i == self.weak_cell[0]:
                mv -= self.weak_cell[1] * min(1.0, t / self.weak_ramp_s)
            out.append(int(mv))
        return out

    def pack_mv(self):
        return sum(self.cell_mvs())

    def pack_ma(self):
        t = time.monotonic() - self.t0

        if self.peak_a:
            # Winch duty: a long standby, then a pull that ramps up and back
            # down. One half-sine per cycle, so the needle sweeps the whole
            # dial rather than stepping between two values.
            phase = t % 20.0
            if phase < 11.0:
                amps = -3.0
            else:
                amps = -self.peak_a * math.sin(math.pi * (phase - 11.0) / 9.0)
            # Each pack takes a slightly different share of the load.
            return int(amps * 1000 * (0.94 + 0.03 * self.addr))

        base = 12000 if self.charging else -14000
        # Each pack carries a slightly different share of the bank current.
        return int(base * (0.9 + 0.1 * self.addr) + 900 * math.sin(t / 11))

    def temps(self):
        t = time.monotonic() - self.t0
        return [int(24 + self.addr + self.temp_offset_c + 2 * math.sin(t / 40 + i))
                for i in range(TEMPS)]

    def tick(self, dt):
        rate = 0.02 if self.charging else -0.015
        self.soc = max(3.0, min(100.0, self.soc + rate * dt))
        self.life = (self.life + 1) & 0xFF

    # -- protocol ----------------------------------------------------------

    def respond(self, cmd):
        """Return a list of 8-byte payloads for `cmd`, or [] if unsupported."""
        cells = self.cell_mvs()
        temps = self.temps()

        if cmd == CMD_SOC:
            return [u16(self.pack_mv() / 100) + b"\x00\x00" +
                    u16(30000 + self.pack_ma() / 100) + u16(self.soc * 10)]

        if cmd == CMD_CELL_MINMAX:
            hi = max(range(CELLS), key=lambda i: cells[i])
            lo = min(range(CELLS), key=lambda i: cells[i])
            return [u16(cells[hi]) + bytes([hi + 1]) +
                    u16(cells[lo]) + bytes([lo + 1]) + b"\x00\x00"]

        if cmd == CMD_TEMP_MINMAX:
            hi = max(range(TEMPS), key=lambda i: temps[i])
            lo = min(range(TEMPS), key=lambda i: temps[i])
            return [bytes([temps[hi] + 40, hi + 1, temps[lo] + 40, lo + 1,
                           0, 0, 0, 0])]

        if cmd == CMD_MOS:
            state = 1 if self.charging else 2
            remaining = int(280000 * self.soc / 100)   # 280 Ah pack
            return [bytes([state, 1, 1, self.life]) + struct.pack(">I", remaining)]

        if cmd == CMD_STATUS:
            return [bytes([CELLS, TEMPS, 1 if self.charging else 0,
                           0 if self.charging else 1, 0]) +
                    u16(self.cycles) + b"\x00"]

        if cmd == CMD_CELL_VOLTS:
            out = []
            for f in range((CELLS + 2) // 3):
                payload = bytes([self.frame_base + f])
                for slot in range(3):
                    idx = f * 3 + slot
                    payload += u16(cells[idx]) if idx < CELLS else b"\x00\x00"
                out.append(payload + b"\x00")
            return out

        if cmd == CMD_CELL_TEMPS:
            out = []
            for f in range((TEMPS + 6) // 7):
                payload = bytes([self.frame_base + f])
                for slot in range(7):
                    idx = f * 7 + slot
                    payload += bytes([temps[idx] + 40 if idx < TEMPS else 40])
                out.append(payload)
            return out

        if cmd == CMD_BALANCE:
            bits = 0
            if self.weak_cell:
                # The weak cell's neighbours get balanced down towards it.
                bits |= 1 << ((self.weak_cell[0] + 1) % CELLS)
            return [struct.pack("<Q", bits)]

        if cmd == CMD_FAULTS:
            return [bytes(self.alarm) + b"\x00"]

        return []


def apply_scenario(name, packs):
    for p in packs:
        p.charging = name == "charging"
    if name == "weak-cell":
        packs[1].weak_cell = (6, 120)      # pack 2, cell 7, drifting 120 mV low
    elif name == "alarm":
        packs[0].alarm[0] = 0x01           # cell overvoltage, byte 0 bit 0
    elif name == "faults":
        # One of each kind, so the footer has to prioritise and count.
        packs[0].alarm[0] = 0x02           # a BMS fault flag
        packs[0].cell_offset_mv = 45       # ~0.7 V above the others: mismatch
        packs[1].weak_cell = (6, 220)      # cell 7 badly adrift
        packs[1].weak_ramp_s = 2.0         # develops immediately
        packs[2].temp_offset_c = 34        # over-temperature
        packs[2].soc = 12.0                # and low
    elif name == "winch":
        for p in packs:
            p.peak_a = 200          # 3 packs -> about 600 A at the bank
    elif name not in ("normal", "offline", "charging"):
        sys.exit(f"unknown scenario: {name}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--channel", default="vcan0",
                    help="CAN channel (default vcan0)")
    ap.add_argument("-I", "--interface", default="socketcan",
                    help="python-can bus type (default socketcan)")
    ap.add_argument("-b", "--bitrate", type=int, default=250000,
                    help="bit rate; ignored by vcan (default 250000)")
    ap.add_argument("-s", "--scenario", default="normal")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="log every request and reply")
    ap.add_argument("--drop", type=float, default=0.0, metavar="P",
                    help="randomly drop this fraction of replies, to exercise "
                         "the multi-frame reassembly (0.0-1.0)")
    ap.add_argument("--frame-base", type=int, choices=(0, 1), default=1,
                    help="first frame number in a 0x95/0x96 burst. Daly's own "
                         "protocol document says 0; the firmware this project "
                         "has met says 1, which is the default. The decoder "
                         "works it out either way - this is how that gets "
                         "tested without a pack of each kind")
    args = ap.parse_args()

    packs = [Pack(1, 84.0, 1), Pack(2, 79.5, 2), Pack(3, 81.2, 3)]
    apply_scenario(args.scenario, packs)
    for p in packs:
        p.frame_base = args.frame_base
    by_addr = {p.addr: p for p in packs}

    # Only ever see host requests: priority 0x18 in the top bits, source 0x40 in
    # the low byte. This keeps our own replies out of the receive path on
    # interfaces that echo them back.
    #
    # The mask must stay inside the 29-bit identifier (0x1FFFFFFF). A mask that
    # reaches bit 31 collides with the CAN_EFF_FLAG that python-can and the
    # kernel keep there, and the filter then silently matches nothing.
    filters = [{
        "can_id": (PRIO << 24) | HOST_ADDR,
        "can_mask": 0x1F0000FF,
        "extended": True,
    }]

    try:
        bus = can.Bus(interface=args.interface, channel=args.channel,
                      bitrate=args.bitrate, can_filters=filters)
    except Exception as e:
        sys.exit(f"cannot open {args.interface}:{args.channel}: {e}\n"
                 f"  sudo modprobe vcan && "
                 f"sudo ip link add dev {args.channel} type vcan && "
                 f"sudo ip link set up {args.channel}")

    print(f"daly_sim: 3 packs on {args.interface}:{args.channel}, "
          f"scenario '{args.scenario}'"
          f"{', drop=%.0f%%' % (args.drop * 100) if args.drop else ''}")
    print(f"addresses 0x01 0x02 0x03, host 0x40, extended frames, "
          f"0x95/0x96 frames numbered from {args.frame_base}")

    rng = random.Random(0)
    start = last = time.monotonic()
    requests = replies = 0

    with bus:
        while True:
            now = time.monotonic()
            dt, last = now - last, now
            elapsed = now - start

            if args.scenario == "offline":
                by_addr[3].online = not (15 <= elapsed < 35)

            for p in packs:
                p.tick(dt)

            msg = bus.recv(timeout=0.2)
            if msg is None or not msg.is_extended_id:
                continue

            cmd = (msg.arbitration_id >> 16) & 0xFF
            dest = (msg.arbitration_id >> 8) & 0xFF
            pack = by_addr.get(dest)
            if pack is None or not pack.online:
                continue

            requests += 1
            resp_id = (PRIO << 24) | (cmd << 16) | (HOST_ADDR << 8) | pack.addr

            for payload in pack.respond(cmd):
                if args.drop and rng.random() < args.drop:
                    if args.verbose:
                        print(f"  drop  {resp_id:08X}")
                    continue
                bus.send(can.Message(arbitration_id=resp_id,
                                     data=payload.ljust(8, b"\x00"),
                                     is_extended_id=True))
                replies += 1
                if args.verbose:
                    print(f"  {msg.arbitration_id:08X} -> {resp_id:08X} "
                          f"{payload.hex(' ')}")

            if not args.verbose and requests % 100 == 0:
                socs = " ".join(f"#{p.addr}:{p.soc:.1f}%"
                                f"{'' if p.online else '(off)'}" for p in packs)
                print(f"\r{requests} req / {replies} rep   {socs}   ", end="",
                      flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
