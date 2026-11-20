# agent.md — Tagger project notes for coding agents

Read this before touching hardware or firmware. It records what has been
**measured on the real device**, what was ruled out, and the decisions taken.
Dates are when the finding was made.

## Project

Spool tagger based on **OpenSpool** (`github.com/spuder/OpenSpool`, ESPHome
YAML) running on a Waveshare ESP32‑S3 3.5" touch LCD board, with **two PN532
NFC readers** and an **HX711 load-cell amplifier**.

**Decision (2026‑11‑05): the PN532 readers move to SPI.** I²C works today but is
an interim setup only (see "I²C findings" for why).

## Target board

| Item | Value |
|---|---|
| Board | Waveshare ESP32‑S3‑Touch‑LCD‑3.5 **family** — variant unresolved, see below |
| SoC | ESP32‑S3 (QFN56) rev v0.2, 240 MHz dual core, Wi‑Fi + BLE 5 |
| Memory | 16 MB QIO flash, 8 MB octal PSRAM |
| USB | Native USB‑Serial/JTAG, `/dev/ttyACM0`, VID:PID `303a:1001` |
| MAC | `44:bd:8d:61:24:08` |
| Onboard I²C (GPIO8 SDA / GPIO7 SCL) | `0x18` ES8311, `0x20` TCA9554, `0x34` AXP2101, `0x38` FT6336 touch, `0x51` PCF85063, `0x6B` QMI8658, `0x7E` unidentified |

**Variant question (unresolved).** The user identifies the board as the
**3.5B**. Evidence says it behaves like the **non‑B 3.5**: the factory firmware
drives an **ST7796** SPI LCD + **FT6336** touch, and touch answers at `0x38`.
The 3.5B instead uses an **AXS15231B** QSPI LCD (GPIO1–5, CS GPIO12, BL GPIO6).
This only affects the display config; the expansion header GPIOs are identical
on both variants. Confirm before writing the display section.

Schematics/examples: `github.com/waveshareteam/ESP32-S3-Touch-LCD-3.5` and
`…/ESP32-S3-Touch-LCD-3.5B`.

### Header pin numbering — important

The user wires by the numbering in
`documents/ESP32-S3-Touch-LCD-3.5B-details-intro.jpg`. That numbering is
**odd/even swapped relative to the schematic's J8**. Always quote header pins
in the *image* numbering.

| Image pin | Signal | | Image pin | Signal |
|---|---|---|---|---|
| 1 | BAT | | 2 | 5V |
| 3 | GND | | 4 | GND |
| 5 | GPIO21 | | 6 | GPIO19 (USB D−) |
| 7 | GPIO38 | | 8 | GPIO20 (USB D+) |
| 9 | GPIO39 | | 10 | GPIO11 (SD CLK) |
| 11 | GPIO40 | | 12 | GPIO10 (SD CMD) |
| 13 | GPIO41 | | 14 | GPIO9 (SD D0) |
| 15 | GPIO42 | | 16 | GPIO17 |
| 17 | GPIO45 (strapping) | | 18 | GPIO18 |
| 19 | GPIO46 (strapping) | | 20 | GPIO0 (BOOT) |
| 21 | GPIO47 | | 22 | RST |
| 23 | GPIO48 | | 24 | PWR |
| 25 | GPIO43 TXD | | 26 | GPIO7 SCL (onboard bus) |
| 27 | GPIO44 RXD | | 28 | GPIO8 SDA (onboard bus) |
| 29 | GND | | 30 | GND |
| 31 | 3V3 | | 32 | 3V3 |

GPIO17/18/21/38–42/45–48 are camera-connector pins; no camera is fitted, so
they are free. Avoid GPIO0/45/46 (boot strapping).

## Target wiring — SPI (adopted)

| Signal | GPIO | Image pin | Goes to |
|---|---|---|---|
| SPI SCK | GPIO11 | 10 | both PN532 SCK |
| SPI MOSI | GPIO10 | 12 | both PN532 MOSI |
| SPI MISO | GPIO9 | 14 | both PN532 MISO |
| PN532 #1 SS | GPIO17 | 16 | reader 1 SS |
| PN532 #2 SS | GPIO18 | 18 | reader 2 SS |
| PN532 #1 RSTPDN (recommended) | GPIO39 | 9 | reader 1 **RSTPDN** pad (side row, labelled on back) |
| PN532 #2 RSTPDN (recommended) | GPIO40 | 11 | reader 2 **RSTPDN** pad (side row, labelled on back) |
| HX711 DT | GPIO21 | 5 | HX711 DT |
| HX711 SCK | GPIO38 | 7 | HX711 SCK |
| 3V3 | — | 31, 32 | all VCC |
| GND | — | 29, 30, 3, 4 | all GND |

