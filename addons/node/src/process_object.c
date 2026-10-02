/* The process object (node_process_object.cc) and process.env
 * (node_env_var.cc): an exotic object over the real environment. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "node.h"

/* process.arch.  Node has no "wasm32": the guest reports x64 (what
   programs check for to pick a supported code path; nothing native is
   loaded), unless NODE_QJS_ARCH says otherwise. */
const char *node_process_arch(void) {
  const char *a = getenv("NODE_QJS_ARCH");
  if (a && *a)
    return a;
#if defined(__aarch64__)
  return "arm64";
#elif defined(__i386__)
  return "ia32";
#else
  return "x64";
#endif
}

extern char **environ;

static const struct { const char *name, *value; } versions[] = {
  { "node", "24.21.0" },
  { "acorn", "8.18.0" },
  { "ada", "4.0.0" },
  { "ares", "1.34.8" },
  { "brotli", "1.2.0" },
  { "cjs_module_lexer", "1.4.1" },
  { "llhttp", "9.4.3" },
  { "modules", "137" },
  { "napi", "10" },
  { "nghttp2", "1.70.0" },
  { "openssl", "3.5.7" },
  { "quickjs", "0.17.0" },
  { "undici", "7.29.1" },
  { "unicode", "17.0" },
  { "uv", NULL },
  { "zlib", "1.3.1" },
};

static JSValue make_versions(JSContext *ctx) {
  JSValue v = JS_NewObject(ctx);
  size_t i;
  for (i = 0; i < countof(versions); i++) {
    const char *val = versions[i].value;
    if (!strcmp(versions[i].name, "uv"))
      val = uv_version_string();
    JS_DefinePropertyValueStr(ctx, v, versions[i].name, JS_NewString(ctx, val),
                              JS_PROP_ENUMERABLE);
  }
  return v;
}

static JSValue raw_debug(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  const char *s = JS_ToCString(ctx, ARG(0));
  if (s) {
    fprintf(stderr, "%s\n", s);
    fflush(stderr);
    JS_FreeCString(ctx, s);
  }
  return JS_UNDEFINED;
}

static JSValue process_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                            JSValueConst *argv) {
  return JS_ThrowTypeError(ctx, "Illegal constructor");
}

JSValue node_create_process_object(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue ctor, proto, process, release, sym;
  ctor = JS_NewCFunction2(ctx, process_ctor, "process", 0, JS_CFUNC_constructor, 0);
  proto = JS_NewObject(ctx);
  JS_SetConstructor(ctx, ctor, proto);
  process = JS_NewObjectProto(ctx, proto);
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, ctor);

  sym = env_get_private_symbol(env, "exit_info_private_symbol");
  {
    JSAtom a = JS_ValueToAtom(ctx, sym);
    JS_DefinePropertyValue(ctx, process, a,
                           nb_new_typed_array(ctx, JS_TYPED_ARRAY_INT32, env->exit_info,
                                              kExitInfoFieldsCount, 4),
                           JS_PROP_C_W_E);
    JS_FreeAtom(ctx, a);
  }
  JS_FreeValue(ctx, sym);

  nb_define_readonly(ctx, process, "version", JS_NewString(ctx, NODE_VERSION_STRING));
  nb_define_readonly(ctx, process, "versions", make_versions(ctx));
  nb_define_readonly(ctx, process, "arch", JS_NewString(ctx, node_process_arch()));
  nb_define_readonly(ctx, process, "platform", JS_NewString(ctx, "linux"));
  release = JS_NewObject(ctx);
  nb_define_readonly(ctx, release, "name", JS_NewString(ctx, "node"));
  nb_define_readonly(ctx, release, "lts", JS_NewString(ctx, "Krypton"));
  nb_define_readonly(ctx, release, "sourceUrl",
                     JS_NewString(ctx, "https://nodejs.org/download/release/v24.21.0/node-v24.21.0.tar.gz"));
  nb_define_readonly(ctx, release, "headersUrl",
                     JS_NewString(ctx, "https://nodejs.org/download/release/v24.21.0/node-v24.21.0-headers.tar.gz"));
  nb_define_readonly(ctx, process, "release", release);
  nb_set_method(ctx, process, "_rawDebug", raw_debug, 0);
  return process;
}

