/* internalBinding('block_list') (node_sockaddr.cc): SocketAddress handles
 * and the BlockList of addresses, ranges and subnets.  Addresses compare as
 * 128-bit IPv6, IPv4 as v4-mapped. */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "node.h"

static JSClassID sockaddr_class_id, blocklist_class_id;

typedef struct {
  int family; /* AF_INET / AF_INET6 */
  uint8_t bytes[16]; /* network order; v4 in the first 4 */
  int port;
  uint32_t flowlabel;
} SockAddr;

/* 128-bit comparable form */
static void norm(const SockAddr *a, uint8_t out[16]) {
  if (a->family == AF_INET) {
    memset(out, 0, 10);
    out[10] = out[11] = 0xff;
    memcpy(out + 12, a->bytes, 4);
  } else {
    memcpy(out, a->bytes, 16);
  }
}

static int cmp(const SockAddr *a, const SockAddr *b) {
  uint8_t x[16], y[16];
  norm(a, x);
  norm(b, y);
  return memcmp(x, y, 16);
}

static bool is_v4_mapped(const uint8_t n[16]) {
  static const uint8_t prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
  return !memcmp(n, prefix, 12);
}

static bool in_network(const SockAddr *a, const SockAddr *net, int prefix) {
  uint8_t x[16], y[16];
  int bits, i;
  norm(a, x);
  norm(net, y);
  if (net->family == AF_INET) {
    if (!is_v4_mapped(x))
      return false;
    bits = prefix + 96;
  } else {
    bits = prefix;
  }
  if (bits > 128)
    bits = 128;
  for (i = 0; i < bits / 8; i++)
    if (x[i] != y[i])
      return false;
  if (bits % 8) {
    uint8_t mask = (uint8_t)(0xff << (8 - bits % 8));
    if ((x[i] & mask) != (y[i] & mask))
      return false;
  }
  return true;
}

static void addr_string(const SockAddr *a, char *buf, size_t size) {
  inet_ntop(a->family, a->bytes, buf, (socklen_t)size);
}

static bool parse_addr(int family, const char *s, SockAddr *out) {
  memset(out, 0, sizeof(*out));
  out->family = family;
  return inet_pton(family, s, out->bytes) == 1;
}

/* ---- SocketAddress ---- */

static void sockaddr_finalizer(JSRuntime *rt, JSValueConst val) {
  free(JS_GetOpaque(val, sockaddr_class_id));
}

/* new SocketAddress(address, port, family, flowlabel) */
static JSValue sockaddr_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj;
  SockAddr *a = calloc(1, sizeof(*a));
  const char *s = JS_ToCString(ctx, ARG(0));
  int family = nb_int32(ctx, ARG(2), AF_INET);
  bool ok = s && parse_addr(family, s, a);
  JS_FreeCString(ctx, s);
  if (!ok) {
    free(a);
    return node_throw_type_error(ctx, "ERR_INVALID_ADDRESS", "Invalid socket address");
  }
  a->port = nb_int32(ctx, ARG(1), 0);
  a->flowlabel = nb_uint32(ctx, ARG(3), 0);
  obj = nb_new_instance(ctx, nt, sockaddr_class_id);
  if (JS_IsException(obj)) {
    free(a);
    return obj;
  }
  JS_SetOpaque(obj, a);
  return obj;
}

static SockAddr *sockaddr_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, sockaddr_class_id);
}

static JSValue sockaddr_detail(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  SockAddr *a = sockaddr_of(ctx, this_val);
  char buf[INET6_ADDRSTRLEN];
  if (!a)
    return JS_EXCEPTION;
  addr_string(a, buf, sizeof(buf));
  JS_SetPropertyStr(ctx, ARG(0), "address", JS_NewString(ctx, buf));
  JS_SetPropertyStr(ctx, ARG(0), "port", JS_NewInt32(ctx, a->port));
  JS_SetPropertyStr(ctx, ARG(0), "family", JS_NewInt32(ctx, a->family));
  JS_SetPropertyStr(ctx, ARG(0), "flowlabel", JS_NewUint32(ctx, a->flowlabel));
  return JS_DupValue(ctx, ARG(0));
}

static JSValue sockaddr_legacy_detail(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  SockAddr *a = sockaddr_of(ctx, this_val);
  char buf[INET6_ADDRSTRLEN];
  JSValue o;
  if (!a)
    return JS_EXCEPTION;
  addr_string(a, buf, sizeof(buf));
  o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "address", JS_NewString(ctx, buf));
  JS_SetPropertyStr(ctx, o, "family", JS_NewString(ctx, a->family == AF_INET6 ? "IPv6" : "IPv4"));
  JS_SetPropertyStr(ctx, o, "port", JS_NewInt32(ctx, a->port));
  return o;
}

static JSValue sockaddr_flowlabel(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
  SockAddr *a = sockaddr_of(ctx, this_val);
  return a ? JS_NewUint32(ctx, a->flowlabel) : JS_EXCEPTION;
}

