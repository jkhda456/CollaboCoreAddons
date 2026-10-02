/* internalBinding('util') (node_util.cc) and internalBinding('types')
 * (node_types.cc). */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "node.h"

/* V8 PropertyFilter */
enum { ALL_PROPERTIES = 0, ONLY_WRITABLE = 1, ONLY_ENUMERABLE = 2,
       ONLY_CONFIGURABLE = 4, SKIP_STRINGS = 8, SKIP_SYMBOLS = 16 };

static bool atom_is_index(JSContext *ctx, JSAtom a) {
  JSValue v = JS_AtomToValue(ctx, a);
  bool r = false;
  if (JS_IsString(v)) {
    const char *s = JS_ToCString(ctx, v);
    if (s && s[0]) {
      const char *p = s;
      if (*p == '0' && p[1] == 0)
        r = true;
      else if (*p >= '1' && *p <= '9') {
        uint64_t n = 0;
        while (*p >= '0' && *p <= '9' && n < 4294967295ULL)
          n = n * 10 + (*p++ - '0');
        r = *p == 0 && n < 4294967295ULL;
      }
    }
    JS_FreeCString(ctx, s);
  } else if (JS_IsNumber(v)) {
    r = true;
  }
  JS_FreeValue(ctx, v);
  return r;
}

static JSValue util_get_own_non_index_properties(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
  JSValueConst obj = ARG(0);
  int32_t filter = nb_int32(ctx, ARG(1), 0);
  JSPropertyEnum *props;
  uint32_t n, i, k = 0;
  int flags = 0;
  JSValue arr;
  if (!JS_IsObject(obj))
    return JS_NewArray(ctx);
  if (!(filter & SKIP_STRINGS)) flags |= JS_GPN_STRING_MASK;
  if (!(filter & SKIP_SYMBOLS)) flags |= JS_GPN_SYMBOL_MASK;
  if (JS_GetOwnPropertyNames(ctx, &props, &n, obj, flags) < 0)
    return JS_EXCEPTION;
  arr = JS_NewArray(ctx);
  for (i = 0; i < n; i++) {
    if (atom_is_index(ctx, props[i].atom))
      continue;
    if (filter & (ONLY_ENUMERABLE | ONLY_WRITABLE | ONLY_CONFIGURABLE)) {
      JSPropertyDescriptor d;
      int r = JS_GetOwnProperty(ctx, &d, obj, props[i].atom);
      if (r < 0) {
        JS_FreePropertyEnum(ctx, props, n);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
      }
      if (r == 0)
        continue;
      JS_FreeValue(ctx, d.value);
      JS_FreeValue(ctx, d.getter);
      JS_FreeValue(ctx, d.setter);
      if ((filter & ONLY_ENUMERABLE) && !(d.flags & JS_PROP_ENUMERABLE))
        continue;
      if ((filter & ONLY_WRITABLE) && !(d.flags & JS_PROP_WRITABLE))
        continue;
      if ((filter & ONLY_CONFIGURABLE) && !(d.flags & JS_PROP_CONFIGURABLE))
        continue;
    }
    JS_SetPropertyUint32(ctx, arr, k++, JS_AtomToValue(ctx, props[i].atom));
  }
  JS_FreePropertyEnum(ctx, props, n);
  return arr;
}

static JSValue util_get_constructor_name(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  JSValue obj = JS_DupValue(ctx, ARG(0));
  while (JS_IsObject(obj)) {
    JSValue c = JS_GetPropertyStr(ctx, obj, "constructor");
    if (JS_IsFunction(ctx, c)) {
      JSValue name = JS_GetPropertyStr(ctx, c, "name");
      JS_FreeValue(ctx, c);
      if (JS_IsString(name)) {
        const char *s = JS_ToCString(ctx, name);
        bool empty = !s || !s[0];
        JS_FreeCString(ctx, s);
        if (!empty) {
          JS_FreeValue(ctx, obj);
          return name;
        }
      }
      JS_FreeValue(ctx, name);
    } else {
      JS_FreeValue(ctx, c);
    }
    {
      JSValue p = JS_GetPrototype(ctx, obj);
      JS_FreeValue(ctx, obj);
      obj = p;
    }
  }
  JS_FreeValue(ctx, obj);
  return JS_NewString(ctx, "");
}