/* ---- PatchProcessObject (process_methods.patchProcessObject) ---- */

static char process_title_buf[1024];

static JSValue title_getter(JSContext *ctx, JSValueConst this_val) {
  char buf[1024];
  static bool cli_title_done;
  if (!cli_title_done) {
    /* --title=TITLE */
    Env *env = env_get(ctx);
    const char *t = env && env->options ? node_option_string(env->options, "--title") : NULL;
    cli_title_done = true;
    if (t && *t) {
      snprintf(process_title_buf, sizeof(process_title_buf), "%s", t);
      uv_set_process_title(process_title_buf);
    }
  }
  if (uv_get_process_title(buf, sizeof(buf)) == 0 && buf[0])
    return JS_NewString(ctx, buf);
  return JS_NewString(ctx, process_title_buf[0] ? process_title_buf : "node");
}

static JSValue title_setter(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
  const char *s = JS_ToCString(ctx, val);
  if (!s)
    return JS_EXCEPTION;
  snprintf(process_title_buf, sizeof(process_title_buf), "%s", s);
  uv_set_process_title(s);
  JS_FreeCString(ctx, s);
  return JS_UNDEFINED;
}

static JSValue ppid_getter(JSContext *ctx, JSValueConst this_val) {
  return JS_NewInt32(ctx, uv_os_getppid());
}

static int debug_port = 9229;
static JSValue debug_port_getter(JSContext *ctx, JSValueConst this_val) {
  return JS_NewInt32(ctx, debug_port);
}
static JSValue debug_port_setter(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
  int32_t p = 0;
  JS_ToInt32(ctx, &p, val);
  debug_port = p;
  return JS_UNDEFINED;
}

static void define_accessor(JSContext *ctx, JSValueConst obj, const char *name,
                            JSCFunctionType get, JSCFunctionType set, bool has_set) {
  JSAtom a = JS_NewAtom(ctx, name);
  JSValue g = JS_NewCFunction2(ctx, get.generic, name, 0, JS_CFUNC_getter, 0);
  JSValue s = has_set ? JS_NewCFunction2(ctx, set.generic, name, 1, JS_CFUNC_setter, 0)
                      : JS_UNDEFINED;
  JS_DefineProperty(ctx, obj, a, JS_UNDEFINED, g, s,
                    JS_PROP_HAS_GET | (has_set ? JS_PROP_HAS_SET : 0) |
                    JS_PROP_HAS_ENUMERABLE | JS_PROP_ENUMERABLE |
                    JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, g);
  JS_FreeValue(ctx, s);
  JS_FreeAtom(ctx, a);
}

JSValue node_patch_process_object(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValueConst process = ARG(0);
  JSCFunctionType g, s;
  if (!JS_IsObject(process))
    return JS_ThrowTypeError(ctx, "process expected");
  g.getter = title_getter;
  s.setter = title_setter;
  define_accessor(ctx, process, "title", g, s, env->owns_process_state);
  JS_SetPropertyStr(ctx, process, "argv", nb_array_from_strings(ctx, env->argv, env->argc));
  JS_SetPropertyStr(ctx, process, "execArgv",
                    nb_array_from_strings(ctx, env->exec_argv, env->exec_argc));
  nb_define_readonly(ctx, process, "pid", JS_NewInt32(ctx, uv_os_getpid()));
  g.getter = ppid_getter;
  define_accessor(ctx, process, "ppid", g, s, false);
  JS_SetPropertyStr(ctx, process, "execPath", JS_NewString(ctx, env->exec_path));
  g.getter = debug_port_getter;
  s.setter = debug_port_setter;
  define_accessor(ctx, process, "debugPort", g, s, env->owns_process_state);
  nb_define_readonly(ctx, process, "versions", make_versions(ctx));
  return JS_UNDEFINED;
}

