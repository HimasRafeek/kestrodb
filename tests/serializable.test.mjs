// BEGIN CONCURRENT is serializable: COMMIT also fails with CONFLICT when a commit after the
// transaction's snapshot changed a row it read (write skew) or added one to a range it read
// (phantoms). ISOLATION LEVEL SNAPSHOT keeps the snapshot-only behaviour. Two transactions at
// once need two sessions: through the server here (src/tests_portable.cpp runs the same cases
// on the engine in one process); the in-process Database (one session per file) checks the
// syntax and the single-session cases.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Database} from '../index.mjs';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));

async function withServer(fn) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-ser-'));
  const child = spawn(serverBinary, ['--db', join(dir, 's.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof'], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  const exited = new Promise(r => child.on('exit', r));
  const open = [];
  try {
    const address = await new Promise((resolve, reject) => {
      let out = ''; child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
      child.on('exit', code => reject(Error('server exited ' + code)));
    });
    await fn(async () => { const c = await RemoteDB.connect(address); open.push(c); return c; });
  } finally {
    for (const c of open) await c.close().catch(() => {});
    child.stdin.end(); await exited; await rm(dir, {recursive: true, force: true});
  }
}
const value = async (db, sql, params) => Object.values((await db.sql(sql, params)).rows[0])[0];
const lost = async db => {
  await assert.rejects(db.sql('COMMIT'), e => e.code === 'CONFLICT' && /changed a row of \w+ that this one read/.test(e.message));
  assert.equal(db.inTransaction, false, 'a failed COMMIT ends the transaction');
};

test('server: write skew (two doctors on call) fails the second COMMIT; ISOLATION LEVEL SNAPSHOT allows it', async () => {
  await withServer(async conn => {
    const a = await conn(), b = await conn(), c = await conn();
    await c.sql('CREATE TABLE doctors (id INTEGER PRIMARY KEY, name TEXT, on_call BOOLEAN)');
    for (const [index, begin] of [[false, 'BEGIN CONCURRENT'], [true, 'BEGIN CONCURRENT ISOLATION LEVEL SERIALIZABLE'], [true, 'BEGIN CONCURRENT ISOLATION LEVEL SNAPSHOT']]) {
      if (index) await c.sql('CREATE INDEX IF NOT EXISTS doctors_on_call ON doctors (on_call)');
      await c.sql('DELETE FROM doctors');
      await c.sql("INSERT INTO doctors (id, name, on_call) VALUES (1, 'alice', true), (2, 'bob', true)");
      await a.sql(begin); await b.sql(begin);
      // Each sees two doctors on call and lets one go: alone, each keeps one on call.
      assert.equal(await value(a, 'SELECT COUNT(*) FROM doctors WHERE on_call = true'), 2);
      assert.equal(await value(b, 'SELECT COUNT(*) FROM doctors WHERE on_call = true'), 2);
      await a.sql('UPDATE doctors SET on_call = false WHERE id = 1');
      await b.sql('UPDATE doctors SET on_call = false WHERE id = 2');
      await a.sql('COMMIT');
      if (begin.endsWith('SNAPSHOT')) {
        await b.sql('COMMIT');
        assert.equal(await value(c, 'SELECT COUNT(*) FROM doctors WHERE on_call = true'), 0, 'snapshot isolation: write skew');
      } else {
        await lost(b);
        assert.equal(await value(c, 'SELECT COUNT(*) FROM doctors WHERE on_call = true'), 1, `${begin}${index ? ' (index)' : ''}: one stays on call`);
        // Run again, it sees the new state and keeps bob on call.
        await b.sql(begin);
        assert.equal(await value(b, 'SELECT COUNT(*) FROM doctors WHERE on_call = true'), 1);
        await b.sql('COMMIT');
      }
    }
  });
});

test('server: a row inserted into a range the transaction read (phantom) fails its COMMIT; outside it does not', async () => {
  await withServer(async conn => {
    const a = await conn(), c = await conn();
    await c.sql('CREATE TABLE orders (id INTEGER PRIMARY KEY, customer INTEGER, amount INTEGER)');
    await c.sql('CREATE INDEX ON orders (customer)');
    await c.sql('CREATE TABLE invoices (id INTEGER PRIMARY KEY, customer INTEGER, total INTEGER)');
    await c.sql('INSERT INTO orders (customer, amount) VALUES (5, 10), (5, 20), (6, 7)');
    const invoice = async () => {
      await a.sql('BEGIN CONCURRENT');
      const total = await value(a, 'SELECT SUM(amount) FROM orders WHERE customer = ?', [5]);
      await a.sql('INSERT INTO invoices (customer, total) VALUES (5, ?)', [total]);
    };
    await invoice();
    await c.sql('INSERT INTO orders (customer, amount) VALUES (6, 1)');  // another customer
    await a.sql('COMMIT');
    await invoice();
    await c.sql('INSERT INTO orders (customer, amount) VALUES (5, 1)');  // the invoice would miss it
    await lost(a);
    // A range of ids, and a whole-table scan.
    await a.sql('BEGIN CONCURRENT');
    assert.equal(await value(a, 'SELECT COUNT(*) FROM orders WHERE id >= 4'), 2);
    await a.sql('UPDATE invoices SET total = 0 WHERE id = 1');
    await c.sql('INSERT INTO orders (customer, amount) VALUES (7, 1)');
    await lost(a);
    await a.sql('BEGIN CONCURRENT');
    assert.equal(await value(a, 'SELECT MAX(amount) FROM orders WHERE amount < 15'), 10);
    await a.sql('UPDATE invoices SET total = 0 WHERE id = 1');
    await c.sql('INSERT INTO orders (customer, amount) VALUES (8, 100)');  // not matching, but the table was read whole
    await lost(a);
    assert.equal(await value(c, 'SELECT COUNT(*) FROM invoices'), 1);
  });
});

test('server: read-only transactions commit; disjoint transactions both commit', async () => {
  await withServer(async conn => {
    const a = await conn(), b = await conn(), c = await conn();
    await c.sql('CREATE TABLE acct (id INTEGER PRIMARY KEY, owner TEXT UNIQUE, grp INTEGER, bal INTEGER)');
    await c.sql('CREATE INDEX ON acct (grp)');
    await c.sql("INSERT INTO acct (owner, grp, bal) VALUES ('ann', 1, 100), ('bob', 2, 100), ('cid', 3, 100), ('dan', 4, 100)");
    // Read-only: whatever changed meanwhile, it read one committed state.
    await a.sql('BEGIN CONCURRENT');
    assert.equal(await value(a, 'SELECT SUM(bal) FROM acct'), 400);
    await c.sql('UPDATE acct SET bal = bal + 1'); await c.sql('DELETE FROM acct WHERE id = 4'); await c.sql("INSERT INTO acct (owner, grp, bal) VALUES ('dan', 4, 100)");
    assert.equal(await value(a, 'SELECT SUM(bal) FROM acct'), 400, 'its snapshot');
    await a.sql('COMMIT');
    // Disjoint reads (by id, by a unique value, by an index value, up to a LIMIT) and writes.
    await a.sql('BEGIN CONCURRENT'); await b.sql('BEGIN CONCURRENT');
    const fromAnn = await value(a, 'SELECT bal FROM acct WHERE id = 1');
    await value(a, "SELECT bal FROM acct WHERE owner = 'bob'");
    assert.equal(await value(a, 'SELECT id FROM acct ORDER BY id LIMIT 1'), 1);
    assert.equal(await value(b, 'SELECT COUNT(*) FROM acct WHERE grp = 3'), 1);
    await value(b, "SELECT bal FROM acct WHERE owner = 'dan'");
    await a.sql('UPDATE acct SET bal = ? WHERE id = 1', [fromAnn - 10]); await a.sql("UPDATE acct SET bal = bal + 10 WHERE owner = 'bob'");
    await b.sql('UPDATE acct SET bal = bal - 10 WHERE grp = 3'); await b.sql("UPDATE acct SET bal = bal + 10 WHERE owner = 'dan'");
    await b.sql("INSERT INTO acct (owner, grp, bal) VALUES ('eve', 5, 0)");
    await b.sql('COMMIT'); await a.sql('COMMIT');
    assert.deepEqual(await c.query('SELECT owner, bal FROM acct ORDER BY owner'),
      [{owner: 'ann', bal: 91}, {owner: 'bob', bal: 111}, {owner: 'cid', bal: 91}, {owner: 'dan', bal: 110}, {owner: 'eve', bal: 0}]);
  });
});

test('server: a read set past its limit counts its tables as read whole', async () => {
  await withServer(async conn => {
    const a = await conn(), c = await conn();
    await c.sql('CREATE TABLE big (id INTEGER PRIMARY KEY, v INTEGER)');
    await c.sql('CREATE INDEX ON big (v)');
    await c.sql('CREATE TABLE note (id INTEGER PRIMARY KEY, n INTEGER)');
    await c.sql('INSERT INTO note (n) VALUES (0), (0)');
    for (let i = 0; i < 20; i++) await c.sql('INSERT INTO big (v) VALUES ' + Array.from({length: 1000}, (_, k) => `(${2 * (i * 1000 + k)})`).join(', '));
    // Even values 0, 2, ... read in statements of 3,000 (one index range each).
    const read = async n => {
      await a.sql('BEGIN CONCURRENT');
      for (let from = 0; from < n; from += 3000) {
        const vals = Array.from({length: Math.min(3000, n - from)}, (_, k) => 2 * (from + k));
        assert.equal(await value(a, `SELECT COUNT(*) FROM big WHERE v IN (${vals.map(() => '?').join(', ')})`, vals), vals.length);
      }
      await value(a, 'SELECT n FROM note WHERE id = 1');
      await a.sql('UPDATE note SET n = n + 1 WHERE id = 1');
    };
    // A few: exact (v = 3 lies between values read; note's row 2 was not read).
    await read(100);
    await c.sql('INSERT INTO big (v) VALUES (3)'); await c.sql('UPDATE note SET n = 5 WHERE id = 2');
    await a.sql('COMMIT');
    // 18,000: past the limit, big counts as read whole; note, read after, stays exact.
    await read(18000);
    await c.sql('UPDATE note SET n = 6 WHERE id = 2');
    await a.sql('COMMIT');
    await read(18000);
    await c.sql('INSERT INTO big (v) VALUES (3)');
    await lost(a);
    assert.equal(await value(c, 'SELECT n FROM note WHERE id = 1'), 2);
  });
});

test('in-process: ISOLATION LEVEL syntax, read-only and single-session serializable transactions', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-ser-ip-'));
  const db = new Database(join(dir, 'p.kdb'), {create: true});
  try {
    db.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)');
    db.sql('CREATE INDEX ON t (v)');
    db.sql('INSERT INTO t (v) VALUES (1), (2), (3)');
    for (const begin of ['BEGIN CONCURRENT', 'BEGIN CONCURRENT ISOLATION LEVEL SERIALIZABLE', 'BEGIN CONCURRENT TRANSACTION ISOLATION LEVEL SNAPSHOT',
      'begin concurrent isolation level repeatable read', 'BEGIN CONCURRENT ISOLATION LEVEL READ COMMITTED', 'BEGIN ISOLATION LEVEL SERIALIZABLE',
      'START TRANSACTION ISOLATION LEVEL READ UNCOMMITTED']) {
      db.sql(begin);
      assert.equal(db.inTransaction, true, begin);
      assert.equal(db.queryValue('SELECT COUNT(*) FROM t WHERE v >= 1'), 3, begin);
      db.sql('COMMIT');
    }
    for (const bad of ['BEGIN CONCURRENT ISOLATION LEVEL', 'BEGIN CONCURRENT ISOLATION LEVEL CHAOS', 'BEGIN ISOLATION SERIALIZABLE', 'BEGIN CONCURRENT ISOLATION LEVEL REPEATABLE'])
      assert.throws(() => db.sql(bad), e => e.code === 'INPUT', bad);
    assert.equal(db.inTransaction, false);
    // Reads (point, range, scan), its own writes, and a COMMIT with nothing changed meanwhile.
    db.sql('BEGIN CONCURRENT');
    assert.equal(db.queryValue('SELECT v FROM t WHERE id = 2'), 2);
    assert.equal(db.queryValue('SELECT SUM(v) FROM t WHERE v BETWEEN 2 AND 3'), 5);
    db.sql('UPDATE t SET v = v * 10 WHERE v >= 2');
    db.sql('INSERT INTO t (v) VALUES (4)');
    assert.deepEqual(db.query('SELECT v FROM t ORDER BY v'), [{v: 1}, {v: 4}, {v: 20}, {v: 30}]);
    assert.equal(db.sql('COMMIT').changes, 3);
    assert.equal(db.queryValue('SELECT SUM(v) FROM t'), 55);
    // Read-only, and rolled back.
    db.sql('BEGIN CONCURRENT'); db.query('SELECT * FROM t'); db.sql('COMMIT');
    db.sql('BEGIN CONCURRENT'); db.query('SELECT * FROM t WHERE v = 20'); db.sql('DELETE FROM t'); db.sql('ROLLBACK');
    assert.equal(db.queryValue('SELECT COUNT(*) FROM t'), 4);
  } finally { db.close(); await rm(dir, {recursive: true, force: true}); }
});
