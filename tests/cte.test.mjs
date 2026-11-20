// WITH (common table expressions, recursive ones too), derived tables (FROM (SELECT ...))
// and SELECT without FROM: the same queries in KestroDB and SQLite (node:sqlite) must give
// the same results (as sets when the query has no ORDER BY); errors and table limits.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {DatabaseSync} from 'node:sqlite';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Database} from '../index.mjs';
import {RemoteDB} from '../remote.mjs';

const schema = [
  'CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, city TEXT, age INTEGER, dept INTEGER)',
  'CREATE TABLE depts (id INTEGER PRIMARY KEY, title TEXT)',
  'CREATE TABLE orders (id INTEGER PRIMARY KEY, user INTEGER, total REAL, status TEXT)',
  'CREATE TABLE tree (id INTEGER PRIMARY KEY, parent INTEGER, name TEXT)',
  'CREATE TABLE edges (id INTEGER PRIMARY KEY, src INTEGER, dst INTEGER)',
];
const cities = ['Colombo', 'Kandy', 'Galle', null];
function rows() {
  const out = [];
  for (let i = 1; i <= 40; i++) out.push(['INSERT INTO users (id, name, city, age, dept) VALUES (?, ?, ?, ?, ?)', [i, 'user' + i, cities[i % 4], 18 + (i * 7) % 50, i % 5 === 0 ? null : (i % 3) + 1]]);
  for (let i = 1; i <= 4; i++) out.push(['INSERT INTO depts (id, title) VALUES (?, ?)', [i, 'dept' + i]]);
  for (let i = 1; i <= 60; i++) out.push(['INSERT INTO orders (id, user, total, status) VALUES (?, ?, ?, ?)', [i, (i * 3) % 45, (i * 13) % 100 + 0.5, i % 4 ? 'paid' : 'open']]);
  // A tree of 31 nodes: node i's parent is i / 2 (rounded down), the root (1) has none.
  for (let i = 1; i <= 31; i++) out.push(['INSERT INTO tree (id, parent, name) VALUES (?, ?, ?)', [i, i === 1 ? null : Math.floor(i / 2), 'n' + i]]);
  // A graph with cycles: 1 -> 2 -> 3 -> 1, 3 -> 4 -> 5 -> 4, and 6 -> 7 apart.
  [[1, 2], [2, 3], [3, 1], [3, 4], [4, 5], [5, 4], [6, 7]].forEach(([s, d], i) => out.push(['INSERT INTO edges (id, src, dst) VALUES (?, ?, ?)', [i + 1, s, d]]));
  return out;
}
export const QUERIES = [
  // One WITH table, several, one using another (also defined after it), column lists
  ['WITH paid AS (SELECT * FROM orders WHERE status = ?) SELECT id, total FROM paid ORDER BY id', ['paid']],
  ['WITH a AS (SELECT id, age FROM users WHERE age > 30), b AS (SELECT id FROM a WHERE age < 50) SELECT * FROM b ORDER BY id', []],
  ['WITH b AS (SELECT id FROM a WHERE id % 2 = 0), a AS (SELECT id FROM users WHERE city = ?) SELECT * FROM b ORDER BY id', ['Kandy']],
  ['WITH c(uid, n) AS (SELECT user, COUNT(*) FROM orders GROUP BY user) SELECT uid, n FROM c WHERE n > 1 ORDER BY uid', []],
  ['WITH x(a, b) AS (SELECT id, name FROM users WHERE id < 4) SELECT * FROM x', []],
  ['WITH users AS (SELECT id, title AS name FROM depts) SELECT name FROM users ORDER BY id', []],
  // Used twice, joined with tables, LEFT JOIN
  ['WITH s AS (SELECT user, SUM(total) AS t FROM orders GROUP BY user) SELECT a.user, a.t, b.t AS next FROM s a JOIN s b ON b.user = a.user + 1 ORDER BY a.user', []],
  ['WITH big AS (SELECT user, MAX(total) AS top FROM orders GROUP BY user) SELECT u.name, big.top FROM users u JOIN big ON big.user = u.id WHERE big.top > ? ORDER BY u.id', [50]],
  ['WITH n AS (SELECT user, COUNT(*) AS k FROM orders WHERE status = ? GROUP BY user) SELECT u.id, n.k FROM users u LEFT JOIN n ON n.user = u.id ORDER BY u.id', ['open']],
  ['WITH d AS (SELECT * FROM depts WHERE id > 1) SELECT u.id, d.title FROM d JOIN users u ON u.dept = d.id WHERE u.age > 40 ORDER BY u.id', []],
  // Aggregates, ORDER BY, LIMIT, DISTINCT, windows over WITH tables
  ['WITH x AS (SELECT city, age FROM users) SELECT city, COUNT(*) AS n, AVG(age) AS a, MIN(age) AS lo FROM x GROUP BY city ORDER BY city', []],
  ['WITH x AS (SELECT id, total FROM orders) SELECT * FROM x ORDER BY total DESC, id LIMIT 5 OFFSET 3', []],
  ['WITH x AS (SELECT status FROM orders) SELECT DISTINCT status FROM x', []],
  ['WITH x AS (SELECT * FROM users WHERE dept IS NULL) SELECT COUNT(*) AS n FROM x', []],
  ['WITH x AS (SELECT city, age FROM users) SELECT city, COUNT(*) AS n FROM x WHERE age > 30 GROUP BY city HAVING COUNT(*) > 2 ORDER BY n DESC, city', []],
  ['WITH x AS (SELECT id, city, age FROM users) SELECT id, RANK() OVER (PARTITION BY city ORDER BY age) AS r FROM x ORDER BY id', []],
  ['WITH top AS (SELECT id, total FROM orders ORDER BY total DESC, id LIMIT 3) SELECT id FROM top ORDER BY id', []],
  // In subqueries, compound bodies and compound statements
  ['WITH k AS (SELECT id FROM users WHERE city = ?) SELECT id, user FROM orders WHERE user IN (SELECT id FROM k) ORDER BY id', ['Galle']],
  ['WITH o AS (SELECT user, total FROM orders) SELECT u.id, (SELECT COUNT(*) FROM o WHERE o.user = u.id) AS n FROM users u ORDER BY u.id', []],
  ['WITH o AS (SELECT user FROM orders) SELECT id FROM users u WHERE NOT EXISTS (SELECT 1 FROM o WHERE o.user = u.id) ORDER BY id', []],
  ['WITH names AS (SELECT name AS v FROM users WHERE id < 5 UNION SELECT title FROM depts) SELECT v FROM names ORDER BY v', []],
  ['WITH a AS (SELECT id FROM users WHERE age < 30) SELECT id FROM a UNION SELECT id FROM depts ORDER BY id', []],
  // Derived tables
  ['SELECT d.user, d.n FROM (SELECT user, COUNT(*) AS n FROM orders GROUP BY user) AS d WHERE d.n >= 2 ORDER BY d.user', []],
  ['SELECT u.name, s.spent FROM users u JOIN (SELECT user, SUM(total) AS spent FROM orders GROUP BY user) s ON s.user = u.id ORDER BY s.spent DESC, u.id LIMIT 10', []],
  ['SELECT COUNT(*) AS n FROM (SELECT DISTINCT city FROM users)', []],
  ['SELECT MAX(n) AS m FROM (SELECT city, COUNT(*) AS n FROM (SELECT * FROM users WHERE age > 25) GROUP BY city)', []],
  ['SELECT * FROM (WITH a AS (SELECT id FROM depts) SELECT id FROM a WHERE id > 1) x ORDER BY id', []],
  ['SELECT a.city, a.n, b.n AS m FROM (SELECT city, COUNT(*) AS n FROM users GROUP BY city) a JOIN (SELECT city, MAX(age) AS n FROM users GROUP BY city) b ON a.city = b.city ORDER BY a.city', []],
  ['WITH paid AS (SELECT * FROM orders WHERE status = ?) SELECT u.city, SUM(p.total) AS s FROM (SELECT id, city FROM users) u JOIN paid p ON p.user = u.id GROUP BY u.city ORDER BY u.city', ['paid']],
  ['SELECT u.id, x.title FROM users u LEFT JOIN (SELECT id, title FROM depts WHERE id <> 2) x ON x.id = u.dept WHERE u.id < 12 ORDER BY u.id', []],
  ['SELECT x.* FROM (SELECT id, name FROM users ORDER BY age DESC, id LIMIT 4) x', []],
  // SELECT without FROM
  ['SELECT 1 AS one, 2 + 3 AS five', []],
  ['SELECT ? * 2 AS d', [21]],
  ['SELECT id FROM depts UNION ALL SELECT 99 ORDER BY id', []],
  // Recursive: counting (UNION ALL, LIMIT and OFFSET in the body, without the RECURSIVE keyword)
  ['WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 100) SELECT COUNT(*) AS k, SUM(n) AS s, MAX(n) AS m FROM c', []],
  ['WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n * 2 FROM c LIMIT 12) SELECT n FROM c ORDER BY n', []],
  ['WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c LIMIT 5 OFFSET 2) SELECT n FROM c ORDER BY n', []],
  ['WITH c(n) AS (SELECT 5 UNION ALL SELECT n - 1 FROM c WHERE n > 0) SELECT n FROM c ORDER BY n', []],
  ['WITH RECURSIVE fib(i, a, b) AS (SELECT 1, 0, 1 UNION ALL SELECT i + 1, b, a + b FROM fib WHERE i < 30) SELECT i, a FROM fib ORDER BY i', []],
  ['WITH RECURSIVE c(n) AS (SELECT 1 UNION SELECT n % 5 + 1 FROM c) SELECT n FROM c ORDER BY n', []],
  ['WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 3 UNION ALL SELECT n + 10 FROM c WHERE n < 3) SELECT n FROM c ORDER BY n', []],
  ['WITH RECURSIVE c(n, m) AS (SELECT 1, 1 UNION ALL SELECT 2, 2 UNION ALL SELECT n + 2, m * 3 FROM c WHERE n < 8) SELECT n, m FROM c ORDER BY n', []],
  // Recursive: tree walks (down with depth and path, up to the root), graphs with cycles (UNION)
  ["WITH RECURSIVE sub(id, name, depth, path) AS (SELECT id, name, 0, name FROM tree WHERE parent IS NULL UNION ALL SELECT t.id, t.name, sub.depth + 1, sub.path || '/' || t.name FROM tree t JOIN sub ON t.parent = sub.id) SELECT id, depth, path FROM sub ORDER BY path", []],
  ['WITH RECURSIVE up(id, parent) AS (SELECT id, parent FROM tree WHERE id = ? UNION ALL SELECT t.id, t.parent FROM tree t JOIN up ON t.id = up.parent) SELECT id FROM up ORDER BY id', [27]],
  ['WITH RECURSIVE sub(id) AS (SELECT id FROM tree WHERE id = 2 UNION ALL SELECT t.id FROM tree t JOIN sub ON t.parent = sub.id) SELECT COUNT(*) AS n, SUM(id) AS s FROM sub', []],
  ['WITH RECURSIVE sub(id, d) AS (SELECT 1, 0 UNION ALL SELECT t.id, sub.d + 1 FROM sub JOIN tree t ON t.parent = sub.id) SELECT d, COUNT(*) AS n FROM sub GROUP BY d ORDER BY d', []],
  ['WITH RECURSIVE reach(n) AS (SELECT ? UNION SELECT e.dst FROM edges e JOIN reach ON e.src = reach.n) SELECT n FROM reach ORDER BY n', [1]],
  ['WITH RECURSIVE reach(n) AS (SELECT ? UNION SELECT e.dst FROM reach JOIN edges e ON e.src = reach.n) SELECT n FROM reach ORDER BY n', [6]],
  ['WITH RECURSIVE sub(id) AS (SELECT id FROM tree WHERE id = 3 UNION ALL SELECT t.id FROM tree t JOIN sub ON t.parent = sub.id) SELECT t.name FROM tree t WHERE t.id IN (SELECT id FROM sub) AND t.id > 10 ORDER BY t.id', []],
  // Recursive and plain WITH tables together
  ['WITH RECURSIVE days(d) AS (SELECT 0 UNION ALL SELECT d + 1 FROM days WHERE d < 6), per AS (SELECT id % 7 AS d, COUNT(*) AS n FROM orders WHERE id < 20 GROUP BY id % 7) SELECT days.d, per.n FROM days LEFT JOIN per ON per.d = days.d ORDER BY days.d', []],
  ['WITH RECURSIVE ids(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM ids WHERE n < 45) SELECT ids.n, COUNT(o.id) AS k FROM ids LEFT JOIN orders o ON o.user = ids.n GROUP BY ids.n ORDER BY ids.n', []],
];

