/* internalBinding('stream_pipe') (stream_pipe.cc): StreamPipe(source, sink)
 * moves a source's data into a native stream without JS in between.  Node
 * uses it for http2's respondWithFD/respondWithFile, with a FileHandle as the
 * source; that is the source supported here.  One chunk is in flight at a
 * time: the next read starts when the sink has taken the previous write
 * (for an Http2Stream, when it went out on the socket). */
#include <stdlib.h>
#include <string.h>

#include "node.h"
#include "streams.h"

bool node_file_handle_read_info(JSValueConst obj, int *fd, int64_t *offset, int64_t *length);

#define PIPE_CHUNK (64 * 1024)

typedef struct {
  AsyncWrap aw;
  Env *env;
  JSValue source, sink;      /* kept alive while piping */
  StreamWrap *sink_stream;
  int fd;
  int64_t offset, remaining; /* -1: the file position / to the end */
  uv_fs_t read_req;
  char *buf;
  bool reading, writing, eof, closed, unpiped, started;
  int pending_writes;
} StreamPipe;

static JSClassID stream_pipe_class_id;

static void pipe_unpipe(StreamPipe *p);
static void pipe_read_next(StreamPipe *p);

/* the source's onread, with streamBaseState set as a stream's read would */
static void source_onread(StreamPipe *p, ssize_t nread) {
  Env *env = p->env;
  JSContext *ctx = env->ctx;
  JSValue fn = JS_GetPropertyStr(ctx, p->source, "onread"), arg = JS_UNDEFINED, r;
  env->stream_base_state[kReadBytesOrError] = (int32_t)nread;
  env->stream_base_state[kArrayBufferOffset] = 0;
  if (JS_IsFunction(ctx, fn)) {
    r = node_make_callback(env, p->source, p->source, fn, 1, (JSValueConst *)&arg, 0, 0,
                           JS_UNDEFINED);
    if (!JS_IsException(r) && !JS_IsUninitialized(r))
      JS_FreeValue(ctx, r);
  }
  JS_FreeValue(ctx, fn);
}

static void pipe_finish(StreamPipe *p) {
  JSContext *ctx = p->env->ctx;
  if (p->closed)
    return;
  p->closed = true;
  if (p->sink_stream) {
    JSValue req = JS_NewObject(ctx);
    int r = node_stream_shutdown(p->sink_stream, req);
    (void)r;
    JS_FreeValue(ctx, req);
  }
  pipe_unpipe(p);
}

static JSValue after_sink_write(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, int magic, JSValue *data) {
  StreamPipe *p = JS_GetOpaque(data[0], stream_pipe_class_id);
  int status = argc > 0 ? (int)nb_int32(ctx, argv[0], 0) : 0;
  if (!p)
    return JS_UNDEFINED;
  p->writing = false;
  p->pending_writes--;
  if (status < 0 || p->unpiped) {
    pipe_unpipe(p);
    return JS_UNDEFINED;
  }
  if (p->eof)
    pipe_finish(p);
  else
    pipe_read_next(p);
  return JS_UNDEFINED;
}

static void pipe_write(StreamPipe *p, size_t len) {
  Env *env = p->env;
  JSContext *ctx = env->ctx;
  JSValue req = JS_NewObject(ctx), fn;
  uv_buf_t b = uv_buf_init(p->buf, (unsigned)len);
  int err;
  fn = JS_NewCFunctionData(ctx, after_sink_write, 3, 0, 1, (JSValueConst *)&p->aw.object);
  JS_SetPropertyStr(ctx, req, "oncomplete", fn);
  p->writing = true;
  p->pending_writes++;
  env->stream_base_state[kLastWriteWasAsync] = 0;
  err = node_stream_write(p->sink_stream, req, &b, 1, JS_UNDEFINED, true);
  JS_FreeValue(ctx, req);
  if (err == 0 && env->stream_base_state[kLastWriteWasAsync])
    return; /* after_sink_write comes later */
  p->writing = false;
  p->pending_writes--;
  if (err < 0) {
    pipe_unpipe(p);
    return;
  }
  if (p->eof)
    pipe_finish(p);
  else
    pipe_read_next(p);
}

