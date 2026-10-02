// The Intl object for Node on QuickJS (see intl.c).  Evaluated once per
// context on first use; `native` carries the C helpers (segmenters, file
// access) and the locale data (intl_data.json, generated from ICU by
// tools/gen-intl-data.js).  Formatting follows V8 + ICU for en-US; other
// locales are accepted and reported but format like en-US.
(function (native) {
'use strict';

// the data: sections stay JSON text until first used
const JSONParse = JSON.parse;
const RAW = JSONParse(native.data);
const D = { __proto__: null };
for (const k of Object.keys(RAW)) {
  if (k !== 'sections') {
    D[k] = RAW[k];
    continue;
  }
  for (const name of Object.keys(RAW.sections)) {
    const define = Object.defineProperty;
    define(D, name, {
      get() {
        const v = JSONParse(RAW.sections[name]);
        define(D, name, { value: v });
        return v;
      },
      configurable: true,
    });
  }
}

// ---------------------------------------------------------------------------
// primordials

const uncurry = (f) => Function.prototype.call.bind(f);
const ObjectDefineProperty = Object.defineProperty;
const ObjectDefineProperties = Object.defineProperties;
const ObjectCreate = Object.create;
const ObjectKeys = Object.keys;
const ObjectFreeze = Object.freeze;
const ObjectIs = Object.is;
const ObjectPrototypeHasOwnProperty = uncurry(Object.prototype.hasOwnProperty);
const ArrayIsArray = Array.isArray;
const ArrayPrototypePush = uncurry(Array.prototype.push);
const ArrayPrototypeUnshift = uncurry(Array.prototype.unshift);
const ArrayPrototypeJoin = uncurry(Array.prototype.join);
const ArrayPrototypeIncludes = uncurry(Array.prototype.includes);
const ArrayPrototypeIndexOf = uncurry(Array.prototype.indexOf);
const ArrayPrototypeSort = uncurry(Array.prototype.sort);
const StringPrototypeSlice = uncurry(String.prototype.slice);
const StringPrototypeIndexOf = uncurry(String.prototype.indexOf);
const StringPrototypeToLowerCase = uncurry(String.prototype.toLowerCase);
const StringPrototypeToUpperCase = uncurry(String.prototype.toUpperCase);
const StringPrototypeSplit = uncurry(String.prototype.split);
const StringPrototypeReplace = uncurry(String.prototype.replace);
const StringPrototypeNormalize = uncurry(String.prototype.normalize);
const StringPrototypeCharCodeAt = uncurry(String.prototype.charCodeAt);
const StringPrototypeCodePointAt = uncurry(String.prototype.codePointAt);
const StringPrototypePadStart = uncurry(String.prototype.padStart);
const StringPrototypeRepeat = uncurry(String.prototype.repeat);
const StringPrototypeStartsWith = uncurry(String.prototype.startsWith);
const StringPrototypeTrim = uncurry(String.prototype.trim);
const RegExpPrototypeExec = uncurry(RegExp.prototype.exec);
const RegExpPrototypeTest = uncurry(RegExp.prototype.test);
const NumberPrototypeToExponential = uncurry(Number.prototype.toExponential);
const NumberPrototypeValueOf = uncurry(Number.prototype.valueOf);
const BigIntPrototypeValueOf = uncurry(BigInt.prototype.valueOf);
const BigIntPrototypeToString = uncurry(BigInt.prototype.toString);
const DatePrototypeValueOf = uncurry(Date.prototype.valueOf);
const DateNow = Date.now;
const DateUTC = Date.UTC;
const DateCtor = Date;
const DatePrototypeGetUTCFullYear = uncurry(Date.prototype.getUTCFullYear);
const DatePrototypeGetUTCMonth = uncurry(Date.prototype.getUTCMonth);
const DatePrototypeGetUTCDate = uncurry(Date.prototype.getUTCDate);
const DatePrototypeGetUTCDay = uncurry(Date.prototype.getUTCDay);
const DatePrototypeGetUTCHours = uncurry(Date.prototype.getUTCHours);
const DatePrototypeGetUTCMinutes = uncurry(Date.prototype.getUTCMinutes);
const DatePrototypeGetUTCSeconds = uncurry(Date.prototype.getUTCSeconds);
const DatePrototypeGetUTCMilliseconds = uncurry(Date.prototype.getUTCMilliseconds);
const DatePrototypeGetTimezoneOffset = uncurry(Date.prototype.getTimezoneOffset);
const MathFloor = Math.floor;
const MathRound = Math.round;
const MathAbs = Math.abs;
const MathMax = Math.max;
const MathMin = Math.min;
const MathTrunc = Math.trunc;
const NumberIsNaN = Number.isNaN;
const NumberIsFinite = Number.isFinite;
const MapCtor = Map;
const SetCtor = Set;
const WeakMapCtor = WeakMap;
const MapPrototypeGet = uncurry(Map.prototype.get);
const MapPrototypeSet = uncurry(Map.prototype.set);
const MapPrototypeHas = uncurry(Map.prototype.has);
const SetPrototypeHas = uncurry(Set.prototype.has);
const SetPrototypeAdd = uncurry(Set.prototype.add);
const WeakMapPrototypeGet = uncurry(WeakMap.prototype.get);
const WeakMapPrototypeSet = uncurry(WeakMap.prototype.set);
const SymbolIterator = Symbol.iterator;
const SymbolToStringTag = Symbol.toStringTag;
const SymbolToPrimitive = Symbol.toPrimitive;
const BigIntCtor = BigInt;
const StringCtor = String;
const NumberCtor = Number;
const TypeErrorCtor = TypeError;
const RangeErrorCtor = RangeError;
const ReflectApply = Reflect.apply;
const ReflectGetPrototypeOf = Reflect.getPrototypeOf;

// ---------------------------------------------------------------------------
// option helpers (ECMA-402 GetOption & co)

function toObjectOptions(options) {
  if (options === undefined) return ObjectCreate(null);
  if (options === null) throw new TypeErrorCtor('Cannot convert undefined or null to object');
  return Object(options);
}

function getOptionsObject(options) {
  if (options === undefined) return ObjectCreate(null);
  if (typeof options === 'object' && options !== null) return options;
  if (typeof options === 'function') return options;
  throw new TypeErrorCtor('Options must be an object');
}

function getOption(opts, prop, type, values, fallback, who) {
  let v = opts[prop];
  if (v === undefined) return fallback;
  if (type === 'boolean') v = !!v;
  else v = StringCtor(v);
  if (values !== undefined && !ArrayPrototypeIncludes(values, v))
    throw new RangeErrorCtor(`Value ${v} out of range for ${who} options property ${prop}`);
  return v;
}

function defaultNumberOption(v, min, max, fallback, prop) {
  if (v === undefined) return fallback;
  v = NumberCtor(v);
  if (NumberIsNaN(v) || v < min || v > max) throw new RangeErrorCtor(`${prop} value is out of range.`);
  return MathFloor(v);
}

function getNumberOption(opts, prop, min, max, fallback) {
  return defaultNumberOption(opts[prop], min, max, fallback, prop);
}

function defineMethods(target, methods) {
  for (const k of ObjectKeys(methods))
    ObjectDefineProperty(target, k, { value: methods[k], writable: true, enumerable: false, configurable: true });
}

function defineGetter(target, name, get) {
  if (typeof name === 'string' && get.name !== 'get ' + name) setFunctionName(get, 'get ' + name);
  ObjectDefineProperty(target, name, { get, enumerable: false, configurable: true });
}

function setFunctionName(f, name) {
  ObjectDefineProperty(f, 'name', { value: name, configurable: true });
  return f;
}

function defineClass(ctor, name, length, proto, tag) {
  setFunctionName(ctor, name);
  ObjectDefineProperty(ctor, 'length', { value: length, configurable: true });
  ObjectDefineProperty(ctor, 'prototype', { value: proto, writable: false, enumerable: false, configurable: false });
  ObjectDefineProperty(proto, 'constructor', { value: ctor, writable: true, enumerable: false, configurable: true });
  if (tag) ObjectDefineProperty(proto, SymbolToStringTag, { value: tag, configurable: true });
}

function unwrap(map, obj, method) {
  const r = typeof obj === 'object' && obj !== null ? WeakMapPrototypeGet(map, obj) : undefined;
  if (r === undefined)
    throw new TypeErrorCtor(`Method ${method} called on incompatible receiver ${describe(obj)}`);
  return r;
}

function describe(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'object' && v !== null) {
    const c = v.constructor;
    return `#<${c && c.name ? c.name : 'Object'}>`;
  }
  return StringCtor(v);
}

// ---------------------------------------------------------------------------
// locales (BCP 47 structure, canonicalization, resolution)

let availableSet;
function available() {
  return availableSet || (availableSet = new SetCtor(StringPrototypeSplit(D.locales.available, ' ')));
}

function parseLanguageTag(tag) {
  if (typeof tag !== 'string' || tag.length === 0) return null;
  const parts = StringPrototypeSplit(StringPrototypeToLowerCase(tag), '-');
  let i = 0;
  const t = { language: '', script: '', region: '', variants: [], ext: [], priv: '' };
  const lang = parts[i++];
  if (!RegExpPrototypeTest(/^[a-z]{2,3}$|^[a-z]{5,8}$/, lang)) return null;
  t.language = lang;
  if (i < parts.length && RegExpPrototypeTest(/^[a-z]{4}$/, parts[i])) {
    const s = parts[i++];
    t.script = StringPrototypeToUpperCase(s[0]) + StringPrototypeSlice(s, 1);
  }
  if (i < parts.length && RegExpPrototypeTest(/^[a-z]{2}$|^[0-9]{3}$/, parts[i]))
    t.region = StringPrototypeToUpperCase(parts[i++]);
  while (i < parts.length && RegExpPrototypeTest(/^[a-z0-9]{5,8}$|^[0-9][a-z0-9]{3}$/, parts[i])) {
    if (ArrayPrototypeIncludes(t.variants, parts[i])) return null;
    ArrayPrototypePush(t.variants, parts[i++]);
  }
  const singletons = [];
  while (i < parts.length && RegExpPrototypeTest(/^[0-9a-wy-z]$/, parts[i])) {
    const s = parts[i++];
    if (ArrayPrototypeIncludes(singletons, s)) return null;
    ArrayPrototypePush(singletons, s);
    const sub = [];
    while (i < parts.length && RegExpPrototypeTest(/^[a-z0-9]{2,8}$/, parts[i])) ArrayPrototypePush(sub, parts[i++]);
    if (sub.length === 0) return null;
    ArrayPrototypePush(t.ext, { s, sub });
  }
  if (i < parts.length && parts[i] === 'x') {
    i++;
    const sub = [];
    while (i < parts.length && RegExpPrototypeTest(/^[a-z0-9]{1,8}$/, parts[i])) ArrayPrototypePush(sub, parts[i++]);
    if (sub.length === 0) return null;
    t.priv = ArrayPrototypeJoin(sub, '-');
  }
  if (i !== parts.length) return null;
  return t;
}

// the -u- extension as [attributes, {key: value}]
function parseUnicodeExtension(sub) {
  const attrs = [], keys = ObjectCreate(null), order = [];
  let i = 0;
  while (i < sub.length && sub[i].length > 2) ArrayPrototypePush(attrs, sub[i++]);
  while (i < sub.length) {
    const k = sub[i++];
    const vals = [];
    while (i < sub.length && sub[i].length > 2) ArrayPrototypePush(vals, sub[i++]);
    if (!(k in keys)) {
      keys[k] = vals.length === 0 || (vals.length === 1 && vals[0] === 'true') ? '' : ArrayPrototypeJoin(vals, '-');
      ArrayPrototypePush(order, k);
    }
  }
  return { attrs, keys, order };
}

function tagToString(t) {
  let s = t.language;
  if (t.script) s += '-' + t.script;
  if (t.region) s += '-' + t.region;
  for (const v of t.variants) s += '-' + v;
  for (const e of t.ext) {
    if (e.s === 'u') {
      const u = parseUnicodeExtension(e.sub);
      const keys = ArrayPrototypeSort(ObjectKeys(u.keys));
      let us = '';
      for (const a of ArrayPrototypeSort(u.attrs)) us += '-' + a;
      for (const k of keys) us += '-' + k + (u.keys[k] ? '-' + u.keys[k] : '');
      if (us) s += '-u' + us;
    } else {
      s += '-' + e.s + '-' + ArrayPrototypeJoin(e.sub, '-');
    }
  }
  if (t.priv) s += '-x-' + t.priv;
  return s;
}

// canonicalization with CLDR's aliases (the data has them as ICU applies
// them: languages, regions, language-region and language-variant pairs)
function canonicalizeTag(tag) {
  const t = parseLanguageTag(tag);
  if (!t) throw new RangeErrorCtor('Incorrect locale information provided');
  const A = D.locales.aliases;
  const apply = (alias, dropRegion, dropVariant) => {
    const a = parseLanguageTag(alias);
    t.language = a.language;
    if (a.script && !t.script) t.script = a.script;
    if (dropRegion) t.region = '';
    if (a.region && !t.region) t.region = a.region;
    if (dropVariant) t.variants = t.variants.filter((v) => v !== dropVariant);
  };
  let p;
  if (t.region && (p = A.pair[t.language + '-' + t.region]) !== undefined) apply(p, true);
  for (const v of t.variants) {
    if ((p = A.pair[t.language + '-' + v]) !== undefined) {
      apply(p, false, v);
      break;
    }
  }
  if ((p = A.language[t.language]) !== undefined) apply(p);
  if (t.region && (p = A.region[t.region]) !== undefined) t.region = p;
  t.variants = t.variants.map((v) => {
    const a = A.variant[v];
    return a && RegExpPrototypeTest(/^[a-z0-9]{5,8}$/, a) ? a : v;
  });
  t.variants = ArrayPrototypeSort(t.variants);
  ArrayPrototypeSort(t.ext, (a, b) => (a.s < b.s ? -1 : a.s > b.s ? 1 : 0));
  return tagToString(t);
}

let LocaleSlots; // WeakMap of Intl.Locale objects (defined below)

function canonicalizeLocaleList(locales) {
  if (locales === undefined) return [];
  const seen = [];
  let list;
  if (typeof locales === 'string' || (typeof locales === 'object' && locales !== null && WeakMapPrototypeGet(LocaleSlots, locales)))
    list = [locales];
  else
    list = Object(locales);
  const len = MathMin(MathMax(MathTrunc(NumberCtor(list.length)) || 0, 0), 2 ** 32 - 1);
  for (let k = 0; k < len; k++) {
    if (!(k in list)) continue;
    const v = list[k];
    if (typeof v !== 'string' && (typeof v !== 'object' || v === null))
      throw new TypeErrorCtor('Language ID should be string or object.');
    let tag;
    const slots = typeof v === 'object' ? WeakMapPrototypeGet(LocaleSlots, v) : undefined;
    if (slots) tag = slots.tag;
    else tag = canonicalizeTag(StringCtor(v));
    if (!ArrayPrototypeIncludes(seen, tag)) ArrayPrototypePush(seen, tag);
  }
  return seen;
}

function splitLocale(tag) {
  const t = parseLanguageTag(tag);
  let base = t.language;
  if (t.script) base += '-' + t.script;
  if (t.region) base += '-' + t.region;
  for (const v of t.variants) base += '-' + v;
  let u = null;
  for (const e of t.ext) if (e.s === 'u') u = parseUnicodeExtension(e.sub);
  return { base, u };
}

// the locales of a service (ICU has collation and plural rules mostly per
// language)
const serviceSets = new MapCtor();
function serviceSet(service) {
  if (service === undefined) return available();
  let set = MapPrototypeGet(serviceSets, service);
  if (!set) {
    const v = D.locales.services[service];
    set = v === undefined || v === 'same' ? available() : new SetCtor(StringPrototypeSplit(v, ' '));
    MapPrototypeSet(serviceSets, service, set);
  }
  return set;
}

function bestAvailable(base, set = available()) {
  let c = base;
  for (;;) {
    if (SetPrototypeHas(set, c)) return c;
    let i = c.lastIndexOf('-');
    if (i < 0) return undefined;
    c = StringPrototypeSlice(c, 0, i);
    i = c.lastIndexOf('-');
    if (i >= 0 && i === c.length - 2) c = StringPrototypeSlice(c, 0, i);
  }
}

let defaultLocaleCache;
function defaultLocale() {
  if (defaultLocaleCache !== undefined) return defaultLocaleCache;
  let v = native.getenv('LC_ALL') || native.getenv('LC_MESSAGES') || native.getenv('LANG') || '';
  v = StringPrototypeSplit(StringPrototypeSplit(v, '.')[0], '@')[0];
  v = StringPrototypeReplace(v, /_/g, '-');
  let loc = 'en-US';
  if (v && v !== 'C' && v !== 'POSIX') {
    try {
      const c = canonicalizeTag(v);
      const b = bestAvailable(splitLocale(c).base);
      if (b) loc = c === b || StringPrototypeStartsWith(c, b + '-') ? splitLocale(c).base : b;
      if (!SetPrototypeHas(available(), loc)) loc = b || 'en-US';
    } catch {}
  }
  defaultLocaleCache = loc;
  return loc;
}

// ResolveLocale: { locale, dataLocale, keys: {k: v} } for the relevant keys,
// whose values come from the -u- extension (validated by `valid`) or options
function resolveLocale(requested, relevant, options, valid, service) {
  let found, u = null;
  const set = serviceSet(service);
  for (const tag of requested) {
    const s = splitLocale(tag);
    const b = bestAvailable(s.base, set);
    if (b !== undefined) {
      found = b;
      u = s.u;
      break;
    }
  }
  if (found === undefined) found = defaultLocale();
  const keys = ObjectCreate(null);
  let ext = '';
  for (const k of relevant) {
    let v;
    let fromExt = false;
    if (u && k in u.keys) {
      const ev = u.keys[k];
      if (valid[k] && valid[k](ev, found)) {
        v = ev;
        fromExt = true;
      }
    }
    const ov = options ? options[k] : undefined;
    if (ov !== undefined && valid[k] && valid[k](ov, found)) {
      if (ov !== v) fromExt = false;
      v = ov;
    }
    keys[k] = v;
    if (fromExt) ext += '-' + k + (v ? '-' + v : '');
  }
  return { locale: ext ? found + '-u' + ext : found, dataLocale: found, keys };
}

function supportedLocales(requested, options, service) {
  if (options !== undefined) {
    const o = toObjectOptions(options);
    getOption(o, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', 'Intl');
  }
  const res = [];
  const set = serviceSet(service);
  for (const tag of requested)
    if (bestAvailable(splitLocale(tag).base, set) !== undefined) ArrayPrototypePush(res, tag);
  return res;
}

function isWellFormedUnicodeType(v) {
  return typeof v === 'string' && RegExpPrototypeTest(/^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/i, v);
}

const isCalendar = (v) => v === 'gregory' || ArrayPrototypeIncludes(D.misc.supported.calendar, v);
const isNumberingSystem = (v) => v === 'latn';

// ---------------------------------------------------------------------------
// decimal numbers: { neg, digits (no leading/trailing zeros), exp } meaning
// 0.digits * 10^exp; zero has digits ''

function decFromNumber(x) {
  if (x === 0) return { neg: ObjectIs(x, -0), digits: '', exp: 0 };
  const neg = x < 0;
  const s = NumberPrototypeToExponential(neg ? -x : x);
  const m = RegExpPrototypeExec(/^(\d)(?:\.(\d+))?e([+-]\d+)$/, s);
  let digits = m[1] + (m[2] || '');
  digits = StringPrototypeReplace(digits, /0+$/, '');
  return { neg, digits, exp: NumberCtor(m[3]) + 1 };
}

function decFromDigitString(neg, s) {
  // s: [int][.frac][e[+-]n]
  const m = RegExpPrototypeExec(/^(\d*)(?:\.(\d*))?(?:e([+-]?\d+))?$/i, s);
  let int = m[1] || '', frac = m[2] || '';
  const e = m[3] ? NumberCtor(m[3]) : 0;
  let digits = int + frac;
  let exp = int.length + e;
  const lead = RegExpPrototypeExec(/^0*/, digits)[0].length;
  digits = StringPrototypeSlice(digits, lead);
  exp -= lead;
  digits = StringPrototypeReplace(digits, /0+$/, '');
  if (!digits) exp = 0;
  return { neg, digits, exp };
}

function decFromBigInt(x) {
  const neg = x < 0n;
  return decFromDigitString(neg, BigIntPrototypeToString(neg ? -x : x));
}

const SPECIAL_NAN = { nan: true }, SPECIAL_INF = { inf: true };

// ToIntlMathematicalValue: Number, BigInt or an exact decimal string
function toIntlMV(v) {
  if (typeof v === 'object' && v !== null) v = toPrimitiveNumber(v);
  if (typeof v === 'bigint') return decFromBigInt(v);
  if (typeof v === 'string') {
    let s = StringPrototypeTrim(v);
    if (s === '') return decFromNumber(0);
    let neg = false;
    if (s[0] === '-' || s[0] === '+') {
      neg = s[0] === '-';
      if (RegExpPrototypeTest(/^[+-]Infinity$/, s)) return { ...SPECIAL_INF, neg };
      s = StringPrototypeSlice(s, 1);
      if (!RegExpPrototypeTest(/^(\d+\.?\d*|\.\d+)(e[+-]?\d+)?$/i, s)) return SPECIAL_NAN;
      return decFromDigitString(neg, s);
    }
    if (s === 'Infinity') return { ...SPECIAL_INF, neg: false };
    if (RegExpPrototypeTest(/^0[xob]/i, s)) {
      try { return decFromBigInt(BigIntCtor(s)); } catch { return SPECIAL_NAN; }
    }
    if (!RegExpPrototypeTest(/^(\d+\.?\d*|\.\d+)(e[+-]?\d+)?$/i, s)) return SPECIAL_NAN;
    return decFromDigitString(false, s);
  }
  const n = NumberCtor(v);
  if (NumberIsNaN(n)) return SPECIAL_NAN;
  if (!NumberIsFinite(n)) return { ...SPECIAL_INF, neg: n < 0 };
  return decFromNumber(n);
}

function toPrimitiveNumber(v) {
  const f = v[SymbolToPrimitive];
  if (f !== undefined && f !== null) {
    const p = ReflectApply(f, v, ['number']);
    if (typeof p === 'object' && p !== null) throw new TypeErrorCtor('Cannot convert object to primitive value');
    return p;
  }
  for (const m of ['valueOf', 'toString']) {
    const g = v[m];
    if (typeof g === 'function') {
      const p = ReflectApply(g, v, []);
      if (typeof p !== 'object' || p === null) return p;
    }
  }
  throw new TypeErrorCtor('Cannot convert object to primitive value');
}

function decIsZero(d) {
  return d.digits === '';
}

// round keeping `keep` leading digits (rounding unit 10^(exp-keep))
function decRound(d, keep, mode) {
  const n = d.digits.length;
  if (keep >= n || decIsZero(d)) return d;
  let kept = keep > 0 ? StringPrototypeSlice(d.digits, 0, keep) : '';
  const rest = keep >= 0 ? StringPrototypeSlice(d.digits, keep) : StringPrototypeRepeat('0', -keep) + d.digits;
  const first = StringPrototypeCharCodeAt(rest, 0) - 48;
  const tail = RegExpPrototypeTest(/[1-9]/, StringPrototypeSlice(rest, 1));
  const half = first > 5 || (first === 5 && tail) ? 1 : first === 5 ? 0 : -1;
  const last = kept ? StringPrototypeCharCodeAt(kept, kept.length - 1) - 48 : 0;
  let inc;
  switch (mode) {
    case 'ceil': inc = !d.neg; break;
    case 'floor': inc = d.neg; break;
    case 'expand': inc = true; break;
    case 'trunc': inc = false; break;
    case 'halfCeil': inc = half > 0 || (half === 0 && !d.neg); break;
    case 'halfFloor': inc = half > 0 || (half === 0 && d.neg); break;
    case 'halfTrunc': inc = half > 0; break;
    case 'halfEven': inc = half > 0 || (half === 0 && (last & 1) === 1); break;
    default: inc = half >= 0; break; // halfExpand
  }
  let exp = d.exp;
  if (keep <= 0) {
    if (!inc) return { neg: d.neg, digits: '', exp: 0 };
    return { neg: d.neg, digits: '1', exp: exp - keep + 1 };
  }
  if (inc) {
    // add one at the last kept digit
    let i = kept.length - 1;
    let arr = StringPrototypeSplit(kept, '');
    while (i >= 0 && arr[i] === '9') arr[i--] = '0';
    if (i < 0) {
      kept = '1' + ArrayPrototypeJoin(arr, '');
      exp++;
    } else {
      arr[i] = StringCtor.fromCharCode(StringPrototypeCharCodeAt(arr[i], 0) + 1);
      kept = ArrayPrototypeJoin(arr, '');
    }
  }
  kept = StringPrototypeReplace(kept, /0+$/, '');
  return { neg: d.neg, digits: kept, exp: kept ? exp : 0 };
}

function decRoundToIncrement(d, maxFD, increment, mode) {
  if (decIsZero(d)) return d;
  // value = N * 10^(exp - len); x = value * 10^maxFD / increment
  const len = d.digits.length;
  let N = BigIntCtor(d.digits);
  let shift = d.exp - len + maxFD;
  let den = BigIntCtor(increment);
  if (shift >= 0) N *= 10n ** BigIntCtor(shift);
  else den *= 10n ** BigIntCtor(-shift);
  let q = N / den;
  const r = N % den;
  if (r !== 0n) {
    const twice = 2n * r;
    const half = twice > den ? 1 : twice === den ? 0 : -1;
    let inc;
    switch (mode) {
      case 'ceil': inc = !d.neg; break;
      case 'floor': inc = d.neg; break;
      case 'expand': inc = true; break;
      case 'trunc': inc = false; break;
      case 'halfCeil': inc = half > 0 || (half === 0 && !d.neg); break;
      case 'halfFloor': inc = half > 0 || (half === 0 && d.neg); break;
      case 'halfTrunc': inc = half > 0; break;
      case 'halfEven': inc = half > 0 || (half === 0 && (q & 1n) === 1n); break;
      default: inc = half >= 0; break;
    }
    if (inc) q += 1n;
  }
  const v = q * BigIntCtor(increment);
  const res = decFromBigInt(v);
  res.neg = d.neg;
  if (!decIsZero(res)) res.exp -= maxFD;
  return res;
}

function decShift(d, n) {
  return decIsZero(d) ? d : { neg: d.neg, digits: d.digits, exp: d.exp + n };
}

// integer and fraction digit strings of d
function decSplit(d) {
  if (decIsZero(d)) return { int: '0', frac: '' };
  const { digits, exp } = d;
  if (exp <= 0) return { int: '0', frac: StringPrototypeRepeat('0', -exp) + digits };
  if (exp >= digits.length) return { int: digits + StringPrototypeRepeat('0', exp - digits.length), frac: '' };
  return { int: StringPrototypeSlice(digits, 0, exp), frac: StringPrototypeSlice(digits, exp) };
}

// ---------------------------------------------------------------------------
// plural rules (en)

function pluralOperands(intStr, fracStr) {
  return { i: intStr, v: fracStr.length, f: fracStr };
}

function pluralCardinal(intStr, fracStr) {
  return intStr === '1' && fracStr.length === 0 ? 'one' : 'other';
}

function pluralOrdinal(intStr, fracStr) {
  // the rules use n: trailing zeros don't matter
  if (RegExpPrototypeTest(/[1-9]/, fracStr)) return 'other';
  const n = NumberCtor(StringPrototypeSlice(intStr, -2));
  const n10 = n % 10, n100 = n % 100;
  if (n10 === 1 && n100 !== 11) return 'one';
  if (n10 === 2 && n100 !== 12) return 'two';
  if (n10 === 3 && n100 !== 13) return 'few';
  return 'other';
}

// ---------------------------------------------------------------------------
// NumberFormat

const NumberFormatSlots = new WeakMapCtor();
const ROUNDING_INCREMENTS = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000];
const ROUNDING_MODES = ['ceil', 'floor', 'expand', 'trunc', 'halfCeil', 'halfFloor', 'halfExpand', 'halfTrunc', 'halfEven'];

function isWellFormedCurrency(c) {
  return typeof c === 'string' && RegExpPrototypeTest(/^[A-Za-z]{3}$/, c);
}

function isWellFormedUnit(u) {
  if (ArrayPrototypeIncludes(D.misc.supported.unit, u)) return true;
  const i = StringPrototypeIndexOf(u, '-per-');
  if (i < 0) return false;
  const a = StringPrototypeSlice(u, 0, i), b = StringPrototypeSlice(u, i + 5);
  return ArrayPrototypeIncludes(D.misc.supported.unit, a) && ArrayPrototypeIncludes(D.misc.supported.unit, b);
}

function currencyDigits(c) {
  const e = D.number.currencies[c];
  return e && e.length > 4 ? e[4] : 2;
}

// SetNumberFormatDigitOptions
function setDigitOptions(r, opts, mnfdDefault, mxfdDefault, notation) {
  const mnid = getNumberOption(opts, 'minimumIntegerDigits', 1, 21, 1);
  const mnfd = opts.minimumFractionDigits, mxfd = opts.maximumFractionDigits;
  const mnsd = opts.minimumSignificantDigits, mxsd = opts.maximumSignificantDigits;
  r.minimumIntegerDigits = mnid;
  let roundingIncrement = getNumberOption(opts, 'roundingIncrement', 1, 5000, 1);
  if (!ArrayPrototypeIncludes(ROUNDING_INCREMENTS, roundingIncrement))
    throw new RangeErrorCtor('roundingIncrement value is out of range.');
  const roundingMode = getOption(opts, 'roundingMode', 'string', ROUNDING_MODES, 'halfExpand', 'Intl.NumberFormat');
  const roundingPriority = getOption(opts, 'roundingPriority', 'string', ['auto', 'morePrecision', 'lessPrecision'], 'auto', 'Intl.NumberFormat');
  const trailingZeroDisplay = getOption(opts, 'trailingZeroDisplay', 'string', ['auto', 'stripIfInteger'], 'auto', 'Intl.NumberFormat');
  if (roundingIncrement !== 1) mxfdDefault = mnfdDefault;
  r.roundingIncrement = roundingIncrement;
  r.roundingMode = roundingMode;
  r.trailingZeroDisplay = trailingZeroDisplay;
  const hasSd = mnsd !== undefined || mxsd !== undefined;
  const hasFd = mnfd !== undefined || mxfd !== undefined;
  let needSd = true, needFd = true;
  if (roundingPriority === 'auto') {
    needSd = hasSd;
    if (needSd || (!hasFd && notation === 'compact')) needFd = false;
  }
  if (needSd) {
    if (hasSd) {
      r.minimumSignificantDigits = defaultNumberOption(mnsd, 1, 21, 1, 'minimumSignificantDigits');
      r.maximumSignificantDigits = defaultNumberOption(mxsd, r.minimumSignificantDigits, 21, 21, 'maximumSignificantDigits');
    } else {
      r.minimumSignificantDigits = 1;
      r.maximumSignificantDigits = 21;
    }
  }
  if (needFd) {
    if (hasFd) {
      let a = defaultNumberOption(mnfd, 0, 100, undefined, 'minimumFractionDigits');
      let b = defaultNumberOption(mxfd, 0, 100, undefined, 'maximumFractionDigits');
      if (a === undefined) a = MathMin(mnfdDefault, b);
      else if (b === undefined) b = MathMax(mxfdDefault, a);
      else if (a > b) throw new RangeErrorCtor('maximumFractionDigits value is out of range.');
      r.minimumFractionDigits = a;
      r.maximumFractionDigits = b;
    } else {
      r.minimumFractionDigits = mnfdDefault;
      r.maximumFractionDigits = mxfdDefault;
    }
  }
  if (!needSd && !needFd) {
    r.minimumFractionDigits = 0;
    r.maximumFractionDigits = 0;
    r.minimumSignificantDigits = 1;
    r.maximumSignificantDigits = 2;
    r.roundingType = 'morePrecision';
    r.roundingPriority = 'morePrecision';
  } else if (roundingPriority === 'auto') {
    r.roundingType = needSd ? 'significantDigits' : 'fractionDigits';
    r.roundingPriority = 'auto';
  } else {
    r.roundingType = roundingPriority;
    r.roundingPriority = roundingPriority;
  }
  if (roundingIncrement !== 1) {
    if (r.roundingType !== 'fractionDigits')
      throw new TypeErrorCtor('roundingIncrement is only valid with fraction digit rounding');
    if (r.maximumFractionDigits !== r.minimumFractionDigits)
      throw new RangeErrorCtor('maximumFractionDigits value is out of range.');
  }
}

function initNumberFormat(nf, locales, options) {
  const requested = canonicalizeLocaleList(locales);
  const opts = toObjectOptions(options);
  const r = {};
  getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', 'Intl.NumberFormat');
  const nu = getOption(opts, 'numberingSystem', 'string', undefined, undefined, 'Intl.NumberFormat');
  if (nu !== undefined && !isWellFormedUnicodeType(nu))
    throw new RangeErrorCtor(`Invalid numberingSystem : ${nu}`);
  const loc = resolveLocale(requested, ['nu'], { nu }, { nu: isNumberingSystem }, 'number');
  r.locale = loc.locale;
  r.L = numberLocale(loc.dataLocale);
  r.numberingSystem = r.L.nu;
  const style = getOption(opts, 'style', 'string', ['decimal', 'percent', 'currency', 'unit'], 'decimal', 'Intl.NumberFormat');
  r.style = style;
  let currency = getOption(opts, 'currency', 'string', undefined, undefined, 'Intl.NumberFormat');
  if (currency !== undefined && !isWellFormedCurrency(currency))
    throw new RangeErrorCtor(`Invalid currency code : ${currency}`);
  const currencyDisplay = getOption(opts, 'currencyDisplay', 'string', ['code', 'symbol', 'narrowSymbol', 'name'], 'symbol', 'Intl.NumberFormat');
  const currencySign = getOption(opts, 'currencySign', 'string', ['standard', 'accounting'], 'standard', 'Intl.NumberFormat');
  const unit = getOption(opts, 'unit', 'string', undefined, undefined, 'Intl.NumberFormat');
  if (unit !== undefined && !isWellFormedUnit(unit))
    throw new RangeErrorCtor(`Invalid unit argument for Intl.NumberFormat() '${unit}'`);
  const unitDisplay = getOption(opts, 'unitDisplay', 'string', ['short', 'narrow', 'long'], 'short', 'Intl.NumberFormat');
  if (style === 'currency') {
    if (currency === undefined) throw new TypeErrorCtor('Currency code is required with currency style.');
    currency = StringPrototypeToUpperCase(currency);
    r.currency = currency;
    r.currencyDisplay = currencyDisplay;
    r.currencySign = currencySign;
  }
  if (style === 'unit') {
    if (unit === undefined) throw new TypeErrorCtor(`Invalid unit argument for Intl.NumberFormat() ''`);
    r.unit = unit;
    r.unitDisplay = unitDisplay;
  }
  const notation = getOption(opts, 'notation', 'string', ['standard', 'scientific', 'engineering', 'compact'], 'standard', 'Intl.NumberFormat');
  let mnfdDefault, mxfdDefault;
  if (style === 'currency' && notation === 'standard') {
    const cd = currencyDigits(currency);
    mnfdDefault = cd;
    mxfdDefault = cd;
  } else {
    mnfdDefault = 0;
    mxfdDefault = style === 'percent' ? 0 : 3;
  }
  setDigitOptions(r, opts, mnfdDefault, mxfdDefault, notation);
  r.notation = notation;
  const compactDisplay = getOption(opts, 'compactDisplay', 'string', ['short', 'long'], 'short', 'Intl.NumberFormat');
  const defaultGrouping = notation === 'compact' ? 'min2' : 'auto';
  let useGrouping = opts.useGrouping;
  if (useGrouping === undefined) useGrouping = defaultGrouping;
  else if (useGrouping === true) useGrouping = 'always';
  else if (useGrouping === false || useGrouping === null || useGrouping === 0 || useGrouping === '') useGrouping = false;
  else {
    useGrouping = typeof useGrouping === 'boolean' ? useGrouping : StringCtor(useGrouping);
    if (useGrouping === 'true' || useGrouping === 'false') useGrouping = defaultGrouping;
    else if (!ArrayPrototypeIncludes(['min2', 'auto', 'always'], useGrouping))
      throw new RangeErrorCtor(`Value ${useGrouping} out of range for Intl.NumberFormat options property useGrouping`);
  }
  r.useGrouping = useGrouping;
  if (notation === 'compact') r.compactDisplay = compactDisplay;
  r.signDisplay = getOption(opts, 'signDisplay', 'string', ['auto', 'never', 'always', 'exceptZero', 'negative'], 'auto', 'Intl.NumberFormat');
  WeakMapPrototypeSet(NumberFormatSlots, nf, r);
  return r;
}

// rounding per the digit options: the rounded decimal
function applyRounding(r, x) {
  const sig = () => {
    let d = decRound(x, r.maximumSignificantDigits, r.roundingMode);
    return { d, mag: (decIsZero(d) ? x.exp : x.exp) - r.maximumSignificantDigits, type: 's' };
  };
  const frac = () => {
    let d = r.roundingIncrement !== 1
      ? decRoundToIncrement(x, r.maximumFractionDigits, r.roundingIncrement, r.roundingMode)
      : decRound(x, x.exp + r.maximumFractionDigits, r.roundingMode);
    return { d, mag: -r.maximumFractionDigits, type: 'f' };
  };
  if (r.roundingType === 'significantDigits') return sig();
  if (r.roundingType === 'fractionDigits') return frac();
  const s = sig(), f = frac();
  if (r.roundingType === 'morePrecision') return s.mag <= f.mag ? s : f;
  return s.mag <= f.mag ? f : s;
}

// the digits after rounding and padding: { int, frac }
function toDigitStrings(r, rounded) {
  const { d, type } = rounded;
  let { int, frac } = decSplit(d);
  if (type === 's') {
    const sigCount = decIsZero(d) ? 1 : (int === '0' ? frac.length - (RegExpPrototypeExec(/^0*/, frac)[0].length) : int.length + frac.length);
    let min = r.minimumSignificantDigits;
    if (sigCount < min) {
      const need = min - sigCount;
      if (decIsZero(d)) frac += StringPrototypeRepeat('0', min - 1);
      else frac += StringPrototypeRepeat('0', need);
    }
  } else {
    if (frac.length < r.minimumFractionDigits) frac += StringPrototypeRepeat('0', r.minimumFractionDigits - frac.length);
  }
  if (r.trailingZeroDisplay === 'stripIfInteger' && RegExpPrototypeTest(/^0*$/, frac)) frac = '';
  if (int.length < r.minimumIntegerDigits) int = StringPrototypeRepeat('0', r.minimumIntegerDigits - int.length) + int;
  return { int, frac };
}

const COMPACT_SHORT = ['', '', '', 'K', 'K', 'K', 'M', 'M', 'M', 'B', 'B', 'B', 'T', 'T', 'T'];
const COMPACT_LONG = ['', '', '', 'thousand', 'thousand', 'thousand', 'million', 'million', 'million',
                      'billion', 'billion', 'billion', 'trillion', 'trillion', 'trillion'];

function compactExponent(e) {
  if (e < 3) return 0;
  if (e >= 15) return 12;
  return e - (e % 3);
}

// the locale's number symbols and patterns (tools/gen-intl-data.js)
const numberLocales = new MapCtor();
function numberLocale(locale) {
  let L = MapPrototypeGet(numberLocales, locale);
  if (L) return L;
  const N = D.nlocales;
  let i = N.map[locale];
  if (i === undefined) {
    const b = bestAvailable(locale);
    i = b !== undefined && N.map[b] !== undefined ? N.map[b] : N.map.en;
  }
  const rec = N.records[i], T = N.templates;
  // (d, p, c: the [positive, negative, plus] patterns of decimal, percent
  // and currency numbers; a: accounting negative)
  L = { ...rec, d: T[rec.d], p: T[rec.p], c: T[rec.c], a: T[rec.a], sym: N.syms[rec.sym] };
  MapPrototypeSet(numberLocales, locale, L);
  return L;
}

// ASCII digits in the locale's numbering system
function localDigits(L, s) {
  if (!L.digits) return s;
  if (!L.digitList) L.digitList = [...L.digits];
  let r = '';
  for (let i = 0; i < s.length; i++) {
    const c = StringPrototypeCharCodeAt(s, i) - 48;
    r += c >= 0 && c <= 9 ? L.digitList[c] : s[i];
  }
  return r;
}

// the decimal and group symbols (currencies may have their own)
function currencySymbols(r) {
  const L = r.L;
  if (r.style !== 'currency') return L;
  const own = L.cpat && L.cpat[r.currency];
  if (own) return { decimal: own[2] || L.decimal, group: own[3] || L.group };
  return { decimal: L.cdecimal || L.decimal, group: L.cgroup || L.group };
}

function groupInteger(r, int, parts) {
  const L = r.L;
  // the pattern's grouping sizes
  const z = (r.style === 'percent' ? L.gp : r.style === 'currency' ? (r.currencySign === 'accounting' ? L.ga : L.gc) : null) ||
    [L.g1, L.g2];
  const g = r.useGrouping;
  const min = g === 'min2' ? 2 : g === 'always' ? 1 : g === 'auto' ? L.mg : 0;
  if (!g || int.length < z[0] + min) {
    ArrayPrototypePush(parts, { type: 'integer', value: localDigits(L, int) });
    return;
  }
  // the last group has g1 digits, the others g2 (Indian: 12,34,567)
  const groups = [StringPrototypeSlice(int, int.length - z[0])];
  let end = int.length - z[0];
  while (end > 0) {
    ArrayPrototypePush(groups, StringPrototypeSlice(int, MathMax(0, end - z[1]), end));
    end -= z[1];
  }
  for (let i = groups.length - 1; i >= 0; i--) {
    ArrayPrototypePush(parts, { type: 'integer', value: localDigits(L, groups[i]) });
    if (i) ArrayPrototypePush(parts, { type: 'group', value: currencySymbols(r).group });
  }
}

// the number's parts (without sign and affixes), and its plural category
function numberCore(r, x) {
  const parts = [];
  if (x.nan) {
    ArrayPrototypePush(parts, { type: 'nan', value: r.L.nan });
    return { parts, expParts: [], compactParts: [], zero: true, nan: true, plural: 'other', neg: false };
  }
  if (x.inf) {
    ArrayPrototypePush(parts, { type: 'infinity', value: r.L.inf });
    return { parts, expParts: [], compactParts: [], zero: false, inf: true, plural: 'other', neg: x.neg };
  }
  if (r.style === 'percent') x = decShift(x, 2);
  let exponent = null, compact = null;
  let rounded;
  if (r.notation === 'scientific' || r.notation === 'engineering') {
    const exponentOf = (d) => {
      if (decIsZero(d)) return 0;
      const e = d.exp - 1;
      return r.notation === 'engineering' ? MathFloor(e / 3) * 3 : e;
    };
    let e = exponentOf(x);
    rounded = applyRounding(r, decShift(x, -e));
    const e2 = exponentOf(decShift(rounded.d, e));
    if (e2 !== e) {
      e = e2;
      rounded = applyRounding(r, decShift(x, -e));
    }
    exponent = e;
  } else if (r.notation === 'compact') {
    const magnitude = (d) => (decIsZero(d) ? 0 : d.exp - 1);
    let k = compactExponent(magnitude(x));
    rounded = applyRounding(r, decShift(x, -k));
    const k2 = compactExponent(magnitude(decShift(rounded.d, k)));
    if (k2 !== k) {
      k = k2;
      rounded = applyRounding(r, decShift(x, -k));
    }
    if (k) compact = (r.compactDisplay === 'long' ? COMPACT_LONG : COMPACT_SHORT)[k];
  } else {
    rounded = applyRounding(r, x);
  }
  const { int, frac } = toDigitStrings(r, rounded);
  groupInteger(r, int, parts);
  if (frac) {
    ArrayPrototypePush(parts, { type: 'decimal', value: currencySymbols(r).decimal });
    ArrayPrototypePush(parts, { type: 'fraction', value: localDigits(r.L, frac) });
  }
  const expParts = [], compactParts = [];
  if (exponent !== null) {
    ArrayPrototypePush(expParts, { type: 'exponentSeparator', value: r.L.exp });
    if (exponent < 0) {
      for (const t of r.L.expNeg)
        ArrayPrototypePush(expParts, t === 'm' ? { type: 'exponentMinusSign', value: r.L.minus } : { type: 'literal', value: t });
    }
    ArrayPrototypePush(expParts, { type: 'exponentInteger', value: localDigits(r.L, StringCtor(MathAbs(exponent))) });
  }
  if (compact) {
    if (r.compactDisplay === 'long') ArrayPrototypePush(compactParts, { type: 'literal', value: ' ' });
    ArrayPrototypePush(compactParts, { type: 'compact', value: compact });
  }
  const zero = decIsZero(rounded.d);
  return { parts, expParts, compactParts, zero, rounded: rounded.d,
           plural: compact || exponent !== null ? 'other' : pluralCardinal(int, frac), neg: x.neg };
}

function signFor(r, core) {
  const neg = core.neg;
  switch (r.signDisplay) {
    case 'never': return '';
    case 'always': return neg ? '-' : '+';
    case 'exceptZero': return core.zero ? '' : neg ? '-' : '+'; // NaN counts as zero
    case 'negative': return neg && !core.zero ? '-' : '';
    default: return neg ? '-' : '';
  }
}

function currencyText(r, plural) {
  const e = D.number.currencies[r.currency];
  switch (r.currencyDisplay) {
    case 'code': return r.currency;
    case 'name': return e ? (plural === 'one' || !e[3] ? e[2] : e[3]) : r.currency;
    case 'narrowSymbol': {
      const l = r.L.sym['n' + r.currency];
      return l !== undefined ? l : e ? e[1] || e[0] || r.currency : r.currency;
    }
    default: {
      const l = r.L.sym[r.currency];
      return l !== undefined ? l : e ? e[0] || r.currency : r.currency;
    }
  }
}

function unitPattern(r, plural) {
  const u = r.unit, d = r.unitDisplay[0];
  const pick = (e) => (typeof e === 'string' ? e : e[plural === 'one' ? 0 : 1]);
  const i = StringPrototypeIndexOf(u, '-per-');
  if (i < 0) return pick(D.number.units[u][d]);
  const special = D.number.perSpecial[u + '|' + d + (plural === 'one' ? 1 : 2)];
  if (special) return special;
  const x = StringPrototypeSlice(u, 0, i), y = StringPrototypeSlice(u, i + 5);
  return StringPrototypeReplace(D.number.perPatterns[y][d], '{0}', pick(D.number.units[x][d]));
}

// the formatted number in ICU's layers: the number with its exponent
// (inner), the affixes of the number pattern - sign, currency symbol,
// percent, compact suffix (middle), and the unit or currency name pattern
// (outer, a function of the plural form)
function numberLayers(r, x) {
  const core = numberCore(r, x);
  const L = r.L;
  const sign = signFor(r, core);
  let outer = null;
  const currency = r.style === 'currency' && r.currencyDisplay !== 'name';
  const percent = r.style === 'percent' || (r.style === 'unit' && r.unit === 'percent');
  // (some currencies have patterns of their own in a locale)
  const own = currency && L.cpat && L.cpat[r.currency];
  const set = own ? D.nlocales.templates[own[0]] : currency ? L.c : percent ? L.p : L.d;
  const acc = own ? D.nlocales.templates[own[1]] : L.a;
  // the locale's pattern: for a sign, its negative one (with the plus sign
  // for "+"), or the accounting one
  const tpl = currency && r.currencySign === 'accounting' && sign !== '+' ? acc[sign === '-' ? 1 : 0]
    : sign === '-' ? set[1] : sign === '+' ? set[2] : set[0];
  const pre = [], suf = [];
  let afterNumber = false;
  for (const t of tpl) {
    let part;
    switch (t) {
      case 'n': afterNumber = true; continue;
      case 'm': part = { type: 'minusSign', value: L.minus }; break;
      case 'p': part = { type: 'plusSign', value: L.plus }; break;
      case '%': part = { type: r.style === 'unit' ? 'unit' : 'percentSign', value: L.percent }; break;
      case 'c': part = { type: 'currency', value: currencyText(r, core.plural) }; break;
      default: part = { type: 'literal', value: t };
    }
    ArrayPrototypePush(afterNumber ? suf : pre, part);
  }
  // currency spacing: a symbol ending (starting) in a letter is set off
  // from a digit after (before) it
  const first = core.parts[0], last = core.parts[core.parts.length - 1];
  const isDigit = (ch) => ch !== undefined && RegExpPrototypeTest(/\p{Nd}/u, ch);
  const lastChar = (v) => { const a = [...v]; return a[a.length - 1]; };
  const spaced = (ch) => !RegExpPrototypeTest(/[\p{S}\p{Z}]/u, ch);
  const pl = pre[pre.length - 1];
  if (pl && pl.type === 'currency' && !core.nan && !core.inf && isDigit(StringCtor.fromCodePoint(StringPrototypeCodePointAt(first.value, 0))) &&
      spaced(StringCtor.fromCodePoint(StringPrototypeCodePointAt(pl.value, pl.value.length - 1) ?? 32)))
    ArrayPrototypePush(pre, { type: 'literal', value: '\u00a0' });
  const sf = suf[0];
  if (sf && sf.type === 'currency' && !core.nan && !core.inf && !core.compactParts.length && !core.expParts.length &&
      isDigit(lastChar(last.value)) && spaced(StringCtor.fromCodePoint(StringPrototypeCodePointAt(sf.value, 0))))
    ArrayPrototypeUnshift(suf, { type: 'literal', value: '\u00a0' });
  for (let i = core.compactParts.length - 1; i >= 0; i--) ArrayPrototypeUnshift(suf, core.compactParts[i]);
  if (r.style === 'currency' && r.currencyDisplay === 'name')
    outer = (plural) => ['', [{ type: 'literal', value: ' ' }, { type: 'currency', value: currencyText(r, plural) }]];
  if (r.style === 'unit' && r.unit !== 'percent') {
    outer = (plural) => {
      const pattern = unitPattern(r, plural);
      const i = StringPrototypeIndexOf(pattern, '{0}');
      return [splitAffix(StringPrototypeSlice(pattern, 0, i), 'unit'), splitAffix(StringPrototypeSlice(pattern, i + 3), 'unit')];
    };
  }
  return { core, pre, suf, outer, plural: core.plural };
}

// affix text as parts: the text is of `kind`, surrounding spaces literals
function splitAffix(t, kind) {
  const res = [];
  if (!t) return res;
  const m = RegExpPrototypeExec(/^(\s*)(.*?)(\s*)$/su, t);
  if (m[1]) ArrayPrototypePush(res, { type: 'literal', value: m[1] });
  if (m[2]) ArrayPrototypePush(res, { type: kind, value: m[2] });
  if (m[3]) ArrayPrototypePush(res, { type: 'literal', value: m[3] });
  return res;
}

function layersToParts(L, plural = L.plural) {
  const o = L.outer ? L.outer(plural) : null;
  const res = [];
  const push = (list) => { for (const p of list) ArrayPrototypePush(res, p); };
  if (o) push(o[0]);
  push(L.pre);
  push(L.core.parts);
  push(L.core.expParts);
  push(L.suf);
  if (o) push(o[1]);
  return res;
}

function formatNumberToParts(r, x) {
  return layersToParts(numberLayers(r, x));
}

function partsToString(parts) {
  let s = '';
  for (const p of parts) s += p.value;
  return s;
}

function formatNumberString(r, x) {
  return partsToString(formatNumberToParts(r, x));
}

function NumberFormat(locales, options) {
  const nf = new.target === undefined ? ObjectCreate(NumberFormat.prototype)
    : ObjectCreate(new.target.prototype || NumberFormat.prototype);
  initNumberFormat(nf, locales, options);
  return nf;
}
const NumberFormatPrototype = {};
defineClass(NumberFormat, 'NumberFormat', 0, NumberFormatPrototype, 'Intl.NumberFormat');
ObjectDefineProperty(NumberFormat, 'supportedLocalesOf', {
  value: setFunctionName(function supportedLocalesOf(locales, options = undefined) {
    return supportedLocales(canonicalizeLocaleList(locales), options, 'number');
  }, 'supportedLocalesOf'), writable: true, configurable: true,
});
defineMethods(NumberFormatPrototype, {
  resolvedOptions() {
    const r = unwrap(NumberFormatSlots, this, 'Intl.NumberFormat.prototype.resolvedOptions');
    const o = {};
    for (const k of ['locale', 'numberingSystem', 'style', 'currency', 'currencyDisplay', 'currencySign', 'unit',
                     'unitDisplay', 'minimumIntegerDigits', 'minimumFractionDigits', 'maximumFractionDigits',
                     'minimumSignificantDigits', 'maximumSignificantDigits', 'useGrouping', 'notation',
                     'compactDisplay', 'signDisplay', 'roundingIncrement', 'roundingMode', 'roundingPriority',
                     'trailingZeroDisplay'])
      if (r[k] !== undefined) o[k] = r[k];
    if (r.roundingType === 'significantDigits') {
      delete o.minimumFractionDigits;
      delete o.maximumFractionDigits;
    } else if (r.roundingType === 'fractionDigits') {
      delete o.minimumSignificantDigits;
      delete o.maximumSignificantDigits;
    }
    // V8 reports "auto" here (it reads the priority back from ICU)
    if (r.trailingZeroDisplay === 'stripIfInteger') o.roundingPriority = 'auto';
    return o;
  },
  formatToParts(value) {
    const r = unwrap(NumberFormatSlots, this, 'Intl.NumberFormat.prototype.formatToParts');
    return formatNumberToParts(r, toIntlMV(value));
  },
  formatRange(start, end) {
    const r = unwrap(NumberFormatSlots, this, 'Intl.NumberFormat.prototype.formatRange');
    return partsToString(formatNumberRange(r, start, end));
  },
  formatRangeToParts(start, end) {
    const r = unwrap(NumberFormatSlots, this, 'Intl.NumberFormat.prototype.formatRangeToParts');
    return formatNumberRange(r, start, end);
  },
});
defineGetter(NumberFormatPrototype, 'format', function format() {
  const r = unwrap(NumberFormatSlots, this, 'get Intl.NumberFormat.prototype.format');
  if (!r.boundFormat) {
    r.boundFormat = setFunctionName((value) => formatNumberString(r, toIntlMV(value)), '');
  }
  return r.boundFormat;
});

function codePointCount(parts) {
  let n = 0;
  for (const p of parts) for (const c of p.value) n++;
  return n;
}

function decEqual(a, b) {
  return a !== undefined && b !== undefined && a.neg === b.neg && a.digits === b.digits && a.exp === b.exp;
}

// ICU's number range formatting with collapse "auto" and the identity
// fallback "approximately"
function formatNumberRange(r, start, end) {
  if (start === undefined || end === undefined) throw new TypeErrorCtor('start or end is undefined');
  const x = toIntlMV(start), y = toIntlMV(end);
  if (x.nan || y.nan) throw new RangeErrorCtor('Invalid number range');
  const A = numberLayers(r, x), B = numberLayers(r, y);
  const tag = (list, source) => list.map((p) => ({ type: p.type, value: p.value, source }));
  const res = [];
  const push = (list, source) => { for (const p of list) ArrayPrototypePush(res, { type: p.type, value: p.value, source }); };
  const sameText = partsToString(layersToParts(A)) === partsToString(layersToParts(B));
  if (sameText && (decEqual(A.core.rounded, B.core.rounded) || A.core.inf || A.core.zero)) {
    const parts = layersToParts(A);
    // the approximately sign goes before the sign
    let i = 0;
    const o = A.outer ? A.outer(A.plural)[0] : [];
    i = o.length;
    const approx = { type: 'approximatelySign', value: r.L.approx };
    return tag([...parts.slice(0, i), approx, ...parts.slice(i)], 'shared');
  }
  const mid = (L) => [...L.pre, ...L.suf];
  const collapseMiddle = partsToString(mid(A)) === partsToString(mid(B)) &&
    partsToString(A.pre) === partsToString(B.pre) && codePointCount(mid(A)) > 1;
  const repeatInner = A.core.expParts.length > 0;
  const repeatMiddle = !collapseMiddle && codePointCount(mid(A)) > 0;
  // the locale's separator, spaced unless everything collapsed
  const R = r.L.range;
  const sep = repeatInner || repeatMiddle
    ? (RegExpPrototypeTest(/^\s/u, R) ? '' : ' ') + R + (RegExpPrototypeTest(/\s$/u, R) ? '' : ' ') : R;
  // the outer modifier collapses, with the plural of the range (en: other)
  const o = A.outer ? A.outer('other') : null;
  if (o) push(o[0], 'shared');
  push(A.pre, collapseMiddle ? 'shared' : 'startRange');
  push(A.core.parts, 'startRange');
  push(A.core.expParts, 'startRange');
  if (!collapseMiddle) push(A.suf, 'startRange');
  ArrayPrototypePush(res, { type: 'literal', value: sep, source: 'shared' });
  if (!collapseMiddle) push(B.pre, 'endRange');
  push(B.core.parts, 'endRange');
  push(B.core.expParts, 'endRange');
  push(B.suf, collapseMiddle ? 'shared' : 'endRange');
  if (o) push(o[1], 'shared');
  return res;
}

// ---------------------------------------------------------------------------
// time zones

let zoneMapsCache;
// lower-case name -> resolved id; resolved id -> zoneinfo names to try
function zoneMaps() {
  if (zoneMapsCache) return zoneMapsCache;
  const ids = new MapCtor(), files = new MapCtor();
  for (const z of ObjectKeys(D.tz.zones)) MapPrototypeSet(ids, StringPrototypeToLowerCase(z), z);
  for (const z of ObjectKeys(D.tz.links)) {
    const id = D.tz.links[z];
    MapPrototypeSet(ids, StringPrototypeToLowerCase(z), id);
    let l = MapPrototypeGet(files, id);
    if (!l) MapPrototypeSet(files, id, (l = []));
    ArrayPrototypePush(l, z);
  }
  return (zoneMapsCache = { ids, files });
}

function offsetZone(name) {
  const m = RegExpPrototypeExec(/^([+-])(\d{2})(?::?(\d{2}))?$/, name);
  if (!m) return null;
  const h = NumberCtor(m[2]), mi = m[3] ? NumberCtor(m[3]) : 0;
  if (h > 23 || mi > 59) return null;
  const off = (m[1] === '-' ? -1 : 1) * (h * 60 + mi);
  const id = (off < 0 ? '-' : '+') + StringPrototypePadStart(StringCtor(h), 2, '0') + ':' + StringPrototypePadStart(StringCtor(mi), 2, '0');
  return { id, fixed: off };
}

// { id, fixed? } or null for an unknown zone
function resolveTimeZone(name) {
  name = StringCtor(name);
  const o = offsetZone(name);
  if (o) return o;
  const id = MapPrototypeGet(zoneMaps().ids, StringPrototypeToLowerCase(name));
  if (id === undefined) return null;
  return { id };
}

const zoneRules = new MapCtor();

function rd32(dv, o) { return dv.getInt32(o, false); }

function parseTZif(buf) {
  const dv = new DataView(buf);
  if (buf.byteLength < 44 || dv.getUint32(0, false) !== 0x545a6966) return null;
  const version = dv.getUint8(4);
  const header = (o) => ({
    isut: dv.getUint32(o + 20, false), isstd: dv.getUint32(o + 24, false), leap: dv.getUint32(o + 28, false),
    time: dv.getUint32(o + 32, false), type: dv.getUint32(o + 36, false), char: dv.getUint32(o + 40, false),
  });
  let h = header(0), o = 44, tsize = 4;
  if (version >= 0x32) {
    o += h.time * 4 + h.time + h.type * 6 + h.char + h.leap * 8 + h.isstd + h.isut;
    h = header(o);
    o += 44;
    tsize = 8;
  }
  const trans = [], idx = [], types = [];
  for (let i = 0; i < h.time; i++) {
    let t;
    if (tsize === 8) t = rd32(dv, o + i * 8) * 4294967296 + dv.getUint32(o + i * 8 + 4, false);
    else t = rd32(dv, o + i * 4);
    ArrayPrototypePush(trans, t);
  }
  o += h.time * tsize;
  for (let i = 0; i < h.time; i++) ArrayPrototypePush(idx, dv.getUint8(o + i));
  o += h.time;
  const typesAt = o;
  o += h.type * 6;
  let chars = '';
  for (let i = 0; i < h.char; i++) chars += StringCtor.fromCharCode(dv.getUint8(o + i));
  for (let i = 0; i < h.type; i++) {
    const p = typesAt + i * 6;
    const ai = dv.getUint8(p + 5);
    ArrayPrototypePush(types, { off: rd32(dv, p) / 60, dst: dv.getUint8(p + 4) !== 0,
                                abbr: StringPrototypeSlice(chars, ai, StringPrototypeIndexOf(chars, '\0', ai)) });
  }
  o += h.char + h.leap * (tsize + 4) + h.isstd + h.isut;
  let footer = null;
  if (tsize === 8 && o < buf.byteLength) {
    let s = '';
    for (let i = o + 1; i < buf.byteLength; i++) {
      const c = dv.getUint8(i);
      if (c === 10) break;
      s += StringCtor.fromCharCode(c);
    }
    footer = s ? parsePosixTZ(s) : null;
  }
  return { trans, idx, types, footer };
}

// POSIX TZ strings: "STDoff[DST[off][,start[/time],end[/time]]]"
function parsePosixTZ(s) {
  let i = 0;
  const name = () => {
    if (s[i] === '<') {
      const j = StringPrototypeIndexOf(s, '>', i);
      const n = StringPrototypeSlice(s, i + 1, j);
      i = j + 1;
      return n;
    }
    const m = RegExpPrototypeExec(/^[A-Za-z]+/, StringPrototypeSlice(s, i));
    if (!m) return null;
    i += m[0].length;
    return m[0];
  };
  const time = () => {
    const m = RegExpPrototypeExec(/^([+-]?)(\d{1,3})(?::(\d{1,2}))?(?::(\d{1,2}))?/, StringPrototypeSlice(s, i));
    if (!m) return null;
    i += m[0].length;
    const v = NumberCtor(m[2]) * 3600 + (m[3] ? NumberCtor(m[3]) * 60 : 0) + (m[4] ? NumberCtor(m[4]) : 0);
    return m[1] === '-' ? -v : v;
  };
  const std = name();
  if (std === null) return null;
  const stdOff = time();
  if (stdOff === null) return null;
  const r = { std, stdOff: -stdOff / 60 };
  if (i >= s.length) return r;
  const dst = name();
  if (dst === null) return r;
  r.dst = dst;
  let dstOff = r.stdOff + 60;
  if (s[i] !== ',' && i < s.length) {
    const t = time();
    if (t !== null) dstOff = -t / 60;
  }
  r.dstOff = dstOff;
  const rule = () => {
    if (s[i] !== ',') return null;
    i++;
    let m, rr;
    if ((m = RegExpPrototypeExec(/^M(\d+)\.(\d)\.(\d)/, StringPrototypeSlice(s, i)))) {
      rr = { kind: 'M', m: NumberCtor(m[1]), w: NumberCtor(m[2]), d: NumberCtor(m[3]) };
    } else if ((m = RegExpPrototypeExec(/^J(\d+)/, StringPrototypeSlice(s, i)))) {
      rr = { kind: 'J', n: NumberCtor(m[1]) };
    } else if ((m = RegExpPrototypeExec(/^(\d+)/, StringPrototypeSlice(s, i)))) {
      rr = { kind: 'N', n: NumberCtor(m[1]) };
    } else return null;
    i += m[0].length;
    rr.time = 7200;
    if (s[i] === '/') {
      i++;
      rr.time = time();
    }
    return rr;
  };
  r.start = rule() || { kind: 'M', m: 3, w: 2, d: 0, time: 7200 };
  r.end = rule() || { kind: 'M', m: 11, w: 1, d: 0, time: 7200 };
  return r;
}

const DAYS_BEFORE_MONTH = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334];
function isLeap(y) { return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0; }

