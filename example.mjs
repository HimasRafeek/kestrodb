// node example.mjs — a tour of the API in a temporary folder.
import {Database} from './index.mjs';
import {mkdtempSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';

const dir = mkdtempSync(join(tmpdir(), 'kestrodb-demo-'));
const db = new Database(join(dir, 'app.kdb'), {create: true});
try {
  db.batch(Array.from({length: 256}, (_, i) => ({type: 'set', collection: 'items', key: String(i), value: {title: `Item ${i}`, count: 0}})));
  const before = db.get('items', '0');
  db.set('items', '0', {...before.value, count: 1}, {expectedVersion: before.version});  // fails if someone changed it meanwhile
  console.log(db.getMany('items', ['0', '1', 'missing']));
  await Promise.all(['a', 'b', 'c'].map(k => db.setAsync('items', k, {k})));            // concurrent writes, one commit
  db.delete('items', '255');
  db.backup(join(dir, 'backup.kdb'));
  db.compactTo(join(dir, 'compact.kdb'));
  console.log({record: db.get('items', '0'), stats: db.stats(), verification: db.verify(), directory: dir});
} finally {
  db.close();
}