static JSValue util_get_promise_details(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  JSValueConst p = ARG(0);
  JSValue arr;
  int st;
  if (!JS_IsPromise(p))
    return JS_UNDEFINED;
  st = JS_PromiseState(ctx, p);
  arr = JS_NewArray(ctx);
  /* V8: kPending 0, kFulfilled 1, kRejected 2 */
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, st == JS_PROMISE_PENDING ? 0 :
                                                st == JS_PROMISE_FULFILLED ? 1 : 2));
  if (st != JS_PROMISE_PENDING)
    JS_SetPropertyUint32(ctx, arr, 1, JS_PromiseResult(ctx, p));
  return arr;
}

/* a revoked proxy's target and handler are null, as V8 reports them */
static JSValue proxy_field(JSContext *ctx, JSValue v) {
  if (JS_IsException(v)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_NULL;
  }
  return v;
}

static JSValue util_get_proxy_details(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  JSValueConst p = ARG(0);
  if (!JS_IsProxy(p))
    return JS_UNDEFINED;
  if (argc < 2 || JS_ToBool(ctx, argv[1])) {
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, proxy_field(ctx, JS_GetProxyTarget(ctx, p)));
    JS_SetPropertyUint32(ctx, arr, 1, proxy_field(ctx, JS_GetProxyHandler(ctx, p)));
    return arr;
  }
  return proxy_field(ctx, JS_GetProxyTarget(ctx, p));
}

static JSValue util_preview_entries(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  bool is_kv = false;
  JSValue entries, arr;
  if (!JS_IsObject(ARG(0)))
    return JS_UNDEFINED;
  entries = JS_NodePreviewEntries(ctx, ARG(0), &is_kv);
  if (JS_IsException(entries) || JS_IsUndefined(entries))
    return entries;
  if (argc < 2 || !JS_ToBool(ctx, argv[1]))
    return entries;
  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, entries);
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewBool(ctx, is_kv));
  return arr;
}

static JSValue util_get_external_value(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  return JS_NewBigUint64(ctx, 0);
}

static JSValue util_get_call_sites(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  uint32_t frames = nb_uint32(ctx, ARG(0), 8), i, n;
  JSValue sites = JS_NodeCaptureCallSites(ctx, 1, frames + 1), arr, lenv;
  if (JS_IsException(sites))
    return sites;
  lenv = JS_GetPropertyStr(ctx, sites, "length");
  JS_ToUint32(ctx, &n, lenv);
  arr = JS_NewArray(ctx);
  /* frame 0 is node:util */
  for (i = 1; i < n; i++) {
    JSValue cs = JS_GetPropertyUint32(ctx, sites, i);
    JSValue o = JS_NewObjectProto(ctx, JS_NULL), v, f;
    char id[16];
    f = JS_GetPropertyStr(ctx, cs, "getFunctionName");
    v = JS_Call(ctx, f, cs, 0, NULL);
    JS_FreeValue(ctx, f);
    JS_SetPropertyStr(ctx, o, "functionName", JS_IsString(v) ? v : JS_NewString(ctx, ""));
    if (!JS_IsString(v)) JS_FreeValue(ctx, v);
    snprintf(id, sizeof(id), "%u", i);
    JS_SetPropertyStr(ctx, o, "scriptId", JS_NewString(ctx, id));
    f = JS_GetPropertyStr(ctx, cs, "getFileName");
    v = JS_Call(ctx, f, cs, 0, NULL);
    JS_FreeValue(ctx, f);
    JS_SetPropertyStr(ctx, o, "scriptName", JS_IsString(v) ? v : JS_NewString(ctx, ""));
    if (!JS_IsString(v)) JS_FreeValue(ctx, v);
    f = JS_GetPropertyStr(ctx, cs, "getLineNumber");
    v = JS_Call(ctx, f, cs, 0, NULL);
    JS_FreeValue(ctx, f);
    JS_SetPropertyStr(ctx, o, "lineNumber", JS_IsNumber(v) ? v : JS_NewInt32(ctx, 0));
    f = JS_GetPropertyStr(ctx, cs, "getColumnNumber");
    v = JS_Call(ctx, f, cs, 0, NULL);
    JS_FreeValue(ctx, f);
    if (!JS_IsNumber(v)) v = JS_NewInt32(ctx, 0);
    JS_SetPropertyStr(ctx, o, "columnNumber", JS_DupValue(ctx, v));
    JS_SetPropertyStr(ctx, o, "column", v);
    JS_SetPropertyUint32(ctx, arr, i - 1, o);
    JS_FreeValue(ctx, cs);
  }
  JS_FreeValue(ctx, sites);
  return arr;
}

