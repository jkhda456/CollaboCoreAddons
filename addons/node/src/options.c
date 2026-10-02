/* Node's command line options (src/node_options.cc): the table of options,
 * their types and defaults comes from Node v24 itself (options.json, written
 * from `internalBinding('options')` of the same release), and the parser
 * follows OptionsParser::Parse. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "node.h"

extern const char node_options_json[];

enum { kNoOp, kV8Option, kBoolean, kInteger, kUInteger, kString, kHostPort, kStringList };
enum { kAllowedInEnvvar, kDisallowedInEnvvar };

static JSContext *opt_ctx;
static JSValue opt_table = { 0 };  /* parsed options.json */
static JSValue opt_values;          /* '--name' -> value */

static JSValue table_get(const char *section, const char *name) {
  JSValue sec = JS_GetPropertyStr(opt_ctx, opt_table, section);
  JSValue v = JS_GetPropertyStr(opt_ctx, sec, name);
  JS_FreeValue(opt_ctx, sec);
  return v;
}

static int option_type(const char *name, int *env_setting) {
  JSValue o = table_get("options", name), t, e;
  int32_t type = -1, es = 1;
  if (JS_IsObject(o)) {
    t = JS_GetPropertyStr(opt_ctx, o, "type");
    e = JS_GetPropertyStr(opt_ctx, o, "env");
    JS_ToInt32(opt_ctx, &type, t);
    JS_ToInt32(opt_ctx, &es, e);
  }
  JS_FreeValue(opt_ctx, o);
  if (env_setting)
    *env_setting = es;
  return type;
}

/* V8 flags people pass to node; accepted and ignored here */
static bool is_known_v8_flag(const char *name) {
  static const char *const flags[] = {
    "--max-old-space-size", "--max-semi-space-size", "--stack-size",
    "--expose-gc", "--expose_gc", "--jitless", "--no-opt", "--predictable",
    "--max-heap-size", "--initial-heap-size", "--harmony", "--single-threaded",
    "--no-expose-wasm", "--wasm-staging", "--experimental-wasm-", "--harmony-",
    "--allow-natives-syntax", "--abort-on-uncaught-exception", "--gc-interval",
    "--random-seed", "--hash-seed", "--stack-trace-limit", "--lazy",
    "--no-lazy", "--max-lazy", "--trace-gc", "--perf-basic-prof",
    "--perf-prof", "--interpreted-frames-native-stack", "--disallow-code-generation-from-strings",
    "--enable-etw-stack-walking", "--no-freeze-flags-after-init", "--trace-deopt",
    NULL,
  };
  int i;
  for (i = 0; flags[i]; i++) {
    size_t n = strlen(flags[i]);
    if (flags[i][n - 1] == '-' ? !strncmp(name, flags[i], n) : !strcmp(name, flags[i]))
      return true;
  }
  return false;
}

static void set_value(const char *name, JSValue v) {
  JS_SetPropertyStr(opt_ctx, opt_values, name, v);
}

static void push_list(const char *name, const char *value) {
  JSValue arr = JS_GetPropertyStr(opt_ctx, opt_values, name);
  if (!JS_IsArray(arr)) {
    JS_FreeValue(opt_ctx, arr);
    arr = JS_NewArray(opt_ctx);
    JS_SetPropertyStr(opt_ctx, opt_values, name, JS_DupValue(opt_ctx, arr));
  }
  {
    JSValue len = JS_GetPropertyStr(opt_ctx, arr, "length");
    uint32_t n = 0;
    JS_ToUint32(opt_ctx, &n, len);
    JS_SetPropertyUint32(opt_ctx, arr, n, JS_NewString(opt_ctx, value));
  }
  JS_FreeValue(opt_ctx, arr);
}

typedef struct {
  char **items;
  int n, cap, pos;
  char **synthetic;
  int nsyn;
} ArgList;

