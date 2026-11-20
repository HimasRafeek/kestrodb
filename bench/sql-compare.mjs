// SQL head to head, in-process: KestroDB Database.sql() vs SQLite (node:sqlite, WAL,
// synchronous=FULL, prepared statements). Same tables, same statements, same parameters.
//
//   node bench/sql-compare.mjs [--rows 20000] [--runs 3]
import {DatabaseSync} from 'node:sqlite';
import {mkdtempSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {Database} from '../index.mjs';

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? Number(process.argv[i + 1]) : d; };
const ROWS = arg('rows', 20000), RUNS = arg('runs', 3);
const cities = ['Colombo', 'Kandy', 'Galle', 'Jaffna', 'Negombo'];
const row = i => [`user${i}@example.com`, `User ${i}`, 18 + i % 60, cities[i % 5]];

function time(n, fn) { const t = performance.now(); for (let i = 0; i < n; i++) fn(i); return n / ((performance.now() - t) / 1000); }
function median(xs) { const s = [...xs].sort((a, b) => a - b); return s[s.length >> 1]; }

function kestro(dir) {
  const db = new Database(join(dir, 'h.kdb'), {create: true});
  db.sql('CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INT, city TEXT)');
  db.sql('CREATE INDEX ON users (city)');
  db.sql('CREATE INDEX ON users (age)');
  const ins = 'INSERT INTO users (email, name, age, city) VALUES (?, ?, ?, ?)';
  return {
    name: 'KestroDB',
    insertOne: i => db.sql(ins, row(ROWS + i)),
    bulk: () => { for (let b = 0; b < ROWS; b += 500) { const vals = [], ps = []; for (let i = b; i < Math.min(ROWS, b + 500); i++) { vals.push('(?, ?, ?, ?)'); ps.push(...row(i)); } db.sql('INSERT INTO users (email, name, age, city) VALUES ' + vals.join(', '), ps); } },
    byId: i => db.sql('SELECT * FROM users WHERE id = ?', [1 + i % ROWS]).rows[0],
    byUnique: i => db.sql('SELECT * FROM users WHERE email = ?', [`user${i % ROWS}@example.com`]).rows[0],
    topByIndex: i => db.sql('SELECT id, name FROM users WHERE city = ? ORDER BY age DESC LIMIT 10', [cities[i % 5]]).rows,
    countScan: () => db.sql('SELECT COUNT(*) AS n FROM users WHERE name LIKE ?', ['User 1%']).rows[0].n,
    groupBy: () => db.sql('SELECT city, COUNT(*) AS n, AVG(age) AS a FROM users GROUP BY city').rows,
    update: i => db.sql('UPDATE users SET age = age + 1 WHERE id = ?', [1 + i % ROWS]),
    close: () => db.close(),
  };
}
function sqlite(dir) {
  const db = new DatabaseSync(join(dir, 's.sqlite'));
  db.exec('PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INT, city TEXT); CREATE INDEX users_city ON users (city); CREATE INDEX users_age ON users (age);');
  const ins = db.prepare('INSERT INTO users (email, name, age, city) VALUES (?, ?, ?, ?)');
  const byId = db.prepare('SELECT * FROM users WHERE id = ?'), byEmail = db.prepare('SELECT * FROM users WHERE email = ?');
  const top = db.prepare('SELECT id, name FROM users WHERE city = ? ORDER BY age DESC LIMIT 10');
  const count = db.prepare('SELECT COUNT(*) AS n FROM users WHERE name LIKE ?'), group = db.prepare('SELECT city, COUNT(*) AS n, AVG(age) AS a FROM users GROUP BY city');
  const upd = db.prepare('UPDATE users SET age = age + 1 WHERE id = ?');
  return {
    name: 'SQLite',
    insertOne: i => ins.run(...row(ROWS + i)),
    bulk: () => { for (let b = 0; b < ROWS; b += 500) { db.exec('BEGIN'); for (let i = b; i < Math.min(ROWS, b + 500); i++) ins.run(...row(i)); db.exec('COMMIT'); } },
    byId: i => byId.get(1 + i % ROWS),
    byUnique: i => byEmail.get(`user${i % ROWS}@example.com`),
    topByIndex: i => top.all(cities[i % 5]),
    countScan: () => count.get('User 1%').n,
    groupBy: () => group.all(),
    update: i => upd.run(1 + i % ROWS),
    close: () => db.close(),
  };
}
const results = {};
for (let run = 0; run < RUNS; run++) {
  for (const make of [kestro, sqlite]) {
    const dir = mkdtempSync(join(tmpdir(), 'kestro-sql-')), e = make(dir);
    const r = results[e.name] ??= {};
    const add = (k, v) => (r[k] ??= []).push(v);
    let t = performance.now(); e.bulk(); add('Bulk insert (500 rows per statement)', ROWS / ((performance.now() - t) / 1000));
    add('Durable single-row INSERT', time(500, e.insertOne));
    add('SELECT by id', time(20000, e.byId));
    add('SELECT by unique column', time(20000, e.byUnique));
    add('Indexed WHERE + ORDER BY + LIMIT 10', time(5000, e.topByIndex));
    add('UPDATE by id (durable)', time(500, e.update));
    add('COUNT(*) with LIKE (full scan)', time(20, e.countScan));
    add('GROUP BY with COUNT/AVG (full scan)', time(20, e.groupBy));
    e.close(); rmSync(dir, {recursive: true, force: true});
  }
}
const fmt = n => n >= 1e3 ? (n / 1e3).toFixed(1) + 'K' : n.toFixed(1);
console.log(`${ROWS.toLocaleString('en-US')} rows, median of ${RUNS} runs, statements/s (scan rows: queries/s)`);
for (const k of Object.keys(results.KestroDB)) {
  const h = median(results.KestroDB[k]), s = median(results.SQLite[k]);
  console.log(`${k.padEnd(40)} KestroDB ${fmt(h).padStart(8)}  SQLite ${fmt(s).padStart(8)}  ${h >= s ? 'KestroDB' : 'SQLite '} ${(Math.max(h, s) / Math.min(h, s)).toFixed(2)}x`);
}
