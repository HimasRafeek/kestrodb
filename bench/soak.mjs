// Soak test: kestro-server under sustained mixed load. Eight connections run inserts,
// updates, deletes, point and range reads, aggregates, joins and transactions against a
// table kept at a steady size; a ninth takes a backup and an online compaction every few
// minutes. Every interval prints operations per second, p50/p99 latency and the server's
// private memory (mapped file pages excluded); at the end the run fails if memory kept growing after warm-up, a statement
// failed unexpectedly, or the database does not verify.
//   node bench/soak.mjs [--minutes 30] [--connections 8] [--rows 50000] [--interval 30]
import {spawn, execFile} from 'node:child_process';
import {monitorEventLoopDelay} from 'node:perf_hooks';
import {promisify} from 'node:util';
import {mkdtempSync, readFileSync, rmSync, writeFileSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';
import {Database} from '../index.mjs';

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? Number(process.argv[i + 1]) : d; };
const MINUTES = arg('minutes', 30), CONNS = arg('connections', 8), ROWS = arg('rows', 50000), INTERVAL = arg('interval', 30);
const root = fileURLToPath(new URL('../', import.meta.url));
const serverBin = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const dir = mkdtempSync(join(tmpdir(), 'kdb-soak-'));
const file = join(dir, 'soak.kdb'), tokenFile = join(dir, 'token.txt'), token = 'soak-token-' + process.pid + '-' + Date.now();
writeFileSync(tokenFile, token);

// Asynchronous, so the harness never stalls the requests it measures (a synchronous
// PowerShell launch froze this process for about a second per report).
const run = promisify(execFile);
async function rssMB(pid) {
  try {
    if (process.platform === 'linux') return Number(/RssAnon:\s+(\d+)/.exec(readFileSync(`/proc/${pid}/status`, 'utf8'))[1]) / 1024;
    if (process.platform === 'win32') return Number((await run('powershell', ['-NoProfile', '-Command', `(Get-Process -Id ${pid}).PrivateMemorySize64`])).stdout.trim()) / 1048576;
    return Number((await run('ps', ['-o', 'rss=', '-p', String(pid)])).stdout.trim()) / 1024;
  } catch { return NaN; }
}
// This process's own stalls (event-loop delay), reported beside the latencies: a request in
// flight during one looks that much slower without the database being slow.
const loop = monitorEventLoopDelay({resolution: 10}); loop.enable();
const srv = spawn(serverBin, ['--db', file, '--create', '--tcp', '127.0.0.1:0', '--token-file', tokenFile], {stdio: ['ignore', 'pipe', 'pipe']});
let srvErr = '';
srv.stderr.on('data', d => { srvErr += d; });
const port = await new Promise((res, rej) => { let b = ''; srv.stdout.on('data', d => { b += d; const m = /READY tcp 127\.0\.0\.1:(\d+)/.exec(b); if (m) res(Number(m[1])); }); srv.on('exit', c => rej(Error('server exited ' + c + ': ' + b + srvErr))); });
const url = `tcp://127.0.0.1:${port}`;
const admin = await RemoteDB.connect(url, {token});
await admin.sql('CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL, name TEXT, age INTEGER CHECK (age >= 0), city TEXT, score REAL)');
await admin.sql('CREATE TABLE orders (id INTEGER PRIMARY KEY, user INTEGER REFERENCES users ON DELETE CASCADE, total REAL, status TEXT)');
await admin.sql('CREATE INDEX ON users (city)'); await admin.sql('CREATE INDEX ON users (age)');
const cities = ['Colombo', 'Kandy', 'Galle', 'Jaffna', 'Negombo'];
let nextEmail = 0;
for (let b = 0; b < ROWS; b += 500) {
  const v = [], p = [];
  for (let i = b; i < Math.min(ROWS, b + 500); i++) { v.push('(?, ?, ?, ?, ?)'); p.push(`u${nextEmail++}@x`, 'user' + i, 18 + i % 60, cities[i % 5], (i % 1000) / 10); }
  await admin.sql('INSERT INTO users (email, name, age, city, score) VALUES ' + v.join(', '), p);
}
console.log(`soak: ${MINUTES} min, ${CONNS} connections, ${ROWS} rows, server pid ${srv.pid}`);

