/* Child processes and signals: internalBinding('process_wrap')
 * (process_wrap.cc), ('spawn_sync') (spawn_sync.cc) and ('signal_wrap')
 * (signal_wrap.cc).  libuv starts children with posix_spawn here (no fork
 * in the guest; deps/uv is patched for it). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"
#include "streams.h"

JSClassID node_process_class_id, node_signal_class_id;

const char *node_signo_string(int signo) {
  switch (signo) {
#define S(x) case x: return #x;
  S(SIGINT) S(SIGILL) S(SIGABRT) S(SIGFPE) S(SIGKILL) S(SIGSEGV) S(SIGTERM)
  S(SIGHUP) S(SIGQUIT) S(SIGTRAP) S(SIGBUS) S(SIGUSR1) S(SIGUSR2) S(SIGPIPE)
  S(SIGALRM) S(SIGCHLD) S(SIGCONT) S(SIGSTOP) S(SIGTSTP) S(SIGTTIN) S(SIGTTOU)
  S(SIGURG) S(SIGXCPU) S(SIGXFSZ) S(SIGVTALRM) S(SIGPROF) S(SIGWINCH) S(SIGIO)
  S(SIGSYS)
#ifdef SIGPWR
  S(SIGPWR)
#endif
#ifdef SIGSTKFLT
  S(SIGSTKFLT)
#endif
#undef S
  default: return "";
  }
}

/* ---- option parsing shared by Process.spawn and spawnSync ---- */

static char **string_array(JSContext *ctx, JSValueConst arr) {
  JSValue lenv;
  uint32_t n = 0, i;
  char **v;
  if (!JS_IsArray(arr))
    return NULL;
  lenv = JS_GetPropertyStr(ctx, arr, "length");
  JS_ToUint32(ctx, &n, lenv);
  v = calloc(n + 1, sizeof(char *));
  for (i = 0; i < n; i++) {
    JSValue e = JS_GetPropertyUint32(ctx, arr, i);
    JSValue s = JS_ToString(ctx, e);
    v[i] = node_string_to_utf8(ctx, s, NULL);
    JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, e);
  }
  return v;
}

static void free_strings(char **v) {
  char **p;
  if (!v)
    return;
  for (p = v; *p; p++)
    free(*p);
  free(v);
}

static bool str_eq(JSContext *ctx, JSValueConst v, const char *s) {
  const char *c;
  bool r;
  if (!JS_IsString(v))
    return false;
  c = JS_ToCString(ctx, v);
  r = c && !strcmp(c, s);
  JS_FreeCString(ctx, c);
  return r;
}

/* stdio for uv_spawn from [{type, handle | fd}] (ParseStdioOptions) */
static int parse_stdio(JSContext *ctx, JSValueConst arr, uv_stdio_container_t **out, int *count) {
  JSValue lenv;
  uint32_t n = 0, i;
  uv_stdio_container_t *st;
  if (!JS_IsArray(arr)) {
    node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "options.stdio must be an array");
    return -1;
  }
  lenv = JS_GetPropertyStr(ctx, arr, "length");
  JS_ToUint32(ctx, &n, lenv);
  st = calloc(n ? n : 1, sizeof(*st));
  for (i = 0; i < n; i++) {
    JSValue o = JS_GetPropertyUint32(ctx, arr, i);
    JSValue type = JS_GetPropertyStr(ctx, o, "type");
    if (str_eq(ctx, type, "ignore")) {
      st[i].flags = UV_IGNORE;
    } else if (str_eq(ctx, type, "pipe") || str_eq(ctx, type, "overlapped") ||
               str_eq(ctx, type, "wrap")) {
      JSValue h = JS_GetPropertyStr(ctx, o, "handle");
      StreamWrap *s = node_stream_wrap_of(h);
      JS_FreeValue(ctx, h);
      if (!s) {
        JS_FreeValue(ctx, type);
        JS_FreeValue(ctx, o);
        free(st);
        node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "invalid stdio handle");
        return -1;
      }
      st[i].flags = str_eq(ctx, type, "wrap")
                        ? UV_INHERIT_STREAM
                        : (uv_stdio_flags)(UV_CREATE_PIPE | UV_READABLE_PIPE | UV_WRITABLE_PIPE);
      st[i].data.stream = s->stream;
    } else {
      JSValue fd = JS_GetPropertyStr(ctx, o, "fd");
      st[i].flags = UV_INHERIT_FD;
      st[i].data.fd = nb_int32(ctx, fd, (int32_t)i);
      JS_FreeValue(ctx, fd);
    }
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, o);
  }
  *out = st;
  *count = n;
  return 0;
}

