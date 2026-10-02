/* Node's builtin modules, precompiled to QuickJS bytecode (tools/js2bc.c),
 * and internalBinding('builtins'). */
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

extern const uint8_t node_builtins_blob[];
extern const struct { const char *id; uint32_t off, len; } node_builtins_index[];
extern const int node_builtins_count;
extern const char node_config_gypi_json[];

int node_builtin_count(void) {
  return node_builtins_count;
}

const char *node_builtin_id(int i) {
  return node_builtins_index[i].id;
}

static int find_builtin(const char *id) {
  int lo = 0, hi = node_builtins_count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2, c = strcmp(node_builtins_index[mid].id, id);
    if (c == 0)
      return mid;
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return -1;
}

const char *node_builtin_source(const char *id, size_t *len) {
  (void)id;
  *len = 0;
  return NULL;
}

/* the wrapper function of a builtin, ready to be called */
JSValue node_compile_builtin(Env *env, const char *id, bool *found) {
  JSContext *ctx = env->ctx;
  int i = find_builtin(id);
  JSValue bc, fn;
  int32_t prev;
  if (i < 0) {
    *found = false;
    return node_throw_error(ctx, "ERR_UNKNOWN_BUILTIN_MODULE", "No such built-in module: %s", id);
  }
  *found = true;
  prev = JS_SetCompileHostDefinedId(env->rt, -1);
  bc = JS_ReadObject(ctx, node_builtins_blob + node_builtins_index[i].off,
                     node_builtins_index[i].len, JS_READ_OBJ_BYTECODE);
  JS_SetCompileHostDefinedId(env->rt, prev);
  if (JS_IsException(bc))
    return bc;
  fn = JS_EvalFunction(ctx, bc);
  return fn;
}

/* ---------------------------------------------------------------------- */
/* internalBinding('builtins') */

static JSValue builtins_compile_function(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  const char *id = JS_ToCString(ctx, ARG(0));
  bool found;
  JSValue r;
  if (!id)
    return JS_EXCEPTION;
  r = node_compile_builtin(env, id, &found);
  JS_FreeCString(ctx, id);
  return r;
}

static JSValue builtins_has_cached_builtins(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
  return JS_TRUE;
}

static JSValue builtins_set_internal_loaders(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->internal_binding_loader);
  JS_FreeValue(ctx, env->builtin_require);
  env->internal_binding_loader = JS_DupValue(ctx, ARG(0));
  env->builtin_require = JS_DupValue(ctx, ARG(1));
  return JS_UNDEFINED;
}

static bool cannot_be_required(const char *id) {
  static const char *const prefixes[] = {
    "internal/bootstrap/", "internal/per_context/", "internal/deps/",
    "internal/main/", NULL,
  };
  static const char *const names[] = {
    "inspector", "inspector/promises", "internal/util/inspector",
    "internal/inspector/network", "internal/inspector/network_http",
    "internal/inspector/network_http2", "internal/inspector/network_undici",
    "internal/inspector_async_hook", "internal/inspector_network_tracking",
    "internal/inspector/webstorage", "trace_events",
    "internal/quic/quic", "internal/quic/symbols", "internal/quic/stats",
    "internal/quic/state", "quic", "sqlite", "stream/iter", "zlib/iter",
    "sys", "wasi", "internal/test/binding", "internal/v8_prof_polyfill", NULL,
  };
  int i;
  for (i = 0; prefixes[i]; i++)
    if (!strncmp(id, prefixes[i], strlen(prefixes[i])))
      return true;
  for (i = 0; names[i]; i++)
    if (!strcmp(id, names[i]))
      return true;
  return false;
}

static JSValue builtins_get_categories(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  JSValue obj = JS_NewObject(ctx), cannot = JS_NewArray(ctx), can = JS_NewArray(ctx);
  uint32_t a = 0, b = 0;
  int i;
  for (i = 0; i < node_builtins_count; i++) {
    const char *id = node_builtins_index[i].id;
    if (cannot_be_required(id))
      JS_SetPropertyUint32(ctx, cannot, a++, JS_NewString(ctx, id));
    else
      JS_SetPropertyUint32(ctx, can, b++, JS_NewString(ctx, id));
  }
  JS_SetPropertyStr(ctx, obj, "cannotBeRequired", cannot);
  JS_SetPropertyStr(ctx, obj, "canBeRequired", can);
  return obj;
}

static JSValue builtins_get_cache_usage(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "compiledWithCache", JS_NewArray(ctx));
  JS_SetPropertyStr(ctx, obj, "compiledWithoutCache", JS_NewArray(ctx));
  JS_SetPropertyStr(ctx, obj, "compiledInSnapshot", JS_NewArray(ctx));
  return obj;
}

JSValue binding_init_builtins(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), ids = JS_NewArray(ctx);
  int i, n = 0;
  for (i = 0; i < node_builtins_count; i++)
    JS_SetPropertyUint32(ctx, ids, n++, JS_NewString(ctx, node_builtins_index[i].id));
  nb_set(ctx, t, "builtinIds", ids);
  nb_set(ctx, t, "builtinCategories", builtins_get_categories(ctx, JS_UNDEFINED, 0, NULL));
  nb_set(ctx, t, "natives", JS_NewObject(ctx));
  nb_set_str(ctx, t, "config", node_config_gypi_json);
  nb_set_method(ctx, t, "compileFunction", builtins_compile_function, 1);
  nb_set_method(ctx, t, "hasCachedBuiltins", builtins_has_cached_builtins, 0);
  nb_set_method(ctx, t, "setInternalLoaders", builtins_set_internal_loaders, 2);
  nb_set_method(ctx, t, "getCacheUsage", builtins_get_cache_usage, 0);
  return t;
}
