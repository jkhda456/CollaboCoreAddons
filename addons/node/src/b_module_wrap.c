/* internalBinding('module_wrap') (module_wrap.cc): ES modules over QuickJS's
 * module records, linked by Node's loader rather than QuickJS's. */
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

int32_t node_host_defined_id_for(Env *env, JSValueConst symbol);
JSContext *node_context_of(Env *env, JSValueConst sandbox);

typedef struct {
  JSModuleDef *m;
  JSContext *ctx;          /* the context it was compiled in */
  bool synthetic;
  bool linked;
  JSValue linked_requests; /* array of ModuleWraps, in request order */
  JSValue evaluation_steps;
  JSValue self;            /* weak: the wrap object */
} ModuleWrap;

static JSClassID module_wrap_class_id;

static void mw_finalizer(JSRuntime *rt, JSValueConst val) {
  ModuleWrap *w = JS_GetOpaque(val, module_wrap_class_id);
  if (w) {
    JS_FreeValueRT(rt, w->linked_requests);
    JS_FreeValueRT(rt, w->evaluation_steps);
    free(w);
  }
}

static void mw_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
  ModuleWrap *w = JS_GetOpaque(val, module_wrap_class_id);
  if (w) {
    JS_MarkValue(rt, w->linked_requests, mark_func);
    JS_MarkValue(rt, w->evaluation_steps, mark_func);
  }
}

JSValue module_wrap_from_module(Env *env, JSModuleDef *m) {
  return JS_GetModulePrivateValue(env->ctx, m);
}

static ModuleWrap *unwrap(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, module_wrap_class_id);
}

/* a synthetic module's evaluation: the JS steps, with the wrap as this */
static int synthetic_init(JSContext *ctx, JSModuleDef *m) {
  Env *env = env_get(ctx);
  JSValue wrap = JS_GetModulePrivateValue(ctx, m), r;
  ModuleWrap *w = JS_GetOpaque(wrap, module_wrap_class_id);
  if (!w || !JS_IsFunction(ctx, w->evaluation_steps)) {
    JS_FreeValue(ctx, wrap);
    return 0;
  }
  (void)env;
  r = JS_Call(ctx, w->evaluation_steps, wrap, 0, NULL);
  JS_FreeValue(ctx, wrap);
  if (JS_IsException(r))
    return -1;
  JS_FreeValue(ctx, r);
  return 0;
}

static JSValue magic_comment(JSContext *ctx, const char *src, size_t len, const char *key) {
  size_t start = len > 8192 ? len - 8192 : 0, klen = strlen(key), i;
  const char *best = NULL;
  for (i = start; i + klen + 4 <= len; i++) {
    if (src[i] == '/' && src[i + 1] == '/' && (src[i + 2] == '#' || src[i + 2] == '@') &&
        src[i + 3] == ' ' && !strncmp(src + i + 4, key, klen) && src[i + 4 + klen] == '=')
      best = src + i + 4 + klen + 1;
  }
  if (!best)
    return JS_UNDEFINED;
  {
    const char *e = best;
    while (e < src + len && *e != '\n' && *e != '\r' && *e != ' ' && *e != '\t')
      e++;
    return e == best ? JS_UNDEFINED : JS_NewStringLen(ctx, best, e - best);
  }
}

static void set_private(Env *env, JSValueConst obj, const char *name, JSValue v) {
  JSContext *ctx = env->ctx;
  JSValue sym = env_get_private_symbol(env, name);
  JSAtom a = JS_ValueToAtom(ctx, sym);
  JS_DefinePropertyValue(ctx, obj, a, v, JS_PROP_C_W_E);
  JS_FreeAtom(ctx, a);
  JS_FreeValue(ctx, sym);
}

/* new ModuleWrap(url, context, source, lineOffset, columnOffset, idSymbol|cachedData)
   new ModuleWrap(url, context, exportNames, evaluationSteps[, importedCJS]) */
