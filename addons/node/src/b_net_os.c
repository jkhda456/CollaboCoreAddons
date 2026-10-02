/* internalBinding('cares_wrap') (cares_wrap.cc: getaddrinfo and the like;
 * the c-ares resolver is not built in), ('os') (node_os.cc) and
 * ('fs_event_wrap') (fs_event_wrap.cc). */
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "node.h"
#include "streams.h"

JSClassID node_fs_event_class_id, node_udp_class_id;
static JSClassID gai_req_class_id, gni_req_class_id, query_req_class_id, channel_class_id;

enum { DNS_ORDER_VERBATIM = 0, DNS_ORDER_IPV4_FIRST = 1, DNS_ORDER_IPV6_FIRST = 2 };

typedef struct {
  AsyncWrap aw;
} DnsReq;

static void dns_req_finalizer(JSRuntime *rt, JSValueConst val) {
  JSClassID id;
  DnsReq *r = JS_GetAnyOpaque(val, &id);
  if (r) {
    async_wrap_destroy(&r->aw);
    free(r);
  }
}

static JSValue dns_req_new(JSContext *ctx, JSValueConst nt, JSClassID id, ProviderType p) {
  JSValue obj = nb_new_instance(ctx, nt, id);
  DnsReq *r;
  if (JS_IsException(obj))
    return obj;
  r = calloc(1, sizeof(*r));
  async_wrap_init(&r->aw, env_get(ctx), obj, p, -1);
  JS_SetOpaque(obj, r);
  return obj;
}

static JSValue gai_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return dns_req_new(ctx, nt, gai_req_class_id, PROVIDER_GETADDRINFOREQWRAP);
}
static JSValue gni_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return dns_req_new(ctx, nt, gni_req_class_id, PROVIDER_GETNAMEINFOREQWRAP);
}
static JSValue query_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return dns_req_new(ctx, nt, query_req_class_id, PROVIDER_QUERYWRAP);
}

static void dns_complete(Env *env, JSValueConst req_obj, int argc, JSValueConst *argv) {
  JSContext *ctx = env->ctx;
  JSClassID id;
  DnsReq *r = JS_GetAnyOpaque(req_obj, &id);
  JSValue ret;
  if (!r)
    return;
  ret = async_wrap_make_callback_name(&r->aw, "oncomplete", argc, argv);
  JS_FreeValue(ctx, ret);
}

typedef struct {
  uv_getaddrinfo_t req;
  Env *env;
  JSValue req_obj;
  int order;
} GaiReq;

static void after_getaddrinfo(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
  GaiReq *g = req->data;
  Env *env = g->env;
  JSContext *ctx = env->ctx;
  JSValue args[2];
  uint32_t n = 0;
  args[0] = JS_NewInt32(ctx, status);
  args[1] = JS_NULL;
  if (status == 0) {
    JSValue results = JS_NewArray(ctx);
    int pass, passes = g->order == DNS_ORDER_VERBATIM ? 1 : 2;
    for (pass = 0; pass < passes; pass++) {
      bool want4 = g->order == DNS_ORDER_VERBATIM ||
                   (g->order == DNS_ORDER_IPV4_FIRST ? pass == 0 : pass == 1);
      bool want6 = g->order == DNS_ORDER_VERBATIM ||
                   (g->order == DNS_ORDER_IPV6_FIRST ? pass == 0 : pass == 1);
      struct addrinfo *p;
      for (p = res; p; p = p->ai_next) {
        char ip[INET6_ADDRSTRLEN];
        const void *addr;
        if (want4 && p->ai_family == AF_INET)
          addr = &((struct sockaddr_in *)p->ai_addr)->sin_addr;
        else if (want6 && p->ai_family == AF_INET6)
          addr = &((struct sockaddr_in6 *)p->ai_addr)->sin6_addr;
        else
          continue;
        if (uv_inet_ntop(p->ai_family, addr, ip, sizeof(ip)))
          continue;
        JS_SetPropertyUint32(ctx, results, n++, JS_NewString(ctx, ip));
      }
    }
    if (n == 0)
      args[0] = JS_NewInt32(ctx, UV_EAI_NODATA);
    args[1] = results;
  }
  uv_freeaddrinfo(res);
  dns_complete(env, g->req_obj, 2, (JSValueConst *)args);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, g->req_obj);
  free(g);
}

/* getaddrinfo(req, hostname, family, hints, order) */
static JSValue c_getaddrinfo(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  Env *env = env_get(ctx);
  GaiReq *g;
  struct addrinfo hints;
  char *host = node_string_to_utf8(ctx, ARG(1), NULL);
  int32_t fam = nb_int32(ctx, ARG(2), 0);
  int err;
  if (!host)
    return JS_EXCEPTION;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = fam == 4 ? AF_INET : fam == 6 ? AF_INET6 : AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = nb_int32(ctx, ARG(3), 0);
  g = calloc(1, sizeof(*g));
  g->env = env;
  g->req.data = g;
  g->req_obj = JS_DupValue(ctx, ARG(0));
  g->order = nb_int32(ctx, ARG(4), DNS_ORDER_VERBATIM);
  err = uv_getaddrinfo(env->loop, &g->req, after_getaddrinfo, host, NULL, &hints);
  free(host);
  if (err) {
    JS_FreeValue(ctx, g->req_obj);
    free(g);
  }
  return JS_NewInt32(ctx, err);
}

typedef struct {
  uv_getnameinfo_t req;
  Env *env;
  JSValue req_obj;
} GniReq;

static void after_getnameinfo(uv_getnameinfo_t *req, int status, const char *hostname,
                              const char *service) {
  GniReq *g = req->data;
  JSContext *ctx = g->env->ctx;
  JSValue args[3];
  args[0] = JS_NewInt32(ctx, status);
  args[1] = status == 0 ? JS_NewString(ctx, hostname) : JS_NULL;
  args[2] = status == 0 ? JS_NewString(ctx, service) : JS_NULL;
  dns_complete(g->env, g->req_obj, 3, (JSValueConst *)args);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, args[2]);
  JS_FreeValue(ctx, g->req_obj);
  free(g);
}

/* getnameinfo(req, ip, port) */
static JSValue c_getnameinfo(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  Env *env = env_get(ctx);
  struct sockaddr_storage ss;
  const char *ip = JS_ToCString(ctx, ARG(1));
  int32_t port = nb_int32(ctx, ARG(2), 0);
  GniReq *g;
  int err;
  if (!ip)
    return JS_EXCEPTION;
  if (uv_ip4_addr(ip, port, (struct sockaddr_in *)&ss) != 0 &&
      uv_ip6_addr(ip, port, (struct sockaddr_in6 *)&ss) != 0) {
    JS_FreeCString(ctx, ip);
    return JS_NewInt32(ctx, UV_EINVAL);
  }
  JS_FreeCString(ctx, ip);
  g = calloc(1, sizeof(*g));
  g->env = env;
  g->req.data = g;
  g->req_obj = JS_DupValue(ctx, ARG(0));
  err = uv_getnameinfo(env->loop, &g->req, after_getnameinfo, (struct sockaddr *)&ss,
                       NI_NAMEREQD);
  if (err) {
    JS_FreeValue(ctx, g->req_obj);
    free(g);
  }
  return JS_NewInt32(ctx, err);
}

static JSValue c_canonicalize_ip(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  const char *ip = JS_ToCString(ctx, ARG(0));
  unsigned char buf[16];
  char out[INET6_ADDRSTRLEN];
  int af;
  if (!ip)
    return JS_EXCEPTION;
  if (uv_inet_pton(af = AF_INET, ip, buf) != 0 && uv_inet_pton(af = AF_INET6, ip, buf) != 0) {
    JS_FreeCString(ctx, ip);
    return JS_UNDEFINED;
  }
  JS_FreeCString(ctx, ip);
  uv_inet_ntop(af, buf, out, sizeof(out));
  return JS_NewString(ctx, out);
}

static JSValue c_convert_ipv6(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  const char *ip = JS_ToCString(ctx, ARG(0));
  unsigned char buf[16];
  if (!ip)
    return JS_EXCEPTION;
  if (uv_inet_pton(AF_INET6, ip, buf) != 0) {
    JS_FreeCString(ctx, ip);
    return JS_ThrowPlainError(ctx, "Invalid IPv6 address");
  }
  JS_FreeCString(ctx, ip);
  return nb_new_buffer(ctx, buf, 16);
}

static JSValue c_strerror(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  int32_t code = nb_int32(ctx, ARG(0), 0);
  if (code == -1000)  /* DNS_ESETSRVPENDING */
    return JS_NewString(ctx, "There are pending queries.");
  return JS_NewString(ctx, code == 0 ? "Successful completion" : uv_strerror(code));
}

/* ChannelWrap: dns.Resolver and dns.resolve*().  Node asks c-ares; this is
 * a DNS client of its own over libuv doing what c-ares does for Node: queries
 * over UDP, again over TCP when the answer is truncated, to the
 * servers of /etc/resolv.conf (or setServers()), each try waiting longer
 * (timeout, tries, maxTimeout); answers parsed into what cares_wrap.cc gives
 * JS. */

enum {
  T_A = 1, T_NS = 2, T_CNAME = 5, T_SOA = 6, T_PTR = 12, T_MX = 15, T_TXT = 16, T_AAAA = 28,
  T_SRV = 33, T_NAPTR = 35, T_OPT = 41, T_TLSA = 52, T_ANY = 255, T_CAA = 257,
};

#define DNS_DEFAULT_TIMEOUT 2000
#define DNS_ESETSRVPENDING (-1000)

typedef struct {
  struct sockaddr_storage addr;
} DnsServer;

typedef struct DnsQuery DnsQuery;

typedef struct {
  AsyncWrap aw;
  int timeout_ms, tries, max_timeout_ms;
  DnsServer *servers;
  int nservers;
  DnsQuery *pending;
  int npending;
  struct sockaddr_in local4;
  struct sockaddr_in6 local6;
  bool has_local4, has_local6;
} Channel;

struct DnsQuery {
  DnsQuery *next, **pprev;
  Channel *c;
  Env *env;
  JSValue req_obj, channel_obj;
  uint16_t qtype;
  bool reverse;           /* getHostByAddr */
  uint16_t id;
  uint8_t *pkt;
  size_t pkt_len;
  int attempt;
  uv_udp_t *udp;          /* this try's sockets: each try has new ones, as the */
  uv_tcp_t *tcp;          /* last try's may still be closing */
  uv_udp_send_t send;
  uv_connect_t conn;
  uv_write_t wr;
  uint8_t lenbuf[2];
  uint8_t *tbuf;
  size_t tlen, tcap;
  uv_timer_t timer;
  int open_handles;
  bool done, refused, onion;
  char rbuf[65536];
};

/* ---- servers ---- */

