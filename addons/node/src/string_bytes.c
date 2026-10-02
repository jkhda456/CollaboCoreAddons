/* String <-> bytes in Node's encodings (src/string_bytes.cc), working on
 * QuickJS strings directly: 8-bit (Latin-1) or 16-bit (UTF-16) characters. */
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

int node_parse_encoding(JSContext *ctx, JSValueConst v, int def) {
  size_t len;
  const char *s;
  char buf[16];
  size_t i;
  int r = def;
  if (!JS_IsString(v))
    return def;
  s = JS_ToCStringLen(ctx, &len, v);
  if (!s)
    return def;
  if (len >= sizeof(buf)) {
    JS_FreeCString(ctx, s);
    return def;
  }
  for (i = 0; i < len; i++)
    buf[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
  buf[len] = 0;
  JS_FreeCString(ctx, s);
  if (!strcmp(buf, "utf8") || !strcmp(buf, "utf-8")) r = ENC_UTF8;
  else if (!strcmp(buf, "ucs2") || !strcmp(buf, "ucs-2") || !strcmp(buf, "utf16le") ||
           !strcmp(buf, "utf-16le")) r = ENC_UCS2;
  else if (!strcmp(buf, "latin1") || !strcmp(buf, "binary")) r = ENC_LATIN1;
  else if (!strcmp(buf, "ascii")) r = ENC_ASCII;
  else if (!strcmp(buf, "base64")) r = ENC_BASE64;
  else if (!strcmp(buf, "base64url")) r = ENC_BASE64URL;
  else if (!strcmp(buf, "hex")) r = ENC_HEX;
  else if (!strcmp(buf, "buffer")) r = ENC_BUFFER;
  return r;
}

/* ---------------------------------------------------------------------- */
/* UTF-8 */

static size_t utf8_len_latin1(const uint8_t *s, size_t n) {
  size_t r = n, i;
  for (i = 0; i < n; i++)
    r += s[i] >> 7;
  return r;
}

static size_t utf8_len_utf16(const uint16_t *s, size_t n) {
  size_t r = 0, i;
  for (i = 0; i < n; i++) {
    uint16_t c = s[i];
    if (c < 0x80) r += 1;
    else if (c < 0x800) r += 2;
    else if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
      r += 4;
      i++;
    } else r += 3; /* includes lone surrogates -> U+FFFD */
  }
  return r;
}

size_t node_utf8_length(JSContext *ctx, JSValueConst str) {
  JSValue v = JS_DupValue(ctx, str);
  uint32_t len;
  int wide;
  const void *d = JS_NodeStringData(ctx, &v, &len, &wide);
  size_t r = 0;
  if (d)
    r = wide ? utf8_len_utf16(d, len) : utf8_len_latin1(d, len);
  JS_FreeValue(ctx, v);
  return r;
}

/* writes whole characters only; returns bytes written, *nchars UTF-16 units read */
static size_t write_utf8_latin1(uint8_t *dst, size_t cap, const uint8_t *s, size_t n,
                                int *nchars) {
  size_t o = 0, i;
  for (i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c < 0x80) {
      if (o + 1 > cap) break;
      dst[o++] = c;
    } else {
      if (o + 2 > cap) break;
      dst[o++] = 0xc0 | (c >> 6);
      dst[o++] = 0x80 | (c & 0x3f);
    }
  }
  if (nchars) *nchars = (int)i;
  return o;
}

static size_t write_utf8_utf16(uint8_t *dst, size_t cap, const uint16_t *s, size_t n,
                               int *nchars) {
  size_t o = 0, i;
  for (i = 0; i < n; i++) {
    uint32_t c = s[i];
    if (c < 0x80) {
      if (o + 1 > cap) break;
      dst[o++] = c;
    } else if (c < 0x800) {
      if (o + 2 > cap) break;
      dst[o++] = 0xc0 | (c >> 6);
      dst[o++] = 0x80 | (c & 0x3f);
    } else if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
      uint32_t cp = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
      if (o + 4 > cap) break;
      dst[o++] = 0xf0 | (cp >> 18);
      dst[o++] = 0x80 | ((cp >> 12) & 0x3f);
      dst[o++] = 0x80 | ((cp >> 6) & 0x3f);
      dst[o++] = 0x80 | (cp & 0x3f);
      i++;
    } else {
      if (c >= 0xd800 && c < 0xe000)
        c = 0xfffd;
      if (o + 3 > cap) break;
      dst[o++] = 0xe0 | (c >> 12);
      dst[o++] = 0x80 | ((c >> 6) & 0x3f);
      dst[o++] = 0x80 | (c & 0x3f);
    }
  }
  if (nchars) *nchars = (int)i;
  return o;
}