static JSValue mw_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                       JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue obj = nb_new_instance(ctx, new_target, module_wrap_class_id);
  ModuleWrap *w;
  JSContext *mctx = ctx;
  char *url;
  if (JS_IsException(obj))
    return obj;
  w = calloc(1, sizeof(*w));
  w->linked_requests = JS_UNDEFINED;
  w->evaluation_steps = JS_UNDEFINED;
  w->self = obj;
  JS_SetOpaque(obj, w);
  url = node_string_to_utf8(ctx, ARG(0), NULL);
  if (!url)
    goto fail;
  if (!JS_IsUndefined(ARG(1))) {
    mctx = node_context_of(env, ARG(1));
    if (!mctx) {
      free(url);
      JS_ThrowTypeError(ctx, "invalid context");
      goto fail;
    }
  }
  w->ctx = mctx;
  if (JS_IsArray(ARG(2))) {
    /* synthetic */
    JSValue lenv = JS_GetPropertyStr(ctx, ARG(2), "length");
    uint32_t n = 0, i;
    JS_ToUint32(ctx, &n, lenv);
    w->synthetic = true;
    w->m = JS_NewCModule(mctx, url, synthetic_init);
    for (i = 0; i < n; i++) {
      JSValue name = JS_GetPropertyUint32(ctx, ARG(2), i);
      const char *s = JS_ToCString(ctx, name);
      if (s)
        JS_AddModuleExport(mctx, w->m, s);
      JS_FreeCString(ctx, s);
      JS_FreeValue(ctx, name);
    }
    w->evaluation_steps = JS_DupValue(ctx, ARG(3));
    if (JS_IsObject(ARG(4))) {
      JSValue sym = env_get_symbol(env, "imported_cjs_symbol");
      JSAtom a = JS_ValueToAtom(ctx, sym);
      JS_SetProperty(ctx, obj, a, JS_DupValue(ctx, ARG(4)));
      JS_FreeAtom(ctx, a);
      JS_FreeValue(ctx, sym);
    }
  } else {
    size_t len;
    char *src = node_string_to_utf8(ctx, ARG(2), &len);
    int32_t line_offset = nb_int32(ctx, ARG(3), 0), id, prev;
    JSValue id_symbol, mv;
    JSEvalOptions opts;
    if (!src) {
      free(url);
      goto fail;
    }
    if (JS_IsSymbol(ARG(5)))
      id_symbol = JS_DupValue(ctx, ARG(5));
    else
      id_symbol = JS_NewSymbol(ctx, url, false);
    id = node_host_defined_id_for(env, id_symbol);
    set_private(env, obj, "host_defined_option_symbol", id_symbol);
    memset(&opts, 0, sizeof(opts));
    opts.version = JS_EVAL_OPTIONS_VERSION;
    opts.eval_flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY;
    opts.filename = url;
    opts.line_num = line_offset + 1;
    prev = JS_SetCompileHostDefinedId(env->rt, id);
    mv = node_cached_compile(mctx, src, len, &opts);
    JS_SetCompileHostDefinedId(env->rt, prev);
    if (JS_IsException(mv)) {
      free(src);
      free(url);
      if (mctx != ctx)
        JS_Throw(ctx, JS_GetException(mctx));
      goto fail;
    }
    w->m = JS_GetModuleDef(mv);
    JS_FreeValue(mctx, mv);
    JS_SetPropertyStr(ctx, obj, "hasTopLevelAwait", JS_NewBool(ctx, JS_ModuleHasTLA(w->m)));
    JS_SetPropertyStr(ctx, obj, "sourceURL", magic_comment(ctx, src, len, "sourceURL"));
    JS_SetPropertyStr(ctx, obj, "sourceMapURL", magic_comment(ctx, src, len, "sourceMappingURL"));
    free(src);
  }
  if (!w->m) {
    free(url);
    goto fail;
  }
  JS_SetModulePrivateValue(mctx, w->m, JS_DupValue(ctx, obj));
  JS_SetPropertyStr(ctx, obj, "synthetic", JS_NewBool(ctx, w->synthetic));
  JS_SetPropertyStr(ctx, obj, "url", JS_DupValue(ctx, ARG(0)));
  set_private(env, obj, "source_map_data_private_symbol", JS_UNDEFINED);
  free(url);
  return obj;
