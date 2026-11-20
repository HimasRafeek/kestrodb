// Full-text search: CREATE FULLTEXT INDEX and MATCH ... AGAINST (natural language and boolean
// mode). Results and BM25 scores are compared with a reference written here (tokenizer,
// boolean rules, BM25) over random rows and queries, through writes, transactions,
// rollbacks, reopen, compaction and ALTER TABLE; then errors, Unicode, the server's
// permissions and snapshots, and a 100,000-row table where MATCH beats a LIKE scan.
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {Database} from '../index.mjs';
import {RemoteDB} from '../remote.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const serverBinary = process.env.KESTRO_SERVER_BINARY || join(root, 'bin', 'kestro-server' + (process.platform === 'win32' ? '.exe' : ''));
async function tempDir(fn) {
  const dir = await mkdtemp(join(tmpdir(), 'kdb-fts-'));
  try { return await fn(dir); } finally { await rm(dir, {recursive: true, force: true}); }
}
let seed = 12345;
const rand = n => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed % n; };
const pick = a => a[rand(a.length)];

// ---------- Reference (ASCII text) ----------
const tokens = s => typeof s === 'string' ? s.toLowerCase().split(/[^a-z0-9]+/).filter(Boolean) : [];
// A row's tokens: each text column's, '' between columns (no phrase crosses one).
const docTokens = (row, cols) => { const out = []; for (const c of cols) { const t = tokens(row[c]); if (!t.length) continue; if (out.length) out.push(''); out.push(...t); } return out; };
function parseQuery(q, boolean) {
  const items = [], add = it => { if (it.words.length && !items.some(x => JSON.stringify(x) === JSON.stringify(it))) items.push(it); };
  if (!boolean) { for (const w of tokens(q)) add({words: [w], prefix: false, kind: 'opt'}); return items; }
  const re = /([+-]?)(?:"([^"]*)"|([^\s"]+))/g;
  for (const m of q.matchAll(re)) {
    const kind = m[1] === '+' ? 'req' : m[1] === '-' ? 'exc' : 'opt';
    let text = m[2] ?? m[3], prefix = false;
    if (m[3] !== undefined) while (text.endsWith('*')) { prefix = true; text = text.slice(0, -1); }
    add({words: tokens(text), prefix, kind});
  }
  return items;
}
const hit = (tok, w, prefix) => tok !== '' && (prefix ? tok.startsWith(w) : tok === w);
function occurrences(it, toks) {
  let n = 0;
  for (let i = 0; i + it.words.length <= toks.length; i++) if (it.words.every((w, j) => hit(toks[i + j], w, it.prefix && j === it.words.length - 1))) n++;
  return n;
}
// Rows matching and their BM25 scores, as the engine computes them.
function reference(rows, cols, q, boolean) {
  const items = parseQuery(q, boolean), docs = rows.map(r => ({id: r.id, toks: docTokens(r, cols)})).filter(d => d.toks.length);
  const lens = docs.map(d => d.toks.filter(Boolean).length), N = docs.length, avg = N ? lens.reduce((a, b) => a + b, 0) / N : 1;
  // Document frequency: rows with every word of the item (anywhere), as the index finds them.
  const df = items.map(it => docs.filter(d => it.words.every((w, j) => d.toks.some(t => hit(t, w, it.prefix && j === it.words.length - 1)))).length);
  const out = new Map();
  for (const d of docs) {
    const dl = d.toks.filter(Boolean).length; let sum = 0, any = false, ok = true;
    items.forEach((it, i) => {
      const tf = occurrences(it, d.toks);
      if (it.kind === 'exc') { if (tf) ok = false; return; }
      if (it.kind === 'req' && !tf) ok = false;
      if (!tf) return;
      any = true;
      const n = Math.max(N, df[i]), idf = Math.log(1 + (n - df[i] + 0.5) / (df[i] + 0.5));
      sum += idf * tf * 2.2 / (tf + 1.2 * (0.25 + 0.75 * dl / Math.max(avg, 1)));
    });
    if (ok && any) out.set(d.id, Math.max(sum, 1e-9));
  }
  return out;
}

