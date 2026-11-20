// CHECK, FOREIGN KEY, multi-column UNIQUE and SAVEPOINT: the same statements on KestroDB and
// SQLite (foreign_keys on). Each must succeed or fail on both, and the tables must match after
// every statement. Then KestroDB-only DDL (ADD/DROP CONSTRAINT, RENAME TO, RENAME COLUMN).
import test from 'node:test';
import assert from 'node:assert/strict';
import {DatabaseSync} from 'node:sqlite';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {Database} from '../index.mjs';

const schema = [
  'CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT UNIQUE, age INTEGER CHECK (age >= 0 AND age < 150))',
  'CREATE TABLE books (id INTEGER PRIMARY KEY, author INTEGER REFERENCES authors(id) ON DELETE CASCADE, title TEXT, price REAL, CONSTRAINT positive CHECK (price > 0), UNIQUE (author, title))',
  'CREATE TABLE reviews (id INTEGER PRIMARY KEY, book INTEGER, stars INTEGER CHECK (stars BETWEEN 1 AND 5), FOREIGN KEY (book) REFERENCES books(id) ON DELETE SET NULL)',
  'CREATE TABLE loans (id INTEGER PRIMARY KEY, book INTEGER REFERENCES books ON DELETE RESTRICT)',
  'CREATE TABLE emp (id INTEGER PRIMARY KEY, boss INTEGER REFERENCES emp(id) ON DELETE CASCADE, code TEXT UNIQUE)',
  'CREATE TABLE tags (id INTEGER PRIMARY KEY, email TEXT REFERENCES authors(email) ON UPDATE CASCADE ON DELETE SET NULL, label TEXT)',
];
const tables = ['authors', 'books', 'reviews', 'loans', 'emp', 'tags'];
// [sql, params]; each must have the same outcome on both databases.
const steps = [
  ["INSERT INTO authors (id, name, email, age) VALUES (1, 'Ann', 'ann@x', 30), (2, 'Bob', 'bob@x', 40)", []],
  ["INSERT INTO authors (id, name, email, age) VALUES (3, 'Cid', 'cid@x', -1)", []],
  ["INSERT INTO authors (id, name, email, age) VALUES (3, 'Cid', 'cid@x', NULL)", []],
  ["INSERT INTO authors (id, name, email, age) VALUES (4, 'Dee', 'dee@x', 200)", []],
  ['INSERT INTO books (id, author, title, price) VALUES (1, 1, ?, 10), (2, 1, ?, 20), (3, 2, ?, 5)', ['A', 'B', 'A']],
  ["INSERT INTO books (id, author, title, price) VALUES (4, 9, 'X', 10)", []],
  ["INSERT INTO books (id, author, title, price) VALUES (4, 2, 'X', 0)", []],
  ["INSERT INTO books (id, author, title, price) VALUES (4, 1, 'A', 7)", []],
  ["INSERT INTO books (id, author, title, price) VALUES (4, NULL, 'A', 7), (5, NULL, 'A', 8)", []],
  ["INSERT INTO books (id, author, title, price) VALUES (6, 3, 'C', 1), (7, 3, 'C', 2)", []],
  ["UPDATE books SET title = 'A' WHERE id = 2", []],
  ['UPDATE books SET price = CASE id WHEN 1 THEN 11 ELSE price + 1 END WHERE id IN (1, 2)', []],
  ['UPDATE books SET price = CASE WHEN price > 15 THEN 0 ELSE price END', []],
  ['UPDATE books SET author = 7 WHERE id = 3', []],
  ['UPDATE books SET author = 3 WHERE id = 3', []],
  ['UPDATE books SET price = -1 WHERE author = 1', []],
  ['INSERT INTO reviews (id, book, stars) VALUES (1, 1, 5), (2, 2, 3), (3, 3, 1)', []],
  ['INSERT INTO reviews (id, book, stars) VALUES (4, 1, 6)', []],
  ['INSERT INTO reviews (id, book, stars) VALUES (4, 99, 4)', []],
  ['INSERT INTO loans (id, book) VALUES (1, 3)', []],
  ['DELETE FROM books WHERE id = 3', []],
  ['DELETE FROM authors WHERE id = 3', []],
  ['DELETE FROM authors WHERE id = 1', []],
  ['DELETE FROM loans', []],
  ['DELETE FROM authors WHERE id = 3', []],
  ["INSERT INTO tags (id, email, label) VALUES (1, 'bob@x', 'x'), (2, 'bob@x', 'y'), (3, NULL, 'z')", []],
  ["INSERT INTO tags (id, email, label) VALUES (4, 'nobody@x', 'w')", []],
  ["UPDATE authors SET email = 'robert@x' WHERE id = 2", []],
  ['DELETE FROM authors WHERE id = 2', []],
  ['INSERT INTO emp (id, boss, code) VALUES (1, NULL, ?), (2, 1, ?), (3, 2, ?), (4, 1, ?), (5, NULL, ?)', ['a', 'b', 'c', 'd', 'e']],
  ['INSERT INTO emp (id, boss, code) VALUES (10, 11, ?), (11, 10, ?)', ['j', 'k']],
  ['INSERT INTO emp (id, boss, code) VALUES (12, 12, ?)', ['l']],
  ['UPDATE emp SET boss = 999 WHERE id = 5', []],
  ['UPDATE emp SET boss = 4 WHERE id = 5', []],
  ['DELETE FROM emp WHERE id = 1', []],
  ['DELETE FROM emp WHERE id IN (10, 11, 12)', []],
  // Transactions and savepoints
  ['BEGIN', []],
  ["INSERT INTO authors (id, name, email, age) VALUES (20, 'T1', 't1@x', 20)", []],
  ['SAVEPOINT a', []],
  ["INSERT INTO books (id, author, title, price) VALUES (20, 20, 'T', 3)", []],
  ['SAVEPOINT b', []],
  ['DELETE FROM authors WHERE id = 20', []],
  ['ROLLBACK TO b', []],
  ["INSERT INTO books (id, author, title, price) VALUES (21, 20, 'U', 4)", []],
  ['ROLLBACK TO SAVEPOINT a', []],
  ["INSERT INTO books (id, author, title, price) VALUES (22, 20, 'V', 5)", []],
  ['RELEASE a', []],
  ['INSERT INTO books (id, author, title, price) VALUES (23, 21, ?, 5)', ['W']],
  ['COMMIT', []],
  ['SAVEPOINT outer1', []],
  ["INSERT INTO authors (id, name, email, age) VALUES (30, 'S', 's@x', 1)", []],
  ['RELEASE SAVEPOINT outer1', []],
  ['SAVEPOINT outer2', []],
  ["INSERT INTO authors (id, name, email, age) VALUES (31, 'R', 'r@x', 1)", []],
  ['ROLLBACK', []],
  // Correlated subqueries in writes
  ["INSERT INTO books (id, author, title, price) VALUES (40, 30, 'P', 2), (41, 30, 'Q', 3)", []],
  ['INSERT INTO reviews (id, book, stars) VALUES (40, 40, 2), (41, 40, 4), (42, 22, 5)', []],
  ['UPDATE books SET price = (SELECT COUNT(*) FROM reviews r WHERE r.book = books.id) + 1', []],
  ['UPDATE books SET price = 0 WHERE NOT EXISTS (SELECT 1 FROM reviews r WHERE r.book = books.id)', []],
  ['DELETE FROM reviews WHERE stars < (SELECT MAX(x.stars) FROM reviews x WHERE x.book = reviews.book)', []],
  ['DELETE FROM authors WHERE NOT EXISTS (SELECT 1 FROM books b WHERE b.author = authors.id)', []],
];

