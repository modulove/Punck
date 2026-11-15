#!/usr/bin/env python3
"""
Punck level calibration through an audio interface (Punck L/R outputs -> two interface inputs).
The Punck is driven over its USB console (test tone / voice triggers), the interface is recorded with
ffmpeg (DirectShow) and analysed here (pure Python, no numpy needed).

  python tools/level_test.py devices                       list capture devices
  python tools/level_test.py detect                        find the input carrying the Punck (quiet -40 dB tone)
  python tools/level_test.py noise  --device "NAME"        noise floor and mains hum, Punck idle
  python tools/level_test.py sweep  --device "NAME" --yes  test tone level sweep -> clean analog ceiling (LOUD tones!)
  python tools/level_test.py groove --device "NAME"        record a 124 BPM groove with several master settings:
                                                           loudness (LUFS), true peak, crest factor

Results and WAV recordings go to build/leveltest/.
"""
import argparse
import array
import math
import os
import re
import subprocess
import sys
import time
import wave

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "leveltest")
FFMPEG = "ffmpeg"


# ------------------------------------------------------------------------------------------------
# Punck console
# ------------------------------------------------------------------------------------------------
class Punck:
    def __init__(self, port):
        self.s = serial.Serial(port, 230400, timeout=0.05, write_timeout=2)
        self.s.reset_input_buffer()

    def cmd(self, c, wait=0.15):
        self.s.write((c + "\r").encode())
        t0, buf = time.time(), b""
        while time.time() - t0 < wait:
            buf += self.s.read(4096)
        return buf.decode("ascii", "replace")

    def send(self, c):                                   # fire and forget (timing critical)
        self.s.write((c + "\r").encode())

    def close(self):
        self.cmd("tone:0")
        self.s.close()


