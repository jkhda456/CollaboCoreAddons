/* worker_threads (node_worker.cc): each Worker is a thread with its own
 * QuickJS runtime, event loop and Node environment, booted the way the main
 * thread is (internal/main/worker_thread) and talking to its parent over a
 * MessagePort pair (b_messaging.c).
 *
 * When a worker ends, its handles and ports are closed the way JS closes
 * them and its loop drained; then its JS heap goes all at once: the runtime
 * allocates from an arena of its own, freed without running finalizers (which
 * expect a live loop), as the main thread's heap is not torn down at exit. */
#include <malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

void node_perf_init(Env *env, uint64_t start_ns, double origin_timestamp_us);
void node_perf_mark_loop_start(Env *env);
size_t node_worker_stack_limit(size_t want);
void *node_port_pair_new(void **other);
JSValue node_port_object(Env *env, void *data);
void node_port_data_release(void *data);

enum { kMaxYoungGenerationSizeMb, kMaxOldGenerationSizeMb, kCodeRangeSizeMb, kStackSizeMb,
       kTotalResourceLimitCount };

typedef struct Worker {
  AsyncWrap aw;               /* the parent's handle object */
  Env *parent;
  pthread_mutex_t lock;
  int refs;                   /* the handle object, and the thread while it runs */
  int thread_id;
  char *name;
  char *exec_path;
  char **exec_argv;
  int exec_argc;
  char *options_json;         /* the parent's option values */
  void *env_store;            /* the worker's process.env (NULL: the real one) */
  double limits[kTotalResourceLimitCount];
  void *child_port;           /* the worker's end of the channel, until it takes it */
  /* the thread */
  uv_thread_t thread;
  bool started, finished, joined;
  uv_async_t exit_async;      /* on the parent's loop: the thread ended */
  bool exit_async_open;
  bool refed;
  atomic_bool stop_requested;
  Env *child;                 /* while it runs (under lock) */
  uv_async_t *wake;           /* the child loop's stop signal (under lock) */
  uv_loop_t *child_loop;
  uint64_t loop_start;        /* hrtime when its loop started, 0 before */
  int exit_code;
} Worker;

static JSClassID worker_class_id;
static atomic_int next_thread_id = 1;

/* ---------------------------------------------------------------------- */
/* the arena a worker's runtime allocates from: every block on one list */

typedef struct ArenaBlock {
  struct ArenaBlock *prev, *next;
} ArenaBlock;

#define ARENA_HDR ((sizeof(ArenaBlock) + 15) & ~(size_t)15)

static void *arena_link(ArenaBlock *head, ArenaBlock *b) {
  b->next = head->next;
  b->prev = head;
  head->next->prev = b;
  head->next = b;
  return (char *)b + ARENA_HDR;
}

static ArenaBlock *arena_block(void *ptr) {
  return (ArenaBlock *)((char *)ptr - ARENA_HDR);
}

static void arena_unlink(ArenaBlock *b) {
  b->prev->next = b->next;
  b->next->prev = b->prev;
}

static void *arena_malloc(void *opaque, size_t size) {
  ArenaBlock *b = malloc(ARENA_HDR + size);
  return b ? arena_link(opaque, b) : NULL;
}

static void *arena_calloc(void *opaque, size_t count, size_t size) {
  ArenaBlock *b;
  if (size && count > (SIZE_MAX - ARENA_HDR) / size)
    return NULL;
  b = calloc(1, ARENA_HDR + count * size);
  return b ? arena_link(opaque, b) : NULL;
}

static void arena_free(void *opaque, void *ptr) {
  ArenaBlock *b;
  if (!ptr)
    return;
  b = arena_block(ptr);
  arena_unlink(b);
  free(b);
}

