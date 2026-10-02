/* The global Intl object.  QuickJS has none; this one follows V8 with ICU
 * for the en-US locale (other locales are accepted and reported, and format
 * like en-US).  The segmenters (UAX #29 grapheme / word / sentence breaks)
 * are C over the Unicode tables in unicode_tables.inc; the formatters are
 * JavaScript (intl.js, embedded here).
 *
 * Installing is lazy: `Intl` is an accessor on the global object and the
 * locale methods of Date, Number, BigInt and String are small C forwarders;
 * the first use evaluates intl.js in that context and the accessor turns
 * into a plain data property. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "node.h"

#include "unicode_tables.inc"

static const char intl_js[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "intl.js"
#pragma clang diagnostic pop
  , 0
};

/* the CLDR data intl.js formats with (tools/gen-intl-data.js) */
static const char intl_data[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "intl_data.json"
#pragma clang diagnostic pop
  , 0
};

/* ---------------------------------------------------------------------- */
/* properties */

enum { GCB_Other, GCB_CR, GCB_LF, GCB_Control, GCB_Extend, GCB_ZWJ, GCB_RI, GCB_Prepend,
       GCB_SpacingMark, GCB_L, GCB_V, GCB_T, GCB_LV, GCB_LVT };
enum { WB_Other, WB_CR, WB_LF, WB_Newline, WB_Extend, WB_ZWJ, WB_RI, WB_Format, WB_Katakana,
       WB_Hebrew_Letter, WB_ALetter, WB_Single_Quote, WB_Double_Quote, WB_MidNumLet,
       WB_MidLetter, WB_MidNum, WB_Numeric, WB_ExtendNumLet, WB_WSegSpace };
enum { SB_Other, SB_CR, SB_LF, SB_Extend, SB_Sep, SB_Format, SB_Sp, SB_Lower, SB_Upper,
       SB_OLetter, SB_Numeric, SB_ATerm, SB_SContinue, SB_STerm, SB_Close };
enum { INCB_None, INCB_Linker, INCB_Consonant, INCB_Extend };
enum { DICT_None, DICT_Ideo, DICT_SA, DICT_Hangul };

static inline uint32_t uprops(uint32_t c) {
  if (c >= 0x110000)
    return 0;
  return uprops_values[uprops_stage2[(uprops_stage1[c >> UPROPS_SHIFT] << UPROPS_SHIFT) +
                                     (c & ((1 << UPROPS_SHIFT) - 1))]];
}
#define P_GCB(p) ((p) & 15)
#define P_EXT(p) (((p) >> 4) & 1)
#define P_INCB(p) (((p) >> 5) & 3)
#define P_WB(p) (((p) >> 7) & 31)
#define P_SB(p) (((p) >> 12) & 15)
#define P_DICT(p) (((p) >> 16) & 3)

/* ---------------------------------------------------------------------- */
/* text: a flat JS string, Latin-1 or UTF-16 */

typedef struct {
  const uint8_t *s8;
  const uint16_t *s16;
  uint32_t len;
} Text;

static inline uint32_t text_cp(const Text *t, uint32_t i, uint32_t *next) {
  uint32_t c;
  if (t->s8) {
    *next = i + 1;
    return t->s8[i];
  }
  c = t->s16[i];
  if (c >= 0xd800 && c < 0xdc00 && i + 1 < t->len) {
    uint32_t d = t->s16[i + 1];
    if (d >= 0xdc00 && d < 0xe000) {
      *next = i + 2;
      return 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
    }
  }
  *next = i + 1;
  return c;
}

/* the code point ending at i (i > 0) and where it starts */
static inline uint32_t text_cp_before(const Text *t, uint32_t i, uint32_t *start) {
  uint32_t c;
  if (t->s8) {
    *start = i - 1;
    return t->s8[i - 1];
  }
  c = t->s16[i - 1];
  if (c >= 0xdc00 && c < 0xe000 && i >= 2) {
    uint32_t h = t->s16[i - 2];
    if (h >= 0xd800 && h < 0xdc00) {
      *start = i - 2;
      return 0x10000 + ((h - 0xd800) << 10) + (c - 0xdc00);
    }
  }
  *start = i - 1;
  return c;
}

/* ---------------------------------------------------------------------- */
/* grapheme clusters (UAX #29 GB rules, Unicode 17) */

