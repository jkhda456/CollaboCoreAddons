/*
 * What the JDK needs from the C library that the guest's musl leaves out on wasm: mmap and its
 * relatives, and fork/vfork/daemon. Linked into every program of the jdk add-on (tools/wasm-jdk-cc).
 *
 * The guest has no mmap: a process is one wasm linear memory, which only grows. So a mapping is
 * memory of our own:
 *  - Mappings are page-aligned (64 KiB, the wasm page and the guest's PAGESIZE) ranges carved out
 *    of memory from sbrk. Fresh memory from sbrk is zero, as anonymous mmap is, and costs nothing
 *    until it is touched. munmap() puts the range on a free list, from which later mappings are
 *    taken (zeroed again if they were written); the memory is never given back to the system.
 *  - MAP_FIXED re-initialises a range in place (zero, or the file's contents). HotSpot reserves its
 *    heap and metaspace with one mapping and commits and uncommits parts of it that way.
 *  - A file mapping is a copy of the file, read at mmap(). MAP_SHARED with PROT_WRITE writes the
 *    pages back on msync() and munmap(); changes are not seen by other processes or mappings.
 *  - mprotect, mlock and the like succeed and do nothing (there is no protection to change).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PG ((uintptr_t)65536)
#define ROUND_UP(x) (((x) + PG - 1) & ~(PG - 1))
#define ROUND_DOWN(x) ((x) & ~(PG - 1))

/* A range of our memory. `dirty`: it may hold something other than zeroes (it was writable, or
 * holds a file's contents); a clean range needs no memset to read as fresh anonymous memory. That
 * keeps reservations cheap: HotSpot reserves its heap (1 GiB by default) and commits only what it
 * uses, and memory nobody touched costs nothing. */
struct range {
  uintptr_t start, end;
  int fd;           /* MAP_SHARED + PROT_WRITE file mappings: where to write back (else -1) */
  off_t off;        /* the file offset of start */
  int dirty;
  struct range *next;
};

static struct range *maps;   /* live mappings, sorted by start, not overlapping */
static struct range *holes;  /* unmapped ranges of our memory, sorted by start */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct range *new_range(uintptr_t start, uintptr_t end, int fd, off_t off, int dirty) {
  struct range *r = malloc(sizeof *r);
  if (!r) abort();
  r->start = start; r->end = end; r->fd = fd; r->off = off; r->dirty = dirty; r->next = 0;
  return r;
}

/* Inserts r into a sorted list; for holes, merges neighbours alike (both clean or both dirty). */
static void insert(struct range **list, struct range *r, int merge) {
  struct range **p = list;
  while (*p && (*p)->start < r->start) p = &(*p)->next;
  r->next = *p;
  *p = r;
  if (!merge) return;
  for (struct range *q = *list; q && q->next; ) {
    if (q->end == q->next->start && q->dirty == q->next->dirty) {
      struct range *n = q->next;
      q->end = n->end;
      q->next = n->next;
      free(n);
    } else {
      q = q->next;
    }
  }
}

static void write_back(struct range *r, uintptr_t start, uintptr_t end) {
  if (r->fd < 0) return;
  if (start < r->start) start = r->start;
  if (end > r->end) end = r->end;
  /* only the file's existing length: a mapping past the end of the file does not extend it */
  off_t size = lseek(r->fd, 0, SEEK_END);
  off_t off = r->off + (off_t)(start - r->start);
  if (size >= 0 && off + (off_t)(end - start) > size) end = start + (size > off ? (uintptr_t)(size - off) : 0);
  while (start < end) {
    ssize_t n = pwrite(r->fd, (void *)start, end - start, off);
    if (n <= 0) break;
    start += n; off += n;
  }
}

/* Removes [start, end) from a list, splitting ranges at the edges. For maps, the removed parts
 * are written back (shared file mappings) and, when `freed` is given, added to it. */
static void cut(struct range **list, uintptr_t start, uintptr_t end, int is_maps, struct range **freed) {
  struct range **p = list;
  while (*p) {
    struct range *r = *p;
    if (r->end <= start || r->start >= end) { p = &r->next; continue; }
    uintptr_t s = r->start > start ? r->start : start;
    uintptr_t e = r->end < end ? r->end : end;
    if (is_maps) write_back(r, s, e);
    if (freed) insert(freed, new_range(s, e, -1, 0, r->dirty), 1);
    if (r->start < s && r->end > e) {          /* split in two */
      struct range *t = new_range(e, r->end, -1, r->off + (off_t)(e - r->start), r->dirty);
      if (r->fd >= 0) t->fd = fcntl(r->fd, F_DUPFD_CLOEXEC, 0);
      r->end = s;
      t->next = r->next;
      r->next = t;
      p = &t->next;
    } else if (r->start < s) {                 /* keep the head */
      r->end = s;
      p = &r->next;
    } else if (r->end > e) {                   /* keep the tail */
      r->off += (off_t)(e - r->start);
      r->start = e;
      p = &r->next;
    } else {                                   /* all of it */
      *p = r->next;
      if (r->fd >= 0) close(r->fd);
      free(r);
    }
  }
}

