// The Rust client (clients/rust) against a real kestro-server: cargo's unit tests, then the
// integration program (clients/rust/examples/client_test.rs) for the client API with every
// parameter type, transactions, key/value versions, error kinds, reconnecting after restarts,
// BUSY, TLS (a throwaway CA made with openssl; the crate's "tls" feature), and following a
// 3-member cluster's leader. Skipped when cargo is not installed (set KESTRO_CARGO to choose
// one; ~/.cargo/bin/cargo is tried too). Builds go to KESTRO_CARGO_TARGET_DIR (default: a
// directory in the system's temp folder), not into the source tree.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import net from 'node:net';
import {homedir, tmpdir} from 'node:os';
import {join, delimiter, dirname, isAbsolute} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const windows = process.platform === 'win32';
const exe = windows ? '.exe' : '';
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + exe);
const rustDir = join(root, 'clients', 'rust');
const TOKEN = 'rust-client-token-0123456789';
const sleep = ms => new Promise(r => setTimeout(r, ms));

function findTool(envName, candidates, args) {
  for (const c of [process.env[envName], ...candidates].filter(Boolean)) {
    const r = spawnSync(c, args, {encoding: 'utf8'});
    if (r.status === 0) return c;
  }
  return null;
}
const cargo = findTool('KESTRO_CARGO', ['cargo', join(homedir(), '.cargo', 'bin', 'cargo' + exe)], ['--version']);
const openssl = findTool('KESTRO_OPENSSL', ['openssl', 'C:\\Program Files\\Git\\mingw64\\bin\\openssl.exe', 'C:\\Program Files\\Git\\usr\\bin\\openssl.exe'], ['version']);
const skip = !cargo ? 'cargo not found (install Rust from rustup.rs, or set KESTRO_CARGO)' : false;
const targetBase = process.env.KESTRO_CARGO_TARGET_DIR || join(tmpdir(), 'kestrodb-rust-target');

// cargo in clients/rust; a cargo found by its path also finds rustc next to it.
function cargoRun(args, target) {
  const env = {...process.env, CARGO_TARGET_DIR: target, CARGO_TERM_COLOR: 'never'};
  if (isAbsolute(cargo)) env.PATH = dirname(cargo) + delimiter + (process.env.PATH ?? '');
  return spawnSync(cargo, args, {cwd: rustDir, encoding: 'utf8', env, maxBuffer: 256 << 20});
}
const targetFor = features => join(targetBase, features ? features.replace(/\W+/g, '-') : 'default');

// The test program, built once per feature set; resolves to its executable.
const builds = new Map();
function build(features = '') {
  if (!builds.has(features)) builds.set(features, (async () => {
    const r = cargoRun(['build', '--example', 'client_test', '--message-format=json-render-diagnostics', ...(features ? ['--features', features] : [])], targetFor(features));
    if (r.status !== 0) throw Error(`building the Rust client failed:\n${r.stdout}${r.stderr}`);
    for (const line of r.stdout.split('\n')) {
      if (!line.startsWith('{')) continue;
      const m = JSON.parse(line);
      if (m.reason === 'compiler-artifact' && m.target?.name === 'client_test' && m.executable) return m.executable;
    }
    throw Error(`cargo did not report the client_test executable:\n${r.stdout}${r.stderr}`);
  })());
  return builds.get(features);
}

