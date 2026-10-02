/* Node.js on QuickJS (collaboCore add-on): shared declarations.
 *
 * Node's own lib/ runs unchanged on top of these: each internalBinding() of
 * Node's C++ src/ has a C counterpart here, written against QuickJS and libuv.
 */
#ifndef COLLABO_NODE_H
#define COLLABO_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "quickjs.h"
#include "uv.h"

#define NODE_VERSION_STRING "v24.21.0"
#define NODE_MAJOR 24
#define NODE_MINOR 21
#define NODE_PATCH 0

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define ARG(i) ((i) < argc ? argv[i] : JS_UNDEFINED)
/* binding methods are enumerable, as V8's SetMethod makes them */
#define NB_FUNC(name, length, func1) JS_CFUNC_DEF2(name, length, func1, JS_PROP_C_W_E)
#define NB_FUNC_MAGIC(name, length, func1, magic)                               \
  { name, JS_PROP_C_W_E, JS_DEF_CFUNC, magic,                                   \
    { .func = { length, JS_CFUNC_generic_magic, { .generic_magic = func1 } } } }

/* AsyncWrap provider types, in Node's order (async_wrap.h) */
#define NODE_ASYNC_PROVIDER_TYPES(V)                                           \
  V(NONE) V(DIRHANDLE) V(DNSCHANNEL) V(ELDHISTOGRAM) V(FILEHANDLE)             \
  V(FILEHANDLECLOSEREQ) V(BLOBREADER) V(FSEVENTWRAP) V(FSREQCALLBACK)          \
  V(FSREQPROMISE) V(GETADDRINFOREQWRAP) V(GETNAMEINFOREQWRAP) V(HEAPSNAPSHOT)  \
  V(HTTP2SESSION) V(HTTP2STREAM) V(HTTP2PING) V(HTTP2SETTINGS)                 \
  V(HTTPINCOMINGMESSAGE) V(HTTPCLIENTREQUEST) V(LOCKS) V(JSSTREAM)             \
  V(JSUDPWRAP) V(MESSAGEPORT) V(PIPECONNECTWRAP) V(PIPESERVERWRAP) V(PIPEWRAP) \
  V(PROCESSWRAP) V(PROMISE) V(QUERYWRAP) V(QUIC_ENDPOINT) V(QUIC_LOGSTREAM)    \
  V(QUIC_SESSION) V(QUIC_STREAM) V(QUIC_UDP) V(SHUTDOWNWRAP) V(SIGNALWRAP)     \
  V(STATWATCHER) V(STREAMPIPE) V(TCPCONNECTWRAP) V(TCPSERVERWRAP) V(TCPWRAP)   \
  V(TTYWRAP) V(UDPSENDWRAP) V(UDPWRAP) V(SIGINTWATCHDOG) V(WORKER)             \
  V(WORKERCPUPROFILE) V(WORKERCPUUSAGE) V(WORKERHEAPPROFILE)                   \
  V(WORKERHEAPSNAPSHOT) V(WORKERHEAPSTATISTICS) V(WRITEWRAP) V(ZLIB)           \
  V(CHECKPRIMEREQUEST) V(PBKDF2REQUEST) V(KEYPAIRGENREQUEST) V(KEYGENREQUEST)  \
  V(KEYEXPORTREQUEST) V(ARGON2REQUEST) V(CIPHERREQUEST) V(DERIVEBITSREQUEST)   \
  V(HASHREQUEST) V(RANDOMBYTESREQUEST) V(RANDOMPRIMEREQUEST) V(SCRYPTREQUEST)  \
  V(SIGNREQUEST) V(TLSWRAP) V(VERIFYREQUEST)

typedef enum {
#define V(p) PROVIDER_##p,
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
  PROVIDERS_LENGTH
} ProviderType;

extern const char *const node_provider_names[PROVIDERS_LENGTH];

/* AsyncHooks fields (env.h) */
enum { kInit, kBefore, kAfter, kDestroy, kPromiseResolve, kTotals, kCheck,
       kStackLength, kUsesExecutionAsyncResource, kAsyncHookFieldsCount };
