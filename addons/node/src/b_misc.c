/* Small bindings: config, symbols, credentials, trace_events,
 * async_context_frame, mksnapshot, permission, inspector, wasm_web_api,
 * diagnostics_channel, performance, heap_utils, v8, report, profiler, sea,
 * watchdog, internal_only_v8, and placeholders for features the guest
 * build leaves out. */
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>

#include "node.h"

extern const char node_config_gypi_json[];

/* ---- config ---- */

static JSValue config_get_default_locale(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  return JS_NewString(ctx, "en-US");
}

JSValue binding_init_config(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_bool(ctx, t, "isDebugBuild", false);
  nb_set_bool(ctx, t, "hasOpenSSL", true);
  nb_set_bool(ctx, t, "openSSLIsBoringSSL", false);
  nb_set_bool(ctx, t, "fipsMode", false);
  nb_set_bool(ctx, t, "hasIntl", false);
  nb_set_bool(ctx, t, "hasSmallICU", false);
  nb_set_bool(ctx, t, "hasTracing", false);
  nb_set_bool(ctx, t, "hasNodeOptions", true);
  nb_set_bool(ctx, t, "hasInspector", false);
  nb_set_bool(ctx, t, "noBrowserGlobals", false);
  nb_set_int(ctx, t, "bits", 32);
  nb_set_method(ctx, t, "getDefaultLocale", config_get_default_locale, 0);
  return t;
}

/* ---- symbols ---- */

JSValue binding_init_symbols(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  JSPropertyEnum *props;
  uint32_t n, i;
  JS_GetOwnPropertyNames(ctx, &props, &n, env->per_isolate_symbols, JS_GPN_STRING_MASK);
  for (i = 0; i < n; i++) {
    JSValue v = JS_GetProperty(ctx, env->per_isolate_symbols, props[i].atom);
    const char *name = JS_AtomToCString(ctx, props[i].atom);
    /* the binding names some of them without _symbol, as V8 strings do */
    if (!strcmp(name, "handle_onclose_symbol"))
      nb_set(ctx, t, "handle_onclose", JS_DupValue(ctx, v));
    else if (!strcmp(name, "oninit_symbol"))
      nb_set(ctx, t, "oninit", JS_DupValue(ctx, v));
    else if (!strcmp(name, "onpskexchange_symbol"))
      nb_set(ctx, t, "onpskexchange", JS_DupValue(ctx, v));
    JS_SetProperty(ctx, t, props[i].atom, v);
    JS_FreeCString(ctx, name);
  }
  JS_FreePropertyEnum(ctx, props, n);
  return t;
}

/* ---- credentials ---- */

static JSValue cred_safe_getenv(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  const char *name = JS_ToCString(ctx, ARG(0));
  const char *v;
  JSValue r;
  if (!name)
    return JS_EXCEPTION;
  v = getenv(name);
  r = v ? JS_NewString(ctx, v) : JS_UNDEFINED;
  JS_FreeCString(ctx, name);
  return r;
}

static JSValue cred_get_temp_dir(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  char buf[4096];
  size_t len = sizeof(buf);
  if (uv_os_tmpdir(buf, &len) != 0)
    return JS_UNDEFINED;
  return JS_NewString(ctx, buf);
}

static JSValue cred_id(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv, int magic) {
  switch (magic) {
  case 0: return JS_NewUint32(ctx, getuid());
  case 1: return JS_NewUint32(ctx, geteuid());
  case 2: return JS_NewUint32(ctx, getgid());
  default: return JS_NewUint32(ctx, getegid());
  }
}

static JSValue cred_getgroups(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  gid_t groups[256];
  int n = getgroups(256, groups), i;
  JSValue arr = JS_NewArray(ctx);
  gid_t egid = getegid();
  bool seen = false;
  if (n < 0)
    n = 0;
  for (i = 0; i < n; i++) {
    JS_SetPropertyUint32(ctx, arr, i, JS_NewUint32(ctx, groups[i]));
    if (groups[i] == egid)
      seen = true;
  }
  if (!seen)
    JS_SetPropertyUint32(ctx, arr, n, JS_NewUint32(ctx, egid));
  return arr;
}

