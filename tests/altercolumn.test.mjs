// ALTER TABLE ... ALTER [COLUMN] (SET/DROP DEFAULT, SET/DROP NOT NULL, [SET DATA] TYPE), MySQL's
// MODIFY, RENAME COLUMN (indexes and constraints follow, compared with SQLite) and VARCHAR(n)
// limits. A failed statement leaves the table as it was; every change survives reopening.
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
const ADMIN = 'admin-token-0123456789', REPLICA = 'replica-token-0123456789';
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function caughtUp(follower, leader, ms = 10000) {
  const want = (await leader.stats()).txid;
  for (const until = Date.now() + ms; Date.now() < until; await sleep(20)) if ((await follower.stats()).txid >= want) return;
  throw Error(`follower did not catch up to ${want}`);
}

const fails = (db, q, re, p = []) => assert.throws(() => db.sql(q, p), re, q);
const col = (db, t, c) => db.query(`DESCRIBE ${t}`).find(r => r.column === c);
const rows = (db, t) => db.query(`SELECT * FROM ${t} ORDER BY id`);
// Runs body with a fresh database file; reopen() closes and opens it again.
async function withDb(body) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-altcol-'));
  const file = join(dir, 'h.kdb');
  let db = new Database(file, {create: true});
  try {
    await body(() => db, () => { db.close(); db = new Database(file); return db; });
  } finally { try { db.close(); } catch {} await rm(dir, {recursive: true, force: true}); }
}

test('SET DEFAULT and DROP DEFAULT', () => withDb((h, reopen) => {
  h().sql('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, plan TEXT, n INTEGER)');
  h().sql("INSERT INTO t (name) VALUES ('a')");
  h().sql("ALTER TABLE t ALTER COLUMN plan SET DEFAULT 'free'");
  h().sql('ALTER TABLE t ALTER n SET DEFAULT -3');
  assert.deepEqual(rows(h(), 't'), [{id: 1, name: 'a'}]);  // existing rows keep what they had
  reopen();
  assert.equal(col(h(), 't', 'plan').default, 'free');
  h().sql("INSERT INTO t (name) VALUES ('b')");
  h().sql("INSERT INTO t (name, plan) VALUES ('c', 'pro')");
  assert.deepEqual(h().query('SELECT name, plan, n FROM t WHERE id > 1 ORDER BY id'), [{name: 'b', plan: 'free', n: -3}, {name: 'c', plan: 'pro', n: -3}]);
  // The default must fit the column.
  fails(h(), "ALTER TABLE t ALTER COLUMN n SET DEFAULT 'x'", /INPUT.*expects an integer/);
  fails(h(), 'ALTER TABLE t ALTER COLUMN n SET DEFAULT ABS(-1)', /INPUT.*constant/);
  fails(h(), 'ALTER TABLE t ALTER COLUMN nope SET DEFAULT 1', /INPUT.*no declared column nope/);
  fails(h(), 'ALTER TABLE t ALTER COLUMN id SET DEFAULT 1', /INPUT.*id/);
  assert.equal(col(h(), 't', 'n').default, -3);
  h().sql('ALTER TABLE t ALTER COLUMN plan DROP DEFAULT');
  h().sql('ALTER TABLE t ALTER COLUMN n SET DEFAULT NULL');  // the same as DROP DEFAULT
  reopen();
  assert.equal(col(h(), 't', 'plan').default, null);
  h().sql("INSERT INTO t (name) VALUES ('d')");
  assert.deepEqual(h().queryOne("SELECT * FROM t WHERE name = 'd'"), {id: 4, name: 'd'});
  // Inside a transaction: refused.
  h().sql('BEGIN');
  fails(h(), "ALTER TABLE t ALTER COLUMN plan SET DEFAULT 'x'", /UNSUPPORTED.*transaction/);
  h().sql('ROLLBACK');
}));