static const JSCFunctionListEntry sockaddr_proto[] = {
  JS_CFUNC_DEF("detail", 1, sockaddr_detail),
  JS_CFUNC_DEF("legacyDetail", 0, sockaddr_legacy_detail),
  JS_CFUNC_DEF("flowlabel", 0, sockaddr_flowlabel),
};

/* ---- BlockList ---- */

enum { RULE_ADDRESS, RULE_RANGE, RULE_SUBNET };

typedef struct {
  int kind;
  SockAddr a, b; /* address / start..end / network */
  int prefix;
} Rule;

typedef struct {
  Rule *rules;
  int n, cap;
} BlockList;

static void blocklist_finalizer(JSRuntime *rt, JSValueConst val) {
  BlockList *b = JS_GetOpaque(val, blocklist_class_id);
  if (!b)
    return;
  free(b->rules);
  free(b);
}

static JSValue blocklist_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  JSValue obj = nb_new_instance(ctx, nt, blocklist_class_id);
  if (JS_IsException(obj))
    return obj;
  JS_SetOpaque(obj, calloc(1, sizeof(BlockList)));
  return obj;
}

static BlockList *bl_of(JSContext *ctx, JSValueConst v) {
  return JS_GetOpaque2(ctx, v, blocklist_class_id);
}

/* newest rules first, like Node's emplace_front */
static void add_rule(BlockList *b, const Rule *r) {
  if (b->n == b->cap) {
    b->cap = b->cap ? b->cap * 2 : 8;
    b->rules = realloc(b->rules, b->cap * sizeof(Rule));
  }
  memmove(b->rules + 1, b->rules, b->n * sizeof(Rule));
  b->rules[0] = *r;
  b->n++;
}

static void remove_rules(BlockList *b, const Rule *r) {
  int i, j = 0;
  for (i = 0; i < b->n; i++) {
    Rule *x = &b->rules[i];
    bool same = x->kind == r->kind && !cmp(&x->a, &r->a) &&
                (r->kind != RULE_RANGE || !cmp(&x->b, &r->b)) &&
                (r->kind != RULE_SUBNET || x->prefix == r->prefix);
    if (!same)
      b->rules[j++] = *x;
  }
  b->n = j;
}

static JSValue bl_add_address(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int remove) {
  BlockList *b = bl_of(ctx, this_val);
  SockAddr *a = sockaddr_of(ctx, ARG(0));
  Rule r = { RULE_ADDRESS };
  if (!b || !a)
    return JS_EXCEPTION;
  r.a = *a;
  if (remove)
    remove_rules(b, &r);
  else
    add_rule(b, &r);
  return JS_TRUE;
}

static JSValue bl_add_addresses(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  BlockList *b = bl_of(ctx, this_val);
  JSValue lenv;
  uint32_t n = 0, i;
  if (!b)
    return JS_EXCEPTION;
  lenv = JS_GetPropertyStr(ctx, ARG(0), "length");
  JS_ToUint32(ctx, &n, lenv);
  for (i = 0; i < n; i++) {
    JSValue item = JS_GetPropertyUint32(ctx, ARG(0), i);
    SockAddr *a = JS_GetOpaque(item, sockaddr_class_id);
    if (a) {
      Rule r = { RULE_ADDRESS };
      r.a = *a;
      add_rule(b, &r);
    }
    JS_FreeValue(ctx, item);
  }
  return JS_TRUE;
}

static JSValue bl_add_range(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int remove) {
  BlockList *b = bl_of(ctx, this_val);
  SockAddr *s = sockaddr_of(ctx, ARG(0)), *e;
  Rule r = { RULE_RANGE };
  if (!b || !s)
    return JS_EXCEPTION;
  e = sockaddr_of(ctx, ARG(1));
  if (!e)
    return JS_EXCEPTION;
  if (!remove && cmp(s, e) > 0)
    return JS_FALSE;
  r.a = *s;
  r.b = *e;
  if (remove)
    remove_rules(b, &r);
  else
    add_rule(b, &r);
  return JS_TRUE;
}

static JSValue bl_add_subnet(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int remove) {
  BlockList *b = bl_of(ctx, this_val);
  SockAddr *n = sockaddr_of(ctx, ARG(0));
  Rule r = { RULE_SUBNET };
  if (!b || !n)
    return JS_EXCEPTION;
  r.a = *n;
  r.prefix = nb_int32(ctx, ARG(1), 0);
  if (remove)
    remove_rules(b, &r);
  else
    add_rule(b, &r);
  return JS_TRUE;
}

static bool bl_apply(BlockList *b, const SockAddr *a) {
  int i;
  for (i = 0; i < b->n; i++) {
    Rule *r = &b->rules[i];
    switch (r->kind) {
    case RULE_ADDRESS:
      if (!cmp(a, &r->a))
        return true;
      break;
    case RULE_RANGE:
      if (cmp(a, &r->a) >= 0 && cmp(a, &r->b) <= 0)
        return true;
      break;
    default:
      if (in_network(a, &r->a, r->prefix))
        return true;
    }
  }
  return false;
}