static uint32_t grapheme_next(const Text *t, uint32_t i) {
  uint32_t j, c = text_cp(t, i, &j), p = uprops(c), g = P_GCB(p);
  int emoji = P_EXT(p) ? 1 : 0;              /* 1: ExtPict Extend*, 2: ... ZWJ */
  int incb = P_INCB(p) == INCB_Consonant ? 1 : 0; /* 1: consonant, 2: + linker */
  int ri = g == GCB_RI ? 1 : 0;
  /* ASCII fast path: printable ASCII followed by printable ASCII breaks */
  if (c >= 0x20 && c < 0x7f && j < t->len) {
    uint32_t d = t->s8 ? t->s8[j] : t->s16[j];
    if (d >= 0x20 && d < 0x7f)
      return j;
  }
  while (j < t->len) {
    uint32_t k, d = text_cp(t, j, &k), q = uprops(d), h = P_GCB(q), qi = P_INCB(q);
    bool join;
    if (g == GCB_CR && h == GCB_LF)
      join = true;
    else if (g == GCB_Control || g == GCB_CR || g == GCB_LF)
      join = false;
    else if (h == GCB_Control || h == GCB_CR || h == GCB_LF)
      join = false;
    else if (g == GCB_L && (h == GCB_L || h == GCB_V || h == GCB_LV || h == GCB_LVT))
      join = true;
    else if ((g == GCB_LV || g == GCB_V) && (h == GCB_V || h == GCB_T))
      join = true;
    else if ((g == GCB_LVT || g == GCB_T) && h == GCB_T)
      join = true;
    else if (h == GCB_Extend || h == GCB_ZWJ || h == GCB_SpacingMark || g == GCB_Prepend)
      join = true;
    else if (qi == INCB_Consonant && incb == 2)
      join = true;
    else if (g == GCB_ZWJ && P_EXT(q) && emoji == 2)
      join = true;
    else if (g == GCB_RI && h == GCB_RI && (ri & 1))
      join = true;
    else
      join = false;
    if (!join)
      break;
    /* states after d */
    if (P_EXT(q))
      emoji = 1;
    else if (h == GCB_Extend && emoji == 1)
      emoji = 1;
    else if (h == GCB_ZWJ && emoji == 1)
      emoji = 2;
    else
      emoji = 0;
    if (qi == INCB_Consonant)
      incb = 1;
    else if (qi == INCB_Linker && incb)
      incb = 2;
    else if (qi == INCB_Extend)
      ;
    else
      incb = 0;
    ri = h == GCB_RI ? ri + 1 : 0;
    g = h;
    j = k;
  }
  return j;
}

/* ---------------------------------------------------------------------- */
/* words (UAX #29 WB rules with ICU's changes; its dictionaries replaced
   by: Han and Hiragana one character a word, Thai/Lao/Khmer/Myanmar letters
   ALetter, runs of Hangul syllables a word) */

#define IS_AH(w) ((w) == WB_ALetter || (w) == WB_Hebrew_Letter)
#define IS_MIDLETQ(w) ((w) == WB_MidLetter || (w) == WB_MidNumLet || (w) == WB_Single_Quote)
#define IS_MIDNUMQ(w) ((w) == WB_MidNum || (w) == WB_MidNumLet || (w) == WB_Single_Quote)
#define IS_IGNORED(w) ((w) == WB_Extend || (w) == WB_Format || (w) == WB_ZWJ)


/* the word class used for rules: ICU takes the letters of the scripts it
   has dictionaries for (Thai, Lao, Khmer, Myanmar) as ALetter */
static inline int wb_class(uint32_t p) {
  int w = P_WB(p), d = P_DICT(p);
  if (d == DICT_SA)
    return w == WB_Other ? WB_ALetter : w;
  /* Han, Hiragana and Hangul syllables join nothing by the rules */
  return d == DICT_None ? w : WB_Other;
}

/* the word class of the first character at or after j that WB4 does not
   skip; -1 at the end */
static int wb_peek(const Text *t, uint32_t j) {
  while (j < t->len) {
    uint32_t k, c = text_cp(t, j, &k);
    int w = wb_class(uprops(c));
    if (!IS_IGNORED(w))
      return w;
    j = k;
  }
  return -1;
}

