import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {Database} from '../index.mjs';
import {NativeDB} from '../worker/client.mjs';

async function tempDir(fn) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-addon-'));
  try { await fn(dir); } finally { await rm(dir, {recursive: true, force: true}); }
}

test('in-process addon: batch, get, getMany, CAS, delete, reopen', () => tempDir(dir => {
  const path = join(dir, 'data');
  let db = new Database(path, {create: true});
  try {
    const r = db.batch(Array.from({length: 100}, (_, i) => ({type: 'set', collection: 'items', key: String(i), value: {n: i, s: 'வணக்கம் 🌿'}})));
    assert.deepEqual(r, {version: 1, operations: 100});
    assert.deepEqual(db.get('items', '7'), {version: 1, value: {n: 7, s: 'வணக்கம் 🌿'}});
    assert.equal(db.get('items', 'absent'), null);
    const many = db.getMany('items', ['3', 'absent', '3']);
    assert.equal(many[0].value.n, 3); assert.equal(many[1], null); assert.deepEqual(many[2], many[0]);
    assert.equal(db.set('items', '0', {n: -1}, {expectedVersion: 1}).version, 2);
    assert.throws(() => db.set('items', '0', {}, {expectedVersion: 1}), /CONFLICT/);
    assert.throws(() => db.batch([{type: 'set', collection: 'items', key: 'x', value: 1}, {type: 'set', collection: 'items', key: 'x', value: 2}]), /duplicate/);
    assert.throws(() => db.set('items', 'bad', {a: undefined}));
    assert.equal(db.delete('items', '1').version, 3);
    assert.equal(db.get('items', '1'), null);
    assert.equal(db.stats().keys, 99);
    assert.equal(db.stats().inProcess, true);
    assert.deepEqual(db.verify(), {ok: true});
  } finally { db.close(); }
  db = new Database(path);
  try { assert.deepEqual(db.get('items', '0'), {version: 2, value: {n: -1}}); assert.equal(db.stats().txid, 3); }
  finally { db.close(); }
}));

test('in-process addon: closed handles, double close, using-declaration', () => tempDir(dir => {
  const db = new Database(join(dir, 'data'), {create: true});
  db.close(); db.close();
  assert.throws(() => db.get('c', 'k'), /CLOSED/);
  {
    using reopened = new Database(join(dir, 'data'));
    assert.equal(reopened.stats().keys, 0);
  }
  using again = new Database(join(dir, 'data'));
  assert.equal(again.stats().txid, 0);
}));

test('in-process addon refuses an existing path on create and a second owner', () => tempDir(async dir => {
  const path = join(dir, 'data');
  const db = new Database(path, {create: true});
  try {
    assert.throws(() => new Database(path, {create: true}));
    assert.throws(() => new Database(path), /LOCKED/);
    await assert.rejects(NativeDB.open(path), /LOCKED/, 'a worker process cannot take the same file');
  } finally { db.close(); }
}));

test('in-process and worker engines read each other\'s files, backups and compactions', () => tempDir(async dir => {
  const a = join(dir, 'a');
  const db = new Database(a, {create: true});
  for (let i = 0; i < 50; i++) db.set('c', 'k' + (i % 10), {i});
  db.backup(join(dir, 'backup')); db.compactTo(join(dir, 'compact')); db.close();
  for (const p of [a, join(dir, 'backup'), join(dir, 'compact')]) {
    const w = await NativeDB.open(p);
    try {
      assert.deepEqual(await w.get('c', 'k9'), {version: 50, value: {i: 49}});
      await w.verify();
      if (p === a) await w.set('c', 'k9', {i: 'worker'}, {expectedVersion: 50});
    } finally { await w.close(); }
  }
  using back = new Database(a);
  assert.deepEqual(back.get('c', 'k9'), {version: 51, value: {i: 'worker'}});
}));

test('in-process async writes: group commit, order, CAS, errors, close settles everything', () => tempDir(async dir => {
  const path = join(dir, 'data');
  const db = new Database(path, {create: true});
  try {
    const receipts = await Promise.all(Array.from({length: 2000}, (_, i) => db.setAsync('c', 'k' + i, {i})));
    assert.deepEqual(receipts.map(r => r.version), Array.from({length: 2000}, (_, i) => i + 1));
    assert.deepEqual(db.get('c', 'k1999'), {version: 2000, value: {i: 1999}});
    const race = await Promise.allSettled(Array.from({length: 32}, () => db.setAsync('c', 'race', 1, {expectedVersion: 0})));
    assert.equal(race.filter(r => r.status === 'fulfilled').length, 1);
    assert.ok(race.filter(r => r.status === 'rejected').every(r => /CONFLICT/.test(r.reason.message)));
    await assert.rejects(db.batchAsync([{type: 'set', collection: 'c', key: 'x', value: undefined}]), 'invalid JSON rejects');
    assert.equal((await db.deleteAsync('c', 'k0')).version, 2002);
    assert.equal(db.get('c', 'k0'), null);
    // close() commits what is queued and settles the promises before returning.
    const late = Array.from({length: 100}, (_, i) => db.setAsync('late', 'k' + i, i));
    db.close();
    assert.equal((await Promise.all(late)).length, 100);
    await assert.rejects(() => db.setAsync('c', 'k', 1), /CLOSED/);
  } finally { db.close(); }
  using reopened = new Database(path);
  assert.equal(reopened.stats().keys, 2000 - 1 + 1 + 100);
  assert.deepEqual(reopened.verify(), {ok: true});
}));

