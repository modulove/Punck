#!/usr/bin/env python3
"""
Punck configuration backup / restore over USB MIDI SysEx (+ optional serial console).

Uses the same SysEx protocol as the web editor (Punck/src/usb/MidiHandler.cpp):
  0x1B GetSequence     0x1C SetSequence
  0x1D GetVoiceConfig  0x1E SetVoiceConfig
  0x1F GetSamples      0x20 GetStatus      0x21 SaveConfig
  0x22 GetReverbConfig 0x23 SetReverbConfig

backup  : read-only. Dumps all 5 sequences (all 4 bars each), every voice config,
          reverb config, sample bank lists; optionally the serial console 'help' and
          'midimap' output. Writes raw binaries, a decoded JSON, web-editor importable
          SequenceN.json files and a reconstructed image of the internal-flash config
          block (as stored at 0x081E0000 by config version 5 firmware).
restore : writes voice configs, reverb and sequences from a backup folder back to the
          module and (with --save) commits them to internal flash. MIDI note map and
          MIDI channel are not settable over SysEx (use MIDI learn / console).

Requires: pip install mido python-rtmidi pyserial
Close the web editor (and any serial terminal on the console port) first.
"""
import argparse
import datetime
import json
import os
import struct
import sys
import time

import mido

mido.set_backend("mido.backends.rtmidi")

CMD = dict(StartStop=0x1A, GetSequence=0x1B, SetSequence=0x1C, GetVoiceConfig=0x1D,
           SetVoiceConfig=0x1E, GetSamples=0x1F, GetStatus=0x20, SaveConfig=0x21,
           GetReverbConfig=0x22, SetReverbConfig=0x23)

VOICES = ["Kick", "Snare", "HiHat", "Sampler_A", "Sampler_B", "Toms", "Claps"]
MAX_BEATS = 24
BAR_BYTES = MAX_BEATS * len(VOICES) * 2          # 336: beat[24][7] of {level, index}
SEQ_COUNT = 5
BARS_PER_SEQ = 4

# Field layouts of the voice Config structs (see Punck/src/voices/*.h, reverb.h)
LAYOUT = {
    "Kick": [("ramp1Inc", "f"), ("ramp2Inc", "f"), ("ramp3Inc", "f"), ("fastSinFreq", "f"),
             ("initSlowSinFreq", "f"), ("sineSlowDownRate", "f")],
    "Snare": [("noiseInitLevel", "f"), ("noiseDecay", "f"), ("baseFreq", "f"), ("basePos", "f"),
              ("partialDecay", "f"), ("partialInitLevel", "3f"), ("partialFreqOffset", "3f")],
    "HiHat": [("attackInc", "f"), ("decay", "f"), ("hpInitCutoff", "f"), ("hpFinalCutoff", "f"),
              ("hpCutoffInc", "f"), ("lpInitCutoff", "f"), ("lpFinalCutoff", "f"),
              ("lpCutoffInc", "f"), ("noiseInitLevel", "f"), ("noiseDecay", "f"),
              ("partialScale", "6f"), ("partialFreq", "6f"), ("partialFM", "6B")],
    "Toms": [("decaySpeed", "2f"), ("rampInc", "f"), ("baseFreq", "f"), ("sineFreqScale", "2f"),
             ("sineInitLevel", "2f"), ("sineSlowDownRate", "f")],
    "Claps": [("initLevel", "f"), ("reverbInitLevel", "f"), ("initDecay", "f"),
              ("reverbDecay", "f"), ("filterCutoff", "f"), ("filterQ", "f"),
              ("unfilteredNoiseLevel", "f")],
    "Reverb": [("reverbLevel", "f"), ("mixerBaseDelay", "f"), ("diffuserCount", "f"),
               ("mixerChannels", "f"), ("filterCutoff", "f")],
}

# Order in which voice configs are serialised into the flash config block
# (VoiceManager::noteMapper order, skipping Sampler_B which has voiceIndex 1)
FLASH_VOICE_ORDER = ["Kick", "Snare", "HiHat", "Sampler_A", "Toms", "Claps"]


def decode_struct(name, raw):
    out, pos = {}, 0
    for field, fmt in LAYOUT[name]:
        size = struct.calcsize("<" + fmt)
        if pos + size > len(raw):
            break
        vals = struct.unpack_from("<" + fmt, raw, pos)
        out[field] = [round(v, 7) if isinstance(v, float) else v for v in vals] if len(vals) > 1 else \
            (round(vals[0], 7) if isinstance(vals[0], float) else vals[0])
        pos += size
    return out


def nibble_decode(data):
    return bytes((data[i] & 0x0F) | ((data[i + 1] & 0x0F) << 4) for i in range(0, len(data) - 1, 2))


