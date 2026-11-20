// The Go client (clients/go) against a real kestro-server: the client API with every
// parameter type, transactions (BEGIN CONCURRENT too), key/value versions, error kinds,
// goroutines, database/sql, reconnecting after restarts, BUSY, TLS (a throwaway CA made
// with openssl), and following a 3-member cluster's leader. The Go side is a compiled test
// binary (clients/go/server_test.go, TestServer). Skipped when go is not installed (set
// KESTRO_GO to choose one).
//
// On Windows without a Windows go, the whole file runs inside WSL when WSL has go and
// KESTRO_WSL_SERVER_BINARY names a Linux kestro-server there (KESTRO_WSL_DISTRO and
// KESTRO_WSL_USER choose the distribution and user). Inside WSL or on Linux/macOS:
//   KESTRO_SERVER_BINARY=/path/to/kestro-server node --test tests/go-client.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import net from 'node:net';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const windows = process.platform === 'win32';
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (windows ? '.exe' : ''));
const goDir = join(root, 'clients', 'go');
const TOKEN = 'go-client-token-0123456789';
const sleep = ms => new Promise(r => setTimeout(r, ms));

function findTool(envName, candidates, args) {
  for (const c of [process.env[envName], ...candidates].filter(Boolean)) {
    const r = spawnSync(c, args, {encoding: 'utf8'});
    if (r.status === 0) return c;
  }
  return null;
}
const go = findTool('KESTRO_GO', ['go', ...(windows ? ['C:\\Program Files\\Go\\bin\\go.exe'] : ['/usr/local/go/bin/go', '/usr/local/bin/go', '/usr/lib/go/bin/go'])], ['version']);
const openssl = findTool('KESTRO_OPENSSL', ['openssl', 'C:\\Program Files\\Git\\mingw64\\bin\\openssl.exe', 'C:\\Program Files\\Git\\usr\\bin\\openssl.exe'], ['version']);