// ---------- Random rows and queries ----------
const VOCAB = ['apple', 'apply', 'applet', 'apricot', 'banana', 'band', 'bandana', 'cat', 'catalog', 'dog', 'dot', 'eagle', 'east', 'fig', 'fire', 'firm', 'grape', 'green', 'hat', 'heat', 'ice', 'idea', 'joke', 'kite', 'lemon', 'lime', 'mango', 'moon', 'night', 'nine', 'x1', 'b52'];
const sentence = (n = 1 + rand(10)) => Array.from({length: n}, (_, i) => { const w = pick(VOCAB); return i && !rand(6) ? pick([', ', '. ', ' - ', '; ']) + (rand(2) ? w.toUpperCase() : w) : (rand(5) ? w : w[0].toUpperCase() + w.slice(1)); }).join(' ');
const value = () => { const r = rand(12); return r === 0 ? null : r === 1 ? 42 : r === 2 ? '' : sentence(); };
function randomQuery() {
  if (rand(3) === 0) return [Array.from({length: 1 + rand(3)}, () => pick(VOCAB)).join(' '), false];
  const parts = [];
  for (let i = 0, n = 1 + rand(4); i < n; i++) {
    const op = pick(['', '', '+', '-']), r = rand(5);
    if (r === 0) parts.push(op + '"' + pick(VOCAB) + ' ' + pick(VOCAB) + '"');
    else if (r === 1) parts.push(op + pick(VOCAB).slice(0, 1 + rand(3)) + '*');
    else parts.push(op + pick(VOCAB));
  }
  return [parts.join(' '), true];
}
const QUERY = 'SELECT id, MATCH (title, body) AGAINST (? IN NATURAL LANGUAGE MODE) AS s FROM docs WHERE MATCH (title, body) AGAINST (? IN NATURAL LANGUAGE MODE) ORDER BY s DESC, id';
const QUERY_BOOL = QUERY.replaceAll('NATURAL LANGUAGE', 'BOOLEAN');
// The engine against the reference: the same rows, scores within 1e-9, best first.
function check(h, mirror, label, queries = 40) {
  const rows = [...mirror.values()]; let found = 0;
  for (let k = 0; k < queries; k++) {
    const [q, boolean] = k === 0 ? ['apple', false] : randomQuery();
    const want = reference(rows, ['title', 'body'], q, boolean);
    const got = h.query(boolean ? QUERY_BOOL : QUERY, [q, q]); found += got.length;
    assert.deepEqual(got.map(r => r.id).sort((a, b) => a - b), [...want.keys()].sort((a, b) => a - b), `${label}: ${q} (${boolean ? 'boolean' : 'natural'})`);
    for (let i = 0; i < got.length; i++) {
      assert.ok(Math.abs(got[i].s - want.get(got[i].id)) <= 1e-9 * Math.max(1, want.get(got[i].id)), `${label}: ${q}: score of ${got[i].id} ${got[i].s} vs ${want.get(got[i].id)}`);
      if (i) assert.ok(got[i - 1].s >= got[i].s, `${label}: ${q}: ranked best first`);
    }
    // The same filter without the index's help (OR keeps it from narrowing the scan).
    const all = h.query(`SELECT id FROM docs WHERE MATCH (title, body) AGAINST (? IN ${boolean ? 'BOOLEAN' : 'NATURAL LANGUAGE'} MODE) OR id < 0 ORDER BY id`, [q]);
    assert.deepEqual(all.map(r => r.id), [...want.keys()].sort((a, b) => a - b), `${label}: ${q} scanned`);
  }
  assert.ok(found > queries, `${label}: the queries find rows`);
}
// Index entries: per row with tokens, one per distinct term and one for its length.
const entries = mirror => { let n = 0; for (const r of mirror.values()) { const t = docTokens(r, ['title', 'body']).filter(Boolean); if (t.length) n += new Set(t).size + 1; } return n; };
const shown = (h, table = 'docs') => h.query('SHOW INDEX FROM ' + table).filter(r => r.type === 'FULLTEXT');

