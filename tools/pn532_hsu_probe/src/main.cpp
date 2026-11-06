// PN532 HSU bring-up probe for the Waveshare ESP32-S3-Touch-LCD-3.5(B).
// Tries both TX/RX orientations on GPIO17/GPIO18, reports the PN532 firmware
// version, configures the SAM, then polls for ISO14443A tags and prints UIDs.
#include <Arduino.h>
#include <Wire.h>
#include "driver/gpio.h"

static const int PIN_A = 17;
static const int PIN_B = 18;
static const uint32_t BAUD = 115200;

HardwareSerial pn(1);

static void hexdump(const char *tag, const uint8_t *d, size_t n) {
  Serial.printf("%s (%u):", tag, (unsigned)n);
  for (size_t i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
  Serial.println();
}

static void wakeup() {
  // HSU wake-up: 0x55 followed by a run of zeros long enough to cover the
  // PN532's oscillator start-up (see UM0701-02 section 7.2.11).
  uint8_t w[24] = {0x55, 0x55, 0x00, 0x00, 0x00};
  pn.write(w, sizeof(w));
  pn.flush();
  delay(10);
  while (pn.available()) pn.read();
}

static void sendFrame(const uint8_t *cmd, uint8_t len) {
  uint8_t f[64];
  size_t n = 0;
  uint8_t l = len + 1;  // TFI + data
  f[n++] = 0x00; f[n++] = 0x00; f[n++] = 0xFF;
  f[n++] = l; f[n++] = (uint8_t)(0x100 - l);
  f[n++] = 0xD4;
  uint8_t sum = 0xD4;
  for (uint8_t i = 0; i < len; i++) { f[n++] = cmd[i]; sum += cmd[i]; }
  f[n++] = (uint8_t)(0x100 - sum);
  f[n++] = 0x00;
  while (pn.available()) pn.read();
  pn.write(f, n);
  pn.flush();
}

static size_t readFor(uint8_t *buf, size_t max, uint32_t ms) {
  size_t n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < ms && n < max) {
    if (pn.available()) buf[n++] = pn.read();
    else delay(1);
  }
  return n;
}

// Sends a command and returns the response payload (after D5 cmd+1), or -1.
static int transact(const uint8_t *cmd, uint8_t len, uint8_t *out, size_t outMax,
                    uint32_t ms, bool verbose) {
  sendFrame(cmd, len);
  uint8_t buf[128];
  size_t n = readFor(buf, sizeof(buf), ms);
  if (verbose) hexdump("  rx", buf, n);
  // Find a response frame: 00 FF LEN LCS D5 (cmd+1) ...
  for (size_t i = 0; i + 6 < n; i++) {
    if (buf[i] == 0x00 && buf[i + 1] == 0xFF && buf[i + 2] != 0x00 &&
        (uint8_t)(buf[i + 2] + buf[i + 3]) == 0 && buf[i + 4] == 0xD5 &&
        buf[i + 5] == cmd[0] + 1) {
      uint8_t l = buf[i + 2];
      if (i + 4 + l + 1 >= n) return -1;
      size_t pl = l - 2;
      if (pl > outMax) pl = outMax;
      memcpy(out, &buf[i + 6], pl);
      return (int)pl;
    }
  }
  return -1;
}

static bool tryOrientation(int rx, int tx) {
  Serial.printf("\n== Trying ESP RX=GPIO%d, ESP TX=GPIO%d @ %lu\n", rx, tx, (unsigned long)BAUD);
  pn.end();
  pinMode(rx, INPUT);
  Serial.printf("  idle level on RX pin (PN532 TX): %d\n", digitalRead(rx));
  pn.begin(BAUD, SERIAL_8N1, rx, tx);
  delay(20);
  wakeup();
  const uint8_t getfw[] = {0x02};
  uint8_t r[16];
  int n = transact(getfw, 1, r, sizeof(r), 200, true);
  if (n >= 4) {
    Serial.printf("  PN532 found: IC=0x%02X  firmware %d.%d  support=0x%02X\n", r[0], r[1], r[2], r[3]);
    return true;
  }
  Serial.println("  no valid GetFirmwareVersion response");
  return false;
}

static bool ready = false;