static void channel_default_servers(Channel *c) {
  FILE *f = fopen("/etc/resolv.conf", "r");
  char line[512];
  free(c->servers);
  c->servers = NULL;
  c->nservers = 0;
  if (f) {
    while (fgets(line, sizeof(line), f)) {
      char ip[256];
      struct sockaddr_storage ss;
      if (sscanf(line, " nameserver %255s", ip) != 1)
        continue;
      memset(&ss, 0, sizeof(ss));
      if (uv_ip4_addr(ip, 53, (struct sockaddr_in *)&ss) != 0 &&
          uv_ip6_addr(ip, 53, (struct sockaddr_in6 *)&ss) != 0)
        continue;
      c->servers = realloc(c->servers, (c->nservers + 1) * sizeof(DnsServer));
      c->servers[c->nservers++].addr = ss;
    }
    fclose(f);
  }
  if (!c->nservers) {
    c->servers = calloc(1, sizeof(DnsServer));
    uv_ip4_addr("127.0.0.1", 53, (struct sockaddr_in *)&c->servers[0].addr);
    c->nservers = 1;
  }
}

/* ---- the query packet ---- */

static int dns_encode_name(const char *name, uint8_t *out, size_t cap, size_t *len) {
  size_t n = strlen(name), o = 0, i = 0;
  if (n && name[n - 1] == '.')
    n--;
  if (n > 253)
    return -1;
  while (i < n) {
    size_t j = i;
    while (j < n && name[j] != '.')
      j++;
    if (j == i || j - i > 63 || o + 1 + (j - i) + 1 > cap)
      return -1;
    out[o++] = (uint8_t)(j - i);
    memcpy(out + o, name + i, j - i);
    o += j - i;
    i = j + 1;
    if (j < n && j + 1 == n)
      return -1;  /* "a." handled above; "a.." is not a name */
  }
  out[o++] = 0;
  *len = o;
  return 0;
}

static uint8_t *dns_build_query(const char *name, uint16_t qtype, uint16_t id, size_t *len) {
  uint8_t *p = malloc(12 + 256 + 4 + 11), *q;
  size_t nlen;
  if (dns_encode_name(name, p + 12, 256, &nlen) < 0) {
    free(p);
    return NULL;
  }
  p[0] = id >> 8;
  p[1] = id & 0xff;
  p[2] = 0x01;  /* RD */
  p[3] = 0;
  p[4] = 0; p[5] = 1;   /* QDCOUNT */
  p[6] = p[7] = p[8] = p[9] = 0;
  p[10] = 0; p[11] = 0; /* no EDNS, as Node's c-ares asks (large answers come over TCP) */
  q = p + 12 + nlen;
  *q++ = qtype >> 8;
  *q++ = qtype & 0xff;
  *q++ = 0;
  *q++ = 1;  /* IN */
  *len = q - p;
  return p;
}

/* ---- the answer ---- */

typedef struct {
  const uint8_t *buf;
  size_t len;
} DnsMsg;

/* a (possibly compressed) name at *off into out; advances *off past it */
static int dns_read_name(const DnsMsg *m, size_t *off, char *out, size_t cap) {
  size_t o = *off, w = 0;
  int hops = 0;
  bool jumped = false;
  for (;;) {
    uint8_t l;
    if (o >= m->len)
      return -1;
    l = m->buf[o];
    if ((l & 0xc0) == 0xc0) {
      size_t ptr;
      if (o + 1 >= m->len || ++hops > 64)
        return -1;
      ptr = ((size_t)(l & 0x3f) << 8) | m->buf[o + 1];
      if (!jumped)
        *off = o + 2;
      jumped = true;
      o = ptr;
      continue;
    }
    if (l & 0xc0)
      return -1;
    o++;
    if (l == 0)
      break;
    if (o + l > m->len || w + l + 2 > cap)
      return -1;
    if (w)
      out[w++] = '.';
    memcpy(out + w, m->buf + o, l);
    w += l;
    o += l;
  }
  out[w] = 0;
  if (!jumped)
    *off = o;
  return 0;
}

typedef struct {
  uint16_t type;
  uint32_t ttl;
  size_t rdata, rdlen;   /* offset and length in the message */
} DnsRR;

/* the answer section's records; -1 for a malformed message */
static int dns_answers(const DnsMsg *m, DnsRR **out, int *n) {
  size_t off = 12;
  int qd, an, i;
  char name[1025];
  *out = NULL;
  *n = 0;
  if (m->len < 12)
    return -1;
  qd = (m->buf[4] << 8) | m->buf[5];
  an = (m->buf[6] << 8) | m->buf[7];
  for (i = 0; i < qd; i++) {
    if (dns_read_name(m, &off, name, sizeof(name)) < 0 || off + 4 > m->len)
      return -1;
    off += 4;
  }
  *out = calloc(an + 1, sizeof(DnsRR));
  for (i = 0; i < an; i++) {
    DnsRR *r = &(*out)[*n];
    if (dns_read_name(m, &off, name, sizeof(name)) < 0 || off + 10 > m->len)
      return -1;
    r->type = (m->buf[off] << 8) | m->buf[off + 1];
    r->ttl = ((uint32_t)m->buf[off + 4] << 24) | ((uint32_t)m->buf[off + 5] << 16) |
             ((uint32_t)m->buf[off + 6] << 8) | m->buf[off + 7];
    r->rdlen = (m->buf[off + 8] << 8) | m->buf[off + 9];
    r->rdata = off + 10;
    off += 10 + r->rdlen;
    if (off > m->len)
      return -1;
    (*n)++;
  }
  return 0;
}

static uint32_t rd32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

static JSValue dns_name_at(JSContext *ctx, const DnsMsg *m, size_t off) {
  char name[1025];
  if (dns_read_name(m, &off, name, sizeof(name)) < 0)
    return JS_EXCEPTION;
  return JS_NewString(ctx, name);
}

/* a <character-string>: length-prefixed */
static JSValue dns_cstring(JSContext *ctx, const DnsMsg *m, size_t *off, size_t end) {
  size_t l;
  if (*off >= end)
    return JS_EXCEPTION;
  l = m->buf[*off];
  if (*off + 1 + l > end)
    return JS_EXCEPTION;
  *off += 1 + l;
  return JS_NewStringLen(ctx, (const char *)m->buf + *off - l, l);
}

static void set_type(JSContext *ctx, JSValueConst o, const char *type, bool need_type) {
  if (need_type)
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, type));
}

/* the records of one type into arr (from index *count); ttls for A/AAAA;
   EBADRESP (-1) on malformed data */
static int dns_collect(JSContext *ctx, const DnsMsg *m, DnsRR *rr, int n, uint16_t type,
                       JSValueConst arr, uint32_t *count, JSValueConst ttls, bool any) {
  int i;
  for (i = 0; i < n; i++) {
    const uint8_t *d = m->buf + rr[i].rdata;
    size_t dl = rr[i].rdlen, off = rr[i].rdata;
    JSValue v = JS_UNDEFINED;
    if (rr[i].type != type)
      continue;
    switch (type) {
    case T_A:
    case T_AAAA: {
      char ip[64];
      if (dl != (type == T_A ? 4u : 16u))
        return -1;
      uv_inet_ntop(type == T_A ? AF_INET : AF_INET6, d, ip, sizeof(ip));
      if (any) {
        v = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, v, "address", JS_NewString(ctx, ip));
        JS_SetPropertyStr(ctx, v, "ttl", JS_NewUint32(ctx, rr[i].ttl));
        set_type(ctx, v, type == T_A ? "A" : "AAAA", true);
      } else {
        v = JS_NewString(ctx, ip);
        if (JS_IsObject(ttls))
          JS_SetPropertyUint32(ctx, ttls, *count, JS_NewUint32(ctx, rr[i].ttl));
      }
      break;
    }
    case T_CNAME:
    case T_NS:
    case T_PTR:
      v = dns_name_at(ctx, m, off);
      if (JS_IsException(v))
        return -1;
      if (any) {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "value", v);
        set_type(ctx, o, type == T_CNAME ? "CNAME" : type == T_NS ? "NS" : "PTR", true);
        v = o;
      }
      break;
    case T_MX:
      if (dl < 3)
        return -1;
      v = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, v, "exchange", dns_name_at(ctx, m, off + 2));
      JS_SetPropertyStr(ctx, v, "priority", JS_NewInt32(ctx, rd16(d)));
      set_type(ctx, v, "MX", true);  /* as cares_wrap.cc: always */
      break;
    case T_TXT: {
      JSValue chunks = JS_NewArray(ctx);
      size_t o = off, end = off + dl;
      uint32_t k = 0;
      while (o < end) {
        JSValue s = dns_cstring(ctx, m, &o, end);
        if (JS_IsException(s)) {
          JS_FreeValue(ctx, chunks);
          return -1;
        }
        JS_SetPropertyUint32(ctx, chunks, k++, s);
      }
      if (any) {
        v = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, v, "entries", chunks);
        set_type(ctx, v, "TXT", true);
      } else {
        v = chunks;
      }
      break;
    }
    case T_SRV:
      if (dl < 7)
        return -1;
      v = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, v, "name", dns_name_at(ctx, m, off + 6));
      JS_SetPropertyStr(ctx, v, "port", JS_NewInt32(ctx, rd16(d + 4)));
      JS_SetPropertyStr(ctx, v, "priority", JS_NewInt32(ctx, rd16(d)));
      JS_SetPropertyStr(ctx, v, "weight", JS_NewInt32(ctx, rd16(d + 2)));
      set_type(ctx, v, "SRV", true);
      break;
    case T_NAPTR: {
      size_t o = off + 4, end = off + dl;
      JSValue flags, service, regexp;
      if (dl < 7)
        return -1;
      v = JS_NewObject(ctx);
      flags = dns_cstring(ctx, m, &o, end);
      service = dns_cstring(ctx, m, &o, end);
      regexp = dns_cstring(ctx, m, &o, end);
      if (JS_IsException(flags) || JS_IsException(service) || JS_IsException(regexp)) {
        JS_FreeValue(ctx, flags);
        JS_FreeValue(ctx, service);
        JS_FreeValue(ctx, regexp);
        JS_FreeValue(ctx, v);
        return -1;
      }
      JS_SetPropertyStr(ctx, v, "flags", flags);
      JS_SetPropertyStr(ctx, v, "service", service);
      JS_SetPropertyStr(ctx, v, "regexp", regexp);
      JS_SetPropertyStr(ctx, v, "replacement", dns_name_at(ctx, m, o));
      JS_SetPropertyStr(ctx, v, "order", JS_NewInt32(ctx, rd16(d)));
      JS_SetPropertyStr(ctx, v, "preference", JS_NewInt32(ctx, rd16(d + 2)));
      set_type(ctx, v, "NAPTR", any);
      break;
    }
    case T_SOA: {
      size_t o = off;
      char ns[1025], host[1025];
      if (dns_read_name(m, &o, ns, sizeof(ns)) < 0 ||
          dns_read_name(m, &o, host, sizeof(host)) < 0 || o + 20 > off + dl)
        return -1;
      v = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, v, "nsname", JS_NewString(ctx, ns));
      JS_SetPropertyStr(ctx, v, "hostmaster", JS_NewString(ctx, host));
      JS_SetPropertyStr(ctx, v, "serial", JS_NewUint32(ctx, rd32(m->buf + o)));
      JS_SetPropertyStr(ctx, v, "refresh", JS_NewInt32(ctx, (int32_t)rd32(m->buf + o + 4)));
      JS_SetPropertyStr(ctx, v, "retry", JS_NewInt32(ctx, (int32_t)rd32(m->buf + o + 8)));
      JS_SetPropertyStr(ctx, v, "expire", JS_NewInt32(ctx, (int32_t)rd32(m->buf + o + 12)));
      JS_SetPropertyStr(ctx, v, "minttl", JS_NewUint32(ctx, rd32(m->buf + o + 16)));
      set_type(ctx, v, "SOA", any);
      break;
    }
    case T_CAA: {
      size_t tl;
      char tag[256];
      if (dl < 2)
        return -1;
      tl = d[1];
      if (2 + tl > dl)
        return -1;
      memcpy(tag, d + 2, tl);
      tag[tl] = 0;
      v = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, v, "critical", JS_NewInt32(ctx, d[0]));
      set_type(ctx, v, "CAA", true);
      JS_SetPropertyStr(ctx, v, tag,
                        JS_NewStringLen(ctx, (const char *)d + 2 + tl, dl - 2 - tl));
      break;
    }
    case T_TLSA:
      if (dl < 3)
        return -1;
      v = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, v, "certUsage", JS_NewInt32(ctx, d[0]));
      JS_SetPropertyStr(ctx, v, "selector", JS_NewInt32(ctx, d[1]));
      JS_SetPropertyStr(ctx, v, "match", JS_NewInt32(ctx, d[2]));
      JS_SetPropertyStr(ctx, v, "data", JS_NewArrayBufferCopy(ctx, d + 3, dl - 3));
      break;
    }
    JS_SetPropertyUint32(ctx, arr, (*count)++, v);
  }
  return 0;
}

