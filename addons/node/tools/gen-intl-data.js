#!/usr/bin/env node
// Generates src/intl_data.json, the en-US locale data src/intl.js formats
// with: date/time patterns, time zone names and links, currencies, units,
// relative time, lists and display names.  The data is CLDR's as ICU ships
// it, read back through the Intl API of the Node.js release we follow (run
// this with Node 24.21: ICU 78.3, CLDR 48):
//
//   /path/to/node-v24.21 tools/gen-intl-data.js > src/intl_data.json
//
// It also reads the TZif files under /usr/share/zoneinfo for the POSIX TZ
// rules of each zone (used when a system has no zoneinfo), and the IANA tz
// source for the links of the main data (set TZDATA_DIR to an unpacked
// tzdata<version>.tar.gz, the version of process.versions.tz).
'use strict';
const fs = require('fs');
const path = require('path');

const L = 'en-US';
const out = { icu: process.versions.icu, cldr: process.versions.cldr };

// ---------------------------------------------------------------------------
// date / time patterns

// a sample instant whose fields render distinctly by width
const SAMPLE = new Date(Date.UTC(2024, 0, 5, 14, 7, 9, 45)); // Fri Jan 5 2024 14:07:09.045

const WEEKDAY = [undefined, 'narrow', 'short', 'long'];
const ERA = [undefined, 'narrow', 'short', 'long'];
const YEAR = [undefined, 'numeric', '2-digit'];
const MONTH = [undefined, 'numeric', '2-digit', 'narrow', 'short', 'long'];
const DAY = [undefined, 'numeric', '2-digit'];
const DAYPERIOD = [undefined, 'narrow', 'short', 'long'];
const HOUR = [undefined, 'numeric', '2-digit'];
const MINUTE = [undefined, 'numeric', '2-digit'];
const SECOND = [undefined, 'numeric', '2-digit'];
const FSD = [undefined, 1, 2, 3];
const HC = [undefined, 'h11', 'h12', 'h23', 'h24'];
const TZN = [undefined, 'short'];

// a pattern from formatToParts: literals as text, fields as {X:w}
// (X: E G y M d a B h H K k m s S z; w: n numeric, 2 2-digit, N narrow,
// s short, l long, or the digit count for S)
function partsToPattern(parts, ro) {
  let p = '';
  for (const { type, value } of parts) {
    switch (type) {
      case 'literal': p += value.replace(/[{}]/g, (c) => (c === '{' ? '{{' : '}}')); break;
      case 'weekday':
        p += '{E:' + ({ F: 'N', Fri: 's', Friday: 'l' })[value] + '}'; break;
      case 'era':
        p += '{G:' + ({ A: 'N', AD: 's', 'Anno Domini': 'l' })[value] + '}'; break;
      case 'year':
        p += '{y:' + (value === '24' ? '2' : 'n') + '}'; break;
      case 'month':
        p += '{M:' + ({ 1: 'n', '01': '2', J: 'N', Jan: 's', January: 'l' })[value] + '}'; break;
      case 'day':
        p += '{d:' + (value === '05' ? '2' : 'n') + '}'; break;
      case 'dayPeriod':
        if (value === 'PM' || value === 'AM') p += '{a:s}';
        else p += '{B:' + ({ narrow: 'N', short: 's', long: 'l' })[ro.dayPeriod || 'short'] + '}';
        break;
      case 'hour': {
        const hc = ro.hourCycle || 'h12';
        const letter = { h11: 'K', h12: 'h', h23: 'H', h24: 'k' }[hc];
        p += '{' + letter + ':' + (value.length === 2 ? '2' : 'n') + '}'; break;
      }
      case 'minute': p += '{m:' + (value.length === 2 ? '2' : 'n') + '}'; break;
      case 'second': p += '{s:' + (value.length === 2 ? '2' : 'n') + '}'; break;
      case 'fractionalSecond': p += '{S:' + value.length + '}'; break;
      case 'timeZoneName': p += '{z:x}'; break;
      default: throw new Error('part ' + type);
    }
  }
  return p;
}

const patterns = [];
const patternIndex = new Map();
function pat(p) {
  let i = patternIndex.get(p);
  if (i === undefined) {
    i = patterns.length;
    patterns.push(p);
    patternIndex.set(p, i);
  }
  return i;
}

function fmtPattern(o) {
  const f = new Intl.DateTimeFormat(L, { timeZone: 'UTC', ...o });
  const ro = f.resolvedOptions();
  return partsToPattern(f.formatToParts(SAMPLE), ro);
}

// date skeletons: key "wgymd" digits
const dateTable = {};
for (let w = 0; w < WEEKDAY.length; w++)
  for (let g = 0; g < ERA.length; g++)
    for (let y = 0; y < YEAR.length; y++)
      for (let m = 0; m < MONTH.length; m++)
        for (let d = 0; d < DAY.length; d++) {
          if (!w && !y && !m && !d) continue; // era alone adds y/M/d: done by intl.js
          const o = { weekday: WEEKDAY[w], era: ERA[g], year: YEAR[y], month: MONTH[m], day: DAY[d] };
          dateTable['' + w + g + y + m + d] = pat(fmtPattern(o));
        }

// time skeletons: key "phmsfcz" digits (dayPeriod hour minute second fsd hc zone)
const timeTable = {};
for (let p = 0; p < DAYPERIOD.length; p++)
  for (let h = 0; h < HOUR.length; h++)
    for (let mi = 0; mi < MINUTE.length; mi++)
      for (let s = 0; s < SECOND.length; s++)
        for (let f = 0; f < FSD.length; f++)
          for (let c = 0; c < HC.length; c++)
            for (let z = 0; z < TZN.length; z++) {
              if (!p && !h && !mi && !s && !f && !z) continue;
              if (!h && c) continue; // the cycle only matters with an hour
              const o = { dayPeriod: DAYPERIOD[p], hour: HOUR[h], minute: MINUTE[mi], second: SECOND[s],
                          fractionalSecondDigits: FSD[f], hourCycle: HC[c], timeZoneName: TZN[z] };
              if (!p && !h && !mi && !s && !f) {
                // a zone without time fields stands with a date only
                const full = fmtPattern({ ...o, year: 'numeric' });
                timeTable['' + p + h + mi + s + f + c + z] = pat(full.replace(/^\{y:n\}, /, ''));
                continue;
              }
              timeTable['' + p + h + mi + s + f + c + z] = pat(fmtPattern(o));
            }