// seconds since the epoch of the rule's moment in year y, local wall time
function ruleLocalSeconds(rr, y) {
  let day; // days since the epoch
  const jan1 = DateUTC(y, 0, 1) / 86400000;
  if (rr.kind === 'M') {
    const first = DateUTC(y, rr.m - 1, 1) / 86400000;
    const wd = ((first + 4) % 7 + 7) % 7; // 1970-01-01 was a Thursday
    let d = (rr.d - wd + 7) % 7 + (rr.w - 1) * 7;
    const dim = rr.m === 2 ? (isLeap(y) ? 29 : 28) : [31, 0, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][rr.m - 1];
    while (d >= dim) d -= 7;
    day = first + d;
  } else if (rr.kind === 'J') {
    day = jan1 + rr.n - 1 + (isLeap(y) && rr.n >= 60 ? 1 : 0);
  } else {
    day = jan1 + rr.n;
  }
  return day * 86400 + rr.time;
}

function posixOffset(p, sec) {
  if (p.dst === undefined) return { off: p.stdOff, dst: false, abbr: p.std };
  const y = DatePrototypeGetUTCFullYear(new DateCtor((sec + p.stdOff * 60) * 1000));
  const start = ruleLocalSeconds(p.start, y) - p.stdOff * 60;
  const end = ruleLocalSeconds(p.end, y) - p.dstOff * 60;
  let inDst;
  if (start < end) inDst = sec >= start && sec < end;
  else inDst = !(sec >= end && sec < start);
  return inDst ? { off: p.dstOff, dst: true, abbr: p.dst } : { off: p.stdOff, dst: false, abbr: p.std };
}

