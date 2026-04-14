// V8 C wrapper implementation.
// This file is compiled with V8's custom libc++ (std::__Cr).
// It exposes a pure C API so consumers can use any libc++.

#include "v8_wrapper.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "libplatform/libplatform.h"
#include "v8.h"

// --- helpers ----------------------------------------------------------------

static char* dup_str(const char* s) {
  size_t len = std::strlen(s);
  char* p = static_cast<char*>(std::malloc(len + 1));
  if (p) std::memcpy(p, s, len + 1);
  return p;
}

static v8w_result make_error(const char* msg) {
  v8w_result r{};
  r.type = V8W_ERROR;
  r.str_val = dup_str(msg);
  return r;
}

static v8w_result value_to_result(v8::Isolate* isolate,
                                  v8::Local<v8::Context> context,
                                  v8::Local<v8::Value> value) {
  v8w_result r{};
  if (value->IsInt32()) {
    r.type = V8W_OK_INT32;
    r.int_val = value.As<v8::Int32>()->Value();
  } else if (value->IsNumber()) {
    r.type = V8W_OK_DOUBLE;
    r.dbl_val = value.As<v8::Number>()->Value();
  } else {
    v8::String::Utf8Value utf8(isolate, value);
    r.type = V8W_OK_STRING;
    r.str_val = dup_str(*utf8 ? *utf8 : "(null)");
  }
  return r;
}

static std::string exception_message(v8::Isolate* isolate,
                                     v8::Local<v8::Context> context,
                                     v8::TryCatch& tc) {
  v8::HandleScope hs(isolate);
  v8::String::Utf8Value ex(isolate, tc.Exception());
  v8::Local<v8::Message> msg = tc.Message();
  if (!msg.IsEmpty()) {
    v8::String::Utf8Value file(isolate, msg->GetScriptResourceName());
    int line = msg->GetLineNumber(context).FromMaybe(0);
    std::string s(*file);
    s += ':';
    s += std::to_string(line);
    s += ": ";
    s += *ex;
    return s;
  }
  return std::string(*ex);
}

// --- opaque types -----------------------------------------------------------

struct v8w_engine {
  std::unique_ptr<v8::Platform> platform;
};

struct v8w_isolate {
  v8::Isolate* isolate;
  v8::ArrayBuffer::Allocator* allocator;
};

struct v8w_context {
  v8w_isolate* owner;
  v8::Global<v8::Context> persistent;
};

// --- C API ------------------------------------------------------------------

extern "C" {

void v8w_free(char* ptr) { std::free(ptr); }

v8w_engine* v8w_engine_new(void) {
  auto* e = new v8w_engine();
  e->platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(e->platform.get());
  if (!v8::V8::Initialize()) {
    delete e;
    return nullptr;
  }
  return e;
}

void v8w_engine_free(v8w_engine* engine) {
  if (!engine) return;
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  delete engine;
}

int v8w_sandbox_is_secure(void) {
#if defined(V8_ENABLE_SANDBOX)
  return v8::V8::IsSandboxConfiguredSecurely() ? 1 : 0;
#else
  return -1;
#endif
}

size_t v8w_sandbox_size_bytes(void) {
#if defined(V8_ENABLE_SANDBOX)
  return v8::V8::GetSandboxSizeInBytes();
#else
  return 0;
#endif
}

v8w_isolate* v8w_isolate_new(v8w_engine* engine) {
  (void)engine;
  auto* iso = new v8w_isolate();
  iso->allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = iso->allocator;
  iso->isolate = v8::Isolate::New(params);
  if (!iso->isolate) {
    delete iso->allocator;
    delete iso;
    return nullptr;
  }
  return iso;
}

void v8w_isolate_free(v8w_isolate* iso) {
  if (!iso) return;
  iso->isolate->Dispose();
  delete iso->allocator;
  delete iso;
}

v8w_context* v8w_context_new(v8w_isolate* iso) {
  auto* ctx = new v8w_context();
  ctx->owner = iso;
  v8::Isolate* isolate = iso->isolate;
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> lctx = v8::Context::New(isolate);
  ctx->persistent.Reset(isolate, lctx);
  return ctx;
}

void v8w_context_free(v8w_context* ctx) {
  if (!ctx) return;
  ctx->persistent.Reset();
  delete ctx;
}

v8w_result v8w_eval(v8w_context* ctx, const char* source) {
  v8::Isolate* isolate = ctx->owner->isolate;
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context =
      v8::Local<v8::Context>::New(isolate, ctx->persistent);
  v8::Context::Scope context_scope(context);

  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> src;
  if (!v8::String::NewFromUtf8(isolate, source).ToLocal(&src))
    return make_error("Failed to create source string");
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, src).ToLocal(&script))
    return make_error(exception_message(isolate, context, try_catch).c_str());
  v8::MaybeLocal<v8::Value> maybe = script->Run(context);
  if (maybe.IsEmpty())
    return make_error(exception_message(isolate, context, try_catch).c_str());
  return value_to_result(isolate, context, maybe.ToLocalChecked());
}

v8w_result v8w_eval_async(v8w_context* ctx, const char* source) {
  v8::Isolate* isolate = ctx->owner->isolate;
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context =
      v8::Local<v8::Context>::New(isolate, ctx->persistent);
  v8::Context::Scope context_scope(context);

  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> src;
  if (!v8::String::NewFromUtf8(isolate, source).ToLocal(&src))
    return make_error("Failed to create source string");
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, src).ToLocal(&script))
    return make_error(exception_message(isolate, context, try_catch).c_str());
  v8::MaybeLocal<v8::Value> maybe = script->Run(context);
  if (maybe.IsEmpty())
    return make_error(exception_message(isolate, context, try_catch).c_str());

  v8::Local<v8::Value> val = maybe.ToLocalChecked();
  if (!val->IsPromise())
    return make_error("Expected a Promise from async function");

  v8::Local<v8::Promise> promise = val.As<v8::Promise>();
  while (promise->State() == v8::Promise::kPending)
    v8::MicrotasksScope::PerformCheckpoint(isolate);
  if (promise->State() == v8::Promise::kRejected)
    return make_error("Promise rejected");
  return value_to_result(isolate, context, promise->Result());
}

} // extern "C"
