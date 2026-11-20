// Window functions over grouped rows, frames with offsets (ROWS, RANGE, GROUPS), named
// windows and FILTER: the same tables in KestroDB and SQLite (node:sqlite), the same queries;
// the results must match (in order when the query has ORDER BY, as sets otherwise).
import test from 'node:test';
import assert from 'node:assert/strict';
import {DatabaseSync} from 'node:sqlite';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {Database} from '../index.mjs';

const schema = [
  'CREATE TABLE sales (id INTEGER PRIMARY KEY, city TEXT, dept INTEGER, amount INTEGER, price REAL, day INTEGER, k)',
  'CREATE TABLE cities (id INTEGER PRIMARY KEY, name TEXT, region TEXT)',
  'CREATE TABLE empty (id INTEGER PRIMARY KEY, city TEXT, amount INTEGER)',
  'CREATE INDEX sales_dept ON sales (dept)',
];
const names = ['Colombo', 'Kandy', 'Galle', 'Jaffna', null];
function rows() {
  const out = [];
  for (let i = 1; i <= 40; i++) {
    const k = i % 11 === 0 ? null : i % 13 === 0 ? 'x' + (i % 3) : (i * 3) % 10;  // numbers, NULLs and text (RANGE over mixed values)
    out.push(['INSERT INTO sales (id, city, dept, amount, price, day, k) VALUES (?, ?, ?, ?, ?, ?, ?)',
      [i, names[i % 5], i % 7 === 0 ? null : (i % 3) + 1, i % 6 === 0 ? null : (i * 7) % 25, ((i * 13) % 40) / 2 + 0.25, i % 9 === 0 ? null : (i * 5) % 12, k]]);
  }
  [['Colombo', 'west'], ['Galle', 'south'], ['Kandy', 'central'], ['Jaffna', 'north'], ['Matara', 'south']].forEach(([n, r], i) => out.push(['INSERT INTO cities (id, name, region) VALUES (?, ?, ?)', [i + 1, n, r]]));
  return out;
}

