// kestro-server admin (status, stats, verify, backup, compact, metrics, connections, kill, slow)
// against a live server: role checks, exit codes, kill closing a connection (its transaction
// rolled back), and the slow-query log (placeholders, no values unless asked, rotation).
import test from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import {spawn, spawnSync} from 'node:child_process';
import {mkdir, mkdtemp, readFile, rm, stat, writeFile} from 'node:fs/promises';
import {existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const ADMIN = 'admin-token-0123456789', RW = 'writer-token-0123456789', RO = 'reader-token-0123456789', SCRAPE = 'scrape-token-0123456789abcdef';
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(fn, ms = 10000) { for (const end = Date.now() + ms; ; await sleep(20)) { const v = await fn(); if (v) return v; if (Date.now() > end) throw Error('timed out'); } }

async function start(args = [], {prepare, users = ''} = {}) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-admin-'));
  if (prepare) await prepare(dir);
  await writeFile(join(dir, 'users'), `admin admin ${ADMIN}\nwriter readwrite ${RW}\nreader readonly ${RO}\n${users}`);
  for (const [f, t] of [['admin.token', ADMIN], ['rw.token', RW], ['ro.token', RO], ['scrape.token', SCRAPE]]) await writeFile(join(dir, f), t + '\n');
  await mkdir(join(dir, 'backups'));
  const child = spawn(serverBinary, ['--db', join(dir, 'a.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', '--users', join(dir, 'users'), '--backup-dir', join(dir, 'backups'),
    '--metrics', '127.0.0.1:0', '--metrics-token-file', join(dir, 'scrape.token'), ...args.map(a => a.replace('$DIR', dir))], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const exited = new Promise(r => child.on('exit', r));
  const [address, http] = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const a = /KESTRODB READY (tcp|tls) (\S+)/.exec(out), h = /KESTRODB METRICS (\S+)/.exec(out); if (a && h) resolve([a[1] === 'tls' ? 'tls://localhost:' + a[2].split(':').pop() : 'tcp://' + a[2], h[1]]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${out}${err}`)));
  });
  // Runs kestro-server admin as a user ('admin', 'rw', 'ro'): {code, out, err}.
  const admin = (as, ...a) => new Promise(resolve => {
    const p = spawn(serverBinary, ['admin', '--server', address, ...(as ? ['--token-file', join(dir, as + '.token')] : []), ...a], {stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true});
    let o = '', e = ''; p.stdout.on('data', d => { o += d; }); p.stderr.on('data', d => { e += d; });
    p.on('close', code => resolve({code, out: o, err: e}));
  });
  return {dir, address, http, admin, log: () => out + err, stop: async () => { child.stdin.end(); await exited; await rm(dir, {recursive: true, force: true}); }};
}
const connections = async s => JSON.parse((await s.admin('admin', 'connections', '--json')).out).connections;

test('admin: status, stats, verify, backup, compact, metrics, connections (an administrator)', async () => {
  const s = await start();
  try {
    const db = await RemoteDB.connect(s.address, {token: RW});
    await db.execute('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)').catch(() => {});  // writer: no DDL
    for (let i = 0; i < 40; i++) await db.set('kv', 'k' + (i % 4), {i});
    let r = await s.admin('admin', 'status');
    assert.equal(r.code, 0, r.err);
    for (const re of [/^server\s+tcp:\/\//m, /^version\s+\d+\.\d+\.\d+ \((windows|linux|macos)\)/m, /^role\s+leader/m, /^uptime\s+\d+s/m, /^keys\s+4$/m, /^size\s+[\d.]+ KB \(transaction 40\)/m, /^connections\s+2$/m, /^user\s+admin \(admin\)/m])
      assert.match(r.out, re);
    r = await s.admin('ro', 'status'); assert.equal(r.code, 0, r.err); assert.match(r.out, /^user\s+reader \(readonly\)/m);
    r = await s.admin('admin', 'stats', '--json'); assert.equal(r.code, 0); const st = JSON.parse(r.out);
    assert.equal(st.keys, 4); assert.equal(st.user, 'admin'); assert.equal(st.serverRole, 'leader'); assert.match(st.version, /^\d/);
    r = await s.admin('admin', 'verify'); assert.equal(r.code, 0, r.err); assert.match(r.out, /intact/);
    r = await s.admin('admin', 'backup', 'nightly.kdb'); assert.equal(r.code, 0, r.err);
    assert.ok(existsSync(join(s.dir, 'backups', 'nightly.kdb')));
    r = await s.admin('admin', 'backup', '../out.kdb'); assert.equal(r.code, 1); assert.match(r.err, /INPUT a backup through the server is a plain file name/);
    r = await s.admin('admin', 'compact'); assert.equal(r.code, 0, r.err); assert.match(r.out, /^compacted: .* -> .*, 4 keys$/m);
    // Metrics over the connection (admin) and from the endpoint (its own token).
    r = await s.admin('admin', 'metrics'); assert.equal(r.code, 0, r.err); assert.match(r.out, /^kestrodb_compactions_total 1$/m); assert.match(r.out, /^kestrodb_backups_total 1$/m);
    r = await s.admin(null, 'metrics', '--metrics-url', s.http, '--metrics-token-file', join(s.dir, 'scrape.token')); assert.equal(r.code, 0, r.err); assert.match(r.out, /^# TYPE kestrodb_requests_total counter$/m);
    r = await s.admin(null, 'metrics', '--metrics-url', s.http); assert.equal(r.code, 1); assert.match(r.err, /AUTH .*401/);
    // Connections: who, from where, in a transaction, listening; the caller marked.
    const listener = await RemoteDB.connect(s.address, {token: RO});
    await listener.listen('ch', () => {});
    await db.execute('BEGIN'); await db.set('kv', 'x', 1).catch(() => {});  // a transaction is open (key/value writes refused meanwhile)
    const list = await connections(s);
    const mine = (await db.stats()).connectionId, theirs = (await listener.stats()).connectionId;
    const w = list.find(c => c.id === mine), l = list.find(c => c.id === theirs), me = list.find(c => c.self);
    assert.deepEqual([w.user, w.role, w.peer, w.inTransaction, w.listening], ['writer', 'readwrite', '127.0.0.1', true, false]);
    assert.deepEqual([l.user, l.role, l.inTransaction, l.listening], ['reader', 'readonly', false, true]);
    assert.equal(me.user, 'admin'); assert.ok(w.requests >= 40); assert.match(w.since, /^\d{4}-\d\d-\d\dT/);
    r = await s.admin('admin', 'connections'); assert.equal(r.code, 0); assert.match(r.out, /^ID\s+USER\s+ROLE\s+PEER/); assert.match(r.out, /writer\s+readwrite\s+127\.0\.0\.1/); assert.match(r.out, /\(this one\)/);
    await db.execute('ROLLBACK');
    await listener.close(); await db.close();
  } finally { await s.stop(); }
});

test('admin: readonly and readwrite users cannot list, kill, read slow queries or metrics; exit codes', async () => {
  const s = await start(['--slow-query-ms', '1']);
  try {
    for (const as of ['ro', 'rw']) {
      for (const cmd of [['connections'], ['kill', '1'], ['slow'], ['metrics'], ['verify'], ['compact'], ['backup', 'x.kdb']]) {
        const r = await s.admin(as, ...cmd);
        assert.equal(r.code, 1, `${as} ${cmd}`); assert.match(r.err, /AUTH user (reader|writer) \((readonly|readwrite)\) may not do this; it needs the admin role/);
      }
    }
    // The protocol requests themselves, for clients other than the CLI.
    const ro = await RemoteDB.connect(s.address, {token: RO});
    await assert.rejects(ro.compact(), /AUTH/);
    await ro.close();
    // Usage: 2.
    for (const args of [[], ['nope'], ['status', 'extra'], ['kill'], ['kill', 'abc'], ['backup'], ['slow', 'x'], ['--bogus', 'status'], ['--cert', 'c.pem', 'status']]) {
      const r = await s.admin('admin', ...args); assert.equal(r.code, 2, `usage: ${args}`); assert.match(r.err, /usage: kestro-server admin/);
    }
    let r = await s.admin(null, '--help'); assert.equal(r.code, 0); assert.match(r.out, /commands:/);
    // Errors: 1.
    r = await s.admin('admin', 'kill', '999999'); assert.equal(r.code, 1); assert.match(r.err, /no connection 999999/);
    r = await s.admin('admin', '--user', 'reader', 'status'); assert.equal(r.code, 1); assert.match(r.err, /not user reader's/);
    r = await s.admin('admin', '--user', 'admin', 'status'); assert.equal(r.code, 0);
    r = await s.admin(null, 'status'); assert.equal(r.code, 1); assert.match(r.err, /AUTH/);
    await writeFile(join(s.dir, 'bad.token'), 'not-the-token-0123456789\n');
    r = await s.admin('bad', 'status'); assert.equal(r.code, 1); assert.match(r.err, /AUTH invalid token/);
    const p = spawn(serverBinary, ['admin', '--server', 'tcp://127.0.0.1:1', 'status'], {stdio: ['ignore', 'pipe', 'pipe']}); let e = ''; p.stderr.on('data', d => { e += d; });
    assert.equal(await new Promise(res => p.on('close', res)), 1); assert.match(e, /IO/);
  } finally { await s.stop(); }
});

test('admin: kill closes the connection, rolls back its transaction, and wakes a client that keeps its end open', async () => {
  const s = await start();
  try {
    const setup = await RemoteDB.connect(s.address, {token: ADMIN});
    await setup.execute('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)'); await setup.close();
    const victim = await RemoteDB.connect(s.address, {token: RW, reconnect: false});
    await victim.execute('BEGIN'); await victim.execute('INSERT INTO t (v) VALUES (?)', ['uncommitted']);
    const id = (await victim.stats()).connectionId;
    let r = await s.admin('admin', 'kill', String(id)); assert.equal(r.code, 0, r.err); assert.match(r.out, new RegExp(`closed connection ${id}`));
    await assert.rejects(victim.execute('COMMIT'), /CLOSED|IO/);
    await until(async () => !(await connections(s)).some(c => c.id === id)).catch(async () => { throw Error('victim still listed: ' + JSON.stringify(await connections(s))); });
    const check = await RemoteDB.connect(s.address, {token: RO});
    assert.deepEqual(await check.query('SELECT COUNT(*) AS n FROM t'), [{n: 0}], 'the open transaction was rolled back');
    // A raw client that logged in and keeps its end open (half-open): the server still ends it.
    const raw = net.connect({host: '127.0.0.1', port: Number(s.address.split(':').pop()), allowHalfOpen: true});
    await new Promise(res => raw.once('connect', res));
    let ended = false; raw.on('data', () => {}); raw.on('end', () => { ended = true; }); raw.on('error', () => { ended = true; });
    const login = Buffer.alloc(5 + RW.length); login.writeUInt32LE(1 + RW.length, 0); login[4] = 'A'.charCodeAt(0); login.write(RW, 5);
    raw.write(login);
    const rawId = await until(async () => (await connections(s)).find(c => c.user === 'writer')?.id).catch(() => { throw Error('the raw client did not log in'); });
    r = await s.admin('admin', 'kill', String(rawId)); assert.equal(r.code, 0, r.err);
    await until(() => ended).catch(() => { throw Error('the half-open client was not ended'); });
    await until(async () => !(await connections(s)).some(c => c.id === rawId)).catch(async () => { throw Error('still listed: ' + JSON.stringify(await connections(s))); });  // its thread finished
    raw.destroy();
    r = await s.admin('admin', 'metrics'); assert.match(r.out, /^kestrodb_connections_killed_total 2$/m);
    await check.close();
  } finally { await s.stop(); }
});

test('slow-query log: placeholders and no values by default, admin slow, JSON lines', async () => {
  const s = await start(['--slow-query-ms', '1', '--slow-query-log', '$DIR/slow.log']);
  try {
    const db = await RemoteDB.connect(s.address, {token: ADMIN});
    await db.execute('CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, secret TEXT)');
    await db.execute('INSERT INTO people (name, secret) VALUES (?, ?)', ['ann', 'param-secret-value']);
    const slow = "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 100000) SELECT COUNT(*) AS k FROM c WHERE n > ? AND 'inline-secret' <> ?";
    await db.query(slow, [10, 'param-secret-value']);
    await assert.rejects(db.query('WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c) SELECT COUNT(*) AS k FROM c WHERE n > ?', [1]), /LIMIT/);  // fails after a while
    const text = await until(async () => { try { const t = await readFile(join(s.dir, 'slow.log'), 'utf8'); return t.split('\n').length >= 3 ? t : null; } catch { return null; } });
    const lines = text.trim().split('\n').map(l => JSON.parse(l));
    const ok = lines.find(l => !l.error && l.sql.startsWith('WITH')), bad = lines.find(l => l.error);
    assert.ok(ok && bad, text);
    assert.deepEqual(Object.keys(ok), ['time', 'ms', 'user', 'peer', 'connection', 'kind', 'rows', 'sql']);
    assert.match(ok.time, /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$/); assert.ok(ok.ms >= 1);
    assert.equal(ok.user, 'admin'); assert.equal(ok.peer, '127.0.0.1'); assert.equal(ok.kind, 'select'); assert.equal(ok.rows, 1);
    assert.equal(ok.sql, 'WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT n + ? FROM c WHERE n < ?) SELECT COUNT(*) AS k FROM c WHERE n > ? AND ? <> ?');
    assert.match(bad.error, /^LIMIT/); assert.equal(bad.rows, 0);
    assert.doesNotMatch(text, /secret-value|inline-secret/, 'no values in the log');
    const r = await s.admin('admin', 'slow', '5'); assert.equal(r.code, 0, r.err);
    const entries = r.out.trim().split('\n').map(l => JSON.parse(l));
    assert.ok(entries.length >= 2 && entries.length <= 5); assert.ok(entries.some(e => e.sql === ok.sql));
    const j = JSON.parse((await s.admin('admin', 'slow', '--json')).out); assert.equal(j.thresholdMs, 1); assert.ok(j.logged >= 2); assert.equal(j.dropped, 0);
    const m = (await s.admin('admin', 'metrics')).out;
    assert.match(m, /^kestrodb_slow_queries_total [2-9]\d*$/m);
    await db.close();
  } finally { await s.stop(); }
});

test('slow-query log: --slow-query-params logs the values; rotation at --slow-query-log-max keeps one .1 file', async () => {
  const s = await start(['--slow-query-ms', '1', '--slow-query-log', '$DIR/slow.log', '--slow-query-log-max', '1', '--slow-query-params']);
  try {
    const db = await RemoteDB.connect(s.address, {token: ADMIN});
    const pad = 'p'.repeat(4000);
    const q = `WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 3000) SELECT COUNT(*) AS k FROM c WHERE '${pad}' <> ?`;
    await db.query(q, ['shown-value']);
    for (let i = 0; i < 2000 && !existsSync(join(s.dir, 'slow.log.1')); i++) await db.query(q, ['v' + i]);
    await until(() => existsSync(join(s.dir, 'slow.log.1')));
    const first = await readFile(join(s.dir, 'slow.log.1'), 'utf8');
    assert.ok((await stat(join(s.dir, 'slow.log.1'))).size >= 1 << 20);
    const line = JSON.parse(first.split('\n')[0]);
    assert.equal(line.params, '["shown-value"]'); assert.ok(line.sql.includes(pad.slice(0, 3000)));
    assert.ok(line.sql.length <= 4096 && line.truncated === true, 'the statement is cut at 4 KB');
    for (const l of first.trim().split('\n')) JSON.parse(l);
    await until(() => existsSync(join(s.dir, 'slow.log')));
    assert.ok((await stat(join(s.dir, 'slow.log'))).size < 1 << 20);
    const m = (await s.admin('admin', 'metrics')).out;
    assert.match(m, /^kestrodb_slow_query_log_rotations_total [1-9]\d*$/m);
    await db.close();
  } finally { await s.stop(); }
});

test('admin: over a Unix socket', {skip: process.platform === 'win32' && 'Unix sockets: Linux and macOS'}, async () => {
  const s = await start(['--unix', '$DIR/s.sock']);
  try {
    const r = await s.admin('admin', '--server', 'unix://' + join(s.dir, 's.sock'), 'connections', '--json');
    assert.equal(r.code, 0, r.err);
    assert.deepEqual(JSON.parse(r.out).connections.map(c => [c.peer, c.self]), [['unix', true]]);
  } finally { await s.stop(); }
});

// TLS: the server's certificate checked against --ca; a user bound to a client certificate logs in by it.
const openssl = [process.env.KESTRO_OPENSSL, 'openssl', 'C:\\Program Files\\Git\\mingw64\\bin\\openssl.exe', 'C:\\Program Files\\Git\\usr\\bin\\openssl.exe'].filter(Boolean).find(c => spawnSync(c, ['version'], {encoding: 'utf8'}).status === 0);
test('admin: over TLS with --ca, and with a client certificate (--cert/--key)', {skip: !openssl && 'openssl not found'}, async () => {
  const pki = async dir => {
    const ssl = (...a) => { const r = spawnSync(openssl, a, {cwd: dir, encoding: 'utf8'}); if (r.status !== 0) throw Error(r.stderr); };
    ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-subj', '/CN=KestroDB Admin Test CA', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
    for (const [name, cn, ext] of [['server', 'localhost', 'basicConstraints=CA:FALSE\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n'], ['ops', 'ops', 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n']]) {
      await writeFile(join(dir, name + '.ext'), ext);
      ssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', name + '.key', '-out', name + '.csr', '-subj', '/CN=' + cn);
      ssl('x509', '-req', '-in', name + '.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', name + '.pem', '-days', '2', '-extfile', name + '.ext');
    }
  };
  const s = await start(['--tls-cert', '$DIR/server.pem', '--tls-key', '$DIR/server.key', '--tls-client-ca', '$DIR/ca.pem', '--tls-client-cert', 'optional'], {prepare: pki, users: 'ops admin - cert=cn:ops\n'});
  try {
    assert.match(s.address, /^tls:/);
    let r = await s.admin('admin', '--ca', join(s.dir, 'ca.pem'), 'status');
    assert.equal(r.code, 0, r.err); assert.match(r.out, /^server\s+tls:\/\/localhost:\d+ \(TLS\)$/m); assert.match(r.out, /^user\s+admin \(admin\)/m);
    r = await s.admin(null, '--ca', join(s.dir, 'ca.pem'), '--cert', join(s.dir, 'ops.pem'), '--key', join(s.dir, 'ops.key'), 'connections', '--json');
    assert.equal(r.code, 0, r.err);
    const me = JSON.parse(r.out).connections.find(c => c.self); assert.equal(me.user, 'ops'); assert.equal(me.tls, true);
    r = await s.admin('admin', 'status'); assert.equal(r.code, 1, 'the server certificate is not trusted without --ca');
  } finally { await s.stop(); }
});
