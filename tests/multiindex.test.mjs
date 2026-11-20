// Composite (multi-column) indexes: CREATE [UNIQUE] INDEX ON t (a, b, ...). Query results are
// compared with SQLite (node:sqlite) with the same indexes; composite UNIQUE is checked in
// and out of transactions (BEGIN and BEGIN CONCURRENT); the index survives reopen, DDL and
// replication; and a large table shows the index is used.
import test from 'node:test';
import assert from 'node:assert/strict';
import {DatabaseSync} from 'node:sqlite';
import {spawn} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Database} from '../index.mjs';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const sleep = ms => new Promise(r => setTimeout(r, ms));

const normalize = (rows, ordered) => {
  const vals = rows.map(r => JSON.stringify(Object.values(r).map(v => typeof v === 'number' ? Math.round(v * 1e9) / 1e9 : v)));
  return ordered ? vals : vals.sort();
};
async function tempDir(fn) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-multi-'));
  try { return await fn(dir); } finally { await rm(dir, {recursive: true, force: true}); }
}

// Rows: a and b small integers (some NULL, some left out), c a number, s a string.
function rows() {
  const out = [];
  for (let i = 1; i <= 1500; i++) {
    const a = i % 23 === 0 ? null : i % 11, b = i % 17 === 0 ? null : (i * 7) % 13, c = (i * 31) % 97, s = 'k' + ((i * 3) % 40);
    if (i % 29 === 0) out.push(['INSERT INTO t (id, c, s) VALUES (?, ?, ?)', [i, c, s]]);  // a and b missing
    else if (i % 31 === 0) out.push(['INSERT INTO t (id, a, c) VALUES (?, ?, ?)', [i, a, c]]);  // b and s missing
    else out.push(['INSERT INTO t (id, a, b, c, s) VALUES (?, ?, ?, ?, ?)', [i, a, b, c, s]]);
  }
  return out;
}
const QUERIES = [
  ['SELECT * FROM t WHERE a = ? AND b = ?', [3, 5]],
  ['SELECT * FROM t WHERE b = ? AND a = ?', [5, 3]],
  ['SELECT id FROM t WHERE a = ?', [4]],
  ['SELECT id, b FROM t WHERE a = ? AND b > ? AND b <= ?', [2, 3, 9]],
  ['SELECT id FROM t WHERE a = ? AND b BETWEEN ? AND ?', [7, 0, 4]],
  ['SELECT id FROM t WHERE a = ? AND b < ?', [1, 6]],
  ['SELECT id FROM t WHERE a = ? AND b IN (?, ?, ?)', [5, 1, 8, 12]],
  ['SELECT id FROM t WHERE a = ? AND b IN (?, NULL)', [5, 1]],
  ['SELECT id FROM t WHERE a IN (?, ?) AND b = ?', [2, 9, 4]],
  ['SELECT id FROM t WHERE a IN (1, 2, 3) AND b IN (4, 5)', []],
  ['SELECT id FROM t WHERE a = ? AND b = ? AND c > ?', [6, 2, 40]],
  ['SELECT id FROM t WHERE a = ? AND b IS NULL', [3]],
  ['SELECT id FROM t WHERE a IS NULL AND b = ?', [4]],
  ['SELECT id FROM t WHERE a IS NULL AND b IS NULL', []],
  ['SELECT id FROM t WHERE b = ?', [7]],
  ['SELECT id FROM t WHERE a = ? AND (b = ? OR b = ?)', [8, 1, 2]],
  ['SELECT id FROM t WHERE a = ? AND b = ? AND id > ?', [0, 0, 500]],
  ['SELECT id FROM t WHERE a = ? AND s LIKE ?', [3, 'k1%']],
  ['SELECT id FROM t WHERE a = ? AND s = ?', [3, 'k12']],
  ['SELECT id FROM t WHERE a > ? AND a < ? AND b = ?', [2, 6, 3]],
  ['SELECT COUNT(*) AS n FROM t WHERE a = ? AND b = ?', [3, 5]],
  ['SELECT COUNT(*) AS n FROM t WHERE a = ? AND b >= ?', [3, 5]],
  ['SELECT a, b, COUNT(*) AS n, SUM(c) AS sc FROM t WHERE a = ? AND b IS NOT NULL GROUP BY a, b ORDER BY b', [4]],
  ['SELECT id, b FROM t WHERE a = ? ORDER BY b, id LIMIT 5', [4]],
  ['SELECT id, b, c FROM t WHERE a = ? ORDER BY b DESC, c DESC, id DESC LIMIT 7', [2]],
  ['SELECT id, c FROM t WHERE a = ? AND b = ? ORDER BY c, id LIMIT 3', [6, 2]],
  ['SELECT id, a, b FROM t ORDER BY a, b, id LIMIT 20', []],
  ['SELECT id, a, b FROM t ORDER BY a DESC, b DESC, id DESC LIMIT 20', []],
  ['SELECT id, a, b FROM t ORDER BY a, b, id LIMIT 10 OFFSET 30', []],
  ['SELECT id, a, b, c FROM t ORDER BY a, b, c, id', []],
  ['SELECT id, a, b FROM t WHERE a > ? ORDER BY a, b, id LIMIT 12', [8]],
  ['SELECT id, a, b FROM t WHERE a IN (?, ?) ORDER BY a, b, id LIMIT 12', [9, 1]],
  ['SELECT id, a, b FROM t WHERE b = ? ORDER BY a, id LIMIT 12', [3]],
  ['SELECT id, a, b FROM t WHERE a = ? AND b > ? ORDER BY b, id LIMIT 4', [5, 6]],
  ['SELECT id FROM t WHERE a = ? ORDER BY c DESC, id LIMIT 5', [5]],
  ['SELECT id, s FROM t WHERE s IS NOT NULL ORDER BY s, a, id LIMIT 9', []],
];
const INDEXES = ['CREATE INDEX t_abc ON t (a, b, c)', 'CREATE INDEX t_as ON t (a, s)'];

