// Change notifications (LISTEN/NOTIFY, watch): in-process and through kestro-server.
import test from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import {Worker} from 'node:worker_threads';
import {spawn} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Database} from '../index.mjs';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const TOKEN = 'notify-token-0123456789';
const tick = (ms = 20) => new Promise(r => setTimeout(r, ms));
async function until(fn, ms = 10000) { const end = Date.now() + ms; while (!fn()) { if (Date.now() > end) throw Error('timed out'); await tick(5); } }

async function withDb(fn) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-notify-'));
  const db = new Database(join(dir, 'n.kdb'), {create: true});
  try { await fn(db); } finally { db.close(); await rm(dir, {recursive: true, force: true}); }
}
async function withServer(fn, args = [], files = {}) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-notify-srv-'));
  for (const [f, text] of Object.entries(files)) await writeFile(join(dir, f), text);
  if (!files.users) await writeFile(join(dir, 'token'), TOKEN + '\n');
  const child = spawn(serverBinary, ['--db', join(dir, 's.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', ...(files.users ? ['--users', join(dir, 'users')] : ['--token-file', join(dir, 'token')]), ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let err = ''; child.stderr.on('data', d => { err += d; });
  const exited = new Promise(r => child.on('exit', r));
  try {
    const address = await new Promise((resolve, reject) => {
      let out = ''; child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
      child.on('exit', code => reject(Error(`server exited ${code}: ${err}`)));
    });
    await fn(address);
  } finally { child.stdin.end(); await exited; await rm(dir, {recursive: true, force: true}); }
}

test('listen/notify in process: delivered after the call, asynchronously, with the commit txid', () => withDb(async db => {
  const got = [];
  const stop = db.listen('jobs', n => got.push(n));
  db.set('c', 'a', 1);
  db.notify('jobs', 'first');
  db.sql("NOTIFY jobs, 'second'");
  db.sql('SELECT pg_notify(?, ?)', ['jobs', 'third']);
  db.sql('NOTIFY jobs');  // no payload
  db.notify('other', 'nobody listens');
  assert.equal(got.length, 0, 'callbacks never run inside the call that notifies');
  await until(() => got.length === 4);
  assert.deepEqual(got, [{channel: 'jobs', payload: 'first', txid: 1}, {channel: 'jobs', payload: 'second', txid: 1}, {channel: 'jobs', payload: 'third', txid: 1}, {channel: 'jobs', payload: '', txid: 1}]);
  assert.deepEqual(db.sql("SELECT pg_notify('jobs', 'x')"), {rows: [{pg_notify: null}]});
  await until(() => got.length === 5);
  stop();
  db.notify('jobs', 'after');
  await tick();
  assert.equal(got.length, 5, 'nothing after stop()');
  // SQL LISTEN delivers as 'notification' events; quoted names may hold any text.
  const events = [];
  db.on('notification', n => events.push(n.channel + '=' + n.payload));
  db.sql('LISTEN "a b:c"');
  db.notify('a b:c', 'spaced');
  await until(() => events.length === 1);
  assert.deepEqual(events, ['a b:c=spaced']);
  db.sql('UNLISTEN *');
  db.notify('a b:c', 'gone');
  await tick();
  assert.equal(events.length, 1);
}));

test('notifications inside a transaction: only after COMMIT, none after ROLLBACK, duplicates once', () => withDb(async db => {
  const got = [];
  db.listen('ch', n => got.push(n.payload));
  db.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)');
  db.sql('BEGIN');
  db.notify('ch', 'a'); db.notify('ch', 'b'); db.notify('ch', 'a'); db.sql("NOTIFY ch, 'b'");
  db.sql('INSERT INTO t (id, v) VALUES (1, 1)');
  await tick();
  assert.deepEqual(got, [], 'nothing before COMMIT');
  db.sql('COMMIT');
  await until(() => got.length === 2);
  assert.deepEqual(got, ['a', 'b'], 'identical channel + payload once per transaction, in order');
  db.sql('BEGIN'); db.notify('ch', 'rolled back'); db.sql('INSERT INTO t (id, v) VALUES (2, 2)'); db.sql('ROLLBACK');
  db.sql('BEGIN'); db.notify('ch', 'kept'); db.sql('SAVEPOINT s'); db.notify('ch', 'undone'); db.sql('ROLLBACK TO s'); db.notify('ch', 'a'); db.sql('COMMIT');
  // A transaction that writes nothing still sends its notifications at COMMIT.
  db.transaction(() => db.notify('ch', 'empty transaction'));
  assert.throws(() => db.transaction(() => { db.notify('ch', 'thrown'); throw Error('boom'); }), /boom/);
  await until(() => got.length === 5);
  await tick();
  assert.deepEqual(got, ['a', 'b', 'kept', 'a', 'empty transaction'], 'a later transaction may send the same payload again');
}));

test('notifications arrive in commit order, also from the async writer thread', () => withDb(async db => {
  const got = [];
  db.listen('seq', n => got.push(n));
  const watched = [];
  db.watch('k', c => watched.push(c));
  const writes = [];
  for (let i = 0; i < 300; i++) {
    if (i % 3 === 0) db.notify('seq', String(i));
    writes.push(db.setAsync('k', 'key' + i, {i}));
    if (i % 50 === 0) await Promise.all(writes.splice(0));
  }
  await Promise.all(writes);
  db.notify('seq', 'end');
  await until(() => got.length === 101 && watched.length === 300);
  assert.deepEqual(got.map(n => n.payload), [...Array.from({length: 100}, (_, i) => String(i * 3)), 'end']);
  assert.deepEqual(watched.map(c => c.key), Array.from({length: 300}, (_, i) => 'key' + i), 'changes in commit order');
  for (let i = 1; i < watched.length; i++) assert.ok(watched[i].txid > watched[i - 1].txid);
  // A NOTIFY between commits carries the txid of the last commit before it.
  for (const n of got) assert.ok(Number.isSafeInteger(n.txid));
  for (let i = 1; i < got.length; i++) assert.ok(got[i].txid >= got[i - 1].txid);
}));

test('close() commits queued async writes first: their notifications are still delivered', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-notify-'));
  try {
    const db = new Database(join(dir, 'n.kdb'), {create: true}), got = [];
    db.watch('q', c => got.push(c.key));
    db.listen('done', n => got.push(n.payload));
    const pending = [db.setAsync('q', 'a', 1), db.setAsync('q', 'b', 2)];
    db.notify('done', 'sync');
    db.close();
    await Promise.all(pending);
    await until(() => got.length === 3);
    assert.deepEqual(got.sort(), ['a', 'b', 'sync']);
    assert.throws(() => db.listen('x', () => {}), /CLOSED/);
  } finally { await rm(dir, {recursive: true, force: true}); }
});

