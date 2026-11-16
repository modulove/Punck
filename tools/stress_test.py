#!/usr/bin/env python3
"""
Brute force stress test for the Punck firmware: every engine combination under hard trigger scenarios, for hours.

  python tools/stress_test.py [--hours 4] [--scenario-secs 3.5] [--seed 1] [--no-record] [--elf build/Punck.elf]

Per pass, all 150 engine combinations (kick 5 x snare 5 x hats 3 x toms/bass 2) run in shuffled order. For each
combination the engines are set over the console, then these scenarios play over USB MIDI (the module's own
trigger inputs can keep running from the modular on top - that is the intended worst case):

  unison     all seven voices on the same 16th at 175 BPM, full velocity
  rolls      every voice rolling at its own rate (20-60 hits/s), random velocity
  flams      bursts of 2-4 hits 1-4 ms apart on every voice (retrigger while ringing)
  chromatic  pitch channels with random notes over the full MIDI range 0-127, random velocity 1-127
  flood      MIDI as fast as USB takes it, random voices and notes
  switching  unison groove plus random engine switches and console hits every 150-400 ms
  sparse     single quiet hits: idle states, stuck voices, autosave in the background

After every scenario the console 'cpu' line is checked:
  UNDERRUN   audio interrupt missed a sample          CPU>95    a sample used more than 95 % of its time
  RESTART    uptime went back / fault count went up   BLOWUP    clipper input above +24 dB or nan / inf
  SILENT     output peak below -60 dBFS while hitting NOCONSOLE console stopped answering (SWD check follows)
Results: build/stress/<start>/results.csv (one row per scenario), events.log, and the audio interface recorded in
1-minute FLAC chunks (mono) for listening to flagged moments. Ctrl+C stops; the original engines are restored.
"""
import argparse
import datetime
import glob
import os
import random
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_test as lt

import mido
mido.set_backend("mido.backends.rtmidi")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINES = {"kick": 5, "snare": 5, "hats": 3, "toms": 2}
ENGINE_VOICE = {"kick": 1, "snare": 2, "hats": 3, "toms": 6}          # console engine:v,e voice numbers
DRUM_CH = 9                                                           # MIDI channel 10 (zero based)
SCENARIOS = ["unison", "rolls", "flams", "chromatic", "flood", "switching", "sparse"]


# ------------------------------------------------------------------------------------------------
# Module access
# ------------------------------------------------------------------------------------------------
class Module:
    """Console (COM port) with reconnect, plus the MIDI output."""

    def __init__(self, port, midi_hint, log):
        self.port, self.log = port, log
        self.con = None
        names = mido.get_output_names()
        match = [n for n in names if midi_hint.lower() in n.lower()]
        if not match:
            sys.exit(f"MIDI output with '{midi_hint}' not found in {names} (close the Tone Lab / Chrome if it holds the port)")
        self.midi = mido.open_output(match[0])
        self.lock = threading.Lock()

    def cmd(self, c, wait=0.5, tries=3):
        for _ in range(tries):
            try:
                if self.con is None:
                    self.con = lt.Punck(self.port)
                if wait > 0:
                    self.con.s.reset_input_buffer()                   # drop late replies of earlier commands
                return self.con.cmd(c, wait=wait)
            except Exception as e:                                    # USB gone (restart, enumeration): reconnect
                self.log(f"console error on '{c}': {type(e).__name__} {e}")
                try:
                    if self.con:
                        self.con.s.close()
                except Exception:
                    pass
                self.con = None
                time.sleep(1.0)
        return None

    def send(self, msg):
        with self.lock:
            self.midi.send(msg)

    def close(self):
        try:
            self.midi.close()
        except Exception:
            pass
        try:
            if self.con:
                self.con.s.close()
        except Exception:
            pass


def parse_cpu(text):
    if not text:
        return None
    m = re.search(r"CPU avg: ([\d.]+)%\s+max: ([\d.]+)%\s+underruns: (\d+)(?:\s+uptime: (\d+) s\s+crashes: (\d+))?", text)
    if not m:
        return None
    r = {"avg": float(m.group(1)), "max": float(m.group(2)), "under": int(m.group(3)),
         "uptime": int(m.group(4)) if m.group(4) else None, "crashes": int(m.group(5)) if m.group(5) else None}
    m2 = re.search(r"into clipper: ([-+\w.]+) dB\s+output peak: ([-+\w.]+) dBFS", text)
    r["clip_db"] = m2.group(1) if m2 else ""
    r["peak_db"] = m2.group(2) if m2 else ""
    m3 = re.search(r"budget \d+\):(.*)", text)
    r["cycles"] = m3.group(1).strip() if m3 else ""
    r["nan"] = bool(re.search(r"\bnan\b|\binf\b", text, re.I))
    return r