/* ---------------------------------------------------------------------- */
/* process.env */

/* The variables behind process.env: the main thread's are the real
   environment (environ, under environ_lock: workers read it too); a worker
   has its own copy (or shares one, with SHARE_ENV), as Node's KVStore. */
typedef struct EnvStore {
  pthread_mutex_t lock;
  int refs;
  char **vars;   /* "NAME=value", malloc'ed */
  int n, cap;
} EnvStore;

static pthread_mutex_t environ_lock = PTHREAD_MUTEX_INITIALIZER;

static EnvStore *store_new(void) {
  EnvStore *st = calloc(1, sizeof(*st));
  pthread_mutex_init(&st->lock, NULL);
  st->refs = 1;
  return st;
}

static void store_add(EnvStore *st, const char *kv) {
  if (st->n == st->cap) {
    st->cap = st->cap ? st->cap * 2 : 32;
    st->vars = realloc(st->vars, st->cap * sizeof(char *));
  }
  st->vars[st->n++] = strdup(kv);
}

static int store_find(EnvStore *st, const char *name) {
  size_t len = strlen(name);
  int i;
  for (i = 0; i < st->n; i++)
    if (!strncmp(st->vars[i], name, len) && st->vars[i][len] == '=')
      return i;
  return -1;
}

/* a copy of env's variables (a worker's start, by default) */
void *node_env_store_copy(Env *env) {
  EnvStore *st = store_new(), *from = env ? env->env_store : NULL;
  char **e;
  int i;
  if (from) {
    pthread_mutex_lock(&from->lock);
    for (i = 0; i < from->n; i++)
      store_add(st, from->vars[i]);
    pthread_mutex_unlock(&from->lock);
  } else {
    pthread_mutex_lock(&environ_lock);
    for (e = environ; e && *e; e++)
      store_add(st, *e);
    pthread_mutex_unlock(&environ_lock);
  }
  return st;
}

/* variables from "NAME=value" strings (the Worker's env option) */
void *node_env_store_from(char **kv, int n) {
  EnvStore *st = store_new();
  int i;
  for (i = 0; i < n; i++)
    store_add(st, kv[i]);
  return st;
}

void *node_env_store_ref(Env *env) {
  EnvStore *st = env->env_store;
  if (st) {
    pthread_mutex_lock(&st->lock);
    st->refs++;
    pthread_mutex_unlock(&st->lock);
  }
  return st;
}

void node_env_store_unref(void *p) {
  EnvStore *st = p;
  bool last;
  int i;
  if (!st)
    return;
  pthread_mutex_lock(&st->lock);
  last = --st->refs == 0;
  pthread_mutex_unlock(&st->lock);
  if (!last)
    return;
  for (i = 0; i < st->n; i++)
    free(st->vars[i]);
  free(st->vars);
  pthread_mutex_destroy(&st->lock);
  free(st);
}

/* the value of name (malloc'ed), or NULL */
static char *var_get(Env *env, const char *name) {
  EnvStore *st = env->env_store;
  char *r = NULL;
  if (!st) {
    const char *v;
    pthread_mutex_lock(&environ_lock);
    v = getenv(name);
    r = v ? strdup(v) : NULL;
    pthread_mutex_unlock(&environ_lock);
    return r;
  }
  pthread_mutex_lock(&st->lock);
  {
    int i = store_find(st, name);
    if (i >= 0)
      r = strdup(st->vars[i] + strlen(name) + 1);
  }
  pthread_mutex_unlock(&st->lock);
  return r;
}

/* a changed TZ takes effect for Date at once (DateTimeConfigurationChangeNotification) */
static void tz_changed(const char *name) {
  if (!strcmp(name, "TZ"))
    tzset();
}