static uint32_t word_next(const Text *t, uint32_t i, bool *word_like) {
  uint32_t j, c = text_cp(t, i, &j), p = uprops(c);
  int w = wb_class(p), prev = w, prevprev = -1, raw = w, ri = w == WB_RI ? 1 : 0;
  int dict = P_DICT(p);
  int letters = 0, enl = 0, other = 0;
  bool trailing = false; /* marks after the last character the rules saw */
#define NOTE(cls, dc)                                                                    \
  do {                                                                                   \
    if (IS_AH(cls) || (cls) == WB_Numeric || (cls) == WB_Katakana || (dc))               \
      letters++;                                                                         \
    else if ((cls) == WB_ExtendNumLet)                                                   \
      enl++;                                                                             \
    else if (!IS_IGNORED(cls))                                                           \
      other++;                                                                           \
  } while (0)
  NOTE(w, dict);
  if (w == WB_CR) {
    uint32_t k;
    if (j < t->len && P_WB(uprops(text_cp(t, j, &k))) == WB_LF)
      j = k;
    *word_like = false;
    return j;
  }
  if (w == WB_LF || w == WB_Newline) {
    *word_like = false;
    return j;
  }
  if (dict == DICT_Hangul) {
    /* a run of Hangul syllables is a word; marks after it end it, and
       then (as in ICU) it is not word-like */
    bool marks = false;
    while (j < t->len) {
      uint32_t k, d = text_cp(t, j, &k), q = uprops(d);
      if (!marks && P_DICT(q) == DICT_Hangul)
        ;
      else if (IS_IGNORED(P_WB(q)))
        marks = true;
      else
        break;
      j = k;
    }
    *word_like = !marks;
    return j;
  }
  if (dict == DICT_Ideo) {
    /* one ideograph (with what extends it) a word */
    while (j < t->len) {
      uint32_t k, d = text_cp(t, j, &k), q = uprops(d);
      if (!IS_IGNORED(P_WB(q)))
        break;
      j = k;
    }
    *word_like = true;
    return j;
  }
  while (j < t->len) {
    uint32_t k, d = text_cp(t, j, &k), q = uprops(d);
    int x = wb_class(q), qd = P_DICT(q);
    bool join = false, ignored = false;
    if (x == WB_CR || x == WB_LF || x == WB_Newline)
      break;
    if (raw == WB_ZWJ && P_EXT(q))
      join = true; /* WB3c */
    else if (raw == WB_WSegSpace && x == WB_WSegSpace)
      join = true; /* WB3d */
    else if (IS_IGNORED(x)) {
      join = true; /* WB4 */
      ignored = true;
    } else if (qd == DICT_Ideo || qd == DICT_Hangul)
      join = false;
    else if (IS_AH(prev) && IS_AH(x))
      join = true; /* WB5 */
    else if (IS_AH(prev) && IS_MIDLETQ(x) && IS_AH(wb_peek(t, k)))
      join = true; /* WB6 */
    else if (prevprev >= 0 && IS_AH(prevprev) && IS_MIDLETQ(prev) && IS_AH(x))
      join = true; /* WB7 */
    else if (prev == WB_Hebrew_Letter && x == WB_Single_Quote)
      join = true; /* WB7a */
    else if (prev == WB_Hebrew_Letter && x == WB_Double_Quote &&
             wb_peek(t, k) == WB_Hebrew_Letter)
      join = true; /* WB7b */
    else if (prevprev == WB_Hebrew_Letter && prev == WB_Double_Quote && x == WB_Hebrew_Letter)
      join = true; /* WB7c */
    else if ((prev == WB_Numeric || IS_AH(prev)) && x == WB_Numeric)
      join = true; /* WB8, WB9 */
    else if (prev == WB_Numeric && IS_AH(x))
      join = true; /* WB10 */
    else if (prevprev == WB_Numeric && IS_MIDNUMQ(prev) && x == WB_Numeric)
      join = true; /* WB11 */
    else if (prev == WB_Numeric && IS_MIDNUMQ(x) && wb_peek(t, k) == WB_Numeric)
      join = true; /* WB12 */
    else if (prev == WB_Katakana && x == WB_Katakana)
      join = true; /* WB13 */
    else if ((IS_AH(prev) || prev == WB_Numeric || prev == WB_Katakana ||
              prev == WB_ExtendNumLet) && x == WB_ExtendNumLet)
      join = true; /* WB13a */
    else if (prev == WB_ExtendNumLet && (IS_AH(x) || x == WB_Numeric || x == WB_Katakana))
      join = true; /* WB13b */
    else if (prev == WB_RI && x == WB_RI && (ri & 1))
      join = true; /* WB15, WB16 */
    if (!join)
      break;
    if (!ignored) {
      prevprev = prev;
      prev = x;
      ri = x == WB_RI ? ri + 1 : 0;
      NOTE(x, qd);
    }
    trailing = ignored;
    raw = x;
    j = k;
  }
#undef NOTE
  /* ICU's rule statuses: letters, numbers, kana, ideographs; a run of two or
     more connector punctuation also counts as a word; not when it ends in
     connector punctuation with marks */
  *word_like = (letters > 0 || (enl >= 2 && other == 0)) && !(prev == WB_ExtendNumLet && trailing);
  return j;
}

