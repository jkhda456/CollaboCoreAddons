/* internalBinding('buffer') (node_buffer.cc), ('string_decoder')
 * (string_decoder.cc) and ('encoding_binding') (encoding_binding.cc). */
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

size_t node_base64_encode(char *dst, const uint8_t *src, size_t n, bool url);
size_t node_base64_encoded_size(size_t n, bool url);

#define K_MAX_LENGTH 0x7fffffffu      /* ArrayBuffers of QuickJS: int byte_length */
#define K_STRING_MAX_LENGTH ((1 << 29) - 24)

static uint8_t *view_data(JSContext *ctx, JSValueConst v, size_t *len) {
  return JS_NodeGetBufferBytes(ctx, v, len);
}

static JSValue throw_not_buffer(JSContext *ctx) {
  return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE", "argument must be a buffer");
}

/* THROW_AND_RETURN_IF_OOB */
static JSValue throw_oob(JSContext *ctx) {
  return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE", "Index out of range");
}

/* ParseArrayIndex: undefined -> def; negative -> out of range */
static int parse_index(JSContext *ctx, JSValueConst v, size_t def, size_t *out) {
  double d;
  if (JS_IsUndefined(v)) {
    *out = def;
    return 0;
  }
  if (JS_ToFloat64(ctx, &d, v) < 0)
    return -1;
  if (d != d)
    d = 0;
  if (d < 0)
    return 1;
  *out = d > 9007199254740991.0 ? (size_t)-1 : (size_t)d;
  return 0;
}

static JSValue buffer_slice(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int enc) {
  size_t len, start = 0, end = 0;
  uint8_t *data = view_data(ctx, ARG(0), &len);
  int r;
  if (!data)
    return throw_not_buffer(ctx);
  if (len == 0)
    return JS_NewString(ctx, "");
  if ((r = parse_index(ctx, ARG(1), 0, &start)) != 0)
    return r < 0 ? JS_EXCEPTION : throw_oob(ctx);
  if ((r = parse_index(ctx, ARG(2), len, &end)) != 0)
    return r < 0 ? JS_EXCEPTION : throw_oob(ctx);
  if (end <= start)
    return JS_NewString(ctx, "");
  if (end > len)
    return throw_oob(ctx);
  return node_string_encode(ctx, data + start, end - start, enc);
}

/* StringWrite: (buf, string, offset, maxLength) */
static JSValue buffer_write(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int enc) {
  size_t len, offset = 0, max_length = 0;
  uint8_t *data = view_data(ctx, ARG(0), &len);
  int r;
  if (!data)
    return throw_not_buffer(ctx);
  if (!JS_IsString(ARG(1)))
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
                                 "The \"argument\" argument must be of type string");
  if ((r = parse_index(ctx, ARG(2), 0, &offset)) != 0)
    return r < 0 ? JS_EXCEPTION : throw_oob(ctx);
  if (offset > len)
    return node_throw_range_error(ctx, "ERR_BUFFER_OUT_OF_BOUNDS",
                                  "\"offset\" is outside of buffer bounds");
  if ((r = parse_index(ctx, ARG(3), len - offset, &max_length)) != 0)
    return r < 0 ? JS_EXCEPTION : throw_oob(ctx);
  if (max_length > len - offset)
    max_length = len - offset;
  if (max_length == 0)
    return JS_NewInt32(ctx, 0);
  return JS_NewUint32(ctx, (uint32_t)node_string_write(ctx, data + offset, max_length,
                                                        ARG(1), enc, NULL));
}

static JSValue buffer_byte_length_utf8(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv) {
  return JS_NewFloat64(ctx, (double)node_utf8_length(ctx, ARG(0)));
}

static JSValue buffer_copy(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  size_t slen, tlen;
  uint8_t *src = view_data(ctx, ARG(0), &slen), *dst = view_data(ctx, ARG(1), &tlen);
  uint32_t target_start = nb_uint32(ctx, ARG(2), 0), source_start = nb_uint32(ctx, ARG(3), 0);
  uint32_t to_copy = nb_uint32(ctx, ARG(4), 0);
  if (!src || !dst)
    return throw_not_buffer(ctx);
  if (target_start > tlen || source_start > slen)
    return JS_NewInt32(ctx, 0);
  if (to_copy > tlen - target_start) to_copy = tlen - target_start;
  if (to_copy > slen - source_start) to_copy = slen - source_start;
  memmove(dst + target_start, src + source_start, to_copy);
  return JS_NewUint32(ctx, to_copy);
}

