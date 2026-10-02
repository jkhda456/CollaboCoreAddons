/* Errors: exceptions built by native code (node_errors.h, api/exceptions.cc),
 * Error.prepareStackTrace glue, the fatal exception report, and
 * internalBinding('errors'). */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

static JSValue make_error(JSContext *ctx, int kind, const char *code,
                          const char *fmt, va_list ap) {
  char buf[1024];
  JSValue err;
  vsnprintf(buf, sizeof(buf), fmt, ap);
  switch (kind) {
  case 1: err = JS_NewTypeError(ctx, "%s", buf); break;
  case 2: err = JS_NewRangeError(ctx, "%s", buf); break;
  default: err = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, buf),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    break;
  }
  if (code)
    JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, code));
  return err;
}

JSValue node_throw_error(JSContext *ctx, const char *code, const char *fmt, ...) {
  va_list ap;
  JSValue e;
  va_start(ap, fmt);
  e = make_error(ctx, 0, code, fmt, ap);
  va_end(ap);
  return JS_Throw(ctx, e);
}

JSValue node_throw_type_error(JSContext *ctx, const char *code, const char *fmt, ...) {
  va_list ap;
  JSValue e;
  va_start(ap, fmt);
  e = make_error(ctx, 1, code, fmt, ap);
  va_end(ap);
  return JS_Throw(ctx, e);
}

JSValue node_throw_range_error(JSContext *ctx, const char *code, const char *fmt, ...) {
  va_list ap;
  JSValue e;
  va_start(ap, fmt);
  e = make_error(ctx, 2, code, fmt, ap);
  va_end(ap);
  return JS_Throw(ctx, e);
}

const char *node_uv_errname(int err) {
  return uv_err_name(err);
}

JSValue node_uv_exception(JSContext *ctx, int errorno, const char *syscall,
                          const char *msg, const char *path, const char *dest) {
  const char *code = uv_err_name(errorno);
  char *m;
  size_t n;
  JSValue e;
  if (!msg || !msg[0])
    msg = uv_strerror(errorno);
  n = strlen(code) + strlen(msg) + strlen(syscall ? syscall : "") +
      (path ? strlen(path) : 0) + (dest ? strlen(dest) : 0) + 32;
  m = malloc(n);
  snprintf(m, n, "%s: %s, %s", code, msg, syscall ? syscall : "");
  if (path) {
    strcat(m, " '");
    strcat(m, path);
    strcat(m, "'");
  }
  if (dest) {
    strcat(m, " -> '");
    strcat(m, dest);
    strcat(m, "'");
  }
  e = JS_NewError(ctx);
  JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, m),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  free(m);
  JS_SetPropertyStr(ctx, e, "errno", JS_NewInt32(ctx, errorno));
  JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, code));
  JS_SetPropertyStr(ctx, e, "syscall", JS_NewString(ctx, syscall ? syscall : ""));
  if (path)
    JS_SetPropertyStr(ctx, e, "path", JS_NewString(ctx, path));
  if (dest)
    JS_SetPropertyStr(ctx, e, "dest", JS_NewString(ctx, dest));
  return e;
}

JSValue node_throw_uv_exception(JSContext *ctx, int errorno, const char *syscall,
                                const char *msg, const char *path, const char *dest) {
  return JS_Throw(ctx, node_uv_exception(ctx, errorno, syscall, msg, path, dest));
}

/* ErrnoException: errno values of the OS (not libuv's negative ones) */
JSValue node_errno_exception(JSContext *ctx, int errorno, const char *syscall,
                             const char *msg, const char *path) {
  return node_uv_exception(ctx, -errorno, syscall, msg && msg[0] ? msg : strerror(errorno),
                           path, NULL);
}

/* ---------------------------------------------------------------------- */

JSValue node_prepare_stack_trace(JSContext *ctx, JSValueConst error,
                                 JSValueConst callsites, void *opaque) {
  Env *env = env_get(ctx);
  JSValue args[3], ret;
  if (!env || !JS_IsFunction(ctx, env->prepare_stack_trace_cb)) {
    /* before the bootstrap installed Node's: V8's own format */
    JSValue s = JS_ToString(ctx, error), out;
    JSValue join, sep, joined;
    if (JS_IsException(s))
      return s;
    join = JS_GetPropertyStr(ctx, callsites, "join");
    sep = JS_NewString(ctx, "\n    at ");
    joined = JS_Call(ctx, join, callsites, 1, (JSValueConst *)&sep);
    JS_FreeValue(ctx, join);
    JS_FreeValue(ctx, sep);
    if (JS_IsException(joined)) {
      JS_FreeValue(ctx, s);
      return joined;
    }
    {
      JSValue len = JS_GetPropertyStr(ctx, callsites, "length");
      int32_t n = 0;
      JS_ToInt32(ctx, &n, len);
      if (n > 0) {
        const char *a = JS_ToCString(ctx, s), *b = JS_ToCString(ctx, joined);
        size_t l = strlen(a) + strlen(b) + 16;
        char *buf = malloc(l);
        snprintf(buf, l, "%s\n    at %s", a, b);
        out = JS_NewString(ctx, buf);
        free(buf);
        JS_FreeCString(ctx, a);
        JS_FreeCString(ctx, b);
      } else {
        out = JS_DupValue(ctx, s);
      }
    }
    JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, joined);
    return out;
  }
  args[0] = env->global;
  args[1] = (JSValue)error;
  args[2] = (JSValue)callsites;
  ret = JS_Call(ctx, env->prepare_stack_trace_cb, JS_UNDEFINED, 3, (JSValueConst *)args);
  return ret;
}