test('SET NOT NULL and DROP NOT NULL', () => withDb((h, reopen) => {
  h().sql('CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT, b INTEGER)');
  h().sql("INSERT INTO t (id, a, b) VALUES (1, 'x', 1), (2, NULL, 2)");
  h().sql('INSERT INTO t (id, b) VALUES (3, 3)');  // a missing
  fails(h(), 'ALTER TABLE t ALTER COLUMN a SET NOT NULL', /CONFLICT.*t\.a is NULL in row id 2/);
  assert.equal(col(h(), 't', 'a').notNull, false);
  h().sql("UPDATE t SET a = 'y' WHERE id = 2");
  fails(h(), 'ALTER TABLE t ALTER COLUMN a SET NOT NULL', /CONFLICT.*row id 3/);  // missing counts as NULL
  h().sql("UPDATE t SET a = 'z' WHERE id = 3");
  h().sql('ALTER TABLE t ALTER COLUMN a SET NOT NULL');
  h().sql('ALTER TABLE t ALTER b SET NOT NULL');
  reopen();
  assert.equal(col(h(), 't', 'a').notNull, true);
  fails(h(), 'INSERT INTO t (id, a, b) VALUES (4, NULL, 4)', /INPUT.*cannot be NULL/);
  fails(h(), 'INSERT INTO t (id, b) VALUES (4, 4)', /INPUT.*NOT NULL and has no DEFAULT/);
  fails(h(), 'UPDATE t SET b = NULL WHERE id = 1', /INPUT.*cannot be NULL/);
  h().sql('ALTER TABLE t ALTER COLUMN a DROP NOT NULL');
  reopen();
  h().sql('INSERT INTO t (id, b) VALUES (4, 4)');
  assert.equal(col(h(), 't', 'a').notNull, false);
  // A foreign key that sets the column to NULL keeps it nullable.
  h().sql('CREATE TABLE c (id INTEGER PRIMARY KEY, p INTEGER REFERENCES t ON DELETE SET NULL)');
  fails(h(), 'ALTER TABLE c ALTER COLUMN p SET NOT NULL', /INPUT.*sets p to NULL/);
}));

