/* internalBinding('zlib') (node_zlib.cc): Zlib, BrotliEncoder/Decoder,
 * ZstdCompress/Decompress streams and crc32.  Async writes run on the
 * libuv threadpool like Node's ThreadPoolWork. */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define ZSTD_STATIC_LINKING_ONLY
#include "brotli/decode.h"
#include "brotli/encode.h"
#include "node.h"
#include "zstd.h"
#include "zstd_errors.h"

enum zmode { NONE, DEFLATE, INFLATE, GZIP, GUNZIP, DEFLATERAW, INFLATERAW, UNZIP,
             BROTLI_DECODE, BROTLI_ENCODE, ZSTD_COMPRESS, ZSTD_DECOMPRESS };

enum { K_ZLIB, K_BROTLI_ENC, K_BROTLI_DEC, K_ZSTD_C, K_ZSTD_D };

#define GZIP_HEADER_ID1 0x1f
#define GZIP_HEADER_ID2 0x8b

static JSClassID zlib_class_ids[5];

/* the write callback lives on the object under a private symbol (an
   internal field in Node), so the GC sees the reference */
static JSAtom write_cb_atom(Env *env) {
  JSValue sym = JS_GetPropertyStr(env->ctx, env->binding_data, "zlibWriteCallback");
  JSAtom a = JS_ValueToAtom(env->ctx, sym);
  JS_FreeValue(env->ctx, sym);
  return a;
}

typedef struct {
  const char *message, *code;
  int err;
  bool is_error;
} CError;

typedef struct ZStream {
  AsyncWrap aw;
  int kind;
  int mode;
  uv_work_t work;
  bool init_done, write_in_progress, pending_close, closed;
  uint32_t *write_result;
  JSValue write_result_obj; /* keeps the Uint32Array alive */
  int flush;
  /* zlib */
  z_stream strm;
  int level, window_bits, mem_level, strategy;
  bool reject_garbage_after_end, zlib_init_done;
  int err;
  int gzip_id_bytes_read;
  uint8_t *dict;
  size_t dict_len;
  /* brotli */
  BrotliEncoderState *benc;
  BrotliDecoderState *bdec;
  BrotliEncoderPreparedDictionary *bprep;
  const uint8_t *next_in;
  uint8_t *next_out;
  size_t avail_in, avail_out;
  int last_result;
  BrotliDecoderErrorCode berror;
  char berror_string[128];
  /* zstd */
  ZSTD_CCtx *cctx;
  ZSTD_DCtx *dctx;
  ZSTD_inBuffer zin;
  ZSTD_outBuffer zout;
  ZSTD_ErrorCode zerror;
  const char *zerror_string, *zerror_code;
  uint64_t pledged_src_size;
  bool has_consumed;
  uint64_t consumed_src_size;
  bool frame_complete;
} ZStream;

static const char *zlib_strerror(int err) {
  switch (err) {
#define V(c) case c: return #c;
  V(Z_OK) V(Z_STREAM_END) V(Z_NEED_DICT) V(Z_ERRNO) V(Z_STREAM_ERROR) V(Z_DATA_ERROR)
  V(Z_MEM_ERROR) V(Z_BUF_ERROR) V(Z_VERSION_ERROR)
#undef V
  default: return "Z_UNKNOWN_ERROR";
  }
}

static const char *zstd_strerror(int err) {
  switch (err) {
#define V(c) case c: return #c;
  V(ZSTD_error_no_error) V(ZSTD_error_GENERIC) V(ZSTD_error_prefix_unknown)
  V(ZSTD_error_version_unsupported) V(ZSTD_error_frameParameter_unsupported)
  V(ZSTD_error_frameParameter_windowTooLarge) V(ZSTD_error_corruption_detected)
  V(ZSTD_error_checksum_wrong) V(ZSTD_error_literals_headerWrong)
  V(ZSTD_error_dictionary_corrupted) V(ZSTD_error_dictionary_wrong)
  V(ZSTD_error_dictionaryCreation_failed) V(ZSTD_error_parameter_unsupported)
  V(ZSTD_error_parameter_combination_unsupported) V(ZSTD_error_parameter_outOfBound)
  V(ZSTD_error_tableLog_tooLarge) V(ZSTD_error_maxSymbolValue_tooLarge)
  V(ZSTD_error_maxSymbolValue_tooSmall) V(ZSTD_error_stabilityCondition_notRespected)
  V(ZSTD_error_stage_wrong) V(ZSTD_error_init_missing) V(ZSTD_error_memory_allocation)
  V(ZSTD_error_workSpace_tooSmall) V(ZSTD_error_dstSize_tooSmall) V(ZSTD_error_srcSize_wrong)
  V(ZSTD_error_dstBuffer_null) V(ZSTD_error_noForwardProgress_destFull)
  V(ZSTD_error_noForwardProgress_inputEmpty)
#undef V
  default: return "ZSTD_error_GENERIC";
  }
}

static CError no_error(void) {
  CError e = { NULL, NULL, 0, false };
  return e;
}