/* the error code string for a DNS response code, or NULL */
static const char *dns_rcode_error(int rcode) {
  switch (rcode) {
  case 0: return NULL;
  case 1: return "EFORMERR";
  case 2: return "ESERVFAIL";
  case 3: return "ENOTFOUND";
  case 4: return "ENOTIMP";
  case 5: return "EREFUSED";
  default: return "EBADRESP";
  }
}

/* oncomplete(...) for the answer */
static void dns_query_answer(DnsQuery *q, const uint8_t *buf, size_t len) {
  JSContext *ctx = q->env->ctx;
  DnsMsg m = { buf, len };
  DnsRR *rr = NULL;
  int n = 0;
  const char *err;
  JSValue args[3], arr;
  uint32_t count = 0;
  int argc = 2, bad = 0;
  if (len < 12) {
    err = "EBADRESP";
  } else if ((err = dns_rcode_error(buf[3] & 0x0f)) == NULL && dns_answers(&m, &rr, &n) < 0) {
    err = "EBADRESP";
  }
  if (err) {
    free(rr);
    args[0] = JS_NewString(ctx, err);
    dns_complete(q->env, q->req_obj, 1, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    return;
  }
  arr = JS_NewArray(ctx);
  args[2] = JS_UNDEFINED;
  if (q->qtype == T_ANY) {
    static const uint16_t order[] = { T_A, T_CNAME, T_AAAA, T_MX, T_NS, T_TXT, T_SRV, T_PTR,
                                      T_NAPTR, T_SOA, T_TLSA, T_CAA };
    size_t k;
    bool has_cname = false;
    int i;
    for (i = 0; i < n; i++)
      has_cname |= rr[i].type == T_CNAME;
    for (k = 0; k < countof(order) && !bad; k++) {
      /* a response with a CNAME is reported as the CNAME, not as A */
      if ((order[k] == T_A && has_cname) || (order[k] == T_CNAME && !has_cname))
        continue;
      bad = dns_collect(ctx, &m, rr, n, order[k], arr, &count, JS_UNDEFINED, true);
    }
  } else {
    if (q->qtype == T_A || q->qtype == T_AAAA) {
      args[2] = JS_NewArray(ctx);
      argc = 3;
    }
    bad = dns_collect(ctx, &m, rr, n, q->reverse ? T_PTR : q->qtype, arr, &count, args[2],
                      false);
  }
  free(rr);
  if (bad || count == 0) {
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, args[2]);
    args[0] = JS_NewString(ctx, bad ? "EBADRESP" : "ENODATA");
    dns_complete(q->env, q->req_obj, 1, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    return;
  }
  if (q->qtype == T_SOA) {
    /* querySoa gives the record itself */
    JSValue soa = JS_GetPropertyUint32(ctx, arr, 0);
    JS_FreeValue(ctx, arr);
    arr = soa;
  }
  args[0] = JS_NewInt32(ctx, 0);
  args[1] = arr;
  dns_complete(q->env, q->req_obj, argc, (JSValueConst *)args);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, args[2]);
}

/* ---- the exchange ---- */

static void dns_try(DnsQuery *q);

static void dns_query_free_if_closed(DnsQuery *q) {
  if (q->open_handles > 0)
    return;
  JS_FreeValue(q->env->ctx, q->req_obj);
  JS_FreeValue(q->env->ctx, q->channel_obj);
  free(q->pkt);
  free(q->tbuf);
  free(q);
}

static void dns_handle_closed(uv_handle_t *h) {
  DnsQuery *q = h->data;
  q->open_handles--;
  dns_query_free_if_closed(q);
}

static void dns_socket_closed(uv_handle_t *h) {
  DnsQuery *q = h->data;
  free(h);
  q->open_handles--;
  dns_query_free_if_closed(q);
}

static void dns_close_sockets(DnsQuery *q) {
  if (q->udp) {
    uv_close((uv_handle_t *)q->udp, dns_socket_closed);
    q->udp = NULL;
  }
  if (q->tcp) {
    uv_close((uv_handle_t *)q->tcp, dns_socket_closed);
    q->tcp = NULL;
  }
}

/* the end of a query: the answer (buf), or err; frees it once its handles closed */
static void dns_query_finish(DnsQuery *q, const uint8_t *buf, size_t len, const char *err) {
  Channel *c = q->c;
  if (q->done)
    return;
  q->done = true;
  if (q->pprev) {
    *q->pprev = q->next;
    if (q->next)
      q->next->pprev = q->pprev;
    q->pprev = NULL;
    if (--c->npending == 0)
      async_wrap_unref(&c->aw);
  }
  uv_timer_stop(&q->timer);
  if (q->env->can_call_into_js) {
    if (err) {
      JSValue arg = JS_NewString(q->env->ctx, err);
      dns_complete(q->env, q->req_obj, 1, (JSValueConst *)&arg);
      JS_FreeValue(q->env->ctx, arg);
    } else {
      dns_query_answer(q, buf, len);
    }
  }
  dns_close_sockets(q);
  uv_close((uv_handle_t *)&q->timer, dns_handle_closed);
}

static bool dns_reply_matches(DnsQuery *q, const uint8_t *buf, size_t len) {
  return len >= 12 && rd16(buf) == q->id && (buf[2] & 0x80);
}

static void dns_next_try(DnsQuery *q) {
  q->attempt++;
  dns_close_sockets(q);
  if (q->attempt >= q->c->tries * q->c->nservers) {
    dns_query_finish(q, NULL, 0, q->refused ? "ECONNREFUSED" : "ETIMEOUT");
    return;
  }
  dns_try(q);
}

static void dns_timer_cb(uv_timer_t *h) {
  DnsQuery *q = h->data;
  if (q->onion) {
    dns_query_finish(q, NULL, 0, "ENOTFOUND");  /* RFC 7686: never asked */
    return;
  }
  if (!q->pkt) {
    dns_query_finish(q, NULL, 0, "EBADNAME");  /* not a name */
    return;
  }
  if (!q->c->nservers) {
    dns_query_finish(q, NULL, 0, "ECONNREFUSED");
    return;
  }
  dns_next_try(q);
}

static void dns_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
  DnsQuery *q = h->data;
  *buf = uv_buf_init(q->rbuf, sizeof(q->rbuf));
}

/* TCP: the 2-byte length, then the message */
static void dns_tcp_read(uv_stream_t *s, ssize_t nread, const uv_buf_t *buf) {
  DnsQuery *q = s->data;
  size_t need;
  if (q->done)
    return;
  if (nread < 0) {
    dns_next_try(q);
    return;
  }
  if (q->tlen + nread > q->tcap) {
    q->tcap = q->tlen + nread + 4096;
    q->tbuf = realloc(q->tbuf, q->tcap);
  }
  memcpy(q->tbuf + q->tlen, buf->base, nread);
  q->tlen += nread;
  if (q->tlen < 2)
    return;
  need = rd16(q->tbuf);
  if (q->tlen < 2 + need)
    return;
  if (!dns_reply_matches(q, q->tbuf + 2, need)) {
    dns_next_try(q);
    return;
  }
  dns_query_finish(q, q->tbuf + 2, need, NULL);
}

static void dns_tcp_written(uv_write_t *w, int status) {
  DnsQuery *q = w->data;
  if (status < 0 && !q->done)
    dns_next_try(q);
}

static void dns_tcp_connected(uv_connect_t *c, int status) {
  DnsQuery *q = c->data;
  uv_buf_t bufs[2];
  if (q->done || status == UV_ECANCELED)
    return;
  if (status < 0) {
    if (status == UV_ECONNREFUSED)
      q->refused = true;
    dns_next_try(q);
    return;
  }
  q->lenbuf[0] = (uint8_t)(q->pkt_len >> 8);
  q->lenbuf[1] = (uint8_t)q->pkt_len;
  bufs[0] = uv_buf_init((char *)q->lenbuf, 2);
  bufs[1] = uv_buf_init((char *)q->pkt, q->pkt_len);
  q->wr.data = q;
  q->tlen = 0;
  if (!q->tcp ||
      uv_write(&q->wr, (uv_stream_t *)q->tcp, bufs, 2, dns_tcp_written) < 0 ||
      uv_read_start((uv_stream_t *)q->tcp, dns_alloc, dns_tcp_read) < 0)
    dns_next_try(q);
}

static void dns_start_tcp(DnsQuery *q) {
  const struct sockaddr *sa = (const struct sockaddr *)&q->c->servers[q->attempt % q->c->nservers].addr;
  dns_close_sockets(q);
  q->tcp = malloc(sizeof(uv_tcp_t));
  uv_tcp_init(q->env->loop, q->tcp);
  q->tcp->data = q;
  q->open_handles++;
  q->conn.data = q;
  if (uv_tcp_connect(&q->conn, q->tcp, sa, dns_tcp_connected) < 0)
    dns_next_try(q);
}

static void dns_udp_read(uv_udp_t *h, ssize_t nread, const uv_buf_t *buf,
                         const struct sockaddr *addr, unsigned flags) {
  DnsQuery *q = h->data;
  const uint8_t *b = (const uint8_t *)buf->base;
  if (q->done || nread == 0)
    return;
  if (nread < 0) {
    if (nread == UV_ECONNREFUSED)
      q->refused = true;
    dns_next_try(q);
    return;
  }
  if (!dns_reply_matches(q, b, nread))
    return;  /* not ours: keep waiting */
  if (b[2] & 0x02) {
    /* truncated: ask again over TCP */
    dns_start_tcp(q);
    return;
  }
  dns_query_finish(q, b, nread, NULL);
}

