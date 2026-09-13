/*
 * Detector.h — The dual-PIR detection state machine, on its own.
 *
 * Header-only and deliberately free of Arduino globals, Serial, storage and
 * config.h, so the SAME logic runs in two places:
 *
 *   door_counter/door_counter.ino  — production firmware, tunables baked in
 *                                    from config.h at startup
 *   tuner/tuner.ino                — live web tuner, tunables changed at
 *                                    runtime from the browser
 *
 * That sharing is the point: numbers dialled in with the tuner are only
 * meaningful if the thing being tuned is the code that ships. If you change
 * the state machine, change it here and both stay in step.
 *
 * Usage:
 *   Detect::Detector det;
 *   det.cfg.detectionWindowMs = 400;   // etc.
 *   det.begin(digitalRead(OUTER), digitalRead(INNER));   // baseline levels
 *   ...
 *   Detect::Outcome o = det.update(millis(), digitalRead(OUTER), digitalRead(INNER));
 *   if (o.result == Detect::ENTRY) { ... }
 */

#ifndef DETECTOR_H
#define DETECTOR_H

#include <Arduino.h>

namespace Detect {

// Runtime-adjustable timings. Defaults mirror config.h; the production sketch
// overwrites them from config.h, the tuner from the browser.
struct Tunables {
  uint32_t detectionWindowMs = 400;   // max gap between the two triggers
  uint32_t debounceMs        = 1000;  // lock-out after a registered event
  uint32_t simultaneousMs    = 10;    // closer than this = glitch, not a person
};

enum Result {
  NONE,                    // nothing concluded this tick
  ENTRY,                   // OUTER -> INNER within the window
  EXIT,                    // INNER -> OUTER within the window
  DISCARDED_NOISE,         // one sensor fired, no follow-up
  DISCARDED_SIMULTANEOUS   // both fired together (glitch / group / crosstalk)
};

// What happened on one call to update(). Edge flags and levels are reported
// whether or not a Result was reached — the tuner draws its traces from them.
struct Outcome {
  Result   result        = NONE;
  uint32_t deltaMs       = 0;      // gap between the two triggers (ENTRY/EXIT/SIMUL)
  bool     firstWasOuter = false;  // which sensor armed the sequence
  bool     outerRise     = false;
  bool     innerRise     = false;
  bool     outerFall     = false;
  bool     innerFall     = false;
  int      outerLevel    = LOW;
  int      innerLevel    = LOW;
  bool     debounced     = false;  // edges were swallowed by the debounce window
};

enum State { IDLE, ARMED };

class Detector {
 public:
  Tunables cfg;

  // Seed the edge-detection baseline with the pins' current levels, so a
  // sensor already sitting HIGH is not seen as a fresh rising edge.
  void begin(int outerLevel, int innerLevel) {
    prevOuter_ = outerLevel;
    prevInner_ = innerLevel;
    state_ = IDLE;
    debounceUntilMs_ = 0;
    armedAtMs_ = 0;
    armedIsOuter_ = false;
    outerEverFired_ = false;
    innerEverFired_ = false;
  }

  // Feed one poll of the two pins. Call as often as you can (the production
  // loop runs ~500 Hz); resolution of the reported delta is your poll period.
  Outcome update(uint32_t now, int outerLevel, int innerLevel) {
    Outcome o;
    o.outerLevel = outerLevel;
    o.innerLevel = innerLevel;
    o.outerRise = (outerLevel == HIGH && prevOuter_ == LOW);
    o.innerRise = (innerLevel == HIGH && prevInner_ == LOW);
    o.outerFall = (outerLevel == LOW && prevOuter_ == HIGH);
    o.innerFall = (innerLevel == LOW && prevInner_ == HIGH);
    prevOuter_ = outerLevel;
    prevInner_ = innerLevel;

    if (o.outerRise) outerEverFired_ = true;
    if (o.innerRise) innerEverFired_ = true;

    // Post-event lock-out: edges are still reported (the tuner wants to see
    // them) but they cannot start or complete a sequence.
    if (now < debounceUntilMs_) {
      o.debounced = true;
      return o;
    }

    switch (state_) {
      case IDLE:
        if (o.outerRise && o.innerRise) {
          o.result = DISCARDED_SIMULTANEOUS;
          o.deltaMs = 0;
          debounceUntilMs_ = now + cfg.debounceMs;
        } else if (o.outerRise) {
          state_ = ARMED;
          armedIsOuter_ = true;
          armedAtMs_ = now;
        } else if (o.innerRise) {
          state_ = ARMED;
          armedIsOuter_ = false;
          armedAtMs_ = now;
        }
        break;

      case ARMED: {
        uint32_t elapsed = now - armedAtMs_;
        if (elapsed > cfg.detectionWindowMs) {
          // Window closed with no follow-up — single-sensor noise.
          o.result = DISCARDED_NOISE;
          o.deltaMs = elapsed;
          o.firstWasOuter = armedIsOuter_;
          state_ = IDLE;
          break;
        }
        bool otherRise = armedIsOuter_ ? o.innerRise : o.outerRise;
        if (otherRise) {
          o.deltaMs = elapsed;
          o.firstWasOuter = armedIsOuter_;
          if (elapsed < cfg.simultaneousMs) {
            o.result = DISCARDED_SIMULTANEOUS;
          } else {
            o.result = armedIsOuter_ ? ENTRY : EXIT;
          }
          state_ = IDLE;
          debounceUntilMs_ = now + cfg.debounceMs;
        }
        break;
      }
    }
    return o;
  }

  State state() const { return state_; }
  bool  outerEverFired() const { return outerEverFired_; }
  bool  innerEverFired() const { return innerEverFired_; }

  // ms remaining on the debounce lock-out (0 when not debouncing).
  uint32_t debounceRemaining(uint32_t now) const {
    return (now < debounceUntilMs_) ? (debounceUntilMs_ - now) : 0;
  }
  // ms the current sequence has been armed (0 when IDLE).
  uint32_t armedFor(uint32_t now) const {
    return (state_ == ARMED) ? (now - armedAtMs_) : 0;
  }
  bool armedIsOuter() const { return armedIsOuter_; }

 private:
  State    state_ = IDLE;
  bool     armedIsOuter_ = false;
  uint32_t armedAtMs_ = 0;
  uint32_t debounceUntilMs_ = 0;
  int      prevOuter_ = LOW;
  int      prevInner_ = LOW;
  bool     outerEverFired_ = false;
  bool     innerEverFired_ = false;
};

}  // namespace Detect

#endif  // DETECTOR_H