static void *arena_realloc(void *opaque, void *ptr, size_t size) {
  ArenaBlock *b, *nb;
  if (!ptr)
    return size ? arena_malloc(opaque, size) : NULL;
  if (!size) {
    arena_free(opaque, ptr);
    return NULL;
  }
  b = arena_block(ptr);
  arena_unlink(b);
  nb = realloc(b, ARENA_HDR + size);
  if (!nb) {
    arena_link(opaque, b);
    return NULL;
  }
  return arena_link(opaque, nb);
}

static size_t arena_usable_size(const void *ptr) {
  size_t n;
  if (!ptr)
    return 0;
  n = malloc_usable_size((void *)((const char *)ptr - ARENA_HDR));
  return n > ARENA_HDR ? n - ARENA_HDR : 0;
}

static const JSMallocFunctions arena_functions = {
  .js_calloc = arena_calloc, .js_malloc = arena_malloc, .js_free = arena_free,
  .js_realloc = arena_realloc, .js_malloc_usable_size = arena_usable_size,
};

static void arena_release(ArenaBlock *head) {
  ArenaBlock *b = head->next, *n;
  while (b != head) {
    n = b->next;
    free(b);
    b = n;
  }
  head->next = head->prev = head;
}

static void worker_unref(Worker *w) {
  bool last;
  int i;
  pthread_mutex_lock(&w->lock);
  last = --w->refs == 0;
  pthread_mutex_unlock(&w->lock);
  if (!last)
    return;
  if (w->child_port)
    node_port_data_release(w->child_port);
  node_env_store_unref(w->env_store);
  for (i = 0; i < w->exec_argc; i++)
    free(w->exec_argv[i]);
  free(w->exec_argv);
  free(w->options_json);
  free(w->exec_path);
  free(w->name);
  pthread_mutex_destroy(&w->lock);
  free(w);
}

/* ---------------------------------------------------------------------- */
/* the worker's thread */

bool node_vm_timeout_interrupt(void); /* b_contextify.c */

static int interrupt_cb(JSRuntime *rt, void *opaque) {
  Worker *w = opaque;
  if (!atomic_load(&w->stop_requested))
    return node_vm_timeout_interrupt();
  if (w->child && !w->child->stopping)
    env_stop(w->child);
  return 1;
}

static void wake_cb(uv_async_t *h) {
  Env *env = h->data;
  if (!env->stopping)
    env_stop(env);
}

static void walk_close(uv_handle_t *h, void *arg) {
  if (!uv_is_closing(h))
    uv_close(h, NULL);
}