static void lineDiagnostics() {
  Serial.println("\n== Line diagnostics (internal ~45k pull-down vs external drive/pull-up)");
  for (int p : {PIN_A, PIN_B}) {
    pinMode(p, INPUT_PULLDOWN); delay(5);
    int pd = digitalRead(p);
    pinMode(p, INPUT_PULLUP); delay(5);
    int pu = digitalRead(p);
    pinMode(p, INPUT);
    Serial.printf("  GPIO%d: with pull-down=%d  with pull-up=%d  -> %s\n", p, pd, pu,
                  pd ? "held HIGH externally (pull-up resistor or driven TX)" :
                  (pu ? "floating / not connected (or PN532 unpowered)" : "held LOW externally"));
  }
}

static void refPins() {
  Serial.println("== Reference: unconnected header pins with pull-down (expect 0)");
  for (int p : {21, 38, 39, 40, 41, 42}) {
    pinMode(p, INPUT_PULLDOWN); delay(5);
    Serial.printf("  GPIO%d=%d", p, digitalRead(p));
    pinMode(p, INPUT);
  }
  Serial.println();
}

static void i2cScan() {
  for (int o = 0; o < 2; o++) {
    int sda = o ? PIN_B : PIN_A, scl = o ? PIN_A : PIN_B;
    Wire.end();
    Wire.begin(sda, scl, 100000);
    Wire.setTimeOut(50);
    Serial.printf("== I2C scan SDA=GPIO%d SCL=GPIO%d:", sda, scl);
    int found = 0;
    for (uint8_t a = 1; a < 0x7F; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); found++; }
    }
    Serial.println(found ? "" : " none");
  }
  Wire.end();
}

static volatile uint32_t edges = 0;
static void IRAM_ATTR onEdge() { edges++; }

