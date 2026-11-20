// Online backups, scheduled backups with retention, continuous archiving and
// point-in-time restore (by transaction and by time) through kestro-server.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {mkdtemp, rm, readdir, readFile} from 'node:fs/promises';
import fs from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Database} from '../index.mjs';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function startServer(dir, args) {
  const child = spawn(serverBinary, ['--db', join(dir, 'app.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', ...args], {stdio: ['pipe', 'pipe', 'pipe']});
  let out = '', err = '';
  child.stderr.on('data', d => { err += d; });
  child.stdout.on('data', d => { out += d; });
  const address = await new Promise((resolve, reject) => {
    const check = () => { const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); };
    child.stdout.on('data', check);
    child.on('exit', code => reject(Error(`server exited ${code}: ${err}`)));
  });
  const exited = new Promise(resolve => child.on('exit', resolve));
  return {address, output: () => out + err, stop: async () => { child.stdin.end(); return exited; }};
}
const restore = (...args) => spawnSync(serverBinary, ['--restore', ...args], {encoding: 'utf8'});
const open = path => new Database(path);

test('server: scheduled online backups, continuous archive, point-in-time restore by transaction and by time', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-backup-'));
  const backups = join(dir, 'backups'), archive = join(dir, 'archive');
  const srv = await startServer(dir, ['--backup-dir', backups, '--backup-every', '1s', '--backup-keep', '2', '--archive-dir', archive, '--archive-every', '200ms']);
  try {
    const db = await RemoteDB.connect(srv.address);
    await db.execute('CREATE TABLE t (id INTEGER PRIMARY KEY, phase TEXT, n INTEGER)');
    // Batch A, then a quiet moment so the archive and the clock pass it.
    for (let i = 0; i < 200; i++) await db.execute('INSERT INTO t (phase, n) VALUES (?, ?)', ['A', i]);
    const txidA = (await db.stats()).txid;
    await sleep(700);
    const timeA = Date.now();
    await sleep(700);
    // Batch B, while backups and archive steps run.
    const until = Date.now() + 2500;
    let b = 0;
    while (Date.now() < until) await db.execute('INSERT INTO t (phase, n) VALUES (?, ?)', ['B', b++]);
    const final = await db.queryValue('SELECT COUNT(*) FROM t');
    assert.equal(final, 200 + b);
    await db.close();
    assert.equal(await srv.stop(), 0);
    assert.match(srv.output(), /KESTRODB BACKUP /);

    // Retention: the newest two backups; each opens and verifies.
    const files = (await readdir(backups)).filter(f => f.endsWith('.kdb')).sort();
    assert.equal(files.length, 2, files.join(','));
    for (const f of files) { const c = open(join(backups, f)); c.verify(); assert.ok(c.queryValue('SELECT COUNT(*) FROM t') >= 200); c.close(); }

    // The archive holds every commit (the server takes a last step when it stops).
    const a = open(join(archive, 'archive.kdb'));
    a.verify();
    assert.equal(a.queryValue('SELECT COUNT(*) FROM t'), final);
    a.close();
    const timeline = (await readFile(join(archive, 'timeline'), 'utf8')).trim().split('\n');
    assert.ok(timeline.length >= 3, timeline.join('|'));

    // Point in time by transaction: exactly batch A.
    let r = restore(join(archive, 'archive.kdb'), '--to', join(dir, 'at-txid.kdb'), '--txid', String(txidA));
    assert.equal(r.status, 0, r.stdout + r.stderr);
    let c = open(join(dir, 'at-txid.kdb'));
    c.verify();
    assert.equal(c.queryValue("SELECT COUNT(*) FROM t WHERE phase = 'A'"), 200);
    assert.equal(c.queryValue("SELECT COUNT(*) FROM t WHERE phase = 'B'"), 0);
    // The restored file is a normal database: it takes new writes.
    c.execute("INSERT INTO t (phase, n) VALUES ('after', 1)");
    c.close();

    // Point in time by clock (ISO 8601 UTC): the last archived point before B started.
    r = restore(join(archive, 'archive.kdb'), '--to', join(dir, 'at-time.kdb'), '--time', new Date(timeA).toISOString());
    assert.equal(r.status, 0, r.stdout + r.stderr);
    c = open(join(dir, 'at-time.kdb'));
    assert.equal(c.queryValue("SELECT COUNT(*) FROM t WHERE phase = 'A'"), 200);
    assert.equal(c.queryValue("SELECT COUNT(*) FROM t WHERE phase = 'B'"), 0);
    c.close();

    // Whole backup restored as is; refusals.
    r = restore(join(backups, files[1]), '--to', join(dir, 'whole.kdb'));
    assert.equal(r.status, 0, r.stdout + r.stderr);
    assert.ok(open(join(dir, 'whole.kdb')).queryValue('SELECT COUNT(*) FROM t') >= 200);
    assert.notEqual(restore(join(archive, 'archive.kdb'), '--to', join(dir, 'whole.kdb')).status, 0, 'refuses to overwrite');
    assert.notEqual(restore(join(archive, 'archive.kdb'), '--to', join(dir, 'x.kdb'), '--txid', '999999999').status, 0, 'transaction beyond the end');
    assert.notEqual(restore(join(archive, 'archive.kdb'), '--to', join(dir, 'y.kdb'), '--time', '2000-01-01T00:00:00Z').status, 0, 'time before the archive');
  } finally { await rm(dir, {recursive: true, force: true}); }
});