const QUERIES = [
  // Windows over GROUP BY and aggregates
  ['SELECT city, COUNT(*) AS n, RANK() OVER (ORDER BY COUNT(*) DESC) AS r, SUM(COUNT(*)) OVER () AS total FROM sales GROUP BY city ORDER BY city', []],
  ['SELECT dept, SUM(amount) AS s, DENSE_RANK() OVER (ORDER BY SUM(amount)) AS d, ROW_NUMBER() OVER (ORDER BY SUM(amount) DESC, dept) AS rn FROM sales GROUP BY dept ORDER BY dept', []],
  ['SELECT day, SUM(amount) AS s, SUM(SUM(amount)) OVER (ORDER BY day) AS running FROM sales GROUP BY day ORDER BY day', []],
  ['SELECT day, COUNT(*) AS n, SUM(COUNT(*)) OVER (ORDER BY day ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running, COUNT(*) OVER (ORDER BY day) AS groups FROM sales GROUP BY day ORDER BY day DESC', []],
  ['SELECT city, dept, COUNT(*) AS n, RANK() OVER (PARTITION BY city ORDER BY COUNT(*) DESC, dept) AS r, SUM(COUNT(*)) OVER (PARTITION BY city) AS city_total FROM sales GROUP BY city, dept ORDER BY city, dept', []],
  ['SELECT city, dept, MAX(amount) AS m, FIRST_VALUE(dept) OVER (PARTITION BY city ORDER BY MAX(amount) DESC, dept) AS top FROM sales GROUP BY city, dept ORDER BY city, dept', []],
  ['SELECT city, AVG(amount) AS a, ROW_NUMBER() OVER (ORDER BY AVG(amount) DESC) AS rn FROM sales GROUP BY city HAVING COUNT(*) > 7 ORDER BY city', []],
  ['SELECT city, SUM(price) AS p, RANK() OVER (ORDER BY SUM(price)) AS r FROM sales GROUP BY city HAVING SUM(price) > 50 AND city IS NOT NULL ORDER BY r', []],
  ['SELECT COUNT(*) AS n, SUM(COUNT(*)) OVER () AS t, RANK() OVER (ORDER BY COUNT(*)) AS r FROM sales', []],
  ['SELECT RANK() OVER (ORDER BY COUNT(*)) AS r FROM sales', []],
  ['SELECT COUNT(*) OVER (ORDER BY COUNT(*)) AS c FROM sales', []],
  ['SELECT city, MAX(price) AS m, RANK() OVER (ORDER BY MAX(price) DESC) AS r FROM sales GROUP BY city ORDER BY r, city', []],
  ['SELECT city, MAX(price) AS m, RANK() OVER (ORDER BY MAX(price) DESC) AS r FROM sales GROUP BY city ORDER BY 3 DESC, 1 LIMIT 3 OFFSET 1', []],
  ['SELECT day, COUNT(*) AS n, LAG(COUNT(*)) OVER (ORDER BY day) AS prev, LEAD(SUM(amount), 1, 0) OVER (ORDER BY day) AS next FROM sales GROUP BY day ORDER BY day', []],
  ['SELECT dept, MIN(amount) AS lo, FIRST_VALUE(dept) OVER (ORDER BY MIN(amount), dept) AS f, LAST_VALUE(dept) OVER (ORDER BY MIN(amount), dept ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS l FROM sales GROUP BY dept ORDER BY dept', []],
  ['SELECT day, SUM(amount) AS s, AVG(SUM(amount)) OVER (ORDER BY day ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS ma FROM sales GROUP BY day ORDER BY day', []],
  ['SELECT day, MAX(amount) AS m, MIN(MAX(amount)) OVER (ORDER BY day ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS lo, MAX(MIN(amount)) OVER (ORDER BY day RANGE BETWEEN 2 PRECEDING AND 2 FOLLOWING) AS hi FROM sales GROUP BY day ORDER BY day', []],
  ['SELECT city, COUNT(*) * 100.0 / SUM(COUNT(*)) OVER () AS pct, CUME_DIST() OVER (ORDER BY COUNT(*)) AS cd, PERCENT_RANK() OVER (ORDER BY COUNT(*)) AS pr, NTILE(2) OVER (ORDER BY city) AS half FROM sales GROUP BY city ORDER BY city', []],
  ['SELECT city, COUNT(*) AS n, SUM(COUNT(*)) OVER (ORDER BY city ROWS BETWEEN ? PRECEDING AND CURRENT ROW) AS w FROM sales WHERE amount > ? GROUP BY city ORDER BY city', [1, 10]],
  ['SELECT dept, COUNT(*) AS n, SUM(COUNT(*)) OVER (ORDER BY dept) AS run FROM sales WHERE dept IN (1, 2) GROUP BY dept ORDER BY dept', []],
  ['SELECT city, SUM(amount) AS s, RANK() OVER (ORDER BY SUM(amount) DESC) AS r FROM sales WHERE dept = 2 GROUP BY city ORDER BY city', []],
  ['SELECT c.region, COUNT(*) AS n, SUM(COUNT(*)) OVER (ORDER BY c.region) AS run, RANK() OVER (ORDER BY COUNT(*) DESC, c.region) AS r FROM sales s JOIN cities c ON c.name = s.city GROUP BY c.region ORDER BY c.region', []],
  ['SELECT city, COUNT(*) AS n, ROW_NUMBER() OVER (PARTITION BY COUNT(*) > 8 ORDER BY city) AS rn FROM sales GROUP BY city ORDER BY city', []],
  ['SELECT DISTINCT COUNT(*) OVER () AS groups FROM sales GROUP BY day', []],
  ['SELECT x.city, SUM(x.amount) AS s, ROW_NUMBER() OVER (ORDER BY SUM(x.amount) DESC, x.city) AS r FROM sales x GROUP BY x.city ORDER BY x.city', []],
  ['SELECT sales.id, RANK() OVER (PARTITION BY sales.city ORDER BY sales.amount DESC, sales.id) AS r FROM sales ORDER BY sales.id', []],
  ['SELECT x.id, RANK() OVER (PARTITION BY x.city ORDER BY x.amount DESC) AS r FROM sales x ORDER BY x.id', []],
  ['SELECT city, COUNT(*) AS n, RANK() OVER (ORDER BY COUNT(*)) AS r FROM empty GROUP BY city', []],
  ['SELECT COUNT(*) AS n, SUM(COUNT(*)) OVER () AS t, ROW_NUMBER() OVER () AS rn FROM empty', []],
  ['SELECT SUM(amount) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS s FROM empty', []],
  // ROWS frames with offsets, on plain rows
  ['SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 1 FOLLOWING) AS s FROM sales ORDER BY id', []],
  ['SELECT id, COUNT(amount) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS c, COUNT(*) OVER (ORDER BY id ROWS 3 PRECEDING) AS c3 FROM sales ORDER BY id', []],
  ['SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN CURRENT ROW AND 3 FOLLOWING) AS s, AVG(price) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND 2 FOLLOWING) AS a FROM sales ORDER BY id', []],
  ['SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN 3 PRECEDING AND UNBOUNDED FOLLOWING) AS s, MAX(amount) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 1 PRECEDING) AS m FROM sales ORDER BY id', []],
  ['SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN 1 FOLLOWING AND 3 FOLLOWING) AS s, MIN(amount) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING) AS lo, MAX(amount) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING) AS hi FROM sales ORDER BY id', []],
  ['SELECT id, city, AVG(amount) OVER (PARTITION BY city ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS a, MIN(price) OVER (PARTITION BY city ORDER BY id ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING) AS p FROM sales ORDER BY id', []],
  ['SELECT id, SUM(amount) OVER (ORDER BY amount, id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS s FROM sales ORDER BY id', []],
  ['SELECT id, SUM(amount) OVER (ORDER BY id DESC ROWS BETWEEN 2 PRECEDING AND 1 FOLLOWING) AS s FROM sales ORDER BY id', []],
  ['SELECT id, COUNT(*) OVER (PARTITION BY dept ORDER BY id ROWS BETWEEN 5 PRECEDING AND 5 FOLLOWING) AS c FROM sales ORDER BY id', []],
  ['SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN ? PRECEDING AND ? FOLLOWING) AS s FROM sales WHERE id < 20 ORDER BY id', [2, 1]],
  ['SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN 0 PRECEDING AND 0 FOLLOWING) AS s FROM sales ORDER BY id', []],
  // RANGE frames: offsets on one numeric ORDER BY value (NULLs, ties, DESC, REAL offsets)
  ['SELECT id, day, SUM(amount) OVER (ORDER BY day RANGE BETWEEN 2 PRECEDING AND 2 FOLLOWING) AS s, COUNT(*) OVER (ORDER BY day RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) AS c FROM sales ORDER BY id', []],
  ['SELECT id, day, COUNT(*) OVER (ORDER BY day DESC RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) AS c, SUM(amount) OVER (ORDER BY day DESC RANGE BETWEEN CURRENT ROW AND 3 FOLLOWING) AS s FROM sales ORDER BY id', []],
  ['SELECT id, price, COUNT(*) OVER (ORDER BY price RANGE BETWEEN 3.5 PRECEDING AND 0.5 FOLLOWING) AS c, MAX(amount) OVER (ORDER BY price RANGE BETWEEN 2.5 PRECEDING AND 2.5 FOLLOWING) AS m FROM sales ORDER BY id', []],
  ['SELECT id, day, COUNT(*) OVER (ORDER BY day RANGE BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) AS c, COUNT(*) OVER (ORDER BY day RANGE BETWEEN UNBOUNDED PRECEDING AND 3 FOLLOWING) AS c2 FROM sales ORDER BY id', []],
  ['SELECT id, day, COUNT(*) OVER (ORDER BY day RANGE BETWEEN 1 FOLLOWING AND 5 FOLLOWING) AS c, SUM(amount) OVER (ORDER BY day RANGE BETWEEN 3 PRECEDING AND 1 PRECEDING) AS s FROM sales ORDER BY id', []],
  ['SELECT id, day, COUNT(*) OVER (ORDER BY day DESC RANGE BETWEEN 2 FOLLOWING AND UNBOUNDED FOLLOWING) AS c, COUNT(*) OVER (ORDER BY day DESC RANGE BETWEEN UNBOUNDED PRECEDING AND 2 PRECEDING) AS c2 FROM sales ORDER BY id', []],
  ['SELECT id, day, SUM(amount) OVER (ORDER BY day RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS s, SUM(amount) OVER (ORDER BY day RANGE UNBOUNDED PRECEDING) AS s2, SUM(amount) OVER (ORDER BY day RANGE CURRENT ROW) AS peers FROM sales ORDER BY id', []],
  ['SELECT id, city, day, COUNT(*) OVER (PARTITION BY city ORDER BY day RANGE BETWEEN 2 PRECEDING AND 2 FOLLOWING) AS c FROM sales ORDER BY id', []],
  ['SELECT id, COUNT(*) OVER (RANGE BETWEEN CURRENT ROW AND CURRENT ROW) AS c FROM sales ORDER BY id', []],
  ['SELECT id, k, COUNT(*) OVER (ORDER BY k RANGE BETWEEN 2 PRECEDING AND 1 FOLLOWING) AS c, COUNT(*) OVER (ORDER BY k DESC RANGE BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING) AS c2 FROM sales ORDER BY id', []],
  // GROUPS frames
  ['SELECT id, day, COUNT(*) OVER (ORDER BY day GROUPS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS c, SUM(amount) OVER (ORDER BY day GROUPS BETWEEN CURRENT ROW AND 2 FOLLOWING) AS s FROM sales ORDER BY id', []],
  ['SELECT id, day, COUNT(*) OVER (ORDER BY day DESC GROUPS BETWEEN 2 PRECEDING AND 1 PRECEDING) AS c FROM sales ORDER BY id', []],
  // FIRST_VALUE / LAST_VALUE / NTH_VALUE / LAG / LEAD with frames
  ['SELECT id, FIRST_VALUE(amount) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING) AS f, LAST_VALUE(amount) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING) AS l FROM sales ORDER BY id', []],
  ['SELECT id, LAST_VALUE(amount) OVER (ORDER BY id ROWS BETWEEN 1 FOLLOWING AND 2 FOLLOWING) AS l, FIRST_VALUE(amount) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND 1 PRECEDING) AS f FROM sales ORDER BY id', []],
  ['SELECT id, NTH_VALUE(amount, 2) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS n2, NTH_VALUE(price, 3) OVER (PARTITION BY city ORDER BY id) AS n3 FROM sales ORDER BY id', []],
  ['SELECT id, day, FIRST_VALUE(id) OVER (ORDER BY day RANGE BETWEEN 2 PRECEDING AND 2 FOLLOWING) AS f, LAST_VALUE(id) OVER (ORDER BY day, id RANGE BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) AS l FROM sales ORDER BY id', []],
  ['SELECT id, LAG(amount, 2) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS p, LEAD(amount) OVER (PARTITION BY city ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS n FROM sales ORDER BY id', []],
  ['SELECT id, city, LAST_VALUE(price) OVER (PARTITION BY city ORDER BY day, id GROUPS BETWEEN CURRENT ROW AND 1 FOLLOWING) AS l FROM sales ORDER BY id', []],
  // FILTER
  ['SELECT city, COUNT(*) FILTER (WHERE amount > 10) AS big, SUM(amount) FILTER (WHERE dept = 1) AS d1, AVG(price) FILTER (WHERE day IS NULL) AS a FROM sales GROUP BY city ORDER BY city', []],
  ['SELECT COUNT(*) FILTER (WHERE amount IS NULL) AS n, COUNT(*) AS all_rows, MAX(amount) FILTER (WHERE city = ?) AS m FROM sales', ['Kandy']],
  ['SELECT dept, COUNT(*) FILTER (WHERE amount > 10) AS big FROM sales WHERE dept = 3 GROUP BY dept', []],
  ['SELECT id, SUM(amount) FILTER (WHERE dept = 2) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS s, COUNT(*) FILTER (WHERE amount > 10) OVER (PARTITION BY city) AS c FROM sales ORDER BY id', []],
  ['SELECT city, COUNT(*) FILTER (WHERE amount > 10) AS big, RANK() OVER (ORDER BY COUNT(*) FILTER (WHERE amount > 10) DESC, city) AS r FROM sales GROUP BY city ORDER BY city', []],
  // Named windows
  ['SELECT id, ROW_NUMBER() OVER w AS rn, SUM(amount) OVER w AS s FROM sales WINDOW w AS (PARTITION BY city ORDER BY id) ORDER BY id', []],
  ['SELECT id, SUM(amount) OVER (w ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS s, COUNT(*) OVER w AS c FROM sales WINDOW w AS (ORDER BY id) ORDER BY id', []],
  ['SELECT id, RANK() OVER b AS r, SUM(amount) OVER (a ORDER BY id DESC) AS s FROM sales s WINDOW a AS (PARTITION BY city), b AS (a ORDER BY day) ORDER BY id', []],
  ['SELECT city, COUNT(*) AS n, RANK() OVER w AS r, SUM(COUNT(*)) OVER w AS run FROM sales GROUP BY city WINDOW w AS (ORDER BY COUNT(*) DESC, city) ORDER BY city', []],
  ['SELECT id, ROW_NUMBER() OVER w AS rn FROM sales WHERE id < 10 WINDOW w AS (ORDER BY id DESC) ORDER BY ROW_NUMBER() OVER w', []],
];
// Statements both must refuse.
const ERRORS = [
  'SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN -1 PRECEDING AND CURRENT ROW) FROM sales',
  'SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN 1.5 PRECEDING AND CURRENT ROW) FROM sales',
  'SELECT id, SUM(amount) OVER (ORDER BY day, id RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM sales',
  'SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN CURRENT ROW AND 1 PRECEDING) FROM sales',
  'SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED FOLLOWING AND CURRENT ROW) FROM sales',
  'SELECT id, SUM(amount) OVER (ORDER BY id ROWS BETWEEN 1 FOLLOWING AND CURRENT ROW) FROM sales',
  'SELECT id, SUM(amount) OVER w FROM sales',
  'SELECT id, SUM(amount) OVER (w PARTITION BY city) FROM sales WINDOW w AS (ORDER BY id)',
  'SELECT id, SUM(amount) OVER (w ORDER BY day) FROM sales WINDOW w AS (ORDER BY id)',
  'SELECT id, SUM(amount) OVER (w) FROM sales WINDOW w AS (ORDER BY id ROWS 1 PRECEDING)',
  'SELECT id, NTH_VALUE(amount, 0) OVER (ORDER BY id) FROM sales',
  'SELECT id, COUNT(DISTINCT amount) OVER (ORDER BY id) FROM sales',
  'SELECT id, ROW_NUMBER() FILTER (WHERE amount > 1) OVER (ORDER BY id) FROM sales',
];