test('in-process async writes do not keep an idle process alive', async () => {
  const {spawnSync} = await import('node:child_process');
  const script = `import {Database} from ${JSON.stringify(new URL('../index.mjs', import.meta.url).href)};
    import {mkdtempSync} from 'node:fs'; import {tmpdir} from 'node:os'; import {join} from 'node:path';
    const db = new Database(join(mkdtempSync(join(tmpdir(), 'kdb-idle-')), 'd'), {create: true});
    await db.setAsync('c', 'k', 1); console.log('done');`;
  const r = spawnSync(process.execPath, ['--input-type=module', '-e', script], {encoding: 'utf8', timeout: 20000, env: process.env});
  assert.equal(r.status, 0, r.stderr);
  assert.match(r.stdout, /done/);
});

test('mapped reads: values written after open, across remaps, and live corruption detection', () => tempDir(async dir => {
  const path = join(dir, 'data');
  let db = new Database(path, {create: true, readMode: 'mapped'});
  try {
    // Enough data to outgrow several reserve steps, so Windows views are replaced while
    // earlier references stay readable.
    const big = 'v'.repeat(60 * 1024);
    for (let i = 0; i < 120; i++) db.set('c', 'k' + i, {i, big});
    for (let i = 0; i < 120; i++) assert.equal(db.get('c', 'k' + i).value.i, i);
    db.verify();
  } finally { db.close(); }
  const {statSync, openSync, writeSync, closeSync} = await import('node:fs');
  const committed = statSync(path).size;
  db = new Database(path, {readMode: 'mapped'});
  try {
    assert.equal(db.get('c', 'k119').value.i, 119);
    db.set('c', 'after', {ok: true});
    assert.deepEqual(db.get('c', 'after').value, {ok: true});
  } finally { db.close(); }
  assert.ok(statSync(path).size > committed, 'grew by one commit');
  // Flip one byte inside a stored value: a mapped read must refuse it.
  db = new Database(path, {readMode: 'mapped'});
  try {
    const fd = openSync(path, 'r+');
    try { writeSync(fd, Buffer.from('X'), 0, 1, 8192 + 64 + 17 + 6 + 20); } finally { closeSync(fd); }
    assert.throws(() => db.get('c', 'k0'), /CORRUPT/);
  } finally { db.close(); }
}));

test('in-process addon read modes', () => tempDir(dir => {
  for (const readMode of ['auto', 'pread', 'mapped']) {
    const path = join(dir, readMode);
    using db = new Database(path, {create: true, readMode, orderedSync: readMode === 'pread'});
    db.set('c', 'k', 'v');
    assert.equal(db.get('c', 'k').value, 'v');
  }
}));

test('jsonValidation: strict by default, basic follows JSON.stringify, on both APIs', () => tempDir(async dir => {
  const date = new Date(0), cyclic = {}; cyclic.self = cyclic;
  {
    using strict = new Database(join(dir, 'strict'), {create: true});
    assert.throws(() => strict.set('c', 'd', {date}));
    assert.throws(() => strict.set('c', 'u', {a: undefined}));
    using basic = new Database(join(dir, 'basic'), {create: true, jsonValidation: 'basic'});
    basic.set('c', 'd', {date, gone: undefined, n: NaN});
    assert.deepEqual(basic.get('c', 'd').value, {date: '1970-01-01T00:00:00.000Z', n: null});
    assert.throws(() => basic.set('c', 'u', undefined), e => e.code === 'INPUT');
    assert.throws(() => basic.set('c', 'x', cyclic));
    assert.throws(() => basic.set('c', 'b', {big: 1n}));
    assert.equal((await basic.setAsync('c', 'a', {date})).version, 2);
    assert.throws(() => new Database(join(dir, 'bad'), {create: true, jsonValidation: 'none'}), /INPUT/);
  }
  const worker = await NativeDB.open(join(dir, 'worker'), {create: true, jsonValidation: 'basic'});
  try {
    await worker.set('c', 'd', {date});
    assert.deepEqual((await worker.get('c', 'd')).value, {date: '1970-01-01T00:00:00.000Z'});
  } finally { await worker.close(); }
  await assert.rejects(NativeDB.open(join(dir, 'w2'), {create: true, jsonValidation: 'none'}), /INPUT/);
}));

test('index snapshot: reopen skips replay, survives commits, worker and addon agree', () => tempDir(async dir => {
  const path = join(dir, 'data');
  {
    using db = new Database(path, {create: true});
    for (let i = 0; i < 200; i++) db.set('c', 'k' + (i % 50), {i});
    assert.equal(db.stats().openedFrom, 'log-replay');
  }
  {
    using db = new Database(path);
    const st = db.stats();
    assert.equal(st.openedFrom, 'index-snapshot');
    assert.ok(['running', 'passed'].includes(st.verification));
    assert.deepEqual(db.get('c', 'k49'), {version: 200, value: {i: 199}});
    db.set('c', 'k0', 'changed', {expectedVersion: 151});
    assert.deepEqual(db.verify(), {ok: true});
  }
  const w = await NativeDB.open(path, {verifyOnOpen: true});
  try {
    const st = await w.stats();
    assert.equal(st.openedFrom, 'index-snapshot');
    assert.equal(st.verification, 'passed');
    assert.deepEqual(await w.get('c', 'k0'), {version: 201, value: 'changed'});
  } finally { await w.close(); }
  using again = new Database(path, {verifyOnOpen: true});
  assert.equal(again.stats().verification, 'passed');
  assert.equal(again.stats().keys, 50);
}));