test('unlisten: the stop function, unlisten(channel), unlisten() and UNLISTEN', () => withDb(async db => {
  const got = [];
  const a = db.listen('x', () => got.push('a')), b = db.listen('x', () => got.push('b'));
  db.listen('y', () => got.push('y'));
  db.notify('x'); db.notify('y');
  await until(() => got.length === 3);
  a();
  a();  // twice: no effect
  db.notify('x');
  await until(() => got.length === 4);
  assert.deepEqual(got.slice(3), ['b']);
  b();
  db.notify('x');
  db.unlisten('y');
  db.notify('y');
  await tick();
  assert.equal(got.length, 4, 'no callback left');
  db.listen('z', () => got.push('z'));
  db.sql('UNLISTEN z');
  db.notify('z');
  db.listen('w', () => got.push('w'));
  db.unlisten();
  db.notify('w');
  await tick();
  assert.equal(got.length, 4);
  // An error in one callback is reported on its own and does not stop the others.
  const errors = [], runner = process.listeners('uncaughtException');
  process.removeAllListeners('uncaughtException');
  process.on('uncaughtException', e => errors.push(e.message));
  try {
    db.listen('e', () => { throw Error('callback failed'); });
    db.listen('e', () => got.push('after error'));
    db.notify('e');
    await until(() => got.length === 5 && errors.length === 1);
  } finally { process.removeAllListeners('uncaughtException'); for (const l of runner) process.on('uncaughtException', l); }
  assert.deepEqual(errors, ['callback failed']);
}));

