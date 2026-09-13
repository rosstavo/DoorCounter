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
 *   - Prints the falling edge too, so you can see pulse WIDTH
 *   - RAW LEVEL MODE (RAW_LEVEL 1): also prints a periodic snapshot of the live
 *     HIGH/LOW level of both pins, whether or not anything changed. This tells a
 *     genuinely-pulsing PIR apart from a floating/miswired input:
 *       - A real HC-SR501 sits LOW at rest and pulses HIGH for ~2.5-3s on motion.
 *       - A floating input (sensor unpowered, signal not connected, no shared
 *         ground) drifts or self-oscillates with no one near it — on the bench
 *         that looks like regular ~3s "triggers" that aren't real.
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

// Set to 1 to also stream a periodic raw HIGH/LOW snapshot of both pins.
// Set to 0 for edge-only output (the original quiet behaviour).
#define RAW_LEVEL          1
#define RAW_SNAPSHOT_MS  500  // How often to print the raw level line.

int lastOuter = LOW;
int lastInner = LOW;
unsigned long lastSnapshotMs = 0;

static const char* lvl(int v) { return v == HIGH ? "HIGH" : "LOW "; }

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
#if RAW_LEVEL
  Serial.println("RAW LEVEL mode ON: periodic HIGH/LOW snapshots below.");
  Serial.println("At rest a healthy PIR reads LOW; a floating pin will not.");
#endif
  Serial.println();
}

void loop() {
  int outer = digitalRead(PIR_OUTER_PIN);
  int inner = digitalRead(PIR_INNER_PIN);

  // Report both edges so you can see pulse WIDTH, not just onset.
  if (outer != lastOuter) {
    Serial.printf("[%lu ms] PIR OUTER %s\n", millis(),
                  outer == HIGH ? "triggered (rising)" : "cleared  (falling)");
  }
  if (inner != lastInner) {
    Serial.printf("[%lu ms] PIR INNER %s\n", millis(),
                  inner == HIGH ? "triggered (rising)" : "cleared  (falling)");
  }

#if RAW_LEVEL
  unsigned long now = millis();
  if (now - lastSnapshotMs >= RAW_SNAPSHOT_MS) {
    lastSnapshotMs = now;
    Serial.printf("[%lu ms] RAW  OUTER=%s  INNER=%s\n", now, lvl(outer),
                  lvl(inner));
  }
#endif

  lastOuter = outer;
  lastInner = inner;
  delay(5);
}