def to_float(s, default=None):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


def read_status(mod):
    t = mod.cmd("status", wait=0.6) or ""
    eng = {}
    for name, key in (("Kick", "kick"), ("Snare", "snare"), ("Hats", "hats"), ("Toms", "toms")):
        m = re.search(rf"{name}\s+engine (\d+)", t)
        if m:
            eng[key] = int(m.group(1))
    sw = re.search(r"Switch: (.+)", t)
    return eng, (sw.group(1).strip() if sw else "?")


def set_engines(mod, combo):
    for key, e in combo.items():
        mod.cmd(f"engine:{ENGINE_VOICE[key]},{e}", wait=0.15)


# ------------------------------------------------------------------------------------------------
# Scenario generators: lists of (time s, message or console command)
# ------------------------------------------------------------------------------------------------
def drum_note(rng, voice):
    return {"kick": 36, "snare": rng.choice((37, 38)), "hats": rng.choice((42, 43, 44)), "sampleA": rng.choice((45, 46, 47)),
            "sampleB": rng.randint(48, 87), "toms": rng.choice((40, 41)), "claps": 39}[voice]


VOICES = ["kick", "snare", "hats", "sampleA", "sampleB", "toms", "claps"]
PITCH_CH = [10, 11, 12, 13, 14, 15]                                   # channels 11-16 (zero based)


def note(ch, n, v):
    return mido.Message("note_on", channel=ch, note=max(0, min(127, n)), velocity=max(1, min(127, v)))


def gen_events(name, secs, rng):
    ev = []
    if name in ("unison", "switching"):
        step = 60.0 / 175 / 4
        t = 0.0
        while t < secs:
            for v in VOICES:
                ev.append((t, note(DRUM_CH, drum_note(rng, v), 127)))
            t += step
        if name == "switching":
            t = 0.2
            while t < secs:
                key = rng.choice(list(ENGINES))
                ev.append((t, f"engine:{ENGINE_VOICE[key]},{rng.randint(1, ENGINES[key])}"))
                ev.append((t + 0.05, "play:" + ";".join(f"{rng.randint(1, 7)},{rng.randint(1, 127)},{rng.randint(0, 3)}"
                                                       for _ in range(rng.randint(1, 4)))))
                t += rng.uniform(0.15, 0.4)
    elif name == "rolls":
        for v in VOICES:
            rate = rng.uniform(20, 60)
            t = rng.uniform(0, 0.05)
            while t < secs:
                ev.append((t, note(DRUM_CH, drum_note(rng, v), rng.randint(1, 127))))
                t += 1.0 / rate
    elif name == "flams":
        t = 0.0
        while t < secs:
            for v in VOICES:
                tt = t + rng.uniform(0, 0.01)
                for _ in range(rng.randint(2, 4)):
                    ev.append((tt, note(DRUM_CH, drum_note(rng, v), rng.randint(40, 127))))
                    tt += rng.uniform(0.001, 0.004)
            t += 0.1
    elif name == "chromatic":
        for ch in PITCH_CH:
            t = rng.uniform(0, 0.03)
            while t < secs:
                n = rng.choice((0, 127)) if rng.random() < 0.1 else rng.randint(0, 127)
                ev.append((t, note(ch, n, rng.randint(1, 127))))
                t += rng.uniform(0.015, 0.04)
    elif name == "flood":
        t = 0.0
        while t < secs:
            if rng.random() < 0.6:
                ev.append((t, note(DRUM_CH, drum_note(rng, rng.choice(VOICES)), rng.randint(1, 127))))
            else:
                ev.append((t, note(rng.choice(PITCH_CH), rng.randint(0, 127), rng.randint(1, 127))))
            t += 0.0007
    elif name == "sparse":
        t = 0.3
        while t < secs:
            ev.append((t, note(DRUM_CH, drum_note(rng, rng.choice(VOICES)), rng.randint(1, 60))))
            t += rng.uniform(0.4, 0.9)
    ev.sort(key=lambda e: e[0])
    return ev


