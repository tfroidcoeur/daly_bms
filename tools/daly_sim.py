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
    alarm       pack 1 raises a level-1 cell voltage high flag - a BMS warning
    faults      several at once: a pack voltage mismatch, a BMS fault flag, a
                badly drifted cell, an over-temperature pack and a low SoC -
                enough to overflow the two footer lines and exercise the
                "+N more" count
    winch       winch duty: long standby broken by pulls that ramp the bank to
                about 600 A (200 A per pack), so the current gauge is driven
                over its full range
    sleepy      packs fall asleep after 30 s with no request, as Daly's BMS does,
                and wake after a few requests go unanswered (--sleep-after,
                --wake-requests). Pack 3 is the slowest to wake

Dashboard poller, to see what a dashboard that is not ours would get back:
    --dashboard-every S   send a 0x90 request to --dashboard-addr every S
                          seconds, from host 0x40, and count the replies
    --dashboard-file P    with the above, poll only while file P exists

Protocol variants, for the two places Daly's own document and the field layout
this project was built from disagree:
    --frame-base 0   number 0x95/0x96 frames from 0, as Daly's document says
                     (default 1, as the firmware we have met does)
    --no-cycles      leave 0x94 bytes 5-7 empty, as Daly's document says they are
                     (default: a cycle count in 5-6)
    --invert-current discharge above the 30000 bias, as dbus-serialbattery reads
                     Daly (default: below it)