test('MATCH ... AGAINST matches the reference through writes, transactions, reopen and compaction', () => tempDir(async dir => {
  const path = join(dir, 'f.kdb');
  let h = new Database(path, {create: true});
  h.sql('CREATE TABLE docs (id INTEGER PRIMARY KEY, title, body, n INTEGER)');  // any values: numbers are not indexed
  const mirror = new Map();
  const insert = (id, title, body) => { h.sql('INSERT INTO docs (id, title, body, n) VALUES (?, ?, ?, ?)', [id, title, body, id % 7]); mirror.set(id, {id, title, body}); };
  for (let i = 1; i <= 150; i++) insert(i, value(), value());
  h.sql('CREATE FULLTEXT INDEX docs_text ON docs (title, body)');  // built over existing rows
  for (let i = 151; i <= 300; i++) insert(i, value(), value());   // kept by commits
  assert.deepEqual(shown(h).map(r => [r.name, r.columns, r.entries]), [['docs_text', ['title', 'body'], entries(mirror)]]);
  check(h, mirror, 'built and inserted');
  // UPDATE (some rows lose all their text), DELETE, multi-row statements.
  for (let k = 0; k < 60; k++) {
    const id = 1 + rand(300), t = value(), b = value();
    h.sql('UPDATE docs SET title = ?, body = ? WHERE id = ?', [t, b, id]);
    if (mirror.has(id)) Object.assign(mirror.get(id), {title: t, body: b});
  }
  h.sql("UPDATE docs SET body = body || ' lemon lemon' WHERE n = 3 AND body IS NOT NULL AND body <> ''");
  for (const r of mirror.values()) if (r.id % 7 === 3 && r.body !== null && r.body !== '') r.body = String(r.body) + ' lemon lemon';  // 42 too: || makes text
  h.sql('DELETE FROM docs WHERE n = 5'); for (const id of [...mirror.keys()]) if (id % 7 === 5) mirror.delete(id);
  // Writes through MATCH itself.
  const hits = reference([...mirror.values()], ['title', 'body'], '+fire -ice', true);
  assert.equal(h.sql("DELETE FROM docs WHERE MATCH (title, body) AGAINST ('+fire -ice' IN BOOLEAN MODE)").changes, hits.size);
  for (const id of hits.keys()) mirror.delete(id);
  const moon = reference([...mirror.values()], ['title', 'body'], 'moon', false);
  assert.equal(h.sql("UPDATE docs SET title = 'renamed by match' WHERE MATCH (title, body) AGAINST ('moon')").changes, moon.size);
  for (const id of moon.keys()) mirror.get(id).title = 'renamed by match';
  assert.equal(shown(h)[0].entries, entries(mirror));
  check(h, mirror, 'after writes');
  // A transaction sees its own rows (not yet in the index); ROLLBACK leaves the index as it was.
  const before = shown(h)[0].entries;
  h.sql('BEGIN');
  h.sql("INSERT INTO docs (id, title, body) VALUES (1001, 'zebra crossing', 'zebra')");
  h.sql("UPDATE docs SET body = 'zebra' WHERE id = (SELECT MIN(id) FROM docs)");
  assert.equal(h.query("SELECT COUNT(*) AS n FROM docs WHERE MATCH (title, body) AGAINST ('zebra')")[0].n, 2);
  assert.deepEqual(h.query("SELECT id FROM docs WHERE MATCH (title, body) AGAINST ('\"zebra crossing\"' IN BOOLEAN MODE)"), [{id: 1001}]);
  h.sql('SAVEPOINT s'); h.sql('DELETE FROM docs WHERE id = 1001');
  assert.equal(h.query("SELECT COUNT(*) AS n FROM docs WHERE MATCH (title, body) AGAINST ('zebra')")[0].n, 1);
  h.sql('ROLLBACK TO s');
  assert.equal(h.query("SELECT COUNT(*) AS n FROM docs WHERE MATCH (title, body) AGAINST ('zebra')")[0].n, 2);
  h.sql('ROLLBACK');
  assert.equal(h.query("SELECT COUNT(*) AS n FROM docs WHERE MATCH (title, body) AGAINST ('zebra')")[0].n, 0);
  assert.equal(shown(h)[0].entries, before);
  // COMMIT puts the transaction's rows in.
  h.sql('BEGIN'); h.sql("INSERT INTO docs (id, title, body) VALUES (1002, 'quiet zebra', NULL)"); h.sql('COMMIT');
  mirror.set(1002, {id: 1002, title: 'quiet zebra', body: null});
  assert.deepEqual(h.query("SELECT id FROM docs WHERE MATCH (title, body) AGAINST ('zebra')"), [{id: 1002}]);
  check(h, mirror, 'after transactions');
  // Scores are the same after reopen (statistics rebuilt from the rows) and compaction.
  const sample = () => h.query(QUERY, ['apple banana lemon zebra', 'apple banana lemon zebra']);
  const s1 = sample();
  assert.ok(s1.length > 50 && new Set(s1.map(r => r.s)).size > 20, 'varied scores');
  h.close(); h = new Database(path);
  assert.deepEqual(shown(h).map(r => [r.name, r.columns, r.entries]), [['docs_text', ['title', 'body'], entries(mirror)]]);
  assert.deepEqual(sample(), s1);
  check(h, mirror, 'after reopen', 20);
  h.compact();
  assert.deepEqual(sample(), s1);
  for (let i = 0; i < 20; i++) { const id = 2000 + i; insert(id, sentence(), sentence()); }
  check(h, mirror, 'after compaction', 20);
  h.close(); h = new Database(path);
  check(h, mirror, 'reopened after compaction', 10);
  // DROP INDEX: MATCH needs it again.
  h.sql('DROP INDEX docs_text');
  assert.deepEqual(shown(h), []);
  assert.throws(() => h.query("SELECT id FROM docs WHERE MATCH (title, body) AGAINST ('apple')"), /needs a FULLTEXT index on exactly these columns of docs/);
  h.close(); h = new Database(path);
  assert.deepEqual(shown(h), []);
  h.close();
}));

