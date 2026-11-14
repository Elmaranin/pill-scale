#!/usr/bin/env python3
"""Control pillscale from a computer. Protocol: docs/api.md.

Examples:
    python tools/scale.py ports
    python tools/scale.py cmd info
    python tools/scale.py cmd tare
    python tools/scale.py cmd cal 100.00
    python tools/scale.py cmd read 50
    python tools/scale.py log --minutes 10 --note "empty platform"
    python tools/scale.py creep 1             # experiment B1 (creep), with prompts and the buzzer
    python tools/scale.py creep 1 --speed 10  # rehearsal: the same scenario 10x faster
"""
import argparse
import csv
import datetime as dt
import pathlib
import sys
import time

import serial
from serial.tools import list_ports


def find_port():
    """An ESP32-C3 with native USB shows up as Espressif USB JTAG/serial."""
    for p in list_ports.comports():
        if p.vid == 0x303A:  # Espressif
            return p.device
    return None


class Device:
    def __init__(self, port, timeout=2.0):
        self.ser = serial.Serial(port, 115200, timeout=timeout)
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def command(self, line, wait=70.0):
        """Send a command and wait for an OK/ERR line.
        wait is generous: read 600 at 10 SPS takes a minute."""
        self.ser.write((line.strip() + "\n").encode())
        deadline = time.time() + wait
        while time.time() < deadline:
            s = self.ser.readline().decode(errors="replace").strip()
            if s.startswith(("OK", "ERR")):
                return s
            if s.startswith("#"):
                print(s, file=sys.stderr)
            # D lines (the stream) are skipped here
        raise TimeoutError(f"no reply to {line!r}")

    def lines(self):
        while True:
            s = self.ser.readline().decode(errors="replace").strip()
            if s:
                yield s


def parse_kv(resp):
    """'OK g=1.234 n=10' -> {'g': '1.234', 'n': '10'}"""
    return dict(t.split("=", 1) for t in resp.split()[1:] if "=" in t)


def cmd_ports(_):
    for p in list_ports.comports():
        mark = "  <- ESP32" if p.vid == 0x303A else ""
        print(f"{p.device}\t{p.description}{mark}")


def cmd_send(args):
    dev = Device(args.port)
    print(dev.command(" ".join(args.words)))


def log_path(out, info, suffix=""):
    out_dir = pathlib.Path(out)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    return out_dir / f"{stamp}_{info.get('adc', 'adc')}{suffix}.csv"


def write_header(f, note, info, avg):
    # Metadata in comments: a month later the CSV is useless without it
    f.write(f"# note: {note}\n")
    f.write(f"# info: {' '.join(f'{k}={v}' for k, v in info.items())}\n")
    f.write(f"# stream: n={avg}\n")  # 1 — raw samples, >1 — means of n


def cmd_log(args):
    dev = Device(args.port)
    info = parse_kv(dev.command("info"))
    dev.command(f"stream on {args.avg}")
    path = log_path(args.out, info)

    end = time.time() + args.minutes * 60 if args.minutes else None
    n = 0
    try:
        with open(path, "w", newline="") as f:
            write_header(f, args.note, info, args.avg)
            w = csv.writer(f)
            w.writerow(["t_ms", "raw", "g"])
            for s in dev.lines():
                if s.startswith("D "):
                    _, t, raw, g = s.split()
                    w.writerow([t, raw, g])
                    n += 1
                    if n % 10 == 0:
                        print(f"\r{n:6d}  {float(g):9.3f} g", end="", flush=True)
                if end and time.time() > end:
                    break
    except KeyboardInterrupt:
        pass
    finally:
        dev.ser.write(b"stream off\n")
        print(f"\nwrote {n} samples to {path}")