static void on_file_read(uv_fs_t *req) {
  StreamPipe *p = req->data;
  JSContext *ctx = p->env->ctx;
  ssize_t n = req->result;
  JSValue keep = JS_DupValue(ctx, p->aw.object);
  uv_fs_req_cleanup(req);
  p->reading = false;
  async_wrap_unref(&p->aw);
  if (p->unpiped || !p->env->can_call_into_js) {
    JS_FreeValue(ctx, keep);
    return;
  }
  env_internal_callback_scope_enter(p->env);
  if (n < 0) {
    source_onread(p, n);
    pipe_unpipe(p);
  } else if (n == 0) {
    p->eof = true;
    source_onread(p, UV_EOF);
    pipe_finish(p);
  } else {
    if (p->offset >= 0)
      p->offset += n;
    if (p->remaining >= 0) {
      p->remaining -= n;
      if (p->remaining <= 0)
        p->eof = true;
    }
    pipe_write(p, (size_t)n);
  }
  env_internal_callback_scope_exit(p->env, false);
  JS_FreeValue(ctx, keep);
}

static void pipe_read_next(StreamPipe *p) {
  size_t want = PIPE_CHUNK;
  uv_buf_t b;
  int err;
  if (p->reading || p->writing || p->unpiped || p->closed)
    return;
  if (p->remaining == 0) {
    p->eof = true;
    pipe_finish(p);
    return;
  }
  if (p->remaining > 0 && (int64_t)want > p->remaining)
    want = (size_t)p->remaining;
  b = uv_buf_init(p->buf, (unsigned)want);
  p->read_req.data = p;
  err = uv_fs_read(p->env->loop, &p->read_req, p->fd, &b, 1, p->offset, on_file_read);
  if (err < 0) {
    source_onread(p, err);
    pipe_unpipe(p);
    return;
  }
  p->reading = true;
  async_wrap_ref(&p->aw); /* the read in flight keeps the pipe */
}

static void unpipe_task(StreamPipe *p) {
  Env *env = p->env;
  JSContext *ctx = env->ctx;
  JSValue fn = JS_GetPropertyStr(ctx, p->aw.object, "onunpipe"), r;
  if (JS_IsFunction(ctx, fn)) {
    r = async_wrap_make_callback(&p->aw, fn, 0, NULL);
    if (!JS_IsException(r) && !JS_IsUninitialized(r))
      JS_FreeValue(ctx, r);
  }
  JS_FreeValue(ctx, fn);
  JS_SetPropertyStr(ctx, p->aw.object, "source", JS_NULL);
  JS_SetPropertyStr(ctx, p->aw.object, "sink", JS_NULL);
  if (JS_IsObject(p->source))
    JS_SetPropertyStr(ctx, p->source, "pipeTarget", JS_NULL);
  if (JS_IsObject(p->sink))
    JS_SetPropertyStr(ctx, p->sink, "pipeSource", JS_NULL);
}

typedef struct {
  uv_idle_t idle;
  StreamPipe *p;
} UnpipeLater;

static void unpipe_idle_closed(uv_handle_t *h) {
  UnpipeLater *u = h->data;
  JSContext *ctx = u->p->env->ctx;
  async_wrap_unref(&u->p->aw);
  free(u);
  (void)ctx;
}

static void unpipe_idle(uv_idle_t *h) {
  UnpipeLater *u = h->data;
  Env *env = u->p->env;
  uv_idle_stop(h);
  if (env->can_call_into_js) {
    env_internal_callback_scope_enter(env);
    unpipe_task(u->p);
    env_internal_callback_scope_exit(env, false);
  }
  uv_close((uv_handle_t *)h, unpipe_idle_closed);
}

/* the JS-facing part on a later turn (as Node, via SetImmediate) */
static void pipe_unpipe(StreamPipe *p) {
  UnpipeLater *u;
  if (p->unpiped)
    return;
  p->unpiped = true;
  u = calloc(1, sizeof(*u));
  u->p = p;
  uv_idle_init(p->env->loop, &u->idle);
  u->idle.data = u;
  async_wrap_ref(&p->aw);
  uv_idle_start(&u->idle, unpipe_idle);
}

static void stream_pipe_finalizer(JSRuntime *rt, JSValueConst val) {
  StreamPipe *p = JS_GetOpaque(val, stream_pipe_class_id);
  if (!p)
    return;
  JS_FreeValueRT(rt, p->source);
  JS_FreeValueRT(rt, p->sink);
  free(p->buf);
  async_wrap_destroy(&p->aw);
  free(p);
}

