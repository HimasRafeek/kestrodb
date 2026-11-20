import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {NativeDB} from '../worker/client.mjs';

async function withDb(fn, options = {}) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-group-'));
  let db;
  try { db = await NativeDB.open(join(dir, 'data'), {create: true, ...options}); await fn(db, dir); }
  finally { await db?.close(); await rm(dir, {recursive: true, force: true}); }
}

test('concurrent writes share flushes but keep one version each, in order', async () => {
  await withDb(async db => {
    const receipts = await Promise.all(Array.from({length: 1000}, (_, i) => db.set('c', 'k' + i, {i})));
    const versions = receipts.map(r => r.version);
    assert.deepEqual(versions, Array.from({length: 1000}, (_, i) => i + 1), 'one transaction ID each, in submission order');
    const values = await db.getMany('c', Array.from({length: 1000}, (_, i) => 'k' + i));
    values.forEach((v, i) => { assert.equal(v.value.i, i); assert.equal(v.version, i + 1); });
    assert.equal((await db.stats()).txid, 1000);
    await db.verify();
  });
});

test('compare-and-swap inside one group: exactly one winner, losers fail alone', async () => {
  await withDb(async db => {
    const outcomes = await Promise.allSettled(Array.from({length: 64}, (_, i) => db.set('c', 'race', {i}, {expectedVersion: 0})));
    const won = outcomes.filter(o => o.status === 'fulfilled');
    assert.equal(won.length, 1);
    for (const o of outcomes.filter(o => o.status === 'rejected')) assert.match(o.reason.message, /CONFLICT/);
    // A later CAS in the same group sees the earlier accepted write.
    const v = won[0].value.version;
    const [a, b, c] = await Promise.allSettled([
      db.set('c', 'race', {step: 1}, {expectedVersion: v}),
      db.set('c', 'race', {step: 2}, {expectedVersion: v + 1}),
      db.set('c', 'race', {step: 3}, {expectedVersion: v}),
    ]);
    assert.equal(a.status, 'fulfilled'); assert.equal(b.status, 'fulfilled'); assert.equal(c.status, 'rejected');
    assert.deepEqual((await db.get('c', 'race')).value, {step: 2});
    await db.verify();
  });
});

test('a read issued after an unawaited write observes it', async () => {
  await withDb(async db => {
    const write = db.set('c', 'k', {n: 1});
    const read = db.get('c', 'k');
    const del = db.delete('c', 'k');
    const after = db.get('c', 'k');
    assert.equal((await write).version, 1);
    assert.deepEqual((await read).value, {n: 1});
    assert.equal((await del).version, 2);
    assert.equal(await after, null);
  });
});

test('invalid batches in a group fail without affecting their neighbours', async () => {
  await withDb(async db => {
    const results = await Promise.allSettled([
      db.set('c', 'a', 1),
      db.batch([{type: 'set', collection: 'c', key: 'b', value: 2}, {type: 'set', collection: 'c', key: 'missing', value: 3, expectedVersion: 7}]),
      db.set('c', 'd', 4),
    ]);
    assert.deepEqual(results.map(r => r.status), ['fulfilled', 'rejected', 'fulfilled']);
    assert.deepEqual(results.filter(r => r.value).map(r => r.value.version), [1, 2], 'the rejected batch does not use a transaction ID');
    assert.equal(await db.get('c', 'b'), null, 'atomic: no part of the failed batch is visible');
    await db.verify();
  });
});

test('group-committed files reopen with the same data under ordered sync', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-group-'));
  try {
    let db = await NativeDB.open(join(dir, 'data'), {create: true, orderedSync: true});
    await Promise.all(Array.from({length: 300}, (_, i) => db.set('c', 'k' + (i % 50), {i})));
    await db.close();
    db = await NativeDB.open(join(dir, 'data'));
    const st = await db.stats();
    assert.equal(st.keys, 50); assert.equal(st.txid, 300);
    assert.equal((await db.get('c', 'k49')).value.i, 299);
    await db.verify(); await db.close();
  } finally { await rm(dir, {recursive: true, force: true}); }
});