char *node_string_to_utf8(JSContext *ctx, JSValueConst str, size_t *plen) {
  JSValue v = JS_DupValue(ctx, str);
  uint32_t len;
  int wide;
  const void *d;
  size_t n;
  char *out;
  if (!JS_IsString(v)) {
    JSValue s = JS_ToString(ctx, v);
    JS_FreeValue(ctx, v);
    if (JS_IsException(s))
      return NULL;
    v = s;
  }
  d = JS_NodeStringData(ctx, &v, &len, &wide);
  if (!d) {
    JS_FreeValue(ctx, v);
    return NULL;
  }
  n = wide ? utf8_len_utf16(d, len) : utf8_len_latin1(d, len);
  out = malloc(n + 1);
  if (wide)
    write_utf8_utf16((uint8_t *)out, n, d, len, NULL);
  else
    write_utf8_latin1((uint8_t *)out, n, d, len, NULL);
  out[n] = 0;
  if (plen)
    *plen = n;
  JS_FreeValue(ctx, v);
  return out;
}

/* UTF-8 decoding with U+FFFD for each maximal invalid subpart (WHATWG) */
/* V8's String::kMaxLength (64-bit), buffer.constants.MAX_STRING_LENGTH */
#define STRING_MAX_LENGTH ((1u << 29) - 24)

static JSValue string_too_long(JSContext *ctx) {
  return node_throw_error(ctx, "ERR_STRING_TOO_LONG",
                          "Cannot create a string longer than 0x%x characters", STRING_MAX_LENGTH);
}

