// HX711 calibration helper. Prints one line per second:
//   AVG n=<samples> mean=<counts> sd=<counts> min=<> max=<>
// Host side (tools/scale_cal.py) turns tare + known-weight captures into a
// counts-per-gram factor. PN532 chip-selects are held high so the readers
// stay quiet on the shared SPI bus.
#include <Arduino.h>
#include <math.h>

static const int PIN_DT = 21, PIN_SCK = 38;

static bool hxRead(int32_t &value, uint32_t timeoutMs = 300) {
  uint32_t t0 = millis();
  while (digitalRead(PIN_DT)) { if (millis() - t0 > timeoutMs) return false; delay(1); }
  uint32_t v = 0;
  noInterrupts();
  for (int i = 0; i < 24; i++) {
    digitalWrite(PIN_SCK, HIGH); delayMicroseconds(1);
    v = (v << 1) | digitalRead(PIN_DT);
    digitalWrite(PIN_SCK, LOW); delayMicroseconds(1);
  }
  digitalWrite(PIN_SCK, HIGH); delayMicroseconds(1);  // channel A, gain 128
  digitalWrite(PIN_SCK, LOW);
  interrupts();
  if (v & 0x800000) v |= 0xFF000000;
  value = (int32_t)v;
  return true;
}

void setup() {
  Serial.begin(115200);
  for (int ss : {17, 18}) { pinMode(ss, OUTPUT); digitalWrite(ss, HIGH); }
  pinMode(PIN_DT, INPUT);
  pinMode(PIN_SCK, OUTPUT);
  digitalWrite(PIN_SCK, LOW);
  delay(1500);
  Serial.println("\n### HX711 calibration stream ###");
}

void loop() {
  uint32_t t0 = millis();
  double sum = 0, sum2 = 0;
  int32_t mn = INT32_MAX, mx = INT32_MIN;
  int n = 0;
  while (millis() - t0 < 1000) {
    int32_t v;
    if (hxRead(v)) {
      sum += v; sum2 += (double)v * v; n++;
      mn = min(mn, v); mx = max(mx, v);
    }
  }
  if (!n) { Serial.println("AVG n=0 (no conversions)"); return; }
  double mean = sum / n;
  double sd = n > 1 ? sqrt(fmax(0.0, (sum2 - n * mean * mean) / (n - 1))) : 0;
  Serial.printf("AVG t=%lu n=%d mean=%.1f sd=%.1f min=%ld max=%ld\n",
                (unsigned long)(millis() / 1000), n, mean, sd, (long)mn, (long)mx);
}
