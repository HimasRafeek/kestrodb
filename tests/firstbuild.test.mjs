// First builds (ordered keys, secondary indexes, the column cache) run without the writer
// lock: commits go on while the first query after a restart builds them, and the built
// structures include the rows those commits changed.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));

async function start(file, args = []) {
  const child = spawn(serverBinary, ['--db', file, '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', '--statement-timeout', '120s', ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  const exited = new Promise(r => child.on('exit', r));
  const address = await new Promise((resolve, reject) => {
    let out = ''; child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error('server exited ' + code)));
  });
  return {address, stop: async () => { child.stdin.end(); await exited; }};
}
const pct = (a, p) => { const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(s.length * p))]; };

// Table t (N rows, an index on name), loaded and closed: the next server builds everything anew.
async function load(dir, N) {
  const file = join(dir, 'f.kdb'), s = await start(file), db = await RemoteDB.connect(s.address);
  await db.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, g INTEGER, v INTEGER, name TEXT, pad TEXT)');
  await db.sql('CREATE INDEX t_name ON t (name)');
  const pad = 'p'.repeat(120);
  for (let b = 0; b < N / 1000; b++) {
    const v = []; for (let i = 1; i <= 1000; i++) { const id = b * 1000 + i; v.push(`(${id % 10}, ${id}, 'n${id}', '${pad}')`); }
    await db.sql('INSERT INTO t (g, v, name, pad) VALUES ' + v.join(', '));
  }
  await db.close(); await s.stop();
  const model = new Map(); for (let id = 1; id <= N; id++) model.set(id, {g: id % 10, v: id, name: 'n' + id});
  return {file, model};
}

// Every query path agrees with the model: the column cache (GROUP BY), the index on name,
// the ordered keys.
async function check(db, model) {
  const groups = new Map(); for (const r of model.values()) { const x = groups.get(r.g) || {n: 0, s: 0}; x.n++; x.s += r.v; groups.set(r.g, x); }
  const agg = (await db.sql('SELECT g, COUNT(*) AS n, SUM(v) AS s FROM t GROUP BY g ORDER BY g')).rows;
  assert.deepEqual(agg, [...groups.keys()].sort((a, b) => a - b).map(g => ({g, n: groups.get(g).n, s: groups.get(g).s})), 'column cache');
  const big = [...model.values()].filter(r => r.name >= 'n5').length;
  assert.equal((await db.sql("SELECT COUNT(*) AS n FROM t WHERE name >= 'n5'")).rows[0].n, big, 'index range');
  for (const [id, r] of [...model].filter(([, r]) => r.name.startsWith('u')).slice(0, 50))
    assert.deepEqual((await db.sql('SELECT id, v FROM t WHERE name = ?', [r.name])).rows, [{id, v: r.v}], 'index: changed row');
  assert.equal((await db.sql('SELECT COUNT(*) AS n FROM t')).rows[0].n, model.size, 'row count');
  const ids = [...model.keys()].sort((a, b) => b - a).slice(0, 3);
  assert.deepEqual((await db.sql('SELECT id FROM t ORDER BY id DESC LIMIT 3')).rows.map(r => r.id), ids, 'ordered keys');
  const idx = (await db.sql('SHOW INDEX FROM t')).rows.find(r => r.name === 't_name');
  assert.equal(idx.entries, model.size, 'index entries');
}

