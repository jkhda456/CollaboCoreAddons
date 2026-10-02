/* The environment: bootstrapping Node's lib/, calling into JS from native code
 * (InternalMakeCallback), microtasks and process.nextTick, uncaught exceptions,
 * async hooks bookkeeping and the event loop.  Mirrors src/env.cc,
 * src/api/callback.cc, src/node_realm.cc and src/api/hooks.cc of Node. */
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "node.h"

const char *const node_provider_names[PROVIDERS_LENGTH] = {
#define V(p) #p,
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
};

static const char *const per_isolate_symbol_names[] = {
  "fs_use_promises_symbol", "async_id_symbol", "constructor_key_symbol",
  "handle_onclose_symbol", "no_message_symbol", "messaging_deserialize_symbol",
  "imported_cjs_symbol", "messaging_transfer_symbol", "messaging_clone_symbol",
  "messaging_transfer_list_symbol", "oninit_symbol", "owner_symbol",
  "onpskexchange_symbol", "resource_symbol", "trigger_async_id_symbol",
  "builtin_source_text_module_hdo", "source_text_module_default_hdo",
  "vm_context_no_contextify", "vm_dynamic_import_default_internal",
  "vm_dynamic_import_main_context_default", "vm_dynamic_import_missing_flag",
  "vm_dynamic_import_no_callback", NULL,
};
/* the descriptions V8 shows for them */
static const char *const per_isolate_symbol_descs[] = {
  "fs_use_promises_symbol", "async_id_symbol", "constructor_key_symbol",
  "handle_onclose", "no_message_symbol", "messaging_deserialize_symbol",
  "imported_cjs_symbol", "messaging_transfer_symbol", "messaging_clone_symbol",
  "messaging_transfer_list_symbol", "oninit", "owner_symbol", "onpskexchange",
  "resource_symbol", "trigger_async_id_symbol", "builtin_source_text_module_hdo",
  "source_text_module_default_hdo", "vm_context_no_contextify",
  "vm_dynamic_import_default_internal", "vm_dynamic_import_main_context_default",
  "vm_dynamic_import_missing_flag", "vm_dynamic_import_no_callback", NULL,
};

static const char *const private_symbol_names[] = {
  "arrow_message_private_symbol", "contextify_context_private_symbol",
  "decorated_private_symbol", "empty_context_frame_sentinel_symbol",
  "transfer_mode_private_symbol", "host_defined_option_symbol",
  "js_transferable_wrapper_private_symbol", "entry_point_module_private_symbol",
  "entry_point_promise_private_symbol", "module_source_private_symbol",
  "module_export_names_private_symbol", "module_circular_visited_private_symbol",
  "module_export_private_symbol", "module_first_parent_private_symbol",
  "module_last_parent_private_symbol", "napi_type_tag", "napi_wrapper",
  "untransferable_object_private_symbol", "exit_info_private_symbol",
  "promise_trace_id", "source_map_data_private_symbol", NULL,
};

static JSValue undefined_or_dup(JSContext *ctx, JSValueConst v) {
  return JS_DupValue(ctx, v);
}

JSValue env_get_symbol(Env *env, const char *name) {
  return JS_GetPropertyStr(env->ctx, env->per_isolate_symbols, name);
}

typedef struct EnvScratch {
  struct EnvScratch *next;
  const char *name;
  size_t size;
  max_align_t data[];
} EnvScratch;

void *env_scratch(Env *env, const char *name, size_t size) {
  EnvScratch *s;
  for (s = env->scratch; s; s = s->next)
    if (!strcmp(s->name, name))
      return s->data;
  s = calloc(1, sizeof(*s) + size);
  s->name = name;
  s->size = size;
  s->next = env->scratch;
  env->scratch = s;
  return s->data;
}

static void env_scratch_free(Env *env) {
  EnvScratch *s = env->scratch, *n;
  for (; s; s = n) {
    n = s->next;
    free(s);
  }
  env->scratch = NULL;
}

JSValue env_binding_data(Env *env, const char *name) {
  JSValue v = JS_GetPropertyStr(env->ctx, env->binding_data, name);
  if (JS_IsUndefined(v)) {
    v = JS_NewObjectProto(env->ctx, JS_NULL);
    JS_SetPropertyStr(env->ctx, env->binding_data, name, JS_DupValue(env->ctx, v));
  }
  return v;
}

JSValue env_get_private_symbol(Env *env, const char *name) {
  return JS_GetPropertyStr(env->ctx, env->private_symbols, name);
}

/* ---------------------------------------------------------------------- */
/* host hooks QuickJS calls */

static JSValue hook_dynamic_import(JSContext *ctx, JSValueConst specifier,
                                   JSValueConst attributes, int32_t id,
                                   JSValueConst referrer_name, void *opaque) {
  Env *env = env_get(ctx);
  JSValue symbol, attrs, args[5], ret;
  if (!JS_IsFunction(ctx, env->import_module_dynamically))
    return JS_ThrowTypeError(ctx, "Dynamic import is not supported here");
  symbol = JS_GetPropertyUint32(ctx, env->host_defined_symbols, (uint32_t)id);
  attrs = JS_IsUndefined(attributes) ? JS_NewObjectProto(ctx, JS_NULL)
                                     : JS_DupValue(ctx, attributes);
  args[0] = symbol;
  args[1] = (JSValue)specifier;
  args[2] = JS_NewInt32(ctx, 2); /* kEvaluationPhase */
  args[3] = attrs;
  args[4] = (JSValue)referrer_name;
  ret = JS_Call(ctx, env->import_module_dynamically, JS_UNDEFINED, 5,
                (JSValueConst *)args);
  JS_FreeValue(ctx, symbol);
  JS_FreeValue(ctx, attrs);
  return ret;
}

JSValue module_wrap_from_module(Env *env, JSModuleDef *m); /* module_wrap.c */