/* ---------------------------------------------------------------------- */
/* sentences (UAX #29 SB rules) */

#define IS_PARASEP(s) ((s) == SB_Sep || (s) == SB_CR || (s) == SB_LF)
#define IS_SATERM(s) ((s) == SB_STerm || (s) == SB_ATerm)

static inline int sb_at(const Text *t, uint32_t i, uint32_t *next) {
  return P_SB(uprops(text_cp(t, i, next)));
}

/* the sentence class before position i skipping Extend/Format; -1 at sot.
   *start receives its position */
static int sb_before(const Text *t, uint32_t i, uint32_t *start) {
  while (i > 0) {
    uint32_t s, c = text_cp_before(t, i, &s);
    int x = P_SB(uprops(c));
    if (x != SB_Extend && x != SB_Format) {
      *start = s;
      return x;
    }
    i = s;
  }
  return -1;
}

static bool sb_is_break(const Text *t, uint32_t pos) {
  uint32_t s, n, c = text_cp_before(t, pos, &s);
  int prevraw = P_SB(uprops(c)), cur = sb_at(t, pos, &n), p, x;
  uint32_t k, k2;
  int sp = 0;
  if (prevraw == SB_CR && cur == SB_LF)
    return false;               /* SB3 */
  if (IS_PARASEP(prevraw))
    return true;                /* SB4 */
  if (cur == SB_Extend || cur == SB_Format)
    return false;               /* SB5 */
  p = sb_before(t, pos, &k);
  if (p == SB_ATerm && cur == SB_Numeric)
    return false;               /* SB6 */
  if (p == SB_ATerm && cur == SB_Upper) {
    int pp = sb_before(t, k, &k2);
    if (pp == SB_Upper || pp == SB_Lower)
      return false;             /* SB7 */
  }
  /* SATerm Close* Sp* before pos? */
  x = p;
  k2 = k;
  while (x == SB_Sp) {
    sp++;
    x = sb_before(t, k2, &k2);
  }
  while (x == SB_Close)
    x = sb_before(t, k2, &k2);
  if (!IS_SATERM(x))
    return false;               /* SB998 */
  if (x == SB_ATerm) {
    /* SB8: ATerm Close* Sp* × (¬(OLetter|Upper|Lower|ParaSep|SATerm))* Lower */
    uint32_t j = pos;
    while (j < t->len) {
      uint32_t jn;
      int y = sb_at(t, j, &jn);
      if (y == SB_Lower)
        return false;
      if (y == SB_OLetter || y == SB_Upper || IS_PARASEP(y) || IS_SATERM(y))
        break;
      j = jn;
    }
  }
  if (cur == SB_SContinue || IS_SATERM(cur))
    return false;               /* SB8a */
  if (sp == 0 && (cur == SB_Close || cur == SB_Sp || IS_PARASEP(cur)))
    return false;               /* SB9 */
  if (cur == SB_Sp || IS_PARASEP(cur))
    return false;               /* SB10 */
  return true;                  /* SB11 */
}

static uint32_t sentence_next(const Text *t, uint32_t i) {
  uint32_t j;
  text_cp(t, i, &j);
  while (j < t->len && !sb_is_break(t, j))
    text_cp(t, j, &j);
  return j;
}

/* ---------------------------------------------------------------------- */
/* native helpers for intl.js */

enum { GRAN_GRAPHEME, GRAN_WORD, GRAN_SENTENCE };

/* the segment iterator: a class (ids are process-wide, registered in each
   runtime), with the string and position in C */
typedef struct {
  JSValue str; /* flat */
  uint32_t pos;
  int gran;
} SegIter;

static JSClassID seg_iter_class_id;