function compareAll(h, s, label) {
  for (const [q, p] of QUERIES) {
    const want = s.prepare(q).all(...p), got = h.query(q, p);
    const ordered = /ORDER BY/i.test(q);
    assert.deepEqual(normalize(got, ordered), normalize(want, ordered), `${label}: ${q} ${JSON.stringify(p)}`);
  }
}

test('composite index queries match SQLite (prefix, range, IN, ORDER BY with LIMIT, NULLs)', () => tempDir(async dir => {
  const path = join(dir, 'm.kdb');
  let h = new Database(path, {create: true});
  const s = new DatabaseSync(':memory:');
  const schema = 'CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, c INTEGER, s TEXT)';
  h.sql(schema); s.exec(schema);
  for (const q of INDEXES) { h.sql(q); s.exec(q); }  // built as rows arrive
  for (const [q, p] of rows()) { h.sql(q, p); s.prepare(q).run(...p); }
  compareAll(h, s, 'index before rows');
  // Changes through the index: UPDATE and DELETE with composite conditions.
  for (const [q, p] of [
    ['UPDATE t SET b = b + 1 WHERE a = ? AND b < ?', [3, 6]],
    ['UPDATE t SET a = NULL WHERE a = ? AND b = ?', [4, 2]],
    ['DELETE FROM t WHERE a = ? AND b IN (?, ?)', [6, 1, 3]],
    ['UPDATE t SET a = 3, b = 5 WHERE id IN (7, 8, 9)', []],
  ]) {
    assert.equal(h.sql(q, p).changes, s.prepare(q).run(...p).changes, q);
  }
  compareAll(h, s, 'after writes');
  // Reopen: the index comes back from the catalog.
  h.close(); h = new Database(path);
  const shown = h.query('SHOW INDEX FROM t');
  assert.deepEqual(shown.filter(r => r.name === 't_abc').map(r => [r.column, r.columns, r.unique]), [['a,b,c', ['a', 'b', 'c'], false]]);
  assert.deepEqual(shown.find(r => r.name === 't_as').columns, ['a', 's']);
  assert.equal(shown.find(r => r.name === 't_abc').entries, s.prepare('SELECT COUNT(*) AS n FROM t').get().n);
  compareAll(h, s, 'after reopen');
  // Built over existing rows (another order of columns), and dropped again.
  h.sql('DROP INDEX t_abc'); h.sql('CREATE INDEX t_ba ON t (b, a)'); s.exec('DROP INDEX t_abc'); s.exec('CREATE INDEX t_ba ON t (b, a)');
  assert.deepEqual(h.query('SHOW INDEX FROM t').map(r => r.name), ['PRIMARY', 't_as', 't_ba']);
  compareAll(h, s, 'index built over rows');
  assert.throws(() => h.sql('CREATE INDEX t_ba2 ON t (b, a)'), /already exists/);
  h.sql('CREATE INDEX IF NOT EXISTS t_ba2 ON t (b, a)');
  assert.throws(() => h.sql('CREATE INDEX x ON t (a, a)'), /twice/);
  assert.throws(() => h.sql('CREATE INDEX x ON t (a, b, c, s, d, e, f, g, h)'), /at most 8 columns/);
  h.sql('DROP INDEX t_ba'); h.sql('DROP INDEX t_as'); s.exec('DROP INDEX t_ba'); s.exec('DROP INDEX t_as');
  assert.deepEqual(h.query('SHOW INDEX FROM t').map(r => r.name), ['PRIMARY']);
  compareAll(h, s, 'without indexes');
  h.close(); h = new Database(path);
  assert.deepEqual(h.query('SHOW INDEX FROM t').map(r => r.name), ['PRIMARY']);
  h.close(); s.close();
}));

