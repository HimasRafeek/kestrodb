// SQL over the network, server against server: kestro-server (RemoteDB.sql) vs MySQL or
// MariaDB (mysql2 driver) and/or PostgreSQL (pg driver, named prepared statements). One Node client, same tables, same statements, same
// parameters, both over TCP on this machine. Durable commits on both sides: the report
// prints MySQL's innodb_flush_log_at_trx_commit (1 = flush to disk on every commit).
//
//   KESTRO_MYSQL_URL=mysql://user:password@127.0.0.1:3306/kestro_bench \
//   KESTRO_MYSQL_DRIVER=/path/to/node_modules/mysql2 \
//   KESTRO_PG_URL=postgres://user:password@127.0.0.1:5432/kestro_bench \
//   KESTRO_PG_DRIVER=/path/to/node_modules/pg \
//   node bench/server-sql-compare.mjs [--rows 20000] [--runs 3] [--connections 16]
//
// The MySQL database must exist; the script creates and drops only its own table
// (kestro_bench_users). The mysql2 driver is not a KestroDB dependency: install it anywhere
// and point KESTRO_MYSQL_DRIVER at it.
import {spawn} from 'node:child_process';
import {createRequire} from 'node:module';
import {mkdtempSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? Number(process.argv[i + 1]) : d; };
const ROWS = arg('rows', 20000), RUNS = arg('runs', 3), CONNS = arg('connections', 16);
const URL_ = process.env.KESTRO_MYSQL_URL, DRIVER = process.env.KESTRO_MYSQL_DRIVER || 'mysql2';
const PG_URL = process.env.KESTRO_PG_URL, PG_DRIVER = process.env.KESTRO_PG_DRIVER || 'pg';
if (!URL_ && !PG_URL) throw Error('set KESTRO_MYSQL_URL=mysql://user:password@host:port/database and/or KESTRO_PG_URL=postgres://...');
const mysql = URL_ ? createRequire(import.meta.url)(DRIVER + '/promise') : null;
const pg = PG_URL ? createRequire(import.meta.url)(PG_DRIVER) : null;
const root = fileURLToPath(new URL('../', import.meta.url));
const serverBin = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const cities = ['Colombo', 'Kandy', 'Galle', 'Jaffna', 'Negombo'];
const row = i => [`user${i}@example.com`, `User ${i}`, 18 + i % 60, cities[i % 5]];
const median = xs => [...xs].sort((a, b) => a - b)[xs.length >> 1];
async function time(n, fn) { const t = performance.now(); for (let i = 0; i < n; i++) await fn(i); return n / ((performance.now() - t) / 1000); }
// n operations spread over `conns` clients running at once.
async function timeConcurrent(clients, n, fn) {
  const t = performance.now(); let next = 0;
  await Promise.all(clients.map(async c => { for (let i; (i = next++) < n;) await fn(c, i); }));
  return n / ((performance.now() - t) / 1000);
}

