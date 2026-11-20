// System check for the Tagger build (SPI wiring, see agent.md).
//
//   PN532 #1  SS GPIO17 (hdr 16)  RSTPDN GPIO39 (hdr 9)
//   PN532 #2  SS GPIO18 (hdr 18)  RSTPDN GPIO40 (hdr 11)
//   shared SPI SCK GPIO11 (hdr 10)  MOSI GPIO10 (hdr 12)  MISO GPIO9 (hdr 14)
//   HX711     DT GPIO21 (hdr 5)   SCK GPIO38 (hdr 7)
//   onboard I2C SDA GPIO8 / SCL GPIO7 (touch, PMU, RTC, IMU, codec, expander)
//
// Phase 1 prints a PASS/FAIL checklist. Phase 2 polls both readers and the
// scale continuously, printing tag events and a STATS line every 10 s.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

static const int PIN_SCK = 11, PIN_MOSI = 10, PIN_MISO = 9;
static const int PIN_HX_DT = 21, PIN_HX_SCK = 38;

// PN532 SPI: LSB-first, mode 0. First byte selects the operation.
static const uint8_t PN_DATA_WRITE = 0x01, PN_STATUS_READ = 0x02, PN_DATA_READ = 0x03;
static SPISettings pnSpi(1000000, LSBFIRST, SPI_MODE0);

struct Reader {
  const char *name;
  int ss, rst;
  bool ok = false;
  bool rstWired = false;
  uint32_t polls = 0, errors = 0, reinits = 0;
  String lastUid;
};
static Reader readers[2] = {{"R1", 17, 39}, {"R2", 18, 40}};

static int passCount = 0, failCount = 0, warnCount = 0;
static void report(const char *level, const char *fmt, ...) {
  char buf[200];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  Serial.printf("[%s] %s\n", level, buf);
  if (!strcmp(level, "PASS")) passCount++;
  else if (!strcmp(level, "FAIL")) failCount++;
  else if (!strcmp(level, "WARN")) warnCount++;
}

// ---------------------------------------------------------------- PN532 SPI
static void pnSelect(Reader &r) { SPI.beginTransaction(pnSpi); digitalWrite(r.ss, LOW); delay(1); }
static void pnDeselect(Reader &r) { digitalWrite(r.ss, HIGH); SPI.endTransaction(); }

static void pnWriteFrame(Reader &r, const uint8_t *cmd, uint8_t len) {
  uint8_t l = len + 1;
  pnSelect(r);
  SPI.transfer(PN_DATA_WRITE);
  SPI.transfer(0x00); SPI.transfer(0x00); SPI.transfer(0xFF);
  SPI.transfer(l); SPI.transfer((uint8_t)(0x100 - l));
  SPI.transfer(0xD4);
  uint8_t sum = 0xD4;
  for (uint8_t i = 0; i < len; i++) { SPI.transfer(cmd[i]); sum += cmd[i]; }
  SPI.transfer((uint8_t)(0x100 - sum));
  SPI.transfer(0x00);
  pnDeselect(r);
}

static uint8_t pnStatus(Reader &r) {
  pnSelect(r);
  SPI.transfer(PN_STATUS_READ);
  uint8_t s = SPI.transfer(0x00);
  pnDeselect(r);
  return s;
}

static bool pnWaitReady(Reader &r, uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (pnStatus(r) & 0x01) return true;
    delay(2);
  }
  return false;
}

static void pnRead(Reader &r, uint8_t *buf, size_t n) {
  pnSelect(r);
  SPI.transfer(PN_DATA_READ);
  for (size_t i = 0; i < n; i++) buf[i] = SPI.transfer(0x00);
  pnDeselect(r);
}