static int hook_init_import_meta(JSContext *ctx, JSModuleDef *m, JSValueConst meta,
                                 void *opaque) {
  Env *env = env_get(ctx);
  JSValue wrap, symbol, args[3], ret;
  int32_t id;
  if (!JS_IsFunction(ctx, env->initialize_import_meta))
    return 0;
  wrap = module_wrap_from_module(env, m);
  if (JS_IsUndefined(wrap))
    return 0;
  id = JS_GetModuleHostDefinedId(m);
  symbol = JS_GetPropertyUint32(ctx, env->host_defined_symbols, (uint32_t)id);
  args[0] = symbol;
  args[1] = (JSValue)meta;
  args[2] = wrap;
  ret = JS_Call(ctx, env->initialize_import_meta, JS_UNDEFINED, 3,
                (JSValueConst *)args);
  JS_FreeValue(ctx, symbol);
  JS_FreeValue(ctx, wrap);
  if (JS_IsException(ret))
    return -1;
  JS_FreeValue(ctx, ret);
  return 0;
}

static void promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                      JSValueConst reason, bool is_handled,
                                      void *opaque) {
  Env *env = env_get(ctx);
  JSValue args[3], ret;
  /* kPromiseRejectWithNoHandler = 0, kPromiseHandlerAddedAfterReject = 1 */
  if (!env || !JS_IsFunction(ctx, env->promise_reject_callback))
    return;
  args[0] = JS_NewInt32(ctx, is_handled ? 1 : 0);
  args[1] = (JSValue)promise;
  args[2] = is_handled ? JS_UNDEFINED : (JSValue)reason;
  ret = JS_Call(ctx, env->promise_reject_callback, JS_UNDEFINED, 3,
                (JSValueConst *)args);
  if (JS_IsException(ret)) {
    JSValue err = JS_GetException(ctx);
    node_trigger_uncaught_exception(env, err, false);
  } else {
    JS_FreeValue(ctx, ret);
  }
}

/* ---------------------------------------------------------------------- */

static void make_symbols(Env *env) {
  JSContext *ctx = env->ctx;
  int i;
  env->private_symbols = JS_NewObjectProto(ctx, JS_NULL);
  for (i = 0; private_symbol_names[i]; i++) {
    char desc[96];
    /* node:arrowMessage and the like; the description is informational */
    snprintf(desc, sizeof(desc), "node:%s", private_symbol_names[i]);
    JS_SetPropertyStr(ctx, env->private_symbols, private_symbol_names[i],
                      JS_NewPrivateSymbol(ctx, desc));
  }
  env->per_isolate_symbols = JS_NewObjectProto(ctx, JS_NULL);
  for (i = 0; per_isolate_symbol_names[i]; i++)
    JS_SetPropertyStr(ctx, env->per_isolate_symbols, per_isolate_symbol_names[i],
                      JS_NewSymbol(ctx, per_isolate_symbol_descs[i], false));
  /* host defined option ids -> their symbols */
  env->host_defined_symbols = JS_NewArray(ctx);
  {
    static const struct { int id; const char *name; } hdo[] = {
      { HDO_VM_DYNAMIC_IMPORT_DEFAULT_INTERNAL, "vm_dynamic_import_default_internal" },
      { HDO_SOURCE_TEXT_MODULE_DEFAULT, "source_text_module_default_hdo" },
      { HDO_VM_DYNAMIC_IMPORT_MAIN_CONTEXT_DEFAULT, "vm_dynamic_import_main_context_default" },
      { HDO_VM_DYNAMIC_IMPORT_MISSING_FLAG, "vm_dynamic_import_missing_flag" },
      { HDO_VM_DYNAMIC_IMPORT_NO_CALLBACK, "vm_dynamic_import_no_callback" },
      { HDO_BUILTIN_SOURCE_TEXT_MODULE, "builtin_source_text_module_hdo" },
    };
    for (i = 0; i < (int)countof(hdo); i++)
      JS_SetPropertyUint32(ctx, env->host_defined_symbols, hdo[i].id,
                           env_get_symbol(env, hdo[i].name));
  }
  env->next_host_defined_id = HDO_FIRST_DYNAMIC;
}

/* ---------------------------------------------------------------------- */
/* runtimes: the main thread's and each worker's */

/* SharedArrayBuffer memory, which worker threads share: reference counted,
   so it outlives the runtime that made it while another one holds it */
typedef struct SabBlock {
  atomic_int refs;
  max_align_t data[];
} SabBlock;

static SabBlock *sab_block(void *ptr) {
  return (SabBlock *)((char *)ptr - offsetof(SabBlock, data));
}

static void *sab_alloc(void *opaque, size_t size) {
  SabBlock *b = calloc(1, sizeof(SabBlock) + (size ? size : 1));
  if (!b)
    return NULL;
  atomic_init(&b->refs, 1);
  return b->data;
}

void node_sab_free(void *opaque, void *ptr) {
  SabBlock *b = sab_block(ptr);
  if (atomic_fetch_sub(&b->refs, 1) == 1)
    free(b);
}

void node_sab_dup(void *opaque, void *ptr) {
  atomic_fetch_add(&sab_block(ptr)->refs, 1);
}

JSRuntime *node_new_runtime(size_t stack_size) {
  return node_new_runtime2(stack_size, NULL, NULL);
}

JSRuntime *node_new_runtime2(size_t stack_size, const JSMallocFunctions *mf, void *opaque) {
  static const JSSharedArrayBufferFunctions sab = {
    .sab_alloc = sab_alloc, .sab_free = node_sab_free, .sab_dup = node_sab_dup,
  };
  JSRuntime *rt = mf ? JS_NewRuntime2(mf, opaque) : JS_NewRuntime();
  if (!rt)
    return NULL;
  JS_SetMaxStackSize(rt, stack_size);
  JS_SetCanBlock(rt, true);
  JS_SetSharedArrayBufferFunctions(rt, &sab);
  return rt;
}

