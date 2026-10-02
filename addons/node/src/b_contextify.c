/* internalBinding('contextify') (node_contextify.cc): scripts, functions
 * compiled for the CommonJS loader, vm contexts, and module syntax
 * detection. */
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

/* ---- host defined options: symbol <-> id ---- */

int32_t node_host_defined_id_for(Env *env, JSValueConst symbol) {
  JSContext *ctx = env->ctx;
  JSValue lenv;
  uint32_t len = 0, i;
  if (!JS_IsSymbol(symbol))
    return -1;
  lenv = JS_GetPropertyStr(ctx, env->host_defined_symbols, "length");
  JS_ToUint32(ctx, &len, lenv);
  for (i = 1; i < len; i++) {
    JSValue v = JS_GetPropertyUint32(ctx, env->host_defined_symbols, i);
    bool same = JS_IsStrictEqual(ctx, v, symbol);
    JS_FreeValue(ctx, v);
    if (same)
      return (int32_t)i;
  }
  i = env->next_host_defined_id++;
  JS_SetPropertyUint32(ctx, env->host_defined_symbols, i, JS_DupValue(ctx, symbol));
  return (int32_t)i;
}

static void set_private(Env *env, JSValueConst obj, const char *name, JSValue v) {
  JSContext *ctx = env->ctx;
  JSValue sym = env_get_private_symbol(env, name);
  JSAtom a = JS_ValueToAtom(ctx, sym);
  JS_DefinePropertyValue(ctx, obj, a, v, JS_PROP_C_W_E);
  JS_FreeAtom(ctx, a);
  JS_FreeValue(ctx, sym);
}

static JSValue get_private(Env *env, JSValueConst obj, const char *name) {
  JSContext *ctx = env->ctx;
  JSValue sym = env_get_private_symbol(env, name), v;
  JSAtom a = JS_ValueToAtom(ctx, sym);
  v = JS_IsObject(obj) ? JS_GetProperty(ctx, obj, a) : JS_UNDEFINED;
  JS_FreeAtom(ctx, a);
  JS_FreeValue(ctx, sym);
  return v;
}

/* the //# sourceMappingURL= or //# sourceURL= comment near the end */
static JSValue magic_comment(JSContext *ctx, const char *src, size_t len, const char *key) {
  size_t start = len > 8192 ? len - 8192 : 0, klen = strlen(key);
  const char *best = NULL;
  size_t i;
  for (i = start; i + klen + 4 <= len; i++) {
    if ((src[i] == '/' && src[i + 1] == '/') && (src[i + 2] == '#' || src[i + 2] == '@') &&
        src[i + 3] == ' ' && !strncmp(src + i + 4, key, klen) && src[i + 4 + klen] == '=')
      best = src + i + 4 + klen + 1;
  }
  if (!best)
    return JS_UNDEFINED;
  {
    const char *e = best;
    while (e < src + len && *e != '\n' && *e != '\r' && *e != ' ' && *e != '\t')
      e++;
    if (e == best)
      return JS_UNDEFINED;
    return JS_NewStringLen(ctx, best, e - best);
  }
}

/* compiles "(function (params) {\n" + body + "\n})" so that body keeps its
   lines and columns; returns the function */