static void dns_udp_sent(uv_udp_send_t *s, int status) {
  DnsQuery *q = s->data;
  if (status < 0 && status != UV_ECANCELED && !q->done) {
    if (status == UV_ECONNREFUSED)
      q->refused = true;
    dns_next_try(q);
  }
}

static void dns_try(DnsQuery *q) {
  Channel *c = q->c;
  const struct sockaddr *sa = (const struct sockaddr *)&c->servers[q->attempt % c->nservers].addr;
  int64_t timeout = c->timeout_ms > 0 ? c->timeout_ms : DNS_DEFAULT_TIMEOUT;
  uv_buf_t b;
  int round = q->attempt / c->nservers, r;
  while (round-- > 0 && timeout < (int64_t)1 << 30)
    timeout *= 2;
  if (c->max_timeout_ms > 0 && timeout > c->max_timeout_ms)
    timeout = c->max_timeout_ms;
  q->udp = malloc(sizeof(uv_udp_t));
  uv_udp_init(q->env->loop, q->udp);
  q->udp->data = q;
  q->open_handles++;
  if (sa->sa_family == AF_INET && c->has_local4)
    uv_udp_bind(q->udp, (const struct sockaddr *)&c->local4, 0);
  else if (sa->sa_family == AF_INET6 && c->has_local6)
    uv_udp_bind(q->udp, (const struct sockaddr *)&c->local6, 0);
  q->send.data = q;
  b = uv_buf_init((char *)q->pkt, q->pkt_len);
  /* connected, as c-ares does: a closed port is ECONNREFUSED, not a timeout */
  r = uv_udp_connect(q->udp, sa);
  if (r == 0)
    r = uv_udp_send(&q->send, q->udp, &b, 1, NULL, dns_udp_sent);
  if (r == 0)
    r = uv_udp_recv_start(q->udp, dns_alloc, dns_udp_read);
  uv_timer_start(&q->timer, dns_timer_cb, (uint64_t)timeout, 0);
  if (r < 0) {
    if (r == UV_ECONNREFUSED)
      q->refused = true;
    uv_timer_stop(&q->timer);
    uv_timer_start(&q->timer, dns_timer_cb, 0, 0);  /* the next try, from the loop */
  }
}

/* ---- ChannelWrap ---- */

static void channel_finalizer(JSRuntime *rt, JSValueConst val) {
  Channel *c = JS_GetOpaque(val, channel_class_id);
  if (c) {
    async_wrap_destroy(&c->aw);
    free(c->servers);
    free(c);
  }
}

/* new ChannelWrap(timeout, tries, maxTimeout) */
static JSValue channel_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, channel_class_id);
  Channel *c;
  if (JS_IsException(obj))
    return obj;
  c = calloc(1, sizeof(*c));
  async_wrap_init(&c->aw, env_get(ctx), obj, PROVIDER_DNSCHANNEL, -1);
  c->timeout_ms = nb_int32(ctx, ARG(0), -1);
  c->tries = nb_int32(ctx, ARG(1), 4);
  if (c->tries < 1)
    c->tries = 1;
  c->max_timeout_ms = nb_int32(ctx, ARG(2), 0);
  channel_default_servers(c);
  JS_SetOpaque(obj, c);
  return obj;
}

static JSValue channel_start(JSContext *ctx, JSValueConst this_val, JSValueConst req,
                             const char *name, uint16_t qtype, bool reverse) {
  Channel *c = JS_GetOpaque2(ctx, this_val, channel_class_id);
  Env *env = env_get(ctx);
  DnsQuery *q;
  uint8_t rnd[2];
  if (!c)
    return JS_EXCEPTION;
  q = calloc(1, sizeof(*q));
  q->c = c;
  q->env = env;
  q->qtype = qtype;
  q->reverse = reverse;
  uv_random(NULL, NULL, rnd, 2, 0, NULL);
  q->id = (uint16_t)((rnd[0] << 8) | rnd[1]);
  q->pkt = dns_build_query(name, qtype, q->id, &q->pkt_len);
  {
    /* .onion names are not resolved (RFC 7686), as c-ares does */
    size_t l = strlen(name);
    if (l && name[l - 1] == '.')
      l--;
    q->onion = (l == 5 && !strncasecmp(name, "onion", 5)) ||
               (l > 5 && !strncasecmp(name + l - 6, ".onion", 6));
  }
  q->req_obj = JS_DupValue(ctx, req);
  q->channel_obj = JS_DupValue(ctx, this_val);
  uv_timer_init(env->loop, &q->timer);
  q->timer.data = q;
  q->open_handles = 1;
  q->next = c->pending;
  if (c->pending)
    c->pending->pprev = &q->next;
  c->pending = q;
  q->pprev = &c->pending;
  if (c->npending++ == 0)
    async_wrap_ref(&c->aw);
  if (!q->pkt || !c->nservers || q->onion) {
    /* not a name, or nowhere to ask: the error from the loop */
    uv_timer_start(&q->timer, dns_timer_cb, 0, 0);
    return JS_NewInt32(ctx, 0);
  }
  dns_try(q);
  return JS_NewInt32(ctx, 0);
}

/* query<Type>(req, name) */
static JSValue channel_query(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic) {
  const char *name = JS_ToCString(ctx, ARG(1));
  JSValue r;
  if (!name)
    return JS_EXCEPTION;
  r = channel_start(ctx, this_val, ARG(0), name, (uint16_t)magic, false);
  JS_FreeCString(ctx, name);
  return r;
}

/* getHostByAddr(req, ip): PTR of in-addr.arpa / ip6.arpa */
static JSValue channel_get_host_by_addr(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  const char *ip = JS_ToCString(ctx, ARG(1));
  uint8_t a[16];
  char name[128], *p = name;
  JSValue r;
  int i;
  if (!ip)
    return JS_EXCEPTION;
  if (uv_inet_pton(AF_INET, ip, a) == 0) {
    snprintf(name, sizeof(name), "%u.%u.%u.%u.in-addr.arpa", a[3], a[2], a[1], a[0]);
  } else if (uv_inet_pton(AF_INET6, ip, a) == 0) {
    for (i = 15; i >= 0; i--)
      p += sprintf(p, "%x.%x.", a[i] & 15, a[i] >> 4);
    strcpy(p, "ip6.arpa");
  } else {
    JS_FreeCString(ctx, ip);
    return JS_NewInt32(ctx, UV_EINVAL);
  }
  JS_FreeCString(ctx, ip);
  r = channel_start(ctx, this_val, ARG(0), name, T_PTR, true);
  return r;
}

static JSValue channel_get_servers(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Channel *c = JS_GetOpaque2(ctx, this_val, channel_class_id);
  JSValue arr;
  int i;
  if (!c)
    return JS_EXCEPTION;
  arr = JS_NewArray(ctx);
  for (i = 0; i < c->nservers; i++) {
    char ip[64];
    int port;
    const struct sockaddr *sa = (const struct sockaddr *)&c->servers[i].addr;
    JSValue e = JS_NewArray(ctx);
    if (sa->sa_family == AF_INET6) {
      uv_ip6_name((const struct sockaddr_in6 *)sa, ip, sizeof(ip));
      port = ntohs(((const struct sockaddr_in6 *)sa)->sin6_port);
    } else {
      uv_ip4_name((const struct sockaddr_in *)sa, ip, sizeof(ip));
      port = ntohs(((const struct sockaddr_in *)sa)->sin_port);
    }
    JS_SetPropertyUint32(ctx, e, 0, JS_NewString(ctx, ip));
    JS_SetPropertyUint32(ctx, e, 1, JS_NewInt32(ctx, port));
    JS_SetPropertyUint32(ctx, arr, i, e);
  }
  return arr;
}

/* setServers([[family, ip, port], ...]): 0, or an error code */
static JSValue channel_set_servers(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
  Channel *c = JS_GetOpaque2(ctx, this_val, channel_class_id);
  JSValue lenv;
  uint32_t n = 0, i;
  DnsServer *list;
  int count = 0;
  if (!c)
    return JS_EXCEPTION;
  if (c->npending)
    return JS_NewInt32(ctx, DNS_ESETSRVPENDING);
  lenv = JS_GetPropertyStr(ctx, ARG(0), "length");
  JS_ToUint32(ctx, &n, lenv);
  JS_FreeValue(ctx, lenv);
  if (n == 0) {
    free(c->servers);
    c->servers = NULL;
    c->nservers = 0;  /* queries end in ECONNREFUSED */
    return JS_NewInt32(ctx, 0);
  }
  list = calloc(n, sizeof(DnsServer));
  for (i = 0; i < n; i++) {
    JSValue e = JS_GetPropertyUint32(ctx, ARG(0), i);
    JSValue f = JS_GetPropertyUint32(ctx, e, 0), ipv = JS_GetPropertyUint32(ctx, e, 1),
            pv = JS_GetPropertyUint32(ctx, e, 2);
    int32_t family = 4, port = 53;
    const char *ip = JS_ToCString(ctx, ipv);
    JS_ToInt32(ctx, &family, f);
    if (JS_IsNumber(pv))
      JS_ToInt32(ctx, &port, pv);
    if (port <= 0)
      port = 53;
    if (ip && ((family == 6 && uv_ip6_addr(ip, port, (struct sockaddr_in6 *)&list[count].addr) == 0) ||
               (family != 6 && uv_ip4_addr(ip, port, (struct sockaddr_in *)&list[count].addr) == 0)))
      count++;
    JS_FreeCString(ctx, ip);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, ipv);
    JS_FreeValue(ctx, pv);
    JS_FreeValue(ctx, e);
  }
  free(c->servers);
  c->servers = list;
  c->nservers = count;
  return JS_NewInt32(ctx, 0);
}

/* setLocalAddress(ip0[, ip1]): one IPv4 and/or one IPv6 address */
static JSValue channel_set_local_address(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
  Channel *c = JS_GetOpaque2(ctx, this_val, channel_class_id);
  struct sockaddr_in a4;
  struct sockaddr_in6 a6;
  bool has4 = false, has6 = false;
  int i;
  if (!c)
    return JS_EXCEPTION;
  for (i = 0; i < 2; i++) {
    JSValueConst v = i < argc ? argv[i] : JS_UNDEFINED;
    const char *ip;
    if (i == 1 && JS_IsUndefined(v))
      break;
    ip = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    if (ip && uv_ip4_addr(ip, 0, &a4) == 0) {
      JS_FreeCString(ctx, ip);
      if (has4)
        return node_throw_type_error(ctx, "ERR_INVALID_ARG_VALUE",
                                     "Cannot specify two IPv4 addresses.");
      has4 = true;
    } else if (ip && uv_ip6_addr(ip, 0, &a6) == 0) {
      JS_FreeCString(ctx, ip);
      if (has6)
        return node_throw_type_error(ctx, "ERR_INVALID_ARG_VALUE",
                                     "Cannot specify two IPv6 addresses.");
      has6 = true;
    } else {
      JS_FreeCString(ctx, ip);
      return node_throw_type_error(ctx, "ERR_INVALID_ARG_VALUE", "Invalid IP address.");
    }
  }
  /* an address not given goes back to the default (any) */
  c->has_local4 = has4;
  c->has_local6 = has6;
  if (has4)
    c->local4 = a4;
  if (has6)
    c->local6 = a6;
  return JS_UNDEFINED;
}