// Windows without go: run this file inside WSL instead, when that can work.
const wslArgs = () => [...(process.env.KESTRO_WSL_DISTRO ? ['-d', process.env.KESTRO_WSL_DISTRO] : []), ...(process.env.KESTRO_WSL_USER ? ['-u', process.env.KESTRO_WSL_USER] : [])];
const wslPath = p => p.replace(/^([A-Za-z]):\\/, (_, d) => `/mnt/${d.toLowerCase()}/`).replace(/\\/g, '/');
const quote = s => `'${String(s).replace(/'/g, `'\\''`)}'`;
let viaWSL = false;
if (!go && windows && process.env.KESTRO_WSL_SERVER_BINARY) {
  const r = spawnSync('wsl.exe', [...wslArgs(), '--', 'bash', '-lc', 'go version && node --version'], {encoding: 'utf8'});
  viaWSL = r.status === 0;
}
const skip = go ? false : viaWSL ? 'runs inside WSL (see the WSL test)' : windows
  ? 'go not found (install Go, set KESTRO_GO, or set KESTRO_WSL_SERVER_BINARY to a Linux kestro-server to run this file in WSL)'
  : 'go not found (install Go 1.21+, or set KESTRO_GO)';

test('Go: the whole suite inside WSL', {skip: viaWSL ? false : 'only on Windows without go, with KESTRO_WSL_SERVER_BINARY set'}, () => {
  const cmd = `cd ${quote(wslPath(root))} && KESTRO_SERVER_BINARY=${quote(process.env.KESTRO_WSL_SERVER_BINARY)} node --test tests/go-client.test.mjs`;
  const r = spawnSync('wsl.exe', [...wslArgs(), '--', 'bash', '-lc', cmd], {encoding: 'utf8', timeout: 15 * 60 * 1000});
  assert.equal(r.status, 0, `${r.stdout}${r.stderr}`);
  assert.doesNotMatch(r.stdout, /# fail [1-9]/, r.stdout);
});

// Builds the test binary once (go test -c), outside the source tree.
let built;
function build() {
  built ??= (async () => {
    const dir = await mkdtemp(join(tmpdir(), 'kdb-go-bin-'));
    const bin = join(dir, 'kestro-go.test' + (windows ? '.exe' : ''));
    const env = {...process.env, GOTOOLCHAIN: 'local', GOFLAGS: process.env.GOFLAGS ?? '', CGO_ENABLED: process.env.CGO_ENABLED ?? '0'};
    let r = spawnSync(go, ['vet', '.'], {cwd: goDir, encoding: 'utf8', env});
    if (r.status !== 0) throw Error(`go vet failed:\n${r.stdout}${r.stderr}`);
    r = spawnSync(go, ['test', '-count=1', '.'], {cwd: goDir, encoding: 'utf8', env});   // the unit tests
    if (r.status !== 0) throw Error(`go test (unit tests) failed:\n${r.stdout}${r.stderr}`);
    r = spawnSync(go, ['test', '-c', '-o', bin, '.'], {cwd: goDir, encoding: 'utf8', env});
    if (r.status !== 0) throw Error(`building the Go test binary failed:\n${r.stdout}${r.stderr}`);
    return bin;
  })();
  return built;
}
test.after(async () => { if (built) { const bin = await built.catch(() => null); if (bin) await rm(join(bin, '..'), {recursive: true, force: true}); } });

// The Go test program; waitFor(line) resolves once it has printed that whole line.
async function startGo(args) {
  const bin = await build();
  const child = spawn(bin, ['-test.run', '^TestServer$', '-test.timeout', '10m', ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  const waiters = [];
  const has = line => out.split(/\r?\n/).slice(0, -1).includes(line);   // complete lines only
  child.stdout.on('data', d => { out += d; for (const w of [...waiters]) if (has(w.text)) { waiters.splice(waiters.indexOf(w), 1); w.resolve(); } });
  child.stderr.on('data', d => { err += d; });
  const exited = new Promise(resolve => child.on('exit', code => { for (const w of waiters) w.reject(Error(`go exited ${code} before "${w.text}":\n${out}${err}`)); resolve(code); }));
  return {
    child, exited,
    output: () => out + err,
    stdout: () => out,
    waitFor: text => has(text) ? Promise.resolve() : new Promise((resolve, reject) => waiters.push({text, resolve, reject})),
    send: line => child.stdin.write(line + '\n'),
  };
}
async function runGo(args) {
  const g = await startGo(args);
  const code = await g.exited;
  return {code, out: g.stdout(), all: g.output()};
}

// A server restarted at once after a SIGKILL that followed writes can briefly find its file
// still locked ("LOCKED file has another owner", on Linux, with no process holding it): retry.
async function startServer(dir, options = {}) {
  for (let attempt = 1; ; attempt++) {
    try { return await startServerOnce(dir, options); } catch (e) {
      if (!/LOCKED/.test(e.message) || attempt >= 30) throw e;
      await sleep(100);
    }
  }
}
async function startServerOnce(dir, {port = 0, create = true, db = 'shared.kdb', args = []} = {}) {
  const tokenFile = join(dir, 'token');
  await writeFile(tokenFile, TOKEN + '\n');
  const child = spawn(serverBinary, ['--db', join(dir, db), ...(create ? ['--create'] : []), '--tcp', `127.0.0.1:${port}`, '--stop-on-stdin-eof', '--token-file', tokenFile, ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const address = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY (tcp|tls) (\S+)/.exec(out); if (m) resolve('tcp://' + m[2]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${out}${err}`)));
  });
  const exited = new Promise(resolve => child.on('exit', resolve));
  return {
    address, port: Number(address.split(':').pop()), log: () => out + err,
    stop: () => stopOrKill(child, exited),
    kill: async () => { child.kill('SIGKILL'); return exited; },
  };
}
// A clean stop, or SIGKILL after 5 s: servers built before the fix hang at exit once they
// have refused a connection with BUSY (a static object destroyed with a thread waiting on
// it), which must not hang this test.
async function stopOrKill(child, exited) {
  child.stdin.end();
  const timer = setTimeout(() => child.kill('SIGKILL'), 5000);
  try { return await exited; } finally { clearTimeout(timer); }
}
// Restarts kill the server rather than stop it (as in java-client.test.mjs): a crash-restart
// is the hardest case for a client, and a clean stop on Windows waits for idle clients.