static JSValue callsite_call(JSContext *ctx, JSValueConst cs, const char *method) {
  JSValue f = JS_GetPropertyStr(ctx, cs, method), v;
  v = JS_Call(ctx, f, cs, 0, NULL);
  JS_FreeValue(ctx, f);
  return v;
}

static JSValue util_get_caller_location(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  /* frames: getCallerLocation, its caller, and that one's caller */
  JSValue sites = JS_NodeCaptureCallSites(ctx, 0, 3), cs, arr;
  cs = JS_GetPropertyUint32(ctx, sites, 2);
  JS_FreeValue(ctx, sites);
  if (!JS_IsObject(cs))
    return JS_UNDEFINED;
  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, callsite_call(ctx, cs, "getLineNumber"));
  JS_SetPropertyUint32(ctx, arr, 1, callsite_call(ctx, cs, "getColumnNumber"));
  JS_SetPropertyUint32(ctx, arr, 2, callsite_call(ctx, cs, "getFileName"));
  JS_FreeValue(ctx, cs);
  return arr;
}

static JSValue util_is_inside_node_modules(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
  int32_t limit = nb_int32(ctx, ARG(0), 10);
  bool def = JS_ToBool(ctx, ARG(1));
  JSValue sites = JS_NodeCaptureCallSites(ctx, 1, limit), lenv;
  uint32_t n = 0, i;
  bool result = def;
  lenv = JS_GetPropertyStr(ctx, sites, "length");
  JS_ToUint32(ctx, &n, lenv);
  for (i = 0; i < n; i++) {
    JSValue cs = JS_GetPropertyUint32(ctx, sites, i);
    JSValue name = callsite_call(ctx, cs, "getFileName");
    const char *s = JS_IsString(name) ? JS_ToCString(ctx, name) : NULL;
    JS_FreeValue(ctx, cs);
    JS_FreeValue(ctx, name);
    if (!s || !s[0] || !strncmp(s, "node:", 5)) {
      JS_FreeCString(ctx, s);
      continue;
    }
    result = strstr(s, "/node_modules/") != NULL || strstr(s, "\\node_modules\\") != NULL;
    JS_FreeCString(ctx, s);
    break;
  }
  JS_FreeValue(ctx, sites);
  return JS_NewBool(ctx, result);
}

static JSValue util_sleep(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  uint32_t ms = nb_uint32(ctx, ARG(0), 0);
  uv_sleep(ms);
  return JS_UNDEFINED;
}

static JSValue util_array_buffer_view_has_buffer(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
  return JS_TRUE;
}

static JSValue util_construct_shared_array_buffer(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv) {
  JSValue ctor = JS_GetPropertyStr(ctx, env_get(ctx)->global, "SharedArrayBuffer");
  JSValue r = JS_CallConstructor(ctx, ctor, argc, argv);
  JS_FreeValue(ctx, ctor);
  return r;
}