/* cancel(): every pending query ends with ECANCELLED */
static JSValue channel_cancel(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  Channel *c = JS_GetOpaque2(ctx, this_val, channel_class_id);
  if (!c)
    return JS_EXCEPTION;
  while (c->pending)
    dns_query_finish(c->pending, NULL, 0, "ECANCELLED");
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry channel_proto[] = {
  JS_CFUNC_MAGIC_DEF("queryAny", 2, channel_query, T_ANY),
  JS_CFUNC_MAGIC_DEF("queryA", 2, channel_query, T_A),
  JS_CFUNC_MAGIC_DEF("queryAaaa", 2, channel_query, T_AAAA),
  JS_CFUNC_MAGIC_DEF("queryCaa", 2, channel_query, T_CAA),
  JS_CFUNC_MAGIC_DEF("queryCname", 2, channel_query, T_CNAME),
  JS_CFUNC_MAGIC_DEF("queryMx", 2, channel_query, T_MX),
  JS_CFUNC_MAGIC_DEF("queryNs", 2, channel_query, T_NS),
  JS_CFUNC_MAGIC_DEF("queryTlsa", 2, channel_query, T_TLSA),
  JS_CFUNC_MAGIC_DEF("queryTxt", 2, channel_query, T_TXT),
  JS_CFUNC_MAGIC_DEF("querySrv", 2, channel_query, T_SRV),
  JS_CFUNC_MAGIC_DEF("queryPtr", 2, channel_query, T_PTR),
  JS_CFUNC_MAGIC_DEF("queryNaptr", 2, channel_query, T_NAPTR),
  JS_CFUNC_MAGIC_DEF("querySoa", 2, channel_query, T_SOA),
  JS_CFUNC_DEF("getHostByAddr", 2, channel_get_host_by_addr),
  JS_CFUNC_DEF("getServers", 0, channel_get_servers),
  JS_CFUNC_DEF("setServers", 1, channel_set_servers),
  JS_CFUNC_DEF("setLocalAddress", 2, channel_set_local_address),
  JS_CFUNC_DEF("cancel", 0, channel_cancel),
};

static const JSCFunctionListEntry dns_req_proto[] = {};

JSValue binding_init_cares_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  NodeClassDef gdef = { .name = "GetAddrInfoReqWrap", .class_id = &gai_req_class_id,
                        .ctor = gai_ctor, .finalizer = dns_req_finalizer,
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef ndef = { .name = "GetNameInfoReqWrap", .class_id = &gni_req_class_id,
                        .ctor = gni_ctor, .finalizer = dns_req_finalizer,
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef qdef = { .name = "QueryReqWrap", .class_id = &query_req_class_id,
                        .ctor = query_ctor, .finalizer = dns_req_finalizer,
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef cdef = { .name = "ChannelWrap", .class_id = &channel_class_id,
                        .ctor = channel_ctor, .ctor_length = 2,
                        .finalizer = channel_finalizer, .proto_funcs = channel_proto,
                        .proto_funcs_count = countof(channel_proto),
                        .parent_ctor = JS_UNDEFINED };
  (void)dns_req_proto;
  JS_FreeValue(ctx, nb_define_class(ctx, t, &gdef));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &ndef));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &qdef));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &cdef));
  nb_set_method(ctx, t, "getaddrinfo", c_getaddrinfo, 5);
  nb_set_method(ctx, t, "getnameinfo", c_getnameinfo, 3);
  nb_set_method(ctx, t, "canonicalizeIP", c_canonicalize_ip, 1);
  nb_set_method(ctx, t, "convertIpv6StringToBuffer", c_convert_ipv6, 1);
  nb_set_method(ctx, t, "strerror", c_strerror, 1);
  nb_set_int(ctx, t, "AF_INET", AF_INET);
  nb_set_int(ctx, t, "AF_INET6", AF_INET6);
  nb_set_int(ctx, t, "AF_UNSPEC", AF_UNSPEC);
  nb_set_int(ctx, t, "AI_ADDRCONFIG", AI_ADDRCONFIG);
  nb_set_int(ctx, t, "AI_ALL", AI_ALL);
  nb_set_int(ctx, t, "AI_V4MAPPED", AI_V4MAPPED);
  nb_set_int(ctx, t, "DNS_ORDER_VERBATIM", DNS_ORDER_VERBATIM);
  nb_set_int(ctx, t, "DNS_ORDER_IPV4_FIRST", DNS_ORDER_IPV4_FIRST);
  nb_set_int(ctx, t, "DNS_ORDER_IPV6_FIRST", DNS_ORDER_IPV6_FIRST);
  return t;
}

/* ---------------------------------------------------------------------- */
/* os */

static void collect_uv_exception(JSContext *ctx, JSValueConst ctxobj, int err,
                                 const char *syscall) {
  if (!JS_IsObject(ctxobj))
    return;
  JS_SetPropertyStr(ctx, ctxobj, "errno", JS_NewInt32(ctx, err));
  JS_SetPropertyStr(ctx, ctxobj, "code", JS_NewString(ctx, uv_err_name(err)));
  JS_SetPropertyStr(ctx, ctxobj, "message", JS_NewString(ctx, uv_strerror(err)));
  JS_SetPropertyStr(ctx, ctxobj, "syscall", JS_NewString(ctx, syscall));
}

static JSValue os_get_hostname(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  char buf[UV_MAXHOSTNAMESIZE];
  size_t len = sizeof(buf);
  int err = uv_os_gethostname(buf, &len);
  if (err) {
    collect_uv_exception(ctx, ARG(argc - 1), err, "uv_os_gethostname");
    return JS_UNDEFINED;
  }
  return JS_NewString(ctx, buf);
}

static JSValue os_get_load_avg(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  size_t len;
  double *a = (double *)nb_buffer_data(ctx, ARG(0), &len), l[3];
  uv_loadavg(l);
  if (a && len >= 24)
    memcpy(a, l, sizeof(l));
  return JS_UNDEFINED;
}

static JSValue os_get_uptime(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  double up;
  int err = uv_uptime(&up);
  if (err) {
    collect_uv_exception(ctx, ARG(argc - 1), err, "uv_uptime");
    return JS_UNDEFINED;
  }
  return JS_NewFloat64(ctx, up);
}

static JSValue os_get_total_mem(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  return JS_NewFloat64(ctx, (double)uv_get_total_memory());
}

static JSValue os_get_free_mem(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  return JS_NewFloat64(ctx, (double)uv_get_free_memory());
}

static JSValue os_get_cpus(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  uv_cpu_info_t *ci;
  int count, i;
  uint32_t k = 0;
  JSValue arr;
  if (uv_cpu_info(&ci, &count))
    return JS_UNDEFINED;
  arr = JS_NewArray(ctx);
  for (i = 0; i < count; i++) {
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, ci[i].model ? ci[i].model : "unknown"));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewFloat64(ctx, ci[i].speed));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewFloat64(ctx, (double)ci[i].cpu_times.user));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewFloat64(ctx, (double)ci[i].cpu_times.nice));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewFloat64(ctx, (double)ci[i].cpu_times.sys));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewFloat64(ctx, (double)ci[i].cpu_times.idle));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewFloat64(ctx, (double)ci[i].cpu_times.irq));
  }
  uv_free_cpu_info(ci, count);
  return arr;
}

static JSValue os_get_interface_addresses(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
  uv_interface_address_t *ifs;
  int count, i, err;
  uint32_t k = 0;
  JSValue arr;
  err = uv_interface_addresses(&ifs, &count);
  if (err == UV_ENOSYS)
    return JS_UNDEFINED;
  if (err) {
    collect_uv_exception(ctx, ARG(argc - 1), err, "uv_interface_addresses");
    return JS_UNDEFINED;
  }
  arr = JS_NewArray(ctx);
  for (i = 0; i < count; i++) {
    char ip[INET6_ADDRSTRLEN], mask[INET6_ADDRSTRLEN], mac[18];
    const char *fam;
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             (unsigned char)ifs[i].phys_addr[0], (unsigned char)ifs[i].phys_addr[1],
             (unsigned char)ifs[i].phys_addr[2], (unsigned char)ifs[i].phys_addr[3],
             (unsigned char)ifs[i].phys_addr[4], (unsigned char)ifs[i].phys_addr[5]);
    if (ifs[i].address.address4.sin_family == AF_INET) {
      uv_ip4_name(&ifs[i].address.address4, ip, sizeof(ip));
      uv_ip4_name(&ifs[i].netmask.netmask4, mask, sizeof(mask));
      fam = "IPv4";
    } else if (ifs[i].address.address4.sin_family == AF_INET6) {
      uv_ip6_name(&ifs[i].address.address6, ip, sizeof(ip));
      uv_ip6_name(&ifs[i].netmask.netmask6, mask, sizeof(mask));
      fam = "IPv6";
    } else {
      snprintf(ip, sizeof(ip), "<unknown sa family>");
      mask[0] = 0;
      fam = "unknown";
    }
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, ifs[i].name));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, ip));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, mask));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, fam));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, mac));
    JS_SetPropertyUint32(ctx, arr, k++, JS_NewBool(ctx, ifs[i].is_internal));
    JS_SetPropertyUint32(ctx, arr, k++, ifs[i].address.address4.sin_family == AF_INET6
                                            ? JS_NewUint32(ctx, ifs[i].address.address6.sin6_scope_id)
                                            : JS_NewInt32(ctx, -1));
  }
  uv_free_interface_addresses(ifs, count);
  return arr;
}

static JSValue os_get_home_directory(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  char buf[4096];
  size_t len = sizeof(buf);
  int err = uv_os_homedir(buf, &len);
  if (err) {
    collect_uv_exception(ctx, ARG(argc - 1), err, "uv_os_homedir");
    return JS_UNDEFINED;
  }
  return node_new_utf8_string(ctx, (uint8_t *)buf, len);
}

static JSValue os_get_user_info(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  uv_passwd_t pwd;
  int enc = ENC_UTF8, err;
  JSValue o;
  if (JS_IsObject(ARG(0))) {
    JSValue e = JS_GetPropertyStr(ctx, ARG(0), "encoding");
    enc = node_parse_encoding(ctx, e, ENC_UTF8);
    JS_FreeValue(ctx, e);
  }
  err = uv_os_get_passwd(&pwd);
  if (err) {
    collect_uv_exception(ctx, ARG(argc - 1), err, "uv_os_get_passwd");
    return JS_UNDEFINED;
  }
  o = JS_NewObjectProto(ctx, JS_NULL);
  JS_SetPropertyStr(ctx, o, "uid", JS_NewFloat64(ctx, (double)pwd.uid));
  JS_SetPropertyStr(ctx, o, "gid", JS_NewFloat64(ctx, (double)pwd.gid));
  JS_SetPropertyStr(ctx, o, "username",
                    node_string_encode(ctx, (uint8_t *)pwd.username, strlen(pwd.username), enc));
  JS_SetPropertyStr(ctx, o, "homedir",
                    node_string_encode(ctx, (uint8_t *)pwd.homedir, strlen(pwd.homedir), enc));
  JS_SetPropertyStr(ctx, o, "shell", pwd.shell ? node_string_encode(ctx, (uint8_t *)pwd.shell,
                                                                     strlen(pwd.shell), enc)
                                               : JS_NULL);
  uv_os_free_passwd(&pwd);
  return o;
}

