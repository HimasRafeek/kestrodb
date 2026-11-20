// Server security: each test is an attack from the 2026-11-19 review, which must now fail.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {existsSync} from 'node:fs';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import net from 'node:net';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const tokens = {admin: 'admin-token-0123456789abcdef0123', writer: 'writer-token-0123456789abcdef012', reader: 'reader-token-0123456789abcdef012',
  scoped: 'scoped-token-0123456789abcdef012', scopedro: 'scopedro-token-0123456789abcdef0'};
const usersFile = ['admin admin ' + tokens.admin, 'writer readwrite ' + tokens.writer, 'reader readonly ' + tokens.reader,
  'scoped readwrite ' + tokens.scoped + ' tables=orders', 'scopedro readonly ' + tokens.scopedro + ' tables=orders', ''].join('\n');
const sleep = ms => new Promise(r => setTimeout(r, ms));

function start(dir, args, {users = true} = {}) {
  const child = spawn(serverBinary, ['--db', join(dir, 's.kdb'), '--create', '--stop-on-stdin-eof', ...(users ? ['--users', join(dir, 'users')] : []), ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const exited = new Promise(r => child.on('exit', code => r({code, out, err})));
  const ready = new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${err}`)));
  });
  ready.catch(() => {});  // a server refusing to start is awaited through exited
  return {ready, exited, child, stop: async () => { child.stdin.end(); return exited; }};
}
async function withServer(args, fn) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-sec-'));
  await writeFile(join(dir, 'users'), usersFile);
  const s = start(dir, ['--tcp', '127.0.0.1:0', ...args]);
  try {
    const address = await s.ready;
    const as = name => RemoteDB.connect(address, {token: tokens[name], reconnect: false});
    const admin = await as('admin');
    await admin.sql('CREATE TABLE orders (id INTEGER PRIMARY KEY, item TEXT, qty INTEGER CHECK (qty >= 0))');
    await admin.sql('CREATE TABLE secrets (id INTEGER PRIMARY KEY, secret TEXT)');
    await admin.sql('CREATE TABLE child (id INTEGER PRIMARY KEY, oid INTEGER REFERENCES orders (id) ON DELETE CASCADE, note TEXT)');
    await admin.sql("INSERT INTO orders (id, item, qty) VALUES (1, 'pen', 1), (2, 'ink', 2)");
    await admin.sql("INSERT INTO secrets (id, secret) VALUES (1, 'TOP-SECRET-1')");
    await admin.sql("INSERT INTO child (id, oid, note) VALUES (10, 1, 'child of 1')");
    await fn({address, as, admin, dir});
    await admin.close();
  } finally { await s.stop(); await rm(dir, {recursive: true, force: true}); }
}

test('table limits cover every part of a statement and foreign key actions', async () => {
  await withServer([], async ({as, admin}) => {
    const ro = await as('scopedro'), rw = await as('scoped');
    for (const q of [
      'SELECT id, item FROM orders UNION SELECT id, secret FROM secrets',
      'SELECT id, item FROM orders EXCEPT SELECT id, secret FROM secrets',
      "SELECT id FROM orders ORDER BY CASE WHEN (SELECT secret FROM secrets WHERE id = 1) >= 'T' THEN id ELSE 0 - id END",
      'SELECT id FROM orders LIMIT (SELECT COUNT(*) FROM secrets)',
      'SELECT id FROM orders GROUP BY id HAVING id > (SELECT COUNT(*) FROM secrets)',
    ]) await assert.rejects(ro.sql(q), /AUTH .*no access to secrets/, q);
    // DELETE cascades into child; the user may not change (or probe) child.
    await assert.rejects(rw.sql('DELETE FROM orders WHERE id = 1'), /AUTH .*no access to child/);
    assert.equal((await admin.sql('SELECT COUNT(*) AS n FROM child')).rows[0].n, 1);
    // SHOW TABLES lists only the user's tables.
    assert.deepEqual((await ro.sql('SHOW TABLES')).rows.map(r => r.table), ['orders']);
    // Rows of a SQL table change through SQL (constraints), not key/value writes.
    await assert.rejects(rw.set('orders', 'kv', {id: 'kv', qty: -99}), /SQL table/);
    await ro.close(); await rw.close();
  });
});

test('transactions: read-only users may not hold the write gate; an idle one is rolled back', async () => {
  await withServer(['--transaction-timeout', '2s'], async ({as, admin}) => {
    const reader = await as('reader'), writer = await as('writer');
    await assert.rejects(reader.sql('BEGIN'), /AUTH .*read-only/);
    await assert.rejects(reader.sql('SAVEPOINT s'), /AUTH .*read-only/);
    await writer.sql('BEGIN');
    await writer.sql("INSERT INTO orders (item, qty) VALUES ('held', 1)");
    // Another writer waits for the gate only until the idle transaction is rolled back.
    const t0 = Date.now();
    await admin.sql("INSERT INTO orders (item, qty) VALUES ('after', 1)");
    const waited = Date.now() - t0;
    assert.ok(waited >= 1000 && waited < 15000, `waited ${waited} ms`);
    assert.equal((await admin.sql("SELECT COUNT(*) AS n FROM orders WHERE item = 'held'")).rows[0].n, 0);
    await assert.rejects(writer.sql('COMMIT'));  // its connection was closed
    await reader.close();
  });
});

test('a statement stops at the time limit and others go on', async () => {
  await withServer(['--statement-timeout', '1s'], async ({as, admin}) => {
    const v = []; for (let i = 0; i < 400; i++) v.push(`('x${i}', ${i})`);
    await admin.sql('INSERT INTO orders (item, qty) VALUES ' + v.join(', '));
    const reader = await as('reader');
    const t0 = Date.now();
    const slow = reader.sql('SELECT COUNT(*) AS n FROM orders a, orders b, orders c WHERE a.id + b.id + c.id < 0');
    await assert.rejects(slow, /LIMIT .*time limit/);
    assert.ok(Date.now() - t0 < 10000, 'stopped promptly');
    await admin.sql("INSERT INTO orders (item, qty) VALUES ('later', 1)");
    await reader.close();
  });
});

test('before login: small frames only, a deadline, and a limit on failed logins', async () => {
  await withServer(['--auth-timeout', '1s'], async ({address}) => {
    const [host, port] = address.slice(6).split(':');
    const raw = bytes => new Promise((resolve) => {
      const s = net.connect(Number(port), host); let got = Buffer.alloc(0); const t0 = Date.now();
      s.on('connect', () => { if (bytes) s.write(bytes); });
      s.on('data', d => { got = Buffer.concat([got, d]); });
      s.on('close', () => resolve({got, ms: Date.now() - t0}));
      s.on('error', () => {});
      setTimeout(() => s.destroy(), 8000);
    });
    // A frame declaring 64 MB before the token: refused at once, nothing allocated.
    const big = Buffer.from([0x00, 0x00, 0x00, 0x04, 0x41]);
    const r1 = await raw(big);
    assert.match(r1.got.toString('latin1'), /AUTH send the access token first/);
    // Silence before the token: the connection is closed after the deadline.
    const r2 = await raw(null);
    assert.ok(r2.ms >= 800 && r2.ms < 6000, `closed after ${r2.ms} ms, having received ${JSON.stringify(r2.got.toString('latin1'))}`);
    // Guessing: after 10 failures from an address, it is refused for a minute.
    for (let i = 0; i < 10; i++) await assert.rejects(RemoteDB.connect(address, {token: 'wrong-token-' + i + '-0123456789abcdef', reconnect: false}), /AUTH invalid/);
    await assert.rejects(RemoteDB.connect(address, {token: tokens.admin, reconnect: false}), /AUTH too many failed logins/);
  });
});

test('backups through the server go only into --backup-dir, by file name', async () => {
  const backups = await mkdtemp(join(tmpdir(), 'kdb-sec-backups-'));
  try {
    await withServer(['--backup-dir', backups], async ({admin, dir}) => {
      await assert.rejects(admin.backup(join(dir, 'elsewhere.kdb')), /plain file name/);
      await assert.rejects(admin.backup('..' + (process.platform === 'win32' ? '\\' : '/') + 'up.kdb'), /plain file name/);
      await admin.backup('nightly.kdb');
      assert.ok(existsSync(join(backups, 'nightly.kdb')));
    });
    await withServer([], async ({admin}) => {
      await assert.rejects(admin.backup('x.kdb'), /--backup-dir/);
    });
  } finally { await rm(backups, {recursive: true, force: true}); }
});

test('the server refuses unsafe configurations at start', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-sec-cfg-'));
  try {
    // No authentication on a non-loopback address.
    let r = await start(dir, ['--tcp', '0.0.0.0:0'], {users: false}).exited;
    assert.notEqual(r.code, 0); assert.match(r.err, /without authentication/);
    // A table limit on an administrator.
    await writeFile(join(dir, 'users'), 'boss admin ' + tokens.admin + ' tables=orders\n');
    r = await start(dir, ['--tcp', '127.0.0.1:0']).exited;
    assert.notEqual(r.code, 0); assert.match(r.err, /cannot limit an admin/);
  } finally { await rm(dir, {recursive: true, force: true}); }
});