test('constraints and savepoints match SQLite', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-cons-'));
  try {
    const h = new Database(join(dir, 'h.kdb'), {create: true});
    const s = new DatabaseSync(':memory:');
    s.exec('PRAGMA foreign_keys = ON');
    for (const q of schema) { h.sql(q); s.exec(q); }
    const dump = (db, t) => (db === h ? h.query(`SELECT * FROM ${t} ORDER BY id`) : s.prepare(`SELECT * FROM ${t} ORDER BY id`).all()).map(r => JSON.stringify(Object.values(r)));
    for (const [q, p] of steps) {
      let he = null, se = null;
      try { h.sql(q, p); } catch (e) { he = e; }
      try { s.prepare(q).run(...p); } catch (e) { se = e; }
      assert.equal(!!he, !!se, `${q}\n  KestroDB: ${he?.message ?? 'ok'}\n  SQLite: ${se?.message ?? 'ok'}`);
      for (const t of tables) assert.deepEqual(dump(h, t), dump(s, t), `${t} after ${q}`);
    }
    h.close(); s.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});

test('constraint DDL: ADD/DROP CONSTRAINT, RENAME, reopen', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-cons2-'));
  const file = join(dir, 'h.kdb');
  const fails = (db, q, p, re) => assert.throws(() => db.sql(q, p), re, q);
  try {
    let h = new Database(file, {create: true});
    h.sql('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)');
    h.sql('CREATE TABLE orders (id INTEGER PRIMARY KEY, user INTEGER, total REAL)');
    h.sql("INSERT INTO users (id, name, age) VALUES (1, 'a', 20), (2, 'b', 30)");
    h.sql('INSERT INTO orders (id, user, total) VALUES (1, 1, 5), (2, 9, 6)');
    // Existing rows are checked.
    fails(h, 'ALTER TABLE orders ADD CONSTRAINT fk_user FOREIGN KEY (user) REFERENCES users (id)', [], /fk_user/);
    fails(h, 'ALTER TABLE users ADD CHECK (age > 25)', [], /CHECK/);
    h.sql('DELETE FROM orders WHERE id = 2');
    h.sql('ALTER TABLE orders ADD CONSTRAINT fk_user FOREIGN KEY (user) REFERENCES users (id) ON DELETE CASCADE');
    h.sql('ALTER TABLE users ADD CONSTRAINT adult CHECK (age >= 18)');
    h.sql('ALTER TABLE users ADD CONSTRAINT name_age UNIQUE (name, age)');
    fails(h, 'INSERT INTO orders (user, total) VALUES (7, 1)', [], /CONFLICT.*fk_user/);
    fails(h, "INSERT INTO users (name, age) VALUES ('c', 10)", [], /CONFLICT.*adult/);
    fails(h, "INSERT INTO users (name, age) VALUES ('a', 20)", [], /CONFLICT.*name_age/);
    fails(h, 'DROP TABLE users', [], /referenced by foreign key fk_user/);
    fails(h, 'ALTER TABLE users DROP COLUMN age', [], /adult/);
    // The foreign key index was made.
    assert.ok(h.query('SHOW INDEX FROM orders').some(r => r.column === 'user'));
    // Survives reopening.
    h.close(); h = new Database(file);
    fails(h, 'INSERT INTO orders (user, total) VALUES (7, 1)', [], /fk_user/);
    // RENAME COLUMN and RENAME TO carry the constraints.
    h.sql('ALTER TABLE users RENAME COLUMN age TO years');
    fails(h, "INSERT INTO users (name, years) VALUES ('d', 3)", [], /adult/);
    h.sql('ALTER TABLE users RENAME TO people');
    assert.deepEqual(h.query('SELECT id, name, years FROM people ORDER BY id'), [{id: 1, name: 'a', years: 20}, {id: 2, name: 'b', years: 30}]);
    fails(h, 'INSERT INTO orders (user, total) VALUES (2000, 1)', [], /no row of people/);
    h.sql('INSERT INTO orders (user, total) VALUES (2, 1)');
    assert.equal(h.queryValue('SELECT COUNT(*) FROM orders'), 2);
    h.sql('DELETE FROM people WHERE id = 1');
    assert.deepEqual(h.query('SELECT user FROM orders'), [{user: 2}]);
    assert.equal(h.queryValue("SELECT COUNT(*) FROM people WHERE name = 'b'"), 1);
    h.sql("INSERT INTO people (name, years) VALUES ('e', 40)");  // ids continue after a rename
    assert.equal(h.queryValue("SELECT id FROM people WHERE name = 'e'"), 3);
    // DROP CONSTRAINT.
    h.sql('ALTER TABLE people DROP CONSTRAINT adult');
    h.sql("INSERT INTO people (name, years) VALUES ('f', 5)");
    h.sql('ALTER TABLE orders DROP FOREIGN KEY fk_user');
    h.sql('INSERT INTO orders (user, total) VALUES (7000, 1)');
    fails(h, 'ALTER TABLE orders DROP CONSTRAINT nope', [], /no constraint/);
    h.sql('ALTER TABLE orders DROP CONSTRAINT IF EXISTS nope');
    h.sql('DROP TABLE people');
    // Targets must be unique; SET NULL needs a nullable column.
    h.sql('CREATE TABLE p (id INTEGER PRIMARY KEY, code TEXT, ref TEXT UNIQUE)');
    fails(h, 'CREATE TABLE c (id INTEGER PRIMARY KEY, code TEXT REFERENCES p (code))', [], /must reference/);
    fails(h, 'CREATE TABLE c (id INTEGER PRIMARY KEY, x INTEGER NOT NULL REFERENCES p ON DELETE SET NULL)', [], /NOT NULL/);
    fails(h, 'CREATE TABLE c (id INTEGER PRIMARY KEY, x INTEGER REFERENCES nothere)', [], /missing table/);
    h.sql('CREATE TABLE c (id INTEGER PRIMARY KEY, r TEXT REFERENCES p (ref))');
    h.sql("INSERT INTO p (code, ref) VALUES ('a', 'r1')");
    h.sql("INSERT INTO c (r) VALUES ('r1')");
    fails(h, "UPDATE p SET ref = 'r2'", [], /still reference/);
    // ADD COLUMN with REFERENCES checks a non-NULL default.
    fails(h, 'ALTER TABLE c ADD COLUMN pid INTEGER DEFAULT 5 REFERENCES p', [], /pid|no row/);
    h.sql('ALTER TABLE c ADD COLUMN pid INTEGER REFERENCES p');
    fails(h, 'UPDATE c SET pid = 99', [], /no row of p/);
    h.sql('UPDATE c SET pid = 1');
    h.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});

