// Client for kestro-server: many processes (and languages) share one database.
//
//   import {RemoteDB} from 'kestrodb/remote';
//   const db = await RemoteDB.connect('tcp://127.0.0.1:7820', {token});
//   await db.set('users', 'kestro', {name: 'Kestro'});
//   await db.get('users', 'kestro');   // {version, value}
//   await db.execute('INSERT INTO users (email) VALUES (:email)', {email: 'a@example.com'});
//   const rows = await db.query('SELECT * FROM users WHERE city = ?', ['Kandy']);
//   await db.transaction(async db => { ... });   // COMMIT, or ROLLBACK if it throws
//   await db.close();
//
// Every call is async and returns the same shapes as Database. Calls made in the same
// tick travel in one write, and the server group-commits concurrent writes. Errors are
// KestroDBError with a code (INPUT, CONFLICT, LIMIT, AUTH, BUSY, IO, CLOSED, ...).
// A connection the server closed is reopened on the next call (not inside a
// transaction, and never for a request already sent: that one fails with code IO).
//
// Notifications (LISTEN/NOTIFY): await db.listen(channel, cb) / db.watch(table, cb) return an
// async unsubscribe function; db.on('notification', n => ...) sees every channel listened to.
// A listening connection that drops emits 'disconnect' (notifications meanwhile are lost) and
// is reopened and listens again at once (with reconnect: true), then emits 'reconnect'.
import net from 'node:net';
import {EventEmitter} from 'node:events';
import tls from 'node:tls';
import {readFileSync} from 'node:fs';
import {name, key, JSON_MODES, KestroDBError} from './validation.mjs';
import {MAX, packet, encodeBatch, encodeKeys, decodeValues, checkPath} from './protocol.mjs';
import {sqlParams, transactionEffect, addSqlMethods} from './sqlapi.mjs';

function parseAddress(address) {
  if (address.startsWith('unix://')) return {path: address.slice(7)};
  const secure = address.startsWith('tls://');
  const hostPort = address.startsWith('tcp://') || secure ? address.slice(6) : address;
  const colon = hostPort.lastIndexOf(':');
  if (colon < 0) throw new KestroDBError('INPUT', 'INPUT address must be tcp://host:port or unix:///path');
  let host = hostPort.slice(0, colon);
  if (host.startsWith('[') && host.endsWith(']')) host = host.slice(1, -1);
  return {host: host || '127.0.0.1', port: Number(hostPort.slice(colon + 1)), secure};
}
// A server error message ("CONFLICT unique ...") as a KestroDBError with that code.
const serverError = message => new KestroDBError(message.split(' ', 1)[0] || 'ERROR', message);

// A notification frame (status 2): u64 txid, u32 channel length, channel, payload.
function decodeNotification(f) {
  const n = f.readUInt32LE(9);
  return {channel: f.toString('utf8', 13, 13 + n), payload: f.toString('utf8', 13 + n), txid: f.readUInt32LE(5) * 0x100000000 + f.readUInt32LE(1)};
}
const report = fn => { try { fn(); } catch (e) { queueMicrotask(() => { throw e; }); } };
const quoteName = s => '"' + s.replaceAll('"', '""') + '"';

export class RemoteDB extends EventEmitter {
  #socket; #pending = []; #error; #closing = false; #json; #corked = false;
  #channels = new Map();  // channel -> Set of {callback} (listen)
  #address; #addresses = []; #connectedTo = null; #options; #reopening = null; #reconnect = true; #lostTransaction = null;
  /** True between BEGIN and COMMIT/ROLLBACK on this connection. */
  inTransaction = false;
  /** The id of the last row an INSERT on this connection created. */
  lastInsertId = null;