static JSValue util_mark_promise_as_handled(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
  if (JS_IsPromise(ARG(0)))
    JS_PromiseMarkAsHandled(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue util_guess_handle_type(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  int32_t fd = nb_int32(ctx, ARG(0), -1);
  /* V8's order: TCP, TTY, UDP, FILE, PIPE, UNKNOWN */
  int idx = 5;
  if (fd < 0)
    return JS_NewInt32(ctx, 5);
  switch (uv_guess_handle(fd)) {
  case UV_TCP: idx = 0; break;
  case UV_TTY: idx = 1; break;
  case UV_UDP: idx = 2; break;
  case UV_FILE: idx = 3; break;
  case UV_NAMED_PIPE: idx = 4; break;
  default: idx = 5; break;
  }
  return JS_NewInt32(ctx, idx);
}

/* defineLazyProperties(target, id, keys, enumerable = true): getters that
   require(id)[key] on first use, then become data properties */
static JSValue lazy_getter(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv, int magic, JSValueConst *data) {
  Env *env = env_get(ctx);
  JSValue mod, args[1], val;
  JSAtom key;
  args[0] = data[0];
  mod = JS_Call(ctx, env->builtin_require, JS_UNDEFINED, 1, (JSValueConst *)args);
  if (JS_IsException(mod))
    return mod;
  key = JS_ValueToAtom(ctx, data[1]);
  val = JS_GetProperty(ctx, mod, key);
  JS_FreeValue(ctx, mod);
  if (!JS_IsException(val) && JS_IsObject(this_val))
    JS_DefinePropertyValue(ctx, this_val, key, JS_DupValue(ctx, val),
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE |
                           (JS_ToBool(ctx, data[2]) ? JS_PROP_ENUMERABLE : 0));
  JS_FreeAtom(ctx, key);
  return val;
}

static JSValue lazy_setter(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv, int magic, JSValueConst *data) {
  JSAtom key = JS_ValueToAtom(ctx, data[1]);
  if (JS_IsObject(this_val))
    JS_DefinePropertyValue(ctx, this_val, key, JS_DupValue(ctx, ARG(0)),
                           JS_PROP_C_W_E);
  JS_FreeAtom(ctx, key);
  return JS_UNDEFINED;
}

static JSValue util_define_lazy_properties(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
  JSValueConst target = ARG(0), id = ARG(1), keys = ARG(2);
  bool enumerable = argc < 4 || JS_ToBool(ctx, argv[3]);
  JSValue lenv = JS_GetPropertyStr(ctx, keys, "length");
  uint32_t n = 0, i;
  JS_ToUint32(ctx, &n, lenv);
  for (i = 0; i < n; i++) {
    JSValue key = JS_GetPropertyUint32(ctx, keys, i);
    JSValue data[3] = { (JSValue)id, key, JS_NewBool(ctx, enumerable) };
    JSValue g = JS_NewCFunctionData(ctx, lazy_getter, 0, 0, 3, (JSValueConst *)data);
    JSValue s = JS_NewCFunctionData(ctx, lazy_setter, 1, 0, 3, (JSValueConst *)data);
    JSAtom a = JS_ValueToAtom(ctx, key);
    /* name the getter like the key, as V8 shows it */
    JS_DefineProperty(ctx, target, a, JS_UNDEFINED, g, s,
                      JS_PROP_HAS_GET | JS_PROP_HAS_SET | JS_PROP_HAS_CONFIGURABLE |
                      JS_PROP_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE |
                      (enumerable ? JS_PROP_ENUMERABLE : 0));
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, key);
  }
  return JS_UNDEFINED;
}

/* dotenv parsing (node_dotenv.cc Dotenv::ParseContent) */
static JSValue util_parse_env(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  size_t len;
  char *src = node_string_to_utf8(ctx, ARG(0), &len);
  JSValue obj = JS_NewObject(ctx);
  char *p, *end;
  if (!src)
    return obj;
  p = src;
  end = src + len;
  while (p < end) {
    char *line_end, *eq, *key, *kend, *val;
    size_t vlen;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
      p++;
    if (p >= end)
      break;
    if (*p == '#') {
      while (p < end && *p != '\n') p++;
      continue;
    }
    line_end = memchr(p, '\n', end - p);
    if (!line_end) line_end = end;
    eq = memchr(p, '=', line_end - p);
    if (!eq) {
      p = line_end;
      continue;
    }
    key = p;
    if (!strncmp(key, "export ", 7))
      key += 7;
    while (key < eq && (*key == ' ' || *key == '\t')) key++;
    kend = eq;
    while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) kend--;
    val = eq + 1;
    while (val < line_end && (*val == ' ' || *val == '\t')) val++;
    if (val < end && (*val == '"' || *val == '\'' || *val == '`')) {
      char q = *val;
      char *close = memchr(val + 1, q, end - val - 1);
      if (close) {
        char *buf = malloc(close - val), *o = buf;
        char *s;
        for (s = val + 1; s < close; s++) {
          if (q == '"' && *s == '\\' && s + 1 < close && s[1] == 'n') {
            *o++ = '\n';
            s++;
          } else {
            *o++ = *s;
          }
        }
        vlen = o - buf;
        {
          JSValue k = JS_NewStringLen(ctx, key, kend - key);
          JSAtom a = JS_ValueToAtom(ctx, k);
          JS_SetProperty(ctx, obj, a, JS_NewStringLen(ctx, buf, vlen));
          JS_FreeAtom(ctx, a);
          JS_FreeValue(ctx, k);
        }
        free(buf);
        p = memchr(close, '\n', end - close);
        if (!p) p = end;
        continue;
      }
    }
    {
      char *vend = line_end, *hash;
      /* an unquoted value ends at the first # (Node 24's rule) */
      hash = memchr(val, '#', vend - val);
      if (hash)
        vend = hash;
      while (vend > val && (vend[-1] == ' ' || vend[-1] == '\t' || vend[-1] == '\r')) vend--;
      if (kend > key) {
        JSValue k = JS_NewStringLen(ctx, key, kend - key);
        JSAtom a = JS_ValueToAtom(ctx, k);
        JS_SetProperty(ctx, obj, a, JS_NewStringLen(ctx, val, vend - val));
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, k);
      }
    }
    p = line_end;
  }
  free(src);
  return obj;
}