fail:
  JS_FreeValue(ctx, obj);
  return JS_EXCEPTION;
}

static JSValue mw_get_module_requests(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  ModuleWrap *w = unwrap(ctx, this_val);
  JSValue arr;
  int i, n;
  if (!w)
    return JS_EXCEPTION;
  arr = JS_NewArray(ctx);
  n = JS_GetModuleRequestCount(w->m);
  for (i = 0; i < n; i++) {
    JSValue req = JS_NewObjectProto(ctx, JS_NULL);
    JSValue attrs = JS_GetModuleRequestAttributes(ctx, w->m, i), a2;
    JS_SetPropertyStr(ctx, req, "specifier", JS_GetModuleRequestSpecifier(ctx, w->m, i));
    a2 = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsObject(attrs)) {
      JSPropertyEnum *props;
      uint32_t np, k;
      JS_GetOwnPropertyNames(ctx, &props, &np, attrs, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);
      for (k = 0; k < np; k++)
        JS_SetProperty(ctx, a2, props[k].atom, JS_GetProperty(ctx, attrs, props[k].atom));
      JS_FreePropertyEnum(ctx, props, np);
    }
    JS_FreeValue(ctx, attrs);
    JS_SetPropertyStr(ctx, req, "attributes", a2);
    JS_SetPropertyStr(ctx, req, "phase", JS_NewInt32(ctx, 2));
    JS_FreezeObject(ctx, req);
    JS_SetPropertyUint32(ctx, arr, i, req);
  }
  return arr;
}

static JSValue mw_link(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
  ModuleWrap *w = unwrap(ctx, this_val);
  JSValue lenv;
  uint32_t n = 0, i;
  if (!w)
    return JS_EXCEPTION;
  if (!JS_IsArray(ARG(0)))
    return JS_ThrowTypeError(ctx, "link: an array of modules expected");
  lenv = JS_GetPropertyStr(ctx, ARG(0), "length");
  JS_ToUint32(ctx, &n, lenv);
  if (n != (uint32_t)JS_GetModuleRequestCount(w->m))
    return node_throw_error(ctx, "ERR_VM_MODULE_LINK_FAILURE",
                            "linked modules do not match the requests");
  for (i = 0; i < n; i++) {
    JSValue d = JS_GetPropertyUint32(ctx, ARG(0), i);
    ModuleWrap *dw = JS_GetOpaque(d, module_wrap_class_id);
    JS_FreeValue(ctx, d);
    if (!dw)
      return node_throw_error(ctx, "ERR_VM_MODULE_LINK_FAILURE",
                              "request %u is not linked to a module", i);
    JS_SetModuleRequestModule(w->m, i, dw->m);
  }
  JS_FreeValue(ctx, w->linked_requests);
  w->linked_requests = JS_DupValue(ctx, ARG(0));
  w->linked = true;
  return JS_UNDEFINED;
}

static JSValue rethrow_from(JSContext *ctx, JSContext *mctx) {
  if (mctx != ctx)
    JS_Throw(ctx, JS_GetException(mctx));
  return JS_EXCEPTION;
}

static JSValue mw_instantiate(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  ModuleWrap *w = unwrap(ctx, this_val);
  if (!w)
    return JS_EXCEPTION;
  if (!w->linked && JS_GetModuleRequestCount(w->m) > 0)
    return node_throw_error(ctx, "ERR_VM_MODULE_LINK_FAILURE", "module is not linked");
  if (JS_InstantiateModule(w->ctx, w->m) < 0)
    return rethrow_from(ctx, w->ctx);
  return JS_UNDEFINED;
}

static JSValue mw_evaluate(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  ModuleWrap *w = unwrap(ctx, this_val);
  JSValue p;
  if (!w)
    return JS_EXCEPTION;
  p = JS_EvaluateModule(w->ctx, w->m);
  if (JS_IsException(p))
    return rethrow_from(ctx, w->ctx);
  return p;
}

