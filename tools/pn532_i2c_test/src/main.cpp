// Dual PN532 I2C test for the Waveshare ESP32-S3-Touch-LCD-3.5(B).
//   Reader 1: own bus (I2C1)      SDA=GPIO18 (hdr 18)  SCL=GPIO17 (hdr 16)
//   Reader 2: onboard bus (I2C0)  SDA=GPIO8  (hdr 28)  SCL=GPIO7  (hdr 26)
// Scans both buses, reads firmware version, then polls both readers for
// ISO14443A tags. Prints tag changes immediately and a stats line every 10 s
// (poll count / error count per reader) so it can run as a soak test.
#include <Arduino.h>
#include <Wire.h>

static const uint8_t PN532_ADDR = 0x24;

struct Reader {
  const char *name;
  TwoWire *bus;
  int sda, scl;
  bool ok = false;
  uint32_t polls = 0, errors = 0, reads = 0;
  String lastUid;
};

static Reader readers[2] = {
  {"R1", &Wire1, 18, 17},
  {"R2", &Wire, 8, 7},
};

static bool pnWrite(TwoWire &w, const uint8_t *cmd, uint8_t len) {
  uint8_t f[48];
  size_t n = 0;
  uint8_t l = len + 1;
  f[n++] = 0x00; f[n++] = 0x00; f[n++] = 0xFF;
  f[n++] = l; f[n++] = (uint8_t)(0x100 - l);
  f[n++] = 0xD4;
  uint8_t sum = 0xD4;
  for (uint8_t i = 0; i < len; i++) { f[n++] = cmd[i]; sum += cmd[i]; }
  f[n++] = (uint8_t)(0x100 - sum);
  f[n++] = 0x00;
  w.beginTransmission(PN532_ADDR);
  w.write(f, n);
  return w.endTransmission() == 0;
}

// PN532 I2C: every read starts with a status byte; bit0 = ready.
struct Reader;
static int transact(TwoWire &w, const uint8_t *cmd, uint8_t len, uint8_t *out, size_t outMax, uint32_t respMs);
static bool traceWait = false;
static bool waitReady(TwoWire &w, uint32_t ms) {
  uint32_t t0 = millis();
  uint32_t nack = 0, busy = 0, other = 0;
  int lastStatus = -1;
  while (millis() - t0 < ms) {
    uint8_t got = w.requestFrom(PN532_ADDR, (uint8_t)1);
    if (got == 1) {
      int s = w.read();
      lastStatus = s;
      if (s & 0x01) return true;
      if (s == 0x00) busy++; else other++;
    } else {
      nack++;
    }
    delay(3);
  }
  if (traceWait)
    Serial.printf("    waitReady timeout: %lu x busy(0x00), %lu x NACK/fail, %lu x other (last=0x%02X)\n",
                  (unsigned long)busy, (unsigned long)nack, (unsigned long)other, lastStatus & 0xFF);
  return false;
}

// Runs once per reader: does switching the RF field on (no target search) break it?
static void rfFieldTest(Reader &r) {
  uint8_t out[16];
  const uint8_t fieldOn[] = {0x32, 0x01, 0x01};
  const uint8_t fieldOff[] = {0x32, 0x01, 0x00};
  const uint8_t getfw[] = {0x02};
  traceWait = true;
  int a = transact(*r.bus, fieldOn, sizeof(fieldOn), out, sizeof(out), 500);
  delay(200);
  int b = transact(*r.bus, getfw, 1, out, sizeof(out), 500);
  int c = transact(*r.bus, fieldOff, sizeof(fieldOff), out, sizeof(out), 500);
  traceWait = false;
  Serial.printf("%s: RF field ON=%d, GetFW while field on=%d, field OFF=%d  (>=0 is OK)\n", r.name, a, b, c);
}

static size_t readBytes(TwoWire &w, uint8_t *buf, size_t n) {
  size_t got = w.requestFrom(PN532_ADDR, (uint8_t)(n + 1));
  if (got == 0) return 0;
  uint8_t status = w.read();
  size_t i = 0;
  while (w.available() && i < n) buf[i++] = w.read();
  return (status & 0x01) ? i : 0;
}