/* ---------------------------------------------------------------------- */
/* Process */

typedef struct {
  HandleWrap hw;
  uv_process_t process;
  bool spawned;
} ProcessWrap;

static void process_on_exit(uv_process_t *h, int64_t status, int term_signal) {
  ProcessWrap *p = h->data;
  JSContext *ctx = p->hw.aw.env->ctx;
  JSValue args[2], r;
  args[0] = JS_NewFloat64(ctx, (double)status);
  args[1] = JS_NewString(ctx, node_signo_string(term_signal));
  r = async_wrap_make_callback_name(&p->hw.aw, "onexit", 2, (JSValueConst *)args);
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, args[1]);
}

static void process_finalizer(JSRuntime *rt, JSValueConst val) {
  ProcessWrap *p = JS_GetOpaque(val, node_process_class_id);
  if (!p)
    return;
  async_wrap_destroy(&p->hw.aw);
  if (p->spawned && p->hw.state == HW_INITIALIZED) {
    node_close_and_free((uv_handle_t *)&p->process, p);
    return;
  }
  free(p);
}

static JSValue process_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, node_process_class_id);
  ProcessWrap *p;
  if (JS_IsException(obj))
    return obj;
  p = calloc(1, sizeof(*p));
  async_wrap_init(&p->hw.aw, env_get(ctx), obj, PROVIDER_PROCESSWRAP, -1);
  p->hw.state = HW_CLOSED;
  p->hw.handle = (uv_handle_t *)&p->process;
  JS_SetOpaque(obj, p);
  return obj;
}

/* spawn(file, args, cwd, envPairs, stdio, flags, uid, gid) */
static JSValue process_spawn(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  Env *env = env_get(ctx);
  ProcessWrap *p = JS_GetOpaque2(ctx, this_val, node_process_class_id);
  uv_process_options_t o;
  char *file, *cwd = NULL, **args = NULL, **envp = NULL;
  uv_stdio_container_t *stdio = NULL;
  int count = 0, err;
  uint32_t flags = nb_uint32(ctx, ARG(5), 0);
  if (!p)
    return JS_EXCEPTION;
  memset(&o, 0, sizeof(o));
  file = node_string_to_utf8(ctx, ARG(0), NULL);
  if (!file)
    return JS_EXCEPTION;
  args = string_array(ctx, ARG(1));
  if (JS_IsString(ARG(2)))
    cwd = node_string_to_utf8(ctx, ARG(2), NULL);
  envp = string_array(ctx, ARG(3));
  if (parse_stdio(ctx, ARG(4), &stdio, &count) < 0) {
    free(file);
    free(cwd);
    free_strings(args);
    free_strings(envp);
    return JS_EXCEPTION;
  }
  o.exit_cb = process_on_exit;
  o.file = file;
  o.args = args;
  o.cwd = cwd && cwd[0] ? cwd : NULL;
  o.env = envp;
  o.stdio = stdio;
  o.stdio_count = count;
  if (flags & 1)
    o.flags |= UV_PROCESS_DETACHED;
  if (JS_IsNumber(ARG(6))) {
    o.flags |= UV_PROCESS_SETUID;
    o.uid = nb_uint32(ctx, ARG(6), 0);
  }
  if (JS_IsNumber(ARG(7))) {
    o.flags |= UV_PROCESS_SETGID;
    o.gid = nb_uint32(ctx, ARG(7), 0);
  }
  err = uv_spawn(env->loop, &p->process, &o);
  if (err == 0) {
    node_handle_wrap_init(&p->hw, env, this_val, (uv_handle_t *)&p->process,
                          PROVIDER_PROCESSWRAP);
    p->spawned = true;
    JS_SetPropertyStr(ctx, this_val, "pid", JS_NewInt32(ctx, p->process.pid));
  }
  free(file);
  free(cwd);
  free_strings(args);
  free_strings(envp);
  free(stdio);
  return JS_NewInt32(ctx, err);
}

