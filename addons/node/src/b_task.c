/* internalBinding('task_queue') (node_task_queue.cc), ('timers')
 * (timers.cc + Environment::RunTimers/CheckImmediate) and ('async_wrap')
 * (async_wrap.cc). */
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

void env_clear_async_id_stack(Env *env);
JSValue env_native_execution_async_resource(Env *env, uint32_t index);

/* ---- task_queue ---- */

static JSValue call_job(JSContext *ctx, int argc, JSValueConst *argv) {
  return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}

static JSValue tq_enqueue_microtask(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  if (!JS_IsFunction(ctx, ARG(0)))
    return JS_ThrowTypeError(ctx, "callback must be a function");
  JS_EnqueueJob(ctx, call_job, 1, argv);
  return JS_UNDEFINED;
}

static JSValue tq_run_microtasks(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  node_run_microtasks(env_get(ctx));
  return JS_UNDEFINED;
}

static JSValue tq_set_tick_callback(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->tick_callback);
  env->tick_callback = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue tq_set_promise_reject_callback(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->promise_reject_callback);
  env->promise_reject_callback = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

JSValue binding_init_task_queue(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), ev = JS_NewObject(ctx);
  nb_set_method(ctx, t, "enqueueMicrotask", tq_enqueue_microtask, 1);
  nb_set_method(ctx, t, "setTickCallback", tq_set_tick_callback, 1);
  nb_set_method(ctx, t, "runMicrotasks", tq_run_microtasks, 0);
  nb_set_method(ctx, t, "setPromiseRejectCallback", tq_set_promise_reject_callback, 1);
  nb_set(ctx, t, "tickInfo",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_INT32, env->tick_info, kTickFieldsCount, 4));
  nb_set_int(ctx, ev, "kPromiseRejectWithNoHandler", 0);
  nb_set_int(ctx, ev, "kPromiseHandlerAddedAfterReject", 1);
  nb_set_int(ctx, ev, "kPromiseResolveAfterResolved", 2);
  nb_set_int(ctx, ev, "kPromiseRejectAfterResolved", 3);
  nb_set(ctx, t, "promiseRejectEvents", ev);
  return t;
}

/* ---- timers ---- */

static int64_t env_get_now(Env *env) {
  uv_update_time(env->loop);
  return (int64_t)(uv_now(env->loop) - env->timer_base);
}

static void run_timers(uv_timer_t *handle) {
  Env *env = handle->data;
  JSContext *ctx = env->ctx;
  JSValue ret = JS_UNDEFINED, arg;
  int64_t expiry_ms = 0;
  if (!env->can_call_into_js)
    return;
  /* InternalCallbackScope(env, process, {0, 0}) */
  env->callback_scope_depth++;
  env_push_async_context(env, 0, 0, env->process);
  for (;;) {
    arg = JS_NewInt64(ctx, env_get_now(env));
    ret = JS_Call(ctx, env->timers_callback, env->process, 1, (JSValueConst *)&arg);
    if (!JS_IsException(ret))
      break;
    {
      JSValue err = JS_GetException(ctx);
      int depth = env->callback_scope_depth;
      env->callback_scope_depth = 0;
      node_trigger_uncaught_exception(env, err, false);
      env->callback_scope_depth = depth;
    }
    if (!env->can_call_into_js || env->stopping)
      break;
  }
  env_pop_async_context(env, 0);
  if (JS_IsException(ret)) {
    env_internal_callback_scope_exit(env, true);
    return;
  }
  JS_ToInt64(ctx, &expiry_ms, ret);
  JS_FreeValue(ctx, ret);
  if (expiry_ms != 0) {
    int64_t duration = llabs(expiry_ms) - (int64_t)(uv_now(env->loop) - env->timer_base);
    uv_timer_start(&env->timer_handle, run_timers, duration > 0 ? duration : 1, 0);
    if (expiry_ms > 0)
      uv_ref((uv_handle_t *)&env->timer_handle);
    else
      uv_unref((uv_handle_t *)&env->timer_handle);
  } else {
    uv_unref((uv_handle_t *)&env->timer_handle);
  }
  env_internal_callback_scope_exit(env, false);
}