JSValue node_new_utf8_string(JSContext *ctx, const uint8_t *buf, size_t len) {
  size_t i = 0, n16 = 0;
  bool all_latin1 = true;
  JSValue s;
  void *data;
  /* ASCII fast path */
  while (i < len && buf[i] < 0x80)
    i++;
  if (i == len)
    return len > STRING_MAX_LENGTH ? string_too_long(ctx) : JS_NodeNewStringLatin1(ctx, buf, len);
  /* count UTF-16 units and see if all fit 8 bits */
  n16 = i;
  {
    size_t j = i;
    while (j < len) {
      uint8_t c = buf[j];
      if (c < 0x80) { n16++; j++; continue; }
      if (c >= 0xc2 && c <= 0xdf && j + 1 < len && (buf[j + 1] & 0xc0) == 0x80) {
        uint32_t cp = ((c & 0x1f) << 6) | (buf[j + 1] & 0x3f);
        if (cp > 0xff) all_latin1 = false;
        n16++; j += 2; continue;
      }
      if (c >= 0xe0 && c <= 0xef) {
        uint8_t lo = c == 0xe0 ? 0xa0 : 0x80, hi = c == 0xed ? 0x9f : 0xbf;
        if (j + 1 < len && buf[j + 1] >= lo && buf[j + 1] <= hi) {
          if (j + 2 < len && (buf[j + 2] & 0xc0) == 0x80) {
            all_latin1 = false; n16++; j += 3; continue;
          }
          all_latin1 = false; n16++; j += 2; continue; /* truncated: one FFFD */
        }
        all_latin1 = false; n16++; j++; continue;
      }
      if (c >= 0xf0 && c <= 0xf4) {
        uint8_t lo = c == 0xf0 ? 0x90 : 0x80, hi = c == 0xf4 ? 0x8f : 0xbf;
        all_latin1 = false;
        if (j + 1 < len && buf[j + 1] >= lo && buf[j + 1] <= hi) {
          if (j + 2 < len && (buf[j + 2] & 0xc0) == 0x80) {
            if (j + 3 < len && (buf[j + 3] & 0xc0) == 0x80) {
              n16 += 2; j += 4; continue;
            }
            n16++; j += 3; continue;
          }
          n16++; j += 2; continue;
        }
        n16++; j++; continue;
      }
      /* invalid lead byte */
      all_latin1 = false;
      n16++; j++;
    }
  }
  if (n16 > STRING_MAX_LENGTH)
    return string_too_long(ctx);
  s = JS_NodeNewStringRaw(ctx, n16, all_latin1 ? 0 : 1, &data);
  if (JS_IsException(s))
    return s;
  if (all_latin1) {
    uint8_t *o = data;
    size_t j = 0, k = 0;
    while (j < len) {
      uint8_t c = buf[j];
      if (c < 0x80) { o[k++] = c; j++; }
      else { o[k++] = ((c & 0x1f) << 6) | (buf[j + 1] & 0x3f); j += 2; }
    }
  } else {
    uint16_t *o = data;
    size_t j = 0, k = 0;
    while (j < len) {
      uint8_t c = buf[j];
      if (c < 0x80) { o[k++] = c; j++; continue; }
      if (c >= 0xc2 && c <= 0xdf && j + 1 < len && (buf[j + 1] & 0xc0) == 0x80) {
        o[k++] = ((c & 0x1f) << 6) | (buf[j + 1] & 0x3f);
        j += 2; continue;
      }
      if (c >= 0xe0 && c <= 0xef) {
        uint8_t lo = c == 0xe0 ? 0xa0 : 0x80, hi = c == 0xed ? 0x9f : 0xbf;
        if (j + 1 < len && buf[j + 1] >= lo && buf[j + 1] <= hi) {
          if (j + 2 < len && (buf[j + 2] & 0xc0) == 0x80) {
            o[k++] = ((c & 0x0f) << 12) | ((buf[j + 1] & 0x3f) << 6) | (buf[j + 2] & 0x3f);
            j += 3; continue;
          }
          o[k++] = 0xfffd; j += 2; continue;
        }
        o[k++] = 0xfffd; j++; continue;
      }
      if (c >= 0xf0 && c <= 0xf4) {
        uint8_t lo = c == 0xf0 ? 0x90 : 0x80, hi = c == 0xf4 ? 0x8f : 0xbf;
        if (j + 1 < len && buf[j + 1] >= lo && buf[j + 1] <= hi) {
          if (j + 2 < len && (buf[j + 2] & 0xc0) == 0x80) {
            if (j + 3 < len && (buf[j + 3] & 0xc0) == 0x80) {
              uint32_t cp = ((c & 0x07) << 18) | ((buf[j + 1] & 0x3f) << 12) |
                            ((buf[j + 2] & 0x3f) << 6) | (buf[j + 3] & 0x3f);
              cp -= 0x10000;
              o[k++] = 0xd800 + (cp >> 10);
              o[k++] = 0xdc00 + (cp & 0x3ff);
              j += 4; continue;
            }
            o[k++] = 0xfffd; j += 3; continue;
          }
          o[k++] = 0xfffd; j += 2; continue;
        }
        o[k++] = 0xfffd; j++; continue;
      }
      o[k++] = 0xfffd; j++;
    }
  }
  return s;
}

/* ---------------------------------------------------------------------- */
/* base64 / hex */

static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64urlchars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int8_t b64val[256];
static void init_b64(void) {
  static bool done;
  int i;
  if (done) return;
  for (i = 0; i < 256; i++) b64val[i] = -1;
  for (i = 0; i < 64; i++) {
    b64val[(uint8_t)b64chars[i]] = i;
    b64val[(uint8_t)b64urlchars[i]] = i;
  }
  done = true;
}

static size_t base64_encoded_size(size_t n, bool url) {
  return url ? (n * 4 + 2) / 3 : ((n + 2) / 3) * 4;
}

