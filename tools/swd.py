#!/usr/bin/env python3
"""
Punck SWD helper: OpenOCD (bundled with STM32CubeIDE) + any CMSIS-DAP probe (Pico debugprobe).

  python tools/swd.py probe                 # connect, show chip ID + read protection level (does not halt)
  python tools/swd.py backup OUTDIR         # halt, dump complete 2 MB internal flash twice, compare, resume
  python tools/swd.py flash build/Punck.elf # program + verify + software reset
  python tools/swd.py restore FILE.bin      # write a full flash dump back to 0x08000000 + verify + reset

Wiring (J6 on component board): pin2 GND, pin3 SWDIO -> Pico GP3, pin4 SWCLK -> Pico GP2. Leave pin1 (3V3) open.
"""
import argparse
import glob
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, "tools", "openocd", "punck_cmsis_dap.cfg")
FLASH_BASE = 0x08000000
FLASH_SIZE = 0x200000                      # 2 MB (bank 1 + bank 2)


def find_openocd():
    exe = sorted(glob.glob(r"C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools."
                           r"openocd.win32_*\tools\bin\openocd.exe"))
    scripts = sorted(glob.glob(r"C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.debug."
                               r"openocd_*\resources\openocd\st_scripts"))
    if not exe or not scripts:
        sys.exit("OpenOCD from STM32CubeIDE not found")
    return exe[-1], scripts[-1]


def openocd(*cmds, timeout=300):
    exe, scripts = find_openocd()
    args = [exe, "-s", scripts, "-f", CFG]
    for c in cmds:
        args += ["-c", c]
    args += ["-c", "shutdown"]
    r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    out = r.stdout + r.stderr
    return r.returncode, out


def path_tcl(p):
    return os.path.abspath(p).replace("\\", "/")


def mdw(out, addr):
    m = re.search(r"0x%08x:\s+([0-9a-f]{8})" % addr, out)
    return int(m.group(1), 16) if m else None


def cmd_probe(_):
    rc, out = openocd("init", "mdw 0x5C001000", "mdw 0x5200201C", "mdw 0x08000000 2", "mdw 0x081E0000 2")
    print("\n".join(l for l in out.splitlines() if l.startswith(("Info", "Warn", "Error")))[-3000:])
    idcode, optsr = mdw(out, 0x5C001000), mdw(out, 0x5200201C)
    if idcode is None:
        sys.exit("No target response - check wiring (GND, SWDIO, SWCLK) and that the module is powered")
    print(f"\nDBGMCU_IDCODE = 0x{idcode:08x} (dev id 0x{idcode & 0xFFF:03x}, 0x450 = STM32H74x/75x)")
    if optsr is not None:
        rdp = (optsr >> 8) & 0xFF
        print(f"FLASH_OPTSR   = 0x{optsr:08x}  RDP = 0x{rdp:02x} "
              f"({'level 0 - readable' if rdp == 0xAA else 'level 2 - LOCKED' if rdp == 0xCC else 'level 1 - read protected'})")
    sp, cfg = mdw(out, 0x08000000), mdw(out, 0x081E0000)
    print(f"Vector table SP = 0x{sp:08x}" if sp is not None else "")
    if cfg is not None:
        print(f"Config sector header = 0x{cfg:08x} ({bytes.fromhex(f'{cfg:08x}')[::-1]!r})")


