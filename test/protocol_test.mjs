/*
 * protocol_test.mjs — every line tuner.ino emits must be valid JSON.
 *
 * A stray quote or a %d/%lu mismatch in one of those printf format strings
 * would produce a line the browser silently drops, which is a miserable thing
 * to debug at a doorway. Rather than restate the formats here (where they would
 * drift), this lifts the emit* functions straight out of tuner.ino, compiles
 * them on the host with Serial.printf mapped to printf, and JSON.parses the
 * result.
 */
import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const src = readFileSync(join(root, 'tuner/tuner.ino'), 'utf8');

// Pull out each emit* function body verbatim.
const wanted = ['emitLog', 'emitCfg', 'emitHello', 'emitEdge', 'emitDetection', 'emitStatus'];
const bodies = [];
for (const name of wanted) {
  const start = src.indexOf(`static void ${name}(`);
  if (start < 0) {
    console.error(`FAIL  ${name}() not found in tuner.ino`);
    process.exit(1);
  }
  let depth = 0, i = src.indexOf('{', start), end = -1;
  for (; i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}' && --depth === 0) { end = i + 1; break; }
  }
  bodies.push(src.slice(start, end));
}

const harness = `
#include <cstdio>
#include <cstdint>
#include <cstring>
#define HIGH 1
#define LOW 0
struct FakeSerial { template <typename... A> void printf(const char* f, A... a) { std::printf(f, a...); } };
static FakeSerial Serial;

// --- values the emitters read, mirroring tuner.ino's globals ---
namespace Detect { enum State { IDLE, ARMED }; }
#define PIR_OUTER_PIN 16
#define PIR_INNER_PIN 17
struct Cfg { unsigned long detectionWindowMs = 400, debounceMs = 1000, simultaneousMs = 10; };
struct DetStub {
  Cfg cfg;
  Detect::State state() const { return Detect::ARMED; }
  unsigned long armedFor(unsigned long) const { return 123; }
  bool armedIsOuter() const { return true; }
  unsigned long debounceRemaining(unsigned long) const { return 456; }
} s_det;
static unsigned long s_warmupMs = 5000, s_warmupEndMs = 9000;
static bool s_counting = false;
static unsigned long s_nEntry = 7, s_nExit = 3, s_nNoise = 11, s_nSimul = 2;

${bodies.join('\n\n')}

int main() {
  emitHello();
  emitCfg();
  emitLog("hello world");
  emitEdge(123456, 'o', 1);
  emitEdge(123457, 'i', 0);
  emitDetection(123999, "entry", 180, true);
  emitDetection(124999, "noise", 401, false);
  emitStatus(5000, HIGH, LOW);
  emitStatus(20000, LOW, HIGH);
  return 0;
}
`;

const dir = mkdtempSync(join(tmpdir(), 'tuner-proto-'));
writeFileSync(join(dir, 'h.cpp'), harness);
execFileSync('c++', ['-std=c++17', '-o', join(dir, 'h'), join(dir, 'h.cpp')], { stdio: 'inherit' });
const out = execFileSync(join(dir, 'h'), { encoding: 'utf8' });

let failures = 0;
const seen = new Set();
console.log('Tuner wire protocol');
for (const line of out.split('\n')) {
  const text = line.trim();
  if (!text) continue;
  try {
    const msg = JSON.parse(text);
    if (!msg.t) throw new Error('no "t" discriminator');
    seen.add(msg.t);
    console.log(`  PASS  ${text}`);
  } catch (err) {
    console.log(`  FAIL  ${text}  <- ${err.message}`);
    failures++;
  }
}
for (const t of ['hello', 'cfg', 'log', 'e', 'd', 's']) {
  if (!seen.has(t)) { console.log(`  FAIL  no "${t}" message emitted`); failures++; }
}
console.log(`\n${failures ? 'FAILED' : 'All checks passed'} (${failures} failure${failures === 1 ? '' : 's'})`);
process.exit(failures ? 1 : 0);