static CError make_error(const char *message, const char *code, int err) {
  CError e = { message, code, err, true };
  return e;
}

/* ---- zlib context ---- */

static CError zlib_error_for(ZStream *z, const char *message) {
  if (z->strm.msg)
    message = z->strm.msg;
  return make_error(message, zlib_strerror(z->err), z->err);
}

static CError zlib_set_dictionary(ZStream *z) {
  if (!z->dict_len)
    return no_error();
  z->err = Z_OK;
  switch (z->mode) {
  case DEFLATE:
  case DEFLATERAW:
    z->err = deflateSetDictionary(&z->strm, z->dict, (uInt)z->dict_len);
    break;
  case INFLATERAW:
    z->err = inflateSetDictionary(&z->strm, z->dict, (uInt)z->dict_len);
    break;
  default:
    break;
  }
  if (z->err != Z_OK)
    return zlib_error_for(z, "Failed to set dictionary");
  return no_error();
}

static void zlib_clear_dict(ZStream *z) {
  free(z->dict);
  z->dict = NULL;
  z->dict_len = 0;
}

/* true on the first call */
static bool zlib_init(ZStream *z) {
  if (z->zlib_init_done)
    return false;
  z->strm.msg = NULL;
  switch (z->mode) {
  case DEFLATE:
  case GZIP:
  case DEFLATERAW:
    z->err = deflateInit2(&z->strm, z->level, Z_DEFLATED, z->window_bits, z->mem_level,
                          z->strategy);
    break;
  case INFLATE:
  case GUNZIP:
  case INFLATERAW:
  case UNZIP:
    z->err = inflateInit2(&z->strm, z->window_bits);
    break;
  default:
    z->err = Z_STREAM_ERROR;
  }
  if (z->err != Z_OK) {
    zlib_clear_dict(z);
    z->mode = NONE;
    return true;
  }
  zlib_set_dictionary(z);
  z->zlib_init_done = true;
  return true;
}

static CError zlib_reset(ZStream *z) {
  bool first = zlib_init(z);
  if (first && z->err != Z_OK)
    return zlib_error_for(z, "Failed to init stream before reset");
  z->err = Z_OK;
  switch (z->mode) {
  case DEFLATE:
  case DEFLATERAW:
  case GZIP:
    z->err = deflateReset(&z->strm);
    break;
  case INFLATE:
  case INFLATERAW:
  case GUNZIP:
    z->err = inflateReset(&z->strm);
    break;
  default:
    break;
  }
  if (z->err != Z_OK)
    return zlib_error_for(z, "Failed to reset stream");
  return zlib_set_dictionary(z);
}

static void zlib_work(ZStream *z) {
  const Bytef *next = NULL;
  bool first = zlib_init(z);
  if (first && z->err != Z_OK)
    return;
  switch (z->mode) {
  case DEFLATE:
  case GZIP:
  case DEFLATERAW:
    z->err = deflate(&z->strm, z->flush);
    break;
  case UNZIP:
    if (z->strm.avail_in > 0)
      next = z->strm.next_in;
    switch (z->gzip_id_bytes_read) {
    case 0:
      if (!next)
        break;
      if (*next == GZIP_HEADER_ID1) {
        z->gzip_id_bytes_read = 1;
        next++;
        if (z->strm.avail_in == 1)
          break;
      } else {
        z->mode = INFLATE;
        break;
      }
      /* fallthrough */
    case 1:
      if (!next)
        break;
      if (*next == GZIP_HEADER_ID2) {
        z->gzip_id_bytes_read = 2;
        z->mode = GUNZIP;
      } else {
        z->mode = INFLATE;
      }
      break;
    default:
      break;
    }
    /* fallthrough */
  case INFLATE:
  case GUNZIP:
  case INFLATERAW:
    z->err = inflate(&z->strm, z->flush);
    if (z->mode != INFLATERAW && z->err == Z_NEED_DICT && z->dict_len) {
      z->err = inflateSetDictionary(&z->strm, z->dict, (uInt)z->dict_len);
      if (z->err == Z_OK)
        z->err = inflate(&z->strm, z->flush);
      else if (z->err == Z_DATA_ERROR)
        z->err = Z_NEED_DICT;
    }
    while (z->strm.avail_in > 0 && z->mode == GUNZIP && z->err == Z_STREAM_END &&
           !z->reject_garbage_after_end && z->strm.next_in[0] != 0x00) {
      zlib_reset(z);
      z->err = inflate(&z->strm, z->flush);
    }
    break;
  default:
    break;
  }
}

static CError zlib_error_info(ZStream *z) {
  switch (z->err) {
  case Z_OK:
  case Z_BUF_ERROR:
    if (z->strm.avail_out != 0 && z->flush == Z_FINISH)
      return zlib_error_for(z, "unexpected end of file");
    /* fallthrough */
  case Z_STREAM_END:
    break;
  case Z_NEED_DICT:
    return zlib_error_for(z, z->dict_len ? "Bad dictionary" : "Missing dictionary");
  default:
    return zlib_error_for(z, "Zlib error");
  }
  return no_error();
}