test('ALTER COLUMN TYPE converts values or fails without changing anything', () => withDb((h, reopen) => {
  h().sql('CREATE TABLE t (id INTEGER PRIMARY KEY, code TEXT, price TEXT, flag TEXT, qty INTEGER)');
  h().sql("INSERT INTO t (id, code, price, flag, qty) VALUES (1, '10', '1.5', 'yes', 7), (2, ' -20 ', '2e3', 'F', 8), (3, NULL, NULL, NULL, NULL), (4, 'x9', '.5', 'true', 9)");
  h().sql('INSERT INTO t (id) VALUES (5)');
  h().sql('CREATE INDEX code_idx ON t (code)');
  const before = rows(h(), 't');
  fails(h(), 'ALTER TABLE t ALTER COLUMN code TYPE INTEGER', /CONFLICT.*t\.code of row id 4 is "x9", which does not fit integer/);
  fails(h(), 'ALTER TABLE t ALTER COLUMN price TYPE INTEGER', /CONFLICT.*row id 1 is "1\.5"/);
  fails(h(), 'ALTER TABLE t ALTER COLUMN code TYPE BOOLEAN', /CONFLICT/);
  fails(h(), 'ALTER TABLE t ALTER COLUMN code TYPE INTEGER USING CAST(code AS INTEGER)', /INPUT.*USING/);
  assert.deepEqual(rows(h(), 't'), before);
  assert.equal(col(h(), 't', 'code').type, 'text');
  h().sql("UPDATE t SET code = '9' WHERE id = 4");
  assert.deepEqual(h().sql('ALTER TABLE t ALTER COLUMN code SET DATA TYPE BIGINT'), {changes: 3});
  h().sql('ALTER TABLE t ALTER COLUMN price TYPE DOUBLE PRECISION');
  h().sql('ALTER TABLE t ALTER COLUMN flag TYPE BOOLEAN');
  h().sql('ALTER TABLE t ALTER COLUMN qty TYPE VARCHAR(3)');
  reopen();
  assert.deepEqual(h().query('SELECT id, code, price, flag, qty FROM t ORDER BY id'), [
    {id: 1, code: 10, price: 1.5, flag: true, qty: '7'},
    {id: 2, code: -20, price: 2000, flag: false, qty: '8'},
    {id: 3, code: null, price: null, flag: null, qty: null},
    {id: 4, code: 9, price: 0.5, flag: true, qty: '9'},
    {id: 5, code: null, price: null, flag: null, qty: null},
  ]);
  assert.deepEqual(h().query('DESCRIBE t').map(r => r.type), ['integer', 'integer', 'number', 'boolean', 'text']);
  // The index follows the new values.
  assert.deepEqual(h().query('SELECT id FROM t WHERE code = 10'), [{id: 1}]);
  assert.deepEqual(h().query("SELECT id FROM t WHERE code = '10'"), []);
  assert.ok(h().query('SHOW INDEX FROM t').some(r => r.name === 'code_idx' && r.entries === 5));
  // New writes are checked against the new types.
  fails(h(), "INSERT INTO t (code) VALUES ('5')", /INPUT.*expects an integer/);
  fails(h(), "INSERT INTO t (qty) VALUES ('1234')", /INPUT.*at most 3 characters/);
  // Back to text: numbers and booleans become text.
  h().sql('ALTER TABLE t ALTER COLUMN code TYPE TEXT');
  h().sql('ALTER TABLE t ALTER COLUMN price TYPE TEXT');
  h().sql('ALTER TABLE t ALTER COLUMN flag TYPE TEXT');
  assert.deepEqual(h().query('SELECT code, price, flag FROM t WHERE id IN (1, 2) ORDER BY id'), [{code: '10', price: '1.5', flag: 'true'}, {code: '-20', price: '2000', flag: 'false'}]);
  // A shorter VARCHAR fails when a value is too long; numbers with a fraction are not integers.
  h().sql("UPDATE t SET code = 'abcdef' WHERE id = 3");
  fails(h(), 'ALTER TABLE t ALTER COLUMN code TYPE VARCHAR(5)', /CONFLICT.*row id 3.*varchar|CONFLICT.*row id 3.*text\(5\)/i);
  h().sql('ALTER TABLE t ALTER COLUMN code TYPE VARCHAR(6)');
  fails(h(), "UPDATE t SET code = 'abcdefg' WHERE id = 3", /INPUT.*at most 6/);
  fails(h(), 'ALTER TABLE t ALTER COLUMN price TYPE INT', /CONFLICT.*"1\.5"/);
  assert.equal(col(h(), 't', 'price').type, 'text');
}));