test('a first aggregate builds without holding up commits; rows changed meanwhile are in it', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-firstbuild-'));
  try {
    const N = 120000, {file, model} = await load(dir, N);
    const s = await start(file);
    const reader = await RemoteDB.connect(s.address), kv = await RemoteDB.connect(s.address), sq = await RemoteDB.connect(s.address);
    try {
      await kv.set('kv', 'warm', {i: 0});  // opening work (the key index) done before the measurement
      // The first aggregate builds the ordered keys, the index on name and the column cache.
      let running = true; const t0 = Date.now();
      const q = reader.sql('SELECT g, COUNT(*) AS n, SUM(v) AS s FROM t GROUP BY g').then(r => { running = false; return {r, ms: Date.now() - t0}; });
      await new Promise(r => setTimeout(r, 10));
      // Durable key/value commits, and SQL writes to t itself (inserts, updates, deletes).
      const kvLat = [], sqlLat = []; let kvN = 0, deleted = 0, inserted = 0;
      const kvLoop = (async () => { while (running) { const a = Date.now(); await kv.set('kv', 'k' + kvN, {i: kvN}); kvN++; kvLat.push(Date.now() - a); } })();
      const sqlLoop = (async () => {
        for (let k = 0; running; k++) {
          const a = Date.now(), id = 1 + Math.floor(Math.random() * N);
          if (k % 3 === 0) { const r = {g: 10 + k % 3, v: 1000000 + k, name: 'u' + k}; await sq.sql('INSERT INTO t (g, v, name) VALUES (?, ?, ?)', [r.g, r.v, r.name]); model.set(sq.lastInsertId, r); inserted++; }
          else if (k % 3 === 1 && model.has(id)) { const r = {g: 20 + k % 2, v: 2000000 + k, name: 'u' + k}; await sq.sql('UPDATE t SET g = ?, v = ?, name = ? WHERE id = ?', [r.g, r.v, r.name, id]); model.set(id, r); }
          else if (model.has(id)) { await sq.sql('DELETE FROM t WHERE id = ?', [id]); model.delete(id); deleted++; }
          sqlLat.push(Date.now() - a);
        }
      })();
      const {r, ms} = await q; await kvLoop; await sqlLoop;
      const n = r.rows.reduce((x, row) => x + row.n, 0);
      assert.ok(n >= N - deleted && n <= N + inserted, `the first result counts one state (${n})`);
      console.log(`  first aggregate ${ms} ms; meanwhile ${kvLat.length} commits (p90 ${pct(kvLat, 0.9)} ms, slowest ${Math.max(...kvLat)} ms) and ${sqlLat.length} SQL writes to t (p90 ${pct(sqlLat, 0.9)} ms, slowest ${Math.max(...sqlLat)} ms)`);
      // Held up by the build, the commits would finish only after it, each taking as long.
      assert.ok(kvLat.length >= 10, `commits went on during the build (${kvLat.length})`);
      assert.ok(pct(kvLat, 0.9) < Math.max(25, ms / 4), `commits waited behind a ${ms} ms build (p90 ${pct(kvLat, 0.9)} ms)`);
      assert.ok(Math.max(...kvLat) < Math.max(100, ms / 2), `a commit waited behind a ${ms} ms build (${Math.max(...kvLat)} ms)`);
      // A SQL write needs the ordered keys (built first, on their own), not the index or the cache.
      assert.ok(sqlLat.length >= 5 && pct(sqlLat, 0.9) < Math.max(50, ms / 3), `SQL writes waited behind a ${ms} ms build (p90 ${pct(sqlLat, 0.9)} ms)`);
      await check(reader, model);
      assert.deepEqual((await reader.get('kv', 'k' + (kvN - 1))).value, {i: kvN - 1});
      // More writes after the build: the published structures are kept current.
      for (let k = 0; k < 30; k++) { const id = [...model.keys()][k * 7]; const r = {g: 30, v: 3000000 + k, name: 'u-after' + k}; await sq.sql('UPDATE t SET g = ?, v = ?, name = ? WHERE id = ?', [r.g, r.v, r.name, id]); model.set(id, r); }
      await check(reader, model);
      assert.equal((await reader.stats()).snapshotMemory, 0, 'the build\'s undo records went with it');
    } finally { await reader.close(); await kv.close(); await sq.close(); await s.stop(); }
    // The same answers after a restart (built again from the file).
    const s2 = await start(file), db = await RemoteDB.connect(s2.address);
    try { await check(db, model); } finally { await db.close(); await s2.stop(); }
  } finally { await rm(dir, {recursive: true, force: true}); }
});

// With 1 MB for sets, the builds sort through a temporary file and page sets out.
test('first queries at the same time share the builds and agree (small set memory)', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-firstbuild-'));
  try {
    const N = 30000, {file, model} = await load(dir, N);
    const s = await start(file, ['--ordered-memory', '1']), conns = [];
    try {
      for (let i = 0; i < 6; i++) conns.push(await RemoteDB.connect(s.address));
      const [a, b, c, d, e, w] = conns;
      let running = true, k = 0;
      const writes = (async () => { while (running) { const id = 1 + (k * 7919) % N, r = {g: 40, v: 4000000 + k, name: 'u' + k}; await w.sql('UPDATE t SET g = ?, v = ?, name = ? WHERE id = ?', [r.g, r.v, r.name, id]); model.set(id, r); k++; } })();
      const results = await Promise.all([
        a.sql('SELECT g, COUNT(*) AS n FROM t GROUP BY g'), b.sql('SELECT g, SUM(v) AS s FROM t GROUP BY g'),
        c.sql("SELECT COUNT(*) AS n FROM t WHERE name >= 'n5'"), d.sql('SELECT id FROM t ORDER BY id LIMIT 3'), e.sql('SHOW TABLES')]);
      running = false; await writes;
      assert.equal(results[0].rows.reduce((x, r) => x + r.n, 0), N);
      assert.deepEqual(results[3].rows.map(r => r.id), [1, 2, 3]);
      assert.deepEqual(results[4].rows.map(r => [r.table, r.rows, r.indexes]), [['t', N, 1]]);
      await check(a, model);
      assert.ok((await a.stats()).orderedFileBytes > 0, 'sets paged out');
    } finally { for (const x of conns) await x.close().catch(() => {}); await s.stop(); }
  } finally { await rm(dir, {recursive: true, force: true}); }
});