/* for --env-file at start-up (main.c): the variables of a .env text */
JSValue node_parse_dotenv(JSContext *ctx, const char *src, size_t len) {
  JSValue str = JS_NewStringLen(ctx, src, len), r;
  if (JS_IsException(str))
    return str;
  r = util_parse_env(ctx, JS_UNDEFINED, 1, (JSValueConst *)&str);
  JS_FreeValue(ctx, str);
  return r;
}

/* WeakReference (internal/util's WeakReference uses this when present) */

JSValue binding_init_util(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), c = JS_NewObject(ctx), ps;
  static const JSCFunctionListEntry funcs[] = {
    NB_FUNC("getOwnNonIndexProperties", 2, util_get_own_non_index_properties),
    NB_FUNC("getConstructorName", 1, util_get_constructor_name),
    NB_FUNC("getPromiseDetails", 1, util_get_promise_details),
    NB_FUNC("getProxyDetails", 2, util_get_proxy_details),
    NB_FUNC("previewEntries", 2, util_preview_entries),
    NB_FUNC("getExternalValue", 1, util_get_external_value),
    NB_FUNC("getCallSites", 1, util_get_call_sites),
    NB_FUNC("getCallerLocation", 0, util_get_caller_location),
    NB_FUNC("isInsideNodeModules", 2, util_is_inside_node_modules),
    NB_FUNC("sleep", 1, util_sleep),
    NB_FUNC("arrayBufferViewHasBuffer", 1, util_array_buffer_view_has_buffer),
    NB_FUNC("constructSharedArrayBuffer", 1, util_construct_shared_array_buffer),
    NB_FUNC("markPromiseAsHandled", 1, util_mark_promise_as_handled),
    NB_FUNC("guessHandleType", 1, util_guess_handle_type),
    NB_FUNC("defineLazyProperties", 4, util_define_lazy_properties),
    NB_FUNC("parseEnv", 1, util_parse_env),
  };
  JS_SetPropertyFunctionList(ctx, t, funcs, countof(funcs));
  ps = JS_NewObject(ctx);
  {
    JSPropertyEnum *props;
    uint32_t n, i;
    JS_GetOwnPropertyNames(ctx, &props, &n, env->private_symbols, JS_GPN_STRING_MASK);
    for (i = 0; i < n; i++)
      JS_SetProperty(ctx, ps, props[i].atom,
                     JS_GetProperty(ctx, env->private_symbols, props[i].atom));
    JS_FreePropertyEnum(ctx, props, n);
  }
  nb_set(ctx, t, "privateSymbols", ps);
  nb_set_int(ctx, c, "kPending", 0);
  nb_set_int(ctx, c, "kFulfilled", 1);
  nb_set_int(ctx, c, "kRejected", 2);
  nb_set_int(ctx, c, "kExiting", kExiting);
  nb_set_int(ctx, c, "kExitCode", kExitCode);
  nb_set_int(ctx, c, "kHasExitCode", kHasExitCode);
  nb_set_int(ctx, c, "ALL_PROPERTIES", ALL_PROPERTIES);
  nb_set_int(ctx, c, "ONLY_WRITABLE", ONLY_WRITABLE);
  nb_set_int(ctx, c, "ONLY_ENUMERABLE", ONLY_ENUMERABLE);
  nb_set_int(ctx, c, "ONLY_CONFIGURABLE", ONLY_CONFIGURABLE);
  nb_set_int(ctx, c, "SKIP_STRINGS", SKIP_STRINGS);
  nb_set_int(ctx, c, "SKIP_SYMBOLS", SKIP_SYMBOLS);
  nb_set_int(ctx, c, "kDisallowCloneAndTransfer", 0);
  nb_set_int(ctx, c, "kTransferable", 1);
  nb_set_int(ctx, c, "kCloneable", 2);
  nb_set(ctx, t, "constants", c);
  nb_set(ctx, t, "shouldAbortOnUncaughtToggle",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32,
                            env_scratch(env, "should_abort_on_uncaught_toggle", 4), 1, 4));
  return t;
}