static int resolve_id(JSContext *ctx, JSValueConst v, bool group, uint32_t *out) {
  if (JS_IsNumber(v))
    return JS_ToUint32(ctx, out, v);
  {
    const char *name = JS_ToCString(ctx, v);
    int ok = -1;
    if (!name)
      return -1;
    if (group) {
      struct group *g = getgrnam(name);
      if (g) { *out = g->gr_gid; ok = 0; }
    } else {
      struct passwd *p = getpwnam(name);
      if (p) { *out = p->pw_uid; ok = 0; }
    }
    JS_FreeCString(ctx, name);
    return ok == 0 ? 0 : 1;
  }
}

static JSValue cred_set(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv, int magic) {
  uint32_t id;
  int r = resolve_id(ctx, ARG(0), magic >= 2, &id), err;
  if (r < 0)
    return JS_EXCEPTION;
  if (r > 0)
    return JS_NewInt32(ctx, 1); /* unknown name: JS throws ERR_UNKNOWN_CREDENTIAL */
  switch (magic) {
  case 0: err = setuid(id); break;
  case 1: err = seteuid(id); break;
  case 2: err = setgid(id); break;
  default: err = setegid(id); break;
  }
  if (err)
    return JS_Throw(ctx, node_errno_exception(ctx, errno, magic == 0 ? "setuid" :
                    magic == 1 ? "seteuid" : magic == 2 ? "setgid" : "setegid", NULL, NULL));
  return JS_NewInt32(ctx, 0);
}

static JSValue cred_setgroups(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  JSValue lenv = JS_GetPropertyStr(ctx, ARG(0), "length");
  uint32_t n = 0, i;
  gid_t *g;
  JS_ToUint32(ctx, &n, lenv);
  g = calloc(n + 1, sizeof(gid_t));
  for (i = 0; i < n; i++) {
    JSValue v = JS_GetPropertyUint32(ctx, ARG(0), i);
    uint32_t id;
    int r = resolve_id(ctx, v, true, &id);
    JS_FreeValue(ctx, v);
    if (r != 0) {
      free(g);
      if (r < 0)
        return JS_EXCEPTION;
      return JS_NewInt32(ctx, i + 1);
    }
    g[i] = id;
  }
  if (setgroups(n, g)) {
    free(g);
    return JS_Throw(ctx, node_errno_exception(ctx, errno, "setgroups", NULL, NULL));
  }
  free(g);
  return JS_NewInt32(ctx, 0);
}

static JSValue cred_initgroups(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  return JS_NewInt32(ctx, 0);
}

JSValue binding_init_credentials(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  static const JSCFunctionListEntry funcs[] = {
    NB_FUNC_MAGIC("getuid", 0, cred_id, 0),
    NB_FUNC_MAGIC("geteuid", 0, cred_id, 1),
    NB_FUNC_MAGIC("getgid", 0, cred_id, 2),
    NB_FUNC_MAGIC("getegid", 0, cred_id, 3),
    NB_FUNC_MAGIC("setuid", 1, cred_set, 0),
    NB_FUNC_MAGIC("seteuid", 1, cred_set, 1),
    NB_FUNC_MAGIC("setgid", 1, cred_set, 2),
    NB_FUNC_MAGIC("setegid", 1, cred_set, 3),
  };
  JS_SetPropertyFunctionList(ctx, t, funcs, countof(funcs));
  nb_set_method(ctx, t, "safeGetenv", cred_safe_getenv, 1);
  nb_set_method(ctx, t, "getTempDir", cred_get_temp_dir, 0);
  nb_set_method(ctx, t, "getgroups", cred_getgroups, 0);
  nb_set_method(ctx, t, "setgroups", cred_setgroups, 1);
  nb_set_method(ctx, t, "initgroups", cred_initgroups, 2);
  nb_set_bool(ctx, t, "implementsPosixCredentials", true);
  return t;
}

/* ---- trace_events: tracing is not built in ---- */

static JSValue noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_UNDEFINED;
}
static JSValue ret_false(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_FALSE;
}
static JSValue ret_true(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_TRUE;
}
static JSValue ret_empty_string(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  return JS_NewString(ctx, "");
}
static JSValue ret_zero(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_NewInt32(ctx, 0);
}

static uint8_t trace_category_disabled[8];
static JSValue trace_get_category_enabled_buffer(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
  return nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT8, trace_category_disabled, 1, 1);
}