async function kestro() {
  const dir = mkdtempSync(join(tmpdir(), 'kestro-ssql-'));
  const child = spawn(serverBin, ['--db', join(dir, 'h.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', '--max-connections', String(CONNS + 8)], {stdio: ['pipe', 'pipe', 'inherit'], windowsHide: true});
  let out = '';
  const address = await new Promise((resolve, reject) => { child.stdout.on('data', d => { out += d; const m = /READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); }); child.on('exit', c => reject(Error('server exited ' + c))); });
  const db = await RemoteDB.connect(address, {jsonValidation: 'basic'});
  const pool = await Promise.all(Array.from({length: CONNS}, () => RemoteDB.connect(address, {jsonValidation: 'basic'})));
  await db.sql('CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INT, city TEXT)');
  await db.sql('CREATE INDEX ON users (city)'); await db.sql('CREATE INDEX ON users (age)');
  const ins = 'INSERT INTO users (email, name, age, city) VALUES (?, ?, ?, ?)';
  return {
    name: 'KestroDB', pool,
    sql: (c, q, p) => c.sql(q, p),
    bulk: async () => { for (let b = 0; b < ROWS; b += 500) { const v = [], ps = []; for (let i = b; i < Math.min(ROWS, b + 500); i++) { v.push('(?, ?, ?, ?)'); ps.push(...row(i)); } await db.sql('INSERT INTO users (email, name, age, city) VALUES ' + v.join(', '), ps); } },
    one: db, ins,
    close: async () => { for (const c of pool) await c.close(); await db.close(); await new Promise(r => { child.on('exit', r); child.stdin.end(); }); rmSync(dir, {recursive: true, force: true}); },
  };
}
async function mysqlEngine() {
  const opts = {uri: URL_, multipleStatements: false};
  const db = await mysql.createConnection(opts);
  const pool = await Promise.all(Array.from({length: CONNS}, () => mysql.createConnection(opts)));
  await db.query('DROP TABLE IF EXISTS kestro_bench_users');
  await db.query('CREATE TABLE kestro_bench_users (id BIGINT AUTO_INCREMENT PRIMARY KEY, email VARCHAR(255) UNIQUE, name VARCHAR(255), age INT, city VARCHAR(64), INDEX (city), INDEX (age)) ENGINE=InnoDB');
  const [[v]] = await db.query("SELECT VERSION() AS version, @@innodb_flush_log_at_trx_commit AS flush, @@log_bin AS binlog");
  const fix = q => q.replaceAll('FROM users', 'FROM kestro_bench_users').replaceAll('INTO users', 'INTO kestro_bench_users').replaceAll('UPDATE users', 'UPDATE kestro_bench_users');
  return {
    name: 'MySQL', pool, info: v,
    sql: (c, q, p) => c.execute(fix(q), p),
    bulk: async () => { for (let b = 0; b < ROWS; b += 500) { const v2 = [], ps = []; for (let i = b; i < Math.min(ROWS, b + 500); i++) { v2.push('(?, ?, ?, ?)'); ps.push(...row(i)); } await db.query('INSERT INTO kestro_bench_users (email, name, age, city) VALUES ' + v2.join(', '), ps); } },
    one: db, ins: 'INSERT INTO users (email, name, age, city) VALUES (?, ?, ?, ?)',
    close: async () => { await db.query('DROP TABLE IF EXISTS kestro_bench_users'); for (const c of pool) await c.end(); await db.end(); },
  };
}