test('Go: client API (every parameter type, transactions, key/value versions, errors, goroutines, contexts), database/sql', {skip}, async () => {
  await build();
  const dir = await mkdtemp(join(tmpdir(), 'kdb-go-'));
  const srv = await startServer(dir);
  try {
    const r = await runGo(['api', srv.address, TOKEN]);
    assert.equal(r.code, 0, r.all);
    assert.match(r.out, /ok go api/);
  } finally { await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('Go: idle clients reconnect after the server restarts; a lost transaction is reported; DisableReconnect', {skip}, async () => {
  await build();
  const dir = await mkdtemp(join(tmpdir(), 'kdb-go-rc-'));
  let srv = await startServer(dir);
  const port = srv.port;
  const restart = async () => { await srv.kill(); srv = await startServer(dir, {port, create: false}); };
  let g;
  try {
    g = await startGo(['reconnect', srv.address, TOKEN]);
    await g.waitFor('READY');
    await restart();
    g.send('go');
    await g.waitFor('READY2');
    await restart();   // with a transaction open: it dies with the server
    g.send('go');
    assert.equal(await g.exited, 0, g.output());
    assert.match(g.stdout(), /ok go reconnect/);
  } finally { g?.child.kill(); await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('Go: BUSY at the connection limit', {skip}, async () => {
  await build();
  const dir = await mkdtemp(join(tmpdir(), 'kdb-go-busy-'));
  const srv = await startServer(dir, {args: ['--max-connections', '3']});
  try {
    const r = await runGo(['busy', srv.address, TOKEN, '3']);
    assert.equal(r.code, 0, r.all);
    assert.match(r.out, /ok go busy/);
  } finally { await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('Go: TLS with a CA file (verification, host names, wrong CA, TLSConfig, database/sql, reconnect)', {skip: skip || (!openssl && 'openssl not found (set KESTRO_OPENSSL)')}, async () => {
  await build();
  const dir = await mkdtemp(join(tmpdir(), 'kdb-go-tls-'));
  const ssl = (...args) => { const r = spawnSync(openssl, args, {cwd: dir, encoding: 'utf8'}); if (r.status !== 0) throw Error(`openssl ${args.join(' ')}: ${r.stdout}${r.stderr}`); };
  await writeFile(join(dir, 'server.ext'), 'basicConstraints=CA:FALSE\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n');
  ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-subj', '/CN=KestroDB Go Test CA', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'wrong.key', '-out', 'wrong.pem', '-days', '2', '-subj', '/CN=KestroDB Wrong CA', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  ssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'server.key', '-out', 'server.csr', '-subj', '/CN=localhost');
  ssl('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'server.pem', '-days', '2', '-extfile', 'server.ext');
  const tlsArgs = ['--tls-cert', join(dir, 'server.pem'), '--tls-key', join(dir, 'server.key')];
  let srv = await startServer(dir, {args: tlsArgs});
  let g;
  try {
    assert.match(srv.log(), /KESTRODB READY tls/);
    g = await startGo(['tls', `tls://localhost:${srv.port}`, TOKEN, join(dir, 'ca.pem'), join(dir, 'wrong.pem')]);
    await g.waitFor('READY');
    const port = srv.port;
    await srv.kill();
    srv = await startServer(dir, {port, create: false, args: tlsArgs});
    g.send('go');
    assert.equal(await g.exited, 0, g.output());
    assert.match(g.stdout(), /ok go tls/);
  } finally { g?.child.kill(); await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

// A 3-member cluster as in tests/cluster.test.mjs.
const ADMIN = 'admin-token-0123456789abcdef0123', REPL = 'repl-token-0123456789abcdef012345';
const freePort = () => new Promise((resolve, reject) => { const s = net.createServer(); s.on('error', reject); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });
async function until(fn, ms = 20000, what = 'condition') { const t0 = Date.now(); let last; for (;;) { try { const v = await fn(); if (v) return v; } catch (e) { last = e; } if (Date.now() - t0 > ms) throw Error(`timed out waiting for ${what}${last ? ': ' + last.message : ''}`); await sleep(50); } }
async function cluster(n = 3) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-go-cluster-'));
  await writeFile(join(dir, 'users'), `admin admin ${ADMIN}\nrepl replica ${REPL}\n`);
  await writeFile(join(dir, 'rtoken'), REPL);
  const ports = []; for (let i = 0; i < n; i++) ports.push(await freePort());
  const addrs = ports.map(p => 'tcp://127.0.0.1:' + p);
  const nodes = addrs.map((addr, i) => ({addr, i, proc: null, log: ''}));
  const start = node => {
    const child = spawn(serverBinary, ['--db', join(dir, `n${node.i}.kdb`), '--create', '--users', join(dir, 'users'), '--tcp', '127.0.0.1:' + ports[node.i],
      '--cluster', addrs.join(','), '--cluster-self', node.addr, '--cluster-token-file', join(dir, 'rtoken'), '--election-timeout', '1s', '--sync-timeout', '3s', '--stop-on-stdin-eof'],
      {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
    node.proc = child; node.exited = new Promise(r => child.on('exit', r));
    child.stdout.on('data', d => { node.log += d; }); child.stderr.on('data', d => { node.log += d; });
    return until(() => /KESTRODB READY/.test(node.log), 15000, `node ${node.i} to start`);
  };
  const kill = async node => { node.proc.kill('SIGKILL'); await node.exited; node.proc = null; };
  const stop = async node => { if (!node.proc) return; await stopOrKill(node.proc, node.exited); node.proc = null; };
  const info = async node => { const c = await RemoteDB.connect(node.addr, {token: ADMIN, reconnect: false, timeout: 1000}); try { return (await c.stats()).cluster; } finally { await c.close(); } };
  const leader = (among = nodes) => until(async () => {
    const live = among.filter(x => x.proc), roles = await Promise.all(live.map(x => info(x).catch(() => null)));
    const leaders = live.filter((x, k) => roles[k]?.role === 'leader');
    if (leaders.length === 1 && roles.every(r => r && r.leader === leaders[0].addr)) return leaders[0];
    return null;
  }, 20000, 'one leader');
  const done = async () => { await Promise.all(nodes.map(stop)); await rm(dir, {recursive: true, force: true}); };
  return {nodes, addrs, start, kill, leader, done};
}
const count = async addr => { const c = await RemoteDB.connect(addr, {token: ADMIN, reconnect: false}); try { return (await c.sql('SELECT COUNT(*) AS n FROM t')).rows[0].n; } finally { await c.close(); } };

test('Go: follows a 3-member cluster\'s leader (READONLY from a follower, failover, a dead first member)', {skip}, async () => {
  await build();
  const c = await cluster(3);
  let g;
  try {
    await Promise.all(c.nodes.map(c.start));
    const L1 = await c.leader();
    const admin = await RemoteDB.connect(L1.addr, {token: ADMIN});
    await admin.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
    await admin.close();
    const f = c.nodes.find(x => x !== L1);
    await until(async () => (await count(f.addr)) === 0, 10000, 'the table to reach the follower');
    const list = [f.addr, ...c.addrs.filter(a => a !== f.addr)];
    g = await startGo(['cluster', list.join(','), ADMIN, f.addr]);
    await g.waitFor('READY');
    const addresses = () => [...g.stdout().matchAll(/ADDRESS (\S+)/g)].map(m => m[1]);
    assert.deepEqual(addresses(), [L1.addr], 'moved from the follower to the leader');
    await c.kill(L1);
    const L2 = await c.leader(c.nodes.filter(x => x !== L1));
    g.send('go');
    assert.equal(await g.exited, 0, g.output());
    assert.match(g.stdout(), /ok go cluster/);
    assert.deepEqual(addresses(), [L1.addr, L2.addr], 'the same client found the new leader');
    // A new client whose first member is the dead leader.
    const r = await runGo(['cluster-new', [L1.addr, ...c.addrs.filter(a => a !== L1.addr)].join(','), ADMIN]);
    assert.equal(r.code, 0, r.all);
    assert.match(r.out, new RegExp('ADDRESS ' + L2.addr.replace(/[.]/g, '\\.')));
    assert.equal(await count(L2.addr), 16, 'every write arrived once (5 + 1 database/sql + 5 + 5)');
  } finally { g?.child.kill(); await c.done(); }
});