Env *env_new(JSRuntime *rt, JSContext *ctx, uv_loop_t *loop, bool is_main) {
  Env *env = calloc(1, sizeof(Env));
  JSValue *vals[] = {
    &env->process, &env->primordials, &env->per_context_exports,
    &env->private_symbols, &env->per_isolate_symbols,
    &env->internal_binding_loader, &env->builtin_require, &env->binding_cache,
    &env->tick_callback, &env->promise_reject_callback,
    &env->prepare_stack_trace_cb, &env->enhance_stack_before_inspector,
    &env->enhance_stack_after_inspector, &env->source_maps_enabled,
    &env->get_source_map_error_source, &env->maybe_cache_generated_source_map,
    &env->async_hooks_binding, &env->async_hooks_init, &env->async_hooks_before,
    &env->async_hooks_after, &env->async_hooks_destroy,
    &env->async_hooks_promise_resolve, &env->async_callback_trampoline,
    &env->timers_callback, &env->immediate_callback,
    &env->import_module_dynamically, &env->initialize_import_meta,
    &env->host_defined_symbols, &env->module_wrap_class_proto,
    &env->emit_warning_sync, &env->buffer_prototype,
    &env->messaging_deserialize_create_object, &env->process_emit_warning,
    &env->execution_async_resources, &env->global, &env->binding_data,
    &env->promise_hook_fns[0], &env->promise_hook_fns[1], &env->promise_hook_fns[2],
    &env->promise_hook_fns[3],
  };
  size_t i;
  JSNodeHooks hooks;
  for (i = 0; i < countof(vals); i++)
    *vals[i] = JS_UNDEFINED;
  env->rt = rt;
  env->ctx = ctx;
  env->loop = loop;
  env->is_main_thread = is_main;
  env->owns_process_state = is_main;
  env->can_call_into_js = true;
  JS_SetContextOpaque(ctx, env);
  env->global = JS_GetGlobalObject(ctx);
  env->binding_cache = JS_NewObjectProto(ctx, JS_NULL);
  env->binding_data = JS_NewObjectProto(ctx, JS_NULL);
  env->source_maps_enabled = JS_FALSE;
  make_symbols(env);

  memset(&hooks, 0, sizeof(hooks));
  hooks.prepare_stack_trace = node_prepare_stack_trace;
  hooks.dynamic_import = hook_dynamic_import;
  hooks.init_import_meta = hook_init_import_meta;
  JS_SetNodeHooks(rt, &hooks, env);
  JS_SetHostPromiseRejectionTracker(rt, promise_rejection_tracker, env);

  /* shared arrays */
  env->tick_info = calloc(kTickFieldsCount, sizeof(int32_t));
  env->immediate_info = calloc(kImmFieldsCount, sizeof(uint32_t));
  env->timeout_info = calloc(1, sizeof(int32_t));
  env->async_hook_fields = calloc(kAsyncHookFieldsCount, sizeof(uint32_t));
  env->async_id_fields = calloc(kUidFieldsCount, sizeof(double));
  env->async_ids_stack_len = 16 * 2;
  env->async_ids_stack = calloc(env->async_ids_stack_len, sizeof(double));
  env->exit_info = calloc(kExitInfoFieldsCount, sizeof(int32_t));
  env->stream_base_state = calloc(kNumStreamBaseStateFields, sizeof(int32_t));
  env->hrtime_buffer = calloc(4, sizeof(uint32_t));
  env->zero_fill_field = calloc(1, 4);
  env->zero_fill_field[0] = 1;
  /* Node starts the counter at 1: the bootstrap's own id */
  env->async_id_fields[kAsyncIdCounter] = 1;
  env->async_id_fields[kDefaultTriggerAsyncId] = -1;
  env->async_hook_fields[kCheck] = 1;
  env->execution_async_resources = JS_NewArray(ctx);
  env->start_time_ns = uv_hrtime();
  node_timers_init(env);
  return env;
}

static void free_val(JSContext *ctx, JSValue *v) {
  JS_FreeValue(ctx, *v);
  *v = JS_UNDEFINED;
}

void env_free(Env *env) {
  JSContext *ctx = env->ctx;
  uint32_t i;
  JSValue *vals[] = {
    &env->process, &env->primordials, &env->per_context_exports,
    &env->private_symbols, &env->per_isolate_symbols,
    &env->internal_binding_loader, &env->builtin_require, &env->binding_cache,
    &env->tick_callback, &env->promise_reject_callback,
    &env->prepare_stack_trace_cb, &env->enhance_stack_before_inspector,
    &env->enhance_stack_after_inspector, &env->source_maps_enabled,
    &env->get_source_map_error_source, &env->maybe_cache_generated_source_map,
    &env->async_hooks_binding, &env->async_hooks_init, &env->async_hooks_before,
    &env->async_hooks_after, &env->async_hooks_destroy,
    &env->async_hooks_promise_resolve, &env->async_callback_trampoline,
    &env->timers_callback, &env->immediate_callback,
    &env->import_module_dynamically, &env->initialize_import_meta,
    &env->host_defined_symbols, &env->module_wrap_class_proto,
    &env->emit_warning_sync, &env->buffer_prototype,
    &env->messaging_deserialize_create_object, &env->process_emit_warning,
    &env->execution_async_resources, &env->global, &env->binding_data,
    &env->promise_hook_fns[0], &env->promise_hook_fns[1], &env->promise_hook_fns[2],
    &env->promise_hook_fns[3],
  };
  for (i = 0; i < countof(vals); i++)
    free_val(ctx, vals[i]);
  for (i = 0; i < env->native_resources_len; i++)
    JS_FreeValue(ctx, env->native_resources[i]);
  env_free_native(env);
}

/* what env holds outside the JS heap (a worker's heap goes all at once) */
void env_free_native(Env *env) {
  free(env->native_resources);
  free(env->tick_info);
  free(env->immediate_info);
  free(env->timeout_info);
  free(env->async_hook_fields);
  free(env->async_id_fields);
  free(env->async_ids_stack);
  free(env->exit_info);
  free(env->stream_base_state);
  free(env->hrtime_buffer);
  free(env->zero_fill_field);
  free(env->destroy_ids);
  free(env->fsd);
  free(env->handle_list);
  free(env->exec_path);
  node_env_store_unref(env->env_store);
  env_scratch_free(env);
  free(env);
}

/* ---------------------------------------------------------------------- */
/* async hooks bookkeeping */

double env_get_default_trigger_async_id(Env *env) {
  double id = env->async_id_fields[kDefaultTriggerAsyncId];
  if (id < 0)
    id = env->async_id_fields[kExecutionAsyncId];
  return id;
}

