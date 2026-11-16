#!/usr/bin/env python3
"""
Long-run monitor for intermittent faults (the 'wobbly pitch after an hour' case).

  python tools/longrun_monitor.py [--hours 3] [--every 15] [--port COM17]

- records the audio interface continuously in 1-minute FLAC chunks named by wall clock time
  (build/leveltest/longrun/<start>/seg_HHMMSS.flac), so the minutes around a fault can be analysed later
  with tools/wobble_check.py and compared with the clean minutes before it
- polls the Punck console (main loop only - does not touch the audio interrupt, no SWD):
  CPU load, underruns, and all pots twice per poll -> pot jitter (ADC noise from ground / power problems)
  written to monitor.csv in the same folder, one line per poll
Stop with Ctrl+C (or after --hours).
"""
import argparse
import datetime
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_test as lt


def read_pots(p):
    vals = {}
    for m in re.finditer(r"^(.+?)\s+([\d.]+) %", p.cmd("pots", wait=0.4), re.M):
        vals[m.group(1).strip()] = float(m.group(2))
    return vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=3.0)
    ap.add_argument("--every", type=float, default=15.0)
    ap.add_argument("--port", default="COM17")
    a = ap.parse_args()

    dev = [d for d in lt.list_devices() if "USB Audio CODEC" in d]
    if not dev:
        sys.exit("capture device not found")
    folder = os.path.join(lt.OUT, "longrun", datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(folder, exist_ok=True)
    rec = subprocess.Popen([lt.FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "dshow", "-audio_buffer_size", "50",
                            "-sample_rate", "48000", "-channels", "2", "-i", "audio=" + dev[0],
                            "-t", f"{a.hours * 3600:.0f}", "-c:a", "flac", "-f", "segment", "-segment_time", "60",
                            "-strftime", "1", os.path.join(folder, "seg_%H%M%S.flac")],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=open(os.path.join(folder, "ffmpeg.log"), "w"))
    csv = open(os.path.join(folder, "monitor.csv"), "w", buffering=1)
    csv.write("time,uptime_s,cpu_avg,cpu_max,underruns,pots_read,max_jitter_pct,max_jitter_pot,pots,cycles\n")
    print(f"Monitoring into {folder}  (Ctrl+C to stop)", flush=True)

    p, t0, last_under = None, time.time(), None
    try:
        while time.time() - t0 < a.hours * 3600 and rec.poll() is None:
            now = datetime.datetime.now().strftime("%H:%M:%S")
            try:
                if p is None:
                    p = lt.Punck(a.port)
                cpu = p.cmd("cpu", wait=0.4)
                m = re.search(r"CPU avg: ([\d.]+)%\s+max: ([\d.]+)%\s+underruns: (\d+)", cpu)
                avg, mx, under = (float(m.group(1)), float(m.group(2)), int(m.group(3))) if m else (None, None, None)
                cy = re.search(r"budget \d+\):(.*)", cpu)				# per-block cycles avg/max (firmware r9+)
                cycles = cy.group(1).strip().replace(",", " ") if cy else ""
                r1, r2 = read_pots(p), read_pots(p)
                common = [k for k in r1 if k in r2]
                jit = max(((abs(r1[k] - r2[k]), k) for k in common), default=(0.0, "-"))
                pots = ";".join(f"{k}={r1[k]:.1f}" for k in sorted(r1))
                csv.write(f"{now},{time.time() - t0:.0f},{avg},{mx},{under},{len(common)},{jit[0]:.2f},{jit[1]},{pots},{cycles}\n")
                flag = ""
                if under is not None and last_under is not None and under > last_under:
                    flag += f"  UNDERRUNS +{under - last_under}"
                if jit[0] >= 0.5:
                    flag += f"  POT JITTER {jit[0]:.1f}% on {jit[1]}"
                last_under = under if under is not None else last_under
                print(f"{now} cpu {avg}/{mx}% underruns {under} pot jitter {jit[0]:.2f}% ({jit[1]}){flag}", flush=True)
            except Exception as e:                       # USB dropped out: note it and keep trying
                csv.write(f"{now},{time.time() - t0:.0f},,,,0,,USB ERROR {type(e).__name__},\n")
                print(f"{now} console not reachable: {e}", flush=True)
                try:
                    if p:
                        p.s.close()
                except Exception:
                    pass
                p = None
            time.sleep(a.every)
    except KeyboardInterrupt:
        pass
    finally:
        rec.terminate()
        if p:
            p.s.close()
        print("stopped", flush=True)


if __name__ == "__main__":
    main()