JSValue node_contextify_compile_function(JSContext *ctx, const char *src, size_t len,
                                         const char *filename, int line_offset,
                                         const char **params, int nparams,
                                         int32_t host_defined_id) {
  size_t plen = 0, n;
  int i;
  char *buf, *p;
  JSEvalOptions opts;
  JSValue r;
  int32_t prev;
  for (i = 0; i < nparams; i++)
    plen += strlen(params[i]) + 2;
  buf = malloc(len + plen + 32);
  p = buf;
  p += sprintf(p, "(function (");
  for (i = 0; i < nparams; i++)
    p += sprintf(p, "%s%s", i ? ", " : "", params[i]);
  p += sprintf(p, ") {\n");
  memcpy(p, src, len);
  /* a leading #! line is a comment to the function body */
  if (len >= 2 && p[0] == '#' && p[1] == '!') {
    p[0] = '/';
    p[1] = '/';
  }
  p += len;
  p += sprintf(p, "\n})");
  n = p - buf;
  memset(&opts, 0, sizeof(opts));
  opts.version = JS_EVAL_OPTIONS_VERSION;
  /* the wrapper's header line is line_offset: the source starts below */
  opts.eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_NODE_LINE | JS_EVAL_FLAG_COMPILE_ONLY;
  opts.filename = filename;
  opts.line_num = line_offset;
  prev = JS_SetCompileHostDefinedId(JS_GetRuntime(ctx), host_defined_id > 0 ? host_defined_id : -1);
  r = node_cached_compile(ctx, buf, n, &opts);
  JS_SetCompileHostDefinedId(JS_GetRuntime(ctx), prev);
  free(buf);
  if (JS_IsException(r))
    return r;
  /* the wrapper expression: its value is the function */
  return JS_EvalFunction(ctx, r);
}

static bool is_syntax_error(JSContext *ctx, JSValueConst e) {
  JSValue name;
  const char *s;
  bool r = false;
  if (!JS_IsError(e))
    return false;
  name = JS_GetPropertyStr(ctx, e, "name");
  s = JS_ToCString(ctx, name);
  r = s && !strcmp(s, "SyntaxError");
  JS_FreeCString(ctx, s);
  JS_FreeValue(ctx, name);
  return r;
}

static bool parses_as_module(JSContext *ctx, const char *src, size_t len, const char *filename) {
  if (JS_NodeCheckModuleSyntax(ctx, src, len, filename))
    return true;
  JS_FreeValue(ctx, JS_GetException(ctx));
  return false;
}

static const char *cjs_params[] = { "exports", "require", "module", "__filename", "__dirname" };

static JSValue c_compile_function_for_cjs_loader(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
  size_t len;
  char *src = node_string_to_utf8(ctx, ARG(0), &len);
  char *filename = node_string_to_utf8(ctx, ARG(1), NULL);
  bool should_detect = JS_ToBool(ctx, ARG(3));
  JSValue fn, res, exc = JS_UNDEFINED;
  bool can_parse_as_esm = false;
  if (!src || !filename) {
    free(src);
    free(filename);
    return JS_EXCEPTION;
  }
  fn = node_contextify_compile_function(ctx, src, len, filename, 0, cjs_params, 5,
                                        HDO_VM_DYNAMIC_IMPORT_DEFAULT_INTERNAL);
  if (JS_IsException(fn)) {
    exc = JS_GetException(ctx);
    if (is_syntax_error(ctx, exc))
      can_parse_as_esm = parses_as_module(ctx, src, len, filename);
    if (!can_parse_as_esm || !should_detect) {
      free(src);
      free(filename);
      if (can_parse_as_esm && !should_detect) {
        /* require() of ES module syntax with require(esm) off: the CJS error */
      }
      return JS_Throw(ctx, exc);
    }
    JS_FreeValue(ctx, exc);
    fn = JS_UNDEFINED;
  }
  res = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, res, "cachedDataRejected", JS_FALSE);
  JS_SetPropertyStr(ctx, res, "sourceMapURL",
                    JS_IsUndefined(fn) ? JS_UNDEFINED : magic_comment(ctx, src, len, "sourceMappingURL"));
  JS_SetPropertyStr(ctx, res, "sourceURL",
                    JS_IsUndefined(fn) ? JS_UNDEFINED : magic_comment(ctx, src, len, "sourceURL"));
  JS_SetPropertyStr(ctx, res, "function", fn);
  JS_SetPropertyStr(ctx, res, "canParseAsESM", JS_NewBool(ctx, can_parse_as_esm));
  free(src);
  free(filename);
  return res;
}

/* containsModuleSyntax(code, filename, resourceName, cjsVar): fails as
   CommonJS but parses as a module */