function loadZone(id) {
  let r = MapPrototypeGet(zoneRules, id);
  if (r !== undefined) return r;
  r = null;
  // the main tz data's rules (Africa/Ouagadougou is Africa/Abidjan's), as
  // in ICU, even where the system's zoneinfo has backzone data
  const names = [D.tz.files[id] || id, id, ...(MapPrototypeGet(zoneMaps().files, id) || [])];
  for (const n of names) {
    if (!RegExpPrototypeTest(/^[A-Za-z0-9_+\-/]+$/, n) || StringPrototypeIndexOf(n, '..') >= 0) continue;
    const buf = native.readFile('/usr/share/zoneinfo/' + n);
    if (buf) {
      r = parseTZif(buf);
      if (r) break;
    }
  }
  if (!r && D.tz.posix[id]) {
    const p = parsePosixTZ(D.tz.posix[id]);
    if (p) r = { trans: [], idx: [], types: [], footer: p };
  }
  if (!r) r = { trans: [], idx: [], types: [], footer: { std: 'UTC', stdOff: 0 } };
  MapPrototypeSet(zoneRules, id, r);
  return r;
}

// { off (minutes east), dst } of zone tz at epoch ms
function zoneOffset(tz, ms) {
  if (tz.fixed !== undefined) return { off: tz.fixed, dst: false };
  if (tz.local) return { off: -DatePrototypeGetTimezoneOffset(new DateCtor(ms)), dst: false };
  if (tz.id === 'UTC') return { off: 0, dst: false };
  const z = loadZone(tz.id);
  const sec = MathFloor(ms / 1000);
  const n = z.trans.length;
  if (n === 0 || sec >= z.trans[n - 1]) {
    if (z.footer) return posixOffset(z.footer, sec);
    if (n === 0) return { off: 0, dst: false };
    return z.types[z.idx[n - 1]];
  }
  if (sec < z.trans[0]) {
    let t = z.types[0];
    for (const x of z.types) if (!x.dst) { t = x; break; }
    return t;
  }
  let lo = 0, hi = n - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (z.trans[mid] <= sec) lo = mid;
    else hi = mid - 1;
  }
  return z.types[z.idx[lo]];
}

