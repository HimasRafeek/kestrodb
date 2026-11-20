// Differential SQL test: the same tables in KestroDB and SQLite (node:sqlite), the same
// queries; the results must match (as sets when the query has no ORDER BY).
import test from 'node:test';
import assert from 'node:assert/strict';
import {DatabaseSync} from 'node:sqlite';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {Database} from '../index.mjs';

const schema = [
  'CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, city TEXT, age INTEGER, dept INTEGER)',
  'CREATE TABLE depts (id INTEGER PRIMARY KEY, title TEXT)',
  'CREATE TABLE orders (id INTEGER PRIMARY KEY, user INTEGER, total REAL, status TEXT)',
];
const cities = ['Colombo', 'Kandy', 'Galle', null];
function rows() {
  const out = [];
  for (let i = 1; i <= 40; i++) out.push(['INSERT INTO users (id, name, city, age, dept) VALUES (?, ?, ?, ?, ?)', [i, 'user' + i, cities[i % 4], 18 + (i * 7) % 50, i % 5 === 0 ? null : (i % 3) + 1]]);
  for (let i = 1; i <= 4; i++) out.push(['INSERT INTO depts (id, title) VALUES (?, ?)', [i, 'dept' + i]]);
  for (let i = 1; i <= 60; i++) out.push(['INSERT INTO orders (id, user, total, status) VALUES (?, ?, ?, ?)', [i, (i * 3) % 45, (i * 13) % 100 + 0.5, i % 4 ? 'paid' : 'open']]);
  return out;
}
// Queries: [sql, params]. Results compared as sets unless the SQL has ORDER BY.
export const QUERIES = [
  // UNION / INTERSECT / EXCEPT
  ['SELECT city FROM users UNION SELECT title FROM depts', []],
  ['SELECT city FROM users UNION ALL SELECT title FROM depts', []],
  ['SELECT age FROM users WHERE age < 30 UNION SELECT age FROM users WHERE age > 60 ORDER BY age', []],
  ['SELECT age FROM users WHERE age < 40 INTERSECT SELECT age FROM users WHERE age > 25 ORDER BY 1 DESC', []],
  ['SELECT user FROM orders EXCEPT SELECT id FROM users WHERE city = ? ORDER BY user', ['Kandy']],
  ['SELECT id, name FROM users WHERE id < 5 UNION SELECT id, title FROM depts ORDER BY id, name LIMIT 5 OFFSET 2', []],
  ['SELECT status FROM orders UNION SELECT status FROM orders', []],
  ['SELECT age FROM users UNION ALL SELECT age FROM users EXCEPT SELECT age FROM users WHERE age > 40', []],
  // RIGHT / FULL joins, USING
  ['SELECT u.name, d.title FROM users u RIGHT JOIN depts d ON u.dept = d.id WHERE u.age > 50 OR u.id IS NULL', []],
  ['SELECT d.title, COUNT(u.id) AS n FROM users u RIGHT JOIN depts d ON u.dept = d.id GROUP BY d.title ORDER BY d.title', []],
  ['SELECT u.id, d.id AS did FROM users u FULL JOIN depts d ON u.dept = d.id AND u.age > 40', []],
  ['SELECT u.id, o.id AS oid FROM users u FULL OUTER JOIN orders o ON o.user = u.id WHERE u.id IS NULL OR o.id IS NULL', []],
  ['SELECT u.name, o.total FROM users u LEFT JOIN orders o ON o.user = u.id WHERE u.city = ? ORDER BY u.id, o.id', ['Galle']],
  ['SELECT a.id, b.id AS bid FROM users a JOIN users b USING (age) WHERE a.id < b.id ORDER BY a.id, b.id', []],
  ['SELECT u.id, o.status FROM users u RIGHT JOIN orders o ON o.user = u.id WHERE o.status = ? ORDER BY o.id', ['open']],
  // Window functions
  ['SELECT id, ROW_NUMBER() OVER (ORDER BY age DESC, id) AS rn FROM users ORDER BY id', []],
  ['SELECT id, city, RANK() OVER (PARTITION BY city ORDER BY age) AS r, DENSE_RANK() OVER (PARTITION BY city ORDER BY age) AS d FROM users ORDER BY id', []],
  ['SELECT id, SUM(total) OVER (ORDER BY id) AS running FROM orders ORDER BY id', []],
  ['SELECT id, user, SUM(total) OVER (PARTITION BY user) AS per_user, COUNT(*) OVER (PARTITION BY user) AS n FROM orders ORDER BY id', []],
  ['SELECT id, LAG(total) OVER (ORDER BY id) AS prev, LEAD(total, 2, 0) OVER (ORDER BY id) AS next2 FROM orders ORDER BY id', []],
  ['SELECT id, AVG(age) OVER (PARTITION BY dept ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS a FROM users ORDER BY id', []],
  ['SELECT id, FIRST_VALUE(name) OVER (PARTITION BY city ORDER BY age, id) AS f, LAST_VALUE(name) OVER (PARTITION BY city ORDER BY age, id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS l FROM users ORDER BY id', []],
  ['SELECT id, NTILE(4) OVER (ORDER BY id) AS q, MIN(age) OVER (PARTITION BY city) AS lo, MAX(age) OVER (PARTITION BY city ORDER BY id) AS hi FROM users ORDER BY id', []],
  ['SELECT id, age, RANK() OVER (ORDER BY age) AS r FROM users WHERE city = ? ORDER BY r, id LIMIT 5', ['Kandy']],
  ['SELECT u.id, SUM(o.total) OVER (PARTITION BY u.id) AS spent FROM users u JOIN orders o ON o.user = u.id ORDER BY u.id, o.id', []],
  // CASE
  ["SELECT id, CASE WHEN age < 30 THEN 'young' WHEN age < 50 THEN 'mid' ELSE 'old' END AS band FROM users ORDER BY id", []],
  ["SELECT id, CASE city WHEN 'Kandy' THEN 1 WHEN ? THEN 2 END AS c FROM users ORDER BY id", ['Galle']],
  ['SELECT city, SUM(CASE WHEN status = ? THEN total ELSE 0 END) AS paid, COUNT(CASE WHEN status = ? THEN 1 END) AS open FROM orders o JOIN users u ON u.id = o.user GROUP BY city ORDER BY city', ['paid', 'open']],
  ["SELECT id FROM users WHERE CASE WHEN dept IS NULL THEN 0 ELSE dept END = 2 ORDER BY CASE WHEN age > 40 THEN 0 ELSE 1 END, id", []],
  ['SELECT id, CASE dept WHEN NULL THEN 9 ELSE dept END AS d FROM users ORDER BY id', []],
  // Correlated subqueries
  ['SELECT u.id FROM users u WHERE EXISTS (SELECT 1 FROM orders o WHERE o.user = u.id AND o.status = ?) ORDER BY u.id', ['open']],
  ['SELECT id FROM users WHERE NOT EXISTS (SELECT 1 FROM orders WHERE orders.user = users.id) ORDER BY id', []],
  ['SELECT u.id, (SELECT COUNT(*) FROM orders o WHERE o.user = u.id) AS n, (SELECT MAX(total) FROM orders o WHERE o.user = u.id) AS top FROM users u ORDER BY u.id', []],
  ['SELECT u.id, u.age FROM users u WHERE u.age > (SELECT AVG(v.age) FROM users v WHERE v.city = u.city) ORDER BY u.id', []],
  ['SELECT o.id FROM orders o WHERE o.total = (SELECT MAX(p.total) FROM orders p WHERE p.user = o.user) ORDER BY o.id', []],
  ['SELECT u.id FROM users u WHERE u.dept IN (SELECT d.id FROM depts d WHERE d.id <> u.id % 3) ORDER BY u.id', []],
  ['SELECT u.name, d.title, (SELECT COUNT(*) FROM orders o WHERE o.user = u.id AND o.total > ?) AS big FROM users u JOIN depts d ON d.id = u.dept ORDER BY u.id', [50]],
  ['SELECT u.city, COUNT(*) AS n FROM users u WHERE EXISTS (SELECT 1 FROM orders o WHERE o.user = u.id) GROUP BY u.city ORDER BY u.city', []],
  ['SELECT u.id FROM users u ORDER BY (SELECT COUNT(*) FROM orders o WHERE o.user = u.id) DESC, u.id LIMIT 5', []],
];

const normalize = (rows, ordered) => {
  const vals = rows.map(r => JSON.stringify(Object.values(r).map(v => typeof v === 'number' ? Math.round(v * 1e9) / 1e9 : v)));
  return ordered ? vals : vals.sort();
};

test('SQL results match SQLite', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-sqldiff-'));
  try {
    const h = new Database(join(dir, 'h.kdb'), {create: true});
    const s = new DatabaseSync(':memory:');
    for (const q of schema) { h.sql(q); s.exec(q); }
    for (const [q, p] of rows()) { h.sql(q, p); s.prepare(q).run(...p); }
    for (const [q, p] of QUERIES) {
      const want = s.prepare(q).all(...p), got = h.query(q, p);
      const ordered = /ORDER BY/i.test(q);
      assert.deepEqual(normalize(got, ordered), normalize(want, ordered), q);
    }
    h.close(); s.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});