/* ---------------------------------------------------------------------- */
/* the source line and caret of an exception (AppendExceptionLine) */

static char *read_source_line(Env *env, const char *filename, int line) {
  const char *path = filename;
  FILE *f;
  char *buf = NULL, *res = NULL;
  size_t cap = 0;
  ssize_t n;
  int cur = 1;
  if (!strncmp(path, "file://", 7))
    path += 7;
  if (!strcmp(filename, "[eval]") && env->options && env->options->eval_string) {
    const char *s = env->options->eval_string, *e;
    while (cur < line && (s = strchr(s, '\n'))) {
      s++;
      cur++;
    }
    if (!s || cur != line)
      return NULL;
    e = strchr(s, '\n');
    return e ? strndup(s, e - s) : strdup(s);
  }
  if (path[0] != '/')
    return NULL;
  f = fopen(path, "r");
  if (!f)
    return NULL;
  while ((n = getline(&buf, &cap, f)) >= 0) {
    if (cur == line) {
      if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = 0;
      res = strdup(buf);
      break;
    }
    cur++;
  }
  free(buf);
  fclose(f);
  return res;
}

/* "file:line\nsource line\n    ^\n", or NULL */
static char *exception_arrow(Env *env, JSValueConst err) {
  JSContext *ctx = env->ctx;
  JSValue fname;
  int line, col;
  const char *f;
  char *src, *out;
  size_t n;
  int i;
  if (JS_NodeGetExceptionPosition(ctx, err, &fname, &line, &col) < 0)
    return NULL;
  f = JS_ToCString(ctx, fname);
  JS_FreeValue(ctx, fname);
  if (!f)
    return NULL;
  if (!strncmp(f, "node:", 5)) {
    JS_FreeCString(ctx, f);
    return NULL;
  }
  src = read_source_line(env, f, line);
  if (!src) {
    JS_FreeCString(ctx, f);
    return NULL;
  }
  n = strlen(f) + strlen(src) + col + 64;
  out = malloc(n);
  snprintf(out, n, "%s:%d\n%s\n", f, line, src);
  {
    size_t l = strlen(out);
    for (i = 1; i < col && l + 2 < n; i++) {
      /* keep tabs so the caret lines up */
      out[l++] = (i - 1 < (int)strlen(src) && src[i - 1] == '\t') ? '\t' : ' ';
    }
    out[l++] = '^';
    out[l++] = '\n';
    out[l] = 0;
  }
  free(src);
  JS_FreeCString(ctx, f);
  return out;
}

static void print_value_to_stderr(JSContext *ctx, JSValueConst v) {
  const char *s = JS_ToCString(ctx, v);
  if (s) {
    fprintf(stderr, "%s\n", s);
    JS_FreeCString(ctx, s);
  } else {
    JS_FreeValue(ctx, JS_GetException(ctx));
    fprintf(stderr, "<toString() threw exception>\n");
  }
}

