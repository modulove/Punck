#!/usr/bin/env python3
"""
Prepare and check samples for the Punck sample banks.

  prep SRC OUT --bank A [--start 1] [--stereo] [--no-trim] [--peak -1.0] [--volume 100]
      Convert every audio file in SRC (sorted by name) with ffmpeg to 48 kHz / 16 bit PCM WAV with a clean
      44-byte header (no metadata chunks -> sample data is 4-byte aligned on the Punck flash), trim leading
      silence, peak-normalise, and name them A01_name.v100.wav, A02_... (mono unless --stereo).
  check DIR_OR_DRIVE
      Show the bank/slot map exactly as the firmware will see it, warn about duplicate slots, >40 per bank,
      sample rates other than 48 kHz, unaligned data, and the stock-firmware MIDI note for each slot.

Firmware rules (Punck/src/voices/samples.cpp):
  * bank = first letter (A/B) of the file name, slot = number right after it; sorted by slot number
  * at most 40 samples per bank, 128 total; '.vNN' in the long name sets volume (0-200 %)
  * each sampler plays one sample at a time (a new hit cuts the previous one)
  * files must be contiguous on the drive: best format via console 'format', then copy all files in one go
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

AUDIO_EXT = (".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".m4a")
STOCK_MIDI = {"A": 48, "B": 60}            # stock firmware: sampler A notes 48-55, sampler B 60-71
STOCK_MIDI_COUNT = {"A": 8, "B": 12}


def ffmpeg():
    exe = shutil.which("ffmpeg") or r"C:\ffmpeg\bin\ffmpeg.exe"
    if not os.path.exists(exe):
        sys.exit("ffmpeg not found")
    return exe


def clean_name(stem, maxlen=18):
    stem = re.sub(r"\.v\d+$", "", stem)                      # drop existing volume tags
    stem = re.sub(r"^[ABab]\d+[_\-. ]*", "", stem)           # drop existing bank/slot prefix
    stem = re.sub(r"[^A-Za-z0-9\-]+", "-", stem).strip("-").lower()
    return (stem or "smp")[:maxlen]


def peak_db(path):
    r = subprocess.run([ffmpeg(), "-hide_banner", "-nostats", "-i", path, "-af", "volumedetect", "-f", "null", "-"],
                       capture_output=True, text=True)
    m = re.search(r"max_volume:\s*(-?[\d.]+) dB", r.stderr)
    return float(m.group(1)) if m else 0.0


def do_prep(a):
    files = sorted(f for f in os.listdir(a.src) if f.lower().endswith(AUDIO_EXT))
    if not files:
        sys.exit(f"No audio files in {a.src}")
    os.makedirs(a.out, exist_ok=True)
    slot = a.start
    for f in files:
        src = os.path.join(a.src, f)
        name = f"{a.bank.upper()}{slot:02d}_{clean_name(os.path.splitext(f)[0])}.v{a.volume}.wav"
        dst = os.path.join(a.out, name)
        filters = []
        if not a.no_trim:
            filters.append("silenceremove=start_periods=1:start_threshold=-60dB:start_silence=0.002")
        gain = a.peak - peak_db(src)
        filters.append(f"volume={gain:.2f}dB")
        cmd = [ffmpeg(), "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-af", ",".join(filters),
               "-ar", "48000", "-c:a", "pcm_s16le", "-map_metadata", "-1", "-fflags", "+bitexact",
               "-flags:a", "+bitexact"]
        if not a.stereo:
            cmd += ["-ac", "1"]
        cmd.append(dst)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(f"  FAILED {f}: {r.stderr.strip()[:200]}")
            continue
        print(f"  {name:40s} <- {f}  (gain {gain:+.1f} dB)")
        slot += 1
    print(f"\n{slot - a.start} files written to {a.out}")


def wav_info(path):
    with open(path, "rb") as fh:
        hdr = fh.read(4096)
    if hdr[:4] != b"RIFF" or hdr[8:12] != b"WAVE":
        return None
    pos, info = 12, {}
    while pos + 8 <= len(hdr):
        cid, size = hdr[pos:pos + 4], struct.unpack_from("<I", hdr, pos + 4)[0]
        if cid == b"fmt ":
            fmt, ch, rate = struct.unpack_from("<HHI", hdr, pos + 8)
            bits = struct.unpack_from("<H", hdr, pos + 22)[0]
            info.update(fmt=fmt, ch=ch, rate=rate, bits=bits)
        elif cid == b"data":
            info.update(data_offset=pos + 8, data_size=size)
            break
        pos += 8 + size + (size & 1)
    return info


def do_check(a):
    files = sorted(f for f in os.listdir(a.path) if f.lower().endswith(".wav"))
    banks = {"A": {}, "B": {}}
    for f in files:
        m = re.match(r"([ABab])(\d+)", f)
        if not m:
            print(f"  ignored (no A/B prefix): {f}")
            continue
        bank, slot = m.group(1).upper(), int(m.group(2))
        banks[bank].setdefault(slot, []).append(f)
    total_bytes = 0
    for bank in "AB":
        slots = banks[bank]
        print(f"\nBank {bank}: {sum(len(v) for v in slots.values())} samples" +
              ("  ** more than 40 - extra samples ignored **" if sum(len(v) for v in slots.values()) > 40 else ""))
        for i, slot in enumerate(sorted(slots)):
            for f in slots[slot]:
                p = os.path.join(a.path, f)
                info = wav_info(p) or {}
                size = os.path.getsize(p)
                total_bytes += size
                warn = []
                if len(slots[slot]) > 1:
                    warn.append("DUPLICATE SLOT")
                if info.get("rate") != 48000:
                    warn.append(f"{info.get('rate')} Hz")
                if info.get("data_offset", 0) % 4:
                    warn.append("data not 4-byte aligned")
                vol = re.search(r"\.v(\d+)", f)
                midi = f"note {STOCK_MIDI[bank] + i}" if i < STOCK_MIDI_COUNT[bank] else "button/knob only"
                secs = info.get("data_size", 0) / max(1, info.get("rate", 48000) * info.get("ch", 1) * info.get("bits", 16) // 8)
                print(f"  pos {i + 1:2d} slot {slot:3d}  {f[:38]:38s} {info.get('ch', '?')}ch {info.get('bits', '?')}bit "
                      f"{secs:5.2f}s vol {vol.group(1) if vol else '100'}%  [{midi}]  {' '.join(warn)}")
    print(f"\nTotal {total_bytes / 1e6:.2f} MB of ~31.9 MB")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prep")
    p.add_argument("src"); p.add_argument("out")
    p.add_argument("--bank", required=True, choices=["A", "B", "a", "b"])
    p.add_argument("--start", type=int, default=1)
    p.add_argument("--stereo", action="store_true", help="keep stereo (pads/fx); default converts to mono")
    p.add_argument("--no-trim", action="store_true")
    p.add_argument("--peak", type=float, default=-1.0, help="normalise peak to this dBFS")
    p.add_argument("--volume", type=int, default=100, help="value for the .vNN volume tag")
    c = sub.add_parser("check")
    c.add_argument("path")
    a = ap.parse_args()
    do_prep(a) if a.cmd == "prep" else do_check(a)


if __name__ == "__main__":
    main()