const normalize = (rows, ordered) => {
  const vals = rows.map(r => JSON.stringify(Object.values(r).map(v => typeof v === 'number' ? Math.round(v * 1e9) / 1e9 : v)));
  return ordered ? vals : vals.sort();
};

async function withDatabases(fn) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-cte-'));
  try {
    const h = new Database(join(dir, 'h.kdb'), {create: true});
    const s = new DatabaseSync(':memory:');
    for (const q of schema) { h.sql(q); s.exec(q); }
    for (const [q, p] of rows()) { h.sql(q, p); s.prepare(q).run(...p); }
    try { await fn(h, s); } finally { h.close(); s.close(); }
  } finally { await rm(dir, {recursive: true, force: true}); }
}

test('WITH, recursive WITH and derived tables match SQLite', async () => {
  await withDatabases((h, s) => {
    for (const [q, p] of QUERIES) {
      const want = s.prepare(q).all(...p), got = h.query(q, p);
      const ordered = /ORDER BY/i.test(q.replace(/\(.*\)/s, m => m.replace(/ORDER BY/gi, '')));  // an outer ORDER BY
      assert.deepEqual(normalize(got, ordered), normalize(want, ordered), q);
    }
  });
});

test('errors: runaway recursion, circular references, malformed WITH', async () => {
  await withDatabases(h => {
    const t0 = Date.now();
    assert.throws(() => h.query('WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c) SELECT COUNT(*) FROM c'), /LIMIT .*1,000,000 rows/);
    assert.ok(Date.now() - t0 < 60000);
    // A runaway that doubles each round stops at the same limit.
    assert.throws(() => h.query('WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n FROM c JOIN depts d ON d.id < 3) SELECT COUNT(*) FROM c'), /LIMIT .*1,000,000 rows/);
    // A LIMIT on the body ends a recursion that has no WHERE.
    assert.equal(h.queryValue('WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c LIMIT 1000) SELECT MAX(n) FROM c'), 1000);
    for (const [q, msg] of [
      ['WITH users AS (SELECT * FROM users) SELECT * FROM users', /circular reference/],
      ['WITH a AS (SELECT * FROM b), b AS (SELECT * FROM a) SELECT * FROM a', /circular reference/],
      ['WITH c(a, b) AS (SELECT id FROM users) SELECT * FROM c', /names 2 columns but its SELECT returns 1/],
      ['WITH a AS (SELECT 1 AS x), a AS (SELECT 2 AS x) SELECT * FROM a', /defines a twice/],
      ['WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 5 ORDER BY n) SELECT * FROM c', /ORDER BY/],
      ['WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT id FROM users WHERE id IN (SELECT n FROM c)) SELECT * FROM c', /once, in its FROM/],
      ['WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT COUNT(*) FROM c) SELECT * FROM c', /aggregates/],
      ['WITH RECURSIVE c(n) AS (SELECT 1 INTERSECT SELECT n FROM c) SELECT * FROM c', /UNION/],
      ['WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n, n FROM c) SELECT * FROM c', /columns/],
      ['SELECT * WHERE 1 = 1', /needs FROM/],
      ['SELECT * FROM (SELECT id FROM nosuch) x', /no such table: nosuch/],
    ]) assert.throws(() => h.query(q), msg, q);
  });
});