static JSValue throw_if_rejected(JSContext *ctx, JSValueConst p) {
  if (JS_IsPromise(p) && JS_PromiseState(ctx, p) == JS_PROMISE_REJECTED) {
    JSValue r = JS_PromiseResult(ctx, p);
    JS_PromiseMarkAsHandled(ctx, p);
    return JS_Throw(ctx, r);
  }
  return JS_UNDEFINED;
}

static JSValue mw_evaluate_sync(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  Env *env = env_get(ctx);
  ModuleWrap *w = unwrap(ctx, this_val);
  JSValue p, r;
  if (!w)
    return JS_EXCEPTION;
  p = JS_EvaluateModule(w->ctx, w->m);
  if (JS_IsException(p))
    return rethrow_from(ctx, w->ctx);
  /* a graph without top-level await settles in the microtasks it queued */
  if (JS_PromiseState(ctx, p) == JS_PROMISE_PENDING && !JS_ModuleIsGraphAsync(w->m))
    node_run_microtasks(env);
  r = throw_if_rejected(ctx, p);
  if (JS_IsException(r)) {
    JS_FreeValue(ctx, p);
    return r;
  }
  if (JS_PromiseState(ctx, p) != JS_PROMISE_FULFILLED) {
    JS_FreeValue(ctx, p);
    return node_throw_error(ctx, "ERR_REQUIRE_ASYNC_MODULE",
                            "require() cannot be used on an ESM graph with top-level await. "
                            "Use import() instead. To see where the top-level await comes from, "
                            "use --experimental-print-required-tla.");
  }
  JS_FreeValue(ctx, p);
  return JS_GetModuleNamespace(w->ctx, w->m);
}

static JSValue mw_get_namespace(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  ModuleWrap *w = unwrap(ctx, this_val);
  if (!w)
    return JS_EXCEPTION;
  if (JS_GetModuleStatus(w->m) < 2)
    return node_throw_error(ctx, "ERR_MODULE_NOT_INSTANTIATED",
                            "cannot get namespace, module has not been instantiated");
  return JS_GetModuleNamespace(w->ctx, w->m);
}

/* V8's Module::Status */
static JSValue mw_get_status(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  ModuleWrap *w = unwrap(ctx, this_val);
  int s, v;
  if (!w)
    return JS_EXCEPTION;
  s = JS_GetModuleStatus(w->m);
  switch (s) {
  case 0: v = 0; break;  /* kUninstantiated */
  case 1: v = 1; break;  /* kInstantiating */
  case 2: v = 2; break;  /* kInstantiated */
  case 3: v = 3; break;  /* kEvaluating */
  case 4:
  case 5: v = 4; break;  /* kEvaluated */
  default: v = 5; break; /* kErrored */
  }
  return JS_NewInt32(ctx, v);
}

static JSValue mw_get_error(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  ModuleWrap *w = unwrap(ctx, this_val);
  if (!w)
    return JS_EXCEPTION;
  return JS_GetModuleException(ctx, w->m);
}