static JSValue c_contains_module_syntax(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  size_t len;
  char *src = node_string_to_utf8(ctx, ARG(0), &len);
  char *filename = node_string_to_utf8(ctx, ARG(1), NULL);
  JSValue fn;
  bool r = false;
  if (!src || !filename) {
    free(src);
    free(filename);
    return JS_EXCEPTION;
  }
  fn = node_contextify_compile_function(ctx, src, len, filename, 0, cjs_params, 5, -1);
  if (JS_IsException(fn)) {
    JSValue e = JS_GetException(ctx);
    if (is_syntax_error(ctx, e))
      r = parses_as_module(ctx, src, len, filename);
    JS_FreeValue(ctx, e);
  } else {
    JS_FreeValue(ctx, fn);
  }
  free(src);
  free(filename);
  return JS_NewBool(ctx, r);
}

static JSValue c_should_retry_as_esm(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  size_t len;
  char *src = node_string_to_utf8(ctx, ARG(1), &len);
  bool r = src && parses_as_module(ctx, src, len, "[retry]");
  free(src);
  return JS_NewBool(ctx, r);
}

/* ---- vm contexts ---- */

typedef struct {
  JSContext *ctx;     /* the context (owned) */
  JSValue sandbox;    /* the contextified object (weak) */
} ContextifyContext;

static JSClassID contextify_context_class_id;

static void cc_finalizer(JSRuntime *rt, JSValueConst val) {
  ContextifyContext *c = JS_GetOpaque(val, contextify_context_class_id);
  if (c) {
    /* contexts are freed with the runtime */
    free(c);
  }
}

JSContext *node_new_vm_context(Env *env);

JSContext *node_context_of(Env *env, JSValueConst sandbox) {
  JSValue w;
  ContextifyContext *c;
  if (JS_IsNull(sandbox) || JS_IsUndefined(sandbox))
    return env->ctx;
  w = get_private(env, sandbox, "contextify_context_private_symbol");
  c = JS_GetOpaque(w, contextify_context_class_id);
  JS_FreeValue(env->ctx, w);
  return c ? c->ctx : NULL;
}

/* the sandbox's properties become the context's globals before a run, and
   what the run changed goes back after it */
static void sync_in(JSContext *dst, JSContext *src, JSValueConst sandbox) {
  JSValue g = JS_GetGlobalObject(dst);
  JSPropertyEnum *props;
  uint32_t n, i;
  if (JS_GetOwnPropertyNames(src, &props, &n, sandbox,
                             JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_ENUM_ONLY) == 0) {
    for (i = 0; i < n; i++) {
      JSValue v = JS_GetProperty(src, sandbox, props[i].atom);
      JS_SetProperty(dst, g, props[i].atom, v);
    }
    JS_FreePropertyEnum(src, props, n);
  }
  JS_FreeValue(dst, g);
}

static void sync_out(JSContext *src, JSContext *dst, JSValueConst sandbox,
                     JSValueConst before_keys) {
  JSValue g = JS_GetGlobalObject(src);
  JSPropertyEnum *props;
  uint32_t n, i;
  (void)before_keys;
  if (JS_GetOwnPropertyNames(src, &props, &n, g,
                             JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_ENUM_ONLY) == 0) {
    for (i = 0; i < n; i++) {
      JSValue v = JS_GetProperty(src, g, props[i].atom);
      JS_SetProperty(dst, sandbox, props[i].atom, v);
    }
    JS_FreePropertyEnum(src, props, n);
  }
  JS_FreeValue(src, g);
}