// a zone named exactly as in tzdata (as ICU looks up TZ)
function exactZone(name) {
  if (ObjectPrototypeHasOwnProperty(D.tz.zones, name)) return { id: name };
  if (ObjectPrototypeHasOwnProperty(D.tz.links, name)) return { id: D.tz.links[name] };
  return null;
}

// the default zone, as V8 finds it: TZ (a zone name; empty is
// Etc/Unknown; anything else, like a POSIX rule, is the C library's local
// time without a name), else /etc/localtime, else UTC
let defaultZoneCache;
function defaultTimeZoneRecord() {
  if (defaultZoneCache !== undefined) return defaultZoneCache;
  let r = null;
  let tz = native.getenv('TZ');
  if (tz !== undefined) {
    if (tz === '') {
      r = { id: 'Etc/Unknown', fixed: 0 };
    } else {
      if (tz[0] === ':') tz = StringPrototypeSlice(tz, 1);
      const k = StringPrototypeIndexOf(tz, 'zoneinfo/');
      if (k >= 0 && tz[0] === '/') tz = StringPrototypeSlice(tz, k + 9);
      r = exactZone(tz) || { id: undefined, local: true };
    }
  } else {
    const l = native.readlink('/etc/localtime');
    if (l) {
      const k = StringPrototypeIndexOf(l, 'zoneinfo/');
      if (k >= 0) r = exactZone(StringPrototypeSlice(l, k + 9));
    }
  }
  defaultZoneCache = r || { id: 'UTC' };
  return defaultZoneCache;
}

// GMT formats ("GMT+5:30", "GMT+05:30"; local mean time has seconds)
function gmtParts(min) {
  const a = MathRound(MathAbs(min) * 60);
  return { sign: min < 0 ? '-' : '+', h: MathFloor(a / 3600), m: MathFloor(a / 60) % 60, s: a % 60 };
}

function gmtShort(min) {
  const g = gmtParts(min);
  if (!g.h && !g.m && !g.s) return 'GMT+0';
  let r = 'GMT' + g.sign + g.h;
  if (g.m || g.s) r += ':' + pad2(g.m);
  if (g.s) r += ':' + pad2(g.s);
  return r;
}

function gmtLong(min) {
  const g = gmtParts(min);
  if (!g.h && !g.m && !g.s) return 'GMT+00:00';
  return 'GMT' + g.sign + pad2(g.h) + ':' + pad2(g.m) + (g.s ? ':' + pad2(g.s) : '');
}

// the zone's name in a style at ms (with offset off); the data has the
// names by period and offset (see tools/gen-intl-data.js)
function zoneName(tz, style, off, ms) {
  if (style === 'shortOffset') return gmtShort(off);
  if (style === 'longOffset') return gmtLong(off);
  const z = tz.fixed === undefined && !tz.local ? D.tz.zones[tz.id] : 0;
  const generic = style === 'shortGeneric' || style === 'longGeneric';
  const long = style === 'long' || style === 'longGeneric';
  let i = 0;
  if (z) {
    const str = (k) => (k ? D.tz.strings[k - 1] : undefined);
    if (ms < 0 && z[0]) {
      // before 1970 ICU has no metazones: the location, or the offset
      if (generic) return str(z[0]);
    } else {
      let p = z[1];
      for (let k = 1; k < z.length; k++) {
        p = z[k];
        if (p[0] === 0 || ms < p[0]) break;
      }
      if (generic) {
        i = p[long ? 2 : 1];
      } else {
        const o = MathRound(off);
        for (let k = 3; k < p.length; k += 3) {
          if (p[k] === o) {
            i = p[k + (long ? 2 : 1)];
            break;
          }
        }
      }
      if (i) return str(i);
    }
  }
  return long ? gmtLong(off) : gmtShort(off);
}

// ---------------------------------------------------------------------------
// DateTimeFormat

const DateTimeFormatSlots = new WeakMapCtor();
let wkSpaceSet;
function wkSpace() {
  return wkSpaceSet || (wkSpaceSet = new SetCtor(StringPrototypeSplit(D.date.wkSpace, ' ')));
}
const MONTHS_LONG = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
const MONTHS_SHORT = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const MONTHS_NARROW = ['J', 'F', 'M', 'A', 'M', 'J', 'J', 'A', 'S', 'O', 'N', 'D'];
const DAYS_LONG = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
const DAYS_SHORT = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const DAYS_NARROW = ['S', 'M', 'T', 'W', 'T', 'F', 'S'];
const ERAS = { N: ['B', 'A'], s: ['BC', 'AD'], l: ['Before Christ', 'Anno Domini'] };

const WIDTH_NAMES = { N: 'narrow', s: 'short', l: 'long', n: 'numeric', 2: '2-digit' };
const patternCache = new MapCtor();

function parsePattern(p) {
  let toks = MapPrototypeGet(patternCache, p);
  if (toks) return toks;
  toks = [];
  let lit = '';
  for (let i = 0; i < p.length;) {
    const c = p[i];
    if (c === '{' && p[i + 1] === '{') { lit += '{'; i += 2; continue; }
    if (c === '}' && p[i + 1] === '}') { lit += '}'; i += 2; continue; }
    if (c === '{') {
      const j = StringPrototypeIndexOf(p, '}', i);
      if (lit) { ArrayPrototypePush(toks, lit); lit = ''; }
      ArrayPrototypePush(toks, { f: p[i + 1], w: StringPrototypeSlice(p, i + 3, j) });
      i = j + 1;
      continue;
    }
    lit += c;
    i++;
  }
  if (lit) ArrayPrototypePush(toks, lit);
  MapPrototypeSet(patternCache, p, toks);
  return toks;
}

const DTF_WEEKDAY = ['narrow', 'short', 'long'];
const DTF_ERA = ['narrow', 'short', 'long'];
const DTF_YEAR = ['2-digit', 'numeric'];
const DTF_MONTH = ['2-digit', 'numeric', 'narrow', 'short', 'long'];
const DTF_TZN = ['short', 'long', 'shortOffset', 'longOffset', 'shortGeneric', 'longGeneric'];
const DTF_STYLES = ['full', 'long', 'medium', 'short'];

function toDateTimeOptions(opts, required, defaults) {
  // ToDateTimeOptions of ECMA-402 for toLocale*String
  const o = ObjectCreate(opts);
  let need = true;
  if (required === 'date' || required === 'any')
    for (const p of ['weekday', 'year', 'month', 'day']) if (o[p] !== undefined) need = false;
  if (required === 'time' || required === 'any')
    for (const p of ['dayPeriod', 'hour', 'minute', 'second', 'fractionalSecondDigits']) if (o[p] !== undefined) need = false;
  if (o.dateStyle !== undefined || o.timeStyle !== undefined) need = false;
  if (required === 'date' && o.timeStyle !== undefined)
    throw new TypeErrorCtor('Invalid option : timeStyle');
  if (required === 'time' && o.dateStyle !== undefined)
    throw new TypeErrorCtor('Invalid option : dateStyle');
  if (need && (defaults === 'date' || defaults === 'all'))
    for (const p of ['year', 'month', 'day']) ObjectDefineProperty(o, p, { value: 'numeric', writable: true, enumerable: true, configurable: true });
  if (need && (defaults === 'time' || defaults === 'all'))
    for (const p of ['hour', 'minute', 'second']) ObjectDefineProperty(o, p, { value: 'numeric', writable: true, enumerable: true, configurable: true });
  return o;
}

function initDateTimeFormat(dtf, locales, options, required = 'any', defaults = 'date') {
  const requested = canonicalizeLocaleList(locales);
  const opts = toObjectOptions(options);
  const W = 'Intl.DateTimeFormat';
  getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', W);
  const ca = getOption(opts, 'calendar', 'string', undefined, undefined, W);
  if (ca !== undefined && !isWellFormedUnicodeType(ca)) throw new RangeErrorCtor(`Invalid calendar : ${ca}`);
  const nu = getOption(opts, 'numberingSystem', 'string', undefined, undefined, W);
  if (nu !== undefined && !isWellFormedUnicodeType(nu)) throw new RangeErrorCtor(`Invalid numberingSystem : ${nu}`);
  const hour12 = getOption(opts, 'hour12', 'boolean', undefined, undefined, W);
  let hourCycle = getOption(opts, 'hourCycle', 'string', ['h11', 'h12', 'h23', 'h24'], undefined, W);
  if (hour12 !== undefined) hourCycle = null;
  const loc = resolveLocale(requested, ['ca', 'hc', 'nu'], { ca, hc: hourCycle === null ? undefined : hourCycle, nu },
                            { ca: (v) => v === 'gregory', hc: (v) => ArrayPrototypeIncludes(['h11', 'h12', 'h23', 'h24'], v), nu: isNumberingSystem });
  const r = { locale: loc.locale, calendar: 'gregory', numberingSystem: 'latn' };
  if (hourCycle === null) {
    // hour12 overrides any hc keyword: drop it from the locale
    r.locale = StringPrototypeReplace(r.locale, /-hc-h\d\d/, '');
    r.locale = StringPrototypeReplace(r.locale, /-u$/, '');
  }
  let tzName = opts.timeZone;
  let tz;
  if (tzName === undefined) {
    tz = defaultTimeZoneRecord();
  } else {
    tzName = StringCtor(tzName);
    tz = resolveTimeZone(tzName);
    if (!tz) throw new RangeErrorCtor(`Invalid time zone specified: ${tzName}`);
  }
  r.tz = tz;
  r.timeZone = tz.id;
  const f = {};
  f.weekday = getOption(opts, 'weekday', 'string', DTF_WEEKDAY, undefined, W);
  f.era = getOption(opts, 'era', 'string', DTF_ERA, undefined, W);
  f.year = getOption(opts, 'year', 'string', DTF_YEAR, undefined, W);
  f.month = getOption(opts, 'month', 'string', DTF_MONTH, undefined, W);
  f.day = getOption(opts, 'day', 'string', DTF_YEAR, undefined, W);
  f.dayPeriod = getOption(opts, 'dayPeriod', 'string', DTF_WEEKDAY, undefined, W);
  f.hour = getOption(opts, 'hour', 'string', DTF_YEAR, undefined, W);
  f.minute = getOption(opts, 'minute', 'string', DTF_YEAR, undefined, W);
  f.second = getOption(opts, 'second', 'string', DTF_YEAR, undefined, W);
  f.fractionalSecondDigits = getNumberOption(opts, 'fractionalSecondDigits', 1, 3, undefined);
  f.timeZoneName = getOption(opts, 'timeZoneName', 'string', DTF_TZN, undefined, W);
  getOption(opts, 'formatMatcher', 'string', ['basic', 'best fit'], 'best fit', W);
  const dateStyle = getOption(opts, 'dateStyle', 'string', DTF_STYLES, undefined, W);
  const timeStyle = getOption(opts, 'timeStyle', 'string', DTF_STYLES, undefined, W);
  // the hour cycle
  let hc;
  if (hour12 !== undefined) hc = hour12 ? 'h12' : 'h23';
  else if (hourCycle) hc = hourCycle;
  else if (loc.keys.hc) hc = loc.keys.hc;
  r.hcRequested = hc;
  if (dateStyle !== undefined || timeStyle !== undefined) {
    for (const k of ObjectKeys(f))
      if (f[k] !== undefined) throw new TypeErrorCtor('Invalid option : option');
    if (required === 'date' && timeStyle !== undefined) throw new TypeErrorCtor('Invalid option : timeStyle');
    if (required === 'time' && dateStyle !== undefined) throw new TypeErrorCtor('Invalid option : dateStyle');
    r.dateStyle = dateStyle;
    r.timeStyle = timeStyle;
    const ds = dateStyle ? ArrayPrototypeIndexOf(DTF_STYLES, dateStyle) + 1 : 0;
    const ts = timeStyle ? ArrayPrototypeIndexOf(DTF_STYLES, timeStyle) + 1 : 0;
    const c = ts ? hcIndex(hc) : 0;
    r.pattern = D.date.patterns[D.date.styleTable['' + ds + ts + c]];
  } else {
    let need = true;
    if (required === 'date' || required === 'any')
      for (const p of ['weekday', 'year', 'month', 'day']) if (f[p] !== undefined) need = false;
    if (required === 'time' || required === 'any')
      for (const p of ['dayPeriod', 'hour', 'minute', 'second', 'fractionalSecondDigits']) if (f[p] !== undefined) need = false;
    if (need && (defaults === 'date' || defaults === 'all')) { f.year = 'numeric'; f.month = 'numeric'; f.day = 'numeric'; }
    if (need && (defaults === 'time' || defaults === 'all')) { f.hour = 'numeric'; f.minute = 'numeric'; f.second = 'numeric'; }
    r.pattern = selectPattern(f, hc);
    r.fields = f;
    r.hc = hc;
  }
  r.tokens = parsePattern(r.pattern);
  r.tzStyle = f.timeZoneName;
  r.dtfRequired = required;
  WeakMapPrototypeSet(DateTimeFormatSlots, dtf, r);
  return r;
}

