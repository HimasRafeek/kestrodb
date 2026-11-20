// Engine adapters for bench/compare.mjs. Every engine performs the same operations
// on the same JSON documents with matched durability: a write (or transaction)
// is acknowledged only after the database asked the OS to flush it to disk.
// Values are stored as JSON text and parsed on read by every engine.
//
// Adapter contract (returned by create):
//   overlap        true if requests can be in flight together (in-flight phases use concurrency)
//   set(i)         durable single write of docs[i] under key(i)
//   setInFlight(i) the write used when many are in flight (defaults to set)
//   batch(a, b)    one durable transaction writing docs[a..b)
//   get(i)         -> parsed document or null
//   getMany(keys)  -> parsed documents (null where missing)
//   count()        -> number of keys
//   reopen()       close, open the existing database again, read one key
//   close()        verify where supported, then close
import {createRequire} from 'node:module';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {DatabaseSync} from 'node:sqlite';
import {NativeDB} from '../worker/client.mjs';

const competitors = createRequire(new URL('./competitors/package.json', import.meta.url));
// Read package.json from disk: some packages (lmdb) do not export it.
const versionOf = pkg => { try { return JSON.parse(readFileSync(new URL(`./competitors/node_modules/${pkg}/package.json`, import.meta.url), 'utf8')).version; } catch { return null; } };
const C = 'c';

export function engineFactories({workerBinary, addonPath} = {}) {
  return [
    {
      id: 'inProcess', name: 'KestroDB in-process', kind: 'kestro',
      durability: 'Each commit durable before it returns: write-through on Windows (flush when the file grows), fsync on Linux, F_FULLFSYNC on macOS; concurrent async writes share one commit; JSON: JSON.stringify (jsonValidation basic)',
      async load() { if (addonPath) process.env.KESTRO_ADDON = addonPath; return (await import('../index.mjs')).Database; },
      async create(Database, dir, ctx) {
        const path = join(dir, 'kestro-inproc.kdb');
        // Same JSON work as every other engine here: JSON.stringify only (see README).
        let db = new Database(path, {create: true, jsonValidation: 'basic'});
        return {
          overlap: true, info: db.stats(),
          set: i => db.set(C, ctx.key(i), ctx.docs[i]),
          setInFlight: i => db.setAsync(C, ctx.key(i), ctx.docs[i]),
          batch: (a, b) => db.batch(ctx.ops(a, b)),
          get: i => db.get(C, ctx.key(i))?.value ?? null,
          getMany: keys => db.getMany(C, keys).map(r => r?.value ?? null),
          count: () => db.stats().keys,
          reopen: () => { db.close(); db = new Database(path); db.get(C, ctx.key(0)); },
          close: () => { db.verify(); db.close(); },
        };
      },
    },
    {
      id: 'worker', name: 'KestroDB worker', kind: 'kestro',
      durability: 'Each commit durable before it returns: write-through on Windows (flush when the file grows), fsync on Linux, F_FULLFSYNC on macOS; queued writes share one commit; JSON: JSON.stringify (jsonValidation basic)',
      async load() { return NativeDB; },
      async create(_, dir, ctx) {
        const path = join(dir, 'kestro-worker.kdb'), opts = {jsonValidation: 'basic', ...(workerBinary ? {binary: workerBinary} : {})};
        let db = await NativeDB.open(path, {create: true, ...opts});
        return {
          overlap: true, info: await db.stats(),
          set: i => db.set(C, ctx.key(i), ctx.docs[i]),
          batch: (a, b) => db.batch(ctx.ops(a, b)),
          get: async i => (await db.get(C, ctx.key(i)))?.value ?? null,
          getMany: async keys => (await db.getMany(C, keys)).map(r => r?.value ?? null),
          count: async () => (await db.stats()).keys,
          reopen: async () => { await db.close(); db = await NativeDB.open(path, opts); await db.get(C, ctx.key(0)); },
          close: async () => { await db.verify(); await db.close(); },
        };
      },
    },
    sqliteFactory({id: 'sqlite', name: 'SQLite (node:sqlite)', version: () => process.versions.sqlite,
      load: async () => path => {
        const db = new DatabaseSync(path);
        return {exec: s => db.exec(s), prepare: s => db.prepare(s), close: () => db.close()};
      }}),
    sqliteFactory({id: 'betterSqlite3', name: 'SQLite (better-sqlite3)', version: () => versionOf('better-sqlite3'),
      load: async () => {
        const Database = competitors('better-sqlite3');
        return path => { const db = new Database(path); return {exec: s => db.exec(s), prepare: s => db.prepare(s), close: () => db.close()}; };
      }}),
    {
      id: 'lmdb', name: 'LMDB (lmdb-js)', kind: 'other', version: () => versionOf('lmdb'),
      durability: 'noSync off, overlappingSync off: each commit synced before its promise resolves',
      async load() { return competitors('lmdb'); },
      async create(lmdb, dir, ctx) {
        const path = join(dir, 'lmdb'), opts = {path, encoding: 'string', compression: false, overlappingSync: false, noSync: false};
        let db = lmdb.open(opts);
        const read = s => s === undefined ? null : JSON.parse(s);
        return {
          overlap: true,
          set: i => db.put(ctx.key(i), JSON.stringify(ctx.docs[i])),
          batch: (a, b) => db.transactionSync(() => { for (let i = a; i < b; i++) db.putSync(ctx.key(i), JSON.stringify(ctx.docs[i])); }),
          get: i => read(db.get(ctx.key(i))),
          getMany: keys => keys.map(k => read(db.get(k))),
          count: () => db.getCount(),
          reopen: async () => { await db.close(); db = lmdb.open(opts); db.get(ctx.key(0)); },
          close: () => db.close(),
        };
      },
    },
    {
      id: 'leveldb', name: 'LevelDB (classic-level)', kind: 'other', version: () => versionOf('classic-level'),
      durability: 'sync: true on every put and batch',
      async load() { return competitors('classic-level').ClassicLevel; },
      async create(ClassicLevel, dir, ctx) {
        const path = join(dir, 'leveldb'), opts = {valueEncoding: 'utf8'}, sync = {sync: true};
        let db = new ClassicLevel(path, opts);
        await db.open();
        const read = s => s === undefined ? null : JSON.parse(s);
        return {
          overlap: true,
          set: i => db.put(ctx.key(i), JSON.stringify(ctx.docs[i]), sync),
          batch: (a, b) => db.batch(Array.from({length: b - a}, (_, j) => ({type: 'put', key: ctx.key(a + j), value: JSON.stringify(ctx.docs[a + j])})), sync),
          get: async i => read(await db.get(ctx.key(i))),
          getMany: async keys => (await db.getMany(keys)).map(read),
          count: async () => { let n = 0; for await (const _ of db.keys()) n++; return n; },
          reopen: async () => { await db.close(); db = new ClassicLevel(path, opts); await db.open(); await db.get(ctx.key(0)); },
          close: () => db.close(),
        };
      },
    },
  ];
}

