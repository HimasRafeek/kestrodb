// SELECT by id or by a UNIQUE column takes a direct path; it must answer exactly as the
// general path does (forced here with "AND 1 = 1").
import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {Database} from '../index.mjs';

test('point SELECTs agree with the general path', () => {
  const dir = mkdtempSync(join(tmpdir(), 'kdb-point-'));
  const db = new Database(join(dir, 'p.kdb'), {create: true});
  try {
    db.sql('CREATE TABLE u (id INTEGER PRIMARY KEY, email TEXT UNIQUE, city TEXT, age INT)');
    db.sql('CREATE INDEX ON u (city)');
    for (let i = 1; i <= 50; i++) db.sql('INSERT INTO u (email, city, age) VALUES (?, ?, ?)', [`u${i}@x`, i % 2 ? 'A' : 'B', 20 + i]);
    db.sql('UPDATE u SET age = 99 WHERE id = 7'); db.sql('DELETE FROM u WHERE id = 8');
    const cases = [
      ['SELECT * FROM u WHERE id = ?', [7]], ['SELECT * FROM u WHERE id = ?', [8]], ['SELECT * FROM u WHERE id = ?', [999]],
      ['SELECT * FROM u WHERE id = ?', ['7']], ['SELECT * FROM u WHERE ? = id', [3]],
      ['SELECT * FROM u WHERE id = 5', []], ['SELECT * FROM u WHERE id = 5.0', []],
      ['SELECT email, age + 1 AS next FROM u WHERE id = ?', [9]], ['SELECT * FROM u WHERE id = ? LIMIT 0', [9]], ['SELECT * FROM u WHERE id = ? LIMIT 1 OFFSET 1', [9]],
      ['SELECT * FROM u WHERE email = ?', ['u12@x']], ['SELECT * FROM u WHERE email = ?', ['u8@x']], ['SELECT * FROM u WHERE email = ?', ['nobody']],
      ['SELECT * FROM u WHERE email = ?', [12]], ['SELECT id FROM u WHERE email = ?', ['u3@x']],
      ['SELECT * FROM u WHERE city = ?', ['A']],  // not unique: the general path
      ['SELECT * FROM u WHERE id = ? ORDER BY age DESC', [10]],
    ];
    const check = () => {
      for (const [q, p] of cases) {
        const fast = db.sql(q, p), general = db.sql(q.replace(/ (LIMIT|ORDER)|$/, ' AND 1 = 1$&'), p);
        assert.deepEqual(fast, general, q + ' ' + JSON.stringify(p));
      }
    };
    check();
    // Inside a transaction its own changes count (the direct path stands aside).
    db.sql('BEGIN'); db.sql('UPDATE u SET age = 1 WHERE id = 7');
    assert.equal(db.sql('SELECT age FROM u WHERE id = ?', [7]).rows[0].age, 1);
    db.sql('ROLLBACK');
    assert.equal(db.sql('SELECT age FROM u WHERE id = ?', [7]).rows[0].age, 99);
    db.close();
    // After reopening (catalog not loaded yet, lazy index): the same answers.
    const again = new Database(join(dir, 'p.kdb'));
    assert.equal(again.sql('SELECT age FROM u WHERE id = ?', [7]).rows[0].age, 99);
    assert.equal(again.sql('SELECT id FROM u WHERE email = ?', ['u12@x']).rows[0].id, 12);
    again.close();
  } finally { rmSync(dir, {recursive: true, force: true}); }
});

test('GROUP BY puts NULL and missing values in one group (column cache and row paths)', () => {
  const dir = mkdtempSync(join(tmpdir(), 'kdb-group-'));
  const db = new Database(join(dir, 'g.kdb'), {create: true});
  try {
    db.sql('CREATE TABLE g (id INTEGER PRIMARY KEY, a INT, b TEXT)');
    db.sql("INSERT INTO g (a, b) VALUES (1, 'x'), (2, NULL), (3, 'x')"); db.sql('INSERT INTO g (a) VALUES (4), (5)');
    const want = [{b: null, n: 3}, {b: 'x', n: 2}];
    assert.deepEqual(db.sql('SELECT b, COUNT(*) AS n FROM g GROUP BY b ORDER BY b').rows, want);
    assert.deepEqual(db.sql('SELECT b, COUNT(*) AS n FROM g WHERE id >= 0 GROUP BY b ORDER BY b').rows, want);  // row path
    assert.equal(db.sql('SELECT b, a % 2 AS odd, COUNT(*) AS n FROM g GROUP BY b, a % 2 ORDER BY b, odd').rows.length, 3);  // (NULL, even), (NULL, odd), (x, odd)
    db.close();
  } finally { rmSync(dir, {recursive: true, force: true}); }
});
