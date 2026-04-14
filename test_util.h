// Shared helpers for test.cc. Header-only so it compiles with the same
// system libc++ as the test itself, independent of V8's custom libc++.
#ifndef V8_MONOLITH_TEST_UTIL_H
#define V8_MONOLITH_TEST_UTIL_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>

#include "v8_wrapper.h"

// Release-safe assertion. Unlike <cassert>'s assert(), this is never
// compiled out, so release builds still exercise every branch.
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::cerr << std::format("CHECK failed: {}\n  at {}:{}\n", #cond,  \
                               __FILE__, __LINE__);                      \
      std::exit(1);                                                      \
    }                                                                    \
  } while (0)

// Abort the test suite on wrapper error, printing the error message.
inline void check_ok(v8w_result r, const char* label) {
  if (r.type == V8W_ERROR) {
    std::cerr << std::format("  {} FAILED: {}\n",
                             label, r.str_val ? r.str_val : "(no message)");
    v8w_result_free(&r);
    std::exit(1);
  }
}

// Allocate a heap copy via malloc so the wrapper can free it. Aborts on OOM.
inline char* c_strdup(const char* s) {
  size_t len = std::strlen(s);
  char* p = static_cast<char*>(std::malloc(len + 1));
  CHECK(p != nullptr);
  std::memcpy(p, s, len + 1);
  return p;
}

// Run a test section in a fresh isolate+context pair.
inline void run_in_fresh_context(v8w_engine* engine, const char* title,
                                 void (*body)(v8w_context*)) {
  v8w_isolate* iso = v8w_isolate_new(engine);
  CHECK(iso != nullptr);
  v8w_context* ctx = v8w_context_new(iso);
  CHECK(ctx != nullptr);
  std::cout << std::format("\n=== {} ===\n", title);
  body(ctx);
  v8w_context_free(ctx);
  v8w_isolate_free(iso);
}

#endif  // V8_MONOLITH_TEST_UTIL_H