// Each statement must succeed or fail on both databases; the table must match afterwards.
test('composite UNIQUE: duplicates refused, NULL exempt, inside BEGIN, built over rows', () => tempDir(async dir => {
  const path = join(dir, 'u.kdb');
  let h = new Database(path, {create: true});
  const s = new DatabaseSync(':memory:');
  for (const q of ['CREATE TABLE u (id INTEGER PRIMARY KEY, a INTEGER, b TEXT, n INTEGER)', 'CREATE UNIQUE INDEX u_ab ON u (a, b)']) { h.sql(q); s.exec(q); }
  const steps = [
    ["INSERT INTO u (id, a, b, n) VALUES (1, 1, 'x', 1), (2, 1, 'y', 2), (3, 2, 'x', 3)", []],
    ["INSERT INTO u (id, a, b, n) VALUES (4, 1, 'x', 4)", []],
    ["INSERT INTO u (id, a, b, n) VALUES (4, 1, 'z', 4), (5, 1, 'z', 5)", []],
    ['INSERT INTO u (id, a, b, n) VALUES (6, NULL, ?, 6), (7, NULL, ?, 7)', ['x', 'x']],
    ['INSERT INTO u (id, a, n) VALUES (8, 1, 8), (9, 1, 9)', []],
    ["UPDATE u SET b = 'x' WHERE id = 2", []],
    ["UPDATE u SET a = 2 WHERE id = 1", []],
    ["UPDATE u SET a = 3 WHERE id = 1", []],
    ["UPDATE u SET b = 'y' WHERE id = 1", []],
    ["INSERT INTO u (id, a, b, n) VALUES (10, 1, 'x', 10)", []],
    ['DELETE FROM u WHERE id = 10', []],
    ["INSERT INTO u (id, a, b, n) VALUES (11, 1, 'x', 11)", []],
    ['BEGIN', []],
    ["INSERT INTO u (id, a, b, n) VALUES (12, 5, 'q', 12)", []],
    ["INSERT INTO u (id, a, b, n) VALUES (13, 5, 'q', 13)", []],
    ["UPDATE u SET b = 'r' WHERE id = 12", []],
    ["INSERT INTO u (id, a, b, n) VALUES (13, 5, 'q', 13)", []],
    ["INSERT INTO u (id, a, b, n) VALUES (14, 2, 'x', 14)", []],
    ['COMMIT', []],
    ['BEGIN', []],
    ["DELETE FROM u WHERE id = 13", []],
    ["INSERT INTO u (id, a, b, n) VALUES (15, 5, 'q', 15)", []],
    ['ROLLBACK', []],
    ["INSERT INTO u (id, a, b, n) VALUES (16, 5, 'q', 16)", []],
    ["INSERT INTO u (id, a, b, n) VALUES (16, 5, 'qq', 16)", []],
  ];
  const all = db => db === h ? h.query('SELECT id, a, b, n FROM u ORDER BY id') : s.prepare('SELECT id, a, b, n FROM u ORDER BY id').all();
  for (const [q, p] of steps) {
    let he = null, se = null;
    try { h.sql(q, p); } catch (e) { he = e; }
    try { s.prepare(q).run(...p); } catch (e) { se = e; }
    assert.equal(he === null, se === null, `${q}: KestroDB ${he?.message ?? 'ok'}, SQLite ${se?.message ?? 'ok'}`);
    if (he) assert.match(he.message, /CONFLICT/, q);
    assert.deepEqual(normalize(all(h), true), normalize(all(s), true), q);
  }
  // Two rows swap their values in one statement: checked against the statement's end state.
  h.sql("UPDATE u SET b = CASE id WHEN 2 THEN 'x' ELSE 'y' END WHERE id IN (2, 11)");
  assert.deepEqual(h.query('SELECT id, b FROM u WHERE a = 1 AND b IN (?, ?) ORDER BY id', ['x', 'y']), [{id: 2, b: 'x'}, {id: 11, b: 'y'}]);
  h.close(); h = new Database(path);
  assert.throws(() => h.sql("INSERT INTO u (a, b) VALUES (5, 'q')"), /CONFLICT/, 'enforced after reopen');
  h.sql("INSERT INTO u (a, b) VALUES (5, NULL), (5, NULL)");
  // Building over duplicates fails and leaves no index; UNIQUE and non-UNIQUE on the same columns conflict.
  h.sql('CREATE TABLE d (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER)');
  h.sql('INSERT INTO d (a, b) VALUES (1, 1), (1, 2), (1, 1), (NULL, 3), (NULL, 3)');
  assert.throws(() => h.sql('CREATE UNIQUE INDEX d_ab ON d (a, b)'), /CONFLICT/);
  assert.deepEqual(h.query('SHOW INDEX FROM d').map(r => r.name), ['PRIMARY']);
  h.sql('DELETE FROM d WHERE id = 3'); h.sql('CREATE UNIQUE INDEX d_ab ON d (a, b)');
  assert.throws(() => h.sql('CREATE INDEX ON d (a, b)'), /different UNIQUE/);
  assert.throws(() => h.sql('INSERT INTO d (a, b) VALUES (1, 2)'), /CONFLICT/);
  h.sql('INSERT INTO d (a, b) VALUES (1, 3), (NULL, 3)');
  // A UNIQUE (a, b) constraint gets a composite index for its lookups.
  h.sql('CREATE TABLE books (id INTEGER PRIMARY KEY, author INTEGER, title TEXT, UNIQUE (author, title))');
  assert.deepEqual(h.query('SHOW INDEX FROM books').map(r => r.columns), [['id'], ['author', 'title']]);
  h.sql("INSERT INTO books (author, title) VALUES (1, 'A'), (1, 'B'), (2, 'A')");
  assert.throws(() => h.sql("INSERT INTO books (author, title) VALUES (1, 'B')"), /CONFLICT/);
  // A foreign key may reference the columns of a composite UNIQUE index.
  h.sql('CREATE TABLE refs (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, FOREIGN KEY (a, b) REFERENCES d (a, b))');
  h.sql('INSERT INTO refs (a, b) VALUES (1, 3)');
  assert.throws(() => h.sql('INSERT INTO refs (a, b) VALUES (1, 9)'), /CONFLICT/);
  h.close(); s.close();
}));

