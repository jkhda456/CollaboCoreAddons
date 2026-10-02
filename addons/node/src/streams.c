/* Handles and streams over libuv: internalBinding('uv') (uv.cc),
 * ('stream_wrap') (stream_wrap.cc, stream_base.cc, handle_wrap.cc),
 * ('tty_wrap'), ('pipe_wrap') and ('tcp_wrap') with their connect/write/
 * shutdown requests. */
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "node.h"
#include "streams.h"

void *node_handle_track(AsyncWrap *aw);
void node_handle_untrack(void *entry);

JSClassID node_tty_class_id, node_pipe_class_id, node_tcp_class_id;
static JSClassID write_wrap_class_id, shutdown_wrap_class_id;
static JSClassID tcp_connect_wrap_class_id, pipe_connect_wrap_class_id;
static JSClassID stream_wrap_base_class_id;

/* ---------------------------------------------------------------------- */
/* uv */

static JSValue uv_errname_fn(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  int32_t err = nb_int32(ctx, ARG(0), 0);
  if (err >= 0)
    return node_throw_type_error(ctx, "ERR_OUT_OF_RANGE", "err >= 0");
  return JS_NewString(ctx, uv_err_name(err));
}

static JSValue uv_get_error_message(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  int32_t err = nb_int32(ctx, ARG(0), 0);
  return JS_NewString(ctx, uv_strerror(err));
}

static JSValue uv_get_error_map(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  JSValue ctor = JS_GetPropertyStr(ctx, env_get(ctx)->global, "Map");
  JSValue map = JS_CallConstructor(ctx, ctor, 0, NULL), set;
  JS_FreeValue(ctx, ctor);
  set = JS_GetPropertyStr(ctx, map, "set");
#define V(name, msg)                                                          \
  {                                                                           \
    JSValue args[2], arr = JS_NewArray(ctx), r;                               \
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, #name));              \
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewString(ctx, msg));                \
    args[0] = JS_NewInt32(ctx, UV_##name);                                    \
    args[1] = arr;                                                            \
    r = JS_Call(ctx, set, map, 2, (JSValueConst *)args);                      \
    JS_FreeValue(ctx, r);                                                     \
    JS_FreeValue(ctx, arr);                                                   \
  }
  UV_ERRNO_MAP(V)
#undef V
  JS_FreeValue(ctx, set);
  return map;
}

JSValue binding_init_uv(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "errname", uv_errname_fn, 1);
  nb_set_method(ctx, t, "getErrorMap", uv_get_error_map, 0);
  nb_set_method(ctx, t, "getErrorMessage", uv_get_error_message, 1);
#define V(name, _) nb_define_readonly(ctx, t, "UV_" #name, JS_NewInt32(ctx, UV_##name));
  UV_ERRNO_MAP(V)
#undef V
  return t;
}

/* ---------------------------------------------------------------------- */
/* HandleWrap */

static void handle_closed(uv_handle_t *h) {
  HandleWrap *w = h->data;
  Env *env = w->aw.env;
  JSContext *ctx = env->ctx;
  w->state = HW_CLOSED;
  node_handle_untrack(w->track);
  w->track = NULL;
  if (env->can_call_into_js) {
    JSValue sym = env_get_symbol(env, "handle_onclose_symbol"), fn;
    JSAtom a = JS_ValueToAtom(ctx, sym);
    fn = JS_GetProperty(ctx, w->aw.object, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, sym);
    if (JS_IsFunction(ctx, fn)) {
      JSValue r = async_wrap_make_callback(&w->aw, fn, 0, NULL);
      JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
  }
  if (w->on_closed)
    w->on_closed(w);
  async_wrap_unref(&w->aw);  /* the open handle's reference */
}

void node_handle_wrap_init(HandleWrap *w, Env *env, JSValueConst obj, uv_handle_t *h,
                           ProviderType provider) {
  async_wrap_init(&w->aw, env, obj, provider, -1);
  w->handle = h;
  h->data = w;
  w->state = HW_INITIALIZED;
  w->track = node_handle_track(&w->aw);
  async_wrap_ref(&w->aw);
}

void node_handle_wrap_close(HandleWrap *w, JSValueConst cb) {
  JSContext *ctx = w->aw.env->ctx;
  if (w->state != HW_INITIALIZED)
    return;
  uv_close(w->handle, handle_closed);
  w->state = HW_CLOSING;
  if (JS_IsFunction(ctx, cb)) {
    JSValue sym = env_get_symbol(w->aw.env, "handle_onclose_symbol");
    JSAtom a = JS_ValueToAtom(ctx, sym);
    JS_SetProperty(ctx, w->aw.object, a, JS_DupValue(ctx, cb));
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, sym);
  }
}

HandleWrap *node_handle_wrap_of(JSValueConst obj) {
  JSClassID id;
  void *p = JS_GetAnyOpaque(obj, &id);
  if (!p)
    return NULL;
  if (id == node_tty_class_id || id == node_pipe_class_id || id == node_tcp_class_id ||
      id == node_process_class_id || id == node_signal_class_id ||
      id == node_fs_event_class_id || id == node_udp_class_id)
    return p;
  return NULL;
}

static void free_handle_data(uv_handle_t *h) {
  free(h->data);
}

void node_close_and_free(uv_handle_t *h, void *container) {
  /* already closed or closing: only when a worker's loop closed everything
     as it ended (the loop may still refer to it, so it stays) */
  if (uv_is_closing(h))
    return;
  h->data = container;
  uv_close(h, free_handle_data);
}

/* stream classes outside this file (TLSWrap, Http2Stream) */
static JSClassID extra_stream_classes[8];
static int n_extra_stream_classes;

void node_register_stream_class(JSClassID id) {
  int i;
  for (i = 0; i < n_extra_stream_classes; i++)
    if (extra_stream_classes[i] == id)
      return;
  if (n_extra_stream_classes < (int)countof(extra_stream_classes))
    extra_stream_classes[n_extra_stream_classes++] = id;
}

StreamWrap *node_stream_wrap_of(JSValueConst obj) {
  JSClassID id;
  void *p = JS_GetAnyOpaque(obj, &id);
  int i;
  if (!p)
    return NULL;
  if (id == node_tty_class_id || id == node_pipe_class_id || id == node_tcp_class_id)
    return p;
  for (i = 0; i < n_extra_stream_classes; i++)
    if (id == extra_stream_classes[i])
      return p;
  return NULL;
}

static JSValue hw_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  HandleWrap *w = node_handle_wrap_of(this_val);
  if (w)
    node_handle_wrap_close(w, ARG(0));
  return JS_UNDEFINED;
}

