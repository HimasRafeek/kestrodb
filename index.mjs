// KestroDB: an embedded database in one file. The engine runs inside this Node process
// through a Node-API addon (kestro.node: bin/ or prebuilds/, see native.mjs). Calls are synchronous, like node:sqlite's
// DatabaseSync. A write returns only once it is durable on disk.
//
// batchAsync/setAsync/deleteAsync queue writes for a background writer thread.
// Every write queued while a flush is running shares the next flush (group
// commit), and each promise resolves after the flush that covers it. Use them
// when many writes are in flight at once, such as in a server.
//
// Advanced: worker/client.mjs (NativeDB) runs the same engine in a separate process.
//
// Change notifications (as PostgreSQL's LISTEN/NOTIFY): listen(channel, cb), watch(collection,
// cb) and notify(channel, payload); also SQL LISTEN / NOTIFY / pg_notify, whose notifications
// are emitted as 'notification' events. Callbacks run later on this thread, after the commit.
import {createRequire} from 'node:module';
import {EventEmitter} from 'node:events';
import {name, key, JSON_MODES, fail} from './validation.mjs';
import {nativeFile, explainLoadError} from './native.mjs';
import {encodeBatch, checkPath} from './protocol.mjs';
import {sqlParams, transactionEffect, addSqlMethods} from './sqlapi.mjs';

const require = createRequire(import.meta.url);
let loaded;
function addon(path) {
  if (path) return require(path);
  if (loaded) return loaded;
  const file = process.env.KESTRO_ADDON || nativeFile('addon');
  try { return loaded = require(file); } catch (e) { throw explainLoadError(e); }
}
const READ_MODES = {auto: 0, mapped: 1, pread: 2};

// A callback's error must not stop the others: it is thrown again on its own.
const report = fn => { try { fn(); } catch (e) { queueMicrotask(() => { throw e; }); } };

export class Database extends EventEmitter {
  #native; #handle; #version = new Float64Array(1); #json;
  #channels = new Map();  // channel -> Set of {callback} (listen)
  /** True between BEGIN and COMMIT/ROLLBACK on this Database. */
  inTransaction = false;
  /** The id of the last row an INSERT through this Database created. */
  lastInsertId = null;

  static open(path, options) { return new Database(path, options); }
  #live() { if (!this.#handle) throw Error('CLOSED'); return this.#handle; }

