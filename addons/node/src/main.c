/* node: the program.  Parses the command line, boots Node's lib/ on QuickJS
 * and runs the event loop (node.cc, node_main_instance.cc). */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "node.h"

int node_options_init(JSContext *ctx);
void node_perf_init(Env *env, uint64_t start_ns, double origin_timestamp_us);
void node_perf_mark_loop_start(Env *env);
void node_handles_close_all(Env *env);

/* stack the interpreter may use, in bytes of the C (shadow) stack: QuickJS
   throws RangeError beyond it (NODE_STACK_SIZE overrides, in KiB) */
#ifndef NODE_DEFAULT_STACK_KB
#ifdef __wasm__
/* each check site also reserves 512 bytes there (qjs_l_wasm_stack): about
   950 bytes a JS call, so 12 MiB of the 16 MiB main stack is ~13k calls */
#define NODE_DEFAULT_STACK_KB (12 * 1024)
#else
#define NODE_DEFAULT_STACK_KB (6 * 1024)
#endif
#endif

static void platform_init(void) {
  struct sigaction act;
  int fd;
  /* make sure 0, 1 and 2 are open, so nothing else takes them */
  for (fd = 0; fd <= 2; fd++) {
    struct stat st;
    if (fstat(fd, &st) == 0)
      continue;
    if (open("/dev/null", O_RDWR) != fd)
      abort();
  }
  memset(&act, 0, sizeof(act));
  act.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &act, NULL);
  sigaction(SIGXFSZ, &act, NULL);
  /* unblock everything a parent may have left blocked */
  {
    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, NULL);
  }
}

#ifdef __wasm__
/* The native WebAssembly stack the engine gives a guest thread, from the
   kernel command line (collabo.wasmstack=, a collaboCore engine with deep
   stacks); wasmtime's default otherwise.  Going past it traps and stops the
   whole machine, so the interpreter's own limit must stay below it. */
static size_t guest_wasm_stack(void) {
  static size_t cached;
  char buf[1024], *p;
  int fd;
  ssize_t n;
  if (cached)
    return cached;
  cached = 512 * 1024;
  fd = open("/proc/cmdline", O_RDONLY);
  if (fd < 0)
    return cached;
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return cached;
  buf[n] = 0;
  p = strstr(buf, "collabo.wasmstack=");
  if (p) {
    unsigned long long v = strtoull(p + strlen("collabo.wasmstack="), NULL, 10);
    if (v >= 64 * 1024)
      cached = (size_t)v;
  }
  return cached;
}
#endif

/* NODE_STACK_SIZE (KiB) or V8's --stack-size=KiB, else the default */
static size_t stack_limit(int argc, char **argv) {
  const char *e = getenv("NODE_STACK_SIZE");
  size_t kb = NODE_DEFAULT_STACK_KB, bytes;
  int i;
  if (e && atoi(e) > 0)
    kb = (size_t)atoi(e);
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--"))
      break;
    if (!strncmp(argv[i], "--stack-size=", 13) && atoi(argv[i] + 13) > 0)
      kb = (size_t)atoi(argv[i] + 13);
    else if (argv[i][0] != '-')
      break; /* the script: what follows is its own */
  }
  bytes = kb * 1024;
#ifdef __wasm__
  /* about 0.75 bytes of shadow stack per byte of native stack in the
     interpreter's frames: half leaves room for C-heavy recursion */
  if (bytes > guest_wasm_stack() / 2)
    bytes = guest_wasm_stack() / 2;
#endif
  return bytes;
}

size_t node_stack_limit(void) {
  return stack_limit(0, NULL);
}

/* the interpreter's limit for a worker thread that asks for `want` bytes */
size_t node_worker_stack_limit(size_t want) {
#ifdef __wasm__
  if (want > guest_wasm_stack() / 2)
    want = guest_wasm_stack() / 2;
#endif
  return want;
}

static void print_version(void) {
  printf("%s\n", NODE_VERSION_STRING);
}

static const char *select_main_script(Env *env, NodeOptions *opts) {
  const char *first = env->argc > 1 ? env->argv[1] : NULL;
  if (first && !strcmp(first, "inspect"))
    return "internal/main/inspect";
  if (opts->print_help)
    return "internal/main/print_help";
  if (node_option_bool(opts, "--prof-process"))
    return "internal/main/prof_process";
  if (opts->has_eval_string && !opts->force_repl)
    return "internal/main/eval_string";
  if (opts->syntax_check_only)
    return "internal/main/check_syntax";
  if (opts->test_runner)
    return "internal/main/test_runner";
  if (opts->watch_mode)
    return "internal/main/watch_mode";
  if (first && strcmp(first, "-"))
    return "internal/main/run_main_module";
  if (opts->force_repl || uv_guess_handle(0) == UV_TTY)
    return "internal/main/repl";
  return "internal/main/eval_stdin";
}