test('relevance: rarer words, more occurrences and shorter rows rank higher; boolean mode details', () => tempDir(async dir => {
  const h = new Database(join(dir, 'r.kdb'), {create: true});
  h.sql('CREATE TABLE a (id INTEGER PRIMARY KEY, txt TEXT, FULLTEXT INDEX a_txt (txt))');
  const rows = ['common words here', 'common rare', 'common common common filler', 'common filler filler filler filler filler filler filler',
    'common filler', 'rare rare', 'Never run mysqld as root', 'run never', 'database databases DataBase', 'x-ray e-mail', 'unrelated'];
  rows.forEach((t, i) => h.sql('INSERT INTO a (id, txt) VALUES (?, ?)', [i + 1, t]));
  const ids = (q, mode = '') => h.query(`SELECT id FROM a WHERE MATCH (txt) AGAINST (? ${mode}) ORDER BY MATCH (txt) AGAINST (? ${mode}) DESC, id`, [q, q]).map(r => r.id);
  const B = 'IN BOOLEAN MODE';
  assert.deepEqual(ids('rare'), [6, 2]);                          // two occurrences first
  assert.deepEqual(ids('common rare').slice(0, 2), [2, 6]);        // both words beat the rare one alone
  assert.deepEqual(ids('common').slice(0, 4), [3, 2, 5, 1]);       // more occurrences, then shorter rows (2 and 5 tie)
  assert.deepEqual(ids('common').at(-1), 4);                       // the longest row last
  assert.deepEqual(ids('database'), [9]);                          // case folded, no stemming (databases is another word)
  assert.deepEqual(ids('databas*', B), [9]);
  assert.deepEqual(ids('"never run"', B), [7]);
  assert.deepEqual(ids('"run never"', B), [8]);
  assert.deepEqual(ids('+run -root', B), [8]);
  assert.deepEqual(ids('-run', B), []);                           // only excluded words match nothing
  assert.deepEqual(ids('+common +rare', B), [2]);
  assert.deepEqual(ids('+common rare', B)[0], 2);                  // an optional word only adds relevance
  assert.equal(ids('+common rare', B).length, 5);
  assert.deepEqual(ids('x-ray', B), [10]);                        // punctuation inside a word: a phrase
  assert.deepEqual(ids('ray-x', B), []);
  assert.deepEqual(ids('e-mail x-ray'), [10]);                     // natural mode: just words
  assert.deepEqual(ids('nothing matches'), []);
  assert.deepEqual(ids(''), []);
  // Relevance in the select list for every row (0 when the row does not match); NULL query.
  assert.deepEqual(h.query("SELECT id, MATCH (txt) AGAINST ('unrelated') > 0 AS m FROM a WHERE id >= 10 ORDER BY id"), [{id: 10, m: false}, {id: 11, m: true}]);
  assert.deepEqual(h.query('SELECT COUNT(*) AS n FROM a WHERE MATCH (txt) AGAINST (NULL)'), [{n: 0}]);
  // Narrowing forms: MATCH > 0, a comparison flipped, a parameter; in aggregates, GROUP BY, HAVING, DISTINCT, LIMIT.
  assert.deepEqual(h.query("SELECT id FROM a WHERE MATCH (txt) AGAINST ('rare') > 0 ORDER BY id").map(r => r.id), [2, 6]);
  assert.deepEqual(h.query("SELECT id FROM a WHERE 0 < MATCH (txt) AGAINST (:q) AND id > 2", {q: 'rare'}).map(r => r.id), [6]);
  assert.deepEqual(h.query("SELECT COUNT(*) AS n, MAX(MATCH (txt) AGAINST ('common')) > 0 AS m FROM a WHERE MATCH (txt) AGAINST ('common')"), [{n: 5, m: true}]);
  assert.deepEqual(h.query("SELECT MATCH (txt) AGAINST ('rare') > 0 AS m, COUNT(*) AS n FROM a GROUP BY MATCH (txt) AGAINST ('rare') > 0 HAVING COUNT(*) > 1 ORDER BY m"), [{m: false, n: 9}, {m: true, n: 2}]);
  assert.deepEqual(h.query("SELECT DISTINCT MATCH (txt) AGAINST ('never') > 0 AS m FROM a ORDER BY m"), [{m: false}, {m: true}]);
  assert.deepEqual(h.query("SELECT id FROM a WHERE MATCH (txt) AGAINST ('common filler') ORDER BY MATCH (txt) AGAINST ('common filler') DESC LIMIT 2 OFFSET 1").length, 2);
  assert.deepEqual(h.query("SELECT id, ROW_NUMBER() OVER (ORDER BY MATCH (txt) AGAINST ('rare') DESC, id) AS r FROM a WHERE MATCH (txt) AGAINST ('rare')").sort((x, y) => x.id - y.id), [{id: 2, r: 2}, {id: 6, r: 1}]);
  // Subqueries, WITH and UNION each prepare their own MATCH.
  assert.deepEqual(h.query("SELECT id FROM a WHERE id IN (SELECT id FROM a WHERE MATCH (txt) AGAINST ('rare')) ORDER BY id").map(r => r.id), [2, 6]);
  assert.deepEqual(h.query("WITH r AS (SELECT id FROM a WHERE MATCH (txt) AGAINST ('rare')) SELECT COUNT(*) AS n FROM r"), [{n: 2}]);
  assert.deepEqual(h.query("SELECT id FROM a WHERE MATCH (txt) AGAINST ('rare') UNION SELECT id FROM a WHERE MATCH (txt) AGAINST ('database') ORDER BY 1").map(r => r.id), [2, 6, 9]);
  assert.deepEqual(h.query("SELECT id FROM a WHERE MATCH (a.txt) AGAINST ('database')"), [{id: 9}]);
  // min_token_length: shorter words are not indexed nor searched (a prefix may be shorter).
  h.sql('CREATE TABLE m (id INTEGER PRIMARY KEY, t TEXT)');
  h.sql("INSERT INTO m (id, t) VALUES (1, 'go to the zoo'), (2, 'going home'), (3, 'a b c')");
  h.sql('CREATE FULLTEXT INDEX m_t ON m (t) WITH (min_token_length = 3)');
  const mids = (q, mode = '') => h.query(`SELECT id FROM m WHERE MATCH (t) AGAINST (? ${mode}) ORDER BY id`, [q]).map(r => r.id);
  assert.deepEqual(mids('go'), []);
  assert.deepEqual(mids('zoo'), [1]);
  assert.deepEqual(mids('go*', B), [2]);
  assert.deepEqual(mids('"to the zoo"', B), [1]);                 // "to" is not indexed: the phrase is "the zoo"
  assert.equal(shown(h, 'm')[0].entries, 3 + 3);                  // the, zoo and going, home, each row with its length; row 3 has no words
  h.close();
}));