static bool args_empty(ArgList *a) {
  return a->nsyn == 0 && a->pos >= a->n;
}
static const char *args_first(ArgList *a) {
  return a->nsyn ? a->synthetic[0] : a->items[a->pos];
}
static char *args_pop(ArgList *a, bool *synthetic) {
  if (a->nsyn) {
    char *s = a->synthetic[0];
    memmove(a->synthetic, a->synthetic + 1, (a->nsyn - 1) * sizeof(char *));
    a->nsyn--;
    *synthetic = true;
    return s;
  }
  *synthetic = false;
  return a->items[a->pos++];
}

static char *errbuf(const char *fmt, const char *arg) {
  size_t n = strlen(fmt) + strlen(arg) + 1;
  char *s = malloc(n);
  snprintf(s, n, fmt, arg);
  return s;
}

/* parses the options in args (from args->pos); exec arguments consumed are
   appended to exec_args; returns 0 or -1 with *err set */
static int parse_options(ArgList *args, char ***exec_args, int *nexec, bool from_env,
                         char **err) {
  while (!args_empty(args)) {
    const char *first = args_first(args);
    char *arg, *name, *eq, *value = NULL;
    bool synthetic, is_negation = false;
    int type, env_setting, i;
    if (strlen(first) <= 1 || first[0] != '-')
      break;
    arg = strdup(args_pop(args, &synthetic));
    if (!synthetic && exec_args && strcmp(arg, "--")) {  /* "--" is not in execArgv */
      *exec_args = realloc(*exec_args, (*nexec + 2) * sizeof(char *));
      (*exec_args)[(*nexec)++] = strdup(arg);
    }
    if (!strcmp(arg, "--")) {
      free(arg);
      if (from_env) {
        *err = strdup("-- is not allowed in NODE_OPTIONS");
        return -1;
      }
      break;
    }
    eq = (arg[0] == '-' && arg[1] == '-') ? strchr(arg, '=') : NULL;
    name = eq ? strndup(arg, eq - arg) : strdup(arg);
    for (i = 2; name[i]; i++)
      if (name[i] == '_')
        name[i] = '-';
    if (!strncmp(name, "--no-", 5)) {
      memmove(name + 2, name + 5, strlen(name + 5) + 1);
      is_negation = true;
    }
    /* aliases */
    for (;;) {
      JSValue exp = table_get("aliases", name);
      char key[512];
      if (!JS_IsArray(exp) && eq) {
        JS_FreeValue(opt_ctx, exp);
        snprintf(key, sizeof(key), "%s=", name);
        exp = table_get("aliases", key);
      }
      if (!JS_IsArray(exp) && !args_empty(args) && args_first(args)[0] &&
          args_first(args)[0] != '-') {
        JS_FreeValue(opt_ctx, exp);
        snprintf(key, sizeof(key), "%s <arg>", name);
        exp = table_get("aliases", key);
      }
      if (!JS_IsArray(exp)) {
        JS_FreeValue(opt_ctx, exp);
        break;
      }
      {
        JSValue lenv = JS_GetPropertyStr(opt_ctx, exp, "length"), f;
        uint32_t len = 0, k;
        const char *fs;
        char *prev = name;
        JS_ToUint32(opt_ctx, &len, lenv);
        f = JS_GetPropertyUint32(opt_ctx, exp, 0);
        fs = JS_ToCString(opt_ctx, f);
        name = strdup(fs);
        JS_FreeCString(opt_ctx, fs);
        JS_FreeValue(opt_ctx, f);
        if (len > 1) {
          args->synthetic = realloc(args->synthetic, (args->nsyn + len) * sizeof(char *));
          memmove(args->synthetic + len - 1, args->synthetic, args->nsyn * sizeof(char *));
          for (k = 1; k < len; k++) {
            JSValue x = JS_GetPropertyUint32(opt_ctx, exp, k);
            const char *xs = JS_ToCString(opt_ctx, x);
            args->synthetic[k - 1] = strdup(xs);
            JS_FreeCString(opt_ctx, xs);
            JS_FreeValue(opt_ctx, x);
          }
          args->nsyn += len - 1;
        }
        JS_FreeValue(opt_ctx, exp);
        if (!strcmp(name, prev)) {
          free(prev);
          break;
        }
        free(prev);
      }
    }
    type = option_type(name, &env_setting);
    if (from_env && (type < 0 || env_setting == kDisallowedInEnvvar)) {
      *err = errbuf("%s is not allowed in NODE_OPTIONS", arg);
      free(name);
      free(arg);
      return -1;
    }
    if (type < 0) {
      if (is_known_v8_flag(arg) || is_known_v8_flag(name)) {
        if (!strcmp(name, "--expose-gc") || !strcmp(name, "--expose_gc"))
          set_value("[expose-gc]", JS_TRUE);
        free(name);
        free(arg);
        continue;
      }
      *err = errbuf("bad option: %s", arg);
      free(name);
      free(arg);
      return -1;
    }
    if (type == kV8Option && !is_negation &&
        (!strcmp(name, "--expose-gc") || !strcmp(name, "--expose_gc")))
      set_value("[expose-gc]", JS_TRUE);
    if (is_negation && type != kBoolean && type != kV8Option) {
      *err = errbuf("%s is an invalid negation because it is not a boolean option", arg);
      free(name);
      free(arg);
      return -1;
    }
    if (type != kBoolean && type != kNoOp && type != kV8Option) {
      if (eq) {
        value = strdup(eq + 1);
        if (!value[0]) {
          *err = errbuf("%s requires an argument", name);
          free(value);
          free(name);
          free(arg);
          return -1;
        }
      } else {
        char *v;
        if (args_empty(args)) {
          *err = errbuf("%s requires an argument", name);
          free(name);
          free(arg);
          return -1;
        }
        v = args_pop(args, &synthetic);
        if (!synthetic && exec_args) {
          *exec_args = realloc(*exec_args, (*nexec + 2) * sizeof(char *));
          (*exec_args)[(*nexec)++] = strdup(v);
        }
        if (v[0] == '-') {
          *err = errbuf("%s requires an argument", name);
          free(name);
          free(arg);
          return -1;
        }
        value = strdup(v[0] == '\\' && v[1] == '-' ? v + 1 : v);
      }
    }
    switch (type) {
    case kBoolean: {
      char neg[512];
      set_value(name, JS_NewBool(opt_ctx, !is_negation));
      snprintf(neg, sizeof(neg), "--no-%s", name + 2);
      set_value(neg, JS_NewBool(opt_ctx, is_negation));
      break;
    }
    case kInteger:
      set_value(name, JS_NewInt64(opt_ctx, atoll(value)));
      break;
    case kUInteger:
      set_value(name, JS_NewInt64(opt_ctx, (int64_t)strtoull(value, NULL, 10)));
      break;
    case kString:
      set_value(name, JS_NewString(opt_ctx, value));
      break;
    case kStringList:
      push_list(name, value);
      break;
    case kHostPort: {
      JSValue hp = JS_GetPropertyStr(opt_ctx, opt_values, name);
      char *colon = strrchr(value, ':');
      if (!JS_IsObject(hp)) {
        JS_FreeValue(opt_ctx, hp);
        hp = JS_NewObjectProto(opt_ctx, JS_NULL);
        JS_SetPropertyStr(opt_ctx, hp, "host", JS_NewString(opt_ctx, "127.0.0.1"));
        JS_SetPropertyStr(opt_ctx, hp, "port", JS_NewInt32(opt_ctx, 9229));
        set_value(name, JS_DupValue(opt_ctx, hp));
      }
      if (colon) {
        *colon = 0;
        JS_SetPropertyStr(opt_ctx, hp, "host", JS_NewString(opt_ctx, value));
        JS_SetPropertyStr(opt_ctx, hp, "port", JS_NewInt32(opt_ctx, atoi(colon + 1)));
      } else if (value[0] >= '0' && value[0] <= '9') {
        JS_SetPropertyStr(opt_ctx, hp, "port", JS_NewInt32(opt_ctx, atoi(value)));
      } else {
        JS_SetPropertyStr(opt_ctx, hp, "host", JS_NewString(opt_ctx, value));
      }
      JS_FreeValue(opt_ctx, hp);
      break;
    }
    default:
      break;
    }
    /* what node.cc derives from them */
    if (!strcmp(name, "--eval"))
      set_value("[has_eval_string]", JS_TRUE);
    free(value);
    free(name);
    free(arg);
  }
  return 0;
}