function hcIndex(hc) {
  return hc === 'h11' ? 1 : hc === 'h12' ? 2 : hc === 'h23' ? 3 : hc === 'h24' ? 4 : 0;
}

function selectPattern(f, hc) {
  const w = f.weekday ? ArrayPrototypeIndexOf(DTF_WEEKDAY, f.weekday) + 1 : 0;
  const g = f.era ? ArrayPrototypeIndexOf(DTF_ERA, f.era) + 1 : 0;
  const y = f.year ? (f.year === 'numeric' ? 1 : 2) : 0;
  const m = f.month ? ({ numeric: 1, '2-digit': 2, narrow: 3, short: 4, long: 5 })[f.month] : 0;
  const d = f.day ? (f.day === 'numeric' ? 1 : 2) : 0;
  const p = f.dayPeriod ? ArrayPrototypeIndexOf(DTF_WEEKDAY, f.dayPeriod) + 1 : 0;
  const h = f.hour ? (f.hour === 'numeric' ? 1 : 2) : 0;
  const mi = f.minute ? (f.minute === 'numeric' ? 1 : 2) : 0;
  const s = f.second ? (f.second === 'numeric' ? 1 : 2) : 0;
  const fs = f.fractionalSecondDigits || 0;
  const c = h ? hcIndex(hc) : 0;
  const z = f.timeZoneName ? 1 : 0;
  const hasDate = w || y || m || d;
  const hasTime = p || h || mi || s || fs || z;
  const di = (((w * 4 + g) * 3 + y) * 6 + m) * 3 + d;
  const ti = (((((p * 3 + h) * 3 + mi) * 3 + s) * 4 + fs) * 5 + c) * 2 + z;
  const dp = hasDate ? D.date.patterns[D.date.date[di]] : '';
  const tp = hasTime ? D.date.patterns[D.date.time[ti]] : '';
  if (!hasDate) return tp;
  if (!hasTime) return dp;
  const tk = '' + p + h + mi + s + fs + c + z;
  const jk = '' + w + g + y + m + d + '|' + tk;
  if (D.date.joint[jk] !== undefined) return D.date.patterns[D.date.joint[jk]];
  let joiner;
  if (w && !g && !y && !m && !d) joiner = SetPrototypeHas(wkSpace(), tk) ? ' ' : ', ';
  else joiner = D.date.JOINERS[NumberCtor(D.date.joiners[di])];
  return dp + joiner + tp;
}

// the calendar fields of epoch ms in the format's zone
function dateFields(r, ms) {
  const { off, dst } = zoneOffset(r.tz, ms);
  const t = new DateCtor(ms + MathRound(off * 60) * 1000);
  return {
    year: DatePrototypeGetUTCFullYear(t), month: DatePrototypeGetUTCMonth(t), day: DatePrototypeGetUTCDate(t),
    weekday: DatePrototypeGetUTCDay(t), hour: DatePrototypeGetUTCHours(t), minute: DatePrototypeGetUTCMinutes(t),
    second: DatePrototypeGetUTCSeconds(t), ms: DatePrototypeGetUTCMilliseconds(t), off, dst, t: ms,
  };
}

function pad2(n) {
  return n < 10 ? '0' + n : StringCtor(n);
}

const FIELD_TYPES = { E: 'weekday', G: 'era', y: 'year', M: 'month', d: 'day', a: 'dayPeriod', B: 'dayPeriod',
                      h: 'hour', H: 'hour', K: 'hour', k: 'hour', m: 'minute', s: 'second', S: 'fractionalSecond',
                      z: 'timeZoneName' };

function flexibleDayPeriod(F, width, showsMinutes, showsSeconds) {
  const h = F.hour;
  if (h === 12 && (!showsMinutes || F.minute === 0) && (!showsSeconds || F.second === 0))
    return width === 'N' ? 'n' : 'noon';
  if (h < 12) return 'in the morning';
  if (h < 18) return 'in the afternoon';
  if (h < 21) return 'in the evening';
  return 'at night';
}

function renderToken(r, t, F, tokens) {
  switch (t.f) {
    case 'E': return (t.w === 'l' ? DAYS_LONG : t.w === 's' ? DAYS_SHORT : DAYS_NARROW)[F.weekday];
    case 'G': return ERAS[t.w][F.year > 0 ? 1 : 0];
    case 'y': {
      const y = F.year > 0 ? F.year : 1 - F.year;
      return t.w === '2' ? pad2(y % 100) : StringCtor(y);
    }
    case 'M':
      switch (t.w) {
        case 'n': return StringCtor(F.month + 1);
        case '2': return pad2(F.month + 1);
        case 'N': return MONTHS_NARROW[F.month];
        case 's': return MONTHS_SHORT[F.month];
        default: return MONTHS_LONG[F.month];
      }
    case 'd': return t.w === '2' ? pad2(F.day) : StringCtor(F.day);
    case 'a': return F.hour < 12 ? 'AM' : 'PM';
    case 'B': {
      let mins = false, secs = false;
      for (const x of tokens) if (typeof x !== 'string') { if (x.f === 'm') mins = true; if (x.f === 's') secs = true; }
      return flexibleDayPeriod(F, t.w, mins, secs);
    }
    case 'h': case 'H': case 'K': case 'k': {
      let h = F.hour;
      if (t.f === 'h') h = h % 12 || 12;
      else if (t.f === 'K') h = h % 12;
      else if (t.f === 'k') h = h || 24;
      return t.w === '2' ? pad2(h) : StringCtor(h);
    }
    case 'm': return t.w === '2' ? pad2(F.minute) : StringCtor(F.minute);
    case 's': return t.w === '2' ? pad2(F.second) : StringCtor(F.second);
    case 'S': return StringPrototypeSlice(StringPrototypePadStart(StringCtor(F.ms), 3, '0'), 0, NumberCtor(t.w));
    case 'z': return zoneName(r.tz, t.w !== 'x' ? t.w : r.tzStyle || (r.timeStyle === 'full' ? 'long' : 'short'), F.off, F.t);
  }
  return '';
}

function toTimeValue(date) {
  let x;
  if (date === undefined) x = DateNow();
  else x = NumberCtor(date);
  if (!NumberIsFinite(x) || MathAbs(x) > 8.64e15) throw new RangeErrorCtor('Invalid time value');
  return MathTrunc(x) + 0;
}

function formatDateToParts(r, ms, tokens = r.tokens) {
  const F = dateFields(r, ms);
  const parts = [];
  for (const t of tokens) {
    if (typeof t === 'string') {
      const last = parts[parts.length - 1];
      if (last && last.type === 'literal') last.value += t;
      else ArrayPrototypePush(parts, { type: 'literal', value: t });
    } else {
      ArrayPrototypePush(parts, { type: FIELD_TYPES[t.f], value: renderToken(r, t, F, tokens) });
    }
  }
  return parts;
}

// format() (unlike formatToParts) has plain spaces for U+202F, as in V8
function formatDate(r, ms) {
  return StringPrototypeReplace(partsToString(formatDateToParts(r, ms)), /\u202f/g, ' ');
}

function DateTimeFormat(locales, options) {
  const dtf = new.target === undefined ? ObjectCreate(DateTimeFormat.prototype)
    : ObjectCreate(new.target.prototype || DateTimeFormat.prototype);
  initDateTimeFormat(dtf, locales, options);
  return dtf;
}
const DateTimeFormatPrototype = {};
defineClass(DateTimeFormat, 'DateTimeFormat', 0, DateTimeFormatPrototype, 'Intl.DateTimeFormat');
ObjectDefineProperty(DateTimeFormat, 'supportedLocalesOf', {
  value: setFunctionName(function supportedLocalesOf(locales, options = undefined) {
    return supportedLocales(canonicalizeLocaleList(locales), options);
  }, 'supportedLocalesOf'), writable: true, configurable: true,
});

function dtfResolvedOptions(r) {
  const o = { locale: r.locale, calendar: r.calendar, numberingSystem: r.numberingSystem, timeZone: r.timeZone };
  const comps = {};
  let hourLetter = null;
  for (const t of r.tokens) {
    if (typeof t === 'string') continue;
    switch (t.f) {
      case 'E': comps.weekday = WIDTH_NAMES[t.w]; break;
      case 'G': comps.era = WIDTH_NAMES[t.w]; break;
      case 'y': comps.year = WIDTH_NAMES[t.w]; break;
      case 'M': comps.month = WIDTH_NAMES[t.w]; break;
      case 'd': comps.day = WIDTH_NAMES[t.w]; break;
      case 'B': comps.dayPeriod = WIDTH_NAMES[t.w]; break;
      case 'h': case 'H': case 'K': case 'k': hourLetter = t.f; comps.hour = WIDTH_NAMES[t.w]; break;
      case 'm': comps.minute = WIDTH_NAMES[t.w]; break;
      case 's': comps.second = WIDTH_NAMES[t.w]; break;
      case 'S': comps.fractionalSecondDigits = NumberCtor(t.w); break;
      case 'z': if (r.tzStyle) comps.timeZoneName = r.tzStyle; break;
    }
  }
  if (hourLetter) {
    o.hourCycle = { K: 'h11', h: 'h12', H: 'h23', k: 'h24' }[hourLetter];
    o.hour12 = hourLetter === 'K' || hourLetter === 'h';
  }
  if (r.dateStyle !== undefined || r.timeStyle !== undefined) {
    if (r.dateStyle !== undefined) o.dateStyle = r.dateStyle;
    if (r.timeStyle !== undefined) o.timeStyle = r.timeStyle;
    return o;
  }
  for (const k of ['weekday', 'era', 'year', 'month', 'day', 'dayPeriod', 'hour', 'minute', 'second',
                   'fractionalSecondDigits', 'timeZoneName'])
    if (comps[k] !== undefined) o[k] = comps[k];
  return o;
}

const DATE_LETTERS = 'GyMdE', TIME_LETTERS = 'hHKkmsSaBz';
const FIELD_LEVEL = { G: 0, y: 1, M: 2, d: 3, E: 3, a: 4, B: 4, h: 5, H: 5, K: 5, k: 5, m: 6, s: 7, S: 8 };

function tokensHave(tokens, letters) {
  for (const t of tokens) if (typeof t !== 'string' && StringPrototypeIndexOf(letters, t.f) >= 0) return true;
  return false;
}

// tokens without the field `letters` and the literal before it; and that
// literal with the field
function stripField(tokens, letters) {
  for (let i = 0; i < tokens.length; i++) {
    const t = tokens[i];
    if (typeof t === 'string' || StringPrototypeIndexOf(letters, t.f) < 0) continue;
    const from = i > 0 && typeof tokens[i - 1] === 'string' ? i - 1 : i;
    return { rest: [...tokens.slice(0, from), ...tokens.slice(i + 1)], field: tokens.slice(from, i + 1), last: i === tokens.length - 1 };
  }
  return { rest: tokens, field: [], last: false };
}

// formatRange: an approximation of ICU's DateIntervalFormat with the en
// interval patterns (the greatest differing field picks the pattern)
function formatDateRangeToParts(r, x, y) {
  if (x === undefined || y === undefined) throw new TypeErrorCtor('startDate or endDate is undefined');
  const a = toTimeValue(x), b = toTimeValue(y);
  const tokens = r.tokens;
  const mark = (parts, source) => parts.map((p) => ({ type: p.type, value: p.value, source }));
  const Fa = dateFields(r, a), Fb = dateFields(r, b);
  let greatest;
  if ((Fa.year > 0) !== (Fb.year > 0)) greatest = 'G';
  else if (Fa.year !== Fb.year) greatest = 'y';
  else if (Fa.month !== Fb.month) greatest = 'M';
  else if (Fa.day !== Fb.day) greatest = 'd';
  else if ((Fa.hour < 12) !== (Fb.hour < 12)) greatest = 'a';
  else if (Fa.hour !== Fb.hour) greatest = 'h';
  else if (Fa.minute !== Fb.minute) greatest = 'm';
  else if (Fa.second !== Fb.second) greatest = 's';
  else greatest = 'S';
  // one date when the fields that differ are all smaller than the
  // pattern's smallest
  let smallest = -1;
  for (const t of tokens) if (typeof t !== 'string') smallest = MathMax(smallest, FIELD_LEVEL[t.f] ?? -1);
  if (FIELD_LEVEL[greatest] > smallest || (a === b))
    return { parts: mark(formatDateToParts(r, a), 'shared'), single: true };
  const sep = { type: 'literal', value: D.date.rangeSeparator, source: 'shared' };
  const side = (ms, toks, source) => mark(formatDateToParts(r, ms, rangeTokens(toks)), source);
  const fallback = (toks) => [...side(a, toks, 'startRange'), sep, ...side(b, toks, 'endRange')];
  const hasDate = tokensHave(tokens, DATE_LETTERS), hasTime = tokensHave(tokens, TIME_LETTERS);
  if (StringPrototypeIndexOf('GyMd', greatest) >= 0) {
    if (hasTime) {
      // a time alone would be ambiguous: with the numeric date
      if (!hasDate) return { parts: fallback(parsePattern(D.date.patterns[D.date.date[(((0 * 4 + 0) * 3 + 1) * 6 + 1) * 3 + 1]] + ', ' + r.pattern)) };
      return { parts: fallback(extendRangeTokens(r, tokens, greatest) || tokens) };
    }
    return { parts: dateOnlyRange(r, a, b, tokens, greatest, side, sep, true) };
  }
  // the same day: the date shared, then a time range
  let first = 0;
  while (first < tokens.length && (typeof tokens[first] === 'string' || StringPrototypeIndexOf(TIME_LETTERS, tokens[first].f) < 0)) first++;
  let dateEnd = first;
  while (dateEnd > 0 && typeof tokens[dateEnd - 1] === 'string') dateEnd--;
  let res = [];
  if (hasDate) res = side(a, [...tokens.slice(0, dateEnd), ', '], 'shared');
  let time = tokens.slice(first);
  const seconds = tokensHave(time, 'sS');
  if (hasDate || !seconds) {
    // interval patterns have short zone names, and no GMT offsets
    const style = r.tzStyle || (r.timeStyle === 'full' ? 'long' : 'short');
    if (style === 'shortOffset' || style === 'longOffset') {
      time = stripField(time, 'z').rest;
    } else {
      const w = style === 'long' ? 'short' : style === 'longGeneric' ? 'shortGeneric' : style;
      time = time.map((t) => (typeof t !== 'string' && t.f === 'z' ? { f: 'z', w } : t));
    }
  }
  if (seconds) return { parts: [...res, ...fallback(time)] };
  const zone = stripField(time, 'z');
  if (zone.field.length && zone.last) time = zone.rest;
  const zoneParts = zone.field.length && zone.last ? side(a, zone.field, 'shared') : [];
  const ap = stripField(time, 'aB');
  if (ap.field.length && ap.last && greatest !== 'a') {
    res = [...res, ...side(a, ap.rest, 'startRange'), sep, ...side(b, ap.rest, 'endRange'), ...side(b, ap.field, 'shared')];
  } else {
    // en's H patterns are HH:mm in ranges
    if (!ap.field.length) time = time.map((t) => (typeof t !== 'string' && t.f === 'H' ? { f: 'H', w: '2' } : t));
    res = [...res, ...side(a, time, 'startRange'), sep, ...side(b, time, 'endRange')];
  }
  return { parts: [...res, ...zoneParts] };
}

// a day without the differing month or year gets them (as ICU does)
function extendRangeTokens(r, tokens, greatest) {
  const hasD = tokensHave(tokens, 'd'), hasY = tokensHave(tokens, 'y'), hasM = tokensHave(tokens, 'M');
  if (!hasD || !r.fields || !((!hasY && (greatest === 'y' || greatest === 'G')) || (!hasM && greatest !== 'd')))
    return null;
  const f = { ...r.fields };
  if (greatest === 'y' || greatest === 'G') f.year = f.year || 'numeric';
  f.month = f.month || 'numeric';
  return parsePattern(selectPattern(f, r.hc));
}

// ICU's interval skeletons: numeric hours (h, K), and numeric months and
// days unless the year is 2-digit
function rangeTokens(tokens) {
  const y2 = tokens.some((t) => typeof t !== 'string' && t.f === 'y' && t.w === '2');
  return tokens.map((t) => {
    if (typeof t === 'string' || t.w !== '2') return t;
    if (t.f === 'h' || t.f === 'K' || (!y2 && (t.f === 'M' || t.f === 'd'))) return { f: t.f, w: 'n' };
    return t;
  });
}

function dateOnlyRange(r, a, b, tokens, greatest, side, sep, extend) {
  const fallback = () => [...side(a, tokens, 'startRange'), sep, ...side(b, tokens, 'endRange')];
  const hasD = tokensHave(tokens, 'd'), hasY = tokensHave(tokens, 'y'), hasM = tokensHave(tokens, 'M');
  const ext = extend && extendRangeTokens(r, tokens, greatest);
  if (ext) return dateOnlyRange(r, a, b, ext, greatest, side, sep, false);
  let textMonth = false;
  for (const t of tokens) if (typeof t !== 'string' && t.f === 'M' && (t.w === 's' || t.w === 'l' || t.w === 'N')) textMonth = true;
  if (!textMonth || greatest === 'y' || greatest === 'G') return fallback();
  const year = stripField(tokens, 'y');
  if (year.field.length && !year.last) return fallback();
  const toks = year.rest;
  const yearParts = year.field.length ? side(a, year.field, 'shared') : [];
  if (greatest === 'M' || tokensHave(toks, 'E') || !hasD)
    return [...side(a, toks, 'startRange'), sep, ...side(b, toks, 'endRange'), ...yearParts];
  // the day differs: "Jan 5 – 8, 2024"
  let i = 0;
  while (i < toks.length && (typeof toks[i] === 'string' || toks[i].f !== 'd')) i++;
  return [...side(a, toks.slice(0, i), 'shared'), ...side(a, toks.slice(i), 'startRange'), sep,
          ...side(b, toks.slice(i), 'endRange'), ...yearParts];
}