static void zlib_close(ZStream *z) {
  if (!z->zlib_init_done) {
    zlib_clear_dict(z);
    z->mode = NONE;
    return;
  }
  if (z->mode == DEFLATE || z->mode == GZIP || z->mode == DEFLATERAW)
    deflateEnd(&z->strm);
  else if (z->mode >= INFLATE && z->mode <= UNZIP)
    inflateEnd(&z->strm);
  z->mode = NONE;
  z->zlib_init_done = false;
  zlib_clear_dict(z);
}

/* ---- brotli ---- */

static CError brotli_init(ZStream *z) {
  if (z->kind == K_BROTLI_ENC) {
    if (z->benc)
      BrotliEncoderDestroyInstance(z->benc);
    z->benc = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (!z->benc)
      return make_error("Could not initialize Brotli instance",
                        "ERR_ZLIB_INITIALIZATION_FAILED", -1);
    if (z->dict_len) {
      if (z->bprep)
        BrotliEncoderDestroyPreparedDictionary(z->bprep);
      z->bprep = BrotliEncoderPrepareDictionary(BROTLI_SHARED_DICTIONARY_RAW, z->dict_len,
                                                z->dict, BROTLI_MAX_QUALITY, NULL, NULL, NULL);
      if (!z->bprep)
        return make_error("Failed to prepare brotli dictionary",
                          "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
      if (!BrotliEncoderAttachPreparedDictionary(z->benc, z->bprep))
        return make_error("Failed to attach brotli dictionary",
                          "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
    }
  } else {
    if (z->bdec)
      BrotliDecoderDestroyInstance(z->bdec);
    z->bdec = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!z->bdec)
      return make_error("Could not initialize Brotli instance",
                        "ERR_ZLIB_INITIALIZATION_FAILED", -1);
    if (z->dict_len &&
        !BrotliDecoderAttachDictionary(z->bdec, BROTLI_SHARED_DICTIONARY_RAW, z->dict_len,
                                       z->dict))
      return make_error("Failed to attach brotli dictionary",
                        "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
    z->berror = BROTLI_DECODER_NO_ERROR;
    z->last_result = BROTLI_DECODER_RESULT_SUCCESS;
  }
  return no_error();
}

static CError brotli_set_param(ZStream *z, int key, uint32_t value) {
  bool ok = z->kind == K_BROTLI_ENC
                ? BrotliEncoderSetParameter(z->benc, (BrotliEncoderParameter)key, value)
                : BrotliDecoderSetParameter(z->bdec, (BrotliDecoderParameter)key, value);
  if (!ok)
    return make_error("Setting parameter failed", "ERR_BROTLI_PARAM_SET_FAILED", -1);
  return no_error();
}

static void brotli_work(ZStream *z) {
  const uint8_t *next_in = z->next_in;
  if (z->kind == K_BROTLI_ENC) {
    z->last_result = BrotliEncoderCompressStream(z->benc, (BrotliEncoderOperation)z->flush,
                                                 &z->avail_in, &next_in, &z->avail_out,
                                                 &z->next_out, NULL);
  } else {
    z->last_result = BrotliDecoderDecompressStream(z->bdec, &z->avail_in, &next_in,
                                                   &z->avail_out, &z->next_out, NULL);
    if (z->last_result == BROTLI_DECODER_RESULT_ERROR) {
      z->berror = BrotliDecoderGetErrorCode(z->bdec);
      snprintf(z->berror_string, sizeof(z->berror_string), "ERR_%s",
               BrotliDecoderErrorString(z->berror));
    }
  }
  z->next_in = next_in;
}

static CError brotli_error_info(ZStream *z) {
  if (z->kind == K_BROTLI_ENC) {
    if (!z->last_result)
      return make_error("Compression failed", "ERR_BROTLI_COMPRESSION_FAILED", -1);
    return no_error();
  }
  if (z->berror != BROTLI_DECODER_NO_ERROR)
    return make_error("Decompression failed", z->berror_string, (int)z->berror);
  if (z->flush == BROTLI_OPERATION_FINISH &&
      z->last_result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT)
    return make_error("unexpected end of file", "Z_BUF_ERROR", Z_BUF_ERROR);
  return no_error();
}

/* ---- zstd ---- */

static CError zstd_init(ZStream *z, uint64_t pledged) {
  if (z->kind == K_ZSTD_C) {
    z->pledged_src_size = pledged;
    z->has_consumed = pledged != ZSTD_CONTENTSIZE_UNKNOWN;
    z->consumed_src_size = 0;
    if (z->cctx)
      ZSTD_freeCCtx(z->cctx);
    z->cctx = ZSTD_createCCtx();
    if (!z->cctx)
      return make_error("Could not initialize zstd instance", "ERR_ZLIB_INITIALIZATION_FAILED",
                        -1);
    if (z->dict_len && ZSTD_isError(ZSTD_CCtx_loadDictionary(z->cctx, z->dict, z->dict_len)))
      return make_error("Failed to load zstd dictionary", "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
    if (ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(z->cctx, pledged)))
      return make_error("Could not set pledged src size", "ERR_ZLIB_INITIALIZATION_FAILED", -1);
  } else {
    z->frame_complete = false;
    if (z->dctx)
      ZSTD_freeDCtx(z->dctx);
    z->dctx = ZSTD_createDCtx();
    if (!z->dctx)
      return make_error("Could not initialize zstd instance", "ERR_ZLIB_INITIALIZATION_FAILED",
                        -1);
    if (z->dict_len && ZSTD_isError(ZSTD_DCtx_loadDictionary(z->dctx, z->dict, z->dict_len)))
      return make_error("Failed to load zstd dictionary", "ERR_ZLIB_DICTIONARY_LOAD_FAILED", -1);
  }
  z->zerror = ZSTD_error_no_error;
  return no_error();
}

static CError zstd_set_param(ZStream *z, int key, int value) {
  size_t r = z->kind == K_ZSTD_C ? ZSTD_CCtx_setParameter(z->cctx, (ZSTD_cParameter)key, value)
                                 : ZSTD_DCtx_setParameter(z->dctx, (ZSTD_dParameter)key, value);
  if (ZSTD_isError(r))
    return make_error("Setting parameter failed", "ERR_ZSTD_PARAM_SET_FAILED", -1);
  return no_error();
}

static void zstd_set_error(ZStream *z, ZSTD_ErrorCode e) {
  z->zerror = e;
  z->zerror_code = zstd_strerror(e);
  z->zerror_string = ZSTD_getErrorString(e);
}

static void zstd_work(ZStream *z) {
  if (z->kind == K_ZSTD_C) {
    size_t pos = z->zin.pos;
    size_t rem = ZSTD_compressStream2(z->cctx, &z->zout, &z->zin, (ZSTD_EndDirective)z->flush);
    if (z->has_consumed)
      z->consumed_src_size += z->zin.pos - pos;
    if (ZSTD_isError(rem)) {
      zstd_set_error(z, ZSTD_getErrorCode(rem));
    } else if (rem == 0 && z->flush == ZSTD_e_end && z->has_consumed) {
      z->has_consumed = false;
      if (z->consumed_src_size != z->pledged_src_size)
        zstd_set_error(z, ZSTD_error_srcSize_wrong);
    }
  } else {
    size_t r;
    if (z->frame_complete && z->zin.size == 0)
      return;
    r = ZSTD_decompressStream(z->dctx, &z->zout, &z->zin);
    if (ZSTD_isError(r)) {
      z->frame_complete = false;
      zstd_set_error(z, ZSTD_getErrorCode(r));
    } else {
      z->frame_complete = r == 0;
    }
  }
}

static CError zstd_error_info(ZStream *z) {
  if (z->zerror != ZSTD_error_no_error)
    return make_error(z->zerror_string, z->zerror_code, (int)z->zerror);
  if (z->kind == K_ZSTD_D && z->flush == ZSTD_e_end && !z->frame_complete &&
      z->zin.pos == z->zin.size && z->zout.pos < z->zout.size)
    return make_error("unexpected end of file", "Z_BUF_ERROR", Z_BUF_ERROR);
  return no_error();
}

/* ---- dispatch ---- */

static void ctx_set_buffers(ZStream *z, const uint8_t *in, uint32_t in_len, uint8_t *out,
                            uint32_t out_len) {
  switch (z->kind) {
  case K_ZLIB:
    z->strm.avail_in = in_len;
    z->strm.next_in = (Bytef *)in;
    z->strm.avail_out = out_len;
    z->strm.next_out = out;
    break;
  case K_BROTLI_ENC:
  case K_BROTLI_DEC:
    z->next_in = in;
    z->next_out = out;
    z->avail_in = in_len;
    z->avail_out = out_len;
    break;
  default:
    z->zin.src = in;
    z->zin.size = in_len;
    z->zin.pos = 0;
    z->zout.dst = out;
    z->zout.size = out_len;
    z->zout.pos = 0;
  }
}

static void ctx_after_offsets(ZStream *z, uint32_t *avail_in, uint32_t *avail_out) {
  switch (z->kind) {
  case K_ZLIB:
    *avail_in = z->strm.avail_in;
    *avail_out = z->strm.avail_out;
    break;
  case K_BROTLI_ENC:
  case K_BROTLI_DEC:
    *avail_in = (uint32_t)z->avail_in;
    *avail_out = (uint32_t)z->avail_out;
    break;
  default:
    *avail_in = (uint32_t)(z->zin.size - z->zin.pos);
    *avail_out = (uint32_t)(z->zout.size - z->zout.pos);
  }
}

static void ctx_work(ZStream *z) {
  switch (z->kind) {
  case K_ZLIB: zlib_work(z); break;
  case K_BROTLI_ENC:
  case K_BROTLI_DEC: brotli_work(z); break;
  default: zstd_work(z);
  }
}

static CError ctx_error_info(ZStream *z) {
  switch (z->kind) {
  case K_ZLIB: return zlib_error_info(z);
  case K_BROTLI_ENC:
  case K_BROTLI_DEC: return brotli_error_info(z);
  default: return zstd_error_info(z);
  }
}

static CError ctx_reset(ZStream *z) {
  switch (z->kind) {
  case K_ZLIB: return zlib_reset(z);
  case K_BROTLI_ENC:
  case K_BROTLI_DEC: return brotli_init(z);
  case K_ZSTD_C: return zstd_init(z, z->pledged_src_size);
  default: return zstd_init(z, ZSTD_CONTENTSIZE_UNKNOWN);
  }
}

static void ctx_close(ZStream *z) {
  switch (z->kind) {
  case K_ZLIB:
    zlib_close(z);
    break;
  case K_BROTLI_ENC:
    if (z->benc)
      BrotliEncoderDestroyInstance(z->benc);
    if (z->bprep)
      BrotliEncoderDestroyPreparedDictionary(z->bprep);
    z->benc = NULL;
    z->bprep = NULL;
    zlib_clear_dict(z);
    z->mode = NONE;
    break;
  case K_BROTLI_DEC:
    if (z->bdec)
      BrotliDecoderDestroyInstance(z->bdec);
    z->bdec = NULL;
    zlib_clear_dict(z);
    z->mode = NONE;
    break;
  case K_ZSTD_C:
    if (z->cctx)
      ZSTD_freeCCtx(z->cctx);
    z->cctx = NULL;
    zlib_clear_dict(z);
    break;
  default:
    if (z->dctx)
      ZSTD_freeDCtx(z->dctx);
    z->dctx = NULL;
    zlib_clear_dict(z);
  }
}

/* ---- CompressionStream ---- */

static ZStream *zs_of(JSContext *ctx, JSValueConst obj) {
  JSClassID id;
  ZStream *z = JS_GetAnyOpaque(obj, &id);
  int i;
  for (i = 0; i < 5; i++)
    if (id == zlib_class_ids[i] && z)
      return z;
  JS_ThrowTypeError(ctx, "Illegal invocation");
  return NULL;
}

static void zs_close(ZStream *z) {
  if (z->write_in_progress) {
    z->pending_close = true;
    return;
  }
  z->pending_close = false;
  z->closed = true;
  ctx_close(z);
}

static void zs_emit_error(ZStream *z, CError e) {
  JSContext *ctx = z->aw.env->ctx;
  JSValue args[3], r;
  args[0] = JS_NewString(ctx, e.message ? e.message : "");
  args[1] = JS_NewInt32(ctx, e.err);
  args[2] = JS_NewString(ctx, e.code ? e.code : "");
  r = async_wrap_make_callback_name(&z->aw, "onerror", 3, (JSValueConst *)args);
  if (!JS_IsException(r))
    JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[2]);
  z->write_in_progress = false;
  if (z->pending_close)
    zs_close(z);
}

static bool zs_check_error(ZStream *z) {
  CError e = ctx_error_info(z);
  if (!e.is_error)
    return true;
  zs_emit_error(z, e);
  return false;
}

static void zs_update_write_result(ZStream *z) {
  if (z->write_result)
    ctx_after_offsets(z, &z->write_result[1], &z->write_result[0]);
}

static void zs_work_cb(uv_work_t *w) {
  ctx_work(w->data);
}

static void zs_after_work_cb(uv_work_t *w, int status) {
  ZStream *z = w->data;
  Env *env = z->aw.env;
  JSContext *ctx = env->ctx;
  JSValue obj = JS_DupValue(ctx, z->aw.object);
  async_wrap_unref(&z->aw);
  z->write_in_progress = false;
  if (status == UV_ECANCELED) {
    zs_close(z);
    goto done;
  }
  if (!zs_check_error(z))
    goto done;
  zs_update_write_result(z);
  {
    JSAtom a = write_cb_atom(env);
    JSValue cb = JS_GetProperty(ctx, obj, a);
    JS_FreeAtom(ctx, a);
    if (JS_IsFunction(ctx, cb)) {
      JSValue r = async_wrap_make_callback(&z->aw, cb, 0, NULL);
      if (!JS_IsException(r))
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, cb);
  }
  if (z->pending_close)
    zs_close(z);
done:
  JS_FreeValue(ctx, obj);
}

/* write(flush, in, in_off, in_len, out, out_off, out_len) */
static JSValue zs_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                        int async) {
  ZStream *z = zs_of(ctx, this_val);
  uint32_t flush, in_off = 0, in_len = 0, out_off, out_len;
  size_t blen;
  uint8_t *in = NULL, *out;
  if (!z)
    return JS_EXCEPTION;
  flush = nb_uint32(ctx, ARG(0), 0);
  if (!JS_IsNull(ARG(1))) {
    uint8_t *d = nb_buffer_data(ctx, ARG(1), &blen);
    in_off = nb_uint32(ctx, ARG(2), 0);
    in_len = nb_uint32(ctx, ARG(3), 0);
    if (!d || (uint64_t)in_off + in_len > blen)
      return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "input buffer is out of bounds");
    in = d + in_off;
  }
  out = nb_buffer_data(ctx, ARG(4), &blen);
  out_off = nb_uint32(ctx, ARG(5), 0);
  out_len = nb_uint32(ctx, ARG(6), 0);
  if (!out || (uint64_t)out_off + out_len > blen)
    return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "output buffer is out of bounds");
  out += out_off;

  z->write_in_progress = true;
  async_wrap_ref(&z->aw);
  ctx_set_buffers(z, in, in_len, out, out_len);
  z->flush = (int)flush;
  if (!async) {
    ctx_work(z);
    if (zs_check_error(z)) {
      zs_update_write_result(z);
      z->write_in_progress = false;
    }
    async_wrap_unref(&z->aw);
    return JS_UNDEFINED;
  }
  z->work.data = z;
  if (uv_queue_work(z->aw.env->loop, &z->work, zs_work_cb, zs_after_work_cb) != 0) {
    /* no threadpool: run it right here and complete asynchronously */
    ctx_work(z);
    zs_after_work_cb(&z->work, 0);
  }
  return JS_UNDEFINED;
}