Both are handled by the decoder rather than assumed, and these switches are how
that gets exercised without owning a pack of each kind.
"""

import argparse
import math
import os
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
        # Which way the current points on the wire; see --invert-current.
        self.current_sign = 1
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
        # Daly's own power saving; see --sleep-after. Only the sleepy scenario
        # turns it on, so every other scenario has packs that never sleep.
        self.sleep_enabled = False
        self.sleep_after = 30.0
        self.wake_requests = 3
        self.asleep = False
        self.last_request = self.t0
        self.wake_seen = 0

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

    def idle_check(self, now):
        """Fall asleep once no request has reached the pack for sleep_after s."""
        if (self.sleep_enabled and not self.asleep and
                now - self.last_request >= self.sleep_after):
            self.asleep = True
            self.wake_seen = 0
            print(f"\npack {self.addr} asleep", flush=True)

    def request_arrived(self, now):
        """Note a request addressed to this pack. True if it should answer."""
        self.last_request = now
        if not self.asleep:
            return True
        # A sleeping pack counts the requests that reach it. The one that wakes
        # it goes unanswered too, so the pack only answers from the next one.
        self.wake_seen += 1
        if self.wake_seen >= self.wake_requests:
            self.asleep = False
            print(f"\npack {self.addr} awake", flush=True)
        return False

    # -- protocol ----------------------------------------------------------

    def respond(self, cmd):
        """Return a list of 8-byte payloads for `cmd`, or [] if unsupported."""
        cells = self.cell_mvs()
        temps = self.temps()

        if cmd == CMD_SOC:
            return [u16(self.pack_mv() / 100) + b"\x00\x00" +
                    u16(30000 + self.current_sign * self.pack_ma() / 100) +
                    u16(self.soc * 10)]

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
        p.sleep_enabled = name == "sleepy"
    if name == "weak-cell":
        packs[1].weak_cell = (6, 120)      # pack 2, cell 7, drifting 120 mV low
    elif name == "alarm":
        packs[0].alarm[0] = 0x01           # cell voltage high, level 1 (a warning)
    elif name == "faults":
        # One of each kind, so the footer has to prioritise and count.
        packs[0].alarm[0] = 0x02           # cell voltage high, level 2 (tripped)
        packs[0].cell_offset_mv = 45       # ~0.7 V above the others: mismatch
        packs[1].weak_cell = (6, 220)      # cell 7 badly adrift
        packs[1].weak_ramp_s = 2.0         # develops immediately
        packs[2].temp_offset_c = 34        # over-temperature
        packs[2].soc = 12.0                # and low
    elif name == "winch":
        for p in packs:
            p.peak_a = 200          # 3 packs -> about 600 A at the bank
    elif name not in ("normal", "offline", "charging", "sleepy"):
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
    ap.add_argument("--invert-current", action="store_true",
                    help="put discharge above the 30000 current bias rather "
                         "than below it, as dbus-serialbattery reads Daly. "
                         "Pair with a -DBMS_INVERT_CURRENT=ON build")
    ap.add_argument("--no-cycles", action="store_true",
                    help="leave bytes 5-7 of 0x94 empty, as Daly's protocol "
                         "document says they are, instead of putting a cycle "
                         "count in 5-6")
    ap.add_argument("--sleep-after", type=float, default=30.0, metavar="S",
                    help="sleepy scenario: a pack with no request for S seconds "
                         "goes to sleep (default 30; the real BMS uses 3600)")
    ap.add_argument("--wake-requests", type=int, default=3, metavar="N",
                    help="sleepy scenario: a sleeping pack wakes after N requests "
                         "addressed to it, all unanswered (default 3). Pack 3 "
                         "takes 2N")
    ap.add_argument("--dashboard-every", type=float, default=0.0, metavar="S",
                    help="send a 0x90 request to --dashboard-addr every S seconds "
                         "from host 0x40 and count the replies (default 0: off)")
    ap.add_argument("--dashboard-addr", type=lambda s: int(s, 0), default=0x10,
                    help="BMS address the dashboard polls (default 0x10)")
    ap.add_argument("--dashboard-file", metavar="PATH",
                    help="with --dashboard-every, poll only while PATH exists, "
                         "so a test can switch the dashboard on and off by "
                         "touching or removing it")
    args = ap.parse_args()

    packs = [Pack(1, 84.0, 1), Pack(2, 79.5, 2), Pack(3, 81.2, 3)]
    apply_scenario(args.scenario, packs)
    for p in packs:
        p.frame_base = args.frame_base
        p.current_sign = -1 if args.invert_current else 1
        if args.no_cycles:
            p.cycles = 0
        p.sleep_after = args.sleep_after
        # Pack 3 is the slow waker: twice as many requests before it answers.
        p.wake_requests = args.wake_requests * (2 if p.addr == 3 else 1)
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

    # Dashboard replies come back from the BMS address to host 0x40, which the
    # filter above does not take (its low byte is the BMS address). Ask for that
    # one id exactly. Only when polling, so a plain run listens as it always did.
    dash_req_id = (PRIO << 24) | (CMD_SOC << 16) | (args.dashboard_addr << 8) | HOST_ADDR
    dash_resp_id = (PRIO << 24) | (CMD_SOC << 16) | (HOST_ADDR << 8) | args.dashboard_addr
    if args.dashboard_every > 0:
        filters.append({"can_id": dash_resp_id, "can_mask": 0x1FFFFFFF,
                        "extended": True})

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
          f"0x95/0x96 frames numbered from {args.frame_base}"
          f"{', 0x94 bytes 5-7 empty' if args.no_cycles else ''}"
          f"{', current inverted' if args.invert_current else ''}")
    if args.dashboard_every > 0:
        print(f"dashboard: 0x90 to 0x{args.dashboard_addr:02X} every "
              f"{args.dashboard_every:g} s"
              + (f" while {args.dashboard_file} exists"
                 if args.dashboard_file else ""))

    rng = random.Random(0)
    start = last = time.monotonic()
    requests = replies = 0
    dash_sent = dash_answered = 0
    next_poll = next_report = start

    with bus:
        while True:
            now = time.monotonic()
            dt, last = now - last, now
            elapsed = now - start

            if args.scenario == "offline":
                by_addr[3].online = not (15 <= elapsed < 35)

            for p in packs:
                p.tick(dt)
                p.idle_check(now)

            # Looked at on every pass, so touching or removing the file takes
            # effect within a fraction of a second.
            dash_on = args.dashboard_every > 0 and (
                args.dashboard_file is None or os.path.exists(args.dashboard_file))
            if not dash_on:
                next_report = now + 10      # first report 10 s after switching on
            else:
                if now >= next_poll:
                    bus.send(can.Message(arbitration_id=dash_req_id,
                                         data=bytes(8), is_extended_id=True))
                    dash_sent += 1
                    next_poll = now + args.dashboard_every
                    if args.verbose:
                        print(f"  dash  {dash_req_id:08X} ->")
                if now >= next_report:
                    print(f"\ndashboard: sent {dash_sent}, answered {dash_answered}",
                          flush=True)
                    next_report = now + 10

            msg = bus.recv(timeout=0.2)
            if msg is None or not msg.is_extended_id:
                continue

            if msg.arbitration_id == dash_resp_id and args.dashboard_every > 0:
                dash_answered += 1
                if args.verbose:
                    print(f"  dash  {msg.arbitration_id:08X} {msg.data.hex(' ')}")
                continue

            cmd = (msg.arbitration_id >> 16) & 0xFF
            dest = (msg.arbitration_id >> 8) & 0xFF
            pack = by_addr.get(dest)
            if pack is None or not pack.online:
                continue

            requests += 1
            resp_id = (PRIO << 24) | (cmd << 16) | (HOST_ADDR << 8) | pack.addr

            answer = pack.request_arrived(time.monotonic())
            if not answer and args.verbose:
                print(f"  {msg.arbitration_id:08X} asleep, no reply")

            for payload in (pack.respond(cmd) if answer else []):
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