static void grow_async_ids_stack(Env *env) {
  JSContext *ctx = env->ctx;
  uint32_t n = env->async_ids_stack_len * 3;
  double *a = calloc(n, sizeof(double));
  JSValue arr;
  memcpy(a, env->async_ids_stack, env->async_ids_stack_len * sizeof(double));
  /* JS re-reads async_wrap.async_ids_stack, so the binding gets a new view;
     the old memory stays valid for views that may linger */
  env->async_ids_stack = a;
  env->async_ids_stack_len = n;
  if (!JS_IsUndefined(env->async_hooks_binding)) {
    arr = nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, a, n, sizeof(double));
    JS_SetPropertyStr(ctx, env->async_hooks_binding, "async_ids_stack", arr);
  }
}

void env_push_async_context(Env *env, double async_id, double trigger_async_id,
                            JSValueConst resource) {
  uint32_t offset = env->async_hook_fields[kStackLength];
  if (offset * 2 >= env->async_ids_stack_len)
    grow_async_ids_stack(env);
  env->async_ids_stack[2 * offset] = env->async_id_fields[kExecutionAsyncId];
  env->async_ids_stack[2 * offset + 1] = env->async_id_fields[kTriggerAsyncId];
  env->async_hook_fields[kStackLength] += 1;
  env->async_id_fields[kExecutionAsyncId] = async_id;
  env->async_id_fields[kTriggerAsyncId] = trigger_async_id;
  if (!JS_IsUndefined(resource)) {
    if (offset + 1 > env->native_resources_cap) {
      uint32_t cap = (offset + 1) * 2;
      env->native_resources = realloc(env->native_resources, cap * sizeof(JSValue));
      env->native_resources_cap = cap;
    }
    while (env->native_resources_len < offset)
      env->native_resources[env->native_resources_len++] = JS_UNDEFINED;
    if (env->native_resources_len > offset) {
      uint32_t i;
      for (i = offset; i < env->native_resources_len; i++)
        JS_FreeValue(env->ctx, env->native_resources[i]);
      env->native_resources_len = offset;
    }
    env->native_resources[env->native_resources_len++] = JS_DupValue(env->ctx, resource);
  }
}

void env_clear_async_id_stack(Env *env);

bool env_pop_async_context(Env *env, double async_id) {
  uint32_t offset;
  JSContext *ctx = env->ctx;
  if (env->async_hook_fields[kStackLength] == 0)
    return false;
  if (env->stopping) {
    /* a terminated script skipped the pops of its own frames
       (InternalCallbackScope::Close: clear the stack when stopping) */
    env_clear_async_id_stack(env);
    return false;
  }
  if (env->async_hook_fields[kCheck] > 0 &&
      env->async_id_fields[kExecutionAsyncId] != async_id) {
    fprintf(stderr,
            "Error: async hook stack has become corrupted (actual: %.f, expected: %.f)\n",
            env->async_id_fields[kExecutionAsyncId], async_id);
    fflush(stderr);
    exit(1);
  }
  offset = env->async_hook_fields[kStackLength] - 1;
  env->async_id_fields[kExecutionAsyncId] = env->async_ids_stack[2 * offset];
  env->async_id_fields[kTriggerAsyncId] = env->async_ids_stack[2 * offset + 1];
  env->async_hook_fields[kStackLength] = offset;
  while (env->native_resources_len > offset) {
    env->native_resources_len--;
    JS_FreeValue(ctx, env->native_resources[env->native_resources_len]);
  }
  {
    JSValue len_v = JS_GetPropertyStr(ctx, env->execution_async_resources, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_v);
    JS_FreeValue(ctx, len_v);
    if (len > offset)
      JS_SetPropertyStr(ctx, env->execution_async_resources, "length",
                        JS_NewUint32(ctx, offset));
  }
  return env->async_hook_fields[kStackLength] > 0;
}

void env_clear_async_id_stack(Env *env) {
  JSContext *ctx = env->ctx;
  if (env->can_call_into_js && JS_IsObject(env->execution_async_resources))
    JS_SetPropertyStr(ctx, env->execution_async_resources, "length", JS_NewUint32(ctx, 0));
  while (env->native_resources_len > 0) {
    env->native_resources_len--;
    JS_FreeValue(ctx, env->native_resources[env->native_resources_len]);
  }
  env->async_id_fields[kExecutionAsyncId] = 0;
  env->async_id_fields[kTriggerAsyncId] = 0;
  env->async_hook_fields[kStackLength] = 0;
}

JSValue env_native_execution_async_resource(Env *env, uint32_t index) {
  if (index < env->native_resources_len)
    return JS_DupValue(env->ctx, env->native_resources[index]);
  return JS_UNDEFINED;
}

static void emit_hook(Env *env, JSValueConst fn, double async_id) {
  JSContext *ctx = env->ctx;
  JSValue arg, ret;
  if (!JS_IsFunction(ctx, fn))
    return;
  arg = JS_NewFloat64(ctx, async_id);
  ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&arg);
  if (JS_IsException(ret)) {
    JSValue err = JS_GetException(ctx);
    node_trigger_uncaught_exception(env, err, false);
    return;
  }
  JS_FreeValue(ctx, ret);
}

static void destroy_ids_cb(uv_idle_t *h) {
  Env *env = h->data;
  JSContext *ctx = env->ctx;
  uv_idle_stop(h);
  env->destroy_ids_scheduled = false;
  while (env->destroy_ids_len > 0) {
    uint32_t n = env->destroy_ids_len, i;
    double *ids = env->destroy_ids;
    env->destroy_ids = NULL;
    env->destroy_ids_len = env->destroy_ids_cap = 0;
    for (i = 0; i < n; i++) {
      if (env->async_hook_fields[kDestroy] > 0 && env->can_call_into_js) {
        JSValue arg = JS_NewFloat64(ctx, ids[i]), ret;
        ret = JS_Call(ctx, env->async_hooks_destroy, JS_UNDEFINED, 1, (JSValueConst *)&arg);
        if (JS_IsException(ret))
          node_trigger_uncaught_exception(env, JS_GetException(ctx), false);
        else
          JS_FreeValue(ctx, ret);
      }
    }
    free(ids);
  }
}

