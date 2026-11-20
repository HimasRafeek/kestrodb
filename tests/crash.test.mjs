// Crash consistency: a writer is killed at random moments (an abrupt process kill, like a
// crash; not a power loss) and the file is reopened each time. Every acknowledged
// transaction must be there, every transaction must be whole or absent, nothing beyond the
// one in flight may appear, and the file must verify. Also through kestro-server with
// several connections committing at once.
//   KESTRO_CRASH_ROUNDS=100 node --test tests/crash.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Database} from '../index.mjs';
import {RemoteDB} from '../remote.mjs';

const ROUNDS = Number(process.env.KESTRO_CRASH_ROUNDS ?? 12);
const root = fileURLToPath(new URL('../', import.meta.url));
const serverBin = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const sleep = ms => new Promise(r => setTimeout(r, ms));
const debug = process.env.KESTRO_CRASH_DEBUG ? (...m) => console.error(new Date().toISOString().slice(17, 23), ...m) : () => {};
let seed = 7;
const rand = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };

// The committed state must be: batches whole, acknowledged ones present, at most `slack`
// unacknowledged ones beyond, counter equal to the number of batches.
function check(file, acked, slack) {
  const db = new Database(file);
  try {
    db.verify();
    const rows = db.query('SELECT batch, COUNT(*) AS n FROM items GROUP BY batch');
    const parts = new Map(rows.map(r => [r.batch, r.n]));
    for (const [b, n] of parts) assert.equal(n, 3, `batch ${b} is torn: ${n} of 3 rows`);
    for (const b of acked) assert.ok(parts.has(b), `acknowledged batch ${b} is missing`);
    const extra = [...parts.keys()].filter(b => !acked.has(b));
    assert.ok(extra.length <= slack, `${extra.length} unacknowledged batches present (at most ${slack} may be in flight)`);
    assert.equal(db.queryValue('SELECT n FROM counter WHERE id = 1'), parts.size, 'counter disagrees with the batches');
    for (const b of extra) acked.add(b);  // committed, though the kill came before the acknowledgement: durable from now on
    let last = 0; for (const b of parts.keys()) if (b > last) last = b;
    return last;  // the highest batch on disk
  } finally { db.close(); }
}

test(`embedded writer killed ${ROUNDS} times`, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-crash-'));
  const file = join(dir, 'c.kdb');
  try {
    const acked = new Set();
    let next = 1, top = 0;
    for (let round = 0; round < ROUNDS; round++) {
      const child = spawn(process.execPath, [join(root, 'tests', 'crash-child.mjs'), file, String(next)], {stdio: ['ignore', 'pipe', 'pipe']});
      const closed = new Promise(r => child.on('close', r));  // listening from the start: it may exit early
      let buf = '', err = '', mine = 0;
      child.stdout.on('data', d => { buf += d; let i; while ((i = buf.indexOf('\n')) >= 0) { const b = Number(buf.slice(0, i)); acked.add(b); if (b > top) top = b; mine++; buf = buf.slice(i + 1); } });
      child.stderr.on('data', d => { err += d; });
      // Wait until this round's child writes, then kill it at a random point.
      const t0 = Date.now(); while (mine === 0 && Date.now() - t0 < 10000) await sleep(5);
      await sleep(20 + Math.floor(rand() * 250));
      // 'close': the child has exited and its output is read (acknowledgements sent just before
      // the kill arrive after 'exit').
      child.kill('SIGKILL');
      await closed;
      debug('round', round, 'killed; acknowledged', acked.size);
      assert.equal(err.trim(), '', 'child failed: ' + err);
      const last = check(file, acked, 1);
      debug('round', round, 'checked; acknowledged or found', acked.size);
      next = Math.max(next, top, last) + 1;  // batch numbers never repeat
    }
    assert.ok(acked.size > ROUNDS, `only ${acked.size} transactions were acknowledged`);
  } finally { await rm(dir, {recursive: true, force: true}); }
});

test(`kestro-server killed ${Math.max(3, ROUNDS >> 1)} times while 4 connections commit`, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-crash-srv-'));
  const file = join(dir, 's.kdb'), tokenFile = join(dir, 'token.txt'), token = 'crash-test-token-' + process.pid;
  await writeFile(tokenFile, token);
  try {
    const acked = new Set(); let next = 1;
    for (let round = 0; round < Math.max(3, ROUNDS >> 1); round++) {
      const srv = spawn(serverBin, ['--db', file, '--create', '--tcp', '127.0.0.1:0', '--token-file', tokenFile], {stdio: ['ignore', 'pipe', 'pipe']});
      const port = await new Promise((res, rej) => { let b = ''; srv.stdout.on('data', d => { b += d; const m = /READY tcp 127\.0\.0\.1:(\d+)/.exec(b); if (m) res(Number(m[1])); }); srv.on('exit', c => rej(Error('server exited ' + c + ' ' + b))); });
      const url = `tcp://127.0.0.1:${port}`;
      const setup = await RemoteDB.connect(url, {token});
      await setup.sql('CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, batch INTEGER NOT NULL, part INTEGER NOT NULL, pad TEXT)');
      await setup.sql('CREATE TABLE IF NOT EXISTS counter (id INTEGER PRIMARY KEY, n INTEGER NOT NULL)');
      await setup.sql('INSERT INTO counter (id, n) VALUES (1, 0) ON CONFLICT (id) DO NOTHING');
      let stop = false;
      const writers = Array.from({length: 4}, async () => {
        const c = await RemoteDB.connect(url, {token, reconnect: false});
        try {
          while (!stop) {
            const b = next++;
            await c.sql('BEGIN');
            for (let p = 0; p < 3; p++) await c.sql('INSERT INTO items (batch, part, pad) VALUES (?, ?, ?)', [b, p, 'y'.repeat(100)]);
            await c.sql('UPDATE counter SET n = n + 1 WHERE id = 1');
            await c.sql('COMMIT');
            acked.add(b);
          }
        } catch { /* the server was killed */ }
      });
      await sleep(150 + Math.floor(rand() * 300));
      const exited = new Promise(r => srv.on('exit', r));
      srv.kill('SIGKILL'); stop = true;
      await exited; await Promise.allSettled(writers);
      check(file, acked, 4);  // each connection may have one commit in flight
    }
    assert.ok(acked.size > 10, `only ${acked.size} transactions were acknowledged`);
  } finally { await rm(dir, {recursive: true, force: true}); }
});