static int normalize_cmp(int val, size_t a_len, size_t b_len) {
  if (val == 0) {
    if (a_len > b_len) return 1;
    if (a_len < b_len) return -1;
    return 0;
  }
  return val > 0 ? 1 : -1;
}

static JSValue buffer_compare(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  size_t alen, blen, n;
  uint8_t *a = view_data(ctx, ARG(0), &alen), *b = view_data(ctx, ARG(1), &blen);
  if (!a || !b)
    return throw_not_buffer(ctx);
  n = alen < blen ? alen : blen;
  return JS_NewInt32(ctx, normalize_cmp(n ? memcmp(a, b, n) : 0, alen, blen));
}

static JSValue buffer_compare_offset(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  size_t slen, tlen, ts = 0, ss = 0, te = 0, se = 0, n;
  uint8_t *s = view_data(ctx, ARG(0), &slen), *t = view_data(ctx, ARG(1), &tlen);
  if (!s || !t)
    return throw_not_buffer(ctx);
  if (parse_index(ctx, ARG(2), 0, &ts) || parse_index(ctx, ARG(3), 0, &ss) ||
      parse_index(ctx, ARG(4), tlen, &te) || parse_index(ctx, ARG(5), slen, &se))
    return JS_HasException(ctx) ? JS_EXCEPTION : throw_oob(ctx);
  if (ss > slen)
    return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE",
                                  "The value of \"sourceStart\" is out of range.");
  if (ts > tlen)
    return node_throw_range_error(ctx, "ERR_OUT_OF_RANGE",
                                  "The value of \"targetStart\" is out of range.");
  if (se < ss) se = ss;
  if (te < ts) te = ts;
  n = se - ss < te - ts ? se - ss : te - ts;
  if (n > slen - ss) n = slen - ss;
  if (n > tlen - ts) n = tlen - ts;
  return JS_NewInt32(ctx, normalize_cmp(n ? memcmp(s + ss, t + ts, n) : 0, se - ss, te - ts));
}

static JSValue buffer_fill(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  size_t len, start = 0, end = 0, fill_length, str_length;
  uint8_t *data = view_data(ctx, ARG(0), &len), *fd;
  size_t flen;
  if (!data)
    return throw_not_buffer(ctx);
  if (parse_index(ctx, ARG(2), 0, &start) || parse_index(ctx, ARG(3), 0, &end))
    return JS_HasException(ctx) ? JS_EXCEPTION : throw_oob(ctx);
  fill_length = end - start;
  if (start > end || fill_length + start > len)
    return JS_NewInt32(ctx, -2);
  if ((fd = view_data(ctx, ARG(1), &flen)) != NULL && !JS_IsString(ARG(1))) {
    str_length = flen;
    memmove(data + start, fd, str_length < fill_length ? str_length : fill_length);
  } else if (!JS_IsString(ARG(1))) {
    uint32_t val;
    if (JS_ToUint32(ctx, &val, ARG(1)) < 0)
      return JS_EXCEPTION;
    memset(data + start, val & 255, fill_length);
    return JS_UNDEFINED;
  } else {
    int enc = node_parse_encoding(ctx, ARG(4), ENC_UTF8);
    str_length = node_string_write(ctx, data + start, fill_length, ARG(1), enc, NULL);
    if (enc == ENC_UTF8 || enc == ENC_UCS2) {
      size_t full = node_string_bytes_size(ctx, ARG(1), enc);
      if (full > str_length && str_length < fill_length) {
        /* a character that did not fit: Node copies its bytes partially */
        uint8_t *tmp = malloc(full);
        node_string_write(ctx, tmp, full, ARG(1), enc, NULL);
        memcpy(data + start + str_length, tmp + str_length,
               (full < fill_length ? full : fill_length) - str_length);
        free(tmp);
        str_length = full;
      }
    }
  }
  if (str_length >= fill_length)
    return JS_UNDEFINED;
  if (str_length == 0)
    return JS_NewInt32(ctx, -1);
  {
    size_t in_there = str_length;
    uint8_t *ptr = data + start + str_length;
    while (in_there < fill_length - in_there) {
      memcpy(ptr, data + start, in_there);
      ptr += in_there;
      in_there *= 2;
    }
    if (in_there < fill_length)
      memcpy(ptr, data + start, fill_length - in_there);
  }
  return JS_UNDEFINED;
}