void env_queue_destroy_async_id(Env *env, double async_id) {
  if (env->async_hook_fields[kDestroy] == 0 || !env->can_call_into_js)
    return;
  if (env->destroy_ids_len == env->destroy_ids_cap) {
    env->destroy_ids_cap = env->destroy_ids_cap ? env->destroy_ids_cap * 2 : 16;
    env->destroy_ids = realloc(env->destroy_ids, env->destroy_ids_cap * sizeof(double));
  }
  env->destroy_ids[env->destroy_ids_len++] = async_id;
  if (!env->destroy_ids_scheduled) {
    env->destroy_ids_scheduled = true;
    if (!env->destroy_idle.loop) {
      uv_idle_init(env->loop, &env->destroy_idle);
      env->destroy_idle.data = env;
      uv_unref((uv_handle_t *)&env->destroy_idle);
    }
    uv_idle_start(&env->destroy_idle, destroy_ids_cb);
  }
}

void async_wrap_init(AsyncWrap *w, Env *env, JSValueConst obj, ProviderType provider,
                     double trigger_async_id) {
  JSContext *ctx = env->ctx;
  w->env = env;
  w->object = (JSValue)obj;
  w->provider = provider;
  w->strong = 0;
  w->context_frame = JS_GetContinuationPreservedData(ctx);
  w->async_id = ++env->async_id_fields[kAsyncIdCounter];
  w->trigger_async_id = trigger_async_id >= 0 ? trigger_async_id
                                              : env_get_default_trigger_async_id(env);
  if (env->async_hook_fields[kInit] > 0 && JS_IsFunction(ctx, env->async_hooks_init)) {
    JSValue args[4], ret;
    args[0] = JS_NewFloat64(ctx, w->async_id);
    args[1] = JS_NewString(ctx, node_provider_names[provider]);
    args[2] = JS_NewFloat64(ctx, w->trigger_async_id);
    args[3] = (JSValue)obj;
    ret = JS_Call(ctx, env->async_hooks_init, obj, 4, (JSValueConst *)args);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(ret))
      node_trigger_uncaught_exception(env, JS_GetException(ctx), false);
    else
      JS_FreeValue(ctx, ret);
  }
}

/* AsyncWrap::AsyncReset: a fresh async id and an init hook for resource */
void async_wrap_reset(AsyncWrap *w, JSValueConst resource) {
  Env *env = w->env;
  JSContext *ctx = env->ctx;
  if (w->async_id > 0)
    env_queue_destroy_async_id(env, w->async_id);
  JS_FreeValue(ctx, w->context_frame);
  w->context_frame = JS_GetContinuationPreservedData(ctx);
  w->async_id = ++env->async_id_fields[kAsyncIdCounter];
  w->trigger_async_id = env_get_default_trigger_async_id(env);
  if (env->async_hook_fields[kInit] > 0 && JS_IsFunction(ctx, env->async_hooks_init)) {
    JSValue args[4], ret;
    JSValueConst res = JS_IsObject(resource) ? resource : (JSValueConst)w->object;
    args[0] = JS_NewFloat64(ctx, w->async_id);
    args[1] = JS_NewString(ctx, node_provider_names[w->provider]);
    args[2] = JS_NewFloat64(ctx, w->trigger_async_id);
    args[3] = (JSValue)res;
    ret = JS_Call(ctx, env->async_hooks_init, res, 4, (JSValueConst *)args);
    JS_FreeValue(ctx, args[1]);
    if (JS_IsException(ret))
      node_trigger_uncaught_exception(env, JS_GetException(ctx), false);
    else
      JS_FreeValue(ctx, ret);
  }
}

void async_wrap_destroy(AsyncWrap *w) {
  if (!w->env)
    return;
  if (w->async_id > 0)
    env_queue_destroy_async_id(w->env, w->async_id);
  JS_FreeValueRT(w->env->rt, w->context_frame);
  w->context_frame = JS_UNDEFINED;
  w->async_id = -1;
}

void async_wrap_ref(AsyncWrap *w) {
  if (w->strong++ == 0)
    JS_DupValue(w->env->ctx, w->object);
}

void async_wrap_unref(AsyncWrap *w) {
  if (w->strong > 0 && --w->strong == 0)
    JS_FreeValue(w->env->ctx, w->object);
}

/* ---------------------------------------------------------------------- */
/* microtasks, ticks and InternalCallbackScope */

void node_run_microtasks(Env *env) {
  JSContext *ctx1;
  int ret;
  for (;;) {
    ret = JS_ExecutePendingJob(env->rt, &ctx1);
    if (ret == 0)
      break;
    if (ret < 0) {
      JSValue err = JS_GetException(ctx1);
      if (JS_IsUninitialized(err))
        continue;
      node_trigger_uncaught_exception(env, err, false);
      if (env->stopping)
        break;
    }
  }
  /* the end of the checkpoint: WeakRef targets need not be kept */
  JS_ClearKeptObjects(env->rt);
}

void env_internal_callback_scope_enter(Env *env) {
  env->callback_scope_depth++;
}

/* InternalCallbackScope::Close() after the async context was popped;
   returns -1 if the tick callback threw (reported already) */
int env_internal_callback_scope_exit(Env *env, bool failed) {
  JSContext *ctx = env->ctx;
  int ret = 0;
  if (env->stopping) {
    env_clear_async_id_stack(env);
    goto out;
  }
  if (failed)
    goto out;
  if (env->callback_scope_depth > 1)
    goto out;
  if (!env->can_call_into_js)
    goto out;
  if (!env->tick_info[kHasTickScheduled]) {
    node_run_microtasks(env);
    if (env->stopping) {
      env_clear_async_id_stack(env);
      goto out;
    }
  }
  if (!env->tick_info[kHasTickScheduled] && !env->tick_info[kHasRejectionToWarn])
    goto out;
  if (!JS_IsFunction(ctx, env->tick_callback))
    goto out;
  {
    JSValue r = JS_Call(ctx, env->tick_callback, env->process, 0, NULL);
    if (JS_IsException(r)) {
      JSValue err = JS_GetException(ctx);
      ret = -1;
      /* the tick callback is called at the outermost scope: report it */
      env->callback_scope_depth--;
      node_trigger_uncaught_exception(env, err, false);
      env->callback_scope_depth++;
    } else {
      JS_FreeValue(ctx, r);
    }
  }
  if (env->stopping)
    env_clear_async_id_stack(env);
out:
  env->callback_scope_depth--;
  return ret;
}