test('ALTER TABLE keeps composite indexes right (RENAME COLUMN, RENAME TO, DROP COLUMN)', () => tempDir(async dir => {
  const h = new Database(join(dir, 'a.kdb'), {create: true});
  h.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, c INTEGER)');
  h.sql('CREATE UNIQUE INDEX t_ab ON t (a, b)'); h.sql('CREATE INDEX ON t (b, c)');
  for (let i = 0; i < 200; i++) h.sql('INSERT INTO t (a, b, c) VALUES (?, ?, ?)', [i % 10, Math.floor(i / 10), i % 7]);
  const q = () => h.query('SELECT id FROM t WHERE a = ? AND b = ? ORDER BY id', [3, 7]).map(r => r.id);
  const before = q();
  assert.equal(before.length, 1);
  h.sql('ALTER TABLE t RENAME COLUMN a TO x');
  assert.deepEqual(h.query('SHOW INDEX FROM t').map(r => [r.name, r.columns, r.unique]), [['PRIMARY', ['id'], true], ['t_b_c_idx', ['b', 'c'], false], ['t_ab', ['x', 'b'], true]]);
  assert.deepEqual(h.query('SELECT id FROM t WHERE x = ? AND b = ? ORDER BY id', [3, 7]).map(r => r.id), before);
  assert.throws(() => h.sql('INSERT INTO t (x, b) VALUES (3, 7)'), /CONFLICT/);
  h.sql('ALTER TABLE t RENAME TO t2');
  assert.deepEqual(h.query('SHOW INDEX FROM t2').map(r => [r.name, r.columns]), [['PRIMARY', ['id']], ['t2_b_c_idx', ['b', 'c']], ['t_ab', ['x', 'b']]]);
  assert.throws(() => h.sql('INSERT INTO t2 (x, b) VALUES (3, 7)'), /CONFLICT/);
  h.sql('ALTER TABLE t2 DROP COLUMN c');
  assert.deepEqual(h.query('SHOW INDEX FROM t2').map(r => r.name), ['PRIMARY', 't_ab']);
  h.close();
}));