static void worker_main(void *arg) {
  Worker *w = arg;
  uv_loop_t *loop = calloc(1, sizeof(*loop));
  uv_async_t *wake = calloc(1, sizeof(*wake));
  NodeOptions *opts = calloc(1, sizeof(*opts));
  ArenaBlock arena = { &arena, &arena };
  JSRuntime *rt;
  JSContext *ctx;
  Env *env;
  size_t want;
  uint64_t start = uv_hrtime();
  uv_timeval64_t now;
  int code = 1, i;

  uv_loop_init(loop);
  uv_loop_configure(loop, UV_METRICS_IDLE_TIME);
  want = w->limits[kStackSizeMb] > 0 ? (size_t)(w->limits[kStackSizeMb] * 1024 * 1024)
                                      : 4 * 1024 * 1024;
  rt = node_new_runtime2(node_worker_stack_limit(want), &arena_functions, &arena);
  JS_SetInterruptHandler(rt, interrupt_cb, w);
  ctx = JS_NewContext(rt);
  node_context_intrinsics(ctx);

  /* the parent's options; argv is the program alone (the script and its
     arguments come in the LOAD_SCRIPT message), execArgv the worker's */
  node_options_init_worker(ctx, opts, w->options_json);
  opts->argc = 1;
  opts->argv = calloc(2, sizeof(char *));
  opts->argv[0] = strdup(w->exec_path);
  opts->exec_argc = w->exec_argc;
  opts->exec_argv = calloc(w->exec_argc + 1, sizeof(char *));
  for (i = 0; i < w->exec_argc; i++)
    opts->exec_argv[i] = strdup(w->exec_argv[i]);

  env = env_new(rt, ctx, loop, false);
  env->thread_id = w->thread_id;
  env->owns_process_state = false;
  env->worker = w;
  env->options = opts;
  env->argc = opts->argc;
  env->argv = opts->argv;
  env->exec_argc = opts->exec_argc;
  env->exec_argv = opts->exec_argv;
  env->exec_path = strdup(w->exec_path);
  env->start_time_ns = start;
  env->env_store = w->env_store;
  w->env_store = NULL;
  memcpy(env_scratch(env, "resource_limits", sizeof(w->limits)), w->limits, sizeof(w->limits));
  uv_gettimeofday(&now);
  node_perf_init(env, start, (double)now.tv_sec * 1e6 + now.tv_usec);

  uv_async_init(loop, wake, wake_cb);
  wake->data = env;
  uv_unref((uv_handle_t *)wake);
  pthread_mutex_lock(&w->lock);
  w->child = env;
  w->wake = wake;
  w->child_loop = loop;
  pthread_mutex_unlock(&w->lock);

  if (!atomic_load(&w->stop_requested) && env_bootstrap(env) == 0 &&
      env_start_execution(env, "internal/main/worker_thread") == 0) {
    w->loop_start = uv_hrtime();
    node_perf_mark_loop_start(env);
    code = env_spin_event_loop(env);
  } else {
    code = env_exit_code(env, 1);
  }
  if (atomic_load(&w->stop_requested))
    code = 1;

  /* no more JS; close what is left and let the loop finish */
  env->can_call_into_js = false;
  env->stopping = true;
  pthread_mutex_lock(&w->lock);
  w->child = NULL;
  w->wake = NULL;
  w->child_loop = NULL;
  pthread_mutex_unlock(&w->lock);
  node_handles_close_all(env);
  uv_close((uv_handle_t *)wake, NULL);
  uv_walk(loop, walk_close, NULL);  /* the rest: timers, the environment's own */
  uv_run(loop, UV_RUN_DEFAULT);
  if (uv_loop_close(loop) == 0) {
    free(loop);
    free(wake);
  }

  /* the heap, at once; then what lives outside it */
  arena_release(&arena);
  env_free_native(env);
  for (i = 0; i < opts->argc; i++)
    free(opts->argv[i]);
  free(opts->argv);
  for (i = 0; i < opts->exec_argc; i++)
    free(opts->exec_argv[i]);
  free(opts->exec_argv);
  free(opts);

  pthread_mutex_lock(&w->lock);
  w->exit_code = code;
  w->finished = true;
  if (w->exit_async_open)
    uv_async_send(&w->exit_async);
  pthread_mutex_unlock(&w->lock);
}

/* ---------------------------------------------------------------------- */
/* the parent's side */

static void exit_async_closed(uv_handle_t *h) {
  Worker *w = h->data;
  worker_unref(w);  /* the thread's reference */
}

static void exit_cb(uv_async_t *h) {
  Worker *w = h->data;
  Env *env = w->aw.env;
  JSContext *ctx = env->ctx;
  JSValue args[3], r;
  bool finished;
  pthread_mutex_lock(&w->lock);
  finished = w->finished;
  pthread_mutex_unlock(&w->lock);
  if (!finished || w->joined)
    return;
  uv_thread_join(&w->thread);
  w->joined = true;
  pthread_mutex_lock(&w->lock);
  w->exit_async_open = false;
  pthread_mutex_unlock(&w->lock);
  uv_close((uv_handle_t *)&w->exit_async, exit_async_closed);
  args[0] = JS_NewInt32(ctx, w->exit_code);
  args[1] = JS_UNDEFINED;
  args[2] = JS_UNDEFINED;
  r = async_wrap_make_callback_name(&w->aw, "onexit", 3, (JSValueConst *)args);
  JS_FreeValue(ctx, r);
  async_wrap_unref(&w->aw);  /* running workers keep their handle */
}