static JSClassID category_set_class_id;
static JSValue category_set_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                                 JSValueConst *argv) {
  return nb_new_instance(ctx, new_target, category_set_class_id);
}
static const JSCFunctionListEntry category_set_proto[] = {
  JS_CFUNC_DEF("enable", 0, noop),
  JS_CFUNC_DEF("disable", 0, noop),
};

JSValue binding_init_trace_events(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  NodeClassDef def = { .name = "CategorySet", .class_id = &category_set_class_id,
                       .ctor = category_set_ctor, .ctor_length = 1,
                       .proto_funcs = category_set_proto,
                       .proto_funcs_count = countof(category_set_proto),
                       .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  nb_set_method(ctx, t, "trace", noop, 5);
  nb_set_method(ctx, t, "isTraceCategoryEnabled", ret_false, 1);
  nb_set_method(ctx, t, "setTraceCategoryStateUpdateHandler", noop, 1);
  nb_set_method(ctx, t, "getCategoryEnabledBuffer", trace_get_category_enabled_buffer, 1);
  nb_set_method(ctx, t, "getEnabledCategories", ret_empty_string, 0);
  nb_set_method(ctx, t, "setLevel", noop, 1);
  return t;
}

/* ---- async_context_frame ---- */

static JSValue acf_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_GetContinuationPreservedData(ctx);
}
static JSValue acf_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JS_SetContinuationPreservedData(ctx, JS_DupValue(ctx, ARG(0)));
  return JS_UNDEFINED;
}

JSValue binding_init_async_context_frame(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "getContinuationPreservedEmbedderData", acf_get, 0);
  nb_set_method(ctx, t, "setContinuationPreservedEmbedderData", acf_set, 1);
  return t;
}

/* ---- mksnapshot: no snapshots ---- */

static uint8_t is_building_snapshot[4];
JSValue binding_init_mksnapshot(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set(ctx, t, "isBuildingSnapshotBuffer",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT8, is_building_snapshot, 1, 1));
  nb_set_method(ctx, t, "setSerializeCallback", noop, 1);
  nb_set_method(ctx, t, "setDeserializeCallback", noop, 1);
  nb_set_method(ctx, t, "setDeserializeMainFunction", noop, 1);
  nb_set_method(ctx, t, "runEmbedderPreload", noop, 2);
  nb_set_method(ctx, t, "compileSerializeMain", noop, 2);
  nb_set_str(ctx, t, "anonymousMainPath", "__node_anonymous_main");
  return t;
}

/* ---- permission: the model is off ---- */

JSValue binding_init_permission(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "has", ret_true, 2);
  nb_set_method(ctx, t, "drop", noop, 2);
  return t;
}

/* ---- inspector: not built in ---- */

JSValue binding_init_inspector(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "isEnabled", ret_false, 0);
  nb_set_method(ctx, t, "waitForDebugger", ret_false, 0);
  nb_set_method(ctx, t, "url", noop, 0);
  nb_set_method(ctx, t, "setConsoleExtensionInstaller", noop, 1);
  nb_set_method(ctx, t, "registerAsyncHook", noop, 2);
  nb_set_method(ctx, t, "emitProtocolEvent", noop, 2);
  nb_set_method(ctx, t, "setupNetworkTracking", noop, 2);
  nb_set_method(ctx, t, "consoleCall", noop, 2);
  return t;
}

/* ---- wasm_web_api ---- */

JSValue binding_init_wasm_web_api(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "setImplementation", noop, 1);
  return t;
}

/* ---- diagnostics_channel: no native channels publish here ---- */

JSValue binding_init_diagnostics_channel(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "linkNativeChannel", noop, 1);
  nb_set(ctx, t, "subscribers",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32,
                            env_scratch(env, "dc_subscribers", 16 * 4), 16, 4));
  return t;
}

/* ---- performance ---- */

enum { MS_TIME_ORIGIN_TIMESTAMP, MS_TIME_ORIGIN, MS_ENVIRONMENT, MS_NODE_START,
       MS_V8_START, MS_LOOP_START, MS_LOOP_EXIT, MS_BOOTSTRAP_COMPLETE, MS_COUNT };
/* per environment: a worker has its own time origin and milestones */
typedef struct PerfState {
  double milestones[MS_COUNT];
  uint32_t observers[8];
  uint64_t start;
  bool init;
} PerfState;

static PerfState *perf_of(Env *env) {
  return env_scratch(env, "perf", sizeof(PerfState));
}

