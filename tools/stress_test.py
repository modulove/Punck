#!/usr/bin/env python3
"""
Brute force stress test for the Punck firmware: every engine combination with randomised Tone Lab parameters under
hard trigger scenarios, for hours.

  python tools/stress_test.py [--hours 4] [--scenario-secs 3.5] [--seed 1] [--no-random] [--no-record]
  python tools/stress_test.py --restore build/stress/<run>      put the settings saved at the start of a run back

Per pass, all 150 engine combinations (kick 5 x snare 5 x hats 3 x toms/bass 2) run in shuffled order. For each
combination the engines are set over the console and the parameters are randomised (below), then these scenarios
play over USB MIDI (no MIDI cable needed; the module's own trigger inputs can keep running from the modular on top -
that is the intended worst case):

  unison     all seven voices on the same 16th at 175 BPM, full velocity
  rolls      every voice rolling at its own rate (20-60 hits/s), random velocity
  flams      bursts of 2-4 hits 1-4 ms apart on every voice (retrigger while ringing)
  chromatic  pitch channels with random notes over the full MIDI range 0-127, random velocity 1-127
  flood      500 MIDI notes per second, random voices and notes
  switching  unison groove plus random engine switches and console hits every 150-400 ms
  sparse     single quiet hits: idle states, stuck voices, autosave in the background

Parameter randomisation (Tone Lab ranges from WebEditor/tonelab.html, so only values the Tone Lab can set):
  25 % of the combinations keep the settings from the start of the run, 60 % get random values for about half of
  the controls that matter for the active engines (voices, reverb, master bus, key & scale), 15 % get extremes
  (each chosen control at its minimum or maximum). Every set is logged in params.jsonl (id in results.csv).
  The settings from the start are saved in snapshot.json and restored at the end (also after Ctrl+C).

After every scenario the console 'cpu' line is checked:
  UNDERRUN   audio interrupt missed a sample          CPU>95    a sample used more than 95 % of its time
  RESTART    uptime went back / fault count went up   NAN       nan / inf in the master bus meter
  RUNAWAY    a second after the notes the level is still 12 dB above the quiet level and not falling over 2.5 s
  TAIL       loud after a second but falling (long rumble / hat / reverb tails, info)
  HOT        clipper input above +24 dB while hitting (info: maximum gain settings reach it legitimately)
  SILENT     output peak below -60 dBFS while hitting NOCONSOLE console stopped answering (SWD check follows)
  RATE       audio interrupt not at 48000/s (+-2 %)   I2SRESTART the firmware's rate watchdog restarted the I2S
Results in build/stress/<start>/: results.csv, events.log, params.jsonl, snapshot.json and the audio interface
recorded in 1-minute FLAC chunks (mono) for listening to flagged moments.
"""
import argparse
import datetime
import glob
import json
import math
import os
import random
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_test as lt
import punck_backup as pb

import mido

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINES = {"kick": 5, "snare": 5, "hats": 3, "toms": 2}
ENGINE_VOICE = {"kick": 1, "snare": 2, "hats": 3, "toms": 6}          # console engine:v,e voice numbers
DRUM_CH = 9                                                           # MIDI channel 10 (zero based)
SCENARIOS = ["unison", "rolls", "flams", "chromatic", "flood", "switching", "sparse"]
VOICE_STRUCTS = {"Kick": 0, "Snare": 1, "HiHat": 2, "Toms": 5, "Claps": 6}   # SysEx voice numbers
STRUCT_ENGINE = {"Kick": "kick", "Snare": "snare", "HiHat": "hats", "Toms": "toms"}
INT_UNITS = {"int", "key", "scale", "tmode", "fx", "chans"}
INT_FIELDS = {"compOn", "compKick", "clipMode", "fxMode", "modalModes", "diffuserCount", "mixerBaseDelay"}