/* Zeroes what of [start, end) may not be zero: the dirty parts of maps and holes there, and what
 * neither covers (memory that is not ours to know). */
static void make_zero(uintptr_t start, uintptr_t end) {
  uintptr_t at = start;
  struct range *m = maps, *h = holes;
  while (at < end) {
    while (m && m->end <= at) m = m->next;
    while (h && h->end <= at) h = h->next;
    struct range *r = 0;
    if (m && m->start <= at) r = m;
    else if (h && h->start <= at) r = h;
    if (r) {
      uintptr_t e = r->end < end ? r->end : end;
      if (r->dirty) memset((void *)at, 0, e - at);
      at = e;
    } else {
      uintptr_t e = end;
      if (m && m->start < e) e = m->start;
      if (h && h->start < e) e = h->start;
      memset((void *)at, 0, e - at);
      at = e;
    }
  }
}

static int contained(struct range *list, uintptr_t start, uintptr_t end) {
  for (; list; list = list->next)
    if (list->start <= start && end <= list->end) return 1;
  return 0;
}

static uintptr_t arena_top;  /* the end of memory after our last sbrk(): ours up to there */

/* n bytes of new memory at the top, contiguous from the current end of memory (a block up to
 * 4 GiB, in steps sbrk() can take). Null when there is no more, or when another thread took
 * memory in between (what was taken is then lost). */
static uintptr_t grab(uintptr_t from, size_t n) {
  size_t got = 0;
  while (got < n) {
    size_t step = n - got > ((size_t)1 << 30) ? ((size_t)1 << 30) : n - got;
    void *p = sbrk((intptr_t)step);
    if (p == (void *)-1 || (uintptr_t)p != from + got) return 0;
    got += step;
  }
  arena_top = from + n;
  return from;
}

/* A free range of len bytes, zero: from the holes (first fit), else from sbrk. */
static uintptr_t take(size_t len) {
  struct range *h;
  for (h = holes; h; h = h->next) {
    if (h->end - h->start >= len) {
      uintptr_t a = h->start;
      if (h->dirty) memset((void *)a, 0, len);
      cut(&holes, a, a + len, 0, 0);
      return a;
    }
  }
  /* The highest hole may end where the memory does: then only the rest is new. (HotSpot reserves,
   * releases and reserves again a bit bigger, to align its heap.) Only when nothing else (malloc)
   * took memory since our last sbrk(): the memory between the hole and the end is then ours too. */
  uintptr_t cur = (uintptr_t)sbrk(0);
  for (h = holes; h && h->next; h = h->next) {}
  if (h && cur == arena_top && h->end == ROUND_DOWN(cur) && h->start + len > cur) {
    uintptr_t a = h->start;
    if (a + len < a || !grab(cur, a + len - cur)) return 0;
    if (h->dirty) memset((void *)a, 0, h->end - a);
    cut(&holes, a, h->end, 0, 0);
    return a;
  }
  if (cur + len + PG < cur) return 0;
  uintptr_t p0 = grab(cur, len + PG);
  if (!p0) return 0;
  uintptr_t a = ROUND_UP(p0);
  uintptr_t top = p0 + len + PG;
  /* the slack after the range stays usable for later mappings */
  if (a + len < top && ROUND_UP(a + len) < ROUND_DOWN(top))
    insert(&holes, new_range(ROUND_UP(a + len), ROUND_DOWN(top), -1, 0, 0), 1);
  return a;
}

static int fill(uintptr_t a, size_t len, int fd, off_t off) {
  size_t done = 0;
  while (done < len) {
    ssize_t n = pread(fd, (char *)a + done, len - done, off + (off_t)done);
    if (n < 0) { if (errno == EINTR) continue; return -1; }
    if (n == 0) break;
    done += n;
  }
  memset((char *)a + done, 0, len - done);
  return 0;
}