static JSValue process_kill(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  ProcessWrap *p = JS_GetOpaque2(ctx, this_val, node_process_class_id);
  if (!p)
    return JS_EXCEPTION;
  if (!p->spawned || p->hw.state != HW_INITIALIZED)
    return JS_NewInt32(ctx, UV_ESRCH);
  return JS_NewInt32(ctx, uv_process_kill(&p->process, nb_int32(ctx, ARG(0), SIGTERM)));
}

static const JSCFunctionListEntry process_proto[] = {
  JS_CFUNC_DEF("spawn", 8, process_spawn),
  JS_CFUNC_DEF("kill", 1, process_kill),
};

JSValue binding_init_process_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), c = JS_NewObject(ctx), ctor, proto;
  NodeClassDef def = { .name = "Process", .class_id = &node_process_class_id,
                       .ctor = process_ctor, .finalizer = process_finalizer,
                       .proto_funcs = process_proto, .proto_funcs_count = countof(process_proto),
                       .parent_ctor = JS_UNDEFINED };
  ctor = nb_define_class(ctx, t, &def);
  proto = JS_GetPropertyStr(ctx, ctor, "prototype");
  JS_SetPropertyFunctionList(ctx, proto, node_handle_wrap_funcs, node_handle_wrap_funcs_count);
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, ctor);
  nb_set_int(ctx, c, "kProcessFlagDetached", 1);
  nb_set_int(ctx, c, "kProcessFlagWindowsHide", 2);
  nb_set_int(ctx, c, "kProcessFlagWindowsVerbatimArguments", 4);
  nb_set(ctx, t, "constants", c);
  return t;
}

/* ---------------------------------------------------------------------- */
/* spawnSync: its own loop, run until the child and its pipes are done */

typedef struct SyncPipe {
  uv_pipe_t pipe;
  struct SyncRunner *runner;
  bool readable, writable; /* from the child's side */
  uint8_t *input;
  size_t input_len;
  uint8_t *out;
  size_t out_len, out_cap;
  uv_write_t write_req;
  uv_shutdown_t shutdown_req;
  bool open;
} SyncPipe;

typedef struct SyncRunner {
  uv_loop_t loop;
  uv_process_t process;
  uv_timer_t timer;
  bool timer_on, process_open;
  SyncPipe *pipes[16];
  int npipes;
  int64_t exit_status;
  int term_signal;
  int error, pipe_error;
  double max_buffer;
  int kill_signal;
  bool killed;
} SyncRunner;

static void sync_set_error(SyncRunner *r, int e) {
  if (!r->error)
    r->error = e;
}

static void sync_close_pipes(SyncRunner *r) {
  int i;
  for (i = 0; i < r->npipes; i++)
    if (r->pipes[i] && r->pipes[i]->open) {
      r->pipes[i]->open = false;
      uv_close((uv_handle_t *)&r->pipes[i]->pipe, NULL);
    }
}

static void sync_kill(SyncRunner *r) {
  if (r->killed)
    return;
  r->killed = true;
  if (r->exit_status < 0 && r->process_open) {
    int e = uv_process_kill(&r->process, r->kill_signal);
    if (e < 0 && e != UV_ESRCH) {
      sync_set_error(r, e);
      uv_process_kill(&r->process, SIGKILL);
    }
  }
  sync_close_pipes(r);
  if (r->timer_on) {
    uv_timer_stop(&r->timer);
  }
}

static void sync_exit_cb(uv_process_t *h, int64_t status, int sig) {
  SyncRunner *r = h->data;
  r->exit_status = status < 0 ? 0 : status;
  r->term_signal = sig;
  uv_close((uv_handle_t *)h, NULL);
  r->process_open = false;
  if (r->timer_on) {
    uv_timer_stop(&r->timer);
    uv_close((uv_handle_t *)&r->timer, NULL);
    r->timer_on = false;
  }
}

static void sync_timeout_cb(uv_timer_t *t) {
  SyncRunner *r = t->data;
  sync_set_error(r, UV_ETIMEDOUT);
  sync_kill(r);
}

