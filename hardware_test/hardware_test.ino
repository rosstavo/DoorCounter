/*
 * Station Books Footfall Counter — HARDWARE TEST SKETCH
 *
 * Purpose: confirm both PIR sensors are wired correctly and both GPIOs are
 * reading BEFORE the full state-machine firmware is layered on top.
 * (PRD: "Initial hardware test — Do not skip this step.")
 *
 * What it does:
 *   - Prints "PIR OUTER triggered" when GPIO 16 goes HIGH
 *   - Prints "PIR INNER triggered" when GPIO 17 goes HIGH
 *   - No counting, no timing window, no Wi-Fi
 *
 * Board: ESP32 Dev Module (Olimex ESP32-DevKit-LiPo is pin-compatible)
 * Serial: 115200 baud
 *
 * Note: PIR sensors need ~60s warm-up after power-on. Expect a few phantom
 * triggers in the first minute — this is normal and is handled by the warm-up
 * delay in the production firmware.
 */

#define PIR_OUTER_PIN 16  // Sensor facing the street
#define PIR_INNER_PIN 17  // Sensor facing the shop interior
#define SERIAL_BAUD   115200

int lastOuter = LOW;
int lastInner = LOW;

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  pinMode(PIR_OUTER_PIN, INPUT);
  pinMode(PIR_INNER_PIN, INPUT);

  Serial.println();
  Serial.println("=== Footfall Counter — Hardware Test ===");
  Serial.println("Watching GPIO 16 (OUTER) and GPIO 17 (INNER).");
  Serial.println("Wave a hand in front of each sensor in turn.");
  Serial.println("Allow ~60s warm-up; early triggers are normal.");
  Serial.println();
}

void loop() {
  int outer = digitalRead(PIR_OUTER_PIN);
  int inner = digitalRead(PIR_INNER_PIN);

  // Report only on the rising edge so the log is readable.
  if (outer == HIGH && lastOuter == LOW) {
    Serial.printf("[%lu ms] PIR OUTER triggered\n", millis());
  }
  if (inner == HIGH && lastInner == LOW) {
    Serial.printf("[%lu ms] PIR INNER triggered\n", millis());
  }

  lastOuter = outer;
  lastInner = inner;
  delay(5);
}