// Returns payload length after D5 (cmd+1), or a negative error:
// -2 no ACK-ready, -3 bad ACK, -4 no response-ready, -5 bad response frame.
static int pnTransact(Reader &r, const uint8_t *cmd, uint8_t len, uint8_t *out, size_t outMax,
                      uint32_t respMs, uint8_t *rawAck = nullptr) {
  pnWriteFrame(r, cmd, len);
  if (!pnWaitReady(r, 50)) return -2;
  uint8_t ack[6];
  pnRead(r, ack, 6);
  if (rawAck) memcpy(rawAck, ack, 6);
  static const uint8_t ACK[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
  if (memcmp(ack, ACK, 6) != 0) return -3;
  if (!pnWaitReady(r, respMs)) return -4;
  uint8_t buf[64];
  pnRead(r, buf, sizeof(buf));
  for (size_t i = 0; i + 6 < sizeof(buf); i++) {
    if (buf[i] == 0x00 && buf[i + 1] == 0xFF && (uint8_t)(buf[i + 2] + buf[i + 3]) == 0 &&
        buf[i + 4] == 0xD5 && buf[i + 5] == cmd[0] + 1) {
      size_t pl = buf[i + 2] - 2;
      if (pl > outMax) pl = outMax;
      memcpy(out, &buf[i + 6], pl);
      return (int)pl;
    }
  }
  return -5;
}

static void pnHardReset(Reader &r) {
  digitalWrite(r.rst, LOW);
  delay(20);
  digitalWrite(r.rst, HIGH);
  delay(60);
}

static void pnWake(Reader &r) {
  // Pulsing SS wakes the PN532 from power-down in SPI mode.
  digitalWrite(r.ss, LOW); delay(2); digitalWrite(r.ss, HIGH); delay(5);
}

static bool pnInit(Reader &r, bool verbose) {
  pnWake(r);
  uint8_t out[16], ack[6] = {0};
  const uint8_t getfw[] = {0x02};
  int n = -1;
  for (int i = 0; i < 3 && n < 4; i++) {
    n = pnTransact(r, getfw, 1, out, sizeof(out), 100, ack);
    if (n < 4) delay(30);
  }
  if (n < 4) {
    if (verbose) {
      report("FAIL", "%s (SS GPIO%d): no answer to GetFirmwareVersion (err %d, ack bytes %02X %02X %02X %02X %02X %02X)",
             r.name, r.ss, n, ack[0], ack[1], ack[2], ack[3], ack[4], ack[5]);
      if (ack[0] == 0xFF && ack[1] == 0xFF) Serial.println("       MISO reads all 1s: reader not driving MISO (mode switches? SS wire? power?)");
      if (ack[0] == 0x00 && ack[1] == 0x00 && ack[2] == 0x00) Serial.println("       MISO reads all 0s: MISO shorted low or reader in another mode");
    }
    return false;
  }
  if (verbose) report("PASS", "%s (SS GPIO%d): PN5%02X firmware %d.%d", r.name, r.ss, out[0], out[1], out[2]);
  const uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};
  const uint8_t retries[] = {0x32, 0x05, 0xFF, 0x01, 0x02};
  int a = pnTransact(r, sam, sizeof(sam), out, sizeof(out), 100);
  int b = pnTransact(r, retries, sizeof(retries), out, sizeof(out), 100);
  if (verbose && (a < 0 || b < 0)) report("FAIL", "%s: configuration failed (SAM %d, MaxRetries %d)", r.name, a, b);
  return a >= 0 && b >= 0;
}

static void pnFieldOff(Reader &r) {
  const uint8_t off[] = {0x32, 0x01, 0x00};
  uint8_t tmp[8];
  pnTransact(r, off, sizeof(off), tmp, sizeof(tmp), 100);
}

// Is RSTPDN wired to our GPIO? Hold reset low: a wired reader stops answering.
static void checkResetLine(Reader &r) {
  pinMode(r.rst, OUTPUT);
  digitalWrite(r.rst, LOW);
  delay(20);
  uint8_t out[16];
  const uint8_t getfw[] = {0x02};
  int whileHeld = pnTransact(r, getfw, 1, out, sizeof(out), 50);
  digitalWrite(r.rst, HIGH);
  delay(60);
  bool backAfter = pnInit(r, false);
  if (!(whileHeld < 0 && backAfter)) pinMode(r.rst, INPUT);  // not RSTPDN: stop driving it
  if (whileHeld < 0 && backAfter) {
    r.rstWired = true;
    report("PASS", "%s: RSTPDN wired to GPIO%d (reader resets and recovers)", r.name, r.rst);
  } else if (whileHeld >= 0) {
    report("WARN", "%s: RSTPDN not connected to GPIO%d (reader kept answering while held in reset)", r.name, r.rst);
  } else {
    report("FAIL", "%s: did not recover after releasing reset on GPIO%d", r.name, r.rst);
  }
}

static void pollReader(Reader &r) {
  if (!r.ok) {
    if (r.rstWired) pnHardReset(r);
    r.ok = pnInit(r, false);
    r.reinits++;
    if (!r.ok) { r.errors++; Serial.printf("%s: re-init failed\n", r.name); return; }
    Serial.printf("%s: re-initialised%s\n", r.name, r.rstWired ? " (hard reset)" : "");
  }
  r.polls++;
  const uint8_t inl[] = {0x4A, 0x01, 0x00};
  uint8_t out[48];
  int n = pnTransact(r, inl, sizeof(inl), out, sizeof(out), 1000);
  if (n >= 0) pnFieldOff(r);  // essential with two readers (agent.md)
  if (n < 0) {
    r.errors++;
    Serial.printf("%s: poll error %d\n", r.name, n);
    r.ok = false;
    return;
  }
  String uid;
  if (n >= 6 && out[0] >= 1) {
    char hex[3];
    for (int i = 0; i < out[5] && 6 + i < n; i++) { snprintf(hex, sizeof(hex), "%02X", out[6 + i]); uid += hex; }
  }
  if (uid != r.lastUid) {
    if (uid.length())
      Serial.printf("%s: TAG PRESENT  UID=%s  SAK=%02X%s\n", r.name, uid.c_str(), out[4],
                    out[4] == 0x00 ? " (NTAG/Ultralight: OpenSpool-compatible)" : "");
    else
      Serial.printf("%s: tag removed\n", r.name);
    r.lastUid = uid;
  }
}