static JSValue hw_ref(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  HandleWrap *w = node_handle_wrap_of(this_val);
  if (w && w->state == HW_INITIALIZED)
    uv_ref(w->handle);
  return JS_UNDEFINED;
}

static JSValue hw_unref(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  HandleWrap *w = node_handle_wrap_of(this_val);
  if (w && w->state == HW_INITIALIZED)
    uv_unref(w->handle);
  return JS_UNDEFINED;
}

static JSValue hw_has_ref(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  HandleWrap *w = node_handle_wrap_of(this_val);
  return JS_NewBool(ctx, w && w->state == HW_INITIALIZED && uv_has_ref(w->handle));
}

static JSValue hw_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  HandleWrap *w = node_handle_wrap_of(this_val);
  return JS_NewFloat64(ctx, w ? w->aw.async_id : -1);
}

static JSValue hw_get_provider_type(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  HandleWrap *w = node_handle_wrap_of(this_val);
  return JS_NewInt32(ctx, w ? w->aw.provider : 0);
}

static JSValue hw_async_reset(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  HandleWrap *w = node_handle_wrap_of(this_val);
  if (w)
    async_wrap_reset(&w->aw, ARG(0));
  return JS_UNDEFINED;
}

const JSCFunctionListEntry node_handle_wrap_funcs[] = {
  JS_CFUNC_DEF("close", 1, hw_close),
  JS_CFUNC_DEF("ref", 0, hw_ref),
  JS_CFUNC_DEF("unref", 0, hw_unref),
  JS_CFUNC_DEF("hasRef", 0, hw_has_ref),
  JS_CFUNC_DEF("getAsyncId", 0, hw_get_async_id),
  JS_CFUNC_DEF("getProviderType", 0, hw_get_provider_type),
  JS_CFUNC_DEF("asyncReset", 1, hw_async_reset),
};
const int node_handle_wrap_funcs_count = countof(node_handle_wrap_funcs);

/* ---------------------------------------------------------------------- */
/* requests: WriteWrap, ShutdownWrap, connect wraps (JS-created objects) */

typedef struct {
  AsyncWrap aw;
} ReqWrap;

static void req_wrap_finalizer(JSRuntime *rt, JSValueConst val) {
  JSClassID id;
  ReqWrap *r = JS_GetAnyOpaque(val, &id);
  if (r) {
    async_wrap_destroy(&r->aw);
    free(r);
  }
}

static JSValue req_wrap_new(JSContext *ctx, JSValueConst new_target, JSClassID id,
                            ProviderType provider) {
  JSValue obj = nb_new_instance(ctx, new_target, id);
  ReqWrap *r;
  if (JS_IsException(obj))
    return obj;
  r = calloc(1, sizeof(*r));
  async_wrap_init(&r->aw, env_get(ctx), obj, provider, -1);
  JS_SetOpaque(obj, r);
  return obj;
}

static JSValue write_wrap_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return req_wrap_new(ctx, nt, write_wrap_class_id, PROVIDER_WRITEWRAP);
}
static JSValue shutdown_wrap_ctor(JSContext *ctx, JSValueConst nt, int argc,
                                  JSValueConst *argv) {
  return req_wrap_new(ctx, nt, shutdown_wrap_class_id, PROVIDER_SHUTDOWNWRAP);
}
static JSValue tcp_connect_wrap_ctor(JSContext *ctx, JSValueConst nt, int argc,
                                     JSValueConst *argv) {
  return req_wrap_new(ctx, nt, tcp_connect_wrap_class_id, PROVIDER_TCPCONNECTWRAP);
}
static JSValue pipe_connect_wrap_ctor(JSContext *ctx, JSValueConst nt, int argc,
                                      JSValueConst *argv) {
  return req_wrap_new(ctx, nt, pipe_connect_wrap_class_id, PROVIDER_PIPECONNECTWRAP);
}

static JSValue req_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  JSClassID id;
  ReqWrap *r = JS_GetAnyOpaque(this_val, &id);
  return JS_NewFloat64(ctx, r ? r->aw.async_id : -1);
}

static const JSCFunctionListEntry req_wrap_proto[] = {
  JS_CFUNC_DEF("getAsyncId", 0, req_get_async_id),
};

static ReqWrap *req_of(JSValueConst obj, JSClassID want) {
  return JS_GetOpaque(obj, want);
}

/* calls req.oncomplete(args) in the request's async context */
static void req_complete(Env *env, JSValueConst req_obj, int argc, JSValueConst *argv) {
  JSContext *ctx = env->ctx;
  JSClassID id;
  ReqWrap *r = JS_GetAnyOpaque(req_obj, &id);
  JSValue fn = JS_GetPropertyStr(ctx, req_obj, "oncomplete"), ret;
  if (!JS_IsFunction(ctx, fn)) {
    JS_FreeValue(ctx, fn);
    return;
  }
  if (r)
    ret = node_make_callback(env, req_obj, req_obj, fn, argc, argv, r->aw.async_id,
                             r->aw.trigger_async_id, r->aw.context_frame);
  else
    ret = node_make_callback(env, req_obj, req_obj, fn, argc, argv, 0, 0, JS_UNDEFINED);
  JS_FreeValue(ctx, ret);
  JS_FreeValue(ctx, fn);
}

void node_req_complete(Env *env, JSValueConst req_obj, int argc, JSValueConst *argv) {
  req_complete(env, req_obj, argc, argv);
}

/* ---------------------------------------------------------------------- */
/* LibuvStreamWrap */

typedef struct {
  uv_write_t req;
  Env *env;
  JSValue req_obj;
  JSValue handle_obj;
  JSValue keep;     /* buffers referenced by bufs */
  char *owned;      /* copied bytes */
  StreamWrap *stream;
  size_t bytes;
} WriteReq;

static void after_write(uv_write_t *req, int status) {
  WriteReq *w = req->data;
  Env *env = w->env;
  JSContext *ctx = env->ctx;
  JSValue args[3];
  if (w->stream)
    w->stream->bytes_written += w->bytes;
  args[0] = JS_NewInt32(ctx, status);
  args[1] = w->handle_obj;
  args[2] = JS_UNDEFINED;
  if (w->stream && w->stream->after_write_hook)
    w->stream->after_write_hook(w->stream, status);
  req_complete(env, w->req_obj, 3, (JSValueConst *)args);
  JS_FreeValue(ctx, w->req_obj);
  JS_FreeValue(ctx, w->handle_obj);
  JS_FreeValue(ctx, w->keep);
  free(w->owned);
  free(w);
}

/* writes bufs: tries a synchronous write first, the rest asynchronously;
   sets streamBaseState and returns 0 or an error */