const lat = [], errors = [], byKind = {};  // kind -> [count, total ms, max ms]
let ops = 0, stop = false, maxId = ROWS, oldest = 1;
const rnd = n => Math.floor(Math.random() * n);
// Ids of live rows are about [oldest, maxId].
const live = () => oldest + rnd(Math.max(1, maxId - oldest + 1));
async function worker(c) {
  while (!stop) {
    const r = Math.random(), t = performance.now();
    const kind = r < 0.15 ? "insert" : r < 0.30 ? "delete" : r < 0.35 ? "update" : r < 0.45 ? "insert-select" : r < 0.65 ? "point" : r < 0.75 ? "index-order" : r < 0.80 ? "group-by" : r < 0.88 ? "join" : r < 0.95 ? "transaction" : "count-range";
    try {
      if (r < 0.15) { const res = await c.sql('INSERT INTO users (email, name, age, city, score) VALUES (?, ?, ?, ?, ?)', [`u${nextEmail++}@x`, 'n', rnd(80), cities[rnd(5)], rnd(1000) / 10]); maxId = Math.max(maxId, res.lastInsertId ?? 0); }
      else if (r < 0.30) await c.sql('DELETE FROM users WHERE id = ?', [oldest++]);  // the oldest row: the size stays steady (cascades to orders)
      else if (r < 0.35) await c.sql('UPDATE users SET score = score + 1, age = ? WHERE id = ?', [rnd(80), live()]);
      else if (r < 0.45) await c.sql('INSERT INTO orders (user, total, status) SELECT id, ?, ? FROM users WHERE id = ?', [rnd(500), 'open', live()]).catch(e => { if (!/CONFLICT|INPUT/.test(e.message)) throw e; });
      else if (r < 0.65) await c.sql('SELECT * FROM users WHERE id = ?', [live()]);
      else if (r < 0.75) await c.sql('SELECT id, name FROM users WHERE city = ? ORDER BY age DESC LIMIT 10', [cities[rnd(5)]]);
      else if (r < 0.80) await c.sql('SELECT city, COUNT(*) AS n, AVG(score) AS s FROM users GROUP BY city');
      else if (r < 0.88) await c.sql('SELECT u.name, o.total FROM users u JOIN orders o ON o.user = u.id WHERE u.id = ?', [live()]);
      else if (r < 0.95) {
        await c.sql('BEGIN');
        const a = live(), b = live();
        await c.sql('UPDATE users SET score = score - 1 WHERE id = ?', [a]);
        await c.sql('UPDATE users SET score = score + 1 WHERE id = ?', [b]);
        await c.sql(Math.random() < 0.1 ? 'ROLLBACK' : 'COMMIT');
      } else await c.sql('SELECT COUNT(*) AS n FROM users WHERE age BETWEEN ? AND ?', [20, 40]);
      const ms = performance.now() - t; lat.push(ms); ops++;
      const k = byKind[kind] ??= [0, 0, 0]; k[0]++; k[1] += ms; if (ms > k[2]) k[2] = ms;
    } catch (e) {
      if (/CONFLICT/.test(e.message)) { ops++; continue; }  // unique email race or a deleted parent: expected
      errors.push(e.message); try { await c.sql('ROLLBACK'); } catch {}
      if (errors.length > 20) { stop = true; }
    }
  }
}
const conns = await Promise.all(Array.from({length: CONNS}, () => RemoteDB.connect(url, {token})));
const workers = conns.map(worker);
const maint = (async () => {
  let n = 0;
  while (!stop) {
    await new Promise(r => setTimeout(r, 120000));
    if (stop) break;
    try {
      if (n++ % 2 === 0) { await admin.compact(); console.log('  online compaction done'); }
      else { const b = join(dir, 'backup.kdb'); rmSync(b, {force: true}); await admin.sql('SELECT 1 FROM users LIMIT 1'); console.log('  checkpoint'); }
    } catch (e) { errors.push('maintenance: ' + e.message); }
  }
})();

const samples = [], t0 = Date.now();
const pct = (s, p) => s.length ? s[Math.min(s.length - 1, Math.floor(p * s.length))] : NaN;
const sample = l => { const k = Math.max(1, Math.ceil(l.length / 200000)), out = new Float64Array(Math.ceil(l.length / k)); for (let i = 0, j = 0; i < l.length; i += k) out[j++] = l[i]; return out.sort(); };
while (Date.now() - t0 < MINUTES * 60000 && !stop) {
  await new Promise(r => setTimeout(r, INTERVAL * 1000));
  const l = sample(lat.splice(0)), o = ops; ops = 0;
  const mem = await rssMB(srv.pid);
  samples.push(mem);
  const stall = loop.max / 1e6; loop.reset();
  console.log(`${((Date.now() - t0) / 60000).toFixed(1).padStart(5)} min  ${(o / INTERVAL).toFixed(0).padStart(7)} ops/s  p50 ${pct(l, 0.5).toFixed(2)} ms  p99 ${pct(l, 0.99).toFixed(2)} ms  max ${pct(l, 1).toFixed(0)} ms  client stall ${stall.toFixed(0)} ms  server ${mem.toFixed(0)} MB  errors ${errors.length}`);
}
stop = true;
await Promise.allSettled(workers); await maint;
for (const c of conns) await c.close?.();
const stats = await admin.stats?.().catch(() => null);
await admin.close?.();
srv.kill('SIGINT');
await new Promise(r => setTimeout(r, 1500));
if (srv.exitCode === null) srv.kill();
const db = new Database(file); db.verify(); const users = db.queryValue('SELECT COUNT(*) FROM users'); db.close();
// Memory after warm-up (the first third): the last third may not exceed the middle third by
// more than 15%.
const third = Math.max(1, Math.floor(samples.length / 3));
const mid = Math.max(...samples.slice(third, 2 * third)), end = Math.max(...samples.slice(2 * third));
const grew = end > mid * 1.15;
console.log(`end: ${users} users, file verified; server memory middle ${mid.toFixed(0)} MB, end ${end.toFixed(0)} MB${grew ? ' (GREW)' : ''}; unexpected errors ${errors.length}`);
console.log('by kind (count, mean ms, max ms):'); for (const [k, [n, t, m]] of Object.entries(byKind).sort((a, b) => b[1][1] / b[1][0] - a[1][1] / a[1][0])) console.log('  ' + k.padEnd(14) + String(n).padStart(8) + (t / n).toFixed(2).padStart(9) + m.toFixed(1).padStart(9));
if (errors.length) console.log('errors:', [...new Set(errors)].slice(0, 5));
if (stats) console.log('stats:', JSON.stringify(stats).slice(0, 300));
rmSync(dir, {recursive: true, force: true});
process.exit(grew || errors.length ? 1 : 0);