// Checks whether the two lines are shorted together, and whether the line we
// are NOT driving ever toggles (i.e. the PN532 transmits anything at all).
static void deepDiagnostics() {
  Serial.println("== Short test (drive one line LOW, read the other with pull-up)");
  for (int o = 0; o < 2; o++) {
    int drv = o ? PIN_B : PIN_A, rd = o ? PIN_A : PIN_B;
    pinMode(rd, INPUT_PULLUP);
    pinMode(drv, OUTPUT); digitalWrite(drv, LOW); delay(5);
    int v = digitalRead(rd);
    pinMode(drv, INPUT);
    Serial.printf("  GPIO%d LOW -> GPIO%d reads %d %s\n", drv, rd, v, v ? "(not shorted)" : "(SHORTED or pulled together!)");
  }
  Serial.println("== Activity test: send wake+GetFirmwareVersion repeatedly, count edges on other line");
  for (int o = 0; o < 2; o++) {
    int tx = o ? PIN_B : PIN_A, rx = o ? PIN_A : PIN_B;
    pn.end();
    pn.begin(BAUD, SERIAL_8N1, -1, tx);  // TX only, so RX pin is free for edge counting
    pinMode(rx, INPUT);
    edges = 0;
    attachInterrupt(digitalPinToInterrupt(rx), onEdge, CHANGE);
    const uint8_t getfw[] = {0x02};
    for (int k = 0; k < 10; k++) {
      uint8_t w[64] = {0x55, 0x55};  // long wake preamble
      pn.write(w, sizeof(w)); pn.flush(); delay(5);
      sendFrame(getfw, 1);
      delay(50);
    }
    detachInterrupt(digitalPinToInterrupt(rx));
    pn.end();
    Serial.printf("  TX on GPIO%d: %lu edges seen on GPIO%d %s\n", tx, (unsigned long)edges, rx,
                  edges ? "<- PN532 IS transmitting" : "(silent)");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n### PN532 HSU probe ###");
  // HSU first, before any I2C or GPIO diagnostics touch the lines.
  {
    const uint8_t getfw[] = {0x02};
    uint8_t r[16];
    pn.begin(BAUD, SERIAL_8N1, PIN_B /*RX=18*/, PIN_A /*TX=17*/);
    delay(50);
    {
      // Self-check: read back our own TX pin while the UART is sending.
      gpio_input_enable((gpio_num_t)PIN_A);
      uint8_t junk[32]; memset(junk, 0x55, sizeof(junk));
      pn.write(junk, sizeof(junk));
      uint32_t t0 = micros(), tog = 0; int last = gpio_get_level((gpio_num_t)PIN_A);
      while (micros() - t0 < 4000) { int v = gpio_get_level((gpio_num_t)PIN_A); if (v != last) { tog++; last = v; } }
      pn.flush();
      Serial.printf("== TX self-check: %lu transitions seen on GPIO17 while sending %s\n", (unsigned long)tog,
                    tog > 50 ? "(ESP TX OK)" : "(ESP TX NOT toggling!)");
      delay(20); while (pn.available()) pn.read();
    }
    for (int v = 0; v < 4 && !ready; v++) {
      Serial.printf("== Clean HSU attempt %d (RX=GPIO18 TX=GPIO17)\n", v);
      if (v == 1) wakeup();
      if (v == 2) { uint8_t w[2] = {0x55, 0x55}; pn.write(w, 2); pn.flush(); delay(100); while (pn.available()) pn.read(); }
      if (v == 3) { uint8_t w[64] = {0x55, 0x55}; pn.write(w, sizeof(w)); pn.flush(); delay(100); while (pn.available()) pn.read(); }
      int n = transact(getfw, 1, r, sizeof(r), 500, true);
      if (n >= 4) {
        Serial.printf("  PN532 found: IC=0x%02X  firmware %d.%d  support=0x%02X\n", r[0], r[1], r[2], r[3]);
        Serial.println("RESULT: RX=GPIO18 TX=GPIO17 (as planned)");
        ready = true;
      }
    }
    if (!ready) {
      for (uint32_t b : {9600UL, 19200UL, 38400UL, 57600UL, 230400UL, 460800UL, 921600UL, 1288000UL}) {
        for (int o = 0; o < 2 && !ready; o++) {
          int rx = o ? PIN_A : PIN_B, tx = o ? PIN_B : PIN_A;
          pn.end(); pn.begin(b, SERIAL_8N1, rx, tx); delay(20);
          uint8_t w[48] = {0x55, 0x55}; pn.write(w, sizeof(w)); pn.flush(); delay(50); while (pn.available()) pn.read();
          sendFrame(getfw, 1);
          uint8_t buf[64]; size_t n = readFor(buf, sizeof(buf), 300);
          Serial.printf("== Baud %lu RX=GPIO%d TX=GPIO%d: ", (unsigned long)b, rx, tx); hexdump("rx", buf, n);
        }
      }
      pn.end();
    }
  }
  if (!ready) {
    lineDiagnostics();
    refPins();
    i2cScan();
    deepDiagnostics();
  }
  for (int attempt = 0; attempt < 3 && !ready; attempt++) {
    if (tryOrientation(PIN_B, PIN_A)) { ready = true; Serial.println("RESULT: RX=GPIO18 TX=GPIO17 (as planned)"); break; }
    if (tryOrientation(PIN_A, PIN_B)) { ready = true; Serial.println("RESULT: RX=GPIO17 TX=GPIO18 (swapped vs plan)"); break; }
  }
  if (!ready) { Serial.println("RESULT: PN532 not detected over HSU"); return; }
  // SAMConfiguration: normal mode, timeout 1s, use IRQ pin
  const uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};
  uint8_t r[8];
  int n = transact(sam, sizeof(sam), r, sizeof(r), 200, false);
  Serial.printf("SAMConfiguration: %s\n", n >= 0 ? "OK" : "FAILED");
  Serial.println("Polling for tags...");
}

void loop() {
  if (!ready) { delay(5000); Serial.println("RESULT: PN532 not detected over HSU (reset to retry)"); return; }
  // InListPassiveTarget: 1 target, 106 kbps type A
  const uint8_t inl[] = {0x4A, 0x01, 0x00};
  uint8_t r[64];
  int n = transact(inl, sizeof(inl), r, sizeof(r), 1200, false);
  if (n > 0 && r[0] >= 1) {
    uint8_t uidLen = r[5];
    Serial.printf("TAG: SENS_RES=%02X%02X SEL_RES=%02X UID=", r[2], r[3], r[4]);
    for (int i = 0; i < uidLen && 6 + i < n; i++) Serial.printf("%02X", r[6 + i]);
    Serial.println();
  } else if (n == 0 || (n > 0 && r[0] == 0)) {
    Serial.println("no tag");
  } else {
    Serial.println("poll: no/invalid response");
    wakeup();
  }
  delay(700);
}
