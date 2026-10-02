/* The compile cache (compile_cache.cc): compiled code kept on disk, keyed by
 * the source, so a program's next start reads QuickJS bytecode instead of
 * parsing its JavaScript again (Claude Code's cli.js is 13 MB: parsing it is
 * most of its start-up).
 *
 * Node's API turns it on: NODE_COMPILE_CACHE=dir, module.enableCompileCache().
 * In the collaboCore guest it is also on by itself for large files, in
 * /tmp/node-compile-cache (NODE_DISABLE_COMPILE_CACHE=1 turns both off); that
 * one is not reported as enabled to JS.
 *
 * An entry: "QJSC" + the payload's SHA-256 + the payload (JS_WriteObject's
 * bytecode); the file name is the SHA-256 of what was compiled: this build of
 * node, the kind of code, the file name, the line offset and the source. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "node.h"

extern const uint8_t node_builtins_blob[];
extern const struct { const char *id; uint32_t off, len; } node_builtins_index[];
extern const int node_builtins_count;

#define IMPLICIT_MIN_SIZE (32 * 1024)

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static char *explicit_dir;   /* enabled through Node's API: <dir>/qjs-<build> */
static char *explicit_root;  /* what was asked for, as getCompileCacheDir reports it */
static char *implicit_dir;   /* the guest's own */
static bool initialized;
static char build_id[17];

/* NODE_DEBUG_NATIVE=COMPILE_CACHE, as in Node */
static bool debug(void) {
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("NODE_DEBUG_NATIVE");
    on = e && strstr(e, "COMPILE_CACHE") != NULL;
  }
  return on;
}

static bool disabled(void) {
  const char *e = getenv("NODE_DISABLE_COMPILE_CACHE");
  return e && *e && strcmp(e, "0");
}

static void sha256(const void *a, size_t alen, const void *b, size_t blen, unsigned char out[32]) {
  EVP_MD_CTX *md = EVP_MD_CTX_new();
  unsigned int n = 32;
  EVP_DigestInit_ex(md, EVP_sha256(), NULL);
  if (alen)
    EVP_DigestUpdate(md, a, alen);
  if (blen)
    EVP_DigestUpdate(md, b, blen);
  EVP_DigestFinal_ex(md, out, &n);
  EVP_MD_CTX_free(md);
}

static void hex(const unsigned char *in, size_t n, char *out) {
  static const char d[] = "0123456789abcdef";
  size_t i;
  for (i = 0; i < n; i++) {
    out[2 * i] = d[in[i] >> 4];
    out[2 * i + 1] = d[in[i] & 15];
  }
  out[2 * n] = 0;
}

static int mkdirs(const char *path) {
  char buf[4096], *p;
  snprintf(buf, sizeof(buf), "%s", path);
  for (p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(buf, 0700) < 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  if (mkdir(buf, 0700) < 0 && errno != EEXIST)
    return -1;
  return 0;
}

/* this node's code: the builtins are QuickJS bytecode of the same engine,
   so they change whenever its bytecode may */
static void init_locked(void) {
  unsigned char h[32];
  char v[64];
  size_t blob_size;
  if (initialized)
    return;
  initialized = true;
  snprintf(v, sizeof(v), "%s/qjs-%s/%d", NODE_VERSION_STRING, JS_GetVersion(), (int)sizeof(void *));
  blob_size = node_builtins_count ? node_builtins_index[node_builtins_count - 1].off +
                                        node_builtins_index[node_builtins_count - 1].len : 0;
  sha256(v, strlen(v), node_builtins_blob, blob_size, h);
  hex(h, 8, build_id);
#ifdef __wasm__
  if (!disabled()) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/node-compile-cache/qjs-%s", build_id);
    if (mkdirs(dir) == 0)
      implicit_dir = strdup(dir);
  }
#endif
}

/* enableCompileCache(directory): FAILED 0, ENABLED 1, ALREADY_ENABLED 2, DISABLED 3 */
int node_compile_cache_enable(const char *dir, char **message, char **directory) {
  char path[4096];
  int status;
  *message = NULL;
  *directory = NULL;
  if (disabled()) {
    *message = strdup("Disabled by NODE_DISABLE_COMPILE_CACHE");
    return 3;
  }
  pthread_mutex_lock(&cache_lock);
  init_locked();
  if (explicit_dir) {
    *directory = strdup(explicit_root);
    pthread_mutex_unlock(&cache_lock);
    return 2;
  }
  snprintf(path, sizeof(path), "%s/qjs-%s", dir, build_id);
  if (mkdirs(path) < 0) {
    char m[4200];
    snprintf(m, sizeof(m), "Cannot create cache directory: %s", strerror(errno));
    *message = strdup(m);
    status = 0;
  } else {
    explicit_dir = strdup(path);
    explicit_root = strdup(dir);
    *directory = strdup(dir);
    status = 1;
  }
  pthread_mutex_unlock(&cache_lock);
  return status;
}

const char *node_compile_cache_dir(void) {
  return explicit_root;
}

