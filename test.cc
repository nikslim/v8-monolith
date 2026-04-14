// Test program for V8 monolithic library via C wrapper.
// This file is compiled with the SYSTEM libc++ — it does not include
// any V8 headers and is fully isolated from V8's custom libc++.

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

#include "v8_wrapper.h"

static void RunSyncTests(v8w_context* ctx);
static void RunAsyncTests(v8w_context* ctx);
static void RunErrorTests(v8w_context* ctx);
static void RunStatePersistenceTests(v8w_context* ctx);
static void RunContextIsolationTests(v8w_isolate* iso);
static void RunMultiIsolateTests(v8w_engine* engine);

static void check(v8w_result r, const char* label) {
  if (r.type == V8W_ERROR) {
    std::cerr << std::format("  {} FAILED: {}\n", label, r.str_val);
    v8w_free(r.str_val);
    std::exit(1);
  }
}

static void run_in_fresh_context(v8w_engine* engine, const char* title,
                                 void (*body)(v8w_context*)) {
  v8w_isolate* iso = v8w_isolate_new(engine);
  v8w_context* ctx = v8w_context_new(iso);
  std::cout << std::format("\n=== {} ===\n", title);
  body(ctx);
  v8w_context_free(ctx);
  v8w_isolate_free(iso);
}

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

  run_in_fresh_context(engine, "Sync tests", RunSyncTests);
  run_in_fresh_context(engine, "Async/await tests", RunAsyncTests);
  run_in_fresh_context(engine, "Error tests", RunErrorTests);
  run_in_fresh_context(engine, "State persistence", RunStatePersistenceTests);

  // Context isolation needs its own isolate, but creates two contexts inside.
  {
    v8w_isolate* iso = v8w_isolate_new(engine);
    std::cout << "\n=== Context isolation ===\n";
    RunContextIsolationTests(iso);
    v8w_isolate_free(iso);
  }

  // Multi-isolate test manages lifecycles directly.
  std::cout << "\n=== Multi-isolate ===\n";
  RunMultiIsolateTests(engine);

  v8w_engine_free(engine);

  std::cout << "\nAll tests completed.\n";
  return 0;
}

// ---------------------------------------------------------------------------
// Sync tests
// ---------------------------------------------------------------------------

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
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 5040);
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
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 30);
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
    assert(std::string(r.str_val) == "Hello, V8");
    std::cout << std::format("  greet('V8') = {}\n", r.str_val);
    v8w_free(r.str_val);
  }

  // 5) Double return
  {
    auto r = v8w_eval(ctx, "Math.PI");
    check(r, "Math.PI");
    assert(r.type == V8W_OK_DOUBLE);
    assert(std::abs(r.dbl_val - 3.14159265358979) < 1e-10);
    std::cout << std::format("  Math.PI = {}\n", r.dbl_val);
  }

  // 6) Boolean — falls to string branch since IsInt32/IsNumber are false.
  {
    auto r = v8w_eval(ctx, "1 < 2");
    check(r, "1<2");
    assert(r.type == V8W_OK_STRING);
    assert(std::string(r.str_val) == "true");
    std::cout << std::format("  (1 < 2) as other = {}\n", r.str_val);
    v8w_free(r.str_val);
  }

  // 7) Array via JSON
  {
    auto r = v8w_eval(ctx, R"(JSON.stringify([1,2,3,"x"]))");
    check(r, "JSON.stringify");
    assert(r.type == V8W_OK_STRING);
    assert(std::string(r.str_val) == R"([1,2,3,"x"])");
    std::cout << std::format("  JSON.stringify([1,2,3,\"x\"]) = {}\n", r.str_val);
    v8w_free(r.str_val);
  }

  // 8) Regex match
  {
    auto r = v8w_eval(ctx, R"('abc123def'.match(/\d+/)[0])");
    check(r, "regex");
    assert(r.type == V8W_OK_STRING);
    assert(std::string(r.str_val) == "123");
    std::cout << std::format("  'abc123def' match /\\d+/ = {}\n", r.str_val);
    v8w_free(r.str_val);
  }

  // 9) Factorial of 20 — overflows int32, returned as double
  {
    auto r = v8w_eval(ctx, R"(
      (function f(n) { return n <= 1 ? 1 : n * f(n - 1); })(20)
    )");
    check(r, "fact(20)");
    assert(r.type == V8W_OK_DOUBLE);
    assert(r.dbl_val == 2432902008176640000.0);
    std::cout << std::format("  fact(20) = {}\n", r.dbl_val);
  }
}

// ---------------------------------------------------------------------------
// Async tests
// ---------------------------------------------------------------------------

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
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 43);
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
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 6);
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
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 20);
    std::cout << std::format("  async + sync double(10) = {}\n", r.int_val);
  }

  // 4) Promise.all
  {
    auto r = v8w_eval_async(ctx, R"(
      Promise.all([Promise.resolve(1), Promise.resolve(2), Promise.resolve(3)])
        .then(a => a.reduce((x, y) => x + y, 0))
    )");
    check(r, "Promise.all");
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 6);
    std::cout << std::format("  Promise.all sum = {}\n", r.int_val);
  }

  // 5) Nested async
  {
    auto r = v8w_eval_async(ctx, R"(
      (async function outer() {
        async function inner(n) {
          const doubled = await Promise.resolve(n * 2);
          return doubled + 1;
        }
        const a = await inner(5);
        const b = await inner(a);
        return b;
      })()
    )");
    check(r, "nested async");
    assert(r.type == V8W_OK_INT32);
    // inner(5)  -> 5*2+1 = 11
    // inner(11) -> 11*2+1 = 23
    assert(r.int_val == 23);
    std::cout << std::format("  nested async = {}\n", r.int_val);
  }

  // 6) Promise rejection — expect V8W_ERROR
  {
    auto r = v8w_eval_async(ctx, R"(
      (async function() { throw new Error('no'); })()
    )");
    assert(r.type == V8W_ERROR);
    assert(r.str_val != nullptr);
    std::cout << std::format("  promise rejection: caught -> {}\n", r.str_val);
    v8w_free(r.str_val);
  }
}