static JSValue c_make_context(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValueConst sandbox = ARG(0);
  JSContext *nctx = node_new_vm_context(env);
  ContextifyContext *c;
  JSValue w, nsym, result;
  int32_t id;
  if (!nctx)
    return JS_ThrowOutOfMemory(ctx);
  JS_NewClassID(env->rt, &contextify_context_class_id);
  if (!JS_IsRegisteredClass(env->rt, contextify_context_class_id)) {
    JSClassDef def = { .class_name = "ContextifyContext", .finalizer = cc_finalizer };
    JS_NewClass(env->rt, contextify_context_class_id, &def);
  }
  c = calloc(1, sizeof(*c));
  c->ctx = nctx;
  w = JS_NewObjectClass(ctx, contextify_context_class_id);
  JS_SetOpaque(w, c);
  nsym = env_get_symbol(env, "vm_context_no_contextify");
  if (JS_IsStrictEqual(ctx, sandbox, nsym)) {
    result = JS_GetGlobalObject(nctx);
  } else {
    result = JS_DupValue(ctx, sandbox);
  }
  JS_FreeValue(ctx, nsym);
  c->sandbox = result;
  set_private(env, result, "contextify_context_private_symbol", w);
  id = node_host_defined_id_for(env, ARG(6));
  if (JS_IsSymbol(ARG(6)))
    set_private(env, result, "host_defined_option_symbol", JS_DupValue(ctx, ARG(6)));
  (void)id;
  return result;
}

/* ---- ContextifyScript ---- */

typedef struct {
  char *source;
  size_t len;
  char *filename;
  int line_offset;
  int32_t host_defined_id;
  JSContext *compiled_in;
  JSValue bytecode;     /* compiled in compiled_in */
} ContextifyScript;

static JSClassID script_class_id;

static void script_finalizer(JSRuntime *rt, JSValueConst val) {
  ContextifyScript *s = JS_GetOpaque(val, script_class_id);
  if (s) {
    JS_FreeValueRT(rt, s->bytecode);
    free(s->source);
    free(s->filename);
    free(s);
  }
}

static JSValue compile_script(JSContext *ctx, ContextifyScript *s) {
  JSEvalOptions opts;
  int32_t prev;
  JSValue r;
  memset(&opts, 0, sizeof(opts));
  opts.version = JS_EVAL_OPTIONS_VERSION;
  opts.eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY;
  opts.filename = s->filename;
  opts.line_num = s->line_offset + 1;
  prev = JS_SetCompileHostDefinedId(JS_GetRuntime(ctx),
                                    s->host_defined_id > 0 ? s->host_defined_id : -1);
  r = JS_Eval2(ctx, s->source, s->len, &opts);
  JS_SetCompileHostDefinedId(JS_GetRuntime(ctx), prev);
  return r;
}

static JSValue script_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                           JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue obj = nb_new_instance(ctx, new_target, script_class_id), bc;
  ContextifyScript *s;
  JSContext *pctx = ctx;
  if (JS_IsException(obj))
    return obj;
  s = calloc(1, sizeof(*s));
  s->bytecode = JS_UNDEFINED;
  s->source = node_string_to_utf8(ctx, ARG(0), &s->len);
  s->filename = node_string_to_utf8(ctx, ARG(1), NULL);
  s->line_offset = nb_int32(ctx, ARG(2), 0);
  s->host_defined_id = node_host_defined_id_for(env, ARG(7));
  JS_SetOpaque(obj, s);
  if (!s->source || !s->filename) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  if (JS_IsObject(ARG(6))) {
    JSContext *c = node_context_of(env, ARG(6));
    if (c)
      pctx = c;
  }
  bc = compile_script(pctx, s);
  if (JS_IsException(bc)) {
    JS_FreeValue(ctx, obj);
    if (pctx != ctx)
      JS_Throw(ctx, JS_GetException(pctx));
    return JS_EXCEPTION;
  }
  s->bytecode = bc;
  s->compiled_in = pctx;
  JS_SetPropertyStr(ctx, obj, "sourceMapURL", magic_comment(ctx, s->source, s->len, "sourceMappingURL"));
  JS_SetPropertyStr(ctx, obj, "sourceURL", magic_comment(ctx, s->source, s->len, "sourceURL"));
  JS_SetPropertyStr(ctx, obj, "cachedDataRejected", JS_FALSE);
  if (JS_IsSymbol(ARG(7)))
    set_private(env, obj, "host_defined_option_symbol", JS_DupValue(ctx, ARG(7)));
  return obj;
}

/* ---- the SIGINT watchdog (sigint_watchdog.cc): while the REPL evaluates,
   Ctrl+C interrupts the script instead of ending the process ---- */