void node_perf_init(Env *env, uint64_t start_ns, double origin_timestamp_us) {
  PerfState *ps = perf_of(env);
  double *perf_milestones = ps->milestones;
  int i;
  ps->init = true;
  for (i = 0; i < MS_COUNT; i++)
    perf_milestones[i] = -1;
  ps->start = start_ns;
  perf_milestones[MS_TIME_ORIGIN_TIMESTAMP] = origin_timestamp_us;
  perf_milestones[MS_TIME_ORIGIN] = (double)start_ns;
  perf_milestones[MS_NODE_START] = (double)start_ns;
  perf_milestones[MS_V8_START] = (double)start_ns;
  perf_milestones[MS_ENVIRONMENT] = (double)uv_hrtime();
}

void node_perf_mark(Env *env, int milestone) {
  perf_of(env)->milestones[milestone] = (double)uv_hrtime();
}

void node_perf_mark_loop_start(Env *env) {
  node_perf_mark(env, MS_LOOP_START);
}

static JSValue perf_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_NewFloat64(ctx, (double)(uv_hrtime() - perf_of(env_get(ctx))->start) / 1e6);
}

static JSValue perf_mark_bootstrap_complete(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
  node_perf_mark(env_get(ctx), MS_BOOTSTRAP_COMPLETE);
  return JS_UNDEFINED;
}

static JSValue perf_loop_idle_time(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Env *env = env_get(ctx);
  return JS_NewFloat64(ctx, (double)uv_metrics_idle_time(env->loop) / 1e6);
}

static JSValue perf_uv_metrics_info(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  Env *env = env_get(ctx);
  uv_metrics_t m;
  JSValue arr = JS_NewArray(ctx);
  uv_metrics_info(env->loop, &m);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, (double)m.loop_count));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, (double)m.events));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, (double)m.events_waiting));
  return arr;
}

JSValue node_histogram_class(Env *env, JSValueConst target); /* histogram.c */
JSValue node_create_eld_histogram(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv);

JSValue binding_init_performance(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), c = JS_NewObject(ctx);
  static const char *const entry_types[] = { "GC", "HTTP", "HTTP2", "NET", "DNS", "QUIC" };
  static const char *const milestone_names[] = {
    "TIME_ORIGIN_TIMESTAMP", "TIME_ORIGIN", "ENVIRONMENT", "NODE_START", "V8_START",
    "LOOP_START", "LOOP_EXIT", "BOOTSTRAP_COMPLETE" };
  char name[96];
  size_t i;
  nb_set(ctx, t, "observerCounts",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32, perf_of(env)->observers, 8, 4));
  nb_set(ctx, t, "milestones",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, perf_of(env)->milestones, MS_COUNT, 8));
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_MAJOR", 4);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_MINOR", 1);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_MINOR_MARK_SWEEP", 2);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_INCREMENTAL", 8);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_WEAKCB", 16);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_FLAGS_NO", 0);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED", 2);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_FLAGS_FORCED", 4);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING", 8);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE", 16);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY", 32);
  nb_set_int(ctx, c, "NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE", 64);
  for (i = 0; i < countof(entry_types); i++) {
    snprintf(name, sizeof(name), "NODE_PERFORMANCE_ENTRY_TYPE_%s", entry_types[i]);
    nb_set_int(ctx, c, name, i);
  }
  for (i = 0; i < countof(milestone_names); i++) {
    snprintf(name, sizeof(name), "NODE_PERFORMANCE_MILESTONE_%s", milestone_names[i]);
    nb_set_int(ctx, c, name, i);
  }
  nb_define_readonly(ctx, t, "constants", c);
  nb_set_method(ctx, t, "setupObservers", noop, 1);
  nb_set_method(ctx, t, "installGarbageCollectionTracking", noop, 0);
  nb_set_method(ctx, t, "removeGarbageCollectionTracking", noop, 0);
  nb_set_method(ctx, t, "notify", noop, 2);
  nb_set_method(ctx, t, "loopIdleTime", perf_loop_idle_time, 0);
  nb_set_method(ctx, t, "createELDHistogram", node_create_eld_histogram, 1);
  nb_set_method(ctx, t, "markBootstrapComplete", perf_mark_bootstrap_complete, 0);
  nb_set_method(ctx, t, "uvMetricsInfo", perf_uv_metrics_info, 0);
  nb_set_method(ctx, t, "now", perf_now, 0);
  node_histogram_class(env, t);
  return t;
}