static void seg_iter_finalizer(JSRuntime *rt, JSValueConst val) {
  SegIter *it = JS_GetOpaque(val, seg_iter_class_id);
  if (it) {
    JS_FreeValueRT(rt, it->str);
    js_free_rt(rt, it);
  }
}

static void seg_iter_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
  SegIter *it = JS_GetOpaque(val, seg_iter_class_id);
  if (it)
    JS_MarkValue(rt, it->str, mark_func);
}

/* atoms of the segment data objects, for the runtime of this thread that
   last used them (refreshed when another runtime, or a new one at the same
   address - its class is not registered yet - comes along) */
static _Thread_local JSRuntime *atoms_rt;
static _Thread_local JSAtom atom_segment, atom_index, atom_input, atom_is_word_like, atom_value, atom_done;

static void ensure_runtime(JSContext *ctx) {
  JSRuntime *rt = JS_GetRuntime(ctx);
  bool fresh = false;
  JS_NewClassID(rt, &seg_iter_class_id);
  if (!JS_IsRegisteredClass(rt, seg_iter_class_id)) {
    JSClassDef def = { .class_name = "Segmenter String Iterator", .finalizer = seg_iter_finalizer,
                       .gc_mark = seg_iter_mark };
    JS_NewClass(rt, seg_iter_class_id, &def);
    fresh = true;
  }
  if (atoms_rt == rt && !fresh)
    return;
  atom_segment = JS_NewAtom(ctx, "segment");
  atom_index = JS_NewAtom(ctx, "index");
  atom_input = JS_NewAtom(ctx, "input");
  atom_is_word_like = JS_NewAtom(ctx, "isWordLike");
  atom_value = JS_NewAtom(ctx, "value");
  atom_done = JS_NewAtom(ctx, "done");
  atoms_rt = rt;
}

/* a flat string's text; *val may be replaced by its flat version */
static bool get_text(JSContext *ctx, JSValue *val, Text *t) {
  uint32_t len;
  int wide;
  const void *d = JS_NodeStringData(ctx, val, &len, &wide);
  if (!d)
    return false;
  t->len = len;
  t->s8 = wide ? NULL : d;
  t->s16 = wide ? d : NULL;
  return true;
}

static uint32_t seg_next(const Text *t, uint32_t i, int gran, bool *word_like) {
  *word_like = false;
  switch (gran) {
  case GRAN_WORD: return word_next(t, i, word_like);
  case GRAN_SENTENCE: return sentence_next(t, i);
  default: return grapheme_next(t, i);
  }
}

static JSValue make_segment(JSContext *ctx, const Text *t, JSValueConst input, uint32_t a,
                            uint32_t b, int gran, bool word_like) {
  JSValue o = JS_NewObject(ctx), s;
  if (t->s8)
    s = JS_NodeNewStringLatin1(ctx, t->s8 + a, b - a);
  else
    s = JS_NodeNewStringUTF16(ctx, t->s16 + a, b - a);
  ensure_runtime(ctx);
  JS_DefinePropertyValue(ctx, o, atom_segment, s, JS_PROP_C_W_E);
  JS_DefinePropertyValue(ctx, o, atom_index, JS_NewUint32(ctx, a), JS_PROP_C_W_E);
  JS_DefinePropertyValue(ctx, o, atom_input, JS_DupValue(ctx, input), JS_PROP_C_W_E);
  if (gran == GRAN_WORD)
    JS_DefinePropertyValue(ctx, o, atom_is_word_like, JS_NewBool(ctx, word_like), JS_PROP_C_W_E);
  return o;
}

/* segment(str, pos, gran): the segment data object starting at pos, or
   undefined at the end */
static JSValue n_segment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSValue str = JS_DupValue(ctx, argv[0]), r;
  Text t;
  uint32_t pos, end;
  int32_t gran;
  bool wl;
  if (!JS_IsString(str) || !get_text(ctx, &str, &t)) {
    JS_FreeValue(ctx, str);
    return JS_ThrowTypeError(ctx, "segment: not a string");
  }
  JS_ToUint32(ctx, &pos, argv[1]);
  JS_ToInt32(ctx, &gran, argv[2]);
  if (pos >= t.len) {
    JS_FreeValue(ctx, str);
    return JS_UNDEFINED;
  }
  end = seg_next(&t, pos, gran, &wl);
  r = make_segment(ctx, &t, str, pos, end, gran, wl);
  JS_FreeValue(ctx, str);
  return r;
}

