// Request encodings shared by the worker client (worker/client.mjs) and the in-process
// engine (index.mjs). Each request buffer starts with a 4-byte length and a command
// byte; the body follows at offset 5. The addon ignores the length field.
import {json, name, key, int} from './validation.mjs';
export const MAX = 64 * 1024 * 1024;

export function packet(cmd, bodyBytes) {
  const b = Buffer.allocUnsafe(bodyBytes + 5);
  b.writeUInt32LE(bodyBytes + 1);
  b[4] = cmd.charCodeAt(0);
  return b;
}

// UTF-8 never needs more than 3 bytes per UTF-16 code unit. Below this bound the
// encoder writes into a buffer of that size instead of measuring every string first.
const SCRATCH_LIMIT = 16 * 1024 * 1024;

// encodeJson: json (strict, default) or jsonBasic, per the database's jsonValidation.
export function encodeBatch(operations, encodeJson = json) {
  if (!Array.isArray(operations) || !operations.length || operations.length > 65536) throw Error('INPUT operation count');
  let bound = 4;
  const seen = new Set(), rows = new Array(operations.length);
  for (let i = 0; i < operations.length; i++) {
    const op = operations[i];
    for (const field in op) if (field !== 'type' && field !== 'collection' && field !== 'key' && field !== 'value' && field !== 'expectedVersion') throw Error('Unsupported field: ' + field);
    if (op.type !== 'set' && op.type !== 'delete') throw Error('INPUT operation type');
    name(op.collection); key(op.key);
    // key() rejects malformed Unicode, so distinct strings are distinct UTF-8 bytes.
    const id = op.collection + '\0' + op.key;
    if (seen.has(id)) throw Error('INPUT duplicate transaction key');
    seen.add(id);
    const value = op.type === 'set' ? encodeJson(op.value) : '';
    const expected = op.expectedVersion === undefined ? -1 : int(op.expectedVersion, 0, Number.MAX_SAFE_INTEGER, 'expectedVersion');
    bound += 17 + 3 * (id.length + value.length);
    rows[i] = {id, value, expected, kind: op.type === 'set' ? 1 : 0};
  }
  // Exact sizing for very large batches, so the scratch buffer never balloons.
  const exact = bound > SCRATCH_LIMIT;
  let capacity = bound;
  if (exact) { capacity = 4; for (const r of rows) capacity += 17 + Buffer.byteLength(r.id) + (r.value ? Buffer.byteLength(r.value) : 0); }
  if (capacity > MAX - 64 && exact) throw Error('LIMIT transaction bytes');
  const b = packet('B', capacity);
  b.writeUInt32LE(rows.length, 5);
  let p = 9;
  for (const r of rows) {
    b[p] = r.kind;
    if (r.expected < 0) { b.writeUInt32LE(0xffffffff, p + 1); b.writeUInt32LE(0xffffffff, p + 5); }
    else { b.writeUInt32LE(r.expected % 0x100000000, p + 1); b.writeUInt32LE(Math.floor(r.expected / 0x100000000), p + 5); }
    const idBytes = b.write(r.id, p + 17, 'utf8');
    const valueBytes = r.value ? b.write(r.value, p + 17 + idBytes, 'utf8') : 0;
    b.writeUInt32LE(idBytes, p + 9); b.writeUInt32LE(valueBytes, p + 13);
    p += 17 + idBytes + valueBytes;
  }
  const size = p - 5;
  if (size > MAX - 64) throw Error('LIMIT transaction bytes');
  b.writeUInt32LE(size + 1, 0);
  return p === b.length ? b : b.subarray(0, p);
}

export function encodeKeys(collection, ids) {
  name(collection);
  if (!Array.isArray(ids) || ids.length > 4096) throw Error('INPUT getMany count');
  let size = 4;
  const keys = ids.map(id => { key(id); const k = collection + '\0' + id; size += 4 + Buffer.byteLength(k); return k; });
  const b = packet('M', size);
  b.writeUInt32LE(keys.length, 5);
  let p = 9;
  for (const k of keys) { const n = b.write(k, p + 4, 'utf8'); b.writeUInt32LE(n, p); p += 4 + n; }
  return b;
}

export function decodeValues(r, count) {
  if (r.length < 5 || r.readUInt32LE(1) !== count) throw Error('Invalid getMany response');
  let p = 5;
  const values = [];
  for (let i = 0; i < count; i++) {
    if (p >= r.length) throw Error('Truncated getMany response');
    const found = r[p++];
    if (found === 0) { values.push(null); continue; }
    if (found !== 1 || p + 12 > r.length) throw Error('Invalid getMany item');
    // Two 32-bit halves instead of a BigInt per value; a high half above 2^21 would
    // not be a safe integer.
    const lo = r.readUInt32LE(p), hi = r.readUInt32LE(p + 4), len = r.readUInt32LE(p + 8); p += 12;
    if (hi > 0x1fffff || p + len > r.length) throw Error('Invalid getMany value');
    values.push({version: hi * 0x100000000 + lo, value: JSON.parse(r.toString('utf8', p, p + len))});
    p += len;
  }
  if (p !== r.length) throw Error('Trailing getMany response');
  return values;
}

export function checkPath(path) {
  if (typeof path !== 'string' || !path.length || !path.isWellFormed() || path.includes('\0')) throw Error('INPUT path');
  return path;
}