static JSValue zs_close_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  ZStream *z = zs_of(ctx, this_val);
  if (!z)
    return JS_EXCEPTION;
  zs_close(z);
  return JS_UNDEFINED;
}

static JSValue zs_reset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  ZStream *z = zs_of(ctx, this_val);
  CError e;
  if (!z)
    return JS_EXCEPTION;
  if (z->write_in_progress)
    return JS_ThrowPlainError(ctx, "Cannot reset zlib stream while a write is in progress");
  e = ctx_reset(z);
  if (e.is_error)
    zs_emit_error(z, e);
  return JS_UNDEFINED;
}

static int zs_init_stream(JSContext *ctx, ZStream *z, JSValueConst write_result,
                          JSValueConst cb) {
  size_t len;
  uint8_t *d = nb_buffer_data(ctx, write_result, &len);
  if (!d || len < 8) {
    JS_ThrowTypeError(ctx, "writeResult must be a Uint32Array");
    return -1;
  }
  z->write_result = (uint32_t *)d;
  JS_FreeValue(ctx, z->write_result_obj);
  z->write_result_obj = JS_DupValue(ctx, write_result);
  {
    JSAtom a = write_cb_atom(z->aw.env);
    JS_DefinePropertyValue(ctx, z->aw.object, a, JS_DupValue(ctx, cb), JS_PROP_WRITABLE);
    JS_FreeAtom(ctx, a);
  }
  z->init_done = true;
  return 0;
}