// The test program; waitFor(line) resolves once it has printed that whole line.
function startRust(binary, args) {
  const child = spawn(binary, args, {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  const waiters = [];
  const has = line => out.split(/\r?\n/).slice(0, -1).includes(line);   // complete lines only
  child.stdout.on('data', d => { out += d; for (const w of [...waiters]) if (has(w.text)) { waiters.splice(waiters.indexOf(w), 1); w.resolve(); } });
  child.stderr.on('data', d => { err += d; });
  const exited = new Promise(resolve => child.on('exit', code => { for (const w of waiters) w.reject(Error(`client_test exited ${code} before "${w.text}":\n${out}${err}`)); resolve(code); }));
  return {
    child, exited,
    output: () => out + err,
    stdout: () => out,
    waitFor: text => has(text) ? Promise.resolve() : new Promise((resolve, reject) => waiters.push({text, resolve, reject})),
    send: line => child.stdin.write(line + '\n'),
  };
}
async function runRust(binary, args) {
  const j = startRust(binary, args);
  const code = await j.exited;
  return {code, out: j.stdout(), all: j.output()};
}

async function startServer(dir, {port = 0, create = true, db = 'shared.kdb', args = []} = {}) {
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
    stop: async () => {
      // A clean stop (stdin EOF), bounded: a server that has not exited after 5 s is killed
      // and reported, so a server-side shutdown hang (Linux builds before the fix for a
      // clean stop after BUSY refusals) cannot hang this file.
      child.stdin.end();
      const timer = setTimeout(() => {
        console.error(`# kestro-server (${address}) did not exit within 5 s of stdin EOF; killed. A server shutdown hang, not a client failure.`);
        child.kill('SIGKILL');
      }, 5000);
      try { return await exited; } finally { clearTimeout(timer); }
    },
    kill: async () => { child.kill('SIGKILL'); return exited; },
  };
}
// Restarts kill the server rather than stop it (see tests/java-client.test.mjs): a crash
// restart is the hardest case for a client to recover from. On Linux the killed server's
// file lock can outlive it by a few hundred milliseconds (the kernel releases it after the
// process is reaped), so the new server is retried while it reports LOCKED.
async function restartServer(srv, dir, options) {
  await srv.kill();
  const t0 = Date.now();
  for (;;) {
    try { return await startServer(dir, {...options, port: srv.port, create: false}); }
    catch (e) { if (!/LOCKED/.test(e.message) || Date.now() - t0 > 5000) throw e; await sleep(100); }
  }
}

test('Rust: cargo unit tests (default features, and the tls feature)', {skip}, () => {
  for (const features of ['', 'tls']) {
    const r = cargoRun(['test', ...(features ? ['--features', features] : [])], targetFor(features));
    assert.equal(r.status, 0, `cargo test ${features}:\n${r.stdout}${r.stderr}`);
    assert.match(r.stdout, /test result: ok/);
  }
});

test('Rust: client API (every parameter type, transactions, key/value versions, error kinds, threads)', {skip}, async () => {
  const bin = await build();
  const dir = await mkdtemp(join(tmpdir(), 'kdb-rust-'));
  const srv = await startServer(dir);
  try {
    const r = await runRust(bin, ['api', srv.address, TOKEN]);
    assert.equal(r.code, 0, r.all);
    assert.match(r.out, /ok rust api/);
  } finally { await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('Rust: idle clients reconnect after the server restarts; a lost transaction is reported once; reconnect(false)', {skip}, async () => {
  const bin = await build();
  const dir = await mkdtemp(join(tmpdir(), 'kdb-rust-rc-'));
  let srv = await startServer(dir);
  const restart = async () => { srv = await restartServer(srv, dir); };
  let j;
  try {
    j = startRust(bin, ['reconnect', srv.address, TOKEN]);
    await j.waitFor('READY');
    await restart();
    j.send('go');
    await j.waitFor('READY2');
    await restart();   // with a transaction open: it dies with the server
    j.send('go');
    assert.equal(await j.exited, 0, j.output());
    assert.match(j.stdout(), /ok rust reconnect/);
  } finally { j?.child.kill(); await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('Rust: BUSY at the connection limit', {skip}, async () => {
  const bin = await build();
  const dir = await mkdtemp(join(tmpdir(), 'kdb-rust-busy-'));
  const srv = await startServer(dir, {args: ['--max-connections', '3']});
  try {
    const r = await runRust(bin, ['busy', srv.address, TOKEN, '3']);
    assert.equal(r.code, 0, r.all);
    assert.match(r.out, /ok rust busy/);
  } finally { await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

test('Rust: TLS with a CA file (verification, host names, wrong CA, reconnect; the tls feature)', {skip: skip || (!openssl && 'openssl not found (set KESTRO_OPENSSL)')}, async () => {
  const bin = await build('tls');
  const dir = await mkdtemp(join(tmpdir(), 'kdb-rust-tls-'));
  const ssl = (...args) => { const r = spawnSync(openssl, args, {cwd: dir, encoding: 'utf8'}); if (r.status !== 0) throw Error(`openssl ${args.join(' ')}: ${r.stdout}${r.stderr}`); };
  await writeFile(join(dir, 'server.ext'), 'basicConstraints=CA:FALSE\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n');
  ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-subj', '/CN=KestroDB Rust Test CA', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'wrong.key', '-out', 'wrong.pem', '-days', '2', '-subj', '/CN=KestroDB Wrong CA', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  ssl('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'server.key', '-out', 'server.csr', '-subj', '/CN=localhost');
  ssl('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial', '-out', 'server.pem', '-days', '2', '-extfile', 'server.ext');
  const tlsArgs = ['--tls-cert', join(dir, 'server.pem'), '--tls-key', join(dir, 'server.key')];
  let srv = await startServer(dir, {args: tlsArgs});
  let j;
  try {
    assert.match(srv.log(), /KESTRODB READY tls/);
    j = startRust(bin, ['tls', `tls://localhost:${srv.port}`, TOKEN, join(dir, 'ca.pem'), join(dir, 'wrong.pem')]);
    await j.waitFor('READY');
    srv = await restartServer(srv, dir, {args: tlsArgs});
    j.send('go');
    assert.equal(await j.exited, 0, j.output());
    assert.match(j.stdout(), /ok rust tls/);
  } finally { j?.child.kill(); await srv.stop(); await rm(dir, {recursive: true, force: true}); }
});

// A 3-member cluster as in tests/cluster.test.mjs.
const ADMIN = 'admin-token-0123456789abcdef0123', REPL = 'repl-token-0123456789abcdef012345';
const freePort = () => new Promise((resolve, reject) => { const s = net.createServer(); s.on('error', reject); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });
async function until(fn, ms = 20000, what = 'condition') { const t0 = Date.now(); let last; for (;;) { try { const v = await fn(); if (v) return v; } catch (e) { last = e; } if (Date.now() - t0 > ms) throw Error(`timed out waiting for ${what}${last ? ': ' + last.message : ''}`); await sleep(50); } }
async function cluster(n = 3) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-rust-cluster-'));
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
  const stop = async node => {
    if (!node.proc) return;
    const proc = node.proc, timer = setTimeout(() => proc.kill('SIGKILL'), 5000);   // bounded, as startServer's stop
    proc.stdin.end(); await node.exited; clearTimeout(timer); node.proc = null;
  };
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

test('Rust: follows a 3-member cluster\'s leader (READONLY from a follower, failover, a dead first member)', {skip}, async () => {
  const bin = await build();
  const c = await cluster(3);
  let j;
  try {
    await Promise.all(c.nodes.map(c.start));
    const L1 = await c.leader();
    const admin = await RemoteDB.connect(L1.addr, {token: ADMIN});
    await admin.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
    await admin.close();
    const f = c.nodes.find(x => x !== L1);
    await until(async () => (await count(f.addr)) === 0, 10000, 'the table to reach the follower');
    const list = [f.addr, ...c.addrs.filter(a => a !== f.addr)];
    j = startRust(bin, ['cluster', list.join(','), ADMIN, f.addr]);
    await j.waitFor('READY');
    const addresses = () => [...j.stdout().matchAll(/ADDRESS (\S+)/g)].map(m => m[1]);
    assert.deepEqual(addresses(), [L1.addr], 'moved from the follower to the leader');
    await c.kill(L1);
    const L2 = await c.leader(c.nodes.filter(x => x !== L1));
    j.send('go');
    assert.equal(await j.exited, 0, j.output());
    assert.match(j.stdout(), /ok rust cluster/);
    assert.deepEqual(addresses(), [L1.addr, L2.addr], 'the same client found the new leader');
    // A new client whose first member is the dead leader.
    const r = await runRust(bin, ['cluster-new', [L1.addr, ...c.addrs.filter(a => a !== L1.addr)].join(','), ADMIN]);
    assert.equal(r.code, 0, r.all);
    assert.match(r.out, new RegExp('ADDRESS ' + L2.addr.replace(/[.]/g, '\\.')));
    assert.equal(await count(L2.addr), 16, 'every write arrived once (5 + 1 + 5 + 5)');
  } finally { j?.child.kill(); await c.done(); }
});