/* ---------------------------------------------------------------------- */
/* types */

#define TYPE_CHECK(fname, flag)                                               \
  static JSValue fname(JSContext *ctx, JSValueConst this_val, int argc,       \
                       JSValueConst *argv) {                                  \
    return JS_NewBool(ctx, (JS_NodeTypeFlags(ARG(0)) & (flag)) != 0);         \
  }

TYPE_CHECK(t_is_arguments, JS_NODE_TYPE_ARGUMENTS)
TYPE_CHECK(t_is_array_buffer, JS_NODE_TYPE_ARRAY_BUFFER)
TYPE_CHECK(t_is_async_function, JS_NODE_TYPE_ASYNC_FUNCTION)
TYPE_CHECK(t_is_bigint_object, JS_NODE_TYPE_BIGINT_OBJECT)
TYPE_CHECK(t_is_boolean_object, JS_NODE_TYPE_BOOLEAN_OBJECT)
TYPE_CHECK(t_is_date, JS_NODE_TYPE_DATE)
TYPE_CHECK(t_is_generator_function, JS_NODE_TYPE_GENERATOR_FUNCTION)
TYPE_CHECK(t_is_generator_object, JS_NODE_TYPE_GENERATOR_OBJECT)
TYPE_CHECK(t_is_map, JS_NODE_TYPE_MAP)
TYPE_CHECK(t_is_map_iterator, JS_NODE_TYPE_MAP_ITERATOR)
TYPE_CHECK(t_is_module_namespace, JS_NODE_TYPE_MODULE_NAMESPACE)
TYPE_CHECK(t_is_native_error, JS_NODE_TYPE_NATIVE_ERROR)
TYPE_CHECK(t_is_number_object, JS_NODE_TYPE_NUMBER_OBJECT)
TYPE_CHECK(t_is_promise, JS_NODE_TYPE_PROMISE)
TYPE_CHECK(t_is_proxy, JS_NODE_TYPE_PROXY)
TYPE_CHECK(t_is_regexp, JS_NODE_TYPE_REGEXP)
TYPE_CHECK(t_is_set, JS_NODE_TYPE_SET)
TYPE_CHECK(t_is_set_iterator, JS_NODE_TYPE_SET_ITERATOR)
TYPE_CHECK(t_is_shared_array_buffer, JS_NODE_TYPE_SHARED_ARRAY_BUFFER)
TYPE_CHECK(t_is_string_object, JS_NODE_TYPE_STRING_OBJECT)
TYPE_CHECK(t_is_symbol_object, JS_NODE_TYPE_SYMBOL_OBJECT)
TYPE_CHECK(t_is_weak_map, JS_NODE_TYPE_WEAKMAP)
TYPE_CHECK(t_is_weak_set, JS_NODE_TYPE_WEAKSET)
TYPE_CHECK(t_is_any_array_buffer, JS_NODE_TYPE_ARRAY_BUFFER | JS_NODE_TYPE_SHARED_ARRAY_BUFFER)
TYPE_CHECK(t_is_boxed_primitive, JS_NODE_TYPE_NUMBER_OBJECT | JS_NODE_TYPE_STRING_OBJECT |
           JS_NODE_TYPE_BOOLEAN_OBJECT | JS_NODE_TYPE_BIGINT_OBJECT | JS_NODE_TYPE_SYMBOL_OBJECT)