static void sync_alloc_cb(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
  SyncPipe *p = h->data;
  if (p->out_cap - p->out_len < 65536) {
    p->out_cap = p->out_cap ? p->out_cap * 2 : 65536;
    if (p->out_cap - p->out_len < 65536)
      p->out_cap = p->out_len + 65536;
    p->out = realloc(p->out, p->out_cap);
  }
  buf->base = (char *)p->out + p->out_len;
  buf->len = p->out_cap - p->out_len;
}

static void sync_read_cb(uv_stream_t *s, ssize_t nread, const uv_buf_t *buf) {
  SyncPipe *p = s->data;
  SyncRunner *r = p->runner;
  if (nread > 0) {
    p->out_len += nread;
    if (r->max_buffer > 0 && (double)p->out_len > r->max_buffer) {
      sync_set_error(r, UV_ENOBUFS);
      sync_kill(r);
    }
  } else if (nread < 0) {
    if (nread != UV_EOF && !r->pipe_error)
      r->pipe_error = (int)nread;
    if (p->open) {
      p->open = false;
      uv_close((uv_handle_t *)&p->pipe, NULL);
    }
  }
}

static void sync_shutdown_cb(uv_shutdown_t *req, int status) {
  SyncPipe *p = req->data;
  if (status < 0 && status != UV_ENOTCONN && !p->runner->pipe_error)
    p->runner->pipe_error = status;
  if (!p->writable && p->open) {
    p->open = false;
    uv_close((uv_handle_t *)&p->pipe, NULL);
  }
}

static void sync_write_cb(uv_write_t *req, int status) {
  SyncPipe *p = req->data;
  if (status < 0 && !p->runner->pipe_error)
    p->runner->pipe_error = status;
  p->shutdown_req.data = p;
  if (p->open)
    uv_shutdown(&p->shutdown_req, (uv_stream_t *)&p->pipe, sync_shutdown_cb);
}