JSValue node_make_callback(Env *env, JSValueConst resource, JSValueConst recv,
                           JSValueConst fn, int argc, JSValueConst *argv,
                           double async_id, double trigger_async_id,
                           JSValueConst context_frame) {
  JSContext *ctx = env->ctx;
  JSValue ret, prior_frame;
  bool use_trampoline = false, skip_hooks = false, failed = false;
  bool outermost;
  uint32_t depth_before;

  env->callback_scope_depth++;
  outermost = env->callback_scope_depth == 1;
  if (!env->can_call_into_js) {
    env->callback_scope_depth--;
    return JS_UNDEFINED;
  }
  if (JS_IsFunction(ctx, env->async_callback_trampoline)) {
    skip_hooks = true;
    use_trampoline = env->async_hook_fields[kBefore] + env->async_hook_fields[kAfter] +
                     env->async_hook_fields[kUsesExecutionAsyncResource] > 0;
  }
  prior_frame = JS_GetContinuationPreservedData(ctx);
  JS_SetContinuationPreservedData(ctx, JS_DupValue(ctx, context_frame));
  depth_before = env->async_hook_fields[kStackLength];
  env_push_async_context(env, async_id, trigger_async_id, resource);
  if (async_id != 0 && !skip_hooks && env->async_hook_fields[kBefore] > 0)
    emit_hook(env, env->async_hooks_before, async_id);

  if (use_trampoline) {
    JSValue *args = alloca((3 + argc) * sizeof(JSValue));
    int i;
    args[0] = JS_NewFloat64(ctx, async_id);
    args[1] = (JSValue)resource;
    args[2] = (JSValue)fn;
    for (i = 0; i < argc; i++)
      args[i + 3] = (JSValue)argv[i];
    ret = JS_Call(ctx, env->async_callback_trampoline, recv, 3 + argc,
                  (JSValueConst *)args);
  } else {
    ret = JS_Call(ctx, fn, recv, argc, argv);
  }
  if (JS_IsException(ret))
    failed = true;

  if (!failed && async_id != 0 && !skip_hooks && env->async_hook_fields[kAfter] > 0)
    emit_hook(env, env->async_hooks_after, async_id);

  if (failed) {
    /* the JS frames that threw left their contexts pushed (emitAfter is not
       in a finally); as in Node the exception is reported inside the scope,
       where a handled one clears the stack, then the scope's frames go */
    if (outermost) {
      JSValue err = JS_GetException(ctx);
      env->callback_scope_depth--;
      node_trigger_uncaught_exception(env, err, false);
      env->callback_scope_depth++;
    }
    while (env->async_hook_fields[kStackLength] > depth_before) {
      uint32_t offset = env->async_hook_fields[kStackLength] - 1;
      env->async_id_fields[kExecutionAsyncId] = env->async_ids_stack[2 * offset];
      env->async_id_fields[kTriggerAsyncId] = env->async_ids_stack[2 * offset + 1];
      env->async_hook_fields[kStackLength] = offset;
    }
    while (env->native_resources_len > env->async_hook_fields[kStackLength]) {
      env->native_resources_len--;
      JS_FreeValue(ctx, env->native_resources[env->native_resources_len]);
    }
    JS_SetContinuationPreservedData(ctx, prior_frame);
    if (outermost) {
      env_internal_callback_scope_exit(env, false);
      return NODE_CB_FAILED;
    }
  } else {
    env_pop_async_context(env, async_id);
    JS_SetContinuationPreservedData(ctx, prior_frame);
  }
  if (env_internal_callback_scope_exit(env, failed) < 0 && !failed) {
    JS_FreeValue(ctx, ret);
    return NODE_CB_FAILED;
  }
  return ret;
}

JSValue async_wrap_make_callback(AsyncWrap *w, JSValueConst fn, int argc,
                                 JSValueConst *argv) {
  return node_make_callback(w->env, w->object, w->object, fn, argc, argv,
                            w->async_id, w->trigger_async_id, w->context_frame);
}

JSValue async_wrap_make_callback_name(AsyncWrap *w, const char *name, int argc,
                                      JSValueConst *argv) {
  JSContext *ctx = w->env->ctx;
  JSValue fn = JS_GetPropertyStr(ctx, w->object, name), ret;
  if (JS_IsException(fn)) {
    node_trigger_uncaught_exception(w->env, JS_GetException(ctx), false);
    return JS_UNDEFINED;
  }
  if (!JS_IsFunction(ctx, fn)) {
    JS_FreeValue(ctx, fn);
    return JS_UNDEFINED;
  }
  ret = async_wrap_make_callback(w, fn, argc, argv);
  JS_FreeValue(ctx, fn);
  return ret;
}

/* ---------------------------------------------------------------------- */
/* uncaught exceptions (node_errors.cc TriggerUncaughtException) */

void node_trigger_uncaught_exception(Env *env, JSValue err, bool from_promise) {
  JSContext *ctx = env->ctx;
  JSValue fatal, args[2], handled;
  int *depth = &env->fatal_depth;

  if (JS_IsUninitialized(err))
    return;
  if (!env->can_call_into_js || env->stopping) {
    JS_FreeValue(ctx, err);
    return;
  }
  if (*depth > 0) {
    /* an exception while handling one: print and leave (kExceptionInFatalExceptionHandler) */
    node_print_uncaught(env, err, from_promise);
    JS_FreeValue(ctx, err);
    fflush(stdout);
    _exit(7);
  }
  fatal = JS_GetPropertyStr(ctx, env->process, "_fatalException");
  if (!JS_IsFunction(ctx, fatal)) {
    JS_FreeValue(ctx, fatal);
    node_print_uncaught(env, err, from_promise);
    JS_FreeValue(ctx, err);
    fflush(stdout);
    exit(6);
  }
  (*depth)++;
  args[0] = err;
  args[1] = JS_NewBool(ctx, from_promise);
  handled = JS_Call(ctx, fatal, env->process, 2, (JSValueConst *)args);
  (*depth)--;
  JS_FreeValue(ctx, fatal);
  if (!env->is_main_thread && env->stopping) {
    /* a worker's handler reported it to the parent and ended the thread
       (process.exit), or the worker is being terminated */
    if (JS_IsException(handled))
      JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, handled);
    JS_FreeValue(ctx, err);
    return;
  }
  if (JS_IsException(handled)) {
    /* the handler threw: report that one */
    JSValue err2 = JS_GetException(ctx);
    node_print_uncaught(env, err2, false);
    JS_FreeValue(ctx, err2);
    JS_FreeValue(ctx, err);
    fflush(stdout);
    _exit(7);
  }
  if (JS_ToBool(ctx, handled)) {
    JS_FreeValue(ctx, handled);
    JS_FreeValue(ctx, err);
    return;
  }
  JS_FreeValue(ctx, handled);
  /* not handled: print it, run 'exit' handlers and leave */
  node_print_uncaught(env, err, from_promise);
  JS_FreeValue(ctx, err);
  {
    /* the code the handler (or an 'exit' listener) set, else 1 */
    int code = env->exit_info[kHasExitCode] ? env->exit_info[kExitCode] : 1;
    /* process._fatalException already emitted 'exit' through process.reallyExit
       when it decided so; otherwise exit here */
    fflush(stdout);
    fflush(stderr);
    if (!env->is_main_thread) {
      env->exit_info[kHasExitCode] = 1;
      env->exit_info[kExitCode] = code;
      env_stop(env);
      return;
    }
    env->stopping = true;
    exit(code);
  }
}

