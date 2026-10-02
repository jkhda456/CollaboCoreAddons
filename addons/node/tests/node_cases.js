// Checks of the Node.js APIs the add-on provides, run by tests/run.sh with the native build and
// in the guest: `node node_cases.js [WORKDIR] [NAME...]`. One PASS/FAIL line per case; the exit
// status is the number of failures.
'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const work = fs.mkdtempSync(path.join(process.argv[2] || os.tmpdir(), 'node-cases-'));
const only = process.argv.slice(3);
const cases = [];
const test = (name, fn) => cases.push({ name, fn });

// A self-signed certificate for localhost and 127.0.0.1 (test only, valid until 2126).
const KEY = `-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgwJJ66wXC25AWwBTg
/yPyMAgglbmseZRQZPFquz42GC2hRANCAASownEsyoi2ff3A10X057TdZky+pEku
9qAMuxua2ATRMWZFAI6vnam0anOqHt8f1dq2C7eVI3ALxGmkxZAvhFjE
-----END PRIVATE KEY-----
`;
const CERT = `-----BEGIN CERTIFICATE-----
MIIBmzCCAUGgAwIBAgIUdrpnOsSPzZ7JP6ZmLxCNSrwPZQkwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MDkzMDA0NTczM1oYDzIxMjYwOTA2
MDQ1NzMzWjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAASownEsyoi2ff3A10X057TdZky+pEku9qAMuxua2ATRMWZFAI6vnam0
anOqHt8f1dq2C7eVI3ALxGmkxZAvhFjEo28wbTAdBgNVHQ4EFgQUEPlU1E1ysXRf
fRPLlFS3zBtSucAwHwYDVR0jBBgwFoAUEPlU1E1ysXRffRPLlFS3zBtSucAwDwYD
VR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SHBH8AAAEwCgYIKoZI
zj0EAwIDSAAwRQIhAOyX7FpA1PpaHigRZYt9Uz5m0H5DD6I3xpZnJDesPoW9AiBn
M2w8dmIFwvg/PAAqdi6XNReVPGFZheheWoKCeQey+w==
-----END CERTIFICATE-----
`;

const listen = (server) => new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server.address().port)));
const body = (res) => new Promise((resolve, reject) => {
  let data = '';
  res.setEncoding('utf8');
  res.on('data', (d) => { data += d; });
  res.on('end', () => resolve(data));
  res.on('error', reject);
});

test('process and globals', () => {
  assert.strictEqual(process.version, 'v24.21.0');
  assert.strictEqual(process.platform, 'linux');
  assert.ok(['x64', 'arm64'].includes(process.arch));
  assert.strictEqual(typeof process.hrtime.bigint(), 'bigint');
  assert.deepStrictEqual(structuredClone({ a: [1, new Map([[1, 2]])] }), { a: [1, new Map([[1, 2]])] });
  assert.strictEqual(new URL('../b?x=1#h', 'https://example.com/a/c').href, 'https://example.com/b?x=1#h');
  assert.strictEqual(new TextDecoder().decode(new TextEncoder().encode('héllo ✓')), 'héllo ✓');
  assert.strictEqual(Buffer.from('aGVsbG8', 'base64url').toString(), 'hello');
  assert.strictEqual(require('util').inspect({ a: 1, b: 'x' }), "{ a: 1, b: 'x' }");
  assert.strictEqual(require('util').format('%s=%d', 'n', 42), 'n=42');
});

test('error stacks name the file and line', () => {
  const stack = new Error('here').stack.split('\n');
  assert.match(stack[1], /node_cases\.js:\d+:\d+/);
  const line = Number(/node_cases\.js:(\d+)/.exec(stack[1])[1]);
  assert.match(fs.readFileSync(__filename, 'utf8').split('\n')[line - 1], /new Error\('here'\)/);
});