test('limits: payload of at most 8000 bytes, channel names, the reserved table_changes: prefix', () => withDb(async db => {
  const got = [];
  db.listen('big', n => got.push(n.payload.length));
  db.notify('big', 'x'.repeat(8000));
  assert.throws(() => db.notify('big', 'x'.repeat(8001)), /LIMIT a notification payload is at most 8000 bytes/);
  assert.throws(() => db.notify('big', 'é'.repeat(4001)), /LIMIT/, 'bytes, not characters');
  db.sql('BEGIN');
  assert.throws(() => db.notify('big', 'y'.repeat(9000)), /LIMIT/);
  db.sql('ROLLBACK');
  assert.throws(() => db.notify('', 'p'), {code: 'INPUT'});
  assert.throws(() => db.notify('c'.repeat(256), 'p'), {code: 'INPUT'});
  assert.throws(() => db.listen('c'.repeat(256), () => {}), {code: 'INPUT'});
  assert.throws(() => db.listen('ok'), {code: 'INPUT'}, 'a callback is required');
  assert.throws(() => db.notify('table_changes:users', 'fake'), /INPUT channels starting with table_changes:/);
  assert.throws(() => db.notify('big', 42), /INPUT SQL: the notification payload must be text/);
  assert.throws(() => db.sql('LISTEN'), /INPUT SQL/);
  await until(() => got.length === 1);
  assert.deepEqual(got, [8000]);
}));

test('watch: inserts, updates and deletes of key/value collections and SQL tables, after commit', () => withDb(async db => {
  const kv = [], rows = [];
  const stop = db.watch('items', c => kv.push(c));
  db.watch('users', c => rows.push(c));
  db.set('items', 'a', {v: 1});
  db.set('items', 'a', {v: 2});
  db.batch([{type: 'set', collection: 'items', key: 'b', value: 1}, {type: 'delete', collection: 'items', key: 'a'}, {type: 'set', collection: 'other', key: 'z', value: 0}]);
  db.delete('items', 'never-existed');
  db.sql('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)');
  db.sql("INSERT INTO users (id, name) VALUES (7, 'ann'), (-2.5e0 + 3.5, 'bob')");
  db.sql("UPDATE users SET name = 'ann b' WHERE id = 7");
  db.transaction(() => { db.sql("INSERT INTO users (id, name) VALUES (9, 'tmp')"); db.sql('DELETE FROM users WHERE id = 9'); db.sql('DELETE FROM users WHERE id = 1'); });
  db.sql('BEGIN'); db.sql("INSERT INTO users (id, name) VALUES (10, 'rolled back')"); db.sql('ROLLBACK');
  db.sql('CREATE TABLE named (id TEXT PRIMARY KEY)');
  db.watch('named', c => rows.push(c));
  db.sql("INSERT INTO named (id) VALUES ('ké\"y')");
  await until(() => kv.length === 4 && rows.length === 5);
  await tick();
  assert.deepEqual(kv.map(({op, table, key}) => [op, table, key]), [['insert', 'items', 'a'], ['update', 'items', 'a'], ['insert', 'items', 'b'], ['delete', 'items', 'a']]);
  assert.equal(kv[2].txid, kv[3].txid, 'one transaction, one txid');
  // SQL rows are keyed by their id (a number here); the two rows of one INSERT share a txid.
  const users = rows.filter(r => r.table === 'users');
  assert.deepEqual(users.slice(0, 2).map(r => r.op + ' ' + r.key).sort(), ['insert 1', 'insert 7']);
  assert.equal(users[0].txid, users[1].txid);
  assert.deepEqual(users.slice(2).map(r => [r.op, r.key]), [['update', 7], ['delete', 1]]);
  assert.equal(typeof users[0].key, 'number');
  assert.deepEqual(rows[4], {op: 'insert', table: 'named', key: 'ké"y', txid: rows[4].txid});
  assert.ok(!rows.some(r => r.key === 9 || r.key === 10), 'a row created and deleted in one transaction, or rolled back, is not reported');
  stop();
  db.set('items', 'c', 1);
  await tick();
  assert.equal(kv.length, 4);
}));