static int64_t index_of_offset(size_t length, int64_t offset, int64_t needle_length,
                               bool is_forward) {
  int64_t len = (int64_t)length;
  if (offset < 0) {
    if (offset + len >= 0) return len + offset;
    if (is_forward || needle_length == 0) return 0;
    return -1;
  }
  if (offset + needle_length <= len) return offset;
  if (needle_length == 0) return len;
  if (is_forward) return -1;
  return len - 1;
}

/* index of needle in hay[0..end) starting at offset; end if none (nbytes::SearchString) */
static size_t search_bytes(const uint8_t *hay, size_t end, const uint8_t *needle,
                           size_t nlen, size_t offset, bool forward) {
  size_t i;
  if (nlen == 0 || nlen > end)
    return end;
  if (forward) {
    for (i = offset; i + nlen <= end; i++) {
      const uint8_t *p = memchr(hay + i, needle[0], end - nlen + 1 - i);
      if (!p) return end;
      i = p - hay;
      if (!memcmp(p, needle, nlen)) return i;
    }
    return end;
  }
  i = offset;
  if (i > end - nlen) i = end - nlen;
  for (;;) {
    if (hay[i] == needle[0] && !memcmp(hay + i, needle, nlen)) return i;
    if (i == 0) return end;
    i--;
  }
}

static size_t search_u16(const uint16_t *hay, size_t end, const uint16_t *needle,
                         size_t nlen, size_t offset, bool forward) {
  size_t i;
  if (nlen == 0 || nlen > end)
    return end;
  if (forward) {
    for (i = offset; i + nlen <= end; i++)
      if (hay[i] == needle[0] && !memcmp(hay + i, needle, nlen * 2)) return i;
    return end;
  }
  i = offset;
  if (i > end - nlen) i = end - nlen;
  for (;;) {
    if (hay[i] == needle[0] && !memcmp(hay + i, needle, nlen * 2)) return i;
    if (i == 0) return end;
    i--;
  }
}

static JSValue index_of_common(JSContext *ctx, uint8_t *hay, size_t hay_len,
                               const uint8_t *needle, size_t nlen, int64_t offset_i64,
                               int enc, bool forward, int64_t end_i64) {
  size_t search_end, offset, result;
  int64_t opt;
  if (enc == ENC_UCS2)
    hay_len &= ~(size_t)1;
  search_end = (size_t)(end_i64 < 0 ? 0 : (end_i64 > (int64_t)hay_len ? (int64_t)hay_len : end_i64));
  if (enc == ENC_UCS2)
    search_end &= ~(size_t)1;
  opt = index_of_offset(hay_len, offset_i64, nlen, forward);
  if (nlen == 0)
    return JS_NewFloat64(ctx, (double)(opt < (int64_t)search_end ? opt : (int64_t)search_end));
  if (hay_len == 0 || opt <= -1)
    return JS_NewInt32(ctx, -1);
  offset = (size_t)opt;
  if (!forward && offset >= search_end) {
    if (search_end == 0)
      return JS_NewInt32(ctx, -1);
    offset = search_end - 1;
  } else if (forward && offset >= search_end) {
    return JS_NewInt32(ctx, -1);
  }
  if ((forward && nlen + offset > search_end) || nlen > search_end)
    return JS_NewInt32(ctx, -1);
  if (enc == ENC_UCS2) {
    if (search_end < 2 || nlen < 2)
      return JS_NewInt32(ctx, -1);
    result = search_u16((const uint16_t *)hay, search_end / 2, (const uint16_t *)needle,
                        nlen / 2, offset / 2, forward);
    result = result >= search_end / 2 ? search_end : result * 2;
  } else {
    result = search_bytes(hay, search_end, needle, nlen, offset, forward);
  }
  return JS_NewInt32(ctx, result >= search_end ? -1 : (int32_t)result);
}

static JSValue buffer_index_of_string(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  size_t len, nlen;
  uint8_t *hay = view_data(ctx, ARG(0), &len), *needle;
  int64_t offset = nb_int64(ctx, ARG(2), 0), end = nb_int64(ctx, ARG(5), (int64_t)len);
  int enc = nb_int32(ctx, ARG(3), ENC_UTF8);
  bool forward = JS_ToBool(ctx, ARG(4));
  JSValue r;
  if (!hay)
    return throw_not_buffer(ctx);
  if (enc != ENC_UCS2 && enc != ENC_UTF8 && enc != ENC_ASCII && enc != ENC_LATIN1)
    enc = ENC_UTF8;
  nlen = node_string_bytes_size(ctx, ARG(1), enc);
  needle = malloc(nlen + 1);
  nlen = node_string_write(ctx, needle, nlen, ARG(1), enc, NULL);
  if (argc < 6)
    end = (int64_t)len;
  r = index_of_common(ctx, hay, len, needle, nlen, offset, enc, forward, end);
  free(needle);
  return r;
}