// how a date and a time combine: the joiner by date skeleton, found with
// several time skeletons; where they disagree the joint pattern is kept
const joiners = {};
const joint = {};
const timeProbe = [
  { hour: 'numeric' }, { hour: 'numeric', minute: '2-digit' }, { hour: 'numeric', hourCycle: 'h23' },
  { hour: 'numeric', minute: '2-digit', second: '2-digit' }, { minute: '2-digit' },
  { hour: 'numeric', dayPeriod: 'short' }, { timeZoneName: 'short' },
  { hour: 'numeric', minute: '2-digit', timeZoneName: 'short' },
  { second: 'numeric', fractionalSecondDigits: 2 },
];
function timeKey(o) {
  return '' + DAYPERIOD.indexOf(o.dayPeriod) + HOUR.indexOf(o.hour) + MINUTE.indexOf(o.minute) +
    SECOND.indexOf(o.second) + FSD.indexOf(o.fractionalSecondDigits) + HC.indexOf(o.hourCycle) +
    TZN.indexOf(o.timeZoneName);
}
for (const [dk, di] of Object.entries(dateTable)) {
  const [w, g, y, m, d] = dk.split('').map(Number);
  const dopt = { weekday: WEEKDAY[w], era: ERA[g], year: YEAR[y], month: MONTH[m], day: DAY[d] };
  const dp = patterns[di];
  let joiner;
  for (const to of timeProbe) {
    const tk = timeKey(to);
    const tp = patterns[timeTable[tk]];
    const full = fmtPattern({ ...dopt, ...to });
    let j;
    if (full.startsWith(dp) && full.endsWith(tp) && full.length >= dp.length + tp.length)
      j = full.slice(dp.length, full.length - tp.length);
    if (joiner === undefined && j !== undefined) joiner = j;
  }
  joiners[dk] = joiner;
}
// a weekday alone with a time: "E h:mm a" style patterns (a space) for the
// time skeletons CLDR has such formats for, ", " otherwise
const wkSpace = [];
for (const tk of Object.keys(timeTable)) {
  const [p, h, mi, s, f, c, z] = tk.split('').map(Number);
  const kinds = new Set();
  for (let w = 1; w < WEEKDAY.length; w++) {
    const o = { weekday: WEEKDAY[w], dayPeriod: DAYPERIOD[p], hour: HOUR[h], minute: MINUTE[mi], second: SECOND[s],
                fractionalSecondDigits: FSD[f], hourCycle: HC[c], timeZoneName: TZN[z] };
    const full = fmtPattern(o), E = patterns[dateTable['' + w + '0000']], T = patterns[timeTable[tk]];
    if (full === E + ' ' + T) kinds.add(' ');
    else if (full === E + ', ' + T) kinds.add(', ');
    else joint['' + w + '0000|' + tk] = pat(full);
  }
  if (kinds.has(' ') && kinds.has(', ')) throw new Error('mixed weekday joiner ' + tk);
  if (kinds.has(' ')) wkSpace.push(tk);
}
// spot check the joiner rule on more combinations; record what differs
for (const dk of Object.keys(dateTable)) {
  const [w, g, y, m, d] = dk.split('').map(Number);
  if (w && !g && !y && !m && !d) continue;
  for (const tk of ['0110000', '0120030', '0100000', '0111000', '0222000', '0000001', '0110001',
                    '1100000', '0001300', '0020000', '0112131']) {
    if (!(tk in timeTable)) continue;
    const [p, h, mi, s, f, c, z] = tk.split('').map(Number);
    const o = { weekday: WEEKDAY[w], era: ERA[g], year: YEAR[y], month: MONTH[m], day: DAY[d],
                dayPeriod: DAYPERIOD[p], hour: HOUR[h], minute: MINUTE[mi], second: SECOND[s],
                fractionalSecondDigits: FSD[f], hourCycle: HC[c], timeZoneName: TZN[z] };
    const full = fmtPattern(o);
    const expect = patterns[dateTable[dk]] + joiners[dk] + patterns[timeTable[tk]];
    if (full !== expect) joint[dk + '|' + tk] = pat(full);
  }
}
if (Object.keys(joint).length) process.stderr.write('joint exceptions: ' + Object.keys(joint).length + '\n');

// dateStyle / timeStyle
const STYLES = [undefined, 'full', 'long', 'medium', 'short'];
const styleTable = {};
for (let ds = 0; ds < STYLES.length; ds++)
  for (let ts = 0; ts < STYLES.length; ts++) {
    if (!ds && !ts) continue;
    for (let c = 0; c < HC.length; c++) {
      if (!ts && c) continue;
      const o = { dateStyle: STYLES[ds], timeStyle: STYLES[ts], hourCycle: HC[c] };
      styleTable['' + ds + ts + c] = pat(fmtPattern(o));
    }
  }

// tables as arrays indexed by mixed-radix keys
const dateArr = new Array(4 * 4 * 3 * 6 * 3).fill(-1), joinArr = new Array(dateArr.length).fill(0);
const JOINERS = ['', ' ', ', ', ' at '];
for (const [k, v] of Object.entries(dateTable)) {
  const [w, g, y, m, d] = k.split('').map(Number);
  const i = (((w * 4 + g) * 3 + y) * 6 + m) * 3 + d;
  dateArr[i] = v;
  const j = JOINERS.indexOf(joiners[k]);
  if (j < 0) throw new Error('joiner ' + JSON.stringify(joiners[k]));
  joinArr[i] = j;
}
const timeArr = new Array(4 * 3 * 3 * 3 * 4 * 5 * 2).fill(-1);
for (const [k, v] of Object.entries(timeTable)) {
  const [p, h, mi, s, f, c, z] = k.split('').map(Number);
  timeArr[(((((p * 3 + h) * 3 + mi) * 3 + s) * 4 + f) * 5 + c) * 2 + z] = v;
}
out.date = { patterns, date: dateArr, time: timeArr, joiners: joinArr.join(''), JOINERS, wkSpace: wkSpace.join(' '), joint, styleTable };

// formatRange separators
{
  const f = new Intl.DateTimeFormat(L, { timeZone: 'UTC' });
  const s = f.formatRange(SAMPLE, new Date(Date.UTC(2024, 0, 8)));
  out.date.rangeSeparator = s.slice(s.indexOf('2024') + 4, s.lastIndexOf('1/8'));
}

