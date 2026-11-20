// SQL conveniences shared by Database (in-process, synchronous) and RemoteDB (server,
// asynchronous). Parameters: an array for ?, ?N and $N placeholders, or a plain object for
// :name and @name (keys with or without the leading : or @).
import {KestroDBError} from './validation.mjs';

export function sqlParams(params) {
  if (params === undefined || params === null) return '[]';
  if (Array.isArray(params)) return JSON.stringify(params);
  if (typeof params !== 'object') throw new KestroDBError('INPUT', 'sql params must be an array or a plain object');
  const named = {};
  for (const [k, v] of Object.entries(params)) named[k.replace(/^[:@]/, '')] = v;
  return JSON.stringify(named);
}

// BEGIN/COMMIT/ROLLBACK sent as plain SQL keep inTransaction right.
export function transactionEffect(text) {
  const head = text.trimStart().slice(0, 8).toUpperCase();
  if (head.startsWith('BEGIN') || head.startsWith('START')) return true;
  if (head.startsWith('COMMIT') || head.startsWith('ROLLBACK') || head.startsWith('END')) return false;
  return undefined;
}

function rowsOf(r) {
  if (!r || !Array.isArray(r.rows)) throw new KestroDBError('INPUT', 'query() needs a statement that returns rows; use execute()');
  return r.rows;
}
const firstValue = row => (row ? (Object.values(row)[0] ?? null) : null);

// Adds query/queryOne/queryValue/execute/transaction/prepare to a class whose sql() is
// synchronous (sync = true) or returns a promise.
export function addSqlMethods(cls, sync) {
  const p = cls.prototype;
  if (sync) {
    p.query = function (text, params) { return rowsOf(this.sql(text, params)); };
    p.queryOne = function (text, params) { return this.query(text, params)[0] ?? null; };
    p.queryValue = function (text, params) { return firstValue(this.queryOne(text, params)); };
    p.execute = function (text, params) { const r = this.sql(text, params); return r.changes ?? 0; };
    // fn(db) runs inside BEGIN ... COMMIT; ROLLBACK and rethrow if it throws (or its promise rejects).
    p.transaction = function (fn) {
      this.sql('BEGIN');
      let result;
      try { result = fn(this); } catch (e) { if (this.inTransaction) this.sql('ROLLBACK'); throw e; }
      if (result && typeof result.then === 'function') {
        return result.then(v => { this.sql('COMMIT'); return v; }, e => { if (this.inTransaction) this.sql('ROLLBACK'); throw e; });
      }
      this.sql('COMMIT');
      return result;
    };
    // Prepared statement, better-sqlite3 style: all(params), get(params), value(params), run(params).
    p.prepare = function (text) {
      const db = this;
      return {
        source: text,
        all: params => db.query(text, params),
        get: params => db.queryOne(text, params),
        value: params => db.queryValue(text, params),
        run: params => { const r = db.sql(text, params); return {changes: r.changes ?? 0, lastInsertId: r.lastInsertId ?? null}; },
      };
    };
  } else {
    p.query = async function (text, params) { return rowsOf(await this.sql(text, params)); };
    p.queryOne = async function (text, params) { return (await this.query(text, params))[0] ?? null; };
    p.queryValue = async function (text, params) { return firstValue(await this.queryOne(text, params)); };
    p.execute = async function (text, params) { const r = await this.sql(text, params); return r.changes ?? 0; };
    p.transaction = async function (fn) {
      await this.sql('BEGIN');
      let result;
      try { result = await fn(this); } catch (e) { if (this.inTransaction) { try { await this.sql('ROLLBACK'); } catch {} } throw e; }
      await this.sql('COMMIT');
      return result;
    };
    p.prepare = function (text) {
      const db = this;
      return {
        source: text,
        all: params => db.query(text, params),
        get: params => db.queryOne(text, params),
        value: params => db.queryValue(text, params),
        run: async params => { const r = await db.sql(text, params); return {changes: r.changes ?? 0, lastInsertId: r.lastInsertId ?? null}; },
      };
    };
  }
}
