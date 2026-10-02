/* internalBinding('process_methods') (node_process_methods.cc). */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "node.h"

JSValue node_patch_process_object(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv);

static JSValue pm_hrtime(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  Env *env = env_get(ctx);
  uint64_t t = uv_hrtime();
  uint64_t sec = t / 1000000000ull;
  env->hrtime_buffer[0] = (uint32_t)(sec >> 32);
  env->hrtime_buffer[1] = (uint32_t)(sec & 0xffffffff);
  env->hrtime_buffer[2] = (uint32_t)(t % 1000000000ull);
  return JS_UNDEFINED;
}

static JSValue pm_hrtime_bigint(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  Env *env = env_get(ctx);
  uint64_t t = uv_hrtime();
  memcpy(env->hrtime_buffer, &t, 8);
  return JS_UNDEFINED;
}

static JSValue pm_cpu_usage(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  uv_rusage_t ru;
  size_t len;
  double *a = (double *)nb_buffer_data(ctx, ARG(0), &len);
  int err = uv_getrusage(&ru);
  if (err)
    return node_throw_uv_exception(ctx, err, "uv_getrusage", NULL, NULL, NULL);
  if (a && len >= 16) {
    a[0] = ru.ru_utime.tv_sec * 1e6 + ru.ru_utime.tv_usec;
    a[1] = ru.ru_stime.tv_sec * 1e6 + ru.ru_stime.tv_usec;
  }
  return JS_UNDEFINED;
}

/* the resident set; a WebAssembly guest's kernel counts none, so there it is
   the linear memory the program has grown to (all of it is in use: the
   guest has no paging) */
static int resident_set(size_t *rss) {
  int err = uv_resident_set_memory(rss);
#ifdef __wasm__
  if (err || *rss == 0) {
    *rss = (size_t)__builtin_wasm_memory_size(0) * 65536;
    err = 0;
  }
#endif
  return err;
}

static JSValue pm_memory_usage(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  size_t len, rss = 0;
  double *a = (double *)nb_buffer_data(ctx, ARG(0), &len);
  JSMemoryUsage mu;
  resident_set(&rss);
  JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mu);
  if (a && len >= 40) {
    a[0] = (double)rss;
    a[1] = (double)mu.malloc_size;
    a[2] = (double)mu.memory_used_size;
    a[3] = 0;
    a[4] = (double)mu.binary_object_size;
  }
  return JS_UNDEFINED;
}

static JSValue pm_rss(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  size_t rss = 0;
  int err = resident_set(&rss);
  if (err)
    return node_throw_uv_exception(ctx, err, "uv_resident_set_memory", NULL, NULL, NULL);
  return JS_NewFloat64(ctx, (double)rss);
}

static JSValue pm_resource_usage(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  uv_rusage_t ru;
  size_t len;
  double *a = (double *)nb_buffer_data(ctx, ARG(0), &len);
  int err = uv_getrusage(&ru);
  if (err)
    return node_throw_uv_exception(ctx, err, "uv_getrusage", NULL, NULL, NULL);
  if (a && len >= 16 * 8) {
    a[0] = ru.ru_utime.tv_sec * 1e6 + ru.ru_utime.tv_usec;
    a[1] = ru.ru_stime.tv_sec * 1e6 + ru.ru_stime.tv_usec;
    a[2] = (double)ru.ru_maxrss;
#ifdef __wasm__
    if (ru.ru_maxrss == 0)  /* KiB, as getrusage gives it */
      a[2] = (double)__builtin_wasm_memory_size(0) * 64;
#endif
    a[3] = (double)ru.ru_ixrss;
    a[4] = (double)ru.ru_idrss;
    a[5] = (double)ru.ru_isrss;
    a[6] = (double)ru.ru_minflt;
    a[7] = (double)ru.ru_majflt;
    a[8] = (double)ru.ru_nswap;
    a[9] = (double)ru.ru_inblock;
    a[10] = (double)ru.ru_oublock;
    a[11] = (double)ru.ru_msgsnd;
    a[12] = (double)ru.ru_msgrcv;
    a[13] = (double)ru.ru_nsignals;
    a[14] = (double)ru.ru_nvcsw;
    a[15] = (double)ru.ru_nivcsw;
  }
  return JS_UNDEFINED;
}

static JSValue pm_constrained_memory(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  return JS_NewFloat64(ctx, (double)uv_get_constrained_memory());
}

static JSValue pm_available_memory(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  return JS_NewFloat64(ctx, (double)uv_get_available_memory());
}