# ------------------------------------------------------------------------------------------------
# Tone Lab model: struct layouts and control ranges, read from the web page so they stay in sync
# ------------------------------------------------------------------------------------------------
class ToneLab:
    SR, LN2, LN1000 = 48000.0, math.log(2), math.log(0.001)

    def __init__(self, path):
        html = open(path, encoding="utf-8").read()
        block = html[html.index("const STRUCTS"):]
        block = block[:block.index("\n};") + 3]
        self.layout = {}                                          # struct -> {field: (offset, count, type)}
        self.size = {}
        self.order = {}                                           # struct -> [(field, count, type)]
        for m in re.finditer(r"(\w+):\s*\{\s*voice:\s*(-?\d+),\s*fields:\s*\[(.*?)\]\s*\}", block, re.S):
            fields = [(n, int(c), t or "f") for n, c, t in re.findall(r'\["(\w+)",(\d+)(?:,"(\w)")?\]', m.group(3))]
            pos, lay = 0, {}
            for n, c, t in fields:
                lay[n] = (pos, c, t)
                pos += c if t == "b" else 4 * c
            self.layout[m.group(1)], self.size[m.group(1)], self.order[m.group(1)] = lay, pos, fields
        ui = html[html.index("const UI = {"):]
        ui = ui[:ui.index("\n};") + 3]
        self.controls = {}                                        # struct -> [control dict]
        cur = None
        for line in ui.splitlines():
            m = re.match(r"\s*(\w+):\s*\{\s*title:", line)
            if m:
                cur = "Mix" if m.group(1) == "Key" else m.group(1)
            for c in re.finditer(r'\["([^"]*)",\s*"(\w+)",\s*(\d+),\s*"(\w+)",\s*"([^"]*)",\s*(-?[\d.eE+-]+),\s*(-?[\d.eE+-]+),'
                                 r'\s*"(\w+)"(?:,\s*\[([\d,\s]*)\])?\]', line):
                field = c.group(2)
                if field.startswith("_") or cur is None or field not in self.layout.get(cur, {}):
                    continue                                      # macro controls (several fields) are skipped
                eng = [int(x) for x in c.group(9).split(",")] if c.group(9) else None
                self.controls.setdefault(cur, []).append(dict(label=c.group(1), field=field, idx=int(c.group(3)), conv=c.group(4),
                                                              unit=c.group(5), lo=float(c.group(6)), hi=float(c.group(7)),
                                                              scale=c.group(8), engines=eng))

    def to_raw(self, conv, v):
        sr = self.SR
        if conv == "omega":
            return v / (sr / 2)
        if conv == "decay":
            return math.exp(self.LN1000 / (sr * v / 1000))
        if conv == "drop":
            return math.exp(-v * self.LN2 / sr)
        if conv == "rise":
            return math.exp(v * self.LN2 / sr)
        if conv == "attack":
            return 1 / (sr / 1000 * v)
        if conv == "pct":
            return v / 100
        if conv == "chans":
            return [0, 4, 8][int(round(v))]
        return v

    def pick(self, rng, c, extreme):
        lo, hi = c["lo"], c["hi"]
        if extreme:
            v = rng.choice((lo, hi))
        elif c["scale"] == "log" and lo > 0 and hi > 0:
            v = math.exp(rng.uniform(math.log(lo), math.log(hi)))
        else:
            v = rng.uniform(lo, hi)
        if c["unit"] in INT_UNITS or c["field"] in INT_FIELDS:
            v = round(v)
        return v

    def offset_of(self, struct_name, field, idx):
        off, count, t = self.layout[struct_name][field]
        return off + idx * (1 if t == "b" else 4) if idx < count else None

    def mix_index(self, field, idx):
        i = 0
        for n, c, t in self.order["Mix"]:
            if n == field:
                return i + idx
            i += c
        return None