static JSValue buffer_index_of_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  size_t len, nlen;
  uint8_t *hay = view_data(ctx, ARG(0), &len), *needle = view_data(ctx, ARG(1), &nlen);
  int64_t offset = nb_int64(ctx, ARG(2), 0), end = nb_int64(ctx, ARG(5), (int64_t)len);
  int enc = nb_int32(ctx, ARG(3), ENC_UTF8);
  bool forward = JS_ToBool(ctx, ARG(4));
  if (!hay || !needle)
    return throw_not_buffer(ctx);
  if (argc < 6)
    end = (int64_t)len;
  return index_of_common(ctx, hay, len, needle, nlen, offset, enc, forward, end);
}

static JSValue buffer_index_of_number(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  size_t len, offset, search_end;
  uint8_t *data = view_data(ctx, ARG(0), &len);
  uint32_t needle = nb_uint32(ctx, ARG(1), 0);
  int64_t offset_i64 = nb_int64(ctx, ARG(2), 0), end_i64 = nb_int64(ctx, ARG(4), (int64_t)len);
  bool forward = JS_ToBool(ctx, ARG(3));
  int64_t opt;
  const uint8_t *p = NULL;
  if (!data)
    return throw_not_buffer(ctx);
  if (argc < 5)
    end_i64 = (int64_t)len;
  opt = index_of_offset(len, offset_i64, 1, forward);
  if (opt <= -1 || len == 0)
    return JS_NewInt32(ctx, -1);
  offset = (size_t)opt;
  search_end = (size_t)(end_i64 < 0 ? 0 : (end_i64 > (int64_t)len ? (int64_t)len : end_i64));
  needle &= 255;
  if (forward) {
    if (offset >= search_end)
      return JS_NewInt32(ctx, -1);
    p = memchr(data + offset, (int)needle, search_end - offset);
  } else {
    size_t back_end = offset + 1 < search_end ? offset + 1 : search_end, i;
    for (i = back_end; i > 0; i--)
      if (data[i - 1] == needle) {
        p = data + i - 1;
        break;
      }
  }
  return JS_NewInt32(ctx, p ? (int32_t)(p - data) : -1);
}

static JSValue buffer_swap(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv, int size) {
  size_t len, i;
  uint8_t *d = view_data(ctx, ARG(0), &len), t;
  if (!d)
    return throw_not_buffer(ctx);
  for (i = 0; i + size <= len; i += size) {
    int a = 0, b = size - 1;
    while (a < b) {
      t = d[i + a];
      d[i + a] = d[i + b];
      d[i + b] = t;
      a++;
      b--;
    }
  }
  return JS_UNDEFINED;
}