static JSValue pm_cwd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  char buf[4096];
  size_t len = sizeof(buf);
  int err = uv_cwd(buf, &len);
  if (err)
    return node_throw_uv_exception(ctx, err, "uv_cwd", NULL, NULL, NULL);
  return node_new_utf8_string(ctx, (const uint8_t *)buf, len);
}

static JSValue pm_chdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  char *dir = node_string_to_utf8(ctx, ARG(0), NULL);
  int err;
  if (!dir)
    return JS_EXCEPTION;
  err = uv_chdir(dir);
  if (err) {
    JSValue e;
    char buf[4096];
    size_t len = sizeof(buf);
    uv_cwd(buf, &len);
    e = node_uv_exception(ctx, err, "chdir", NULL, buf, dir);
    free(dir);
    return JS_Throw(ctx, e);
  }
  free(dir);
  return JS_UNDEFINED;
}

static JSValue pm_umask(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
  mode_t old;
  if (JS_IsUndefined(ARG(0))) {
    old = umask(0);
    umask(old);
  } else {
    old = umask((mode_t)nb_uint32(ctx, ARG(0), 0));
  }
  return JS_NewUint32(ctx, old);
}

static JSValue pm_uptime(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  Env *env = env_get(ctx);
  return JS_NewFloat64(ctx, (double)(uv_hrtime() - env->start_time_ns) / 1e9);
}

static JSValue pm_kill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t pid = nb_int32(ctx, ARG(0), 0), sig = nb_int32(ctx, ARG(1), SIGTERM);
  return JS_NewInt32(ctx, uv_kill(pid, sig));
}

static JSValue pm_really_exit(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  Env *env = env_get(ctx);
  int32_t code = nb_int32(ctx, ARG(0), env_exit_code(env, 0));
  fflush(stdout);
  fflush(stderr);
  env->stopping = true;
  if (!env->is_main_thread) {
    /* a worker ends its thread with the code (Worker::Exit) */
    env->exit_info[kHasExitCode] = 1;
    env->exit_info[kExitCode] = code;
    env_stop(env);
    return JS_UNDEFINED;
  }
  exit(code);
  return JS_UNDEFINED;
}

static JSValue pm_abort(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
  fflush(stdout);
  fflush(stderr);
  abort();
  return JS_UNDEFINED;
}

static JSValue pm_raw_debug(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  const char *s = JS_ToCString(ctx, ARG(0));
  if (s) {
    fprintf(stderr, "%s\n", s);
    fflush(stderr);
    JS_FreeCString(ctx, s);
  }
  return JS_UNDEFINED;
}

static JSValue pm_dlopen(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  char *f = node_string_to_utf8(ctx, ARG(1), NULL);
  JSValue e = JS_NewError(ctx);
  char msg[4200];
  snprintf(msg, sizeof(msg),
           "%s: native addons cannot be loaded by Node.js on collaboCore (WebAssembly)",
           f ? f : "?");
  JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, msg),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, "ERR_DLOPEN_FAILED"));
  free(f);
  return JS_Throw(ctx, e);
}

static JSValue pm_empty_array(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  return JS_NewArray(ctx);
}

JSValue node_active_handles(Env *env);       /* handles.c */
JSValue node_active_resources_info(Env *env);

static JSValue pm_get_active_handles(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  return node_active_handles(env_get(ctx));
}

static JSValue pm_get_active_resources_info(JSContext *ctx, JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  return node_active_resources_info(env_get(ctx));
}

static JSValue pm_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue pm_set_emit_warning_sync(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->emit_warning_sync);
  env->emit_warning_sync = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

JSValue util_parse_env_public(JSContext *ctx, const char *src, size_t len);