static size_t base64_encode(char *dst, const uint8_t *src, size_t n, bool url) {
  const char *t = url ? b64urlchars : b64chars;
  size_t i = 0, o = 0;
  while (i + 2 < n) {
    uint32_t v = (src[i] << 16) | (src[i + 1] << 8) | src[i + 2];
    dst[o++] = t[v >> 18];
    dst[o++] = t[(v >> 12) & 63];
    dst[o++] = t[(v >> 6) & 63];
    dst[o++] = t[v & 63];
    i += 3;
  }
  if (i < n) {
    uint32_t v = src[i] << 16;
    if (i + 1 < n) v |= src[i + 1] << 8;
    dst[o++] = t[v >> 18];
    dst[o++] = t[(v >> 12) & 63];
    if (i + 1 < n) {
      dst[o++] = t[(v >> 6) & 63];
      if (!url) dst[o++] = '=';
    } else if (!url) {
      dst[o++] = '=';
      dst[o++] = '=';
    }
  }
  return o;
}

/* Node's decoder: skips characters that are not base64, stops at '=' */
static size_t base64_decoded_size_chars(const void *s, size_t n, int wide) {
  size_t size;
  if (n == 0) return 0;
  /* trailing padding */
  if (wide) {
    const uint16_t *p = s;
    if (p[n - 1] == '=') { n--; if (n && p[n - 1] == '=') n--; }
  } else {
    const uint8_t *p = s;
    if (p[n - 1] == '=') { n--; if (n && p[n - 1] == '=') n--; }
  }
  size = (n / 4) * 3;
  switch (n % 4) {
  case 2: size += 1; break;
  case 3: size += 2; break;
  case 1: size += (n == 1) ? 0 : 0; break;
  }
  return size;
}

static size_t base64_decode_chars(uint8_t *dst, size_t cap, const void *s, size_t n, int wide) {
  uint32_t acc = 0;
  int bits = 0;
  size_t o = 0, i;
  init_b64();
  for (i = 0; i < n && o < cap; i++) {
    uint32_t c = wide ? ((const uint16_t *)s)[i] : ((const uint8_t *)s)[i];
    int8_t v;
    if (c == '=')
      break;
    if (c > 255 || (v = b64val[c]) < 0)
      continue;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      dst[o++] = (acc >> bits) & 0xff;
    }
  }
  return o;
}

static int hexval(uint32_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* ---------------------------------------------------------------------- */

size_t node_string_bytes_size(JSContext *ctx, JSValueConst str, int enc) {
  JSValue v = JS_DupValue(ctx, str);
  uint32_t len;
  int wide;
  const void *d = JS_NodeStringData(ctx, &v, &len, &wide);
  size_t r = 0;
  if (!d) {
    JS_FreeValue(ctx, v);
    return 0;
  }
  switch (enc) {
  case ENC_ASCII:
  case ENC_LATIN1:
    r = len;
    break;
  case ENC_BUFFER:
  case ENC_UTF8:
    r = wide ? utf8_len_utf16(d, len) : utf8_len_latin1(d, len);
    break;
  case ENC_UCS2:
    r = (size_t)len * 2;
    break;
  case ENC_BASE64:
  case ENC_BASE64URL:
    r = base64_decoded_size_chars(d, len, wide);
    break;
  case ENC_HEX:
    r = len / 2;
    break;
  }
  JS_FreeValue(ctx, v);
  return r;
}

size_t node_string_write(JSContext *ctx, uint8_t *dst, size_t cap, JSValueConst str,
                         int enc, int *nchars) {
  JSValue v = JS_DupValue(ctx, str);
  uint32_t len, i;
  int wide;
  const void *d = JS_NodeStringData(ctx, &v, &len, &wide);
  size_t o = 0;
  if (nchars) *nchars = 0;
  if (!d) {
    JS_FreeValue(ctx, v);
    return 0;
  }
  switch (enc) {
  case ENC_ASCII:
  case ENC_LATIN1:
    o = len < cap ? len : cap;
    if (wide) {
      const uint16_t *s = d;
      for (i = 0; i < o; i++) dst[i] = (uint8_t)s[i];
    } else {
      memcpy(dst, d, o);
    }
    if (nchars) *nchars = (int)o;
    break;
  case ENC_BUFFER:
  case ENC_UTF8:
    o = wide ? write_utf8_utf16(dst, cap, d, len, nchars)
             : write_utf8_latin1(dst, cap, d, len, nchars);
    break;
  case ENC_UCS2: {
    size_t n = len;
    if (n > cap / 2) n = cap / 2;
    if (wide) {
      memcpy(dst, d, n * 2);
    } else {
      const uint8_t *s = d;
      for (i = 0; i < n; i++) { dst[2 * i] = s[i]; dst[2 * i + 1] = 0; }
    }
    o = n * 2;
    if (nchars) *nchars = (int)n;
    break;
  }
  case ENC_BASE64:
  case ENC_BASE64URL:
    o = base64_decode_chars(dst, cap, d, len, wide);
    if (nchars) *nchars = (int)len;
    break;
  case ENC_HEX: {
    size_t n = len / 2;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) {
      uint32_t c1 = wide ? ((const uint16_t *)d)[2 * i] : ((const uint8_t *)d)[2 * i];
      uint32_t c2 = wide ? ((const uint16_t *)d)[2 * i + 1] : ((const uint8_t *)d)[2 * i + 1];
      int a = hexval(c1), b = hexval(c2);
      if (a < 0 || b < 0) break;
      dst[i] = (a << 4) | b;
    }
    o = i;
    if (nchars) *nchars = (int)(i * 2);
    break;
  }
  }
  JS_FreeValue(ctx, v);
  return o;
}