test('TYPE keeps NOT NULL and the default (converted), and checks constraints', () => withDb((h, reopen) => {
  h().sql("CREATE TABLE t (id INTEGER PRIMARY KEY, n TEXT NOT NULL DEFAULT '5', u TEXT UNIQUE, k TEXT CHECK (LENGTH(k) < 3))");
  h().sql("INSERT INTO t (id, u, k) VALUES (1, '5', 'ab'), (2, ' 5', 'cd'), (3, '6', NULL)");
  fails(h(), 'ALTER TABLE t ALTER COLUMN u TYPE INTEGER', /CONFLICT.*repeat 5.*UNIQUE/);  // '5' and ' 5' both become 5
  assert.deepEqual(h().query('SELECT u FROM t ORDER BY id'), [{u: '5'}, {u: ' 5'}, {u: '6'}]);
  h().sql("UPDATE t SET u = '7' WHERE id = 2");
  h().sql('ALTER TABLE t ALTER COLUMN u TYPE INTEGER');
  fails(h(), 'INSERT INTO t (id, u) VALUES (9, 7)', /CONFLICT/);
  // The CHECK no longer holds on numbers: LENGTH needs text.
  h().sql("UPDATE t SET k = '12' WHERE id = 1");
  h().sql("UPDATE t SET k = '34' WHERE id = 2");
  fails(h(), 'ALTER TABLE t ALTER COLUMN k TYPE INTEGER', /LENGTH needs text/);
  assert.equal(h().queryValue('SELECT k FROM t WHERE id = 1'), '12');
  h().sql('ALTER TABLE t ALTER COLUMN n TYPE INTEGER');
  reopen();
  const n = col(h(), 't', 'n');
  assert.deepEqual([n.type, n.notNull, n.default], ['integer', true, 5]);
  h().sql('INSERT INTO t (id) VALUES (10)');
  assert.equal(h().queryValue('SELECT n FROM t WHERE id = 10'), 5);
  h().sql("ALTER TABLE t ALTER COLUMN n SET DEFAULT 6");
  h().sql('ALTER TABLE t ALTER COLUMN n TYPE TEXT');
  assert.equal(col(h(), 't', 'n').default, '6');
  h().sql("ALTER TABLE t ALTER COLUMN n SET DEFAULT 'abc'");
  fails(h(), 'ALTER TABLE t ALTER COLUMN n TYPE INTEGER', /INPUT.*DEFAULT "abc"/);
  // Foreign key columns keep their type (values compare strictly).
  h().sql('CREATE TABLE c (id INTEGER PRIMARY KEY, p INTEGER REFERENCES t (u))');
  fails(h(), 'ALTER TABLE c ALTER COLUMN p TYPE TEXT', /INPUT.*foreign key/);
  fails(h(), 'ALTER TABLE t ALTER COLUMN u TYPE TEXT', /INPUT.*foreign key.*of c/);
  h().sql('ALTER TABLE c ALTER COLUMN p TYPE BIGINT');  // same class: allowed
  fails(h(), 'ALTER TABLE t ALTER COLUMN u TYPE TEXT', /INPUT/);
}));

test('MODIFY replaces the column definition (MySQL)', () => withDb((h, reopen) => {
  h().sql('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL DEFAULT \'n\', age TEXT, tag TEXT)');
  h().sql("INSERT INTO t (id, name, age, tag) VALUES (1, 'ann', '30', 'a'), (2, 'bob', NULL, 'b')");
  fails(h(), 'ALTER TABLE t MODIFY age INT NOT NULL', /CONFLICT.*NULL in row id 2/);
  assert.equal(h().queryValue('SELECT age FROM t WHERE id = 1'), '30');
  h().sql('ALTER TABLE t MODIFY COLUMN age INT NOT NULL DEFAULT 0 AFTER name'.replace(' NOT NULL', ''));
  assert.deepEqual(h().query('SELECT age FROM t ORDER BY id'), [{age: 30}, {age: null}]);
  fails(h(), "ALTER TABLE t MODIFY name VARCHAR(2) NOT NULL", /CONFLICT.*"ann".*text\(2\)/);
  h().sql('ALTER TABLE t MODIFY name VARCHAR(10)');  // no NOT NULL and no DEFAULT any more
  reopen();
  const name = col(h(), 't', 'name');
  assert.deepEqual([name.type, name.notNull, name.default], ['text', false, null]);
  assert.equal(col(h(), 't', 'age').default, 0);
  h().sql('INSERT INTO t (id, tag) VALUES (3, \'a\')');
  assert.deepEqual(h().queryOne('SELECT name, age FROM t WHERE id = 3'), {name: null, age: 0});
  fails(h(), "INSERT INTO t (id, name) VALUES (4, 'abcdefghijk')", /INPUT.*at most 10/);
  // UNIQUE adds a unique index once the values do not repeat.
  fails(h(), 'ALTER TABLE t MODIFY tag TEXT UNIQUE', /CONFLICT.*repeat "a"/);
  assert.ok(!h().query('SHOW INDEX FROM t').some(r => r.column === 'tag'));
  h().sql("UPDATE t SET tag = 'c' WHERE id = 3");
  h().sql('ALTER TABLE t MODIFY tag VARCHAR(5) UNIQUE');
  assert.ok(h().query('SHOW INDEX FROM t').some(r => r.column === 'tag' && r.unique));
  fails(h(), "INSERT INTO t (id, tag) VALUES (5, 'a')", /CONFLICT/);
  fails(h(), 'ALTER TABLE t MODIFY tag TEXT CHECK (tag <> \'\')', /INPUT.*ADD CONSTRAINT/);
  fails(h(), 'ALTER TABLE t MODIFY id TEXT', /INPUT.*id/);
  fails(h(), 'ALTER TABLE t MODIFY nope TEXT', /INPUT.*no declared column/);
}));