static Worker *worker_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, worker_class_id);
}

static char **strings_of(JSContext *ctx, JSValueConst arr, int *count) {
  JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
  uint32_t n = 0, i;
  char **v;
  JS_ToUint32(ctx, &n, lenv);
  JS_FreeValue(ctx, lenv);
  v = calloc(n + 1, sizeof(char *));
  for (i = 0; i < n; i++) {
    JSValue e = JS_GetPropertyUint32(ctx, arr, i), s = JS_ToString(ctx, e);
    v[i] = node_string_to_utf8(ctx, s, NULL);
    JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, e);
  }
  *count = (int)n;
  return v;
}

/* new Worker(url, env, execArgv, resourceLimits, trackUnmanagedFds, isInternal, name) */
static JSValue worker_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue obj = nb_new_instance(ctx, nt, worker_class_id), port;
  Worker *w;
  void *parent_end;
  int i;
  if (JS_IsException(obj))
    return obj;
  w = calloc(1, sizeof(*w));
  pthread_mutex_init(&w->lock, NULL);
  w->refs = 1;
  w->parent = env;
  w->thread_id = atomic_fetch_add(&next_thread_id, 1);
  w->exec_path = strdup(env->exec_path ? env->exec_path : "node");
  w->name = JS_IsString(ARG(6)) ? node_string_to_utf8(ctx, ARG(6), NULL) : strdup("WorkerThread");
  w->refed = true;
  async_wrap_init(&w->aw, env, obj, PROVIDER_WORKER, -1);
  JS_SetOpaque(obj, w);

  /* process.env: null copies the parent's, an object gives the variables,
     undefined (SHARE_ENV) shares the parent's */
  if (JS_IsNull(ARG(1))) {
    w->env_store = node_env_store_copy(env);
  } else if (JS_IsObject(ARG(1))) {
    JSPropertyEnum *props;
    uint32_t n = 0, k;
    char **kv = NULL;
    int count = 0;
    if (!JS_GetOwnPropertyNames(ctx, &props, &n, ARG(1), JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
      kv = calloc(n + 1, sizeof(char *));
      for (k = 0; k < n; k++) {
        const char *name = JS_AtomToCString(ctx, props[k].atom);
        JSValue v = JS_GetProperty(ctx, ARG(1), props[k].atom), sv = JS_ToString(ctx, v);
        char *value = node_string_to_utf8(ctx, sv, NULL);
        if (name && value && name[0] && !strchr(name, '=')) {
          kv[count] = malloc(strlen(name) + strlen(value) + 2);
          sprintf(kv[count++], "%s=%s", name, value);
        }
        free(value);
        JS_FreeValue(ctx, sv);
        JS_FreeValue(ctx, v);
        JS_FreeCString(ctx, name);
      }
      JS_FreePropertyEnum(ctx, props, n);
    }
    w->env_store = node_env_store_from(kv, count);
    for (i = 0; i < count; i++)
      free(kv[i]);
    free(kv);
  } else {
    w->env_store = node_env_store_ref(env);
  }

  if (JS_IsArray(ARG(2))) {
    int k, m = 0;
    w->exec_argv = strings_of(ctx, ARG(2), &w->exec_argc);
    for (k = 0; k < w->exec_argc; k++) {  /* "--" is not in execArgv */
      if (!strcmp(w->exec_argv[k], "--"))
        free(w->exec_argv[k]);
      else
        w->exec_argv[m++] = w->exec_argv[k];
    }
    w->exec_argc = m;
    w->exec_argv[m] = NULL;
  } else {
    w->exec_argc = env->exec_argc;
    w->exec_argv = calloc(env->exec_argc + 1, sizeof(char *));
    for (i = 0; i < env->exec_argc; i++)
      w->exec_argv[i] = strdup(env->exec_argv[i]);
  }
  {
    size_t len = 0;
    double *lim = (double *)nb_buffer_data(ctx, ARG(3), &len);
    for (i = 0; i < kTotalResourceLimitCount; i++)
      w->limits[i] = lim && len >= (i + 1) * sizeof(double) ? lim[i] : -1;
  }
  w->options_json = node_options_values_json(env->options);

  /* the channel: our end is messagePort, the worker's waits for it */
  port = node_port_object(env, node_port_pair_new(&parent_end));
  w->child_port = parent_end;
  if (JS_IsException(port)) {
    JS_FreeValue(ctx, obj);
    return port;
  }
  JS_DefinePropertyValueStr(ctx, obj, "messagePort", port, JS_PROP_C_W_E);
  JS_DefinePropertyValueStr(ctx, obj, "threadId", JS_NewInt32(ctx, w->thread_id), JS_PROP_C_W_E);
  JS_DefinePropertyValueStr(ctx, obj, "threadName", JS_NewString(ctx, w->name), JS_PROP_C_W_E);
  return obj;
}

