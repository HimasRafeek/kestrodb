// Mutual TLS (--tls-client-ca, --tls-client-cert required|optional): a throwaway CA, server and
// client certificates made with openssl at test time (Schannel on Windows, OpenSSL elsewhere).
// A good certificate is accepted; none, one from another CA or one not for client use is
// refused with a clear AUTH error before any request; users bound to certificates (cert= in the
// users file) log in by them alone or need token and certificate both, with their role's
// permissions; followers and cluster members show theirs with --replica-cert/--replica-key.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {mkdtemp, rm, writeFile, readFile} from 'node:fs/promises';
import {X509Certificate} from 'node:crypto';
import net from 'node:net';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const ADMIN = 'admin-token-0123456789abcdef0123456789', BOB = 'bob-token-0123456789abcdef0123456789', WRITER = 'writer-token-0123456789abcdef0123456789';
const sleep = ms => new Promise(r => setTimeout(r, ms));

function findTool(envName, candidates, args = ['--version']) {
  for (const c of [process.env[envName], ...candidates].filter(Boolean)) {
    const r = spawnSync(c, args, {encoding: 'utf8'});
    if (r.status === 0 && !/not found|Microsoft Store/i.test(r.stdout + r.stderr)) return c;
  }
  return null;
}
const openssl = findTool('KESTRO_OPENSSL', ['openssl', 'C:\\Program Files\\Git\\mingw64\\bin\\openssl.exe', 'C:\\Program Files\\Git\\usr\\bin\\openssl.exe'], ['version']);
const python = findTool('KESTRO_PYTHON', process.platform === 'win32' ? ['py', 'python'] : ['python3']);
const hasTls = (() => {
  const r = spawnSync(serverBinary, ['--db', join(tmpdir(), 'kdb-mtls-probe.kdb'), '--tls-cert', 'x', '--tls-key', 'x'], {encoding: 'utf8'});
  return !/UNSUPPORTED TLS/.test(r.stdout + r.stderr);
})();
const skip = !openssl ? 'openssl not found (set KESTRO_OPENSSL)' : !hasTls ? 'this kestro-server has no TLS' : false;