int node_stream_write(StreamWrap *s, JSValueConst req_obj, uv_buf_t *bufs, unsigned nbufs,
                      JSValueConst keep, bool copy) {
  Env *env = s->hw.aw.env;
  JSContext *ctx = env->ctx;
  size_t total = 0, written = 0;
  unsigned i;
  int err;
  WriteReq *w;
  if (s->write_override)
    return s->write_override(s, req_obj, bufs, nbufs, keep);
  for (i = 0; i < nbufs; i++)
    total += bufs[i].len;
  env->stream_base_state[kLastWriteWasAsync] = 0;
  if (s->hw.state != HW_INITIALIZED) {
    env->stream_base_state[kBytesWritten] = 0;
    return UV_EBADF;
  }
  err = uv_try_write(s->stream, bufs, nbufs);
  if (err >= 0) {
    written = err;
    err = 0;
  } else if (err != UV_EAGAIN && err != UV_ENOSYS) {
    env->stream_base_state[kBytesWritten] = 0;
    return err;
  } else {
    err = 0;
  }
  s->bytes_written += written;
  env->stream_base_state[kBytesWritten] = (int32_t)total;
  if (written == total)
    return 0;
  /* skip what went out */
  while (nbufs > 0 && written >= bufs[0].len) {
    written -= bufs[0].len;
    bufs++;
    nbufs--;
  }
  if (nbufs > 0 && written > 0) {
    bufs[0].base += written;
    bufs[0].len -= written;
  }
  w = calloc(1, sizeof(*w));
  w->env = env;
  w->req.data = w;
  w->stream = s;
  w->req_obj = JS_DupValue(ctx, req_obj);
  w->handle_obj = JS_DupValue(ctx, s->hw.aw.object);
  w->keep = JS_DupValue(ctx, keep);
  for (i = 0; i < nbufs; i++)
    w->bytes += bufs[i].len;
  if (copy) {
    size_t n = w->bytes, off = 0;
    w->owned = malloc(n ? n : 1);
    for (i = 0; i < nbufs; i++) {
      memcpy(w->owned + off, bufs[i].base, bufs[i].len);
      off += bufs[i].len;
    }
    {
      uv_buf_t one = uv_buf_init(w->owned, (unsigned)n);
      err = uv_write(&w->req, s->stream, &one, 1, after_write);
    }
  } else {
    err = uv_write(&w->req, s->stream, bufs, nbufs, after_write);
  }
  if (err) {
    JS_FreeValue(ctx, w->req_obj);
    JS_FreeValue(ctx, w->handle_obj);
    JS_FreeValue(ctx, w->keep);
    free(w->owned);
    free(w);
    return err;
  }
  env->stream_base_state[kLastWriteWasAsync] = 1;
  return 0;
}

static JSValue sw_write_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  size_t len;
  uint8_t *data;
  uv_buf_t buf;
  if (!s)
    return JS_NewInt32(ctx, UV_EBADF);
  data = nb_buffer_data(ctx, ARG(1), &len);
  if (!data)
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "Second argument must be a buffer");
  buf = uv_buf_init((char *)data, (unsigned)len);
  return JS_NewInt32(ctx, node_stream_write(s, ARG(0), &buf, 1, ARG(1), false));
}

static JSValue sw_write_string(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int enc) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  size_t cap, len;
  char *data;
  uv_buf_t buf;
  int r;
  if (!s)
    return JS_NewInt32(ctx, UV_EBADF);
  if (!JS_IsString(ARG(1)))
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "Second argument must be a string");
  cap = node_string_bytes_size(ctx, ARG(1), enc);
  data = malloc(cap + 1);
  len = node_string_write(ctx, (uint8_t *)data, cap, ARG(1), enc, NULL);
  buf = uv_buf_init(data, (unsigned)len);
  r = node_stream_write(s, ARG(0), &buf, 1, JS_UNDEFINED, true);
  free(data);
  return JS_NewInt32(ctx, r);
}

/* writev(req, chunks, allBuffers) */
static JSValue sw_writev(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  bool all_buffers = JS_ToBool(ctx, ARG(2));
  JSValue lenv;
  uint32_t n = 0, i, count;
  uv_buf_t *bufs;
  char **owned;
  int r;
  if (!s)
    return JS_NewInt32(ctx, UV_EBADF);
  lenv = JS_GetPropertyStr(ctx, ARG(1), "length");
  JS_ToUint32(ctx, &n, lenv);
  count = all_buffers ? n : n / 2;
  bufs = calloc(count ? count : 1, sizeof(uv_buf_t));
  owned = calloc(count ? count : 1, sizeof(char *));
  for (i = 0; i < count; i++) {
    JSValue chunk = JS_GetPropertyUint32(ctx, ARG(1), all_buffers ? i : 2 * i);
    size_t len;
    uint8_t *d = nb_buffer_data(ctx, chunk, &len);
    if (d && !JS_IsString(chunk)) {
      bufs[i] = uv_buf_init((char *)d, (unsigned)len);
    } else {
      JSValue encv = JS_GetPropertyUint32(ctx, ARG(1), 2 * i + 1);
      int enc = node_parse_encoding(ctx, encv, ENC_UTF8);
      size_t cap = node_string_bytes_size(ctx, chunk, enc);
      JS_FreeValue(ctx, encv);
      owned[i] = malloc(cap + 1);
      len = node_string_write(ctx, (uint8_t *)owned[i], cap, chunk, enc, NULL);
      bufs[i] = uv_buf_init(owned[i], (unsigned)len);
    }
    JS_FreeValue(ctx, chunk);
  }
  r = node_stream_write(s, ARG(0), bufs, count, ARG(1), true);
  for (i = 0; i < count; i++)
    free(owned[i]);
  free(owned);
  free(bufs);
  return JS_NewInt32(ctx, r);
}

/* ---- reading ---- */

static void on_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
  HandleWrap *hw = h->data;
  StreamWrap *s = (StreamWrap *)hw;
  if (s->alloc_override) {
    s->alloc_override(s, suggested, buf);
    return;
  }
  (void)suggested;
  buf->base = malloc(65536);
  buf->len = buf->base ? 65536 : 0;
}

static void *ab_realloc(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
  if (size == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, size);
}

/* to whoever reads s: a native consumer (read_override: an HTTP/2 session,
   the HTTP parser) or JS (onread); data is malloc'ed and taken */
void node_stream_emit_read(StreamWrap *s, ssize_t nread, char *data) {
  if (s->read_override) {
    uv_buf_t b = uv_buf_init(data, nread > 0 ? (unsigned)nread : 0);
    s->read_override(s, nread, &b);
    return;
  }
  node_stream_emit_read_js(s, nread, data);
}

