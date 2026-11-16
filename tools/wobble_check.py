#!/usr/bin/env python3
"""
Pitch-wobble check: record the Punck while it plays (modular triggers, sequencer, buttons) and measure
  - kick hits: pitch curve per hit (cycle by cycle) and hit-to-hit spread, pitch drift inside the tail
  - double triggers: two onsets within 40 ms (noisy trigger cables / ground loop retrigger the voices)
  - hats: level and brightness per hit
Reference: hits fired from the console are identical to 0.1 Hz (kick 61 -> 49.9 Hz), so any spread here
comes from the trigger path or the outside chain, not from the voice code.

  python tools/wobble_check.py rec 15          record 15 s from the interface, then analyse
  python tools/wobble_check.py file X.wav      analyse an existing recording
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_test as lt


def onepole_lp(x, fc, rate, passes=2):
    a = math.exp(-2 * math.pi * fc / rate)
    for _ in range(passes):
        s, y = 0.0, []
        for v in x:
            s = (1 - a) * v + a * s
            y.append(s)
        x = y
    return x


def onepole_hp(x, fc, rate):
    lp = onepole_lp(x, fc, rate, passes=1)
    return [v - l for v, l in zip(x, lp)]


def envelope(x, rate, ms=2.0):
    blk = max(int(rate * ms / 1000), 1)
    return [max(abs(v) for v in x[i:i + blk]) for i in range(0, len(x) - blk, blk)], blk


def onsets(x, rate, rise_db=10.0, floor_db=-50.0, min_gap_ms=4.0):
    env, blk = envelope(x, rate)
    res, last, slow = [], -10 ** 9, 1e-6
    for k, e in enumerate(env):
        if lt.db(e) > floor_db and e > slow * 10 ** (rise_db / 20) and (k - last) * blk > min_gap_ms * rate / 1000:
            res.append(k * blk)
            last = k
        slow = max(0.97 * slow, e) if k - last > 2 else max(slow, e)
    return res


def cycles(x, rate):
    xs = [i - 1 + (-x[i - 1]) / (x[i] - x[i - 1]) for i in range(1, len(x)) if x[i - 1] < 0 <= x[i]]
    return [rate / (xs[k + 1] - xs[k]) for k in range(len(xs) - 1)]


def analyse(path):
    rate, ch = lt.read_wav(path)
    x = ch[0]
    pk = lt.peak(x)
    clipped = sum(1 for v in x if abs(v) > 0.999)
    print(f"{path}: {len(x) / rate:.1f} s, peak {lt.db(pk):.1f} dBFS, clipped samples {clipped}"
          + ("  <- interface clipping: turn the mixer master down" if clipped > 100 else ""))

    # Kick: low band
    low = onepole_lp(x, 150, rate)
    kicks = onsets(low, rate, rise_db=12, floor_db=lt.db(lt.peak(low)) - 20, min_gap_ms=60)
    print(f"\nKick hits: {len(kicks)}")
    curves = []
    for o in kicks:
        cy = cycles(onepole_lp(x[o:o + int(0.6 * rate)], 300, rate), rate)
        cy = [c for c in cy if 25 < c < 400]
        if len(cy) >= 8:
            ref = sorted(cy[2:6])[1]
            while cy and cy[0] > 1.4 * ref:					# partial first cycle at the onset
                cy = cy[1:]
        if len(cy) >= 12:
            curves.append((o, cy))
    # group hits with the same start pitch (kick vs toms / bass in the low band)
    groups = []
    for o, cy in curves:
        for g in groups:
            if abs(g[0][1][1] / cy[1] - 1) < 0.08:
                g.append((o, cy))
                break
        else:
            groups.append([(o, cy)])
    for g in groups:
        print(f"  group starting at {g[0][1][0]:.0f} Hz: {len(g)} hits")
        for o, cy in g[:16]:
            tail = cy[20:32]
            drift = f"{1200 * math.log2(max(tail) / min(tail)):5.1f} c" if len(tail) > 4 else "   -"
            print(f"    {o / rate:7.3f}s  " + " ".join(f"{c:5.1f}" for c in cy[:12]) + f"   tail wobble {drift}")
        if len(g) >= 2:
            n = min(min(len(c) for _, c in g), 16)
            spread = [1200 * math.log2(max(c[k] for _, c in g) / min(c[k] for _, c in g)) for k in range(n)]
            print(f"    hit-to-hit spread over the first {n} cycles: median {sorted(spread)[len(spread) // 2]:.1f} c, "
                  f"max {max(spread):.1f} c   (console reference: < 1 c)")

    # Double triggers (any voice): onsets on the full band closer than 40 ms
    allon = onsets(x, rate, rise_db=10, floor_db=lt.db(pk) - 30, min_gap_ms=3)
    doubles = [(a, b) for a, b in zip(allon, allon[1:]) if (b - a) < 0.040 * rate]
    print(f"\nOnsets (all voices): {len(allon)}, pairs closer than 40 ms: {len(doubles)}")
    for a, b in doubles[:12]:
        print(f"  {a / rate:7.3f}s + {1000 * (b - a) / rate:4.1f} ms")

    # Hats: high band level / brightness per hit
    high = onepole_hp(x, 5000, rate)
    hats = onsets(high, rate, rise_db=12, floor_db=lt.db(lt.peak(high)) - 24, min_gap_ms=40)
    print(f"\nHat-band hits: {len(hats)}")
    rows = []
    for o in hats[:24]:
        seg = high[o:o + int(0.03 * rate)]
        full = x[o:o + int(0.03 * rate)]
        rows.append(f"{o / rate:.2f}s {lt.db(lt.rms(seg)):5.1f}dB/{lt.db(lt.rms(seg)) - lt.db(lt.rms(full)):+4.1f}")
    for k in range(0, len(rows), 4):
        print("  " + "   ".join(rows[k:k + 4]))


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "file":
        analyse(sys.argv[2])
        return
    secs = float(sys.argv[2]) if len(sys.argv) >= 3 and sys.argv[1] == "rec" else 15.0
    dev = [d for d in lt.list_devices() if "USB Audio CODEC" in d]
    if not dev:
        sys.exit("capture device not found")
    os.makedirs(lt.OUT, exist_ok=True)
    path = os.path.join(lt.OUT, "wobble_play.wav")
    print(f"Recording {secs:.0f} s from {dev[0]} ...")
    lt.record(dev[0], secs, path)
    analyse(path)


if __name__ == "__main__":
    main()
