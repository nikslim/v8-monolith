// Test program for V8 monolithic library via C wrapper.
// This file is compiled with the SYSTEM libc++ — it does not include
// any V8 headers and is fully isolated from V8's custom libc++.

#include <cassert>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string>

#include "v8_wrapper.h"

static void RunSyncTests(v8w_context* ctx);
static void RunAsyncTests(v8w_context* ctx);

int main() {
  v8w_engine* engine = v8w_engine_new();
  if (!engine) {
    std::cerr << "v8w_engine_new() failed\n";
    return 1;
  }

  int sandbox = v8w_sandbox_is_secure();
  if (sandbox == 1) {
    std::cout << std::format("V8 sandbox: ENABLED (secure, {} MB)\n",
        v8w_sandbox_size_bytes() / (1024 * 1024));
  } else if (sandbox == 0) {
    std::cerr << "V8 sandbox: WARNING — fallback mode, not fully secure\n";
  } else {
    std::cout << "V8 sandbox: DISABLED (build without V8_ENABLE_SANDBOX)\n";
  }

  v8w_isolate* iso = v8w_isolate_new(engine);
  v8w_context* ctx = v8w_context_new(iso);

  std::cout << "=== Sync tests ===\n";
  RunSyncTests(ctx);

  std::cout << "\n=== Async/await tests ===\n";
  RunAsyncTests(ctx);

  v8w_context_free(ctx);
  v8w_isolate_free(iso);
  v8w_engine_free(engine);

  std::cout << "\nAll tests completed.\n";
  return 0;
}

static void check(v8w_result r, const char* label) {
  if (r.type == V8W_ERROR) {
    std::cerr << std::format("  {} FAILED: {}\n", label, r.str_val);
    v8w_free(r.str_val);
    std::exit(1);
  }
}

static void RunSyncTests(v8w_context* ctx) {
  // 1) Simple expression
  {
    auto r = v8w_eval(ctx, "17 + 25");
    check(r, "17+25");
    assert(r.type == V8W_OK_INT32);
    std::cout << std::format("  17 + 25 = {}\n", r.int_val);
  }

  // 2) Factorial
  {
    auto r = v8w_eval(ctx, R"(
      (function fact(n) {
        if (n <= 1) return 1;
        return n * fact(n - 1);
      })(7)
    )");
    check(r, "fact(7)");
    std::cout << std::format("  fact(7) = {}\n", r.int_val);
  }

  // 3) Object access
  {
    auto r = v8w_eval(ctx, R"(
      (function() {
        const o = { x: 10, y: 20 };
        return o.x + o.y;
      })()
    )");
    check(r, "object");
    std::cout << std::format("  object.x + object.y = {}\n", r.int_val);
  }

  // 4) String return
  {
    auto r = v8w_eval(ctx, R"(
      (function() {
        const greet = (name) => 'Hello, ' + name;
        return greet('V8');
      })()
    )");
    check(r, "greet");
    assert(r.type == V8W_OK_STRING);
    std::cout << std::format("  greet('V8') = {}\n", r.str_val);
    v8w_free(r.str_val);
  }
}

static void RunAsyncTests(v8w_context* ctx) {
  // 1) await Promise.resolve(value)
  {
    auto r = v8w_eval_async(ctx, R"(
      (async function() {
        const x = await Promise.resolve(42);
        return x + 1;
      })()
    )");
    check(r, "async simple");
    std::cout << std::format("  await Promise.resolve(42) + 1 = {}\n", r.int_val);
  }

  // 2) Chained awaits
  {
    auto r = v8w_eval_async(ctx, R"(
      (async function() {
        const a = await Promise.resolve(1);
        const b = await Promise.resolve(2);
        const c = await Promise.resolve(3);
        return a + b + c;
      })()
    )");
    check(r, "async chain");
    std::cout << std::format("  chained await 1+2+3 = {}\n", r.int_val);
  }

  // 3) Async calling sync helper
  {
    auto r = v8w_eval_async(ctx, R"(
      (async function() {
        function doubleSync(n) { return n * 2; }
        const x = await Promise.resolve(10);
        return doubleSync(x);
      })()
    )");
    check(r, "async+sync");
    std::cout << std::format("  async + sync double(10) = {}\n", r.int_val);
  }
}
