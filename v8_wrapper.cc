// V8 C wrapper implementation.
// This file is compiled with V8's custom libc++ (std::__Cr).
// It exposes a pure C API so consumers can use any libc++.

#include "v8_wrapper.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

struct CallbackData {
  v8w_callback cb;
  void*        user_data;
};

struct v8w_context {
  v8w_isolate* owner;
  v8::Global<v8::Context> persistent;
  // Stable storage for C callbacks bound into this context. v8::External
  // holds raw pointers into these; they must outlive the context.
  std::vector<std::unique_ptr<CallbackData>> callbacks;
};

// Trampoline: unpack JS args, call C callback, convert v8w_result back to JS.
static void callback_trampoline(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> context = isolate->GetCurrentContext();

  auto* data = static_cast<CallbackData*>(
      info.Data().As<v8::External>()->Value());

  int argc = info.Length();
  std::vector<v8w_arg> args(argc);
  // Hold Utf8Value objects alive for the duration of the call so that
  // borrowed `str_val` pointers remain valid.
  std::vector<std::unique_ptr<v8::String::Utf8Value>> utf8s;
  utf8s.reserve(argc);

  for (int i = 0; i < argc; ++i) {
    v8::Local<v8::Value> v = info[i];
    if (v->IsInt32()) {
      args[i].type = V8W_OK_INT32;
      args[i].int_val = v.As<v8::Int32>()->Value();
    } else if (v->IsNumber()) {
      args[i].type = V8W_OK_DOUBLE;
      args[i].dbl_val = v.As<v8::Number>()->Value();
    } else {
      auto u = std::make_unique<v8::String::Utf8Value>(isolate, v);
      args[i].type = V8W_OK_STRING;
      args[i].str_val = **u ? **u : "";
      utf8s.push_back(std::move(u));
    }
  }

  v8w_result out{};
  data->cb(argc, args.data(), data->user_data, &out);

  if (out.type == V8W_ERROR) {
    const char* msg = out.str_val ? out.str_val : "callback error";
    v8::Local<v8::String> s =
        v8::String::NewFromUtf8(isolate, msg).ToLocalChecked();
    isolate->ThrowException(v8::Exception::Error(s));
    if (out.str_val) std::free(out.str_val);
    return;
  }

  switch (out.type) {
    case V8W_OK_INT32:
      info.GetReturnValue().Set(out.int_val);
      break;
    case V8W_OK_DOUBLE:
      info.GetReturnValue().Set(out.dbl_val);
      break;
    case V8W_OK_STRING: {
      const char* s = out.str_val ? out.str_val : "";
      v8::Local<v8::String> js =
          v8::String::NewFromUtf8(isolate, s).ToLocalChecked();
      info.GetReturnValue().Set(js);
      if (out.str_val) std::free(out.str_val);
      break;
    }
    default:
      info.GetReturnValue().SetUndefined();
      break;
  }
}

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

int v8w_register_function(v8w_context* ctx, const char* name,
                          v8w_callback cb, void* user_data) {
  if (!ctx || !name || !cb) return 1;
  v8::Isolate* isolate = ctx->owner->isolate;
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context =
      v8::Local<v8::Context>::New(isolate, ctx->persistent);
  v8::Context::Scope context_scope(context);

  auto data = std::make_unique<CallbackData>();
  data->cb = cb;
  data->user_data = user_data;
  v8::Local<v8::External> ext = v8::External::New(isolate, data.get());

  v8::Local<v8::FunctionTemplate> tpl =
      v8::FunctionTemplate::New(isolate, callback_trampoline, ext);
  v8::Local<v8::Function> fn;
  if (!tpl->GetFunction(context).ToLocal(&fn)) return 2;

  v8::Local<v8::String> js_name;
  if (!v8::String::NewFromUtf8(isolate, name).ToLocal(&js_name)) return 3;

  v8::Maybe<bool> ok = context->Global()->Set(context, js_name, fn);
  if (ok.IsNothing() || !ok.FromJust()) return 4;

  ctx->callbacks.push_back(std::move(data));
  return 0;
}

} // extern "C"