test('Unicode: Latin-1, Latin Extended-A, Greek and Cyrillic fold case; other scripts are words; accents stay', () => tempDir(async dir => {
  const h = new Database(join(dir, 'u.kdb'), {create: true});
  h.sql('CREATE TABLE u (id INTEGER PRIMARY KEY, t TEXT)');
  h.sql('CREATE FULLTEXT INDEX ON u (t)');
  const texts = ['Café CRÈME brûlée', 'ÉCOLE Straße ŁÓDŹ', 'ΑΘΗΝΑ Σοφία', 'МОСКВА привет', '東京タワー 東京', 'naïve—dash…ellipsis', 'cafe creme', 'ÆØÅ æøå', 'tab\there nbsp'];
  texts.forEach((t, i) => h.sql('INSERT INTO u (id, t) VALUES (?, ?)', [i + 1, t]));
  const ids = q => h.query('SELECT id FROM u WHERE MATCH (t) AGAINST (?) ORDER BY id', [q]).map(r => r.id);
  assert.deepEqual(ids('café'), [1]);
  assert.deepEqual(ids('cafe'), [7]);                // accents are not removed
  assert.deepEqual(ids('crème'), [1]);
  assert.deepEqual(ids('école straße'), [2]);
  assert.deepEqual(ids('łódź'), [2]);
  assert.deepEqual(ids('αθηνα'), [3]);
  assert.deepEqual(ids('ΣΟΦΊΑ'), [3]);
  assert.deepEqual(ids('москва ПРИВЕТ'), [4]);
  assert.deepEqual(ids('東京'), [5]);
  assert.deepEqual(ids('タワー'), []);                // no word segmentation: 東京タワー is one word
  assert.deepEqual(ids('dash ellipsis naïve'), [6]); // general punctuation separates
  assert.deepEqual(ids('æøå'), [8]);
  assert.deepEqual(ids('nbsp'), [9]);
  assert.deepEqual(h.query("SELECT id FROM u WHERE MATCH (t) AGAINST ('東京*' IN BOOLEAN MODE) ORDER BY id").map(r => r.id), [5]);
  h.close();
}));

test('after a reopen: first builds without the writer lock leave FULLTEXT indexes right; MATCH in a first UPDATE/DELETE', () => tempDir(async dir => {
  const path = join(dir, 'cold.kdb'), words = ['red', 'green', 'blue', 'amber', 'violet'];
  const text = i => `${words[i % 5]} ${words[(i * 3) % 5]} item${i}`;
  const ref = new Map();
  {
    const h = new Database(path, {create: true});
    h.sql('CREATE TABLE c (id INTEGER PRIMARY KEY, kind TEXT, t TEXT)');
    h.sql('CREATE INDEX ON c (kind)');
    h.sql('CREATE FULLTEXT INDEX c_t ON c (t)');
    for (let i = 1; i <= 3000; i++) { h.sql('INSERT INTO c (id, kind, t) VALUES (?, ?, ?)', [i, 'k' + (i % 7), text(i)]); ref.set(i, text(i)); }
    h.close();
  }
  const want = w => [...ref].filter(([, t]) => t.split(' ').includes(w)).map(([id]) => id).sort((a, b) => a - b);
  // First statement after the reopen: an UPDATE whose WHERE is a MATCH (writes prepare only unique indexes).
  let h = new Database(path);
  const amber = want('amber');
  assert.equal(h.sql("UPDATE c SET t = 'violet repainted' WHERE MATCH (t) AGAINST ('amber')").changes, amber.length);
  for (const id of amber) ref.set(id, 'violet repainted');
  assert.deepEqual(h.query("SELECT id FROM c WHERE MATCH (t) AGAINST ('violet') ORDER BY id").map(r => r.id), want('violet'));
  h.close();
  // A query through the plain index first (builds the table's indexes without the writer lock), then MATCH.
  h = new Database(path);
  assert.equal(h.query("SELECT COUNT(*) AS n FROM c WHERE kind = 'k3'")[0].n, [...ref.keys()].filter(i => i % 7 === 3).length);
  for (const w of ['red', 'green', 'blue', 'violet', 'repainted']) assert.deepEqual(h.query('SELECT id FROM c WHERE MATCH (t) AGAINST (?) ORDER BY id', [w]).map(r => r.id), want(w), w);
  h.close();
  // A first DELETE by MATCH.
  h = new Database(path);
  const blue = want('blue');
  assert.equal(h.sql("DELETE FROM c WHERE MATCH (t) AGAINST ('blue')").changes, blue.length);
  for (const id of blue) ref.delete(id);
  assert.deepEqual(h.query("SELECT id FROM c WHERE MATCH (t) AGAINST ('red') ORDER BY id").map(r => r.id), want('red'));
  assert.equal(h.query('SELECT COUNT(*) AS n FROM c')[0].n, ref.size);
  h.close();
}));