test('server: a connection that LISTENs gets notifications from another, in commit order', () => withServer(async address => {
  const listener = await RemoteDB.connect(address, {token: TOKEN}), writer = await RemoteDB.connect(address, {token: TOKEN}), plain = await RemoteDB.connect(address, {token: TOKEN});
  try {
    const got = [], all = [], changes = [];
    const stop = await listener.listen('orders', n => got.push(n));
    listener.on('notification', n => all.push(n.channel));
    await listener.watch('stock', c => changes.push(c));
    await listener.watch('kv', c => changes.push(c));
    await writer.sql('CREATE TABLE stock (id INTEGER PRIMARY KEY, n INTEGER)');
    await writer.notify('orders', 'one');
    await writer.transaction(async t => { await t.notify('orders', 'two'); await t.sql('INSERT INTO stock (id, n) VALUES (1, 5)'); await t.notify('orders', 'two'); });
    await writer.sql('BEGIN'); await writer.notify('orders', 'rolled back'); await writer.sql('ROLLBACK');
    await writer.set('kv', 'a', {n: 1});
    await writer.sql('UPDATE stock SET n = 4 WHERE id = 1');
    await Promise.all(Array.from({length: 200}, (_, i) => writer.notify('orders', 'p' + i)));
    await writer.notify('orders', 'x'.repeat(8000));
    await assert.rejects(writer.notify('orders', 'x'.repeat(8001)), e => e.code === 'LIMIT');
    // A connection that never listens gets only its replies.
    for (let i = 0; i < 20; i++) assert.equal((await plain.sql('SELECT 1 AS one')).rows[0].one, 1);
    await until(() => got.length === 203 && changes.length === 3);
    assert.deepEqual(got.slice(0, 2).map(n => n.payload), ['one', 'two']);
    assert.deepEqual(got.slice(2, 202).map(n => n.payload), Array.from({length: 200}, (_, i) => 'p' + i), 'pipelined notifications in order');
    assert.equal(got[202].payload.length, 8000);
    assert.ok(got[1].txid > got[0].txid, 'the transaction committed a row: a later txid');
    assert.deepEqual(changes.map(({op, table, key}) => `${op} ${table} ${key}`), ['insert stock 1', 'insert kv a', 'update stock 1']);
    assert.equal(all.filter(c => c === 'orders').length, 203);
    await stop();
    await writer.notify('orders', 'after unlisten');
    await listener.sql('SELECT 1');  // a round trip: anything sent before it has arrived
    await tick(50);
    assert.equal(got.length, 203);
    // LISTEN through SQL on the server, then events only.
    await listener.sql('LISTEN raw');
    await writer.sql("NOTIFY raw, 'r'");
    await until(() => all.includes('raw'));
  } finally { await listener.close(); await writer.close(); await plain.close(); }
}));

test('server: a slow listener never holds up commits; past --notify-queue it is disconnected', () => withServer(async address => {
  const [host, port] = address.slice(6).split(':');
  // A raw client: log in, LISTEN, then stop reading.
  const raw = net.connect({host, port: Number(port)});
  await new Promise(r => raw.once('connect', r));
  const frame = (cmd, body) => { const b = Buffer.alloc(5 + body.length); b.writeUInt32LE(body.length + 1); b[4] = cmd.charCodeAt(0); body.copy(b, 5); return b; };
  const sql = text => { const t = Buffer.from(text), j = Buffer.from('[]'), body = Buffer.alloc(4 + t.length + j.length); body.writeUInt32LE(t.length); t.copy(body, 4); j.copy(body, 4 + t.length); return frame('E', body); };
  let buffered = Buffer.alloc(0);
  const onData = d => { buffered = Buffer.concat([buffered, d]); };
  raw.on('data', onData);
  raw.write(frame('A', Buffer.from(TOKEN))); raw.write(sql('LISTEN busy'));
  await until(() => buffered.length >= 5 + 4 + '{"changes":0}'.length + 1);
  raw.off('data', onData); raw.pause();
  const fast = await RemoteDB.connect(address, {token: TOKEN}), writer = await RemoteDB.connect(address, {token: TOKEN});
  try {
    let received = 0, last = -1, inOrder = true;
    await fast.listen('busy', n => { const i = Number(n.payload.slice(0, 6)); if (i !== last + 1) inOrder = false; last = i; received++; });
    // Rounds of 20 notifications (7 KB each) until the server drops the listener that does not
    // read (its socket buffers fill first), each round timed; the reading listener keeps up.
    const pad = 'p'.repeat(7000);
    let sent = 0, slowest = 0;
    for (let round = 0; round < 3000; round++) {
      const t0 = Date.now();
      await Promise.all(Array.from({length: 20}, () => writer.notify('busy', String(sent++).padStart(6, '0') + pad)));
      await writer.set('c', 'k' + round, {round});
      slowest = Math.max(slowest, Date.now() - t0);
      await until(() => received === sent);
      if (round % 20 === 19 && (await writer.stats()).listenersDropped > 0) break;
    }
    assert.equal((await writer.stats()).listenersDropped, 1, 'the listener that fell behind was dropped, and only it');
    const N = sent;
    assert.ok(inOrder, 'the reading listener got every notification in order');
    assert.ok(slowest < 5000, `writes kept going while a listener did not read (slowest round of 20: ${slowest} ms)`);
    // The slow connection was closed once it fell more than the queue behind: it ends before N notifications.
    let tail = Buffer.alloc(0), ended = false;
    raw.on('data', d => { tail = Buffer.concat([tail, d]); });
    raw.on('error', () => {});
    const closed = new Promise(r => raw.once('close', () => { ended = true; r(); }));
    raw.resume();
    await Promise.race([closed, new Promise(r => setTimeout(r, 20000).unref())]);
    assert.ok(ended, 'the server disconnected the listener that fell behind');
    let count = 0;
    for (let p = 0; p + 4 <= tail.length;) { const len = tail.readUInt32LE(p); if (p + 4 + len > tail.length) break; assert.equal(tail[p + 4], 2, 'only notification frames'); count++; p += 4 + len; }
    assert.ok(count > 0 && count < N, `the slow listener got ${count} of ${N} before it was dropped`);
    assert.equal((await writer.get('c', 'k0')).value.round, 0, 'the server is still serving');
  } finally { raw.destroy(); await fast.close(); await writer.close(); }
}, ['--notify-queue', '1']));