# ------------------------------------------------------------------------------------------------
# Module access: console (COM port, reconnecting) and USB MIDI (notes + SysEx through punck_backup)
# ------------------------------------------------------------------------------------------------
class Module:
    def __init__(self, port, midi_hint, log):
        self.port, self.log, self.con, self.midi_hint = port, log, None, midi_hint
        self.sx = pb.Punck(midi_hint)                             # opens the MIDI in and out port (exclusive on Windows)

    def cmd(self, c, wait=0.5, tries=3):
        """Console command. A write timeout keeps the port open (reopening sends control requests, which a busy
        module can answer too late); only a vanished port (module restart) is reopened."""
        import serial
        for _ in range(tries):
            try:
                if self.con is None:
                    self.con = lt.Punck(self.port)
                if wait > 0:
                    self.con.s.reset_input_buffer()                   # drop late replies of earlier commands
                return self.con.cmd(c, wait=wait)
            except serial.SerialTimeoutException as e:                # module busy: wait, keep the handle
                self.log(f"console write timeout on '{c}'")
                time.sleep(1.0)
            except Exception as e:                                    # port gone (restart, re-enumeration): reopen
                self.log(f"console error on '{c}': {type(e).__name__} {e}")
                try:
                    if self.con:
                        self.con.s.close()
                except Exception:
                    pass
                self.con = None
                time.sleep(2.0)
        return None

    def reopen_midi(self):
        """After a module restart the USB MIDI device is new: the old port handles are dead."""
        try:
            self.sx.close()
        except Exception:
            pass
        for _ in range(30):
            try:
                self.sx = pb.Punck(self.midi_hint)
                return True
            except BaseException:                                     # find_port raises SystemExit while the port is missing
                time.sleep(1.0)
        return False

    def midi_call(self, fn):
        try:
            return fn()
        except Exception as e:
            self.log(f"MIDI error: {type(e).__name__} {e}; reopening the port")
            self.reopen_midi()
            return fn()

    def send(self, msg):
        self.midi_call(lambda: self.sx.out.send(msg))

    def read_struct(self, name):
        return bytes(self.midi_call(lambda: self.sx.reverb_config() if name == "Reverb" else self.sx.voice_config(VOICE_STRUCTS[name])))

    def write_struct(self, name, raw):
        if name == "Reverb":
            self.midi_call(lambda: self.sx.send([pb.CMD["SetReverbConfig"]] + pb.nibble_encode(raw)))
        else:
            self.midi_call(lambda: self.sx.send([pb.CMD["SetVoiceConfig"], VOICE_STRUCTS[name]] + pb.nibble_encode(raw)))

    def read_mix(self, expected=29):
        vals = {}
        for _ in range(4):
            t = self.cmd("mix", wait=1.0) or ""
            vals = {}
            for m in re.finditer(r"^\s*(\d+) .*?(-?[\d.]+)\s*$", t, re.M):
                vals[int(m.group(1))] = float(m.group(2))
            if len(vals) >= expected:
                break
            time.sleep(0.5)
        return vals

    def close(self):
        for f in (lambda: self.sx.close(), lambda: self.con and self.con.s.close()):
            try:
                f()
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
    m4 = re.search(r"Audio rate: (\d+)/s \(48000\)\s+I2S restarts: (\d+)", text)            # firmware r13+
    r["rate"], r["restarts"] = (int(m4.group(1)), int(m4.group(2))) if m4 else (None, None)
    m5 = re.search(r"USB EP0 aborts: (\d+)", text)                                             # firmware r15+
    r["ep0"] = int(m5.group(1)) if m5 else None
    return r


def to_float(s, default=None):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


def read_status(mod):
    eng, sw = {}, None
    for _ in range(4):                                                # the first command after opening the port can get lost
        t = mod.cmd("status", wait=0.8) or ""
        eng = {}
        for name, key in (("Kick", "kick"), ("Snare", "snare"), ("Hats", "hats"), ("Toms", "toms")):
            m = re.search(rf"{name}\s+engine (\d+)", t)
            if m:
                eng[key] = int(m.group(1))
        sw = re.search(r"Switch: (.+)", t)
        if len(eng) == 4:
            break
        time.sleep(0.5)
    return eng, (sw.group(1).strip() if sw else "?")


def set_engines(mod, combo):
    for key, e in combo.items():
        mod.cmd(f"engine:{ENGINE_VOICE[key]},{e}", wait=0.15)