test('ALTER TABLE keeps FULLTEXT indexes right; CREATE TABLE and ALTER TABLE ADD FULLTEXT', () => tempDir(async dir => {
  const path = join(dir, 'a.kdb');
  let h = new Database(path, {create: true});
  h.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, title TEXT, body TEXT, tag TEXT, FULLTEXT (title, body))');
  h.sql("INSERT INTO t (title, body, tag) VALUES ('red apple', 'sweet fruit', 'x'), ('green pear', 'apple tree', 'y'), ('blue sky', NULL, 'apple')");
  const ids = (cols, q) => h.query(`SELECT id FROM t WHERE MATCH (${cols}) AGAINST (?) ORDER BY id`, [q]).map(r => r.id);
  assert.deepEqual(shown(h, 't').map(r => [r.name, r.columns]), [['t_title_body_fulltext', ['title', 'body']]]);
  assert.deepEqual(ids('body, title', 'apple'), [1, 2]);  // columns in any order
  assert.throws(() => ids('title', 'apple'), /needs a FULLTEXT index on exactly these columns/);
  // A plain index on a column of the FULLTEXT one stays separate.
  h.sql('CREATE INDEX t_title ON t (title)');
  assert.deepEqual(h.query("SELECT id FROM t WHERE title = 'blue sky'"), [{id: 3}]);
  // RENAME COLUMN: the index follows (its automatic name too).
  h.sql('ALTER TABLE t RENAME COLUMN body TO content');
  assert.deepEqual(shown(h, 't').map(r => [r.name, r.columns]), [['t_title_content_fulltext', ['title', 'content']]]);
  assert.deepEqual(ids('title, content', 'apple'), [1, 2]);
  assert.throws(() => ids('title, body', 'apple'), /needs a FULLTEXT index/);
  h.sql("UPDATE t SET content = 'apple pie' WHERE id = 3");
  assert.deepEqual(ids('title, content', 'pie'), [3]);
  // DROP COLUMN: the index keeps its other columns; dropping the last one drops it.
  h.sql('ALTER TABLE t DROP COLUMN content');
  assert.deepEqual(shown(h, 't').map(r => [r.name, r.columns]), [['t_title_fulltext', ['title']]]);
  assert.deepEqual(ids('title', 'apple'), [1]);
  assert.deepEqual(ids('title', 'pie'), []);
  // ALTER TABLE ADD FULLTEXT with a name and options; RENAME TO keeps names it was given.
  h.sql('ALTER TABLE t ADD FULLTEXT INDEX tags (tag) WITH (min_token_length = 2)');
  assert.deepEqual(ids('tag', 'apple'), [3]);
  h.sql('ALTER TABLE t RENAME TO t2');
  assert.deepEqual(shown(h, 't2').map(r => [r.name, r.columns]), [['t2_title_fulltext', ['title']], ['tags', ['tag']]]);
  assert.deepEqual(h.query("SELECT id FROM t2 WHERE MATCH (tag) AGAINST ('apple x')").map(r => r.id), [3]);  // x is shorter than 2
  h.sql('ALTER TABLE t2 DROP COLUMN title');
  assert.deepEqual(shown(h, 't2').map(r => r.name), ['tags']);
  h.close(); h = new Database(path);
  assert.deepEqual(h.query('SHOW INDEX FROM t2').map(r => [r.name, r.type]), [['PRIMARY', 'BTREE'], ['tags', 'FULLTEXT']]);
  assert.deepEqual(h.query("SELECT id FROM t2 WHERE MATCH (tag) AGAINST ('apple')"), [{id: 3}]);
  h.sql('ALTER TABLE t2 DROP INDEX tags');
  assert.deepEqual(shown(h, 't2'), []);
  // DROP TABLE takes its FULLTEXT indexes along.
  h.sql('CREATE TABLE d (id INTEGER PRIMARY KEY, x TEXT, FULLTEXT KEY dx (x))'); h.sql("INSERT INTO d (x) VALUES ('hello')");
  h.sql('DROP TABLE d'); h.sql('CREATE TABLE d (id INTEGER PRIMARY KEY, x TEXT)');
  assert.deepEqual(shown(h, 'd'), []);
  h.close();
}));

test('errors: syntax, MATCH without its index, places MATCH does not work, bad queries', () => tempDir(async dir => {
  const h = new Database(join(dir, 'e.kdb'), {create: true});
  h.sql('CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT, b TEXT)');
  h.sql('CREATE TABLE o (id INTEGER PRIMARY KEY, t_id INTEGER)');
  h.sql("INSERT INTO t (a, b) VALUES ('one two', 'three')");
  assert.throws(() => h.query("SELECT id FROM t WHERE MATCH (a) AGAINST ('one')"), /MATCH \(a\) needs a FULLTEXT index on exactly these columns of t/);
  h.sql('CREATE FULLTEXT INDEX t_a ON t (a)');
  assert.throws(() => h.sql('CREATE FULLTEXT INDEX t_a2 ON t (a)'), /already exists/);
  h.sql('CREATE FULLTEXT INDEX IF NOT EXISTS t_a2 ON t (a)');
  assert.throws(() => h.sql('CREATE FULLTEXT INDEX t_a ON t (b)'), /already used/);
  for (const [q, re] of [
    ['CREATE FULLTEXT t_x ON t (a)', /expected INDEX after CREATE FULLTEXT/],
    ['CREATE FULLTEXT INDEX x ON t (a, a)', /twice/],
    ['CREATE FULLTEXT INDEX x ON t (id)', /not id/],
    ['CREATE FULLTEXT INDEX x ON t (a) WITH (min_token_length = 0)', /from 1 to 64/],
    ['CREATE FULLTEXT INDEX x ON t (a) WITH (min_token_length = 65)', /from 1 to 64/],
    ['CREATE FULLTEXT INDEX x ON t (a) WITH (parser = 1)', /min_token_length/],
    ['CREATE FULLTEXT INDEX x ON nosuch (a)', /no such table/],
    ["SELECT id FROM t WHERE MATCH (a) AGAINST ('x' IN NATURAL MODE)", /expected LANGUAGE/],
    ["SELECT id FROM t WHERE MATCH (a) AGAINST ('x' WITH QUERY EXPANSION)", /not WITH QUERY EXPANSION/],
    ["SELECT id FROM t WHERE MATCH (a) AGAINST ('x' IN BOOLEAN)", /expected MODE/],
    ["SELECT id FROM t WHERE MATCH (a) ('x')", /expected AGAINST/],
    ["SELECT id FROM t WHERE MATCH (a + 1) AGAINST ('x')", /expected '\)'/],
    ["SELECT id FROM t WHERE MATCH () AGAINST ('x')", /expected a name/],
    ['SELECT id FROM t WHERE MATCH (a) AGAINST (b)', /AGAINST takes a constant text/],
    ['SELECT id FROM t WHERE MATCH (a) AGAINST (1)', /AGAINST needs text/],
    ["SELECT id FROM t WHERE MATCH (a, b) AGAINST ('x')", /needs a FULLTEXT index on exactly these columns/],
    ["SELECT id FROM t WHERE MATCH (a) AGAINST ('(x)' IN BOOLEAN MODE)", /not '\('/],
    ["SELECT id FROM t WHERE MATCH (a) AGAINST ('x ~y' IN BOOLEAN MODE)", /not '~'/],
    ["SELECT id FROM t WHERE MATCH (a) AGAINST ('\"open' IN BOOLEAN MODE)", /unterminated phrase/],
    ["SELECT t.id FROM t JOIN o ON o.t_id = t.id WHERE MATCH (t.a) AGAINST ('one')", /not supported in joins/],
    ["INSERT INTO t (a) VALUES (MATCH (a) AGAINST ('one'))", /MATCH \.\.\. AGAINST works in SELECT, UPDATE and DELETE/],
    ["CREATE TABLE c (id INTEGER PRIMARY KEY, a TEXT CHECK (MATCH (a) AGAINST ('x')))", /CHECK takes an expression/],
    ['CREATE TABLE c (id INTEGER PRIMARY KEY, a TEXT, FULLTEXT (a, a))', /twice/],
    ['CREATE TABLE c (id INTEGER PRIMARY KEY, a TEXT, FULLTEXT (id))', /not id/],
    ['SELECT id FROM t WHERE MATCH (a) AGAINST (?)', /missing value for parameter 1/],
  ]) assert.throws(() => h.sql(q), re, q);
  assert.equal(h.query('SHOW TABLES').filter(r => r.table === 'c').length, 0);
  // A long query is refused; MATCH with no index gives the same error in UPDATE and DELETE.
  assert.throws(() => h.query('SELECT id FROM t WHERE MATCH (a) AGAINST (?)', [Array.from({length: 70}, (_, i) => 'w' + i).join(' ')]), /at most 64 words/);
  assert.throws(() => h.sql("UPDATE t SET a = 'x' WHERE MATCH (b) AGAINST ('three')"), /needs a FULLTEXT index/);
  assert.throws(() => h.sql("DELETE FROM t WHERE MATCH (b) AGAINST ('three')"), /needs a FULLTEXT index/);
  // MATCH is still an ordinary column name.
  h.sql('CREATE TABLE g (id INTEGER PRIMARY KEY, match TEXT)'); h.sql("INSERT INTO g (match) VALUES ('m')");
  assert.deepEqual(h.query('SELECT match FROM g'), [{match: 'm'}]);
  h.close();
}));

