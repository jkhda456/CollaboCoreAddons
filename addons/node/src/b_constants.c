/* internalBinding('constants') (node_constants.cc): constants_gen.inc is
 * Node's own Define*Constants functions, made C (tools/gen-constants.py). */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/ec.h>
#include <openssl/ssl.h>
#include <zlib.h>
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zstd.h>

#include "node.h"

#define HAVE_OPENSSL 1
#define __POSIX__ 1
#if !defined(RSA_PKCS1_PADDING)
#define RSA_PKCS1_PADDING 1
#endif
#if !defined(RSA_SSLV23_PADDING)
#define RSA_SSLV23_PADDING 2
#endif
#if !defined(RSA_NO_PADDING)
#define RSA_NO_PADDING 3
#endif
#if !defined(RSA_PKCS1_OAEP_PADDING)
#define RSA_PKCS1_OAEP_PADDING 4
#endif
#if !defined(RSA_X931_PADDING)
#define RSA_X931_PADDING 5
#endif
#if !defined(RSA_PKCS1_PSS_PADDING)
#define RSA_PKCS1_PSS_PADDING 6
#endif
#if !defined(DH_CHECK_P_NOT_PRIME)
#define DH_CHECK_P_NOT_PRIME 0x01
#endif
#if !defined(DH_CHECK_P_NOT_SAFE_PRIME)
#define DH_CHECK_P_NOT_SAFE_PRIME 0x02
#endif
#if !defined(DH_UNABLE_TO_CHECK_GENERATOR)
#define DH_UNABLE_TO_CHECK_GENERATOR 0x04
#endif
#if !defined(DH_NOT_SUITABLE_GENERATOR)
#define DH_NOT_SUITABLE_GENERATOR 0x08
#endif
#ifndef OPENSSL_NO_ENGINE
#define OPENSSL_NO_ENGINE 1
#endif
#define ENGINE_METHOD_RSA 0x0001u
#define ENGINE_METHOD_DSA 0x0002u
#define ENGINE_METHOD_DH 0x0004u
#define ENGINE_METHOD_RAND 0x0008u
#define ENGINE_METHOD_CIPHERS 0x0040u
#define ENGINE_METHOD_DIGESTS 0x0080u
#define ENGINE_METHOD_PKEY_METHS 0x0200u
#define ENGINE_METHOD_PKEY_ASN1_METHS 0x0400u
#define ENGINE_METHOD_EC 0x0800u
#define ENGINE_METHOD_ALL 0xFFFFu
#define ENGINE_METHOD_NONE 0x0000u

#define EXTENSIONLESS_FORMAT_JAVASCRIPT (0)
#define EXTENSIONLESS_FORMAT_WASM (1)

#define TRACE_EVENT_PHASE_BEGIN ('B')
#define TRACE_EVENT_PHASE_END ('E')
#define TRACE_EVENT_PHASE_COMPLETE ('X')
#define TRACE_EVENT_PHASE_INSTANT ('I')
#define TRACE_EVENT_PHASE_ASYNC_BEGIN ('S')
#define TRACE_EVENT_PHASE_ASYNC_STEP_INTO ('T')
#define TRACE_EVENT_PHASE_ASYNC_STEP_PAST ('p')
#define TRACE_EVENT_PHASE_ASYNC_END ('F')
#define TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN ('b')
#define TRACE_EVENT_PHASE_NESTABLE_ASYNC_END ('e')
#define TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT ('n')
#define TRACE_EVENT_PHASE_FLOW_BEGIN ('s')
#define TRACE_EVENT_PHASE_FLOW_STEP ('t')
#define TRACE_EVENT_PHASE_FLOW_END ('f')
#define TRACE_EVENT_PHASE_METADATA ('M')
#define TRACE_EVENT_PHASE_COUNTER ('C')
#define TRACE_EVENT_PHASE_SAMPLE ('P')
#define TRACE_EVENT_PHASE_CREATE_OBJECT ('N')
#define TRACE_EVENT_PHASE_SNAPSHOT_OBJECT ('O')
#define TRACE_EVENT_PHASE_DELETE_OBJECT ('D')
#define TRACE_EVENT_PHASE_MEMORY_DUMP ('v')
#define TRACE_EVENT_PHASE_MARK ('R')
#define TRACE_EVENT_PHASE_CLOCK_SYNC ('c')
#define TRACE_EVENT_PHASE_ENTER_CONTEXT ('(')
#define TRACE_EVENT_PHASE_LEAVE_CONTEXT (')')
#define TRACE_EVENT_PHASE_LINK_IDS ('=')

