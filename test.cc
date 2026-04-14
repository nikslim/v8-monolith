// Test program for V8 monolithic library.
// Exercises sync code, async/await, and C++ ↔ JS interaction.
//
// Build (adjust paths to your V8 build and include). V8 was built with libc++;
// link it explicitly so std::__Cr::* from the .a resolve. macOS also needs Security.
//   c++ -std=c++23 -I include -I include/libplatform test.cc \
//       lib/libv8_monolith.a \
//       -lpthread -ldl -lc++ \
//       -framework CoreFoundation -framework Security \
//       -o v8_test
//   (Linux: drop -framework flags, add -lrt if needed.)

#include <cassert>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <string>

#include "libplatform/libplatform.h"
#include "v8.h"
#include "v8-microtask.h"

static void RunSyncTests(v8::Isolate* isolate, v8::Local<v8::Context> context);
static void RunAsyncTests(v8::Isolate* isolate, v8::Local<v8::Context> context);
[[nodiscard]] static std::expected<v8::Local<v8::Value>, std::string> ExecuteScript(
    v8::Isolate* isolate, v8::Local<v8::Context> context, const char* source);
static std::string ExceptionMessage(v8::Isolate* isolate, v8::Local<v8::Context> context, v8::TryCatch& try_catch);

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  if (!v8::V8::Initialize()) {
    std::cerr << std::format("V8::Initialize failed\n");
    return 1;
  }

#if defined(V8_ENABLE_SANDBOX)
  if (v8::V8::IsSandboxConfiguredSecurely()) {
    std::cout << std::format("V8 sandbox: ENABLED (secure, {} MB)\n",
        v8::V8::GetSandboxSizeInBytes() / (1024 * 1024));
  } else {
    std::cerr << "V8 sandbox: WARNING — fallback mode, not fully secure\n";
  }
#else
  std::cout << "V8 sandbox: DISABLED (build without V8_ENABLE_SANDBOX)\n";
#endif

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    std::cout << "=== Sync tests ===\n";
    RunSyncTests(isolate, context);

    std::cout << "\n=== Async/await tests ===\n";
    RunAsyncTests(isolate, context);
  }

  isolate->Dispose();
  delete create_params.array_buffer_allocator;
  v8::V8::Dispose();
  v8::V8::DisposePlatform();

  std::cout << "\nAll tests completed.\n";
  return 0;
}

static void RunSyncTests(v8::Isolate* isolate, v8::Local<v8::Context> context) {
  auto run = [&](const char* source) { return ExecuteScript(isolate, context, source); };

  // 1) Simple expression
  auto r1 = run("17 + 25");
  if (!r1) { std::cerr << r1.error() << "\n"; return; }
  v8::Local<v8::Value> result = *r1;
  assert(result->IsInt32());
  std::cout << std::format("  17 + 25 = {}\n", result.As<v8::Int32>()->Value());

  // 2) Blocking function: factorial
  const char* factorial_js = R"(
    (function fact(n) {
      if (n <= 1) return 1;
      return n * fact(n - 1);
    })(7)
  )";
  auto r2 = run(factorial_js);
  if (!r2) { std::cerr << r2.error() << "\n"; return; }
  std::cout << std::format("  fact(7) = {}\n", r2->As<v8::Int32>()->Value());

  // 3) Blocking function: string and object
  const char* object_js = R"(
    (function() {
      const o = { x: 10, y: 20 };
      return o.x + o.y;
    })()
  )";
  auto r3 = run(object_js);
  if (!r3) { std::cerr << r3.error() << "\n"; return; }
  std::cout << std::format("  object.x + object.y = {}\n", r3->As<v8::Int32>()->Value());

  // 4) Sync function returning string
  const char* string_js = R"(
    (function() {
      const greet = (name) => 'Hello, ' + name;
      return greet('V8');
    })()
  )";
  auto r4 = run(string_js);
  if (!r4) { std::cerr << r4.error() << "\n"; return; }
  v8::String::Utf8Value utf8(isolate, *r4);
  std::cout << std::format("  greet('V8') = {}\n", *utf8);
}

