#!/usr/bin/env python3
"""
Send commands to the Punck serial console (USB CDC) and print the replies.

  python tools/punck_console.py COM17 help
  python tools/punck_console.py COM17 format --until "Successfully formatted|Failed to format" --timeout 90
  python tools/punck_console.py COM17 flushcache cacheinfo samplelist

Each command waits until the reply has been quiet for --idle seconds, or until the --until regex
matches (then returns immediately), or --timeout expires. Exit code 2 if --until never matched.
"""
import argparse
import re
import sys
import time

import serial


def run(port, cmds, until=None, timeout=10.0, idle=0.8):
    ok = True
    with serial.Serial(port, 230400, timeout=0.1, write_timeout=2) as s:
        s.reset_input_buffer()
        for c in cmds:
            s.write((c + "\r").encode())
            t0 = last = time.time()
            buf = b""
            while True:
                chunk = s.read(4096)
                if chunk:
                    buf += chunk
                    last = time.time()
                    sys.stdout.write(chunk.decode("ascii", "replace"))
                    sys.stdout.flush()
                text = buf.decode("ascii", "replace")
                if until and re.search(until, text):
                    break
                if not until and buf and time.time() - last > idle:
                    break
                if time.time() - t0 > timeout:
                    if until:
                        ok = False
                    break
            print()
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("commands", nargs="+")
    ap.add_argument("--until", help="regex that ends waiting for each command")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--idle", type=float, default=0.8)
    a = ap.parse_args()
    sys.exit(0 if run(a.port, a.commands, a.until, a.timeout, a.idle) else 2)


if __name__ == "__main__":
    main()
