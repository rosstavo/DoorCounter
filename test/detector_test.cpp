#include "Detector.h"
#include <cstdio>
#include <vector>
#include <string>
using namespace Detect;

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

// Drive the detector over a scripted timeline of pin levels, one entry per ms.
struct Sim {
  Detector det;
  uint32_t t = 0;
  int o = LOW, i = LOW;
  std::vector<std::string> results;

  Sim(uint32_t win = 400, uint32_t deb = 1000, uint32_t sim = 10) {
    det.cfg.detectionWindowMs = win;
    det.cfg.debounceMs = deb;
    det.cfg.simultaneousMs = sim;
    det.begin(LOW, LOW);
  }
  void advance(uint32_t ms) {           // poll every 1 ms, like the real loop
    for (uint32_t k = 0; k < ms; k++) {
      Outcome r = det.update(t, o, i);
      switch (r.result) {
        case ENTRY: results.push_back("entry:" + std::to_string(r.deltaMs)); break;
        case EXIT:  results.push_back("exit:" + std::to_string(r.deltaMs)); break;
        case DISCARDED_NOISE: results.push_back("noise"); break;
        case DISCARDED_SIMULTANEOUS: results.push_back("simul"); break;
        default: break;
      }
      t++;
    }
  }
  void pulse(int& pin, uint32_t width = 200) { pin = HIGH; advance(width); pin = LOW; }
  std::string joined() const {
    std::string s;
    for (auto& r : results) { if (!s.empty()) s += ","; s += r; }
    return s;
  }
};

int main() {
  printf("Detector behaviour\n");

  { // OUTER then INNER 150ms later = ENTRY with that delta
    Sim s; s.o = HIGH; s.advance(150); s.i = HIGH; s.advance(5);
    check(s.joined() == "entry:150", "OUTER->INNER within window = ENTRY, delta reported");
  }
  { // INNER then OUTER = EXIT
    Sim s; s.i = HIGH; s.advance(80); s.o = HIGH; s.advance(5);
    check(s.joined() == "exit:80", "INNER->OUTER within window = EXIT");
  }
  { // second sensor arrives after the window = noise, no count
    Sim s; s.o = HIGH; s.advance(500); s.i = HIGH; s.advance(50);
    check(s.joined() == "noise", "follow-up after the window = DISCARDED_NOISE");
  }
  { // lone trigger, never a follow-up
    Sim s; s.pulse(s.o, 100); s.advance(600);
    check(s.joined() == "noise", "lone trigger with no follow-up = DISCARDED_NOISE");
  }
  { // both rise on the same poll
    Sim s; s.o = HIGH; s.i = HIGH; s.advance(5);
    check(s.joined() == "simul", "both sensors rising together = DISCARDED_SIMULTANEOUS");
  }
  { // closer than SIMULTANEOUS_MS
    Sim s; s.o = HIGH; s.advance(4); s.i = HIGH; s.advance(5);
    check(s.joined() == "simul", "delta below simultaneous threshold = discarded");
  }
  { // exactly at the threshold counts (>= simultaneousMs)
    Sim s; s.o = HIGH; s.advance(10); s.i = HIGH; s.advance(5);
    check(s.joined() == "entry:10", "delta exactly at simultaneous threshold = counted");
  }
  { // debounce swallows a second sequence inside the lock-out
    Sim s; s.o = HIGH; s.advance(100); s.i = HIGH; s.advance(50);
    s.o = LOW; s.i = LOW; s.advance(50);
    s.o = HIGH; s.advance(100); s.i = HIGH; s.advance(50);   // still inside 1000ms
    check(s.joined() == "entry:100", "second sequence inside debounce is ignored");
  }
  { // ...and is honoured again once the lock-out expires
    Sim s; s.o = HIGH; s.advance(100); s.i = HIGH; s.advance(50);
    s.o = LOW; s.i = LOW; s.advance(1200);
    s.o = HIGH; s.advance(120); s.i = HIGH; s.advance(50);
    check(s.joined() == "entry:100,entry:120", "sequence after debounce expires is counted");
  }
  { // widening the window turns a discarded pass into a counted one
    Sim a(400); a.o = HIGH; a.advance(600); a.i = HIGH; a.advance(50);
    Sim b(900); b.o = HIGH; b.advance(600); b.i = HIGH; b.advance(50);
    check(a.joined() == "noise" && b.joined() == "entry:600",
          "same 600ms walk-through: noise at window=400, ENTRY at window=900");
  }
  { // begin() baselines HIGH pins so they are not seen as fresh edges
    Detector d; d.cfg.detectionWindowMs = 400; d.cfg.debounceMs = 1000; d.cfg.simultaneousMs = 10;
    d.begin(HIGH, HIGH);
    Outcome r = d.update(0, HIGH, HIGH);
    check(r.result == NONE && !r.outerRise && !r.innerRise,
          "begin() with pins already HIGH produces no spurious edge");
  }
  { // edges are still reported during the debounce lock-out (the tuner draws them)
    Sim s; s.o = HIGH; s.advance(100); s.i = HIGH; s.advance(5);
    Outcome r = s.det.update(s.t, LOW, HIGH);   // OUTER falls mid-debounce
    check(r.outerFall && r.debounced && r.result == NONE,
          "edges are reported but inert during debounce");
  }
  { // sensor-fired health flags
    Sim s; s.pulse(s.o, 50); s.advance(500);
    check(s.det.outerEverFired() && !s.det.innerEverFired(),
          "everFired flags track each sensor independently");
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "All checks passed",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