/* ---------------------------------------------------------------------- */
/* vm contexts: more contexts in the same runtime, sharing the Env */

/* --expose-gc: globalThis.gc([options]) */
static JSValue js_gc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JS_RunGC(JS_GetRuntime(ctx));
  if (argc > 0 && JS_IsObject(argv[0])) {
    JSValue ex = JS_GetPropertyStr(ctx, argv[0], "execution");
    const char *s = JS_IsString(ex) ? JS_ToCString(ctx, ex) : NULL;
    bool async = s && !strcmp(s, "async");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, ex);
    if (async) {
      JSValue res[2], p = JS_NewPromiseCapability(ctx, res), u = JS_UNDEFINED;
      JS_FreeValue(ctx, JS_Call(ctx, res[0], JS_UNDEFINED, 1, (JSValueConst *)&u));
      JS_FreeValue(ctx, res[0]);
      JS_FreeValue(ctx, res[1]);
      return p;
    }
  }
  return JS_UNDEFINED;
}

static void define_gc(Env *env, JSContext *ctx) {
  JSValue g;
  if (!node_option_bool(env->options, "[expose-gc]"))
    return;
  g = JS_GetGlobalObject(ctx);
  JS_DefinePropertyValueStr(ctx, g, "gc", JS_NewCFunction(ctx, js_gc, "gc", 0),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, g);
}

JSContext *node_new_vm_context(Env *env) {
  JSContext *nctx = JS_NewContext(env->rt);
  if (!nctx)
    return NULL;
  JS_SetContextOpaque(nctx, env);
  node_context_intrinsics(nctx);
  define_gc(env, nctx);
  return nctx;
}

/* ---------------------------------------------------------------------- */
/* bootstrap */

static JSValue run_per_context(Env *env, const char *id) {
  JSContext *ctx = env->ctx;
  bool found;
  JSValue fn = node_compile_builtin(env, id, &found), args[4], ret;
  if (JS_IsException(fn))
    return fn;
  args[0] = env->per_context_exports;
  args[1] = env->primordials;
  args[2] = env->private_symbols;
  args[3] = env->per_isolate_symbols;
  ret = JS_Call(ctx, fn, JS_UNDEFINED, 4, (JSValueConst *)args);
  JS_FreeValue(ctx, fn);
  return ret;
}

static JSValue get_internal_binding_fn(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  const char *name = JS_ToCString(ctx, ARG(0));
  JSValue ret;
  if (!name)
    return JS_EXCEPTION;
  ret = node_get_internal_binding(env, name);
  JS_FreeCString(ctx, name);
  return ret;
}

static JSValue get_linked_binding_fn(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  const char *name = JS_ToCString(ctx, ARG(0));
  JSValue ret;
  if (!name)
    return JS_EXCEPTION;
  ret = node_get_linked_binding(env, name);
  JS_FreeCString(ctx, name);
  return ret;
}

static JSValue execute_bootstrapper(Env *env, const char *id) {
  JSContext *ctx = env->ctx;
  bool found;
  JSValue fn = node_compile_builtin(env, id, &found), args[4], ret;
  if (JS_IsException(fn))
    return fn;
  if (!strcmp(id, "internal/bootstrap/realm")) {
    args[0] = env->process;
    args[1] = JS_NewCFunction(ctx, get_linked_binding_fn, "getLinkedBinding", 1);
    args[2] = JS_NewCFunction(ctx, get_internal_binding_fn, "getInternalBinding", 1);
    args[3] = env->primordials;
    ret = JS_Call(ctx, fn, JS_UNDEFINED, 4, (JSValueConst *)args);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
  } else {
    args[0] = env->process;
    args[1] = env->builtin_require;
    args[2] = env->internal_binding_loader;
    args[3] = env->primordials;
    ret = JS_Call(ctx, fn, JS_UNDEFINED, 4, (JSValueConst *)args);
  }
  JS_FreeValue(ctx, fn);
  if (JS_IsException(ret))
    env_clear_async_id_stack(env);
  return ret;
}

JSValue node_create_process_object(Env *env); /* process_object.c */
JSValue node_create_env_proxy(Env *env);      /* process_env.c */

static void report_bootstrap_failure(Env *env, const char *what) {
  JSContext *ctx = env->ctx;
  JSValue err = JS_GetException(ctx);
  if (!env->is_main_thread && env->stopping) {
    JS_FreeValue(ctx, err);  /* a worker terminated while it started */
    return;
  }
  fprintf(stderr, "node: bootstrap failed in %s\n", what);
  node_print_uncaught(env, err, false);
  JS_FreeValue(ctx, err);
}