void node_print_uncaught(Env *env, JSValueConst err, bool from_promise) {
  JSContext *ctx = env->ctx;
  JSValue stack = JS_UNDEFINED, arrow_v = JS_UNDEFINED, sym, decorated_v = JS_UNDEFINED;
  char *arrow = NULL;
  bool decorated = false;
  (void)from_promise;

  if (JS_IsObject(err)) {
    sym = env_get_private_symbol(env, "arrow_message_private_symbol");
    if (JS_IsSymbol(sym)) {
      JSAtom a = JS_ValueToAtom(ctx, sym);
      arrow_v = JS_GetProperty(ctx, err, a);
      JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, sym);
    sym = env_get_private_symbol(env, "decorated_private_symbol");
    if (JS_IsSymbol(sym)) {
      JSAtom a = JS_ValueToAtom(ctx, sym);
      decorated_v = JS_GetProperty(ctx, err, a);
      decorated = JS_ToBool(ctx, decorated_v);
      JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, decorated_v);
  }
  if (JS_IsString(arrow_v)) {
    const char *s = JS_ToCString(ctx, arrow_v);
    arrow = s ? strdup(s) : NULL;
    JS_FreeCString(ctx, s);
  } else {
    arrow = exception_arrow(env, err);
  }
  JS_FreeValue(ctx, arrow_v);

  if (JS_IsObject(err)) {
    if (env->can_call_into_js && JS_IsFunction(ctx, env->enhance_stack_before_inspector)) {
      JSValue r = JS_Call(ctx, env->enhance_stack_before_inspector, JS_UNDEFINED, 1, &err);
      if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
      else {
        JS_FreeValue(ctx, stack);
        stack = r;
      }
    }
    if (env->can_call_into_js && JS_IsFunction(ctx, env->enhance_stack_after_inspector)) {
      JSValue r = JS_Call(ctx, env->enhance_stack_after_inspector, JS_UNDEFINED, 1, &err);
      if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
      else {
        JS_FreeValue(ctx, stack);
        stack = r;
      }
    }
    if (JS_IsUndefined(stack)) {
      stack = JS_GetPropertyStr(ctx, err, "stack");
      if (JS_IsException(stack)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        stack = JS_UNDEFINED;
      }
    }
  }

  if (!JS_IsUndefined(stack) && !JS_IsNull(stack)) {
    const char *t = JS_ToCString(ctx, stack);
    if (t && t[0]) {
      if (arrow && !decorated)
        fprintf(stderr, "%s\n%s\n", arrow, t);
      else
        fprintf(stderr, "%s\n", t);
      JS_FreeCString(ctx, t);
      goto done;
    }
    if (t)
      JS_FreeCString(ctx, t);
    else
      JS_FreeValue(ctx, JS_GetException(ctx));
  }
  {
    JSValue message = JS_UNDEFINED, name = JS_UNDEFINED;
    if (JS_IsObject(err)) {
      message = JS_GetPropertyStr(ctx, err, "message");
      name = JS_GetPropertyStr(ctx, err, "name");
    }
    if (JS_IsException(message) || JS_IsUndefined(message) ||
        JS_IsException(name) || JS_IsUndefined(name)) {
      if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
      if (arrow)
        fprintf(stderr, "%s\n", arrow);
      if (JS_IsString(err) || !JS_IsObject(err)) {
        print_value_to_stderr(ctx, err);
      } else {
        print_value_to_stderr(ctx, err);
      }
    } else {
      const char *n = JS_ToCString(ctx, name), *m = JS_ToCString(ctx, message);
      if (arrow && !decorated)
        fprintf(stderr, "%s\n%s: %s\n", arrow, n ? n : "", m ? m : "");
      else
        fprintf(stderr, "%s: %s\n", n ? n : "", m ? m : "");
      JS_FreeCString(ctx, n);
      JS_FreeCString(ctx, m);
    }
    JS_FreeValue(ctx, message);
    JS_FreeValue(ctx, name);
    if (!node_option_bool(env->options, "--trace-uncaught"))
      fprintf(stderr, "(Use `node --trace-uncaught ...` to show where the exception was thrown)\n");
  }
done:
  JS_FreeValue(ctx, stack);
  free(arrow);
  if (!node_option_bool(env->options, "--no-extra-info-on-fatal-exception"))
    fprintf(stderr, "\nNode.js %s\n", NODE_VERSION_STRING);
  fflush(stderr);
}

void node_report_exception(Env *env, JSValueConst err) {
  node_print_uncaught(env, err, false);
}

/* ---------------------------------------------------------------------- */
/* internalBinding('errors') */

static JSValue set_prepare_stack_trace_callback(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->prepare_stack_trace_cb);
  env->prepare_stack_trace_cb = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue set_get_source_map_error_source(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->get_source_map_error_source);
  env->get_source_map_error_source = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue set_source_maps_enabled(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->source_maps_enabled);
  env->source_maps_enabled = JS_NewBool(ctx, JS_ToBool(ctx, ARG(0)));
  return JS_UNDEFINED;
}

static JSValue set_maybe_cache_generated_source_map(JSContext *ctx, JSValueConst this_val,
                                                    int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->maybe_cache_generated_source_map);
  env->maybe_cache_generated_source_map = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue set_enhance_stack_for_fatal_exception(JSContext *ctx, JSValueConst this_val,
                                                     int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->enhance_stack_before_inspector);
  JS_FreeValue(ctx, env->enhance_stack_after_inspector);
  env->enhance_stack_before_inspector = JS_DupValue(ctx, ARG(0));
  env->enhance_stack_after_inspector = JS_DupValue(ctx, ARG(1));
  return JS_UNDEFINED;
}