static JSValue spawn_sync_fn(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  JSValueConst opts = ARG(0);
  SyncRunner *r = calloc(1, sizeof(*r));
  uv_process_options_t o;
  uv_stdio_container_t st[16];
  JSValue v, res, stdio_v, out;
  char *file = NULL, *cwd = NULL, **args = NULL, **envp = NULL;
  uint32_t n = 0, i;
  int err;
  double timeout = 0;

  memset(&o, 0, sizeof(o));
  memset(st, 0, sizeof(st));
  r->exit_status = -1;
  r->kill_signal = SIGTERM;
  uv_loop_init(&r->loop);

  v = JS_GetPropertyStr(ctx, opts, "file");
  file = node_string_to_utf8(ctx, v, NULL);
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, opts, "args");
  args = string_array(ctx, v);
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, opts, "cwd");
  if (JS_IsString(v))
    cwd = node_string_to_utf8(ctx, v, NULL);
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, opts, "envPairs");
  envp = string_array(ctx, v);
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, opts, "timeout");
  timeout = nb_double(ctx, v, 0);
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, opts, "maxBuffer");
  r->max_buffer = nb_double(ctx, v, 0);
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, opts, "killSignal");
  if (JS_IsNumber(v))
    r->kill_signal = nb_int32(ctx, v, SIGTERM);
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, opts, "detached");
  if (JS_ToBool(ctx, v))
    o.flags |= UV_PROCESS_DETACHED;
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, opts, "uid");
  if (JS_IsNumber(v)) {
    o.flags |= UV_PROCESS_SETUID;
    o.uid = nb_uint32(ctx, v, 0);
  }
  JS_FreeValue(ctx, v);
  v = JS_GetPropertyStr(ctx, opts, "gid");
  if (JS_IsNumber(v)) {
    o.flags |= UV_PROCESS_SETGID;
    o.gid = nb_uint32(ctx, v, 0);
  }
  JS_FreeValue(ctx, v);

  stdio_v = JS_GetPropertyStr(ctx, opts, "stdio");
  {
    JSValue lenv = JS_GetPropertyStr(ctx, stdio_v, "length");
    JS_ToUint32(ctx, &n, lenv);
  }
  if (n > 16)
    n = 16;
  for (i = 0; i < n; i++) {
    JSValue so = JS_GetPropertyUint32(ctx, stdio_v, i);
    JSValue type = JS_GetPropertyStr(ctx, so, "type");
    if (str_eq(ctx, type, "pipe")) {
      SyncPipe *p = calloc(1, sizeof(*p));
      JSValue rd = JS_GetPropertyStr(ctx, so, "readable"), wr = JS_GetPropertyStr(ctx, so, "writable");
      JSValue input = JS_GetPropertyStr(ctx, so, "input");
      size_t len;
      uint8_t *d;
      p->runner = r;
      p->readable = JS_ToBool(ctx, rd);
      p->writable = JS_ToBool(ctx, wr);
      if ((d = nb_buffer_data(ctx, input, &len)) != NULL && !JS_IsUndefined(input)) {
        p->input = malloc(len ? len : 1);
        memcpy(p->input, d, len);
        p->input_len = len;
      }
      JS_FreeValue(ctx, rd);
      JS_FreeValue(ctx, wr);
      JS_FreeValue(ctx, input);
      uv_pipe_init(&r->loop, &p->pipe, 0);
      p->pipe.data = p;
      p->open = true;
      r->pipes[i] = p;
      st[i].flags = UV_CREATE_PIPE | (p->readable ? UV_READABLE_PIPE : 0) |
                    (p->writable ? UV_WRITABLE_PIPE : 0);
      st[i].data.stream = (uv_stream_t *)&p->pipe;
    } else if (str_eq(ctx, type, "inherit") || str_eq(ctx, type, "fd")) {
      JSValue fd = JS_GetPropertyStr(ctx, so, "fd");
      st[i].flags = UV_INHERIT_FD;
      st[i].data.fd = nb_int32(ctx, fd, (int32_t)i);
      JS_FreeValue(ctx, fd);
    } else {
      st[i].flags = UV_IGNORE;
    }
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, so);
  }
  JS_FreeValue(ctx, stdio_v);
  r->npipes = n;

  o.exit_cb = sync_exit_cb;
  o.file = file;
  o.args = args;
  o.cwd = cwd && cwd[0] ? cwd : NULL;
  o.env = envp;
  o.stdio = st;
  o.stdio_count = n;
  r->process.data = r;
  err = uv_spawn(&r->loop, &r->process, &o);
  if (err) {
    sync_set_error(r, err);
    uv_close((uv_handle_t *)&r->process, NULL);
    sync_close_pipes(r);
  } else {
    r->process_open = true;
    for (i = 0; i < n; i++) {
      SyncPipe *p = r->pipes[i];
      if (!p)
        continue;
      if (p->writable)
        uv_read_start((uv_stream_t *)&p->pipe, sync_alloc_cb, sync_read_cb);
      if (p->readable) {
        if (p->input_len) {
          uv_buf_t b = uv_buf_init((char *)p->input, (unsigned)p->input_len);
          p->write_req.data = p;
          uv_write(&p->write_req, (uv_stream_t *)&p->pipe, &b, 1, sync_write_cb);
        } else {
          p->shutdown_req.data = p;
          uv_shutdown(&p->shutdown_req, (uv_stream_t *)&p->pipe, sync_shutdown_cb);
        }
      }
    }
    if (timeout > 0) {
      uv_timer_init(&r->loop, &r->timer);
      r->timer.data = r;
      uv_timer_start(&r->timer, sync_timeout_cb, (uint64_t)timeout, 0);
      r->timer_on = true;
    }
  }
  uv_run(&r->loop, UV_RUN_DEFAULT);
  if (r->timer_on) {
    uv_close((uv_handle_t *)&r->timer, NULL);
    r->timer_on = false;
  }
  sync_close_pipes(r);
  uv_run(&r->loop, UV_RUN_DEFAULT);
  uv_loop_close(&r->loop);

  res = JS_NewObject(ctx);
  if (r->error || r->pipe_error)
    JS_SetPropertyStr(ctx, res, "error", JS_NewInt32(ctx, r->error ? r->error : r->pipe_error));
  if (r->exit_status >= 0 && r->term_signal == 0)
    JS_SetPropertyStr(ctx, res, "status", JS_NewFloat64(ctx, (double)r->exit_status));
  else
    JS_SetPropertyStr(ctx, res, "status", JS_NULL);
  JS_SetPropertyStr(ctx, res, "signal", r->term_signal > 0
                                           ? JS_NewString(ctx, node_signo_string(r->term_signal))
                                           : JS_NULL);
  if (r->exit_status >= 0) {
    out = JS_NewArray(ctx);
    for (i = 0; i < n; i++) {
      SyncPipe *p = r->pipes[i];
      if (!p || !p->writable)
        JS_SetPropertyUint32(ctx, out, i, JS_NULL);
      else
        JS_SetPropertyUint32(ctx, out, i, nb_new_buffer(ctx, p->out, p->out_len));
    }
    JS_SetPropertyStr(ctx, res, "output", out);
  } else {
    JS_SetPropertyStr(ctx, res, "output", JS_NULL);
  }
  JS_SetPropertyStr(ctx, res, "pid", JS_NewInt32(ctx, r->process.pid));

  for (i = 0; i < n; i++)
    if (r->pipes[i]) {
      free(r->pipes[i]->input);
      free(r->pipes[i]->out);
      free(r->pipes[i]);
    }
  free(file);
  free(cwd);
  free_strings(args);
  free_strings(envp);
  free(r);
  return res;
}