test('fs', async () => {
  const f = path.join(work, 'a.txt');
  fs.writeFileSync(f, 'one\n');
  fs.appendFileSync(f, 'two\n');
  assert.strictEqual(fs.readFileSync(f, 'utf8'), 'one\ntwo\n');
  assert.strictEqual(fs.statSync(f).size, 8);
  fs.mkdirSync(path.join(work, 'd/e/f'), { recursive: true });
  fs.renameSync(f, path.join(work, 'd/e/f/b.txt'));
  assert.deepStrictEqual(fs.readdirSync(path.join(work, 'd/e/f')), ['b.txt']);
  const entries = await fs.promises.readdir(path.join(work, 'd'), { recursive: true });
  assert.deepStrictEqual(entries.sort(), ['e', 'e/f', 'e/f/b.txt']);
  fs.symlinkSync('e/f/b.txt', path.join(work, 'd/link'));
  assert.strictEqual(await fs.promises.readFile(path.join(work, 'd/link'), 'utf8'), 'one\ntwo\n');
  await fs.promises.rm(path.join(work, 'd'), { recursive: true });
  assert.ok(!fs.existsSync(path.join(work, 'd')));
  assert.throws(() => fs.readFileSync(path.join(work, 'missing')), { code: 'ENOENT' });
});

test('fs.watch', async () => {
  // inotify natively; stat polling in the guest (no inotify there)
  const dir = fs.mkdtempSync(path.join(work, 'watch-'));
  const seen = [];
  const w = fs.watch(dir, (type, name) => seen.push(`${type}:${name}`));
  const until = async (what) => {
    for (let i = 0; i < 50 && !seen.includes(what); i++) await new Promise((r) => setTimeout(r, 100));
    assert.ok(seen.includes(what), `${what} not in ${seen}`);
  };
  try {
    await new Promise((r) => setTimeout(r, 100));
    fs.writeFileSync(path.join(dir, 'a.txt'), 'one');
    await until('rename:a.txt');
    await new Promise((r) => setTimeout(r, 50));
    fs.appendFileSync(path.join(dir, 'a.txt'), 'two');
    await until('change:a.txt');
    fs.unlinkSync(path.join(dir, 'a.txt'));
  } finally {
    w.close();
  }
});

test('streams: pipeline through gzip and back', async () => {
  const zlib = require('zlib');
  const { pipeline } = require('stream/promises');
  const src = path.join(work, 'big.txt');
  fs.writeFileSync(src, 'line of text\n'.repeat(20000));
  await pipeline(fs.createReadStream(src), zlib.createGzip(), fs.createWriteStream(src + '.gz'));
  await pipeline(fs.createReadStream(src + '.gz'), zlib.createGunzip(), fs.createWriteStream(src + '.out'));
  assert.ok(fs.statSync(src + '.gz').size < 2000);
  assert.ok(fs.readFileSync(src + '.out').equals(fs.readFileSync(src)));
});

test('zlib: gzip, deflate, brotli, zstd', () => {
  const zlib = require('zlib');
  const data = Buffer.from('collaboCore '.repeat(1000));
  assert.ok(zlib.gunzipSync(zlib.gzipSync(data)).equals(data));
  assert.ok(zlib.inflateRawSync(zlib.deflateRawSync(data, { level: 9 })).equals(data));
  assert.ok(zlib.brotliDecompressSync(zlib.brotliCompressSync(data)).equals(data));
  assert.ok(zlib.zstdDecompressSync(zlib.zstdCompressSync(data)).equals(data));
  assert.strictEqual(zlib.crc32('hello'), 907060870);
});

test('child_process', async () => {
  const cp = require('child_process');
  assert.strictEqual(cp.execFileSync('sh', ['-c', 'echo $((6*7))'], { encoding: 'utf8' }), '42\n');
  const r = cp.spawnSync('sh', ['-c', 'echo out; echo err >&2; exit 3'], { encoding: 'utf8' });
  assert.deepStrictEqual([r.status, r.stdout, r.stderr], [3, 'out\n', 'err\n']);
  const child = cp.spawn('sh', ['-c', 'read x; echo "got $x"; echo "$FOO"'], { env: { ...process.env, FOO: 'bar' } });
  child.stdin.end('input\n');
  const [out, code] = await Promise.all([body(child.stdout), new Promise((resolve) => child.on('close', resolve))]);
  assert.deepStrictEqual([out, code], ['got input\nbar\n', 0]);
  const fromNode = cp.execFileSync(process.execPath, ['-e', 'console.log(process.argv.length, 1 + 1)'], { encoding: 'utf8' });
  assert.strictEqual(fromNode, '1 2\n');
});