/* to JS's onread, whatever consumes s */
void node_stream_emit_read_js(StreamWrap *s, ssize_t nread, char *data) {
  Env *env = s->hw.aw.env;
  JSContext *ctx = env->ctx;
  JSValue ab = JS_UNDEFINED, onread, r;
  env->stream_base_state[kReadBytesOrError] = (int32_t)nread;
  env->stream_base_state[kArrayBufferOffset] = 0;
  if (nread > 0) {
    char *p = realloc(data, nread);
    ab = JS_NewArrayBuffer(ctx, (uint8_t *)(p ? p : data), nread, 0, ab_realloc, NULL, false);
    s->bytes_read += nread;
  } else {
    free(data);
  }
  onread = JS_GetPropertyStr(ctx, s->hw.aw.object, "onread");
  if (JS_IsFunction(ctx, onread)) {
    r = node_make_callback(env, s->hw.aw.object, s->hw.aw.object, onread, 1,
                           (JSValueConst *)&ab, s->hw.aw.async_id, s->hw.aw.trigger_async_id,
                           s->hw.aw.context_frame);
    JS_FreeValue(ctx, r);
  }
  JS_FreeValue(ctx, onread);
  JS_FreeValue(ctx, ab);
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  HandleWrap *hw = stream->data;
  StreamWrap *s = (StreamWrap *)hw;
  if (s->read_override) {
    s->read_override(s, nread, buf);
    return;
  }
  if (nread == 0) {
    free(buf->base);
    return;
  }
  if (nread < 0) {
    free(buf->base);
    node_stream_emit_read(s, nread, NULL);
    return;
  }
  if (s->pending_handles_cb && hw->handle->type == UV_NAMED_PIPE &&
      ((uv_pipe_t *)stream)->ipc && uv_pipe_pending_count((uv_pipe_t *)stream) > 0)
    s->pending_handles_cb(s);
  node_stream_emit_read(s, nread, buf->base);
}

int node_stream_read_start(StreamWrap *s) {
  if (s->hw.state != HW_INITIALIZED)
    return UV_EBADF;
  if (s->read_start_override)
    return s->read_start_override(s);
  s->reading = true;
  return uv_read_start(s->stream, on_alloc, on_read);
}

int node_stream_read_stop(StreamWrap *s) {
  if (s->hw.state != HW_INITIALIZED)
    return UV_EBADF;
  if (s->read_stop_override)
    return s->read_stop_override(s);
  s->reading = false;
  return s->stream ? uv_read_stop(s->stream) : 0;
}

static JSValue sw_read_start(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  return JS_NewInt32(ctx, s ? node_stream_read_start(s) : UV_EBADF);
}

static JSValue sw_read_stop(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  return JS_NewInt32(ctx, s ? node_stream_read_stop(s) : UV_EBADF);
}

typedef struct {
  uv_shutdown_t req;
  Env *env;
  JSValue req_obj, handle_obj;
} ShutdownReq;

static void after_shutdown(uv_shutdown_t *req, int status) {
  ShutdownReq *sr = req->data;
  JSContext *ctx = sr->env->ctx;
  JSValue args[2];
  args[0] = JS_NewInt32(ctx, status);
  args[1] = sr->handle_obj;
  req_complete(sr->env, sr->req_obj, 2, (JSValueConst *)args);
  JS_FreeValue(ctx, sr->req_obj);
  JS_FreeValue(ctx, sr->handle_obj);
  free(sr);
}

/* shutdown of the libuv stream; req.oncomplete(status, handle) when done */
int node_stream_shutdown(StreamWrap *s, JSValueConst req_obj) {
  JSContext *ctx = s->hw.aw.env->ctx;
  ShutdownReq *sr;
  int err;
  if (s->shutdown_override)
    return s->shutdown_override(s, req_obj);
  if (s->hw.state != HW_INITIALIZED || !s->stream)
    return UV_EBADF;
  sr = calloc(1, sizeof(*sr));
  sr->env = s->hw.aw.env;
  sr->req.data = sr;
  sr->req_obj = JS_DupValue(ctx, req_obj);
  sr->handle_obj = JS_DupValue(ctx, s->hw.aw.object);
  err = uv_shutdown(&sr->req, s->stream, after_shutdown);
  if (err) {
    JS_FreeValue(ctx, sr->req_obj);
    JS_FreeValue(ctx, sr->handle_obj);
    free(sr);
  }
  return err;
}

static JSValue sw_shutdown(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  if (!s)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, node_stream_shutdown(s, ARG(0)));
}

static JSValue sw_use_user_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  /* reads still go to fresh buffers: kBuffer streams copy out of them */
  return JS_UNDEFINED;
}

static JSValue sw_set_blocking(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  if (!s || s->hw.state != HW_INITIALIZED || !s->stream)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, uv_stream_set_blocking(s->stream, JS_ToBool(ctx, ARG(0))));
}

static JSValue sw_get_fd(JSContext *ctx, JSValueConst this_val) {
  HandleWrap *w = node_handle_wrap_of(this_val);
  uv_os_fd_t fd;
  if (!w || w->state != HW_INITIALIZED || !w->handle || uv_fileno(w->handle, &fd) != 0)
    return JS_NewInt32(ctx, -1);
  return JS_NewInt32(ctx, fd);
}

static JSValue sw_bytes_read(JSContext *ctx, JSValueConst this_val) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  return JS_NewFloat64(ctx, s ? (double)s->bytes_read : 0);
}

static JSValue sw_bytes_written(JSContext *ctx, JSValueConst this_val) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  return JS_NewFloat64(ctx, s ? (double)s->bytes_written : 0);
}

static JSValue sw_write_queue_size(JSContext *ctx, JSValueConst this_val) {
  StreamWrap *s = node_stream_wrap_of(this_val);
  if (!s || s->hw.state != HW_INITIALIZED || !s->stream)
    return JS_NewInt32(ctx, 0);
  return JS_NewFloat64(ctx, (double)s->stream->write_queue_size);
}

static JSValue sw_true_getter(JSContext *ctx, JSValueConst this_val) {
  return JS_TRUE;
}

static JSValue sw_external_stream(JSContext *ctx, JSValueConst this_val) {
  return JS_NewBigUint64(ctx, (uintptr_t)node_stream_wrap_of(this_val));
}