static void copy_dict(JSContext *ctx, ZStream *z, JSValueConst v) {
  size_t len;
  uint8_t *d;
  zlib_clear_dict(z);
  if (JS_IsUndefined(v) || JS_IsNull(v))
    return;
  d = nb_buffer_data(ctx, v, &len);
  if (d && len) {
    z->dict = malloc(len);
    memcpy(z->dict, d, len);
    z->dict_len = len;
  }
}

/* Zlib.init(windowBits, level, memLevel, strategy, writeResult, writeCallback,
             dictionary[, rejectGarbageAfterEnd]) */
static JSValue zlib_init_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  ZStream *z = zs_of(ctx, this_val);
  int window_bits;
  if (!z)
    return JS_EXCEPTION;
  if (argc == 5)
    fprintf(stderr, "WARNING: You are likely using a version of node-tar or npm that "
                    "is incompatible with this version of Node.js.\nPlease use "
                    "either the version of npm that is bundled with Node.js, or "
                    "a version of npm (> 5.5.1 or < 5.4.0) or node-tar (> 4.0.1) "
                    "that is compatible with Node.js 9 and above.\n");
  window_bits = (int)nb_uint32(ctx, ARG(0), 15);
  if (zs_init_stream(ctx, z, ARG(4), ARG(5)) < 0)
    return JS_EXCEPTION;
  copy_dict(ctx, z, ARG(6));
  z->level = nb_int32(ctx, ARG(1), Z_DEFAULT_COMPRESSION);
  z->mem_level = (int)nb_uint32(ctx, ARG(2), 8);
  z->strategy = (int)nb_uint32(ctx, ARG(3), Z_DEFAULT_STRATEGY);
  z->reject_garbage_after_end = argc > 7 && JS_ToBool(ctx, ARG(7));
  z->flush = Z_NO_FLUSH;
  z->err = Z_OK;
  z->strm.zalloc = Z_NULL;
  z->strm.zfree = Z_NULL;
  z->strm.opaque = Z_NULL;
  if (z->mode == GZIP || z->mode == GUNZIP)
    window_bits += 16;
  if (z->mode == UNZIP)
    window_bits += 32;
  if (z->mode == DEFLATERAW || z->mode == INFLATERAW)
    window_bits *= -1;
  z->window_bits = window_bits;
  return JS_UNDEFINED;
}

