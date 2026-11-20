// Node SQL API: Database (in-process) and RemoteDB (server) share query/queryOne/
// queryValue/execute/transaction/prepare, named and numbered parameters, and errors with
// codes. RemoteDB reconnects after a server restart.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Database} from '../index.mjs';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const TOKEN = 'sqlapi-token-0123456789';

async function startServer(dir, {port = 0, create = true} = {}) {
  await writeFile(join(dir, 'token'), TOKEN + '\n');
  const child = spawn(serverBinary, ['--db', join(dir, 's.kdb'), ...(create ? ['--create'] : []), '--tcp', `127.0.0.1:${port}`, '--stop-on-stdin-eof', '--token-file', join(dir, 'token')], {stdio: ['pipe', 'pipe', 'pipe']});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const address = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${err}`)));
  });
  const exited = new Promise(resolve => child.on('exit', resolve));
  return {address, port: Number(address.split(':').pop()), stop: async () => { child.stdin.end(); await exited; }};
}

// The same checks for both; `a` awaits a value or a promise.
async function check(db) {
  const a = v => Promise.resolve(v);
  await a(db.execute('CREATE TABLE IF NOT EXISTS people (id INTEGER PRIMARY KEY, email TEXT UNIQUE, name TEXT, age INTEGER)'));
  await a(db.execute('DELETE FROM people'));
  assert.equal(await a(db.execute('INSERT INTO people (email, name, age) VALUES (:email, :name, :age)', {email: 'a@x.org', ':name': 'Ann', age: 31})), 1);
  const ann = db.lastInsertId;
  await a(db.execute('INSERT INTO people (email, name, age) VALUES (?, ?, ?)', ['b@x.org', 'Bob', 25]));
  await a(db.execute('INSERT INTO people (email, name, age) VALUES ($2, $1, $3)', ['Cy', 'c@x.org', 41]));
  assert.equal(await a(db.queryValue('SELECT name FROM people WHERE id = ?', [ann])), 'Ann');
  assert.deepEqual((await a(db.query('SELECT name FROM people WHERE age > :min ORDER BY age', {min: 26}))).map(r => r.name), ['Ann', 'Cy']);
  assert.deepEqual(await a(db.queryOne('SELECT name, age FROM people WHERE email = @e', {e: 'b@x.org'})), {name: 'Bob', age: 25});
  assert.equal(await a(db.queryOne('SELECT * FROM people WHERE id = ?', [999])), null);
  assert.equal(await a(db.queryValue('SELECT COUNT(*) FROM people')), 3);
  // Errors carry codes.
  const code = async (f, c) => { try { await f(); assert.fail('accepted'); } catch (e) { assert.equal(e.code, c, e.message); } };
  await code(() => db.execute('INSERT INTO people (email) VALUES (?)', ['a@x.org']), 'CONFLICT');
  await code(() => db.query('SELEC nonsense'), 'INPUT');
  await code(() => db.query('SELECT * FROM people WHERE id = :id', {}), 'INPUT');
  await code(() => db.query('SELECT * FROM people WHERE id = :id AND age = ?', [1, 2]), 'INPUT');
  await code(() => db.query('SELECT * FROM people', 'bad'), 'INPUT');
  // Prepared statements.
  const byAge = db.prepare('SELECT name FROM people WHERE age >= ? ORDER BY age');
  assert.deepEqual((await a(byAge.all([30]))).map(r => r.name), ['Ann', 'Cy']);
  assert.deepEqual(await a(byAge.get([40])), {name: 'Cy'});
  assert.equal(await a(byAge.value([0])), 'Bob');
  const ins = db.prepare('INSERT INTO people (email, name, age) VALUES (:email, :name, :age)');
  const r = await a(ins.run({email: 'd@x.org', name: 'Dévi ✓ 日本', age: null}));
  assert.equal(r.changes, 1);
  assert.deepEqual(await a(db.queryOne('SELECT name, age FROM people WHERE id = ?', [r.lastInsertId])), {name: 'Dévi ✓ 日本', age: null});
  // Transactions.
  await a(db.transaction(t => t.execute('UPDATE people SET age = 50 WHERE email = ?', ['b@x.org'])));
  assert.equal(await a(db.queryValue('SELECT age FROM people WHERE email = ?', ['b@x.org'])), 50);
  await assert.rejects(async () => a(db.transaction(async t => { await a(t.execute('DELETE FROM people')); assert.equal(t.inTransaction, true); throw Error('boom'); })), /boom/);
  assert.equal(db.inTransaction, false);
  assert.equal(await a(db.queryValue('SELECT COUNT(*) FROM people')), 4);
  assert.equal(await a(db.transaction(t => t.queryValue('SELECT COUNT(*) FROM people'))), 4);
}

test('Database (in-process): SQL API', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-sqlapi-'));
  try {
    const db = new Database(join(dir, 'a.kdb'), {create: true});
    await check(db);
    // A synchronous transaction that throws rolls back.
    assert.throws(() => db.transaction(t => { t.execute('DELETE FROM people'); throw Error('sync boom'); }), /sync boom/);
    assert.equal(db.queryValue('SELECT COUNT(*) FROM people'), 4);
    db.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});

test('RemoteDB (server): SQL API, error codes, reconnect after a server restart', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-sqlapi-r-'));
  let srv = await startServer(dir);
  try {
    const db = await RemoteDB.connect(srv.address, {token: TOKEN});
    await check(db);
    await assert.rejects(RemoteDB.connect(srv.address, {token: 'wrong-token-0123456789'}), e => e.code === 'AUTH');
    await assert.rejects(RemoteDB.connect('tcp://127.0.0.1:1', {timeout: 1000}), e => e.code === 'IO');
    // Restart the server on the same port: the next call reconnects.
    const port = srv.port;
    await srv.stop();
    srv = await startServer(dir, {port, create: false});
    await new Promise(r => setTimeout(r, 100));
    assert.equal(await db.queryValue('SELECT COUNT(*) FROM people'), 4);
    // Connection lost inside a transaction: the transaction is reported lost, then the
    // connection is reopened for the next call.
    await db.execute('BEGIN');
    await srv.stop();
    srv = await startServer(dir, {port, create: false});
    await new Promise(r => setTimeout(r, 100));
    await assert.rejects(db.execute('DELETE FROM people'), e => e.code === 'IO' && /transaction/.test(e.message));
    assert.equal(db.inTransaction, false);
    assert.equal(await db.queryValue('SELECT COUNT(*) FROM people'), 4);
    await db.close();
    await assert.rejects(db.query('SELECT COUNT(*) FROM people'), e => e.code === 'CLOSED');
  } finally { await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});