test('VARCHAR(n) and CHAR(n) limits on INSERT and UPDATE', () => withDb((h, reopen) => {
  h().sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v VARCHAR(3), c CHAR(2), w CHARACTER VARYING(4), x TEXT, d DECIMAL(10, 2), n NVARCHAR(2))');
  h().sql("INSERT INTO t (v, c, w, x, d) VALUES ('abc', 'ab', 'abcd', 'anything at all', 1.25)");
  h().sql("INSERT INTO t (v, c) VALUES ('héé', 'ñ')");  // characters, not bytes
  fails(h(), "INSERT INTO t (v) VALUES ('abcd')", /INPUT.*t\.v holds at most 3 characters, got 4/);
  fails(h(), "INSERT INTO t (c) VALUES ('abc')", /INPUT.*t\.c holds at most 2/);
  fails(h(), "INSERT INTO t (w) VALUES ('abcde')", /INPUT.*t\.w holds at most 4/);
  fails(h(), "INSERT INTO t (n) VALUES ('abc')", /INPUT.*t\.n holds at most 2/);
  fails(h(), "UPDATE t SET v = v || 'x' WHERE id = 1", /INPUT.*at most 3/);
  fails(h(), "INSERT INTO t (id, v) VALUES (1, 'a') ON CONFLICT (id) DO UPDATE SET v = 'long'", /INPUT.*at most 3/);
  fails(h(), 'INSERT INTO t (v) VALUES (?)', /INPUT/, ['toolong']);
  fails(h(), 'CREATE TABLE bad (id INTEGER PRIMARY KEY, v VARCHAR(0))', /INPUT/);
  reopen();
  fails(h(), "INSERT INTO t (v) VALUES ('abcd')", /INPUT.*at most 3/);
  assert.equal(h().queryValue('SELECT COUNT(*) FROM t'), 2);
  h().sql("UPDATE t SET v = 'xy' WHERE id = 1");
  h().sql('ALTER TABLE t ALTER COLUMN v TYPE TEXT');  // no limit any more
  h().sql("INSERT INTO t (v) VALUES ('a much longer text')");
  h().sql('ALTER TABLE t ADD COLUMN z VARCHAR(2) DEFAULT \'ok\'');
  fails(h(), "UPDATE t SET z = 'abc'", /INPUT.*t\.z holds at most 2/);
}));

