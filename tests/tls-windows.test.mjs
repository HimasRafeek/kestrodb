// TLS on Windows (Schannel, src/tls_windows.hpp): a throwaway CA, an intermediate and server
// certificates made with openssl; the server takes PEM keys in every common format and PKCS#12;
// clients (Node, PHP, Python: standard TLS libraries) verify it; wrong CAs, wrong names, plain
// text and slow handshakes are refused; a follower replicates over tls:// with --replica-ca.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {mkdtemp, rm, writeFile, readFile} from 'node:fs/promises';
import net from 'node:net';
import tls from 'node:tls';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const ADMIN = 'admin-token-0123456789', REPLICA = 'replica-token-0123456789';
const sleep = ms => new Promise(r => setTimeout(r, ms));

function findTool(envName, candidates, args = ['--version']) {
  for (const c of [process.env[envName], ...candidates].filter(Boolean)) {
    const r = spawnSync(c, args, {encoding: 'utf8'});
    if (r.status === 0 && !/not found|Microsoft Store/i.test(r.stdout + r.stderr)) return c;
  }
  return null;
}
const windows = process.platform === 'win32';
const openssl = windows ? findTool('KESTRO_OPENSSL', ['openssl', 'C:\\Program Files\\Git\\mingw64\\bin\\openssl.exe', 'C:\\Program Files\\Git\\usr\\bin\\openssl.exe'], ['version']) : null;
const php = windows ? findTool('KESTRO_PHP', ['php']) : null;
const python = windows ? findTool('KESTRO_PYTHON', ['py', 'python']) : null;
const skip = !windows ? 'Windows only (Schannel)' : !openssl ? 'openssl not found (set KESTRO_OPENSSL)' : false;

// Certificates, once for every test.
let dir;
const pki = {};
function ssl(...args) {
  const r = spawnSync(openssl, args, {cwd: dir, encoding: 'utf8'});
  if (r.status !== 0) throw Error(`openssl ${args.join(' ')}: ${r.stdout}${r.stderr}`);
}
async function makePki() {
  dir = await mkdtemp(join(tmpdir(), 'kdb-wintls-'));
  await writeFile(join(dir, 'ca.ext'), 'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n');
  await writeFile(join(dir, 'server.ext'), 'basicConstraints=CA:FALSE\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n');
  await writeFile(join(dir, 'other.ext'), 'basicConstraints=CA:FALSE\nsubjectAltName=DNS:other.example\nextendedKeyUsage=serverAuth\n');
  ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-subj', '/CN=KestroDB Test Root CA', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'wrong-ca.key', '-out', 'wrong-ca.pem', '-days', '2', '-subj', '/CN=KestroDB Wrong CA', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  // An intermediate CA: the server sends it with its certificate (cert file = leaf + intermediate).
  ssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'inter.key', '-out', 'inter.csr', '-subj', '/CN=KestroDB Test Intermediate');
  ssl('x509', '-req', '-in', 'inter.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'inter.pem', '-days', '2', '-extfile', 'ca.ext');
  ssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'rsa.key', '-out', 'rsa.csr', '-subj', '/CN=localhost');
  ssl('x509', '-req', '-in', 'rsa.csr', '-CA', 'inter.pem', '-CAkey', 'inter.key', '-CAcreateserial', '-out', 'rsa.pem', '-days', '2', '-extfile', 'server.ext');
  await writeFile(join(dir, 'rsa-chain.pem'), (await readFile(join(dir, 'rsa.pem'), 'utf8')) + (await readFile(join(dir, 'inter.pem'), 'utf8')));
  ssl('rsa', '-in', 'rsa.key', '-traditional', '-out', 'rsa-pkcs1.key');
  ssl('ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', 'ec.key');  // SEC1 "EC PRIVATE KEY"
  ssl('pkcs8', '-topk8', '-nocrypt', '-in', 'ec.key', '-out', 'ec-pkcs8.key');
  ssl('req', '-new', '-key', 'ec.key', '-out', 'ec.csr', '-subj', '/CN=localhost');
  ssl('x509', '-req', '-in', 'ec.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'ec.pem', '-days', '2', '-extfile', 'server.ext');
  ssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'other.key', '-out', 'other.csr', '-subj', '/CN=other.example');
  ssl('x509', '-req', '-in', 'other.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'other.pem', '-days', '2', '-extfile', 'other.ext');
  ssl('pkcs12', '-export', '-in', 'rsa.pem', '-inkey', 'rsa.key', '-certfile', 'inter.pem', '-out', 'rsa.pfx', '-passout', 'pass:pfx-secret-123');
  ssl('pkcs12', '-export', '-in', 'ec.pem', '-inkey', 'ec.key', '-out', 'ec-nopass.pfx', '-passout', 'pass:');
  await writeFile(join(dir, 'pfx-password'), 'pfx-secret-123\n');
  await writeFile(join(dir, 'users'), `admin admin ${ADMIN}\nrepl replica ${REPLICA}\n`);
  await writeFile(join(dir, 'replica-token'), REPLICA + '\n');
  pki.ca = await readFile(join(dir, 'ca.pem'));
  pki.wrongCa = await readFile(join(dir, 'wrong-ca.pem'));
}
const f = name => join(dir, name);

