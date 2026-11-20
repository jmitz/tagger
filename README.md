# Tagger

A filament spool tagger with a touchscreen, two NFC readers and a scale. It is
built on [OpenSpool](https://github.com/spuder/OpenSpool) and runs on a
Waveshare ESP32‑S3 3.5" touch LCD board.

- Two **PN532** NFC readers read and write OpenSpool NFC tags (NTAG215/216).
- An **HX711** load cell weighs the spool.
- The 3.5" touchscreen (480×320, mounted landscape) shows reader and scale status.

> **Status:** the ESPHome firmware runs on the device. The touchscreen shows both
> readers and the scale weight, and has a Tare button. OpenSpool tags are decoded
> and displayed. The Bambu printer link is not done yet. Detailed findings are
> in [agent.md](agent.md).

## Hardware

| Part | Notes |
|---|---|
| Waveshare ESP32‑S3‑Touch‑LCD‑3.5 (non‑B: ST7796 + FT6336) | 16 MB flash, 8 MB PSRAM, USB‑C |
| 2 × Elechouse PN532 NFC Module V3 | Set to **SPI** mode |
| HX711 + load cell | |

## Wiring (SPI)

The header pin numbers follow the numbering in
[documents/ESP32-S3-Touch-LCD-3.5B-details-intro.jpg](documents/ESP32-S3-Touch-LCD-3.5B-details-intro.jpg).

| Connection | ESP32 GPIO | Header pin |
|---|---|---|
| SCK → both readers | GPIO11 | 10 |
| MOSI → both readers | GPIO10 | 12 |
| MISO → both readers | GPIO9 | 14 |
| Reader 1 SS | GPIO17 | 16 |
| Reader 2 SS | GPIO18 | 18 |
| Reader 1 RSTO *(recommended)* | GPIO39 | 9 |
| Reader 2 RSTO *(recommended)* | GPIO40 | 11 |
| HX711 DT | GPIO21 | 5 |
| HX711 SCK | GPIO38 | 7 |
| 3.3 V | — | 31, 32 |
| GND | — | 29, 30 |

**Before powering up:**
- **Set each PN532 to SPI mode:** switch 1 OFF, switch 2 ON. The module reads
  its switches only at power-up, so unplug USB for 5 s after changing them.
- **Power everything from 3.3 V.** Don't use the 5V or BAT header pins.
- **Leave the microSD slot empty.** It shares the SPI pins.

## Repository layout

```
agent.md              Findings, decisions and next steps (for agents and maintainers)
TODO.md               Active to-do list
esphome/              Device firmware: tagger.yaml, tagger_tags.h, secrets template
documents/            Board pinout image and PN532 module photos
firmware-backup/      Factory flash checksum and boot log (the 16 MB image is local only)
tools/
  pn532_hsu_probe/    PlatformIO diagnostics for HSU/UART, I²C and line levels
  pn532_i2c_test/     PlatformIO dual-reader I²C test and soak test
  system_check/       PlatformIO pass/fail hardware check (SPI readers, HX711, I²C)
  spi_diag/           Low-level SPI/HX711 wiring diagnostics
  scale_cal/          HX711 calibration stream (with scale_cal.py)
```

## Building and flashing the firmware

The device firmware is [ESPHome](https://esphome.io/), in `esphome/`:

```bash
python3 -m venv venv && venv/bin/pip install esphome platformio esptool pyserial
cp esphome/secrets.example.yaml esphome/secrets.yaml   # then fill in Wi-Fi and OTA password
venv/bin/esphome run esphome/tagger.yaml --device /dev/ttyACM0
```

On screen: two reader cards (left = reader 1, right = reader 2) and the scale
with a **Tare** button. Tare whenever the empty-scale reading drifts.
For an OpenSpool tag, a card shows the brand, material, colour and print
temperatures. Other tags show their UID.

## Building and flashing the test tools

The tools use [PlatformIO](https://platformio.org/):

```bash
python3 -m venv venv && venv/bin/pip install platformio esptool pyserial
cd tools/pn532_i2c_test
../../venv/bin/pio run -t upload --upload-port /dev/ttyACM0
../../venv/bin/pio device monitor
```

If the upload doesn't start, put the board into download mode: hold **BOOT**,
tap **RESET**, then release **BOOT**.

## Restoring the factory firmware

```bash
sha256sum -c firmware-backup/factory_backup_16MB.bin.sha256
esptool --port /dev/ttyACM0 write-flash 0 firmware-backup/factory_backup_16MB.bin
```

## Roadmap

1. ~~Rewire the readers to SPI and soak-test both readers~~ (done)
2. ~~ESPHome firmware with touchscreen UI for tag status and spool weight~~ (done)
3. Two-tag spool capture with one Send weight button (see [TODO.md](TODO.md)).
4. Verify OpenSpool tags end to end with NTAG215s.
5. Write tags from the touchscreen.