enum { kExecutionAsyncId, kTriggerAsyncId, kAsyncIdCounter,
       kDefaultTriggerAsyncId, kUidFieldsCount };
/* TickInfo, ImmediateInfo, exit info */
enum { kHasTickScheduled, kHasRejectionToWarn, kTickFieldsCount };
enum { kImmCount, kImmRefCount, kImmHasOutstanding, kImmFieldsCount };
enum { kExiting, kExitCode, kHasExitCode, kExitInfoFieldsCount };
/* StreamBase state */
enum { kReadBytesOrError, kArrayBufferOffset, kBytesWritten,
       kLastWriteWasAsync, kNumStreamBaseStateFields };

/* host defined option ids (QuickJS bytecode carries the id; the symbol is
   looked up in Env.host_defined_symbols) */
enum {
  HDO_NONE = 0,
  HDO_VM_DYNAMIC_IMPORT_DEFAULT_INTERNAL = 1,
  HDO_SOURCE_TEXT_MODULE_DEFAULT = 2,
  HDO_VM_DYNAMIC_IMPORT_MAIN_CONTEXT_DEFAULT = 3,
  HDO_VM_DYNAMIC_IMPORT_MISSING_FLAG = 4,
  HDO_VM_DYNAMIC_IMPORT_NO_CALLBACK = 5,
  HDO_BUILTIN_SOURCE_TEXT_MODULE = 6,
  HDO_FIRST_DYNAMIC = 16,
};

typedef struct Env Env;
typedef struct NodeOptions NodeOptions;

/* the receiver of a native object: its JS object (not owned) and async ids */
typedef struct AsyncWrap {
  Env *env;
  JSValue object;        /* the JS object this wraps (weak, not dup'ed) */
  JSValue context_frame; /* AsyncContextFrame at creation (owned) */
  double async_id;
  double trigger_async_id;
  ProviderType provider;
  int strong;            /* strong references held while active */
} AsyncWrap;

typedef struct Env {
  JSRuntime *rt;
  JSContext *ctx;
  uv_loop_t *loop;
  bool is_main_thread;
  bool owns_process_state;
  int thread_id;

  /* lifecycle */
  bool can_call_into_js;
  bool stopping;
  bool exiting;
  int callback_scope_depth;
  int exit_code_default;

  /* JS state (owned references) */
  JSValue global;
  JSValue process;
  JSValue primordials;
  JSValue per_context_exports;
  JSValue private_symbols;
  JSValue per_isolate_symbols;
  JSValue internal_binding_loader;
  JSValue builtin_require;
  JSValue binding_cache;        /* name -> binding object */
  JSValue tick_callback;
  JSValue promise_reject_callback;
  JSValue prepare_stack_trace_cb;
  JSValue enhance_stack_before_inspector;
  JSValue enhance_stack_after_inspector;
  JSValue source_maps_enabled;   /* bool */
  JSValue get_source_map_error_source;
  JSValue maybe_cache_generated_source_map;
  JSValue async_hooks_binding;
  JSValue async_hooks_init, async_hooks_before, async_hooks_after,
      async_hooks_destroy, async_hooks_promise_resolve;
  JSValue async_callback_trampoline;
  JSValue timers_callback;       /* processTimers */
  JSValue immediate_callback;    /* processImmediate */
  JSValue import_module_dynamically;
  JSValue initialize_import_meta;
  JSValue host_defined_symbols;  /* array: id -> symbol */
  int32_t next_host_defined_id;
  JSValue module_wrap_class_proto;
  JSValue emit_warning_sync;
  JSValue buffer_prototype;
  JSValue messaging_deserialize_create_object;
  JSValue process_emit_warning;
  JSValue binding_data;          /* state bindings keep, by name */

  /* shared with JS through typed arrays */
  int32_t *tick_info;
  uint32_t *immediate_info;
  int32_t *timeout_info;
  uint32_t *async_hook_fields;
  double *async_id_fields;
  double *async_ids_stack;
  uint32_t async_ids_stack_len;  /* in doubles */
  JSValue execution_async_resources; /* JS array (owned) */
  JSValue *native_resources;     /* C stack of resources (owned) */
  uint32_t native_resources_len, native_resources_cap;
  int32_t *stream_base_state;
  int32_t *exit_info;
  uint32_t *hrtime_buffer;       /* 3 x uint32 */
  uint8_t *fs_stat_values_backing;
  double *fs_stat_values;        /* 2 * kFsStatsFieldsNumber (+ statfs) */
  int64_t *fs_bigint_stat_values;
  double *fs_statfs_values;
  int64_t *fs_bigint_statfs_values;
  uint8_t *zero_fill_field;

  /* destroy ids waiting to be emitted */
  double *destroy_ids;
  uint32_t destroy_ids_len, destroy_ids_cap;
  bool destroy_ids_scheduled;
  uv_idle_t destroy_idle;

  /* timers and immediates */
  uv_timer_t timer_handle;
  uv_check_t immediate_check;
  uv_idle_t immediate_idle;
  int64_t timer_base;

  /* native handles to close at exit */
  void *handle_list;             /* struct HandleWrap list (handles.c) */
  void *cleanup_hooks;

  NodeOptions *options;
  char *exec_path;
  int argc;
  char **argv;
  int exec_argc;
  char **exec_argv;
  uint64_t start_time_ns;
  double time_origin;            /* performance.timeOrigin (ms) */
  uint64_t time_origin_ns;
  void *worker;                  /* Worker owning this env (worker_threads) */

  /* per-environment state of the bindings (what Node keeps per realm) */
  void *scratch;                 /* env_scratch() blocks */
  JSValue promise_hook_fns[4];   /* init, before, after, settled */
  bool promise_hooks_set;
  void *fsd;                     /* fs binding data (b_fs.c) */
  int fatal_depth;               /* in TriggerUncaughtException */
  void *env_store;               /* process.env of a worker (NULL: environ) */
} Env;