  // jsonValidation: 'strict' (default) rejects values JSON would silently change, such as
  // Dates, Maps, getters and sparse arrays; 'basic' applies JSON.stringify as is.
  // indexMemory: bytes of RAM for the key index before it moves to <path>.idx (default 32 MB; 0 = all in RAM).
  // orderedMemory: bytes of RAM for ordered keys and secondary indexes (SQL) before their pages move to <path>.sets (default 16 MB; 0 = all in RAM).
  constructor(path, {create = false, readMode = 'auto', orderedSync = false, jsonValidation = 'strict', verifyOnOpen = false, indexMemory, orderedMemory, addon: addonPath} = {}) {
    super();
    checkPath(path);
    if (!(readMode in READ_MODES) || typeof create !== 'boolean' || typeof orderedSync !== 'boolean') throw Error('INPUT create/readMode/orderedSync');
    if (!Object.hasOwn(JSON_MODES, jsonValidation)) throw Error('INPUT jsonValidation');
    this.#json = JSON_MODES[jsonValidation];
    this.#native = addon(addonPath);
    if (indexMemory !== undefined && !(Number.isSafeInteger(indexMemory) && indexMemory >= 0)) throw Error('INPUT indexMemory');
    if (orderedMemory !== undefined && !(Number.isSafeInteger(orderedMemory) && orderedMemory >= 0)) throw Error('INPUT orderedMemory');
    // The dispatcher holds this object weakly: listening never keeps an unreachable one alive.
    const self = new WeakRef(this);
    this.#handle = this.#native.open(path, create, READ_MODES[readMode], orderedSync, this.#version, verifyOnOpen === true, indexMemory, orderedMemory, (notes, lost) => self.deref()?.#dispatch(notes, lost));
  }
  #dispatch(notes, lost) {
    for (const n of notes) {
      const set = this.#channels.get(n.channel);
      if (set) for (const entry of set) report(() => entry.callback(n));
      report(() => this.emit('notification', n));
    }
    if (lost) report(() => this.emit('notificationsLost', lost));
  }
  /**
   * Calls callback({channel, payload, txid}) for each notification on channel (NOTIFY channel,
   * 'payload' or notify()), after its transaction commits, in commit order, asynchronously on
   * this thread. Returns a function that stops this callback. While any channel is listened to,
   * the database keeps the process running (like a server socket); close() or unlisten() ends it.
   * More than 64 MB of undelivered notifications are dropped: 'notificationsLost' (count).
   */
  listen(channel, callback) {
    if (typeof channel !== 'string' || !channel.length || channel.includes('\0') || Buffer.byteLength(channel) > 255) fail('INPUT', 'a channel name is 1 to 255 bytes, without NUL');
    if (typeof callback !== 'function') fail('INPUT', 'listen needs a callback function');
    this.#native.listen(this.#live(), channel);
    let set = this.#channels.get(channel);
    if (!set) this.#channels.set(channel, set = new Set());
    const entry = {callback}; set.add(entry);
    return () => {
      if (!set.delete(entry) || set.size || this.#channels.get(channel) !== set) return;
      this.#channels.delete(channel);
      if (this.#handle) this.#native.unlisten(this.#handle, channel);
    };
  }
  /** Stops every callback on channel, or on every channel (also SQL LISTENs). */
  unlisten(channel) {
    if (channel === undefined) this.#channels.clear(); else this.#channels.delete(channel);
    if (this.#handle) this.#native.unlisten(this.#handle, channel === undefined ? null : String(channel));
  }
  /**
   * Calls callback({op: 'insert'|'update'|'delete', table, key, txid}) for each key of the
   * collection (or row of the SQL table: key is its id) that a commit changes, after it commits.
   * Returns a function that stops it. Same as listen('table_changes:' + collection).
   */
  watch(collection, callback) {
    name(collection);
    if (typeof callback !== 'function') fail('INPUT', 'watch needs a callback function');
    return this.listen('table_changes:' + collection, n => { const change = JSON.parse(n.payload); change.txid = n.txid; callback(change); });
  }
  /** NOTIFY channel, payload (at most 8000 bytes): inside a transaction, sent when it commits. */
  notify(channel, payload = '') { this.sql('SELECT pg_notify(?, ?)', [channel, payload]); }

  get(collection, id) {
    name(collection); key(id);
    const value = this.#native.get(this.#live(), collection + '\0' + id);
    return value === null ? null : {version: this.#version[0], value: JSON.parse(value)};
  }
  #versions = new Float64Array(4096);
  getMany(collection, ids) {
    name(collection);
    if (!Array.isArray(ids) || ids.length > 4096) throw Error('INPUT getMany count');
    const keys = new Array(ids.length);
    for (let i = 0; i < ids.length; i++) keys[i] = collection + '\0' + key(ids[i]);
    const handle = this.#live();
    // Only this object's async writer thread can change the data (a second open of the
    // file is refused). With no async commit outstanding, nothing can run between these
    // synchronous gets, so a plain loop already reads one consistent snapshot.
    const values = this.#outstanding === 0 ? this.#native.getEach(handle, keys, this.#versions) : this.#native.getManyArray(handle, keys, this.#versions);
    for (let i = 0; i < values.length; i++) if (values[i] !== null) values[i] = {version: this.#versions[i], value: JSON.parse(values[i])};
    return values;
  }
  batch(operations) { return {version: this.#native.batch(this.#live(), encodeBatch(operations, this.#json)), operations: operations.length}; }
  set(collection, key, value, options = {}) { return this.batch([{type: 'set', collection, key, value, ...options}]); }
  delete(collection, key, options = {}) { return this.batch([{type: 'delete', collection, key, ...options}]); }
  // Async writes issued in the same tick go to the native writer in one call and
  // commit as one group; each still gets its own version, or fails alone.
  #queued = []; #queuedBytes = 0; #flushScheduled = false; #outstanding = 0;
  batchAsync(operations) {
    let buffer;
    try { this.#live(); buffer = encodeBatch(operations, this.#json); } catch (e) { return Promise.reject(e); }
    return new Promise((resolve, reject) => {
      this.#queued.push({buffer, resolve, reject, count: operations.length});
      this.#queuedBytes += buffer.length;
      if (this.#queuedBytes >= 16 * 1024 * 1024 || this.#queued.length >= 4096) this.#flush();
      else if (!this.#flushScheduled) { this.#flushScheduled = true; queueMicrotask(() => this.#flush()); }
    });
  }
  #flush() {
    this.#flushScheduled = false;
    const items = this.#queued;
    if (!items.length) return;
    this.#queued = []; this.#queuedBytes = 0;
    let done;
    try { done = this.#native.batchAsync(this.#live(), items.map(i => i.buffer)); } catch (e) { for (const i of items) i.reject(e); return; }
    this.#outstanding++;
    done.then(results => {
      this.#outstanding--;
      for (let k = 0; k < items.length; k++) {
        const r = results[k];
        if (typeof r === 'number') items[k].resolve({version: r, operations: items[k].count}); else items[k].reject(Error(r));
      }
    }, e => { this.#outstanding--; for (const i of items) i.reject(e); });
  }
  setAsync(collection, key, value, options = {}) { return this.batchAsync([{type: 'set', collection, key, value, ...options}]); }
  deleteAsync(collection, key, options = {}) { return this.batchAsync([{type: 'delete', collection, key, ...options}]); }
  /**
   * Runs one SQL statement (see SQL.md). params: an array for ?, ?N and $N, or an object
   * for :name / @name. Also: query, queryOne, queryValue, execute, transaction, prepare.
   * SELECT returns {rows}; INSERT/UPDATE/DELETE return {changes, lastInsertId?}.
   */
  sql(text, params = []) {
    if (typeof text !== 'string') fail('INPUT', 'sql text must be a string');
    const r = JSON.parse(this.#native.sql(this.#live(), text, sqlParams(params)));
    const tx = transactionEffect(text); if (tx !== undefined) this.inTransaction = tx;
    if (r.lastInsertId !== undefined) this.lastInsertId = r.lastInsertId;
    return r;
  }
  stats() { return JSON.parse(this.#native.stats(this.#live())); }
  verify() { this.#native.verify(this.#live()); return {ok: true}; }
  backup(path) { this.#native.snapshot(this.#live(), checkPath(path), false); return {path}; }
  compactTo(path) { this.#native.snapshot(this.#live(), checkPath(path), true); return {path}; }
  /**
   * Rewrites the file to hold only current values (reclaims the space of old versions and
   * deleted keys) while reads and writes continue; writes pause only for the switch.
   * Returns {before, after, keys}: committed bytes of the old file at the switch and of the new one.
   */
  compact() { this.#flush(); return JSON.parse(this.#native.compact(this.#live())); }
  /**
   * Continuous archive in dir (archive.kdb + timeline): appends what was committed since the
   * last call (the first copies everything). Call it on a timer. Returns {txid, end}.
   */
  archive(dir) { const [txid, end] = this.#native.archive(this.#live(), checkPath(dir)).split(' ').map(Number); return {txid, end}; }
  /**
   * A new database at dest from source (a database, backup or archive.kdb), cut after
   * transaction txid, or at the last archived point at or before time (Date or ms).
   */
  static restore(source, dest, {txid = 0, time} = {}) {
    const at = time === undefined ? -1 : time instanceof Date ? time.getTime() : Number(time);
    if (!Number.isSafeInteger(txid) || txid < 0 || !(at === -1 || (Number.isFinite(at) && at >= 0))) fail('INPUT', 'restore: txid must be a non-negative integer, time a Date or milliseconds');
    const [t, end] = addon().restore(checkPath(source), checkPath(dest), txid, at).split(' ').map(Number);
    return {txid: t, end};
  }
  // Commits queued async writes first; their promises settle after close returns.
  close() { if (this.#handle) { this.#flush(); this.#native.close(this.#handle); this.#handle = undefined; } this.inTransaction = false; }
  [Symbol.dispose]() { this.close(); }
}
addSqlMethods(Database, true);