/* JDK_WASM_MMAP_TRACE=1: each mmap and munmap on stderr */
static int trace = -1;
static void trace_call(const char *what, uintptr_t a, size_t len, int flags, int fd) {
  if (trace < 0) trace = getenv("JDK_WASM_MMAP_TRACE") != 0;
  if (trace)
    fprintf(stderr, "[wasm-mmap] %s %#lx +%#zx flags %#x fd %d (brk %p)\n", what, (unsigned long)a,
            len, flags, fd, sbrk(0));
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  if (!len || (off & (PG - 1)) || ((flags & MAP_FIXED) && ((uintptr_t)addr & (PG - 1)))) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  if (len > SIZE_MAX - 2 * PG) { errno = ENOMEM; return MAP_FAILED; }
  int anon = (flags & MAP_ANONYMOUS) || fd < 0;
  if (anon) fd = -1;
  size_t size = ROUND_UP(len);
  int wfd = -1;
  if (!anon && (flags & MAP_TYPE) != MAP_PRIVATE && (prot & PROT_WRITE)) {
    wfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (wfd < 0) return MAP_FAILED;
  }
  pthread_mutex_lock(&lock);
  uintptr_t a = 0;
  if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
    a = (uintptr_t)addr;
    if ((flags & MAP_FIXED_NOREPLACE) && !(flags & MAP_FIXED) && !contained(holes, a, a + size)) {
      pthread_mutex_unlock(&lock);
      if (wfd >= 0) close(wfd);
      errno = EEXIST;
      return MAP_FAILED;
    }
    if (anon) make_zero(a, a + size);
    cut(&maps, a, a + size, 1, 0);
    cut(&holes, a, a + size, 0, 0);
  } else if (addr && !((uintptr_t)addr & (PG - 1)) && contained(holes, (uintptr_t)addr, (uintptr_t)addr + size)) {
    a = (uintptr_t)addr;  /* the hint is free: take it */
    make_zero(a, a + size);
    cut(&holes, a, a + size, 0, 0);
  } else {
    a = take(size);
    if (!a) {
      pthread_mutex_unlock(&lock);
      if (wfd >= 0) close(wfd);
      errno = ENOMEM;
      return MAP_FAILED;
    }
  }
  insert(&maps, new_range(a, a + size, wfd, off, !anon || (prot & PROT_WRITE)), 0);
  pthread_mutex_unlock(&lock);
  trace_call("mmap", a, size, flags, fd);
  if (!anon && fill(a, size, fd, off) < 0) {
    int e = errno;
    munmap((void *)a, size);
    errno = e;
    return MAP_FAILED;
  }
  return (void *)a;
}

int munmap(void *addr, size_t len) {
  uintptr_t a = (uintptr_t)addr;
  if ((a & (PG - 1)) || !len) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&lock);
  cut(&maps, a, a + ROUND_UP(len), 1, &holes);
  pthread_mutex_unlock(&lock);
  trace_call("munmap", a, ROUND_UP(len), 0, -1);
  return 0;
}

int msync(void *addr, size_t len, int flags) {
  (void)flags;
  uintptr_t a = ROUND_DOWN((uintptr_t)addr), e = (uintptr_t)addr + len;
  pthread_mutex_lock(&lock);
  for (struct range *r = maps; r; r = r->next)
    if (r->start < e && r->end > a) write_back(r, a, e);
  pthread_mutex_unlock(&lock);
  return 0;
}

int madvise(void *addr, size_t len, int advice) {
  if (advice == MADV_DONTNEED) {
    /* private anonymous pages read as zero afterwards */
    uintptr_t a = (uintptr_t)addr;
    pthread_mutex_lock(&lock);
    if (contained(maps, a, a + len)) make_zero(a, a + len);
    pthread_mutex_unlock(&lock);
  }
  return 0;
}

/* There is no protection to change; making memory writable only means it may not stay zero. */
int mprotect(void *addr, size_t len, int prot) {
  if (prot & PROT_WRITE) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    pthread_mutex_lock(&lock);
    for (struct range *r = maps; r; r = r->next)
      if (r->start < e && r->end > a) r->dirty = 1;
    pthread_mutex_unlock(&lock);
  }
  return 0;
}

int posix_madvise(void *addr, size_t len, int advice) { (void)addr; (void)len; (void)advice; return 0; }
int mincore(void *addr, size_t len, unsigned char *vec) {
  (void)addr;
  memset(vec, 1, (len + PG - 1) / PG);
  return 0;
}
int mlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int munlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int mlockall(int flags) { (void)flags; return 0; }
int munlockall(void) { return 0; }
void *mremap(void *old, size_t old_len, size_t new_len, int flags, ...) {
  (void)old; (void)old_len; (void)new_len; (void)flags;
  errno = ENOMEM;
  return MAP_FAILED;
}