const JSCFunctionListEntry node_stream_base_funcs[] = {
  JS_CFUNC_DEF("readStart", 0, sw_read_start),
  JS_CFUNC_DEF("readStop", 0, sw_read_stop),
  JS_CFUNC_DEF("shutdown", 1, sw_shutdown),
  JS_CFUNC_DEF("useUserBuffer", 1, sw_use_user_buffer),
  JS_CFUNC_DEF("writev", 3, sw_writev),
  JS_CFUNC_DEF("writeBuffer", 2, sw_write_buffer),
  JS_CFUNC_MAGIC_DEF("writeAsciiString", 2, sw_write_string, ENC_ASCII),
  JS_CFUNC_MAGIC_DEF("writeUtf8String", 2, sw_write_string, ENC_UTF8),
  JS_CFUNC_MAGIC_DEF("writeUcs2String", 2, sw_write_string, ENC_UCS2),
  JS_CFUNC_MAGIC_DEF("writeLatin1String", 2, sw_write_string, ENC_LATIN1),
  JS_CFUNC_DEF("setBlocking", 1, sw_set_blocking),
  JS_CGETSET_DEF("fd", sw_get_fd, NULL),
  JS_CGETSET_DEF("bytesRead", sw_bytes_read, NULL),
  JS_CGETSET_DEF("bytesWritten", sw_bytes_written, NULL),
  JS_CGETSET_DEF("writeQueueSize", sw_write_queue_size, NULL),
  JS_CGETSET_DEF("isStreamBase", sw_true_getter, NULL),
  JS_CGETSET_DEF("_externalStream", sw_external_stream, NULL),
};
const int node_stream_base_funcs_count = countof(node_stream_base_funcs);

static JSValue base_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* the LibuvStreamWrap constructor; TCP, Pipe and TTY inherit from it */
static JSValue stream_base_ctor(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue ctor = JS_GetPropertyStr(ctx, env->binding_data, "LibuvStreamWrap");
  if (JS_IsFunction(ctx, ctor))
    return ctor;
  JS_FreeValue(ctx, ctor);
  {
    NodeClassDef def = { .name = "LibuvStreamWrap", .class_id = &stream_wrap_base_class_id,
                         .ctor = base_ctor, .proto_funcs = node_stream_base_funcs,
                         .proto_funcs_count = countof(node_stream_base_funcs),
                         .parent_ctor = JS_UNDEFINED };
    JSValue proto;
    ctor = nb_define_class(ctx, JS_UNDEFINED, &def);
    proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    JS_SetPropertyFunctionList(ctx, proto, node_handle_wrap_funcs, node_handle_wrap_funcs_count);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, env->binding_data, "LibuvStreamWrap", JS_DupValue(ctx, ctor));
  }
  return ctor;
}

JSValue node_stream_wrap_proto(Env *env) {
  JSValue c = stream_base_ctor(env), p = JS_GetPropertyStr(env->ctx, c, "prototype");
  JS_FreeValue(env->ctx, c);
  return p;
}

static void stream_finalizer(JSRuntime *rt, JSValueConst val) {
  JSClassID id;
  StreamWrap *s = JS_GetAnyOpaque(val, &id);
  if (!s)
    return;
  async_wrap_destroy(&s->hw.aw);
  if (s->hw.state == HW_INITIALIZED) {
    /* never closed: close it now and free when libuv is done */
    s->hw.aw.env = NULL;
    node_close_and_free(s->hw.handle, s);
    return;
  }
  free(s);
}

JSValue binding_init_stream_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), base;
  NodeClassDef wdef = { .name = "WriteWrap", .class_id = &write_wrap_class_id,
                        .ctor = write_wrap_ctor, .finalizer = req_wrap_finalizer,
                        .proto_funcs = req_wrap_proto, .proto_funcs_count = countof(req_wrap_proto),
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef sdef = { .name = "ShutdownWrap", .class_id = &shutdown_wrap_class_id,
                        .ctor = shutdown_wrap_ctor, .finalizer = req_wrap_finalizer,
                        .proto_funcs = req_wrap_proto, .proto_funcs_count = countof(req_wrap_proto),
                        .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &wdef));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &sdef));
  base = stream_base_ctor(env);
  nb_set(ctx, t, "LibuvStreamWrap", base);
  nb_set_int(ctx, t, "kReadBytesOrError", kReadBytesOrError);
  nb_set_int(ctx, t, "kArrayBufferOffset", kArrayBufferOffset);
  nb_set_int(ctx, t, "kBytesWritten", kBytesWritten);
  nb_set_int(ctx, t, "kLastWriteWasAsync", kLastWriteWasAsync);
  nb_set(ctx, t, "streamBaseState",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_INT32, env->stream_base_state,
                            kNumStreamBaseStateFields, 4));
  return t;
}

/* ---------------------------------------------------------------------- */
/* TTY */

typedef struct {
  StreamWrap s;
  uv_tty_t tty;
} TTYWrap;

static void set_uv_exception_info(JSContext *ctx, JSValueConst ctxobj, int err,
                                  const char *syscall) {
  if (!JS_IsObject(ctxobj))
    return;
  JS_SetPropertyStr(ctx, ctxobj, "errno", JS_NewInt32(ctx, err));
  JS_SetPropertyStr(ctx, ctxobj, "code", JS_NewString(ctx, uv_err_name(err)));
  JS_SetPropertyStr(ctx, ctxobj, "message", JS_NewString(ctx, uv_strerror(err)));
  JS_SetPropertyStr(ctx, ctxobj, "syscall", JS_NewString(ctx, syscall));
}

static JSValue tty_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue obj = nb_new_instance(ctx, nt, node_tty_class_id);
  TTYWrap *t;
  int32_t fd = nb_int32(ctx, ARG(0), -1);
  int err;
  if (JS_IsException(obj))
    return obj;
  t = calloc(1, sizeof(*t));
  err = uv_tty_init(env->loop, &t->tty, fd, 0);
  t->s.stream = (uv_stream_t *)&t->tty;
  JS_SetOpaque(obj, t);
  if (err) {
    /* like Node: the object exists, the error goes to ctx */
    async_wrap_init(&t->s.hw.aw, env, obj, PROVIDER_TTYWRAP, -1);
    t->s.hw.state = HW_CLOSED;
    t->s.hw.handle = (uv_handle_t *)&t->tty;
    set_uv_exception_info(ctx, ARG(1), err, "uv_tty_init");
    return obj;
  }
  node_handle_wrap_init(&t->s.hw, env, obj, (uv_handle_t *)&t->tty, PROVIDER_TTYWRAP);
  return obj;
}