test('http: server, client, fetch', async () => {
  const http = require('http');
  const server = http.createServer((req, res) => {
    let data = '';
    req.on('data', (d) => { data += d; });
    req.on('end', () => {
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ method: req.method, url: req.url, body: data, ua: req.headers['user-agent'] || null }));
    });
  });
  const port = await listen(server);
  try {
    const got = await new Promise((resolve, reject) => {
      http.get(`http://127.0.0.1:${port}/x?y=1`, (res) => body(res).then(resolve, reject)).on('error', reject);
    });
    assert.deepStrictEqual(JSON.parse(got), { method: 'GET', url: '/x?y=1', body: '', ua: null });
    const res = await fetch(`http://127.0.0.1:${port}/post`, { method: 'POST', body: 'payload' });
    assert.strictEqual(res.status, 200);
    const j = await res.json();
    assert.deepStrictEqual([j.method, j.body, j.ua], ['POST', 'payload', 'node']);
    // keep-alive: several requests on one agent
    const agent = new http.Agent({ keepAlive: true });
    for (let i = 0; i < 5; i++) {
      await new Promise((resolve, reject) => http.get({ port, host: '127.0.0.1', path: '/' + i, agent }, (r) => body(r).then(resolve, reject)));
    }
    agent.destroy();
  } finally {
    server.close();
  }
});

test('https and tls', async () => {
  const https = require('https');
  const server = https.createServer({ key: KEY, cert: CERT }, (req, res) => res.end('secure ' + req.socket.getProtocol()));
  const port = await listen(server);
  try {
    const got = await new Promise((resolve, reject) => {
      https.get({ host: 'localhost', port, ca: CERT, path: '/' }, (res) => body(res).then(resolve, reject)).on('error', reject);
    });
    assert.strictEqual(got, 'secure TLSv1.3');
    await assert.rejects(new Promise((resolve, reject) => {
      https.get({ host: 'localhost', port, path: '/' }, resolve).on('error', reject);
    }), { code: 'DEPTH_ZERO_SELF_SIGNED_CERT' });
  } finally {
    server.close();
  }
});

test('http2: h2c and h2 over TLS, GET, POST, trailers', async () => {
  const http2 = require('http2');
  const onStream = (stream, headers) => {
    if (headers[':method'] === 'POST') {
      const chunks = [];
      stream.on('data', (d) => chunks.push(d));
      stream.on('end', () => {
        stream.respond({ ':status': 200 }, { waitForTrailers: true });
        stream.on('wantTrailers', () => stream.sendTrailers({ 'x-sum': String(Buffer.concat(chunks).length) }));
        stream.end(Buffer.concat(chunks).toString().toUpperCase());
      });
    } else {
      stream.respond({ ':status': 200, 'x-path': headers[':path'] });
      stream.end('h2 ' + headers[':path']);
    }
  };
  const exchange = (client, headers, body) => new Promise((resolve, reject) => {
    const req = client.request(headers);
    let data = '', response, trailers;
    req.setEncoding('utf8');
    req.on('response', (h) => { response = h; });
    req.on('trailers', (t) => { trailers = t; });
    req.on('data', (d) => { data += d; });
    req.on('end', () => resolve({ response, data, trailers }));
    req.on('error', reject);
    req.end(body);
  });
  for (const secure of [false, true]) {
    const server = secure ? http2.createSecureServer({ key: KEY, cert: CERT }) : http2.createServer();
    server.on('stream', onStream);
    const port = await listen(server);
    const client = http2.connect(`${secure ? 'https' : 'http'}://localhost:${port}`, secure ? { ca: CERT } : {});
    try {
      const get = await exchange(client, { ':path': '/get' });
      assert.deepStrictEqual([get.response[':status'], get.response['x-path'], get.data], [200, '/get', 'h2 /get']);
      const post = await exchange(client, { ':path': '/post', ':method': 'POST' }, 'payload'.repeat(5000));
      assert.strictEqual(post.data, 'PAYLOAD'.repeat(5000));
      assert.strictEqual(post.trailers['x-sum'], '35000');
      if (secure)
        assert.strictEqual(client.alpnProtocol, 'h2');
    } finally {
      client.close();
      server.close();
    }
  }
});