static atomic_int sigint_watchers, sigint_pending, sigint_breaks;
static struct sigaction sigint_saved;
static pthread_mutex_t sigint_lock = PTHREAD_MUTEX_INITIALIZER;

static void on_sigint(int signo) {
  atomic_store(&sigint_pending, 1);
}

static JSValue c_start_sigint_watchdog(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  pthread_mutex_lock(&sigint_lock);
  if (sigint_watchers++ == 0) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    atomic_store(&sigint_pending, 0);
    sigaction(SIGINT, &sa, &sigint_saved);
  }
  pthread_mutex_unlock(&sigint_lock);
  return JS_TRUE;
}

/* true when a SIGINT came while it watched */
static JSValue c_stop_sigint_watchdog(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  bool had;
  pthread_mutex_lock(&sigint_lock);
  had = atomic_exchange(&sigint_pending, 0) != 0;
  if (sigint_watchers > 0 && --sigint_watchers == 0)
    sigaction(SIGINT, &sigint_saved, NULL);
  pthread_mutex_unlock(&sigint_lock);
  return JS_NewBool(ctx, had);
}

static JSValue c_watchdog_has_pending_sigint(JSContext *ctx, JSValueConst this_val, int argc,
                                             JSValueConst *argv) {
  return JS_NewBool(ctx, atomic_load(&sigint_pending) != 0);
}

/* for the main runtime's interrupt handler: stop the running script */
bool node_sigint_interrupt(void) {
  return atomic_load(&sigint_breaks) > 0 && atomic_load(&sigint_pending) != 0;
}

/* the script's result; an interruption by SIGINT becomes Node's error */
static JSValue sigint_result(JSContext *ctx, JSValue r) {
  JSValue e;
  if (!JS_IsException(r))
    return r;
  e = JS_GetException(ctx);
  if (JS_IsUncatchableError(e) && atomic_exchange(&sigint_pending, 0)) {
    JS_FreeValue(ctx, e);
    JS_ResetUncatchableError(ctx);
    return node_throw_error(ctx, "ERR_SCRIPT_EXECUTION_INTERRUPTED",
                            "Script execution was interrupted by `SIGINT`");
  }
  return JS_Throw(ctx, e);
}

/* ---- the timeout option (Watchdog): a deadline for this thread's runtime,
   which its interrupt handler checks ---- */

static _Thread_local uint64_t vm_deadline;   /* uv_hrtime(), 0: none */
static _Thread_local bool vm_timed_out;

bool node_vm_timeout_interrupt(void) {
  if (vm_deadline && uv_hrtime() >= vm_deadline) {
    vm_timed_out = true;
    return true;
  }
  return false;
}

/* eval with a timeout (ms, <= 0: none) and the SIGINT break; the result, or
   Node's errors for an interruption */
static JSValue eval_guarded(JSContext *ctx, JSContext *rctx, JSValue bc, int64_t timeout,
                            bool brk) {
  uint64_t saved = vm_deadline;
  JSValue r, e;
  if (timeout > 0) {
    uint64_t d = uv_hrtime() + (uint64_t)timeout * 1000000;
    if (!vm_deadline || d < vm_deadline)
      vm_deadline = d;
  }
  if (brk)
    atomic_fetch_add(&sigint_breaks, 1);
  r = JS_EvalFunction(rctx, bc);
  if (brk)
    atomic_fetch_sub(&sigint_breaks, 1);
  vm_deadline = saved;
  if (!JS_IsException(r))
    return r;
  e = JS_GetException(rctx);
  if (JS_IsUncatchableError(e) && vm_timed_out && timeout > 0 &&
      (!saved || uv_hrtime() < saved)) {
    /* our own deadline (an outer one still running stays uncatchable) */
    vm_timed_out = false;
    JS_FreeValue(rctx, e);
    JS_ResetUncatchableError(rctx);
    if (rctx != ctx)
      JS_ResetUncatchableError(ctx);
    return node_throw_error(ctx, "ERR_SCRIPT_EXECUTION_TIMEOUT",
                            "Script execution timed out after %lldms", (long long)timeout);
  }
  JS_Throw(ctx, e);
  return brk ? sigint_result(ctx, JS_EXCEPTION) : JS_EXCEPTION;
}

