// Seeds the fuzz corpora from the repository: SQL statements found in the tests and SQL.md,
// JSON documents, valid protocol requests and real database files.
//   node fuzz/seed.mjs
import {mkdirSync, readFileSync, readdirSync, writeFileSync, copyFileSync, rmSync, existsSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {fileURLToPath, pathToFileURL} from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const dir = n => { const d = join(root, 'fuzz', 'corpus-' + n); mkdirSync(d, {recursive: true}); return d; };
const put = (d, data) => writeFileSync(join(d, createHash('sha1').update(data).digest('hex').slice(0, 16)), data);

// SQL text: string literals in the tests that start with a statement keyword, and SQL.md code.
const sql = new Set();
const lit = /(['"`])((?:SELECT|INSERT|UPDATE|DELETE|CREATE|ALTER|DROP|BEGIN|COMMIT|ROLLBACK|SAVEPOINT|RELEASE|SHOW|DESCRIBE|REPLACE|WITH)\b(?:\\.|(?!\1)[^\\])*)\1/g;
for (const f of readdirSync(join(root, 'tests')).filter(f => f.endsWith('.mjs'))) {
  for (const m of readFileSync(join(root, 'tests', f), 'utf8').matchAll(lit)) if (!m[2].includes('${')) sql.add(m[2]);
}
for (const f of ['src/tests_portable.cpp']) for (const m of readFileSync(join(root, f), 'utf8').matchAll(/"((?:SELECT|INSERT|UPDATE|DELETE|CREATE|ALTER|DROP)\b(?:\\.|[^"\\])*)"/g)) sql.add(m[1].replace(/\\"/g, '"'));
for (const block of (existsSync(join(root, 'SQL.md')) ? readFileSync(join(root, 'SQL.md'), 'utf8') : '').matchAll(/```sql\n([\s\S]*?)```/g))  // when the docs are present
  for (const s of block[1].split(';')) { const t = s.replace(/--.*$/gm, '').trim(); if (t) sql.add(t); }
const parse = dir('parse'), exec = dir('sql');
for (const s of sql) { put(parse, s); put(exec, s); }
// A few multi-statement inputs with parameters for the executor.
for (const s of ['[1, "a"]\nSELECT * FROM users WHERE id = ? OR name = ?', '{"n": "a"}\nSELECT * FROM users WHERE name = :n',
  'BEGIN\nINSERT INTO users (name) VALUES (\'z\')\nSAVEPOINT s\nDELETE FROM users\nROLLBACK TO s\nCOMMIT',
  'DELETE FROM users WHERE id = 1\nSELECT COUNT(*) FROM orders', 'UPDATE orders SET user = 99 WHERE id = 1',
  // Full-text search (the executor's tags table has a FULLTEXT index on label).
  '["x", "x y"]\nSELECT id, MATCH (label) AGAINST (? IN NATURAL LANGUAGE MODE) AS s FROM tags WHERE MATCH (label) AGAINST (?) ORDER BY s DESC',
  'SELECT id FROM tags WHERE MATCH (label) AGAINST (\'+x -"y z" w*\' IN BOOLEAN MODE)',
  'CREATE FULLTEXT INDEX users_name ON users (name, city) WITH (min_token_length = 2)\nSELECT id FROM users WHERE MATCH (city, name) AGAINST (\'k a\') > 0\nALTER TABLE users RENAME COLUMN city TO town\nSELECT id FROM users WHERE MATCH (name, town) AGAINST (\'k\')',
  'BEGIN\nINSERT INTO tags (label) VALUES (\'x y\')\nUPDATE tags SET label = \'q\' WHERE MATCH (label) AGAINST (\'"x y"\' IN BOOLEAN MODE)\nROLLBACK\nDELETE FROM tags WHERE MATCH (label) AGAINST (\'z\')',
  'ALTER TABLE tags ADD FULLTEXT INDEX tl (label)\nDROP INDEX tags_label_fulltext\nSELECT COUNT(*) FROM tags WHERE MATCH (label) AGAINST (\'x\')'])
  put(exec, s);
for (const j of ['{"a":1,"b":[1,2,{"c":"x"}],"d":null,"e":true}', '[1.5e300,-0,"\\u00e9\\ud83d\\ude00",{}]', '{"a":{"b":"c"}}', '"x"', '12345678901234567890'])
  put(parse, j);
for (const q of ['users where age >= 18 order by age desc limit 10', 'orders where status = "open"']) put(parse, q);
console.log('sql/parse seeds:', sql.size);

// Protocol requests: a batch with one put and one delete, a key list.
const proto = dir('protocol');
const batch = () => { const k = Buffer.from('k1'), v = Buffer.from('{"a":1}'); const b = Buffer.alloc(5 + 17 + k.length + v.length + 17 + k.length); let p = 0;
  b[p++] = 0x42; b.writeUInt32LE(2, p); p += 4;
  b[p++] = 1; b.writeBigUInt64LE(0xffffffffffffffffn, p); p += 8; b.writeUInt32LE(k.length, p); b.writeUInt32LE(v.length, p + 4); p += 8; k.copy(b, p); p += k.length; v.copy(b, p); p += v.length;
  b[p++] = 0; b.writeBigUInt64LE(3n, p); p += 8; b.writeUInt32LE(k.length, p); b.writeUInt32LE(0, p + 4); p += 8; k.copy(b, p); return b; };
put(proto, batch());
{ const b = Buffer.alloc(5 + 4 + 2 + 4 + 3); b[0] = 0x4d; b.writeUInt32LE(2, 1); b.writeUInt32LE(2, 5); b.write('k1', 9); b.writeUInt32LE(3, 11); b.write('abc', 15); put(proto, b); }

// Database files: the old-format fixtures and a new file with tables, indexes, constraints.
const open = dir('open');
for (const f of readdirSync(join(root, 'fixtures')).filter(f => f.endsWith('.hdb'))) copyFileSync(join(root, 'fixtures', f), join(open, f));
const {Database} = await import(pathToFileURL(join(root, 'index.mjs')));
const p = join(tmpdir(), 'kdb-seed-' + process.pid + '.kdb');
const db = new Database(p, {create: true});
db.sql('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER CHECK (age >= 0))');
db.sql('CREATE TABLE orders (id INTEGER PRIMARY KEY, user INTEGER REFERENCES users ON DELETE CASCADE, total REAL)');
db.sql('CREATE INDEX ON users (age)');
db.sql('CREATE FULLTEXT INDEX ON users (name)');
for (let i = 0; i < 20; i++) db.sql('INSERT INTO users (name, age) VALUES (?, ?)', ['u' + i, i]);
for (let i = 0; i < 20; i++) db.sql('INSERT INTO orders (user, total) VALUES (?, ?)', [1 + i % 20, i * 1.5]);
db.sql('UPDATE users SET age = age + 1 WHERE id < 5'); db.sql('DELETE FROM users WHERE id = 7');
db.close();
copyFileSync(p, join(open, 'sql-with-snapshot.kdb'));
for (const ext of ['', '.idx', '.sets']) rmSync(p + ext, {force: true});
console.log('open seeds:', readdirSync(open).length);