static bool is_utf8(const uint8_t *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint8_t c = s[i];
    if (c < 0x80) { i++; continue; }
    if (c >= 0xc2 && c <= 0xdf) {
      if (i + 1 >= n || (s[i + 1] & 0xc0) != 0x80) return false;
      i += 2;
    } else if (c >= 0xe0 && c <= 0xef) {
      uint8_t lo = c == 0xe0 ? 0xa0 : 0x80, hi = c == 0xed ? 0x9f : 0xbf;
      if (i + 2 >= n || s[i + 1] < lo || s[i + 1] > hi || (s[i + 2] & 0xc0) != 0x80)
        return false;
      i += 3;
    } else if (c >= 0xf0 && c <= 0xf4) {
      uint8_t lo = c == 0xf0 ? 0x90 : 0x80, hi = c == 0xf4 ? 0x8f : 0xbf;
      if (i + 3 >= n || s[i + 1] < lo || s[i + 1] > hi || (s[i + 2] & 0xc0) != 0x80 ||
          (s[i + 3] & 0xc0) != 0x80)
        return false;
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

static JSValue buffer_is_utf8(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  size_t len;
  uint8_t *d = view_data(ctx, ARG(0), &len);
  if (!d)
    return throw_not_buffer(ctx);
  return JS_NewBool(ctx, is_utf8(d, len));
}

static JSValue buffer_is_ascii(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  size_t len, i;
  uint8_t *d = view_data(ctx, ARG(0), &len);
  if (!d)
    return throw_not_buffer(ctx);
  for (i = 0; i < len; i++)
    if (d[i] & 0x80)
      return JS_FALSE;
  return JS_TRUE;
}

static JSValue buffer_set_buffer_prototype(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv) {
  Env *env = env_get(ctx);
  JS_FreeValue(ctx, env->buffer_prototype);
  env->buffer_prototype = JS_DupValue(ctx, ARG(0));
  return JS_UNDEFINED;
}

static JSValue buffer_set_detach_key(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
  return JS_UNDEFINED;
}

static JSValue buffer_copy_array_buffer(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
  size_t dlen, slen;
  uint8_t *d = view_data(ctx, ARG(0), &dlen), *s = view_data(ctx, ARG(2), &slen);
  uint32_t doff = nb_uint32(ctx, ARG(1), 0), soff = nb_uint32(ctx, ARG(3), 0);
  uint32_t n = nb_uint32(ctx, ARG(4), 0);
  if (!d || !s || doff > dlen || soff > slen || dlen - doff < n || slen - soff < n)
    return JS_ThrowRangeError(ctx, "copyArrayBuffer out of range");
  memcpy(d + doff, s + soff, n);
  return JS_UNDEFINED;
}

static void *ab_realloc(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
  if (size == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, size);
}

static JSValue buffer_create_unsafe_array_buffer(JSContext *ctx, JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
  double d = nb_double(ctx, ARG(0), -1);
  size_t size;
  uint8_t *p;
  if (argc != 1 || d < 0 || d > K_MAX_LENGTH)
    return JS_ThrowRangeError(ctx, "Invalid array buffer length");
  size = (size_t)d;
  p = malloc(size ? size : 1);
  if (!p)
    return node_throw_range_error(ctx, "ERR_MEMORY_ALLOCATION_FAILED",
                                  "Failed to allocate memory");
  return JS_NewArrayBuffer(ctx, p, size, 0, ab_realloc, NULL, false);
}

/* atob/btoa: forgiving base64 (WHATWG), returning -1 (bad char), -2 (bad
   length), -3 (non latin1) like node's binding */
static JSValue buffer_btoa(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  JSValue s = JS_ToString(ctx, ARG(0)), r;
  uint32_t len, i;
  int wide;
  const void *d;
  uint8_t *bytes;
  char *out;
  size_t n;
  if (JS_IsException(s))
    return s;
  d = JS_NodeStringData(ctx, &s, &len, &wide);
  bytes = malloc(len + 1);
  for (i = 0; i < len; i++) {
    uint32_t c = wide ? ((const uint16_t *)d)[i] : ((const uint8_t *)d)[i];
    if (c > 255) {
      free(bytes);
      JS_FreeValue(ctx, s);
      return JS_NewInt32(ctx, -1);
    }
    bytes[i] = c;
  }
  JS_FreeValue(ctx, s);
  n = node_base64_encoded_size(len, false);
  out = malloc(n + 1);
  node_base64_encode(out, bytes, len, false);
  r = JS_NewStringLen(ctx, out, n);
  free(out);
  free(bytes);
  return r;
}

static JSValue buffer_atob(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  JSValue s = JS_ToString(ctx, ARG(0)), r;
  uint32_t len, i, k = 0, pad = 0;
  int wide;
  const void *d;
  char *clean;
  uint8_t *out;
  uint32_t acc = 0;
  int bits = 0;
  size_t o = 0;
  if (JS_IsException(s))
    return s;
  d = JS_NodeStringData(ctx, &s, &len, &wide);
  clean = malloc(len + 1);
  for (i = 0; i < len; i++) {
    uint32_t c = wide ? ((const uint16_t *)d)[i] : ((const uint8_t *)d)[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r')
      continue;
    if (c > 127) {
      free(clean);
      JS_FreeValue(ctx, s);
      return JS_NewInt32(ctx, -1);
    }
    clean[k++] = (char)c;
  }
  JS_FreeValue(ctx, s);
  if (k % 4 == 0 && k > 0) {
    if (clean[k - 1] == '=') { k--; pad++; }
    if (k > 0 && clean[k - 1] == '=') { k--; pad++; }
  }
  if (k % 4 == 1) {
    free(clean);
    return JS_NewInt32(ctx, -2);
  }
  out = malloc(k + 1);
  for (i = 0; i < k; i++) {
    char c = clean[i];
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else {
      free(out);
      free(clean);
      return JS_NewInt32(ctx, -1);
    }
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (acc >> bits) & 0xff;
    }
  }
  (void)pad;
  r = JS_NodeNewStringLatin1(ctx, out, o);
  free(out);
  free(clean);
  return r;
}

JSValue binding_init_buffer(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  static const JSCFunctionListEntry funcs[] = {
    NB_FUNC("setBufferPrototype", 1, buffer_set_buffer_prototype),
    NB_FUNC("byteLengthUtf8", 1, buffer_byte_length_utf8),
    NB_FUNC("copy", 5, buffer_copy),
    NB_FUNC("compare", 2, buffer_compare),
    NB_FUNC("compareOffset", 6, buffer_compare_offset),
    NB_FUNC("fill", 5, buffer_fill),
    NB_FUNC("indexOfBuffer", 6, buffer_index_of_buffer),
    NB_FUNC("indexOfNumber", 5, buffer_index_of_number),
    NB_FUNC("indexOfString", 6, buffer_index_of_string),
    NB_FUNC("copyArrayBuffer", 5, buffer_copy_array_buffer),
    NB_FUNC("createUnsafeArrayBuffer", 1, buffer_create_unsafe_array_buffer),
    NB_FUNC_MAGIC("swap16", 1, buffer_swap, 2),
    NB_FUNC_MAGIC("swap32", 1, buffer_swap, 4),
    NB_FUNC_MAGIC("swap64", 1, buffer_swap, 8),
    NB_FUNC("isUtf8", 1, buffer_is_utf8),
    NB_FUNC("isAscii", 1, buffer_is_ascii),
    NB_FUNC("atob", 1, buffer_atob),
    NB_FUNC("btoa", 1, buffer_btoa),
    NB_FUNC("setDetachKey", 2, buffer_set_detach_key),
    NB_FUNC_MAGIC("asciiSlice", 3, buffer_slice, ENC_ASCII),
    NB_FUNC_MAGIC("base64Slice", 3, buffer_slice, ENC_BASE64),
    NB_FUNC_MAGIC("base64urlSlice", 3, buffer_slice, ENC_BASE64URL),
    NB_FUNC_MAGIC("latin1Slice", 3, buffer_slice, ENC_LATIN1),
    NB_FUNC_MAGIC("hexSlice", 3, buffer_slice, ENC_HEX),
    NB_FUNC_MAGIC("ucs2Slice", 3, buffer_slice, ENC_UCS2),
    NB_FUNC_MAGIC("utf8Slice", 3, buffer_slice, ENC_UTF8),
    NB_FUNC_MAGIC("base64Write", 4, buffer_write, ENC_BASE64),
    NB_FUNC_MAGIC("base64urlWrite", 4, buffer_write, ENC_BASE64URL),
    NB_FUNC_MAGIC("hexWrite", 4, buffer_write, ENC_HEX),
    NB_FUNC_MAGIC("ucs2Write", 4, buffer_write, ENC_UCS2),
    NB_FUNC_MAGIC("asciiWriteStatic", 4, buffer_write, ENC_ASCII),
    NB_FUNC_MAGIC("latin1WriteStatic", 4, buffer_write, ENC_LATIN1),
    NB_FUNC_MAGIC("utf8WriteStatic", 4, buffer_write, ENC_UTF8),
  };
  JS_SetPropertyFunctionList(ctx, t, funcs, countof(funcs));
  nb_set_double(ctx, t, "kMaxLength", K_MAX_LENGTH);
  nb_set_double(ctx, t, "kStringMaxLength", K_STRING_MAX_LENGTH);
  return t;
}

/* ---------------------------------------------------------------------- */
/* string_decoder */

enum { kIncompleteCharactersStart = 0, kIncompleteCharactersEnd = 4,
       kMissingBytes = 4, kBufferedBytes = 5, kEncodingField = 6, kNumFields = 7 };

static JSValue make_string(JSContext *ctx, const uint8_t *data, size_t n, int enc) {
  return node_string_encode(ctx, data, n, enc == ENC_BUFFER ? ENC_UTF8 : enc);
}

static JSValue concat2(JSContext *ctx, JSValue a, JSValue b) {
  JSValue r;
  if (JS_IsUndefined(a))
    return b;
  r = JS_NodeConcatStrings(ctx, a, b);
  return r;
}

static JSValue sd_decode(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  size_t slen, nread;
  uint8_t *state = view_data(ctx, ARG(0), &slen);
  const uint8_t *data = view_data(ctx, ARG(1), &nread);
  JSValue prepend = JS_UNDEFINED, body;
  int enc;
  if (!state || slen < kNumFields)
    return JS_ThrowTypeError(ctx, "invalid decoder");
  if (!data || !JS_NodeIsArrayBufferView(ARG(1)))
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
        "The \"buf\" argument must be an instance of Buffer, TypedArray, or DataView.%s", "");
  enc = state[kEncodingField];
  if (enc == ENC_UTF8 || enc == ENC_UCS2 || enc == ENC_BASE64 || enc == ENC_BASE64URL) {
    uint8_t *inc = state + kIncompleteCharactersStart;
    if (state[kMissingBytes] > 0) {
      size_t found, i;
      if (enc == ENC_UTF8) {
        for (i = 0; i < nread && i < state[kMissingBytes]; i++) {
          if ((data[i] & 0xc0) != 0x80) {
            state[kMissingBytes] = 0;
            memcpy(inc + state[kBufferedBytes], data, i);
            state[kBufferedBytes] += i;
            data += i;
            nread -= i;
            break;
          }
        }
      }
      found = nread < state[kMissingBytes] ? nread : state[kMissingBytes];
      memcpy(inc + state[kBufferedBytes], data, found);
      data += found;
      nread -= found;
      state[kMissingBytes] -= found;
      state[kBufferedBytes] += found;
      if (state[kMissingBytes] == 0) {
        prepend = make_string(ctx, inc, state[kBufferedBytes], enc);
        state[kBufferedBytes] = 0;
      }
    }
    if (nread == 0) {
      body = JS_IsUndefined(prepend) ? JS_NewString(ctx, "") : prepend;
      return body;
    }
    if (enc == ENC_UTF8 && (data[nread - 1] & 0x80)) {
      size_t i;
      for (i = nread - 1;; --i) {
        state[kBufferedBytes]++;
        if ((data[i] & 0xc0) == 0x80) {
          if (state[kBufferedBytes] >= 4 || i == 0) {
            state[kMissingBytes] = 0;
            state[kBufferedBytes] = 0;
            break;
          }
        } else {
          if ((data[i] & 0xe0) == 0xc0)
            state[kMissingBytes] = 2;
          else if ((data[i] & 0xf0) == 0xe0)
            state[kMissingBytes] = 3;
          else if ((data[i] & 0xf8) == 0xf0)
            state[kMissingBytes] = 4;
          else {
            state[kBufferedBytes] = 0;
            break;
          }
          if (state[kBufferedBytes] >= state[kMissingBytes]) {
            state[kMissingBytes] = 0;
            state[kBufferedBytes] = 0;
          }
          state[kMissingBytes] -= state[kBufferedBytes];
          break;
        }
      }
    } else if (enc == ENC_UCS2) {
      if ((nread % 2) == 1) {
        state[kBufferedBytes] = 1;
        state[kMissingBytes] = 1;
      } else if ((data[nread - 1] & 0xfc) == 0xd8) {
        state[kBufferedBytes] = 2;
        state[kMissingBytes] = 2;
      }
    } else if (enc == ENC_BASE64 || enc == ENC_BASE64URL) {
      state[kBufferedBytes] = nread % 3;
      if (state[kBufferedBytes] > 0)
        state[kMissingBytes] = 3 - state[kBufferedBytes];
    }
    if (state[kBufferedBytes] > 0) {
      nread -= state[kBufferedBytes];
      memcpy(inc, data + nread, state[kBufferedBytes]);
    }
    body = nread > 0 ? make_string(ctx, data, nread, enc) : JS_NewString(ctx, "");
    if (JS_IsException(body)) {
      JS_FreeValue(ctx, prepend);
      return body;
    }
    return concat2(ctx, prepend, body);
  }
  return make_string(ctx, data, nread, enc);
}

static JSValue sd_flush(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
  size_t slen;
  uint8_t *state = view_data(ctx, ARG(0), &slen);
  JSValue r;
  int enc;
  if (!state || slen < kNumFields)
    return JS_ThrowTypeError(ctx, "invalid decoder");
  enc = state[kEncodingField];
  if (enc == ENC_UCS2 && state[kBufferedBytes] % 2 == 1) {
    state[kMissingBytes]--;
    state[kBufferedBytes]--;
  }
  if (state[kBufferedBytes] == 0)
    return JS_NewString(ctx, "");
  r = make_string(ctx, state + kIncompleteCharactersStart, state[kBufferedBytes], enc);
  state[kMissingBytes] = 0;
  state[kBufferedBytes] = 0;
  return r;
}

JSValue binding_init_string_decoder(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx), encs = JS_NewArray(ctx);
  static const char *const names[] = { "ascii", "utf8", "base64", "utf16le", "latin1",
                                       "hex", "buffer", "base64url" };
  size_t i;
  for (i = 0; i < countof(names); i++)
    JS_SetPropertyUint32(ctx, encs, i, JS_NewString(ctx, names[i]));
  nb_set(ctx, t, "encodings", encs);
  nb_set_int(ctx, t, "kIncompleteCharactersStart", kIncompleteCharactersStart);
  nb_set_int(ctx, t, "kIncompleteCharactersEnd", kIncompleteCharactersEnd);
  nb_set_int(ctx, t, "kMissingBytes", kMissingBytes);
  nb_set_int(ctx, t, "kBufferedBytes", kBufferedBytes);
  nb_set_int(ctx, t, "kEncodingField", kEncodingField);
  nb_set_int(ctx, t, "kNumFields", kNumFields);
  nb_set_int(ctx, t, "kSize", kNumFields);
  nb_set_method(ctx, t, "decode", sd_decode, 2);
  nb_set_method(ctx, t, "flush", sd_flush, 1);
  return t;
}