static JSValue mw_set_export(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  ModuleWrap *w = unwrap(ctx, this_val);
  const char *name;
  if (!w)
    return JS_EXCEPTION;
  name = JS_ToCString(ctx, ARG(0));
  if (!name)
    return JS_EXCEPTION;
  JS_SetModuleExport(w->ctx, w->m, name, JS_DupValue(ctx, ARG(1)));
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

static JSValue mw_has_async_graph(JSContext *ctx, JSValueConst this_val) {
  ModuleWrap *w = JS_GetOpaque(this_val, module_wrap_class_id);
  if (!w)
    return JS_UNDEFINED;
  return JS_NewBool(ctx, JS_ModuleIsGraphAsync(w->m));
}

static JSValue mw_unsupported(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  return node_throw_error(ctx, "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM",
                          "This is not available on QuickJS");
}

static JSValue mw_create_cached_data(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  return nb_new_buffer(ctx, "", 0);
}

static const JSCFunctionListEntry mw_proto[] = {
  JS_CFUNC_DEF("link", 1, mw_link),
  JS_CFUNC_DEF("getModuleRequests", 0, mw_get_module_requests),
  JS_CFUNC_DEF("instantiate", 0, mw_instantiate),
  JS_CFUNC_DEF("evaluateSync", 0, mw_evaluate_sync),
  JS_CFUNC_DEF("evaluate", 2, mw_evaluate),
  JS_CFUNC_DEF("setExport", 2, mw_set_export),
  JS_CFUNC_DEF("setModuleSourceObject", 1, mw_unsupported),
  JS_CFUNC_DEF("getModuleSourceObject", 0, mw_unsupported),
  JS_CFUNC_DEF("createCachedData", 0, mw_create_cached_data),
  JS_CFUNC_DEF("getNamespace", 0, mw_get_namespace),
  JS_CFUNC_DEF("getStatus", 0, mw_get_status),
  JS_CFUNC_DEF("getError", 0, mw_get_error),
  JS_CGETSET_DEF("hasAsyncGraph", mw_has_async_graph, NULL),
};

static JSValue mw_set_import_module_dynamically_callback(JSContext *ctx, JSValueConst this_val,
                                                         int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->import_module_dynamically);
  env->import_module_dynamically = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue mw_set_initialize_import_meta_object_callback(JSContext *ctx,
                                                             JSValueConst this_val,
                                                             int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->initialize_import_meta);
  env->initialize_import_meta = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue mw_throw_if_promise_rejected(JSContext *ctx, JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  return throw_if_rejected(ctx, ARG(0));
}

/* the namespace of `export * from original; export { default } ...` */
static JSValue mw_create_required_module_facade(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  ModuleWrap *orig = unwrap(ctx, ARG(0));
  static const char src[] =
      "export * from 'original'; export { default } from 'original'; "
      "export const __esModule = true;";
  JSValue mv, p, ns;
  JSModuleDef *m;
  int i, n;
  int32_t prev;
  if (!orig)
    return JS_EXCEPTION;
  prev = JS_SetCompileHostDefinedId(env->rt, -1);
  mv = JS_Eval(ctx, src, sizeof(src) - 1, "node:internal/require_module_default_facade",
               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  JS_SetCompileHostDefinedId(env->rt, prev);
  if (JS_IsException(mv))
    return mv;
  m = JS_GetModuleDef(mv);
  JS_FreeValue(ctx, mv);
  n = JS_GetModuleRequestCount(m);
  for (i = 0; i < n; i++)
    JS_SetModuleRequestModule(m, i, orig->m);
  if (JS_InstantiateModule(ctx, m) < 0)
    return JS_EXCEPTION;
  p = JS_EvaluateModule(ctx, m);
  if (JS_IsException(p))
    return p;
  node_run_microtasks(env);
  JS_FreeValue(ctx, p);
  ns = JS_GetModuleNamespace(ctx, m);
  return ns;
}

JSValue binding_init_module_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  NodeClassDef def = { .name = "ModuleWrap", .class_id = &module_wrap_class_id,
                       .ctor = mw_ctor, .ctor_length = 6, .finalizer = mw_finalizer,
                       .gc_mark = mw_mark, .proto_funcs = mw_proto,
                       .proto_funcs_count = countof(mw_proto), .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  nb_set_method(ctx, t, "setImportModuleDynamicallyCallback",
                mw_set_import_module_dynamically_callback, 1);
  nb_set_method(ctx, t, "setInitializeImportMetaObjectCallback",
                mw_set_initialize_import_meta_object_callback, 1);
  nb_set_method(ctx, t, "createRequiredModuleFacade", mw_create_required_module_facade, 1);
  nb_set_method(ctx, t, "throwIfPromiseRejected", mw_throw_if_promise_rejected, 1);
  nb_set_int(ctx, t, "kUninstantiated", 0);
  nb_set_int(ctx, t, "kInstantiating", 1);
  nb_set_int(ctx, t, "kInstantiated", 2);
  nb_set_int(ctx, t, "kEvaluating", 3);
  nb_set_int(ctx, t, "kEvaluated", 4);
  nb_set_int(ctx, t, "kErrored", 5);
  nb_set_int(ctx, t, "kEvaluationPhase", 2);
  nb_set_int(ctx, t, "kSourcePhase", 1);
  return t;
}