# ---- Experiment B: creep or drift (docs/experiments.md, "План · Крип или дрейф") ----
#
# A scenario is a list of steps: (second from start, what to do, expected weight after).
# Weight None — the "box" of B2: its weight is measured at the start of the recording.
LIGHTER, LIGHTER_BAR = 108.60, 172.92
CREEP = {
    "1": dict(
        title="B1. Weight steps: creep versus drift",
        minutes=95,
        start=0.0,
        check="Platform empty",
        steps=[
            (10 * 60, "PLACE the lighter (108.60 g) in the centre", LIGHTER),
            (30 * 60, "REMOVE the lighter", 0.0),
            (50 * 60, "PLACE the lighter + bar (172.92 g) in the centre", LIGHTER_BAR),
            (70 * 60, "REMOVE everything", 0.0),
        ]),
    "2": dict(
        title="B2. Real box: 'after' error versus time without the box",
        minutes=100,
        start=None,
        check="The pillbox stands on the scale (ideally for 30 minutes already), take nothing from it",
        steps=[
            (30 * 60, "REMOVE the box for 10 seconds", 0.0),
            (30 * 60 + 10, "RETURN the box", None),
            (35 * 60, "REMOVE the box for 1 minute", 0.0),
            (36 * 60, "RETURN the box", None),
            (41 * 60, "REMOVE the box for 5 minutes", 0.0),
            (46 * 60, "RETURN the box", None),
            (56 * 60, "REMOVE the box for 30 minutes", 0.0),
            (86 * 60, "RETURN the box", None),
        ]),
}
CONDITIONS = [
    "window closed, no radiator or sun nearby, temperature steady for at least an hour",
    "the scale has been on for at least 30 minutes",
    "the laptop will not sleep: run via caffeinate -i, lid open",
    "if you have a thermometer, note the temperature now and at the end",
]


def beep(dev, ms, hz):
    """Beep without waiting for the reply: the OK goes into the stream and is skipped."""
    dev.ser.write(f"beep {ms} {hz}\n".encode())


def mmss(sec):
    sec = max(0, int(round(sec)))
    return f"{sec // 60:02d}:{sec % 60:02d}"