Rules:
- **All peripherals on 3.3 V.** Never 5V (pin 2) or BAT (pin 1); S3 GPIOs are not 5 V tolerant.
- **PN532 switches for SPI: switch 1 OFF, switch 2 ON** (module table `SPI 0 - 1`;
  `I2C 1 - 0` was verified as switch 1 ON / switch 2 OFF).
- **PN532 reads its mode switches only at power-up.** An ESP reset over USB does
  *not* power-cycle the readers. Unplug USB ≥5 s after any switch change.
  The `/dev/ttyACM0` mtime shows when the board last enumerated.
- **Keep the SD slot empty.** GPIO9/10/11 are the SD card lines.
- Optional 10 kΩ pull-ups SS→3V3 keep readers deselected during boot.
- Use the module's 8-pin header (SCK, MISO, MOSI, SS, VCC, GND, IRQ, RSTO).
- **RSTO on the 8-pin header is NOT the reset input** (verified 2026‑11‑19: holding it
  low does not reset the reader; it is most likely RSTOUT). The reset input is the
  **RSTPDN** pad on the 10-hole side row. Never drive a GPIO wired to RSTO.

## Findings (2026‑11‑05)

### PN532 modules
- Both are **Elechouse "NFC MODULE V3"** (photos in `documents/`). 4-pin header:
  front `SCL SDA VCC GND`, back `RXD TXD VCC GND`. Mode table
  `HSU 0-0, I2C 1-0, SPI 0-1`.
- Module 2 has visible rework (glued PWR LED and SOT‑23 near header, scorched
  header joints). It nevertheless **works** on I²C.
- Both report **PN532 firmware 1.6**, support `0x07`.

### HSU (UART) — ruled out
Both modules were **completely silent** in HSU: switches both OFF and both ON,
power-cycled, both TX/RX orientations, baud 9600–1 288 000, four wake-up
variants; ESP TX verified toggling on the pin. Do not spend more time on HSU
with these modules. (ESPHome also has no `pn532_uart` component.)

### I²C — works, interim only
Verified wiring: reader 1 SDA GPIO18 / SCL GPIO17 (own bus, I²C1); reader 2 on
the onboard bus SDA GPIO8 / SCL GPIO7 (I²C0), 100 kHz.
10-minute soak: **3077 polls per reader, 0 errors**, tag add/remove events all
correct on both readers.

Problems that drove the move to SPI:
- A PN532 **held the onboard SCL low** after the ESP was reset mid-transaction.
  That took out touch, PMU, RTC and IMU until a USB power-cycle. Bus-recovery
  clocking cannot fix a stuck SCL.
- Only two I²C controllers exist, and both PN532s are fixed at `0x24`, so one
  reader must share the onboard bus.

### Two readers: RF field must be switched off after each poll (applies to SPI too)
After `InListPassiveTarget` the PN532 **leaves its RF field on**. The next
reader then sits "busy" forever, because PN532 RF collision avoidance waits for
the external field to drop. Symptom: the second-polled reader always times out,
and the failure follows the poll order, not the module.
**Fix:** after every poll send `RFConfiguration` item 1 = off
(`D4 32 01 00`). With this in place: 0 errors.
In ESPHome, check whether `pn532` already turns the RF field off after each
`update()`. If it doesn't, add it via an external component or a lambda.

### Tag seen in testing
UID `058DDEFEFFD200`, SAK `0x20` (ISO 14443‑4, not an NTAG). OpenSpool expects
**NTAG215/216** (SAK `0x00`) carrying an NDEF `application/json` record with
`"protocol":"openspool"`.

### SPI — verified (2026‑11‑19)
Two separate faults had to be fixed: the PN532 mode switches were wrong (the
user corrected them; the peripheral-independent bit-banged test in
`tools/spi_diag` showed MISO stuck high before that), **and** the firmware bug
below. After both fixes, `tools/system_check`: both readers answer on the shared bus (SCK 11, MOSI 10,
MISO 9, SS 17/18), firmware 1.6, 0 poll errors, with RF-field-off after each
poll. Onboard I²C (all 6 devices) still healthy. HX711: see below.

Tag reading over SPI verified: MIFARE Classic 1K (UID `A140FEA2`, SAK `0x08`)
read on both readers, 376 polls each, 0 errors. (Not an OpenSpool tag — NTAG215
still needed for NDEF tests.)

