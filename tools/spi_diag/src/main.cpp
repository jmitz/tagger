// Low-level wiring diagnostics for the Tagger SPI build.
// - Classifies every relevant header GPIO as floating / pulled high / pulled low.
// - Checks which SS line makes a reader drive MISO.
// - Sweeps PN532 SPI bit order, mode and clock, printing raw status bytes.
// - Detects a swapped HX711 DT/SCK pair.
// Reset pins GPIO39/40 are left as INPUTS (never driven) here.
#include <Arduino.h>
#include <SPI.h>

static const int SCK_ = 11, MOSI_ = 10, MISO_ = 9, SS1 = 17, SS2 = 18;
static const int HX_A = 21, HX_B = 38, RST1 = 39, RST2 = 40;

static const char *classify(int p) {
  pinMode(p, INPUT_PULLDOWN); delay(3);
  int pd = digitalRead(p);
  pinMode(p, INPUT_PULLUP); delay(3);
  int pu = digitalRead(p);
  pinMode(p, INPUT);
  if (pd && pu) return "HIGH (driven/pulled up)";
  if (!pd && !pu) return "LOW (driven/pulled down)";
  if (!pd && pu) return "floating";
  return "?";
}

static uint8_t rev(uint8_t b) { uint8_t r = 0; for (int i = 0; i < 8; i++) if (b & (1 << i)) r |= 1 << (7 - i); return r; }

static void frame(uint8_t *f, size_t &n) {
  // GetFirmwareVersion, preceded by the SPI DataWrite byte 0x01.
  const uint8_t raw[] = {0x01, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
  memcpy(f, raw, sizeof(raw)); n = sizeof(raw);
}

static void trySweep(int ss, const char *name) {
  Serial.printf("\n== %s (SS GPIO%d) protocol sweep\n", name, ss);
  struct { uint8_t order; uint8_t mode; uint32_t hz; const char *label; } combos[] = {
      {LSBFIRST, SPI_MODE0, 1000000, "LSB mode0 1MHz (correct)"},
      {LSBFIRST, SPI_MODE0, 100000, "LSB mode0 100kHz"},
      {LSBFIRST, SPI_MODE3, 1000000, "LSB mode3 1MHz"},
      {MSBFIRST, SPI_MODE0, 1000000, "MSB mode0 1MHz (wrong order, sanity)"},
  };
  for (auto &c : combos) {
    SPISettings s(c.hz, c.order, c.mode);
    // Wake: hold SS low for 2 ms.
    digitalWrite(ss, LOW); delay(2); digitalWrite(ss, HIGH); delay(5);
    uint8_t f[16]; size_t n; frame(f, n);
    SPI.beginTransaction(s);
    digitalWrite(ss, LOW); delay(2);
    for (size_t i = 0; i < n; i++) SPI.transfer(c.order == LSBFIRST ? f[i] : f[i]);
    digitalWrite(ss, HIGH);
    SPI.endTransaction();
    // Sample the status byte several times.
    Serial.printf("  %-36s status:", c.label);
    bool ready = false;
    for (int k = 0; k < 8; k++) {
      delay(5);
      SPI.beginTransaction(s);
      digitalWrite(ss, LOW); delay(1);
      SPI.transfer(0x02);
      uint8_t st = SPI.transfer(0x00);
      digitalWrite(ss, HIGH);
      SPI.endTransaction();
      Serial.printf(" %02X", st);
      if (st == 0x01) ready = true;
    }
    if (ready) {
      SPI.beginTransaction(s);
      digitalWrite(ss, LOW); delay(1);
      SPI.transfer(0x03);
      Serial.print("  -> ACK/resp:");
      for (int k = 0; k < 20; k++) Serial.printf(" %02X", SPI.transfer(0x00));
      digitalWrite(ss, HIGH);
      SPI.endTransaction();
    }
    Serial.println();
  }
}

static void misoPerSS() {
  Serial.println("\n== MISO level per selected reader (no clock)");
  for (int sel : {-1, SS1, SS2}) {
    digitalWrite(SS1, sel == SS1 ? LOW : HIGH);
    digitalWrite(SS2, sel == SS2 ? LOW : HIGH);
    delay(5);
    Serial.printf("  %-14s MISO: %s\n", sel < 0 ? "none selected" : (sel == SS1 ? "R1 (GPIO17)" : "R2 (GPIO18)"), classify(MISO_));
  }
  digitalWrite(SS1, HIGH); digitalWrite(SS2, HIGH);
}

static void hx711() {
  Serial.println("\n== HX711 pin check (both pins as inputs first)");
  Serial.printf("  GPIO21 (DT per plan):  %s\n", classify(HX_A));
  Serial.printf("  GPIO38 (SCK per plan): %s\n", classify(HX_B));
  for (int swap = 0; swap < 2; swap++) {
    int dt = swap ? HX_B : HX_A, sck = swap ? HX_A : HX_B;
    pinMode(sck, OUTPUT); digitalWrite(sck, LOW);
    pinMode(dt, INPUT);
    delay(400);
    int lows = 0, highs = 0;
    for (int i = 0; i < 200; i++) { if (digitalRead(dt)) highs++; else lows++; delay(1); }
    int32_t v = 0; bool got = false;
    uint32_t t0 = millis();
    while (millis() - t0 < 300) {
      if (!digitalRead(dt)) {
        uint32_t x = 0;
        for (int i = 0; i < 24; i++) { digitalWrite(sck, HIGH); delayMicroseconds(1); x = (x << 1) | digitalRead(dt); digitalWrite(sck, LOW); delayMicroseconds(1); }
        digitalWrite(sck, HIGH); delayMicroseconds(1); digitalWrite(sck, LOW);
        if (x & 0x800000) x |= 0xFF000000;
        v = (int32_t)x; got = true; break;
      }
    }
    Serial.printf("  DT=GPIO%d SCK=GPIO%d: DT high %d / low %d samples, read %s %ld\n", dt, sck, highs, lows,
                  got ? "value" : "none", (long)v);
    pinMode(sck, INPUT);
  }
}

// Pairwise connectivity: hold one pin LOW briefly, see which other pins follow.
static void shortMatrix() {
  const int pins[] = {11, 10, 9, 17, 18, 39, 40, 21, 38, 41, 42, 47, 48, 7, 8};
  const int N = sizeof(pins) / sizeof(pins[0]);
  Serial.println("\n== Connectivity matrix (drive one pin LOW 2 ms, others read with pull-up)");
  bool any = false;
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++) pinMode(pins[j], INPUT_PULLUP);
    delay(2);
    pinMode(pins[i], OUTPUT); digitalWrite(pins[i], LOW);
    delayMicroseconds(500);
    String follow;
    for (int j = 0; j < N; j++) {
      if (j == i) continue;
      if (digitalRead(pins[j]) == LOW) { follow += " GPIO"; follow += pins[j]; }
    }
    pinMode(pins[i], INPUT_PULLUP);
    if (follow.length()) { any = true; Serial.printf("  GPIO%d LOW pulls:%s\n", pins[i], follow.c_str()); }
  }
  if (!any) Serial.println("  no pins are connected to each other");
  for (int j = 0; j < N; j++) pinMode(pins[j], INPUT);
}