/* Zlib.params(level, strategy) */
static JSValue zlib_params_fn(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  ZStream *z = zs_of(ctx, this_val);
  bool first;
  if (!z)
    return JS_EXCEPTION;
  first = zlib_init(z);
  if (first && z->err != Z_OK) {
    zs_emit_error(z, zlib_error_for(z, "Failed to init stream before set parameters"));
    return JS_UNDEFINED;
  }
  z->err = Z_OK;
  if (z->mode == DEFLATE || z->mode == DEFLATERAW)
    z->err = deflateParams(&z->strm, nb_int32(ctx, ARG(0), 0), nb_int32(ctx, ARG(1), 0));
  if (z->err != Z_OK && z->err != Z_BUF_ERROR)
    zs_emit_error(z, zlib_error_for(z, "Failed to set parameters"));
  return JS_UNDEFINED;
}

static JSValue throw_init_failed(JSContext *ctx, const char *msg) {
  return node_throw_error(ctx, "ERR_ZLIB_INITIALIZATION_FAILED", msg);
}

/* Brotli*.init(params, writeResult, writeCallback[, dictionary]) */
static JSValue brotli_init_fn(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  ZStream *z = zs_of(ctx, this_val);
  CError e;
  size_t len, i;
  uint32_t *params;
  if (!z)
    return JS_EXCEPTION;
  if (zs_init_stream(ctx, z, ARG(1), ARG(2)) < 0)
    return JS_EXCEPTION;
  if (argc == 4 && !JS_IsUndefined(ARG(3))) {
    if (!nb_is_array_buffer_view(ctx, ARG(3)))
      return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                                   "dictionary must be an ArrayBufferView if provided");
    copy_dict(ctx, z, ARG(3));
  }
  e = brotli_init(z);
  if (e.is_error) {
    zs_emit_error(z, e);
    return throw_init_failed(ctx, "Initialization failed");
  }
  params = (uint32_t *)nb_buffer_data(ctx, ARG(0), &len);
  for (i = 0; params && i < len / 4; i++) {
    if (params[i] == (uint32_t)-1)
      continue;
    e = brotli_set_param(z, (int)i, params[i]);
    if (e.is_error) {
      zs_emit_error(z, e);
      return throw_init_failed(ctx, "Initialization failed");
    }
  }
  return JS_UNDEFINED;
}