static void worker_finalizer(JSRuntime *rt, JSValueConst val) {
  Worker *w = JS_GetOpaque(val, worker_class_id);
  if (!w)
    return;
  async_wrap_destroy(&w->aw);
  w->aw.env = NULL;
  worker_unref(w);
}

static JSValue worker_start_thread(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Worker *w = worker_of(ctx, this_val);
  Env *env = env_get(ctx);
  uv_thread_options_t o;
  size_t want;
  int r;
  if (!w)
    return JS_EXCEPTION;
  if (w->started)
    return JS_UNDEFINED;
  uv_async_init(env->loop, &w->exit_async, exit_cb);
  w->exit_async.data = w;
  w->exit_async_open = true;
  if (!w->refed)
    uv_unref((uv_handle_t *)&w->exit_async);
  pthread_mutex_lock(&w->lock);
  w->refs++;  /* the thread's */
  pthread_mutex_unlock(&w->lock);
  async_wrap_ref(&w->aw);
  /* room for the interpreter's stack and the C below and above it */
  want = w->limits[kStackSizeMb] > 0 ? (size_t)(w->limits[kStackSizeMb] * 1024 * 1024)
                                      : 4 * 1024 * 1024;
  o.flags = UV_THREAD_HAS_STACK_SIZE;
  o.stack_size = node_worker_stack_limit(want) + 4 * 1024 * 1024;
  w->started = true;
  r = uv_thread_create_ex(&w->thread, &o, worker_main, w);
  if (r) {
    w->started = false;
    w->exit_async_open = false;
    uv_close((uv_handle_t *)&w->exit_async, exit_async_closed);
    async_wrap_unref(&w->aw);
    return node_throw_uv_exception(ctx, r, "uv_thread_create", NULL, NULL, NULL);
  }
  return JS_UNDEFINED;
}

static JSValue worker_stop_thread(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  Worker *w = worker_of(ctx, this_val);
  if (!w)
    return JS_EXCEPTION;
  atomic_store(&w->stop_requested, true);
  pthread_mutex_lock(&w->lock);
  if (w->wake)
    uv_async_send(w->wake);
  pthread_mutex_unlock(&w->lock);
  return JS_UNDEFINED;
}

static JSValue worker_ref(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  Worker *w = worker_of(ctx, this_val);
  if (!w)
    return JS_EXCEPTION;
  w->refed = true;
  if (w->exit_async_open)
    uv_ref((uv_handle_t *)&w->exit_async);
  return JS_UNDEFINED;
}

static JSValue worker_unref_m(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  Worker *w = worker_of(ctx, this_val);
  if (!w)
    return JS_EXCEPTION;
  w->refed = false;
  if (w->exit_async_open)
    uv_unref((uv_handle_t *)&w->exit_async);
  return JS_UNDEFINED;
}

static JSValue worker_has_ref(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  Worker *w = worker_of(ctx, this_val);
  if (!w)
    return JS_EXCEPTION;
  return JS_NewBool(ctx, w->exit_async_open && w->refed);
}