/* at start: NODE_COMPILE_CACHE turns it on, as in Node */
void node_compile_cache_init(void) {
  const char *dir = getenv("NODE_COMPILE_CACHE");
  char *m, *d;
  pthread_mutex_lock(&cache_lock);
  init_locked();
  pthread_mutex_unlock(&cache_lock);
  if (dir && *dir && !disabled()) {
    node_compile_cache_enable(dir, &m, &d);
    free(m);
    free(d);
  }
}

static uint8_t *read_entry(const char *path, size_t *plen) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  struct stat st;
  uint8_t *buf, h[32];
  size_t got = 0;
  if (fd < 0)
    return NULL;
  if (fstat(fd, &st) < 0 || st.st_size < 36) {
    close(fd);
    return NULL;
  }
  buf = malloc(st.st_size);
  while (got < (size_t)st.st_size) {
    ssize_t n = read(fd, buf + got, st.st_size - got);
    if (n <= 0)
      break;
    got += n;
  }
  close(fd);
  if (got != (size_t)st.st_size || memcmp(buf, "QJSC", 4)) {
    free(buf);
    return NULL;
  }
  sha256(buf + 36, got - 36, NULL, 0, h);
  if (memcmp(h, buf + 4, 32)) {
    free(buf);
    return NULL;
  }
  *plen = got;
  return buf;
}

static void write_entry(const char *path, const uint8_t *data, size_t len) {
  char tmp[4200];
  uint8_t head[36];
  int fd;
  static atomic_int seq;
  /* unique between processes and between a process's threads */
  snprintf(tmp, sizeof(tmp), "%s.%d.%d.tmp", path, (int)getpid(), atomic_fetch_add(&seq, 1));
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    return;
  memcpy(head, "QJSC", 4);
  sha256(data, len, NULL, 0, head + 4);
  if (write(fd, head, 36) == 36 && write(fd, data, len) == (ssize_t)len && close(fd) == 0) {
    if (rename(tmp, path) < 0)
      unlink(tmp);
    return;
  }
  close(fd);
  unlink(tmp);
}

/* JS_Eval2 of a COMPILE_ONLY script or module, through the cache */
JSValue node_cached_compile(JSContext *ctx, const char *src, size_t len, JSEvalOptions *opts) {
  const char *dir;
  char path[4096], name[65];
  unsigned char key[32];
  uint8_t *entry, *bc;
  size_t elen, bclen;
  JSValue r;
  EVP_MD_CTX *md;
  unsigned int n = 32;
  pthread_mutex_lock(&cache_lock);
  if (!initialized)
    init_locked();
  dir = explicit_dir ? explicit_dir : (len >= IMPLICIT_MIN_SIZE ? implicit_dir : NULL);
  pthread_mutex_unlock(&cache_lock);
  if (!dir || !(opts->eval_flags & JS_EVAL_FLAG_COMPILE_ONLY))
    return JS_Eval2(ctx, src, len, opts);

  md = EVP_MD_CTX_new();
  EVP_DigestInit_ex(md, EVP_sha256(), NULL);
  EVP_DigestUpdate(md, build_id, strlen(build_id));
  EVP_DigestUpdate(md, &opts->eval_flags, sizeof(opts->eval_flags));
  EVP_DigestUpdate(md, &opts->line_num, sizeof(opts->line_num));
  EVP_DigestUpdate(md, opts->filename ? opts->filename : "", strlen(opts->filename ? opts->filename : "") + 1);
  EVP_DigestUpdate(md, src, len);
  EVP_DigestFinal_ex(md, key, &n);
  EVP_MD_CTX_free(md);
  hex(key, 32, name);
  snprintf(path, sizeof(path), "%s/%s", dir, name);

  entry = read_entry(path, &elen);
  if (entry) {
    r = JS_ReadObject(ctx, entry + 36, elen - 36, JS_READ_OBJ_BYTECODE);
    free(entry);
    if (!JS_IsException(r)) {
      if (debug())
        fprintf(stderr, "[compile cache] %s: hit %s\n", opts->filename, name);
      return r;
    }
    if (debug()) {
      JSValue e = JS_GetException(ctx);
      const char *m = JS_ToCString(ctx, e);
      fprintf(stderr, "[compile cache] %s: unreadable entry (%s)\n", opts->filename, m ? m : "?");
      JS_FreeCString(ctx, m);
      JS_FreeValue(ctx, e);
    } else {
      JS_FreeValue(ctx, JS_GetException(ctx));
    }
    unlink(path);
  } else if (debug()) {
    fprintf(stderr, "[compile cache] %s: miss %s\n", opts->filename, name);
  }
  r = JS_Eval2(ctx, src, len, opts);
  if (JS_IsException(r))
    return r;
  bc = JS_WriteObject(ctx, &bclen, r, JS_WRITE_OBJ_BYTECODE);
  if (bc) {
    write_entry(path, bc, bclen);
    js_free(ctx, bc);
  } else {
    JS_FreeValue(ctx, JS_GetException(ctx));
  }
  return r;
}