/* ---- heap_utils, v8: what QuickJS can tell ---- */

/* the environment's heapStatisticsBuffer (lib/v8.js calls the update
   function unbound) */
static double *heap_stats_of(Env *env) {
  return env_scratch(env, "heap_stats", 14 * sizeof(double));
}

/* V8's default limit for a 64-bit process; the guest's memory is 4 GiB in all */
#ifdef __wasm__
#define HEAP_LIMIT 2147483648.0
#else
#define HEAP_LIMIT 4345298944.0
#endif

static JSValue heap_stats_into(JSContext *ctx, double *a, size_t len, int kind) {
  JSMemoryUsage mu;
  double limit = node_option_number(env_get(ctx)->options, "--max-old-space-size", 0) * 1048576.0;
  if (limit <= 0)
    limit = HEAP_LIMIT;
  JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mu);
  if (!a)
    return JS_UNDEFINED;
  if (kind == 0 && len >= 14 * 8) {
    /* total_heap_size, total_heap_size_executable, total_physical_size,
       total_available_size, used_heap_size, heap_size_limit, malloced_memory,
       peak_malloced_memory, does_zap_garbage, number_of_native_contexts,
       number_of_detached_contexts, total_global_handles_size,
       used_global_handles_size, external_memory */
    a[0] = (double)mu.malloc_size;
    a[1] = 0;
    a[2] = (double)mu.malloc_size;
    a[3] = limit > (double)mu.malloc_size ? limit - (double)mu.malloc_size : 0;
    a[4] = (double)mu.memory_used_size;
    a[5] = limit;
    a[6] = (double)mu.malloc_size;
    a[7] = (double)mu.malloc_size;
    a[8] = 0;
    a[9] = 1;
    a[10] = 0;
    a[11] = 0;
    a[12] = 0;
    a[13] = 0;
  }
  return JS_UNDEFINED;
}

static JSValue v8_update_heap_statistics(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  return heap_stats_into(ctx, heap_stats_of(env_get(ctx)), 14 * sizeof(double), 0);
}

static JSValue v8_cached_data_version_tag(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  return JS_NewInt32(ctx, 0x51ab5c1); /* fixed: QuickJS bytecode of this build */
}

static JSValue v8_set_flags_from_string(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue v8_set_heap_snapshot_near_heap_limit(JSContext *ctx, JSValueConst this_val,
                                                    int argc, JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue v8_is_string_one_byte(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  JSValue v = JS_DupValue(ctx, ARG(0));
  uint32_t len;
  int wide = 0;
  JS_NodeStringData(ctx, &v, &len, &wide);
  JS_FreeValue(ctx, v);
  return JS_NewBool(ctx, !wide);
}

static JSValue v8_get_cpp_heap_statistics(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "committed_size_bytes", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, o, "resident_size_bytes", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, o, "used_size_bytes", JS_NewInt32(ctx, 0));
  JS_SetPropertyStr(ctx, o, "space_statistics", JS_NewArray(ctx));
  JS_SetPropertyStr(ctx, o, "type_names", JS_NewArray(ctx));
  return o;
}

static JSValue v8_get_hash_seed(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  return JS_NewInt32(ctx, 0);
}

static JSValue gc_profiler_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue proto = JS_GetPropertyStr(ctx, nt, "prototype");
  JSValue o = JS_NewObjectProto(ctx, proto);
  JS_FreeValue(ctx, proto);
  return o;
}

static JSValue gc_profiler_stop(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  return JS_UNDEFINED;
}