/* NODE_OPTIONS: split on spaces, with "double quotes" and \ escapes */
static int split_env_options(const char *s, char ***out) {
  int n = 0;
  char **v = NULL;
  while (*s) {
    char buf[4096];
    size_t k = 0;
    bool quoted = false;
    while (*s == ' ')
      s++;
    if (!*s)
      break;
    while (*s && (quoted || *s != ' ') && k + 1 < sizeof(buf)) {
      if (*s == '"') {
        quoted = !quoted;
        s++;
        continue;
      }
      if (*s == '\\' && quoted && s[1]) {
        s++;
      }
      buf[k++] = *s++;
    }
    buf[k] = 0;
    v = realloc(v, (n + 1) * sizeof(char *));
    v[n++] = strdup(buf);
  }
  *out = v;
  return n;
}

int node_parse_args(int argc, char **argv, NodeOptions *opts, char **errmsg) {
  ArgList args;
  char **exec_args = NULL;
  int nexec = 0, i;
  const char *env_opts;
  JSValue defaults;

  defaults = JS_GetPropertyStr(opt_ctx, opt_table, "values");
  opt_values = JS_NewObjectProto(opt_ctx, JS_NULL);
  {
    JSPropertyEnum *props;
    uint32_t n, k;
    JS_GetOwnPropertyNames(opt_ctx, &props, &n, defaults, JS_GPN_STRING_MASK);
    for (k = 0; k < n; k++) {
      JSValue v = JS_GetProperty(opt_ctx, defaults, props[k].atom);
      JS_SetProperty(opt_ctx, opt_values, props[k].atom, v);
    }
    JS_FreePropertyEnum(opt_ctx, props, n);
  }
  JS_FreeValue(opt_ctx, defaults);

  /* NODE_OPTIONS first, then the command line (which wins) */
  env_opts = getenv("NODE_OPTIONS");
  if (env_opts && env_opts[0]) {
    ArgList ea;
    memset(&ea, 0, sizeof(ea));
    ea.n = split_env_options(env_opts, &ea.items);
    if (parse_options(&ea, NULL, NULL, true, errmsg) < 0)
      return 9;
    for (i = 0; i < ea.n; i++)
      free(ea.items[i]);
    free(ea.items);
  }

  memset(&args, 0, sizeof(args));
  args.items = argv;
  args.n = argc;
  args.pos = 1;
  if (parse_options(&args, &exec_args, &nexec, false, errmsg) < 0)
    return 9;

  opts->exec_argc = nexec;
  opts->exec_argv = exec_args ? exec_args : calloc(1, sizeof(char *));
  /* argv: the program, then everything after the options */
  opts->argc = 1 + (argc - args.pos);
  opts->argv = calloc(opts->argc + 1, sizeof(char *));
  opts->argv[0] = argv[0];
  for (i = args.pos; i < argc; i++)
    opts->argv[1 + i - args.pos] = argv[i];

  opts->values = opt_values;
  {
    JSValue v = JS_GetPropertyStr(opt_ctx, opt_values, "--eval");
    if (JS_IsString(v) && node_option_bool(opts, "[has_eval_string]")) {
      const char *s = JS_ToCString(opt_ctx, v);
      opts->eval_string = strdup(s);
      opts->has_eval_string = true;
      JS_FreeCString(opt_ctx, s);
    }
    JS_FreeValue(opt_ctx, v);
  }
  opts->print_eval = node_option_bool(opts, "--print");
  if (opts->print_eval && !opts->has_eval_string) {
    /* `node -p` alone: the script is the first argument */
    if (opts->argc > 1) {
      opts->eval_string = strdup(opts->argv[1]);
      opts->has_eval_string = true;
      memmove(opts->argv + 1, opts->argv + 2, (opts->argc - 1) * sizeof(char *));
      opts->argc--;
    } else {
      *errmsg = strdup("-p requires an argument");
      return 9;
    }
    set_value("--eval", JS_NewString(opt_ctx, opts->eval_string));
    set_value("[has_eval_string]", JS_TRUE);
  }
  opts->force_repl = node_option_bool(opts, "--interactive");
  opts->syntax_check_only = node_option_bool(opts, "--check");
  opts->print_help = node_option_bool(opts, "--help");
  opts->print_version = node_option_bool(opts, "--version");
  opts->print_v8_help = node_option_bool(opts, "--v8-options");
  opts->test_runner = node_option_bool(opts, "--test");
  opts->watch_mode = node_option_bool(opts, "--watch");
  opts->values = opt_values;
  opts->ctx = opt_ctx;
  opts->table = opt_table;
  return 0;
}