static JSValue bl_check(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  BlockList *b = bl_of(ctx, this_val);
  SockAddr *a = sockaddr_of(ctx, ARG(0));
  if (!b || !a)
    return JS_EXCEPTION;
  return JS_NewBool(ctx, bl_apply(b, a));
}

static JSValue bl_check_string(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  BlockList *b = bl_of(ctx, this_val);
  const char *s;
  SockAddr a;
  bool ok;
  if (!b)
    return JS_EXCEPTION;
  s = JS_ToCString(ctx, ARG(0));
  ok = s && parse_addr(nb_int32(ctx, ARG(1), AF_INET), s, &a);
  JS_FreeCString(ctx, s);
  if (!ok)
    return JS_FALSE;
  return JS_NewBool(ctx, bl_apply(b, &a));
}

static JSValue bl_get_rules(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  BlockList *b = bl_of(ctx, this_val);
  JSValue arr;
  uint32_t k = 0;
  int pass, i;
  if (!b)
    return JS_EXCEPTION;
  arr = JS_NewArray(ctx);
  /* addresses, then subnets, then ranges (Node's ListRules order) */
  for (pass = 0; pass < 3; pass++) {
    int kind = pass == 0 ? RULE_ADDRESS : pass == 1 ? RULE_SUBNET : RULE_RANGE;
    for (i = 0; i < b->n; i++) {
      Rule *r = &b->rules[i];
      char a[INET6_ADDRSTRLEN], e[INET6_ADDRSTRLEN], line[160];
      if (r->kind != kind)
        continue;
      addr_string(&r->a, a, sizeof(a));
      if (kind == RULE_ADDRESS)
        snprintf(line, sizeof(line), "Address: %s %s", r->a.family == AF_INET ? "IPv4" : "IPv6", a);
      else if (kind == RULE_SUBNET)
        snprintf(line, sizeof(line), "Subnet: %s %s/%d", r->a.family == AF_INET ? "IPv4" : "IPv6", a,
                 r->prefix);
      else {
        addr_string(&r->b, e, sizeof(e));
        snprintf(line, sizeof(line), "Range: %s %s-%s", r->a.family == AF_INET ? "IPv4" : "IPv6", a,
                 e);
      }
      JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, line));
    }
  }
  return arr;
}

static JSValue bl_get_size(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  BlockList *b = bl_of(ctx, this_val);
  return b ? JS_NewInt32(ctx, b->n) : JS_EXCEPTION;
}

static JSValue bl_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  BlockList *b = bl_of(ctx, this_val);
  if (!b)
    return JS_EXCEPTION;
  b->n = 0;
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry blocklist_proto[] = {
  JS_CFUNC_MAGIC_DEF("addAddress", 1, bl_add_address, 0),
  JS_CFUNC_DEF("addAddresses", 1, bl_add_addresses),
  JS_CFUNC_MAGIC_DEF("addRange", 2, bl_add_range, 0),
  JS_CFUNC_MAGIC_DEF("addSubnet", 2, bl_add_subnet, 0),
  JS_CFUNC_MAGIC_DEF("removeAddress", 1, bl_add_address, 1),
  JS_CFUNC_MAGIC_DEF("removeRange", 2, bl_add_range, 1),
  JS_CFUNC_MAGIC_DEF("removeSubnet", 2, bl_add_subnet, 1),
  JS_CFUNC_DEF("check", 1, bl_check),
  JS_CFUNC_DEF("checkString", 2, bl_check_string),
  JS_CFUNC_DEF("getRules", 0, bl_get_rules),
  JS_CFUNC_DEF("getSize", 0, bl_get_size),
  JS_CFUNC_DEF("clear", 0, bl_clear),
};

JSValue binding_init_block_list(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  NodeClassDef bdef = { .name = "BlockList", .class_id = &blocklist_class_id,
                        .ctor = blocklist_ctor, .finalizer = blocklist_finalizer,
                        .proto_funcs = blocklist_proto, .proto_funcs_count = countof(blocklist_proto),
                        .parent_ctor = JS_UNDEFINED };
  NodeClassDef sdef = { .name = "SocketAddress", .class_id = &sockaddr_class_id,
                        .ctor = sockaddr_ctor, .ctor_length = 4, .finalizer = sockaddr_finalizer,
                        .proto_funcs = sockaddr_proto, .proto_funcs_count = countof(sockaddr_proto),
                        .parent_ctor = JS_UNDEFINED };
  JS_FreeValue(ctx, nb_define_class(ctx, t, &bdef));
  JS_FreeValue(ctx, nb_define_class(ctx, t, &sdef));
  nb_set_int(ctx, t, "AF_INET", AF_INET);
  nb_set_int(ctx, t, "AF_INET6", AF_INET6);
  return t;
}