let dir;
const f = name => join(dir, name);
function ssl(...args) {
  const r = spawnSync(openssl, args, {cwd: dir, encoding: 'utf8'});
  if (r.status !== 0) throw Error(`openssl ${args.join(' ')}: ${r.stdout}${r.stderr}`);
}
// A certificate signed by ca (name.pem, name.key) with the given extensions.
function issue(name, cn, ca, ext) {
  ssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', name + '.key', '-out', name + '.csr', '-subj', '/CN=' + cn);
  ssl('x509', '-req', '-in', name + '.csr', '-CA', ca + '.pem', '-CAkey', ca + '.key', '-CAcreateserial', '-out', name + '.pem', '-days', '2', '-extfile', ext);
}
const pki = {};
async function makePki() {
  dir = await mkdtemp(join(tmpdir(), 'kdb-mtls-'));
  await writeFile(f('server.ext'), 'basicConstraints=CA:FALSE\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n');
  await writeFile(f('client.ext'), 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n');
  for (const [name, cn] of [['ca', 'KestroDB mTLS Test CA'], ['rogue', 'KestroDB Rogue CA']])
    ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', name + '.key', '-out', name + '.pem', '-days', '2', '-subj', '/CN=' + cn, '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  issue('server', 'localhost', 'ca', 'server.ext');
  for (const n of ['alice', 'bob', 'carol', 'replica']) issue(n, n === 'replica' ? 'kestro-replica' : n, 'ca', 'client.ext');
  issue('rogue-alice', 'alice', 'rogue', 'client.ext');  // the same name, from a CA the server does not trust
  issue('server-only', 'alice', 'ca', 'server.ext');      // the right CA, but not for client authentication
  for (const n of ['ca', 'alice', 'bob', 'carol', 'rogue-alice', 'server-only']) pki[n] = {cert: await readFile(f(n + '.pem')), key: await readFile(f(n + '.key'))};
  const bobFingerprint = new X509Certificate(pki.bob.cert).fingerprint256;  // AB:CD:...
  await writeFile(f('users'), [
    `admin admin ${ADMIN}`,
    'alice readonly - cert=cn:alice',                       // her certificate alone
    `bob readwrite ${BOB} cert=sha256:${bobFingerprint}`,   // token and certificate both
    `writer readwrite ${WRITER} tables=notes`,
    'repl replica - cert=CN:kestro-replica',
  ].join('\n') + '\n');
}

let serial = 0;
async function start(args, {expectExit = false, users = true} = {}) {
  const db = f(`db${++serial}.kdb`);
  const child = spawn(serverBinary, ['--db', db, ...(users ? ['--users', f('users')] : []), '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const exited = new Promise(resolve => child.on('exit', code => resolve(code)));
  if (expectExit) { child.stdout.on('data', d => { out += d; }); const code = await exited; return {code, log: out + err}; }
  const port = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY (?:tls|tcp) \S+:(\d+)/.exec(out); if (m) resolve(Number(m[1])); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${out}${err}`)));
  });
  return {port, log: () => out + err, stop: async () => { child.stdin.end(); return exited; }};
}
const serverArgs = (...more) => ['--create', '--tls-cert', f('server.pem'), '--tls-key', f('server.key'), '--tls-client-ca', f('ca.pem'), ...more];
const client = (who, extra = {}) => ({tls: {ca: pki.ca.cert, ...(who ? {cert: pki[who].cert, key: pki[who].key} : {})}, timeout: 10000, reconnect: false, ...extra});
const connect = (port, who, extra) => RemoteDB.connect(`tls://localhost:${port}`, client(who, extra));
// The first call's result (or error) on a connection that sends no token.
async function firstCall(port, who, fn = db => db.stats()) {
  const db = await connect(port, who);
  try { return await fn(db); } finally { await db.close(); }
}

test('mutual TLS', {skip}, async t => {
  await makePki();
  try {
    await t.test('required: a good certificate is accepted; none, another CA or the wrong usage is refused before any request', async () => {
      const s = await start(serverArgs());
      try {
        // An unmapped certificate from the CA, with a token: the token's user.
        const db = await connect(s.port, 'carol', {token: ADMIN});
        await db.sql('CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT)');
        await db.sql('INSERT INTO notes (body) VALUES (?)', ['first']);
        await db.set('kv', 'a', {n: 1});
        const st = await db.stats();
        assert.equal(st.tls, true); assert.equal(st.user, 'admin');
        await db.close();
        // No certificate: refused, even with a valid token.
        await assert.rejects(connect(s.port, null, {token: ADMIN}), e => e.code === 'AUTH' && /TLS client certificate required/.test(e.message));
        await assert.rejects(firstCall(s.port, null), e => e.code === 'AUTH' && /TLS client certificate required/.test(e.message));
        // A certificate from another CA (with the name of a real user): refused.
        await assert.rejects(connect(s.port, 'rogue-alice', {token: ADMIN}), e => e.code === 'AUTH' && /TLS client certificate refused: not issued by a trusted CA/.test(e.message));
        await assert.rejects(firstCall(s.port, 'rogue-alice'), e => e.code === 'AUTH' && /client certificate refused/.test(e.message));
        // The right CA, but a server-only certificate (extended key usage serverAuth).
        await assert.rejects(connect(s.port, 'server-only', {token: ADMIN}), e => e.code === 'AUTH' && /client certificate refused/.test(e.message));
        // Plain text on the TLS port: no reply, the connection closes.
        await assert.rejects(RemoteDB.connect(`tcp://127.0.0.1:${s.port}`, {token: ADMIN, timeout: 5000, reconnect: false}));
        assert.match(s.log(), /KESTRODB TLS client certificate "alice" from 127\.0\.0\.1: refused/);
        assert.match(s.log(), /KESTRODB TLS client certificate from 127\.0\.0\.1: required/);
        // And the server is still fine.
        const again = await connect(s.port, 'carol', {token: ADMIN});
        assert.deepEqual((await again.sql('SELECT COUNT(*) AS n FROM notes')).rows, [{n: 1}]);
        await again.close();
      } finally { assert.equal(await s.stop(), 0); }
    });

    await t.test('certificate -> user: alone (token -), with a token (both required), and the user\'s permissions', async () => {
      const s = await start(serverArgs());
      try {
        const admin = await connect(s.port, 'carol', {token: ADMIN});
        await admin.sql('CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT)');
        await admin.sql('CREATE TABLE secrets (id INTEGER PRIMARY KEY, body TEXT)');
        await admin.sql('INSERT INTO notes (body) VALUES (?)', ['hello']);
        await admin.set('kv', 'k', {v: 1});
        await admin.close();
        // alice: her certificate alone logs her in, read-only.
        const alice = await connect(s.port, 'alice');
        const st = await alice.stats();
        assert.equal(st.user, 'alice'); assert.equal(st.role, 'readonly');
        assert.deepEqual((await alice.sql('SELECT body FROM notes')).rows, [{body: 'hello'}]);
        assert.deepEqual((await alice.get('kv', 'k')).value, {v: 1});
        await assert.rejects(alice.sql('INSERT INTO notes (body) VALUES (?)', ['no']), e => e.code === 'AUTH' && /alice is read-only/.test(e.message));
        await assert.rejects(alice.set('kv', 'k', {v: 2}), e => e.code === 'AUTH' && /read-only/.test(e.message));
        await assert.rejects(alice.verify(), e => e.code === 'AUTH' && /admin role/.test(e.message));
        await alice.close();
        // A token switches the connection to that user (alice's certificate, admin's token).
        const switched = await connect(s.port, 'alice', {token: ADMIN});
        assert.equal((await switched.stats()).user, 'admin'); await switched.close();
        // bob: token and certificate both.
        const bob = await connect(s.port, 'bob', {token: BOB});
        assert.equal((await bob.stats()).user, 'bob');
        await bob.sql('INSERT INTO notes (body) VALUES (?)', ['from bob']);
        await bob.set('kv', 'bob', {b: true});
        await assert.rejects(bob.sql('DROP TABLE notes'), e => e.code === 'AUTH' && /admin role/.test(e.message));
        await bob.close();
        await assert.rejects(connect(s.port, 'carol', {token: BOB}), e => e.code === 'AUTH' && /user bob must also show its TLS client certificate/.test(e.message));
        await assert.rejects(connect(s.port, 'alice', {token: BOB}), e => e.code === 'AUTH' && /bob must also show/.test(e.message));
        await assert.rejects(firstCall(s.port, 'bob'), e => e.code === 'AUTH' && /send the access token first/.test(e.message));  // his certificate alone is not enough
        // A certificate-bound user cannot be reached by a token at all: alice has none.
        await assert.rejects(connect(s.port, 'carol', {token: '-'}), e => e.code === 'AUTH' && /invalid token/.test(e.message));
        // An unmapped certificate without a token is not logged in.
        await assert.rejects(firstCall(s.port, 'carol'), e => e.code === 'AUTH' && /send the access token first/.test(e.message));
        // A table-limited token user, with any good certificate.
        const writer = await connect(s.port, 'carol', {token: WRITER});
        await writer.sql('INSERT INTO notes (body) VALUES (?)', ['writer']);
        await assert.rejects(writer.sql('SELECT * FROM secrets'), e => e.code === 'AUTH' && /no access to secrets/.test(e.message));
        await writer.close();
        const check = await connect(s.port, 'alice');
        assert.deepEqual((await check.sql('SELECT body FROM notes ORDER BY id')).rows.map(r => r.body), ['hello', 'from bob', 'writer']);
        await check.close();
      } finally { assert.equal(await s.stop(), 0); }
    });

    await t.test('optional: no certificate is fine (token), a bad one is still refused, a mapped one logs in', async () => {
      const s = await start(serverArgs('--tls-client-cert', 'optional'));
      try {
        const plain = await connect(s.port, null, {token: ADMIN});
        assert.equal((await plain.stats()).user, 'admin'); await plain.close();
        assert.equal((await firstCall(s.port, 'alice')).user, 'alice');
        await assert.rejects(connect(s.port, 'rogue-alice', {token: ADMIN}), e => e.code === 'AUTH' && /client certificate refused/.test(e.message));
        await assert.rejects(connect(s.port, null, {token: BOB}), e => e.code === 'AUTH' && /bob must also show/.test(e.message));
      } finally { assert.equal(await s.stop(), 0); }
    });

    await t.test('no users: required client certificates are the authentication', async () => {
      const s = await start(serverArgs(), {users: false});
      try {
        assert.match(s.log(), /every client with a valid certificate is an administrator/);
        const st = await firstCall(s.port, 'carol');
        assert.equal(st.tls, true); assert.equal(st.user, null);
        await assert.rejects(firstCall(s.port, null), /TLS client certificate required/);
      } finally { assert.equal(await s.stop(), 0); }
    });

    await t.test('startup checks', async () => {
      let r = await start(['--create', '--tls-client-ca', f('ca.pem')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /--tls-client-ca needs TLS/);
      r = await start(['--create', '--tls-cert', f('server.pem'), '--tls-key', f('server.key'), '--tls-client-cert', 'required'], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /--tls-client-cert needs --tls-client-ca/);
      r = await start(serverArgs('--tls-client-cert', 'sometimes'), {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /required or optional/);
      r = await start(['--create', '--tls-cert', f('server.pem'), '--tls-key', f('server.key'), '--tls-client-ca', f('server.key')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /--tls-client-ca/);
      for (const [line, why] of [['x readonly -', /without a token \(-\) needs cert=/], ['x readonly - cert=sha256:abcd', /64 hex digits/], ['x readonly - cert=ou:x', /cert= must be/],
        ['x readonly - cert=cn:alice\ny readonly - cert=cn:alice', /share a certificate/], ['x readonly - cert=cn:a cert=cn:b', /cert= appears twice/]]) {
        await writeFile(f('bad-users'), line + '\n');
        r = await start(serverArgs('--users', f('bad-users')), {expectExit: true, users: false});
        assert.equal(r.code, 1, line); assert.match(r.log, why, line);
      }
      r = await start(['--replica-of', 'tls://127.0.0.1:1', '--replica-cert', f('replica.pem')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /--replica-cert and --replica-key go together/);
      r = await start(['--create', '--replica-cert', f('replica.pem'), '--replica-key', f('replica.key')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /--replica-cert goes with --replica-of or --cluster/);
      r = await start(['--replica-of', 'tls://127.0.0.1:1', '--replica-cert', f('replica.pem'), '--replica-key', f('alice.key')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /--replica-(cert|key)|does not match/);
    });

    await t.test('replication: a follower shows its certificate (--replica-cert), mapped to a replica user without a token', async () => {
      const leader = await start(serverArgs());
      let follower;
      try {
        const L = await connect(leader.port, 'carol', {token: ADMIN});
        await L.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
        for (let i = 0; i < 50; i++) await L.sql('INSERT INTO t (v) VALUES (?)', ['r' + i]);
        const replicaArgs = ['--replica-of', `tls://127.0.0.1:${leader.port}`, '--replica-ca', f('ca.pem')];
        follower = await start([...replicaArgs, '--replica-cert', f('replica.pem'), '--replica-key', f('replica.key')]);
        assert.match(follower.log(), /KESTRODB SEEDED/);
        const F = await RemoteDB.connect(`tcp://127.0.0.1:${follower.port}`, {token: ADMIN});
        for (let i = 50; i < 80; i++) await L.sql('INSERT INTO t (v) VALUES (?)', ['r' + i]);
        const want = (await L.stats()).txid;
        for (const until = Date.now() + 15000; (await F.stats()).txid < want; await sleep(20)) if (Date.now() > until) throw Error('follower did not catch up: ' + JSON.stringify((await F.stats()).replica));
        assert.deepEqual((await F.sql('SELECT COUNT(*) AS n FROM t')).rows, [{n: 80}]);
        assert.equal((await F.stats()).replica.state, 'streaming');
        await F.close(); await L.close();
        // Without a certificate, or with one the leader does not map to a replica user.
        let r = await start([...replicaArgs], {expectExit: true});
        assert.equal(r.code, 1); assert.match(r.log, /AUTH TLS client certificate required/);
        r = await start([...replicaArgs, '--replica-cert', f('alice.pem'), '--replica-key', f('alice.key')], {expectExit: true});
        assert.equal(r.code, 1); assert.match(r.log, /AUTH user alice \(readonly\) may not replicate/);
      } finally { if (follower) assert.equal(await follower.stop(), 0); assert.equal(await leader.stop(), 0); }
    });

    await t.test('a cluster whose members link over mutual TLS (no tokens: their certificates)', async () => {
      const ports = [];
      for (let i = 0; i < 3; i++) ports.push(await new Promise(resolve => { const srv = net.createServer(); srv.listen(0, '127.0.0.1', () => { const p = srv.address().port; srv.close(() => resolve(p)); }); }));
      const addrs = ports.map(p => 'tls://127.0.0.1:' + p);  // not localhost: Windows takes seconds to refuse ::1
      const nodes = [];
      try {
        for (let i = 0; i < 3; i++) {
          const child = spawn(serverBinary, ['--db', f(`cluster${i}.kdb`), '--create', '--users', f('users'), '--tcp', '127.0.0.1:' + ports[i], '--tls-cert', f('server.pem'), '--tls-key', f('server.key'), '--tls-client-ca', f('ca.pem'),
            '--cluster', addrs.join(','), '--cluster-self', addrs[i], '--replica-ca', f('ca.pem'), '--replica-cert', f('replica.pem'), '--replica-key', f('replica.key'), '--election-timeout', '1000ms', '--stop-on-stdin-eof'], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
          const node = {child, log: '', exited: new Promise(r => child.on('exit', r))};
          child.stdout.on('data', d => { node.log += d; }); child.stderr.on('data', d => { node.log += d; });
          nodes.push(node);
        }
        const open = a => RemoteDB.connect(a, client('carol', {token: ADMIN, timeout: 2000}));
        let leader;
        for (const until = Date.now() + 30000; !leader; await sleep(100)) {
          if (Date.now() > until) throw Error('no leader: ' + nodes.map(n => n.log).join('\n'));
          for (const a of addrs) { try { const c = await open(a); const st = await c.stats(); await c.close(); if (st.cluster?.role === 'leader') leader = a; } catch {} }
        }
        const db = await open(leader);
        await db.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
        for (let i = 0; i < 10; i++) await db.sql('INSERT INTO t (v) VALUES (?)', ['r' + i]);  // each confirmed by a member over mutual TLS
        assert.equal((await db.stats()).cluster.members, 3);
        await db.close();
        for (const a of addrs) {
          for (const until = Date.now() + 10000; ; await sleep(50)) {
            const c = await open(a); const n = (await c.sql('SELECT COUNT(*) AS n FROM t')).rows[0].n; await c.close();
            if (n === 10) break;
            if (Date.now() > until) throw Error(`member ${a} has ${n} rows`);
          }
        }
      } catch (e) { e.message += '\n' + nodes.map(n => n.log).join('\n'); throw e; }
      finally { for (const n of nodes) { n.child.stdin.end(); await n.exited; } }
    });

    await t.test('Python client with a client certificate', {skip: python ? false : 'Python not found'}, async () => {
      const s = await start(serverArgs());
      try {
        const code = `import sys; sys.path.insert(0, ${JSON.stringify(join(root, 'clients/python'))}); import kestrodb, json
tls = {'cafile': ${JSON.stringify(f('ca.pem'))}, 'certfile': ${JSON.stringify(f('alice.pem'))}, 'keyfile': ${JSON.stringify(f('alice.key'))}}
c = kestrodb.connect('tls://localhost:${s.port}', tls=tls)
st = c.stats(); print(json.dumps({'user': st['user'], 'tls': st['tls']}, separators=(',', ':')))
try:
    kestrodb.connect('tls://localhost:${s.port}', token='${ADMIN}', tls={'cafile': tls['cafile']}).stats(); print('BAD')
except Exception as e:
    print('refused' if 'client certificate required' in str(e) else 'other: ' + str(e))`;
        const r = await new Promise(resolve => {
          const c = spawn(python, ['-c', code], {stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true}); let out = '';
          c.stdout.on('data', d => { out += d; }); c.stderr.on('data', d => { out += d; }); c.on('close', code => resolve({code, out}));
        });
        assert.match(r.out, /\{"user":"alice","tls":true\}/, r.out); assert.match(r.out, /refused/, r.out);
      } finally { assert.equal(await s.stop(), 0); }
    });
  } finally { await rm(dir, {recursive: true, force: true}); }
});