# ------------------------------------------------------------------------------------------------
# Recording (ffmpeg DirectShow) and WAV reading
# ------------------------------------------------------------------------------------------------
def list_devices():
    r = subprocess.run([FFMPEG, "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return re.findall(r'"([^"]+)" \(audio\)', r.stderr)


def record_cmd(device, seconds, path):
    return [FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "dshow", "-audio_buffer_size", "50",
            "-sample_rate", "48000", "-channels", "2", "-i", "audio=" + device,
            "-t", f"{seconds:.2f}", "-c:a", "pcm_f32le", "-y", path]


def record(device, seconds, path):
    r = subprocess.run(record_cmd(device, seconds, path), capture_output=True, text=True)
    if r.returncode != 0:          # device may not offer 48 kHz stereo: let DirectShow pick, then resample
        cmd = [FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "dshow", "-audio_buffer_size", "50",
               "-i", "audio=" + device, "-t", f"{seconds:.2f}", "-ar", "48000", "-ac", "2", "-c:a", "pcm_f32le", "-y", path]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"recording from '{device}' failed: {r.stderr.strip()[:200]}")
    return read_wav(path)


def read_wav(path):
    # ffmpeg writes WAVE_FORMAT_IEEE_FLOAT (or EXTENSIBLE) which the wave module refuses: parse the chunks by hand
    with open(path, "rb") as f:
        data = f.read()
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], int.from_bytes(data[pos + 4:pos + 8], "little")
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = (int.from_bytes(body[0:2], "little"), int.from_bytes(body[2:4], "little"),
                   int.from_bytes(body[4:8], "little"), int.from_bytes(body[14:16], "little"))
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    tag, chans, rate, bits = fmt
    if bits == 32 and tag in (3, 0xFFFE):
        a = array.array("f"); a.frombytes(raw[:len(raw) // 4 * 4])
        x = list(a)
    elif bits == 16:
        a = array.array("h"); a.frombytes(raw[:len(raw) // 2 * 2])
        x = [v / 32768.0 for v in a]
    else:
        raise RuntimeError(f"unsupported wav format {fmt}")
    return rate, [x[c::chans] for c in range(chans)]


# ------------------------------------------------------------------------------------------------
# Analysis
# ------------------------------------------------------------------------------------------------
def db(x):
    return 20.0 * math.log10(max(x, 1e-9))


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(len(x), 1))


def peak(x):
    return max((abs(v) for v in x), default=0.0)


def goertzel_amp(x, f, rate, win):
    # Amplitude of the sinusoid at f (Hann windowed, amplitude-corrected)
    w = 2.0 * math.pi * f / rate
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v, h in zip(x, win):
        s0 = v * h + c * s1 - s2
        s2, s1 = s1, s0
    re_ = s1 - s2 * math.cos(w)
    im = s2 * math.sin(w)
    return 2.0 * math.sqrt(re_ * re_ + im * im) / sum(win)


def hann(n):
    return [0.5 - 0.5 * math.cos(2.0 * math.pi * i / (n - 1)) for i in range(n)]


def thd(x, f0, rate, harmonics=7):
    win = hann(len(x))
    fund = goertzel_amp(x, f0, rate, win)
    h = [goertzel_amp(x, f0 * k, rate, win) for k in range(2, harmonics + 2) if f0 * k < rate / 2 - 500]
    return fund, 100.0 * math.sqrt(sum(a * a for a in h)) / max(fund, 1e-12), h


RATE_FILE = os.path.join(OUT, "punck_rate_factor.txt")


def punck_rate_factor():
    # Old firmware: the I2S clock ran from the internal RC oscillator (nominal 47619 Hz, measured ~47810 Hz), so a
    # 1000 Hz test tone arrives at ~996 Hz. 'detect' measures the factor; default = last measurement.
    try:
        return float(open(RATE_FILE).read())
    except (OSError, ValueError):
        return 1.0                      # exact 48 kHz since the PLL3 clock fix (0.99605 with the old HSI clock)


def find_freq(x, f0, rate):
    # Strongest frequency near f0 (-1.5 % .. +0.5 %): coarse Goertzel grid on 0.25 s, then a fine grid on all of x
    seg = x[:rate // 4]
    win = hann(len(seg))
    best = max((f0 * (1 + k * 0.0005) for k in range(-30, 11)), key=lambda f: goertzel_amp(seg, f, rate, win))
    win = hann(len(x))
    return max((best + d * 0.1 for d in range(-12, 13)), key=lambda f: goertzel_amp(x, f, rate, win))


def middle(x, rate, seconds):
    n = int(seconds * rate)
    start = max((len(x) - n) // 2, 0)
    return x[start:start + n]


def loudness(path):
    # Integrated loudness and true peak from ffmpeg's EBU R128 meter
    r = subprocess.run([FFMPEG, "-hide_banner", "-nostats", "-i", path, "-af", "ebur128=peak=true", "-f", "null", "-"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    tail = r.stderr[r.stderr.rfind("Summary:"):]
    i = re.search(r"I:\s+(-?[\d.]+) LUFS", tail)
    lra = re.search(r"LRA:\s+(-?[\d.]+) LU", tail)
    tp = re.search(r"Peak:\s+(-?[\d.]+) dBFS", tail)
    return (float(i.group(1)) if i else None, float(lra.group(1)) if lra else None, float(tp.group(1)) if tp else None)


# ------------------------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------------------------
def cmd_detect(a, p):
    devs = list_devices()
    print("Quiet 1 kHz test tone at -40 dB, recording each input with the tone on and off ...")
    best = None
    for d in devs:
        try:
            p.cmd("tone:0"); time.sleep(0.2)
            rate, off = record(d, 0.8, os.path.join(OUT, "detect_off.wav"))
            p.cmd("tone:1000,-40"); time.sleep(0.3)
            rate, on = record(d, 1.2, os.path.join(OUT, "detect_on.wav"))
        except RuntimeError as e:
            print(f"  {d:55s} (no recording: {str(e)[:60]})")
            continue
        lv_on = [db(rms(middle(x, rate, 0.8))) for x in on[:2]]
        lv_off = [db(rms(middle(x, rate, 0.5))) for x in off[:2]]
        diff = max(lv_on[0] - lv_off[0], lv_on[-1] - lv_off[-1])
        print(f"  {d:55s} tone on L {lv_on[0]:6.1f} R {lv_on[-1]:6.1f}   off L {lv_off[0]:6.1f} R {lv_off[-1]:6.1f} dBFS")
        if best is None or diff > best[1]:
            best = (d, diff, lv_on, on, rate)
    p.cmd("tone:0")
    if best and best[1] > 10:
        d, _, lv, on, rate = best
        f = find_freq(middle(on[0], rate, 1.0), 1000.0, rate)
        with open(RATE_FILE, "w") as fh:
            fh.write(f"{f / 1000.0:.6f}")
        print(f"\nPunck found on: \"{d}\"  (L {lv[0]:.1f} / R {lv[-1]:.1f} dBFS at -40 dB)")
        print(f"Punck 0 dB (clipper ceiling) arrives at about {lv[0] + 40 + 3.0:+.1f} dBFS peak on this input"
              f"{'  -> INTERFACE GAIN TOO HIGH: turn it down so this is around -6 dBFS' if lv[0] + 43 > -3 else ''}")
        print(f"Test tone measured at {f:.2f} Hz: Punck sample rate about {48000 * f / 1000:.0f} Hz")
        if abs(lv[0] - lv[-1]) > 1.0:
            print("Note: left and right differ by more than 1 dB - check both cables and the two input gain knobs.")
    else:
        print("\nNo input shows the tone. Check cables, interface input selection and that monitoring apps release the device.")


def cmd_noise(a, p):
    p.cmd("tone:0")
    time.sleep(0.3)
    rate, ch = record(a.device, 3.0, os.path.join(OUT, "noise.wav"))
    print("Noise floor, Punck idle (all voices silent):")
    for name, x in zip("LR", ch):
        seg = middle(x, rate, 2.0)
        win = hann(len(seg))
        hum = [db(goertzel_amp(seg, f, rate, win) / math.sqrt(2)) for f in (50, 100, 150, 200)]
        print(f"  {name}: {db(rms(seg)):6.1f} dBFS rms   hum 50/100/150/200 Hz: " + " ".join(f"{h:6.1f}" for h in hum))


def cmd_sweep(a, p):
    if not a.yes:
        sys.exit("The sweep plays sine tones up to full scale. Turn monitoring / speakers down, then add --yes.")
    levels = [-30, -20, -12, -6, -3, -2, -1, -0.5, 0, 0.3, 0.6]
    results = []
    for f in a.freqs:
        print(f"\n{f:.0f} Hz   Punck dB  ->  interface L / R dBFS   THD L / R      gain error")
        ref = None
        for L in levels:
            p.cmd(f"tone:{f:.0f},{L}")
            time.sleep(0.25)
            rate, ch = record(a.device, 1.4, os.path.join(OUT, f"sweep_{f:.0f}_{L}.wav"))
            row = []
            fa = f * punck_rate_factor()                         # actual tone frequency (Punck clock)
            for x in ch[:2]:
                seg = middle(x, rate, 1.0)
                fund, t, _ = thd(seg, fa, rate)
                row.append((db(fund / math.sqrt(2)), t, db(peak(seg))))
            lvl = (row[0][0] + row[-1][0]) / 2
            if ref is None:
                ref = lvl - L
            err = lvl - L - ref                                  # compression of the analog path vs the -30 dB step
            hot = max(r[2] for r in row) > -1.0
            print(f"        {L:+6.1f}  ->  {row[0][0]:7.2f} / {row[-1][0]:7.2f}      {row[0][1]:6.3f}% / {row[-1][1]:6.3f}%   "
                  f"{err:+5.2f} dB{'   INTERFACE NEAR CLIP' if hot else ''}")
            results.append((f, L, row, err, hot))
    p.cmd("tone:0")

    clean = [r for r in results if max(x[1] for x in r[2]) < a.thd and abs(r[3]) < 0.3 and not r[4]]
    ceiling = min(max((r[1] for r in clean if r[0] == f), default=-99) for f in a.freqs)
    print(f"\nHighest clean level on every frequency (THD < {a.thd}%, gain within 0.3 dB, interface not clipping): {ceiling:+.1f} dB")
    if any(r[4] for r in results):
        print("The interface itself got close to clipping: lower its input gain and run the sweep again,")
        print("otherwise the interface and not the Punck limits the result.")
    with open(os.path.join(OUT, "sweep.txt"), "w") as fh:
        for f, L, row, err, hot in results:
            fh.write(f"{f} {L} " + " ".join(f"{x[0]:.2f} {x[1]:.4f} {x[2]:.2f}" for x in row) + f" {err:.3f} {int(hot)}\n")


MASTER = {
    "original": {6: 0, 16: 0},
    "glue":     {6: 1, 7: -12, 8: 3, 9: 15, 10: 120, 11: 6, 12: 3, 13: 1, 14: 90, 15: 0, 16: 1, 17: 2, 18: 0},
    "loud":     {6: 1, 7: -16, 8: 3, 9: 12, 10: 100, 11: 6, 12: 4, 13: 1, 14: 90, 15: 0, 16: 1, 17: 5, 18: -0.5},
    "warm":     {6: 1, 7: -12, 8: 3, 9: 15, 10: 120, 11: 6, 12: 3, 13: 1, 14: 90, 15: 0, 16: 3, 17: 2, 18: 0},
}
# 16 steps: one char per 16th, '.' = rest, 1-9 = velocity (same as the Tone Lab 'Techno 4/4' pattern)
PATTERN = {(1, 0): "9...9...9...9...", (3, 0): "3.6.3.6.3.6.3.6.", (3, 2): "..7...7...7...7.", (7, 0): "....6.......6...",
           (6, 0): "..........4....."}


def read_mix(p):
    vals = {}
    for line in p.cmd("mix", wait=0.6).splitlines():
        m = re.match(r"\s*(\d+)\s+.*?\s(-?[\d.]+)\s*$", line)
        if m:
            vals[int(m.group(1))] = float(m.group(2))
    return vals


def cmd_groove(a, p):
    saved = read_mix(p)
    step = 60.0 / a.bpm / 4.0
    seconds = a.bars * 16 * step
    print(f"Groove: {a.bars} bars at {a.bpm} BPM (current engines and pots), master settings: {', '.join(a.master)}")
    rows = []
    try:
        for name in a.master:
            for idx, v in MASTER[name].items():
                p.cmd(f"mix:{idx},{v}", wait=0.05)
            path = os.path.join(OUT, f"groove_{name}.wav")
            rec = subprocess.Popen(record_cmd(a.device, seconds + 2.5, path), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            time.sleep(1.0)                                          # DirectShow start-up
            t0 = time.perf_counter()
            for s in range(a.bars * 16):
                hits = [f"{v},{round(int(pat[s % 16]) / 9 * 127)},{o}" for (v, o), pat in PATTERN.items() if pat[s % 16] != "."]
                while time.perf_counter() < t0 + s * step:
                    pass
                if hits:
                    p.send("play:" + ";".join(hits))
            rec.wait()
            rate, ch = read_wav(path)
            mono = [0.5 * (l + r) for l, r in zip(ch[0], ch[-1])]
            start = next((i for i, v in enumerate(mono) if abs(v) > 0.01), 0)
            seg_l, seg_r = ch[0][start:start + int(seconds * rate)], ch[-1][start:start + int(seconds * rate)]
            pk = db(max(peak(seg_l), peak(seg_r)))
            rm = db(math.sqrt((rms(seg_l) ** 2 + rms(seg_r) ** 2) / 2))
            lufs, lra, tp = loudness(path)
            rows.append((name, lufs, tp, pk, rm, pk - rm))
    finally:
        for idx, v in saved.items():
            p.cmd(f"mix:{idx},{v}", wait=0.05)
    print("\n  master      LUFS-I   true peak   sample peak   rms      crest")
    for name, lufs, tp, pk, rm, cr in rows:
        print(f"  {name:10s} {lufs if lufs is not None else float('nan'):7.1f}   {tp if tp is not None else float('nan'):7.1f}     "
              f"{pk:7.1f}      {rm:6.1f}   {cr:5.1f} dB")
    print("\n(levels are interface dBFS; the module settings were restored afterwards)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("test", choices=["devices", "detect", "noise", "sweep", "groove"])
    ap.add_argument("--port", default="COM17")
    ap.add_argument("--device", help="capture device name (see 'devices' / 'detect')")
    ap.add_argument("--yes", action="store_true", help="confirm loud test tones (sweep)")
    ap.add_argument("--freqs", type=float, nargs="+", default=[1000.0, 60.0])
    ap.add_argument("--thd", type=float, default=0.3, help="THD limit in percent for 'clean'")
    ap.add_argument("--bpm", type=float, default=124.0)
    ap.add_argument("--bars", type=int, default=8)
    ap.add_argument("--master", nargs="+", default=["original", "glue", "loud"], choices=list(MASTER))
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    if a.test == "devices":
        print("\n".join(list_devices()))
        return
    if a.test != "detect" and not a.device:
        sys.exit("--device is required (run 'detect' first)")
    p = Punck(a.port)
    try:
        {"detect": cmd_detect, "noise": cmd_noise, "sweep": cmd_sweep, "groove": cmd_groove}[a.test](a, p)
    finally:
        p.close()


if __name__ == "__main__":
    main()