static JSValue os_set_priority(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  int err = uv_os_setpriority(nb_int32(ctx, ARG(0), 0), nb_int32(ctx, ARG(1), 0));
  if (err)
    collect_uv_exception(ctx, ARG(2), err, "uv_os_setpriority");
  return JS_NewInt32(ctx, err);
}

static JSValue os_get_priority(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  int prio = 0, err = uv_os_getpriority(nb_int32(ctx, ARG(0), 0), &prio);
  if (err) {
    collect_uv_exception(ctx, ARG(1), err, "uv_os_getpriority");
    return JS_UNDEFINED;
  }
  return JS_NewInt32(ctx, prio);
}

static JSValue os_get_available_parallelism(JSContext *ctx, JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  return JS_NewUint32(ctx, uv_available_parallelism());
}

static JSValue os_get_os_information(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  uv_utsname_t info;
  int err = uv_os_uname(&info);
  JSValue arr;
  if (err) {
    collect_uv_exception(ctx, ARG(argc - 1), err, "uv_os_uname");
    return JS_UNDEFINED;
  }
  arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, info.sysname));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewString(ctx, info.version));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewString(ctx, info.release));
  JS_SetPropertyUint32(ctx, arr, 3, JS_NewString(ctx, info.machine));
  return arr;
}

JSValue binding_init_os(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set_method(ctx, t, "getHostname", os_get_hostname, 1);
  nb_set_method(ctx, t, "getLoadAvg", os_get_load_avg, 1);
  nb_set_method(ctx, t, "getUptime", os_get_uptime, 1);
  nb_set_method(ctx, t, "getTotalMem", os_get_total_mem, 0);
  nb_set_method(ctx, t, "getFreeMem", os_get_free_mem, 0);
  nb_set_method(ctx, t, "getCPUs", os_get_cpus, 0);
  nb_set_method(ctx, t, "getInterfaceAddresses", os_get_interface_addresses, 1);
  nb_set_method(ctx, t, "getHomeDirectory", os_get_home_directory, 1);
  nb_set_method(ctx, t, "getUserInfo", os_get_user_info, 2);
  nb_set_method(ctx, t, "setPriority", os_set_priority, 3);
  nb_set_method(ctx, t, "getPriority", os_get_priority, 2);
  nb_set_method(ctx, t, "getAvailableParallelism", os_get_available_parallelism, 0);
  nb_set_method(ctx, t, "getOSInformation", os_get_os_information, 1);
  nb_set_bool(ctx, t, "isBigEndian", false);
  return t;
}

/* ---------------------------------------------------------------------- */
/* fs_event_wrap: fs.watch
 *
 * Over inotify where the kernel has it.  The collaboCore guest's has not (and
 * a virtiofs mount would not report the host's changes anyway): there the
 * watcher polls, comparing stat()s of the file, or of a directory's entries,
 * and reports what inotify would ('rename' for names that came or went,
 * 'change' for contents). */

#define FS_POLL_MS 500
#define FS_POLL_MAX_ENTRIES 20000

typedef struct {
  char *name;               /* relative to the watched directory ("" for itself) */
  uint64_t ino, size;
  int64_t mtime_s, mtime_ns, ctime_s, ctime_ns;
} FsSnap;

typedef struct {
  HandleWrap hw;
  uv_fs_event_t ev;
  uv_timer_t poll;          /* instead of ev, without inotify */
  bool polling;
  char *path;
  bool recursive;
  FsSnap *snap;             /* the last scan, sorted by name */
  int nsnap;
  bool exists;              /* the watched path, at the last scan */
  int encoding;
  bool initialized;
} FSEventWrap;

static void fs_event_emit(FSEventWrap *w, const char *filename, int events, int status) {
  JSContext *ctx = w->hw.aw.env->ctx;
  JSValue args[3], r;
  const char *type = (events & UV_RENAME) ? "rename" : "change";
  args[0] = JS_NewInt32(ctx, status);
  args[1] = JS_NewString(ctx, type);
  args[2] = filename ? node_string_encode(ctx, (const uint8_t *)filename, strlen(filename),
                                          w->encoding)
                     : JS_NULL;
  r = async_wrap_make_callback_name(&w->hw.aw, "onchange", 3, (JSValueConst *)args);
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, args[2]);
}

static void fs_event_cb(uv_fs_event_t *h, const char *filename, int events, int status) {
  fs_event_emit(h->data, filename, events, status);
}

/* ---- polling ---- */

static bool inotify_available(void) {
  static int ok = -1;
  if (ok < 0) {
    uv_loop_t loop;
    uv_fs_event_t ev;
    int r;
    /* libuv makes the loop's inotify descriptor on the first start */
    ok = 1;
    if (uv_loop_init(&loop) == 0) {
      uv_fs_event_init(&loop, &ev);
      r = uv_fs_event_start(&ev, NULL, "/", 0);
      if (r == UV_ENOSYS || r == UV_ENOTSUP)
        ok = 0;
      uv_close((uv_handle_t *)&ev, NULL);
      uv_run(&loop, UV_RUN_DEFAULT);
      uv_loop_close(&loop);
    }
  }
  return ok == 1;
}

static int snap_cmp(const void *a, const void *b) {
  return strcmp(((const FsSnap *)a)->name, ((const FsSnap *)b)->name);
}

static void snap_free(FsSnap *s, int n) {
  int i;
  for (i = 0; i < n; i++)
    free(s[i].name);
  free(s);
}

static bool snap_stat(const char *path, FsSnap *e) {
  uv_fs_t req;
  bool ok = uv_fs_lstat(NULL, &req, path, NULL) == 0;
  if (ok) {
    e->ino = req.statbuf.st_ino;
    e->size = req.statbuf.st_size;
    e->mtime_s = req.statbuf.st_mtim.tv_sec;
    e->mtime_ns = req.statbuf.st_mtim.tv_nsec;
    e->ctime_s = req.statbuf.st_ctim.tv_sec;
    e->ctime_ns = req.statbuf.st_ctim.tv_nsec;
  }
  uv_fs_req_cleanup(&req);
  return ok;
}

typedef struct {
  FsSnap *v;
  int n, cap;
} SnapList;

static void snap_add(SnapList *l, const char *name, const FsSnap *e) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 64;
    l->v = realloc(l->v, l->cap * sizeof(FsSnap));
  }
  l->v[l->n] = *e;
  l->v[l->n++].name = strdup(name);
}

static void snap_dir(SnapList *l, const char *root, const char *rel, bool recursive, int depth) {
  uv_fs_t req;
  uv_dirent_t ent;
  char dir[4096];
  snprintf(dir, sizeof(dir), "%s%s%s", root, *rel ? "/" : "", rel);
  if (uv_fs_scandir(NULL, &req, dir, 0, NULL) < 0) {
    uv_fs_req_cleanup(&req);
    return;
  }
  while (uv_fs_scandir_next(&req, &ent) != UV_EOF && l->n < FS_POLL_MAX_ENTRIES) {
    char name[4096], full[4096];
    FsSnap e;
    snprintf(name, sizeof(name), "%s%s%s", rel, *rel ? "/" : "", ent.name);
    snprintf(full, sizeof(full), "%s/%s", root, name);
    memset(&e, 0, sizeof(e));
    if (!snap_stat(full, &e))
      continue;
    snap_add(l, name, &e);
    if (recursive && ent.type == UV_DIRENT_DIR && depth < 32)
      snap_dir(l, root, name, recursive, depth + 1);
  }
  uv_fs_req_cleanup(&req);
}

/* the watched path now: itself (name ""), and its entries if a directory */
static FsSnap *snap_take(FSEventWrap *w, int *n, bool *exists) {
  SnapList l = { 0 };
  FsSnap self;
  uv_fs_t req;
  bool is_dir = false;
  memset(&self, 0, sizeof(self));
  *exists = snap_stat(w->path, &self);
  if (*exists) {
    snap_add(&l, "", &self);
    if (uv_fs_stat(NULL, &req, w->path, NULL) == 0)
      is_dir = S_ISDIR(req.statbuf.st_mode);
    uv_fs_req_cleanup(&req);
    if (is_dir)
      snap_dir(&l, w->path, "", w->recursive, 0);
  }
  if (l.n > 1)
    qsort(l.v + 1, l.n - 1, sizeof(FsSnap), snap_cmp);
  *n = l.n;
  return l.v;
}

static const char *base_name(const char *path) {
  const char *s = strrchr(path, '/');
  return s && s[1] ? s + 1 : path;
}

static void fs_poll_cb(uv_timer_t *h) {
  FSEventWrap *w = h->data;
  FsSnap *now, *old = w->snap;
  int n, nold = w->nsnap, i = 1, j = 1;
  bool exists;
  if (!w->polling || w->hw.state != HW_INITIALIZED)
    return;
  now = snap_take(w, &n, &exists);
  w->snap = now;
  w->nsnap = n;
  /* the watched path itself */
  if (!exists || !w->exists) {
    if (exists != w->exists)
      fs_event_emit(w, base_name(w->path), UV_RENAME, 0);
    w->exists = exists;
    snap_free(old, nold);
    return;
  }
  if (now[0].ino != old[0].ino) {
    fs_event_emit(w, base_name(w->path), UV_RENAME, 0);
  } else if (n == 1 && nold == 1 &&
             (now[0].size != old[0].size || now[0].mtime_s != old[0].mtime_s ||
              now[0].mtime_ns != old[0].mtime_ns || now[0].ctime_s != old[0].ctime_s ||
              now[0].ctime_ns != old[0].ctime_ns)) {
    /* a file: its contents or attributes */
    fs_event_emit(w, base_name(w->path), UV_CHANGE, 0);
  }
  /* a directory's entries: merge the two sorted lists */
  while (i < n || j < nold) {
    int c = i >= n ? 1 : j >= nold ? -1 : strcmp(now[i].name, old[j].name);
    if (w->hw.state != HW_INITIALIZED)
      break;  /* closed from a callback */
    if (c < 0) {
      fs_event_emit(w, now[i++].name, UV_RENAME, 0);
    } else if (c > 0) {
      fs_event_emit(w, old[j++].name, UV_RENAME, 0);
    } else {
      if (now[i].ino != old[j].ino)
        fs_event_emit(w, now[i].name, UV_RENAME, 0);
      else if (now[i].size != old[j].size || now[i].mtime_s != old[j].mtime_s ||
               now[i].mtime_ns != old[j].mtime_ns)
        fs_event_emit(w, now[i].name, UV_CHANGE, 0);
      i++;
      j++;
    }
  }
  snap_free(old, nold);
}

static void fs_event_closed(HandleWrap *hw) {
  FSEventWrap *w = (FSEventWrap *)hw;
  snap_free(w->snap, w->nsnap);
  w->snap = NULL;
  w->nsnap = 0;
  free(w->path);
  w->path = NULL;
}