// ---------------------------------------------------------------- HX711
static bool hxReady() { return digitalRead(PIN_HX_DT) == LOW; }

// extraPulses selects the gain of the NEXT conversion: 1 = A/128, 2 = B/32, 3 = A/64.
static bool hxRead(int32_t &value, uint32_t timeoutMs = 200, int extraPulses = 1) {
  uint32_t t0 = millis();
  while (!hxReady()) { if (millis() - t0 > timeoutMs) return false; delay(1); }
  uint32_t v = 0;
  noInterrupts();
  for (int i = 0; i < 24; i++) {
    digitalWrite(PIN_HX_SCK, HIGH); delayMicroseconds(1);
    v = (v << 1) | digitalRead(PIN_HX_DT);
    digitalWrite(PIN_HX_SCK, LOW); delayMicroseconds(1);
  }
  for (int i = 0; i < extraPulses; i++) {
    digitalWrite(PIN_HX_SCK, HIGH); delayMicroseconds(1);
    digitalWrite(PIN_HX_SCK, LOW); delayMicroseconds(1);
  }
  interrupts();
  if (v & 0x800000) v |= 0xFF000000;
  value = (int32_t)v;
  return true;
}

static int32_t hxMin = INT32_MAX, hxMax = INT32_MIN, hxLast = 0;
static uint32_t hxReads = 0, hxTimeouts = 0;

static void checkHx711() {
  pinMode(PIN_HX_DT, INPUT);
  pinMode(PIN_HX_SCK, OUTPUT);
  // Power-cycle the HX711 via SCK (high > 60 us = power down).
  digitalWrite(PIN_HX_SCK, HIGH); delayMicroseconds(100); digitalWrite(PIN_HX_SCK, LOW);
  delay(500);
  pinMode(PIN_HX_DT, INPUT_PULLDOWN); delay(2);
  int pd = digitalRead(PIN_HX_DT);
  pinMode(PIN_HX_DT, INPUT_PULLUP); delay(2);
  int pu = digitalRead(PIN_HX_DT);
  pinMode(PIN_HX_DT, INPUT);
  if (pd == 0 && pu == 1) {
    report("FAIL", "HX711 DT (GPIO21) floating: not connected or HX711 unpowered");
    return;
  }
  int32_t v[10];
  int got = 0;
  for (int i = 0; i < 10; i++) if (hxRead(v[got], 300)) got++;
  if (got == 0) {
    report("FAIL", "HX711: no conversions (DT never went low). Check SCK (GPIO38) and power");
    return;
  }
  int32_t mn = v[0], mx = v[0];
  int64_t sum = 0;
  for (int i = 0; i < got; i++) { mn = min(mn, v[i]); mx = max(mx, v[i]); sum += v[i]; }
  int32_t avg = (int32_t)(sum / got);
  bool railed = (mx >= 0x7FFFFF || mn <= -0x800000);
  bool allSame = (mn == mx);
  if (railed) {
    report("FAIL", "HX711: %d reads, but saturated (min %ld max %ld): load cell wiring (E+/E-/A+/A-)?", got, (long)mn, (long)mx);
    // Diagnose: lower gain on A, and channel B (B+/B- normally unconnected).
    struct { int pulses; const char *label; } modes[] = {{3, "channel A gain 64"}, {2, "channel B gain 32"}};
    for (auto &m : modes) {
      int32_t x;
      hxRead(x, 300, m.pulses);  // this read was converted at the old gain; it selects the new one
      hxRead(x, 300, m.pulses);  // discard first conversion at the new gain (settling)
      int32_t lo = INT32_MAX, hi = INT32_MIN; int64_t s = 0; int k = 0;
      for (int i = 0; i < 5; i++) if (hxRead(x, 300, m.pulses)) { lo = min(lo, x); hi = max(hi, x); s += x; k++; }
      if (k) Serial.printf("       %s: avg %ld (min %ld max %ld)%s\n", m.label, (long)(s / k), (long)lo, (long)hi,
                           (hi >= 0x7FFFFF || lo <= -0x800000) ? "  SATURATED" : "");
    }
    hxRead(v[0], 300, 1);  // back to channel A gain 128
    hxRead(v[0], 300, 1);
  }
  else if (allSame && (mn == 0 || mn == -1))
    report("FAIL", "HX711: %d reads all %ld: data line stuck", got, (long)mn);
  else
    report("PASS", "HX711: %d/10 reads, avg %ld, noise p-p %ld counts", got, (long)avg, (long)(mx - mn));
}