  // tls://host:port uses TLS; the server certificate is verified against the system CAs, or
  // {tls: {ca: PEM | caFile: path}}. {tls: {rejectUnauthorized: false}} skips verification
  // (testing only). SNI is sent only for host names, never IP addresses. A client certificate
  // (for a server with --tls-client-ca): {tls: {cert, key: PEM | certFile, keyFile: paths,
  // passphrase?}}; a user bound to it in the server's users file needs no token.
  // A cluster: several addresses (an array, or separated by commas). The client connects to
  // any member and sends writes to the leader: a member that follows answers READONLY naming
  // it (nothing was applied), and the client moves there and sends the request again, also
  // while an election is under way, for up to {failoverTimeout} ms (15000).
  static async connect(address = 'tcp://127.0.0.1:7820', options = {}) {
    const {jsonValidation = 'strict', reconnect = true} = options;
    if (!Object.hasOwn(JSON_MODES, jsonValidation)) throw new KestroDBError('INPUT', 'INPUT jsonValidation');
    const db = new RemoteDB();
    db.#json = JSON_MODES[jsonValidation];
    db.#addresses = (Array.isArray(address) ? address : String(address).split(',')).map(a => a.trim()).filter(Boolean);
    if (!db.#addresses.length) throw new KestroDBError('INPUT', 'INPUT no server address');
    for (const a of db.#addresses) parseAddress(a);
    db.#address = db.#addresses[0]; db.#options = options; db.#reconnect = reconnect;
    await db.#open();
    return db;
  }
  /** The member this connection uses now. */
  get address() { return this.#connectedTo ?? this.#address; }
  // The current address first, then the other members.
  async #open() {
    const order = [this.#address, ...this.#addresses.filter(a => a !== this.#address)];
    let last;
    for (const a of order) {
      try { await this.#openOne(a); this.#address = this.#connectedTo = a; return; }
      catch (e) { last = e; if (e.code === 'AUTH') throw e; }
    }
    throw last;
  }
  async #openOne(address) {
    const {token, timeout = 5000, tls: tlsOptions = {}} = this.#options;
    const {secure, ...where} = parseAddress(address);
    const file = (value, path) => value ?? (path ? readFileSync(path) : undefined);
    const ca = file(tlsOptions.ca, tlsOptions.caFile), cert = file(tlsOptions.cert, tlsOptions.certFile), key = file(tlsOptions.key, tlsOptions.keyFile);
    const socket = secure
      ? tls.connect({...where, servername: tlsOptions.servername ?? (net.isIP(where.host) ? undefined : where.host), ca, cert, key, passphrase: tlsOptions.passphrase, rejectUnauthorized: tlsOptions.rejectUnauthorized !== false, minVersion: 'TLSv1.2'})
      : net.connect({...where, noDelay: true});
    if (secure) socket.setNoDelay(true);
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => { socket.destroy(); reject(new KestroDBError('IO', 'IO connect timeout')); }, timeout);
      socket.once(secure ? 'secureConnect' : 'connect', () => { clearTimeout(timer); resolve(); });
      socket.once('error', e => { clearTimeout(timer); reject(new KestroDBError('IO', `IO cannot connect to ${address}: ${e.message}`, {cause: e})); });
    });
    this.#socket = socket; this.#error = undefined; this.#pending = [];
    let header = Buffer.alloc(4), headPos = 0, body = null, bodyPos = 0;
    const deliver = frame => { if (frame[0] === 2) { this.#notification(decodeNotification(frame)); return; } const item = this.#pending.shift(); if (!item) throw Error('Unexpected response'); if (frame[0] === 255) item.reject(serverError(frame.toString('utf8', 1))); else item.resolve(frame); };
    socket.on('data', chunk => {
      try {
        let pos = 0;
        while (pos < chunk.length) {
          if (!body && headPos === 0 && chunk.length - pos >= 4) {
            const len = chunk.readUInt32LE(pos);
            if (!len || len > MAX) throw Error('Invalid server response length');
            if (chunk.length - pos - 4 >= len) { deliver(chunk.subarray(pos + 4, pos + 4 + len)); pos += 4 + len; continue; }
          }
          if (!body) {
            const n = Math.min(4 - headPos, chunk.length - pos); chunk.copy(header, headPos, pos, pos + n); headPos += n; pos += n;
            if (headPos < 4) continue;
            const len = header.readUInt32LE(); if (!len || len > MAX) throw Error('Invalid server response length');
            body = Buffer.allocUnsafe(len); bodyPos = 0;
          }
          const n = Math.min(body.length - bodyPos, chunk.length - pos); chunk.copy(body, bodyPos, pos, pos + n); bodyPos += n; pos += n;
          if (bodyPos === body.length) { const frame = body; body = null; headPos = 0; deliver(frame); }
        }
      } catch (e) { this.#fail(socket, new KestroDBError('IO', 'IO ' + e.message)); socket.destroy(); }
    });
    socket.on('error', e => this.#fail(socket, new KestroDBError('IO', 'IO ' + e.message, {cause: e})));
    socket.on('close', () => this.#fail(socket, new KestroDBError('IO', 'IO connection closed. A write without a response has an unknown outcome.')));
    if (token !== undefined) {
      const t = Buffer.from(String(token)), p = packet('A', t.length); t.copy(p, 5);
      try { await this.#send('A', p); } catch (e) { socket.destroy(); throw e; }
    }
    // A reopened connection listens again.
    try { for (const channel of this.#channels.keys()) await new Promise((resolve, reject) => { this.#pending.push({resolve, reject}); socket.write(this.#sqlPacket('LISTEN ' + quoteName(channel))); }); }
    catch (e) { socket.destroy(); throw e; }
  }
  #notification(n) {
    const set = this.#channels.get(n.channel);
    if (set) for (const entry of set) report(() => entry.callback(n));
    report(() => this.emit('notification', n));
  }
  #fail(socket, e) {
    if (socket !== this.#socket) return;  // an old, replaced connection
    if (!this.#error) {
      if (this.inTransaction) {
        // The next call reports the lost transaction instead of running outside it.
        this.#error = this.#lostTransaction = new KestroDBError('IO', 'IO connection lost inside a transaction; it was rolled back');
      } else this.#error = e;
      this.inTransaction = false;
      if (this.#channels.size && !this.#closing) {
        report(() => this.emit('disconnect', e));
        if (this.#reconnect && !this.#lostTransaction) this.#reopening ??= this.#open().then(() => report(() => this.emit('reconnect')), () => {}).finally(() => { this.#reopening = null; });
      }
    }
    for (const p of this.#pending) p.reject(this.#error);
    this.#pending = [];
  }
  // Follows the cluster's leader (see connect); one server: as #sendOnce.
  async #send(cmd, p) {
    if (this.#addresses.length < 2 || cmd === 'A' || cmd === 'Q') return this.#sendOnce(cmd, p);
    const deadline = Date.now() + (this.#options.failoverTimeout ?? 15000);
    for (let wait = 50;; wait = Math.min(wait * 2, 500)) {
      try { return await this.#sendOnce(cmd, p); }
      catch (e) {
        const moved = /^READONLY/.test(e.message), unreachable = e.code === 'IO' && /cannot connect|connect timeout/.test(e.message);
        if ((!moved && !unreachable) || this.inTransaction || this.#closing || Date.now() > deadline) throw e;
        const named = /the leader is (\S+)/.exec(e.message);
        if (named && this.#addresses.includes(named[1])) await this.#moveTo(named[1]);
        else { await new Promise(r => setTimeout(r, wait)); if (moved) await this.#moveTo(this.#addresses[(this.#addresses.indexOf(this.#address) + 1) % this.#addresses.length]); }
      }
    }
  }
  // Another member: once the requests in flight on this connection are answered.
  async #moveTo(address) {
    if (this.#connectedTo === address && !this.#error) return;
    this.#address = address;
    while (this.#pending.length && !this.#error) await new Promise(r => setTimeout(r, 2));
    if (this.#connectedTo === address && !this.#error) return;
    const old = this.#socket; this.#socket = null; this.#connectedTo = null; old?.destroy();
    this.#error ??= new KestroDBError('IO', 'IO moving to the cluster leader');
  }
  #sendOnce(cmd, p) {
    if (this.#closing && cmd !== 'Q') return Promise.reject(new KestroDBError('CLOSED', 'CLOSED the connection was closed'));
    if (p.length > MAX) return Promise.reject(new KestroDBError('LIMIT', 'LIMIT request too large'));
    if (this.#lostTransaction && cmd !== 'Q') { const e = this.#lostTransaction; this.#lostTransaction = null; return Promise.reject(e); }
    if (this.#error) {
      // Reopen a connection the server closed; requests already sent failed with IO.
      if (!this.#reconnect || cmd === 'Q' || cmd === 'A') return Promise.reject(this.#error);
      this.#reopening ??= this.#open().finally(() => { this.#reopening = null; });
      return this.#reopening.then(() => this.#sendOnce(cmd, p));
    }
    const socket = this.#socket;
    if (!this.#corked) { this.#corked = true; socket.cork(); process.nextTick(() => { this.#corked = false; socket.uncork(); }); }
    return new Promise((resolve, reject) => { this.#pending.push({resolve, reject}); socket.write(p); });
  }
  #call(cmd, data = Buffer.alloc(0)) { const p = packet(cmd, data.length); data.copy(p, 5); return this.#send(cmd, p); }

  async get(collection, id) {
    name(collection); key(id);
    const k = collection + '\0' + id, p = packet('G', Buffer.byteLength(k)); p.write(k, 5, 'utf8');
    const r = await this.#send('G', p);
    if (r[0] === 1) return null;
    return {version: Number(r.readBigUInt64LE(1)), value: JSON.parse(r.toString('utf8', 9))};
  }
  async getMany(collection, ids) { const r = await this.#send('M', encodeKeys(collection, ids)); return decodeValues(r, ids.length); }
  async batch(operations) {
    const r = await this.#send('B', encodeBatch(operations, this.#json));
    return {version: Number(r.readBigUInt64LE(1)), operations: operations.length};
  }
  set(collection, key, value, options = {}) { return this.batch([{type: 'set', collection, key, value, ...options}]); }
  delete(collection, key, options = {}) { return this.batch([{type: 'delete', collection, key, ...options}]); }
  /**
   * One SQL statement on the server (see SQL.md). params: an array for ?, ?N and $N, or an
   * object for :name / @name. SELECT returns {rows}; writes return {changes, lastInsertId?}.
   */
  async sql(text, params = []) {
    if (typeof text !== 'string') throw new KestroDBError('INPUT', 'INPUT sql text must be a string');
    let reply;
    // A COMMIT that fails (a conflict) has rolled the transaction back on the server.
    try { reply = await this.#send('E', this.#sqlPacket(text, params)); } catch (e) { if (transactionEffect(text) === false && e.code !== 'IO') this.inTransaction = false; throw e; }
    const r = JSON.parse(reply.toString('utf8', 1));
    const tx = transactionEffect(text); if (tx !== undefined) this.inTransaction = tx;
    if (r.lastInsertId !== undefined) this.lastInsertId = r.lastInsertId;
    return r;
  }
  #sqlPacket(text, params = []) {
    const t = Buffer.from(text), j = Buffer.from(sqlParams(params)), p = packet('E', 4 + t.length + j.length);
    p.writeUInt32LE(t.length, 5); t.copy(p, 9); j.copy(p, 9 + t.length);
    return p;
  }
  /**
   * Calls callback({channel, payload, txid}) for each notification on channel, after its
   * transaction commits, in commit order (LISTEN; not over tls://). Resolves to an async
   * function that stops this callback (UNLISTEN once none is left).
   */
  async listen(channel, callback) {
    if (typeof channel !== 'string' || !channel.length || channel.includes('\0') || Buffer.byteLength(channel) > 255) throw new KestroDBError('INPUT', 'INPUT a channel name is 1 to 255 bytes, without NUL');
    if (typeof callback !== 'function') throw new KestroDBError('INPUT', 'INPUT listen needs a callback function');
    const entry = {callback};
    let set = this.#channels.get(channel);
    if (!set) {
      await this.sql('LISTEN ' + quoteName(channel));
      set = this.#channels.get(channel);  // another listen() may have added it meanwhile
      if (!set) this.#channels.set(channel, set = new Set());
    }
    set.add(entry);
    return async () => {
      if (!set.delete(entry) || set.size || this.#channels.get(channel) !== set) return;
      this.#channels.delete(channel);
      if (!this.#closing && !this.#error) await this.sql('UNLISTEN ' + quoteName(channel));
    };
  }
  /** Stops every callback on channel, or on every channel. */
  async unlisten(channel) {
    if (channel === undefined) { this.#channels.clear(); await this.sql('UNLISTEN *'); }
    else { this.#channels.delete(channel); await this.sql('UNLISTEN ' + quoteName(String(channel))); }
  }
  /** callback({op: 'insert'|'update'|'delete', table, key, txid}) for each change committed to the collection or SQL table. */
  watch(collection, callback) {
    name(collection);
    if (typeof callback !== 'function') throw new KestroDBError('INPUT', 'INPUT watch needs a callback function');
    return this.listen('table_changes:' + collection, n => { const change = JSON.parse(n.payload); change.txid = n.txid; callback(change); });
  }
  /** NOTIFY channel, payload (at most 8000 bytes): inside a transaction, sent when it commits. */
  async notify(channel, payload = '') { await this.sql('SELECT pg_notify(?, ?)', [channel, payload]); }
  async stats() { return JSON.parse((await this.#call('S')).toString('utf8', 1)); }
  async verify() { await this.#call('V'); return {ok: true}; }
  async backup(path) { await this.#call('P', Buffer.from(checkPath(path))); return {path}; }
  async compactTo(path) { await this.#call('C', Buffer.from(checkPath(path))); return {path}; }
  /** Turns a replica into a writable server (admin); it stops following its leader. */
  async promote() { await this.#call('X'); return {ok: true}; }
  /** Online compaction of the server's file (admin): {before, after, keys}. */
  async compact() { return JSON.parse((await this.#call('K')).toString('utf8', 1)); }
  async close() {
    if (this.#closing) return;
    try { if (!this.#error) await this.#call('Q'); } catch {}
    this.#closing = true; this.inTransaction = false;
    this.#socket?.end();
  }
}
addSqlMethods(RemoteDB, false);