test('RENAME COLUMN carries indexes and constraints (compared with SQLite)', () => withDb((h, reopen) => {
  const s = new DatabaseSync(':memory:');
  s.exec('PRAGMA foreign_keys = ON');
  const both = (q, p = []) => {
    let he = null, se = null;
    try { h().sql(q, p); } catch (e) { he = e; }
    try { s.prepare(q).run(...p); } catch (e) { se = e; }
    assert.equal(!!he, !!se, `${q}\n  KestroDB: ${he?.message ?? 'ok'}\n  SQLite: ${se?.message ?? 'ok'}`);
  };
  const same = (t) => assert.deepEqual(
    h().query(`SELECT * FROM ${t} ORDER BY id`).map(r => JSON.stringify(Object.values(r))),
    s.prepare(`SELECT * FROM ${t} ORDER BY id`).all().map(r => JSON.stringify(Object.values(r))), t);
  const steps = [
    'CREATE TABLE parent (id INTEGER PRIMARY KEY, code TEXT UNIQUE, score INTEGER CHECK (score >= 0), a INTEGER, b INTEGER, UNIQUE (a, b))',
    'CREATE TABLE child (id INTEGER PRIMARY KEY, pcode TEXT REFERENCES parent (code) ON UPDATE CASCADE ON DELETE CASCADE, pid INTEGER REFERENCES parent (id))',
    'CREATE INDEX by_score ON parent (score)',
    "INSERT INTO parent (id, code, score, a, b) VALUES (1, 'p1', 5, 1, 1), (2, 'p2', 7, 1, 2)",
    "INSERT INTO child (id, pcode, pid) VALUES (1, 'p1', 1), (2, 'p2', 2), (3, 'p1', NULL)",
    'ALTER TABLE parent RENAME COLUMN code TO ref',
    'ALTER TABLE parent RENAME COLUMN score TO points',
    'ALTER TABLE parent RENAME a TO x',
    'ALTER TABLE child RENAME COLUMN pcode TO parent_ref',
    'ALTER TABLE child RENAME COLUMN pid TO parent_id',
    'ALTER TABLE parent RENAME COLUMN nothere TO y',
    'ALTER TABLE parent RENAME COLUMN points TO ref',
    // The constraints now use the new names.
    "INSERT INTO parent (id, ref, points, x, b) VALUES (3, 'p3', -1, 9, 9)",
    "INSERT INTO parent (id, ref, points, x, b) VALUES (3, 'p1', 1, 9, 9)",
    "INSERT INTO parent (id, ref, points, x, b) VALUES (3, 'p3', 1, 1, 2)",
    "INSERT INTO parent (id, ref, points, x, b) VALUES (3, 'p3', 1, 2, 2)",
    "INSERT INTO child (id, parent_ref) VALUES (4, 'nope')",
    'INSERT INTO child (id, parent_id) VALUES (4, 99)',
    "INSERT INTO child (id, parent_ref, parent_id) VALUES (4, 'p3', 3)",
    "UPDATE parent SET ref = 'p1x' WHERE id = 1",
    'DELETE FROM parent WHERE id = 2',
    'UPDATE parent SET points = -5 WHERE id = 1',
  ];
  for (const q of steps) {
    both(q);
    if (q.startsWith('CREATE TABLE')) continue;
    for (const t of ['parent', 'child']) same(t);
    if (q.startsWith('ALTER')) { reopen(); for (const t of ['parent', 'child']) same(t); }
  }
  // Indexes moved with the column; a named one keeps its name.
  const idx = h().query('SHOW INDEX FROM parent');
  assert.ok(idx.some(r => r.name === 'by_score' && r.column === 'points' && r.entries === 2), JSON.stringify(idx));
  assert.ok(idx.some(r => r.column === 'ref' && r.unique), JSON.stringify(idx));
  assert.ok(!idx.some(r => r.column === 'code' || r.column === 'score'));
  assert.ok(h().query('SHOW INDEX FROM child').some(r => r.column === 'parent_ref'));
  assert.deepEqual(h().query('SELECT id FROM parent WHERE points = 7'), []);
  assert.deepEqual(h().query('SELECT id FROM parent WHERE points = 1'), [{id: 3}]);
  assert.deepEqual(h().query("SELECT id FROM parent WHERE ref = 'p1x'"), [{id: 1}]);
  // The schema has the new names; the old ones are gone.
  assert.deepEqual(h().query('DESCRIBE parent').map(r => r.column), ['id', 'ref', 'points', 'x', 'b']);
  fails(h(), 'ALTER TABLE parent RENAME COLUMN id TO key', /INPUT.*id/);
  s.close();
}));

