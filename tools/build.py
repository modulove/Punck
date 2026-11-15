#!/usr/bin/env python3
"""
Headless firmware build for Punck using the GNU toolchain bundled with STM32CubeIDE.

Mirrors the CubeIDE 'Debug' configuration (Punck/.cproject):
  C++20 / gnu17, -O1, Cortex-M7 FPv5-D16 hard float, STM32H743xx + DEBUG defines,
  newlib-nano with float printf, linker script STM32H743ZITX_FLASH.ld.

Usage:  python tools/build.py [--clean] [--toolchain DIR] [-j N] [-D NAME=VAL ...]
Output: build/Punck.elf, build/Punck.bin, build/Punck.hex, build/Punck.map
"""
import argparse
import concurrent.futures
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJ = os.path.join(ROOT, "Punck")
BUILD = os.path.join(ROOT, "build")

SRC_DIRS = ["src", "src/usb", "src/voices", "src/dsp", "Drivers/FatFs/src",
            "src/third_party/stmlib/dsp", "src/third_party/stmlib/utils", "src/third_party/plaits"]
INC_DIRS = ["Drivers/CMSIS/Include", "Drivers/CMSIS/Device/ST/STM32H7xx/Include", "src", "src/usb",
            "src/voices", "src/dsp", "Drivers/FatFs/src", "src/third_party"]
DEFINES = ["STM32H743xx", "DEBUG"]

CPU = ["-mcpu=cortex-m7", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-mthumb"]
COMMON = CPU + ["-O1", "-g3", "-ffunction-sections", "-fdata-sections", "-Wall", "--specs=nano.specs",
                "-Wno-unused-variable", "-Wno-psabi", "-Wno-stringop-truncation"]
CFLAGS = ["-std=gnu17"]
CXXFLAGS = ["-std=gnu++20", "-fno-exceptions", "-fno-rtti", "-fno-use-cxa-atexit", "-Wno-volatile",
            "-Wno-register"]
LDFLAGS = CPU + ["-T" + os.path.join(PROJ, "STM32H743ZITX_FLASH.ld"), "--specs=nosys.specs",
                 "--specs=nano.specs", "-u", "_printf_float", "-Wl,--gc-sections", "-static",
                 "-Wl,--start-group", "-lc", "-lm", "-lstdc++", "-lsupc++", "-Wl,--end-group"]


def find_toolchain(explicit):
    if explicit:
        return explicit
    on_path = shutil.which("arm-none-eabi-gcc")
    if on_path:
        return os.path.dirname(on_path)
    cands = sorted(glob.glob(r"C:\ST\STM32CubeIDE_*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu."
                             r"externaltools.gnu-tools-for-stm32.*win32*\tools\bin"))
    if not cands:
        sys.exit("arm-none-eabi toolchain not found (pass --toolchain)")
    return cands[-1]


# Same exclusions as the CubeIDE project (.cproject sourceEntries)
EXCLUDE = {"sysmem.c", "SerialHandler.cpp", "USB_bu.cpp", "sdram_diskio.cpp", "test.s", "diskio.c",
           "ff_gen_drv.c"}


def sources():
    out = []
    for d in SRC_DIRS:
        for ext in ("*.c", "*.cpp", "*.cc", "*.s", "*.S"):
            out += [f for f in glob.glob(os.path.join(PROJ, d, ext)) if os.path.basename(f) not in EXCLUDE]
    return sorted(set(os.path.normcase(f) for f in out))     # Windows globs are case-insensitive


def obj_path(src):
    rel = os.path.relpath(src, PROJ)
    return os.path.join(BUILD, "obj", os.path.splitext(rel)[0] + ".o")


def needs_build(src, obj):
    if not os.path.exists(obj):
        return True
    dep = obj[:-2] + ".d"
    t_obj = os.path.getmtime(obj)
    files = [src]
    if os.path.exists(dep):
        txt = open(dep).read().replace("\\\n", " ")
        files += [f for f in txt.split(":", 1)[-1].split() if os.path.exists(f)]
    return any(os.path.getmtime(f) > t_obj for f in files)


def compile_one(tc, src, extra_defs):
    obj = obj_path(src)
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    incs = ["-I" + os.path.join(PROJ, i) for i in INC_DIRS]
    defs = ["-D" + d for d in DEFINES + extra_defs]
    if src.endswith((".cpp", ".cc")):
        cmd = [os.path.join(tc, "arm-none-eabi-g++")] + COMMON + CXXFLAGS
    elif src.endswith((".s", ".S")):
        cmd = [os.path.join(tc, "arm-none-eabi-gcc")] + CPU + ["-x", "assembler-with-cpp", "-g3"]
    else:
        cmd = [os.path.join(tc, "arm-none-eabi-gcc")] + COMMON + CFLAGS
    cmd += defs + incs + ["-MMD", "-MP", "-c", src, "-o", obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return src, r.returncode, (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--toolchain")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("-D", action="append", default=[], help="extra define")
    args = ap.parse_args()

    tc = find_toolchain(args.toolchain)
    if args.clean and os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
    os.makedirs(BUILD, exist_ok=True)

    srcs = sources()
    todo = [s for s in srcs if needs_build(s, obj_path(s))]
    print(f"toolchain: {tc}\n{len(srcs)} sources, {len(todo)} to compile")
    failed = False
    with concurrent.futures.ThreadPoolExecutor(args.j) as ex:
        for src, rc, msg in ex.map(lambda s: compile_one(tc, s, args.D), todo):
            rel = os.path.relpath(src, PROJ)
            if msg:
                print(f"--- {rel}\n{msg}")
            if rc:
                failed = True
                print(f"FAILED: {rel}")
    if failed:
        sys.exit(1)

    elf = os.path.join(BUILD, "Punck.elf")
    objs = [obj_path(s) for s in srcs]
    r = subprocess.run([os.path.join(tc, "arm-none-eabi-g++")] + objs + LDFLAGS +
                       ["-Wl,-Map=" + os.path.join(BUILD, "Punck.map"), "-o", elf],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("LINK FAILED")
    for fmt, ext in (("binary", "bin"), ("ihex", "hex")):
        subprocess.run([os.path.join(tc, "arm-none-eabi-objcopy"), "-O", fmt, elf,
                        os.path.join(BUILD, "Punck." + ext)], check=True)
    print(subprocess.run([os.path.join(tc, "arm-none-eabi-size"), elf], capture_output=True,
                         text=True).stdout)


if __name__ == "__main__":
    main()