/* node_zlib.cc */
enum node_zlib_mode { NONE, DEFLATE, INFLATE, GZIP, GUNZIP, DEFLATERAW, INFLATERAW,
                      UNZIP, BROTLI_DECODE, BROTLI_ENCODE, ZSTD_COMPRESS, ZSTD_DECOMPRESS };
#define Z_MIN_CHUNK 64
#define Z_MAX_CHUNK INFINITY
#define Z_DEFAULT_CHUNK (16 * 1024)
#define Z_MIN_MEMLEVEL 1
#define Z_MAX_MEMLEVEL 9
#define Z_DEFAULT_MEMLEVEL 8
#define Z_MIN_LEVEL -1
#define Z_MAX_LEVEL 9
#define Z_DEFAULT_LEVEL Z_DEFAULT_COMPRESSION
#define Z_MIN_WINDOWBITS 8
#define Z_MAX_WINDOWBITS 15
#define Z_DEFAULT_WINDOWBITS 15

static JSValue number(JSContext *ctx, double v) {
  if (v == (double)(int32_t)v)
    return JS_NewInt32(ctx, (int32_t)v);
  return JS_NewFloat64(ctx, v);
}

#define DEF(target, name) \
  JS_DefinePropertyValueStr(ctx, target, #name, number(ctx, (double)(name)), JS_PROP_ENUMERABLE)
#define DEF_STR(target, name, value) \
  JS_DefinePropertyValueStr(ctx, target, name, JS_NewString(ctx, value), JS_PROP_ENUMERABLE)

#include "constants_gen.inc"

JSValue binding_init_constants(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObjectProto(ctx, JS_NULL);
  JSValue os = JS_NewObjectProto(ctx, JS_NULL), err = JS_NewObjectProto(ctx, JS_NULL);
  JSValue sig = JS_NewObjectProto(ctx, JS_NULL), prio = JS_NewObjectProto(ctx, JS_NULL);
  JSValue fs = JS_NewObjectProto(ctx, JS_NULL), crypto = JS_NewObjectProto(ctx, JS_NULL);
  JSValue zlib = JS_NewObjectProto(ctx, JS_NULL), dl = JS_NewObjectProto(ctx, JS_NULL);
  JSValue trace = JS_NewObjectProto(ctx, JS_NULL), internal = JS_NewObjectProto(ctx, JS_NULL);
  DefineErrnoConstants(ctx, err);
  DefineSignalConstants(ctx, sig);
  DefinePriorityConstants(ctx, prio);
  DefineFsConstants(ctx, fs);
  DefineCryptoConstants(ctx, crypto);
  DefineZlibConstants(ctx, zlib);
  DefineDLOpenConstants(ctx, dl);
  DefineTraceConstants(ctx, trace);
  DefineInternalConstants(ctx, internal);
  DEF(os, UV_UDP_REUSEADDR);
  JS_SetPropertyStr(ctx, os, "dlopen", dl);
  JS_SetPropertyStr(ctx, os, "errno", err);
  JS_SetPropertyStr(ctx, os, "signals", sig);
  JS_SetPropertyStr(ctx, os, "priority", prio);
  JS_SetPropertyStr(ctx, t, "os", os);
  JS_SetPropertyStr(ctx, t, "fs", fs);
  JS_SetPropertyStr(ctx, t, "crypto", crypto);
  JS_SetPropertyStr(ctx, t, "zlib", zlib);
  JS_SetPropertyStr(ctx, t, "trace", trace);
  JS_SetPropertyStr(ctx, t, "internal", internal);
  return t;
}
