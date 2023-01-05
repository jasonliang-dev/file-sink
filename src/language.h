#pragma once

#include <stdint.h>

#define array_size(a) (sizeof(a) / sizeof(a[0]))

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// using isize = ptrdiff_t;
// using usize = size_t;

template <class F> struct Defer {
  F f;
  Defer(F f) : f(f) {}
  ~Defer() { f(); }
};

template <class F> Defer<F> defer_func(F f) { return Defer<F>(f); }

#define DEFER_1(x, y) x##y
#define DEFER_2(x, y) DEFER_1(x, y)
#define defer(code)                                                            \
  auto DEFER_2(_defer_, __COUNTER__) = defer_func([&]() { code; })