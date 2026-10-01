// Fast std::fma for the x86-64-v2 build (saintsrow_compat.exe, CPUs without
// FMA3: Sandy/Ivy Bridge and older). The recompiled code turns every PowerPC
// fmadds/fmsubs/fnmadds/fnmsubs/fmadd... into std::fma(double, double, double).
// Without FMA3 that is a call into the C runtime's software fma, ~30-50 ns
// each (measured 33 ns on a 14700F with the runtime's FMA3 path off, vs 3.7 ns
// with it), and the game runs ~9,500 such sites, many per object per frame.
//
// Same result, bit for bit: when a and b are both exactly representable as
// floats (always the case for single-precision PowerPC operands), a * b is
// exact in double (24 + 24 bits <= 53, exponents far inside double range), so
// a * b + c has one rounding - exactly what fma does. Anything else (doubles,
// NaN) still goes to the runtime's fma. -ffp-contract=off keeps the multiply
// and add separate.
//
// Force-included into the generated sources of the v2 build only (see
// CMakeLists.txt). A file "fma_crt" next to the exe makes every call use the
// runtime's fma again (A/B tests).
#pragma once
#include <cmath>

extern "C" bool sr_fma_use_crt;

inline double sr_fast_fma(double a, double b, double c) {
  if (__builtin_expect(!sr_fma_use_crt, 1) && double(float(a)) == a && double(float(b)) == b) {
    return a * b + c;
  }
  return ::fma(a, b, c);
}
namespace std {
using ::sr_fast_fma;
}
#define fma(a, b, c) sr_fast_fma(a, b, c)