static void toggle_immediate_ref(Env *env, bool ref);

static void idle_noop(uv_idle_t *h) {
  (void)h;
}

static void check_immediate(uv_check_t *handle) {
  Env *env = handle->data;
  JSContext *ctx = env->ctx;
  if (env->immediate_info[kImmCount] == 0 || !env->can_call_into_js)
    return;
  do {
    JSValue r = node_make_callback(env, env->process, env->process,
                                   env->immediate_callback, 0, NULL, 0, 0, JS_UNDEFINED);
    JS_FreeValue(ctx, r);
  } while (env->immediate_info[kImmHasOutstanding] && env->can_call_into_js &&
           !env->stopping);
  if (env->immediate_info[kImmRefCount] == 0)
    toggle_immediate_ref(env, false);
}

static void toggle_immediate_ref(Env *env, bool ref) {
  if (env->stopping)
    return;
  if (ref)
    uv_idle_start(&env->immediate_idle, idle_noop);
  else
    uv_idle_stop(&env->immediate_idle);
}

void node_timers_init(Env *env) {
  uv_timer_init(env->loop, &env->timer_handle);
  env->timer_handle.data = env;
  uv_unref((uv_handle_t *)&env->timer_handle);
  uv_check_init(env->loop, &env->immediate_check);
  env->immediate_check.data = env;
  uv_unref((uv_handle_t *)&env->immediate_check);
  uv_check_start(&env->immediate_check, check_immediate);
  uv_idle_init(env->loop, &env->immediate_idle);
  env->immediate_idle.data = env;
  uv_update_time(env->loop);
  env->timer_base = uv_now(env->loop);
}

void node_timers_close(Env *env) {
  uv_close((uv_handle_t *)&env->timer_handle, NULL);
  uv_close((uv_handle_t *)&env->immediate_check, NULL);
  uv_close((uv_handle_t *)&env->immediate_idle, NULL);
}

static JSValue timers_get_libuv_now(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  return JS_NewFloat64(ctx, (double)env_get_now(env_get(ctx)));
}

static JSValue timers_setup(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->immediate_callback);
  JS_FreeValue(ctx, env->timers_callback);
  env->immediate_callback = JS_DupValue(ctx, ARG(0));
  env->timers_callback = JS_DupValue(ctx, ARG(1));
  return JS_UNDEFINED;
}

static JSValue timers_schedule(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  Env *env = env_get(ctx);
  int64_t ms = nb_int64(ctx, ARG(0), 1);
  if (ms < 1)
    ms = 1;
  uv_timer_start(&env->timer_handle, run_timers, ms, 0);
  return JS_UNDEFINED;
}

static JSValue timers_toggle_timer_ref(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  Env *env = env_get(ctx);
  if (JS_ToBool(ctx, ARG(0)))
    uv_ref((uv_handle_t *)&env->timer_handle);
  else
    uv_unref((uv_handle_t *)&env->timer_handle);
  return JS_UNDEFINED;
}

static JSValue timers_toggle_immediate_ref(JSContext *ctx, JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  toggle_immediate_ref(env_get(ctx), JS_ToBool(ctx, ARG(0)));
  return JS_UNDEFINED;
}

JSValue binding_init_timers(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "getLibuvNow", timers_get_libuv_now, 0);
  nb_set_method(ctx, t, "setupTimers", timers_setup, 2);
  nb_set_method(ctx, t, "scheduleTimer", timers_schedule, 1);
  nb_set_method(ctx, t, "toggleTimerRef", timers_toggle_timer_ref, 1);
  nb_set_method(ctx, t, "toggleImmediateRef", timers_toggle_immediate_ref, 1);
  nb_set(ctx, t, "immediateInfo",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32, env->immediate_info, kImmFieldsCount, 4));
  nb_set(ctx, t, "timeoutInfo",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_INT32, env->timeout_info, 1, 4));
  return t;
}