static void RunAsyncTests(v8::Isolate* isolate, v8::Local<v8::Context> context) {
  auto run = [&](const char* source) { return ExecuteScript(isolate, context, source); };

  auto drain_promise = [&](v8::Local<v8::Value> value) -> std::expected<v8::Local<v8::Value>, std::string> {
    if (!value->IsPromise())
      return std::unexpected("Expected a Promise from async function");
    v8::Local<v8::Promise> promise = value.As<v8::Promise>();
    while (promise->State() == v8::Promise::kPending)
      v8::MicrotasksScope::PerformCheckpoint(isolate);
    if (promise->State() == v8::Promise::kRejected)
      return std::unexpected("Promise rejected");
    return promise->Result();
  };

  // 1) async/await: await Promise.resolve(value)
  const char* async_simple_js = R"(
    (async function() {
      const x = await Promise.resolve(42);
      return x + 1;
    })()
  )";
  auto r1 = run(async_simple_js);
  if (!r1) { std::cerr << r1.error() << "\n"; return; }
  auto f1 = drain_promise(*r1);
  if (!f1) { std::cerr << f1.error() << "\n"; return; }
  std::cout << std::format("  await Promise.resolve(42) + 1 = {}\n", f1->As<v8::Int32>()->Value());

  // 2) async/await: chained awaits
  const char* async_chain_js = R"(
    (async function() {
      const a = await Promise.resolve(1);
      const b = await Promise.resolve(2);
      const c = await Promise.resolve(3);
      return a + b + c;
    })()
  )";
  auto r2 = run(async_chain_js);
  if (!r2) { std::cerr << r2.error() << "\n"; return; }
  auto f2 = drain_promise(*r2);
  if (!f2) { std::cerr << f2.error() << "\n"; return; }
  std::cout << std::format("  chained await 1+2+3 = {}\n", f2->As<v8::Int32>()->Value());

  // 3) async function calling sync helper
  const char* async_with_sync_js = R"(
    (async function() {
      function doubleSync(n) { return n * 2; }
      const x = await Promise.resolve(10);
      return doubleSync(x);
    })()
  )";
  auto r3 = run(async_with_sync_js);
  if (!r3) { std::cerr << r3.error() << "\n"; return; }
  auto f3 = drain_promise(*r3);
  if (!f3) { std::cerr << f3.error() << "\n"; return; }
  std::cout << std::format("  async + sync double(10) = {}\n", f3->As<v8::Int32>()->Value());
}

[[nodiscard]] static std::expected<v8::Local<v8::Value>, std::string> ExecuteScript(
    v8::Isolate* isolate, v8::Local<v8::Context> context, const char* source) {
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> source_handle;
  if (!v8::String::NewFromUtf8(isolate, source).ToLocal(&source_handle))
    return std::unexpected("Failed to create source string");
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, source_handle).ToLocal(&script))
    return std::unexpected(ExceptionMessage(isolate, context, try_catch));
  v8::MaybeLocal<v8::Value> maybe_result = script->Run(context);
  if (maybe_result.IsEmpty())
    return std::unexpected(ExceptionMessage(isolate, context, try_catch));
  return maybe_result.ToLocalChecked();
}

static std::string ExceptionMessage(v8::Isolate* isolate, v8::Local<v8::Context> context, v8::TryCatch& try_catch) {
  v8::HandleScope handle_scope(isolate);
  v8::String::Utf8Value exception(isolate, try_catch.Exception());
  v8::Local<v8::Message> message = try_catch.Message();
  if (!message.IsEmpty()) {
    v8::String::Utf8Value filename(isolate, message->GetScriptResourceName());
    int line = message->GetLineNumber(context).FromMaybe(0);
    return std::format("{}:{}: {}", *filename, line, *exception);
  }
  return std::string(*exception);
}