test('WITH reads the transaction\'s own changes', async () => {
  await withDatabases(h => {
    h.sql('BEGIN');
    h.sql("INSERT INTO tree (id, parent, name) VALUES (100, 31, 'leaf')");
    const q = 'WITH RECURSIVE up(id, parent) AS (SELECT id, parent FROM tree WHERE id = 100 UNION ALL SELECT t.id, t.parent FROM tree t JOIN up ON t.id = up.parent) SELECT COUNT(*) AS n FROM (SELECT * FROM up)';
    assert.equal(h.queryValue(q), 6);
    h.sql('ROLLBACK');
    assert.equal(h.queryValue(q), 0);
  });
});

// A table-limited server user: tables read only inside WITH bodies and derived tables need
// access too; WITH names are not tables.
const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const tokens = {admin: 'admin-token-0123456789abcdef0123', scopedro: 'scopedro-token-0123456789abcdef0'};
test('table limits cover tables read inside WITH and derived tables', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-cte-sec-'));
  await writeFile(join(dir, 'users'), ['admin admin ' + tokens.admin, 'scopedro readonly ' + tokens.scopedro + ' tables=orders', ''].join('\n'));
  const child = spawn(serverBinary, ['--db', join(dir, 's.kdb'), '--create', '--stop-on-stdin-eof', '--users', join(dir, 'users'), '--tcp', '127.0.0.1:0'], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const exited = new Promise(r => child.on('exit', r));
  try {
    const address = await new Promise((resolve, reject) => {
      child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
      child.on('exit', code => reject(Error(`server exited ${code}: ${err}`)));
    });
    const admin = await RemoteDB.connect(address, {token: tokens.admin, reconnect: false});
    await admin.sql('CREATE TABLE orders (id INTEGER PRIMARY KEY, item TEXT, qty INTEGER)');
    await admin.sql('CREATE TABLE secrets (id INTEGER PRIMARY KEY, secret TEXT)');
    await admin.sql("INSERT INTO orders (id, item, qty) VALUES (1, 'pen', 1), (2, 'ink', 2)");
    await admin.sql("INSERT INTO secrets (id, secret) VALUES (1, 'TOP-SECRET-1')");
    const ro = await RemoteDB.connect(address, {token: tokens.scopedro, reconnect: false});
    for (const q of [
      'WITH s AS (SELECT * FROM secrets) SELECT id FROM orders',
      'WITH s AS (SELECT * FROM secrets) SELECT o.id, s.secret FROM orders o JOIN s ON s.id = o.id',
      'SELECT * FROM (SELECT secret FROM secrets) x',
      'SELECT o.id, x.secret FROM orders o JOIN (SELECT id, secret FROM secrets) x ON x.id = o.id',
      'WITH a AS (SELECT * FROM b), b AS (SELECT * FROM secrets) SELECT * FROM a',
      'WITH o AS (SELECT id FROM orders) SELECT * FROM o WHERE id IN (SELECT id FROM (SELECT id FROM secrets) z)',
      'WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < (SELECT COUNT(*) FROM secrets)) SELECT * FROM r',
      'WITH RECURSIVE r(id, s) AS (SELECT id, item FROM orders UNION ALL SELECT x.id, x.secret FROM secrets x JOIN r ON r.id = x.id) SELECT * FROM r',
      'SELECT * FROM (WITH q AS (SELECT * FROM secrets) SELECT * FROM q) w',
    ]) await assert.rejects(ro.sql(q), /AUTH .*no access to secrets/, q);
    // WITH names are not tables: no access needed, even one named like a table.
    assert.deepEqual((await ro.sql('WITH o AS (SELECT id, item FROM orders) SELECT item FROM o ORDER BY id')).rows, [{item: 'pen'}, {item: 'ink'}]);
    assert.deepEqual((await ro.sql('WITH secrets AS (SELECT id, item FROM orders) SELECT item FROM secrets ORDER BY id')).rows, [{item: 'pen'}, {item: 'ink'}]);
    assert.deepEqual((await ro.sql('SELECT COUNT(*) AS n FROM (SELECT * FROM orders) x')).rows, [{n: 2}]);
    // A WITH table named like a table and reading that name reads itself: an error, never the table.
    await assert.rejects(ro.sql('WITH secrets AS (SELECT * FROM secrets) SELECT * FROM secrets'), /circular reference/);
    const rec = await ro.sql('WITH secrets AS (SELECT id, item FROM orders UNION SELECT id, secret FROM secrets WHERE id > 1) SELECT * FROM secrets');
    assert.deepEqual(rec.rows, [{id: 1, item: 'pen'}, {id: 2, item: 'ink'}, {id: 2, item: null}]);  // recursive: "secrets" is the WITH table, never the stored one
    await ro.close(); await admin.close();
  } finally { child.stdin.end(); await exited; await rm(dir, {recursive: true, force: true}); }
});