static JSValue t_is_external(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  return JS_FALSE;
}

JSValue binding_init_types(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  static const JSCFunctionListEntry funcs[] = {
    NB_FUNC("isExternal", 1, t_is_external),
    NB_FUNC("isDate", 1, t_is_date),
    NB_FUNC("isArgumentsObject", 1, t_is_arguments),
    NB_FUNC("isBigIntObject", 1, t_is_bigint_object),
    NB_FUNC("isBooleanObject", 1, t_is_boolean_object),
    NB_FUNC("isNumberObject", 1, t_is_number_object),
    NB_FUNC("isStringObject", 1, t_is_string_object),
    NB_FUNC("isSymbolObject", 1, t_is_symbol_object),
    NB_FUNC("isNativeError", 1, t_is_native_error),
    NB_FUNC("isRegExp", 1, t_is_regexp),
    NB_FUNC("isAsyncFunction", 1, t_is_async_function),
    NB_FUNC("isGeneratorFunction", 1, t_is_generator_function),
    NB_FUNC("isGeneratorObject", 1, t_is_generator_object),
    NB_FUNC("isPromise", 1, t_is_promise),
    NB_FUNC("isMap", 1, t_is_map),
    NB_FUNC("isSet", 1, t_is_set),
    NB_FUNC("isMapIterator", 1, t_is_map_iterator),
    NB_FUNC("isSetIterator", 1, t_is_set_iterator),
    NB_FUNC("isWeakMap", 1, t_is_weak_map),
    NB_FUNC("isWeakSet", 1, t_is_weak_set),
    NB_FUNC("isArrayBuffer", 1, t_is_array_buffer),
    NB_FUNC("isSharedArrayBuffer", 1, t_is_shared_array_buffer),
    NB_FUNC("isProxy", 1, t_is_proxy),
    NB_FUNC("isModuleNamespaceObject", 1, t_is_module_namespace),
    NB_FUNC("isAnyArrayBuffer", 1, t_is_any_array_buffer),
    NB_FUNC("isBoxedPrimitive", 1, t_is_boxed_primitive),
  };
  JS_SetPropertyFunctionList(ctx, t, funcs, countof(funcs));
  return t;
}