async function pgEngine() {
  const opts = {connectionString: PG_URL};
  const db = new pg.Client(opts); await db.connect();
  const pool = await Promise.all(Array.from({length: CONNS}, async () => { const c = new pg.Client(opts); await c.connect(); return c; }));
  await db.query('DROP TABLE IF EXISTS kestro_bench_users');
  await db.query('CREATE TABLE kestro_bench_users (id BIGSERIAL PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INT, city TEXT)');
  await db.query('CREATE INDEX ON kestro_bench_users (city)'); await db.query('CREATE INDEX ON kestro_bench_users (age)');
  const v = (await db.query("SELECT version() AS version, current_setting('fsync') AS fsync, current_setting('synchronous_commit') AS sync, current_setting('wal_sync_method') AS method")).rows[0];
  const names = new Map();
  // ? -> $1, $2, ...; table name; one named (prepared) statement per distinct text.
  const fix = q => { let n = 0; return q.replaceAll('FROM users', 'FROM kestro_bench_users').replaceAll('INTO users', 'INTO kestro_bench_users').replaceAll('UPDATE users', 'UPDATE kestro_bench_users').replace(/\?/g, () => '$' + ++n); };
  const run = (c, q, p = []) => { const text = fix(q); let name = names.get(text); if (!name) { name = 's' + names.size; names.set(text, name); } return c.query({name, text, values: p}); };
  return {
    name: 'PostgreSQL', pool, info: v,
    sql: run,
    bulk: async () => { for (let b = 0; b < ROWS; b += 500) { const v2 = [], ps = []; for (let i = b; i < Math.min(ROWS, b + 500); i++) { v2.push('(?, ?, ?, ?)'); ps.push(...row(i)); } await db.query({text: fix('INSERT INTO users (email, name, age, city) VALUES ' + v2.join(', ')), values: ps}); } },
    one: db, ins: 'INSERT INTO users (email, name, age, city) VALUES (?, ?, ?, ?)',
    close: async () => { await db.query('DROP TABLE IF EXISTS kestro_bench_users'); for (const c of pool) await c.end(); await db.end(); },
  };
}
const results = {}; const infos = {};
const makers = [kestro, ...(URL_ ? [mysqlEngine] : []), ...(PG_URL ? [pgEngine] : [])];
for (let run = 0; run < RUNS; run++) {
  for (const make of makers) {
    const e = await make(); if (e.info) infos[e.name] = e.info;
    const r = results[e.name] ??= {}; const add = (k, v) => (r[k] ??= []).push(v);
    const q = (q2, p) => e.sql(e.one, q2, p);
    let t = performance.now(); await e.bulk(); add('Bulk insert (500 rows per statement)', ROWS / ((performance.now() - t) / 1000));
    add('Durable single-row INSERT', await time(300, i => q(e.ins, row(ROWS + i))));
    add('SELECT by id', await time(5000, i => q('SELECT * FROM users WHERE id = ?', [1 + i % ROWS])));
    add('SELECT by unique column', await time(5000, i => q('SELECT * FROM users WHERE email = ?', [`user${i % ROWS}@example.com`])));
    add('Indexed WHERE + ORDER BY + LIMIT 10', await time(2000, i => q('SELECT id, name FROM users WHERE city = ? ORDER BY age DESC LIMIT 10', [cities[i % 5]])));
    add('UPDATE by id (durable)', await time(300, i => q('UPDATE users SET age = age + 1 WHERE id = ?', [1 + i % ROWS])));
    add('COUNT(*) with LIKE (full scan)', await time(10, () => q('SELECT COUNT(*) AS n FROM users WHERE name LIKE ?', ['User 1%'])));
    add('GROUP BY with COUNT/AVG (full scan)', await time(10, () => q('SELECT city, COUNT(*) AS n, AVG(age) AS a FROM users GROUP BY city')));
    add(`SELECT by id, ${CONNS} connections`, await timeConcurrent(e.pool, 20000, (c, i) => e.sql(c, 'SELECT * FROM users WHERE id = ?', [1 + (i * 7919) % ROWS])));
    add(`Durable INSERT, ${CONNS} connections`, await timeConcurrent(e.pool, 2000, (c, i) => e.sql(c, e.ins, row(ROWS * 2 + i))));
    await e.close();
  }
}
const fmt = n => n >= 1e3 ? (n / 1e3).toFixed(1) + 'K' : n.toFixed(1);
const others = Object.keys(results).filter(n => n !== 'KestroDB');
const notes = [];
if (infos.MySQL) notes.push(`MySQL ${infos.MySQL.version}, innodb_flush_log_at_trx_commit=${infos.MySQL.flush}, binlog=${infos.MySQL.binlog}`);
if (infos.PostgreSQL) notes.push(`${infos.PostgreSQL.version.split(',')[0]}, fsync=${infos.PostgreSQL.fsync}, synchronous_commit=${infos.PostgreSQL.sync}, wal_sync_method=${infos.PostgreSQL.method}`);
console.log(`${ROWS.toLocaleString('en-US')} rows, median of ${RUNS} runs, operations/s over TCP. ${notes.join('; ')}`);
console.log('Test'.padEnd(40) + ['KestroDB', ...others].map(n => n.padStart(12)).join('') + '   Result');
for (const k of Object.keys(results.KestroDB)) {
  const h = median(results.KestroDB[k]); const o = others.map(n => median(results[n][k]));
  const best = Math.max(...o), bi = o.indexOf(best);
  console.log(k.padEnd(40) + [h, ...o].map(x => fmt(x).padStart(12)).join('') + '   ' + (h >= best ? `KestroDB ${(h / best).toFixed(2)}x ahead of ${others[bi]}` : `${others[bi]} ${(best / h).toFixed(2)}x ahead`));
}