static void var_set(Env *env, const char *name, const char *value) {
  EnvStore *st = env->env_store;
  char *kv;
  int i;
  if (!st) {
    pthread_mutex_lock(&environ_lock);
    setenv(name, value, 1);
    tz_changed(name);
    pthread_mutex_unlock(&environ_lock);
    return;
  }
  kv = malloc(strlen(name) + strlen(value) + 2);
  sprintf(kv, "%s=%s", name, value);
  pthread_mutex_lock(&st->lock);
  i = store_find(st, name);
  if (i >= 0) {
    free(st->vars[i]);
    st->vars[i] = kv;
  } else {
    store_add(st, kv);
    free(kv);
  }
  pthread_mutex_unlock(&st->lock);
}

static void var_unset(Env *env, const char *name) {
  EnvStore *st = env->env_store;
  int i;
  if (!st) {
    pthread_mutex_lock(&environ_lock);
    unsetenv(name);
    tz_changed(name);
    pthread_mutex_unlock(&environ_lock);
    return;
  }
  pthread_mutex_lock(&st->lock);
  i = store_find(st, name);
  if (i >= 0) {
    free(st->vars[i]);
    st->vars[i] = st->vars[--st->n];
  }
  pthread_mutex_unlock(&st->lock);
}

/* the "NAME=value" strings (each malloc'ed, and the array) */
static char **var_list(Env *env, int *count) {
  EnvStore *st = env->env_store;
  char **r;
  int n = 0, i;
  if (!st) {
    char **e;
    pthread_mutex_lock(&environ_lock);
    for (e = environ; e && *e; e++)
      n++;
    r = calloc(n + 1, sizeof(char *));
    for (i = 0; i < n; i++)
      r[i] = strdup(environ[i]);
    pthread_mutex_unlock(&environ_lock);
  } else {
    pthread_mutex_lock(&st->lock);
    n = st->n;
    r = calloc(n + 1, sizeof(char *));
    for (i = 0; i < n; i++)
      r[i] = strdup(st->vars[i]);
    pthread_mutex_unlock(&st->lock);
  }
  *count = n;
  return r;
}

static JSClassID env_class_id;

typedef struct {
  JSValue symbols; /* ordinary object holding symbol-keyed properties */
} EnvProxy;

static char *atom_name(JSContext *ctx, JSAtom atom) {
  const char *s = JS_AtomToCString(ctx, atom);
  char *r = s ? strdup(s) : NULL;
  JS_FreeCString(ctx, s);
  return r;
}

static bool atom_is_symbol(JSContext *ctx, JSAtom atom) {
  JSValue v = JS_AtomToValue(ctx, atom);
  bool r = JS_IsSymbol(v);
  JS_FreeValue(ctx, v);
  return r;
}

static int env_get_own_property(JSContext *ctx, JSPropertyDescriptor *desc,
                                JSValueConst obj, JSAtom prop) {
  EnvProxy *p = JS_GetOpaque(obj, env_class_id);
  char *name, *v;
  if (atom_is_symbol(ctx, prop)) {
    JSPropertyDescriptor d;
    int r = JS_GetOwnProperty(ctx, &d, p->symbols, prop);
    if (r > 0) {
      if (desc)
        *desc = d;
      else {
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
      }
    }
    return r;
  }
  name = atom_name(ctx, prop);
  v = name ? var_get(env_get(ctx), name) : NULL;
  free(name);
  if (!v)
    return 0;
  if (desc) {
    desc->flags = JS_PROP_C_W_E;
    desc->value = node_new_utf8_string(ctx, (const uint8_t *)v, strlen(v));
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
  }
  free(v);
  return 1;
}

