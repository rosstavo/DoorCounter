/*
 * Arduino.h — minimal stub so Detector.h compiles on the host for the tests.
 * Detector.h deliberately uses nothing from Arduino but the level constants and
 * the fixed-width integer types, which is what makes host testing possible.
 */
#pragma once
#include <cstdint>
#include <cstring>
#define HIGH 1
#define LOW 0