let serial = 0;
async function start(args, {expectExit = false} = {}) {
  const db = f(`db${++serial}.kdb`);
  const child = spawn(serverBinary, ['--db', db, '--users', f('users'), '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
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
async function roundTrip(address, tls) {
  const db = await RemoteDB.connect(address, {token: ADMIN, tls, timeout: 10000});
  try {
    await db.set('kv', 'hello', {n: 1});
    assert.deepEqual((await db.get('kv', 'hello')).value, {n: 1});
    return (await db.stats()).tls;
  } finally { await db.close(); }
}

test('Windows TLS', {skip}, async t => {
  await makePki();
  try {
    await t.test('PEM certificate chain and PKCS#8 RSA key: SQL and key/value over tls://, stats().tls', async () => {
      const s = await start(['--create', '--tls-cert', f('rsa-chain.pem'), '--tls-key', f('rsa.key')]);
      try {
        assert.match(s.log(), /KESTRODB READY tls 127\.0\.0\.1:/);
        for (const host of ['localhost', '127.0.0.1']) {
          const db = await RemoteDB.connect(`tls://${host}:${s.port}`, {token: ADMIN, tls: {ca: pki.ca}});
          try {
            if (host === 'localhost') {
              await db.sql('CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT, n INTEGER)');
              const big = 'x'.repeat(300000);  // many TLS records each way
              for (let i = 0; i < 20; i++) await db.sql('INSERT INTO notes (body, n) VALUES (?, ?)', [i === 7 ? big : `note ${i}`, i]);
              await db.batch(Array.from({length: 200}, (_, i) => ({type: 'set', collection: 'kv', key: 'k' + i, value: {i}})));
            }
            assert.deepEqual((await db.sql('SELECT COUNT(*) AS c, SUM(n) AS s FROM notes')).rows, [{c: 20, s: 190}]);
            assert.equal((await db.sql('SELECT body FROM notes WHERE n = 7')).rows[0].body.length, 300000);
            assert.deepEqual((await db.get('kv', 'k199')).value, {i: 199});
            assert.equal((await db.getMany('kv', ['k0', 'k1', 'nope'])).filter(Boolean).length, 2);
            // Pipelined requests.
            const got = await Promise.all(Array.from({length: 100}, (_, i) => db.get('kv', 'k' + i)));
            assert.deepEqual(got.map(g => g.value.i), Array.from({length: 100}, (_, i) => i));
            const st = await db.stats();
            assert.equal(st.tls, true); assert.equal(st.user, 'admin');
          } finally { await db.close(); }
        }
        // Many connections at once.
        const many = await Promise.all(Array.from({length: 16}, () => RemoteDB.connect(`tls://localhost:${s.port}`, {token: ADMIN, tls: {ca: pki.ca}})));
        await Promise.all(many.map((db, i) => db.set('many', 'c' + i, i)));
        await Promise.all(many.map(db => db.close()));
      } finally { assert.equal(await s.stop(), 0); }
    });

    await t.test('key formats: PKCS#1 RSA, SEC1 EC, PKCS#8 EC, PKCS#12 with and without a password', async () => {
      const cases = [
        ['--tls-cert', f('rsa-chain.pem'), '--tls-key', f('rsa-pkcs1.key')],
        ['--tls-cert', f('ec.pem'), '--tls-key', f('ec.key')],
        ['--tls-cert', f('ec.pem'), '--tls-key', f('ec-pkcs8.key')],
        ['--tls-pfx', f('rsa.pfx'), '--tls-pfx-password-file', f('pfx-password')],
        ['--tls-pfx', f('ec-nopass.pfx')],
      ];
      for (const args of cases) {
        const s = await start(['--create', ...args]);
        try { assert.equal(await roundTrip(`tls://localhost:${s.port}`, {ca: pki.ca}), true, args.join(' ')); }
        finally { assert.equal(await s.stop(), 0); }
      }
    });

    await t.test('bad keys are refused at startup', async () => {
      let r = await start(['--create', '--tls-cert', f('ec.pem'), '--tls-key', f('rsa.key')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /does not match the certificate/);
      r = await start(['--create', '--tls-cert', f('rsa.pem'), '--tls-key', f('ec.key')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /EC key but the certificate/);
      r = await start(['--create', '--tls-cert', f('rsa.pem'), '--tls-key', f('rsa.pem')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /holds no PEM PRIVATE KEY/);
      ssl('pkcs8', '-topk8', '-in', 'rsa.key', '-out', 'rsa-enc.key', '-passout', 'pass:abcdefgh');
      r = await start(['--create', '--tls-cert', f('rsa.pem'), '--tls-key', f('rsa-enc.key')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /encrypted/);
      await writeFile(f('bad-password'), 'not-the-password\n');
      r = await start(['--create', '--tls-pfx', f('rsa.pfx'), '--tls-pfx-password-file', f('bad-password')], {expectExit: true});
      assert.equal(r.code, 1); assert.match(r.log, /--tls-pfx cannot be opened/);
    });

    await t.test('refused: a wrong CA, a wrong host name, plain text on the TLS port, a stalled handshake', async () => {
      const s = await start(['--create', '--auth-timeout', '1s', '--tls-cert', f('rsa-chain.pem'), '--tls-key', f('rsa.key')]);
      try {
        await assert.rejects(RemoteDB.connect(`tls://localhost:${s.port}`, {token: ADMIN, tls: {ca: pki.wrongCa}}), /certificate|self.signed|unable to verify/i);
        await assert.rejects(RemoteDB.connect(`tls://localhost:${s.port}`, {token: ADMIN}), /certificate|unable to verify|issuer/i);  // not in the system store
        await assert.rejects(RemoteDB.connect(`tls://localhost:${s.port}`, {token: ADMIN, tls: {ca: pki.ca, servername: 'other.example'}}), /altnames|does not match|Hostname/i);
        // A plain-text client gets the connection closed, promptly, with no reply.
        const t0 = Date.now();
        await assert.rejects(RemoteDB.connect(`tcp://127.0.0.1:${s.port}`, {token: ADMIN, timeout: 5000}));
        assert.ok(Date.now() - t0 < 4000, 'plain-text client refused promptly');
        const raw = await new Promise((resolve, reject) => {
          const sock = net.connect(s.port, '127.0.0.1', () => sock.write(Buffer.from('GET / HTTP/1.0\r\n\r\n')));
          let data = Buffer.alloc(0);
          sock.on('data', d => { data = Buffer.concat([data, d]); }); sock.on('error', () => {}); sock.on('close', () => resolve(data)); setTimeout(() => reject(Error('not closed')), 5000);
        });
        assert.ok(raw.length === 0 || raw[0] === 0x15, 'nothing but a TLS alert');  // 0x15: an alert record
        // A client that connects and says nothing is dropped after the auth timeout.
        const stalled = await new Promise((resolve, reject) => {
          const t1 = Date.now(), sock = net.connect(s.port, '127.0.0.1');
          sock.on('error', () => {}); sock.on('close', () => resolve(Date.now() - t1)); setTimeout(() => { sock.destroy(); reject(Error('stalled handshake not dropped')); }, 8000);
        });
        assert.ok(stalled < 5000, `dropped after ${stalled} ms`);
        // TLS 1.1 is refused by the server (TLS 1.2 is the minimum).
        await assert.rejects(new Promise((resolve, reject) => {
          const sock = tls.connect({host: '127.0.0.1', port: s.port, ca: pki.ca, servername: 'localhost', minVersion: 'TLSv1', maxVersion: 'TLSv1.1', ciphers: 'DEFAULT:@SECLEVEL=0'}, () => { sock.end(); resolve(); });
          sock.on('error', reject);
        }), e => e.code === 'ECONNRESET' || /protocol|version/i.test(e.message));  // the server drops it (Node itself would allow TLS 1.1 at SECLEVEL=0)
        // And the server is still fine.
        assert.equal(await roundTrip(`tls://localhost:${s.port}`, {ca: pki.ca}), true);
      } finally { assert.equal(await s.stop(), 0); }
    });

    await t.test('replication over tls:// with --replica-ca; wrong CA and wrong name refused by the follower', async () => {
      const leader = await start(['--create', '--tls-cert', f('rsa-chain.pem'), '--tls-key', f('rsa.key')]);
      let follower;
      try {
        const L = await RemoteDB.connect(`tls://localhost:${leader.port}`, {token: ADMIN, tls: {ca: pki.ca}});
        await L.sql('CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, n INTEGER)');
        for (let i = 0; i < 100; i++) await L.sql('INSERT INTO users (email, n) VALUES (?, ?)', [`u${i}@x`, i]);
        const big = 'y'.repeat(200000);
        await L.set('blobs', 'big', big);
        follower = await start(['--replica-of', `tls://127.0.0.1:${leader.port}`, '--replica-ca', f('ca.pem'), '--replica-token-file', f('replica-token')]);
        assert.match(follower.log(), /KESTRODB SEEDED/);
        const F = await RemoteDB.connect(`tcp://127.0.0.1:${follower.port}`, {token: ADMIN});
        for (let i = 100; i < 150; i++) await L.sql('INSERT INTO users (email, n) VALUES (?, ?)', [`u${i}@x`, i]);
        const want = (await L.stats()).txid;
        for (const until = Date.now() + 15000; (await F.stats()).txid < want; await sleep(20)) if (Date.now() > until) throw Error('follower did not catch up: ' + JSON.stringify((await F.stats()).replica));
        assert.deepEqual((await F.sql('SELECT COUNT(*) AS c, SUM(n) AS s FROM users')).rows, [{c: 150, s: 149 * 150 / 2}]);
        assert.equal((await F.get('blobs', 'big')).value.length, 200000);
        const st = await F.stats();
        assert.equal(st.replica.state, 'streaming'); assert.equal(st.readOnly, true);
        await F.close(); await L.close();
        // By DNS name too (the certificate names localhost and 127.0.0.1).
        const byName = await start(['--replica-of', `tls://localhost:${leader.port}`, '--replica-ca', f('ca.pem'), '--replica-token-file', f('replica-token')]);
        assert.match(byName.log(), /KESTRODB SEEDED/); assert.equal(await byName.stop(), 0);
        // A follower that trusts another CA, or the system store, or reaches the leader by a
        // name its certificate does not carry, never connects.
        let r = await start(['--replica-of', `tls://127.0.0.1:${leader.port}`, '--replica-ca', f('wrong-ca.pem'), '--replica-token-file', f('replica-token')], {expectExit: true});
        assert.equal(r.code, 1); assert.match(r.log, /TLS handshake with the leader failed \(certificate: not issued by a trusted CA/);
        r = await start(['--replica-of', `tls://127.0.0.1:${leader.port}`, '--replica-token-file', f('replica-token')], {expectExit: true});
        assert.equal(r.code, 1); assert.match(r.log, /certificate: not issued by a trusted CA/);
        const other = await start(['--create', '--tls-cert', f('other.pem'), '--tls-key', f('other.key')]);
        try {
          r = await start(['--replica-of', `tls://localhost:${other.port}`, '--replica-ca', f('ca.pem'), '--replica-token-file', f('replica-token')], {expectExit: true});
          assert.equal(r.code, 1); assert.match(r.log, /certificate: its name does not match localhost/);
        } finally { await other.stop(); }
        // A tls:// follower pointed at a plain leader fails cleanly.
        const plain = await start(['--create']);
        try {
          r = await start(['--replica-of', `tls://127.0.0.1:${plain.port}`, '--replica-ca', f('ca.pem'), '--replica-token-file', f('replica-token')], {expectExit: true});
          assert.equal(r.code, 1); assert.match(r.log, /TLS handshake with the leader failed/);
        } finally { await plain.stop(); }
      } finally { if (follower) assert.equal(await follower.stop(), 0); assert.equal(await leader.stop(), 0); }
    });

    await t.test('a cluster whose members link over tls:// (elections, majority-confirmed writes)', async () => {
      const ports = [];
      for (let i = 0; i < 3; i++) ports.push(await new Promise(resolve => { const srv = net.createServer(); srv.listen(0, '127.0.0.1', () => { const p = srv.address().port; srv.close(() => resolve(p)); }); }));
      const addrs = ports.map(p => 'tls://127.0.0.1:' + p);
      const nodes = [];
      try {
        for (let i = 0; i < 3; i++) {
          const child = spawn(serverBinary, ['--db', f(`cluster${i}.kdb`), '--create', '--users', f('users'), '--tcp', '127.0.0.1:' + ports[i], '--tls-cert', f('rsa-chain.pem'), '--tls-key', f('rsa.key'),
            '--cluster', addrs.join(','), '--cluster-self', addrs[i], '--cluster-token-file', f('replica-token'), '--replica-ca', f('ca.pem'), '--election-timeout', '400ms', '--stop-on-stdin-eof'], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
          const node = {child, log: '', exited: new Promise(r => child.on('exit', r))};
          child.stdout.on('data', d => { node.log += d; }); child.stderr.on('data', d => { node.log += d; });
          nodes.push(node);
        }
        const connect = a => RemoteDB.connect(a, {token: ADMIN, tls: {ca: pki.ca}, reconnect: false, timeout: 2000});
        let leader;
        for (const until = Date.now() + 30000; !leader; await sleep(100)) {
          if (Date.now() > until) throw Error('no leader: ' + nodes.map(n => n.log).join('\n'));
          for (const a of addrs) { try { const c = await connect(a); const st = await c.stats(); await c.close(); if (st.cluster?.role === 'leader') leader = a; } catch {} }
        }
        const db = await connect(leader);
        await db.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
        for (let i = 0; i < 20; i++) await db.sql('INSERT INTO t (v) VALUES (?)', ['r' + i]);  // each confirmed by a follower over TLS
        assert.equal((await db.stats()).cluster.members, 3);
        await db.close();
        for (const a of addrs) {
          for (const until = Date.now() + 10000; ; await sleep(50)) {
            const c = await connect(a); const n = (await c.sql('SELECT COUNT(*) AS n FROM t')).rows[0].n; await c.close();
            if (n === 20) break;
            if (Date.now() > until) throw Error(`member ${a} has ${n} rows`);
          }
        }
      } finally { for (const n of nodes) { n.child.stdin.end(); await n.exited; } }
    });

    await t.test('PHP and Python clients over tls://', {skip: php || python ? false : 'neither PHP nor Python found'}, async () => {
      const s = await start(['--create', '--tls-cert', f('rsa-chain.pem'), '--tls-key', f('rsa.key')]);
      const run = (cmd, args) => new Promise(resolve => {
        const c = spawn(cmd, args, {stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true}); let out = '';
        c.stdout.on('data', d => { out += d; }); c.stderr.on('data', d => { out += d; }); c.on('close', code => resolve({code, out}));
      });
      try {
        if (php) {
          const code = `require ${JSON.stringify(join(root, 'clients/php/KestroDB.php'))};
            $db = new KestroDB\\Client('tls://localhost:${s.port}', '${ADMIN}', 10.0, ['tls' => ['cafile' => ${JSON.stringify(f('ca.pem'))}]]);
            $db->set('php', 'k', ['a' => 1]); $g = $db->get('php', 'k');
            $db->sql('CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT)'); $db->sql('INSERT INTO t (v) VALUES (?)', ['from php']);
            $st = $db->stats(); echo json_encode(['v' => $g['value'], 'n' => $db->queryValue('SELECT COUNT(*) FROM t'), 'tls' => $st['tls']]), "\\n";
            try { new KestroDB\\Client('tls://localhost:${s.port}', '${ADMIN}', 5.0, ['tls' => ['cafile' => ${JSON.stringify(f('wrong-ca.pem'))}]]); echo "BAD\\n"; } catch (KestroDB\\Exception $e) { echo "refused\\n"; }`;
          const r = await run(php, ['-r', code]);
          assert.match(r.out, /\{"v":\{"a":1\},"n":1,"tls":true\}/, r.out); assert.match(r.out, /refused/, r.out);
        }
        if (python) {
          const code = `import sys; sys.path.insert(0, ${JSON.stringify(join(root, 'clients/python'))}); import kestrodb, json
c = kestrodb.connect('tls://localhost:${s.port}', token='${ADMIN}', tls={'cafile': ${JSON.stringify(f('ca.pem'))}})
c.set('py', 'k', {'b': 2}); g = c.get('py', 'k')
c.sql('CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT)'); c.sql('INSERT INTO t (v) VALUES (?)', ['from python'])
print(json.dumps({'v': g['value'], 'n': c.sql('SELECT COUNT(*) AS n FROM t')['rows'][0]['n'], 'tls': c.stats()['tls']}, separators=(',', ':')))
try:
    kestrodb.connect('tls://localhost:${s.port}', token='${ADMIN}', tls={'cafile': ${JSON.stringify(f('wrong-ca.pem'))}}); print('BAD')
except Exception as e:
    print('refused')`;
          const r = await run(python, ['-c', code]);
          assert.match(r.out, /\{"v":\{"b":2\},"n":\d,"tls":true\}/, r.out); assert.match(r.out, /refused/, r.out);
        }
      } finally { assert.equal(await s.stop(), 0); }
    });
  } finally { await rm(dir, {recursive: true, force: true}); }
});