def cmd_creep(args):
    plan = dict(CREEP[args.part])
    if args.part == "1" and (args.w1 or args.w2):
        # Own weights instead of the lighter and the bar: weigh them on the "Scale" tab or with `read`
        w1, w2 = args.w1 or LIGHTER, args.w2 or LIGHTER_BAR
        plan["steps"] = [
            (10 * 60, f"PLACE weight 1 ({w1:.2f} g) in the centre", w1),
            (30 * 60, "REMOVE weight 1", 0.0),
            (50 * 60, f"PLACE weight 2 ({w2:.2f} g) in the centre", w2),
            (70 * 60, "REMOVE weight 2", 0.0),
        ]
        if w2 / w1 < 1.5:
            print(f"Warning: weight 2 is only {w2 / w1:.2f}x heavier than weight 1 — "
                  "creep proportionality will be harder to see, 1:2 is better")
    speed = args.speed
    print(f"\n{plan['title']}  ({plan['minutes']} min"
          + (f", rehearsal x{speed:g} — {plan['minutes'] / speed:.1f} min" if speed != 1 else "") + ")\n")
    print("Conditions:")
    for c in CONDITIONS:
        print(f"  • {c}")
    print(f"  • {plan['check']}")
    print("\nSteps:")
    for t, text, _ in plan["steps"]:
        print(f"  {mmss(t)}  {text}")
    print(f"  {mmss(plan['minutes'] * 60)}  end of recording")
    print("\n10 s before a step — a short beep and a prompt, at 3-2-1 s — ticks, at the step — a long beep.")
    if not args.yes:
        input("Enter — start, Ctrl+C — cancel… ")

    dev = Device(args.port)
    info = parse_kv(dev.command("info"))
    dev.command("stream on 1")
    beep(dev, 150, 2700)
    path = log_path(args.out, info, f"_creep{args.part}")

    steps = [dict(t=t, text=text, expect=e, warned=False, ticks=set(), done=False, checked=False)
             for t, text, e in plan["steps"]]
    # Steps closer than 15 s to each other (remove for 10 s — return): the second
    # gets no 10 s warning, only ticks — otherwise the beeps overlap.
    # No weight check 15 s after the first of them: the box is already back.
    for a, b in zip(steps, steps[1:]):
        if b["t"] - a["t"] < 15:
            b["warned"] = True
            a["checked"] = True
    end_s = plan["minutes"] * 60
    box_g = plan["start"]  # B2: box weight is measured at the start
    recent = []  # (scenario second, grams) — for step checks
    t_ms = "0"
    n = 0
    t0 = time.time()

    def mark(f, text):
        f.write(f"# mark t_ms={t_ms} at={mmss((time.time() - t0) * speed)} {text}\n")
        f.flush()

    def say(text):
        print(f"\r{' ' * 78}\r{text}", flush=True)

    try:
        with open(path, "w", newline="") as f:
            write_header(f, f"creep {plan['title']}; speed={speed:g}; {args.note}", info, 1)
            w = csv.writer(f)
            w.writerow(["t_ms", "raw", "g"])
            mark(f, "start")
            for s in dev.lines():
                el = (time.time() - t0) * speed  # scenario seconds since start
                if s.startswith("D "):
                    _, t_ms, raw, g = s.split()
                    w.writerow([t_ms, raw, g])
                    n += 1
                    recent.append((el, float(g)))
                    recent = recent[-200:]
                    if box_g is None and n == 30:  # ~3 s — box weight for the B2 checks
                        box_g = sum(x for _, x in recent) / len(recent)
                        say(f"weight on the scale at start: {box_g:.2f} g")
                    if n % 5 == 0:
                        nxt = next((x for x in steps if not x["done"]), None)
                        tail = f"next step in {mmss(nxt['t'] - el)}" if nxt else f"end in {mmss(end_s - el)}"
                        print(f"\r  {mmss(el)}   {float(g):9.3f} g   {tail}   ", end="", flush=True)
                for st in steps:
                    left = st["t"] - el
                    if not st["warned"] and left <= 10:
                        st["warned"] = True
                        say(f"\n>>> in 10 s: {st['text']}")
                        beep(dev, 120, 2700)
                    for k in (3, 2, 1):
                        if left <= k and k not in st["ticks"] and not st["done"]:
                            st["ticks"].add(k)
                            beep(dev, 60, 2700)
                    if not st["done"] and left <= 0:
                        st["done"] = True
                        say(f">>> NOW [{mmss(st['t'])}]: {st['text']}")
                        beep(dev, 700, 3200)
                        mark(f, st["text"])
                    after = [x for e, x in recent if e >= st["t"] + 8]  # the hand is gone
                    if st["done"] and not st["checked"] and el - st["t"] >= 15 and after:
                        st["checked"] = True
                        want = box_g if st["expect"] is None else st["expect"]
                        have = sum(after) / len(after)
                        if want is not None and abs(have - want) > 3.0:
                            say(f"!!! after the step the scale shows {have:.2f} g, expected ~{want:.2f} g — check")
                            for _ in range(3):
                                beep(dev, 200, 900)
                                time.sleep(0.35)
                            mark(f, f"check_fail have={have:.2f} want={want:.2f}")
                        else:
                            say(f"    ok: scale shows {have:.2f} g")
                if el >= end_s:
                    beep(dev, 150, 2700)
                    time.sleep(0.25)
                    beep(dev, 150, 3200)
                    break
            mark(f, "end")
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        dev.ser.write(b"stream off\n")
        print(f"\nwrote {n} samples to {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="serial port, by default the ESP32 is found automatically")
    sub = ap.add_subparsers(dest="action", required=True)

    sub.add_parser("ports", help="list serial ports").set_defaults(fn=cmd_ports)

    p = sub.add_parser("cmd", help="send a command")
    p.add_argument("words", nargs="+")
    p.set_defaults(fn=cmd_send)

    p = sub.add_parser("log", help="record the stream to CSV (Ctrl+C — stop)")
    p.add_argument("--minutes", type=float, default=None)
    p.add_argument("--note", default="", help="what was going on — always write it")
    p.add_argument("--out", default="data")
    p.add_argument("--avg", type=int, default=1,
                   help="average N samples per line (default 1 — raw samples)")
    p.set_defaults(fn=cmd_log)

    p = sub.add_parser("creep", help="experiment B (creep): recording with prompts and buzzer cues")
    p.add_argument("part", choices=sorted(CREEP), help="1 — weight steps, 2 — real box")
    p.add_argument("--speed", type=float, default=1.0,
                   help="speed up the scenario for a rehearsal (10 — 10x faster)")
    p.add_argument("--note", default="", help="conditions: temperature, anything else that matters")
    p.add_argument("--out", default="data")
    p.add_argument("--yes", action="store_true", help="do not wait for Enter before starting")
    p.add_argument("--w1", type=float, default=None,
                   help="B1: weight 1, g (default: the lighter, 108.60)")
    p.add_argument("--w2", type=float, default=None,
                   help="B1: weight 2, g (default: lighter + bar, 172.92); ideally twice weight 1")
    p.set_defaults(fn=cmd_creep)

    args = ap.parse_args()
    if args.action != "ports" and args.port is None:
        args.port = find_port()
        if args.port is None:
            sys.exit("ESP32 not found, pass --port (list: scale.py ports)")

    args.fn(args)


if __name__ == "__main__":
    main()
