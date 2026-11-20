import { createHash } from 'node:crypto';

export const SLOT_BYTES = 4096;
export const DATA_START = 8192;
export const MAX_PAYLOAD = 2 * 1024 * 1024;
export const ZERO_HASH = '0'.repeat(64);
export const sha = buffer => createHash('sha256').update(buffer).digest();
export class KestroDBError extends Error {
  constructor(code, message, options) { super(message, options); this.name = 'KestroDBError'; this.code = code; }
}
export function fail(code, message) { throw new KestroDBError(code, message); }
// Collection names already accepted (bounded), so hot paths skip the regex.
const validNames = new Set();
export function name(value) {
  if (validNames.has(value)) return value;
  if (typeof value !== 'string' || !/^[A-Za-z_][A-Za-z0-9_]{0,63}$/.test(value)) fail('INPUT', 'Collection must be a 1–64 character ASCII identifier.');
  if (validNames.size < 1024) validNames.add(value);
  return value;
}
export function key(value) {
  // length * 3 bounds the UTF-8 size, so short keys skip the byte count.
  if (typeof value !== 'string' || !value.length || !value.isWellFormed() || value.includes('\0') || (value.length * 3 > 512 && Buffer.byteLength(value) > 512)) fail('INPUT', 'Key must contain 1–512 UTF-8 bytes, valid Unicode, and no NUL.');
  return value;
}
export const compound = (collection, id) => `${collection}\0${id}`;
export function int(value, min, max, label) {
  if (!Number.isSafeInteger(value) || value < min || value > max) fail('INPUT', `Invalid ${label}.`);
  return value;
}
export function json(value) {
  const seen = new WeakSet();
  function walk(v, depth) {
    if (depth > 64) fail('INPUT', 'JSON nesting exceeds 64 levels.');
    if (v === null || typeof v === 'boolean' || typeof v === 'string') return;
    if (typeof v === 'number' && Number.isFinite(v) && (!Number.isInteger(v) || Number.isSafeInteger(v))) return;
    if (typeof v !== 'object' || seen.has(v)) fail('INPUT', 'Use finite, acyclic JSON; store large integers as strings.');
    const array = Array.isArray(v);
    if (!array && Object.getPrototypeOf(v) !== Object.prototype && Object.getPrototypeOf(v) !== null) fail('INPUT', 'Only plain JSON objects are supported.');
    seen.add(v);
    const properties = Reflect.ownKeys(v);
    if (array && properties.length !== v.length + 1) fail('INPUT', 'Sparse arrays and extra array properties are unsupported.');
    for (const property of properties) {
      if (array && property === 'length') continue;
      const descriptor = Object.getOwnPropertyDescriptor(v, property);
      if (typeof property !== 'string' || !descriptor.enumerable || !('value' in descriptor)) fail('INPUT', 'Accessors, hidden properties and symbols are unsupported.');
      if (array && (!/^(0|[1-9][0-9]*)$/.test(property) || Number(property) >= v.length)) fail('INPUT', 'Invalid array property.');
      walk(descriptor.value, depth + 1);
    }
    seen.delete(v);
  }
  walk(value, 0);
  const body = JSON.stringify(value);
  // length * 3 bounds the UTF-8 size, so most values skip the byte count.
  if (body.length * 3 > 256 * 1024 && Buffer.byteLength(body) > 256 * 1024) fail('LIMIT', 'A value may contain at most 256 KiB of JSON.');
  return body;
}
// jsonValidation: 'basic'. JSON.stringify semantics, which is what the SQLite, LMDB and
// LevelDB bindings for Node apply: cycles and BigInt throw, but Dates become strings,
// Maps become {}, undefined/function properties are dropped and NaN becomes null.
// Faster than json() because it does not walk the value first.
export function jsonBasic(value) {
  const body = JSON.stringify(value);
  if (typeof body !== 'string') fail('INPUT', 'Value has no JSON representation.');
  if (body.length * 3 > 256 * 1024 && Buffer.byteLength(body) > 256 * 1024) fail('LIMIT', 'A value may contain at most 256 KiB of JSON.');
  return body;
}
export const JSON_MODES = {strict: json, basic: jsonBasic};