// ---------------------------------------------------------------------------
// Error tests
// ---------------------------------------------------------------------------

static void RunErrorTests(v8w_context* ctx) {
  // 1) Syntax error — Script::Compile fails
  {
    auto r = v8w_eval(ctx, "function (");
    assert(r.type == V8W_ERROR);
    assert(r.str_val != nullptr);
    std::cout << std::format("  syntax error: {}\n", r.str_val);
    v8w_free(r.str_val);
  }

  // 2) Runtime throw — script compiles, Run fails, message propagates
  {
    auto r = v8w_eval(ctx, "throw new Error('boom')");
    assert(r.type == V8W_ERROR);
    assert(r.str_val != nullptr);
    // The message from V8 should contain "boom" — proves exception_message
    // is actually carrying V8's real exception text across the C ABI.
    std::string_view msg = r.str_val;
    assert(msg.find("boom") != std::string_view::npos);
    std::cout << std::format("  runtime throw: {}\n", r.str_val);
    v8w_free(r.str_val);
  }

  // 3) ReferenceError
  {
    auto r = v8w_eval(ctx, "nopeNotAVariable + 1");
    assert(r.type == V8W_ERROR);
    assert(r.str_val != nullptr);
    std::string_view msg = r.str_val;
    assert(msg.find("nopeNotAVariable") != std::string_view::npos);
    std::cout << std::format("  ReferenceError: {}\n", r.str_val);
    v8w_free(r.str_val);
  }
}

// ---------------------------------------------------------------------------
// State persistence: globalThis survives across eval calls in same context
// ---------------------------------------------------------------------------

static void RunStatePersistenceTests(v8w_context* ctx) {
  {
    auto r = v8w_eval(ctx, "globalThis.counter = 10");
    check(r, "set counter");
  }
  {
    auto r = v8w_eval(ctx, "globalThis.counter += 5");
    check(r, "inc counter");
  }
  {
    auto r = v8w_eval(ctx, "globalThis.counter");
    check(r, "read counter");
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 15);
    std::cout << std::format("  globalThis.counter = {}\n", r.int_val);
  }
}

// ---------------------------------------------------------------------------
// Context isolation: two contexts in one isolate have independent globalThis
// ---------------------------------------------------------------------------

static void RunContextIsolationTests(v8w_isolate* iso) {
  v8w_context* ctxA = v8w_context_new(iso);
  v8w_context* ctxB = v8w_context_new(iso);

  // Set tag in A
  {
    auto r = v8w_eval(ctxA, "globalThis.tag = 'A'");
    check(r, "set tag in A");
  }

  // Read in B — should be undefined (stringified)
  {
    auto r = v8w_eval(ctxB, "typeof globalThis.tag");
    check(r, "read tag in B");
    assert(r.type == V8W_OK_STRING);
    assert(std::string(r.str_val) == "undefined");
    std::cout << std::format("  ctxB.tag typeof = {}\n", r.str_val);
    v8w_free(r.str_val);
  }

  // Read in A — still set
  {
    auto r = v8w_eval(ctxA, "globalThis.tag");
    check(r, "read tag in A");
    assert(r.type == V8W_OK_STRING);
    assert(std::string(r.str_val) == "A");
    std::cout << std::format("  ctxA.tag = {}\n", r.str_val);
    v8w_free(r.str_val);
  }

  v8w_context_free(ctxB);
  v8w_context_free(ctxA);
}

// ---------------------------------------------------------------------------
// Multi-isolate lifecycle: two isolates, independent state
// ---------------------------------------------------------------------------

static void RunMultiIsolateTests(v8w_engine* engine) {
  v8w_isolate* iso1 = v8w_isolate_new(engine);
  v8w_context* ctx1 = v8w_context_new(iso1);

  {
    auto r = v8w_eval(ctx1, "globalThis.x = 111; globalThis.x");
    check(r, "iso1.x = 111");
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 111);
    std::cout << std::format("  iso1.x = {}\n", r.int_val);
  }

  // Create second isolate while first is still alive
  v8w_isolate* iso2 = v8w_isolate_new(engine);
  v8w_context* ctx2 = v8w_context_new(iso2);

  {
    auto r = v8w_eval(ctx2, "globalThis.x = 222; globalThis.x");
    check(r, "iso2.x = 222");
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 222);
    std::cout << std::format("  iso2.x = {}\n", r.int_val);
  }

  // Back to iso1 — x should still be 111
  {
    auto r = v8w_eval(ctx1, "globalThis.x");
    check(r, "iso1.x still");
    assert(r.type == V8W_OK_INT32);
    assert(r.int_val == 111);
    std::cout << std::format("  iso1.x (still) = {}\n", r.int_val);
  }

  // Tear down in reverse
  v8w_context_free(ctx2);
  v8w_isolate_free(iso2);
  v8w_context_free(ctx1);
  v8w_isolate_free(iso1);
}