def nibble_encode(raw):
    out = []
    for b in raw:
        out += [b & 0x0F, b >> 4]
    return out


def find_port(names, wanted):
    for n in names:
        if wanted.lower() in n.lower():
            return n
    raise SystemExit(f"MIDI port containing '{wanted}' not found. Available: {names}")


class Punck:
    def __init__(self, port_hint):
        self.inp = mido.open_input(find_port(mido.get_input_names(), port_hint))
        self.out = mido.open_output(find_port(mido.get_output_names(), port_hint))

    def close(self):
        self.inp.close()
        self.out.close()

    def request(self, data, expect, timeout=2.0):
        for _ in self.inp.iter_pending():
            pass
        self.out.send(mido.Message("sysex", data=data))
        t0 = time.time()
        while time.time() - t0 < timeout:
            for msg in self.inp.iter_pending():
                if msg.type == "sysex" and len(msg.data) and msg.data[0] == expect:
                    return list(msg.data)
            time.sleep(0.005)
        raise TimeoutError(f"No reply to SysEx {data[:3]}")

    def send(self, data):
        self.out.send(mido.Message("sysex", data=data))
        time.sleep(0.05)

    def status(self):
        d = self.request([CMD["GetStatus"], 0], CMD["GetStatus"])
        return dict(playing=bool(d[1]), activeSequence=d[2], bar=d[3], beat=d[4], i2sUnderrun=d[5])

    def voice_config(self, v):
        d = self.request([CMD["GetVoiceConfig"], v], CMD["GetVoiceConfig"])
        return nibble_decode(d[2:])

    def reverb_config(self):
        d = self.request([CMD["GetReverbConfig"], 0], CMD["GetReverbConfig"])
        return nibble_decode(d[1:])

    def samples(self, bank):
        d = self.request([CMD["GetSamples"], bank], CMD["GetSamples"])
        raw = bytes(d[2:])
        return [raw[i:i + 8].decode("ascii", "replace").rstrip() for i in range(0, len(raw) - len(raw) % 8, 8)]

    def bar(self, seq, bar):
        d = self.request([CMD["GetSequence"], seq, bar], CMD["GetSequence"])
        info = dict(seq=d[1], beatsPerBar=d[2], bars=d[3], bar=d[4])
        raw = bytes(d[5:5 + BAR_BYTES])
        if len(raw) != BAR_BYTES:
            raise ValueError(f"Seq {seq} bar {bar}: got {len(raw)} bytes, expected {BAR_BYTES}")
        return info, raw


def serial_readout(port, cmds=("help", "midimap")):
    try:
        import serial
    except ImportError:
        return None
    res = {}
    with serial.Serial(port, 230400, timeout=0.2) as s:
        s.reset_input_buffer()
        for c in cmds:
            s.write((c + "\r").encode())
            t0, buf = time.time(), b""
            while time.time() - t0 < 1.5:
                buf += s.read(4096)
            res[c] = buf.decode("ascii", "replace")
    return res


def parse_midimap(text):
    order = {"Kick": 0, "Snare": 1, "Hi Hats": 2, "Sample A": 3, "Sample B": 4, "Toms": 5, "Claps": 6}
    m, chn = [None] * 7, None
    for line in text.splitlines():
        if "Channel:" in line:
            chn = int(line.split("Channel:")[1].strip())
        for k, i in order.items():
            if line.startswith(k) and ":" in line:
                lo, hi = line.split(":")[1].split(",")
                m[i] = (int(lo), int(hi))
    return (m if all(m) else None), chn


def web_editor_json(seq, info, bars_raw):
    data, i = {}, 0
    data[str(i)] = dict(seq=seq, beatsPerBar=info["beatsPerBar"], bars=info["bars"], bar=0, playing=False,
                        playingSeq=0, playingBar=0, playingBeat=0)
    i += 1
    for bar in range(max(1, info["bars"])):
        raw = bars_raw[bar]
        for b in range(info["beatsPerBar"]):
            for v, name in enumerate(VOICES):
                off = (b * len(VOICES) + v) * 2
                data[str(i)] = {"pos": f"{bar}{name}{b}", "level": raw[off], "index": raw[off + 1]}
                i += 1
    return data