/* ---- async_wrap ---- */

static JSValue aw_setup_hooks(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValueConst fns = ARG(0);
  JSValue *slots[] = { &env->async_hooks_init, &env->async_hooks_before,
                       &env->async_hooks_after, &env->async_hooks_destroy,
                       &env->async_hooks_promise_resolve };
  static const char *const names[] = { "init", "before", "after", "destroy",
                                       "promise_resolve" };
  size_t i;
  if (!JS_IsObject(fns))
    return JS_ThrowTypeError(ctx, "hooks object expected");
  for (i = 0; i < countof(names); i++) {
    JSValue f = JS_GetPropertyStr(ctx, fns, names[i]);
    JS_FreeValue(ctx, *slots[i]);
    *slots[i] = JS_IsFunction(ctx, f) ? f : (JS_FreeValue(ctx, f), JS_UNDEFINED);
  }
  return JS_UNDEFINED;
}

static JSValue aw_set_callback_trampoline(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->async_callback_trampoline);
  env->async_callback_trampoline = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue aw_push_async_context(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  Env *env = env_get(ctx);
  env_push_async_context(env, nb_double(ctx, ARG(0), 0), nb_double(ctx, ARG(1), 0),
                         JS_UNDEFINED);
  return JS_UNDEFINED;
}

static JSValue aw_pop_async_context(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  Env *env = env_get(ctx);
  return JS_NewBool(ctx, env_pop_async_context(env, nb_double(ctx, ARG(0), 0)));
}

static JSValue aw_execution_async_resource(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
  return env_native_execution_async_resource(env_get(ctx), nb_uint32(ctx, ARG(0), 0));
}

static JSValue aw_clear_async_id_stack(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  env_clear_async_id_stack(env_get(ctx));
  return JS_UNDEFINED;
}

static JSValue aw_queue_destroy_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  env_queue_destroy_async_id(env_get(ctx), nb_double(ctx, ARG(0), 0));
  return JS_UNDEFINED;
}

/* V8 promise hooks over QuickJS's (the functions are the environment's) */
static void promise_hook(JSContext *ctx, JSPromiseHookType type, JSValueConst promise,
                         JSValueConst parent, void *opaque) {
  JSValue *promise_hook_fns = env_get(ctx)->promise_hook_fns;
  int idx;
  JSValue r, args[2];
  switch (type) {
  case JS_PROMISE_HOOK_INIT: idx = 0; break;
  case JS_PROMISE_HOOK_BEFORE: idx = 1; break;
  case JS_PROMISE_HOOK_AFTER: idx = 2; break;
  default: idx = 3; break;
  }
  if (!JS_IsFunction(ctx, promise_hook_fns[idx]))
    return;
  args[0] = (JSValue)promise;
  args[1] = (JSValue)parent;
  r = JS_Call(ctx, promise_hook_fns[idx], JS_UNDEFINED, idx == 0 ? 2 : 1,
              (JSValueConst *)args);
  if (JS_IsException(r))
    node_trigger_uncaught_exception(env_get(ctx), JS_GetException(ctx), false);
  else
    JS_FreeValue(ctx, r);
}

static JSValue aw_set_promise_hooks(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue *promise_hook_fns = env->promise_hook_fns;
  int i;
  bool any = false;
  for (i = 0; i < 4; i++) {
    JS_FreeValue(ctx, promise_hook_fns[i]);
    promise_hook_fns[i] = JS_IsFunction(ctx, ARG(i)) ? JS_DupValue(ctx, ARG(i)) : JS_UNDEFINED;
    if (!JS_IsUndefined(promise_hook_fns[i]))
      any = true;
  }
  JS_SetPromiseHook(env->rt, any ? promise_hook : NULL, NULL);
  env->promise_hooks_set = any;
  return JS_UNDEFINED;
}

