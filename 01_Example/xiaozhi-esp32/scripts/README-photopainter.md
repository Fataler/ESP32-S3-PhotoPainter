# PhotoPainter helper scripts

Run from anywhere:

```bash
/Users/fataler/Documents/Esp/ESP32-S3-PhotoPainter/01_Example/xiaozhi-esp32/scripts/pp-build.sh
/Users/fataler/Documents/Esp/ESP32-S3-PhotoPainter/01_Example/xiaozhi-esp32/scripts/pp-flash.sh
/Users/fataler/Documents/Esp/ESP32-S3-PhotoPainter/01_Example/xiaozhi-esp32/scripts/pp-build-flash.sh
/Users/fataler/Documents/Esp/ESP32-S3-PhotoPainter/01_Example/xiaozhi-esp32/scripts/pp-merge.sh
/Users/fataler/Documents/Esp/ESP32-S3-PhotoPainter/01_Example/xiaozhi-esp32/scripts/pp-monitor.sh
/Users/fataler/Documents/Esp/ESP32-S3-PhotoPainter/01_Example/xiaozhi-esp32/scripts/pp-size.sh
/Users/fataler/Documents/Esp/ESP32-S3-PhotoPainter/01_Example/xiaozhi-esp32/scripts/pp-convert-pngs.py
```

The scripts auto-detect `/dev/cu.usbmodem*`. You can pass the port explicitly:

```bash
scripts/pp-flash.sh /dev/cu.usbmodem1101
```

`pp-merge.sh` creates `build/photopainter_merged.bin` for ESP Launchpad at flash address `0x0`.

Size breakdown:

```bash
scripts/pp-size.sh archives
scripts/pp-size.sh files
```

Convert a folder of PNG files to PhotoPainter BMP files:

```bash
scripts/pp-convert-pngs.py pictures
scripts/pp-convert-pngs.py pictures --size=800x480
scripts/pp-convert-pngs.py pictures --size=1200x1600 --rotate-180
```