def do_backup(args):
    out = args.out or os.path.join(os.getcwd(), "punck_backup_" + datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(os.path.join(out, "raw"), exist_ok=True)
    os.makedirs(os.path.join(out, "web_editor_sequences"), exist_ok=True)

    p = Punck(args.port)
    try:
        st = p.status()
        print("Status:", st)
        if st["playing"]:
            raise SystemExit("Sequencer is playing - stop it first (reading sequences would switch patterns).")

        result = {"created": datetime.datetime.now().isoformat(timespec="seconds"), "status": st,
                  "voices": {}, "samples": {}, "sequences": []}

        voice_raw = {}
        for v, name in enumerate(VOICES):
            raw = p.voice_config(v)
            voice_raw[name] = raw
            open(os.path.join(out, "raw", f"voice{v}_{name}.bin"), "wb").write(raw)
            result["voices"][name] = {"bytes": len(raw), **(decode_struct(name, raw) if name in LAYOUT else {})}
            print(f"  voice {name:9s}: {len(raw)} bytes")

        rev = p.reverb_config()
        open(os.path.join(out, "raw", "reverb.bin"), "wb").write(rev)
        result["reverb"] = {"bytes": len(rev), **decode_struct("Reverb", rev)}
        print(f"  reverb      : {len(rev)} bytes")

        for bank in (0, 1):
            result["samples"]["AB"[bank]] = p.samples(bank)
            print(f"  bank {'AB'[bank]}: {len(result['samples']['AB'[bank]]) - 1} samples (+random)")

        seq_blob = b""
        for s in range(SEQ_COUNT):
            bars_raw, info = [], None
            for b in range(BARS_PER_SEQ):
                info_b, raw = p.bar(s, b)
                info = info or info_b
                bars_raw.append(raw)
                open(os.path.join(out, "raw", f"seq{s + 1}_bar{b + 1}.bin"), "wb").write(raw)
            seq_blob += bytes([info["bars"], info["beatsPerBar"]]) + b"".join(bars_raw)
            hits = sum(1 for raw in bars_raw[:info["bars"]] for k in range(0, BAR_BYTES, 2) if raw[k] > 0)
            result["sequences"].append(dict(seq=s + 1, bars=info["bars"], beatsPerBar=info["beatsPerBar"], hits=hits))
            with open(os.path.join(out, "web_editor_sequences", f"Sequence{s + 1}.json"), "w") as f:
                json.dump(web_editor_json(s, info, bars_raw), f)
            print(f"  sequence {s + 1}: {info['bars']} bar(s) x {info['beatsPerBar']} steps, {hits} hits")

        # Leave the originally active sequence selected again
        p.bar(st["activeSequence"], 0)
    finally:
        p.close()

    midimap, chn = None, None
    if args.com:
        try:
            con = serial_readout(args.com)
            if con:
                open(os.path.join(out, "serial_console.txt"), "w").write(
                    "\n".join(f"===== > {k}\n{v}" for k, v in con.items()))
                midimap, chn = parse_midimap(con.get("midimap", ""))
                result["midimap"] = midimap
                result["midiChannel"] = chn
                result["firmwareHasMidiChannel"] = "midichn" in con.get("help", "")
        except Exception as e:  # serial is optional
            print("Serial readout skipped:", e)

    # Reconstruct the internal flash config block exactly as firmware config version 5 writes it
    if midimap:
        blob = bytearray(b"CFG\x00") + bytes([5, 0, 0, 0])
        for name in FLASH_VOICE_ORDER:
            blob += voice_raw[name]
        blob += rev
        blob += bytes(x for lo_hi in midimap for x in lo_hi)
        blob += seq_blob
        blob += b"END\x00"
        open(os.path.join(out, "flash_config_v5_reconstructed.bin"), "wb").write(blob)
        result["flashConfigBytes"] = len(blob)
        print(f"  reconstructed v5 flash config block: {len(blob)} bytes")

    with open(os.path.join(out, "punck_config.json"), "w") as f:
        json.dump(result, f, indent=1)
    print("Backup written to", out)


def do_restore(args):
    src = args.src
    p = Punck(args.port)
    try:
        if p.status()["playing"]:
            raise SystemExit("Stop the sequencer first.")
        for v, name in enumerate(VOICES):
            fn = os.path.join(src, "raw", f"voice{v}_{name}.bin")
            if os.path.exists(fn) and os.path.getsize(fn) > 0 and name in LAYOUT:
                p.send([CMD["SetVoiceConfig"], v] + nibble_encode(open(fn, "rb").read()))
                print("restored voice", name)
        fn = os.path.join(src, "raw", "reverb.bin")
        if os.path.exists(fn):
            p.send([CMD["SetReverbConfig"]] + nibble_encode(open(fn, "rb").read()))
            print("restored reverb")
        meta = json.load(open(os.path.join(src, "punck_config.json")))
        for s in meta["sequences"]:
            for b in range(BARS_PER_SEQ):
                raw = open(os.path.join(src, "raw", f"seq{s['seq']}_bar{b + 1}.bin"), "rb").read()
                p.send([CMD["SetSequence"], s["seq"] - 1, s["beatsPerBar"], s["bars"], b] + list(raw))
            print("restored sequence", s["seq"])
        if args.save:
            p.send([CMD["SaveConfig"], 0])
            time.sleep(1.0)
            print("SaveConfig sent - settings committed to internal flash")
        else:
            print("Not saved to flash (use --save). Settings are live until power-off.")
    finally:
        p.close()


# ---------------------------------------------------------------------------------------------
# Live voice editing: values take effect immediately, only persist after SaveConfig (--save)
# ---------------------------------------------------------------------------------------------
EDITABLE = {"kick": "Kick", "snare": "Snare", "hihat": "HiHat", "hats": "HiHat", "toms": "Toms",
            "claps": "Claps", "clap": "Claps", "reverb": "Reverb"}


def encode_struct(name, fields, raw):
    """Pack field dict back into the struct layout, keeping any trailing bytes (padding / unknown fields)."""
    out, pos = bytearray(), 0
    for field, fmt in LAYOUT[name]:
        size = struct.calcsize("<" + fmt)
        if pos + size > len(raw):
            break
        val = fields[field]
        vals = val if isinstance(val, list) else [val]
        out += struct.pack("<" + fmt, *[int(v) if fmt.endswith("B") else float(v) for v in vals])
        pos += size
    return bytes(out) + raw[pos:]


def read_cfg(p, name):
    return p.reverb_config() if name == "Reverb" else p.voice_config(VOICES.index(name))


def write_cfg(p, name, raw):
    if name == "Reverb":
        p.send([CMD["SetReverbConfig"]] + nibble_encode(raw))
    else:
        p.send([CMD["SetVoiceConfig"], VOICES.index(name)] + nibble_encode(raw))


def apply_changes(p, changes):
    """changes: {StructName: {field or field[i]: value}}"""
    for name, upd in changes.items():
        raw = read_cfg(p, name)
        fields = decode_struct(name, raw)
        for key, val in upd.items():
            if "[" in key:
                base, idx = key[:-1].split("[")
                fields[base][int(idx)] = val
            elif key not in fields:
                raise SystemExit(f"{name} has no field '{key}'. Fields: {list(fields)}")
            else:
                fields[key] = val
        write_cfg(p, name, encode_struct(name, fields, raw))
        print(f"  {name}: " + ", ".join(f"{k}={v}" for k, v in upd.items()))


def do_show(args):
    p = Punck(args.port)
    try:
        for name in ["Kick", "Snare", "HiHat", "Toms", "Claps", "Reverb"]:
            print(f"{name}:")
            for k, v in decode_struct(name, read_cfg(p, name)).items():
                print(f"    {k:18s} {v}")
    finally:
        p.close()


def finish(p, save):
    if save:
        p.send([CMD["SaveConfig"], 0])
        time.sleep(1.0)
        print("Saved to internal flash.")
    else:
        print("Live only (not saved) - power cycle reverts. Run again with --save to keep.")


def do_set(args):
    changes = {}
    for a in args.assign:
        lhs, val = a.split("=", 1)
        voice, field = lhs.split(".", 1)
        name = EDITABLE.get(voice.lower())
        if not name:
            raise SystemExit(f"Unknown voice '{voice}' (use {sorted(set(EDITABLE))})")
        changes.setdefault(name, {})[field] = json.loads(val)
    p = Punck(args.port)
    try:
        apply_changes(p, changes)
        finish(p, args.save)
    finally:
        p.close()


def do_preset(args):
    preset = json.load(open(args.file, encoding="utf-8"))
    changes = {EDITABLE.get(k.lower(), k): v for k, v in preset.items() if not k.startswith("_")}
    p = Punck(args.port)
    try:
        print(f"Preset {os.path.basename(args.file)}: {preset.get('_description', '')}")
        apply_changes(p, changes)
        finish(p, args.save)
    finally:
        p.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="Mountjoy Punck", help="MIDI port name substring")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("backup")
    b.add_argument("--out", help="output folder")
    b.add_argument("--com", help="serial console port, e.g. COM17 (optional)")
    r = sub.add_parser("restore")
    r.add_argument("src", help="backup folder")
    r.add_argument("--save", action="store_true", help="commit to internal flash afterwards")
    sub.add_parser("show", help="print current voice + reverb settings")
    s = sub.add_parser("set", help="set values live, e.g. kick.initSlowSinFreq=52 'snare.partialInitLevel[1]=0.4'")
    s.add_argument("assign", nargs="+")
    s.add_argument("--save", action="store_true")
    pr = sub.add_parser("preset", help="apply a preset JSON live")
    pr.add_argument("file")
    pr.add_argument("--save", action="store_true")
    sv = sub.add_parser("save", help="commit current live settings to internal flash")
    args = ap.parse_args()
    if args.cmd == "save":
        p = Punck(args.port)
        try:
            finish(p, True)
        finally:
            p.close()
        return
    {"backup": do_backup, "restore": do_restore, "show": do_show, "set": do_set, "preset": do_preset}[args.cmd](args)


if __name__ == "__main__":
    main()
