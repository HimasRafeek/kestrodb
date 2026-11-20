// Automatic failover (--cluster): elections, writes confirmed by a majority, a failed leader
// replaced, and a former leader with an unconfirmed write cutting its log back to rejoin.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import net from 'node:net';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const ADMIN = 'admin-token-0123456789abcdef0123', REPL = 'repl-token-0123456789abcdef012345';
const sleep = ms => new Promise(r => setTimeout(r, ms));
const freePort = () => new Promise((resolve, reject) => { const s = net.createServer(); s.on('error', reject); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });
async function until(fn, ms = 20000, what = 'condition') { const t0 = Date.now(); let last; for (;;) { try { const v = await fn(); if (v) return v; } catch (e) { last = e; } if (Date.now() - t0 > ms) throw Error(`timed out waiting for ${what}${last ? ': ' + last.message : ''}`); await sleep(50); } }

async function cluster(n = 3) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-cluster-'));
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
  const stop = async node => { if (!node.proc) return; node.proc.stdin.end(); await node.exited; node.proc = null; };
  const info = async node => { const c = await RemoteDB.connect(node.addr, {token: ADMIN, reconnect: false, timeout: 1000}); try { return (await c.stats()).cluster; } finally { await c.close(); } };
  const leader = (among = nodes) => until(async () => {
    const live = among.filter(x => x.proc), roles = await Promise.all(live.map(x => info(x).catch(() => null)));
    const leaders = live.filter((x, k) => roles[k]?.role === 'leader');
    // One leader, and every live member knows it.
    if (leaders.length === 1 && roles.every(r => r && (r.leader === leaders[0].addr))) return leaders[0];
    return null;
  }, 20000, 'one leader');
  const done = async () => { await Promise.all(nodes.map(stop)); await rm(dir, {recursive: true, force: true}); };
  return {dir, nodes, addrs, start, kill, stop, info, leader, done};
}
const count = async (addr, where = '') => { const c = await RemoteDB.connect(addr, {token: ADMIN, reconnect: false}); try { return (await c.sql('SELECT COUNT(*) AS n FROM t' + where)).rows[0].n; } finally { await c.close(); } };

test('three members elect one leader; clients write through any member; followers have the rows', async () => {
  const c = await cluster(3);
  try {
    await Promise.all(c.nodes.map(c.start));
    const L = await c.leader();
    // The client is given every member and starts at a follower: writes go to the leader.
    const follower = c.nodes.find(x => x !== L);
    const db = await RemoteDB.connect([follower.addr, ...c.addrs.filter(a => a !== follower.addr)], {token: ADMIN});
    await db.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
    for (let i = 0; i < 30; i++) await db.sql('INSERT INTO t (v) VALUES (?)', ['r' + i]);
    await db.set('kv', 'k', {n: 1});
    assert.equal(db.address, L.addr, 'the client moved to the leader');
    // Confirmed by a majority: at least one follower has every row at once; both soon.
    const now = await Promise.all(c.nodes.filter(x => x !== L).map(x => count(x.addr)));
    assert.ok(now.includes(30), `a follower has every confirmed row (${now})`);
    for (const x of c.nodes) await until(async () => (await count(x.addr)) === 30, 10000, 'replication');
    const s = await c.info(L); assert.equal(s.members, 3); assert.equal(s.majority, 2);
    await db.close();
  } finally { await c.done(); }
});

test('the leader dies: another is elected, no confirmed write is lost, the old one rejoins', async () => {
  const c = await cluster(3);
  try {
    await Promise.all(c.nodes.map(c.start));
    const L1 = await c.leader();
    const db = await RemoteDB.connect(c.addrs, {token: ADMIN, failoverTimeout: 20000});
    await db.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
    let confirmed = 0;
    for (; confirmed < 20; confirmed++) await db.sql('INSERT INTO t (v) VALUES (?)', ['before' + confirmed]);
    const t0 = Date.now();
    await c.kill(L1);
    const L2 = await c.leader();
    const took = Date.now() - t0;
    assert.notEqual(L2, L1);
    assert.equal(await count(L2.addr), 20, 'every confirmed write survived');
    // The same client goes on: its next write reaches the new leader.
    for (let i = 0; i < 10; i++) { await db.sql('INSERT INTO t (v) VALUES (?)', ['after' + i]); confirmed++; }
    assert.equal(db.address, L2.addr);
    // The old leader comes back as a follower and catches up.
    await c.start(L1);
    await until(async () => (await c.info(L1)).role === 'follower' && (await count(L1.addr)) === confirmed, 15000, 'the old leader to catch up');
    assert.equal((await c.info(L1)).leader, L2.addr);
    console.log(`  failover: new leader after ${took} ms`);
    await db.close();
  } finally { await c.done(); }
});