defineMethods(DateTimeFormatPrototype, {
  resolvedOptions() {
    return dtfResolvedOptions(unwrap(DateTimeFormatSlots, this, 'Intl.DateTimeFormat.prototype.resolvedOptions'));
  },
  formatToParts(date) {
    const r = unwrap(DateTimeFormatSlots, this, 'Intl.DateTimeFormat.prototype.formatToParts');
    return formatDateToParts(r, toTimeValue(date));
  },
  formatRange(startDate, endDate) {
    const r = unwrap(DateTimeFormatSlots, this, 'Intl.DateTimeFormat.prototype.formatRange');
    const { parts, single } = formatDateRangeToParts(r, startDate, endDate);
    const s = partsToString(parts);
    return single ? StringPrototypeReplace(s, /\u202f/g, ' ') : s;
  },
  formatRangeToParts(startDate, endDate) {
    const r = unwrap(DateTimeFormatSlots, this, 'Intl.DateTimeFormat.prototype.formatRangeToParts');
    return formatDateRangeToParts(r, startDate, endDate).parts;
  },
});
defineGetter(DateTimeFormatPrototype, 'format', function format() {
  const r = unwrap(DateTimeFormatSlots, this, 'get Intl.DateTimeFormat.prototype.format');
  if (!r.boundFormat) r.boundFormat = setFunctionName((date) => formatDate(r, toTimeValue(date)), '');
  return r.boundFormat;
});

// ---------------------------------------------------------------------------
// PluralRules

const PluralRulesSlots = new WeakMapCtor();

function PluralRules(locales, options) {
  if (new.target === undefined) throw new TypeErrorCtor("Constructor Intl.PluralRules requires 'new'");
  const pr = ObjectCreate(new.target.prototype || PluralRules.prototype);
  const requested = canonicalizeLocaleList(locales);
  const opts = toObjectOptions(options);
  getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', 'Intl.PluralRules');
  const type = getOption(opts, 'type', 'string', ['cardinal', 'ordinal'], 'cardinal', 'Intl.PluralRules');
  const r = { type };
  setDigitOptions(r, opts, 0, 3, 'standard');
  r.locale = resolveLocale(requested, [], null, {}, 'plural').locale;
  r.useGrouping = false;
  r.minimumIntegerDigits = r.minimumIntegerDigits || 1;
  WeakMapPrototypeSet(PluralRulesSlots, pr, r);
  return pr;
}

function pluralSelect(r, n) {
  const x = toIntlMV(NumberCtor(n));
  if (x.nan || x.inf) return 'other';
  const rounded = applyRounding(r, { ...x, neg: false });
  const { int, frac } = toDigitStrings({ ...r, minimumIntegerDigits: 1 }, rounded);
  return r.type === 'ordinal' ? pluralOrdinal(int, frac) : pluralCardinal(int, frac);
}

const PluralRulesPrototype = {};
defineClass(PluralRules, 'PluralRules', 0, PluralRulesPrototype, 'Intl.PluralRules');
ObjectDefineProperty(PluralRules, 'supportedLocalesOf', {
  value: setFunctionName(function supportedLocalesOf(locales, options = undefined) {
    return supportedLocales(canonicalizeLocaleList(locales), options, 'plural');
  }, 'supportedLocalesOf'), writable: true, configurable: true,
});
defineMethods(PluralRulesPrototype, {
  select(value) {
    return pluralSelect(unwrap(PluralRulesSlots, this, 'Intl.PluralRules.prototype.select'), value);
  },
  selectRange(start, end) {
    const r = unwrap(PluralRulesSlots, this, 'Intl.PluralRules.prototype.selectRange');
    if (start === undefined || end === undefined) throw new TypeErrorCtor('start or end is undefined');
    const x = NumberCtor(start), y = NumberCtor(end);
    if (NumberIsNaN(x) || NumberIsNaN(y)) throw new RangeErrorCtor('Invalid number range');
    const a = pluralSelect(r, x), b = pluralSelect(r, y);
    if (r.type === 'ordinal') return b;
    return a === 'other' && b === 'one' ? 'one' : 'other';
  },
  resolvedOptions() {
    const r = unwrap(PluralRulesSlots, this, 'Intl.PluralRules.prototype.resolvedOptions');
    const o = { locale: r.locale, type: r.type, minimumIntegerDigits: r.minimumIntegerDigits };
    if (r.roundingType !== 'significantDigits') {
      o.minimumFractionDigits = r.minimumFractionDigits;
      o.maximumFractionDigits = r.maximumFractionDigits;
    }
    if (r.roundingType !== 'fractionDigits') {
      o.minimumSignificantDigits = r.minimumSignificantDigits;
      o.maximumSignificantDigits = r.maximumSignificantDigits;
    }
    o.pluralCategories = r.type === 'ordinal' ? ['one', 'two', 'few', 'other'] : ['one', 'other'];
    o.roundingIncrement = r.roundingIncrement;
    o.roundingMode = r.roundingMode;
    o.roundingPriority = r.roundingPriority;
    o.trailingZeroDisplay = r.trailingZeroDisplay;
    return o;
  },
});

// ---------------------------------------------------------------------------
// RelativeTimeFormat

const RelativeTimeFormatSlots = new WeakMapCtor();
const RT_UNITS = ['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'];

function RelativeTimeFormat(locales, options) {
  if (new.target === undefined) throw new TypeErrorCtor("Constructor Intl.RelativeTimeFormat requires 'new'");
  const rtf = ObjectCreate(new.target.prototype || RelativeTimeFormat.prototype);
  const requested = canonicalizeLocaleList(locales);
  const opts = getOptionsObject(options);
  const W = 'Intl.RelativeTimeFormat';
  getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', W);
  const nu = getOption(opts, 'numberingSystem', 'string', undefined, undefined, W);
  if (nu !== undefined && !isWellFormedUnicodeType(nu)) throw new RangeErrorCtor(`Invalid numberingSystem : ${nu}`);
  const loc = resolveLocale(requested, ['nu'], { nu }, { nu: isNumberingSystem });
  const style = getOption(opts, 'style', 'string', ['long', 'short', 'narrow'], 'long', W);
  const numeric = getOption(opts, 'numeric', 'string', ['always', 'auto'], 'always', W);
  const nf = ObjectCreate(null);
  const nfr = initNumberFormat(nf, [loc.dataLocale], undefined);
  WeakMapPrototypeSet(RelativeTimeFormatSlots, rtf, { locale: loc.locale, style, numeric, numberingSystem: 'latn', nfr });
  return rtf;
}

function singularUnit(unit, method) {
  let u = StringCtor(unit);
  if (u[u.length - 1] === 's' && ArrayPrototypeIncludes(RT_UNITS, StringPrototypeSlice(u, 0, -1))) u = StringPrototypeSlice(u, 0, -1);
  if (!ArrayPrototypeIncludes(RT_UNITS, u))
    throw new RangeErrorCtor(`Invalid unit argument for ${method}() '${unit}'`);
  return u;
}

function formatRelativeToParts(r, value, unit, method) {
  value = NumberCtor(value);
  const u = singularUnit(unit, method);
  if (!NumberIsFinite(value)) throw new RangeErrorCtor(`Value need to be finite number for ${method}()`);
  const data = D.misc.rt[r.style][u];
  if (r.numeric === 'auto' && value > -2.1 && value < 2.1) {
    // ICU: offsets within 1% of -2..2 have their words ("tomorrow")
    const x100 = value * 100;
    const k = x100 < 0 ? MathTrunc(x100 - 0.5) : MathTrunc(x100 + 0.5);
    const key = k % 100 === 0 ? StringCtor(k / 100) : undefined;
    if (key !== undefined && data.auto[key] !== undefined) return [{ type: 'literal', value: data.auto[key] }];
  }
  const past = value < 0 || ObjectIs(value, -0);
  const x = toIntlMV(MathAbs(value));
  const core = numberCore(r.nfr, x);
  let pattern;
  if (value === 0) pattern = past ? data.nz : data.z;
  else if (past) pattern = core.plural === 'one' ? data.p1 : data.p2;
  else pattern = core.plural === 'one' ? data.f1 : data.f2;
  const i = StringPrototypeIndexOf(pattern, '{0}');
  const res = [];
  if (i > 0) ArrayPrototypePush(res, { type: 'literal', value: StringPrototypeSlice(pattern, 0, i) });
  for (const p of core.parts) ArrayPrototypePush(res, { type: p.type, value: p.value, unit: u });
  if (i + 3 < pattern.length) ArrayPrototypePush(res, { type: 'literal', value: StringPrototypeSlice(pattern, i + 3) });
  return res;
}

const RelativeTimeFormatPrototype = {};
defineClass(RelativeTimeFormat, 'RelativeTimeFormat', 0, RelativeTimeFormatPrototype, 'Intl.RelativeTimeFormat');
ObjectDefineProperty(RelativeTimeFormat, 'supportedLocalesOf', {
  value: setFunctionName(function supportedLocalesOf(locales, options = undefined) {
    return supportedLocales(canonicalizeLocaleList(locales), options);
  }, 'supportedLocalesOf'), writable: true, configurable: true,
});
defineMethods(RelativeTimeFormatPrototype, {
  format(value, unit) {
    const r = unwrap(RelativeTimeFormatSlots, this, 'Intl.RelativeTimeFormat.prototype.format');
    return partsToString(formatRelativeToParts(r, value, unit, 'Intl.RelativeTimeFormat.prototype.format'));
  },
  formatToParts(value, unit) {
    const r = unwrap(RelativeTimeFormatSlots, this, 'Intl.RelativeTimeFormat.prototype.formatToParts');
    return formatRelativeToParts(r, value, unit, 'Intl.RelativeTimeFormat.prototype.formatToParts');
  },
  resolvedOptions() {
    const r = unwrap(RelativeTimeFormatSlots, this, 'Intl.RelativeTimeFormat.prototype.resolvedOptions');
    return { locale: r.locale, style: r.style, numeric: r.numeric, numberingSystem: r.numberingSystem };
  },
});

// ---------------------------------------------------------------------------
// ListFormat

const ListFormatSlots = new WeakMapCtor();

function ListFormat(locales, options) {
  if (new.target === undefined) throw new TypeErrorCtor("Constructor Intl.ListFormat requires 'new'");
  const lf = ObjectCreate(new.target.prototype || ListFormat.prototype);
  const requested = canonicalizeLocaleList(locales);
  const opts = getOptionsObject(options);
  const W = 'Intl.ListFormat';
  getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', W);
  const loc = resolveLocale(requested, [], null, {}, 'list');
  const type = getOption(opts, 'type', 'string', ['conjunction', 'disjunction', 'unit'], 'conjunction', W);
  const style = getOption(opts, 'style', 'string', ['long', 'short', 'narrow'], 'long', W);
  WeakMapPrototypeSet(ListFormatSlots, lf, { locale: loc.locale, type, style, p: D.misc.lists[type[0] + style[0]] });
  return lf;
}

function stringListFromIterable(iterable) {
  if (iterable === undefined) return [];
  const res = [];
  for (const v of iterable) {
    if (typeof v !== 'string') throw new TypeErrorCtor(`Iterable yielded ${describeValue(v)} which is not a string`);
    ArrayPrototypePush(res, v);
  }
  return res;
}

function describeValue(v) {
  if (typeof v === 'string') return JSON.stringify(v);
  if (typeof v === 'object' && v !== null) return ArrayIsArray(v) ? '[object Array]' : '[object Object]';
  if (typeof v === 'symbol') return v.toString();
  return StringCtor(v);
}

function formatListToParts(r, list) {
  const n = list.length;
  const el = (v) => ({ type: 'element', value: v });
  if (n === 0) return [];
  if (n === 1) return [el(list[0])];
  const apply = (pattern, a, b) => {
    // pattern "{0}X{1}" with a, b part arrays
    const i0 = StringPrototypeIndexOf(pattern, '{0}'), i1 = StringPrototypeIndexOf(pattern, '{1}');
    const res = [];
    const lit = (s) => { if (s) ArrayPrototypePush(res, { type: 'literal', value: s }); };
    lit(StringPrototypeSlice(pattern, 0, i0));
    for (const p of a) ArrayPrototypePush(res, p);
    lit(StringPrototypeSlice(pattern, i0 + 3, i1));
    for (const p of b) ArrayPrototypePush(res, p);
    lit(StringPrototypeSlice(pattern, i1 + 3));
    return res;
  };
  if (n === 2) return apply(r.p.pair, [el(list[0])], [el(list[1])]);
  let parts = apply(r.p.end, [el(list[n - 2])], [el(list[n - 1])]);
  for (let i = n - 3; i >= 0; i--) parts = apply(i === 0 ? r.p.start : r.p.middle, [el(list[i])], parts);
  return parts;
}

const ListFormatPrototype = {};
defineClass(ListFormat, 'ListFormat', 0, ListFormatPrototype, 'Intl.ListFormat');
ObjectDefineProperty(ListFormat, 'supportedLocalesOf', {
  value: setFunctionName(function supportedLocalesOf(locales, options = undefined) {
    return supportedLocales(canonicalizeLocaleList(locales), options, 'list');
  }, 'supportedLocalesOf'), writable: true, configurable: true,
});
defineMethods(ListFormatPrototype, {
  format(list) {
    const r = unwrap(ListFormatSlots, this, 'Intl.ListFormat.prototype.format');
    return partsToString(formatListToParts(r, stringListFromIterable(list)));
  },
  formatToParts(list) {
    const r = unwrap(ListFormatSlots, this, 'Intl.ListFormat.prototype.formatToParts');
    return formatListToParts(r, stringListFromIterable(list));
  },
  resolvedOptions() {
    const r = unwrap(ListFormatSlots, this, 'Intl.ListFormat.prototype.resolvedOptions');
    return { locale: r.locale, type: r.type, style: r.style };
  },
});

// ---------------------------------------------------------------------------
// Collator: a multi-level comparison approximating CLDR root collation
// (variable characters, then digits, then letters by script; accents at
// the second level, case at the third, lower case first)

const CollatorSlots = new WeakMapCtor();
// Primary weights: spaces and punctuation ("variable"), symbols, currency,
// digits, then letters by script (Latin, Greek, Cyrillic, ..., Hangul,
// kana, other scripts, Han last); common punctuation and symbols in CLDR
// root order from the data, the others by code point within their group.
const SPACE_ORDER = '\t\n\v\f\r\u0085   ';
const P_SYMBOL = 4000, P_DIGIT = 10000, P_LATIN = 20000, P_OTHER = 30000, P_HAN = 0x200000;
let collationOrder;
function punctWeights() {
  if (collationOrder) return collationOrder;
  const C = D.collation, map = new MapCtor();
  let g = -1;
  let tertiary = 0;
  for (const ch of C.order) {
    if (ch === '\x7f') {
      tertiary++;
      continue;
    }
    if (tertiary === 0) g++;
    MapPrototypeSet(map, ch, { g, t: tertiary });
    tertiary = 0;
  }
  const marks = new MapCtor();
  let r = -1;
  let same = false;
  for (const ch of C.marks) {
    if (ch === '\x7f') {
      same = true;
      continue;
    }
    if (!same) r++;
    MapPrototypeSet(marks, ch, r);
    same = false;
  }
  return (collationOrder = { map, symbols: C.symbols, currency: C.currency, marks, special: C.special });
}

function markWeight(m) {
  const w = MapPrototypeGet(punctWeights().marks, m);
  return w !== undefined ? w : 1000 + StringPrototypeCodePointAt(m, 0) / 0x110000;
}

function compareSecondary(a, b) {
  const n = MathMin(a.length, b.length);
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  return a.length === b.length ? 0 : a.length < b.length ? -1 : 1;
}
const latinP = (c) => P_LATIN + (StringPrototypeCharCodeAt(c, 0) - 97) * 4;
// letters of their own after a Latin letter (the letters that are others
// with a secondary difference, like ø and æ, are in the data)
const LATIN_AFTER = { 'ı': ['i', 1], 'ŋ': ['n', 1], 'ŧ': ['t', 1], 'þ': ['z', 2], 'ĸ': ['q', 1], 'ƀ': ['b', 1],
                      'ɨ': ['i', 2], 'ƶ': ['z', 1], 'ǥ': ['g', 1] };
// small kana and the kana they are small forms of
const SMALL_KANA = { 'ぁ': 'あ', 'ぃ': 'い', 'ぅ': 'う', 'ぇ': 'え', 'ぉ': 'お', 'っ': 'つ', 'ゃ': 'や', 'ゅ': 'ゆ', 'ょ': 'よ',
                     'ゎ': 'わ', 'ゕ': 'か', 'ゖ': 'け' };

// primary weight of one character (lower case, decomposed, hiragana)
function primaryWeight(c, cp) {
  let i = StringPrototypeIndexOf(SPACE_ORDER, c);
  if (i >= 0) return 100 + i;
  if (cp >= 97 && cp <= 122) return latinP(c);
  if (cp >= 48 && cp <= 57) return P_DIGIT + (cp - 48);
  if (LATIN_AFTER[c]) return latinP(LATIN_AFTER[c][0]) + LATIN_AFTER[c][1];
  const P = punctWeights();
  const e = MapPrototypeGet(P.map, c);
  if (e) return 200 + e.g + e.t / 64;
  if (RegExpPrototypeTest(/\p{White_Space}/u, c)) return 100 + SPACE_ORDER.length;
  if (RegExpPrototypeTest(/\p{P}/u, c)) return 200 + P.symbols - 1 + cp / 0x110000;
  if (RegExpPrototypeTest(/\p{Sc}/u, c)) return 200 + P.currency + 900 + cp / 0x110000;
  if (RegExpPrototypeTest(/\p{S}/u, c)) return 200 + P.currency - 1 + cp / 0x110000;
  if (RegExpPrototypeTest(/\p{Nd}/u, c)) {
    // other scripts' digits sort with the ASCII ones
    let v = 0;
    for (let base = cp; base > cp - 10; base--) {
      if (!RegExpPrototypeTest(/\p{Nd}/u, StringCtor.fromCodePoint(base - 1))) { v = cp - base; break; }
    }
    return P_DIGIT + v;
  }
  if (RegExpPrototypeTest(/\p{Script=Han}/u, c)) return P_HAN + cp;
  return P_OTHER + cp;
}

// tertiary weights (UCA): by the compatibility decomposition type of the
// character and its case; kana by size and script
function tertiaryWeight(orig, ocp, upper) {
  let t = 0; // none
  if (ocp >= 0xff01 && ocp <= 0xff5e || ocp === 0x3000 || (ocp >= 0xffe0 && ocp <= 0xffe6)) t = 1; // wide
  else if (ocp >= 0x2460 && ocp <= 0x24ff && !(ocp >= 0x2474 && ocp <= 0x249b)) t = 4; // circle
  else if (ocp >= 0x1d400 && ocp <= 0x1d7ff || (ocp >= 0x2102 && ocp <= 0x2149)) t = 3; // font
  else if (ocp === 0xaa || ocp === 0xba || ocp === 0xb9 || ocp === 0xb2 || ocp === 0xb3 || (ocp >= 0x2070 && ocp <= 0x207f) ||
           (ocp >= 0x2b0 && ocp <= 0x2b8) || (ocp >= 0x1d2c && ocp <= 0x1d61) || (ocp >= 0x2e0 && ocp <= 0x2e4)) return upper ? 0x1d : 0x14;
  else if ((ocp >= 0x2080 && ocp <= 0x209c) || (ocp >= 0x1d62 && ocp <= 0x1d6a)) return upper ? 0x1d : 0x15;
  else if (ocp === 0xa0 || ocp === 0x2007 || ocp === 0x2011 || ocp === 0x202f) return 0x1b; // noBreak
  else if (ocp >= 0x3300 && ocp <= 0x33ff) return 0x1c; // square
  else if ((ocp >= 0xbc && ocp <= 0xbe) || (ocp >= 0x2150 && ocp <= 0x215f)) return 0x1e; // fraction
  else if (ocp >= 0xfe10 && ocp <= 0xfe48) return 0x16; // vertical
  else if (orig) t = 2; // other compatibility forms
  return (upper ? 8 : 2) + t;
}

