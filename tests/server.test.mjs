import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import fs from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';
import {Database} from '../index.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const TOKEN = 'test-token-0123456789';

// Starts kestro-server on a free port; stop() closes its stdin, which shuts it down cleanly.
async function startServer(dir, {token = TOKEN, args = []} = {}) {
  const tokenFile = join(dir, 'token');
  if (token) await writeFile(tokenFile, token + '\n');
  const child = spawn(serverBinary, ['--db', join(dir, 'shared.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', ...(token ? ['--token-file', tokenFile] : []), ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const address = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${err}`)));
  });
  const exited = new Promise(resolve => child.on('exit', code => resolve(code)));
  return {address, child, stop: async () => { child.stdin.end(); const code = await exited; return {code, out, err}; }};
}
async function withServer(fn, options) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-server-'));
  const server = await startServer(dir, options);
  try { await fn(server, dir); } finally { if (server.child.exitCode === null) await server.stop(); await rm(dir, {recursive: true, force: true}); }
}
function findTool(envName, candidates) {
  for (const c of [process.env[envName], ...candidates].filter(Boolean)) {
    const r = spawnSync(c, ['--version'], {encoding: 'utf8'});
    if (r.status === 0 && !/not found|Microsoft Store/i.test(r.stdout + r.stderr)) return c;
  }
  return null;
}
function runProcess(cmd, args) {
  return new Promise(resolve => {
    const child = spawn(cmd, args, {stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true});
    let out = '';
    child.stdout.on('data', d => { out += d; }); child.stderr.on('data', d => { out += d; });
    child.on('close', code => resolve({code, out}));
  });
}

test('server: many connections write concurrently, each write gets its own version', () => withServer(async ({address}) => {
  const clients = await Promise.all(Array.from({length: 8}, () => RemoteDB.connect(address, {token: TOKEN})));
  try {
    const receipts = await Promise.all(clients.flatMap((db, c) => Array.from({length: 200}, (_, i) => db.set('items', `c${c}-${i}`, {c, i}))));
    const versions = receipts.map(r => r.version).sort((a, b) => a - b);
    assert.deepEqual(versions, Array.from({length: 1600}, (_, i) => i + 1), 'every write committed once');
    // Any connection sees every other connection's writes.
    const reader = clients[3];
    for (let c = 0; c < 8; c++) assert.deepEqual((await reader.get('items', `c${c}-199`)).value, {c, i: 199});
    const st = await reader.stats();
    assert.equal(st.keys, 1600); assert.equal(st.connections, 8); assert.equal(st.server, true);
  } finally { await Promise.all(clients.map(c => c.close())); }
}));

test('server: SQL from 8 connections at once gets unique auto ids and one consistent table', () => withServer(async ({address}) => {
  const clients = await Promise.all(Array.from({length: 8}, () => RemoteDB.connect(address, {token: TOKEN})));
  try {
    await clients[0].sql('CREATE TABLE orders (id INTEGER PRIMARY KEY, ref TEXT UNIQUE, total INT)');
    const ids = (await Promise.all(clients.flatMap((db, c) => Array.from({length: 100}, (_, i) => db.sql('INSERT INTO orders (ref, total) VALUES (?, ?)', [`c${c}-${i}`, i]))))).map(r => r.lastInsertId).sort((a, b) => a - b);
    assert.deepEqual(ids, Array.from({length: 800}, (_, i) => i + 1));
    assert.deepEqual((await clients[3].sql('SELECT COUNT(*) AS n, SUM(total) AS s FROM orders')).rows, [{n: 800, s: 8 * 4950}]);
    await assert.rejects(clients[1].sql('INSERT INTO orders (ref) VALUES (?)', ['c0-0']), /CONFLICT/);
    const page = await clients[2].sql('SELECT ref FROM orders WHERE total >= ? ORDER BY total DESC, ref LIMIT 3', [99]);
    assert.deepEqual(page.rows.map(r => r.ref), ['c0-99', 'c1-99', 'c2-99']);
  } finally { await Promise.all(clients.map(c => c.close())); }
}));

test('server: SQL transactions per connection, rolled back when the connection drops; joins', () => withServer(async ({address}) => {
  const a = await RemoteDB.connect(address, {token: TOKEN}), b = await RemoteDB.connect(address, {token: TOKEN});
  try {
    await a.sql('CREATE TABLE acct (id INTEGER PRIMARY KEY, owner TEXT UNIQUE, balance INT)');
    await a.sql('INSERT INTO acct (owner, balance) VALUES (?, ?), (?, ?)', ['x', 100, 'y', 0]);
    await a.sql('BEGIN');
    await a.sql('UPDATE acct SET balance = balance - 40 WHERE owner = ?', ['x']);
    await a.sql('UPDATE acct SET balance = balance + 40 WHERE owner = ?', ['y']);
    assert.deepEqual((await a.sql('SELECT balance FROM acct ORDER BY owner')).rows, [{balance: 60}, {balance: 40}]);
    assert.deepEqual((await b.sql('SELECT balance FROM acct ORDER BY owner')).rows, [{balance: 100}, {balance: 0}]);  // not yet
    const waiting = b.sql('INSERT INTO acct (owner, balance) VALUES (?, ?)', ['z', 1]);  // waits for COMMIT
    await a.sql('COMMIT');
    await waiting;
    assert.deepEqual((await b.sql('SELECT owner, balance FROM acct ORDER BY owner')).rows, [{owner: 'x', balance: 60}, {owner: 'y', balance: 40}, {owner: 'z', balance: 1}]);
    // A connection that drops mid-transaction is rolled back, and others can write again.
    const c = await RemoteDB.connect(address, {token: TOKEN});
    await c.sql('BEGIN'); await c.sql('DELETE FROM acct'); await c.close();
    await b.sql('UPDATE acct SET balance = balance + 1 WHERE owner = ?', ['z']);
    assert.deepEqual((await b.sql('SELECT COUNT(*) AS n, SUM(balance) AS s FROM acct')).rows, [{n: 3, s: 102}]);
    await assert.rejects(a.sql('COMMIT'), /no transaction/);
    // Key/value writes on a connection with an open transaction are refused, not deadlocked.
    await a.sql('BEGIN'); await assert.rejects(a.set('kv', 'k', 1), /transaction is open/); await a.sql('ROLLBACK');
    await a.sql('CREATE TABLE moves (id INTEGER PRIMARY KEY, acct_id INT, amount INT)');
    await a.sql('INSERT INTO moves (acct_id, amount) VALUES (1, -40), (2, 40), (2, 5)');
    assert.deepEqual((await b.sql('SELECT a.owner, SUM(m.amount) AS net FROM acct a JOIN moves m ON m.acct_id = a.id GROUP BY a.owner ORDER BY a.owner')).rows, [{owner: 'x', net: -40}, {owner: 'y', net: 45}]);
  } finally { await a.close(); await b.close(); }
}));

test('server: users, roles and table limits are enforced for SQL and key/value requests', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-users-'));
  const tokens = {admin: 'admin-token-0123456789', writer: 'writer-token-0123456789', reader: 'reader-token-0123456789', scoped: 'scoped-token-0123456789'};
  const hashed = spawnSync(serverBinary, ['--hash-token'], {input: tokens.reader + '\n', encoding: 'utf8'}).stdout.trim();
  assert.match(hashed, /^sha256:[0-9a-f]{64}$/);
  await writeFile(join(dir, 'users'), [
    '# name role token [tables=...]',
    `admin admin ${tokens.admin}`,
    `writer readwrite ${tokens.writer}`,
    `reader readonly ${hashed}`,
    `scoped readwrite ${tokens.scoped} tables=orders,notes`,
  ].join('\n') + '\n');
  const server = await startServer(dir, {token: '', args: ['--users', join(dir, 'users')]});
  const as = name => RemoteDB.connect(server.address, {token: tokens[name]});
  try {
    const admin = await as('admin'), writer = await as('writer'), reader = await as('reader'), scoped = await as('scoped');
    await admin.sql('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)');
    await admin.sql('CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INT, total INT)');
    await writer.sql('INSERT INTO users (name) VALUES (?)', ['Ann']);
    await scoped.sql('INSERT INTO orders (user_id, total) VALUES (1, 10)');
    // readonly: reads only
    assert.deepEqual((await reader.sql('SELECT name FROM users')).rows, [{name: 'Ann'}]);
    await assert.rejects(reader.sql('INSERT INTO users (name) VALUES (?)', ['x']), /AUTH .*read-only/);
    await assert.rejects(reader.sql('UPDATE users SET name = ? WHERE id = 1', ['x']), /AUTH/);
    await assert.rejects(reader.set('users', 'k', 1), /AUTH/);
    await assert.rejects(reader.backup(join(dir, 'copy.kdb')), /AUTH .*admin/);
    const st = await reader.stats(); assert.equal(st.user, 'reader'); assert.equal(st.role, 'readonly');
    // readwrite: no schema changes
    await assert.rejects(writer.sql('CREATE TABLE t2 (id INT)'), /AUTH .*admin/);
    await assert.rejects(writer.sql('DROP TABLE users'), /AUTH/);
    await writer.sql('BEGIN'); await writer.sql('UPDATE users SET name = ? WHERE id = 1', ['Anne']); await writer.sql('COMMIT');
    // limited to orders: every table a statement touches is checked
    assert.deepEqual((await scoped.sql('SELECT total FROM orders')).rows, [{total: 10}]);
    await assert.rejects(scoped.sql('SELECT * FROM users'), /AUTH .*no access to users/);
    await assert.rejects(scoped.sql('SELECT o.total FROM orders o JOIN users u ON u.id = o.user_id'), /no access to users/);
    await assert.rejects(scoped.sql('SELECT total FROM orders WHERE user_id IN (SELECT id FROM users)'), /no access to users/);
    await assert.rejects(scoped.get('users', 'x'), /no access to users/);
    await assert.rejects(scoped.set('users', 'x', 1), /no access to users/);
    await scoped.set('notes', 'kv', {ok: true});  // a key/value collection it lists
    await assert.rejects(scoped.set('orders', 'kv', {ok: true}), /is a SQL table/);  // rows of SQL tables change through SQL
    // admin: everything
    await admin.verify(); await admin.sql('CREATE INDEX ON orders (user_id)');
    // A wrong token is refused, slowly.
    const t0 = Date.now(); await assert.rejects(RemoteDB.connect(server.address, {token: 'wrong-token-0123456789'}), /AUTH invalid/); assert.ok(Date.now() - t0 >= 200);
    await Promise.all([admin, writer, reader, scoped].map(c => c.close()));
  } finally { await server.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('server: a users file with a weak token or unknown role is refused at start', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-users-'));
  try {
    for (const line of ['a admin short', 'a superuser long-enough-token-0123', 'a admin long-enough-token-0123 rows=x']) {
      await writeFile(join(dir, 'users'), line + '\n');
      const r = spawnSync(serverBinary, ['--db', join(dir, 'x.kdb'), '--create', '--users', join(dir, 'users')], {encoding: 'utf8', timeout: 10000});
      assert.notEqual(r.status, 0); assert.match(r.stderr, /users file line 1/);
      await rm(join(dir, 'x.kdb'), {force: true});
    }
  } finally { await rm(dir, {recursive: true, force: true}); }
});

test('server: compare-and-swap across 16 connections has exactly one winner', () => withServer(async ({address}) => {
  const clients = await Promise.all(Array.from({length: 16}, () => RemoteDB.connect(address, {token: TOKEN})));
  try {
    const outcomes = await Promise.allSettled(clients.map((db, i) => db.set('race', 'k', {winner: i}, {expectedVersion: 0})));
    assert.equal(outcomes.filter(o => o.status === 'fulfilled').length, 1);
    assert.ok(outcomes.filter(o => o.status === 'rejected').every(o => /^CONFLICT/.test(o.reason.message)));
  } finally { await Promise.all(clients.map(c => c.close())); }
}));

test('server: a connection reads its own pipelined writes in order', () => withServer(async ({address}) => {
  const db = await RemoteDB.connect(address, {token: TOKEN});
  try {
    const w = db.set('c', 'k', 1), r = db.get('c', 'k'), d = db.delete('c', 'k'), after = db.get('c', 'k');
    assert.equal((await w).version, 1); assert.equal((await r).value, 1); assert.equal((await d).version, 2); assert.equal(await after, null);
    const many = await db.getMany('c', ['k', 'nope']);
    assert.deepEqual(many, [null, null]);
  } finally { await db.close(); }
}));

test('server: access token is required and checked', () => withServer(async ({address}) => {
  await assert.rejects(RemoteDB.connect(address, {token: 'wrong'}), /AUTH invalid/);
  const anonymous = await RemoteDB.connect(address);
  await assert.rejects(anonymous.get('c', 'k'), /AUTH required/);
}));

test('server: connection limit refuses extra clients with BUSY', () => withServer(async ({address}) => {
  const a = await RemoteDB.connect(address), b = await RemoteDB.connect(address);
  const c = await RemoteDB.connect(address);
  await assert.rejects(c.stats(), /BUSY|closed|ECONNRESET/);  // Windows may report the refused connection as a reset
  await a.close(); await b.close();
}, {token: '', args: ['--max-connections', '2']}));

test('server: clean shutdown keeps every write and leaves an index snapshot', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-server-'));
  try {
    const server = await startServer(dir);
    const db = await RemoteDB.connect(server.address, {token: TOKEN});
    await Promise.all(Array.from({length: 300}, (_, i) => db.set('c', 'k' + i, i)));
    await db.close();
    const {code, out} = await server.stop();
    assert.equal(code, 0); assert.match(out, /KESTRODB STOPPED/);
    using local = new Database(join(dir, 'shared.kdb'));
    assert.equal(local.stats().keys, 300);
    assert.equal(local.stats().openedFrom, 'index-snapshot');
  } finally { await rm(dir, {recursive: true, force: true}); }
});

const php = findTool('KESTRO_PHP', ['php']);
const python = findTool('KESTRO_PYTHON', ['python3', 'python', 'py']);
test('server: 4 PHP processes at once', {skip: php ? false : 'PHP not found (set KESTRO_PHP)'}, () => withServer(async ({address}) => {
  const runs = await Promise.all([1, 2, 3, 4].map(w => runProcess(php, [join(root, 'clients/php/test.php'), address, TOKEN, String(w), '150'])));
  for (const r of runs) assert.equal(r.code, 0, r.out);
  const db = await RemoteDB.connect(address, {token: TOKEN});
  try {
    assert.equal((await db.stats()).keys, 4 * 152 + 4 * 20 + 2);  // + SQL rows, catalog, id sequence
    assert.equal((await db.get('php', 'w3-149')).value.i, 149);
    assert.deepEqual((await db.sql('SELECT COUNT(*) AS n FROM items')).rows, [{n: 80}]);
  } finally { await db.close(); }
}));

// PHP with in-process reads (readFrom, PHP FFI + the C library) while writing through the server.
const phpFfi = php && spawnSync(php, ['-d', 'extension=ffi', '-r', 'exit(extension_loaded("ffi") ? 0 : 1);']).status === 0;
const library = join(root, 'bin', process.platform === 'win32' ? 'kestrodb.dll' : process.platform === 'darwin' ? 'libkestrodb.dylib' : 'libkestrodb.so');
const haveLibrary = fs.existsSync(library);
test('server: 4 PHP processes read in-process (FFI) while writing through the server', {skip: !php ? 'PHP not found (set KESTRO_PHP)' : !phpFfi ? 'PHP FFI extension not available' : !haveLibrary ? 'C library not built' : false}, () => withServer(async ({address}, dir) => {
  const runs = await Promise.all([1, 2, 3, 4].map(w => runProcess(php, ['-d', 'extension=ffi', join(root, 'clients/php/test.php'), address, TOKEN, String(w), '150', join(dir, 'shared.kdb')])));
  for (const r of runs) assert.equal(r.code, 0, r.out);
  const db = await RemoteDB.connect(address, {token: TOKEN});
  try { assert.equal((await db.stats()).keys, 4 * 152 + 4 * 20 + 2); } finally { await db.close(); }
}));

test('server: 4 Python processes read in-process (ctypes) while writing through the server', {skip: !python ? 'Python not found (set KESTRO_PYTHON)' : !haveLibrary ? 'C library not built' : false}, () => withServer(async ({address}, dir) => {
  const runs = await Promise.all([1, 2, 3, 4].map(w => runProcess(python, [join(root, 'clients/python/test_kestrodb.py'), address, TOKEN, String(w), '150', join(dir, 'shared.kdb')])));
  for (const r of runs) assert.equal(r.code, 0, r.out);
}));

test('C library: Python embedded mode owns, locks and reopens a database', {skip: !python ? 'Python not found (set KESTRO_PYTHON)' : !haveLibrary ? 'C library not built' : false}, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-embedded-'));
  try { const r = await runProcess(python, [join(root, 'clients/python/test_embedded.py'), dir]); assert.equal(r.code, 0, r.out); } finally { await rm(dir, {recursive: true, force: true}); }
});

test('server: 4 Python processes at once', {skip: python ? false : 'Python not found (set KESTRO_PYTHON)'}, () => withServer(async ({address}) => {
  const runs = await Promise.all([1, 2, 3, 4].map(w => runProcess(python, [join(root, 'clients/python/test_kestrodb.py'), address, TOKEN, String(w), '150'])));
  for (const r of runs) assert.equal(r.code, 0, r.out);
  const db = await RemoteDB.connect(address, {token: TOKEN});
  try { assert.equal((await db.stats()).keys, 4 * 152 + 4 * 20 + 2); } finally { await db.close(); }
}));
