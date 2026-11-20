// Stress test for kestro-server: many concurrent connections, mixed reads and durable writes.
//
//   node bench/stress.mjs [--connections 10,100,1000,2000] [--seconds 10] [--keys 100000]
//        [--writes 0.1] [--memory-keys 1000000] [--clients 4]
//
// Each connection behaves like one PHP/Python worker: one request at a time, waiting for
// the reply (no pipelining), --writes of them durable single-key writes, the rest point
// reads of random existing keys. --clients Node processes share the connections so the
// load generator is not the bottleneck. Reports throughput, latency percentiles, errors,
// and the server's memory (private/anonymous bytes and resident set).
// --memory-keys loads that many keys into a fresh server and reports memory per key.
import {spawn, execFileSync} from 'node:child_process';
import {mkdtempSync, rmSync, readFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {fork} from 'node:child_process';

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? process.argv[i + 1] : d; };
const root = fileURLToPath(new URL('../', import.meta.url));
const server = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const doc = i => ({id: i, name: `user-${i}`, email: `user${i}@example.com`, tags: ['a', 'b', 'c'], score: i * 7 % 1000, bio: 'x'.repeat(120)});

// ---------- Load-generator child: --child ADDRESS CONNECTIONS SECONDS KEYS WRITES ----------
if (process.argv[2] === '--child') {
  const {RemoteDB} = await import('../remote.mjs');
  const [address, conns, seconds, keys, writes] = process.argv.slice(3);
  const n = Number(conns), until = Date.now() + Number(seconds) * 1000, K = Number(keys), W = Number(writes);
  const clients = [];
  for (let i = 0; i < n; i += 50) clients.push(...await Promise.all(Array.from({length: Math.min(50, n - i)}, () => RemoteDB.connect(address, {jsonValidation: 'basic', timeout: 30000}))));
  process.send({ready: true});
  await new Promise(r => process.once('message', r));  // start together
  const lat = [], errors = {};
  let ops = 0, w = 0;
  await Promise.all(clients.map(async (db, c) => {
    let seed = c * 2654435761 >>> 0;
    while (Date.now() < until) {
      seed = (seed * 1103515245 + 12345) >>> 0;
      const k = seed % K, t = performance.now();
      try {
        if ((seed >>> 8) % 1000 < W * 1000) { await db.set('c', `k${k}`, doc(k)); w++; }
        else if ((await db.get('c', `k${k}`)).value.id !== k) throw Error('WRONG value');
        lat.push(performance.now() - t); ops++;
      } catch (e) { const kind = String(e.message).split(' ')[0]; errors[kind] = (errors[kind] || 0) + 1; }
    }
  }));
  await Promise.all(clients.map(db => db.close().catch(() => {})));
  // Send a sample of latencies (sorted percentiles are computed by the parent).
  lat.sort((a, b) => a - b);
  const pick = p => lat.length ? lat[Math.min(lat.length - 1, Math.floor(p * lat.length))] : 0;
  process.send({ops, writes: w, errors, p50: pick(0.5), p99: pick(0.99), p999: pick(0.999), max: lat[lat.length - 1] ?? 0, count: lat.length});
  process.exit(0);
}