static void stream_pipe_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
  StreamPipe *p = JS_GetOpaque(val, stream_pipe_class_id);
  if (!p)
    return;
  JS_MarkValue(rt, p->source, mark);
  JS_MarkValue(rt, p->sink, mark);
}

/* new StreamPipe(source, sink) */
static JSValue stream_pipe_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  StreamWrap *sink = node_stream_wrap_of(ARG(1));
  JSValue obj;
  StreamPipe *p;
  int fd;
  int64_t offset, length;
  if (!sink)
    return JS_ThrowTypeError(ctx, "StreamPipe: the sink must be a native stream");
  if (!node_file_handle_read_info(ARG(0), &fd, &offset, &length))
    return JS_ThrowTypeError(ctx, "StreamPipe: the source must be an open FileHandle");
  obj = nb_new_instance(ctx, nt, stream_pipe_class_id);
  if (JS_IsException(obj))
    return obj;
  p = calloc(1, sizeof(*p));
  p->env = env;
  p->source = JS_DupValue(ctx, ARG(0));
  p->sink = JS_DupValue(ctx, ARG(1));
  p->sink_stream = sink;
  p->fd = fd;
  p->offset = offset;
  p->remaining = length;
  p->buf = malloc(PIPE_CHUNK);
  async_wrap_init(&p->aw, env, obj, PROVIDER_STREAMPIPE, -1);
  JS_SetOpaque(obj, p);
  JS_SetPropertyStr(ctx, obj, "source", JS_DupValue(ctx, ARG(0)));
  JS_SetPropertyStr(ctx, obj, "sink", JS_DupValue(ctx, ARG(1)));
  JS_SetPropertyStr(ctx, ARG(0), "pipeTarget", JS_DupValue(ctx, obj));
  JS_SetPropertyStr(ctx, ARG(1), "pipeSource", JS_DupValue(ctx, obj));
  return obj;
}

static StreamPipe *pipe_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, stream_pipe_class_id);
}

static JSValue stream_pipe_start(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  StreamPipe *p = pipe_of(ctx, this_val);
  if (!p)
    return JS_EXCEPTION;
  if (!p->started) {
    p->started = true;
    pipe_read_next(p);
  }
  return JS_UNDEFINED;
}

static JSValue stream_pipe_unpipe_fn(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  StreamPipe *p = pipe_of(ctx, this_val);
  if (!p)
    return JS_EXCEPTION;
  pipe_unpipe(p);
  return JS_UNDEFINED;
}

static JSValue stream_pipe_is_closed(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  StreamPipe *p = pipe_of(ctx, this_val);
  if (!p)
    return JS_EXCEPTION;
  return JS_NewBool(ctx, p->closed || p->unpiped);
}

static JSValue stream_pipe_pending_writes(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  StreamPipe *p = pipe_of(ctx, this_val);
  if (!p)
    return JS_EXCEPTION;
  return JS_NewInt32(ctx, p->pending_writes);
}

static JSValue stream_pipe_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  StreamPipe *p = pipe_of(ctx, this_val);
  return JS_NewFloat64(ctx, p ? p->aw.async_id : -1);
}

static const JSCFunctionListEntry stream_pipe_proto[] = {
  JS_CFUNC_DEF("unpipe", 0, stream_pipe_unpipe_fn),
  JS_CFUNC_DEF("start", 0, stream_pipe_start),
  JS_CFUNC_DEF("isClosed", 0, stream_pipe_is_closed),
  JS_CFUNC_DEF("pendingWrites", 0, stream_pipe_pending_writes),
  JS_CFUNC_DEF("getAsyncId", 0, stream_pipe_get_async_id),
};

JSValue binding_init_stream_pipe(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  NodeClassDef def = { .name = "StreamPipe", .class_id = &stream_pipe_class_id,
                       .ctor = stream_pipe_ctor, .ctor_length = 2,
                       .finalizer = stream_pipe_finalizer, .gc_mark = stream_pipe_mark,
                       .proto_funcs = stream_pipe_proto,
                       .proto_funcs_count = countof(stream_pipe_proto),
                       .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  return t;
}