// Through a server: a BEGIN CONCURRENT transaction that spans an ALTER COLUMN must run again,
// and a follower applies every change (schema, converted values, limits).
async function start(args) {
  const child = spawn(serverBinary, [...args, '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof'], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '';
  child.stderr.on('data', d => { out += d; });
  const address = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${out}`)));
  });
  const exited = new Promise(resolve => child.on('exit', resolve));
  return {address, stop: async () => { child.stdin.end(); return exited; }};
}
test('server: transactions across ALTER COLUMN, and replication', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-altcol-srv-'));
  await writeFile(join(dir, 'users'), `admin admin ${ADMIN}\nrepl replica ${REPLICA}\n`);
  await writeFile(join(dir, 'replica-token'), REPLICA + '\n');
  const users = ['--users', join(dir, 'users')];
  const leader = await start(['--db', join(dir, 'l.kdb'), '--create', ...users]);
  let follower;
  try {
    const L = await RemoteDB.connect(leader.address, {token: ADMIN}), L2 = await RemoteDB.connect(leader.address, {token: ADMIN});
    await L.execute('CREATE TABLE t (id INTEGER PRIMARY KEY, code TEXT, name VARCHAR(4), n INTEGER)');
    await L.execute('CREATE INDEX ON t (code)');
    for (let i = 1; i <= 50; i++) await L.execute('INSERT INTO t (code, name, n) VALUES (?, ?, ?)', [String(i), 'n' + i, i % 3 ? i : null]);
    follower = await start(['--db', join(dir, 'f.kdb'), ...users, '--replica-of', leader.address, '--replica-token-file', join(dir, 'replica-token')]);
    const F = await RemoteDB.connect(follower.address, {token: ADMIN});
    // A concurrent transaction that began before the change fails at COMMIT.
    await L2.sql('BEGIN CONCURRENT');
    await L2.sql("UPDATE t SET code = '500' WHERE id = 5");
    await L.execute('ALTER TABLE t ALTER COLUMN code TYPE INTEGER');
    await assert.rejects(L2.sql('COMMIT'), e => /CONFLICT/.test(e.message) && /schema/.test(e.message));
    await L.execute("ALTER TABLE t ALTER COLUMN name SET DEFAULT 'none'");
    await L.execute('ALTER TABLE t RENAME COLUMN name TO label');
    await assert.rejects(L.execute('ALTER TABLE t ALTER COLUMN n SET NOT NULL'), e => e.code === 'CONFLICT');
    await L.execute('UPDATE t SET n = 0 WHERE n IS NULL');
    await L.execute('ALTER TABLE t ALTER COLUMN n SET NOT NULL');
    await L.execute('ALTER TABLE t MODIFY n TEXT NOT NULL');
    await L.execute('INSERT INTO t (code, n) VALUES (51, ?)', ['x']);
    await caughtUp(F, L);
    for (const q of ['DESCRIBE t', 'SHOW INDEX FROM t', 'SELECT * FROM t ORDER BY id', 'SELECT id FROM t WHERE code = 7'])
      assert.deepEqual(await F.query(q), await L.query(q), q);
    assert.deepEqual(await F.query('SELECT code, label, n FROM t WHERE id IN (3, 51) ORDER BY id'), [{code: 3, label: 'n3', n: '0'}, {code: 51, label: 'none', n: 'x'}]);
    await assert.rejects(L.execute("INSERT INTO t (code, label, n) VALUES (1, 'toolong', '1')"), e => e.code === 'INPUT');
    await L.close(); await L2.close(); await F.close();
  } finally {
    if (follower) await follower.stop();
    await leader.stop();
    await rm(dir, {recursive: true, force: true});
  }
});
