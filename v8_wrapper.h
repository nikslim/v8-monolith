// C wrapper around V8 embedding API.
// Compiled with V8's libc++, consumed by code using any libc++.
#ifndef V8_WRAPPER_H
#define V8_WRAPPER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct v8w_engine  v8w_engine;
typedef struct v8w_isolate v8w_isolate;
typedef struct v8w_context v8w_context;

// Result of JS evaluation
typedef enum {
  V8W_OK_INT32  = 0,
  V8W_OK_DOUBLE = 1,
  V8W_OK_STRING = 2,
  V8W_OK_OTHER  = 3,
  V8W_ERROR     = -1,
} v8w_result_type;

typedef struct {
  v8w_result_type type;
  int32_t         int_val;
  double          dbl_val;
  char*           str_val;  // heap-allocated; free with v8w_free()
} v8w_result;

void v8w_free(char* ptr);

// Engine (platform + V8 init)
v8w_engine* v8w_engine_new(void);
void        v8w_engine_free(v8w_engine* engine);
int         v8w_sandbox_is_secure(void);   // 1=secure, 0=fallback, -1=not compiled
size_t      v8w_sandbox_size_bytes(void);  // 0 if no sandbox

// Isolate
v8w_isolate* v8w_isolate_new(v8w_engine* engine);
void         v8w_isolate_free(v8w_isolate* iso);

// Context (enters isolate+context scope on creation)
v8w_context* v8w_context_new(v8w_isolate* iso);
void         v8w_context_free(v8w_context* ctx);

// Evaluate JS source synchronously
v8w_result v8w_eval(v8w_context* ctx, const char* source);

// Evaluate JS source, drain microtasks if result is a Promise,
// return the resolved value (or error on rejection)
v8w_result v8w_eval_async(v8w_context* ctx, const char* source);

#ifdef __cplusplus
}
#endif
#endif // V8_WRAPPER_H
