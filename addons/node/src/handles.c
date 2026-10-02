/* Bookkeeping of open native handles: process._getActiveHandles() and
 * process.getActiveResourcesInfo(). */
#include <stdio.h>
#include <stdlib.h>

#include "node.h"
#include "streams.h"

typedef struct HandleEntry {
  struct HandleEntry *next, *prev;
  AsyncWrap *aw;
  uv_handle_t *handle;
  void (*close)(AsyncWrap *aw);  /* how to close it (NULL: a HandleWrap) */
} HandleEntry;

/* listed while open and keeping the loop alive (HandleWrap::HasRef) */
static bool listed(HandleEntry *e, Env *env) {
  return e->aw && e->aw->env == env && e->handle && !uv_is_closing(e->handle) &&
         uv_has_ref(e->handle);
}

/* the environment's list (each worker thread has its own) */
static HandleEntry *list_of(Env *env) {
  HandleEntry *h = env->handle_list;
  if (!h) {
    h = env->handle_list = calloc(1, sizeof(*h));
    h->next = h->prev = h;
  }
  return h;
}

void *node_handle_track_fn(AsyncWrap *aw, uv_handle_t *handle, void (*close)(AsyncWrap *aw)) {
  HandleEntry *e = calloc(1, sizeof(*e)), *handles = list_of(aw->env);
  e->aw = aw;
  e->handle = handle;
  e->close = close;
  e->next = handles;
  e->prev = handles->prev;
  handles->prev->next = e;
  handles->prev = e;
  return e;
}

void node_handle_untrack(void *entry) {
  HandleEntry *e = entry;
  if (!e)
    return;
  e->prev->next = e->next;
  e->next->prev = e->prev;
  free(e);
}

JSValue node_active_handles(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue arr = JS_NewArray(ctx);
  HandleEntry *e, *handles = list_of(env);
  uint32_t i = 0;
  for (e = handles->next; e != handles; e = e->next) {
    if (listed(e, env) && JS_IsObject(e->aw->object)) {
      JSValue owner = JS_DupValue(ctx, e->aw->object);
      JSValue sym = env_get_symbol(env, "owner_symbol"), o;
      JSAtom a = JS_ValueToAtom(ctx, sym);
      o = JS_GetProperty(ctx, owner, a);
      JS_FreeAtom(ctx, a);
      JS_FreeValue(ctx, sym);
      if (JS_IsObject(o)) {
        JS_FreeValue(ctx, owner);
        owner = o;
      } else {
        JS_FreeValue(ctx, o);
      }
      JS_SetPropertyUint32(ctx, arr, i++, owner);
    }
  }
  return arr;
}

JSValue node_active_resources_info(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue arr = JS_NewArray(ctx);
  HandleEntry *e, *handles = list_of(env);
  uint32_t i = 0;
  for (e = handles->next; e != handles; e = e->next) {
    if (listed(e, env)) {
      const char *n = node_provider_names[e->aw->provider];
      char buf[64];
      /* TCPWRAP -> TCPSocketWrap and the like are what Node prints; keep the
         provider names it uses for handles */
      if (!strcmp(n, "TTYWRAP")) n = "TTYWrap";
      else if (!strcmp(n, "TCPWRAP")) n = "TCPSocketWrap";
      else if (!strcmp(n, "TCPSERVERWRAP")) n = "TCPServerWrap";
      else if (!strcmp(n, "PIPEWRAP")) n = "PipeWrap";
      else if (!strcmp(n, "PROCESSWRAP")) n = "ProcessWrap";
      else if (!strcmp(n, "SIGNALWRAP")) n = "SignalWrap";
      else if (!strcmp(n, "FSEVENTWRAP")) n = "FSEventWrap";
      else if (!strcmp(n, "MESSAGEPORT")) n = "MessagePort";
      else {
        snprintf(buf, sizeof(buf), "%s", n);
        n = buf;
      }
      JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, n));
    }
  }
  return arr;
}

void *node_handle_track(AsyncWrap *aw) {
  return node_handle_track_fn(aw, ((HandleWrap *)aw)->handle, NULL);
}

/* closes every handle of env (a worker's, when it ends), the way JS would */
void node_handles_close_all(Env *env) {
  HandleEntry *e, *handles = list_of(env);
  struct { AsyncWrap *aw; void (*close)(AsyncWrap *); } *todo;
  size_t n = 0, i;
  for (e = handles->next; e != handles; e = e->next)
    n++;
  todo = calloc(n + 1, sizeof(*todo));
  n = 0;
  for (e = handles->next; e != handles; e = e->next) {
    todo[n].aw = e->aw;
    todo[n++].close = e->close;
  }
  for (i = 0; i < n; i++) {
    if (todo[i].close)
      todo[i].close(todo[i].aw);
    else
      node_handle_wrap_close((HandleWrap *)todo[i].aw, JS_UNDEFINED);
  }
  free(todo);
}