async function startServer(dir, args) {
  const child = spawn(serverBinary, ['--db', join(dir, 's.kdb'), '--create', '--tcp', '127.0.0.1:0', '--stop-on-stdin-eof', ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '';
  child.stderr.on('data', d => { out += d; });
  const address = await new Promise((resolve, reject) => {
    child.stdout.on('data', d => { out += d; const m = /KESTRODB READY tcp (\S+)/.exec(out); if (m) resolve('tcp://' + m[1]); });
    child.on('exit', code => reject(Error(`server exited ${code}: ${out}`)));
  });
  const exited = new Promise(r => child.on('exit', r));
  return {address, stop: async () => { child.stdin.end(); return exited; }};
}

test('server: permissions for FULLTEXT DDL and MATCH; BEGIN CONCURRENT reads its snapshot', () => tempDir(async dir => {
  const tokens = {admin: 'admin-token-0123456789abcdef0123', writer: 'writer-token-0123456789abcdef012', reader: 'reader-token-0123456789abcdef012', scoped: 'scoped-token-0123456789abcdef012'};
  await writeFile(join(dir, 'users'), ['admin admin ' + tokens.admin, 'writer readwrite ' + tokens.writer, 'reader readonly ' + tokens.reader, 'scoped readonly ' + tokens.scoped + ' tables=notes', ''].join('\n'));
  const server = await startServer(dir, ['--users', join(dir, 'users')]);
  const open = [];
  try {
    const as = async name => { const c = await RemoteDB.connect(server.address, {token: tokens[name], reconnect: false}); open.push(c); return c; };
    const admin = await as('admin'), writer = await as('writer'), reader = await as('reader'), scoped = await as('scoped');
    await admin.sql('CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT)');
    await admin.sql('CREATE TABLE secrets (id INTEGER PRIMARY KEY, body TEXT, FULLTEXT (body))');
    await admin.sql("INSERT INTO secrets (body) VALUES ('launch codes')");
    await assert.rejects(writer.sql('CREATE FULLTEXT INDEX ON notes (body)'), /AUTH/);
    await assert.rejects(reader.sql('CREATE FULLTEXT INDEX ON notes (body)'), /AUTH/);
    await admin.sql('CREATE FULLTEXT INDEX notes_body ON notes (body)');
    await writer.sql("INSERT INTO notes (body) VALUES ('alpha beta'), ('beta gamma'), ('gamma delta')");
    assert.deepEqual((await reader.sql("SELECT id FROM notes WHERE MATCH (body) AGAINST ('beta') ORDER BY id")).rows, [{id: 1}, {id: 2}]);
    await assert.rejects(reader.sql("DELETE FROM notes WHERE MATCH (body) AGAINST ('beta')"), /AUTH/);
    assert.equal((await writer.sql("DELETE FROM notes WHERE MATCH (body) AGAINST ('delta')")).changes, 1);
    assert.deepEqual((await scoped.sql("SELECT id FROM notes WHERE MATCH (body) AGAINST ('alpha')")).rows, [{id: 1}]);
    await assert.rejects(scoped.sql("SELECT id FROM secrets WHERE MATCH (body) AGAINST ('launch')"), /AUTH .*secrets/);
    await assert.rejects(scoped.sql("SELECT id FROM notes WHERE id IN (SELECT id FROM secrets WHERE MATCH (body) AGAINST ('launch'))"), /AUTH .*secrets/);
    await assert.rejects(writer.sql('DROP INDEX notes_body ON notes'), /AUTH/);
    // A BEGIN CONCURRENT transaction keeps reading its snapshot: a row changed (or added)
    // since does not change its MATCH results.
    const a = await as('admin');
    await a.sql('BEGIN CONCURRENT');
    assert.deepEqual((await a.sql("SELECT id FROM notes WHERE MATCH (body) AGAINST ('beta') ORDER BY id")).rows, [{id: 1}, {id: 2}]);
    await writer.sql("UPDATE notes SET body = 'omega' WHERE id = 1");
    await writer.sql("INSERT INTO notes (body) VALUES ('beta again')");
    assert.deepEqual((await a.sql("SELECT id FROM notes WHERE MATCH (body) AGAINST ('beta') ORDER BY id")).rows, [{id: 1}, {id: 2}]);
    await a.sql('COMMIT');
    assert.deepEqual((await a.sql("SELECT id FROM notes WHERE MATCH (body) AGAINST ('beta') ORDER BY id")).rows, [{id: 2}, {id: 4}]);
    // Serializable: a writing transaction whose MATCH missed a row added since (a phantom)
    // fails at COMMIT; with no such row it commits.
    await a.sql('BEGIN CONCURRENT');
    assert.equal((await a.sql("SELECT COUNT(*) AS n FROM notes WHERE MATCH (body) AGAINST ('beta')")).rows[0].n, 2);
    await writer.sql("INSERT INTO notes (body) VALUES ('beta three')");
    await a.sql("UPDATE notes SET body = 'beta two' WHERE id = 2");
    await assert.rejects(a.sql('COMMIT'), /CONFLICT/);
    await a.sql('BEGIN CONCURRENT');
    assert.equal((await a.sql("SELECT COUNT(*) AS n FROM notes WHERE MATCH (body) AGAINST ('beta')")).rows[0].n, 3);
    await a.sql("UPDATE notes SET body = 'beta two' WHERE id = 2");
    await a.sql('COMMIT');
  } finally { for (const x of open) await x.close().catch(() => {}); await server.stop(); }
}));

test('a MATCH on 100,000 rows answers far faster than a LIKE scan', t => tempDir(async dir => {
  const h = new Database(join(dir, 'big.kdb'), {create: true});
  h.sql('CREATE TABLE posts (id INTEGER PRIMARY KEY, title TEXT, body TEXT)');
  const N = 100000, words = Array.from({length: 5000}, (_, i) => 'w' + i.toString(36) + 'x');
  for (let i = 0; i < N; i += 1000) {
    const v = [], p = [];
    for (let k = i; k < i + 1000; k++) {
      const body = Array.from({length: 12}, () => words[rand(words.length)]);
      if (k % 10000 === 77) body[5] = 'kestrel';  // 10 rows
      v.push('(?, ?)'); p.push('post ' + k, body.join(' '));
    }
    h.sql('INSERT INTO posts (title, body) VALUES ' + v.join(', '), p);
  }
  let t0 = process.hrtime.bigint();
  h.sql('CREATE FULLTEXT INDEX posts_text ON posts (title, body)');
  const build = Number(process.hrtime.bigint() - t0) / 1e6;
  const time = (q, n) => {
    assert.equal(h.query(q).length, n, q);
    const t1 = process.hrtime.bigint(); for (let r = 0; r < 10; r++) h.query(q);
    return Number(process.hrtime.bigint() - t1) / 10 / 1e6;
  };
  const like = time("SELECT id FROM posts WHERE body LIKE '%kestrel%'", 10);
  const match = time("SELECT id FROM posts WHERE MATCH (title, body) AGAINST ('kestrel')", 10);
  const ranked = time("SELECT id, MATCH (title, body) AGAINST ('kestrel') AS s FROM posts WHERE MATCH (title, body) AGAINST ('kestrel') ORDER BY s DESC LIMIT 5", 5);
  t0 = process.hrtime.bigint();
  for (let i = 0; i < 1000; i++) h.sql('INSERT INTO posts (title, body) VALUES (?, ?)', ['late ' + i, 'kestrel ' + words[i]]);
  const inserts = Number(process.hrtime.bigint() - t0) / 1000 / 1e6;
  t.diagnostic(`100,000 rows: index built in ${build.toFixed(0)} ms; LIKE '%term%' ${like.toFixed(2)} ms, MATCH ${match.toFixed(3)} ms, ranked top 5 ${ranked.toFixed(3)} ms; insert with the index ${inserts.toFixed(3)} ms`);
  assert.ok(match * 20 < like, `MATCH ${match.toFixed(3)} ms vs LIKE ${like.toFixed(3)} ms`);
  assert.equal(h.query("SELECT COUNT(*) AS n FROM posts WHERE MATCH (title, body) AGAINST ('kestrel')")[0].n, 1010);
  h.close();
}));