/* runInContext(contextifiedObject | null, timeout, displayErrors,
   breakOnSigint, breakOnFirstLine) */
static JSValue script_run_in_context(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  Env *env = env_get(ctx);
  ContextifyScript *s = JS_GetOpaque2(ctx, this_val, script_class_id);
  JSContext *rctx;
  JSValue bc, r;
  bool sandboxed, brk = JS_ToBool(ctx, ARG(3));
  int64_t timeout = -1;
  if (!s)
    return JS_EXCEPTION;
  if (JS_IsNumber(ARG(1)))
    JS_ToInt64(ctx, &timeout, ARG(1));
  rctx = node_context_of(env, ARG(0));
  if (!rctx)
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                                 "The \"contextifiedObject\" argument must be an vm.Context");
  sandboxed = rctx != env->ctx;
  if (rctx == s->compiled_in) {
    bc = JS_DupValue(ctx, s->bytecode);
  } else {
    bc = compile_script(rctx, s);
    if (JS_IsException(bc)) {
      JS_Throw(ctx, JS_GetException(rctx));
      return JS_EXCEPTION;
    }
  }
  if (sandboxed) {
    JSValue sb = get_private(env, ARG(0), "contextify_context_private_symbol");
    ContextifyContext *c = JS_GetOpaque(sb, contextify_context_class_id);
    JS_FreeValue(ctx, sb);
    if (c && !JS_IsStrictEqual(ctx, c->sandbox, JS_GetGlobalObject(rctx)))
      sync_in(rctx, ctx, ARG(0));
    r = eval_guarded(ctx, rctx, bc, timeout, brk);
    if (c)
      sync_out(rctx, ctx, ARG(0), JS_UNDEFINED);
    return r;
  }
  if (!brk && timeout <= 0)
    return JS_EvalFunction(ctx, bc);
  return eval_guarded(ctx, ctx, bc, timeout, brk);
}

static JSValue script_create_cached_data(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  return nb_new_buffer(ctx, "", 0);
}

static const JSCFunctionListEntry script_proto[] = {
  JS_CFUNC_DEF("runInContext", 5, script_run_in_context),
  JS_CFUNC_DEF("createCachedData", 0, script_create_cached_data),
};

/* compileFunction(code, filename, lineOffset, columnOffset, cachedData,
   produceCachedData, parsingContext, contextExtensions, params,
   hostDefinedOptionId) */
static JSValue c_compile_function(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  Env *env = env_get(ctx);
  size_t len;
  char *src = node_string_to_utf8(ctx, ARG(0), &len);
  char *filename = node_string_to_utf8(ctx, ARG(1), NULL);
  int line_offset = nb_int32(ctx, ARG(2), 0);
  JSValueConst params_v = ARG(8);
  const char **params = NULL;
  int nparams = 0, i;
  JSContext *pctx = ctx;
  int32_t id = node_host_defined_id_for(env, ARG(9));
  JSValue fn, res;
  if (!src || !filename) {
    free(src);
    free(filename);
    return JS_EXCEPTION;
  }
  if (JS_IsArray(params_v)) {
    JSValue lenv = JS_GetPropertyStr(ctx, params_v, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, lenv);
    params = calloc(n + 1, sizeof(char *));
    for (i = 0; i < (int)n; i++) {
      JSValue p = JS_GetPropertyUint32(ctx, params_v, i);
      params[i] = JS_ToCString(ctx, p);
      JS_FreeValue(ctx, p);
    }
    nparams = n;
  }
  if (JS_IsObject(ARG(6))) {
    JSContext *c = node_context_of(env, ARG(6));
    if (c)
      pctx = c;
  }
  fn = node_contextify_compile_function(pctx, src, len, filename, line_offset, params,
                                        nparams, id);
  for (i = 0; i < nparams; i++)
    JS_FreeCString(ctx, params[i]);
  free(params);
  if (JS_IsException(fn)) {
    free(src);
    free(filename);
    if (pctx != ctx)
      JS_Throw(ctx, JS_GetException(pctx));
    return JS_EXCEPTION;
  }
  if (JS_IsSymbol(ARG(9)))
    set_private(env, fn, "host_defined_option_symbol", JS_DupValue(ctx, ARG(9)));
  res = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, res, "function", fn);
  JS_SetPropertyStr(ctx, res, "sourceMapURL", magic_comment(ctx, src, len, "sourceMappingURL"));
  JS_SetPropertyStr(ctx, res, "sourceURL", magic_comment(ctx, src, len, "sourceURL"));
  if (JS_ToBool(ctx, ARG(5))) {
    JS_SetPropertyStr(ctx, res, "cachedDataProduced", JS_FALSE);
  }
  if (!JS_IsUndefined(ARG(4)))
    JS_SetPropertyStr(ctx, res, "cachedDataRejected", JS_TRUE);
  free(src);
  free(filename);
  return res;
}