static JSValue worker_get_resource_limits(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  Worker *w = worker_of(ctx, this_val);
  JSValue arr;
  double *d;
  if (!w)
    return JS_EXCEPTION;
  d = nb_alloc_shared_array(ctx, JS_TYPED_ARRAY_FLOAT64, kTotalResourceLimitCount, 8, &arr);
  memcpy(d, w->limits, sizeof(w->limits));
  return arr;
}

static JSValue worker_loop_idle_time(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  Worker *w = worker_of(ctx, this_val);
  double t = -1;
  if (!w)
    return JS_EXCEPTION;
  pthread_mutex_lock(&w->lock);
  if (w->child && w->loop_start)
    t = (double)uv_metrics_idle_time(w->child_loop) / 1e6;
  pthread_mutex_unlock(&w->lock);
  return JS_NewFloat64(ctx, t);
}

static JSValue worker_loop_start_time(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  Worker *w = worker_of(ctx, this_val);
  Env *env = env_get(ctx);
  if (!w)
    return JS_EXCEPTION;
  if (!w->loop_start)
    return JS_NewFloat64(ctx, -1);
  return JS_NewFloat64(ctx, (double)(w->loop_start - env->start_time_ns) / 1e6);
}

static JSValue worker_not_available(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry worker_proto[] = {
  JS_CFUNC_DEF("startThread", 0, worker_start_thread),
  JS_CFUNC_DEF("stopThread", 0, worker_stop_thread),
  JS_CFUNC_DEF("ref", 0, worker_ref),
  JS_CFUNC_DEF("unref", 0, worker_unref_m),
  JS_CFUNC_DEF("hasRef", 0, worker_has_ref),
  JS_CFUNC_DEF("getResourceLimits", 0, worker_get_resource_limits),
  JS_CFUNC_DEF("loopIdleTime", 0, worker_loop_idle_time),
  JS_CFUNC_DEF("loopStartTime", 0, worker_loop_start_time),
  JS_CFUNC_DEF("takeHeapSnapshot", 1, worker_not_available),
  JS_CFUNC_DEF("getHeapStatistics", 0, worker_not_available),
  JS_CFUNC_DEF("cpuUsage", 0, worker_not_available),
  JS_CFUNC_DEF("startCpuProfile", 0, worker_not_available),
  JS_CFUNC_DEF("stopCpuProfile", 1, worker_not_available),
  JS_CFUNC_DEF("startHeapProfile", 0, worker_not_available),
  JS_CFUNC_DEF("stopHeapProfile", 0, worker_not_available),
};

JSValue node_worker_class(Env *env, JSValueConst target) {
  JSContext *ctx = env->ctx;
  NodeClassDef def = { .name = "Worker", .class_id = &worker_class_id,
                       .ctor = worker_ctor, .ctor_length = 7, .finalizer = worker_finalizer,
                       .proto_funcs = worker_proto, .proto_funcs_count = countof(worker_proto),
                       .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, target, &def));
  return JS_UNDEFINED;
}

/* the worker's own end of the channel (getEnvMessagePort), one object */
JSValue node_worker_env_message_port(Env *env) {
  JSContext *ctx = env->ctx;
  Worker *w = env->worker;
  JSValue holder, port;
  if (!w)
    return JS_UNDEFINED;
  holder = env_binding_data(env, "worker");
  port = JS_GetPropertyStr(ctx, holder, "envMessagePort");
  if (JS_IsUndefined(port) && w->child_port) {
    port = node_port_object(env, w->child_port);
    w->child_port = NULL;
    if (!JS_IsException(port))
      JS_SetPropertyStr(ctx, holder, "envMessagePort", JS_DupValue(ctx, port));
  }
  JS_FreeValue(ctx, holder);
  return port;
}

const char *node_worker_name(Env *env) {
  Worker *w = env->worker;
  return w && w->name ? w->name : "WorkerThread";
}