static void fs_event_finalizer(JSRuntime *rt, JSValueConst val) {
  FSEventWrap *w = JS_GetOpaque(val, node_fs_event_class_id);
  if (!w)
    return;
  async_wrap_destroy(&w->hw.aw);
  if (w->initialized && w->hw.state == HW_INITIALIZED) {
    node_close_and_free(w->hw.handle, w);
    return;
  }
  fs_event_closed(&w->hw);
  free(w);
}

static JSValue fs_event_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, node_fs_event_class_id);
  FSEventWrap *w;
  if (JS_IsException(obj))
    return obj;
  w = calloc(1, sizeof(*w));
  async_wrap_init(&w->hw.aw, env_get(ctx), obj, PROVIDER_FSEVENTWRAP, -1);
  w->hw.state = HW_CLOSED;
  w->hw.handle = (uv_handle_t *)&w->ev;
  w->hw.on_closed = fs_event_closed;
  JS_SetOpaque(obj, w);
  return obj;
}

/* start(filename, persistent, recursive, encoding) */
static JSValue fs_event_start(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  Env *env = env_get(ctx);
  FSEventWrap *w = JS_GetOpaque2(ctx, this_val, node_fs_event_class_id);
  char *path;
  int err;
  unsigned flags = 0;
  if (!w)
    return JS_EXCEPTION;
  if (w->initialized)
    return JS_NewInt32(ctx, 0);
  path = node_string_to_utf8(ctx, ARG(0), NULL);
  if (!path)
    return JS_EXCEPTION;
  if (JS_ToBool(ctx, ARG(2)))
    flags |= UV_FS_EVENT_RECURSIVE;
  w->encoding = node_parse_encoding(ctx, ARG(3), ENC_UTF8);
  if (!inotify_available()) {
    uv_fs_t req;
    err = uv_fs_stat(NULL, &req, path, NULL);
    uv_fs_req_cleanup(&req);
    if (err == 0)
      err = uv_timer_init(env->loop, &w->poll);
    if (err == 0) {
      w->polling = true;
      w->initialized = true;
      w->path = path;
      path = NULL;
      w->recursive = (flags & UV_FS_EVENT_RECURSIVE) != 0;
      w->snap = snap_take(w, &w->nsnap, &w->exists);
      node_handle_wrap_init(&w->hw, env, this_val, (uv_handle_t *)&w->poll, PROVIDER_FSEVENTWRAP);
      uv_timer_start(&w->poll, fs_poll_cb, FS_POLL_MS, FS_POLL_MS);
      if (!JS_ToBool(ctx, ARG(1)))
        uv_unref((uv_handle_t *)&w->poll);
    }
    free(path);
    return JS_NewInt32(ctx, err);
  }
  err = uv_fs_event_init(env->loop, &w->ev);
  if (err == 0) {
    w->initialized = true;
    node_handle_wrap_init(&w->hw, env, this_val, (uv_handle_t *)&w->ev, PROVIDER_FSEVENTWRAP);
    err = uv_fs_event_start(&w->ev, fs_event_cb, path, flags);
    if (err == 0 && !JS_ToBool(ctx, ARG(1)))
      uv_unref((uv_handle_t *)&w->ev);
    if (err)
      node_handle_wrap_close(&w->hw, JS_UNDEFINED);
  }
  free(path);
  return JS_NewInt32(ctx, err);
}

static JSValue fs_event_initialized(JSContext *ctx, JSValueConst this_val) {
  FSEventWrap *w = JS_GetOpaque(this_val, node_fs_event_class_id);
  return JS_NewBool(ctx, w && w->initialized);
}

static const JSCFunctionListEntry fs_event_proto[] = {
  JS_CFUNC_DEF("start", 4, fs_event_start),
  JS_CGETSET_DEF("initialized", fs_event_initialized, NULL),
};

JSValue binding_init_fs_event_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), ctor, proto;
  NodeClassDef def = { .name = "FSEvent", .class_id = &node_fs_event_class_id,
                       .ctor = fs_event_ctor, .finalizer = fs_event_finalizer,
                       .proto_funcs = fs_event_proto, .proto_funcs_count = countof(fs_event_proto),
                       .parent_ctor = JS_UNDEFINED };
  ctor = nb_define_class(ctx, t, &def);
  proto = JS_GetPropertyStr(ctx, ctor, "prototype");
  JS_SetPropertyFunctionList(ctx, proto, node_handle_wrap_funcs, node_handle_wrap_funcs_count);
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, ctor);
  return t;
}

/* ---------------------------------------------------------------------- */
/* udp_wrap: dgram */

typedef struct {
  HandleWrap hw;
  uv_udp_t udp;
} UDPWrap;

typedef struct {
  AsyncWrap aw;
} UDPSendReq;

static JSClassID udp_send_class_id;

typedef struct {
  uv_udp_send_t req;
  Env *env;
  JSValue req_obj;
  bool has_callback;
  size_t msg_size;
  uint8_t data[];
} UDPSend;

static UDPWrap *udp_of(JSContext *ctx, JSValueConst obj) {
  UDPWrap *u = JS_GetOpaque(obj, node_udp_class_id);
  if (!u || u->hw.state != HW_INITIALIZED)
    return NULL;
  return u;
}

static void udp_finalizer(JSRuntime *rt, JSValueConst val) {
  UDPWrap *u = JS_GetOpaque(val, node_udp_class_id);
  if (!u)
    return;
  async_wrap_destroy(&u->hw.aw);
  if (u->hw.state == HW_INITIALIZED) {
    node_close_and_free((uv_handle_t *)&u->udp, u);
    return;
  }
  free(u);
}

static JSValue udp_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JSValue obj = nb_new_instance(ctx, nt, node_udp_class_id);
  UDPWrap *u;
  if (JS_IsException(obj))
    return obj;
  u = calloc(1, sizeof(*u));
  uv_udp_init(env->loop, &u->udp);
  JS_SetOpaque(obj, u);
  node_handle_wrap_init(&u->hw, env, obj, (uv_handle_t *)&u->udp, PROVIDER_UDPWRAP);
  return obj;
}

static void udp_send_finalizer(JSRuntime *rt, JSValueConst val) {
  UDPSendReq *r = JS_GetOpaque(val, udp_send_class_id);
  if (r) {
    async_wrap_destroy(&r->aw);
    free(r);
  }
}

static JSValue udp_send_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, udp_send_class_id);
  UDPSendReq *r;
  if (JS_IsException(obj))
    return obj;
  r = calloc(1, sizeof(*r));
  async_wrap_init(&r->aw, env_get(ctx), obj, PROVIDER_UDPSENDWRAP, -1);
  JS_SetOpaque(obj, r);
  return obj;
}

static int sockaddr_for(int family, const char *ip, int port, struct sockaddr_storage *ss) {
  if (family == AF_INET6)
    return uv_ip6_addr(ip, port, (struct sockaddr_in6 *)ss);
  return uv_ip4_addr(ip, port, (struct sockaddr_in *)ss);
}

static JSValue udp_bind_common(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int family) {
  UDPWrap *u = udp_of(ctx, this_val);
  struct sockaddr_storage ss;
  const char *ip;
  int err;
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  ip = JS_ToCString(ctx, ARG(0));
  if (!ip)
    return JS_EXCEPTION;
  err = sockaddr_for(family, ip, nb_uint32(ctx, ARG(1), 0), &ss);
  JS_FreeCString(ctx, ip);
  if (err == 0)
    err = uv_udp_bind(&u->udp, (struct sockaddr *)&ss, nb_uint32(ctx, ARG(2), 0));
  return JS_NewInt32(ctx, err);
}

static JSValue udp_bind4(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return udp_bind_common(ctx, t, argc, argv, AF_INET);
}
static JSValue udp_bind6(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return udp_bind_common(ctx, t, argc, argv, AF_INET6);
}

static JSValue udp_connect_common(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv, int family) {
  UDPWrap *u = udp_of(ctx, this_val);
  struct sockaddr_storage ss;
  const char *ip;
  int err;
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  ip = JS_ToCString(ctx, ARG(0));
  if (!ip)
    return JS_EXCEPTION;
  err = sockaddr_for(family, ip, nb_uint32(ctx, ARG(1), 0), &ss);
  JS_FreeCString(ctx, ip);
  if (err == 0)
    err = uv_udp_connect(&u->udp, (struct sockaddr *)&ss);
  return JS_NewInt32(ctx, err);
}

static JSValue udp_connect4(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return udp_connect_common(ctx, t, argc, argv, AF_INET);
}
static JSValue udp_connect6(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return udp_connect_common(ctx, t, argc, argv, AF_INET6);
}

static JSValue udp_disconnect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  UDPWrap *u = udp_of(ctx, t);
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, uv_udp_connect(&u->udp, NULL));
}

static JSValue udp_open(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  UDPWrap *u = udp_of(ctx, t);
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, uv_udp_open(&u->udp, nb_int32(ctx, ARG(0), -1)));
}

static void udp_after_send(uv_udp_send_t *req, int status) {
  UDPSend *s = req->data;
  JSContext *ctx = s->env->ctx;
  if (s->has_callback) {
    UDPSendReq *r = JS_GetOpaque(s->req_obj, udp_send_class_id);
    if (r) {
      JSValue args[2], ret;
      args[0] = JS_NewInt32(ctx, status);
      args[1] = JS_NewFloat64(ctx, (double)s->msg_size);
      ret = async_wrap_make_callback_name(&r->aw, "oncomplete", 2, (JSValueConst *)args);
      JS_FreeValue(ctx, ret);
    }
  }
  JS_FreeValue(ctx, s->req_obj);
  free(s);
}

/* send(req, chunks, count[, port, address], hasCallback) */
static JSValue udp_send_common(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int family) {
  Env *env = env_get(ctx);
  UDPWrap *u = udp_of(ctx, this_val);
  bool sendto = argc == 6;
  uint32_t count = nb_uint32(ctx, ARG(2), 0), i;
  struct sockaddr_storage ss;
  struct sockaddr *addr = NULL;
  size_t total = 0, off = 0;
  uv_buf_t buf;
  UDPSend *s;
  int err = 0;
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  for (i = 0; i < count; i++) {
    JSValue c = JS_GetPropertyUint32(ctx, ARG(1), i);
    size_t len = 0;
    nb_buffer_data(ctx, c, &len);
    total += len;
    JS_FreeValue(ctx, c);
  }
  if (sendto) {
    const char *ip = JS_ToCString(ctx, ARG(4));
    if (!ip)
      return JS_EXCEPTION;
    err = sockaddr_for(family, ip, nb_uint32(ctx, ARG(3), 0), &ss);
    JS_FreeCString(ctx, ip);
    if (err)
      return JS_NewInt32(ctx, err);
    addr = (struct sockaddr *)&ss;
  }
  s = malloc(sizeof(*s) + (total ? total : 1));
  for (i = 0; i < count; i++) {
    JSValue c = JS_GetPropertyUint32(ctx, ARG(1), i);
    size_t len = 0;
    uint8_t *d = nb_buffer_data(ctx, c, &len);
    if (d && len)
      memcpy(s->data + off, d, len);
    off += len;
    JS_FreeValue(ctx, c);
  }
  buf = uv_buf_init((char *)s->data, (unsigned)total);
  err = uv_udp_try_send(&u->udp, &buf, 1, addr);
  if (err >= 0 && (size_t)err == total) {
    free(s);
    return JS_NewFloat64(ctx, (double)total + 1); /* sent synchronously */
  }
  s->env = env;
  s->req.data = s;
  s->req_obj = JS_DupValue(ctx, ARG(0));
  s->has_callback = JS_ToBool(ctx, sendto ? ARG(5) : ARG(3));
  s->msg_size = total;
  err = uv_udp_send(&s->req, &u->udp, &buf, 1, addr, udp_after_send);
  if (err) {
    JS_FreeValue(ctx, s->req_obj);
    free(s);
  }
  return JS_NewInt32(ctx, err);
}

