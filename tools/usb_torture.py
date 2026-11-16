#!/usr/bin/env python3
"""
USB control path torture: opens and closes the console COM port over and over (each open/close makes Windows send
CDC control requests: GET/SET_LINE_CODING, SET_CONTROL_LINE_STATE) and asks 'cpu' each time, while a thread sends
500 MIDI notes per second to the drum and pitch channels. Reproduces the upstream USB driver hang (EP0 not ready for
the next SETUP -> the COM port open blocks until the module restarts).

  python tools/usb_torture.py [--minutes 5] [--port COM17] [--midi Punck]

Exit code 0 = every open answered, 1 = an open or a reply hung (the module's USB needs a re-plug / restart then).
"""
import argparse
import os
import random
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_test as lt

import mido
mido.set_backend("mido.backends.rtmidi")


def midi_load(port_hint, stop, counter):
    names = [n for n in mido.get_output_names() if port_hint.lower() in n.lower()]
    if not names:
        print("MIDI port not found", flush=True)
        return
    out = mido.open_output(names[0])
    rng = random.Random(1)
    drum = [36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47] + list(range(48, 88))
    try:
        t0 = time.perf_counter()
        n = 0
        while not stop.is_set():
            if rng.random() < 0.6:
                msg = mido.Message("note_on", channel=9, note=rng.choice(drum), velocity=rng.randint(1, 127))
            else:
                msg = mido.Message("note_on", channel=rng.randint(10, 15), note=rng.randint(0, 127), velocity=rng.randint(1, 127))
            try:
                out.send(msg)
            except Exception as e:
                print(f"MIDI send error: {e}", flush=True)
                time.sleep(0.5)
            n += 1
            counter[0] = n
            wait = t0 + n * 0.002 - time.perf_counter()               # 500 notes/s
            if wait > 0:
                time.sleep(wait)
    finally:
        out.close()


def open_and_ask(port, result, heavy=False):
    try:
        t0 = time.time()
        p = lt.Punck(port)
        result["open_s"] = time.time() - t0
        reply = ""
        for _ in range(3):                                            # the first command after an open can get lost
            p.s.reset_input_buffer()
            reply = p.cmd("cpu", wait=0.6)
            if "CPU avg" in reply:
                break
        if heavy and "CPU avg" in reply:
            p.cmd("mix", wait=0.0)                                    # big reply (30 lines), port closed while it streams
        result["reply"] = reply
        p.s.close()
        result["ok"] = "CPU avg" in reply
    except Exception as e:
        result["error"] = f"{type(e).__name__} {e}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=5.0)
    ap.add_argument("--port", default="COM17")
    ap.add_argument("--midi", default="Punck")
    ap.add_argument("--heavy", action="store_true", help="heaviest engines (rumble kick, modal snare, 909 hats, bass) and big console replies")
    a = ap.parse_args()
    restore = None
    if a.heavy:
        p = lt.Punck(a.port)
        for _ in range(3):
            st = p.cmd("status", wait=0.8)
            if "Kick" in st:
                break
        import re
        restore = [int(re.search(rf"{n}\s+engine (\d+)", st).group(1)) for n in ("Kick", "Snare", "Hats", "Toms")]
        for cmd in ("engine:1,5", "engine:2,5", "engine:3,3", "engine:6,2"):
            p.cmd(cmd, wait=0.3)
        p.s.close()
        print(f"heavy engines set (restoring {restore} at the end)", flush=True)

    stop, counter = threading.Event(), [0]
    th = threading.Thread(target=midi_load, args=(a.midi, stop, counter), daemon=True)
    th.start()
    t_end = time.time() + a.minutes * 60
    cycles, fails, slow, ep0 = 0, 0, 0, None
    try:
        while time.time() < t_end:
            res = {}
            w = threading.Thread(target=open_and_ask, args=(a.port, res, a.heavy), daemon=True)
            w.start()
            w.join(8.0)
            cycles += 1
            if w.is_alive():
                print(f"cycle {cycles}: HUNG (open or reply did not return within 8 s) after {counter[0]} MIDI notes", flush=True)
                fails += 1
                break
            if not res.get("ok"):
                fails += 1
                print(f"cycle {cycles}: FAILED {res.get('error', 'no reply')}", flush=True)
                time.sleep(2.0)
                continue
            if res["open_s"] > 1.0:
                slow += 1
            for line in res["reply"].splitlines():
                if "USB EP0 aborts" in line:
                    ep0 = line.split("USB EP0 aborts:")[-1].strip()
            if cycles % 20 == 0:
                first = res["reply"].splitlines()[0] if res["reply"] else ""
                print(f"cycle {cycles}: ok ({counter[0]} MIDI notes)  {first.strip()}  EP0 aborts {ep0}", flush=True)
    finally:
        stop.set()
        if restore and not fails:
            try:
                p = lt.Punck(a.port)
                for v, e in zip((1, 2, 3, 6), restore):
                    p.cmd(f"engine:{v},{e}", wait=0.3)
                p.s.close()
            except Exception as e:
                print(f"could not restore engines {restore}: {e}", flush=True)
    print(f"result: {cycles} open/close cycles, {fails} failed, {slow} slow opens (> 1 s), {counter[0]} MIDI notes, EP0 aborts {ep0}",
          flush=True)
    os._exit(1 if fails else 0)                                       # a hung worker thread must not keep the process alive


if __name__ == "__main__":
    main()