test('net: echo over TCP', async () => {
  const net = require('net');
  const server = net.createServer((s) => s.pipe(s));
  const port = await listen(server);
  const sock = net.connect(port, '127.0.0.1');
  sock.end('echo me');
  assert.strictEqual(await body(sock), 'echo me');
  server.close();
});

test('dns and os', async () => {
  const { address } = await require('dns').promises.lookup('localhost', { family: 4 });
  assert.strictEqual(address, '127.0.0.1');
  assert.ok(os.cpus().length >= 1);
  assert.ok(os.totalmem() > 0);
  assert.strictEqual(typeof os.hostname(), 'string');
  assert.strictEqual(os.EOL, '\n');
});

test('crypto', async () => {
  const crypto = require('crypto');
  assert.strictEqual(crypto.createHash('sha256').update('abc').digest('hex'),
                     'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  assert.strictEqual(crypto.createHmac('sha1', 'key').update('data').digest('base64'), 'EEFSxb/coHvGM+69RhmfAlXJ9J0=');
  assert.match(crypto.randomUUID(), /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
  assert.strictEqual(crypto.pbkdf2Sync('pw', 'salt', 1000, 16, 'sha256').toString('hex'), '0a38253555ce37f5c72a6b703f996814');
  const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');
  const sig = crypto.sign(null, Buffer.from('msg'), privateKey);
  assert.ok(crypto.verify(null, Buffer.from('msg'), publicKey, sig));
  const key = crypto.randomBytes(32), iv = crypto.randomBytes(12);
  const c = crypto.createCipheriv('aes-256-gcm', key, iv);
  const enc = Buffer.concat([c.update('secret text'), c.final()]);
  const d = crypto.createDecipheriv('aes-256-gcm', key, iv);
  d.setAuthTag(c.getAuthTag());
  assert.strictEqual(Buffer.concat([d.update(enc), d.final()]).toString(), 'secret text');
  const digest = await crypto.webcrypto.subtle.digest('SHA-1', new TextEncoder().encode('abc'));
  assert.strictEqual(Buffer.from(digest).toString('hex'), 'a9993e364706816aba3e25717850c26c9cd0d89d');
  assert.strictEqual(new crypto.X509Certificate(CERT).subject, 'CN=localhost');
});

test('ES modules: static, dynamic, JSON, top-level await', async () => {
  fs.writeFileSync(path.join(work, 'lib.mjs'), 'export const two = 2; export default function add(a, b) { return a + b; }\n');
  fs.writeFileSync(path.join(work, 'data.json'), '{"answer": 42}\n');
  fs.writeFileSync(path.join(work, 'main.mjs'), [
    "import add, { two } from './lib.mjs';",
    "import data from './data.json' with { type: 'json' };",
    "import { readFileSync } from 'node:fs';",
    "const { sep } = await import('node:path');",
    "export const result = [add(two, 3), data.answer, typeof readFileSync, sep, import.meta.filename.endsWith('main.mjs')];",
  ].join('\n'));
  const { result } = await import(require('url').pathToFileURL(path.join(work, 'main.mjs')).href);
  assert.deepStrictEqual(result, [5, 42, 'function', '/', true]);
  // require() of an ES module (Node 22.12+)
  assert.strictEqual(require(path.join(work, 'lib.mjs')).two, 2);
});

test('WebAssembly', async () => {
  // (module (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add))
  const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 7, 1, 96, 2, 127, 127, 1, 127, 3, 2, 1, 0,
    7, 7, 1, 3, 97, 100, 100, 0, 0, 10, 9, 1, 7, 0, 32, 0, 32, 1, 106, 11]);
  const { instance } = await WebAssembly.instantiate(bytes);
  assert.strictEqual(instance.exports.add(40, 2), 42);
});