// Bit-banged PN532 SPI (mode 0, LSB first, ~50 kHz) - independent of the SPI peripheral.
static uint8_t bbXfer(uint8_t out) {
  uint8_t in = 0;
  for (int i = 0; i < 8; i++) {
    digitalWrite(MOSI_, (out >> i) & 1);
    delayMicroseconds(10);
    digitalWrite(SCK_, HIGH);
    delayMicroseconds(5);
    in |= digitalRead(MISO_) << i;
    delayMicroseconds(5);
    digitalWrite(SCK_, LOW);
  }
  return in;
}

static void bitBangTest(int ss, const char *name) {
  pinMode(SCK_, OUTPUT); digitalWrite(SCK_, LOW);
  pinMode(MOSI_, OUTPUT); digitalWrite(MOSI_, LOW);
  pinMode(MISO_, INPUT);
  Serial.printf("\n== %s bit-banged LSB-first GetFirmwareVersion\n", name);
  digitalWrite(ss, LOW); delay(2); digitalWrite(ss, HIGH); delay(10);  // wake
  const uint8_t f[] = {0x01, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
  digitalWrite(ss, LOW); delay(2);
  for (uint8_t b : f) bbXfer(b);
  digitalWrite(ss, HIGH);
  Serial.print("  status:");
  bool ready = false;
  for (int k = 0; k < 10 && !ready; k++) {
    delay(5);
    digitalWrite(ss, LOW); delay(1);
    bbXfer(0x02);
    uint8_t st = bbXfer(0x00);
    digitalWrite(ss, HIGH);
    Serial.printf(" %02X", st);
    ready = (st == 0x01);
  }
  if (ready) {
    digitalWrite(ss, LOW); delay(1);
    bbXfer(0x03);
    Serial.print("  -> ACK:");
    for (int k = 0; k < 6; k++) Serial.printf(" %02X", bbXfer(0x00));
    digitalWrite(ss, HIGH);
    delay(10);
    digitalWrite(ss, LOW); delay(1);
    bbXfer(0x02); uint8_t st = bbXfer(0x00);
    digitalWrite(ss, HIGH);
    if (st == 0x01) {
      digitalWrite(ss, LOW); delay(1);
      bbXfer(0x03);
      Serial.print("  RESP:");
      for (int k = 0; k < 14; k++) Serial.printf(" %02X", bbXfer(0x00));
      digitalWrite(ss, HIGH);
    }
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n### SPI / HX711 wiring diagnostics ###");
  pinMode(RST1, INPUT); pinMode(RST2, INPUT);
  Serial.println("== Idle levels (nothing driven)");
  for (int p : {SCK_, MOSI_, MISO_, SS1, SS2, RST1, RST2, HX_A, HX_B, 39, 40, 41, 42})
    Serial.printf("  GPIO%-2d %s\n", p, classify(p));

  shortMatrix();
  pinMode(SS1, OUTPUT); digitalWrite(SS1, HIGH);
  pinMode(SS2, OUTPUT); digitalWrite(SS2, HIGH);
  bitBangTest(SS1, "R1");
  bitBangTest(SS2, "R2");
  misoPerSS();  // before SPI.begin(): pinMode() would detach MISO from the peripheral
  SPI.begin(SCK_, MISO_, MOSI_, -1);
  trySweep(SS1, "R1");
  trySweep(SS2, "R2");

  // Try again with MOSI and SCK swapped in case they are crossed.
  SPI.end();
  pinMode(MISO_, INPUT);
  SPI.begin(MOSI_, MISO_, SCK_, -1);
  Serial.println("\n== Retry with SCK<->MOSI swapped (GPIO10 as clock, GPIO11 as data)");
  trySweep(SS1, "R1 swapped");
  trySweep(SS2, "R2 swapped");
  SPI.end();

  hx711();
  Serial.println("\n### done ###");
}

void loop() { delay(1000); }