// Both SQLite bindings: WAL + synchronous=FULL, a WITHOUT ROWID key/value table,
// prepared statements, one transaction per write or per batch.
function sqliteFactory({id, name, version, load}) {
  return {
    id, name, kind: 'other', version,
    durability: 'journal_mode=WAL, synchronous=FULL: each commit synced before returning',
    load,
    async create(open, dir, ctx) {
      const path = join(dir, id + '.sqlite');
      let db, put, getStmt;
      const connect = () => {
        db = open(path);
        db.exec('PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT NOT NULL, version INTEGER NOT NULL) WITHOUT ROWID;');
        put = db.prepare('INSERT INTO kv (k, v, version) VALUES (?, ?, 1) ON CONFLICT(k) DO UPDATE SET v = excluded.v, version = version + 1');
        getStmt = db.prepare('SELECT v FROM kv WHERE k = ?');
      };
      connect();
      const get = k => { const r = getStmt.get(k); return r ? JSON.parse(r.v) : null; };
      return {
        overlap: false,
        set: i => put.run(ctx.key(i), JSON.stringify(ctx.docs[i])),
        batch: (a, b) => { db.exec('BEGIN'); try { for (let i = a; i < b; i++) put.run(ctx.key(i), JSON.stringify(ctx.docs[i])); db.exec('COMMIT'); } catch (e) { db.exec('ROLLBACK'); throw e; } },
        get: i => get(ctx.key(i)),
        getMany: keys => keys.map(get),
        count: () => db.prepare('SELECT count(*) AS n FROM kv').get().n,
        reopen: () => { db.close(); connect(); get(ctx.key(0)); },
        close: () => db.close(),
      };
    },
  };
}