static JSValue c_measure_memory(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  JSMemoryUsage mu;
  JSValue o = JS_NewObject(ctx), total = JS_NewObject(ctx), arr;
  JSValue res[2], p;
  JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mu);
  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, (double)mu.malloc_size));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, (double)mu.malloc_size));
  JS_SetPropertyStr(ctx, total, "jsMemoryEstimate", JS_NewFloat64(ctx, (double)mu.memory_used_size));
  JS_SetPropertyStr(ctx, total, "jsMemoryRange", arr);
  JS_SetPropertyStr(ctx, o, "total", total);
  p = JS_NewPromiseCapability(ctx, res);
  JS_FreeValue(ctx, JS_Call(ctx, res[0], JS_UNDEFINED, 1, (JSValueConst *)&o));
  JS_FreeValue(ctx, o);
  JS_FreeValue(ctx, res[0]);
  JS_FreeValue(ctx, res[1]);
  return p;
}

static JSValue c_false(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_FALSE;
}

JSValue binding_init_contextify(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), c = JS_NewObject(ctx), mm = JS_NewObject(ctx);
  JSValue mode = JS_NewObject(ctx), exec = JS_NewObject(ctx);
  NodeClassDef def = { .name = "ContextifyScript", .class_id = &script_class_id,
                       .ctor = script_ctor, .ctor_length = 8,
                       .finalizer = script_finalizer, .proto_funcs = script_proto,
                       .proto_funcs_count = countof(script_proto),
                       .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  nb_set_method(ctx, t, "makeContext", c_make_context, 7);
  nb_set_method(ctx, t, "compileFunction", c_compile_function, 10);
  nb_set_method(ctx, t, "compileFunctionForCJSLoader", c_compile_function_for_cjs_loader, 4);
  nb_set_method(ctx, t, "containsModuleSyntax", c_contains_module_syntax, 4);
  nb_set_method(ctx, t, "shouldRetryAsESM", c_should_retry_as_esm, 3);
  nb_set_method(ctx, t, "startSigintWatchdog", c_start_sigint_watchdog, 0);
  nb_set_method(ctx, t, "stopSigintWatchdog", c_stop_sigint_watchdog, 0);
  nb_set_method(ctx, t, "watchdogHasPendingSigint", c_watchdog_has_pending_sigint, 0);
  nb_set_method(ctx, t, "measureMemory", c_measure_memory, 2);
  nb_set_int(ctx, mode, "SUMMARY", 0);
  nb_set_int(ctx, mode, "DETAILED", 1);
  nb_set_int(ctx, exec, "DEFAULT", 0);
  nb_set_int(ctx, exec, "EAGER", 1);
  nb_set(ctx, mm, "mode", mode);
  nb_set(ctx, mm, "execution", exec);
  nb_set(ctx, c, "measureMemory", mm);
  nb_set(ctx, t, "constants", c);
  return t;
}