// ---------------------------------------------------------------- onboard I2C
static void checkOnboardI2C() {
  Wire.begin(8, 7, 100000);
  Wire.setTimeOut(50);
  struct { uint8_t a; const char *n; } expect[] = {
      {0x18, "ES8311 codec"}, {0x20, "TCA9554 IO expander"}, {0x34, "AXP2101 PMU"},
      {0x38, "FT6336 touch"}, {0x51, "PCF85063 RTC"}, {0x6B, "QMI8658 IMU"}};
  String missing, extra;
  for (auto &e : expect) {
    Wire.beginTransmission(e.a);
    if (Wire.endTransmission() != 0) { missing += " "; missing += e.n; }
  }
  for (uint8_t a = 1; a < 0x7F; a++) {
    bool known = (a == 0x7E);
    for (auto &e : expect) if (e.a == a) known = true;
    if (known) continue;
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { char b[8]; snprintf(b, sizeof(b), " 0x%02X", a); extra += b; }
  }
  if (missing.length()) report("FAIL", "Onboard I2C missing:%s", missing.c_str());
  else report("PASS", "Onboard I2C: all 6 board devices present");
  if (extra.length()) report("WARN", "Onboard I2C has unexpected devices:%s (old PN532 still wired to pins 26/28?)", extra.c_str());
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n### Tagger system check ###");

  for (auto &r : readers) {
    pinMode(r.ss, OUTPUT); digitalWrite(r.ss, HIGH);
    pinMode(r.rst, INPUT);  // never drive by default: may be wired to RSTO (an output)
  }
  delay(100);

  checkOnboardI2C();

  // Measure MISO's idle level BEFORE SPI.begin(): pinMode() would detach it
  // from the SPI peripheral, and a second SPI.begin() is a no-op.
  pinMode(PIN_MISO, INPUT_PULLDOWN); delay(2);
  int misoPd = digitalRead(PIN_MISO);
  pinMode(PIN_MISO, INPUT_PULLUP); delay(2);
  int misoPu = digitalRead(PIN_MISO);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  Serial.printf("       MISO idle: pull-down->%d pull-up->%d\n", misoPd, misoPu);
  {
    // With no reader selected MISO is pulled high, so the SPI peripheral must read 0xFF.
    SPI.beginTransaction(pnSpi);
    uint8_t a = SPI.transfer(0x00), b = SPI.transfer(0x55);
    SPI.endTransaction();
    if (a == 0xFF && b == 0xFF) report("PASS", "SPI peripheral reads MISO (idle 0xFF)");
    else report("FAIL", "SPI peripheral reads %02X %02X with nothing selected: MISO not routed to GPIO%d", a, b, PIN_MISO);
  }

  for (auto &r : readers) r.ok = pnInit(r, true);
  for (auto &r : readers) if (r.ok) checkResetLine(r);

  // Cross-talk check: each reader answers only on its own SS.
  if (readers[0].ok && readers[1].ok) {
    report("PASS", "Both readers answer independently on the shared SPI bus");
  }

  checkHx711();

  Serial.printf("\n### Checklist: %d PASS, %d WARN, %d FAIL ###\n", passCount, warnCount, failCount);
  Serial.println("Polling readers and scale... place a tag on each reader, press on the load cell.\n");
}

void loop() {
  static uint32_t lastStats = 0;
  for (auto &r : readers) pollReader(r);
  int32_t v;
  if (hxRead(v, 150)) {
    hxReads++; hxLast = v;
    hxMin = min(hxMin, v); hxMax = max(hxMax, v);
  } else {
    hxTimeouts++;
  }
  if (millis() - lastStats > 10000) {
    lastStats = millis();
    Serial.printf("STATS t=%lus  R1 polls=%lu err=%lu reinit=%lu | R2 polls=%lu err=%lu reinit=%lu | HX711 last=%ld min=%ld max=%ld reads=%lu timeouts=%lu\n",
                  (unsigned long)(millis() / 1000),
                  (unsigned long)readers[0].polls, (unsigned long)readers[0].errors, (unsigned long)readers[0].reinits,
                  (unsigned long)readers[1].polls, (unsigned long)readers[1].errors, (unsigned long)readers[1].reinits,
                  (long)hxLast, (long)hxMin, (long)hxMax, (unsigned long)hxReads, (unsigned long)hxTimeouts);
    hxMin = INT32_MAX; hxMax = INT32_MIN;
  }
  delay(50);
}