def play_events(mod, events):
    """Sends the events on time (busy waits for the last 2 ms: Windows sleep is too coarse for flams)."""
    t0 = time.perf_counter()
    late = 0.0
    for t, e in events:
        while True:
            d = t - (time.perf_counter() - t0)
            if d <= 0:
                break
            if d > 0.003:
                time.sleep(d - 0.002)
        late = max(late, (time.perf_counter() - t0) - t)
        if isinstance(e, str):
            mod.cmd(e, wait=0.0, tries=1)                             # console command (fire and forget)
        else:
            try:
                mod.send(e)
            except Exception as ex:
                return f"MIDI error {ex}", late
    return "", late


# ------------------------------------------------------------------------------------------------
# SWD check when the console is gone (only then: SWD polling during play is avoided otherwise)
# ------------------------------------------------------------------------------------------------
def swd_check(elf, log):
    try:
        import swd
        nm = glob.glob(r"C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\*gnu-tools-for-stm32*\tools\bin\arm-none-eabi-nm.exe")
        if not nm:
            return "no nm"
        syms = {}
        for line in subprocess.run([nm[0], elf], capture_output=True, text=True).stdout.splitlines():
            p = line.split()
            if len(p) == 3 and p[2] in ("SysTickVal", "i2sUnderrun"):
                syms[p[2]] = int(p[0], 16)
        a = syms.get("SysTickVal")
        if a is None:
            return "SysTickVal not in ELF"
        rc, out = swd.openocd("init", f"stm32h743.ap0 read_memory 0x{a:08x} 32 1", "sleep 500",
                              f"stm32h743.ap0 read_memory 0x{a:08x} 32 1", timeout=60)
        vals = [int(l.split()[0], 16) for l in out.splitlines() if re.match(r"^\s*0x[0-9a-f]+\s*$", l)]
        if len(vals) == 2:
            return f"SWD: uptime {vals[0] / 1000:.1f} s -> {vals[1] / 1000:.1f} s ({'firmware running, USB down' if vals[1] > vals[0] else 'FIRMWARE STOPPED'})"
        return "SWD read failed: " + out[-200:].replace("\n", " ")
    except Exception as e:
        return f"SWD check error {e}"