JSValue binding_init_spawn_sync(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "spawn", spawn_sync_fn, 1);
  return t;
}

/* ---------------------------------------------------------------------- */
/* Signal */

typedef struct {
  HandleWrap hw;
  uv_signal_t signal;
  bool active;
} SignalWrap;

static void signal_cb(uv_signal_t *h, int signum) {
  SignalWrap *s = h->data;
  JSContext *ctx = s->hw.aw.env->ctx;
  JSValue arg = JS_NewInt32(ctx, signum), r;
  r = async_wrap_make_callback_name(&s->hw.aw, "onsignal", 1, (JSValueConst *)&arg);
  JS_FreeValue(ctx, r);
}

static void signal_finalizer(JSRuntime *rt, JSValueConst val) {
  SignalWrap *s = JS_GetOpaque(val, node_signal_class_id);
  if (!s)
    return;
  async_wrap_destroy(&s->hw.aw);
  if (s->hw.state == HW_INITIALIZED) {
    node_close_and_free((uv_handle_t *)&s->signal, s);
    return;
  }
  free(s);
}

static JSValue signal_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue obj = nb_new_instance(ctx, nt, node_signal_class_id);
  SignalWrap *s;
  if (JS_IsException(obj))
    return obj;
  s = calloc(1, sizeof(*s));
  uv_signal_init(env->loop, &s->signal);
  JS_SetOpaque(obj, s);
  node_handle_wrap_init(&s->hw, env, obj, (uv_handle_t *)&s->signal, PROVIDER_SIGNALWRAP);
  return obj;
}

static JSValue signal_start(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  SignalWrap *s = JS_GetOpaque2(ctx, this_val, node_signal_class_id);
  int32_t signum = nb_int32(ctx, ARG(0), 0);
  int err;
  if (!s)
    return JS_EXCEPTION;
  if (s->hw.state != HW_INITIALIZED)
    return JS_NewInt32(ctx, UV_EBADF);
  err = uv_signal_start(&s->signal, signal_cb, signum);
  if (err == 0)
    s->active = true;
  return JS_NewInt32(ctx, err);
}

static JSValue signal_stop(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  SignalWrap *s = JS_GetOpaque2(ctx, this_val, node_signal_class_id);
  if (!s)
    return JS_EXCEPTION;
  if (s->hw.state != HW_INITIALIZED)
    return JS_NewInt32(ctx, UV_EBADF);
  s->active = false;
  return JS_NewInt32(ctx, uv_signal_stop(&s->signal));
}

static const JSCFunctionListEntry signal_proto[] = {
  JS_CFUNC_DEF("start", 1, signal_start),
  JS_CFUNC_DEF("stop", 0, signal_stop),
};

JSValue binding_init_signal_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), ctor, proto;
  NodeClassDef def = { .name = "Signal", .class_id = &node_signal_class_id,
                       .ctor = signal_ctor, .finalizer = signal_finalizer,
                       .proto_funcs = signal_proto, .proto_funcs_count = countof(signal_proto),
                       .parent_ctor = JS_UNDEFINED };
  ctor = nb_define_class(ctx, t, &def);
  proto = JS_GetPropertyStr(ctx, ctor, "prototype");
  JS_SetPropertyFunctionList(ctx, proto, node_handle_wrap_funcs, node_handle_wrap_funcs_count);
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, ctor);
  return t;
}