/* V8's Object::NoSideEffectsToString */
static JSValue no_side_effects_to_string(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  JSValueConst v = ARG(0);
  uint32_t flags;
  if (!JS_IsObject(v)) {
    if (JS_IsSymbol(v)) {
      JSValue d = JS_GetPropertyStr(ctx, v, "description"), r;
      const char *s = JS_ToCString(ctx, d);
      char buf[512];
      snprintf(buf, sizeof(buf), "Symbol(%s)", s && !JS_IsUndefined(d) ? s : "");
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, d);
      r = JS_NewString(ctx, buf);
      return r;
    }
    return JS_ToString(ctx, v);
  }
  flags = JS_NodeTypeFlags(v);
  if (flags & JS_NODE_TYPE_NATIVE_ERROR) {
    JSValue r = JS_ToString(ctx, v);
    if (JS_IsException(r)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_NewString(ctx, "Error");
    }
    return r;
  }
  if (JS_IsFunction(ctx, v)) {
    JSValue r = JS_ToString(ctx, v);
    if (JS_IsException(r)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_NewString(ctx, "function () { [native code] }");
    }
    return r;
  }
  if (JS_IsArray(v))
    return JS_NewString(ctx, "#<Array>");
  return JS_NewString(ctx, "#<Object>");
}

static JSValue trigger_uncaught_exception(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  node_trigger_uncaught_exception(env, JS_DupValue(ctx, ARG(0)), JS_ToBool(ctx, ARG(1)));
  return JS_UNDEFINED;
}

static JSValue get_error_source_positions(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue fname, obj;
  int line, col;
  char *src = NULL;
  obj = JS_NewObject(ctx);
  if (JS_NodeGetExceptionPosition(ctx, ARG(0), &fname, &line, &col) == 0) {
    const char *f = JS_ToCString(ctx, fname);
    if (f)
      src = read_source_line(env, f, line);
    JS_FreeCString(ctx, f);
    JS_SetPropertyStr(ctx, obj, "sourceLine", JS_NewString(ctx, src ? src : ""));
    JS_SetPropertyStr(ctx, obj, "scriptResourceName", fname);
    JS_SetPropertyStr(ctx, obj, "lineNumber", JS_NewInt32(ctx, line));
    JS_SetPropertyStr(ctx, obj, "startColumn", JS_NewInt32(ctx, col > 0 ? col - 1 : 0));
    free(src);
  } else {
    JS_SetPropertyStr(ctx, obj, "sourceLine", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "scriptResourceName", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "lineNumber", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "startColumn", JS_NewInt32(ctx, 0));
  }
  return obj;
}

JSValue binding_init_errors(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), codes;
  static const struct { const char *name; int code; } exit_codes[] = {
    { "kNoFailure", 0 }, { "kGenericUserError", 1 }, { "kInternalJSParseError", 3 },
    { "kInternalJSEvaluationFailure", 4 }, { "kV8FatalError", 5 },
    { "kInvalidFatalExceptionMonkeyPatching", 6 },
    { "kExceptionInFatalExceptionHandler", 7 }, { "kInvalidCommandLineArgument", 9 },
    { "kBootstrapFailure", 10 }, { "kInvalidCommandLineArgument2", 12 },
    { "kUnsettledTopLevelAwait", 13 }, { "kStartupSnapshotFailure", 14 },
    { "kAbort", 134 },
  };
  size_t i;
  nb_set_method(ctx, t, "setPrepareStackTraceCallback", set_prepare_stack_trace_callback, 1);
  nb_set_method(ctx, t, "setGetSourceMapErrorSource", set_get_source_map_error_source, 1);
  nb_set_method(ctx, t, "setSourceMapsEnabled", set_source_maps_enabled, 1);
  nb_set_method(ctx, t, "setMaybeCacheGeneratedSourceMap", set_maybe_cache_generated_source_map, 1);
  nb_set_method(ctx, t, "setEnhanceStackForFatalException", set_enhance_stack_for_fatal_exception, 2);
  nb_set_method(ctx, t, "noSideEffectsToString", no_side_effects_to_string, 1);
  nb_set_method(ctx, t, "triggerUncaughtException", trigger_uncaught_exception, 2);
  nb_set_method(ctx, t, "getErrorSourcePositions", get_error_source_positions, 1);
  codes = JS_NewObject(ctx);
  for (i = 0; i < countof(exit_codes); i++)
    nb_set_int(ctx, codes, exit_codes[i].name, exit_codes[i].code);
  nb_define_readonly(ctx, t, "exitCodes", codes);
  return t;
}