JSValue binding_init_v8(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), buf;
  static double heap_code_stats[4], heap_space_stats[5];
  buf = nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, heap_stats_of(env), 14, 8);
  nb_set(ctx, t, "heapStatisticsBuffer", buf);
  nb_set(ctx, t, "heapCodeStatisticsBuffer",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, heap_code_stats, 4, 8));
  nb_set(ctx, t, "heapSpaceStatisticsBuffer",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_FLOAT64, heap_space_stats, 5, 8));
  {
    static const char *const spaces[] = {
      "read_only_space", "new_space", "old_space", "code_space", "shared_space",
      "trusted_space", "shared_trusted_space", "new_large_object_space",
      "large_object_space", "code_large_object_space", "shared_large_object_space",
      "trusted_large_object_space", "shared_trusted_large_object_space" };
    JSValue arr = JS_NewArray(ctx), dl = JS_NewObject(ctx), gp, gpp;
    size_t i;
    for (i = 0; i < countof(spaces); i++)
      JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, spaces[i]));
    nb_set(ctx, t, "kHeapSpaces", arr);
    nb_set_int(ctx, dl, "DETAILED", 0);
    nb_set_int(ctx, dl, "BRIEF", 1);
    nb_set(ctx, t, "detailLevel", dl);
    nb_set_method(ctx, t, "getCppHeapStatistics", v8_get_cpp_heap_statistics, 1);
    nb_set_method(ctx, t, "getHashSeed", v8_get_hash_seed, 0);
    gp = JS_NewCFunction2(ctx, gc_profiler_ctor, "GCProfiler", 0, JS_CFUNC_constructor_or_func, 0);
    gpp = JS_NewObject(ctx);
    nb_set_method(ctx, gpp, "start", gc_profiler_stop, 0);
    nb_set_method(ctx, gpp, "stop", gc_profiler_stop, 0);
    JS_SetConstructor(ctx, gp, gpp);
    JS_FreeValue(ctx, gpp);
    nb_set(ctx, t, "GCProfiler", gp);
  }
  nb_set_method(ctx, t, "updateHeapStatisticsBuffer", v8_update_heap_statistics, 0);
  nb_set_method(ctx, t, "updateHeapCodeStatisticsBuffer", noop, 0);
  nb_set_method(ctx, t, "updateHeapSpaceStatisticsBuffer", noop, 1);
  nb_set_method(ctx, t, "cachedDataVersionTag", v8_cached_data_version_tag, 0);
  nb_set_method(ctx, t, "setFlagsFromString", v8_set_flags_from_string, 1);
  nb_set_method(ctx, t, "setHeapSnapshotNearHeapLimit", v8_set_heap_snapshot_near_heap_limit, 1);
  nb_set_method(ctx, t, "isStringOneByteRepresentation", v8_is_string_one_byte, 1);
  nb_set_method(ctx, t, "startCpuProfile", noop, 0);
  nb_set_method(ctx, t, "stopCpuProfile", noop, 0);
  {
    static const char *const names[] = {
      "kTotalHeapSizeIndex", "kTotalHeapSizeExecutableIndex", "kTotalPhysicalSizeIndex",
      "kTotalAvailableSize", "kUsedHeapSizeIndex", "kHeapSizeLimitIndex",
      "kMallocedMemoryIndex", "kPeakMallocedMemoryIndex", "kDoesZapGarbageIndex",
      "kNumberOfNativeContextsIndex", "kNumberOfDetachedContextsIndex",
      "kTotalGlobalHandlesSizeIndex", "kUsedGlobalHandlesSizeIndex",
      "kExternalMemoryIndex" };
    static const char *const code[] = { "kCodeAndMetadataSizeIndex", "kBytecodeAndMetadataSizeIndex",
                                        "kExternalScriptSourceSizeIndex", "kCPUProfilerMetaDataSizeIndex" };
    static const char *const space[] = { "kSpaceSizeIndex", "kSpaceUsedSizeIndex",
                                         "kSpaceAvailableSizeIndex", "kPhysicalSpaceSizeIndex" };
    size_t i;
    for (i = 0; i < countof(names); i++) nb_set_int(ctx, t, names[i], i);
    for (i = 0; i < countof(code); i++) nb_set_int(ctx, t, code[i], i);
    for (i = 0; i < countof(space); i++) nb_set_int(ctx, t, space[i], i);
  }
  return t;
}

static JSValue heap_utils_unsupported(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  return node_throw_error(ctx, "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM",
                          "Heap snapshots are not available in this build");
}

JSValue binding_init_heap_utils(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "buildEmbedderGraph", heap_utils_unsupported, 0);
  nb_set_method(ctx, t, "triggerHeapSnapshot", heap_utils_unsupported, 1);
  nb_set_method(ctx, t, "createHeapSnapshotStream", heap_utils_unsupported, 1);
  return t;
}

/* ---- report ---- */

