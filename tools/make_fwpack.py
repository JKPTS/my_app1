#!/usr/bin/env python3
# tools/make_fwpack.py
# Build a single-file update package for the ESP32 OTA endpoint (/api/fwupdate).
# Format:
#   fwpack_hdr_t (20 bytes) + app.bin + spiffs.bin
#
# Header (little-endian):
#   magic[4]   = "FWPK"
#   ver        = 1
#   app_len    = len(app.bin)
#   spiffs_len = len(spiffs.bin)
#   flags      = 0
#
import argparse, struct, sys
from pathlib import Path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True, help="path to app image .bin (e.g. build/my_app.bin)")
    ap.add_argument("--spiffs", required=True, help="path to SPIFFS image .bin (e.g. build/storage.bin)")
    ap.add_argument("--out", required=True, help="output .fwpack file (e.g. build/footswitch.fwpack)")
    args = ap.parse_args()

    app_p = Path(args.app)
    sp_p  = Path(args.spiffs)
    out_p = Path(args.out)

    if not app_p.is_file():
        print(f"ERROR: app not found: {app_p}", file=sys.stderr); return 2
    if not sp_p.is_file():
        print(f"ERROR: spiffs not found: {sp_p}", file=sys.stderr); return 2

    app = app_p.read_bytes()
    sp  = sp_p.read_bytes()

    hdr = struct.pack("<4sIIII", b"FWPK", 1, len(app), len(sp), 0)

    out_p.parent.mkdir(parents=True, exist_ok=True)
    out_p.write_bytes(hdr + app + sp)

    print("OK")
    print(f"  app   : {app_p} ({len(app)} bytes)")
    print(f"  spiffs: {sp_p} ({len(sp)} bytes)")
    print(f"  out   : {out_p} ({out_p.stat().st_size} bytes)")

    return 0

if __name__ == "__main__":
    raise SystemExit(main())
