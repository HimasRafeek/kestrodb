// Child process for tests/crash.test.mjs: writes durable transactions until it is killed.
// Each transaction inserts three rows with the same batch number and bumps a counter row;
// the batch number is printed only after COMMIT returns (so the parent knows it is durable).
//   node tests/crash-child.mjs <file> <first batch>
import {existsSync} from 'node:fs';
import {Database} from '../index.mjs';

const [file, first] = [process.argv[2], Number(process.argv[3])];
const db = new Database(file, {create: !existsSync(file)});
db.sql('CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, batch INTEGER NOT NULL, part INTEGER NOT NULL, pad TEXT)');
db.sql('CREATE TABLE IF NOT EXISTS counter (id INTEGER PRIMARY KEY, n INTEGER NOT NULL)');
db.sql('INSERT INTO counter (id, n) VALUES (1, 0) ON CONFLICT (id) DO NOTHING');
const pad = 'x'.repeat(200);
for (let b = first; ; b++) {
  db.sql('BEGIN');
  for (let p = 0; p < 3; p++) db.sql('INSERT INTO items (batch, part, pad) VALUES (?, ?, ?)', [b, p, pad]);
  db.sql('UPDATE counter SET n = n + 1 WHERE id = 1');
  db.sql('COMMIT');
  process.stdout.write(b + '\n');
  if (b % 7 === 0) db.set('kv', 'b' + b, {b});  // key/value writes in between too
}