test('server: RemoteDB reports a dropped listening connection, reconnects and listens again', () => withServer(async address => {
  // The listener runs in a worker whose thread is blocked for a while, so it stops reading.
  const flag = new Int32Array(new SharedArrayBuffer(4));
  const worker = new Worker(`
    const {parentPort, workerData: {url, address, token, flag}} = require('node:worker_threads');
    import(url).then(async ({RemoteDB}) => {
      const db = await RemoteDB.connect(address, {token});
      db.on('disconnect', () => parentPort.postMessage('disconnect'));
      db.on('reconnect', () => parentPort.postMessage('reconnect'));
      await db.listen('w', n => { if (n.payload === 'after') parentPort.postMessage('after'); });
      parentPort.on('message', async m => { if (m === 'block') Atomics.wait(flag, 0, 0); else { await db.close(); parentPort.close(); } });
      parentPort.postMessage('ready');
    });`, {eval: true, workerData: {url: new URL('../remote.mjs', import.meta.url).href, address, token: TOKEN, flag}});
  const seen = [];
  worker.on('message', m => seen.push(m));
  const writer = await RemoteDB.connect(address, {token: TOKEN});
  try {
    await until(() => seen.includes('ready'));
    worker.postMessage('block');
    const pad = 'w'.repeat(7000);
    for (let round = 0; round < 2000 && !(await writer.stats()).listenersDropped; round++) await Promise.all(Array.from({length: 20}, () => writer.notify('w', pad)));
    Atomics.store(flag, 0, 1); Atomics.notify(flag, 0);
    await until(() => seen.includes('reconnect'));
    assert.deepEqual(seen, ['ready', 'disconnect', 'reconnect']);
    await writer.notify('w', 'after');
    await until(() => seen.includes('after'));
  } finally { Atomics.store(flag, 0, 1); Atomics.notify(flag, 0); worker.postMessage('close'); await writer.close(); await new Promise(r => worker.once('exit', r)); }
}, ['--notify-queue', '1']));

test('server: a table-limited user may watch only its own tables', () => withServer(async address => {
  const admin = await RemoteDB.connect(address, {token: 'admin-token-0123456789abcdef'}), limited = await RemoteDB.connect(address, {token: 'limited-token-0123456789abcd'});
  try {
    const got = [];
    await limited.watch('mine', c => got.push(c.key));
    await assert.rejects(limited.watch('theirs', () => {}), /no access to theirs/);
    await limited.listen('anything', () => {});
    await admin.set('mine', 'm1', 1); await admin.set('theirs', 't1', 1);
    await until(() => got.length === 1);
    assert.deepEqual(got, ['m1']);
  } finally { await admin.close(); await limited.close(); }
}, [], {users: 'admin admin admin-token-0123456789abcdef\nlimited readwrite limited-token-0123456789abcd tables=mine\n'}));