# ------------------------------------------------------------------------------------------------
# Snapshot / restore of all settings, and the randomiser
# ------------------------------------------------------------------------------------------------
def take_snapshot(mod, tl, path):
    snap = {"structs": {}, "mix": {}}
    for name in list(VOICE_STRUCTS) + ["Reverb"]:
        raw = mod.read_struct(name)
        snap["structs"][name] = raw.hex()
        if len(raw) != tl.size.get(name, -1):
            snap.setdefault("size_mismatch", {})[name] = [len(raw), tl.size.get(name)]
    snap["mix"] = mod.read_mix()
    json.dump(snap, open(path, "w"), indent=1)
    return snap


def set_mix(mod, values):
    """Writes only the mix values that differ from the module's: every 'mix:i,v' makes it print its whole table,
    and dozens of those in a row kept the console (and USB) busy for many seconds."""
    cur = mod.read_mix()
    written = 0
    for i, v in sorted((int(k), float(x)) for k, x in values.items()):
        if i in cur and abs(cur[i] - v) < 0.006:
            continue
        mod.cmd(f"mix:{i},{v:.5f}", wait=0.4)
        written += 1
    return written


def restore_snapshot(mod, snap, log):
    for name, hexraw in snap["structs"].items():
        mod.write_struct(name, bytes.fromhex(hexraw))             # also restores the engine stored in each config
        time.sleep(0.1)
    set_mix(mod, snap["mix"])
    log(f"settings restored from snapshot ({len(snap['structs'])} configs, {len(snap['mix'])} mix values)")


def randomise(mod, tl, rng, combo, snap, mismatch):
    """Returns (mode, {struct: {field[idx]: value}}). Writes the new values to the module."""
    r = rng.random()
    mode = "snapshot" if r < 0.25 else ("random" if r < 0.85 else "extreme")
    chosen = {}
    if mode == "snapshot":
        for name in list(VOICE_STRUCTS) + ["Reverb"]:
            raw = bytearray(bytes.fromhex(snap["structs"][name]))
            if name in STRUCT_ENGINE and "engine" in tl.layout[name]:
                off = tl.layout[name]["engine"][0]
                struct.pack_into("<f", raw, off, float(combo[STRUCT_ENGINE[name]] - 1))    # keep this combination's engine
            mod.write_struct(name, bytes(raw))
            time.sleep(0.05)
        set_mix(mod, snap["mix"])
        return mode, chosen
    extreme = mode == "extreme"
    for name in list(VOICE_STRUCTS) + ["Reverb"]:
        if name in mismatch:
            continue                                              # layout differs from the Tone Lab: do not touch
        raw = bytearray(mod.read_struct(name))
        eng = combo[STRUCT_ENGINE[name]] - 1 if name in STRUCT_ENGINE else None
        vals = {}
        for c in tl.controls.get(name, []):
            if c["engines"] is not None and eng is not None and eng not in c["engines"]:
                continue                                          # control of an inactive engine
            if rng.random() > 0.5:
                continue
            off = tl.offset_of(name, c["field"], c["idx"])
            if off is None or off + 4 > len(raw):
                continue
            v = tl.pick(rng, c, extreme)
            struct.pack_into("<f", raw, off, float(tl.to_raw(c["conv"], v)))
            vals[f"{c['field']}[{c['idx']}]"] = v
        if vals:
            mod.write_struct(name, bytes(raw))
            chosen[name] = vals
            time.sleep(0.05)
    mixvals, mixraw = {}, {}
    for c in tl.controls.get("Mix", []):
        if rng.random() > 0.4:
            continue
        i = tl.mix_index(c["field"], c["idx"])
        if i is None:
            continue
        v = tl.pick(rng, c, extreme)
        mixraw[i] = tl.to_raw(c["conv"], v)
        mixvals[f"{c['field']}[{c['idx']}]"] = v
    if mixraw:
        set_mix(mod, mixraw)
    if mixvals:
        chosen["Mix"] = mixvals
    return mode, chosen