JSValue node_string_encode(JSContext *ctx, const uint8_t *buf, size_t len, int enc) {
  JSValue s;
  void *data;
  size_t i, chars;
  switch (enc) {
  case ENC_UCS2: chars = len / 2; break;
  case ENC_HEX: chars = len * 2; break;
  case ENC_BASE64: case ENC_BASE64URL: chars = base64_encoded_size(len, enc == ENC_BASE64URL); break;
  case ENC_BUFFER: case ENC_UTF8: chars = 0; break;  /* checked as it is decoded */
  default: chars = len; break;
  }
  if (chars > STRING_MAX_LENGTH)
    return string_too_long(ctx);
  switch (enc) {
  case ENC_ASCII:
    s = JS_NodeNewStringRaw(ctx, len, 0, &data);
    if (JS_IsException(s)) return s;
    for (i = 0; i < len; i++) ((uint8_t *)data)[i] = buf[i] & 0x7f;
    return s;
  case ENC_LATIN1:
    return JS_NodeNewStringLatin1(ctx, buf, len);
  case ENC_UCS2: {
    size_t n = len / 2;
    uint16_t *tmp = malloc(n * 2 + 2);
    for (i = 0; i < n; i++) tmp[i] = buf[2 * i] | (buf[2 * i + 1] << 8);
    s = JS_NodeNewStringUTF16(ctx, tmp, n);
    free(tmp);
    return s;
  }
  case ENC_HEX: {
    static const char hex[] = "0123456789abcdef";
    s = JS_NodeNewStringRaw(ctx, len * 2, 0, &data);
    if (JS_IsException(s)) return s;
    for (i = 0; i < len; i++) {
      ((uint8_t *)data)[2 * i] = hex[buf[i] >> 4];
      ((uint8_t *)data)[2 * i + 1] = hex[buf[i] & 15];
    }
    return s;
  }
  case ENC_BASE64:
  case ENC_BASE64URL: {
    bool url = enc == ENC_BASE64URL;
    size_t n = base64_encoded_size(len, url);
    s = JS_NodeNewStringRaw(ctx, n, 0, &data);
    if (JS_IsException(s)) return s;
    base64_encode(data, buf, len, url);
    return s;
  }
  case ENC_BUFFER:
    return nb_new_buffer(ctx, buf, len);
  case ENC_UTF8:
  default:
    return node_new_utf8_string(ctx, buf, len);
  }
}

/* exported for buffer.c (atob/btoa) */
size_t node_base64_encode(char *dst, const uint8_t *src, size_t n, bool url) {
  return base64_encode(dst, src, n, url);
}
size_t node_base64_encoded_size(size_t n, bool url) {
  return base64_encoded_size(n, url);
}