def sha256(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


CHUNK = 0x2000                             # 8 KB chunks; a failed chunk is simply re-read


def dump_pass(tmpdir, tag):
    """Read the complete flash in 64 KB chunks through AP0 (module keeps running), retrying failed chunks."""
    prefix = os.path.join(tmpdir, tag)
    rc, out = openocd("init", f"punck_dump {{{path_tcl(prefix)}}} 0x{FLASH_BASE:08x} 0x{FLASH_SIZE:x} 0x{CHUNK:x} 40",
                      timeout=1800)
    n = FLASH_SIZE // CHUNK
    files = [f"{prefix}_{i:04d}.bin" for i in range(n)]
    if "DUMPOK" not in out or not all(os.path.exists(f) and os.path.getsize(f) == CHUNK for f in files):
        return None, out
    data = b"".join(open(f, "rb").read() for f in files)
    for f in files:
        os.remove(f)
    return data, out


def cmd_backup(a):
    os.makedirs(a.outdir, exist_ok=True)
    import tempfile
    tmp = tempfile.mkdtemp(prefix="punck_swd_")      # local temp, not the (Drive-synced) output folder
    os.makedirs(tmp, exist_ok=True)
    d1, out1 = dump_pass(tmp, "pass1")
    d2, out2 = dump_pass(tmp, "pass2") if d1 else (None, "")
    open(os.path.join(a.outdir, "openocd_backup.log"), "w").write(out1 + "\n\n" + out2)
    if d1 is None or d2 is None:
        print((out1 + out2)[-2500:])
        sys.exit("Dump failed (see openocd_backup.log)")
    if d1 != d2:
        diffs = [i for i in range(0, FLASH_SIZE, CHUNK) if d1[i:i + CHUNK] != d2[i:i + CHUNK]]
        sys.exit(f"Two reads differ in chunks at offsets {[hex(x) for x in diffs]} - link unreliable")
    os.rmdir(tmp)
    f1 = os.path.join(a.outdir, "punck_internal_flash_2MB.bin")
    open(f1, "wb").write(d1)
    h1 = sha256(f1)
    data = d1
    open(os.path.join(a.outdir, "fw_bank1_sector0.bin"), "wb").write(data[:0x20000])
    open(os.path.join(a.outdir, "config_bank2_sector7.bin"), "wb").write(data[0x1E0000:0x200000])
    with open(os.path.join(a.outdir, "SHA256.txt"), "w") as f:
        for n in ("punck_internal_flash_2MB.bin", "fw_bank1_sector0.bin", "config_bank2_sector7.bin"):
            f.write(f"{sha256(os.path.join(a.outdir, n))}  {n}\n")
    print(f"Backup OK (two identical reads), sha256 {h1}\nWritten to {a.outdir}")


def cmd_watch(_):
    """Live wiring check: connect every ~2 s, report connection and read quality (Ctrl+C to stop)."""
    import time
    probe_reads = " ; ".join(
        "if {[catch {stm32h743.ap0 read_memory 0x08002000 32 256}]} {echo RFAIL} else {echo ROK}" for _ in range(5))
    print("Watching SWD link - adjust wiring, Ctrl+C to stop")
    while True:
        try:
            rc, out = openocd("init", "catch {punck_dbg_clocks}", probe_reads, timeout=30)
        except subprocess.TimeoutExpired:
            out = ""
        stamp = time.strftime("%H:%M:%S")
        if "DPIDR 0x6ba02477" not in out:
            print(f"{stamp}  NO RESPONSE  - check GND / pin positions / SWDIO-SWCLK order", flush=True)
        else:
            ok = out.count("ROK")
            verdict = "GOOD - ready" if ok == 5 else "CONNECTED but unreliable (noise)"
            print(f"{stamp}  CONNECTED    reads {ok}/5 ok   {verdict}", flush=True)
        time.sleep(1)


def cmd_flash(a):
    rc, out = openocd("init", "halt", f"program {{{path_tcl(a.elf)}}} verify", "reset run", timeout=600)
    print("\n".join(l for l in out.splitlines() if re.search(r"(Info|Warn|Error|wrote|verified|Programming)", l))[-3000:])
    if "Verified OK" not in out and "verified" not in out.lower():
        sys.exit("FLASH/VERIFY FAILED - module still has its previous firmware unless erase started; restore with backup")
    print("Flash + verify OK, module restarted")


def cmd_restore(a):
    rc, out = openocd("init", "halt", f"program {{{path_tcl(a.bin)}}} 0x{FLASH_BASE:08x} verify", "reset run", timeout=900)
    print("\n".join(l for l in out.splitlines() if re.search(r"(Info|Warn|Error|wrote|verified)", l))[-3000:])
    if "verified" not in out.lower():
        sys.exit("RESTORE/VERIFY FAILED")
    print("Restore + verify OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("probe")
    sub.add_parser("watch")
    b = sub.add_parser("backup"); b.add_argument("outdir")
    f = sub.add_parser("flash"); f.add_argument("elf")
    r = sub.add_parser("restore"); r.add_argument("bin")
    a = ap.parse_args()
    {"probe": cmd_probe, "watch": cmd_watch, "backup": cmd_backup, "flash": cmd_flash, "restore": cmd_restore}[a.cmd](a)


if __name__ == "__main__":
    main()