static JSValue pm_load_env_file(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  char *path = JS_IsUndefined(ARG(0)) ? strdup(".env") : node_string_to_utf8(ctx, ARG(0), NULL);
  FILE *f = fopen(path, "rb");
  char *buf;
  long n;
  JSValue parse, s, obj, util;
  if (!f) {
    JSValue e = node_uv_exception(ctx, -errno, "open", NULL, path, NULL);
    free(path);
    return JS_Throw(ctx, e);
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc(n + 1);
  n = fread(buf, 1, n, f);
  fclose(f);
  free(path);
  util = node_get_internal_binding(env_get(ctx), "util");
  parse = JS_GetPropertyStr(ctx, util, "parseEnv");
  s = node_new_utf8_string(ctx, (uint8_t *)buf, n);
  free(buf);
  obj = JS_Call(ctx, parse, JS_UNDEFINED, 1, (JSValueConst *)&s);
  JS_FreeValue(ctx, s);
  JS_FreeValue(ctx, parse);
  JS_FreeValue(ctx, util);
  if (JS_IsException(obj))
    return obj;
  {
    JSPropertyEnum *props;
    uint32_t cnt, i;
    JS_GetOwnPropertyNames(ctx, &props, &cnt, obj, JS_GPN_STRING_MASK);
    for (i = 0; i < cnt; i++) {
      const char *k = JS_AtomToCString(ctx, props[i].atom);
      JSValue v = JS_GetProperty(ctx, obj, props[i].atom);
      const char *vs = JS_ToCString(ctx, v);
      if (k && vs && !getenv(k))
        setenv(k, vs, 0);
      JS_FreeCString(ctx, k);
      JS_FreeCString(ctx, vs);
      JS_FreeValue(ctx, v);
    }
    JS_FreePropertyEnum(ctx, props, cnt);
  }
  JS_FreeValue(ctx, obj);
  return JS_UNDEFINED;
}

static char **array_to_strv(JSContext *ctx, JSValueConst arr) {
  JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
  uint32_t n = 0, i;
  char **v;
  JS_ToUint32(ctx, &n, lenv);
  v = calloc(n + 1, sizeof(char *));
  for (i = 0; i < n; i++) {
    JSValue e = JS_GetPropertyUint32(ctx, arr, i);
    v[i] = node_string_to_utf8(ctx, e, NULL);
    JS_FreeValue(ctx, e);
  }
  return v;
}

static JSValue pm_execve(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  char *path = node_string_to_utf8(ctx, ARG(0), NULL);
  char **args = array_to_strv(ctx, ARG(1)), **envp = array_to_strv(ctx, ARG(2));
  fflush(stdout);
  fflush(stderr);
  execve(path, args, envp);
  {
    JSValue e = node_errno_exception(ctx, errno, "execve", NULL, path);
    free(path);
    return JS_Throw(ctx, e);
  }
}

JSValue binding_init_process_methods(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  static const JSCFunctionListEntry funcs[] = {
    NB_FUNC("_debugProcess", 1, pm_noop),
    NB_FUNC("_debugEnd", 0, pm_noop),
    NB_FUNC("abort", 0, pm_abort),
    NB_FUNC("causeSegfault", 0, pm_abort),
    NB_FUNC("chdir", 1, pm_chdir),
    NB_FUNC("umask", 1, pm_umask),
    NB_FUNC("cpuUsage", 1, pm_cpu_usage),
    NB_FUNC("threadCpuUsage", 1, pm_cpu_usage),
    NB_FUNC("memoryUsage", 1, pm_memory_usage),
    NB_FUNC("constrainedMemory", 0, pm_constrained_memory),
    NB_FUNC("availableMemory", 0, pm_available_memory),
    NB_FUNC("rss", 0, pm_rss),
    NB_FUNC("resourceUsage", 1, pm_resource_usage),
    NB_FUNC("_getActiveRequests", 0, pm_empty_array),
    NB_FUNC("_getActiveHandles", 0, pm_get_active_handles),
    NB_FUNC("getActiveResourcesInfo", 0, pm_get_active_resources_info),
    NB_FUNC("_kill", 2, pm_kill),
    NB_FUNC("_rawDebug", 1, pm_raw_debug),
    NB_FUNC("cwd", 0, pm_cwd),
    NB_FUNC("dlopen", 3, pm_dlopen),
    NB_FUNC("reallyExit", 1, pm_really_exit),
    NB_FUNC("uptime", 0, pm_uptime),
    NB_FUNC("patchProcessObject", 1, node_patch_process_object),
    NB_FUNC("hrtime", 0, pm_hrtime),
    NB_FUNC("hrtimeBigInt", 0, pm_hrtime_bigint),
    NB_FUNC("setEmitWarningSync", 1, pm_set_emit_warning_sync),
    NB_FUNC("loadEnvFile", 1, pm_load_env_file),
    NB_FUNC("execve", 3, pm_execve),
    NB_FUNC("resetStdioForTesting", 0, pm_noop),
  };
  JS_SetPropertyFunctionList(ctx, t, funcs, countof(funcs));
  /* 3 x uint32 for hrtime(); its first 8 bytes for hrtimeBigInt() */
  nb_set(ctx, t, "hrtimeBuffer",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32, env->hrtime_buffer, 3, 4));
  return t;
}
