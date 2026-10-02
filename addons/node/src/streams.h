/* Native handles and streams shared by the bindings (handle_wrap.h,
 * stream_base.h of Node). */
#ifndef COLLABO_NODE_STREAMS_H
#define COLLABO_NODE_STREAMS_H

#include "node.h"

enum { HW_INITIALIZED, HW_CLOSING, HW_CLOSED };

typedef struct HandleWrap {
  AsyncWrap aw;            /* must come first */
  uv_handle_t *handle;
  int state;
  void *track;
  void (*on_closed)(struct HandleWrap *);
} HandleWrap;

typedef struct StreamWrap StreamWrap;
struct StreamWrap {
  HandleWrap hw;           /* must come first */
  uv_stream_t *stream;
  bool reading;
  uint64_t bytes_read, bytes_written;
  /* a consumer of the raw stream (TLS) can take over these */
  void (*alloc_override)(StreamWrap *s, size_t suggested, uv_buf_t *buf);
  void (*read_override)(StreamWrap *s, ssize_t nread, const uv_buf_t *buf);
  int (*read_start_override)(StreamWrap *s);
  int (*read_stop_override)(StreamWrap *s);
  int (*write_override)(StreamWrap *s, JSValueConst req, uv_buf_t *bufs, unsigned nbufs,
                        JSValueConst keep);
  int (*shutdown_override)(StreamWrap *s, JSValueConst req);
  void (*after_write_hook)(StreamWrap *s, int status);
  void (*pending_handles_cb)(StreamWrap *s);
  void *listener;
};

extern JSClassID node_tty_class_id, node_pipe_class_id, node_tcp_class_id;
extern JSClassID node_process_class_id, node_signal_class_id, node_fs_event_class_id,
    node_udp_class_id;

extern const JSCFunctionListEntry node_handle_wrap_funcs[];
extern const int node_handle_wrap_funcs_count;

void node_handle_wrap_init(HandleWrap *w, Env *env, JSValueConst obj, uv_handle_t *h,
                           ProviderType provider);
void node_handle_wrap_close(HandleWrap *w, JSValueConst cb);
HandleWrap *node_handle_wrap_of(JSValueConst obj);
StreamWrap *node_stream_wrap_of(JSValueConst obj);
int node_stream_write(StreamWrap *s, JSValueConst req_obj, uv_buf_t *bufs, unsigned nbufs,
                      JSValueConst keep, bool copy);
void node_stream_emit_read(StreamWrap *s, ssize_t nread, char *data);
void node_stream_emit_read_js(StreamWrap *s, ssize_t nread, char *data);
int node_stream_read_start(StreamWrap *s);
int node_stream_read_stop(StreamWrap *s);
int node_stream_shutdown(StreamWrap *s, JSValueConst req_obj);
void node_register_stream_class(JSClassID id);
/* req.oncomplete(...) in the request's async context */
void node_req_complete(Env *env, JSValueConst req_obj, int argc, JSValueConst *argv);
extern const JSCFunctionListEntry node_stream_base_funcs[];
extern const int node_stream_base_funcs_count;
JSValue node_new_pipe(Env *env, bool ipc);
void node_address_to_js(JSContext *ctx, const struct sockaddr *addr, JSValueConst out);

#endif
