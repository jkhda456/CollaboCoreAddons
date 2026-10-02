/* internalBinding(): the native modules by name.  A binding this build does
 * not implement yet resolves to an empty object (and, with
 * NODE_QJS_TRACE_BINDINGS=1, reports what JS looks up on it). */
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

#define V(name)                                                               \
  __attribute__((weak)) JSValue binding_init_##name(Env *env) {               \
    return JS_NewObject(env->ctx);                                            \
  }
NODE_BINDINGS(V)
#undef V

/* globals V8 has built in and QuickJS does not (WebAssembly, Intl):
   b_wasm.c and intl.c provide them */
__attribute__((weak)) void node_wasm_install(JSContext *ctx) {}
__attribute__((weak)) void node_intl_install(JSContext *ctx) {}

void node_context_intrinsics(JSContext *ctx) {
  node_wasm_install(ctx);
  node_intl_install(ctx);
}

static const BindingDef bindings[] = {
#define V(name) { #name, binding_init_##name },
  NODE_BINDINGS(V)
#undef V
};

static const char *const trace_wrapper_src =
  "(function (name, target) {\n"
  "  return new Proxy(target, { get(t, k, r) {\n"
  "    if (typeof k === 'string' && !(k in t) && k !== 'then' && k !== 'toJSON')\n"
  "      print_missing(name, k);\n"
  "    return Reflect.get(t, k, r);\n"
  "  } });\n"
  "})";

static JSValue print_missing(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  const char *b = JS_ToCString(ctx, ARG(0)), *k = JS_ToCString(ctx, ARG(1));
  fprintf(stderr, "[node] internalBinding('%s').%s is not implemented\n", b, k);
  JS_FreeCString(ctx, b);
  JS_FreeCString(ctx, k);
  return JS_UNDEFINED;
}

static JSValue trace_wrap(Env *env, const char *name, JSValue obj) {
  JSContext *ctx = env->ctx;
  static int enabled = -1;
  JSValue fn, args[2], r;
  if (enabled < 0) {
    const char *e = getenv("NODE_QJS_TRACE_BINDINGS");
    enabled = e && e[0] && e[0] != '0';
    if (enabled)
      JS_SetPropertyStr(ctx, env->global, "print_missing",
                        JS_NewCFunction(ctx, print_missing, "print_missing", 2));
  }
  if (!enabled)
    return obj;
  fn = JS_Eval(ctx, trace_wrapper_src, strlen(trace_wrapper_src), "<trace>", JS_EVAL_TYPE_GLOBAL);
  args[0] = JS_NewString(ctx, name);
  args[1] = obj;
  r = JS_Call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst *)args);
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, obj);
  return r;
}

JSValue node_get_internal_binding(Env *env, const char *name) {
  JSContext *ctx = env->ctx;
  JSValue cached = JS_GetPropertyStr(ctx, env->binding_cache, name), obj;
  size_t i;
  if (!JS_IsUndefined(cached))
    return cached;
  for (i = 0; i < countof(bindings); i++) {
    if (!strcmp(bindings[i].name, name)) {
      obj = bindings[i].init(env);
      if (JS_IsException(obj))
        return obj;
      obj = trace_wrap(env, name, obj);
      JS_SetPropertyStr(ctx, env->binding_cache, name, JS_DupValue(ctx, obj));
      return obj;
    }
  }
  return node_throw_error(ctx, NULL, "No such binding: %s", name);
}

JSValue node_get_linked_binding(Env *env, const char *name) {
  return node_throw_error(env->ctx, NULL, "No such binding: %s", name);
}