// ---------------------------------------------------------------------------
// time zones

function zoneFiles() {
  const root = '/usr/share/zoneinfo';
  const res = [];
  const skip = new Set(['posix', 'right', 'posixrules', 'localtime', 'Factory']);
  (function walk(dir, rel) {
    for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
      if (skip.has(ent.name)) continue;
      const p = path.join(dir, ent.name);
      const r = rel ? rel + '/' + ent.name : ent.name;
      if (ent.isDirectory()) walk(p, r);
      else if (/^[A-Z]/.test(ent.name)) {
        const b = fs.readFileSync(p);
        if (b.slice(0, 4).toString() === 'TZif') res.push(r);
      }
    }
  })(root, '');
  return res.sort();
}

function tzFooter(name) {
  const b = fs.readFileSync('/usr/share/zoneinfo/' + name);
  const s = b.toString('latin1');
  const end = s.lastIndexOf('\n');
  const start = s.lastIndexOf('\n', end - 1);
  return s.slice(start + 1, end);
}

// The links of the main tz data (without backzone, as ICU has it): from
// the IANA source in $TZDATA_DIR (https://data.iana.org/time-zones/releases/
// tzdata<version>.tar.gz, the version of process.versions.tz), else from
// /usr/share/zoneinfo/tzdata.zi (which may include backzone)
const TZ_SOURCES = ['africa', 'antarctica', 'asia', 'australasia', 'europe', 'northamerica', 'southamerica',
                    'etcetera', 'backward', 'factory'];