/* The add-on's settings from the app (collaboCore writes them to this file
   when it boots a guest with the node add-on): defaults for the environment
   of node and what it runs.  The API key itself normally stays on the host,
   which adds it to https requests for baseUrl's host (replacing whatever key
   the guest sends), so clients here get a placeholder to send. */
#define ADDON_SETTINGS "/etc/collabo/addons/node.json"
static void setenv_default(const char *name, const char *value) {
  if (value && !getenv(name))
    setenv(name, value, 0);
}

static void addon_settings(JSContext *ctx) {
  FILE *f = fopen(ADDON_SETTINGS, "r");
  char *text;
  long len;
  JSValue s, v;
  const char *provider, *key_var, *url_var;
  if (!f)
    return;
  fseek(f, 0, SEEK_END);
  len = ftell(f);
  fseek(f, 0, SEEK_SET);
  text = calloc(1, len > 0 ? len + 1 : 1);
  if (len > 0 && fread(text, 1, len, f) != (size_t)len)
    len = 0;
  fclose(f);
  s = JS_ParseJSON(ctx, text, len > 0 ? len : 0, ADDON_SETTINGS);
  free(text);
  if (!JS_IsObject(s)) {
    if (JS_IsException(s))
      JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, s);
    return;
  }
  /* "env": {"NAME": "value", ...} (or that as a JSON string: --addon-config
     on the engine's command line gives strings) */
  v = JS_GetPropertyStr(ctx, s, "env");
  if (JS_IsString(v)) {
    size_t n;
    const char *j = JS_ToCStringLen(ctx, &n, v);
    JSValue o = j ? JS_ParseJSON(ctx, j, n, "env") : JS_UNDEFINED;
    JS_FreeCString(ctx, j);
    JS_FreeValue(ctx, v);
    if (JS_IsException(o))
      JS_FreeValue(ctx, JS_GetException(ctx));
    v = o;
  }
  if (JS_IsObject(v)) {
    JSPropertyEnum *props;
    uint32_t n, i;
    if (!JS_GetOwnPropertyNames(ctx, &props, &n, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
      for (i = 0; i < n; i++) {
        JSValue x = JS_GetProperty(ctx, v, props[i].atom);
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        const char *value = JS_IsNull(x) || JS_IsUndefined(x) ? NULL : JS_ToCString(ctx, x);
        if (name && *name && !strchr(name, '='))
          setenv_default(name, value);
        JS_FreeCString(ctx, value);
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, x);
      }
      JS_FreePropertyEnum(ctx, props, n);
    }
  }
  JS_FreeValue(ctx, v);
  /* "provider", "baseUrl", "apiKey" (only here when baseUrl is plain http) */
  v = JS_GetPropertyStr(ctx, s, "provider");
  provider = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
  JS_FreeValue(ctx, v);
  if (provider && !strcmp(provider, "openai")) {
    key_var = "OPENAI_API_KEY";
    url_var = "OPENAI_BASE_URL";
  } else {
    key_var = "ANTHROPIC_API_KEY";
    url_var = "ANTHROPIC_BASE_URL";
  }
  JS_FreeCString(ctx, provider);
  v = JS_GetPropertyStr(ctx, s, "baseUrl");
  if (JS_IsString(v)) {
    const char *u = JS_ToCString(ctx, v);
    setenv_default(url_var, u);
    JS_FreeCString(ctx, u);
  }
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, s, "apiKey");
  if (JS_IsString(v)) {
    const char *k = JS_ToCString(ctx, v);
    setenv_default(key_var, k);
    JS_FreeCString(ctx, k);
  } else {
    setenv_default(key_var, "collabo-key-on-host");
  }
  JS_FreeValue(ctx, v);
  JS_FreeValue(ctx, s);
}

bool node_sigint_interrupt(void); /* b_contextify.c */
bool node_vm_timeout_interrupt(void);

static int main_interrupt(JSRuntime *rt, void *opaque) {
  return node_sigint_interrupt() || node_vm_timeout_interrupt();
}

JSValue node_parse_dotenv(JSContext *ctx, const char *src, size_t len); /* b_util.c */

/* --env-file FILE, --env-file=FILE, --env-file-if-exists (node.cc,
   Dotenv::GetDataFromArgs): before the options are parsed, as NODE_OPTIONS
   may come from the file; variables already in the environment win */
static void env_set_defaults(JSContext *ctx, JSValueConst vars) {
  JSPropertyEnum *props;
  uint32_t cnt, k;
  if (JS_GetOwnPropertyNames(ctx, &props, &cnt, vars, JS_GPN_STRING_MASK))
    return;
  for (k = 0; k < cnt; k++) {
    const char *name = JS_AtomToCString(ctx, props[k].atom);
    JSValue v = JS_GetProperty(ctx, vars, props[k].atom);
    const char *value = JS_ToCString(ctx, v);
    if (name && value && *name && !strchr(name, '='))
      setenv(name, value, 0);
    JS_FreeCString(ctx, value);
    JS_FreeValue(ctx, v);
    JS_FreeCString(ctx, name);
  }
  JS_FreePropertyEnum(ctx, props, cnt);
}

