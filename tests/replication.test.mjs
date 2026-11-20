// Replication: a follower seeds from its leader, streams every commit (key/value, SQL,
// DDL, indexes), serves identical reads, refuses writes, survives restarts on both sides,
// stops on a leader compaction (STALE) and becomes writable when promoted.
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
const sleep = ms => new Promise(r => setTimeout(r, ms));
const ADMIN = 'admin-token-0123456789', REPLICA = 'replica-token-0123456789';

async function start(args, port = 0) {
  const child = spawn(serverBinary, [...args, '--tcp', `127.0.0.1:${port}`, '--stop-on-stdin-eof'], {stdio: ['pipe', 'pipe', 'pipe']});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  const address = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${out}${err}`)));
  });
  const exited = new Promise(resolve => child.on('exit', resolve));
  return {address, log: () => out + err, stop: async () => { child.stdin.end(); return exited; }};
}
async function caughtUp(follower, leader, ms = 10000) {
  const want = (await leader.stats()).txid;
  for (const until = Date.now() + ms; Date.now() < until; await sleep(20)) if ((await follower.stats()).txid >= want) return want;
  throw Error(`follower did not catch up to ${want}: ${JSON.stringify((await follower.stats()).replica)}`);
}

test('replication: seed, stream, identical reads, read-only, restarts, STALE after a leader compaction, promotion', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-repl-'));
  await writeFile(join(dir, 'users'), `admin admin ${ADMIN}\nrepl replica ${REPLICA}\n`);
  await writeFile(join(dir, 'replica-token'), REPLICA + '\n');
  const leaderArgs = ['--db', join(dir, 'leader.kdb'), '--users', join(dir, 'users')];
  let leader = await start([...leaderArgs, '--create']);
  let follower;
  const followerArgs = () => ['--db', join(dir, 'follower.kdb'), '--users', join(dir, 'users'), '--replica-of', leader.address, '--replica-token-file', join(dir, 'replica-token')];
  try {
    let L = await RemoteDB.connect(leader.address, {token: ADMIN});
    await L.execute('CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT UNIQUE, city TEXT, n INTEGER)');
    await L.execute('CREATE INDEX ON users (city)');
    for (let i = 0; i < 300; i++) await L.execute('INSERT INTO users (email, city, n) VALUES (?, ?, ?)', [`u${i}@x`, `c${i % 7}`, i]);
    await L.batch(Array.from({length: 500}, (_, i) => ({type: 'set', collection: 'kv', key: 'k' + i, value: {i}})));

    // Seed a new follower from the leader, then stream.
    follower = await start(followerArgs());
    assert.match(follower.log(), /KESTRODB SEEDED/);
    let F = await RemoteDB.connect(follower.address, {token: ADMIN});
    for (let i = 300; i < 400; i++) await L.execute('INSERT INTO users (email, city, n) VALUES (?, ?, ?)', [`u${i}@x`, `c${i % 7}`, i]);
    await L.execute("UPDATE users SET city = 'moved' WHERE n < 50");
    await L.execute('DELETE FROM users WHERE n >= 390');
    await L.execute('CREATE TABLE orders (id INTEGER PRIMARY KEY, user INTEGER, total REAL)');
    await L.execute('CREATE INDEX ON orders (user)');
    for (let i = 0; i < 50; i++) await L.execute('INSERT INTO orders (user, total) VALUES (?, ?)', [i % 10, i * 1.5]);
    await L.execute('ALTER TABLE users ADD COLUMN vip BOOLEAN DEFAULT false');
    await L.delete('kv', 'k1');
    await caughtUp(F, L);
    const sameQueries = [
      ['SELECT COUNT(*) FROM users', []], ["SELECT COUNT(*) FROM users WHERE city = 'moved'", []], ['SELECT id, email FROM users WHERE city = ? ORDER BY id', ['c3']],
      ['SELECT user, COUNT(*) AS n, SUM(total) AS t FROM orders GROUP BY user ORDER BY user', []], ['SELECT vip FROM users WHERE id = ?', [5]],
    ];
    for (const [q, p] of sameQueries) assert.deepEqual(await F.query(q, p), await L.query(q, p), q);
    assert.deepEqual(await F.getMany('kv', ['k0', 'k1', 'k499']), await L.getMany('kv', ['k0', 'k1', 'k499']));
    const ls = await L.stats(), fs_ = await F.stats();
    assert.equal(fs_.txid, ls.txid); assert.equal(fs_.bytes, ls.bytes, 'byte-identical log');
    assert.equal(fs_.readOnly, true); assert.equal(fs_.replica.state, 'streaming'); assert.equal(fs_.replica.lagTransactions, 0);

    // The follower refuses writes; the replica role may only replicate.
    await assert.rejects(F.execute("INSERT INTO users (email) VALUES ('no@x')"), e => e.code === 'READONLY');
    await assert.rejects(F.set('kv', 'x', 1), e => e.code === 'READONLY');
    await assert.rejects(F.execute('CREATE TABLE nope (id INTEGER PRIMARY KEY)'), e => e.code === 'READONLY');
    await assert.rejects(F.compact(), e => e.code === 'READONLY');
    await assert.rejects(F.sql('BEGIN'), e => e.code === 'READONLY');  // a transaction would hold the write gate and stall replication
    const R = await RemoteDB.connect(leader.address, {token: REPLICA});
    await assert.rejects(R.get('kv', 'k0'), e => e.code === 'AUTH');
    await R.close();

    // Follower restart: it continues from its file.
    await F.close(); await follower.stop();
    for (let i = 0; i < 20; i++) await L.set('kv', 'r' + i, i);
    follower = await start(followerArgs());
    assert.doesNotMatch(follower.log(), /SEEDED/);
    F = await RemoteDB.connect(follower.address, {token: ADMIN});
    await caughtUp(F, L);
    assert.equal((await F.get('kv', 'r19')).value, 19);

    // Leader restart (same port): the follower reconnects.
    const port = leader.address.split(':').pop();
    await L.close(); await leader.stop();
    leader = await start([...leaderArgs], Number(port));
    L = await RemoteDB.connect(leader.address, {token: ADMIN});
    await L.set('kv', 'after-restart', true);
    await caughtUp(F, L, 15000);
    assert.equal((await F.get('kv', 'after-restart')).value, true);

    // A compaction on the leader rewrites its log: a follower on the old leader address stops as STALE.
    {
      await L.compact();
      await L.set('kv', 'after-compact', 1);
      for (const until = Date.now() + 10000; Date.now() < until; await sleep(50)) if ((await F.stats()).replica.state === 'stopped') break;
      const st = (await F.stats()).replica;
      assert.equal(st.state, 'stopped'); assert.match(st.lastError, /STALE/);
    }

    // Promotion: the follower becomes writable.
    await F.promote();
    assert.equal((await F.stats()).readOnly, false);
    await F.execute("INSERT INTO users (email, city, n) VALUES ('promoted@x', 'p', 1)");
    assert.equal(await F.queryValue("SELECT COUNT(*) FROM users WHERE city = 'p'"), 1);
    await F.close(); await L.close();
  } finally {
    if (follower) await follower.stop().catch(() => {});
    await leader.stop().catch(() => {});
    await rm(dir, {recursive: true, force: true});
  }
});