HX711 verified (2026‑11‑19): DT GPIO21 / SCK GPIO38, idle about −110 000 counts,
noise ~150–270 counts p‑p, ~±80 000 counts under hand pressure, returns to zero.
Earlier it read exactly +8 388 607 (full scale) on every gain *and* channel B.
The cause was the load cell's **wire pairs mixed** (one excitation and one signal wire
swapped). Identify pairs by resistance: the two pairs read highest (~1 kΩ),
cross-pair readings are lower (~750–900 Ω). Calibration (tare + scale factor)
not done yet; the sign of the reading may need inverting.

**Calibration (2026‑11‑19, after the load cell was screwed down):** tare
−82 537.5 counts, **441.888 counts/g** (positive sign), reference 1188 g,
noise-limited resolution about 0.04 g (1 s average), full scale about 19 kg. Verified:
1188 g reads 1188.1 g. Values in `tools/scale_cal/calibration.json`; the
procedure is `tools/scale_cal` firmware + `tools/scale_cal.py tare|weigh|check`.
The zero shifted by about 920 counts (≈2 g) once while the cell wasn't fixed
down, so the firmware must offer **tare on demand**. Don't hard-code the tare.
ESPHome: `hx711` + `filters: - calibrate_linear: [-82537.5 -> 0, 442425.3 -> 1188]`.

**System check status: 6 PASS, 2 WARN (RSTPDN not wired), 0 FAIL.**

**Firmware gotcha (arduino-esp32):** calling `pinMode()` on an SPI pin after
`SPI.begin()` detaches it from the SPI peripheral, and a second `SPI.begin()` is
a no-op. The symptom is the peripheral reading 0x00 for every byte, which looks
exactly like "reader not answering". Measure pin levels *before* `SPI.begin()`.
`system_check` now self-tests this (idle read must be 0xFF).

## Recommendations / next steps
1. Rewire both PN532s to SPI per the table above, and set the switches to SPI.
2. Wire RSTPDN to GPIO39/40 so firmware can hard-reset a hung reader.
3. Reconnect the HX711 on GPIO21/38.
4. Write a quick SPI dual-reader test (port `tools/pn532_i2c_test`: same frames.
   PN532 SPI is **LSB-first, mode 0, ≤5 MHz**, with the status/data-write/
   data-read prefix bytes 0x02/0x01/0x03). Soak-test it before ESPHome.
5. Build the ESPHome config from OpenSpool's `firmware/` packages, keeping:
   - `spi:` clk GPIO11, mosi GPIO10, miso GPIO9; `pn532_spi` cs GPIO17 / GPIO18
   - RF-field-off after every poll
   - `hx711` dout GPIO21, clk GPIO38
   - onboard `i2c:` GPIO8/7 for touch/PMU/IO expander. The display driver
     depends on the variant question above.
   - logger on USB‑Serial/JTAG (GPIO43/44 stay free)
   OpenSpool's own S3 pin map (SPI 11/12/13, CS 4–7) **clashes with this board**
   and must not be used as-is.

## Tooling
- No `pio`/`esphome` on PATH. Use the project venv (`venv/`, gitignored) with
  `platformio`, `esptool`, `pyserial`. Recreate with
  `python3 -m venv venv && venv/bin/pip install platformio esptool pyserial`.
- `~/.platformio` already holds the espressif32 platform and toolchains.
- `tools/pn532_hsu_probe/`: HSU/I²C/line diagnostics (historical).
- `tools/pn532_i2c_test/`: dual-reader I²C test with bus recovery,
  RF-field-off and stats (historical).
- `tools/system_check/`: **current** PASS/FAIL checklist for the SPI build:
  onboard I²C, SPI MISO self-test, both PN532s, RSTPDN wiring, HX711, then a
  soak loop with tag events and stats. **This is what's flashed on the board.**
- `tools/spi_diag/`: low-level wiring diagnostics (pin levels, connectivity
  matrix, bit-banged PN532 SPI, SPI mode sweep, HX711 swap detection).
- `tools/serial_capture.py SECONDS [PORT] [--noreset]`: resets the board and
  streams serial output (run with `venv/bin/python`).
- Restore factory firmware: `esptool --port /dev/ttyACM0 write-flash 0
  firmware-backup/factory_backup_16MB.bin` (image is local only, not in git;
  verify with `sha256sum -c firmware-backup/factory_backup_16MB.bin.sha256`).
