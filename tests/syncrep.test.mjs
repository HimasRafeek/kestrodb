// Synchronous replication (--sync-replicas): a write is answered once a follower has it.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const ADMIN = 'admin-token-0123456789abcdef0123', REPL = 'repl-token-0123456789abcdef012345';
const sleep = ms => new Promise(r => setTimeout(r, ms));

function server(args, port = 0) {
  const child = spawn(serverBinary, [...args, '--tcp', '127.0.0.1:' + port], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const ready = new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${err}`)));
  });
  ready.catch(() => {});
  const exited = new Promise(r => child.on('exit', r));
  return {child, ready, exited, get err() { return err; }, kill: async () => { child.kill('SIGKILL'); await exited; }, stop: async () => { child.stdin.end(); await exited; }};
}
async function until(fn, ms = 15000) { const t0 = Date.now(); for (;;) { try { const v = await fn(); if (v) return v; } catch {} if (Date.now() - t0 > ms) throw Error('timed out'); await sleep(50); } }

async function setup(leaderArgs) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-sync-'));
  await writeFile(join(dir, 'users'), `admin admin ${ADMIN}\nrepl replica ${REPL}\n`);
  await writeFile(join(dir, 'rtoken'), REPL);
  const leader = server(['--db', join(dir, 'leader.kdb'), '--create', '--users', join(dir, 'users'), '--stop-on-stdin-eof', ...leaderArgs]);
  const address = await leader.ready;
  const port = address.split(':').pop();
  const startFollower = (fport = 0) => server(['--db', join(dir, 'follower.kdb'), '--replica-of', address, '--replica-token-file', join(dir, 'rtoken'), '--users', join(dir, 'users'), '--stop-on-stdin-eof'], fport);
  // The follower first: with --sync-strict no write is answered before one has it.
  const follower = startFollower(); const faddr = await follower.ready;
  const db = await RemoteDB.connect(address, {token: ADMIN});
  await db.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
  return {dir, leader, address, port, startFollower, db, follower, faddr};
}
const synced = async db => { const s = await db.stats(); return s.sync && s.sync.followers > 0 && s.sync.acknowledgedEnd >= s.bytes && !s.sync.degraded ? s : null; };

test('acknowledged writes are on the follower: the leader can die at once', async () => {
  const {dir, leader, db, follower, faddr} = await setup(['--sync-replicas', '1', '--sync-timeout', '5s']);
  try {
    await until(() => synced(db));
    for (let i = 0; i < 20; i++) await db.sql('INSERT INTO t (v) VALUES (?)', ['row' + i]);
    await db.set('kv', 'last', {n: 20});
    await leader.kill();  // no clean shutdown: only what the follower already has survives
    const f = await RemoteDB.connect(faddr, {token: ADMIN});
    assert.equal((await f.sql('SELECT COUNT(*) AS n FROM t')).rows[0].n, 20, 'every answered INSERT is on the follower');
    assert.deepEqual((await f.get('kv', 'last')).value, {n: 20});
    await f.close();
  } finally { await follower.stop().catch(() => {}); await leader.kill().catch(() => {}); await rm(dir, {recursive: true, force: true}); }
});

test('without its follower the leader goes on after --sync-timeout, then synchronous again', async () => {
  let {dir, leader, startFollower, db, follower, faddr} = await setup(['--sync-replicas', '1', '--sync-timeout', '1s']);
  try {
    await until(() => synced(db));
    const fport = faddr.split(':').pop();
    await follower.kill();
    let t0 = Date.now(); await db.sql("INSERT INTO t (v) VALUES ('alone')"); const first = Date.now() - t0;
    assert.ok(first >= 800, `the first write waited for the follower (${first} ms)`);
    assert.equal((await db.stats()).sync.degraded, true);
    t0 = Date.now(); await db.sql("INSERT INTO t (v) VALUES ('alone2')"); assert.ok(Date.now() - t0 < 800, 'degraded: later writes do not wait');
    follower = startFollower(Number(fport)); faddr = await follower.ready;
    await until(async () => { await db.sql("INSERT INTO t (v) VALUES ('probe')"); return !(await db.stats()).sync.degraded; });
    const f = await RemoteDB.connect(faddr, {token: ADMIN});
    await until(async () => (await f.sql("SELECT COUNT(*) AS n FROM t WHERE v = 'alone2'")).rows[0].n === 1);
    await f.close();
  } finally { await follower.stop().catch(() => {}); await leader.stop().catch(() => {}); await rm(dir, {recursive: true, force: true}); }
});

test('--sync-strict: writes wait for a follower however long it takes', async () => {
  let {dir, leader, startFollower, db, follower, faddr} = await setup(['--sync-replicas', '1', '--sync-strict']);
  try {
    await until(() => synced(db));
    const fport = faddr.split(':').pop();
    await follower.kill();
    let done = false;
    const write = db.sql("INSERT INTO t (v) VALUES ('waits')").then(() => { done = true; });
    await sleep(2000);
    assert.equal(done, false, 'the write waits while no follower has it');
    follower = startFollower(Number(fport)); await follower.ready;
    await write;
    assert.equal(done, true);
  } finally { await follower.stop().catch(() => {}); await leader.stop().catch(() => {}); await rm(dir, {recursive: true, force: true}); }
});