/* iterator(str, gran, proto): a segment iterator */
static JSValue n_iterator(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSValue str = JS_DupValue(ctx, argv[0]), obj;
  Text t;
  int32_t gran;
  SegIter *it;
  if (!JS_IsString(str) || !get_text(ctx, &str, &t)) {
    JS_FreeValue(ctx, str);
    return JS_ThrowTypeError(ctx, "iterator: not a string");
  }
  JS_ToInt32(ctx, &gran, argv[1]);
  ensure_runtime(ctx);
  obj = JS_NewObjectProtoClass(ctx, argv[2], seg_iter_class_id);
  it = js_mallocz(ctx, sizeof(*it));
  if (JS_IsException(obj) || !it) {
    js_free(ctx, it);
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, str);
    return JS_EXCEPTION;
  }
  it->str = str;
  it->gran = gran;
  JS_SetOpaque(obj, it);
  return obj;
}

/* %SegmentIterator.prototype%.next */
static JSValue n_iterator_next(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  SegIter *it = seg_iter_class_id ? JS_GetOpaque(this_val, seg_iter_class_id) : NULL;
  JSValue res, value;
  Text t;
  uint32_t end;
  bool wl;
  if (!it)
    return JS_ThrowTypeError(ctx, "Method %%SegmentIterator.prototype%%.next called on incompatible receiver");
  ensure_runtime(ctx);
  res = JS_NewObject(ctx);
  if (JS_IsException(res))
    return res;
  if (!get_text(ctx, &it->str, &t) || it->pos >= t.len) {
    it->pos = UINT32_MAX;
    value = JS_UNDEFINED;
  } else {
    end = seg_next(&t, it->pos, it->gran, &wl);
    value = make_segment(ctx, &t, it->str, it->pos, end, it->gran, wl);
    it->pos = end;
  }
  JS_DefinePropertyValue(ctx, res, atom_value, value, JS_PROP_C_W_E);
  JS_DefinePropertyValue(ctx, res, atom_done, JS_NewBool(ctx, JS_IsUndefined(value)), JS_PROP_C_W_E);
  return res;
}

/* containing(str, index, gran): the segment containing index, or undefined */
static JSValue n_containing(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  JSValue str = JS_DupValue(ctx, argv[0]), r = JS_UNDEFINED;
  Text t;
  double d;
  uint32_t pos = 0, end, idx;
  int32_t gran;
  bool wl;
  if (!JS_IsString(str) || !get_text(ctx, &str, &t)) {
    JS_FreeValue(ctx, str);
    return JS_ThrowTypeError(ctx, "containing: not a string");
  }
  JS_ToFloat64(ctx, &d, argv[1]);
  JS_ToInt32(ctx, &gran, argv[2]);
  if (!(d >= 0) || d >= t.len) {
    JS_FreeValue(ctx, str);
    return JS_UNDEFINED;
  }
  idx = (uint32_t)d;
  while (pos < t.len) {
    end = seg_next(&t, pos, gran, &wl);
    if (idx < end) {
      r = make_segment(ctx, &t, str, pos, end, gran, wl);
      break;
    }
    pos = end;
  }
  JS_FreeValue(ctx, str);
  return r;
}

/* boundaries(str, gran): every segment end as an array (tests, counting) */
static JSValue n_count(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  JSValue str = JS_DupValue(ctx, argv[0]);
  Text t;
  uint32_t pos = 0, n = 0;
  int32_t gran;
  bool wl;
  if (!JS_IsString(str) || !get_text(ctx, &str, &t)) {
    JS_FreeValue(ctx, str);
    return JS_ThrowTypeError(ctx, "count: not a string");
  }
  JS_ToInt32(ctx, &gran, argv[1]);
  while (pos < t.len) {
    pos = seg_next(&t, pos, gran, &wl);
    n++;
  }
  JS_FreeValue(ctx, str);
  return JS_NewUint32(ctx, n);
}

static JSValue n_getenv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *name = JS_ToCString(ctx, argv[0]), *v;
  if (!name)
    return JS_EXCEPTION;
  v = getenv(name);
  JS_FreeCString(ctx, name);
  return v ? JS_NewString(ctx, v) : JS_UNDEFINED;
}