// A commit that writes rows of several indexed tables keeps every index exact (the index
// plans of one commit once pointed into a vector that moved when another table was added).
test('transactions over several indexed tables keep indexes exact', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-multi-'));
  try {
    const h = new Database(join(dir, 'h.kdb'), {create: true});
    const names = ['t1', 't2', 't3', 't4', 't5'];
    for (const t of names) { h.sql(`CREATE TABLE ${t} (id INTEGER PRIMARY KEY, k INTEGER, v TEXT)`); h.sql(`CREATE INDEX ON ${t} (k)`); h.sql(`CREATE INDEX ON ${t} (v)`); }
    for (let i = 0; i < 300; i++) {
      h.sql('BEGIN');
      for (const t of names) h.sql(`INSERT INTO ${t} (k, v) VALUES (?, ?)`, [i % 7, 'v' + (i % 3)]);
      if (i % 5 === 0) for (const t of names) h.sql(`UPDATE ${t} SET k = k + 100 WHERE id = ?`, [i]);
      h.sql('COMMIT');
    }
    for (const t of names) {
      const all = h.query(`SELECT id, k, v FROM ${t}`);
      for (const k of [0, 3, 6, 100, 105]) assert.equal(h.queryValue(`SELECT COUNT(*) FROM ${t} WHERE k = ?`, [k]), all.filter(r => r.k === k).length, `${t}.k = ${k}`);
      for (const v of ['v0', 'v2']) assert.equal(h.query(`SELECT id FROM ${t} WHERE v = ?`, [v]).length, all.filter(r => r.v === v).length, `${t}.v = ${v}`);
    }
    h.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});