// Returns payload length after D5 (cmd+1), or -1 on error.
static int transact(TwoWire &w, const uint8_t *cmd, uint8_t len, uint8_t *out,
                    size_t outMax, uint32_t respMs) {
  if (!pnWrite(w, cmd, len)) return -1;
  if (!waitReady(w, 50)) return -2;
  uint8_t ack[6];
  static const uint8_t ACK[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
  if (readBytes(w, ack, 6) != 6 || memcmp(ack, ACK, 6) != 0) return -3;
  if (!waitReady(w, respMs)) return -4;
  uint8_t buf[64];
  size_t n = readBytes(w, buf, sizeof(buf));
  for (size_t i = 0; i + 6 < n; i++) {
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

static void scan(TwoWire &w, const char *label) {
  Serial.printf("I2C scan %s:", label);
  for (uint8_t a = 1; a < 0x7F; a++) {
    w.beginTransmission(a);
    if (w.endTransmission() == 0) Serial.printf(" 0x%02X", a);
  }
  Serial.println();
}

static bool initReader(Reader &r) {
  uint8_t out[16];
  const uint8_t getfw[] = {0x02};
  int n = -1;
  for (int i = 0; i < 3 && n < 4; i++) {
    n = transact(*r.bus, getfw, 1, out, sizeof(out), 100);
    if (n < 4) delay(50);
  }
  if (n < 4) {
    Serial.printf("%s: GetFirmwareVersion FAILED (err %d)\n", r.name, n);
    return false;
  }
  Serial.printf("%s: PN5%02X firmware %d.%d support=0x%02X\n", r.name, out[0], out[1], out[2], out[3]);
  const uint8_t sam[] = {0x14, 0x01, 0x14, 0x01};
  Serial.printf("%s: SAMConfiguration %s\n", r.name, transact(*r.bus, sam, sizeof(sam), out, sizeof(out), 100) >= 0 ? "OK" : "FAILED");
  // MaxRetries: ATR=0xFF, PSL=1, PassiveActivation=2 -> InListPassiveTarget returns quickly when no tag.
  const uint8_t retries[] = {0x32, 0x05, 0xFF, 0x01, 0x02};
  Serial.printf("%s: RFConfiguration(MaxRetries) %s\n", r.name, transact(*r.bus, retries, sizeof(retries), out, sizeof(out), 100) >= 0 ? "OK" : "FAILED");
  return true;
}

static void pollReader(Reader &r) {
  if (!r.ok) {
    r.ok = initReader(r);
    if (!r.ok) { r.errors++; return; }
  }
  r.polls++;
  const uint8_t inl[] = {0x4A, 0x01, 0x00};
  uint32_t tPoll = millis();
  uint8_t out[48];
  int n = transact(*r.bus, inl, sizeof(inl), out, sizeof(out), 2000);
  {
    // Turn this reader's RF field off so it doesn't block the other reader
    // (PN532 RF collision avoidance waits while an external field is present).
    const uint8_t fieldOff[] = {0x32, 0x01, 0x00};
    uint8_t tmp[8];
    if (n >= 0) transact(*r.bus, fieldOff, sizeof(fieldOff), tmp, sizeof(tmp), 100);
  }
  if (n < 0 || r.polls < 4) Serial.printf("%s: poll took %lums (n=%d)\n", r.name, (unsigned long)(millis() - tPoll), n);
  if (n < 0) {
    r.errors++;
    Serial.printf("%s: poll error %d\n", r.name, n);
    if (r.errors % 5 == 0) r.ok = false;  // re-init after repeated failures
    return;
  }
  String uid;
  if (n >= 6 && out[0] >= 1) {
    uint8_t uidLen = out[5];
    char hex[3];
    for (int i = 0; i < uidLen && 6 + i < n; i++) { snprintf(hex, sizeof(hex), "%02X", out[6 + i]); uid += hex; }
    r.reads++;
  }
  if (uid != r.lastUid) {
    if (uid.length()) Serial.printf("%s: TAG PRESENT  UID=%s  SAK=%02X\n", r.name, uid.c_str(), out[4]);
    else Serial.printf("%s: tag removed\n", r.name);
    r.lastUid = uid;
  }
}

// Standard I2C bus recovery: if a slave is holding SDA low (e.g. we were reset
// mid-transaction), clock SCL up to 9 times until it lets go, then issue STOP.
static void busRecover(int sda, int scl, const char *label) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delay(1);
  int s0 = digitalRead(sda), c0 = digitalRead(scl);
  int pulses = 0;
  if (!s0) {
    pinMode(scl, OUTPUT_OPEN_DRAIN);
    for (; pulses < 9 && !digitalRead(sda); pulses++) {
      digitalWrite(scl, LOW); delayMicroseconds(10);
      digitalWrite(scl, HIGH); delayMicroseconds(10);
    }
    // STOP: SDA low->high while SCL high
    pinMode(sda, OUTPUT_OPEN_DRAIN);
    digitalWrite(sda, LOW); delayMicroseconds(10);
    digitalWrite(scl, HIGH); delayMicroseconds(10);
    digitalWrite(sda, HIGH); delayMicroseconds(10);
    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, INPUT_PULLUP);
  }
  Serial.printf("Bus %s: before SDA=%d SCL=%d, %d recovery clocks, after SDA=%d SCL=%d\n",
                label, s0, c0, pulses, digitalRead(sda), digitalRead(scl));
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n### Dual PN532 I2C test ###");
  busRecover(18, 17, "R1");
  busRecover(8, 7, "onboard");
  Wire.begin(8, 7, 100000);
  Wire.setTimeOut(50);
  Wire1.begin(18, 17, 100000);
  Wire1.setTimeOut(50);
  scan(Wire1, "R1 bus (SDA18/SCL17)");
  scan(Wire, "onboard bus (SDA8/SCL7)");
  for (auto &r : readers) r.ok = initReader(r);
  for (auto &r : readers) rfFieldTest(r);
  traceWait = true;
  Serial.println("Polling... hold a tag over each reader.");
}

void loop() {
  static uint32_t lastStats = 0;
  for (auto &r : readers) pollReader(r);
  if (millis() - lastStats > 10000) {
    lastStats = millis();
    Serial.printf("STATS t=%lus  R1 polls=%lu err=%lu %s | R2 polls=%lu err=%lu %s\n",
                  (unsigned long)(millis() / 1000),
                  (unsigned long)readers[0].polls, (unsigned long)readers[0].errors, readers[0].ok ? "ok" : "DOWN",
                  (unsigned long)readers[1].polls, (unsigned long)readers[1].errors, readers[1].ok ? "ok" : "DOWN");
  }
  delay(100);
}