static JSValue report_get_report(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"header\":{\"reportVersion\":5,\"nodejsVersion\":\"%s\",\"glibcVersionRuntime\":null,"
           "\"arch\":\"%s\",\"platform\":\"linux\"},\"sharedObjects\":[]}",
           NODE_VERSION_STRING, node_process_arch());
  return JS_NewString(ctx, buf);
}

JSValue binding_init_report(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "writeReport", ret_empty_string, 4);
  nb_set_method(ctx, t, "getReport", report_get_report, 1);
  nb_set_method(ctx, t, "getCompact", ret_false, 0);
  nb_set_method(ctx, t, "setCompact", noop, 1);
  nb_set_method(ctx, t, "getDirectory", ret_empty_string, 0);
  nb_set_method(ctx, t, "setDirectory", noop, 1);
  nb_set_method(ctx, t, "getFilename", ret_empty_string, 0);
  nb_set_method(ctx, t, "setFilename", noop, 1);
  nb_set_method(ctx, t, "getSignal", ret_empty_string, 0);
  nb_set_method(ctx, t, "setSignal", noop, 1);
  nb_set_method(ctx, t, "shouldReportOnFatalError", ret_false, 0);
  nb_set_method(ctx, t, "setReportOnFatalError", noop, 1);
  nb_set_method(ctx, t, "shouldReportOnSignal", ret_false, 0);
  nb_set_method(ctx, t, "setReportOnSignal", noop, 1);
  nb_set_method(ctx, t, "shouldReportOnUncaughtException", ret_false, 0);
  nb_set_method(ctx, t, "setReportOnUncaughtException", noop, 1);
  nb_set_method(ctx, t, "getExcludeNetwork", ret_false, 0);
  nb_set_method(ctx, t, "setExcludeNetwork", noop, 1);
  nb_set_method(ctx, t, "getExcludeEnv", ret_false, 0);
  nb_set_method(ctx, t, "setExcludeEnv", noop, 1);
  return t;
}

/* ---- profiler, sea, watchdog, internal_only_v8 ---- */

JSValue binding_init_profiler(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "setCoverageDirectory", noop, 1);
  nb_set_method(ctx, t, "setSourceMapCacheGetter", noop, 1);
  nb_set_method(ctx, t, "takeCoverage", noop, 0);
  nb_set_method(ctx, t, "stopCoverage", noop, 0);
  return t;
}

static JSValue sea_get_asset(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  return JS_UNDEFINED;
}

JSValue binding_init_sea(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "isSea", ret_false, 0);
  nb_set_method(ctx, t, "isExperimentalSeaWarningNeeded", ret_false, 0);
  nb_set_method(ctx, t, "getAsset", sea_get_asset, 1);
  nb_set_method(ctx, t, "getAssetKeys", noop, 0);
  return t;
}

static JSClassID watchdog_class_id;
static JSValue watchdog_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                             JSValueConst *argv) {
  return nb_new_instance(ctx, new_target, watchdog_class_id);
}
static const JSCFunctionListEntry watchdog_proto[] = {
  JS_CFUNC_DEF("start", 0, ret_true),
  JS_CFUNC_DEF("stop", 0, ret_false),
};

JSValue binding_init_watchdog(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  NodeClassDef def = { .name = "TraceSigintWatchdog", .class_id = &watchdog_class_id,
                       .ctor = watchdog_ctor, .proto_funcs = watchdog_proto,
                       .proto_funcs_count = countof(watchdog_proto),
                       .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  nb_set_method(ctx, t, "startSigintWatchdog", noop, 0);
  nb_set_method(ctx, t, "stopSigintWatchdog", ret_false, 0);
  nb_set_method(ctx, t, "watchdogHasPendingSigint", ret_false, 0);
  return t;
}

JSValue binding_init_internal_only_v8(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "queryObjects", ret_zero, 1);
  return t;
}

/* ---- features this build leaves out ---- */

static JSValue unavailable(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  return node_throw_error(ctx, "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM",
                          "This feature is not available in Node.js for collaboCore");
}

static JSValue empty_binding(Env *env) {
  return JS_NewObject(env->ctx);
}

JSValue binding_init_wasi(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = empty_binding(env);
  nb_set_method(ctx, t, "WASI", unavailable, 0);
  return t;
}

JSValue binding_init_quic(Env *env) { return empty_binding(env); }
JSValue binding_init_webstorage(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = empty_binding(env);
  nb_set_method(ctx, t, "Storage", unavailable, 0);
  return t;
}