test('Database.archive() and Database.restore(): embedded point-in-time restore', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-embarch-'));
  try {
    const db = new Database(join(dir, 'a.kdb'), {create: true});
    db.execute('CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)');
    for (let i = 0; i < 50; i++) db.execute('INSERT INTO t (v) VALUES (?)', ['one']);
    const first = db.archive(join(dir, 'arch'));
    await sleep(30);
    const between = new Date();
    await sleep(30);
    db.execute("UPDATE t SET v = 'two'");
    const second = db.archive(join(dir, 'arch'));
    assert.ok(second.txid > first.txid);
    assert.deepEqual(db.archive(join(dir, 'arch')), second, 'nothing new: same point');
    db.close();
    assert.equal(Database.restore(join(dir, 'arch', 'archive.kdb'), join(dir, 'r1.kdb'), {txid: first.txid}).txid, first.txid);
    assert.equal(Database.restore(join(dir, 'arch', 'archive.kdb'), join(dir, 'r2.kdb'), {time: between}).txid, first.txid);
    for (const f of ['r1.kdb', 'r2.kdb']) { const r = new Database(join(dir, f)); assert.equal(r.queryValue("SELECT COUNT(*) FROM t WHERE v = 'one'"), 50); r.close(); }
    const all = Database.restore(join(dir, 'arch', 'archive.kdb'), join(dir, 'r3.kdb'));
    assert.equal(all.txid, second.txid);
    assert.throws(() => Database.restore(join(dir, 'arch', 'archive.kdb'), join(dir, 'r4.kdb'), {txid: -1}), e => e.code === 'INPUT');
  } finally { await rm(dir, {recursive: true, force: true}); }
});

test('Database.backup() is online: async writers keep committing while it copies', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-online-'));
  try {
    const db = new Database(join(dir, 'a.kdb'), {create: true});
    for (let i = 0; i < 20000; i += 1000) db.batch(Array.from({length: 1000}, (_, j) => ({type: 'set', collection: 'c', key: 'k' + (i + j), value: {i: i + j, pad: 'x'.repeat(200)}})));
    let writes = 0, stop = false;
    const writer = (async () => { while (!stop) { await db.setAsync('w', 'k' + writes, writes); writes++; } })();
    await sleep(20);
    const before = writes;
    db.backup(join(dir, 'b.kdb'));
    stop = true; await writer;
    const b = new Database(join(dir, 'b.kdb'));
    b.verify();
    assert.equal(b.getMany('c', ['k0', 'k19999']).filter(Boolean).length, 2);
    const copied = b.stats().keys;
    b.close();
    assert.ok(copied >= 20000 + before, `copy holds the writes acknowledged before it (${copied})`);
    db.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});

test('server: online compaction while another connection keeps writing; the database reopens with every value', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-compact-'));
  const srv = await startServer(dir, []);
  try {
    const admin = await RemoteDB.connect(srv.address), writer = await RemoteDB.connect(srv.address);
    for (let r = 0; r < 4; r++) await admin.batch(Array.from({length: 2000}, (_, i) => ({type: 'set', collection: 'c', key: 'k' + i, value: {r, pad: 'x'.repeat(100)}})));
    const expect = new Map(Array.from({length: 2000}, (_, i) => ['k' + i, 3]));
    let stop = false, n = 0;
    const writing = (async () => { while (!stop) { const k = 'k' + (n % 2000); await writer.set('c', k, {r: 100 + n, pad: 'y'}); expect.set(k, 100 + n); n++; } })();
    await sleep(30);
    const res = await admin.compact();
    await sleep(30);
    stop = true; await writing;
    assert.ok(res.after < res.before, JSON.stringify(res));
    for (const [k, r] of expect) assert.equal((await admin.get('c', k)).value.r, r, k);
    await admin.verify();
    await admin.close(); await writer.close();
    assert.equal(await srv.stop(), 0);
    const db = open(join(dir, 'app.kdb'));
    db.verify();
    for (const [k, r] of expect) assert.equal(db.get('c', k).value.r, r);
    assert.equal(fs.readdirSync(dir).filter(f => f.includes('compact')).length, 0, 'no leftover files');
    assert.ok(db.compact().after > 0);
    db.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});