static JSValue n_read_file(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  struct stat st;
  int fd;
  uint8_t *buf;
  ssize_t n, off = 0;
  JSValue r;
  if (!path)
    return JS_EXCEPTION;
  fd = open(path, O_RDONLY | O_CLOEXEC);
  JS_FreeCString(ctx, path);
  if (fd < 0)
    return JS_NULL;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > (16 << 20)) {
    close(fd);
    return JS_NULL;
  }
  buf = malloc(st.st_size ? st.st_size : 1);
  while (off < st.st_size) {
    n = read(fd, buf + off, st.st_size - off);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    off += n;
  }
  close(fd);
  r = JS_NewArrayBufferCopy(ctx, buf, off);
  free(buf);
  return r;
}

static JSValue n_readlink(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  const char *path = JS_ToCString(ctx, argv[0]);
  char buf[4096];
  ssize_t n;
  if (!path)
    return JS_EXCEPTION;
  n = readlink(path, buf, sizeof(buf) - 1);
  JS_FreeCString(ctx, path);
  if (n <= 0)
    return JS_NULL;
  buf[n] = 0;
  return JS_NewString(ctx, buf);
}


static const JSCFunctionListEntry native_funcs[] = {
  JS_CFUNC_DEF("segment", 3, n_segment),
  JS_CFUNC_DEF("containing", 3, n_containing),
  JS_CFUNC_DEF("iterator", 3, n_iterator),
  JS_CFUNC_DEF("next", 0, n_iterator_next),
  JS_CFUNC_DEF("count", 2, n_count),
  JS_CFUNC_DEF("getenv", 1, n_getenv),
  JS_CFUNC_DEF("readFile", 1, n_read_file),
  JS_CFUNC_DEF("readlink", 1, n_readlink),
  JS_PROP_STRING_DEF("unicodeVersion", UPROPS_UNICODE_VERSION, 0),
};

/* ---------------------------------------------------------------------- */
/* lazy installation */

enum { F_DATE_LOCALE, F_DATE_LOCALE_DATE, F_DATE_LOCALE_TIME, F_NUMBER_LOCALE,
       F_BIGINT_LOCALE, F_STRING_LOCALE_COMPARE, F_COUNT,
       F_DATE_TO_STRING = F_COUNT, F_DATE_TO_TIME_STRING };
static const char *const forward_names[F_COUNT] = {
  "dateToLocaleString", "dateToLocaleDateString", "dateToLocaleTimeString",
  "numberToLocaleString", "bigintToLocaleString", "localeCompare",
};

/* holder.api: what intl.js returns; evaluated on first use */
static JSValue intl_api(JSContext *ctx, JSValueConst holder) {
  JSValue api = JS_GetPropertyStr(ctx, holder, "api"), fn, native, global, intl;
  if (!JS_IsUndefined(api))
    return api;
  fn = JS_Eval(ctx, intl_js, sizeof(intl_js) - 1, "node:internal/intl", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(fn))
    return fn;
  native = JS_NewObjectProto(ctx, JS_NULL);
  JS_SetPropertyFunctionList(ctx, native, native_funcs, countof(native_funcs));
  JS_SetPropertyStr(ctx, native, "data", JS_NewStringLen(ctx, intl_data, sizeof(intl_data) - 1));
  api = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&native);
  JS_FreeValue(ctx, native);
  JS_FreeValue(ctx, fn);
  if (JS_IsException(api))
    return api;
  JS_SetPropertyStr(ctx, holder, "api", JS_DupValue(ctx, api));
  /* Intl becomes a plain data property, as in V8 */
  global = JS_GetGlobalObject(ctx);
  intl = JS_GetPropertyStr(ctx, api, "Intl");
  JS_DefinePropertyValueStr(ctx, global, "Intl", intl, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, global);
  return api;
}

static JSValue intl_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                        int magic, JSValueConst *data) {
  JSValue api = intl_api(ctx, data[0]), r;
  if (JS_IsException(api))
    return api;
  r = JS_GetPropertyStr(ctx, api, "Intl");
  JS_FreeValue(ctx, api);
  return r;
}

static JSValue intl_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                        int magic, JSValueConst *data) {
  JSValue global = JS_GetGlobalObject(ctx);
  JS_DefinePropertyValueStr(ctx, global, "Intl", JS_DupValue(ctx, argc ? argv[0] : JS_UNDEFINED),
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, global);
  return JS_UNDEFINED;
}