static JSValue aw_get_promise_hooks(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  JSValue arr = JS_NewArray(ctx);
  int i;
  for (i = 0; i < 4; i++)
    JS_SetPropertyUint32(ctx, arr, i, JS_DupValue(ctx, env_get(ctx)->promise_hook_fns[i]));
  return arr;
}

static JSValue aw_register_destroy_hook(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  /* emitted when the resource is collected: approximated by FinalizationRegistry
     in JS land; nothing to do natively */
  return JS_UNDEFINED;
}

JSValue binding_init_async_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), c = JS_NewObject(ctx), p = JS_NewObject(ctx);
  int i;
  nb_set_method(ctx, t, "setupHooks", aw_setup_hooks, 1);
  nb_set_method(ctx, t, "setCallbackTrampoline", aw_set_callback_trampoline, 1);
  nb_set_method(ctx, t, "pushAsyncContext", aw_push_async_context, 3);
  nb_set_method(ctx, t, "popAsyncContext", aw_pop_async_context, 1);
  nb_set_method(ctx, t, "executionAsyncResource", aw_execution_async_resource, 1);
  nb_set_method(ctx, t, "clearAsyncIdStack", aw_clear_async_id_stack, 0);
  nb_set_method(ctx, t, "queueDestroyAsyncId", aw_queue_destroy_async_id, 1);
  nb_set_method(ctx, t, "setPromiseHooks", aw_set_promise_hooks, 4);
  nb_set_method(ctx, t, "getPromiseHooks", aw_get_promise_hooks, 0);
  nb_set_method(ctx, t, "registerDestroyHook", aw_register_destroy_hook, 3);
  nb_define_readonly(ctx, t, "async_hook_fields",
                     nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32, env->async_hook_fields,
                                        kAsyncHookFieldsCount, 4));
  nb_define_readonly(ctx, t, "async_id_fields",
                     nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, env->async_id_fields,
                                        kUidFieldsCount, 8));
  nb_define_readonly(ctx, t, "execution_async_resources",
                     JS_DupValue(ctx, env->execution_async_resources));
  JS_SetPropertyStr(ctx, t, "async_ids_stack",
                    nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, env->async_ids_stack,
                                       env->async_ids_stack_len, 8));
  nb_set_int(ctx, c, "kInit", kInit);
  nb_set_int(ctx, c, "kBefore", kBefore);
  nb_set_int(ctx, c, "kAfter", kAfter);
  nb_set_int(ctx, c, "kDestroy", kDestroy);
  nb_set_int(ctx, c, "kPromiseResolve", kPromiseResolve);
  nb_set_int(ctx, c, "kTotals", kTotals);
  nb_set_int(ctx, c, "kCheck", kCheck);
  nb_set_int(ctx, c, "kExecutionAsyncId", kExecutionAsyncId);
  nb_set_int(ctx, c, "kTriggerAsyncId", kTriggerAsyncId);
  nb_set_int(ctx, c, "kAsyncIdCounter", kAsyncIdCounter);
  nb_set_int(ctx, c, "kDefaultTriggerAsyncId", kDefaultTriggerAsyncId);
  nb_set_int(ctx, c, "kUsesExecutionAsyncResource", kUsesExecutionAsyncResource);
  nb_set_int(ctx, c, "kStackLength", kStackLength);
  nb_define_readonly(ctx, t, "constants", c);
  for (i = 0; i < PROVIDERS_LENGTH; i++)
    nb_define_readonly(ctx, p, node_provider_names[i], JS_NewInt32(ctx, i));
  nb_define_readonly(ctx, t, "Providers", p);
  JS_FreeValue(ctx, env->async_hooks_binding);
  env->async_hooks_binding = JS_DupValue(ctx, t);
  return t;
}