static inline Env *env_get(JSContext *ctx) { return (Env *)JS_GetContextOpaque(ctx); }

/* ---- env.c ---- */
/* process.env stores (process_object.c): a worker's own variables */
void *node_env_store_copy(Env *env);
void *node_env_store_from(char **kv, int n);
void *node_env_store_ref(Env *env);
void node_env_store_unref(void *store);
/* compile_cache.c */
JSValue node_cached_compile(JSContext *ctx, const char *src, size_t len, JSEvalOptions *opts);
int node_compile_cache_enable(const char *dir, char **message, char **directory);
const char *node_compile_cache_dir(void);
void node_compile_cache_init(void);
JSRuntime *node_new_runtime(size_t stack_size);
JSRuntime *node_new_runtime2(size_t stack_size, const JSMallocFunctions *mf, void *opaque);
/* handles.c: what env has open, and closing it all */
void *node_handle_track_fn(AsyncWrap *aw, uv_handle_t *handle, void (*close)(AsyncWrap *aw));
void node_handle_untrack(void *entry);
void node_handles_close_all(Env *env);
void node_sab_dup(void *opaque, void *ptr);   /* SharedArrayBuffer memory refs */
void node_sab_free(void *opaque, void *ptr);
Env *env_new(JSRuntime *rt, JSContext *ctx, uv_loop_t *loop, bool is_main);
void env_free(Env *env);
void env_free_native(Env *env);
int env_bootstrap(Env *env);          /* per-context, realm, node bootstraps */
int env_start_execution(Env *env, const char *main_script); /* internal/main/... */
int env_spin_event_loop(Env *env);    /* returns the exit code */
int env_exit_code(Env *env, int default_code);
void env_stop(Env *env);
void env_run_cleanup(Env *env);
JSValue env_get_symbol(Env *env, const char *name); /* per-isolate symbol (dup) */
JSValue env_get_private_symbol(Env *env, const char *name);
JSValue env_binding_data(Env *env, const char *name); /* object per name (dup) */
/* a zeroed block of size bytes kept per environment under name (the same
   block on every call), freed with the environment: for the arrays bindings
   share with JS, which must not be shared between worker threads */
void *env_scratch(Env *env, const char *name, size_t size);

/* calling JS from native code (InternalMakeCallback): returns the result,
   JS_EXCEPTION when it threw into JS that called us, or NODE_CB_FAILED when
   the exception was reported as uncaught (nothing is pending then) */