int node_options_init(JSContext *ctx) {
  opt_ctx = ctx;
  opt_table = JS_ParseJSON(ctx, node_options_json, strlen(node_options_json), "options.json");
  if (JS_IsException(opt_table))
    return -1;
  opt_values = JS_UNDEFINED;
  return 0;
}

void node_options_free(JSContext *ctx) {
  JS_FreeValue(ctx, opt_table);
  JS_FreeValue(ctx, opt_values);
  opt_table = JS_UNDEFINED;
  opt_values = JS_UNDEFINED;
}

JSValue node_options_values(JSContext *ctx, NodeOptions *opts) {
  return JS_DupValue(ctx, opts->values);
}

/* the context opts' values live in: the main one while parsing */
static JSContext *values_ctx(NodeOptions *opts) {
  return opts && opts->ctx ? opts->ctx : opt_ctx;
}

const char *node_option_string(NodeOptions *opts, const char *name) {
  static _Thread_local char buf[4096];
  JSContext *c = values_ctx(opts);
  JSValue v;
  const char *s;
  if (!opts || !c)
    return NULL;
  v = JS_GetPropertyStr(c, opts->values, name);
  if (!JS_IsString(v)) {
    JS_FreeValue(c, v);
    return NULL;
  }
  s = JS_ToCString(c, v);
  snprintf(buf, sizeof(buf), "%s", s);
  JS_FreeCString(c, s);
  JS_FreeValue(c, v);
  return buf;
}