const normalize = (rows, ordered) => {
  const vals = rows.map(r => JSON.stringify(Object.values(r).map(v => typeof v === 'number' ? Math.round(v * 1e9) / 1e9 : v)));
  return ordered ? vals : vals.sort();
};

test('window functions over groups, frames, named windows and FILTER match SQLite', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-windowgroup-'));
  try {
    const h = new Database(join(dir, 'h.kdb'), {create: true});
    const s = new DatabaseSync(':memory:');
    for (const q of schema) { h.sql(q); s.exec(q); }
    for (const [q, p] of rows()) { h.sql(q, p); s.prepare(q).run(...p); }
    const failures = [];
    for (const [q, p] of QUERIES) {
      let want, got;
      try { want = s.prepare(q).all(...p); } catch (e) { failures.push({q, sqlite: e.message}); continue; }
      try { got = h.query(q, p); } catch (e) { failures.push({q, kestro: e.message}); continue; }
      const ordered = /ORDER BY/i.test(q.replace(/OVER\s*\([^)]*\)|AS\s*\([^)]*\)/gi, ''));
      const a = normalize(got, ordered), b = normalize(want, ordered);
      if (JSON.stringify(a) !== JSON.stringify(b)) failures.push({q, got: a, want: b});
    }
    for (const q of ERRORS) {
      assert.throws(() => s.prepare(q).all(), undefined, 'SQLite accepts ' + q);
      try { h.query(q); failures.push({q, kestro: 'accepted'}); } catch (e) { if (!/INPUT SQL/.test(e.message)) failures.push({q, kestro: e.message}); }
    }
    assert.deepEqual(failures, []);
    // The same answers from documents instead of the column cache: a transaction that wrote to
    // the table reads its own rows.
    h.sql('BEGIN');
    h.sql('UPDATE sales SET amount = amount WHERE id = 1');
    for (const [q, p] of QUERIES.slice(0, 20)) assert.deepEqual(normalize(h.query(q, p), true), normalize(s.prepare(q).all(...p), true), q);
    h.sql('ROLLBACK');
    h.close(); s.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});