#define NODE_CB_FAILED JS_UNINITIALIZED
JSValue node_make_callback(Env *env, JSValueConst resource, JSValueConst recv,
                           JSValueConst fn, int argc, JSValueConst *argv,
                           double async_id, double trigger_async_id,
                           JSValueConst context_frame);
/* calls fn with the wrap's async context; on an uncaught exception at the top
   level, reports it; returns the result or JS_EXCEPTION */
JSValue async_wrap_make_callback(AsyncWrap *w, JSValueConst fn, int argc,
                                 JSValueConst *argv);
/* looks up obj[name] and calls it as above (undefined if not a function) */
JSValue async_wrap_make_callback_name(AsyncWrap *w, const char *name, int argc,
                                      JSValueConst *argv);
void node_run_microtasks(Env *env);
void node_trigger_uncaught_exception(Env *env, JSValue err, bool from_promise);
void node_report_exception(Env *env, JSValueConst err); /* print, no exit */
int node_process_emit(Env *env, const char *event, JSValueConst arg);
void node_emit_process_warning(Env *env, const char *msg, const char *type, const char *code);
void env_internal_callback_scope_enter(Env *env);
int env_internal_callback_scope_exit(Env *env, bool failed);

/* AsyncWrap */
void async_wrap_init(AsyncWrap *w, Env *env, JSValueConst obj, ProviderType provider,
                     double trigger_async_id /* -1: default */);
void async_wrap_destroy(AsyncWrap *w);
void async_wrap_reset(AsyncWrap *w, JSValueConst resource);   /* queue destroy hook, free frame */
void async_wrap_ref(AsyncWrap *w);       /* keep the JS object alive */
void async_wrap_unref(AsyncWrap *w);
double env_get_default_trigger_async_id(Env *env);
void env_push_async_context(Env *env, double async_id, double trigger_async_id,
                            JSValueConst resource);
bool env_pop_async_context(Env *env, double async_id);
void env_queue_destroy_async_id(Env *env, double async_id);

/* ---- bindings ---- */
typedef JSValue (*BindingInit)(Env *env);
typedef struct { const char *name; BindingInit init; } BindingDef;
JSValue node_get_internal_binding(Env *env, const char *name);
JSValue node_get_linked_binding(Env *env, const char *name);

/* ---- errors.c ---- */
JSValue node_throw_error(JSContext *ctx, const char *code, const char *fmt, ...);
JSValue node_throw_type_error(JSContext *ctx, const char *code, const char *fmt, ...);
JSValue node_throw_range_error(JSContext *ctx, const char *code, const char *fmt, ...);
JSValue node_uv_exception(JSContext *ctx, int errorno, const char *syscall,
                          const char *msg, const char *path, const char *dest);
JSValue node_throw_uv_exception(JSContext *ctx, int errorno, const char *syscall,
                                const char *msg, const char *path, const char *dest);
JSValue node_errno_exception(JSContext *ctx, int errorno, const char *syscall,
                             const char *msg, const char *path);
const char *node_uv_errname(int err);
JSValue node_prepare_stack_trace(JSContext *ctx, JSValueConst error,
                                 JSValueConst callsites, void *opaque);
void node_print_uncaught(Env *env, JSValueConst err, bool from_promise);

/* ---- util.c: small helpers ---- */
void nb_set_method(JSContext *ctx, JSValueConst obj, const char *name,
                   JSCFunction *fn, int length);
void nb_set(JSContext *ctx, JSValueConst obj, const char *name, JSValue v);
void nb_set_int(JSContext *ctx, JSValueConst obj, const char *name, int64_t v);
void nb_set_double(JSContext *ctx, JSValueConst obj, const char *name, double v);
void nb_set_str(JSContext *ctx, JSValueConst obj, const char *name, const char *v);
void nb_set_bool(JSContext *ctx, JSValueConst obj, const char *name, bool v);
void nb_define_readonly(JSContext *ctx, JSValueConst obj, const char *name, JSValue v);
JSValue nb_new_typed_array(JSContext *ctx, JSTypedArrayEnum type, void *data,
                           size_t count, size_t elem_size);