bool node_option_bool(NodeOptions *opts, const char *name) {
  JSContext *c = values_ctx(opts);
  JSValue v;
  bool r;
  if (!opts || !c || JS_IsUndefined(opts->values))
    return false;
  v = JS_GetPropertyStr(c, opts->values, name);
  r = JS_ToBool(c, v);
  JS_FreeValue(c, v);
  return r;
}

double node_option_number(NodeOptions *opts, const char *name, double def) {
  JSContext *c = values_ctx(opts);
  JSValue v;
  double d = def;
  if (!opts || !c || JS_IsUndefined(opts->values))
    return def;
  v = JS_GetPropertyStr(c, opts->values, name);
  if (JS_IsNumber(v) || JS_IsBigInt(v))
    JS_ToFloat64(c, &d, v);
  JS_FreeValue(c, v);
  return d;
}

char *node_options_values_json(NodeOptions *opts) {
  JSContext *c = values_ctx(opts);
  JSValue j;
  char *r = NULL;
  if (!c || JS_IsUndefined(opts->values))
    return strdup("{}");
  j = JS_JSONStringify(c, opts->values, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsString(j)) {
    const char *s = JS_ToCString(c, j);
    r = strdup(s ? s : "{}");
    JS_FreeCString(c, s);
  } else if (JS_IsException(j)) {
    JS_FreeValue(c, JS_GetException(c));
  }
  JS_FreeValue(c, j);
  return r ? r : strdup("{}");
}