# ------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=4.0)
    ap.add_argument("--scenario-secs", type=float, default=3.5)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--port", default="COM17")
    ap.add_argument("--midi", default="Punck")
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "Punck.elf"))
    ap.add_argument("--no-record", action="store_true")
    ap.add_argument("--combos", type=int, default=0, help="limit combinations per pass (0 = all 150)")
    a = ap.parse_args()

    out = os.path.join(ROOT, "build", "stress", datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(out, exist_ok=True)
    evlog = open(os.path.join(out, "events.log"), "w", buffering=1)

    def log(msg):
        line = f"{datetime.datetime.now():%H:%M:%S} {msg}"
        print(line, flush=True)
        evlog.write(line + "\n")

    mod = Module(a.port, a.midi, log)
    original, switch = read_status(mod)
    if len(original) < 4:
        sys.exit("could not read the engines from 'status' (console?)")
    if "learn" in switch.lower():
        sys.exit("mode switch is on MIDI learn: notes would change the MIDI map. Move it to Play or Sequence.")
    log(f"start: engines {original}, switch {switch}, results in {out}")
    log("crash record: " + (mod.cmd("crash", wait=0.5) or "?").strip().replace("\r\n", " | "))

    rec = None
    if not a.no_record:
        dev = [d for d in lt.list_devices() if "USB Audio CODEC" in d]
        if dev:
            rec = subprocess.Popen([lt.FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "dshow", "-audio_buffer_size", "50",
                                    "-sample_rate", "48000", "-channels", "2", "-i", "audio=" + dev[0], "-ac", "1",
                                    "-t", f"{a.hours * 3600 + 120:.0f}", "-c:a", "flac", "-f", "segment", "-segment_time", "60",
                                    "-strftime", "1", os.path.join(out, "seg_%H%M%S.flac")],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "ffmpeg.log"), "w"))
        else:
            log("audio interface not found: not recording")

    csv = open(os.path.join(out, "results.csv"), "w", buffering=1)
    csv.write("time,pass,kick,snare,hats,toms,scenario,events,late_ms,cpu_avg,cpu_max,underruns,uptime_s,crashes,clip_db,peak_db,flags,cycles\n")

    combos = [{"kick": k, "snare": s, "hats": h, "toms": t} for k in range(1, 6) for s in range(1, 6)
              for h in range(1, 4) for t in range(1, 3)]
    rng = random.Random(a.seed)
    t_end = time.time() + a.hours * 3600
    last = parse_cpu(mod.cmd("cpu", wait=0.6))
    totals = {"scenarios": 0, "flags": {}}
    worst = []
    dead_since = None
    pass_no = 0
    try:
        while time.time() < t_end:
            pass_no += 1
            order = combos[:]
            rng.shuffle(order)
            if a.combos:
                order = order[:a.combos]
            log(f"pass {pass_no}: {len(order)} engine combinations")
            for ci, combo in enumerate(order):
                if time.time() >= t_end:
                    break
                set_engines(mod, combo)
                time.sleep(0.8)                                          # engine preview hits and LED blinks
                mod.cmd("cpu", wait=0.5)                                 # reset the maxima
                combo_flags = []
                for sc in SCENARIOS:
                    events = gen_events(sc, a.scenario_secs, rng)
                    err, late = play_events(mod, events)
                    if sc == "switching":
                        set_engines(mod, combo)                          # back to this combination
                    time.sleep(0.25)                                     # let the tails ring into the meter
                    text = mod.cmd("cpu", wait=0.6)
                    r = parse_cpu(text)
                    flags = []
                    if err:
                        flags.append("MIDIERR")
                    if r is None:
                        flags.append("NOCONSOLE")
                        dead_since = dead_since or time.time()
                        if time.time() - dead_since > 30:
                            log(swd_check(a.elf, log))
                            dead_since = time.time()
                    else:
                        dead_since = None
                        if last and r["uptime"] is not None and last["uptime"] is not None and r["uptime"] < last["uptime"]:
                            flags.append("RESTART")
                        if last and r["crashes"] is not None and last["crashes"] is not None and r["crashes"] > last["crashes"]:
                            flags.append("FAULT")
                        if last and r["under"] > last["under"] and "RESTART" not in flags:
                            flags.append(f"UNDERRUN+{r['under'] - last['under']}")
                        if r["max"] >= 95.0:
                            flags.append("CPU>95")
                        clip = to_float(r["clip_db"])
                        if r["nan"] or (clip is not None and clip > 24.0):
                            flags.append("BLOWUP")
                        peak = to_float(r["peak_db"])
                        if sc != "sparse" and peak is not None and peak < -60.0:
                            flags.append("SILENT")
                        if "RESTART" in flags or "FAULT" in flags:
                            log("crash record: " + (mod.cmd("crash", wait=0.5) or "?").strip().replace("\r\n", " | "))
                        last = r
                    totals["scenarios"] += 1
                    for f in flags:
                        k = f.split("+")[0]
                        totals["flags"][k] = totals["flags"].get(k, 0) + 1
                    rr = r or {}
                    csv.write(f"{datetime.datetime.now():%H:%M:%S},{pass_no},{combo['kick']},{combo['snare']},{combo['hats']},{combo['toms']},"
                              f"{sc},{len(events)},{late * 1000:.1f},{rr.get('avg', '')},{rr.get('max', '')},{rr.get('under', '')},"
                              f"{rr.get('uptime', '')},{rr.get('crashes', '')},{rr.get('clip_db', '')},{rr.get('peak_db', '')},"
                              f"{' '.join(flags)},{rr.get('cycles', '').replace(',', ' ')}\n")
                    if r:
                        worst.append((r["max"], combo, sc))
                    if flags:
                        combo_flags.append(f"{sc}:{'/'.join(flags)}")
                        log(f"  FLAG {combo} {sc}: {' '.join(flags)}  cpu {rr.get('avg')}/{rr.get('max')}%  {rr.get('cycles', '')}")
                cm = max((w[0] for w in worst[-len(SCENARIOS):]), default=0)
                log(f"[{pass_no}.{ci + 1:03d}] k{combo['kick']} s{combo['snare']} h{combo['hats']} t{combo['toms']}  peak cpu {cm:.0f}%"
                    + (f"  flags: {', '.join(combo_flags)}" if combo_flags else "  ok"))
    except KeyboardInterrupt:
        log("stopped by user")
    finally:
        try:
            set_engines(mod, original)
            log(f"engines restored to {original}")
        except Exception as e:
            log(f"could not restore engines: {e}")
        worst.sort(key=lambda w: -w[0])
        log(f"summary: {totals['scenarios']} scenarios, flags {totals['flags'] or 'none'}")
        for m, combo, sc in worst[:10]:
            log(f"  highest CPU {m:.1f}%  {combo} {sc}")
        if rec:
            rec.terminate()
        mod.close()


if __name__ == "__main__":
    main()