function kanaTertiary(ocp, small) {
  if (ocp >= 0x3041 && ocp <= 0x309f) return small ? 0xd : 0xe;
  if (ocp >= 0xff66 && ocp <= 0xff9d) return small ? 0x10 : 0x12;
  if (ocp >= 0x32d0 && ocp <= 0x32fe) return 0x13;
  return small ? 0xf : 0x11;
}

// collation elements: { p, sec, ter, up } per base character; marks join
// the element before as secondary weights
function collationElements(s, ignorePunctuation) {
  const els = [];
  const special = punctWeights().special;
  const push = (ch, ocp, compat) => {
    if (RegExpPrototypeTest(/\p{M}/u, ch)) {
      const last = els[els.length - 1];
      if (last) ArrayPrototypePush(last.sec, markWeight(ch));
      return;
    }
    let lower = StringPrototypeToLowerCase(ch);
    const cp = StringPrototypeCodePointAt(ch, 0);
    if (lower.length > 2 || (StringPrototypeCodePointAt(lower, 0) > 0xffff) !== (cp > 0xffff)) lower = ch;
    const up = lower !== ch;
    const l = special[lower];
    if (l) {
      for (let i = 0; i < l[0].length; i++)
        ArrayPrototypePush(els, { p: latinP(l[0][i]), sec: i === 0 ? [l[1]] : [], ter: tertiaryWeight(compat, ocp, up), up });
      return;
    }
    let lcp = StringPrototypeCodePointAt(lower, 0);
    let ter;
    const kata = (lcp >= 0x30a1 && lcp <= 0x30fa) || lcp === 0x30fd || lcp === 0x30fe;
    if (kata || (lcp >= 0x3041 && lcp <= 0x3096) || lcp === 0x309d || lcp === 0x309e) {
      // kana: the hiragana; size and script are tertiary
      if (kata) lcp -= 0x60;
      lower = StringCtor.fromCodePoint(lcp);
      const big = SMALL_KANA[lower];
      if (big) lower = big;
      ter = kanaTertiary(ocp, !!big);
      lcp = StringPrototypeCodePointAt(lower, 0);
    } else {
      ter = tertiaryWeight(compat, ocp, up);
    }
    const p = primaryWeight(lower, lcp);
    // digits from compatibility characters (①) are not numbers for numeric
    const nd = p >= P_DIGIT && p < P_DIGIT + 10 && !compat;
    // ignored punctuation still ends a number
    const ignored = ignorePunctuation && p < 200 + punctWeights().symbols;
    ArrayPrototypePush(els, { p, sec: [], ter, up, nd, ignored });
  };
  for (const ch of StringPrototypeNormalize(s, 'NFD')) {
    const ocp = StringPrototypeCodePointAt(ch, 0);
    // ignorable: controls (but not the spaces among them) and formats
    if (RegExpPrototypeTest(/[\p{Cc}\p{Cf}]/u, ch) && StringPrototypeIndexOf(SPACE_ORDER, ch) < 0) continue;
    const k = StringPrototypeNormalize(ch, 'NFKD');
    if (k !== ch && !special[StringPrototypeToLowerCase(ch)]) {
      // compatibility characters: their decomposition, tertiary different
      for (const c of k) push(c, ocp, true);
    } else {
      push(ch, ocp, false);
    }
  }
  return els;
}

function collatorCompare(r, x, y) {
  if (x === y) return 0;
  let a = collationElements(x, r.ignorePunctuation), b = collationElements(y, r.ignorePunctuation);
  if (r.numeric) {
    // digit runs become one element compared by value
    const fold = (els) => {
      const res = [];
      for (let i = 0; i < els.length;) {
        if (els[i].nd) {
          let j = i, v = '';
          while (j < els.length && els[j].nd) v += StringCtor(els[j++].p - P_DIGIT);
          v = StringPrototypeReplace(v, /^0+(?=\d)/, '');
          ArrayPrototypePush(res, { p: P_DIGIT, num: v, sec: [], ter: 2, up: false });
          i = j;
        } else if (els[i].p >= P_DIGIT && els[i].p < P_DIGIT + 10) {
          // after all numbers
          ArrayPrototypePush(res, { ...els[i], p: els[i].p + 10 });
          i++;
        } else ArrayPrototypePush(res, els[i++]);
      }
      return res;
    };
    a = fold(a);
    b = fold(b);
  }
  if (r.ignorePunctuation) {
    a = a.filter((e) => !e.ignored);
    b = b.filter((e) => !e.ignored);
  }
  const n = MathMin(a.length, b.length);
  for (let i = 0; i < n; i++) {
    const ea = a[i], eb = b[i];
    if (ea.num !== undefined && eb.num !== undefined) {
      if (ea.num.length !== eb.num.length) return ea.num.length < eb.num.length ? -1 : 1;
      if (ea.num !== eb.num) return ea.num < eb.num ? -1 : 1;
      continue;
    }
    if (ea.p !== eb.p) return ea.p < eb.p ? -1 : 1;
  }
  if (a.length !== b.length) return a.length < b.length ? -1 : 1;
  if (r.sensitivity === 'base') return 0;
  if (r.sensitivity !== 'case') {
    for (let i = 0; i < n; i++) {
      const c = compareSecondary(a[i].sec, b[i].sec);
      if (c) return c;
    }
  }
  if (r.sensitivity === 'accent') return 0;
  // case first (when asked, or alone for sensitivity "case"), then the rest
  // of the tertiary weights
  if (r.caseFirst === 'upper' || r.caseFirst === 'lower' || r.sensitivity === 'case') {
    const upperFirst = r.caseFirst === 'upper';
    for (let i = 0; i < n; i++)
      if (a[i].up !== b[i].up) return a[i].up === upperFirst ? -1 : 1;
    if (r.sensitivity === 'case') return 0;
  }
  for (let i = 0; i < n; i++)
    if (a[i].ter !== b[i].ter) return a[i].ter < b[i].ter ? -1 : 1;
  return 0;
}

// the collations of a locale (beyond the root order: emoji and eor) and
// its default (Chinese: pinyin, or stroke for Traditional)
function localeCollations(locale) {
  const extra = D.locales.collations[parseLanguageTag(locale).language];
  return ['emoji', 'eor', ...(extra ? StringPrototypeSplit(extra, ' ') : [])];
}

function defaultCollation(locale) {
  const t = parseLanguageTag(locale);
  if (t.language !== 'zh') return 'standard';
  return likelyAdd(t).script === 'Hant' ? 'stroke' : 'pinyin';
}

function Collator(locales, options) {
  const col = new.target === undefined ? ObjectCreate(Collator.prototype)
    : ObjectCreate(new.target.prototype || Collator.prototype);
  const requested = canonicalizeLocaleList(locales);
  const opts = toObjectOptions(options);
  const W = 'Intl.Collator';
  const usage = getOption(opts, 'usage', 'string', ['sort', 'search'], 'sort', W);
  getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', W);
  const co = getOption(opts, 'collation', 'string', undefined, undefined, W);
  if (co !== undefined && !isWellFormedUnicodeType(co)) throw new RangeErrorCtor(`Invalid collation : ${co}`);
  let kn = getOption(opts, 'numeric', 'boolean', undefined, undefined, W);
  if (kn !== undefined) kn = kn ? 'true' : 'false';
  const kf = getOption(opts, 'caseFirst', 'string', ['upper', 'lower', 'false'], undefined, W);
  const loc = resolveLocale(requested, ['co', 'kf', 'kn'], { co, kf, kn: kn === 'true' ? '' : kn },
                            { co: (v, found) => ArrayPrototypeIncludes(localeCollations(found), v),
                              kf: (v) => ArrayPrototypeIncludes(['upper', 'lower', 'false'], v),
                              kn: (v) => v === '' || v === 'true' || v === 'false' }, 'collator');
  const sensitivity = getOption(opts, 'sensitivity', 'string', ['base', 'accent', 'case', 'variant'], 'variant', W);
  const ignorePunctuation = getOption(opts, 'ignorePunctuation', 'boolean', undefined, false, W);
  const r = {
    locale: loc.locale, usage, sensitivity, ignorePunctuation,
    collation: loc.keys.co && loc.keys.co !== defaultCollation(loc.dataLocale) ? loc.keys.co : 'default',
    numeric: loc.keys.kn === '' || loc.keys.kn === 'true',
    caseFirst: loc.keys.kf || 'false',
  };
  WeakMapPrototypeSet(CollatorSlots, col, r);
  return col;
}
const CollatorPrototype = {};
defineClass(Collator, 'Collator', 0, CollatorPrototype, 'Intl.Collator');
ObjectDefineProperty(Collator, 'supportedLocalesOf', {
  value: setFunctionName(function supportedLocalesOf(locales, options = undefined) {
    return supportedLocales(canonicalizeLocaleList(locales), options, 'collator');
  }, 'supportedLocalesOf'), writable: true, configurable: true,
});
defineMethods(CollatorPrototype, {
  resolvedOptions() {
    const r = unwrap(CollatorSlots, this, 'Intl.Collator.prototype.resolvedOptions');
    return { locale: r.locale, usage: r.usage, sensitivity: r.sensitivity, ignorePunctuation: r.ignorePunctuation,
             collation: r.collation, numeric: r.numeric, caseFirst: r.caseFirst };
  },
});
defineGetter(CollatorPrototype, 'compare', function compare() {
  const r = unwrap(CollatorSlots, this, 'get Intl.Collator.prototype.compare');
  if (!r.boundCompare) r.boundCompare = setFunctionName((x, y) => collatorCompare(r, StringCtor(x), StringCtor(y)), '');
  return r.boundCompare;
});

// ---------------------------------------------------------------------------
// Segmenter (the segmentation itself is native)

const SegmenterSlots = new WeakMapCtor();
const SegmentsSlots = new WeakMapCtor();
const GRANULARITIES = ['grapheme', 'word', 'sentence'];

function Segmenter(locales, options) {
  if (new.target === undefined) throw new TypeErrorCtor("Constructor Intl.Segmenter requires 'new'");
  const seg = ObjectCreate(new.target.prototype || Segmenter.prototype);
  const requested = canonicalizeLocaleList(locales);
  const opts = getOptionsObject(options);
  getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', 'Intl.Segmenter');
  const loc = resolveLocale(requested, [], null, {});
  const granularity = getOption(opts, 'granularity', 'string', GRANULARITIES, 'grapheme', 'Intl.Segmenter');
  WeakMapPrototypeSet(SegmenterSlots, seg, { locale: loc.locale, granularity, g: ArrayPrototypeIndexOf(GRANULARITIES, granularity) });
  return seg;
}
const SegmenterPrototype = {};
defineClass(Segmenter, 'Segmenter', 0, SegmenterPrototype, 'Intl.Segmenter');
ObjectDefineProperty(Segmenter, 'supportedLocalesOf', {
  value: setFunctionName(function supportedLocalesOf(locales, options = undefined) {
    return supportedLocales(canonicalizeLocaleList(locales), options);
  }, 'supportedLocalesOf'), writable: true, configurable: true,
});

const SegmentsPrototype = {};
const SegmentIteratorPrototype = ObjectCreate(ReflectGetPrototypeOf(ReflectGetPrototypeOf([][SymbolIterator]())));

defineMethods(SegmenterPrototype, {
  segment(string) {
    const r = unwrap(SegmenterSlots, this, 'Intl.Segmenter.prototype.segment');
    const s = StringCtor(string);
    const segs = ObjectCreate(SegmentsPrototype);
    WeakMapPrototypeSet(SegmentsSlots, segs, { s, g: r.g });
    return segs;
  },
  resolvedOptions() {
    const r = unwrap(SegmenterSlots, this, 'Intl.Segmenter.prototype.resolvedOptions');
    return { locale: r.locale, granularity: r.granularity };
  },
});

defineMethods(SegmentsPrototype, {
  containing(index) {
    const r = unwrap(SegmentsSlots, this, '%Segments.prototype%.containing');
    let n = NumberCtor(index);
    n = NumberIsNaN(n) ? 0 : MathTrunc(n);
    if (n < 0 || n >= r.s.length) return undefined;
    return native.containing(r.s, n, r.g);
  },
});
ObjectDefineProperty(SegmentsPrototype, SymbolIterator, {
  value: setFunctionName(function () {
    const r = unwrap(SegmentsSlots, this, '%Segments.prototype%[@@iterator]');
    return native.iterator(r.s, r.g, SegmentIteratorPrototype);
  }, '[Symbol.iterator]'), writable: true, configurable: true,
});
// next() is native: the iterator keeps the string and position in C
ObjectDefineProperty(SegmentIteratorPrototype, 'next', { value: native.next, writable: true, configurable: true });
ObjectDefineProperty(SegmentIteratorPrototype, SymbolToStringTag, { value: 'Segmenter String Iterator', configurable: true });

// ---------------------------------------------------------------------------
// Locale

LocaleSlots = new WeakMapCtor();
const LOCALE_KEYS = { calendar: 'ca', collation: 'co', hourCycle: 'hc', caseFirst: 'kf', numeric: 'kn', numberingSystem: 'nu', firstDayOfWeek: 'fw' };

function Locale(tag, options) {
  if (new.target === undefined) throw new TypeErrorCtor("Constructor Intl.Locale requires 'new'");
  if (typeof tag !== 'string' && (typeof tag !== 'object' || tag === null))
    throw new TypeErrorCtor("First argument to Intl.Locale constructor can't be empty or missing");
  const slots0 = typeof tag === 'object' ? WeakMapPrototypeGet(LocaleSlots, tag) : undefined;
  let str = slots0 ? slots0.tag : StringCtor(tag);
  if (str === '') throw new RangeErrorCtor("First argument to Intl.Locale constructor can't be empty or missing");
  const opts = getOptionsObject(options);
  const t = parseLanguageTag(canonicalizeTag(str));
  const W = 'Intl.Locale';
  const lang = getOption(opts, 'language', 'string', undefined, undefined, W);
  if (lang !== undefined) {
    if (!RegExpPrototypeTest(/^([a-z]{2,3}|[a-z]{5,8})$/i, lang)) throw new RangeErrorCtor('Incorrect locale information provided');
    t.language = StringPrototypeToLowerCase(lang);
  }
  const script = getOption(opts, 'script', 'string', undefined, undefined, W);
  if (script !== undefined) {
    if (!RegExpPrototypeTest(/^[a-z]{4}$/i, script)) throw new RangeErrorCtor('Incorrect locale information provided');
    t.script = StringPrototypeToUpperCase(script[0]) + StringPrototypeToLowerCase(StringPrototypeSlice(script, 1));
  }
  const region = getOption(opts, 'region', 'string', undefined, undefined, W);
  if (region !== undefined) {
    if (!RegExpPrototypeTest(/^([a-z]{2}|[0-9]{3})$/i, region)) throw new RangeErrorCtor('Incorrect locale information provided');
    t.region = StringPrototypeToUpperCase(region);
  }
  // keywords: options override the -u- extension
  let u = null;
  for (const e of t.ext) if (e.s === 'u') u = parseUnicodeExtension(e.sub);
  const kw = u ? { ...u.keys } : ObjectCreate(null);
  const attrs = u ? u.attrs : [];
  const set = (opt, key, values) => {
    let v = opts[opt];
    if (v === undefined) return;
    v = opt === 'numeric' ? StringCtor(!!v) : StringCtor(v);
    if (values ? !ArrayPrototypeIncludes(values, v) : !isWellFormedUnicodeType(v))
      throw new RangeErrorCtor(`Value ${v} out of range for Intl.Locale options property ${opt}`);
    kw[key] = v === 'true' && key === 'kn' ? '' : v;
  };
  set('calendar', 'ca');
  set('collation', 'co');
  set('firstDayOfWeek', 'fw');
  set('hourCycle', 'hc', ['h11', 'h12', 'h23', 'h24']);
  set('caseFirst', 'kf', ['upper', 'lower', 'false']);
  set('numeric', 'kn');
  set('numberingSystem', 'nu');
  t.ext = t.ext.filter((e) => e.s !== 'u');
  const keys = ArrayPrototypeSort(ObjectKeys(kw));
  if (keys.length || attrs.length) {
    const sub = [...attrs];
    for (const k of keys) { ArrayPrototypePush(sub, k); if (kw[k]) ArrayPrototypePush(sub, ...StringPrototypeSplit(kw[k], '-')); }
    ArrayPrototypePush(t.ext, { s: 'u', sub });
    ArrayPrototypeSort(t.ext, (a, b) => (a.s < b.s ? -1 : a.s > b.s ? 1 : 0));
  }
  const loc = ObjectCreate(new.target.prototype || Locale.prototype);
  WeakMapPrototypeSet(LocaleSlots, loc, { tag: tagToString(t), t, kw });
  return loc;
}

function localeSlots(l, m) {
  return unwrap(LocaleSlots, l, m);
}

// add likely subtags (UTS #35): language and region give the script,
// language and script the region; und from the region or script
function likelyAdd(t) {
  const X = D.locales.likelyExtra;
  let lang = t.language, script = t.script, region = t.region;
  if (lang === 'und') {
    let m;
    if (region && (m = X['und-' + region])) {
      [lang, script] = [StringPrototypeSplit(m, '-')[0], script || StringPrototypeSplit(m, '-')[1]];
    } else if (script && (m = X['und-' + script])) {
      [lang, region] = [StringPrototypeSplit(m, '-')[0], region || StringPrototypeSplit(m, '-')[1]];
    } else {
      lang = 'en';
    }
  }
  const lk = D.locales.likely[lang];
  if (!lk) return { ...t, language: lang, script, region };
  const [ds, dr] = StringPrototypeSplit(lk, '-');
  if (!script) script = (region && X[lang + '-' + region]) || ds;
  if (!region) region = (t.script && X[lang + '-' + t.script]) || dr;
  return { ...t, language: lang, script, region };
}

function localeRegion(t) {
  return t.region || likelyAdd(t).region;
}

const LocalePrototype = {};
defineClass(Locale, 'Locale', 1, LocalePrototype, 'Intl.Locale');
defineMethods(LocalePrototype, {
  toString() { return localeSlots(this, 'Intl.Locale.prototype.toString').tag; },
  maximize() {
    const s = localeSlots(this, 'Intl.Locale.prototype.maximize');
    return new Locale(tagToString(likelyAdd(s.t)));
  },
  minimize() {
    const s = localeSlots(this, 'Intl.Locale.prototype.minimize');
    const max = likelyAdd(s.t);
    const tries = [{ ...max, script: '', region: '' }, { ...max, script: '' }, { ...max, region: '' }];
    for (const c of tries) {
      const m = likelyAdd(c);
      if (m.script === max.script && m.region === max.region) return new Locale(tagToString(c));
    }
    return new Locale(tagToString(max));
  },
  getCalendars() {
    const s = localeSlots(this, 'Intl.Locale.prototype.getCalendars');
    return [s.kw.ca || 'gregory'];
  },
  getCollations() {
    const s = localeSlots(this, 'Intl.Locale.prototype.getCollations');
    if (s.kw.co) return [s.kw.co];
    const extra = D.locales.collations[s.t.language];
    return ArrayPrototypeSort(['emoji', 'eor', ...(extra ? StringPrototypeSplit(extra, ' ') : [])]);
  },
  getHourCycles() {
    const s = localeSlots(this, 'Intl.Locale.prototype.getHourCycles');
    if (s.kw.hc) return [s.kw.hc];
    // (languages without data have the root's h23)
    const m = likelyAdd(s.t);
    if (!D.locales.likely[m.language]) return ['h23'];
    return [D.locales.hourCycleLocale[m.language + '-' + m.region] || D.locales.hourCycle[m.region] || 'h23'];
  },
  getNumberingSystems() {
    const s = localeSlots(this, 'Intl.Locale.prototype.getNumberingSystems');
    return [s.kw.nu || 'latn'];
  },
  getTimeZones() {
    const s = localeSlots(this, 'Intl.Locale.prototype.getTimeZones');
    if (!s.t.region) return undefined;
    const z = D.locales.timeZones[s.t.region];
    return z ? StringPrototypeSplit(z, ' ') : [];
  },
  getTextInfo() {
    const s = localeSlots(this, 'Intl.Locale.prototype.getTextInfo');
    const L = D.locales;
    const script = s.t.script || likelyAdd(s.t).script;
    const rtl = s.t.script ? ArrayPrototypeIncludes(StringPrototypeSplit(L.rtlScripts, ' '), script)
      : ArrayPrototypeIncludes(StringPrototypeSplit(L.rtl, ' '), s.t.language) || ArrayPrototypeIncludes(StringPrototypeSplit(L.rtlScripts, ' '), script);
    return { direction: rtl ? 'rtl' : 'ltr' };
  },
  getWeekInfo() {
    const s = localeSlots(this, 'Intl.Locale.prototype.getWeekInfo');
    const w = D.locales.week[localeRegion(s.t)] || '1|6,7';
    const [first, weekend] = StringPrototypeSplit(w, '|');
    let firstDay = NumberCtor(first);
    const fw = s.kw.fw;
    if (fw) {
      const i = ArrayPrototypeIndexOf(['mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun'], fw);
      if (i >= 0) firstDay = i + 1;
    }
    return { firstDay, weekend: StringPrototypeSplit(weekend, ',').map(NumberCtor) };
  },
});
for (const [name, key] of [['calendar', 'ca'], ['caseFirst', 'kf'], ['collation', 'co'], ['firstDayOfWeek', 'fw'],
                           ['hourCycle', 'hc'], ['numberingSystem', 'nu']])
  defineGetter(LocalePrototype, name, setFunctionName(function () {
    const v = localeSlots(this, `get Intl.Locale.prototype.${name}`).kw[key];
    return v === undefined ? undefined : v;
  }, 'get ' + name));