function tzSource() {
  const zones = new Set(), links = {};
  const add = (text) => {
    for (const line of text.split('\n')) {
      const f = line.replace(/#.*/, '').trim().split(/\s+/);
      if (f[0] === 'Zone' || f[0] === 'Z') zones.add(f[1]);
      else if (f[0] === 'Link' || f[0] === 'L') links[f[2]] = f[1];
    }
  };
  const dir = process.env.TZDATA_DIR;
  if (dir) {
    const v = fs.readFileSync(path.join(dir, 'version'), 'utf8').trim();
    if (v !== process.versions.tz) process.stderr.write(`warning: tzdata ${v}, ICU has ${process.versions.tz}\n`);
    for (const f of TZ_SOURCES) add(fs.readFileSync(path.join(dir, f), 'utf8'));
  } else {
    process.stderr.write('warning: no TZDATA_DIR, links from tzdata.zi\n');
    add(fs.readFileSync('/usr/share/zoneinfo/tzdata.zi', 'utf8'));
  }
  for (const z of zones) delete links[z];
  const target = (n) => { const seen = new Set(); while (links[n] && !seen.has(n)) { seen.add(n); n = links[n]; } return n; };
  return { zones, links, target };
}
const TZSRC = tzSource();

const zoneNames = new Set(Intl.supportedValuesOf('timeZone'));
const links = {};      // input -> resolved id (when different from the input)
for (const f of [...new Set([...zoneFiles(), ...TZSRC.zones, ...Object.keys(TZSRC.links)])].sort()) {
  let resolved;
  try {
    resolved = new Intl.DateTimeFormat(L, { timeZone: f }).resolvedOptions().timeZone;
  } catch {
    continue;
  }
  if (resolved !== f) links[f] = resolved;
  zoneNames.add(resolved);
}
for (const extra of ['UTC', 'GMT', 'Etc/UTC', 'Etc/GMT', 'EST', 'MST', 'HST', 'EST5EDT', 'CST6CDT', 'MST7MDT', 'PST8PDT']) {
  try {
    const r = new Intl.DateTimeFormat(L, { timeZone: extra }).resolvedOptions().timeZone;
    if (r !== extra) links[extra] = r;
    zoneNames.add(r);
  } catch {}
}
// the zoneinfo file with the main data's rules of a zone
const files = {};
for (const z of zoneNames) {
  const t = TZSRC.target(z);
  if (t !== z) files[z] = t;
}

function offsetOf(zone, date) {
  const p = new Intl.DateTimeFormat(L, { timeZone: zone, timeZoneName: 'longOffset' })
    .formatToParts(date).find((x) => x.type === 'timeZoneName').value;
  const m = /GMT(?:([+-])(\d\d):(\d\d))?/.exec(p);
  return m[1] ? (m[1] === '-' ? -1 : 1) * (+m[2] * 60 + +m[3]) : 0;
}
function gmtShort(min) {
  if (!min) return 'GMT+0';
  const s = min < 0 ? '-' : '+', a = Math.abs(min), h = Math.floor(a / 60), mm = a % 60;
  return 'GMT' + s + h + (mm ? ':' + String(mm).padStart(2, '0') : '');
}
function gmtLong(min) {
  if (!min) return 'GMT+00:00';
  const s = min < 0 ? '-' : '+', a = Math.abs(min);
  return 'GMT' + s + String(Math.floor(a / 60)).padStart(2, '0') + ':' + String(a % 60).padStart(2, '0');
}
const znCache = new Map();
function zname(zone, date, style) {
  const k = zone + '|' + style;
  let f = znCache.get(k);
  if (!f) znCache.set(k, (f = new Intl.DateTimeFormat(L, { timeZone: zone, timeZoneName: style })));
  return f.formatToParts(date).find((x) => x.type === 'timeZoneName').value;
}

const strings = [];
const stringIndex = new Map();
function str(s) {
  if (s === null || s === undefined) return 0;
  let i = stringIndex.get(s);
  if (i === undefined) {
    i = strings.length + 1;
    strings.push(s);
    stringIndex.set(s, i);
  }
  return i;
}
const keep = (v, min, long) => (v === (long ? gmtLong(min) : gmtShort(min)) ? 0 : str(v));

// The names of a zone change over time (metazones: Europe/Kiev was on
// Moscow time until 1990; generic names become the standard name in years
// without DST), so they come in periods, found by sampling ICU four times a
// year and bisecting to the minute where they change:
//   zones[z] = 0 (GMT offsets only) or [location, period, ...]
//   period   = [end (ms, 0 for the last), shortGeneric, longGeneric,
//               offset, short, long, offset, short, long, ...]
// with names as string indexes (0: the GMT offset format), offsets in
// minutes.  Before 1970 ICU has no metazones: specific names are GMT
// offsets and generic ones the location ("New York Time").
const zones = {};
const posix = {};
const SAMPLES = [];
for (let y = 1970; y <= 2037; y++) for (const m of [0, 3, 6, 9]) SAMPLES.push(Date.UTC(y, m, 15, 12));
function zoneInfo(z, t) {
  const d = new Date(t), off = offsetOf(z, d);
  return { off, s: keep(zname(z, d, 'short'), off, false), l: keep(zname(z, d, 'long'), off, true),
           sg: keep(zname(z, d, 'shortGeneric'), off, false), lg: keep(zname(z, d, 'longGeneric'), off, true) };
}
function belongs(p, I) {
  if (I.sg !== p.sg || I.lg !== p.lg) return false;
  const n = p.names.get(I.off);
  return !n || (n.s === I.s && n.l === I.l);
}
for (const z of [...zoneNames].sort()) {
  const file = files[z] || z;
  try {
    posix[z] = tzFooter(file);
  } catch {}
  const PRE = new Date(-1);
  const location = keep(zname(z, PRE, 'longGeneric'), offsetOf(z, PRE), true);
  const periods = [];
  let I = zoneInfo(z, SAMPLES[0]);
  let cur = { sg: I.sg, lg: I.lg, names: new Map([[I.off, I]]) };
  for (let i = 1; i < SAMPLES.length; i++) {
    I = zoneInfo(z, SAMPLES[i]);
    if (!belongs(cur, I)) {
      let lo = SAMPLES[i - 1], hi = SAMPLES[i];
      while (hi - lo > 60000) {
        const mid = lo + Math.floor((hi - lo) / 120000) * 60000;
        if (belongs(cur, zoneInfo(z, mid))) lo = mid;
        else hi = mid;
      }
      cur.end = hi;
      periods.push(cur);
      cur = { sg: I.sg, lg: I.lg, names: new Map() };
    }
    if (!cur.names.has(I.off)) cur.names.set(I.off, I);
  }
  cur.end = 0;
  periods.push(cur);
  const enc = periods.map((p) => {
    const e = [p.end, p.sg, p.lg];
    for (const [off, n] of [...p.names].sort((a, b) => a[0] - b[0])) if (n.s || n.l) e.push(off, n.s, n.l);
    return e;
  });
  // merge neighbours that ended up the same
  const merged = [];
  for (const e of enc) {
    const last = merged[merged.length - 1];
    if (last && JSON.stringify(last.slice(1)) === JSON.stringify(e.slice(1))) last[0] = e[0];
    else merged.push(e);
  }
  const empty = !location && merged.every((e) => e.length === 3 && !e[1] && !e[2]);
  zones[z] = empty ? 0 : [location, ...merged];
}
out.tz = { strings, zones, links, files, posix, supported: Intl.supportedValuesOf('timeZone') };

// ---------------------------------------------------------------------------
// numbers

const currencies = {};
for (const c of Intl.supportedValuesOf('currency')) {
  const sym = new Intl.NumberFormat(L, { style: 'currency', currency: c }).formatToParts(1).find((p) => p.type === 'currency').value;
  const nar = new Intl.NumberFormat(L, { style: 'currency', currency: c, currencyDisplay: 'narrowSymbol' }).formatToParts(1).find((p) => p.type === 'currency').value;
  const one = new Intl.NumberFormat(L, { style: 'currency', currency: c, currencyDisplay: 'name', maximumFractionDigits: 0 }).formatToParts(1).find((p) => p.type === 'currency').value;
  const other = new Intl.NumberFormat(L, { style: 'currency', currency: c, currencyDisplay: 'name', maximumFractionDigits: 0 }).formatToParts(2).find((p) => p.type === 'currency').value;
  const digits = new Intl.NumberFormat(L, { style: 'currency', currency: c }).resolvedOptions().maximumFractionDigits;
  const e = [sym === c ? 0 : sym, nar === sym ? 0 : nar, one, other === one ? 0 : other];
  if (digits !== 2) e.push(digits);
  currencies[c] = e;
}

// unit patterns: [one, other] per display, "{0}" for the number
function unitPattern(unit, display, n) {
  const parts = new Intl.NumberFormat(L, { style: 'unit', unit, unitDisplay: display }).formatToParts(n);
  return parts.map((p) => (p.type === 'integer' ? '{0}' : p.value)).join('');
}
const units = {};
const simpleUnits = Intl.supportedValuesOf('unit');
for (const u of simpleUnits) {
  const e = {};
  for (const d of ['long', 'short', 'narrow']) {
    const one = unitPattern(u, d, 1), other = unitPattern(u, d, 2);
    e[d[0]] = one === other ? one : [one, other];
  }
  units[u] = e;
}
// "per" units: X-per-Y as the X pattern in Y's per pattern, unless special
const perPatterns = {};
for (const y of simpleUnits) {
  const e = {};
  for (const d of ['long', 'short', 'narrow']) {
    // derive Y's per pattern from a unit unlikely to be special-cased
    const s1 = unitPattern('bit-per-' + y, d, 2);
    const bit = unitPattern('bit', d, 2);
    e[d[0]] = s1.replace(bit.replace('{0}', '\u0000'), '\u0000').replace('\u0000', '{0}');
    // keep as the wrapper around X's text: "{0} per hour", "{0}/h"
    e[d[0]] = s1.startsWith(bit) ? '{0}' + s1.slice(bit.length) : s1;
  }
  perPatterns[y] = e;
}
const perSpecial = {};
for (const x of simpleUnits)
  for (const y of simpleUnits) {
    if (x === y) continue;
    for (const d of ['long', 'short', 'narrow'])
      for (const n of [1, 2]) {
        let actual;
        try { actual = unitPattern(x + '-per-' + y, d, n); } catch { continue; }
        const xu = units[x][d[0]], xp = Array.isArray(xu) ? xu[n === 1 ? 0 : 1] : xu;
        const wrap = perPatterns[y][d[0]];
        const expect = wrap.replace('{0}', xp);
        if (actual !== expect) perSpecial[x + '-per-' + y + '|' + d[0] + n] = actual;
      }
  }
out.number = { currencies, units, perPatterns, perSpecial };

// ---------------------------------------------------------------------------
// relative time, lists, plural categories, display names

const RT_UNITS = ['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'];
const rt = {};
for (const st of ['long', 'short', 'narrow']) {
  const always = new Intl.RelativeTimeFormat(L, { style: st, numeric: 'always' });
  const auto = new Intl.RelativeTimeFormat(L, { style: st, numeric: 'auto' });
  const e = {};
  for (const u of RT_UNITS) {
    const x = {
      f1: always.format(1, u).replace('1', '{0}'), f2: always.format(2, u).replace('2', '{0}'),
      p1: always.format(-1, u).replace('1', '{0}'), p2: always.format(-2, u).replace('2', '{0}'),
      auto: {},
    };
    for (const v of [-2, -1, 0, 1, 2]) {
      const a = auto.format(v, u);
      if (a !== always.format(v, u)) x.auto[v] = a;
    }
    // -0 and 0 in always mode
    x.z = always.format(0, u).replace('0', '{0}');
    x.nz = always.format(-0, u).replace('0', '{0}');
    e[u] = x;
  }
  rt[st] = e;
}

const lists = {};
for (const type of ['conjunction', 'disjunction', 'unit'])
  for (const style of ['long', 'short', 'narrow']) {
    const lf = new Intl.ListFormat(L, { type, style });
    const two = lf.format(['\u0001', '\u0002']);
    const three = lf.format(['\u0001', '\u0002', '\u0003']);
    const four = lf.format(['\u0001', '\u0002', '\u0003', '\u0004']);
    lists[type[0] + style[0]] = {
      pair: two.replace('\u0001', '{0}').replace('\u0002', '{1}'),
      start: three.slice(0, three.indexOf('\u0002')).replace('\u0001', '{0}') + '{1}',
      middle: four.slice(four.indexOf('\u0002'), four.indexOf('\u0003')).replace('\u0002', '{0}') + '{1}',
      end: '{0}' + three.slice(three.indexOf('\u0002') + 1).replace('\u0003', '{1}'),
    };
  }

function displayNames(type, codes, style = 'long') {
  const dn = new Intl.DisplayNames(L, { type, style, fallback: 'none' });
  const res = {};
  for (const c of codes) {
    let v;
    try { v = dn.of(c); } catch { continue; }
    if (v !== undefined) res[c] = v;
  }
  return res;
}
const A = 'abcdefghijklmnopqrstuvwxyz';
const langCodes = [];
for (const a of A) for (const b of A) langCodes.push(a + b);
for (const a of A) for (const b of A) for (const c of A) langCodes.push(a + b + c);
const extraLangs = ['en-US', 'en-GB', 'en-AU', 'en-CA', 'es-419', 'es-MX', 'fr-CA', 'pt-BR', 'pt-PT',
  'zh-Hans', 'zh-Hant', 'zh-CN', 'zh-TW', 'zh-HK', 'nl-BE', 'de-AT', 'de-CH', 'sr-Latn', 'ar-001'];
const regionCodes = [];
for (const a of A.toUpperCase()) for (const b of A.toUpperCase()) regionCodes.push(a + b);
for (let i = 1; i < 1000; i++) regionCodes.push(String(i).padStart(3, '0'));
const scriptCodes = ['Adlm', 'Arab', 'Aran', 'Armn', 'Beng', 'Bopo', 'Brai', 'Cakm', 'Cans', 'Cher', 'Cyrl',
  'Deva', 'Ethi', 'Geor', 'Grek', 'Gujr', 'Guru', 'Hanb', 'Hang', 'Hani', 'Hans', 'Hant', 'Hebr', 'Hira', 'Hrkt',
  'Jamo', 'Jpan', 'Kana', 'Khmr', 'Knda', 'Kore', 'Laoo', 'Latn', 'Mlym', 'Mong', 'Mtei', 'Mymr', 'Nkoo',
  'Olck', 'Orya', 'Rohg', 'Sinh', 'Sund', 'Syrc', 'Taml', 'Telu', 'Tfng', 'Thaa', 'Thai', 'Tibt', 'Vaii',
  'Yiii', 'Zmth', 'Zsye', 'Zsym', 'Zxxx', 'Zyyy', 'Zzzz'];
const DT_FIELDS = ['era', 'year', 'quarter', 'month', 'weekOfYear', 'weekday', 'day', 'dayPeriod', 'hour', 'minute',
  'second', 'timeZoneName'];
out.misc = {
  rt, lists,
  languages: { ...displayNames('language', langCodes), ...displayNames('language', extraLangs) },
  languagesShort: (() => { const l = displayNames('language', [...langCodes, ...extraLangs], 'short'), s = displayNames('language', [...langCodes, ...extraLangs]); const r = {}; for (const k in l) if (l[k] !== s[k]) r[k] = l[k]; return r; })(),
  regions: displayNames('region', regionCodes),
  regionsShort: (() => { const l = displayNames('region', regionCodes, 'short'), s = displayNames('region', regionCodes); const r = {}; for (const k in l) if (l[k] !== s[k]) r[k] = l[k]; return r; })(),
  scripts: displayNames('script', scriptCodes),
  currencyNames: displayNames('currency', langCodes.filter((c) => c.length === 3).map((c) => c.toUpperCase())),
  calendars: displayNames('calendar', Intl.supportedValuesOf('calendar')),
  dateTimeFields: displayNames('dateTimeField', DT_FIELDS),
  dateTimeFieldsShort: displayNames('dateTimeField', DT_FIELDS, 'short'),
  dateTimeFieldsNarrow: displayNames('dateTimeField', DT_FIELDS, 'narrow'),
  supported: {
    calendar: Intl.supportedValuesOf('calendar'), collation: Intl.supportedValuesOf('collation'),
    currency: Intl.supportedValuesOf('currency'), numberingSystem: Intl.supportedValuesOf('numberingSystem'),
    unit: Intl.supportedValuesOf('unit'),
  },
};

// locales ICU has data for (a requested locale resolves to the longest of
// these it starts with), and the likely subtags of their languages
{
  const langs = Object.keys(out.misc.languages).filter((c) => /^[a-z]{2,3}$/.test(c));
  const regions = Object.keys(out.misc.regions).filter((c) => /^[A-Z]{2}$/.test(c)).concat(['001', '419', '150']);
  const scripts = ['Arab', 'Cyrl', 'Latn', 'Hans', 'Hant', 'Deva', 'Guru', 'Adlm', 'Vaii', 'Tfng', 'Beng', 'Mtei', 'Olck', 'Dsrt', 'Shaw', 'Jpan', 'Kore', 'Mong', 'Rohg', 'Nkoo', 'Ethi', 'Hebr', 'Grek', 'Geor', 'Armn', 'Thai', 'Tibt', 'Mymr', 'Khmr', 'Laoo', 'Sinh', 'Taml', 'Telu', 'Knda', 'Mlym', 'Gujr', 'Orya'];
  const ok = (t) => new Intl.DateTimeFormat(t).resolvedOptions().locale === t;
  const avail = [];
  const likely = {};
  for (const l of langs) {
    if (!ok(l)) continue;
    avail.push(l);
    for (const g of regions) if (ok(l + '-' + g)) avail.push(l + '-' + g);
    for (const sc of scripts) {
      if (!ok(l + '-' + sc)) continue;
      avail.push(l + '-' + sc);
      for (const g of regions) if (ok(l + '-' + sc + '-' + g)) avail.push(l + '-' + sc + '-' + g);
    }
  }
  for (const l of langs) {
    try {
      const m = new Intl.Locale(l).maximize();
      if (m.language === l && m.script && m.region) likely[l] = m.script + '-' + m.region;
    } catch {}
  }
  // the exceptions: by language and region (script), language and script
  // (region), and for und
  const likelyExtra = {};
  const max = (t) => { try { return new Intl.Locale(t).maximize(); } catch { return null; } };
  for (const l of Object.keys(likely)) {
    if (!avail.includes(l)) continue;
    const [ds, dr] = likely[l].split('-');
    for (const g of regions) {
      const m = max(l + '-' + g);
      if (m && m.language === l && m.script !== ds) likelyExtra[l + '-' + g] = m.script;
    }
    for (const sc of scripts) {
      const m = max(l + '-' + sc);
      if (m && m.language === l && m.region !== dr) likelyExtra[l + '-' + sc] = m.region;
    }
  }
  for (const g of regions) {
    const m = max('und-' + g);
    if (m) likelyExtra['und-' + g] = m.language + '-' + m.script;
  }
  for (const sc of scripts) {
    const m = max('und-' + sc);
    if (m) likelyExtra['und-' + sc] = m.language + '-' + m.region;
  }
  // per region: week (when not Monday first, Saturday-Sunday weekend), hour
  // cycle (when not h23), time zones
  const week = {}, hourCycle = {}, timeZones = {};
  for (const g of regions) {
    const m = max('und-' + g);
    if (!m) continue;
    const loc = new Intl.Locale(m.language + '-' + g);
    const w = loc.getWeekInfo();
    const ws = w.firstDay + '|' + w.weekend.join(',');
    if (ws !== '1|6,7') week[g] = ws;
    const hc = loc.getHourCycles()[0];
    if (hc !== 'h23') hourCycle[g] = hc;
    const tz = loc.getTimeZones();
    if (tz) timeZones[g] = tz.join(' ');
  }
  // right-to-left languages and scripts
  const rtl = langs.filter((l) => new Intl.Locale(l).getTextInfo().direction === 'rtl');
  const rtlScripts = scripts.filter((sc) => new Intl.Locale('und-' + sc).getTextInfo().direction === 'rtl');
  // collations (beyond emoji and eor)
  const collations = {};
  for (const l of langs) {
    const c = new Intl.Locale(l).getCollations().filter((x) => x !== 'emoji' && x !== 'eor');
    if (c.length) collations[l] = c.join(' ');
  }
  // hour cycles of locales that differ from their region's (fr-CA)
  const hourCycleLocale = {};
  for (const t of avail) {
    const m = max(t);
    if (!m || !m.region) continue;
    const hc = new Intl.Locale(t).getHourCycles()[0];
    if (hc !== (hourCycle[m.region] || 'h23')) hourCycleLocale[m.language + '-' + m.region] = hc;
  }
  // the locales of services that have fewer (ICU has collation and plural
  // rules mostly per language); 'same' for those like DateTimeFormat
  const services = {};
  for (const [name, make] of [['collator', (t) => new Intl.Collator(t)], ['plural', (t) => new Intl.PluralRules(t)],
                              ['relative', (t) => new Intl.RelativeTimeFormat(t)], ['list', (t) => new Intl.ListFormat(t)],
                              ['display', (t) => new Intl.DisplayNames(t, { type: 'region' })],
                              ['segmenter', (t) => new Intl.Segmenter(t)], ['number', (t) => new Intl.NumberFormat(t)]]) {
    const set = new Set();
    for (const t of avail) {
      // (not the default locale ICU falls back to)
      const r = make(t).resolvedOptions().locale;
      if (r === t || t.startsWith(r + '-')) set.add(r);
    }
    services[name] = set.size === avail.length ? 'same' : [...set].sort().join(' ');
  }
  out.locales = { available: avail.join(' '), likely, likelyExtra, week, hourCycle, hourCycleLocale, timeZones,
                  rtl: rtl.join(' '), rtlScripts: rtlScripts.join(' '), collations, services, aliases: localeAliases() };
}

// canonicalization (UTS #35 with CLDR's alias data, as ICU applies it):
// language subtags (all two- and three-letter ones tried), regions,
// language-region and language-variant pairs, variants
function localeAliases() {
  const canon = (t) => { try { return Intl.getCanonicalLocales(t)[0]; } catch { return null; } };
  const A = 'abcdefghijklmnopqrstuvwxyz';
  const language = {}, region = {}, pair = {}, variant = {};
  for (const a of A) {
    for (const b of A) {
      const t = a + b;
      const c = canon(t);
      if (c && c !== t) language[t] = c;
      for (const d of A) {
        const c3 = canon(t + d);
        if (c3 && c3 !== t + d) language[t + d] = c3;
      }
    }
  }
  const regions = [];
  for (const a of A.toUpperCase()) for (const b of A.toUpperCase()) regions.push(a + b);
  for (let i = 0; i < 1000; i++) regions.push(String(i).padStart(3, '0'));
  for (const r of regions) {
    const c = canon('und-' + r);
    if (c && c !== 'und-' + r) region[r] = c.slice(4);
    const g = canon('sgn-' + r);
    if (g && g !== 'sgn-' + (region[r] || r)) pair['sgn-' + r] = g;
  }
  for (const t of ['art-lojban', 'cel-gaulish', 'zh-guoyu', 'zh-hakka', 'zh-xiang', 'zh-min', 'zh-wuu', 'zh-yue',
                   'hy-arevela', 'hy-arevmda', 'sv-aaland', 'no-bokmal', 'no-nynorsk', 'en-scouse']) {
    const c = canon(t);
    if (c && c !== t) pair[t] = c;
  }
  for (const v of ['heploc', 'polytoni', 'arevela', 'arevmda', 'aaland', 'bokmal', 'nynorsk', 'posix', 'saaho',
                   'scouse', 'boont', 'hepburn', 'lojban', 'gaulish', 'guoyu', 'hakka', 'xiang']) {
    const c = canon('und-Latn-' + v);
    if (c && c !== 'und-Latn-' + v) variant[v] = c.slice(9);
  }
  return { language, region, pair, variant };
}

// collation: the root order of common punctuation and symbols (as one
// string; DEL marks a character with the same primary weight as the one
// before), and where the symbols start
{
  const c = new Intl.Collator('en'), base = new Intl.Collator('en', { sensitivity: 'base' });
  const ranges = [[0x20, 0x7e], [0xa0, 0xbf], [0xd7, 0xd7], [0xf7, 0xf7], [0x2b9, 0x2ff], [0x2010, 0x205e],
                  [0x20a0, 0x20c0], [0x2100, 0x214f], [0x2190, 0x23ff], [0x2500, 0x27bf], [0x2900, 0x2bff],
                  [0x3000, 0x303f], [0xfe30, 0xfe6b], [0xff01, 0xff0f]];
  const chars = [];
  for (const [a, b] of ranges) {
    for (let i = a; i <= b; i++) {
      const ch = String.fromCodePoint(i);
      if (/[\p{P}\p{S}\p{Zs}]/u.test(ch) && ch.normalize('NFKD') === ch) chars.push(ch);
    }
  }
  chars.sort(c.compare);
  let order = '', groups = 0, symbols = -1, currency = -1;
  for (let i = 0; i < chars.length; i++) {
    if (i && base.compare(chars[i - 1], chars[i]) === 0) {
      order += '\x7f';
    } else {
      if (symbols < 0 && !/[\p{P}\p{Zs}]/u.test(chars[i])) symbols = groups;
      if (currency < 0 && /\p{Sc}/u.test(chars[i])) currency = groups;
      groups++;
    }
    order += chars[i];
  }
  // secondary weights: combining marks in order (DEL: the same weight as
  // the one before), and letters that are another letter with a secondary
  // difference (their rank among the marks, .5 between two)
  const acc = new Intl.Collator('en', { sensitivity: 'accent' });
  const marks = [];
  for (const [a, b] of [[0x300, 0x36f], [0x483, 0x489], [0x591, 0x5c7], [0x610, 0x61a], [0x64b, 0x65f], [0x670, 0x670],
                        [0x6d6, 0x6ed], [0x900, 0x903], [0x93a, 0x94f], [0x951, 0x957], [0x962, 0x963], [0xe31, 0xe3a],
                        [0xe47, 0xe4e], [0x1ab0, 0x1ace], [0x1dc0, 0x1dff], [0x20d0, 0x20f0], [0x3099, 0x309a],
                        [0xfe20, 0xfe2f]]) {
    for (let i = a; i <= b; i++) {
      const m = String.fromCodePoint(i);
      if (/\p{M}/u.test(m)) marks.push(m);
    }
  }
  marks.sort((x, y) => acc.compare('a' + x, 'a' + y));
  let mstr = '';
  const rank = [];
  let r = -1;
  for (let i = 0; i < marks.length; i++) {
    if (i && acc.compare('a' + marks[i - 1], 'a' + marks[i]) === 0) mstr += '\x7f';
    else r++;
    mstr += marks[i];
    rank.push(r);
  }
  const special = {};
  for (const [ch, b] of [['ø', 'o'], ['ł', 'l'], ['đ', 'd'], ['ð', 'd'], ['ħ', 'h'], ['ŀ', 'l'], ['ſ', 's'],
                         ['æ', 'ae'], ['œ', 'oe'], ['ß', 'ss']]) {
    const t = (m) => b[0] + m + b.slice(1);
    let k = 0;
    while (k < marks.length && acc.compare(t(marks[k]), ch) < 0) k++;
    special[ch] = [b, k < marks.length && acc.compare(t(marks[k]), ch) === 0 ? rank[k] : (k ? rank[k - 1] : -1) + 0.5];
  }
  out.collation = { order, symbols, currency, marks: mstr, special };
}

// number formatting per locale: symbols, grouping, digits, and the patterns
// (as tokens: n number, m minus, p plus, % percent, c currency, or literal
// text) of decimal, percent and currency numbers; records are shared
{
  const CURS = ['USD', 'EUR', 'JPY', 'GBP', 'CNY', 'KRW', 'INR', 'CAD', 'AUD', 'CHF', 'HKD', 'SGD', 'SEK', 'NOK',
                'DKK', 'PLN', 'CZK', 'HUF', 'RUB', 'TRY', 'BRL', 'MXN', 'ARS', 'CLP', 'COP', 'ZAR', 'ILS', 'AED',
                'SAR', 'THB', 'TWD', 'IDR', 'MYR', 'PHP', 'VND', 'NZD', 'EGP', 'NGN', 'PKR', 'UAH'];
  const enSym = {};
  const enNarrow = {};
  for (const c of CURS) {
    enSym[c] = new Intl.NumberFormat('en', { style: 'currency', currency: c }).formatToParts(1).find((p) => p.type === 'currency').value;
    enNarrow[c] = new Intl.NumberFormat('en', { style: 'currency', currency: c, currencyDisplay: 'narrowSymbol' }).formatToParts(1).find((p) => p.type === 'currency').value;
  }
  const tokens = (parts) => {
    const t = [];
    for (const p of parts) {
      let k;
      switch (p.type) {
        case 'integer': case 'group': case 'decimal': case 'fraction': case 'nan': case 'infinity': k = 'n'; break;
        case 'minusSign': k = 'm'; break;
        case 'plusSign': k = 'p'; break;
        case 'percentSign': k = '%'; break;
        case 'currency': k = 'c'; break;
        default: k = p.value; // literal
      }
      if (k === 'n' && t[t.length - 1] === 'n') continue;
      t.push(k);
    }
    return t;
  };
  const records = [], recIndex = new Map(), map = {}, syms = [], symIndex = new Map();
  // templates are shared too
  const templates = [], tplIndex = new Map();
  const intern = (t) => {
    const k = JSON.stringify(t);
    let i = tplIndex.get(k);
    if (i === undefined) {
      i = templates.length;
      templates.push(t);
      tplIndex.set(k, i);
    }
    return i;
  };
  const locs = out.locales.available.split(' ');
  for (const loc of locs) {
    const nf = (o) => new Intl.NumberFormat(loc, o);
    const parts = nf({}).formatToParts(-1234567.891);
    const get = (ps, type) => (ps.find((p) => p.type === type) || {}).value;
    const r = {};
    r.decimal = get(parts, 'decimal');
    r.group = get(parts, 'group');
    r.minus = get(parts, 'minusSign');
    r.plus = get(nf({ signDisplay: 'always' }).formatToParts(1), 'plusSign');
    r.percent = get(nf({ style: 'percent' }).formatToParts(0.5), 'percentSign');
    r.exp = get(nf({ notation: 'scientific' }).formatToParts(1234), 'exponentSeparator');
    r.nan = nf({}).format(NaN);
    r.inf = nf({}).format(Infinity);
    r.approx = get(nf({}).formatRangeToParts(1, 1), 'approximatelySign');
    r.range = nf({}).formatRangeToParts(3, 5).find((p) => p.type === 'literal' && p.source === 'shared').value;
    // grouping: sizes and the minimum digits
    const big = nf({}).formatToParts(123456789012).filter((p) => p.type === 'integer').map((p) => [...p.value].length);
    r.g1 = big[big.length - 1];
    r.g2 = big.length > 2 ? big[big.length - 2] : r.g1;
    r.mg = nf({}).formatToParts(1234).some((p) => p.type === 'group') ? 1 : 2;
    const ns = nf({}).resolvedOptions().numberingSystem;
    if (ns !== 'latn') r.digits = [...nf({ useGrouping: false }).format(1234567890)].slice(-1).concat([...nf({ useGrouping: false }).format(123456789)]).join('');
    r.nu = ns;
    // the patterns from infinities: no digits, so no currency spacing in them
    const tpl = (o) => [tokens(nf(o).formatToParts(Infinity)), tokens(nf(o).formatToParts(-Infinity)),
                        tokens(nf({ ...o, signDisplay: 'always' }).formatToParts(Infinity))];
    r.d = intern(tpl({}));
    r.p = intern(tpl({ style: 'percent' }));
    // currency patterns: from a currency shown as a symbol (EUR has
    // patterns of its own in some locales: en-150 "-€1.00" but "-1.00 £")
    const symOf = (c) => nf({ style: 'currency', currency: c }).formatToParts(1).find((p) => p.type === 'currency').value;
    const C0 = ['GBP', 'JPY', 'KRW', 'ILS', 'INR', 'NGN', 'USD', 'EUR'].find((c) => /^\p{S}+$/u.test(symOf(c))) || 'GBP';
    const ctpl = (c) => intern(tpl({ style: 'currency', currency: c }));
    const atpl = (c) => intern([tokens(nf({ style: 'currency', currency: c, currencySign: 'accounting' }).formatToParts(Infinity)),
                                tokens(nf({ style: 'currency', currency: c, currencySign: 'accounting' }).formatToParts(-Infinity))]);
    r.c = ctpl(C0);
    r.a = atpl(C0);
    // currency formats may have symbols of their own (de-AT, fr-CH)
    const cp = nf({ style: 'currency', currency: C0, minimumFractionDigits: 2 }).formatToParts(-1234567.891);
    if (get(cp, 'decimal') !== r.decimal) r.cdecimal = get(cp, 'decimal');
    if (get(cp, 'group') !== r.group) r.cgroup = get(cp, 'group');
    for (const c of CURS) {
      const t = ctpl(c), a = atpl(c);
      const xp = nf({ style: 'currency', currency: c, minimumFractionDigits: 2 }).formatToParts(-1234567.891);
      const xd = get(xp, 'decimal'), xg = get(xp, 'group');
      if (t !== r.c || a !== r.a || xd !== (r.cdecimal || r.decimal) || xg !== (r.cgroup || r.group))
        (r.cpat || (r.cpat = {}))[c] = [t, a, xd, xg];
    }
    // a negative exponent: the parts between the separator and the digits
    {
      const ep = nf({ notation: 'scientific' }).formatToParts(0.001);
      const i = ep.findIndex((p) => p.type === 'exponentSeparator'), j = ep.findIndex((p) => p.type === 'exponentInteger');
      r.expNeg = ep.slice(i + 1, j).map((p) => (p.type === 'exponentMinusSign' ? 'm' : p.value));
    }
    // grouping sizes of the percent, currency and accounting patterns,
    // where they differ (bn: #,##,##0 but #,##0%)
    const sizes = (o) => {
      const g = nf(o).formatToParts(123456789012).filter((p) => p.type === 'integer').map((p) => [...p.value].length);
      return [g[g.length - 1], g.length > 2 ? g[g.length - 2] : g[g.length - 1]];
    };
    for (const [k, o] of [['gp', { style: 'percent' }], ['gc', { style: 'currency', currency: 'EUR' }],
                          ['ga', { style: 'currency', currency: 'EUR', currencySign: 'accounting' }]]) {
      const z = sizes(o);
      if (z[0] !== r.g1 || z[1] !== r.g2) r[k] = z;
    }
    const sym = {};
    for (const c of CURS) {
      const v = nf({ style: 'currency', currency: c }).formatToParts(1).find((p) => p.type === 'currency').value;
      if (v !== enSym[c]) sym[c] = v;
      const w = nf({ style: 'currency', currency: c, currencyDisplay: 'narrowSymbol' }).formatToParts(1).find((p) => p.type === 'currency').value;
      if (w !== enNarrow[c]) sym['n' + c] = w;
    }
    const sk = JSON.stringify(sym);
    let si = symIndex.get(sk);
    if (si === undefined) {
      si = syms.length;
      syms.push(sym);
      symIndex.set(sk, si);
    }
    r.sym = si;
    const key = JSON.stringify(r);
    let i = recIndex.get(key);
    if (i === undefined) {
      i = records.length;
      records.push(r);
      recIndex.set(key, i);
    }
    map[loc] = i;
  }
  out.nlocales = { records, templates, syms, map };
}

// sections are JSON text in the JSON, parsed when first used
const final = { icu: out.icu, cldr: out.cldr, tzdata: process.versions.tz, sections: {} };
for (const k of ['date', 'tz', 'number', 'misc', 'locales', 'collation', 'nlocales']) final.sections[k] = JSON.stringify(out[k]);
process.stdout.write(JSON.stringify(final) + '\n');