test('a leader cut off from its followers steps down; its unconfirmed write is cut from its log when it rejoins', async () => {
  const c = await cluster(3);
  try {
    await Promise.all(c.nodes.map(c.start));
    const L1 = await c.leader();
    const others = c.nodes.filter(x => x !== L1);
    const db1 = await RemoteDB.connect(L1.addr, {token: ADMIN, reconnect: false});
    await db1.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
    await db1.sql("INSERT INTO t (v) VALUES ('kept')");
    for (const x of others) await until(async () => (await count(x.addr)) === 1, 10000, 'replication');
    // Both followers die: the leader's next write reaches only its own log.
    for (const x of others) await c.kill(x);
    await assert.rejects(db1.sql("INSERT INTO t (v) VALUES ('unconfirmed')"), /UNAVAILABLE/);
    assert.equal((await c.info(L1)).role, 'follower', 'it stepped down: no majority');
    await db1.close().catch(() => {});
    await c.kill(L1);
    // The followers return without it, elect a leader, and take new writes.
    for (const x of others) await c.start(x);
    const L2 = await c.leader(others);
    const db2 = await RemoteDB.connect(L2.addr, {token: ADMIN});
    await db2.sql("INSERT INTO t (v) VALUES ('new1'), ('new2')");
    // The former leader returns: its log has 'unconfirmed' where the others have new rows.
    await c.start(L1);
    await until(async () => /log cut back to transaction/.test(L1.log), 20000, 'the log to be cut back');
    await until(async () => (await c.info(L1)).role === 'follower' && (await count(L1.addr)) === 3, 20000, 'the former leader to follow');
    assert.equal(await count(L1.addr, " WHERE v = 'unconfirmed'"), 0, 'the unconfirmed write is gone');
    assert.equal(await count(L1.addr, " WHERE v = 'new2'"), 1);
    await db2.close();
  } finally { await c.done(); }
});

// PHP and Python: the same address list; a follower first, then (after the leader dies) the
// dead leader first.
import {spawnSync} from 'node:child_process';
function findTool(envName, candidates) {
  for (const c of [process.env[envName], ...candidates].filter(Boolean)) {
    const r = spawnSync(c, ['--version'], {encoding: 'utf8'});
    if (r.status === 0 && !/not found|Microsoft Store/i.test(r.stdout + r.stderr)) return c;
  }
  return null;
}
const php = findTool('KESTRO_PHP', ['php']);
const python = findTool('KESTRO_PYTHON', process.platform === 'win32' ? ['py', 'python'] : ['python3', 'python']);
const scripts = {
  php: (list, tag) => [php, ['-r', `require ${JSON.stringify(join(root, 'clients', 'php', 'KestroDB.php'))}; $db = new KestroDB\\Client(${JSON.stringify(list.join(','))}, ${JSON.stringify(ADMIN)}, 2.0, ['failoverTimeout' => 20]); for ($i = 0; $i < 5; $i++) $db->sql('INSERT INTO t (v) VALUES (?)', [${JSON.stringify(tag)} . $i]); echo $db->address(), "\n";`]],
  python: (list, tag) => [python, ['-c', `import sys; sys.path.insert(0, ${JSON.stringify(join(root, 'clients', 'python'))}); import kestrodb; db = kestrodb.connect(${JSON.stringify(list)}, token=${JSON.stringify(ADMIN)}, timeout=2.0, failover_timeout=20)\nfor i in range(5): db.sql('INSERT INTO t (v) VALUES (?)', [${JSON.stringify(tag)} + str(i)])\nprint(db.address)`]],
};
for (const lang of ['php', 'python']) {
  test(`${lang} client follows the cluster's leader`, {skip: (lang === 'php' ? !php : !python) && `${lang} not installed`}, async () => {
    const c = await cluster(3);
    try {
      await Promise.all(c.nodes.map(c.start));
      const L1 = await c.leader();
      const admin = await RemoteDB.connect(L1.addr, {token: ADMIN}); await admin.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)'); await admin.close();
      const run = (list, tag) => new Promise((resolve, reject) => {
        const [cmd, args] = scripts[lang](list, tag); const p = spawn(cmd, args, {encoding: 'utf8'}); let out = '', err = '';
        p.stdout.on('data', d => { out += d; }); p.stderr.on('data', d => { err += d; });
        p.on('exit', code => code === 0 ? resolve(out.trim()) : reject(Error(`${lang} exited ${code}: ${err}${out}`)));
      });
      const f = c.nodes.find(x => x !== L1);
      assert.equal(await run([f.addr, ...c.addrs.filter(a => a !== f.addr)], 'a'), L1.addr, 'moved from a follower to the leader');
      await c.kill(L1);
      const L2 = await c.leader(c.nodes.filter(x => x !== L1));
      assert.equal(await run([L1.addr, ...c.addrs.filter(a => a !== L1.addr)], 'b'), L2.addr, 'past the dead leader to the new one');
      assert.equal(await count(L2.addr), 10);
    } finally { await c.done(); }
  });
}
