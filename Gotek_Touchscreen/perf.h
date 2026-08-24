#pragma once
//
// Phase timing for the slow operations.
//
// "Loading a game takes a while" is not something you can optimise. Which part
// takes the while is. This splits an operation into named phases and reports
// them as one line, so the answer to "where did those eight seconds go" is on
// screen instead of in somebody's imagination.
//
// Deliberately tiny: a fixed array, no allocation, no floating point beyond the
// one division for a rate. It runs inside the operation it is measuring.

#include <Arduino.h>

#define PERF_MAX_PHASES 8

class Perf {
 public:
  explicit Perf(const char *what) : _what(what), _n(0) {
    _start = _last = millis();
  }

  // Close off a phase. The name is not copied, so pass a literal.
  void mark(const char *name) {
    const uint32_t now = millis();
    if (_n < PERF_MAX_PHASES) {
      _name[_n] = name;
      _ms[_n]   = now - _last;
      _n++;
    }
    _last = now;
  }

  // Record how many bytes moved, so the summary can carry a rate.
  void bytes(uint32_t n) { _bytes = n; }

  uint32_t total() const { return millis() - _start; }

  // "DAV load: connect 412ms, headers 88ms, transfer 3214ms, mount 41ms
  //  | 880 KB in 3755ms (234 KB/s)"
  String summary() const {
    String s = String(_what) + ":";
    for (uint8_t i = 0; i < _n; i++) {
      s += (i ? ", " : " ");
      s += String(_name[i]) + " " + String(_ms[i]) + "ms";
    }
    const uint32_t t = millis() - _start;
    s += " | total " + String(t) + "ms";
    if (_bytes > 0) {
      s += ", " + String(_bytes / 1024) + " KB";
      if (t > 0) s += " (" + String((_bytes / 1024) * 1000 / t) + " KB/s)";
    }
    return s;
  }

 private:
  const char *_what;
  const char *_name[PERF_MAX_PHASES];
  uint32_t    _ms[PERF_MAX_PHASES];
  uint8_t     _n;
  uint32_t    _start, _last;
  uint32_t    _bytes = 0;
};