int node_options_init_worker(JSContext *ctx, NodeOptions *opts, const char *values_json) {
  JSValue v, o;
  JSPropertyEnum *props;
  uint32_t n, k;
  opts->ctx = ctx;
  opts->table = JS_ParseJSON(ctx, node_options_json, strlen(node_options_json), "options.json");
  if (JS_IsException(opts->table))
    return -1;
  v = JS_ParseJSON(ctx, values_json, strlen(values_json), "<options>");
  if (JS_IsException(v))
    return -1;
  /* a null-prototype object, as the parser makes */
  o = JS_NewObjectProto(ctx, JS_NULL);
  if (!JS_GetOwnPropertyNames(ctx, &props, &n, v, JS_GPN_STRING_MASK)) {
    for (k = 0; k < n; k++)
      JS_SetProperty(ctx, o, props[k].atom, JS_GetProperty(ctx, v, props[k].atom));
    JS_FreePropertyEnum(ctx, props, n);
  }
  JS_FreeValue(ctx, v);
  opts->values = o;
  return 0;
}

/* ---------------------------------------------------------------------- */
/* internalBinding('options') */

static JSValue get_cli_options_values(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue obj = JS_NewObjectProto(ctx, JS_NULL);
  JSPropertyEnum *props;
  uint32_t n, k;
  JS_GetOwnPropertyNames(ctx, &props, &n, env->options->values, JS_GPN_STRING_MASK);
  for (k = 0; k < n; k++) {
    JSValue v = JS_GetProperty(ctx, env->options->values, props[k].atom);
    if (JS_IsArray(v)) {
      /* a copy: JS may not change the parsed lists */
      JSValue c = JS_NewArray(ctx), lenv = JS_GetPropertyStr(ctx, v, "length");
      uint32_t len = 0, i;
      JS_ToUint32(ctx, &len, lenv);
      for (i = 0; i < len; i++)
        JS_SetPropertyUint32(ctx, c, i, JS_GetPropertyUint32(ctx, v, i));
      JS_FreeValue(ctx, v);
      v = c;
    }
    JS_SetProperty(ctx, obj, props[k].atom, v);
  }
  JS_FreePropertyEnum(ctx, props, n);
  return obj;
}