static int env_get_own_property_names(JSContext *ctx, JSPropertyEnum **ptab,
                                      uint32_t *plen, JSValueConst obj) {
  EnvProxy *p = JS_GetOpaque(obj, env_class_id);
  JSPropertyEnum *tab, *stab = NULL;
  uint32_t n = 0, i, k, sn = 0;
  int count;
  char **environ = var_list(env_get(ctx), &count);
  n = count;
  JS_GetOwnPropertyNames(ctx, &stab, &sn, p->symbols, JS_GPN_SYMBOL_MASK);
  tab = js_malloc(ctx, sizeof(JSPropertyEnum) * (n + sn + 1));
  k = 0;
  for (i = 0; i < n; i++) {
    const char *eq = strchr(environ[i], '=');
    size_t len = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
    uint32_t j;
    JSAtom a;
    if (len == 0)
      continue;
    a = JS_NewAtomLen(ctx, environ[i], len);
    /* skip duplicates */
    for (j = 0; j < k; j++)
      if (tab[j].atom == a)
        break;
    if (j < k) {
      JS_FreeAtom(ctx, a);
      continue;
    }
    tab[k].is_enumerable = true;
    tab[k].atom = a;
    k++;
  }
  for (i = 0; i < sn; i++) {
    tab[k].is_enumerable = stab[i].is_enumerable;
    tab[k].atom = JS_DupAtom(ctx, stab[i].atom);
    k++;
  }
  JS_FreePropertyEnum(ctx, stab, sn);
  for (i = 0; i < n; i++)
    free(environ[i]);
  free(environ);
  *ptab = tab;
  *plen = k;
  return 0;
}

static int env_delete_property(JSContext *ctx, JSValueConst obj, JSAtom prop) {
  EnvProxy *p = JS_GetOpaque(obj, env_class_id);
  char *name;
  if (atom_is_symbol(ctx, prop))
    return 1;  /* EnvDeleter: nothing to delete, and true */
  name = atom_name(ctx, prop);
  if (name)
    var_unset(env_get(ctx), name);
  free(name);
  return 1;
}

static int env_set_string(JSContext *ctx, JSAtom prop, JSValueConst val) {
  char *name = atom_name(ctx, prop);
  JSValue s;
  char *v;
  if (!name)
    return -1;
  if (!JS_IsString(val) && !JS_IsNumber(val) && !JS_IsBool(val)) {
    static bool warned;
    if (!warned) {
      Env *env = env_get(ctx);
      warned = true;
      node_emit_process_warning(env,
          "Assigning any value other than a string, number, or boolean to a "
          "process.env property is deprecated. Please make sure to convert the "
          "value to a string before setting process.env with it.",
          "DeprecationWarning", "DEP0104");
    }
  }
  s = JS_ToString(ctx, val);
  if (JS_IsException(s)) {
    free(name);
    return -1;
  }
  v = node_string_to_utf8(ctx, s, NULL);
  JS_FreeValue(ctx, s);
  if (name[0] && !strchr(name, '='))
    var_set(env_get(ctx), name, v ? v : "");
  free(v);
  free(name);
  return 1;
}

static int env_define_own_property(JSContext *ctx, JSValueConst this_obj, JSAtom prop,
                                   JSValueConst val, JSValueConst getter,
                                   JSValueConst setter, int flags) {
  EnvProxy *p = JS_GetOpaque(this_obj, env_class_id);
  if (atom_is_symbol(ctx, prop)) {
    /* EnvDefiner: the name is converted to a string */
    JS_ThrowTypeError(ctx, "Cannot convert a Symbol value to a string");
    return -1;
  }
  /* EnvDefiner: a data descriptor that says configurable, writable and
     enumerable, all three */
  if (flags & JS_PROP_HAS_VALUE) {
    const int all = JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE | JS_PROP_HAS_WRITABLE |
                    JS_PROP_WRITABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_ENUMERABLE;
    if ((flags & all) != all) {
      node_throw_type_error(ctx, "ERR_INVALID_OBJECT_DEFINE_PROPERTY",
                            "'process.env' only accepts a configurable, writable, "
                            "and enumerable data descriptor");
      return -1;
    }
    return env_set_string(ctx, prop, val);
  }
  if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
    node_throw_type_error(ctx, "ERR_INVALID_OBJECT_DEFINE_PROPERTY",
                          "'process.env' does not accept an accessor(getter/setter) descriptor");
    return -1;
  }
  node_throw_type_error(ctx, "ERR_INVALID_OBJECT_DEFINE_PROPERTY",
                        "'process.env' only accepts a configurable, writable, "
                        "and enumerable data descriptor");
  return -1;
}

