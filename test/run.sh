#!/usr/bin/env bash
#
# Host-side tests. No board required — these compile the firmware's pure logic
# for the Mac and exercise it directly.
#
#   detector_test.cpp   the shared detection state machine (door_counter/Detector.h),
#                       which is what both the production firmware and the tuner run
#   protocol_test.mjs   every JSON line tuner.ino emits parses cleanly
#
# Usage: test/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

echo "== detector =="
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
    -I "$ROOT/test" -I "$ROOT/door_counter" \
    -o "$BUILD/detector_test" "$ROOT/test/detector_test.cpp"
"$BUILD/detector_test"

echo
echo "== protocol =="
node "$ROOT/test/protocol_test.mjs"