/* ---------------------------------------------------------------------- */
/* encoding_binding */

static uint32_t *encode_into_results_of(Env *env) {
  return env_scratch(env, "encode_into_results", 2 * sizeof(uint32_t));
}

static JSValue enc_encode_into(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  size_t len;
  uint8_t *dst = view_data(ctx, ARG(1), &len);
  int nchars = 0;
  size_t written;
  if (!dst)
    return throw_not_buffer(ctx);
  uint32_t *encode_into_results = encode_into_results_of(env_get(ctx));
  written = node_string_write(ctx, dst, len, ARG(0), ENC_UTF8, &nchars);
  encode_into_results[0] = nchars;
  encode_into_results[1] = (uint32_t)written;
  return JS_UNDEFINED;
}

static JSValue enc_encode_utf8_string(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
  size_t n;
  char *s = node_string_to_utf8(ctx, ARG(0), &n);
  JSValue r;
  if (!s)
    return JS_EXCEPTION;
  r = JS_NewUint8Array(ctx, (uint8_t *)s, n, ab_realloc, NULL, false);
  return r;
}

static JSValue enc_decode_utf8(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  size_t len;
  uint8_t *d = view_data(ctx, ARG(0), &len);
  bool ignore_bom = JS_ToBool(ctx, ARG(1)), fatal = JS_ToBool(ctx, ARG(2));
  if (!d)
    return node_throw_type_error(ctx, "ERR_INVALID_ARG_TYPE",
        "The \"list\" argument must be an instance of SharedArrayBuffer, ArrayBuffer or ArrayBufferView.");
  if (fatal && !is_utf8(d, len))
    return node_throw_type_error(ctx, "ERR_ENCODING_INVALID_ENCODED_DATA",
                                 "The encoded data was not valid for encoding utf-8");
  if (!ignore_bom && len >= 3 && d[0] == 0xef && d[1] == 0xbb && d[2] == 0xbf) {
    d += 3;
    len -= 3;
  }
  return node_new_utf8_string(ctx, d, len);
}

JSValue node_url_domain_to_ascii(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv);
JSValue node_url_domain_to_unicode(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv);

JSValue binding_init_encoding_binding(Env *env) {
  JSContext *ctx = env->ctx;
  JSValue t = JS_NewObject(ctx);
  nb_set(ctx, t, "encodeIntoResults",
         nb_new_typed_array(ctx, JS_TYPED_ARRAY_UINT32, encode_into_results_of(env), 2, 4));
  nb_set_method(ctx, t, "encodeInto", enc_encode_into, 2);
  nb_set_method(ctx, t, "encodeUtf8String", enc_encode_utf8_string, 1);
  nb_set_method(ctx, t, "decodeUTF8", enc_decode_utf8, 3);
  nb_set_method(ctx, t, "toASCII", node_url_domain_to_ascii, 1);
  nb_set_method(ctx, t, "toUnicode", node_url_domain_to_unicode, 1);
  return t;
}