static JSValue tty_get_window_size(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  TTYWrap *t = JS_GetOpaque(this_val, node_tty_class_id);
  int w = 0, h = 0, err;
  if (!t || t->s.hw.state != HW_INITIALIZED)
    return JS_NewInt32(ctx, UV_EBADF);
  err = uv_tty_get_winsize(&t->tty, &w, &h);
  if (err == 0) {
    JS_SetPropertyUint32(ctx, ARG(0), 0, JS_NewInt32(ctx, w));
    JS_SetPropertyUint32(ctx, ARG(0), 1, JS_NewInt32(ctx, h));
  }
  return JS_NewInt32(ctx, err);
}

static JSValue tty_set_raw_mode(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  TTYWrap *t = JS_GetOpaque(this_val, node_tty_class_id);
  int32_t mode = nb_int32(ctx, ARG(0), 0);
  if (!t || t->s.hw.state != HW_INITIALIZED)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, uv_tty_set_mode(&t->tty, (uv_tty_mode_t)mode));
}

static const JSCFunctionListEntry tty_proto[] = {
  JS_CFUNC_DEF("getWindowSize", 1, tty_get_window_size),
  JS_CFUNC_DEF("setRawMode", 1, tty_set_raw_mode),
};

static JSValue tty_is_tty(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  int32_t fd = nb_int32(ctx, ARG(0), -1);
  return JS_NewBool(ctx, fd >= 0 && uv_guess_handle(fd) == UV_TTY);
}

JSValue binding_init_tty_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), base = stream_base_ctor(env);
  NodeClassDef def = { .name = "TTY", .class_id = &node_tty_class_id, .ctor = tty_ctor,
                       .ctor_length = 2, .finalizer = stream_finalizer, .proto_funcs = tty_proto,
                       .proto_funcs_count = countof(tty_proto), .parent_ctor = base };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  JS_FreeValue(ctx, base);
  nb_set_method(ctx, t, "isTTY", tty_is_tty, 1);
  nb_set_int(ctx, t, "UV_TTY_MODE_NORMAL", UV_TTY_MODE_NORMAL);
  nb_set_int(ctx, t, "UV_TTY_MODE_RAW", UV_TTY_MODE_RAW);
  nb_set_int(ctx, t, "UV_TTY_MODE_IO", UV_TTY_MODE_IO);
#ifdef UV_TTY_MODE_RAW_VT
  nb_set_int(ctx, t, "UV_TTY_MODE_RAW_VT", UV_TTY_MODE_RAW_VT);
#else
  nb_set_int(ctx, t, "UV_TTY_MODE_RAW_VT", 3);
#endif
  return t;
}

/* ---------------------------------------------------------------------- */
/* connection wraps (TCP and Pipe) */

typedef struct {
  StreamWrap s;
  union {
    uv_tcp_t tcp;
    uv_pipe_t pipe;
  } h;
  int type;      /* SOCKET, SERVER, IPC */
  bool is_tcp;
} ConnWrap;

enum { CONN_SOCKET = 0, CONN_SERVER = 1, CONN_IPC = 2 };

static JSValue conn_instantiate(Env *env, bool tcp, int type);

static void on_connection(uv_stream_t *server, int status) {
  ConnWrap *srv = (ConnWrap *)server->data;
  Env *env = srv->s.hw.aw.env;
  JSContext *ctx = env->ctx;
  JSValue client = JS_UNDEFINED, args[2], r;
  if (status == 0) {
    ConnWrap *cw;
    client = conn_instantiate(env, srv->is_tcp, CONN_SOCKET);
    if (JS_IsException(client)) {
      node_trigger_uncaught_exception(env, JS_GetException(ctx), false);
      return;
    }
    cw = JS_GetOpaque(client, srv->is_tcp ? node_tcp_class_id : node_pipe_class_id);
    if (uv_accept(server, cw->s.stream)) {
      JS_FreeValue(ctx, client);
      return;
    }
  }
  args[0] = JS_NewInt32(ctx, status);
  args[1] = client;
  r = async_wrap_make_callback_name(&srv->s.hw.aw, "onconnection", 2, (JSValueConst *)args);
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, client);
}

typedef struct {
  uv_connect_t req;
  Env *env;
  JSValue req_obj, handle_obj;
} ConnectReq;

static void after_connect(uv_connect_t *req, int status) {
  ConnectReq *c = req->data;
  JSContext *ctx = c->env->ctx;
  bool readable = false, writable = false;
  JSValue args[5];
  if (!status) {
    readable = uv_is_readable(req->handle) != 0;
    writable = uv_is_writable(req->handle) != 0;
  }
  args[0] = JS_NewInt32(ctx, status);
  args[1] = c->handle_obj;
  args[2] = c->req_obj;
  args[3] = JS_NewBool(ctx, readable);
  args[4] = JS_NewBool(ctx, writable);
  req_complete(c->env, c->req_obj, 5, (JSValueConst *)args);
  JS_FreeValue(ctx, c->req_obj);
  JS_FreeValue(ctx, c->handle_obj);
  free(c);
}

static ConnectReq *connect_req(JSContext *ctx, JSValueConst req_obj, JSValueConst handle) {
  ConnectReq *c = calloc(1, sizeof(*c));
  c->env = env_get(ctx);
  c->req.data = c;
  c->req_obj = JS_DupValue(ctx, req_obj);
  c->handle_obj = JS_DupValue(ctx, handle);
  return c;
}

static void connect_req_free(JSContext *ctx, ConnectReq *c) {
  JS_FreeValue(ctx, c->req_obj);
  JS_FreeValue(ctx, c->handle_obj);
  free(c);
}

static ConnWrap *conn_of(JSValueConst obj) {
  JSClassID id;
  void *p = JS_GetAnyOpaque(obj, &id);
  if (p && (id == node_tcp_class_id || id == node_pipe_class_id))
    return p;
  return NULL;
}

static JSValue conn_listen(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  ConnWrap *c = conn_of(this_val);
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, uv_listen(c->s.stream, nb_int32(ctx, ARG(0), 511), on_connection));
}

static JSValue conn_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  ConnWrap *c = conn_of(this_val);
  int32_t fd = nb_int32(ctx, ARG(0), -1);
  int err;
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  if (c->is_tcp)
    err = uv_tcp_open(&c->h.tcp, fd);
  else
    err = uv_pipe_open(&c->h.pipe, fd);
  return JS_NewInt32(ctx, err);
}

/* ---- TCP ---- */

static int parse_addr(JSContext *ctx, JSValueConst host, int port, bool v6,
                      struct sockaddr_storage *ss) {
  const char *s = JS_ToCString(ctx, host);
  int err;
  if (!s)
    return UV_EINVAL;
  if (v6)
    err = uv_ip6_addr(s, port, (struct sockaddr_in6 *)ss);
  else
    err = uv_ip4_addr(s, port, (struct sockaddr_in *)ss);
  JS_FreeCString(ctx, s);
  return err;
}