test('timers, microtasks, AsyncLocalStorage, AbortSignal', async () => {
  const order = [];
  // scheduled from a timer callback: ticks, then microtasks, then the check phase
  await new Promise((resolve) => setTimeout(() => {
    setTimeout(() => { order.push('timeout'); resolve(); }, 5);
    setImmediate(() => order.push('immediate'));
    queueMicrotask(() => order.push('microtask'));
    process.nextTick(() => order.push('tick'));
  }, 0));
  assert.deepStrictEqual(order, ['tick', 'microtask', 'immediate', 'timeout']);
  const { setTimeout: sleep } = require('timers/promises');
  await assert.rejects(sleep(10000, null, { signal: AbortSignal.timeout(20) }), { name: 'AbortError' });
  const { AsyncLocalStorage } = require('async_hooks');
  const als = new AsyncLocalStorage();
  const seen = await als.run({ id: 7 }, async () => {
    await sleep(1);
    return new Promise((resolve) => setTimeout(() => resolve(als.getStore().id), 1));
  });
  assert.strictEqual(seen, 7);
});

test('events, readline, string_decoder, vm', async () => {
  const { once, EventEmitter } = require('events');
  const e = new EventEmitter();
  setImmediate(() => e.emit('go', 1, 2));
  assert.deepStrictEqual(await once(e, 'go'), [1, 2]);
  const rl = require('readline').createInterface({ input: require('stream').Readable.from(['a\nb', '\nc\n']) });
  const lines = [];
  for await (const line of rl) lines.push(line);
  assert.deepStrictEqual(lines, ['a', 'b', 'c']);
  const { StringDecoder } = require('string_decoder');
  const sd = new StringDecoder('utf8');
  assert.strictEqual(sd.write(Buffer.from([0xe2, 0x82])) + sd.write(Buffer.from([0xac])), '€');
  assert.strictEqual(require('vm').runInNewContext('x * 2 + y', { x: 20, y: 2 }), 42);
});

test('Intl', () => {
  assert.strictEqual(new Intl.NumberFormat('en-US').format(1234567.891), '1,234,567.891');
  assert.strictEqual(new Intl.NumberFormat('de-DE', { style: 'currency', currency: 'EUR' }).format(12.5), '12,50 €');
  assert.strictEqual(new Intl.DateTimeFormat('en-US', { timeZone: 'UTC' }).format(new Date(Date.UTC(2026, 8, 30))), '9/30/2026');
  const graphemes = [...new Intl.Segmenter('en', { granularity: 'grapheme' }).segment('é👍🏽한')];
  assert.strictEqual(graphemes.length, 3);
  assert.strictEqual('ı'.toLocaleUpperCase('tr'), 'I');
  assert.deepStrictEqual(['b', 'a', 'C'].sort(new Intl.Collator('en').compare), ['a', 'b', 'C']);
  assert.strictEqual(new Intl.PluralRules('en').select(1), 'one');
  assert.strictEqual(new Intl.RelativeTimeFormat('en').format(-1, 'day'), '1 day ago');
  assert.strictEqual(Intl.DateTimeFormat().resolvedOptions().locale.length > 0, true);
});

test('worker_threads', async () => {
  const { Worker, MessageChannel } = require('worker_threads');
  const w = new Worker(`
    const { parentPort, workerData } = require('worker_threads');
    parentPort.on('message', (m) => parentPort.postMessage(m * workerData.factor));
  `, { eval: true, workerData: { factor: 3 } });
  const answer = new Promise((resolve) => w.once('message', resolve));
  w.postMessage(14);
  assert.strictEqual(await answer, 42);
  await w.terminate();
  const { port1, port2 } = new MessageChannel();
  const got = new Promise((resolve) => port2.once('message', resolve));
  port1.postMessage({ hello: 'port' });
  assert.deepStrictEqual(await got, { hello: 'port' });
  port1.close();
});

(async () => {
  let failed = 0;
  for (const { name, fn } of cases) {
    if (only.length && !only.some((o) => name.toLowerCase().includes(o.toLowerCase()))) continue;
    const started = Date.now();
    try {
      await Promise.race([fn(), new Promise((_, reject) => setTimeout(() => reject(new Error('timed out after 60 s')), 60000).unref())]);
      console.log(`PASS ${name} (${Date.now() - started} ms)`);
    } catch (err) {
      failed++;
      console.log(`FAIL ${name}\n     ${String(err && err.stack || err).split('\n').slice(0, 6).join('\n     ')}`);
    }
  }
  fs.rmSync(work, { recursive: true, force: true });
  process.exitCode = failed;
})();