static JSValue intl_forward(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                            int magic, JSValueConst *data) {
  JSValue api = intl_api(ctx, data[0]), fn, r;
  if (JS_IsException(api))
    return api;
  fn = JS_GetPropertyStr(ctx, api, forward_names[magic]);
  JS_FreeValue(ctx, api);
  r = JS_Call(ctx, fn, this_val, argc, argv);
  JS_FreeValue(ctx, fn);
  return r;
}

/* Date.prototype.toString / toTimeString: QuickJS's, plus the zone name
   in parentheses as V8 has it ("... GMT+0900 (Korean Standard Time)") */
static JSValue date_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                              int magic, JSValueConst *data) {
  JSValue orig = JS_GetPropertyStr(ctx, data[0], magic == F_DATE_TO_STRING ? "toString" : "toTimeString");
  JSValue s = JS_Call(ctx, orig, this_val, 0, NULL), api, fn, r;
  JS_FreeValue(ctx, orig);
  if (JS_IsException(s))
    return s;
  api = intl_api(ctx, data[0]);
  if (JS_IsException(api)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return s;
  }
  fn = JS_GetPropertyStr(ctx, api, "dateZoneSuffix");
  JS_FreeValue(ctx, api);
  r = JS_Call(ctx, fn, this_val, 1, (JSValueConst *)&s);
  JS_FreeValue(ctx, fn);
  if (JS_IsException(r)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return s;
  }
  JS_FreeValue(ctx, s);
  return r;
}

static void install_date_to_string(JSContext *ctx, JSValueConst global, JSValueConst holder) {
  JSValue c = JS_GetPropertyStr(ctx, global, "Date"), proto, f;
  static const char *const names[2] = { "toString", "toTimeString" };
  if (!JS_IsObject(c)) {
    JS_FreeValue(ctx, c);
    return;
  }
  proto = JS_GetPropertyStr(ctx, c, "prototype");
  for (int i = 0; i < 2; i++) {
    /* the originals live in the holder */
    JS_SetPropertyStr(ctx, holder, names[i], JS_GetPropertyStr(ctx, proto, names[i]));
    f = JS_NewCFunctionData2(ctx, date_to_string, names[i], 0, F_DATE_TO_STRING + i, 1, &holder);
    JS_DefinePropertyValueStr(ctx, proto, names[i], f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  }
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, c);
}

static void install_forward(JSContext *ctx, JSValueConst global, const char *ctor,
                            const char *name, int length, int magic, JSValueConst holder) {
  JSValue c = JS_GetPropertyStr(ctx, global, ctor), proto, f;
  if (!JS_IsObject(c)) {
    JS_FreeValue(ctx, c);
    return;
  }
  proto = JS_GetPropertyStr(ctx, c, "prototype");
  f = JS_NewCFunctionData2(ctx, intl_forward, name, length, magic, 1, &holder);
  JS_DefinePropertyValueStr(ctx, proto, name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeValue(ctx, proto);
  JS_FreeValue(ctx, c);
}

void node_intl_install(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue holder = JS_NewObjectProto(ctx, JS_NULL), get, set;
  JSAtom a = JS_NewAtom(ctx, "Intl");
  get = JS_NewCFunctionData2(ctx, intl_get, "get Intl", 0, 0, 1, (JSValueConst *)&holder);
  set = JS_NewCFunctionData2(ctx, intl_set, "set Intl", 1, 0, 1, (JSValueConst *)&holder);
  JS_DefinePropertyGetSet(ctx, global, a, get, set, JS_PROP_CONFIGURABLE);
  JS_FreeAtom(ctx, a);
  install_forward(ctx, global, "Date", "toLocaleString", 0, F_DATE_LOCALE, holder);
  install_forward(ctx, global, "Date", "toLocaleDateString", 0, F_DATE_LOCALE_DATE, holder);
  install_forward(ctx, global, "Date", "toLocaleTimeString", 0, F_DATE_LOCALE_TIME, holder);
  install_forward(ctx, global, "Number", "toLocaleString", 0, F_NUMBER_LOCALE, holder);
  install_forward(ctx, global, "BigInt", "toLocaleString", 0, F_BIGINT_LOCALE, holder);
  install_forward(ctx, global, "String", "localeCompare", 1, F_STRING_LOCALE_COMPARE, holder);
  install_date_to_string(ctx, global, holder);
  JS_FreeValue(ctx, holder);
  JS_FreeValue(ctx, global);
}