test('the composite index is used: a large table answers indexed queries far faster than a scan', () => tempDir(async dir => {
  const h = new Database(join(dir, 'big.kdb'), {create: true});
  h.sql('CREATE TABLE ev (id INTEGER PRIMARY KEY, user INTEGER, day INTEGER, kind TEXT, pad TEXT)');
  const N = 100000, pad = 'p'.repeat(60);
  for (let i = 0; i < N; i += 1000) {
    const v = [], p = [];
    for (let k = i; k < i + 1000; k++) { v.push('(?, ?, ?, ?)'); p.push(k % 500, Math.floor(k / 500), k % 3 ? 'view' : 'buy', pad); }
    h.sql('INSERT INTO ev (user, day, kind, pad) VALUES ' + v.join(', '), p);
  }
  const queries = [
    ['SELECT id FROM ev WHERE user = ? AND day = ?', [123, 77], 1],
    ['SELECT id FROM ev WHERE user = ? AND day BETWEEN ? AND ?', [123, 10, 19], 10],
    ['SELECT id, day FROM ev WHERE user = ? ORDER BY day DESC LIMIT 3', [42], 3],
    ['SELECT id FROM ev WHERE user = ? AND day IN (?, ?, ?)', [7, 1, 2, 3], 3],
  ];
  const time = () => {
    const out = [];
    for (const [q, p, n] of queries) {
      assert.equal(h.query(q, p).length, n, q);
      const t0 = process.hrtime.bigint(); for (let r = 0; r < 20; r++) h.query(q, p);
      out.push(Number(process.hrtime.bigint() - t0) / 20);
    }
    return out;
  };
  const scan = time();
  h.sql('CREATE INDEX ev_user_day ON ev (user, day)');
  const indexed = time();
  assert.deepEqual(h.query(queries[2][0], queries[2][1]).map(r => r.day), [199, 198, 197]);
  for (let i = 0; i < queries.length; i++) assert.ok(indexed[i] * 20 < scan[i], `${queries[i][0]}: ${(indexed[i] / 1e6).toFixed(3)} ms with the index, ${(scan[i] / 1e6).toFixed(3)} ms scanning`);
  h.close();
}));