static JSValue udp_send4(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return udp_send_common(ctx, t, argc, argv, AF_INET);
}
static JSValue udp_send6(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return udp_send_common(ctx, t, argc, argv, AF_INET6);
}

static void udp_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
  buf->base = malloc(65536);
  buf->len = 65536;
}

static void udp_recv(uv_udp_t *h, ssize_t nread, const uv_buf_t *buf,
                     const struct sockaddr *addr, unsigned flags) {
  UDPWrap *u = h->data;
  JSContext *ctx;
  JSValue args[4], ret;
  if (!u || (nread == 0 && addr == NULL)) {
    free(buf->base);
    return;
  }
  ctx = u->hw.aw.env->ctx;
  args[0] = JS_NewInt32(ctx, (int32_t)nread);
  args[1] = JS_DupValue(ctx, u->hw.aw.object);
  args[2] = JS_UNDEFINED;
  args[3] = JS_UNDEFINED;
  if (nread >= 0) {
    args[2] = nb_new_buffer(ctx, buf->base, (size_t)nread);
    args[3] = JS_NewObject(ctx);
    node_address_to_js(ctx, addr, args[3]);
  }
  free(buf->base);
  ret = async_wrap_make_callback_name(&u->hw.aw, "onmessage", 4, (JSValueConst *)args);
  JS_FreeValue(ctx, ret);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, args[2]);
  JS_FreeValue(ctx, args[3]);
}

static JSValue udp_recv_start(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  UDPWrap *u = udp_of(ctx, t);
  int err;
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  err = uv_udp_recv_start(&u->udp, udp_alloc, udp_recv);
  if (err == UV_EALREADY)
    err = 0;
  return JS_NewInt32(ctx, err);
}

static JSValue udp_recv_stop(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  UDPWrap *u = udp_of(ctx, t);
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  return JS_NewInt32(ctx, uv_udp_recv_stop(&u->udp));
}

static JSValue udp_sockname(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv,
                            int peer) {
  UDPWrap *u = udp_of(ctx, t);
  struct sockaddr_storage ss;
  int len = sizeof(ss), err;
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  err = peer ? uv_udp_getpeername(&u->udp, (struct sockaddr *)&ss, &len)
             : uv_udp_getsockname(&u->udp, (struct sockaddr *)&ss, &len);
  if (err == 0)
    node_address_to_js(ctx, (struct sockaddr *)&ss, ARG(0));
  return JS_NewInt32(ctx, err);
}

static JSValue udp_getsockname(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return udp_sockname(ctx, t, argc, argv, 0);
}
static JSValue udp_getpeername(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  return udp_sockname(ctx, t, argc, argv, 1);
}

static JSValue udp_membership(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv,
                              int magic) {
  UDPWrap *u = udp_of(ctx, t);
  const char *a, *b = NULL, *c = NULL;
  int err;
  bool ssm = magic >= 2;
  uv_membership m = (magic & 1) ? UV_LEAVE_GROUP : UV_JOIN_GROUP;
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  a = JS_ToCString(ctx, ARG(0));
  if (ssm) {
    b = JS_ToCString(ctx, ARG(1));
    if (!JS_IsUndefined(ARG(2)) && !JS_IsNull(ARG(2)))
      c = JS_ToCString(ctx, ARG(2));
    err = uv_udp_set_source_membership(&u->udp, b, c, a, m);
  } else {
    if (!JS_IsUndefined(ARG(1)) && !JS_IsNull(ARG(1)))
      b = JS_ToCString(ctx, ARG(1));
    err = uv_udp_set_membership(&u->udp, a, b, m);
  }
  JS_FreeCString(ctx, a);
  if (b)
    JS_FreeCString(ctx, b);
  if (c)
    JS_FreeCString(ctx, c);
  return JS_NewInt32(ctx, err);
}

static JSValue udp_set_multicast_interface(JSContext *ctx, JSValueConst t, int argc,
                                           JSValueConst *argv) {
  UDPWrap *u = udp_of(ctx, t);
  const char *i;
  int err;
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  i = JS_ToCString(ctx, ARG(0));
  err = uv_udp_set_multicast_interface(&u->udp, i);
  JS_FreeCString(ctx, i);
  return JS_NewInt32(ctx, err);
}

static JSValue udp_set_opt(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv,
                           int magic) {
  UDPWrap *u = udp_of(ctx, t);
  int v = nb_int32(ctx, ARG(0), 0);
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  switch (magic) {
  case 0: return JS_NewInt32(ctx, uv_udp_set_multicast_ttl(&u->udp, v));
  case 1: return JS_NewInt32(ctx, uv_udp_set_multicast_loop(&u->udp, JS_ToBool(ctx, ARG(0))));
  case 2: return JS_NewInt32(ctx, uv_udp_set_broadcast(&u->udp, JS_ToBool(ctx, ARG(0))));
  default: return JS_NewInt32(ctx, uv_udp_set_ttl(&u->udp, v));
  }
}

static JSValue udp_buffer_size(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
  UDPWrap *u = udp_of(ctx, t);
  bool recv = JS_ToBool(ctx, ARG(1));
  const char *fn = recv ? "uv_recv_buffer_size" : "uv_send_buffer_size";
  int size, err;
  if (!u)
    return JS_NewInt32(ctx, UV_EBADF);
  if (!JS_IsNumber(ARG(0)) || nb_double(ctx, ARG(0), -1) > INT32_MAX) {
    collect_uv_exception(ctx, ARG(2), UV_EINVAL, fn);
    return JS_UNDEFINED;
  }
  size = nb_int32(ctx, ARG(0), 0);
  err = recv ? uv_recv_buffer_size((uv_handle_t *)&u->udp, &size)
             : uv_send_buffer_size((uv_handle_t *)&u->udp, &size);
  if (err) {
    collect_uv_exception(ctx, ARG(2), err, fn);
    return JS_UNDEFINED;
  }
  return JS_NewInt32(ctx, size);
}

static JSValue udp_send_queue_size(JSContext *ctx, JSValueConst t, int argc,
                                   JSValueConst *argv) {
  UDPWrap *u = JS_GetOpaque(t, node_udp_class_id);
  return JS_NewFloat64(ctx, u ? (double)uv_udp_get_send_queue_size(&u->udp) : 0);
}

static JSValue udp_send_queue_count(JSContext *ctx, JSValueConst t, int argc,
                                    JSValueConst *argv) {
  UDPWrap *u = JS_GetOpaque(t, node_udp_class_id);
  return JS_NewFloat64(ctx, u ? (double)uv_udp_get_send_queue_count(&u->udp) : 0);
}

static JSValue udp_get_fd(JSContext *ctx, JSValueConst t) {
  UDPWrap *u = JS_GetOpaque(t, node_udp_class_id);
  uv_os_fd_t fd = -1;
  if (u && u->hw.state == HW_INITIALIZED)
    uv_fileno((uv_handle_t *)&u->udp, &fd);
  return JS_NewInt32(ctx, fd);
}

static const JSCFunctionListEntry udp_proto[] = {
  JS_CFUNC_DEF("open", 1, udp_open),
  JS_CFUNC_DEF("bind", 3, udp_bind4),
  JS_CFUNC_DEF("bind6", 3, udp_bind6),
  JS_CFUNC_DEF("connect", 2, udp_connect4),
  JS_CFUNC_DEF("connect6", 2, udp_connect6),
  JS_CFUNC_DEF("send", 6, udp_send4),
  JS_CFUNC_DEF("send6", 6, udp_send6),
  JS_CFUNC_DEF("disconnect", 0, udp_disconnect),
  JS_CFUNC_DEF("recvStart", 0, udp_recv_start),
  JS_CFUNC_DEF("recvStop", 0, udp_recv_stop),
  JS_CFUNC_DEF("getsockname", 1, udp_getsockname),
  JS_CFUNC_DEF("getpeername", 1, udp_getpeername),
  JS_CFUNC_MAGIC_DEF("addMembership", 2, udp_membership, 0),
  JS_CFUNC_MAGIC_DEF("dropMembership", 2, udp_membership, 1),
  JS_CFUNC_MAGIC_DEF("addSourceSpecificMembership", 3, udp_membership, 2),
  JS_CFUNC_MAGIC_DEF("dropSourceSpecificMembership", 3, udp_membership, 3),
  JS_CFUNC_DEF("setMulticastInterface", 1, udp_set_multicast_interface),
  JS_CFUNC_MAGIC_DEF("setMulticastTTL", 1, udp_set_opt, 0),
  JS_CFUNC_MAGIC_DEF("setMulticastLoopback", 1, udp_set_opt, 1),
  JS_CFUNC_MAGIC_DEF("setBroadcast", 1, udp_set_opt, 2),
  JS_CFUNC_MAGIC_DEF("setTTL", 1, udp_set_opt, 3),
  JS_CFUNC_DEF("bufferSize", 3, udp_buffer_size),
  JS_CFUNC_DEF("getSendQueueSize", 0, udp_send_queue_size),
  JS_CFUNC_DEF("getSendQueueCount", 0, udp_send_queue_count),
  JS_CGETSET_DEF("fd", udp_get_fd, NULL),
};

JSValue binding_init_udp_wrap(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), c = JS_NewObject(ctx), ctor, proto;
  NodeClassDef def = { .name = "UDP", .class_id = &node_udp_class_id,
                       .ctor = udp_ctor, .finalizer = udp_finalizer,
                       .proto_funcs = udp_proto, .proto_funcs_count = countof(udp_proto),
                       .parent_ctor = JS_UNDEFINED };
  NodeClassDef sdef = { .name = "SendWrap", .class_id = &udp_send_class_id,
                        .ctor = udp_send_ctor, .finalizer = udp_send_finalizer,
                        .parent_ctor = JS_UNDEFINED };
  ctor = nb_define_class(ctx, t, &def);
  proto = JS_GetPropertyStr(ctx, ctor, "prototype");
  JS_SetPropertyFunctionList(ctx, proto, node_handle_wrap_funcs, node_handle_wrap_funcs_count);
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, nb_define_class(ctx, t, &sdef));
  nb_set_int(ctx, c, "UV_UDP_IPV6ONLY", UV_UDP_IPV6ONLY);
  nb_set_int(ctx, c, "UV_UDP_REUSEADDR", UV_UDP_REUSEADDR);
  nb_set_int(ctx, c, "UV_UDP_REUSEPORT", UV_UDP_REUSEPORT);
  nb_set(ctx, t, "constants", c);
  return t;
}