static JSValue tcp_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                        int v6) {
  ConnWrap *c = JS_GetOpaque(this_val, node_tcp_class_id);
  struct sockaddr_storage ss;
  int32_t port = nb_int32(ctx, ARG(1), 0);
  uint32_t flags = v6 ? nb_uint32(ctx, ARG(2), 0) : 0;
  int err;
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  err = parse_addr(ctx, ARG(0), port, v6, &ss);
  if (err == 0)
    err = uv_tcp_bind(&c->h.tcp, (struct sockaddr *)&ss, flags);
  return JS_NewInt32(ctx, err);
}

static JSValue tcp_connect(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv, int v6) {
  ConnWrap *c = JS_GetOpaque(this_val, node_tcp_class_id);
  struct sockaddr_storage ss;
  int32_t port = nb_int32(ctx, ARG(2), 0);
  ConnectReq *r;
  int err;
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  if (!v6) {
    /* connect(req, address, port): the address decides the family */
    const char *s = JS_ToCString(ctx, ARG(1));
    v6 = s && strchr(s, ':') != NULL;
    JS_FreeCString(ctx, s);
  }
  err = parse_addr(ctx, ARG(1), port, v6, &ss);
  if (err)
    return JS_NewInt32(ctx, err);
  r = connect_req(ctx, ARG(0), this_val);
  err = uv_tcp_connect(&r->req, &c->h.tcp, (struct sockaddr *)&ss, after_connect);
  if (err)
    connect_req_free(ctx, r);
  return JS_NewInt32(ctx, err);
}

static JSValue tcp_bind4(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return tcp_bind(ctx, t, argc, argv, 0);
}
static JSValue tcp_bind6(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return tcp_bind(ctx, t, argc, argv, 1);
}
static JSValue tcp_connect4(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return tcp_connect(ctx, t, argc, argv, 0);
}
static JSValue tcp_connect6(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return tcp_connect(ctx, t, argc, argv, 1);
}

/* fills out with address, family, port (AddressToJS) */
void node_address_to_js(JSContext *ctx, const struct sockaddr *addr, JSValueConst out) {
  char ip[INET6_ADDRSTRLEN];
  int port = 0;
  const char *family = "IPv4";
  if (addr->sa_family == AF_INET6) {
    const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)addr;
    uv_ip6_name(a6, ip, sizeof(ip));
    port = ntohs(a6->sin6_port);
    family = "IPv6";
    JS_SetPropertyStr(ctx, out, "address", JS_NewString(ctx, ip));
    JS_SetPropertyStr(ctx, out, "family", JS_NewString(ctx, family));
    JS_SetPropertyStr(ctx, out, "port", JS_NewInt32(ctx, port));
    if (a6->sin6_flowinfo)
      JS_SetPropertyStr(ctx, out, "flowlabel", JS_NewUint32(ctx, ntohl(a6->sin6_flowinfo)));
    return;
  }
  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in *a4 = (const struct sockaddr_in *)addr;
    uv_ip4_name(a4, ip, sizeof(ip));
    port = ntohs(a4->sin_port);
    JS_SetPropertyStr(ctx, out, "address", JS_NewString(ctx, ip));
    JS_SetPropertyStr(ctx, out, "family", JS_NewString(ctx, family));
    JS_SetPropertyStr(ctx, out, "port", JS_NewInt32(ctx, port));
    return;
  }
  JS_SetPropertyStr(ctx, out, "address", JS_NewString(ctx, ""));
}

static JSValue tcp_sockname(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int peer) {
  ConnWrap *c = JS_GetOpaque(this_val, node_tcp_class_id);
  struct sockaddr_storage ss;
  int len = sizeof(ss), err;
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  err = peer ? uv_tcp_getpeername(&c->h.tcp, (struct sockaddr *)&ss, &len)
             : uv_tcp_getsockname(&c->h.tcp, (struct sockaddr *)&ss, &len);
  if (err == 0)
    node_address_to_js(ctx, (struct sockaddr *)&ss, ARG(0));
  return JS_NewInt32(ctx, err);
}

static JSValue tcp_getsockname(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return tcp_sockname(ctx, t, argc, argv, 0);
}
static JSValue tcp_getpeername(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return tcp_sockname(ctx, t, argc, argv, 1);
}

static JSValue tcp_set_no_delay(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  ConnWrap *c = JS_GetOpaque(this_val, node_tcp_class_id);
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, uv_tcp_nodelay(&c->h.tcp, JS_ToBool(ctx, ARG(0))));
}

static JSValue tcp_set_keep_alive(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  ConnWrap *c = JS_GetOpaque(this_val, node_tcp_class_id);
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, uv_tcp_keepalive(&c->h.tcp, JS_ToBool(ctx, ARG(0)),
                                           nb_uint32(ctx, ARG(1), 0)));
}

static JSValue tcp_set_simultaneous_accepts(JSContext *ctx, JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  return JS_NewInt32(ctx, 0);
}

static JSValue tcp_set_tos(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  return JS_NewInt32(ctx, 0);
}

static JSValue tcp_get_tos(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  return JS_NewInt32(ctx, 0);
}

static JSValue tcp_reset(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  ConnWrap *c = JS_GetOpaque(this_val, node_tcp_class_id);
  int err;
  if (!c || c->s.hw.state != HW_INITIALIZED)
    return JS_NewInt32(ctx, UV_EBADF);
  if (JS_IsFunction(ctx, ARG(0))) {
    JSValue sym = env_get_symbol(c->s.hw.aw.env, "handle_onclose_symbol");
    JSAtom a = JS_ValueToAtom(ctx, sym);
    JS_SetProperty(ctx, this_val, a, JS_DupValue(ctx, ARG(0)));
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, sym);
  }
  err = uv_tcp_close_reset(&c->h.tcp, handle_closed);
  if (err == 0)
    c->s.hw.state = HW_CLOSING;
  return JS_NewInt32(ctx, err);
}