# ------------------------------------------------------------------------------------------------
# Scenario generators: lists of (time s, message or console command)
# ------------------------------------------------------------------------------------------------
VOICES = ["kick", "snare", "hats", "sampleA", "sampleB", "toms", "claps"]
PITCH_CH = [10, 11, 12, 13, 14, 15]                                   # channels 11-16 (zero based)


def drum_note(rng, voice):
    return {"kick": 36, "snare": rng.choice((37, 38)), "hats": rng.choice((42, 43, 44)), "sampleA": rng.choice((45, 46, 47)),
            "sampleB": rng.randint(48, 87), "toms": rng.choice((40, 41)), "claps": 39}[voice]


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
            t += 0.002                                                # 500 notes/s: far above any controller
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
def swd_check(elf):
    try:
        import swd
        nm = glob.glob(r"C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\*gnu-tools-for-stm32*\tools\bin\arm-none-eabi-nm.exe")
        if not nm:
            return "no nm"
        syms = {}
        for line in subprocess.run([nm[0], elf], capture_output=True, text=True).stdout.splitlines():
            p = line.split()
            if len(p) == 3 and p[2] in ("SysTickVal",):
                syms[p[2]] = int(p[0], 16)
        a = syms.get("SysTickVal")
        if a is None:
            return "SysTickVal not in ELF"
        rc, out = swd.openocd("init", f"stm32h743.ap0 read_memory 0x{a:08x} 32 1", "sleep 500",
                              f"stm32h743.ap0 read_memory 0x{a:08x} 32 1", timeout=60)
        vals = [int(l.split()[0], 16) for l in out.splitlines() if re.match(r"^\s*0x[0-9a-f]+\s*$", l)]
        if len(vals) == 2:
            state = "firmware running, USB down" if vals[1] > vals[0] else "FIRMWARE STOPPED"
            return f"SWD: uptime {vals[0] / 1000:.1f} s -> {vals[1] / 1000:.1f} s ({state})"
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
    ap.add_argument("--no-random", action="store_true", help="keep the current parameters (engines still change)")
    ap.add_argument("--combos", type=int, default=0, help="limit combinations per pass (0 = all 150)")
    ap.add_argument("--restore", metavar="RUN_FOLDER", help="restore the settings saved at the start of that run, then exit")
    a = ap.parse_args()

    if a.restore:
        mod = Module(a.port, a.midi, print)
        try:
            restore_snapshot(mod, json.load(open(os.path.join(a.restore, "snapshot.json"))), print)
        finally:
            mod.close()
        return

    out = os.path.join(ROOT, "build", "stress", datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(out, exist_ok=True)
    evlog = open(os.path.join(out, "events.log"), "w", buffering=1)

    def log(msg):
        line = f"{datetime.datetime.now():%H:%M:%S} {msg}"
        print(line, flush=True)
        evlog.write(line + "\n")

    tl = ToneLab(os.path.join(ROOT, "WebEditor", "tonelab.html"))
    mod = Module(a.port, a.midi, log)
    original, switch = read_status(mod)
    if len(original) < 4:
        sys.exit("could not read the engines from 'status' (console?)")
    if "learn" in switch.lower():
        sys.exit("mode switch is on MIDI learn: notes would change the MIDI map. Move it to Play or Sequence.")
    snap = take_snapshot(mod, tl, os.path.join(out, "snapshot.json"))
    mismatch = set(snap.get("size_mismatch", {}))
    mix_count = sum(c for _, c, _ in tl.order["Mix"])
    if len(snap["mix"]) < mix_count or len(snap["structs"]) < len(VOICE_STRUCTS) + 1:
        mod.close()
        sys.exit(f"incomplete snapshot ({len(snap['mix'])} of {mix_count} mix values): not starting, nothing was changed")
    log(f"start: engines {original}, switch {switch}, results in {out}")
    log(f"snapshot saved ({len(snap['structs'])} configs, {len(snap['mix'])} mix values)"
        + (f"; layout differs from the Tone Lab, not randomised: {snap['size_mismatch']}" if mismatch else ""))
    log(f"randomised controls: " + ", ".join(f"{k} {len(v)}" for k, v in tl.controls.items()))
    log("crash record: " + (mod.cmd("crash", wait=0.5) or "?").strip().replace("\r\n", " | "))

    rec = None
    if not a.no_record:
        dev = [d for d in lt.list_devices() if "USB Audio CODEC" in d]
        if dev:
            rec = subprocess.Popen([lt.FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "dshow", "-audio_buffer_size", "50",
                                    "-sample_rate", "48000", "-channels", "2", "-i", "audio=" + dev[0], "-ac", "1",
                                    "-t", f"{a.hours * 3600 + 300:.0f}", "-c:a", "flac", "-f", "segment", "-segment_time", "60",
                                    "-strftime", "1", os.path.join(out, "seg_%H%M%S.flac")],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "ffmpeg.log"), "w"))
        else:
            log("audio interface not found: not recording")

    csv = open(os.path.join(out, "results.csv"), "w", buffering=1)
    csv.write("time,pass,kick,snare,hats,toms,params_id,params_mode,scenario,events,late_ms,cpu_avg,cpu_max,underruns,uptime_s,"
              "crashes,clip_db,peak_db,rate,i2s_restarts,flags,cycles\n")
    plog = open(os.path.join(out, "params.jsonl"), "w", buffering=1)

    combos = [{"kick": k, "snare": s, "hats": h, "toms": t} for k in range(1, 6) for s in range(1, 6)
              for h in range(1, 4) for t in range(1, 3)]
    rng = random.Random(a.seed)
    t_end = time.time() + a.hours * 3600
    last = parse_cpu(mod.cmd("cpu", wait=0.6))
    totals = {"scenarios": 0, "flags": {}}
    worst, restarts = [], []
    dead_since = None
    pass_no, pid = 0, 0
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
                pid += 1
                mode, chosen = ("current", {})
                if not a.no_random:
                    try:
                        mode, chosen = randomise(mod, tl, rng, combo, snap, mismatch)
                    except Exception as e:
                        log(f"randomise failed: {type(e).__name__} {e}")
                plog.write(json.dumps({"id": pid, "pass": pass_no, "combo": combo, "mode": mode, "values": chosen}) + "\n")
                mod.cmd("cpu", wait=0.5)                                 # reset the maxima
                time.sleep(1.0)
                base = parse_cpu(mod.cmd("cpu", wait=0.6))               # quiet level with these settings (modular triggers only)
                base_clip = to_float(base["clip_db"]) if base else None
                combo_flags, combo_max = [], 0.0
                for sc in SCENARIOS:
                    events = gen_events(sc, a.scenario_secs, rng)
                    err, late = play_events(mod, events)
                    if sc == "switching":
                        set_engines(mod, combo)                          # back to this combination
                    time.sleep(0.25)                                     # let the tails ring into the meter
                    r = parse_cpu(mod.cmd("cpu", wait=0.6))
                    flags = []
                    if err:
                        flags.append("MIDIERR")
                    if r is None:
                        flags.append("NOCONSOLE")
                        dead_since = dead_since or time.time()
                        if time.time() - dead_since > 30:
                            log(swd_check(a.elf))
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
                        if r["rate"] is not None and abs(r["rate"] - 48000) > 960:
                            flags.append(f"RATE{r['rate']}")
                        if last and r["restarts"] is not None and last.get("restarts") is not None and r["restarts"] > last["restarts"]:
                            flags.append(f"I2SRESTART+{r['restarts'] - last['restarts']}")
                        if last and r.get("ep0") is not None and last.get("ep0") is not None and r["ep0"] > last["ep0"]:
                            flags.append(f"EP0ABORT+{r['ep0'] - last['ep0']}")     # info: USB control transfer recovered
                        clip = to_float(r["clip_db"])
                        if r["nan"]:
                            flags.append("NAN")
                        if clip is not None and clip > 24.0:
                            flags.append("HOT")                          # info: legitimately hot settings reach this too
                        # Runaway: a second without notes, the level must fall back near this combination's quiet level
                        time.sleep(1.0)
                        q = parse_cpu(mod.cmd("cpu", wait=0.6))
                        q_clip = to_float(q["clip_db"]) if q else None
                        if q and q["nan"]:
                            flags.append("NAN")
                        limit = max(base_clip, -20.0) + 12.0 if base_clip is not None else None
                        if q_clip is not None and limit is not None and q_clip > limit:
                            # still loud after a second: long tail (rumble feedback, long hats) or self oscillation?
                            # Runaway only if it does not fall by at least 3 dB over the next 2.5 s
                            time.sleep(2.5)
                            q2 = parse_cpu(mod.cmd("cpu", wait=0.6))
                            q2_clip = to_float(q2["clip_db"]) if q2 else None
                            if q2_clip is not None and q2_clip > limit and q2_clip > q_clip - 3.0:
                                flags.append(f"RUNAWAY{q_clip:+.0f}>{q2_clip:+.0f}dB")
                            else:
                                flags.append(f"TAIL{q_clip:+.0f}>{q2_clip if q2_clip is not None else 0:+.0f}dB")
                        peak = to_float(r["peak_db"])
                        if sc != "sparse" and peak is not None and peak < -60.0:
                            flags.append("SILENT")
                        if "RESTART" in flags or "FAULT" in flags:
                            log("crash record: " + (mod.cmd("crash", wait=0.5) or "?").strip().replace("\r\n", " | "))
                            restarts.append(time.time())
                            if len([t for t in restarts if time.time() - t < 120]) >= 2:
                                log("two restarts within 2 minutes: restoring the snapshot settings (boot loop protection)")
                                restore_snapshot(mod, snap, log)
                        last = r
                        combo_max = max(combo_max, r["max"])
                        worst.append((r["max"], combo, sc, pid))
                    totals["scenarios"] += 1
                    for f in flags:
                        k = f.split("+")[0]
                        totals["flags"][k] = totals["flags"].get(k, 0) + 1
                    rr = r or {}
                    csv.write(f"{datetime.datetime.now():%H:%M:%S},{pass_no},{combo['kick']},{combo['snare']},{combo['hats']},{combo['toms']},"
                              f"{pid},{mode},{sc},{len(events)},{late * 1000:.1f},{rr.get('avg', '')},{rr.get('max', '')},{rr.get('under', '')},"
                              f"{rr.get('uptime', '')},{rr.get('crashes', '')},{rr.get('clip_db', '')},{rr.get('peak_db', '')},{rr.get('rate', '')},{rr.get('restarts', '')},"
                              f"{' '.join(flags)},{rr.get('cycles', '').replace(',', ' ')}\n")
                    if flags:
                        combo_flags.append(f"{sc}:{'/'.join(flags)}")
                        log(f"  FLAG {combo} params {pid} ({mode}) {sc}: {' '.join(flags)}  cpu {rr.get('avg')}/{rr.get('max')}%  "
                            f"{rr.get('cycles', '')}")
                log(f"[{pass_no}.{ci + 1:03d}] k{combo['kick']} s{combo['snare']} h{combo['hats']} t{combo['toms']} params {pid} {mode:8s} "
                    f"peak cpu {combo_max:.0f}%" + (f"  flags: {', '.join(combo_flags)}" if combo_flags else "  ok"))
    except KeyboardInterrupt:
        log("stopped by user")
    finally:
        try:
            restore_snapshot(mod, snap, log)
            set_engines(mod, original)
            log(f"engines restored to {original}")
        except Exception as e:
            log(f"could not restore: {e}  ->  run: python tools/stress_test.py --restore {out}")
        worst.sort(key=lambda w: -w[0])
        log(f"summary: {totals['scenarios']} scenarios, flags {totals['flags'] or 'none'}")
        for m, combo, sc, p in worst[:10]:
            log(f"  highest CPU {m:.1f}%  {combo} {sc} params {p}")
        if rec:
            rec.terminate()
        mod.close()


if __name__ == "__main__":
    main()