static JSValue get_cli_options_info(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  JSValue res = JS_NewObjectProto(ctx, JS_NULL);
  JSValue map_ctor = JS_GetPropertyStr(ctx, env_get(ctx)->global, "Map");
  JSValue options = JS_CallConstructor(ctx, map_ctor, 0, NULL);
  JSValue aliases = JS_CallConstructor(ctx, map_ctor, 0, NULL);
  JSValue set_fn = JS_GetPropertyStr(ctx, options, "set");
  Env *env = env_get(ctx);
  /* the runtime's own table (a worker's, or the main one) */
  JSValue table = env && env->options && env->options->ctx ? env->options->table : opt_table;
  JSValue tbl_opts = JS_GetPropertyStr(ctx, table, "options");
  JSValue tbl_aliases = JS_GetPropertyStr(ctx, table, "aliases");
  JSPropertyEnum *props;
  uint32_t n, k;
  JS_GetOwnPropertyNames(ctx, &props, &n, tbl_opts, JS_GPN_STRING_MASK);
  for (k = 0; k < n; k++) {
    JSValue o = JS_GetProperty(ctx, tbl_opts, props[k].atom);
    JSValue info = JS_NewObjectProto(ctx, JS_NULL), args[2], r;
    JS_SetPropertyStr(ctx, info, "helpText", JS_GetPropertyStr(ctx, o, "help"));
    JS_SetPropertyStr(ctx, info, "envVarSettings", JS_GetPropertyStr(ctx, o, "env"));
    JS_SetPropertyStr(ctx, info, "type", JS_GetPropertyStr(ctx, o, "type"));
    JS_SetPropertyStr(ctx, info, "defaultIsTrue", JS_GetPropertyStr(ctx, o, "defaultIsTrue"));
    args[0] = JS_AtomToString(ctx, props[k].atom);
    args[1] = info;
    r = JS_Call(ctx, set_fn, options, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, info);
    JS_FreeValue(ctx, o);
  }
  JS_FreePropertyEnum(ctx, props, n);
  JS_GetOwnPropertyNames(ctx, &props, &n, tbl_aliases, JS_GPN_STRING_MASK);
  for (k = 0; k < n; k++) {
    JSValue args[2], r;
    args[0] = JS_AtomToString(ctx, props[k].atom);
    args[1] = JS_GetProperty(ctx, tbl_aliases, props[k].atom);
    r = JS_Call(ctx, set_fn, aliases, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
  }
  JS_FreePropertyEnum(ctx, props, n);
  JS_FreeValue(ctx, set_fn);
  JS_FreeValue(ctx, tbl_opts);
  JS_FreeValue(ctx, tbl_aliases);
  JS_FreeValue(ctx, map_ctor);
  JS_SetPropertyStr(ctx, res, "options", options);
  JS_SetPropertyStr(ctx, res, "aliases", aliases);
  return res;
}

static JSValue get_embedder_options(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  JSValue o = JS_NewObjectProto(ctx, JS_NULL);
  JS_SetPropertyStr(ctx, o, "shouldNotRegisterESMLoader", JS_FALSE);
  JS_SetPropertyStr(ctx, o, "noGlobalSearchPaths", JS_FALSE);
  JS_SetPropertyStr(ctx, o, "noBrowserGlobals", JS_FALSE);
  JS_SetPropertyStr(ctx, o, "hasEmbedderPreload", JS_FALSE);
  return o;
}

static JSValue get_options_as_flags(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  return nb_array_from_strings(ctx, env->options->exec_argv, env->options->exec_argc);
}

static JSValue get_env_options_input_type(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  JSValue map_ctor = JS_GetPropertyStr(ctx, env_get(ctx)->global, "Map");
  JSValue m = JS_CallConstructor(ctx, map_ctor, 0, NULL);
  JS_FreeValue(ctx, map_ctor);
  return m;
}

JSValue binding_init_options(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), es = JS_NewObject(ctx), ty = JS_NewObject(ctx);
  nb_set_method(ctx, t, "getCLIOptionsValues", get_cli_options_values, 0);
  nb_set_method(ctx, t, "getCLIOptionsInfo", get_cli_options_info, 0);
  nb_set_method(ctx, t, "getEmbedderOptions", get_embedder_options, 0);
  nb_set_method(ctx, t, "getOptionsAsFlags", get_options_as_flags, 0);
  nb_set_method(ctx, t, "getEnvOptionsInputType", get_env_options_input_type, 0);
  nb_set_method(ctx, t, "getNamespaceOptionsInputType", get_env_options_input_type, 0);
  nb_set_int(ctx, es, "kAllowedInEnvvar", 0);
  nb_set_int(ctx, es, "kDisallowedInEnvvar", 1);
  nb_set(ctx, t, "envSettings", es);
  nb_set_int(ctx, ty, "kNoOp", 0);
  nb_set_int(ctx, ty, "kV8Option", 1);
  nb_set_int(ctx, ty, "kBoolean", 2);
  nb_set_int(ctx, ty, "kInteger", 3);
  nb_set_int(ctx, ty, "kUInteger", 4);
  nb_set_int(ctx, ty, "kString", 5);
  nb_set_int(ctx, ty, "kHostPort", 6);
  nb_set_int(ctx, ty, "kStringList", 7);
  nb_set(ctx, t, "types", ty);
  return t;
}

JSValue node_options_info(JSContext *ctx) {
  return get_cli_options_info(ctx, JS_UNDEFINED, 0, NULL);
}