/* Zstd*.init(params, pledgedSrcSize, writeResult, writeCallback[, dictionary]) */
static JSValue zstd_init_fn(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  ZStream *z = zs_of(ctx, this_val);
  uint64_t pledged = ZSTD_CONTENTSIZE_UNKNOWN;
  CError e;
  size_t len, i;
  uint32_t *params;
  if (!z)
    return JS_EXCEPTION;
  if (zs_init_stream(ctx, z, ARG(2), ARG(3)) < 0)
    return JS_EXCEPTION;
  if (!JS_IsUndefined(ARG(1))) {
    double d;
    if (!JS_IsNumber(ARG(1)))
      return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "pledgedSrcSize must be a number");
    JS_ToFloat64(ctx, &d, ARG(1));
    if (d != (double)(int64_t)d || d > 9007199254740991.0 || d < -9007199254740991.0)
      return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE",
                                    "pledgedSrcSize must be a safe integer");
    if (d < 0)
      return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE",
                                    "pledgedSrcSize must be non-negative");
    pledged = (uint64_t)d;
  }
  if (argc == 5 && !JS_IsUndefined(ARG(4))) {
    if (!nb_is_array_buffer_view(ctx, ARG(4)))
      return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                                   "dictionary must be an ArrayBufferView if provided");
    copy_dict(ctx, z, ARG(4));
  }
  e = zstd_init(z, pledged);
  if (e.is_error) {
    zs_emit_error(z, e);
    return throw_init_failed(ctx, e.message);
  }
  params = (uint32_t *)nb_buffer_data(ctx, ARG(0), &len);
  for (i = 0; params && i < len / 4; i++) {
    if (params[i] == (uint32_t)-1)
      continue;
    e = zstd_set_param(z, (int)i, (int)params[i]);
    if (e.is_error) {
      zs_emit_error(z, e);
      return throw_init_failed(ctx, e.message);
    }
  }
  return JS_UNDEFINED;
}