int env_bootstrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue r;
  static const char *const per_context[] = {
    "internal/per_context/primordials", "internal/per_context/domexception",
    "internal/per_context/messageport", NULL,
  };
  int i;

  /* InitializePrimordials */
  env->per_context_exports = JS_NewObjectProto(ctx, JS_NULL);
  env->primordials = JS_NewObjectProto(ctx, JS_NULL);
  JS_SetPropertyStr(ctx, env->per_context_exports, "primordials",
                    JS_DupValue(ctx, env->primordials));
  for (i = 0; per_context[i]; i++) {
    r = run_per_context(env, per_context[i]);
    if (JS_IsException(r)) {
      report_bootstrap_failure(env, per_context[i]);
      return -1;
    }
    JS_FreeValue(ctx, r);
  }

  env->process = node_create_process_object(env);
  if (JS_IsException(env->process)) {
    report_bootstrap_failure(env, "process object");
    return -1;
  }

  /* RunBootstrapping */
  r = execute_bootstrapper(env, "internal/bootstrap/realm");
  if (JS_IsException(r)) {
    report_bootstrap_failure(env, "internal/bootstrap/realm");
    return -1;
  }
  JS_FreeValue(ctx, r);
  {
    static const char *const ids[] = {
      "internal/bootstrap/node",
      "internal/bootstrap/web/exposed-wildcard",
      "internal/bootstrap/web/exposed-window-or-worker",
      NULL, NULL, NULL,
    };
    const char *seq[8];
    int n = 0;
    seq[n++] = ids[0];
    if (!node_option_bool(env->options, "--no-browser-globals")) {
      seq[n++] = ids[1];
      seq[n++] = ids[2];
    }
    seq[n++] = env->is_main_thread ? "internal/bootstrap/switches/is_main_thread"
                                   : "internal/bootstrap/switches/is_not_main_thread";
    seq[n++] = env->owns_process_state
                   ? "internal/bootstrap/switches/does_own_process_state"
                   : "internal/bootstrap/switches/does_not_own_process_state";
    for (i = 0; i < n; i++) {
      r = execute_bootstrapper(env, seq[i]);
      if (JS_IsException(r)) {
        report_bootstrap_failure(env, seq[i]);
        return -1;
      }
      JS_FreeValue(ctx, r);
    }
  }
  /* process.env */
  JS_SetPropertyStr(ctx, env->process, "env", node_create_env_proxy(env));
  define_gc(env, ctx);
  return 0;
}

int env_start_execution(Env *env, const char *main_script) {
  JSContext *ctx = env->ctx;
  JSValue r, resource = JS_NewObject(ctx);
  bool failed = false;

  /* InternalCallbackScope(env, {}, {1, 0}, kSkipAsyncHooks) */
  env->callback_scope_depth++;
  env_push_async_context(env, 1, 0, resource);
  r = execute_bootstrapper(env, main_script);
  if (JS_IsException(r)) {
    JSValue err = JS_GetException(ctx);
    failed = true;
    env_pop_async_context(env, 1);
    env->callback_scope_depth--;
    node_trigger_uncaught_exception(env, err, false);
    env->callback_scope_depth++;
    env_internal_callback_scope_exit(env, false);
    JS_FreeValue(ctx, resource);
    return -1;
  }
  JS_FreeValue(ctx, r);
  env_pop_async_context(env, 1);
  env_internal_callback_scope_exit(env, failed);
  JS_FreeValue(ctx, resource);
  return 0;
}

/* ---------------------------------------------------------------------- */
/* the event loop and process exit (api/hooks.cc) */

int node_process_emit(Env *env, const char *event, JSValueConst arg) {
  JSContext *ctx = env->ctx;
  JSValue emit, args[2], ret;
  if (!env->can_call_into_js)
    return -1;
  emit = JS_GetPropertyStr(ctx, env->process, "emit");
  if (!JS_IsFunction(ctx, emit)) {
    JS_FreeValue(ctx, emit);
    return 0;
  }
  args[0] = JS_NewString(ctx, event);
  args[1] = (JSValue)arg;
  ret = node_make_callback(env, env->process, env->process, emit, 2,
                           (JSValueConst *)args, 0, 0, JS_UNDEFINED);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, emit);
  if (JS_IsException(ret))
    return -1;
  JS_FreeValue(ctx, ret);
  return 0;
}

int env_exit_code(Env *env, int default_code) {
  return env->exit_info[kHasExitCode] ? env->exit_info[kExitCode] : default_code;
}

void env_stop(Env *env) {
  env->stopping = true;
  env->can_call_into_js = false;
  uv_stop(env->loop);
}

int env_spin_event_loop(Env *env) {
  bool more;
  JSContext *ctx = env->ctx;
  do {
    if (env->stopping)
      break;
    uv_run(env->loop, UV_RUN_DEFAULT);
    if (env->stopping)
      break;
    more = uv_loop_alive(env->loop);
    if (more && !env->stopping)
      continue;
    /* EmitProcessBeforeExit */
    if (env->destroy_ids_len)
      destroy_ids_cb(&env->destroy_idle);
    if (!env->can_call_into_js)
      break;
    if (!env->exit_info[kExiting]) {
      JSValue code = JS_NewInt32(ctx, env_exit_code(env, 0));
      node_process_emit(env, "beforeExit", code);
    }
    more = uv_loop_alive(env->loop);
  } while (more && !env->stopping);
  if (env->stopping)
    return env_exit_code(env, 0);
  /* EmitProcessExitInternal */
  env->exit_info[kExiting] = 1;
  {
    JSValue code = JS_NewInt32(ctx, env_exit_code(env, 0));
    JS_SetPropertyStr(ctx, env->process, "_exiting", JS_TRUE);
    node_process_emit(env, "exit", code);
  }
  return env_exit_code(env, 0);
}

void node_emit_process_warning(Env *env, const char *msg, const char *type,
                               const char *code) {
  JSContext *ctx = env->ctx;
  JSValue fn = JS_GetPropertyStr(ctx, env->process, "emitWarning"), args[3], r;
  if (!JS_IsFunction(ctx, fn)) {
    JS_FreeValue(ctx, fn);
    fprintf(stderr, "(node) %s\n", msg);
    return;
  }
  args[0] = JS_NewString(ctx, msg);
  args[1] = type ? JS_NewString(ctx, type) : JS_UNDEFINED;
  args[2] = code ? JS_NewString(ctx, code) : JS_UNDEFINED;
  r = JS_Call(ctx, fn, env->process, code ? 3 : (type ? 2 : 1), (JSValueConst *)args);
  if (JS_IsException(r))
    JS_FreeValue(ctx, JS_GetException(ctx));
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, args[2]);
  JS_FreeValue(ctx, fn);
}