static int env_files(JSContext *ctx, int argc, char **argv) {
  JSValue all = JS_NewObject(ctx);  /* a later file's value wins */
  int i;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i], *path = NULL;
    bool optional;
    FILE *f;
    char *buf;
    long n;
    JSValue vars;
    if (!strcmp(a, "--"))
      break;
    optional = !strncmp(a, "--env-file-if-exists", 20);
    if (!strcmp(a, "--env-file") || !strcmp(a, "--env-file-if-exists")) {
      if (i + 1 >= argc)
        break;
      path = argv[++i];
    } else if (!strncmp(a, "--env-file=", 11) || !strncmp(a, "--env-file-if-exists=", 21)) {
      path = strchr(a, '=') + 1;
    } else {
      continue;
    }
    f = fopen(path, "rb");
    if (!f) {
      if (optional) {
        fprintf(stderr, "%s not found. Continuing without it.\n", path);
        continue;
      }
      fprintf(stderr, "%s: %s: not found\n", argv[0], path);
      JS_FreeValue(ctx, all);
      return 9;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n > 0 ? n : 1);
    n = n > 0 ? (long)fread(buf, 1, n, f) : 0;
    fclose(f);
    vars = node_parse_dotenv(ctx, buf, n);
    free(buf);
    if (JS_IsObject(vars)) {
      JSPropertyEnum *props;
      uint32_t cnt, k;
      if (!JS_GetOwnPropertyNames(ctx, &props, &cnt, vars, JS_GPN_STRING_MASK)) {
        for (k = 0; k < cnt; k++)
          JS_SetProperty(ctx, all, props[k].atom, JS_GetProperty(ctx, vars, props[k].atom));
        JS_FreePropertyEnum(ctx, props, cnt);
      }
    } else if (JS_IsException(vars)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, vars);
  }
  env_set_defaults(ctx, all);
  JS_FreeValue(ctx, all);
  return 0;
}

int main(int argc, char **argv) {
  JSRuntime *rt;
  JSContext *ctx;
  NodeOptions opts;
  Env *env;
  char *err = NULL;
  int code;
  char exe[4096];
  size_t exe_len = sizeof(exe);
  uint64_t start = uv_hrtime();
  uv_timeval64_t now;

  argv = uv_setup_args(argc, argv);
  platform_init();
#ifdef __wasm__
  /* The guest cannot run the native helpers programs bundle for x64/arm64
     hosts: Claude Code's vendored ripgrep is one, so point it at the rg on
     PATH unless the environment says otherwise. */
  setenv("USE_BUILTIN_RIPGREP", "0", 0);
#endif

  rt = node_new_runtime(stack_limit(argc, argv));
  JS_SetInterruptHandler(rt, main_interrupt, NULL);
  ctx = JS_NewContext(rt);
  node_context_intrinsics(ctx);
  addon_settings(ctx);
  code = env_files(ctx, argc, argv);
  if (code)
    return code;
  node_compile_cache_init();
  if (node_options_init(ctx) < 0) {
    fprintf(stderr, "node: cannot read the options table\n");
    return 1;
  }
  memset(&opts, 0, sizeof(opts));
  code = node_parse_args(argc, argv, &opts, &err);
  if (code) {
    fprintf(stderr, "%s: %s\n", argv[0], err ? err : "bad option");
    return code;
  }
  if (opts.print_version) {
    print_version();
    return 0;
  }
  if (opts.print_v8_help) {
    printf("Node.js for collaboCore runs on QuickJS: V8 options are accepted and ignored.\n");
    return 0;
  }

  env = env_new(rt, ctx, uv_default_loop(), true);
  env->options = &opts;
  env->argc = opts.argc;
  env->argv = opts.argv;
  env->exec_argc = opts.exec_argc;
  env->exec_argv = opts.exec_argv;
  if (uv_exepath(exe, &exe_len) == 0)
    env->exec_path = strdup(exe);
  else
    env->exec_path = strdup(argv[0]);
  env->start_time_ns = start;
  uv_gettimeofday(&now);
  node_perf_init(env, start, (double)now.tv_sec * 1e6 + now.tv_usec);

  if (env_bootstrap(env) < 0) {
    fflush(stdout);
    return 10;
  }
  if (env_start_execution(env, select_main_script(env, &opts)) < 0 && env->stopping) {
    uv_tty_reset_mode();
    return env_exit_code(env, 1);
  }
  node_perf_mark_loop_start(env);
  code = env_spin_event_loop(env);
  env->can_call_into_js = false;
  uv_tty_reset_mode();
  fflush(stdout);
  fflush(stderr);
  /* like node: no teardown of the heap at exit, only the process's own state */
  exit(code);
}