static const JSCFunctionListEntry tcp_proto[] = {
  JS_CFUNC_DEF("open", 1, conn_open),
  JS_CFUNC_DEF("bind", 2, tcp_bind4),
  JS_CFUNC_DEF("listen", 1, conn_listen),
  JS_CFUNC_DEF("connect", 3, tcp_connect4),
  JS_CFUNC_DEF("bind6", 3, tcp_bind6),
  JS_CFUNC_DEF("connect6", 3, tcp_connect6),
  JS_CFUNC_DEF("getsockname", 1, tcp_getsockname),
  JS_CFUNC_DEF("getpeername", 1, tcp_getpeername),
  JS_CFUNC_DEF("setNoDelay", 1, tcp_set_no_delay),
  JS_CFUNC_DEF("setKeepAlive", 2, tcp_set_keep_alive),
  JS_CFUNC_DEF("setSimultaneousAccepts", 1, tcp_set_simultaneous_accepts),
  JS_CFUNC_DEF("setTypeOfService", 1, tcp_set_tos),
  JS_CFUNC_DEF("getTypeOfService", 0, tcp_get_tos),
  JS_CFUNC_DEF("reset", 1, tcp_reset),
};

static JSValue conn_new(JSContext *ctx, JSValueConst nt, bool tcp, int type) {
  Env *env = env_get(ctx);
  JSValue obj;
  ConnWrap *c;
  if (JS_IsUndefined(nt)) {
    JSValue proto = JS_GetClassProto(ctx, tcp ? node_tcp_class_id : node_pipe_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, tcp ? node_tcp_class_id : node_pipe_class_id);
    JS_FreeValue(ctx, proto);
  } else {
    obj = nb_new_instance(ctx, nt, tcp ? node_tcp_class_id : node_pipe_class_id);
  }
  if (JS_IsException(obj))
    return obj;
  c = calloc(1, sizeof(*c));
  c->is_tcp = tcp;
  c->type = type;
  if (tcp)
    uv_tcp_init(env->loop, &c->h.tcp);
  else
    uv_pipe_init(env->loop, &c->h.pipe, type == CONN_IPC);
  c->s.stream = (uv_stream_t *)&c->h;
  JS_SetOpaque(obj, c);
  node_handle_wrap_init(&c->s.hw, env, obj, (uv_handle_t *)&c->h,
                        tcp ? (type == CONN_SERVER ? PROVIDER_TCPSERVERWRAP : PROVIDER_TCPWRAP)
                            : (type == CONN_SERVER ? PROVIDER_PIPESERVERWRAP : PROVIDER_PIPEWRAP));
  return obj;
}

static JSValue conn_instantiate(Env *env, bool tcp, int type) {
  return conn_new(env->ctx, JS_UNDEFINED, tcp, type);
}

static JSValue tcp_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return conn_new(ctx, nt, true, nb_int32(ctx, ARG(0), CONN_SOCKET));
}

JSValue binding_init_tcp_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), base = stream_base_ctor(env), c = JS_NewObject(ctx);
  NodeClassDef def = { .name = "TCP", .class_id = &node_tcp_class_id, .ctor = tcp_ctor,
                       .ctor_length = 1, .finalizer = stream_finalizer, .proto_funcs = tcp_proto,
                       .proto_funcs_count = countof(tcp_proto), .parent_ctor = base };
  NodeClassDef cdef = { .name = "TCPConnectWrap", .class_id = &tcp_connect_wrap_class_id,
                        .ctor = tcp_connect_wrap_ctor, .finalizer = req_wrap_finalizer,
                        .proto_funcs = req_wrap_proto, .proto_funcs_count = countof(req_wrap_proto),
                        .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &cdef));
  JS_FreeValue(ctx, base);
  nb_set_int(ctx, c, "SOCKET", CONN_SOCKET);
  nb_set_int(ctx, c, "SERVER", CONN_SERVER);
  nb_set_int(ctx, c, "UV_TCP_IPV6ONLY", UV_TCP_IPV6ONLY);
  nb_set(ctx, t, "constants", c);
  return t;
}

/* ---- Pipe ---- */

static JSValue pipe_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  ConnWrap *c = JS_GetOpaque(this_val, node_pipe_class_id);
  char *name;
  int err;
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  name = node_string_to_utf8(ctx, ARG(0), NULL);
  err = uv_pipe_bind(&c->h.pipe, name);
  free(name);
  return JS_NewInt32(ctx, err);
}

static JSValue pipe_connect(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  ConnWrap *c = JS_GetOpaque(this_val, node_pipe_class_id);
  char *name;
  ConnectReq *r;
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  name = node_string_to_utf8(ctx, ARG(1), NULL);
  r = connect_req(ctx, ARG(0), this_val);
  uv_pipe_connect(&r->req, &c->h.pipe, name, after_connect);
  free(name);
  return JS_NewInt32(ctx, 0);
}

static JSValue pipe_fchmod(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  ConnWrap *c = JS_GetOpaque(this_val, node_pipe_class_id);
  if (!c)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, uv_pipe_chmod(&c->h.pipe, nb_int32(ctx, ARG(0), 0)));
}

static JSValue pipe_set_pending_instances(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry pipe_proto[] = {
  JS_CFUNC_DEF("open", 1, conn_open),
  JS_CFUNC_DEF("bind", 1, pipe_bind),
  JS_CFUNC_DEF("listen", 1, conn_listen),
  JS_CFUNC_DEF("connect", 2, pipe_connect),
  JS_CFUNC_DEF("fchmod", 1, pipe_fchmod),
  JS_CFUNC_DEF("setPendingInstances", 1, pipe_set_pending_instances),
};

static JSValue pipe_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return conn_new(ctx, nt, false, nb_int32(ctx, ARG(0), CONN_SOCKET));
}

JSValue node_new_pipe(Env *env, bool ipc) {
  return conn_instantiate(env, false, ipc ? CONN_IPC : CONN_SOCKET);
}

JSValue binding_init_pipe_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), base = stream_base_ctor(env), c = JS_NewObject(ctx);
  NodeClassDef def = { .name = "Pipe", .class_id = &node_pipe_class_id, .ctor = pipe_ctor,
                       .ctor_length = 1, .finalizer = stream_finalizer, .proto_funcs = pipe_proto,
                       .proto_funcs_count = countof(pipe_proto), .parent_ctor = base };
  NodeClassDef cdef = { .name = "PipeConnectWrap", .class_id = &pipe_connect_wrap_class_id,
                        .ctor = pipe_connect_wrap_ctor, .finalizer = req_wrap_finalizer,
                        .proto_funcs = req_wrap_proto, .proto_funcs_count = countof(req_wrap_proto),
                        .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &cdef));
  JS_FreeValue(ctx, base);
  nb_set_int(ctx, c, "SOCKET", CONN_SOCKET);
  nb_set_int(ctx, c, "SERVER", CONN_SERVER);
  nb_set_int(ctx, c, "IPC", CONN_IPC);
  nb_set_int(ctx, c, "UV_READABLE", UV_READABLE);
  nb_set_int(ctx, c, "UV_WRITABLE", UV_WRITABLE);
  nb_set(ctx, t, "constants", c);
  return t;
}