void *nb_alloc_shared_array(JSContext *ctx, JSTypedArrayEnum type, size_t count,
                            size_t elem_size, JSValue *out);
char *nb_to_cstring_dup(JSContext *ctx, JSValueConst v); /* malloc'ed or NULL */
int nb_to_int64(JSContext *ctx, int64_t *pres, JSValueConst v);
int64_t nb_int64(JSContext *ctx, JSValueConst v, int64_t def);
int32_t nb_int32(JSContext *ctx, JSValueConst v, int32_t def);
uint32_t nb_uint32(JSContext *ctx, JSValueConst v, uint32_t def);
double nb_double(JSContext *ctx, JSValueConst v, double def);
bool nb_is_array_buffer_view(JSContext *ctx, JSValueConst v);
/* bytes of an ArrayBuffer, a view or a SharedArrayBuffer (no copy) */
uint8_t *nb_buffer_data(JSContext *ctx, JSValueConst v, size_t *plen);
JSValue nb_new_buffer(JSContext *ctx, const void *data, size_t len); /* FastBuffer */
JSValue nb_new_buffer_owned(JSContext *ctx, void *data, size_t len); /* takes malloc'ed */
JSValue nb_new_array_buffer_copy(JSContext *ctx, const void *data, size_t len);
JSValue nb_new_uint8_array_copy(JSContext *ctx, const void *data, size_t len);
JSValue nb_call(JSContext *ctx, JSValueConst fn, JSValueConst this_val, int argc,
                JSValueConst *argv);
JSValue nb_get(JSContext *ctx, JSValueConst obj, const char *name);
JSValue nb_new_string_utf8(JSContext *ctx, const char *s, size_t len);
JSValue nb_array_from_strings(JSContext *ctx, char **strs, int n);
uint64_t nb_hrtime(void);

/* classes: a native constructor whose instances carry an opaque pointer */
typedef struct {
  const char *name;
  JSClassID *class_id;
  JSCFunction *ctor;            /* called with new_target as this_val */
  int ctor_length;
  JSClassFinalizer *finalizer;
  JSClassGCMark *gc_mark;
  const JSCFunctionListEntry *proto_funcs;
  int proto_funcs_count;
  const JSCFunctionListEntry *static_funcs;
  int static_funcs_count;
  JSValueConst parent_ctor;     /* inherit prototype from this ctor */
} NodeClassDef;
JSValue nb_define_class(JSContext *ctx, JSValueConst target, const NodeClassDef *def);
/* V8 built-ins QuickJS lacks, added to every new context */
void node_context_intrinsics(JSContext *ctx);
void node_wasm_install(JSContext *ctx);
void node_intl_install(JSContext *ctx);
JSValue nb_new_instance(JSContext *ctx, JSValueConst new_target, JSClassID id);
JSValue nb_construct(JSContext *ctx, JSValueConst ctor, int argc, JSValueConst *argv);

/* ---- string encodings (string_bytes.c) ---- */
enum encoding { ENC_ASCII, ENC_UTF8, ENC_BASE64, ENC_UCS2, ENC_BINARY /* latin1 */,
                ENC_HEX, ENC_BUFFER, ENC_BASE64URL, ENC_LATIN1 = ENC_BINARY,
                ENC_UTF16LE = ENC_UCS2 };
int node_parse_encoding(JSContext *ctx, JSValueConst v, int def);
size_t node_string_bytes_size(JSContext *ctx, JSValueConst str, int enc);
/* writes up to cap bytes; returns bytes written; *nchars if wanted */
size_t node_string_write(JSContext *ctx, uint8_t *dst, size_t cap, JSValueConst str,
                         int enc, int *nchars);
JSValue node_string_encode(JSContext *ctx, const uint8_t *buf, size_t len, int enc);
/* utf8 bytes of a string (malloc'ed, NUL-terminated) */
char *node_string_to_utf8(JSContext *ctx, JSValueConst str, size_t *plen);
size_t node_utf8_length(JSContext *ctx, JSValueConst str);
JSValue node_new_utf8_string(JSContext *ctx, const uint8_t *buf, size_t len);

