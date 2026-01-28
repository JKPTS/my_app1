# Single-file firmware update (.fwpack)

This update adds support to upload ONE file to /api/fwupdate:
- Raw app .bin (old behavior)
- Package .fwpack = header + app.bin + spiffs.bin (new behavior)

## Create a .fwpack
1) Build normally:
   idf.py build

2) Find the binaries in ./build
   - app:   build/<project>.bin   (example: build/my_app.bin)
   - spiffs: build/<partition_label>.bin or build/spiffs.bin
     (look for a .bin whose size matches your SPIFFS partition size)

3) Make the package:
   python tools/make_fwpack.py --app build/my_app.bin --spiffs build/storage.bin --out footswitch.fwpack

## Upload
Connect to the device portal (192.168.4.1) and upload:
- footswitch.fwpack  (updates BOTH firmware + web UI files in one shot)

The device will reboot after a successful upload.