static JSValue noop_params(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue zs_get_async_id(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  ZStream *z = zs_of(ctx, this_val);
  return z ? JS_NewFloat64(ctx, z->aw.async_id) : JS_EXCEPTION;
}

static JSValue zs_get_provider_type(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
  ZStream *z = zs_of(ctx, this_val);
  return z ? JS_NewInt32(ctx, z->aw.provider) : JS_EXCEPTION;
}

static void zs_finalizer(JSRuntime *rt, JSValueConst val) {
  JSClassID id;
  ZStream *z = JS_GetAnyOpaque(val, &id);
  if (!z)
    return;
  z->write_in_progress = false;
  ctx_close(z);
  JS_FreeValueRT(rt, z->write_result_obj);
  async_wrap_destroy(&z->aw);
  free(z);
}

static JSValue zs_new(JSContext *ctx, JSValueConst nt, int kind, int mode) {
  JSValue obj = nb_new_instance(ctx, nt, zlib_class_ids[kind]);
  ZStream *z;
  if (JS_IsException(obj))
    return obj;
  z = calloc(1, sizeof(*z));
  z->kind = kind;
  z->mode = mode;
  z->write_result_obj = JS_UNDEFINED;
  z->pledged_src_size = ZSTD_CONTENTSIZE_UNKNOWN;
  async_wrap_init(&z->aw, env_get(ctx), obj, PROVIDER_ZLIB, -1);
  JS_SetOpaque(obj, z);
  return obj;
}

static JSValue zlib_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return zs_new(ctx, nt, K_ZLIB, nb_int32(ctx, ARG(0), NONE));
}
static JSValue brotli_enc_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return zs_new(ctx, nt, K_BROTLI_ENC, nb_int32(ctx, ARG(0), BROTLI_ENCODE));
}
static JSValue brotli_dec_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return zs_new(ctx, nt, K_BROTLI_DEC, nb_int32(ctx, ARG(0), BROTLI_DECODE));
}
static JSValue zstd_c_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return zs_new(ctx, nt, K_ZSTD_C, ZSTD_COMPRESS);
}
static JSValue zstd_d_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
  return zs_new(ctx, nt, K_ZSTD_D, ZSTD_DECOMPRESS);
}

#define STREAM_FUNCS(init, params)                                  \
  JS_CFUNC_MAGIC_DEF("write", 7, zs_write, 1),                      \
  JS_CFUNC_MAGIC_DEF("writeSync", 7, zs_write, 0),                  \
  JS_CFUNC_DEF("close", 0, zs_close_fn),                            \
  JS_CFUNC_DEF("init", 8, init),                                    \
  JS_CFUNC_DEF("params", 2, params),                                \
  JS_CFUNC_DEF("reset", 0, zs_reset),                               \
  JS_CFUNC_DEF("getAsyncId", 0, zs_get_async_id),                   \
  JS_CFUNC_DEF("getProviderType", 0, zs_get_provider_type)

static const JSCFunctionListEntry zlib_proto[] = { STREAM_FUNCS(zlib_init_fn, zlib_params_fn) };
static const JSCFunctionListEntry brotli_proto[] = { STREAM_FUNCS(brotli_init_fn, noop_params) };
static const JSCFunctionListEntry zstd_proto[] = { STREAM_FUNCS(zstd_init_fn, noop_params) };

static JSValue z_crc32(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  uint32_t value = nb_uint32(ctx, ARG(1), 0);
  if (JS_IsString(ARG(0))) {
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, ARG(0));
    if (!s)
      return JS_EXCEPTION;
    value = (uint32_t)crc32(value, (const Bytef *)s, (uInt)len);
    JS_FreeCString(ctx, s);
  } else {
    size_t len;
    uint8_t *d = nb_buffer_data(ctx, ARG(0), &len);
    if (d)
      value = (uint32_t)crc32(value, d, (uInt)len);
  }
  return JS_NewUint32(ctx, value);
}

JSValue binding_init_zlib(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  struct {
    const char *name;
    int kind;
    JSCFunction *ctor;
    const JSCFunctionListEntry *proto;
    int n;
  } classes[] = {
    { "Zlib", K_ZLIB, zlib_ctor, zlib_proto, countof(zlib_proto) },
    { "BrotliEncoder", K_BROTLI_ENC, brotli_enc_ctor, brotli_proto, countof(brotli_proto) },
    { "BrotliDecoder", K_BROTLI_DEC, brotli_dec_ctor, brotli_proto, countof(brotli_proto) },
    { "ZstdCompress", K_ZSTD_C, zstd_c_ctor, zstd_proto, countof(zstd_proto) },
    { "ZstdDecompress", K_ZSTD_D, zstd_d_ctor, zstd_proto, countof(zstd_proto) },
  };
  size_t i;
  JS_SetPropertyStr(ctx, env->binding_data, "zlibWriteCallback",
                    JS_NewPrivateSymbol(ctx, "zlib.writeCallback"));
  for (i = 0; i < countof(classes); i++) {
    NodeClassDef def = { .name = classes[i].name, .class_id = &zlib_class_ids[classes[i].kind],
                         .ctor = classes[i].ctor, .finalizer = zs_finalizer,
                         .proto_funcs = classes[i].proto, .proto_funcs_count = classes[i].n,
                         .parent_ctor = JS_UNDEFINED };
    JS_FreeValue(ctx, nb_define_class(ctx, t, &def));
  }
  nb_set_method(ctx, t, "crc32", z_crc32, 2);
  nb_set_str(ctx, t, "ZLIB_VERSION", ZLIB_VERSION);
  return t;
}