/* ---- builtins.c ---- */
JSValue node_compile_builtin(Env *env, const char *id, bool *found);
int node_builtin_count(void);
const char *node_builtin_id(int i);
const char *node_builtin_source(const char *id, size_t *len); /* NULL unless kept */

/* ---- options.c ---- */
struct NodeOptions {
  int argc;           /* the arguments left for the script */
  char **argv;
  int exec_argc;
  char **exec_argv;
  char *eval_string;
  bool has_eval_string;
  bool print_eval;
  bool force_repl;
  bool syntax_check_only;
  bool interactive;
  bool print_help;
  bool print_version;
  bool print_v8_help;
  bool test_runner;
  bool watch_mode;
  char *run_script;  /* --run */
  JSValue values;    /* the '--option' -> value object (built lazily) */
  JSContext *ctx;    /* the context values and table live in (a worker's own) */
  JSValue table;     /* options.json, parsed in ctx */
};
int node_parse_args(int argc, char **argv, NodeOptions *opts, char **errmsg);
JSValue node_options_values(JSContext *ctx, NodeOptions *opts);
JSValue node_options_info(JSContext *ctx);
const char *node_option_string(NodeOptions *opts, const char *name);
bool node_option_bool(NodeOptions *opts, const char *name);
double node_option_number(NodeOptions *opts, const char *name, double def);
/* for worker_threads: the values as JSON (on the thread that owns opts), and
   options for a new runtime's context from that */
char *node_options_values_json(NodeOptions *opts);
int node_options_init_worker(JSContext *ctx, NodeOptions *opts, const char *values_json);

/* ---- per binding init functions ---- */
#define NODE_BINDINGS(V)                                                       \
  V(async_context_frame) V(async_wrap) V(blob) V(block_list) V(buffer)         \
  V(builtins) V(cares_wrap) V(cjs_lexer) V(config) V(constants)                \
  V(contextify) V(credentials) V(crypto) V(encoding_binding) V(errors) V(fs)   \
  V(fs_dir) V(fs_event_wrap) V(heap_utils) V(http_parser) V(inspector)         \
  V(internal_only_v8) V(js_stream) V(js_udp_wrap) V(locks) V(messaging)        \
  V(mksnapshot) V(module_wrap) V(modules) V(options) V(os) V(performance)      \
  V(permission) V(pipe_wrap) V(process_methods) V(process_wrap) V(profiler)    \
  V(report) V(sea) V(serdes) V(signal_wrap) V(spawn_sync) V(stream_pipe)       \
  V(stream_wrap) V(string_decoder) V(symbols) V(task_queue) V(tcp_wrap)        \
  V(timers) V(tls_wrap) V(trace_events) V(tty_wrap) V(types) V(udp_wrap)       \
  V(url) V(url_pattern) V(util) V(uv) V(v8) V(wasi) V(wasm_web_api)            \
  V(watchdog) V(worker) V(zlib) V(icu) V(sqlite) V(webstorage) V(http2)        \
  V(ipc_serdes) V(diagnostics_channel)

#define V(name) JSValue binding_init_##name(Env *env);
NODE_BINDINGS(V)
#undef V

/* small internal helpers shared between bindings */
JSValue node_buffer_prototype(Env *env);
int node_fs_fill_stats(Env *env, const uv_stat_t *s, bool bigint, int offset);
JSValue node_fs_stats_array(Env *env, bool bigint);
JSValue node_contextify_compile_function(JSContext *ctx, const char *src, size_t len,
                                         const char *filename, int line_offset,
                                         const char **params, int nparams,
                                         int32_t host_defined_id);
void node_timers_init(Env *env);
void node_timers_close(Env *env);
void node_stream_base_init_state(Env *env);
JSValue node_stream_wrap_proto(Env *env); /* LibuvStreamWrap.prototype */
JSValue node_handle_wrap_proto(Env *env); /* HandleWrap methods holder */
int node_process_title_set(const char *title);
const char *node_process_arch(void);
/* uv_close(h) and free(container) when libuv is done with the handle
   (h->data is overwritten) */
void node_close_and_free(uv_handle_t *h, void *container);

#endif