static int env_has_property(JSContext *ctx, JSValueConst obj, JSAtom atom) {
  EnvProxy *p = JS_GetOpaque(obj, env_class_id);
  char *name;
  bool r;
  if (atom_is_symbol(ctx, atom))
    return JS_HasProperty(ctx, p->symbols, atom);
  name = atom_name(ctx, atom);
  {
    char *v = name ? var_get(env_get(ctx), name) : NULL;
    r = v != NULL;
    free(v);
  }
  free(name);
  if (!r) {
    /* Object.prototype members are reachable through `in` */
    JSValue proto = JS_GetPrototype(ctx, obj);
    int h = JS_IsObject(proto) ? JS_HasProperty(ctx, proto, atom) : 0;
    JS_FreeValue(ctx, proto);
    return h;
  }
  return 1;
}

static JSValue env_get_property(JSContext *ctx, JSValueConst obj, JSAtom atom,
                                JSValueConst receiver) {
  EnvProxy *p = JS_GetOpaque(obj, env_class_id);
  char *name, *v;
  if (atom_is_symbol(ctx, atom)) {
    JSValue r = JS_GetProperty(ctx, p->symbols, atom);
    if (JS_IsUndefined(r)) {
      JSValue proto = JS_GetPrototype(ctx, obj);
      if (JS_IsObject(proto))
        r = JS_GetProperty(ctx, proto, atom);
      JS_FreeValue(ctx, proto);
    }
    return r;
  }
  name = atom_name(ctx, atom);
  v = name ? var_get(env_get(ctx), name) : NULL;
  free(name);
  if (v) {
    JSValue r = node_new_utf8_string(ctx, (const uint8_t *)v, strlen(v));
    free(v);
    return r;
  }
  {
    JSValue proto = JS_GetPrototype(ctx, obj), r = JS_UNDEFINED;
    if (JS_IsObject(proto))
      r = JS_GetProperty(ctx, proto, atom);
    JS_FreeValue(ctx, proto);
    return r;
  }
}

static int env_set_property(JSContext *ctx, JSValueConst obj, JSAtom atom,
                            JSValueConst value, JSValueConst receiver, int flags) {
  EnvProxy *p = JS_GetOpaque(obj, env_class_id);
  if (atom_is_symbol(ctx, atom)) {
    /* EnvSetter: the name is converted to a string */
    JS_ThrowTypeError(ctx, "Cannot convert a Symbol value to a string");
    return -1;
  }
  return env_set_string(ctx, atom, value);
}

static void env_finalizer(JSRuntime *rt, JSValueConst val) {
  EnvProxy *p = JS_GetOpaque(val, env_class_id);
  if (p) {
    JS_FreeValueRT(rt, p->symbols);
    free(p);
  }
}

static void env_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
  EnvProxy *p = JS_GetOpaque(val, env_class_id);
  if (p)
    JS_MarkValue(rt, p->symbols, mark_func);
}

static JSClassExoticMethods env_exotic = {
  .get_own_property = env_get_own_property,
  .get_own_property_names = env_get_own_property_names,
  .delete_property = env_delete_property,
  .define_own_property = env_define_own_property,
  .has_property = env_has_property,
  .get_property = env_get_property,
  .set_property = env_set_property,
};

JSValue node_create_env_proxy(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue obj;
  EnvProxy *p;
  JS_NewClassID(env->rt, &env_class_id);
  if (!JS_IsRegisteredClass(env->rt, env_class_id)) {
    JSClassDef def = { .class_name = "Object", .finalizer = env_finalizer,
                       .gc_mark = env_mark, .exotic = &env_exotic };
    JS_NewClass(env->rt, env_class_id, &def);
  }
  {
    JSValue tmp = JS_NewObject(ctx), proto = JS_GetPrototype(ctx, tmp);
    obj = JS_NewObjectProtoClass(ctx, proto, env_class_id);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, tmp);
  }
  p = calloc(1, sizeof(*p));
  p->symbols = JS_NewObject(ctx);
  JS_SetOpaque(obj, p);
  return obj;
}