async function startServer(args) {
  const child = spawn(serverBinary, [...args, '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof'], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '';
  child.stderr.on('data', d => { out += d; });
  const address = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${out}`)));
  });
  const exited = new Promise(r => child.on('exit', r));
  return {address, stop: async () => { child.stdin.end(); return exited; }};
}

test('composite UNIQUE under BEGIN CONCURRENT: the second committer of the same tuple fails', () => tempDir(async dir => {
  const server = await startServer(['--db', join(dir, 'c.kdb'), '--create']);
  const open = [];
  try {
    const conn = async () => { const c = await RemoteDB.connect(server.address); open.push(c); return c; };
    const a = await conn(), b = await conn(), c = await conn();
    await c.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER)');
    await c.sql('CREATE UNIQUE INDEX t_ab ON t (a, b)');
    await c.sql('INSERT INTO t (a, b) VALUES (1, 1), (1, 2)');
    await a.sql('BEGIN CONCURRENT'); await b.sql('BEGIN CONCURRENT');
    await a.sql('INSERT INTO t (a, b) VALUES (2, 2)'); await b.sql('INSERT INTO t (a, b) VALUES (2, 2)');
    await a.sql('INSERT INTO t (a, b) VALUES (3, NULL)'); await b.sql('INSERT INTO t (a, b) VALUES (3, NULL)');
    await assert.rejects(c.sql('INSERT INTO t (a, b) VALUES (1, 2)'), /CONFLICT/);
    await a.sql('COMMIT');
    await assert.rejects(b.sql('COMMIT'), /CONFLICT/);
    assert.equal((await c.sql('SELECT COUNT(*) AS n FROM t WHERE a = 2 AND b = 2')).rows[0].n, 1);
    // Different tuples commit side by side; NULL is exempt.
    await a.sql('BEGIN CONCURRENT'); await b.sql('BEGIN CONCURRENT');
    await a.sql('INSERT INTO t (a, b) VALUES (4, 1), (5, NULL)'); await b.sql('INSERT INTO t (a, b) VALUES (4, 2), (5, NULL)');
    await a.sql('COMMIT'); await b.sql('COMMIT');
    assert.deepEqual((await c.sql('SELECT a, b FROM t WHERE a IN (4, 5) ORDER BY a, b, id')).rows, [{a: 4, b: 1}, {a: 4, b: 2}, {a: 5, b: null}, {a: 5, b: null}]);
  } finally { for (const x of open) await x.close().catch(() => {}); await server.stop(); }
}));

test('a composite index replicates to a follower', () => tempDir(async dir => {
  const ADMIN = 'admin-token-0123456789', REPLICA = 'replica-token-0123456789';
  await writeFile(join(dir, 'users'), `admin admin ${ADMIN}\nrepl replica ${REPLICA}\n`);
  await writeFile(join(dir, 'replica-token'), REPLICA + '\n');
  const leader = await startServer(['--db', join(dir, 'leader.kdb'), '--users', join(dir, 'users'), '--create']);
  let follower, L, F;
  try {
    L = await RemoteDB.connect(leader.address, {token: ADMIN});
    await L.execute('CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT)');
    for (let i = 0; i < 100; i++) await L.execute('INSERT INTO t (a, b) VALUES (?, ?)', [i % 5, 'v' + i]);
    follower = await startServer(['--db', join(dir, 'follower.kdb'), '--users', join(dir, 'users'), '--replica-of', leader.address, '--replica-token-file', join(dir, 'replica-token')]);
    F = await RemoteDB.connect(follower.address, {token: ADMIN});
    await L.execute('CREATE UNIQUE INDEX t_ab ON t (a, b)');
    await L.execute("INSERT INTO t (a, b) VALUES (9, 'z')");
    const want = (await L.stats()).txid;
    for (const until = Date.now() + 10000; (await F.stats()).txid < want; await sleep(20)) if (Date.now() > until) throw Error('follower did not catch up');
    assert.deepEqual((await F.query('SHOW INDEX FROM t')).map(r => [r.name, r.columns, r.unique]), [['PRIMARY', ['id'], true], ['t_ab', ['a', 'b'], true]]);
    for (const [q, p] of [['SELECT id FROM t WHERE a = ? AND b = ?', [2, 'v7']], ['SELECT id FROM t WHERE a = ? AND b = ?', [3, 'v8']], ['SELECT id, b FROM t WHERE a = ? ORDER BY b, id LIMIT 4', [3]], ['SELECT COUNT(*) AS n FROM t WHERE a = 9', []]])
      assert.deepEqual(await F.query(q, p), await L.query(q, p), q);
  } finally {
    await F?.close().catch(() => {}); await L?.close().catch(() => {});
    if (follower) await follower.stop();
    await leader.stop();
  }
}));
