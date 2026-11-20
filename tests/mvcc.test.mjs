// Snapshot reads (MVCC): a long query lets writes through and still reads one state.
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

test('writes are not held up by a long query, which still sees one snapshot', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-mvcc-'));
  const child = spawn(serverBinary, ['--db', join(dir, 'm.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', '--statement-timeout', '120s'], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  const exited = new Promise(r => child.on('exit', r));
  try {
    const address = await new Promise((resolve, reject) => {
      let out = ''; child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
      child.on('exit', code => reject(Error('server exited ' + code)));
    });
    const reader = await RemoteDB.connect(address), writer = await RemoteDB.connect(address);
    await writer.sql('CREATE TABLE acct (id INTEGER PRIMARY KEY, bal INTEGER, pad TEXT)');
    const N = 150000, pad = 'x'.repeat(200);
    for (let b = 0; b < N / 1000; b++) {
      const v = []; for (let i = 0; i < 1000; i++) v.push(`(100, '${pad}')`);
      await writer.sql('INSERT INTO acct (bal, pad) VALUES ' + v.join(', '));
    }
    // A long query: every row is read and parsed (a key condition keeps it off the column cache).
    const long = 'SELECT COUNT(*) AS n, SUM(bal) AS s, MAX(LENGTH(pad)) AS p FROM acct WHERE id >= 0';
    let t0 = Date.now(); await reader.sql(long); const alone = Date.now() - t0;
    // The same query again, with transfers (SUM unchanged) committed meanwhile.
    const lat = []; let running = true;
    const q = (async () => { t0 = Date.now(); const r = await reader.sql(long); running = false; return {r, ms: Date.now() - t0}; })();
    await new Promise(r => setTimeout(r, Math.min(50, alone / 4)));
    while (running) {
      const a = 1 + Math.floor(Math.random() * N), b = 1 + Math.floor(Math.random() * N), s = Date.now();
      await writer.sql('BEGIN'); await writer.sql('UPDATE acct SET bal = bal - 5 WHERE id = ?', [a]); await writer.sql('UPDATE acct SET bal = bal + 5 WHERE id = ?', [b]); await writer.sql('COMMIT');
      lat.push(Date.now() - s);
    }
    const {r, ms} = await q;
    assert.equal(r.rows[0].n, N); assert.equal(r.rows[0].s, 100 * N, 'one snapshot: transfers never show half done');
    const worst = Math.max(...lat), p90 = [...lat].sort((x, y) => x - y)[Math.floor(lat.length * 0.9)], stats = await writer.stats();
    console.log(`  query ${alone} ms alone, ${ms} ms with writes; ${lat.length} transactions meanwhile, p90 ${p90} ms, slowest ${worst} ms; ${stats.snapshotYields} yields`);
    // Writes held up by the query would complete only before and after it, each taking about
    // as long as the query; a single disk flush on a busy machine can take tens of ms anyway.
    assert.ok(lat.length >= 5, `writes went on during the query (${lat.length})`);
    assert.ok(p90 < Math.max(50, ms / 3), `writes waited behind a ${ms} ms query (p90 ${p90} ms)`);
    assert.ok(stats.snapshotYields > 0);
    assert.equal(stats.snapshotMemory, 0, 'the undo records went when the query ended');
    await reader.close(); await writer.close();
  } finally { child.stdin.end(); await exited; await rm(dir, {recursive: true, force: true}); }
});

async function withServer(fn) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-mvcc-'));
  const child = spawn(serverBinary, ['--db', join(dir, 'c.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof'], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  const exited = new Promise(r => child.on('exit', r));
  try {
    const address = await new Promise((resolve, reject) => {
      let out = ''; child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
      child.on('exit', code => reject(Error('server exited ' + code)));
    });
    const open = [], conn = async () => { const c = await RemoteDB.connect(address); open.push(c); return c; };
    await fn(conn);
    for (const c of open) await c.close().catch(() => {});
  } finally { child.stdin.end(); await exited; await rm(dir, {recursive: true, force: true}); }
}
const n = async (db, sql = 'SELECT COUNT(*) AS n FROM t') => (await db.sql(sql)).rows[0].n;

test('BEGIN CONCURRENT: inserts side by side, first committer wins on the same row', async () => {
  await withServer(async conn => {
    const a = await conn(), b = await conn(), c = await conn();
    await c.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)');
    await c.sql('INSERT INTO t (v) VALUES (1), (2), (3)');
    // Both insert into the same table: distinct ids, both commit.
    await a.sql('BEGIN CONCURRENT'); await b.sql('BEGIN CONCURRENT');
    const ia = (await a.sql('INSERT INTO t (v) VALUES (10)')).lastInsertId, ib = (await b.sql('INSERT INTO t (v) VALUES (20)')).lastInsertId;
    assert.notEqual(ia, ib);
    // An autocommit INSERT meanwhile takes neither id, and does not wait for them.
    const t0 = Date.now(); const ic = (await c.sql('INSERT INTO t (v) VALUES (30)')).lastInsertId; assert.ok(Date.now() - t0 < 1000);
    assert.ok(ic !== ia && ic !== ib);
    await a.sql('COMMIT'); await b.sql('COMMIT');
    assert.equal(await n(c), 6);
    // Both change row 1: the second COMMIT fails and changes nothing.
    await a.sql('BEGIN CONCURRENT'); await b.sql('BEGIN CONCURRENT');
    await a.sql('UPDATE t SET v = v + 100 WHERE id = 1'); await b.sql('UPDATE t SET v = v + 1000 WHERE id = 1');
    await b.sql('UPDATE t SET v = 7 WHERE id = 2');
    await a.sql('COMMIT');
    await assert.rejects(b.sql('COMMIT'), /CONFLICT another transaction changed a row of t/);
    assert.equal(b.inTransaction, false);
    assert.equal(await n(c, 'SELECT v AS n FROM t WHERE id = 1'), 101);
    assert.equal(await n(c, 'SELECT v AS n FROM t WHERE id = 2'), 2, 'the failed transaction left nothing');
  });
});

test('BEGIN CONCURRENT: reads see the transaction\'s snapshot and its own writes', async () => {
  await withServer(async conn => {
    const a = await conn(), c = await conn();
    await c.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)');
    await c.sql('CREATE INDEX t_v ON t (v)');
    await c.sql('INSERT INTO t (v) VALUES (1), (2), (3)');
    await a.sql('BEGIN CONCURRENT ISOLATION LEVEL SNAPSHOT');  // reads not checked at COMMIT (serializable.test.mjs)
    assert.equal(await n(a), 3);
    await c.sql('INSERT INTO t (v) VALUES (4)'); await c.sql('UPDATE t SET v = 99 WHERE id = 1'); await c.sql('DELETE FROM t WHERE id = 2');
    assert.equal(await n(a), 3, 'committed rows after BEGIN are not seen');
    assert.equal(await n(a, 'SELECT SUM(v) AS n FROM t'), 6);
    assert.equal(await n(a, 'SELECT COUNT(*) AS n FROM t WHERE v >= 1'), 3, 'through the index too');
    assert.equal(await n(a, 'SELECT v AS n FROM t WHERE id = 1'), 1);
    await a.sql('INSERT INTO t (v) VALUES (50)');
    assert.equal(await n(a), 4, 'its own row');
    await a.sql('COMMIT');  // it wrote no row the others changed
    assert.equal(await n(c), 4);
    assert.equal(await n(c, 'SELECT SUM(v) AS n FROM t'), 99 + 3 + 4 + 50);
    // Serializable (the default): rows it read changed meanwhile, so its COMMIT fails.
    await a.sql('BEGIN CONCURRENT'); assert.equal(await n(a), 4);
    await c.sql('UPDATE t SET v = 98 WHERE id = 1'); await a.sql('INSERT INTO t (v) VALUES (60)');
    await assert.rejects(a.sql('COMMIT'), /CONFLICT another transaction changed a row of t that this one read \(id 1\)/);
  });
});

test('BEGIN CONCURRENT: constraints are checked again at COMMIT', async () => {
  await withServer(async conn => {
    const a = await conn(), c = await conn();
    await c.sql('CREATE TABLE p (id INTEGER PRIMARY KEY, name TEXT)');
    await c.sql('CREATE TABLE ch (id INTEGER PRIMARY KEY, pid INTEGER REFERENCES p (id), note TEXT)');
    await c.sql("INSERT INTO p (id, name) VALUES (1, 'one'), (2, 'two')");
    await a.sql('BEGIN CONCURRENT');
    await a.sql("INSERT INTO ch (pid, note) VALUES (1, 'child of 1')");  // parent 1 exists now
    await c.sql('DELETE FROM p WHERE id = 1');                             // no committed child: allowed
    await assert.rejects(a.sql('COMMIT'), /CONFLICT/);
    assert.equal(await n(c, 'SELECT COUNT(*) AS n FROM ch'), 0, 'no orphan');
  });
});