function memoryOf(pid) {
  if (process.platform === 'linux') {
    const s = readFileSync(`/proc/${pid}/status`, 'utf8'), kb = k => Number((new RegExp(k + ':\\s+(\\d+)').exec(s) || [])[1] || 0) * 1024;
    return {private: kb('RssAnon'), resident: kb('VmRSS'), threads: kb('Threads') / 1024};
  }
  if (process.platform === 'win32') {
    const out = execFileSync('powershell', ['-NoProfile', '-Command', `$p=Get-Process -Id ${pid}; "$($p.PrivateMemorySize64) $($p.WorkingSet64) $($p.Threads.Count)"`], {encoding: 'utf8'}).trim().split(' ').map(Number);
    return {private: out[0], resident: out[1], threads: out[2]};
  }
  return {private: 0, resident: 0, threads: 0};
}
async function startServer(dir, maxConnections) {
  const child = spawn(server, ['--db', join(dir, 's.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', '--max-connections', String(maxConnections)], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '';
  const address = await new Promise((resolve, reject) => { child.stdout.on('data', d => { out += d; const m = /READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); }); child.on('exit', c => reject(Error('server exited ' + c))); });
  return {address, pid: child.pid, stop: () => new Promise(r => { child.on('exit', r); child.stdin.end(); })};
}
async function seed(address, keys) {
  const {RemoteDB} = await import('../remote.mjs');
  const db = await RemoteDB.connect(address, {jsonValidation: 'basic'});
  for (let i = 0; i < keys; i += 1000) await db.batch(Array.from({length: Math.min(1000, keys - i)}, (_, j) => ({type: 'set', collection: 'c', key: `k${i + j}`, value: doc(i + j)})));
  await db.close();
}
const mb = b => (b / 1048576).toFixed(0) + ' MB';

// ---------- Memory per key ----------
const memKeys = Number(arg('memory-keys', 0));
if (memKeys) {
  const dir = mkdtempSync(join(tmpdir(), 'kestro-stress-'));
  try {
    const srv = await startServer(dir, 16);
    const before = memoryOf(srv.pid);
    const t = performance.now(); await seed(srv.address, memKeys); const secs = (performance.now() - t) / 1000;
    const after = memoryOf(srv.pid);
    await new Promise(r => setTimeout(r, 8000));  // after the idle index snapshot
    const settled = memoryOf(srv.pid);
    console.log(`${memKeys.toLocaleString('en-US')} keys loaded in ${secs.toFixed(1)} s | server private memory ${mb(before.private)} -> ${mb(after.private)} (${((after.private - before.private) / memKeys).toFixed(0)} bytes/key) -> settled ${mb(settled.private)} (${((settled.private - before.private) / memKeys).toFixed(0)} bytes/key) | resident ${mb(after.resident)} (includes cached file pages)`);
    await srv.stop();
  } finally { rmSync(dir, {recursive: true, force: true}); }
  process.exit(0);
}

// ---------- Concurrency sweep ----------
const LEVELS = arg('connections', '10,100,1000,2000').split(',').map(Number);
const SECONDS = Number(arg('seconds', 10)), KEYS = Number(arg('keys', 100000)), WRITES = Number(arg('writes', 0.1)), CLIENTS = Number(arg('clients', 4));
const dir = mkdtempSync(join(tmpdir(), 'kestro-stress-'));
try {
  const srv = await startServer(dir, Math.max(...LEVELS) + 16);
  await seed(srv.address, KEYS);
  const idle = memoryOf(srv.pid);
  console.log(`server seeded with ${KEYS.toLocaleString('en-US')} keys: private ${mb(idle.private)}, resident ${mb(idle.resident)}, ${idle.threads} threads`);
  for (const level of LEVELS) {
    const per = Array.from({length: CLIENTS}, (_, i) => Math.floor(level / CLIENTS) + (i < level % CLIENTS ? 1 : 0)).filter(Boolean);
    const kids = per.map(n => fork(fileURLToPath(import.meta.url), ['--child', srv.address, String(n), String(SECONDS + 2), String(KEYS), String(WRITES)], {stdio: ['ignore', 'inherit', 'inherit', 'ipc']}));
    const died = k => new Promise((_, reject) => k.once('exit', code => reject(Error('load generator exited with ' + code))));
    await Promise.race([Promise.all(kids.map(k => new Promise(r => k.once('message', r)))), ...kids.map(died)]);
    const results = kids.map(k => Promise.race([new Promise(r => k.once('message', r)), died(k)]));
    kids.forEach(k => k.send('go'));
    await new Promise(r => setTimeout(r, SECONDS * 500));
    const peak = memoryOf(srv.pid);
    const rs = await Promise.all(results);
    const ops = rs.reduce((s, r) => s + r.ops, 0), writes = rs.reduce((s, r) => s + r.writes, 0);
    const errors = {}; for (const r of rs) for (const [k, v] of Object.entries(r.errors)) errors[k] = (errors[k] || 0) + v;
    const worst = f => Math.max(...rs.map(r => r[f]));
    console.log(`${String(level).padStart(5)} connections | ${Math.round(ops / (SECONDS + 2)).toLocaleString('en-US').padStart(9)} ops/s (${Math.round(writes / (SECONDS + 2)).toLocaleString('en-US')} durable writes/s) | latency p50 ${rs[0].p50.toFixed(2)} ms, p99 ${worst('p99').toFixed(1)} ms, p99.9 ${worst('p999').toFixed(1)} ms, max ${worst('max').toFixed(0)} ms | errors ${JSON.stringify(errors)} | server private ${mb(peak.private)}, resident ${mb(peak.resident)}, ${peak.threads} threads`);
  }
  await srv.stop();
} finally { rmSync(dir, {recursive: true, force: true}); }