defineGetter(LocalePrototype, 'numeric', setFunctionName(function () {
  const v = localeSlots(this, 'get Intl.Locale.prototype.numeric').kw.kn;
  return v === '' || v === 'true';
}, 'get numeric'));
defineGetter(LocalePrototype, 'language', setFunctionName(function () {
  const l = localeSlots(this, 'get Intl.Locale.prototype.language').t.language;
  return l === 'und' ? undefined : l;
}, 'get language'));
defineGetter(LocalePrototype, 'script', setFunctionName(function () { return localeSlots(this, 'get Intl.Locale.prototype.script').t.script || undefined; }, 'get script'));
defineGetter(LocalePrototype, 'region', setFunctionName(function () { return localeSlots(this, 'get Intl.Locale.prototype.region').t.region || undefined; }, 'get region'));
defineGetter(LocalePrototype, 'baseName', setFunctionName(function () {
  const t = localeSlots(this, 'get Intl.Locale.prototype.baseName').t;
  return tagToString({ ...t, ext: [], priv: '' });
}, 'get baseName'));
for (const [name, m] of [['calendars', 'getCalendars'], ['collations', 'getCollations'], ['hourCycles', 'getHourCycles'],
                         ['numberingSystems', 'getNumberingSystems'], ['textInfo', 'getTextInfo'],
                         ['timeZones', 'getTimeZones'], ['weekInfo', 'getWeekInfo']])
  defineGetter(LocalePrototype, name, setFunctionName(function () { return LocalePrototype[m].call(this); }, 'get ' + name));

// ---------------------------------------------------------------------------
// DisplayNames

const DisplayNamesSlots = new WeakMapCtor();
const DN_TYPES = ['language', 'region', 'script', 'currency', 'calendar', 'dateTimeField'];

function DisplayNames(locales, options) {
  if (new.target === undefined) throw new TypeErrorCtor("Constructor Intl.DisplayNames requires 'new'");
  const dn = ObjectCreate(new.target.prototype || DisplayNames.prototype);
  const requested = canonicalizeLocaleList(locales);
  if (options === undefined) throw new TypeErrorCtor('invalid_argument');
  const opts = getOptionsObject(options);
  const W = 'Intl.DisplayNames';
  getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', W);
  const loc = resolveLocale(requested, [], null, {});
  const style = getOption(opts, 'style', 'string', ['narrow', 'short', 'long'], 'long', W);
  const type = getOption(opts, 'type', 'string', DN_TYPES, undefined, W);
  if (type === undefined) throw new TypeErrorCtor('invalid_argument');
  const fallback = getOption(opts, 'fallback', 'string', ['code', 'none'], 'code', W);
  const languageDisplay = getOption(opts, 'languageDisplay', 'string', ['dialect', 'standard'], 'dialect', W);
  const r = { locale: loc.locale, style, type, fallback };
  if (type === 'language') r.languageDisplay = languageDisplay;
  WeakMapPrototypeSet(DisplayNamesSlots, dn, r);
  return dn;
}

function displayNameOf(r, code) {
  const M = D.misc;
  code = StringCtor(code);
  switch (r.type) {
    case 'language': {
      const t = parseLanguageTag(code);
      if (!t || t.ext.length || t.priv) throw new RangeErrorCtor('invalid_argument');
      const canon = tagToString(t);
      if (r.languageDisplay === 'dialect') {
        const d = (r.style !== 'long' && M.languagesShort[canon]) || M.languages[canon];
        if (d !== undefined && StringPrototypeIndexOf(canon, '-') > 0) return d;
      }
      let name = (r.style !== 'long' && M.languagesShort[t.language]) || M.languages[t.language];
      if (name === undefined) return r.fallback === 'code' ? canon : undefined;
      const extra = [];
      if (t.script) ArrayPrototypePush(extra, M.scripts[t.script] || t.script);
      if (t.region) ArrayPrototypePush(extra, (r.style !== 'long' && M.regionsShort[t.region]) || M.regions[t.region] || t.region);
      for (const v of t.variants) ArrayPrototypePush(extra, StringPrototypeToUpperCase(v));
      return extra.length ? name + ' (' + ArrayPrototypeJoin(extra, ', ') + ')' : name;
    }
    case 'region': {
      if (!RegExpPrototypeTest(/^([a-z]{2}|[0-9]{3})$/i, code)) throw new RangeErrorCtor('invalid_argument');
      const c = StringPrototypeToUpperCase(code);
      const v = (r.style !== 'long' && M.regionsShort[c]) || M.regions[c];
      return v !== undefined ? v : r.fallback === 'code' ? c : undefined;
    }
    case 'script': {
      if (!RegExpPrototypeTest(/^[a-z]{4}$/i, code)) throw new RangeErrorCtor('invalid_argument');
      const c = StringPrototypeToUpperCase(code[0]) + StringPrototypeToLowerCase(StringPrototypeSlice(code, 1));
      const v = M.scripts[c];
      return v !== undefined ? v : r.fallback === 'code' ? c : undefined;
    }
    case 'currency': {
      if (!isWellFormedCurrency(code)) throw new RangeErrorCtor('invalid_argument');
      const c = StringPrototypeToUpperCase(code);
      const v = M.currencyNames[c];
      return v !== undefined ? v : r.fallback === 'code' ? c : undefined;
    }
    case 'calendar': {
      if (!isWellFormedUnicodeType(code)) throw new RangeErrorCtor('invalid_argument');
      const v = M.calendars[code];
      return v !== undefined ? v : r.fallback === 'code' ? code : undefined;
    }
    default: {
      const v = (r.style === 'short' ? M.dateTimeFieldsShort : r.style === 'narrow' ? M.dateTimeFieldsNarrow : M.dateTimeFields)[code];
      if (v === undefined) throw new RangeErrorCtor('invalid_argument');
      return v;
    }
  }
}

const DisplayNamesPrototype = {};
defineClass(DisplayNames, 'DisplayNames', 2, DisplayNamesPrototype, 'Intl.DisplayNames');
ObjectDefineProperty(DisplayNames, 'supportedLocalesOf', {
  value: setFunctionName(function supportedLocalesOf(locales, options = undefined) {
    return supportedLocales(canonicalizeLocaleList(locales), options);
  }, 'supportedLocalesOf'), writable: true, configurable: true,
});
defineMethods(DisplayNamesPrototype, {
  of(code) {
    return displayNameOf(unwrap(DisplayNamesSlots, this, 'Intl.DisplayNames.prototype.of'), code);
  },
  resolvedOptions() {
    const r = unwrap(DisplayNamesSlots, this, 'Intl.DisplayNames.prototype.resolvedOptions');
    const o = { locale: r.locale, style: r.style, type: r.type, fallback: r.fallback };
    if (r.languageDisplay !== undefined) o.languageDisplay = r.languageDisplay;
    return o;
  },
});

// ---------------------------------------------------------------------------
// DurationFormat (en)

const DurationFormatSlots = new WeakMapCtor();
const DUR_UNITS = ['years', 'months', 'weeks', 'days', 'hours', 'minutes', 'seconds', 'milliseconds', 'microseconds', 'nanoseconds'];
const DUR_SINGULAR = { years: 'year', months: 'month', weeks: 'week', days: 'day', hours: 'hour', minutes: 'minute',
                       seconds: 'second', milliseconds: 'millisecond', microseconds: 'microsecond', nanoseconds: 'nanosecond' };

function DurationFormat(locales, options) {
  if (new.target === undefined) throw new TypeErrorCtor("Constructor Intl.DurationFormat requires 'new'");
  const df = ObjectCreate(new.target.prototype || DurationFormat.prototype);
  const requested = canonicalizeLocaleList(locales);
  const opts = getOptionsObject(options);
  const W = 'Intl.DurationFormat';
  getOption(opts, 'localeMatcher', 'string', ['lookup', 'best fit'], 'best fit', W);
  const nu = getOption(opts, 'numberingSystem', 'string', undefined, undefined, W);
  const loc = resolveLocale(requested, ['nu'], { nu }, { nu: isNumberingSystem });
  const style = getOption(opts, 'style', 'string', ['long', 'short', 'narrow', 'digital'], 'short', W);
  const r = { locale: loc.locale, numberingSystem: 'latn', style };
  for (const u of DUR_UNITS) {
    const isTime = ArrayPrototypeIncludes(['hours', 'minutes', 'seconds'], u);
    const allowed = isTime ? ['long', 'short', 'narrow', 'numeric', '2-digit'] : ['long', 'short', 'narrow'];
    let def = style === 'digital' ? (isTime ? 'numeric' : 'short') : style;
    if (style === 'digital' && (u === 'minutes' || u === 'seconds')) def = '2-digit';
    if (ArrayPrototypeIncludes(['milliseconds', 'microseconds', 'nanoseconds'], u)) {
      ArrayPrototypePush(allowed, 'numeric');
      if (style === 'digital') def = 'numeric';
    }
    r[u] = getOption(opts, u, 'string', allowed, def, W);
    r[u + 'Display'] = getOption(opts, u + 'Display', 'string', ['auto', 'always'],
                                 style === 'digital' && isTime ? 'always' : 'auto', W);
  }
  r.fractionalDigits = getNumberOption(opts, 'fractionalDigits', 0, 9, undefined);
  WeakMapPrototypeSet(DurationFormatSlots, df, r);
  return df;
}

function toDurationRecord(d) {
  if (typeof d === 'string') throw new RangeErrorCtor('Invalid duration');
  if (typeof d !== 'object' || d === null) throw new TypeErrorCtor('Invalid duration');
  const rec = {};
  let any = false, sign = 0;
  for (const u of DUR_UNITS) {
    let v = d[u];
    if (v === undefined) { rec[u] = 0; continue; }
    v = NumberCtor(v);
    if (!NumberIsFinite(v) || MathTrunc(v) !== v) throw new RangeErrorCtor('Invalid duration');
    if (v) {
      const s = v < 0 ? -1 : 1;
      if (sign && s !== sign) throw new RangeErrorCtor('Invalid duration');
      sign = s;
    }
    rec[u] = v;
    any = true;
  }
  if (!any) throw new TypeErrorCtor('Invalid duration');
  return rec;
}

function formatDurationToParts(r, d) {
  const rec = toDurationRecord(d);
  const list = [];
  const unitNf = (u, display, value) => {
    const nf = ObjectCreate(null);
    const o = { style: 'unit', unit: DUR_SINGULAR[u], unitDisplay: display };
    return formatNumberToParts(initNumberFormat(nf, ['en'], o), toIntlMV(value));
  };
  const digital = [];
  for (const u of DUR_UNITS) {
    const v = rec[u], style = r[u], display = r[u + 'Display'];
    if (style === 'numeric' || style === '2-digit') {
      if (u === 'hours' || u === 'minutes' || u === 'seconds') {
        ArrayPrototypePush(digital, { u, v, style });
        continue;
      }
    }
    if (v === 0 && display === 'auto') continue;
    ArrayPrototypePush(list, unitNf(u, style, v));
  }
  if (digital.length && digital.some((x) => x.v !== 0 || r[x.u + 'Display'] === 'always')) {
    const parts = [];
    digital.forEach((x, i) => {
      if (i) ArrayPrototypePush(parts, { type: 'literal', value: ':' });
      ArrayPrototypePush(parts, { type: 'integer', value: x.style === '2-digit' || i ? pad2(MathAbs(x.v)) : StringCtor(MathAbs(x.v)), unit: DUR_SINGULAR[x.u] });
    });
    ArrayPrototypePush(list, parts);
  }
  const type = r.style === 'long' ? 'ul' : r.style === 'narrow' ? 'un' : 'us';
  const p = D.misc.lists[type];
  const lr = { p };
  const strings = list.map(partsToString);
  const lparts = formatListToParts(lr, strings);
  const res = [];
  let k = 0;
  for (const lp of lparts) {
    if (lp.type === 'element') for (const x of list[k++]) ArrayPrototypePush(res, x);
    else ArrayPrototypePush(res, lp);
  }
  return res;
}

const DurationFormatPrototype = {};
defineClass(DurationFormat, 'DurationFormat', 0, DurationFormatPrototype, 'Intl.DurationFormat');
ObjectDefineProperty(DurationFormat, 'supportedLocalesOf', {
  value: setFunctionName(function supportedLocalesOf(locales, options = undefined) {
    return supportedLocales(canonicalizeLocaleList(locales), options);
  }, 'supportedLocalesOf'), writable: true, configurable: true,
});
defineMethods(DurationFormatPrototype, {
  format(duration) {
    return partsToString(formatDurationToParts(unwrap(DurationFormatSlots, this, 'Intl.DurationFormat.prototype.format'), duration));
  },
  formatToParts(duration) {
    return formatDurationToParts(unwrap(DurationFormatSlots, this, 'Intl.DurationFormat.prototype.formatToParts'), duration);
  },
  resolvedOptions() {
    const r = unwrap(DurationFormatSlots, this, 'Intl.DurationFormat.prototype.resolvedOptions');
    const o = { locale: r.locale, numberingSystem: r.numberingSystem, style: r.style };
    for (const u of DUR_UNITS) { o[u] = r[u]; o[u + 'Display'] = r[u + 'Display']; }
    if (r.fractionalDigits !== undefined) o.fractionalDigits = r.fractionalDigits;
    return o;
  },
});

// ---------------------------------------------------------------------------
// the Intl namespace

const Intl = {};
ObjectDefineProperty(Intl, SymbolToStringTag, { value: 'Intl', configurable: true });
defineMethods(Intl, {
  getCanonicalLocales(locales) {
    return canonicalizeLocaleList(locales);
  },
  supportedValuesOf(key) {
    key = StringCtor(key);
    switch (key) {
      case 'calendar': return [...D.misc.supported.calendar];
      case 'collation': return [...D.misc.supported.collation];
      case 'currency': return [...D.misc.supported.currency];
      case 'numberingSystem': return [...D.misc.supported.numberingSystem];
      case 'timeZone': return [...D.tz.supported];
      case 'unit': return [...D.misc.supported.unit];
    }
    throw new RangeErrorCtor(`Invalid key : ${key}`);
  },
});
for (const [name, ctor] of [['DateTimeFormat', DateTimeFormat], ['NumberFormat', NumberFormat], ['Collator', Collator],
                            ['PluralRules', PluralRules], ['RelativeTimeFormat', RelativeTimeFormat],
                            ['ListFormat', ListFormat], ['Locale', Locale], ['DisplayNames', DisplayNames],
                            ['Segmenter', Segmenter], ['DurationFormat', DurationFormat]])
  ObjectDefineProperty(Intl, name, { value: ctor, writable: true, enumerable: false, configurable: true });

// ---------------------------------------------------------------------------
// the locale methods of the built-ins

let defaultNumberFormat, defaultCollator;
const defaultDateFormats = new MapCtor();

function cachedDateFormat(kind, required, defaults, locales, options) {
  if (locales === undefined && options === undefined) {
    let r = MapPrototypeGet(defaultDateFormats, kind);
    if (!r) {
      r = initDateTimeFormat(ObjectCreate(null), undefined, undefined, required, defaults);
      MapPrototypeSet(defaultDateFormats, kind, r);
    }
    return r;
  }
  return initDateTimeFormat(ObjectCreate(null), locales, options, required, defaults);
}

function thisTimeValue(v, method) {
  try {
    return DatePrototypeValueOf(v);
  } catch {
    throw new TypeErrorCtor(`Method ${method} called on incompatible receiver ${describe(v)}`);
  }
}

function dateLocale(kind, required, defaults, method) {
  return function (locales = undefined, options = undefined) {
    const t = thisTimeValue(this, method);
    if (NumberIsNaN(t)) return 'Invalid Date';
    const r = cachedDateFormat(kind, required, defaults, locales, options);
    return formatDate(r, t);
  };
}

return {
  Intl,
  dateToLocaleString: dateLocale('any', 'any', 'all', 'Date.prototype.toLocaleString'),
  dateToLocaleDateString: dateLocale('date', 'date', 'date', 'Date.prototype.toLocaleDateString'),
  dateToLocaleTimeString: dateLocale('time', 'time', 'time', 'Date.prototype.toLocaleTimeString'),
  numberToLocaleString(locales = undefined, options = undefined) {
    let x;
    try {
      x = NumberPrototypeValueOf(this);
    } catch {
      throw new TypeErrorCtor("Number.prototype.toLocaleString requires that 'this' be a Number");
    }
    let r;
    if (locales === undefined && options === undefined) {
      if (!defaultNumberFormat) defaultNumberFormat = initNumberFormat(ObjectCreate(null), undefined, undefined);
      r = defaultNumberFormat;
    } else {
      r = initNumberFormat(ObjectCreate(null), locales, options);
    }
    return formatNumberString(r, toIntlMV(x));
  },
  bigintToLocaleString(locales = undefined, options = undefined) {
    let x;
    try {
      x = BigIntPrototypeValueOf(this);
    } catch {
      throw new TypeErrorCtor("BigInt.prototype.toLocaleString requires that 'this' be a BigInt");
    }
    let r;
    if (locales === undefined && options === undefined) {
      if (!defaultNumberFormat) defaultNumberFormat = initNumberFormat(ObjectCreate(null), undefined, undefined);
      r = defaultNumberFormat;
    } else {
      r = initNumberFormat(ObjectCreate(null), locales, options);
    }
    return formatNumberString(r, decFromBigInt(x));
  },
  // Date.prototype.toString and toTimeString end with the zone's name
  dateZoneSuffix(str) {
    const t = DatePrototypeValueOf(this);
    if (NumberIsNaN(t)) return str;
    const tz = defaultTimeZoneRecord();
    const { off } = zoneOffset(tz, t);
    return str + ' (' + zoneName(tz, 'long', off, t) + ')';
  },
  localeCompare(that, locales = undefined, options = undefined) {
    if (this === undefined || this === null)
      throw new TypeErrorCtor('String.prototype.localeCompare called on null or undefined');
    const s = StringCtor(this), t = StringCtor(that);
    let r;
    if (locales === undefined && options === undefined) {
      if (!defaultCollator) defaultCollator = WeakMapPrototypeGet(CollatorSlots, new Collator());
      r = defaultCollator;
    } else {
      r = WeakMapPrototypeGet(CollatorSlots, new Collator(locales, options));
    }
    return collatorCompare(r, s, t);
  },
  // for tests
  native,
};
})