pid_t fork(void) { errno = ENOSYS; return -1; }
pid_t vfork(void) { errno = ENOSYS; return -1; }
int daemon(int nochdir, int noclose) { (void)nochdir; (void)noclose; errno = ENOSYS; return -1; }

/*
 * dlopen/dlsym. Nothing can be loaded in the guest, so the JDK's native libraries are linked into
 * the program (the JDK's static build) and found by name in a table of the program's own functions:
 * __jdk_wasm_symbols, which tools/gen-symtab.py writes at link time (JNI functions, JNI_OnLoad_<lib>,
 * the JVM's entry points, and a few C library functions the JDK looks up). dlopen(NULL) is the
 * program itself; dlopen of a file fails, as it would for a library that is not there.
 */
#include <dlfcn.h>

struct jdk_wasm_symbol { const char *name; void *addr; };
extern const struct jdk_wasm_symbol __jdk_wasm_symbols[] __attribute__((weak));
extern const unsigned __jdk_wasm_symbol_count __attribute__((weak));

#define SELF ((void *)&__jdk_wasm_self)
static const char __jdk_wasm_self = 0;
void __dl_seterr(const char *, ...);   /* the C library's: dlerror() returns it */

void *dlopen(const char *file, int mode) {
  (void)mode;
  if (!file) return SELF;
  __dl_seterr("%s: cannot load a shared library in the WebAssembly guest "
              "(the JDK's native libraries are linked into the program)", file);
  return 0;
}

int dlclose(void *handle) { (void)handle; return 0; }

void *dlsym(void *restrict handle, const char *restrict name) {
  (void)handle;
  if (&__jdk_wasm_symbol_count) {
    unsigned lo = 0, hi = __jdk_wasm_symbol_count;
    while (lo < hi) {
      unsigned mid = (lo + hi) / 2;
      int c = strcmp(__jdk_wasm_symbols[mid].name, name);
      if (c == 0) return __jdk_wasm_symbols[mid].addr;
      if (c < 0) lo = mid + 1; else hi = mid;
    }
  }
  __dl_seterr("symbol not found: %s", name);
  return 0;
}

/* Every function is in the program itself: dladdr() names the program (as on Linux for a function
 * of the executable); the JVM finds its home from it (<java.home>/bin/java). */
static char exe_path[4096];
static pthread_once_t exe_once = PTHREAD_ONCE_INIT;

static void read_exe_path(void) {
  ssize_t n = readlink("/proc/self/exe", exe_path, sizeof exe_path - 1);
  exe_path[n > 0 ? n : 0] = 0;
}

int dladdr(const void *addr, Dl_info *info) {
  (void)addr;
  memset(info, 0, sizeof *info);
  pthread_once(&exe_once, read_exe_path);
  if (!exe_path[0]) return 0;
  info->dli_fname = exe_path;
  return 1;
}

/*
 * pthread_getattr_np() of the main thread: the guest's musl has no way to find its stack and traps
 * (a_crash). HotSpot asks for it on the thread that runs a signal handler or attaches. The main
 * thread's stack is the one the linker made: [__stack_low, __stack_high). (tools/wasm-jdk-cc
 * links programs with --wrap=pthread_getattr_np.)
 */
extern char __stack_low[], __stack_high[];
static pthread_t main_thread;
__attribute__((constructor)) static void note_main_thread(void) { main_thread = pthread_self(); }

int __real_pthread_getattr_np(pthread_t, pthread_attr_t *);
int __wrap_pthread_getattr_np(pthread_t t, pthread_attr_t *a) {
  if (!main_thread || !pthread_equal(t, main_thread)) return __real_pthread_getattr_np(t, a);
  pthread_attr_init(a);
  return pthread_attr_setstack(a, __stack_low, (size_t)(__stack_high - __stack_low));
}

/* JDK_WASM_TRAP_SIGNAL=1 (debugging): SIGSYS makes the thread that takes it trap, so that the
 * engine prints the wasm stack of where that thread was (send it with tgkill to one thread). */
#include <signal.h>
static void trap_handler(int sig) { (void)sig; __builtin_trap(); }
__attribute__((constructor)) static void install_trap_signal(void) {
  if (getenv("JDK_WASM_TRAP_SIGNAL")) signal(SIGSYS, trap_handler);
}
